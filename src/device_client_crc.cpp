// Linux CRC32 verification (Stage B, part 2): slicing-by-8 CRC identical
// to the Windows client, GF(2) combine, disk re-read fallback, and the
// remote CRC/SHA-256 queries with size-scaled deadlines via runSmallOp
// (control channel when available, primary otherwise).
#include "device_client.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#include "log_facade.h"

namespace {
uint32_t g_crc32Table[8][256];
std::once_flag g_crc32InitFlag;

void initCrc32Table() {
    std::call_once(g_crc32InitFlag, []() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
            g_crc32Table[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = g_crc32Table[0][i];
            for (int s = 1; s < 8; ++s) {
                c = g_crc32Table[0][c & 0xFF] ^ (c >> 8);
                g_crc32Table[s][i] = c;
            }
        }
    });
}

// Processes 8 bytes per iteration; unaligned-safe via memcpy.
void crc32UpdateSlice8(uint32_t& crc, const void* data, size_t len) {
    initCrc32Table();
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len >= 8) {
        uint32_t a = 0, b = 0;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + 4, 4);
        a ^= crc;
        crc = g_crc32Table[7][(a) & 0xFF] ^ g_crc32Table[6][(a >> 8) & 0xFF] ^
            g_crc32Table[5][(a >> 16) & 0xFF] ^ g_crc32Table[4][(a >> 24)] ^
            g_crc32Table[3][(b) & 0xFF] ^ g_crc32Table[2][(b >> 8) & 0xFF] ^
            g_crc32Table[1][(b >> 16) & 0xFF] ^ g_crc32Table[0][(b >> 24)];
        p += 8;
        len -= 8;
    }
    while (len-- > 0) crc = g_crc32Table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
}

uint32_t computeLocalCrc32(const std::string& path, std::atomic<float>* progress = nullptr) {
    initCrc32Table();
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    std::error_code ec;
    const uint64_t totalSize = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
    uint64_t processed = 0;
    uint32_t crc = ~(uint32_t)0;
    std::vector<char> buf(4 * 1024 * 1024);
    for (;;) {
        ssize_t n = 0;
        do {
            n = ::read(fd, buf.data(), buf.size());
        } while (n < 0 && errno == EINTR);
        if (n <= 0) break;
        crc32UpdateSlice8(crc, buf.data(), static_cast<size_t>(n));
        processed += static_cast<uint64_t>(n);
        if (progress && totalSize > 0)
            progress->store(static_cast<float>(static_cast<double>(processed) /
                                               static_cast<double>(totalSize)));
    }
    if (progress) progress->store(1.0f);
    ::close(fd);
    return ~crc;
}

uint32_t scaledCrcTimeout(uint64_t fileSize) {
    // ~200MB/s device read + margin; 30s floor, 5min cap. Same as Windows.
    uint64_t timeout = 30000 + (fileSize / (100ULL * 1024 * 1024)) * 1000;
    return static_cast<uint32_t>(std::min<uint64_t>(timeout, 300000));
}
}  // namespace

void DeviceClient::inlineCrcUpdate(const void* data, size_t len) {
    crc32UpdateSlice8(m_inlineCrc, data, len);
}

void crc32Update(uint32_t& crc, const void* data, size_t len) {
    crc32UpdateSlice8(crc, data, len);
}

namespace {
uint32_t gf2MatrixTimes(const uint32_t* mat, uint32_t vec) {
    uint32_t sum = 0;
    for (int i = 0; vec; ++i, vec >>= 1)
        if (vec & 1) sum ^= mat[i];
    return sum;
}

void gf2MatrixSquare(uint32_t* sq, const uint32_t* mat) {
    for (int n = 0; n < 32; ++n) sq[n] = gf2MatrixTimes(mat, mat[n]);
}
}  // namespace

uint32_t crc32Combine(uint32_t crc1, uint32_t crc2, uint64_t len2) {
    if (len2 == 0) return crc1;
    uint32_t even[32], odd[32];
    odd[0] = 0xEDB88320;
    uint32_t row = 1;
    for (int n = 1; n < 32; ++n) {
        odd[n] = row;
        row <<= 1;
    }
    gf2MatrixSquare(even, odd);
    gf2MatrixSquare(odd, even);
    do {
        gf2MatrixSquare(even, odd);
        if (len2 & 1) crc1 = gf2MatrixTimes(even, crc1);
        len2 >>= 1;
        if (len2 == 0) break;
        gf2MatrixSquare(odd, even);
        if (len2 & 1) crc1 = gf2MatrixTimes(odd, crc1);
        len2 >>= 1;
    } while (len2 != 0);
    return crc1 ^ crc2;
}

bool DeviceClient::verifyFileCrc(const std::string& remotePath, const std::string& localPath,
                                 std::string& detail, std::atomic<float>* crcProgress,
                                 std::atomic<int>* crcPhase, double* remoteMs, double* localMs) {
    if (!m_connected) {
        detail = "Not connected";
        return false;
    }
    std::error_code ec;
    const uint64_t localFileSize =
        static_cast<uint64_t>(std::filesystem::file_size(localPath, ec));
    const uint32_t crcTimeout = scaledCrcTimeout(ec ? 0 : localFileSize);
    LOG_INFO("CRC", "verifyFileCrc: " + remotePath + " (size=" + std::to_string(localFileSize) +
                        " timeout=" + std::to_string(crcTimeout) + "ms)");

    if (crcPhase) crcPhase->store(1);
    if (crcProgress) crcProgress->store(0.0f);

    const auto remoteStart = std::chrono::steady_clock::now();
    uint32_t remoteCrc = 0;
    {
        MsgHeader hdr{};
        std::vector<char> payload;
        if (!runSmallOp(CMD_CRC32, remotePath.data(), static_cast<uint32_t>(remotePath.size()),
                        hdr, payload, crcTimeout)) {
            detail = "Failed to receive remote CRC";
            LOG_ERROR("CRC", detail);
            return false;
        }
        if (hdr.cmd == RSP_ERROR) {
            detail = "Server error: " + std::string(payload.data(), payload.size());
            LOG_ERROR("CRC", detail);
            return false;
        }
        if (hdr.cmd != RSP_OK || payload.size() < 4) {
            detail = "Invalid CRC response";
            return false;
        }
        std::memcpy(&remoteCrc, payload.data(), 4);
    }
    const auto remoteEnd = std::chrono::steady_clock::now();
    const double remoteDur =
        std::chrono::duration<double, std::milli>(remoteEnd - remoteStart).count();
    if (remoteMs) *remoteMs = remoteDur;
    LOG_INFO("CRC", "Remote CRC: " + std::to_string(remoteCrc) + " (" +
                        std::to_string(static_cast<int>(remoteDur)) + "ms) - checking local CRC...");

    if (crcPhase) crcPhase->store(2);
    if (crcProgress) crcProgress->store(0.0f);
    const auto localStart = std::chrono::steady_clock::now();
    uint32_t localCrc = 0;
    if (m_inlineCrcPath == localPath) {
        localCrc = m_inlineCrc;  // computed during transfer — zero extra disk I/O
        if (crcProgress) crcProgress->store(1.0f);
        LOG_INFO("CRC", "Using inline CRC (no re-read needed)");
    } else {
        LOG_INFO("CRC", "No inline CRC — reading file from disk");
        localCrc = computeLocalCrc32(localPath, crcProgress);
    }
    const auto localEnd = std::chrono::steady_clock::now();
    const double localDur =
        std::chrono::duration<double, std::milli>(localEnd - localStart).count();
    if (localMs) *localMs = localDur;

    char buf[128];
    ::snprintf(buf, sizeof(buf), "Local: %08X  Remote: %08X", localCrc, remoteCrc);
    detail = buf;
    LOG_INFO("CRC", std::string(buf) + (localCrc == remoteCrc ? " MATCH" : " MISMATCH") +
                        " (remote=" + std::to_string(static_cast<int>(remoteDur)) + "ms local=" +
                        std::to_string(static_cast<int>(localDur)) + "ms)");
    return localCrc == remoteCrc;
}

bool DeviceClient::getRemoteCrc32(const std::string& remotePath, uint32_t& outCrc,
                                  std::string& detail, uint64_t fileSize, double* remoteMs) {
    if (!m_connected) {
        detail = "Not connected";
        return false;
    }
    const uint32_t crcTimeout = scaledCrcTimeout(fileSize);
    LOG_INFO("CRC", "getRemoteCrc32: " + remotePath + " (size=" + std::to_string(fileSize) +
                        " timeout=" + std::to_string(crcTimeout) + "ms)");

    const auto remoteStart = std::chrono::steady_clock::now();
    MsgHeader hdr{};
    std::vector<char> payload;
    if (!runSmallOp(CMD_CRC32, remotePath.data(), static_cast<uint32_t>(remotePath.size()), hdr,
                    payload, crcTimeout)) {
        detail = "Failed to receive remote CRC";
        LOG_ERROR("CRC", detail);
        return false;
    }
    if (hdr.cmd == RSP_ERROR) {
        detail = "Server error: " + std::string(payload.data(), payload.size());
        LOG_ERROR("CRC", detail);
        return false;
    }
    if (hdr.cmd != RSP_OK || payload.size() < 4) {
        detail = "Invalid CRC response";
        return false;
    }
    std::memcpy(&outCrc, payload.data(), 4);
    const auto remoteEnd = std::chrono::steady_clock::now();
    const double remoteDur =
        std::chrono::duration<double, std::milli>(remoteEnd - remoteStart).count();
    if (remoteMs) *remoteMs = remoteDur;
    detail = "Remote: " + std::to_string(outCrc);
    LOG_INFO("CRC",
             "Remote CRC: " + std::to_string(outCrc) + " (" +
                 std::to_string(static_cast<int>(remoteDur)) + "ms)");
    return true;
}

bool DeviceClient::getRemoteSha256(const std::string& remotePath, std::string& outSha256,
                                    std::string& detail, uint64_t fileSize, double* remoteMs) {
    if (!m_connected) {
        detail = "Not connected";
        return false;
    }
    uint64_t timeout = 30000 + (fileSize / (50ULL * 1024 * 1024)) * 1000;
    const uint32_t hashTimeout = static_cast<uint32_t>(std::min<uint64_t>(timeout, 600000));
    LOG_INFO("SHA256", "getRemoteSha256: " + remotePath + " (size=" + std::to_string(fileSize) +
                           " timeout=" + std::to_string(hashTimeout) + "ms)");

    const auto remoteStart = std::chrono::steady_clock::now();
    MsgHeader hdr{};
    std::vector<char> payload;
    if (!runSmallOp(CMD_SHA256, remotePath.data(), static_cast<uint32_t>(remotePath.size()), hdr,
                    payload, hashTimeout)) {
        detail = "Failed to receive remote SHA-256";
        LOG_ERROR("SHA256", detail);
        return false;
    }
    if (hdr.cmd == RSP_ERROR) {
        detail = "Server error: " + std::string(payload.data(), payload.size());
        LOG_ERROR("SHA256", detail);
        return false;
    }
    if (hdr.cmd != RSP_OK || payload.size() < 32) {
        detail = "Invalid SHA-256 response";
        return false;
    }
    static const char* hex = "0123456789abcdef";
    outSha256.clear();
    outSha256.reserve(64);
    for (int i = 0; i < 32; ++i) {
        const auto b = static_cast<unsigned char>(payload[i]);
        outSha256.push_back(hex[b >> 4]);
        outSha256.push_back(hex[b & 0x0F]);
    }
    const auto remoteEnd = std::chrono::steady_clock::now();
    const double remoteDur =
        std::chrono::duration<double, std::milli>(remoteEnd - remoteStart).count();
    if (remoteMs) *remoteMs = remoteDur;
    detail = "Remote SHA-256: " + outSha256;
    LOG_INFO("SHA256", "Remote SHA-256: " + outSha256 + " (" +
                           std::to_string(static_cast<int>(remoteDur)) + "ms)");
    return true;
}
