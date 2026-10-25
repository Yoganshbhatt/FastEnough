// Linux archive + MCRAW support (Stage C, part 2): archiveRemoteDirectory,
// extractRemoteArchive, listMcraw, pullMcrawItem. Same wire behavior as
// src/device_client.cpp. Long operations (tar over minutes, DNG generation)
// use explicit per-call poll deadlines instead of SO_RCVTIMEO juggling.
#include "device_client.h"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <mutex>
#include <vector>

#include "log_facade.h"
#include "net_posix.h"

namespace {
constexpr uint32_t kLongOpMs = 300000;  // tar/archive window, same as Windows
constexpr uint32_t kMcrawMs = 60000;    // DNG generation window

std::string sanitizePathPart(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-')
            out.push_back(c);
        else
            out.push_back('_');
    }
    return out.empty() ? "unknown" : out;
}
}  // namespace

bool DeviceClient::archiveRemoteDirectory(const std::string& remotePath,
                                          const std::string& localTarPath, bool excludeCache,
                                          uint64_t& outFileSize, ProgressCallback progress) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    LOG_INFO("BackupServer", "Archive START: " + remotePath + " -> " + localTarPath);
    const std::lock_guard<std::mutex> lk(m_mutex);
    if (m_socket == ~(uintptr_t)0) {
        m_lastError = "Not connected";
        return false;
    }
    const int fd = static_cast<int>(m_socket);

    const uint32_t exclude = excludeCache ? 1u : 0u;
    std::vector<char> request(4 + remotePath.size());
    std::memcpy(request.data(), &exclude, 4);
    std::memcpy(request.data() + 4, remotePath.data(), remotePath.size());

    std::string error;
    auto failNet = [&](const char* what) {
        m_lastError = std::string(what) + ": " + error;
        m_connected = false;
        return false;
    };
    MsgHeader hdr{CMD_ARCHIVE_PATH, static_cast<uint32_t>(request.size())};
    if (!net::sendAll(fd, &hdr, sizeof(hdr), kLongOpMs, error) ||
        !net::sendAll(fd, request.data(), request.size(), kLongOpMs, error))
        return failNet("send CMD_ARCHIVE_PATH failed");
    if (!net::recvAll(fd, &hdr, sizeof(hdr), kLongOpMs, error))
        return failNet("archive response failed");
    if (hdr.cmd == RSP_ERROR) {
        if (hdr.length > AFM_CHUNK_SIZE + 65536) {
            m_lastError = "Protocol error: payload too large";
            m_connected = false;
            return false;
        }
        std::vector<char> errPayload(hdr.length);
        if (hdr.length > 0 && !net::recvAll(fd, errPayload.data(), hdr.length, kLongOpMs, error))
            return failNet("archive error text failed");
        m_lastError = std::string(errPayload.data(), errPayload.size());
        return false;
    }
    if (hdr.cmd != RSP_OK || hdr.length < sizeof(PullHeader)) {
        m_lastError = "Invalid archive response";
        return false;
    }
    // Consume the full OK payload (PullHeader first) so no bytes leak into
    // the data stream even if the server ever extends it.
    PullHeader ph{};
    {
        std::vector<char> okPayload(hdr.length);
        if (!net::recvAll(fd, okPayload.data(), okPayload.size(), kLongOpMs, error))
            return failNet("archive header failed");
        std::memcpy(&ph, okPayload.data(), sizeof(ph));
    }
    outFileSize = ph.file_size;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(localTarPath).parent_path(), ec);
    const int outFd =
        ::open(localTarPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (outFd < 0) {
        m_lastError = "Failed to create local archive: " + localTarPath;
        return false;
    }

    uint64_t received = 0;
    bool success = true;
    std::vector<char> buf(AFM_CHUNK_SIZE);
    while (true) {
        if (!net::recvAll(fd, &hdr, sizeof(hdr), kLongOpMs, error)) {
            success = false;
            break;
        }
        if (hdr.cmd == RSP_DONE) break;
        if (hdr.cmd != RSP_DATA || hdr.length == 0 || hdr.length > AFM_CHUNK_SIZE) {
            success = false;
            break;
        }
        if (!net::recvAll(fd, buf.data(), hdr.length, kLongOpMs, error)) {
            success = false;
            break;
        }
        size_t off = 0;
        while (off < hdr.length) {
            const ssize_t n = ::write(outFd, buf.data() + off, hdr.length - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                m_lastError = "Failed to write local archive";
                success = false;
                break;
            }
            off += static_cast<size_t>(n);
        }
        if (!success) break;
        received += hdr.length;
        if (progress && !progress(received, outFileSize)) {
            m_lastError = "Cancelled";
            success = false;
            break;
        }
    }
    ::close(outFd);
    if (!success) m_lastError = "Archive FAILED: " + m_lastError;
    else LOG_INFO("BackupServer", "Archive OK: " + std::to_string(received) + " bytes");
    return success;
}

bool DeviceClient::extractRemoteArchive(const std::string& localTarPath,
                                        const std::string& remoteTarget,
                                        ProgressCallback progress) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    std::error_code ec;
    const uint64_t size = static_cast<uint64_t>(std::filesystem::file_size(localTarPath, ec));
    if (ec) {
        m_lastError = "Could not read local archive size";
        return false;
    }
    const std::string remoteArchive =
        "/data/local/tmp/afm-restore-" + sanitizePathPart(remoteTarget) + ".tar";
    if (!pushFile(localTarPath, remoteArchive, size, progress)) return false;

    const std::lock_guard<std::mutex> lk(m_mutex);
    if (m_socket == ~(uintptr_t)0) {
        m_lastError = "Not connected";
        return false;
    }
    const int fd = static_cast<int>(m_socket);
    const uint32_t targetLen = static_cast<uint32_t>(remoteTarget.size());
    const uint32_t archiveLen = static_cast<uint32_t>(remoteArchive.size());
    std::vector<char> payload(8 + targetLen + archiveLen);
    std::memcpy(payload.data(), &targetLen, 4);
    std::memcpy(payload.data() + 4, &archiveLen, 4);
    std::memcpy(payload.data() + 8, remoteTarget.data(), targetLen);
    std::memcpy(payload.data() + 8 + targetLen, remoteArchive.data(), archiveLen);

    std::string error;
    const MsgHeader cmd{CMD_EXTRACT_ARCHIVE, static_cast<uint32_t>(payload.size())};
    if (!net::sendAll(fd, &cmd, sizeof(cmd), kLongOpMs, error) ||
        !net::sendAll(fd, payload.data(), payload.size(), kLongOpMs, error)) {
        m_lastError = "send CMD_EXTRACT_ARCHIVE failed: " + error;
        m_connected = false;
        return false;
    }
    MsgHeader hdr{};
    if (!net::recvAll(fd, &hdr, sizeof(hdr), kLongOpMs, error)) {
        m_lastError = "extract response failed: " + error;
        m_connected = false;
        return false;
    }
    if (hdr.cmd == RSP_ERROR) {
        if (hdr.length > AFM_CHUNK_SIZE + 65536) {
            m_lastError = "Protocol error: payload too large";
            m_connected = false;
            return false;
        }
        std::vector<char> errPayload(hdr.length);
        if (!errPayload.empty() &&
            !net::recvAll(fd, errPayload.data(), errPayload.size(), kLongOpMs, error)) {
            m_lastError = "extract error text failed: " + error;
            m_connected = false;
            return false;
        }
        m_lastError = std::string(errPayload.data(), errPayload.size());
        return false;
    }
    return hdr.cmd == RSP_OK;
}

std::vector<DeviceFileEntry> DeviceClient::listMcraw(const std::string& mcrawPath) {
    std::vector<DeviceFileEntry> entries;
    if (!m_connected) return entries;

    MsgHeader hdr{};
    std::vector<char> payload;
    if (!runSmallOp(CMD_MCRAW_LIST, mcrawPath.data(), static_cast<uint32_t>(mcrawPath.size()), hdr,
                    payload, 30000)) {
        LOG_ERROR("MCRAW", "listMcraw failed");
        return entries;
    }
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return entries;
    }
    if (hdr.cmd != RSP_OK || payload.size() < 4) return entries;

    // Same wire format as listDirectory; server order is logical (no re-sort).
    uint32_t count = 0;
    std::memcpy(&count, payload.data(), 4);
    size_t offset = 4;
    for (uint32_t i = 0; i < count && offset < payload.size(); ++i) {
        if (offset + sizeof(FileEntryHeader) > payload.size()) break;
        FileEntryHeader feh{};
        std::memcpy(&feh, payload.data() + offset, sizeof(feh));
        offset += sizeof(feh);
        if (offset + feh.name_len > payload.size()) break;
        DeviceFileEntry e;
        e.type = feh.type;
        e.size = feh.size;
        e.mtime = feh.mtime;
        e.name = std::string(payload.data() + offset, feh.name_len);
        offset += feh.name_len;
        entries.push_back(std::move(e));
    }
    return entries;
}

bool DeviceClient::pullMcrawItem(const std::string& mcrawPath, const std::string& virtualName,
                                 const std::string& localPath, uint64_t& outFileSize,
                                 ProgressCallback progress) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    const std::lock_guard<std::mutex> lk(m_mutex);
    if (m_socket == ~(uintptr_t)0) {
        m_lastError = "Not connected";
        return false;
    }
    const int fd = static_cast<int>(m_socket);
    std::string error;

    const uint32_t mpLen = static_cast<uint32_t>(mcrawPath.size());
    std::vector<char> extractPayload(4 + mpLen + virtualName.size());
    std::memcpy(extractPayload.data(), &mpLen, 4);
    std::memcpy(extractPayload.data() + 4, mcrawPath.data(), mpLen);
    std::memcpy(extractPayload.data() + 4 + mpLen, virtualName.data(), virtualName.size());

    // 60s window: the server decompresses the frame and generates the DNG.
    const MsgHeader cmd{CMD_MCRAW_EXTRACT, static_cast<uint32_t>(extractPayload.size())};
    if (!net::sendAll(fd, &cmd, sizeof(cmd), kMcrawMs, error) ||
        !net::sendAll(fd, extractPayload.data(), extractPayload.size(), kMcrawMs, error))
        return false;
    MsgHeader hdr{};
    if (!net::recvAll(fd, &hdr, sizeof(hdr), kMcrawMs, error)) {
        LOG_ERROR("MCRAW", "pullMcrawItem recv failed, disconnecting to prevent desync");
        ::close(fd);
        m_socket = ~(uintptr_t)0;
        m_connected = false;
        return false;
    }
    auto recvErrText = [&](uint32_t len) {
        if (len > AFM_CHUNK_SIZE + 65536) {
            m_lastError = "Protocol error: payload too large";
            m_connected = false;
            return m_lastError;
        }
        std::vector<char> errPayload(len);
        if (!errPayload.empty())
            net::recvAll(fd, errPayload.data(), errPayload.size(), 10000, error);
        return std::string(errPayload.data(), errPayload.size());
    };
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = recvErrText(hdr.length);
        return false;
    }
    if (hdr.cmd != RSP_OK || hdr.length < sizeof(PullHeader)) return false;
    PullHeader ph{};
    {
        std::vector<char> okPayload(hdr.length);
        if (!net::recvAll(fd, okPayload.data(), okPayload.size(), kMcrawMs, error)) return false;
        std::memcpy(&ph, okPayload.data(), sizeof(ph));
    }
    outFileSize = ph.file_size;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(localPath).parent_path(), ec);
    const int outFd = ::open(localPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (outFd < 0) {
        m_lastError = "Failed to create local file: " + localPath;
        MsgHeader drainHdr{};
        while (net::recvAll(fd, &drainHdr, sizeof(drainHdr), 10000, error)) {
            if (drainHdr.cmd == RSP_DONE) break;
            if (drainHdr.cmd != RSP_DATA || drainHdr.length == 0) break;
            if (m_transferBuf.size() < drainHdr.length)
                m_transferBuf.resize(drainHdr.length);
            if (!net::recvAll(fd, m_transferBuf.data(), drainHdr.length, 10000, error)) break;
        }
        return false;
    }

    uint64_t received = 0;
    bool success = true;
    std::vector<char> bufA(AFM_CHUNK_SIZE), bufB(AFM_CHUNK_SIZE);
    char* recvBuf = bufA.data();
    std::future<bool> writeFut;
    while (received < outFileSize && success) {
        if (!net::recvAll(fd, &hdr, sizeof(hdr), 10000, error)) {
            success = false;
            break;
        }
        if (hdr.cmd == RSP_DONE) break;
        if (hdr.cmd != RSP_DATA) {
            success = false;
            break;
        }
        if (!net::recvAll(fd, recvBuf, hdr.length, 10000, error)) {
            success = false;
            break;
        }
        if (writeFut.valid() && !writeFut.get()) success = false;
        char* writeBuf = recvBuf;
        const uint32_t writeLen = hdr.length;
        writeFut = std::async(std::launch::async, [outFd, writeBuf, writeLen] {
            size_t off = 0;
            while (off < writeLen) {
                const ssize_t n = ::write(outFd, writeBuf + off, writeLen - off);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    return false;
                }
                off += static_cast<size_t>(n);
            }
            return true;
        });
        recvBuf = (recvBuf == bufA.data()) ? bufB.data() : bufA.data();
        received += hdr.length;
        if (progress && !progress(received, outFileSize)) {
            success = false;
            m_lastError = "Cancelled";
            break;
        }
    }
    if (writeFut.valid() && !writeFut.get()) success = false;
    ::close(outFd);

    // ALWAYS drain until RSP_DONE: the loop exits at received >= outFileSize
    // but the server still sends RSP_DONE, which would poison the next command.
    if (m_connected) {
        MsgHeader drainHdr{};
        while (net::recvAll(fd, &drainHdr, sizeof(drainHdr), 10000, error)) {
            if (drainHdr.cmd == RSP_DONE) break;
            if (drainHdr.cmd != RSP_DATA || drainHdr.length == 0) break;
            if (m_transferBuf.size() < drainHdr.length)
                m_transferBuf.resize(drainHdr.length);
            if (!net::recvAll(fd, m_transferBuf.data(), drainHdr.length, 10000, error)) break;
        }
    }
    return success;
}
