// Linux bulk-transfer implementation (Stage B, part 1): pull / push /
// resume / range read-write. Same wire behavior as src/device_client.cpp;
// POSIX file IO replaces the Win32 HANDLE paths. Async pipeline structure
// (double-buffered pull, triple-pipelined push) is preserved verbatim.
#include "device_client.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "log_facade.h"

namespace {
class LocalFile {
public:
    LocalFile() = default;
    ~LocalFile() { reset(); }
    LocalFile(const LocalFile&) = delete;
    LocalFile& operator=(const LocalFile&) = delete;
    bool openWrite(const std::string& path, bool truncate) {
        reset();
        m_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | (truncate ? O_TRUNC : 0), 0666);
        return m_fd >= 0;
    }
    bool openRead(const std::string& path) {
        reset();
        m_fd = ::open(path.c_str(), O_RDONLY);
        if (m_fd >= 0) ::posix_fadvise(m_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
        return m_fd >= 0;
    }
    bool seek(uint64_t offset) {
        return ::lseek(m_fd, static_cast<off_t>(offset), SEEK_SET) >= 0;
    }
    bool writeAll(const char* data, size_t len, size_t& written) {
        written = 0;
        while (written < len) {
            const ssize_t n = ::write(m_fd, data + written, len - written);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            written += static_cast<size_t>(n);
        }
        return true;
    }
    ssize_t readSome(char* data, size_t len) {
        for (;;) {
            const ssize_t n = ::read(m_fd, data, len);
            if (n < 0 && errno == EINTR) continue;
            return n;
        }
    }
    int get() const { return m_fd; }
    void reset() {
        if (m_fd >= 0) ::close(m_fd);
        m_fd = -1;
    }
private:
    int m_fd = -1;
};

uint64_t localFileSize(const std::string& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(size);
}
}  // namespace

bool DeviceClient::pullFile(const std::string& remotePath, const std::string& localPath,
                            uint64_t& outFileSize, ProgressCallback progress) {
    if (!m_connected) {
        m_lastError = "Not connected";
        LOG_ERROR("Transfer", "pullFile: not connected");
        return false;
    }
    LOG_INFO("Transfer", "pullFile START: " + remotePath + " -> " + localPath);
    const std::lock_guard<std::mutex> lk(m_mutex);

    if (!sendMsg(CMD_PULL, remotePath.data(), static_cast<uint32_t>(remotePath.size()))) {
        LOG_ERROR("Transfer", "pullFile: sendMsg CMD_PULL failed");
        return false;
    }
    MsgHeader hdr{};
    std::vector<char> payload;
    if (!recvMsg(hdr, payload)) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    if (hdr.cmd != RSP_OK || payload.size() < sizeof(PullHeader)) return false;
    PullHeader ph{};
    std::memcpy(&ph, payload.data(), sizeof(ph));
    outFileSize = ph.file_size;

    LocalFile file;
    if (!file.openWrite(localPath, true)) {
        // Parent directory may not exist yet (race with parallel creation).
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(localPath).parent_path(), ec);
        if (!file.openWrite(localPath, true)) {
            m_lastError = "Failed to create local file: " + localPath + " (errno " +
                std::to_string(errno) + ")";
            while (recvAll(&hdr, sizeof(hdr))) {
                if (hdr.cmd == RSP_DONE) break;
                if (hdr.cmd != RSP_DATA || hdr.length == 0) break;
                if (m_transferBuf.size() < hdr.length) m_transferBuf.resize(hdr.length);
                if (!recvAll(m_transferBuf.data(), hdr.length)) break;
            }
            return false;
        }
    }

    uint64_t received = 0;
    bool success = true;
    inlineCrcReset();

    // Double-buffered pull: async write+CRC of previous chunk while recv
    // blocks on the next one.
    std::vector<char> bufA(AFM_CHUNK_SIZE), bufB(AFM_CHUNK_SIZE);
    char* recvBuf = bufA.data();
    std::future<bool> writeFut;
    bool gotDone = false;
    while (true) {
        if (!recvAll(&hdr, sizeof(hdr))) {
            success = false;
            break;
        }
        if (hdr.cmd == RSP_DONE) {
            gotDone = true;
            break;
        }
        if (hdr.cmd != RSP_DATA || hdr.length == 0) {
            success = false;
            break;
        }
        if (!recvAll(recvBuf, hdr.length)) {
            success = false;
            break;
        }
        if (writeFut.valid() && !writeFut.get()) {
            success = false;
            break;
        }
        if (progress && received > 0 && !progress(received, outFileSize)) {
            success = false;
            m_lastError = "Cancelled";
            break;
        }
        char* writeBuf = recvBuf;
        const uint32_t writeLen = hdr.length;
        const int fd = file.get();
        writeFut = std::async(std::launch::async, [this, writeBuf, writeLen, fd, &received] {
            const char* p = writeBuf;
            size_t left = writeLen;
            int64_t total = 0;
            while (left > 0) {
                const ssize_t n = ::write(fd, p, left);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    return false;
                }
                p += n;
                left -= static_cast<size_t>(n);
                total += n;
            }
            inlineCrcUpdate(writeBuf, writeLen);
            received += static_cast<uint64_t>(total);
            return true;
        });
        recvBuf = (recvBuf == bufA.data()) ? bufB.data() : bufA.data();
    }
    if (writeFut.valid() && !writeFut.get()) success = false;
    if (success && progress) progress(received, outFileSize);
    file.reset();

    // Drain until RSP_DONE only if the loop didn't consume it — otherwise the
    // drain would block until timeout and kill a healthy connection.
    if (m_connected && !gotDone) {
        MsgHeader drainHdr{};
        while (recvAll(&drainHdr, sizeof(drainHdr))) {
            if (drainHdr.cmd == RSP_DONE) break;
            if (drainHdr.cmd != RSP_DATA || drainHdr.length == 0) break;
            if (m_transferBuf.size() < drainHdr.length)
                m_transferBuf.resize(drainHdr.length);
            if (!recvAll(m_transferBuf.data(), drainHdr.length)) break;
        }
    }
    if (success) {
        m_inlineCrc = ~m_inlineCrc;
        m_inlineCrcPath = localPath;
        LOG_INFO("Transfer", "pullFile OK: " + std::to_string(received) + " bytes received");
    } else {
        LOG_ERROR("Transfer", "pullFile FAILED: " + m_lastError);
    }
    return success;
}

bool DeviceClient::pushFile(const std::string& localPath, const std::string& remotePath,
                            uint64_t fileSize, ProgressCallback progress) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    const std::lock_guard<std::mutex> lk(m_mutex);

    std::vector<char> pushPayload(sizeof(PushHeader) + remotePath.size());
    const PushHeader ph{fileSize};
    std::memcpy(pushPayload.data(), &ph, sizeof(ph));
    std::memcpy(pushPayload.data() + sizeof(ph), remotePath.data(), remotePath.size());
    if (!sendMsg(CMD_PUSH, pushPayload.data(), static_cast<uint32_t>(pushPayload.size())))
        return false;

    MsgHeader hdr{};
    std::vector<char> payload;
    if (!recvMsg(hdr, payload)) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    if (hdr.cmd != RSP_OK) return false;

    LocalFile file;
    if (!file.openRead(localPath)) {
        m_lastError = "Failed to open local file: " + localPath;
        sendMsg(RSP_DONE);  // don't leave the server hanging
        return false;
    }

    // Triple-pipelined push: send chunk N while reading + CRC-ing chunk N+1.
    const size_t pushBufSize = sizeof(MsgHeader) + AFM_CHUNK_SIZE;
    std::vector<char> bufA(pushBufSize), bufB(pushBufSize), bufC(pushBufSize);
    char* sendBuf = bufA.data();
    char* readBuf = bufB.data();
    char* spareBuf = bufC.data();
    uint64_t sent = 0;
    bool success = true;
    inlineCrcReset();

    uint32_t sendBytes = 0;
    {
        const uint32_t firstToRead =
            static_cast<uint32_t>(std::min<uint64_t>(AFM_CHUNK_SIZE, fileSize));
        const ssize_t n = file.readSome(sendBuf + sizeof(MsgHeader), firstToRead);
        if (n <= 0) {
            sendMsg(RSP_DONE);
            recvMsg(hdr, payload);
            return false;
        }
        sendBytes = static_cast<uint32_t>(n);
        inlineCrcUpdate(sendBuf + sizeof(MsgHeader), sendBytes);
    }

    while (sent < fileSize) {
        auto* hdrPtr = reinterpret_cast<MsgHeader*>(sendBuf);
        hdrPtr->cmd = RSP_DATA;
        hdrPtr->length = sendBytes;

        const uint64_t nextOffset = sent + sendBytes;
        uint32_t nextBytes = 0;
        const bool hasNext = nextOffset < fileSize;

        std::future<bool> sendFut = std::async(std::launch::async, [&, sendBuf, sendBytes] {
            return sendAll(sendBuf, sizeof(MsgHeader) + sendBytes);
        });

        bool readOk = true;
        if (hasNext) {
            const uint32_t toRead =
                static_cast<uint32_t>(std::min<uint64_t>(AFM_CHUNK_SIZE, fileSize - nextOffset));
            const ssize_t n = file.readSome(readBuf + sizeof(MsgHeader), toRead);
            if (n <= 0) {
                readOk = false;
            } else {
                nextBytes = static_cast<uint32_t>(n);
                inlineCrcUpdate(readBuf + sizeof(MsgHeader), nextBytes);
            }
        }

        if (!sendFut.get()) {
            success = false;
            break;
        }
        sent += sendBytes;
        if (progress && !progress(sent, fileSize)) {
            success = false;
            m_lastError = "Cancelled";
            break;
        }
        if (!hasNext || !readOk) break;

        char* oldSendBuf = sendBuf;
        sendBuf = readBuf;
        readBuf = spareBuf;
        spareBuf = oldSendBuf;
        sendBytes = nextBytes;
    }
    file.reset();

    sendMsg(RSP_DONE);
    // ALWAYS read the server confirmation to keep the TCP stream in sync
    // (even on cancel — otherwise RSP_OK corrupts the next command).
    if (recvMsg(hdr, payload) && success && hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    if (success) {
        m_inlineCrc = ~m_inlineCrc;
        m_inlineCrcPath = localPath;
    }
    return success;
}

bool DeviceClient::resumePullFile(const std::string& remotePath, const std::string& localPath,
                                  uint64_t& outFileSize, ProgressCallback progress) {
    const uint64_t localSize = localFileSize(localPath);
    if (localSize == 0) return pullFile(remotePath, localPath, outFileSize, progress);

    const std::lock_guard<std::mutex> lk(m_mutex);

    std::vector<char> payload(8 + remotePath.size());
    std::memcpy(payload.data(), &localSize, 8);
    std::memcpy(payload.data() + 8, remotePath.data(), remotePath.size());
    if (!sendMsg(CMD_RESUME_PULL, payload.data(), static_cast<uint32_t>(payload.size())))
        return false;

    MsgHeader hdr{};
    std::vector<char> resp;
    if (!recvMsg(hdr, resp)) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return false;
    }
    if (hdr.cmd != RSP_OK || resp.size() < sizeof(PullHeader)) return false;
    PullHeader ph{};
    std::memcpy(&ph, resp.data(), sizeof(ph));
    outFileSize = ph.file_size;

    LocalFile file;
    if (!file.openWrite(localPath, false) || !file.seek(localSize)) {
        m_lastError = "Failed to open local file: " + localPath;
        return false;
    }

    uint64_t received = localSize;
    bool success = true;
    while (true) {
        if (!recvAll(&hdr, sizeof(hdr))) {
            success = false;
            break;
        }
        if (hdr.cmd == RSP_DONE) break;
        if (hdr.cmd != RSP_DATA || hdr.length == 0) {
            success = false;
            break;
        }
        if (m_transferBuf.size() < hdr.length) m_transferBuf.resize(hdr.length);
        if (!recvAll(m_transferBuf.data(), hdr.length)) {
            success = false;
            break;
        }
        size_t written = 0;
        if (!file.writeAll(m_transferBuf.data(), hdr.length, written)) {
            m_lastError = "Failed writing local file (errno " + std::to_string(errno) + ")";
            success = false;
            break;
        }
        received += written;
        if (progress && !progress(received, outFileSize)) {
            success = false;
            m_lastError = "Cancelled";
            break;
        }
    }
    file.reset();

    if (m_connected) {
        while (recvAll(&hdr, sizeof(hdr))) {
            if (hdr.cmd == RSP_DONE) break;
            if (hdr.cmd != RSP_DATA || hdr.length == 0) break;
            if (m_transferBuf.size() < hdr.length) m_transferBuf.resize(hdr.length);
            if (!recvAll(m_transferBuf.data(), hdr.length)) break;
        }
    }
    return success;
}

bool DeviceClient::resumePushFile(const std::string& localPath, const std::string& remotePath,
                                  uint64_t fileSize, ProgressCallback progress) {
    const std::lock_guard<std::mutex> lk(m_mutex);

    std::vector<char> payload(8 + remotePath.size());
    std::memcpy(payload.data(), &fileSize, 8);
    std::memcpy(payload.data() + 8, remotePath.data(), remotePath.size());
    if (!sendMsg(CMD_RESUME_PUSH, payload.data(), static_cast<uint32_t>(payload.size())))
        return false;

    // Server replies RSP_OK + [8B offset] = bytes it already holds.
    MsgHeader hdr{};
    std::vector<char> resp;
    if (!recvMsg(hdr, resp)) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return false;
    }
    if (hdr.cmd != RSP_OK || resp.size() < 8) return false;
    uint64_t offset = 0;
    std::memcpy(&offset, resp.data(), 8);

    LocalFile file;
    if (!file.openRead(localPath)) {
        m_lastError = "Failed to open local file: " + localPath;
        sendMsg(RSP_DONE);
        return false;
    }
    if (offset > 0 && !file.seek(offset)) {
        m_lastError = "Failed seeking local file: " + localPath;
        sendMsg(RSP_DONE);
        return false;
    }

    const size_t pushBufSize = sizeof(MsgHeader) + AFM_CHUNK_SIZE;
    if (m_transferBuf.size() < pushBufSize) m_transferBuf.resize(pushBufSize);
    char* buf = m_transferBuf.data();
    uint64_t sent = offset;
    bool success = true;
    while (sent < fileSize) {
        const uint32_t toRead =
            static_cast<uint32_t>(std::min<uint64_t>(AFM_CHUNK_SIZE, fileSize - sent));
        const ssize_t n = file.readSome(buf + sizeof(MsgHeader), toRead);
        if (n <= 0) break;
        const uint32_t bytesRead = static_cast<uint32_t>(n);
        auto* hdrPtr = reinterpret_cast<MsgHeader*>(buf);
        hdrPtr->cmd = RSP_DATA;
        hdrPtr->length = bytesRead;
        if (!sendAll(buf, sizeof(MsgHeader) + bytesRead)) {
            success = false;
            break;
        }
        sent += bytesRead;
        if (progress && !progress(sent, fileSize)) {
            success = false;
            m_lastError = "Cancelled";
            break;
        }
    }
    file.reset();

    sendMsg(RSP_DONE);
    if (recvMsg(hdr, resp) && success && hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return false;
    }
    return success;
}

bool DeviceClient::createFile(const std::string& remotePath, uint64_t totalSize) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    std::vector<char> payload(8 + remotePath.size());
    std::memcpy(payload.data(), &totalSize, 8);
    std::memcpy(payload.data() + 8, remotePath.data(), remotePath.size());
    MsgHeader hdr{};
    std::vector<char> resp;
    if (!runSmallOp(CMD_CREATE_FILE, payload.data(), static_cast<uint32_t>(payload.size()), hdr,
                    resp))
        return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return false;
    }
    return hdr.cmd == RSP_OK;
}

uint64_t DeviceClient::readRange(const std::string& remotePath, uint64_t offset, uint64_t length,
                                 void* outBuffer) {
    bool ok = false;
    return readRangeInternal(remotePath, offset, length, outBuffer, ok);
}

#if !defined(_WIN32)
uint64_t DeviceClient::readRangeWithStatus(const std::string& remotePath, uint64_t offset,
                                           uint64_t length, void* outBuffer, bool& ok) {
    return readRangeInternal(remotePath, offset, length, outBuffer, ok);
}
#endif

uint64_t DeviceClient::readRangeInternal(const std::string& remotePath, uint64_t offset,
                                         uint64_t length, void* outBuffer, bool& ok) {
    ok = false;
    if (!m_connected) {
        m_lastError = "Not connected";
        return 0;
    }
    const std::lock_guard<std::mutex> lk(m_mutex);
    m_lastError.clear();

    std::vector<char> payload(16 + remotePath.size());
    std::memcpy(payload.data(), &offset, 8);
    std::memcpy(payload.data() + 8, &length, 8);
    std::memcpy(payload.data() + 16, remotePath.data(), remotePath.size());
    if (!sendMsg(CMD_READ_RANGE, payload.data(), static_cast<uint32_t>(payload.size()))) return 0;

    MsgHeader hdr{};
    std::vector<char> resp;
    if (!recvMsg(hdr, resp)) return 0;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return 0;
    }
    if (hdr.cmd != RSP_OK || resp.size() < 8) return 0;
    uint64_t actualLen = 0;
    std::memcpy(&actualLen, resp.data(), 8);
    if (actualLen > length) {
        m_lastError = "Range response exceeds requested length";
        return 0;
    }

    uint64_t received = 0;
    char* dst = static_cast<char*>(outBuffer);
    bool failed = false;
    while (true) {
        if (!recvAll(&hdr, sizeof(hdr))) { failed = true; break; }
        if (hdr.cmd == RSP_DONE) break;
        if (hdr.cmd != RSP_DATA || hdr.length == 0 || hdr.length > AFM_CHUNK_SIZE) {
            failed = true;
            break;
        }
        if (received > actualLen) { failed = true; break; }
        const uint64_t remaining = actualLen - received;
        if (hdr.length <= remaining) {
            if (!recvAll(dst + received, hdr.length)) { failed = true; break; }
        } else {
            if (m_transferBuf.size() < hdr.length) m_transferBuf.resize(hdr.length);
            if (!recvAll(m_transferBuf.data(), hdr.length)) { failed = true; break; }
            if (remaining > 0)
                std::memcpy(dst + received, m_transferBuf.data(), static_cast<size_t>(remaining));
        }
        received += hdr.length;
        if (received > actualLen) { failed = true; break; }
    }
    if (failed) {
        if (m_lastError.empty()) m_lastError = "Read range failed";
        return 0;
    }
    ok = true;
    return received;
}

uint64_t DeviceClient::readRangeStreaming(const std::string& remotePath, uint64_t offset,
                                          uint64_t length, ReadRangeCallback chunkCallback) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return 0;
    }
    const std::lock_guard<std::mutex> lk(m_mutex);

    std::vector<char> payload(16 + remotePath.size());
    std::memcpy(payload.data(), &offset, 8);
    std::memcpy(payload.data() + 8, &length, 8);
    std::memcpy(payload.data() + 16, remotePath.data(), remotePath.size());
    if (!sendMsg(CMD_READ_RANGE, payload.data(), static_cast<uint32_t>(payload.size()))) return 0;

    MsgHeader hdr{};
    std::vector<char> resp;
    if (!recvMsg(hdr, resp)) return 0;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return 0;
    }
    if (hdr.cmd != RSP_OK || resp.size() < 8) return 0;
    uint64_t actualLen = 0;
    std::memcpy(&actualLen, resp.data(), 8);

    uint64_t received = 0;
    if (m_transferBuf.size() < AFM_CHUNK_SIZE) m_transferBuf.resize(AFM_CHUNK_SIZE);
    while (true) {
        if (!recvAll(&hdr, sizeof(hdr))) break;
        if (hdr.cmd == RSP_DONE) break;
        if (hdr.cmd != RSP_DATA || hdr.length == 0) break;
        if (m_transferBuf.size() < hdr.length) m_transferBuf.resize(hdr.length);
        if (!recvAll(m_transferBuf.data(), hdr.length)) break;
        if (!chunkCallback(m_transferBuf.data(), hdr.length, offset + received)) {
            // Callback aborted — drain the remaining stream to keep it in sync.
            while (recvAll(&hdr, sizeof(hdr))) {
                if (hdr.cmd == RSP_DONE) break;
                if (hdr.cmd != RSP_DATA || hdr.length == 0) break;
                if (m_transferBuf.size() < hdr.length) m_transferBuf.resize(hdr.length);
                if (!recvAll(m_transferBuf.data(), hdr.length)) break;
            }
            break;
        }
        received += hdr.length;
    }
    return received;
}

uint64_t DeviceClient::writeRange(const std::string& remotePath, uint64_t offset, const void* data,
                                  uint32_t length) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return 0;
    }
    const std::lock_guard<std::mutex> lk(m_mutex);

    const uint64_t len64 = length;
    std::vector<char> cmdPayload(16 + remotePath.size());
    std::memcpy(cmdPayload.data(), &offset, 8);
    std::memcpy(cmdPayload.data() + 8, &len64, 8);
    std::memcpy(cmdPayload.data() + 16, remotePath.data(), remotePath.size());
    if (!sendMsg(CMD_WRITE_RANGE, cmdPayload.data(), static_cast<uint32_t>(cmdPayload.size())))
        return 0;

    MsgHeader hdr{};
    std::vector<char> resp;
    if (!recvMsg(hdr, resp)) return 0;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return 0;
    }
    if (hdr.cmd != RSP_OK) return 0;

    // Servers may drop oversized single frames: stream protocol-sized chunks.
    const char* p = static_cast<const char*>(data);
    uint64_t sent = 0;
    while (sent < len64) {
        const uint32_t chunkLen =
            static_cast<uint32_t>(std::min<uint64_t>(AFM_CHUNK_SIZE, len64 - sent));
        const MsgHeader dataHdr{RSP_DATA, chunkLen};
        if (!sendAll(&dataHdr, sizeof(dataHdr))) return 0;
        if (!sendAll(p + sent, chunkLen)) return 0;
        sent += chunkLen;
    }
    if (!sendMsg(RSP_DONE)) return 0;
    if (!recvMsg(hdr, resp)) return 0;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return 0;
    }
    if (hdr.cmd != RSP_OK || resp.size() < 8) return 0;
    uint64_t written = 0;
    std::memcpy(&written, resp.data(), 8);
    return written;
}

uint64_t DeviceClient::writeRangeStreaming(const std::string& remotePath, uint64_t remoteOffset,
                                           const std::string& localPath, uint64_t localOffset,
                                           uint64_t length, ProgressCallback progress) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return 0;
    }
    const std::lock_guard<std::mutex> lk(m_mutex);

    LocalFile file;
    if (!file.openRead(localPath)) {
        m_lastError = "Failed to open local file: " + localPath;
        return 0;
    }
    if (!file.seek(localOffset)) {
        m_lastError = "Failed seeking local file: " + localPath;
        return 0;
    }

    std::vector<char> cmdPayload(16 + remotePath.size());
    std::memcpy(cmdPayload.data(), &remoteOffset, 8);
    std::memcpy(cmdPayload.data() + 8, &length, 8);
    std::memcpy(cmdPayload.data() + 16, remotePath.data(), remotePath.size());
    if (!sendMsg(CMD_WRITE_RANGE, cmdPayload.data(), static_cast<uint32_t>(cmdPayload.size())))
        return 0;

    MsgHeader hdr{};
    std::vector<char> resp;
    if (!recvMsg(hdr, resp)) return 0;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return 0;
    }
    if (hdr.cmd != RSP_OK) return 0;

    // Triple-pipelined streaming: send chunk N while reading chunk N+1.
    const size_t pushBufSize = sizeof(MsgHeader) + AFM_CHUNK_SIZE;
    std::vector<char> bufA(pushBufSize), bufB(pushBufSize), bufC(pushBufSize);
    char* sendBuf = bufA.data();
    char* readBuf = bufB.data();
    char* spareBuf = bufC.data();
    uint64_t sent = 0;
    bool success = true;

    uint32_t sendBytes = 0;
    {
        const uint32_t firstToRead =
            static_cast<uint32_t>(std::min<uint64_t>(AFM_CHUNK_SIZE, length));
        const ssize_t n = file.readSome(sendBuf + sizeof(MsgHeader), firstToRead);
        if (n <= 0) {
            sendMsg(RSP_DONE);
            recvMsg(hdr, resp);
            return 0;
        }
        sendBytes = static_cast<uint32_t>(n);
    }

    while (sent < length) {
        auto* hdrPtr = reinterpret_cast<MsgHeader*>(sendBuf);
        hdrPtr->cmd = RSP_DATA;
        hdrPtr->length = sendBytes;

        const uint64_t nextOffset = sent + sendBytes;
        uint32_t nextBytes = 0;
        const bool hasNext = nextOffset < length;

        std::future<bool> sendFut = std::async(std::launch::async, [&, sendBuf, sendBytes] {
            return sendAll(sendBuf, sizeof(MsgHeader) + sendBytes);
        });

        bool readOk = true;
        if (hasNext) {
            const uint32_t toRead =
                static_cast<uint32_t>(std::min<uint64_t>(AFM_CHUNK_SIZE, length - nextOffset));
            const ssize_t n = file.readSome(readBuf + sizeof(MsgHeader), toRead);
            if (n <= 0) readOk = false;
            else nextBytes = static_cast<uint32_t>(n);
        }

        if (!sendFut.get()) {
            success = false;
            break;
        }
        sent += sendBytes;
        if (progress && !progress(sent, length)) {
            success = false;
            m_lastError = "Cancelled";
            break;
        }
        if (!hasNext || !readOk) break;

        char* oldSendBuf = sendBuf;
        sendBuf = readBuf;
        readBuf = spareBuf;
        spareBuf = oldSendBuf;
        sendBytes = nextBytes;
    }
    file.reset();

    sendMsg(RSP_DONE);
    if (!recvMsg(hdr, resp)) return 0;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return 0;
    }
    if (hdr.cmd != RSP_OK || resp.size() < 8) return success ? sent : 0;
    uint64_t written = 0;
    std::memcpy(&written, resp.data(), 8);
    return written;
}
