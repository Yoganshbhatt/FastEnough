// Linux device-to-device relay (Stage C, part 1): relayFile / relayRange.
// Zero disk I/O — chunks stream from the source socket to the destination
// socket through PC memory. Same framing, buffering, drain discipline, and
// address-ordered locking as src/device_client.cpp.
#include "device_client.h"

#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <utility>
#include <vector>

#include "log_facade.h"

bool DeviceClient::relayFile(DeviceClient& src, DeviceClient& dst, const std::string& srcPath,
                             const std::string& dstPath, uint64_t fileSize,
                             ProgressCallback progress) {
    if (!src.m_connected) {
        src.m_lastError = "Source not connected";
        return false;
    }
    if (!dst.m_connected) {
        dst.m_lastError = "Destination not connected";
        return false;
    }
    LOG_INFO("Transfer", "relayFile START: " + srcPath + " -> " + dstPath);

    // Lock lower address first to prevent deadlock.
    DeviceClient* first = &src < &dst ? &src : &dst;
    DeviceClient* second = &src < &dst ? &dst : &src;
    const std::lock_guard<std::mutex> lk1(first->m_mutex);
    const std::lock_guard<std::mutex> lk2(second->m_mutex);

    if (!src.sendMsg(CMD_PULL, srcPath.data(), static_cast<uint32_t>(srcPath.size()))) {
        src.m_lastError = "Failed to send CMD_PULL";
        return false;
    }
    MsgHeader hdr{};
    std::vector<char> payload;
    if (!src.recvMsg(hdr, payload)) {
        src.m_lastError = "No response to CMD_PULL";
        return false;
    }
    if (hdr.cmd == RSP_ERROR) {
        src.m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    if (hdr.cmd != RSP_OK || payload.size() < sizeof(PullHeader)) {
        src.m_lastError = "Unexpected response to CMD_PULL";
        return false;
    }
    PullHeader ph{};
    std::memcpy(&ph, payload.data(), sizeof(ph));
    const uint64_t actualSize = ph.file_size;
    if (fileSize == 0) fileSize = actualSize;

    std::vector<char> pushPayload(sizeof(PushHeader) + dstPath.size());
    const PushHeader pushHdr{actualSize};
    std::memcpy(pushPayload.data(), &pushHdr, sizeof(pushHdr));
    std::memcpy(pushPayload.data() + sizeof(pushHdr), dstPath.data(), dstPath.size());

    auto drainSrc = [&]() {
        while (src.recvAll(&hdr, sizeof(hdr))) {
            if (hdr.cmd == RSP_DONE || hdr.cmd != RSP_DATA || hdr.length == 0) break;
            std::vector<char> drain(hdr.length);
            if (!src.recvAll(drain.data(), hdr.length)) break;
        }
    };
    if (!dst.sendMsg(CMD_PUSH, pushPayload.data(), static_cast<uint32_t>(pushPayload.size()))) {
        dst.m_lastError = "Failed to send CMD_PUSH";
        drainSrc();
        return false;
    }
    MsgHeader dstHdr{};
    std::vector<char> dstPayload;
    if (!dst.recvMsg(dstHdr, dstPayload) || dstHdr.cmd != RSP_OK) {
        dst.m_lastError = dstHdr.cmd == RSP_ERROR
            ? std::string(dstPayload.data(), dstPayload.size())
            : "Destination rejected CMD_PUSH";
        drainSrc();
        return false;
    }

    // Double-buffered relay: recv into one buffer while async-sending the other.
    const size_t bufSize = AFM_CHUNK_SIZE + sizeof(MsgHeader);
    std::vector<char> bufA(bufSize), bufB(bufSize);
    char* recvBuf = bufA.data();
    uint64_t relayed = 0;
    bool success = true;
    std::future<bool> sendFut;

    while (true) {
        if (!src.recvAll(&hdr, sizeof(hdr))) {
            success = false;
            src.m_lastError = "recv header failed";
            break;
        }
        if (hdr.cmd == RSP_DONE) break;
        if (hdr.cmd != RSP_DATA || hdr.length == 0) {
            success = false;
            src.m_lastError = "unexpected response in relay";
            break;
        }
        if (!src.recvAll(recvBuf + sizeof(MsgHeader), hdr.length)) {
            success = false;
            src.m_lastError = "recv data failed";
            break;
        }
        if (sendFut.valid() && !sendFut.get()) {
            success = false;
            dst.m_lastError = "send to dst failed";
            break;
        }
        auto* fwdHdr = reinterpret_cast<MsgHeader*>(recvBuf);
        fwdHdr->cmd = RSP_DATA;
        fwdHdr->length = hdr.length;
        char* sendBuf = recvBuf;
        const uint32_t sendLen = sizeof(MsgHeader) + hdr.length;
        sendFut = std::async(std::launch::async, [&dst, sendBuf, sendLen] {
            return dst.sendAll(sendBuf, sendLen);
        });
        relayed += hdr.length;
        if (progress && !progress(relayed, fileSize)) {
            success = false;
            src.m_lastError = "Cancelled";
            break;
        }
        recvBuf = (recvBuf == bufA.data()) ? bufB.data() : bufA.data();
    }
    if (sendFut.valid() && !sendFut.get()) {
        success = false;
        dst.m_lastError = "send to dst failed";
    }

    dst.sendMsg(RSP_DONE);
    if (dst.recvMsg(dstHdr, dstPayload) && success && dstHdr.cmd == RSP_ERROR) {
        dst.m_lastError = std::string(dstPayload.data(), dstPayload.size());
        success = false;
    }
    if (!success && src.m_connected) drainSrc();

    if (success) {
        LOG_INFO("Transfer", "relayFile OK: " + std::to_string(relayed) + " bytes relayed");
    } else {
        LOG_ERROR("Transfer",
                  "relayFile FAILED: src=" + src.m_lastError + " dst=" + dst.m_lastError);
    }
    return success;
}

bool DeviceClient::relayRange(DeviceClient& src, DeviceClient& dst, const std::string& srcPath,
                              const std::string& dstPath, uint64_t srcOffset, uint64_t dstOffset,
                              uint64_t length, ProgressCallback progress, RelayRangeStats* stats) {
    if (!src.m_connected) {
        src.m_lastError = "Source not connected";
        return false;
    }
    if (!dst.m_connected) {
        dst.m_lastError = "Destination not connected";
        return false;
    }
    if (length == 0) return true;

    DeviceClient* first = &src < &dst ? &src : &dst;
    DeviceClient* second = &src < &dst ? &dst : &src;
    const std::lock_guard<std::mutex> lk1(first->m_mutex);
    const std::lock_guard<std::mutex> lk2(second->m_mutex);

    std::vector<char> readPayload(16 + srcPath.size());
    std::memcpy(readPayload.data(), &srcOffset, 8);
    std::memcpy(readPayload.data() + 8, &length, 8);
    std::memcpy(readPayload.data() + 16, srcPath.data(), srcPath.size());
    if (!src.sendMsg(CMD_READ_RANGE, readPayload.data(),
                     static_cast<uint32_t>(readPayload.size()))) {
        src.m_lastError = "Failed to send CMD_READ_RANGE";
        return false;
    }
    MsgHeader srcHdr{};
    std::vector<char> srcResp;
    if (!src.recvMsg(srcHdr, srcResp)) {
        src.m_lastError = "No response to CMD_READ_RANGE";
        return false;
    }
    if (srcHdr.cmd == RSP_ERROR) {
        src.m_lastError = std::string(srcResp.data(), srcResp.size());
        return false;
    }
    if (srcHdr.cmd != RSP_OK || srcResp.size() < 8) {
        src.m_lastError = "Unexpected response to CMD_READ_RANGE";
        return false;
    }
    uint64_t actualLen = 0;
    std::memcpy(&actualLen, srcResp.data(), 8);
    actualLen = std::min(actualLen, length);
    if (actualLen == 0) return true;

    std::vector<char> writePayload(16 + dstPath.size());
    std::memcpy(writePayload.data(), &dstOffset, 8);
    std::memcpy(writePayload.data() + 8, &actualLen, 8);
    std::memcpy(writePayload.data() + 16, dstPath.data(), dstPath.size());

    auto drainSrc = [&]() {
        while (src.recvAll(&srcHdr, sizeof(srcHdr))) {
            if (srcHdr.cmd == RSP_DONE) break;
            if (srcHdr.cmd != RSP_DATA || srcHdr.length == 0) break;
            std::vector<char> drain(srcHdr.length);
            if (!src.recvAll(drain.data(), srcHdr.length)) break;
        }
    };
    if (!dst.sendMsg(CMD_WRITE_RANGE, writePayload.data(),
                     static_cast<uint32_t>(writePayload.size()))) {
        dst.m_lastError = "Failed to send CMD_WRITE_RANGE";
        drainSrc();
        return false;
    }
    MsgHeader dstHdr{};
    std::vector<char> dstResp;
    if (!dst.recvMsg(dstHdr, dstResp)) {
        dst.m_lastError = "No response to CMD_WRITE_RANGE";
        return false;
    }
    if (dstHdr.cmd == RSP_ERROR) {
        dst.m_lastError = std::string(dstResp.data(), dstResp.size());
        return false;
    }
    if (dstHdr.cmd != RSP_OK) {
        dst.m_lastError = "Destination rejected CMD_WRITE_RANGE";
        return false;
    }

    const size_t bufSize = AFM_CHUNK_SIZE + sizeof(MsgHeader);
    std::vector<char> bufA(bufSize), bufB(bufSize);
    char* recvBuf = bufA.data();
    std::future<std::pair<bool, double>> sendFut;
    uint32_t inFlightLen = 0;
    bool success = true;
    uint64_t relayed = 0;
    uint64_t sentToDst = 0;
    uint32_t srcRangeCrc = ~(uint32_t)0;

    auto finishSend = [&]() -> bool {
        if (!sendFut.valid()) return true;
        auto result = sendFut.get();
        if (stats) stats->dstSendSec += result.second;
        if (!result.first) {
            dst.m_lastError = "send range data failed";
            return false;
        }
        sentToDst += inFlightLen;
        if (progress && !progress(sentToDst, actualLen)) {
            src.m_lastError = "Cancelled";
            return false;
        }
        inFlightLen = 0;
        return true;
    };

    while (success) {
        const auto recvStart = std::chrono::steady_clock::now();
        if (!src.recvAll(&srcHdr, sizeof(srcHdr))) {
            success = false;
            src.m_lastError = "recv range header failed";
            break;
        }
        if (srcHdr.cmd == RSP_DONE) break;
        if (srcHdr.cmd != RSP_DATA || srcHdr.length == 0 || srcHdr.length > AFM_CHUNK_SIZE) {
            success = false;
            src.m_lastError = "unexpected response in range relay";
            break;
        }
        if (!src.recvAll(recvBuf + sizeof(MsgHeader), srcHdr.length)) {
            success = false;
            src.m_lastError = "recv range data failed";
            break;
        }
        if (stats)
            stats->srcRecvSec += std::chrono::duration<double>(
                                     std::chrono::steady_clock::now() - recvStart)
                                     .count();
        crc32Update(srcRangeCrc, recvBuf + sizeof(MsgHeader), srcHdr.length);

        if (!finishSend()) {
            success = false;
            break;
        }
        auto* fwdHdr = reinterpret_cast<MsgHeader*>(recvBuf);
        fwdHdr->cmd = RSP_DATA;
        fwdHdr->length = srcHdr.length;
        relayed += srcHdr.length;

        char* sendBuf = recvBuf;
        const uint32_t sendLen = sizeof(MsgHeader) + srcHdr.length;
        inFlightLen = srcHdr.length;
        sendFut = std::async(std::launch::async, [&dst, sendBuf, sendLen] {
            const auto sendStart = std::chrono::steady_clock::now();
            const bool ok = dst.sendAll(sendBuf, sendLen);
            const double sendSec =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - sendStart)
                    .count();
            return std::make_pair(ok, sendSec);
        });
        recvBuf = (recvBuf == bufA.data()) ? bufB.data() : bufA.data();
    }
    if (!finishSend()) success = false;

    dst.sendMsg(RSP_DONE);
    if (dst.recvMsg(dstHdr, dstResp)) {
        if (success && dstHdr.cmd == RSP_ERROR) {
            dst.m_lastError = std::string(dstResp.data(), dstResp.size());
            success = false;
        } else if (success && dstHdr.cmd == RSP_OK && dstResp.size() >= 12 && stats) {
            uint64_t dstWritten = 0;
            uint32_t dstCrc = 0;
            std::memcpy(&dstWritten, dstResp.data(), 8);
            std::memcpy(&dstCrc, dstResp.data() + 8, 4);
            if (dstWritten >= actualLen) {
                stats->dstCrc = dstCrc;
                stats->hasDstCrc = true;
            }
        }
    } else if (success) {
        dst.m_lastError = "No write confirmation";
        success = false;
    }

    if (!success && src.m_connected) drainSrc();

    if (stats) {
        stats->bytes += sentToDst;
        if (success && sentToDst >= actualLen && relayed >= actualLen) {
            stats->srcCrc = ~srcRangeCrc;
            stats->hasSrcCrc = true;
        }
    }
    return success && sentToDst >= actualLen && relayed >= actualLen;
}
