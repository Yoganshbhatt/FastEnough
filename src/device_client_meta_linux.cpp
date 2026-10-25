// Linux-only metadata mutations added for the FUSE write backend.
// Kept out of the Windows translation unit because the Windows client does
// not expose these methods; the commands themselves are additive.
#include "device_client.h"

#include "protocol.h"

#include <climits>
#include <cstring>
#include <vector>

namespace {
bool validRemotePath(const std::string& path) {
    return !path.empty() && path.size() < PATH_MAX;
}
}  // namespace

uint32_t DeviceClient::capabilities() {
    if (!m_connected) {
        m_lastError = "Not connected";
        return 0;
    }
    MsgHeader hdr{};
    std::vector<char> payload;
    if (!runSmallOp(CMD_CAPABILITIES, nullptr, 0, hdr, payload)) return 0;
    if (hdr.cmd != RSP_OK || payload.size() < sizeof(uint32_t)) return 0;
    uint32_t flags = 0;
    std::memcpy(&flags, payload.data(), sizeof(flags));
    return flags;
}

bool DeviceClient::createFileEx(const std::string& remotePath, uint64_t totalSize,
                                bool truncate, bool exclusive) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    if (!validRemotePath(remotePath) || totalSize > static_cast<uint64_t>(INT64_MAX)) {
        m_lastError = "Invalid path or size";
        return false;
    }
    uint32_t flags = 0;
    if (truncate) flags |= AFM_CREATE_TRUNCATE;
    if (exclusive) flags |= AFM_CREATE_EXCLUSIVE;

    std::vector<char> request(12 + remotePath.size());
    std::memcpy(request.data(), &flags, 4);
    std::memcpy(request.data() + 4, &totalSize, 8);
    std::memcpy(request.data() + 12, remotePath.data(), remotePath.size());

    MsgHeader hdr{};
    std::vector<char> response;
    if (!runSmallOp(CMD_CREATE_FILE_EX, request.data(),
                    static_cast<uint32_t>(request.size()), hdr, response)) {
        return false;
    }
    if (hdr.cmd == RSP_ERROR) {
        m_lastError.assign(response.data(), response.size());
        return false;
    }
    return hdr.cmd == RSP_OK;
}

bool DeviceClient::truncateFile(const std::string& remotePath, uint64_t size) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    if (!validRemotePath(remotePath) || size > static_cast<uint64_t>(INT64_MAX)) {
        m_lastError = "Invalid path or size";
        return false;
    }
    std::vector<char> request(8 + remotePath.size());
    std::memcpy(request.data(), &size, 8);
    std::memcpy(request.data() + 8, remotePath.data(), remotePath.size());

    MsgHeader hdr{};
    std::vector<char> response;
    if (!runSmallOp(CMD_TRUNCATE, request.data(),
                    static_cast<uint32_t>(request.size()), hdr, response)) {
        return false;
    }
    if (hdr.cmd == RSP_ERROR) {
        m_lastError.assign(response.data(), response.size());
        return false;
    }
    return hdr.cmd == RSP_OK;
}

bool DeviceClient::renameFileEx(const std::string& oldPath, const std::string& newPath,
                                bool replace) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    if (!validRemotePath(oldPath) || !validRemotePath(newPath)) {
        m_lastError = "Invalid path";
        return false;
    }
    const uint32_t flags = replace ? AFM_RENAME_REPLACE : 0u;
    const uint32_t oldSize = static_cast<uint32_t>(oldPath.size());

    std::vector<char> request(8 + oldPath.size() + newPath.size());
    std::memcpy(request.data(), &flags, 4);
    std::memcpy(request.data() + 4, &oldSize, 4);
    std::memcpy(request.data() + 8, oldPath.data(), oldPath.size());
    std::memcpy(request.data() + 8 + oldPath.size(), newPath.data(), newPath.size());

    MsgHeader hdr{};
    std::vector<char> response;
    if (!runSmallOp(CMD_RENAME_EX, request.data(),
                    static_cast<uint32_t>(request.size()), hdr, response)) {
        return false;
    }
    if (hdr.cmd == RSP_ERROR) {
        m_lastError.assign(response.data(), response.size());
        return false;
    }
    return hdr.cmd == RSP_OK;
}

bool DeviceClient::unlinkFile(const std::string& remotePath) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    if (!validRemotePath(remotePath)) {
        m_lastError = "Invalid path";
        return false;
    }
    MsgHeader hdr{};
    std::vector<char> response;
    if (!runSmallOp(CMD_REMOVE_FILE, remotePath.data(),
                    static_cast<uint32_t>(remotePath.size()), hdr, response)) {
        return false;
    }
    if (hdr.cmd == RSP_ERROR) {
        m_lastError.assign(response.data(), response.size());
        return false;
    }
    return hdr.cmd == RSP_OK;
}

bool DeviceClient::rmdirDirectory(const std::string& remotePath) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    if (!validRemotePath(remotePath)) {
        m_lastError = "Invalid path";
        return false;
    }
    MsgHeader hdr{};
    std::vector<char> response;
    if (!runSmallOp(CMD_RMDIR, remotePath.data(),
                    static_cast<uint32_t>(remotePath.size()), hdr, response)) {
        return false;
    }
    if (hdr.cmd == RSP_ERROR) {
        m_lastError.assign(response.data(), response.size());
        return false;
    }
    return hdr.cmd == RSP_OK;
}
