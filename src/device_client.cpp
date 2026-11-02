#define _CRT_SECURE_NO_WARNINGS
#include "device_client.h"
#include "app.h" // for LOG macros
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <sstream>
#include <charconv>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <mutex>
#include <fstream>
#include <iomanip>
#include <unordered_map>
#include <nlohmann/json.hpp>

#pragma comment(lib, "ws2_32.lib")

// UTF-8 to wide string helper
static std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wide.data(), len);
    return wide;
}

// UTF-8 string to std::filesystem::path (via wide string to avoid ANSI codepage issues)
static std::filesystem::path toFsPath(const std::string& utf8) {
    return std::filesystem::path(toWide(utf8));
}

static std::string wideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), utf8.data(), len, nullptr, nullptr);
    return utf8;
}

static std::string pathToUtf8(const std::filesystem::path& p) {
    return wideToUtf8(p.wstring());
}

static struct WsaInit {
    WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); }
    ~WsaInit() { WSACleanup(); }
} g_wsaInit;

static std::string sanitizeAdbOutput(std::string text);

static std::string lastWin32ErrorText() {
    DWORD error = GetLastError();
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::string result = message ? wideToUtf8(message) :
        "Windows error " + std::to_string(error);
    if (message) LocalFree(message);
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == '.')) result.pop_back();
    return result;
}

class TransferWorker {
public:
    TransferWorker() : m_thread([this] {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            m_cv.wait(lock, [this] { return m_pending || m_stop; });
            if (m_stop) return;
            auto task = std::move(m_task);
            lock.unlock();
            bool result = false;
            try { result = task(); } catch (...) {}
            lock.lock();
            m_result = result;
            m_pending = false;
            m_cv.notify_all();
        }
    }) {}

    ~TransferWorker() {
        wait();
        { std::lock_guard<std::mutex> lock(m_mutex); m_stop = true; }
        m_cv.notify_all();
        m_thread.join();
    }

    void submit(std::function<bool()> task) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_task = std::move(task);
        m_pending = true;
        m_cv.notify_all();
    }

    bool wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return !m_pending; });
        return m_result;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::function<bool()> m_task;
    bool m_pending = false;
    bool m_stop = false;
    bool m_result = true;
    std::thread m_thread;
};

struct FileHandleCloser {
    void operator()(void* file) const { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
};
using TransferFileHandle = std::unique_ptr<void, FileHandleCloser>;

static TransferFileHandle openTransferSource(const std::string& path, uint64_t offset,
                                             uint64_t length, std::string& error, bool wholeFile = false) {
    TransferFileHandle file(CreateFileW(toWide(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        error = "Failed to open local file: " + path + " (" + lastWin32ErrorText() + ")";
        return {};
    }
    LARGE_INTEGER size{}, position{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 ||
        offset > (uint64_t)size.QuadPart || length > (uint64_t)size.QuadPart - offset ||
        (wholeFile && length != (uint64_t)size.QuadPart)) {
        error = "Source size changed before the transfer";
        return {};
    }
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN)) {
        error = "Failed to seek local file: " + lastWin32ErrorText();
        return {};
    }
    return file;
}

DeviceClient::DeviceClient() {
    // Don't call findAdb() here - it runs a process which blocks
    // ADB will be found on first use by the background poll thread
}

DeviceClient::~DeviceClient() {
    if (m_ownsServer) stopServer();
    else disconnectTcp();
}

bool DeviceClient::findAdb() {
    auto tryPath = [&](const std::filesystem::path& p) -> bool {
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) {
            m_adbPath = pathToUtf8(p);
            return true;
        }
        return false;
    };

    if (!m_adbPath.empty() && tryPath(toFsPath(m_adbPath))) return true;
    m_adbPath.clear();

    // Check bundled ADB first.
    wchar_t exePath[32768];
    DWORD exePathLen = GetModuleFileNameW(nullptr, exePath, 32768);
    if (exePathLen > 0 && exePathLen < 32768) {
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        if (tryPath(exeDir / "platform-tools" / "adb.exe")) return true;
        if (tryPath(exeDir / "adb.exe")) return true;
    }

    // SDK paths
    if (const wchar_t* la = _wgetenv(L"LOCALAPPDATA"))
        if (tryPath(std::filesystem::path(la) / "Android" / "Sdk" / "platform-tools" / "adb.exe")) return true;
    if (const wchar_t* ah = _wgetenv(L"ANDROID_HOME"))
        if (tryPath(std::filesystem::path(ah) / "platform-tools" / "adb.exe")) return true;
    if (const wchar_t* sr = _wgetenv(L"ANDROID_SDK_ROOT"))
        if (tryPath(std::filesystem::path(sr) / "platform-tools" / "adb.exe")) return true;

    wchar_t searchPath[32768];
    DWORD searchLength = SearchPathW(nullptr, L"adb.exe", nullptr, 32768, searchPath, nullptr);
    if (searchLength > 0 && searchLength < 32768 && tryPath(searchPath)) return true;
    return false;
}

std::string DeviceClient::runProcess(const std::string& command) {
    ProcessOptions options;
    options.commandLine = toWide(command);
    options.shouldCancel = [this]() { return m_cancelCommands.load(); };
    auto result = runChildProcess(options);
    if (!result.succeeded()) return "error: Command failed. " + result.diagnostics();
    return result.standardOutput + result.standardError;
}

bool DeviceClient::runProcessToFile(const std::string& command, const std::string& outputPath, std::string& output,
                                    uint32_t timeoutMs, ProgressCallback progress, uint64_t totalBytes) {
    ProcessOptions options;
    options.commandLine = toWide(command);
    options.outputPath = toWide(outputPath);
    options.timeoutMs = timeoutMs ? timeoutMs : 30 * 60 * 1000;
    options.shouldCancel = [this]() { return m_cancelCommands.load(); };
    if (progress) options.progress = [progress, totalBytes](uint64_t bytes) { return progress(bytes, totalBytes); };
    auto result = runChildProcess(options);
    output = result.diagnostics();
    if (!result.succeeded()) LOG_ERROR("Process", "Streaming command failed: " + sanitizeAdbOutput(output));
    return result.succeeded();
}

bool DeviceClient::runProcessFromFile(const std::string& command, const std::string& inputPath, std::string& output,
                                      uint32_t timeoutMs) {
    ProcessOptions options;
    options.commandLine = toWide(command);
    options.inputPath = toWide(inputPath);
    options.timeoutMs = timeoutMs ? timeoutMs : 30 * 60 * 1000;
    options.shouldCancel = [this]() { return m_cancelCommands.load(); };
    auto result = runChildProcess(options);
    output = result.succeeded() ? result.standardOutput + result.standardError : result.diagnostics();
    return result.succeeded();
}

ProcessResult DeviceClient::runAdbCommandResult(const std::string& args, uint32_t timeoutMs,
                                               std::function<bool()> shouldCancel) {
    const bool pairing = args.starts_with("pair ");
    LOG_DEBUG("ADB", "$ adb " + (pairing ? std::string("pair [redacted]") : args));
    ProcessOptions options;
    options.commandLine = toWide("\"" + m_adbPath + "\" " + args);
    options.timeoutMs = timeoutMs;
    options.shouldCancel = [this, shouldCancel]() {
        return m_cancelCommands.load() || (shouldCancel && shouldCancel());
    };
    auto result = runChildProcess(options);
    std::string logged = result.succeeded() ? result.standardOutput + result.standardError : result.diagnostics();
    while (!logged.empty() && (logged.back() == '\r' || logged.back() == '\n')) logged.pop_back();
    if (logged.size() > 200) logged = logged.substr(0, 200) + "...";
    if (!logged.empty() && !pairing) LOG_DEBUG("ADB", "  -> " + logged);
    return result;
}

std::string DeviceClient::runAdbCommand(const std::string& args, uint32_t timeoutMs) {
    auto result = runAdbCommandResult(args, timeoutMs);
    if (!result.succeeded()) return "error: Command failed. " + result.diagnostics();
    if (result.outputTruncated) return "error: Command output exceeded the capture limit.";
    return result.standardOutput + result.standardError;
}

std::vector<DeviceInfo> DeviceClient::getDevices() {
    if (m_adbPath.empty()) return {};
    return parseAdbDevices(runAdbCommand("devices -l"));
}

static bool adbProtoSend(SOCKET sock, const std::string& cmd) {
    char lenBuf[5];
    snprintf(lenBuf, sizeof(lenBuf), "%04x", (unsigned)cmd.size());
    std::string msg = std::string(lenBuf) + cmd;
    int total = (int)msg.size();
    const char* p = msg.c_str();
    while (total > 0) {
        int n = send(sock, p, total, 0);
        if (n <= 0) return false;
        p += n; total -= n;
    }
    return true;
}

static bool adbProtoRecvStatus(SOCKET sock) {
    char status[4];
    int total = 0;
    while (total < 4) {
        int n = recv(sock, status + total, 4 - total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return memcmp(status, "OKAY", 4) == 0;
}

static std::string adbProtoRecvPayload(SOCKET sock) {
    // Read 4-byte hex length, then payload
    char lenBuf[5] = {};
    int total = 0;
    while (total < 4) {
        int n = recv(sock, lenBuf + total, 4 - total, 0);
        if (n <= 0) {
            // A partial frame cannot be resumed as a new length header.
            if (n == 0 || total > 0) WSASetLastError(WSAECONNRESET);
            return "";
        }
        total += n;
    }
    unsigned len = 0;
    auto [end, error] = std::from_chars(lenBuf, lenBuf + 4, len, 16);
    if (error != std::errc{} || end != lenBuf + 4) {
        WSASetLastError(WSAEPROTONOSUPPORT);
        return "";
    }
    if (len == 0) return " "; // empty device list (return non-empty to distinguish from error)
    std::string payload(len, '\0');
    total = 0;
    while ((unsigned)total < len) {
        int n = recv(sock, &payload[total], len - total, 0);
        if (n <= 0) {
            WSASetLastError(WSAECONNRESET);
            return "";
        }
        total += n;
    }
    return payload;
}

uintptr_t DeviceClient::openTrackDevices() {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return (uintptr_t)INVALID_SOCKET;
    DWORD handshakeTimeout = 3000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&handshakeTimeout, sizeof(handshakeTimeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&handshakeTimeout, sizeof(handshakeTimeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5037); // ADB server port
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        LOG_WARN("ADB", "track-devices: cannot connect to ADB server");
        closesocket(sock);
        return (uintptr_t)INVALID_SOCKET;
    }

    if (!adbProtoSend(sock, "host:track-devices-l")) {
        LOG_WARN("ADB", "track-devices: send failed");
        closesocket(sock);
        return (uintptr_t)INVALID_SOCKET;
    }

    if (!adbProtoRecvStatus(sock)) {
        LOG_WARN("ADB", "track-devices: server rejected");
        closesocket(sock);
        return (uintptr_t)INVALID_SOCKET;
    }

    LOG_INFO("ADB", "track-devices: connected — waiting for device events");
    return (uintptr_t)sock;
}

std::vector<DeviceInfo> DeviceClient::readTrackDevicesUpdate(uintptr_t sockHandle, bool* receivedUpdate) {
    std::vector<DeviceInfo> devices;
    SOCKET sock = (SOCKET)sockHandle;
    if (receivedUpdate) *receivedUpdate = false;

    std::string payload = adbProtoRecvPayload(sock);
    if (payload.empty()) {
        // Real error or timeout — caller checks WSAGetLastError
        return devices;
    }

    if (payload == " ") {
        // Empty device list is a valid update, not a socket failure.
        if (receivedUpdate) *receivedUpdate = true;
        LOG_INFO("ADB", "track-devices: all devices disconnected");
        return devices;
    }

    if (receivedUpdate) *receivedUpdate = true;

    return parseAdbDevices(payload);
}

void DeviceClient::closeTrackDevices(uintptr_t sock) {
    if (sock != (uintptr_t)INVALID_SOCKET)
        closesocket((SOCKET)sock);
}

std::string DeviceClient::getServerBinaryPath() {
    static std::mutex extractionMutex;
    std::lock_guard<std::mutex> extractionLock(extractionMutex);
    // First check next to the exe (for development)
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::string beside = (exeDir / "afm-server").string();
    if (std::filesystem::exists(beside)) return beside;

    // Extract from embedded resource to temp directory
    char tempDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tempDir);
    std::string extractDir = std::string(tempDir) + "FastEnough";
    CreateDirectoryA(extractDir.c_str(), nullptr);
    std::string extractPath = extractDir + "\\afm-server";

    HRSRC hRes = FindResource(nullptr, MAKEINTRESOURCE(200), RT_RCDATA);
    if (!hRes) return beside; // no embedded resource, fall back

    DWORD resSize = SizeofResource(nullptr, hRes);
    HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) return beside;
    const char* data = (const char*)LockResource(hData);
    if (!data) return beside;
    bool needExtract = true;
    {
        std::ifstream cached(toFsPath(extractPath), std::ios::binary | std::ios::ate);
        if (cached && cached.tellg() == (std::streamoff)resSize) {
            cached.seekg(0);
            std::vector<char> buffer(65536);
            size_t offset = 0;
            while (offset < resSize) {
                size_t count = std::min<size_t>(buffer.size(), resSize - offset);
                if (!cached.read(buffer.data(), (std::streamsize)count) ||
                    memcmp(buffer.data(), data + offset, count) != 0) break;
                offset += count;
            }
            needExtract = offset != resSize;
        }
    }

    if (needExtract) {
        std::string temporary = extractPath + "." + std::to_string(GetCurrentProcessId()) + ".tmp";
        HANDLE hFile = CreateFileW(toWide(temporary).c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) { m_lastError = "Could not extract Android helper"; return beside; }
        DWORD written = 0;
        bool success = WriteFile(hFile, data, resSize, &written, nullptr) && written == resSize;
        CloseHandle(hFile);
        if (!success || !MoveFileExW(toWide(temporary).c_str(), toWide(extractPath).c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(toWide(temporary).c_str());
            m_lastError = "Could not update Android helper";
            return beside;
        }
        LOG_INFO("Server", "Extracted embedded afm-server (" + std::to_string(resSize) + " bytes)");
    }

    return extractPath;
}

static std::string getAppLabelsDexPath() {
    char tempDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tempDir);
    std::string extractDir = std::string(tempDir) + "FastEnough";
    CreateDirectoryA(extractDir.c_str(), nullptr);
    std::string extractPath = extractDir + "\\afm-labels.dex";

    HRSRC resource = FindResource(nullptr, MAKEINTRESOURCE(201), RT_RCDATA);
    if (!resource) return {};
    DWORD resourceSize = SizeofResource(nullptr, resource);
    bool extract = true;
    std::error_code ec;
    if (std::filesystem::exists(extractPath, ec) && !ec)
        extract = std::filesystem::file_size(extractPath, ec) != resourceSize;
    if (!extract && !ec) return extractPath;

    HGLOBAL loaded = LoadResource(nullptr, resource);
    void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data) return {};
    HANDLE file = CreateFileW(toWide(extractPath).c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    DWORD written = 0;
    bool ok = WriteFile(file, data, resourceSize, &written, nullptr) && written == resourceSize;
    CloseHandle(file);
    return ok ? extractPath : std::string{};
}

// Detect device's network IP for direct connection (USB tethering only)
std::string DeviceClient::detectDeviceIp(const std::string& serial) {
    // Only check USB tethering interfaces — wlan0 is NOT a direct USB connection
    const char* interfaces[] = { "rndis0", "usb0", "ncm0", nullptr };

    for (int i = 0; interfaces[i]; i++) {
        std::string out = runAdbCommand("-s " + serial + " shell \"ip -4 addr show " +
                                         interfaces[i] + " 2>/dev/null\"");
        // Parse "inet X.X.X.X/YY" from output
        auto inetPos = out.find("inet ");
        if (inetPos != std::string::npos) {
            inetPos += 5;
            auto slashPos = out.find('/', inetPos);
            auto spacePos = out.find(' ', inetPos);
            auto endPos = std::min(slashPos, spacePos);
            if (endPos != std::string::npos && endPos > inetPos) {
                std::string ip = out.substr(inetPos, endPos - inetPos);
                // Validate it looks like an IP and isn't loopback
                if (ip.find('.') != std::string::npos && ip != "127.0.0.1") {
                    return ip;
                }
            }
        }
    }
    return ""; // no reachable IP found
}

bool DeviceClient::tryDirectConnect(const std::string& ip, int port) {
    if (ip.empty()) return false;
    disconnectTcp();
    if (connectTcp(ip, port) && verifyConnection()) {
        m_directConnection = true;
        m_serverPort = port;
        m_deviceIp = ip;
        publishConnectionProfile();
        return true;
    }
    disconnectTcp();
    return false;
}

bool DeviceClient::verifyConnection() {
    std::lock_guard<std::mutex> lk(m_mutex);
    // Use a short recv timeout for PING so we don't block m_mutex for 10s
    SOCKET sock = (SOCKET)m_socket;
    DWORD shortTimeout = 3000; // 3s is plenty for a PING response
    DWORD origTimeout = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&shortTimeout, sizeof(shortTimeout));

    LOG_DEBUG("TCP", "Sending PING to verify connection...");
    if (!sendMsg(CMD_PING)) {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
        LOG_ERROR("TCP", "PING send failed: " + m_lastError); return false;
    }
    MsgHeader hdr; std::vector<char> payload;
    if (!recvMsg(hdr, payload)) {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
        LOG_ERROR("TCP", "PING recv failed: " + m_lastError); return false;
    }
    // Restore normal timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));

    if (hdr.cmd != RSP_OK || payload.size() < 8) {
        m_lastError = "Helper did not report a compatible protocol version";
        LOG_ERROR("TCP", "PING bad response: cmd=" + std::to_string(hdr.cmd) + " len=" + std::to_string(payload.size()));
        return false;
    }
    uint32_t magic;
    memcpy(&magic, payload.data(), 4);
    if (magic != AFM_MAGIC) { LOG_ERROR("TCP", "PING wrong magic: 0x" + std::to_string(magic)); return false; }
    // Every supported helper reports its protocol version.
    if (payload.size() >= 8) {
        uint32_t serverVersion;
        memcpy(&serverVersion, payload.data() + 4, 4);
        if (serverVersion != AFM_VERSION) {
            m_lastError = "Protocol version mismatch: client=" + std::to_string(AFM_VERSION) +
                          " server=" + std::to_string(serverVersion);
            LOG_ERROR("TCP", m_lastError);
            return false;
        }
    }
    m_serverIsRoot = false;
    if (payload.size() >= 12) {
        uint32_t effectiveUid;
        memcpy(&effectiveUid, payload.data() + 8, 4);
        m_serverIsRoot = effectiveUid == 0;
    }
    LOG_INFO("TCP", "PING OK (payload " + std::to_string(payload.size()) + " bytes, root=" +
        std::string(m_serverIsRoot ? "Y" : "N") + ")");
    return true;
}

bool DeviceClient::isRootAvailable(const std::string& serial) {
    std::string out = runAdbCommand("-s " + serial + " shell \"su -c id 2>/dev/null\"");
    return out.find("uid=0") != std::string::npos;
}

// Wrap a shell command in `su -c '...'` when root mode is on. Uses single
// quotes inside the outer double-quoted adb shell string.
static std::string wrapSu(const std::string& innerCmd, bool useRoot) {
    if (!useRoot) return innerCmd;
    return "su -c '" + innerCmd + "'";
}

static std::string serverLaunchCommand(int port, bool useRoot) {
    return wrapSu("nohup /data/local/tmp/afm-server " + std::to_string(port) +
        " > /data/local/tmp/afm-server.log 2>&1 &", useRoot);
}

void DeviceClient::stopDeviceServerProcesses(const std::string& serial) {
    runAdbCommand("-s " + serial +
        " shell \"killall afm-server 2>/dev/null; su -c 'killall afm-server' 2>/dev/null\"");

    // An installer spawned by an older helper may have inherited its listening socket.
    // Stop every process that still owns the reserved server port before relaunching.
    std::string stopPortOwners =
        "for pid in $(ss -ltnp 2>/dev/null | grep :" + std::to_string(AFM_PORT) +
        " | grep -o pid=[0-9]* | cut -d= -f2); do kill -9 $pid; done";
    runAdbCommand("-s " + serial + " shell \"" + stopPortOwners + " 2>/dev/null\"");
    runAdbCommand("-s " + serial + " shell \"su -c '" + stopPortOwners + "' 2>/dev/null\"");
}

static std::string sanitizeAdbOutput(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
    return text;
}

static std::string numericValueAfter(const std::string& text, const std::string& marker) {
    const size_t markerPosition = text.find(marker);
    if (markerPosition == std::string::npos) return {};
    size_t position = markerPosition + marker.size();
    while (position < text.size() && !std::isdigit((unsigned char)text[position])) ++position;
    const size_t start = position;
    while (position < text.size() && std::isdigit((unsigned char)text[position])) ++position;
    return text.substr(start, position - start);
}

static std::string friendlyRestoreFailure(const std::string& rawOutput,
                                          const std::string& appName,
                                          const std::string& packageName,
                                          bool allowDowngrade) {
    const std::string heading = (appName.empty() ? packageName : appName) +
        (appName.empty() || appName == packageName ? "" : " (" + packageName + ")");
    std::string result = heading + "\n";
    if (rawOutput.find("INSTALL_FAILED_VERSION_DOWNGRADE") != std::string::npos) {
        result += "The APK was not installed because the backup contains an older app version than the device.\n";
        const std::string backupVersion = numericValueAfter(rawOutput, "Update version code");
        const std::string installedVersion = numericValueAfter(rawOutput, "older than current");
        if (!backupVersion.empty()) result += "Backup version code: " + backupVersion + "\n";
        if (!installedVersion.empty()) result += "Installed version code: " + installedVersion + "\n";
        result += allowDowngrade
            ? "Android rejected the downgrade. Uninstall the newer version first, then restore again."
            : "Enable Allow downgrade and retry, or uninstall the newer version first.";
    } else if (rawOutput.find("INSTALL_FAILED_UPDATE_INCOMPATIBLE") != std::string::npos) {
        result += "The installed app has a different signing certificate from the backup.\n";
        result += "Uninstall the installed version before restoring this backup.";
    } else if (rawOutput.find("INSTALL_FAILED_INSUFFICIENT_STORAGE") != std::string::npos) {
        result += "The device does not have enough free storage to install this app.";
    } else if (rawOutput.find("INSTALL_FAILED_MISSING_SPLIT") != std::string::npos) {
        result += "The backup is missing one or more required split APK files.";
    } else if (rawOutput.find("INSTALL_FAILED_INVALID_APK") != std::string::npos ||
               rawOutput.find("INSTALL_PARSE_FAILED") != std::string::npos) {
        result += "Android rejected the backed-up APK because it is invalid or incomplete.";
    } else if (rawOutput.find("INSTALL_FAILED_USER_RESTRICTED") != std::string::npos) {
        result += "Android blocked the installation because app installation is restricted on this device.";
    } else if (rawOutput.find("INSTALL_FAILED_CONFLICTING_PROVIDER") != std::string::npos) {
        result += "The app conflicts with a content provider owned by another installed app.";
    } else if (rawOutput.find("data.tar is missing") != std::string::npos ||
               rawOutput.find("data_de.tar is missing") != std::string::npos) {
        result += "The backup is missing one or more selected app-data archives.";
    } else {
        const size_t failurePosition = rawOutput.find("Failure [");
        if (failurePosition != std::string::npos) {
            size_t end = rawOutput.find_first_of("\r\n", failurePosition);
            result += "Android reported: " + rawOutput.substr(failurePosition,
                end == std::string::npos ? std::string::npos : end - failurePosition);
        } else {
            result += "The app could not be restored.";
            const std::string cleaned = sanitizeAdbOutput(rawOutput);
            if (!cleaned.empty()) result += "\nDevice response: " + cleaned;
        }
    }
    return result;
}

static std::string quoteCmdArg(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}

static std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

static std::string sanitizePathPart(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') out.push_back(c);
        else out.push_back('_');
    }
    return out.empty() ? "unknown" : out;
}

static std::string nowBackupStamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &tt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

static uint64_t directorySizeBytes(const std::filesystem::path& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    }
    return total;
}

static std::vector<std::string> splitLines(std::string text) {
    std::vector<std::string> lines;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

static bool isAdbSuccess(const std::string& out) {
    return out.find("Success") != std::string::npos &&
           out.find("Failure") == std::string::npos &&
           out.find("Exception") == std::string::npos &&
           out.find("Error") == std::string::npos;
}

std::vector<InstalledAppEntry> DeviceClient::listInstalledApps(const std::string& serial, bool includeSystem) {
    std::vector<InstalledAppEntry> apps;
    if (serial.empty()) return apps;

    auto parseList = [&](const std::string& out, bool isSystem) {
        std::istringstream ss(out);
        std::string line;
        while (std::getline(ss, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            const std::string prefix = "package:";
            if (line.rfind(prefix, 0) != 0) continue;
            std::string val = line.substr(prefix.size());
            auto eq = val.rfind('=');
            if (eq == std::string::npos) continue;
            InstalledAppEntry app;
            app.apkPath = val.substr(0, eq);
            app.packageName = val.substr(eq + 1);
            app.isSystem = isSystem;
            if (!app.packageName.empty()) apps.push_back(std::move(app));
        }
    };

    parseList(runAdbCommand("-s " + serial + " shell pm list packages -f -3"), false);
    if (includeSystem)
        parseList(runAdbCommand("-s " + serial + " shell pm list packages -f -s"), true);

    static std::mutex labelQueryMutex;
    std::lock_guard<std::mutex> labelLock(labelQueryMutex);
    std::string labelsDex = getAppLabelsDexPath();
    if (!labelsDex.empty()) {
        std::string remoteDex = "/data/local/tmp/afm-labels.dex";
        std::string pushResult = runAdbCommand("-s " + serial + " push " + quoteCmdArg(labelsDex) + " " + remoteDex);
        if (pushResult.find("error") == std::string::npos) {
            std::string labels = runAdbCommand("-s " + serial + " shell \"CLASSPATH=" + remoteDex +
                " app_process /system/bin com.fastenough.tools.AppLabels 2>/dev/null; rm -f " + remoteDex + "\"");
            std::unordered_map<std::string, std::string> names;
            for (const auto& line : splitLines(labels)) {
                size_t separator = line.find('\t');
                if (separator == std::string::npos) continue;
                names[line.substr(0, separator)] = line.substr(separator + 1);
            }
            for (auto& app : apps) {
                auto found = names.find(app.packageName);
                if (found != names.end()) app.appName = found->second;
            }
        }
    }

    std::sort(apps.begin(), apps.end(), [](const InstalledAppEntry& a, const InstalledAppEntry& b) {
        const std::string& aName = a.appName.empty() ? a.packageName : a.appName;
        const std::string& bName = b.appName.empty() ? b.packageName : b.appName;
        int nameOrder = _stricmp(aName.c_str(), bName.c_str());
        return nameOrder != 0 ? nameOrder < 0 : _stricmp(a.packageName.c_str(), b.packageName.c_str()) < 0;
    });
    return apps;
}

bool DeviceClient::installApk(const std::string& serial, const std::string& apkPath, std::string& output,
                              bool reinstall, bool grantRuntimePermissions, bool allowDowngrade) {
    if (serial.empty() || apkPath.empty()) {
        output = "No device or APK path was provided.";
        return false;
    }
    std::string args = "-s " + serial + " install";
    if (reinstall) args += " -r";
    if (grantRuntimePermissions) args += " -g";
    if (allowDowngrade) args += " -d";
    args += " " + quoteCmdArg(apkPath);
    output = runAdbCommand(args, 10 * 60 * 1000);
    return isAdbSuccess(output);
}

bool DeviceClient::installBackupApksBatch(const std::vector<std::string>& backupPaths,
                                          const AppBackupOptions& options,
                                          std::vector<bool>& installed, std::string& output,
                                          ProgressCallback progress,
                                          InstallProgressCallback installProgress) {
    installed.assign(backupPaths.size(), true);
    output.clear();
    if (!options.includeApk || backupPaths.empty()) return true;
    if (!m_connected || !useRoot()) {
        m_lastError = "Root device server is not connected";
        output = m_lastError;
        return false;
    }

    struct LocalGroup {
        size_t backupIndex = 0;
        std::string packageName;
        std::vector<std::filesystem::path> files;
        std::vector<std::string> remotePaths;
        uint64_t totalBytes = 0;
    };
    std::vector<LocalGroup> groups;
    uint64_t totalBytes = 0;
    std::error_code ec;

    for (size_t i = 0; i < backupPaths.size(); ++i) {
        std::filesystem::path dir = toFsPath(backupPaths[i]);
        std::ifstream mf(dir / "backup.json", std::ios::binary);
        nlohmann::json meta = nlohmann::json::parse(mf, nullptr, false);
        if (meta.is_discarded()) {
            installed[i] = false;
            output += backupPaths[i] + ": Backup metadata is not valid.\n";
            continue;
        }
        if (!meta.value("has", nlohmann::json::object()).value("apk", false)) continue;

        LocalGroup group;
        group.backupIndex = i;
        group.packageName = meta.value("packageName", backupPaths[i]);
        for (const auto& apk : meta.value("apkFiles", nlohmann::json::array())) {
            std::filesystem::path file = dir / apk.value("name", "");
            if (!std::filesystem::is_regular_file(file, ec)) {
                installed[i] = false;
                output += backupPaths[i] + ": An APK file is missing.\n";
                group.files.clear();
                break;
            }
            uint64_t fileSize = std::filesystem::file_size(file, ec);
            totalBytes += fileSize;
            group.totalBytes += fileSize;
            group.files.push_back(std::move(file));
        }
        if (!group.files.empty()) {
            installed[i] = false;
            groups.push_back(std::move(group));
        }
    }
    if (groups.empty())
        return std::all_of(installed.begin(), installed.end(), [](bool value) { return value; });

    std::string stagingRoot = "/data/local/tmp/afm-install-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    if (!createDirectory(stagingRoot)) {
        output += "Could not create the APK staging directory: " + m_lastError;
        return false;
    }

    uint64_t completedBytes = 0;
    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        auto& group = groups[groupIndex];
        uint64_t groupCompletedBytes = 0;
        for (size_t apkIndex = 0; apkIndex < group.files.size(); ++apkIndex) {
            uint64_t size = std::filesystem::file_size(group.files[apkIndex], ec);
            std::string remote = stagingRoot + "/" + std::to_string(groupIndex) + "/" +
                std::to_string(apkIndex) + ".apk";
            auto uploadProgress = [&](uint64_t done, uint64_t) {
                if (installProgress) {
                    AppInstallProgress appProgress;
                    appProgress.packageName = group.packageName;
                    appProgress.stageLabel = "Uploading APKs";
                    appProgress.backupIndex = (int)group.backupIndex;
                    appProgress.totalApps = (int)backupPaths.size();
                    appProgress.appProgress = group.totalBytes > 0
                        ? 0.55f * (float)(groupCompletedBytes + done) / (float)group.totalBytes
                        : 0.55f;
                    installProgress(appProgress);
                }
                return !progress || progress(completedBytes + done, totalBytes);
            };
            if (!pushFile(pathToUtf8(group.files[apkIndex]), remote, size, uploadProgress)) {
                output += backupPaths[group.backupIndex] + ": APK upload failed: " + m_lastError + "\n";
                deleteFile(stagingRoot);
                return false;
            }
            completedBytes += size;
            groupCompletedBytes += size;
            group.remotePaths.push_back(std::move(remote));
        }
    }

    nlohmann::json request;
    request["root"] = stagingRoot;
    request["reinstall"] = options.reinstall;
    request["grant"] = options.grantRuntimePermissions;
    request["downgrade"] = options.allowDowngrade;
    request["parallelism"] = 3;
    request["packages"] = nlohmann::json::array();
    for (const auto& group : groups) request["packages"].push_back({{"apks", group.remotePaths}});
    std::string encoded = request.dump();

    MsgHeader hdr{};
    std::vector<char> responsePayload;
    bool received = false;
    bool allOk = true;
    std::vector<bool> resultSeen(groups.size(), false);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        SOCKET sock = (SOCKET)m_socket;
        DWORD installTimeout = 600000;
        DWORD normalTimeout = 10000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&installTimeout, sizeof(installTimeout));
        bool sent = sendMsg(CMD_INSTALL_BATCH, encoded.data(), (uint32_t)encoded.size());
        received = sent && recvMsg(hdr, responsePayload);
        if (received && hdr.cmd == RSP_OK) {
            while (recvMsg(hdr, responsePayload)) {
                if (hdr.cmd == RSP_DONE) break;
                if (hdr.cmd == RSP_ERROR) {
                    output += std::string(responsePayload.data(), responsePayload.size());
                    received = false;
                    break;
                }
                if (hdr.cmd != RSP_DATA) {
                    received = false;
                    break;
                }
                nlohmann::json event = nlohmann::json::parse(responsePayload.begin(), responsePayload.end(), nullptr, false);
                if (event.is_discarded()) continue;
                size_t groupIndex = event.value("index", groups.size());
                if (groupIndex >= groups.size()) continue;
                std::string state = event.value("state", "");
                if (installProgress) {
                    AppInstallProgress appProgress;
                    appProgress.packageName = groups[groupIndex].packageName;
                    appProgress.backupIndex = (int)groups[groupIndex].backupIndex;
                    appProgress.completedApps = event.value("completed", 0);
                    appProgress.totalApps = (int)groups.size();
                    appProgress.stageLabel = state == "installing" ? "Installing app" :
                        (event.value("success", false) ? "App installed" : "Install failed");
                    appProgress.appProgress = state == "installing" ? 0.70f : 1.0f;
                    installProgress(appProgress);
                }
                if (state == "complete") {
                    bool success = event.value("success", false);
                    installed[groups[groupIndex].backupIndex] = success;
                    resultSeen[groupIndex] = true;
                    if (!success) {
                        allOk = false;
                        output += backupPaths[groups[groupIndex].backupIndex] + ":\n" +
                            event.value("output", "Install failed") + "\n";
                    }
                }
            }
        }
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&normalTimeout, sizeof(normalTimeout));
    }
    if (!received) {
        output += "The device server did not return the batch install result.";
        return false;
    }
    if (hdr.cmd == RSP_ERROR) {
        output += std::string(responsePayload.data(), responsePayload.size());
        return false;
    }
    if (hdr.cmd != RSP_DONE || std::find(resultSeen.begin(), resultSeen.end(), false) != resultSeen.end()) {
        output += "The device server returned an invalid batch install result.";
        return false;
    }
    if (progress) progress(totalBytes, totalBytes);
    return allOk;
}

bool DeviceClient::restoreStagedBackupArchive(const std::string& remoteArchive,
                                              const std::vector<ArchiveRestoreSelection>& selections,
                                              const AppBackupOptions& options,
                                              std::vector<bool>& restored,
                                              std::string& output,
                                              InstallProgressCallback progress) {
    restored.assign(selections.size(), false);
    output.clear();
    if (!m_connected || selections.empty()) {
        m_lastError = m_connected ? "No archive restore selections were provided" : "Not connected";
        output = m_lastError;
        return false;
    }
    const auto restoreStarted = std::chrono::steady_clock::now();
    nlohmann::json request;
    request["archive"] = remoteArchive;
    request["includeApk"] = options.includeApk;
    request["includeData"] = options.includeData;
    request["includeDeviceProtectedData"] = options.includeDeviceProtectedData;
    request["includeExternalData"] = options.includeExternalData;
    request["includeObb"] = options.includeObb;
    request["includeMedia"] = options.includeMedia;
    request["grantRuntimePermissions"] = options.grantRuntimePermissions;
    request["reinstall"] = options.reinstall;
    request["allowDowngrade"] = options.allowDowngrade;
    request["parallelism"] = 4;
    request["apps"] = nlohmann::json::array();
    std::vector<std::string> restoreAppNames;
    std::vector<std::string> restorePackageNames;
    restoreAppNames.reserve(selections.size());
    restorePackageNames.reserve(selections.size());
    for (const auto& selection : selections) {
        nlohmann::json metadata = nlohmann::json::parse(selection.metadataJson, nullptr, false);
        if (metadata.is_discarded()) {
            output = "A selected app has invalid backup metadata";
            return false;
        }
        restoreAppNames.push_back(metadata.value("label", ""));
        restorePackageNames.push_back(metadata.value("packageName", ""));
        request["apps"].push_back({{"prefix", selection.prefix}, {"metadata", std::move(metadata)}});
    }
    std::string encoded = request.dump();

    std::lock_guard<std::mutex> lock(m_mutex);
    SOCKET sock = (SOCKET)m_socket;
    DWORD longTimeout = 1800000;
    DWORD normalTimeout = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&longTimeout, sizeof(longTimeout));
    MsgHeader header{};
    std::vector<char> payload;
    bool received = sendMsg(CMD_RESTORE_BACKUP_ARCHIVE, encoded.data(), (uint32_t)encoded.size()) &&
        recvMsg(header, payload);
    if (!received || header.cmd != RSP_OK) {
        if (received && header.cmd == RSP_ERROR) output.assign(payload.begin(), payload.end());
        else output = m_lastError.empty() ? "The device did not accept the restore archive" : m_lastError;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&normalTimeout, sizeof(normalTimeout));
        return false;
    }
    const double stagingSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - restoreStarted).count();
    LOG_INFO("BackupManager", "Android restore staging completed in " +
        std::to_string(stagingSeconds) + " s, starting " +
        std::to_string(selections.size()) + " app restore(s) with 4 workers");

    bool protocolOk = false;
    while (recvMsg(header, payload)) {
        if (header.cmd == RSP_DONE) {
            protocolOk = true;
            break;
        }
        if (header.cmd == RSP_ERROR) {
            output.append(payload.begin(), payload.end());
            break;
        }
        if (header.cmd != RSP_DATA) break;
        nlohmann::json event = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
        if (event.is_discarded()) continue;
        const size_t index = event.value("index", selections.size());
        if (index >= selections.size()) continue;
        const std::string state = event.value("state", "");
        if (state == "complete") {
            restored[index] = event.value("success", false);
            if (!restored[index]) {
                output += friendlyRestoreFailure(event.value("output", ""), restoreAppNames[index],
                    restorePackageNames[index], options.allowDowngrade) + "\n\n";
            }
        }
        if (progress) {
            AppInstallProgress update;
            update.packageName = event.value("packageName", "");
            update.stageLabel = event.value("stage", state);
            update.backupIndex = (int)index;
            update.completedApps = event.value("completed", 0);
            update.totalApps = (int)selections.size();
            update.appProgress = event.value("progress", state == "complete" ? 1.0f : 0.0f);
            progress(update);
        }
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&normalTimeout, sizeof(normalTimeout));
    if (!protocolOk && output.empty()) output = "The device returned an incomplete archive restore result";
    const double totalSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - restoreStarted).count();
    LOG_INFO("BackupManager", "Android archive restore processing finished in " +
        std::to_string(totalSeconds) + " s");
    return protocolOk && std::all_of(restored.begin(), restored.end(), [](bool value) { return value; });
}

bool DeviceClient::createDeviceBackupArchive(const std::string& serial,
                                             const std::string& created,
                                             const std::vector<ArchiveBackupApp>& apps,
                                             const AppBackupOptions& options,
                                             std::string& remoteArchive,
                                             uint64_t& archiveSize,
                                             std::vector<bool>& backedUp,
                                             std::string& output,
                                             InstallProgressCallback progress) {
    remoteArchive.clear();
    archiveSize = 0;
    backedUp.assign(apps.size(), false);
    output.clear();
    if (!m_connected || apps.empty()) {
        output = m_connected ? "No apps were selected for backup" : "The device helper is not connected";
        return false;
    }

    nlohmann::json request;
    request["serial"] = serial;
    request["created"] = created;
    request["includeApk"] = options.includeApk;
    request["includeData"] = options.includeData;
    request["includeDeviceProtectedData"] = options.includeDeviceProtectedData;
    request["includeExternalData"] = options.includeExternalData;
    request["includeObb"] = options.includeObb;
    request["includeMedia"] = options.includeMedia;
    request["excludeCache"] = options.excludeCache;
    request["apps"] = nlohmann::json::array();
    for (const auto& app : apps) {
        request["apps"].push_back({{"packageName", app.packageName}, {"label", app.label}});
    }
    const std::string encoded = request.dump();

    std::lock_guard<std::mutex> lock(m_mutex);
    SOCKET sock = (SOCKET)m_socket;
    DWORD longTimeout = 1800000;
    DWORD normalTimeout = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&longTimeout, sizeof(longTimeout));
    MsgHeader header{};
    std::vector<char> payload;
    bool received = sendMsg(CMD_CREATE_BACKUP_ARCHIVE, encoded.data(), (uint32_t)encoded.size()) &&
        recvMsg(header, payload);
    if (!received || header.cmd != RSP_OK) {
        if (received && header.cmd == RSP_ERROR) output.assign(payload.begin(), payload.end());
        else output = m_lastError.empty() ? "The device did not accept the backup request" : m_lastError;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&normalTimeout, sizeof(normalTimeout));
        return false;
    }

    bool protocolOk = false;
    while (recvMsg(header, payload)) {
        if (header.cmd == RSP_DONE) {
            nlohmann::json done = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
            if (!done.is_discarded()) {
                remoteArchive = done.value("archive", "");
                archiveSize = done.value("size", uint64_t{0});
                if (done.contains("results") && done["results"].is_array()) {
                    const size_t count = std::min(backedUp.size(), done["results"].size());
                    for (size_t index = 0; index < count; ++index) {
                        if (done["results"][index].is_boolean()) backedUp[index] = done["results"][index].get<bool>();
                    }
                }
                protocolOk = !remoteArchive.empty() && archiveSize > 0;
            }
            break;
        }
        if (header.cmd == RSP_ERROR) {
            output.append(payload.begin(), payload.end());
            break;
        }
        if (header.cmd != RSP_DATA) break;
        nlohmann::json event = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
        if (event.is_discarded()) continue;
        const size_t index = event.value("index", apps.size());
        if (index >= apps.size()) continue;
        const std::string state = event.value("state", "");
        if (state == "complete") {
            backedUp[index] = event.value("success", false);
            if (!backedUp[index]) {
                output += apps[index].packageName + ": " + event.value("output", "Backup failed") + "\n";
            }
        }
        if (progress) {
            AppInstallProgress update;
            update.packageName = event.value("packageName", apps[index].packageName);
            update.stageLabel = event.value("stage", state);
            update.backupIndex = (int)index;
            update.completedApps = event.value("completed", 0);
            update.totalApps = (int)apps.size();
            update.appProgress = event.value("progress", state == "complete" ? 1.0f : 0.0f);
            progress(update);
        }
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&normalTimeout, sizeof(normalTimeout));
    if (!protocolOk && output.empty()) output = "The device returned an incomplete backup archive result";
    return protocolOk;
}

bool DeviceClient::uninstallPackage(const std::string& serial, const std::string& packageName, std::string& output,
                                    bool keepData, bool userOnly, bool useRoot) {
    if (serial.empty() || packageName.empty()) {
        output = "No device or package was provided.";
        return false;
    }

    std::string flags;
    if (keepData) flags += " -k";
    if (userOnly) flags += " --user 0";

    if (useRoot) {
        std::string inner = "pm uninstall" + flags + " " + packageName;
        output = runAdbCommand("-s " + serial + " shell \"su -c '" + inner + "'\"");
    } else if (userOnly) {
        output = runAdbCommand("-s " + serial + " shell pm uninstall" + flags + " " + packageName);
    } else {
        output = runAdbCommand("-s " + serial + " uninstall" + (keepData ? " -k " : " ") + packageName);
    }
    return isAdbSuccess(output);
}

bool DeviceClient::backupApp(const std::string& serial, const std::string& packageName, const std::string& appLabel,
                             const std::string& backupRoot,
                             const AppBackupOptions& options, AppBackupRecord& outRecord, std::string& output,
                             BackupProgressCallback progress) {
    LOG_INFO("BackupEngine", "backupApp begin package=" + packageName + " serial=" + serial);
    bool cancelled = false;
    auto emitProgress = [&](const std::string& stage, int stageIndex, int stageCount,
                            uint64_t bytesDone = 0, uint64_t bytesTotal = 0) -> bool {
        if (!progress) return true;
        AppBackupProgress p;
        p.packageName = packageName;
        p.stageLabel = stage;
        p.stageIndex = stageIndex;
        p.stageCount = std::max(1, stageCount);
        p.bytesTransferred = bytesDone;
        p.bytesTotal = bytesTotal;
        if (!progress(p)) cancelled = true;
        return !cancelled;
    };
    auto enabledStageCount = [&]() {
        int count = 4; // prepare, package query, permissions, metadata
        if (options.includeApk) count++;
        if (options.includeData) count++;
        if (options.includeDeviceProtectedData) count++;
        if (options.includeExternalData) count++;
        if (options.includeObb) count++;
        if (options.includeMedia) count++;
        return count;
    };
    int stageTotal = enabledStageCount();
    int stageIndex = 0;
    output.clear();
    outRecord = {};
    if (!emitProgress("Preparing backup", stageIndex, stageTotal)) {
        output = "Backup cancelled.";
        return false;
    }
    if (serial.empty() || packageName.empty()) {
        output = "No device or package was provided.";
        LOG_ERROR("BackupEngine", output);
        return false;
    }
    if (!(m_connected && useRoot()) && !isRootAvailable(serial)) {
        output = "Root access is required for app and data backups.";
        LOG_ERROR("BackupEngine", output);
        return false;
    }

    std::filesystem::path root = toFsPath(backupRoot);
    std::string stamp = nowBackupStamp();
    std::filesystem::path appDir = root / sanitizePathPart(packageName) / stamp;
    std::error_code ec;
    std::filesystem::create_directories(appDir, ec);
    if (ec) {
        output = "Could not create backup folder.";
        LOG_ERROR("BackupEngine", output + " " + pathToUtf8(appDir));
        return false;
    }
    LOG_INFO("BackupEngine", "Backup folder: " + pathToUtf8(appDir));
    auto cancelBackup = [&]() {
        std::filesystem::remove_all(appDir, ec);
        outRecord = {};
        output = "Backup cancelled.";
        LOG_INFO("BackupEngine", "Backup cancelled: " + packageName);
        return false;
    };

    stageIndex++;
    if (!emitProgress("Reading APK paths", stageIndex, stageTotal)) return cancelBackup();
    LOG_INFO("BackupEngine", "Querying APK paths for " + packageName);
    std::string apkOut = runAdbCommand("-s " + serial + " shell pm path " + packageName);
    std::vector<std::string> apkPaths;
    for (auto& line : splitLines(apkOut)) {
        const std::string prefix = "package:";
        if (line.rfind(prefix, 0) == 0) apkPaths.push_back(line.substr(prefix.size()));
    }
    if (apkPaths.empty()) {
        output = "Package is not installed or Package Manager did not return APK paths.";
        LOG_ERROR("BackupEngine", output + " pm path output: " + sanitizeAdbOutput(apkOut));
        return false;
    }
    LOG_INFO("BackupEngine", "APK path count for " + packageName + ": " + std::to_string(apkPaths.size()));

    stageIndex++;
    if (!emitProgress("Reading permissions", stageIndex, stageTotal)) return cancelBackup();
    LOG_INFO("BackupEngine", "Reading package permissions for " + packageName);
    std::string dumpsys = runAdbCommand("-s " + serial + " shell dumpsys package " + packageName);
    std::vector<std::string> grantedPerms;
    for (auto& line : splitLines(dumpsys)) {
        auto p = line.find("android.permission.");
        if (p != std::string::npos && line.find("granted=true") != std::string::npos) {
            std::string perm = line.substr(p);
            auto colon = perm.find(':');
            if (colon != std::string::npos) perm = perm.substr(0, colon);
            auto sp = perm.find(' ');
            if (sp != std::string::npos) perm = perm.substr(0, sp);
            if (std::find(grantedPerms.begin(), grantedPerms.end(), perm) == grantedPerms.end())
                grantedPerms.push_back(perm);
        }
    }

    auto adbExecOutRootToFile = [&](const std::string& rootCmd, const std::filesystem::path& localFile, std::string& err,
                                    ProgressCallback streamProgress = nullptr, uint64_t totalBytes = 0) {
        std::string cmd = "\"" + m_adbPath + "\" -s " + serial + " exec-out su -c " + quoteCmdArg(rootCmd);
        return runProcessToFile(cmd, pathToUtf8(localFile), err, 0, streamProgress, totalBytes);
    };
    auto rootTestDir = [&](const std::string& path) {
        std::string out = runAdbCommand("-s " + serial + " shell \"su -c " + quoteCmdArg("test -d " + shellQuote(path) + " && echo yes") + "\"");
        return out.find("yes") != std::string::npos;
    };
    auto remoteFileSize = [&](const std::string& path) -> uint64_t {
        std::string out = runAdbCommand("-s " + serial + " shell \"su -c " + quoteCmdArg("stat -c %s " + shellQuote(path) + " 2>/dev/null") + "\"");
        try {
            return std::stoull(sanitizeAdbOutput(out));
        } catch (...) {
            return 0;
        }
    };
    auto backupTar = [&](const std::string& key, const std::string& source, const char* filename, bool enabled, bool& hasFlag) -> bool {
        hasFlag = false;
        if (cancelled) return false;
        if (!enabled) {
            LOG_INFO("BackupEngine", key + " disabled");
            return true;
        }
        if (!rootTestDir(source)) {
            LOG_INFO("BackupEngine", key + " skipped, missing path: " + source);
            return true;
        }
        std::string excludes;
        if (options.excludeCache) excludes = " --exclude=cache --exclude=code_cache --exclude=no_backup";
        std::string cmd = "cd " + shellQuote(source) + " && tar -cpf -" + excludes + " .";
        std::string err;
        std::filesystem::path outPath = appDir / filename;
        stageIndex++;
        if (!emitProgress(key, stageIndex, stageTotal)) return false;
        LOG_INFO("BackupEngine", key + " tar start: " + source);
        auto streamProgress = [&](uint64_t done, uint64_t total) {
            return emitProgress(key, stageIndex, stageTotal, done, total);
        };
        bool archived = false;
        if (m_connected) {
            uint64_t outSize = 0;
            archived = archiveRemoteDirectory(source, pathToUtf8(outPath), options.excludeCache, outSize, streamProgress);
            if (!archived) err = m_lastError;
        } else {
            archived = adbExecOutRootToFile(cmd, outPath, err, streamProgress, 0);
        }
        if (!archived) {
            if (cancelled) {
                std::filesystem::remove(outPath, ec);
                return false;
            }
            output += key + " failed: " + sanitizeAdbOutput(err) + "\n";
            LOG_ERROR("BackupEngine", key + " tar failed: " + sanitizeAdbOutput(err));
            std::filesystem::remove(outPath, ec);
            return false;
        }
        if (std::filesystem::file_size(outPath, ec) == 0) {
            LOG_INFO("BackupEngine", key + " produced empty archive, removing: " + source);
            std::filesystem::remove(outPath, ec);
            return true;
        }
        hasFlag = true;
        LOG_INFO("BackupEngine", key + " tar complete: " + pathToUtf8(outPath));
        return true;
    };

    bool ok = true;
    nlohmann::json apkList = nlohmann::json::array();
    if (options.includeApk) {
        stageIndex++;
        for (const auto& apk : apkPaths) {
            std::string name = apk.substr(apk.find_last_of('/') + 1);
            if (name.empty()) name = "base.apk";
            std::filesystem::path target = appDir / sanitizePathPart(name);
            std::string err;
            uint64_t apkSize = remoteFileSize(apk);
            if (!emitProgress("APK: " + name, stageIndex, stageTotal, 0, apkSize)) break;
            LOG_INFO("BackupEngine", "APK stream start: " + apk);
            auto streamProgress = [&](uint64_t done, uint64_t total) {
                return emitProgress("APK: " + name, stageIndex, stageTotal, done, total);
            };
            bool apkOk = false;
            if (m_connected) {
                uint64_t outSize = 0;
                apkOk = pullFile(apk, pathToUtf8(target), outSize, streamProgress);
                if (!apkOk) err = m_lastError;
            } else {
                apkOk = adbExecOutRootToFile("cat " + shellQuote(apk), target, err, streamProgress, apkSize);
            }
            if (apkOk) {
                apkList.push_back({{"name", pathToUtf8(target.filename())}, {"source", apk}});
                outRecord.hasApk = true;
                LOG_INFO("BackupEngine", "APK stream complete: " + pathToUtf8(target));
            } else {
                ok = false;
                output += "APK backup failed for " + apk + ": " + sanitizeAdbOutput(err) + "\n";
                LOG_ERROR("BackupEngine", "APK stream failed: " + apk + ": " + sanitizeAdbOutput(err));
            }
        }
    }
    if (cancelled) return cancelBackup();

    std::string dataPath = "/data/data/" + packageName;
    std::string dePath = "/data/user_de/0/" + packageName;
    std::string extPath = "/sdcard/Android/data/" + packageName;
    std::string obbPath = "/sdcard/Android/obb/" + packageName;
    std::string mediaPath = "/sdcard/Android/media/" + packageName;

    ok = backupTar("App data", dataPath, "data.tar", options.includeData, outRecord.hasData) && ok;
    ok = backupTar("Device protected data", dePath, "data_de.tar", options.includeDeviceProtectedData, outRecord.hasDeviceProtectedData) && ok;
    ok = backupTar("External data", extPath, "external.tar", options.includeExternalData, outRecord.hasExternalData) && ok;
    ok = backupTar("OBB data", obbPath, "obb.tar", options.includeObb, outRecord.hasObb) && ok;
    ok = backupTar("Media data", mediaPath, "media.tar", options.includeMedia, outRecord.hasMedia) && ok;
    if (cancelled) return cancelBackup();

    outRecord.packageName = packageName;
    outRecord.label = appLabel;
    outRecord.serial = serial;
    outRecord.backupPath = pathToUtf8(appDir);
    outRecord.created = stamp;
    outRecord.sizeBytes = directorySizeBytes(appDir);

    nlohmann::json meta;
    meta["format"] = "FastEnoughAppBackup";
    meta["formatVersion"] = 1;
    meta["packageName"] = packageName;
    meta["label"] = appLabel;
    meta["serial"] = serial;
    meta["created"] = stamp;
    meta["apkFiles"] = apkList;
    meta["permissions"] = grantedPerms;
    meta["paths"] = {
        {"data", dataPath},
        {"data_de", dePath},
        {"external", extPath},
        {"obb", obbPath},
        {"media", mediaPath}
    };
    meta["has"] = {
        {"apk", outRecord.hasApk},
        {"data", outRecord.hasData},
        {"data_de", outRecord.hasDeviceProtectedData},
        {"external", outRecord.hasExternalData},
        {"obb", outRecord.hasObb},
        {"media", outRecord.hasMedia}
    };
    meta["source"] = "Root APK plus tar archive backup";
    stageIndex++;
    if (!emitProgress("Writing metadata", stageIndex, stageTotal)) return cancelBackup();
    LOG_INFO("BackupEngine", "Writing backup metadata for " + packageName);
    std::ofstream mf(appDir / "backup.json", std::ios::binary);
    mf << meta.dump(2);

    if (!ok) {
        LOG_ERROR("BackupEngine", "backupApp finished with errors for " + packageName + ": " + sanitizeAdbOutput(output));
        return false;
    }
    output = "Backup saved to " + outRecord.backupPath;
    if (!emitProgress("Complete", stageTotal, stageTotal)) return cancelBackup();
    LOG_INFO("BackupEngine", "backupApp complete package=" + packageName + " size=" + std::to_string(outRecord.sizeBytes));
    return true;
}

bool DeviceClient::restoreAppBackup(const std::string& serial, const std::string& backupPath, const AppBackupOptions& options,
                                    std::string& output, BackupProgressCallback progress) {
    output.clear();
    if (serial.empty() || backupPath.empty()) {
        output = "No device or backup was provided.";
        return false;
    }
    if (!(m_connected && useRoot()) && !isRootAvailable(serial)) {
        output = "Root access is required for app data restore.";
        return false;
    }

    std::filesystem::path dir = toFsPath(backupPath);
    std::ifstream mf(dir / "backup.json", std::ios::binary);
    if (!mf) {
        output = "Backup metadata is missing.";
        return false;
    }
    nlohmann::json meta = nlohmann::json::parse(mf, nullptr, false);
    if (meta.is_discarded()) {
        output = "Backup metadata is not valid JSON.";
        return false;
    }
    std::string packageName = meta.value("packageName", "");
    if (packageName.empty()) {
        output = "Backup metadata has no package name.";
        return false;
    }

    auto paths = meta["paths"];
    auto has = meta["has"];
    int stageTotal = 0;
    if (options.includeApk && has.value("apk", false)) stageTotal++;
    if (options.includeData && has.value("data", false)) stageTotal++;
    if (options.includeDeviceProtectedData && has.value("data_de", false)) stageTotal++;
    if (options.includeExternalData && has.value("external", false)) stageTotal++;
    if (options.includeObb && has.value("obb", false)) stageTotal++;
    if (options.includeMedia && has.value("media", false)) stageTotal++;
    if (options.grantRuntimePermissions && meta.contains("permissions")) stageTotal++;
    if (stageTotal <= 0) stageTotal = 1;
    int stageIndex = 0;
    auto emitProgress = [&](const std::string& label, int stage, uint64_t done = 0, uint64_t total = 0) {
        if (!progress) return true;
        AppBackupProgress p;
        p.packageName = packageName;
        p.stageLabel = label;
        p.stageIndex = stage;
        p.stageCount = stageTotal;
        p.bytesTransferred = done;
        p.bytesTotal = total;
        return progress(p);
    };

    if (options.includeApk && meta["has"].value("apk", false)) {
        stageIndex++;
        emitProgress("Installing APK", stageIndex);
        std::string args = "-s " + serial + " install";
        if (options.reinstall) args += " -r";
        if (options.grantRuntimePermissions) args += " -g";
        if (options.allowDowngrade) args += " -d";
        auto apkFiles = meta.value("apkFiles", nlohmann::json::array());
        if (apkFiles.size() > 1) args = "-s " + serial + " install-multiple" + (options.reinstall ? " -r" : "") +
            (options.grantRuntimePermissions ? " -g" : "") + (options.allowDowngrade ? " -d" : "");
        for (auto& apk : apkFiles) {
            std::filesystem::path p = dir / apk.value("name", "");
            args += " " + quoteCmdArg(pathToUtf8(p));
        }
        std::string installOut = runAdbCommand(args, 10 * 60 * 1000);
        output += installOut + "\n";
        if (!isAdbSuccess(installOut)) return false;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    runAdbCommand("-s " + serial + " shell su -c " +
        quoteCmdArg("am force-stop --user 0 " + packageName + " 2>/dev/null"));

    auto rootPathExists = [&](const std::string& path) {
        std::string out = runAdbCommand("-s " + serial + " shell su -c " + quoteCmdArg("test -d " + path + " && echo yes"));
        return out.find("yes") != std::string::npos;
    };
    auto ownerContext = [&](const std::string& path) {
        std::string out = runAdbCommand("-s " + serial + " shell su -c " + quoteCmdArg("stat -c '%u:%g %C' " + path + " 2>/dev/null"));
        return sanitizeAdbOutput(out);
    };
    auto restoreTar = [&](const char* label, const char* filename, const std::string& target, bool enabled, bool present) -> bool {
        if (!enabled || !present) return true;
        stageIndex++;
        std::filesystem::path archive = dir / filename;
        if (!std::filesystem::exists(archive)) {
            output += std::string(label) + " archive is missing.\n";
            return false;
        }
        std::error_code archiveEc;
        uint64_t archiveSize = std::filesystem::file_size(archive, archiveEc);
        if (archiveEc) archiveSize = 0;
        emitProgress(std::string(label) + ": pushing archive", stageIndex, 0, archiveSize);
        std::string uidgidcon;
        bool existed = rootPathExists(target);
        if (existed) uidgidcon = ownerContext(target);
        std::string remote = "/data/local/tmp/afm-" + sanitizePathPart(packageName) + "-" + filename;
        if (m_connected) {
            auto transferProgress = [&](uint64_t done, uint64_t total) {
                return emitProgress(std::string(label) + ": pushing archive", stageIndex, done, total);
            };
            if (!extractRemoteArchive(pathToUtf8(archive), target, transferProgress)) {
                output += std::string(label) + " extract failed: " + m_lastError + "\n";
                return false;
            }
            emitProgress(std::string(label) + ": extracting archive", stageIndex, archiveSize, archiveSize);
        } else {
            std::string pushOut = runAdbCommand("-s " + serial + " push " + quoteCmdArg(pathToUtf8(archive)) + " " + remote,
                30 * 60 * 1000);
            output += pushOut + "\n";
            if (pushOut.find("error") != std::string::npos || pushOut.find("failed") != std::string::npos) return false;
            std::string cmd =
                "mkdir -p " + target +
                " && rm -rf " + target + "/*" +
                " && tar -xpf " + remote + " -C " + target +
                " && rm -f " + remote;
            std::string out = runAdbCommand("-s " + serial + " shell su -c " + quoteCmdArg(cmd), 30 * 60 * 1000);
            output += out + "\n";
            std::string low = out;
            std::transform(low.begin(), low.end(), low.begin(), ::tolower);
            if (low.find("error") != std::string::npos || low.find("failed") != std::string::npos || low.find("cannot") != std::string::npos) return false;
        }
        if (!uidgidcon.empty()) {
            std::istringstream iss(uidgidcon);
            std::string owner, context;
            iss >> owner >> context;
            if (!owner.empty()) {
                std::string fix = "chown -R " + owner + " " + target + " ; restorecon -RF " + target + " 2>/dev/null";
                if (!context.empty() && context != "?") fix += " ; chcon -R -h " + context + " " + target + " 2>/dev/null";
                output += runAdbCommand("-s " + serial + " shell su -c " + quoteCmdArg(fix), 30 * 60 * 1000) + "\n";
            }
        }
        emitProgress(std::string(label) + ": restored", stageIndex, archiveSize, archiveSize);
        return true;
    };
    bool ok = true;
    ok = restoreTar("App data", "data.tar", paths.value("data", "/data/data/" + packageName), options.includeData, has.value("data", false)) && ok;
    ok = restoreTar("Device protected data", "data_de.tar", paths.value("data_de", "/data/user_de/0/" + packageName), options.includeDeviceProtectedData, has.value("data_de", false)) && ok;
    ok = restoreTar("External data", "external.tar", paths.value("external", "/sdcard/Android/data/" + packageName), options.includeExternalData, has.value("external", false)) && ok;
    ok = restoreTar("OBB data", "obb.tar", paths.value("obb", "/sdcard/Android/obb/" + packageName), options.includeObb, has.value("obb", false)) && ok;
    ok = restoreTar("Media data", "media.tar", paths.value("media", "/sdcard/Android/media/" + packageName), options.includeMedia, has.value("media", false)) && ok;

    if (options.grantRuntimePermissions && meta.contains("permissions")) {
        stageIndex++;
        emitProgress("Restoring permissions", stageIndex);
        std::filesystem::path permissionList = std::filesystem::temp_directory_path() /
            ("afm-permissions-" + sanitizePathPart(packageName) + ".txt");
        std::ofstream permissionFile(permissionList, std::ios::binary | std::ios::trunc);
        int permissionCount = 0;
        if (!permissionFile) {
            LOG_ERROR("BackupEngine", "Could not create runtime permission list for " + packageName);
        } else {
            for (auto& p : meta["permissions"]) {
                if (!p.is_string()) continue;
                permissionFile << p.get<std::string>() << '\n';
                permissionCount++;
            }
            permissionFile.close();
        }
        if (permissionCount > 0) {
            std::string remoteList = "/data/local/tmp/afm-permissions-" + sanitizePathPart(packageName) + ".txt";
            std::string pushOut = runAdbCommand("-s " + serial + " push " +
                quoteCmdArg(pathToUtf8(permissionList)) + " " + remoteList);
            if (pushOut.find("error") == std::string::npos && pushOut.find("failed") == std::string::npos) {
                std::string grantScript = "while IFS= read -r permission; do pm grant --user 0 " +
                    shellQuote(packageName) + " \"$permission\" 2>/dev/null; done < " +
                    shellQuote(remoteList) + "; rm -f " + shellQuote(remoteList);
                runAdbCommand("-s " + serial + " shell su -c " + quoteCmdArg(grantScript));
                LOG_INFO("BackupEngine", "Restored " + std::to_string(permissionCount) +
                    " runtime permissions in one root shell for " + packageName);
            } else {
                LOG_ERROR("BackupEngine", "Could not push runtime permission list for " + packageName + ": " +
                    sanitizeAdbOutput(pushOut));
            }
            std::error_code permissionEc;
            std::filesystem::remove(permissionList, permissionEc);
        }
    }

    if (ok) output += "Restore complete.";
    emitProgress("Complete", stageTotal, 0, 0);
    return ok;
}

bool DeviceClient::startServer(const std::string& serial, bool preferAdbForward, bool useRoot) {
    std::lock_guard<std::mutex> serverLk(m_serverMutex);
    m_startServerActive = true;
    struct ConnectActivityReset {
        std::atomic<bool>& active;
        ~ConnectActivityReset() { active = false; }
    } connectActivityReset{m_startServerActive};
    bool changingPrivilege = m_connected && m_serial == serial && m_serverIsRoot != useRoot;
    bool canReuseConnection = m_connected && m_serial == serial && m_serverIsRoot == useRoot;
    LOG_INFO("Server", std::string("startServer(serial=") + serial + " prefer=" + (preferAdbForward ? "ADBForward" : "DirectTCP") + " root=" + (useRoot ? "Y" : "N") + ") begin");
    if (canReuseConnection) {
        m_useRoot = useRoot;
        LOG_INFO("Server", "Already connected to " + serial + " with the requested privilege mode");
        return true;
    }
    if (m_connected && m_serial == serial && useRoot)
        LOG_INFO("Server", "Existing helper is not verified as root, replacing it");
    if (useRoot && !isRootAvailable(serial)) {
        m_lastError = "Root access is not available or was denied. Check that this device is rooted and grant root access to ADB in Magisk or KernelSU.";
        LOG_ERROR("Server", m_lastError);
        m_statusText = "ERROR: " + m_lastError;
        return false;
    }
    m_useRoot = useRoot;

    // If we previously had a connection to this device that died, kill the old server
    // so it releases its dead client socket and can accept new connections
    bool wasConnectedToSameDevice = (!m_connected && m_serial == serial && !m_serial.empty());
    disconnectTcp();
    m_connected = false;
    m_directConnection = false;
    std::string savedIp = m_deviceIp; // preserve IP from tethering detection
    LOG_DEBUG("Server", "savedIp from tethering: '" + savedIp + "'");
    m_deviceIp.clear();
    m_serial = serial;
    // Additional device slots use their own remote helper port. This lets a slot
    // recover without root if a legacy process is still holding the default port.
    int launchPort = m_localPort == AFM_PORT ? AFM_PORT : m_localPort;
    m_serverPort = AFM_PORT;

    if (m_useRoot || changingPrivilege) {
        LOG_INFO("Server", "Restarting helper with the requested privilege mode");
        stopDeviceServerProcesses(serial);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (wasConnectedToSameDevice) {
        LOG_INFO("Server", "Reconnecting to same device - killing old server to clear dead sockets");
        // Kill both shell-user and root server instances regardless of mode.
        stopDeviceServerProcesses(serial);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        // Restart server immediately
        std::string serverBin = getServerBinaryPath();
        if (std::filesystem::exists(serverBin)) {
            runAdbCommand("-s " + serial + " push \"" + serverBin + "\" /data/local/tmp/afm-server");
            runAdbCommand("-s " + serial + " shell chmod 755 /data/local/tmp/afm-server");
            runAdbCommand("-s " + serial + " shell rm -f /data/local/tmp/afm-server.log");
            std::string launchInner = serverLaunchCommand(launchPort, m_useRoot);
            auto launch = runAdbCommandResult("-s " + serial + " shell \"" + launchInner + "\"", 5000);
            if (launch.cancelled) return false;
            if (!launch.succeeded()) LOG_WARN("Server", "Helper launch: " + launch.diagnostics());
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            LOG_INFO("Server", "Fresh server started after reconnect");
        }
    }

    // 1. Try connecting to an already-running server. Root mode skips this so
    // we do not accidentally attach to an old shell-user helper.
    m_statusText = "Checking for existing server...";
    std::string deviceIp = detectDeviceIp(serial);
    LOG_INFO("Server", "detectDeviceIp -> '" + deviceIp + "'");
    if (deviceIp.empty() && !savedIp.empty()) { deviceIp = savedIp; LOG_INFO("Server", "Using savedIp: " + savedIp); }
    if (m_useRoot || changingPrivilege) {
        LOG_INFO("Server", "Skipping existing-server probe for privilege change");
    } else if (!deviceIp.empty() && !preferAdbForward) {
        LOG_INFO("Server", "Step 1a: Trying existing server via Direct TCP (" + deviceIp + ")");
        m_statusText = "Trying Direct TCP (" + deviceIp + ")...";
        if (tryDirectConnect(deviceIp)) {
            LOG_INFO("Server", "Connected to existing server via Direct TCP");
            m_statusText = "Connected via Direct TCP";
            m_connected = true;
            return true;
        }
        LOG_WARN("Server", "Direct TCP to existing server failed");
    } else if (preferAdbForward) {
        LOG_INFO("Server", "Skipping Direct TCP (ADB Forward preferred)");
    } else {
        LOG_WARN("Server", "No device IP available, skipping direct TCP");
    }

    if (!m_useRoot && !changingPrivilege) {
        LOG_INFO("Server", "Step 1b: Trying existing server via ADB Forward");
        m_statusText = "Trying ADB Forward...";
        runAdbCommand("-s " + serial + " forward --remove tcp:" + std::to_string(m_localPort));
        runAdbCommand("-s " + serial + " forward tcp:" + std::to_string(m_localPort) +
                      " tcp:" + std::to_string(AFM_PORT));
        if (connectTcp("127.0.0.1", m_localPort) && verifyConnection()) {
            LOG_INFO("Server", "Connected to existing server via ADB Forward");
            m_statusText = "Connected via ADB Forward";
            m_directConnection = false;
            m_connected = true;
            return true;
        }
        LOG_WARN("Server", "ADB Forward to existing server failed");
        disconnectTcp();
    }

    m_serverPort = launchPort;

    // 2. Server not running - push and start it
    m_statusText = "Pushing server binary to device...";
    std::string serverBin = getServerBinaryPath();
    LOG_INFO("Server", "Step 2: Push server binary: " + serverBin);
    if (!std::filesystem::exists(serverBin)) {
        m_lastError = "Server binary not found: " + serverBin;
        LOG_ERROR("Server", m_lastError);
        m_statusText = "ERROR: " + m_lastError;
        return false;
    }

    std::string pushOut = runAdbCommand("-s " + serial + " push \"" + serverBin + "\" /data/local/tmp/afm-server");
    if (pushOut.find("error") != std::string::npos && pushOut.find("pushed") == std::string::npos) {
        m_lastError = "Failed to push server: " + pushOut;
        LOG_ERROR("Server", m_lastError);
        m_statusText = "ERROR: " + m_lastError;
        return false;
    }
    LOG_INFO("Server", "Push result: OK");

    LOG_INFO("Server", "Step 3: Starting server process...");
    m_statusText = "Starting server on device...";
    runAdbCommand("-s " + serial + " shell chmod 755 /data/local/tmp/afm-server");
    // Kill any prior server, whether it was started as shell or root.
    stopDeviceServerProcesses(serial);
    runAdbCommand("-s " + serial + " shell rm -f /data/local/tmp/afm-server.log");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Bound the launcher separately from the helper readiness checks.
    std::string launchInner = serverLaunchCommand(launchPort, m_useRoot);
    auto launch = runAdbCommandResult("-s " + serial + " shell \"" + launchInner + "\"", 5000);
    if (launch.cancelled) return false;
    if (!launch.succeeded()) LOG_WARN("Server", "Helper launch: " + launch.diagnostics());

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Verify server is actually running on the device
    std::string psCheck = runAdbCommand("-s " + serial + " shell \"pidof afm-server 2>/dev/null\"");
    bool serverPidFound = !psCheck.empty() && psCheck.find_first_of("0123456789") != std::string::npos;
    LOG_INFO("Server", "pidof afm-server: '" + psCheck + "' -> " + std::string(serverPidFound ? "FOUND" : "NOT FOUND"));
    if (!serverPidFound) {
        std::string startupLog = sanitizeAdbOutput(runAdbCommand("-s " + serial + " shell \"cat /data/local/tmp/afm-server.log 2>/dev/null\""));
        if (!startupLog.empty()) {
            LOG_ERROR("Server", "afm-server startup log: " + startupLog);
            m_lastError = "Device helper failed to start: " + startupLog;
        } else {
            std::string fileInfo = sanitizeAdbOutput(runAdbCommand("-s " + serial + " shell \"ls -l /data/local/tmp/afm-server 2>/dev/null; getprop ro.product.cpu.abi; getprop ro.build.version.sdk\""));
            if (!fileInfo.empty()) LOG_ERROR("Server", "afm-server not running, device info: " + fileInfo);
        }
    }

    // 3. Try direct TCP — use savedIp if detectDeviceIp fails (ADB may be flaky after tethering)
    std::string retryIp = detectDeviceIp(serial);
    LOG_INFO("Server", "Step 4: retryIp='" + retryIp + "' savedIp='" + savedIp + "' deviceIp='" + deviceIp + "'");
    if (retryIp.empty()) retryIp = savedIp;
    if (retryIp.empty()) retryIp = deviceIp;

    if (!retryIp.empty() && !preferAdbForward) {
        LOG_INFO("Server", "Step 5: Direct TCP retries to " + retryIp);
        int delayMs = 1000;
        for (int attempt = 0; attempt < 15; attempt++) {
            LOG_DEBUG("Server", "Direct TCP attempt " + std::to_string(attempt+1) + "/15 (delay=" + std::to_string(delayMs) + "ms)");
            m_statusText = "Direct TCP (" + retryIp + ") attempt " + std::to_string(attempt + 1) + "/15"
                + (serverPidFound ? "" : " [server not detected]") + " ...";
            if (tryDirectConnect(retryIp, launchPort)) {
                LOG_INFO("Server", "Direct TCP connected on attempt " + std::to_string(attempt+1));
                m_statusText = "Connected via Direct TCP";
                m_connected = true;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            if (delayMs < 8000) delayMs = std::min(delayMs * 2, 8000);
            // Re-detect IP periodically in case it changed
            if (attempt % 5 == 4) {
                LOG_DEBUG("Server", "Re-detecting IP and checking server process...");
                std::string newIp = detectDeviceIp(serial);
                if (!newIp.empty() && newIp != retryIp) { LOG_INFO("Server", "IP changed: " + retryIp + " -> " + newIp); retryIp = newIp; }
                // Also re-check if server is running
                psCheck = runAdbCommand("-s " + serial + " shell \"pidof afm-server 2>/dev/null\"");
                serverPidFound = !psCheck.empty() && psCheck.find_first_of("0123456789") != std::string::npos;
                LOG_INFO("Server", std::string("pidof re-check: ") + (serverPidFound ? "FOUND" : "NOT FOUND"));
                // If server isn't running, try to start it again
                if (!serverPidFound) {
                    LOG_WARN("Server", "Server not running - restarting...");
                    m_statusText = "Server not running - restarting...";
                    runAdbCommand("-s " + serial + " shell rm -f /data/local/tmp/afm-server.log");
                    auto restart = runAdbCommandResult("-s " + serial + " shell \"" +
                        serverLaunchCommand(launchPort, m_useRoot) + "\"", 5000);
                    if (restart.cancelled) return false;
                    if (!restart.succeeded()) LOG_WARN("Server", "Helper restart: " + restart.diagnostics());
                    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                }
            }
        }
        LOG_WARN("Server", "All 15 Direct TCP attempts failed");
    } else {
        LOG_WARN("Server", "No IP for Direct TCP retries, skipping");
    }

    // 4. ADB forward (reliable fallback)
    LOG_INFO("Server", "Step 6: ADB Forward fallback");
    m_statusText = "Setting up ADB Forward...";
    runAdbCommand("-s " + serial + " forward --remove tcp:" + std::to_string(m_localPort));
    runAdbCommand("-s " + serial + " forward tcp:" + std::to_string(m_localPort) +
                  " tcp:" + std::to_string(launchPort));

    for (int attempt = 0; attempt < 5; attempt++) {
        LOG_DEBUG("Server", "ADB Forward attempt " + std::to_string(attempt+1) + "/5");
        m_statusText = "ADB Forward attempt " + std::to_string(attempt + 1) + "/5...";
        if (connectTcp("127.0.0.1", m_localPort) && verifyConnection()) {
            LOG_INFO("Server", "Connected via ADB Forward on attempt " + std::to_string(attempt+1));
            m_directConnection = false;
            m_connected = true;
            m_statusText = "Connected via ADB Forward";
            return true;
        }
        disconnectTcp();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    m_lastError = "Failed to connect to server after all attempts";
    LOG_ERROR("Server", m_lastError);
    m_statusText = "ERROR: " + m_lastError;
    return false;
}

bool DeviceClient::connectDirect(const std::string& ip) {
    disconnectTcp();
    if (connectTcp(ip, AFM_PORT) && verifyConnection()) {
        m_connected = true;
        m_directConnection = true;
        m_deviceIp = ip;
        publishConnectionProfile();
        // Remove adb forward if it was set
        if (!m_serial.empty())
            runAdbCommand("-s " + m_serial + " forward --remove tcp:" + std::to_string(m_localPort));
        return true;
    }
    disconnectTcp();
    return false;
}

bool DeviceClient::connectDirectForSerial(const std::string& serial, const std::string& ip, int port) {
    if (serial.empty() || ip.empty() || port <= 0) return false;
    std::lock_guard<std::mutex> serverLk(m_serverMutex);
    disconnectTcp();
    m_serial = serial;
    m_serverPort = port;
    m_directConnection = false;
    m_deviceIp.clear();
    if (connectTcp(ip, port) && verifyConnection()) {
        m_connected = true;
        m_directConnection = true;
        m_deviceIp = ip;
        m_statusText = "Connected via WiFi failover";
        publishConnectionProfile();
        LOG_INFO("Server", "Connected directly through WiFi failover: " + serial +
            " on helper port " + std::to_string(port));
        return true;
    }
    disconnectTcp();
    return false;
}

bool DeviceClient::tryEnableUsbTethering(const std::string& serial) {
    std::string ser = serial.empty() ? m_serial : serial;
    if (ser.empty()) return false;
    LOG_INFO("Tether", "tryEnableUsbTethering(serial=" + ser + ")");
    m_statusText = "Enabling USB tethering...";

    // Enabling rndis changes USB function which causes ADB to disconnect/reconnect
    runAdbCommand("-s " + ser + " shell svc usb setFunctions rndis");

    // Wait for ADB to reconnect after USB mode change
    m_statusText = "Waiting for USB to settle...";
    for (int i = 0; i < 15; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto devices = getDevices();
        for (auto& d : devices) {
            if (d.state == "device") {
                // A device is back (serial might change after USB mode switch)
                std::this_thread::sleep_for(std::chrono::seconds(2));
                // Check all USB tethering interfaces — different devices use different names
                const char* tetherIfaces[] = { "rndis0", "usb0", "ncm0", nullptr };
                for (int j = 0; tetherIfaces[j]; j++) {
                    std::string out = runAdbCommand("-s " + d.serial +
                        " shell \"ip -4 addr show " + tetherIfaces[j] + " 2>/dev/null | grep inet\"");
                    if (out.find("inet ") != std::string::npos) {
                        // Parse and save the IP now while ADB still works
                        auto inetPos = out.find("inet ") + 5;
                        auto slashPos = out.find('/', inetPos);
                        auto spacePos = out.find(' ', inetPos);
                        auto endPos = std::min(slashPos, spacePos);
                        if (endPos != std::string::npos && endPos > inetPos)
                            m_deviceIp = out.substr(inetPos, endPos - inetPos);
                        LOG_INFO("Tether", std::string("Tethering enabled! ") + tetherIfaces[j] + " IP: " + m_deviceIp + " (device serial: " + d.serial + ")");
                        m_statusText = "USB tethering enabled";
                        return true;
                    }
                }
                LOG_DEBUG("Tether", "Device " + d.serial + " found but no USB tethering IP yet (attempt " + std::to_string(i) + "/15)");
            }
        }
    }

    LOG_WARN("Tether", "Tethering not available after 15 attempts");
    m_statusText = "Tethering not available";
    return false;
}

bool DeviceClient::tryUpgradeToDirect() {
    if (!m_connected || m_serial.empty() || m_directConnection) return m_directConnection;
    std::string ip = detectDeviceIp(m_serial);
    if (ip.empty()) return false;
    uintptr_t candidate = openServerSocket(ip, m_serverPort, "");
    if (candidate == (uintptr_t)INVALID_SOCKET) return false;
    MsgHeader header{};
    std::vector<char> response;
    bool valid = sendMsgOn(candidate, CMD_PING, nullptr, 0) && recvMsgOn(candidate, header, response);
    uint32_t magic = 0, version = 0, uid = UINT32_MAX;
    if (valid && header.cmd == RSP_OK && response.size() >= 12) {
        memcpy(&magic, response.data(), 4);
        memcpy(&version, response.data() + 4, 4);
        memcpy(&uid, response.data() + 8, 4);
    }
    if (magic != AFM_MAGIC || version != AFM_VERSION || (m_useRoot && uid != 0)) {
        closesocket((SOCKET)candidate); return false;
    }
    {
        std::scoped_lock lock(m_mutex, m_ctlMutex);
        if (!m_connected) { closesocket((SOCKET)candidate); return false; }
        if (m_socket != (uintptr_t)INVALID_SOCKET) closesocket((SOCKET)m_socket);
        closeControlChannelLocked();
        m_socket = candidate;
        m_directConnection = true;
        m_deviceIp = ip;
        m_serverIsRoot = uid == 0;
        m_lastConnectHost = ip;
        m_lastConnectPort = m_serverPort;
        m_lastConnectBindIp.clear();
        publishConnectionProfile();
    }
    runAdbCommand("-s " + m_serial + " forward --remove tcp:" + std::to_string(m_localPort));
    return true;
}

void DeviceClient::stopServer() {
    std::lock_guard<std::mutex> serverLk(m_serverMutex);
    if (m_connected) {
        std::lock_guard<std::mutex> lk(m_mutex);
        sendMsg(CMD_QUIT);
        MsgHeader hdr; std::vector<char> payload;
        recvMsg(hdr, payload);
    }
    disconnectTcp();

    if (!m_serial.empty() && !m_adbPath.empty()) {
        if (!m_directConnection)
            runAdbCommand("-s " + m_serial + " forward --remove tcp:" + std::to_string(m_localPort));
        runAdbCommand("-s " + m_serial + " shell killall afm-server 2>/dev/null");
    }

    m_connected = false;
    m_directConnection = false;
    m_deviceIp.clear();
    m_serial.clear();
}

uintptr_t DeviceClient::openServerSocket(const std::string& host, int port,
                                         const std::string& bindLocalIp) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        LOG_ERROR("TCP", "socket() failed: WSA " + std::to_string(WSAGetLastError()));
        return (uintptr_t)INVALID_SOCKET;
    }
    int bufsize = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&bufsize, sizeof(bufsize));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsize, sizeof(bufsize));
    BOOL nodelay = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

    if (!bindLocalIp.empty()) {
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = 0;
        inet_pton(AF_INET, bindLocalIp.c_str(), &bindAddr.sin_addr);
        if (bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) != 0) {
            closesocket(sock);
            return (uintptr_t)INVALID_SOCKET;
        }
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
    int ret = connect(sock, (sockaddr*)&addr, sizeof(addr));
    if (ret != 0) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) { closesocket(sock); return (uintptr_t)INVALID_SOCKET; }
        fd_set writeSet, exceptSet;
        FD_ZERO(&writeSet); FD_SET(sock, &writeSet);
        FD_ZERO(&exceptSet); FD_SET(sock, &exceptSet);
        timeval tv = { 5, 0 };
        int selRet = select(0, nullptr, &writeSet, &exceptSet, &tv);
        if (selRet <= 0 || FD_ISSET(sock, &exceptSet)) {
            closesocket(sock); return (uintptr_t)INVALID_SOCKET;
        }
        int sockErr = 0; int optLen = sizeof(sockErr);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&sockErr, &optLen);
        if (sockErr != 0) { closesocket(sock); return (uintptr_t)INVALID_SOCKET; }
    }
    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);

    DWORD sndTimeout = 30000;
    DWORD rcvTimeout = 10000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTimeout, sizeof(rcvTimeout));

    BOOL keepAlive = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepAlive, sizeof(keepAlive));
    DWORD keepIdle = 5000, keepInterval = 1000, keepCount = 3;
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, (const char*)&keepIdle, sizeof(keepIdle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, (const char*)&keepInterval, sizeof(keepInterval));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, (const char*)&keepCount, sizeof(keepCount));

    return (uintptr_t)sock;
}

bool DeviceClient::connectTcp(const std::string& host, int port, const std::string& bindLocalIp) {
    LOG_DEBUG("TCP", "Connecting to " + host + ":" + std::to_string(port) + "...");
    uintptr_t sockU = openServerSocket(host, port, bindLocalIp);
    if (sockU == (uintptr_t)INVALID_SOCKET) {
        LOG_WARN("TCP", "openServerSocket failed for " + host + ":" + std::to_string(port));
        return false;
    }
    LOG_INFO("TCP", "Connected to " + host + ":" + std::to_string(port));
    std::scoped_lock lock(m_mutex, m_ctlMutex);
    if (m_socket != (uintptr_t)INVALID_SOCKET) closesocket((SOCKET)m_socket);
    closeControlChannelLocked();
    m_socket = sockU;
    m_lastConnectHost = host;
    m_lastConnectPort = port;
    m_lastConnectBindIp = bindLocalIp;
    publishConnectionProfile();
    m_connected = true;
    return true;
}

void DeviceClient::publishConnectionProfile() {
    std::lock_guard<std::mutex> lock(m_profileMutex);
    m_connectionProfile = {m_lastConnectHost, m_lastConnectBindIp, m_serial, m_adbPath, m_deviceIp,
        m_lastConnectPort, m_serverPort, m_directConnection, m_useRoot};
}

bool DeviceClient::connectParallelFrom(const DeviceClient& source) {
    if (&source == this) return false;
    ConnectionProfile profile;
    {
        std::lock_guard<std::mutex> lock(source.m_profileMutex);
        if (!source.m_connected || source.m_startServerActive) return false;
        profile = source.m_connectionProfile;
    }
    if (profile.host.empty()) return false;
    m_adbPath = profile.adbPath;
    m_serial = profile.serial;
    m_useRoot = profile.useRoot;
    m_directConnection = profile.direct;
    m_deviceIp = profile.deviceIp;
    m_serverPort = profile.serverPort;
    m_ownsServer = false;
    if (!connectTcp(profile.host, profile.port, profile.bindIp)) return false;
    if (!verifyConnection() || (m_useRoot && !m_serverIsRoot)) {
        disconnectTcp();
        return false;
    }
    return true;
}

// Static send/recv helpers operating on an arbitrary socket.
bool DeviceClient::sendAllOn(uintptr_t sockU, const void* data, size_t len) {
    SOCKET sock = (SOCKET)sockU;
    if (sock == INVALID_SOCKET) return false;
    const char* p = (const char*)data;
    while (len > 0) {
        int n = send(sock, p, (int)std::min(len, (size_t)INT_MAX), 0);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}
bool DeviceClient::recvAllOn(uintptr_t sockU, void* data, size_t len) {
    SOCKET sock = (SOCKET)sockU;
    if (sock == INVALID_SOCKET) return false;
    char* p = (char*)data;
    while (len > 0) {
        int n = recv(sock, p, (int)std::min(len, (size_t)INT_MAX), 0);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}
bool DeviceClient::sendMsgOn(uintptr_t sock, uint32_t cmd, const void* payload, uint32_t len) {
    MsgHeader hdr = { cmd, len };
    if (!sendAllOn(sock, &hdr, sizeof(hdr))) return false;
    if (len > 0 && !sendAllOn(sock, payload, len)) return false;
    return true;
}
bool DeviceClient::recvMsgOn(uintptr_t sock, MsgHeader& hdr, std::vector<char>& payload) {
    if (!recvAllOn(sock, &hdr, sizeof(hdr))) return false;
    const uint32_t maxPayload = AFM_CHUNK_SIZE + 65536;
    if (hdr.length > maxPayload) return false;
    payload.resize(hdr.length);
    if (hdr.length > 0 && !recvAllOn(sock, payload.data(), hdr.length)) return false;
    return true;
}

bool DeviceClient::ensureControlChannelLocked() {
    if (m_ctlConnected && m_ctlSocket != (uintptr_t)INVALID_SOCKET) return true;
    if (!m_connected || m_lastConnectHost.empty()) return false;
    uintptr_t sockU = openServerSocket(m_lastConnectHost, m_lastConnectPort, m_lastConnectBindIp);
    if (sockU == (uintptr_t)INVALID_SOCKET) return false;
    m_ctlSocket = sockU;
    m_ctlConnected = true;
    LOG_INFO("TCP", "Control channel connected");
    return true;
}

void DeviceClient::closeControlChannelLocked() {
    if (m_ctlSocket != (uintptr_t)INVALID_SOCKET) {
        closesocket((SOCKET)m_ctlSocket);
        m_ctlSocket = (uintptr_t)INVALID_SOCKET;
    }
    m_ctlConnected = false;
}

bool DeviceClient::runSmallOp(uint32_t cmd, const void* payload, uint32_t plen,
                              MsgHeader& outHdr, std::vector<char>& outPayload,
                              uint32_t recvTimeoutMs) {
    // Try control channel first.
    {
        std::lock_guard<std::mutex> lk(m_ctlMutex);
        if (ensureControlChannelLocked()) {
            SOCKET sock = (SOCKET)m_ctlSocket;
            DWORD origTimeout = 10000;
            if (recvTimeoutMs) {
                DWORD t = recvTimeoutMs;
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
            }
            bool ok = sendMsgOn(m_ctlSocket, cmd, payload, plen) &&
                      recvMsgOn(m_ctlSocket, outHdr, outPayload);
            if (recvTimeoutMs) {
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
            }
            if (ok) return true;
            // Channel died — close it; we'll fall back to primary.
            closeControlChannelLocked();
            if (cmd == CMD_DELETE || cmd == CMD_MKDIR || cmd == CMD_RENAME || cmd == CMD_TRUNCATE_FILE) {
                m_lastError = "Connection lost before the operation was confirmed. Refresh to check the result.";
                return false;
            }
        }
    }
    // Fall back to primary channel.
    std::unique_lock<std::mutex> lk(m_mutex, std::try_to_lock);
    if (!lk.owns_lock()) {
        m_lastError = "Control connection unavailable while a transfer is active";
        return false;
    }
    SOCKET sock = (SOCKET)m_socket;
    DWORD origTimeout = 10000;
    if (recvTimeoutMs) {
        DWORD t = recvTimeoutMs;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
    }
    bool ok = sendMsg(cmd, payload, plen) && recvMsg(outHdr, outPayload);
    if (recvTimeoutMs) {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
    }
    return ok;
}

void DeviceClient::flushStaleData() {
    if (!m_connected) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    SOCKET sock = (SOCKET)m_socket;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(sock, &readable);
    timeval immediate{};
    int ready = select(0, &readable, nullptr, nullptr, &immediate);
    if (ready < 0) { abortTransfer("Could not check transfer connection"); return; }
    if (ready > 0) abortTransfer("Transfer connection has unread data and must reconnect");
}

void DeviceClient::interruptBlocking() {
    SOCKET sock = (SOCKET)m_socket;
    if (sock != INVALID_SOCKET) {
        shutdown(sock, SD_BOTH);
        m_connected = false;
    }
    SOCKET control = (SOCKET)m_ctlSocket.load();
    if (control != INVALID_SOCKET) shutdown(control, SD_BOTH);
}

void DeviceClient::restoreTimeouts() {
    SOCKET sock = (SOCKET)m_socket;
    if (sock != INVALID_SOCKET) {
        DWORD timeout = 10000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        timeout = 30000;
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    }
}

void DeviceClient::disconnectTcp() {
    interruptBlocking();
    std::scoped_lock lock(m_mutex, m_ctlMutex);
    if (m_socket != (uintptr_t)INVALID_SOCKET) {
        closesocket((SOCKET)m_socket);
        m_socket = (uintptr_t)INVALID_SOCKET;
    }
    m_connected = false;
    m_directConnection = false;
    m_serverIsRoot = false;
    closeControlChannelLocked();
}

bool DeviceClient::sendAll(const void* data, size_t len) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    const char* p = (const char*)data;
    SOCKET sock = (SOCKET)m_socket;
    while (len > 0) {
        int n = send(sock, p, (int)std::min(len, (size_t)INT_MAX), 0);
        if (n <= 0) {
            int wsaErr = WSAGetLastError();
            m_lastError = "Send failed (WSA error " + std::to_string(wsaErr) + ")";
            LOG_ERROR("TCP", m_lastError);
            m_connected = false;
            return false;
        }
        p += n; len -= n;
    }
    return true;
}

bool DeviceClient::recvAll(void* data, size_t len) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    char* p = (char*)data;
    SOCKET sock = (SOCKET)m_socket;
    while (len > 0) {
        int n = recv(sock, p, (int)std::min(len, (size_t)INT_MAX), 0);
        if (n <= 0) {
            int wsaErr = WSAGetLastError();
            m_lastError = "Recv failed (WSA error " + std::to_string(wsaErr) + ")";
            LOG_ERROR("TCP", m_lastError);
            m_connected = false;
            return false;
        }
        p += n; len -= n;
    }
    return true;
}

bool DeviceClient::sendMsg(uint32_t cmd, const void* payload, uint32_t len) {
    MsgHeader hdr = { cmd, len };
    if (!sendAll(&hdr, sizeof(hdr))) return false;
    if (len > 0 && !sendAll(payload, len)) return false;
    return true;
}

bool DeviceClient::recvMsg(MsgHeader& hdr, std::vector<char>& payload) {
    if (!recvAll(&hdr, sizeof(hdr))) return false;
    // Sanity check: reject absurd payload sizes (max ~8MB: chunk + headers + path)
    const uint32_t maxPayload = AFM_CHUNK_SIZE + 65536;
    if (hdr.length > maxPayload) {
        m_lastError = "Protocol error: payload too large (" + std::to_string(hdr.length) + " bytes)";
        m_connected = false;
        return false;
    }
    payload.resize(hdr.length);
    if (hdr.length > 0 && !recvAll(payload.data(), hdr.length)) return false;
    return true;
}

std::string DeviceClient::detectStoragePath() {
    if (!m_connected) return "/storage/emulated/0";
    MsgHeader hdr; std::vector<char> payload;
    if (!runSmallOp(CMD_STORAGE, nullptr, 0, hdr, payload)) return "/storage/emulated/0";
    if (hdr.cmd == RSP_OK && !payload.empty())
        return std::string(payload.data(), payload.size());
    return "/storage/emulated/0";
}

static bool parseDirectoryEntries(const std::vector<char>& payload, std::vector<DeviceFileEntry>& entries) {
    if (payload.size() < 4) return false;
    uint32_t count = 0;
    memcpy(&count, payload.data(), 4);
    if (count > (payload.size() - 4) / (sizeof(FileEntryHeader) + 1)) return false;
    entries.reserve(count);
    size_t offset = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (payload.size() - offset < sizeof(FileEntryHeader)) return false;
        FileEntryHeader header{};
        memcpy(&header, payload.data() + offset, sizeof(header));
        offset += sizeof(header);
        if (header.type > 2 || header.name_len == 0 || header.name_len > payload.size() - offset) return false;
        std::string name(payload.data() + offset, header.name_len);
        if (name == "." || name == ".." || name.find('\0') != std::string::npos || name.find('/') != std::string::npos)
            return false;
        entries.push_back({std::move(name), header.type, header.size, header.mtime});
        offset += header.name_len;
    }
    return offset == payload.size();
}

std::vector<DeviceFileEntry> DeviceClient::listDirectory(const std::string& path, bool* succeeded) {
    std::vector<DeviceFileEntry> entries;
    if (succeeded) *succeeded = false;
    m_lastError.clear();
    if (!m_connected) { m_lastError = "Not connected"; return entries; }
    MsgHeader header{};
    std::vector<char> payload;
    if (!runSmallOp(CMD_LIST, path.data(), (uint32_t)path.size(), header, payload)) {
        if (m_lastError.empty()) m_lastError = "Directory request failed";
        return entries;
    }
    if (header.cmd == RSP_ERROR) { m_lastError.assign(payload.begin(), payload.end()); return entries; }
    if (header.cmd != RSP_OK || !parseDirectoryEntries(payload, entries)) {
        m_lastError = "Invalid directory listing response";
        entries.clear();
        return entries;
    }
    std::sort(entries.begin(), entries.end(), [](const DeviceFileEntry& a, const DeviceFileEntry& b) {
        if (a.isDirectory() != b.isDirectory()) return a.isDirectory();
        return a.name < b.name;
    });
    if (succeeded) *succeeded = true;
    return entries;
}

std::vector<DeviceFileEntry> DeviceClient::listMcraw(const std::string& mcrawPath) {
    std::vector<DeviceFileEntry> entries;
    m_lastError.clear();
    if (!m_connected) { m_lastError = "Not connected"; return entries; }
    MsgHeader header{};
    std::vector<char> payload;
    if (!runSmallOp(CMD_MCRAW_LIST, mcrawPath.data(), (uint32_t)mcrawPath.size(), header, payload, 30000)) {
        if (m_lastError.empty()) m_lastError = "MCRAW listing request failed";
        return entries;
    }
    if (header.cmd == RSP_ERROR) { m_lastError.assign(payload.begin(), payload.end()); return entries; }
    if (header.cmd != RSP_OK || !parseDirectoryEntries(payload, entries)) {
        m_lastError = "Invalid MCRAW listing response";
        entries.clear();
    }
    return entries;
}

void DeviceClient::abortTransfer(const std::string& error) {
    m_lastError = error;
    m_inlineCrcPath.clear();
    m_connected = false;
    if (m_socket != (uintptr_t)INVALID_SOCKET) shutdown((SOCKET)m_socket, SD_BOTH);
}

bool DeviceClient::receiveFileStream(const std::string& localPath, uint64_t totalSize,
                                     uint64_t offset, bool calculateCrc, ProgressCallback progress) {
    m_inlineCrcPath.clear();
    if (totalSize > INT64_MAX || offset > totalSize) {
        abortTransfer("Partial file is larger than the source");
        return false;
    }
    if (progress && !progress(offset, totalSize)) { abortTransfer("Cancelled"); return false; }
    TransferFileHandle file(CreateFileW(toWide(localPath).c_str(), GENERIC_WRITE, 0, nullptr,
        offset ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE && !offset) {
        std::error_code error;
        std::filesystem::create_directories(toFsPath(localPath).parent_path(), error);
        file.reset(CreateFileW(toWide(localPath).c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    }
    if (file.get() == INVALID_HANDLE_VALUE) {
        abortTransfer("Failed to create local file: " + localPath + " (" + lastWin32ErrorText() + ")");
        return false;
    }
    if (offset) {
        LARGE_INTEGER currentSize{};
        if (!GetFileSizeEx(file.get(), &currentSize) || currentSize.QuadPart < 0 ||
            (uint64_t)currentSize.QuadPart != offset) {
            abortTransfer("Partial file changed before resume"); return false;
        }
    }
    LARGE_INTEGER position{};
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN)) {
        abortTransfer("Failed to seek local file: " + lastWin32ErrorText());
        return false;
    }
    if (calculateCrc) inlineCrcReset();
    if (totalSize - offset <= AFM_CHUNK_SIZE) {
        uint64_t received = offset;
        while (true) {
            MsgHeader header{};
            if (!recvAll(&header, sizeof(header))) return false;
            if (header.cmd == RSP_DONE) {
                if (header.length || received != totalSize) { abortTransfer("Incomplete file response"); return false; }
                if (calculateCrc) { m_inlineCrc = ~m_inlineCrc; m_inlineCrcPath = localPath; }
                return true;
            }
            if (header.cmd != RSP_DATA || header.length == 0 || header.length > AFM_CHUNK_SIZE ||
                header.length > totalSize - received) { abortTransfer("Invalid file data length"); return false; }
            if (m_transferBuf.size() < header.length) m_transferBuf.resize(header.length);
            if (!recvAll(m_transferBuf.data(), header.length)) return false;
            DWORD written = 0;
            if (!WriteFile(file.get(), m_transferBuf.data(), header.length, &written, nullptr) || written != header.length) {
                abortTransfer("Failed to write local file"); return false;
            }
            if (calculateCrc) inlineCrcUpdate(m_transferBuf.data(), header.length);
            received += header.length;
            if (progress && !progress(received, totalSize)) { abortTransfer("Cancelled"); return false; }
        }
    }
    std::vector<char> first(AFM_CHUNK_SIZE), second(AFM_CHUNK_SIZE);
    char* buffer = first.data();
    TransferWorker writer;
    uint64_t received = offset;
    bool complete = false;
    while (true) {
        MsgHeader header{};
        if (!recvAll(&header, sizeof(header))) break;
        if (header.cmd == RSP_DONE) {
            complete = header.length == 0 && received == totalSize;
            if (!complete) abortTransfer("Incomplete file response");
            break;
        }
        if (header.cmd != RSP_DATA || header.length == 0 || header.length > AFM_CHUNK_SIZE ||
            header.length > totalSize - received) {
            abortTransfer("Invalid file data length");
            break;
        }
        if (!recvAll(buffer, header.length)) break;
        if (!writer.wait()) { abortTransfer("Failed to write local file"); break; }
        char* writeBuffer = buffer;
        DWORD writeLength = header.length;
        writer.submit([this, handle = file.get(), writeBuffer, writeLength, calculateCrc] {
            DWORD written = 0;
            if (!WriteFile(handle, writeBuffer, writeLength, &written, nullptr) || written != writeLength)
                return false;
            if (calculateCrc) inlineCrcUpdate(writeBuffer, writeLength);
            return true;
        });
        received += header.length;
        buffer = buffer == first.data() ? second.data() : first.data();
        if (progress && !progress(received, totalSize)) { abortTransfer("Cancelled"); break; }
    }
    if (!writer.wait()) { abortTransfer("Failed to write local file"); complete = false; }
    if (!complete) {
        abortTransfer(m_lastError.empty() ? "Incomplete file response" : m_lastError);
        return false;
    }
    if (calculateCrc) { m_inlineCrc = ~m_inlineCrc; m_inlineCrcPath = localPath; }
    return true;
}

bool DeviceClient::pullMcrawItem(const std::string& mcrawPath, const std::string& virtualName,
                                 const std::string& localPath, uint64_t& outFileSize,
                                 ProgressCallback progress) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    uint32_t pathLength = (uint32_t)mcrawPath.size();
    std::vector<char> request(4 + pathLength + virtualName.size());
    memcpy(request.data(), &pathLength, 4);
    memcpy(request.data() + 4, mcrawPath.data(), pathLength);
    memcpy(request.data() + 4 + pathLength, virtualName.data(), virtualName.size());
    DWORD timeout = 60000;
    setsockopt((SOCKET)m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    MsgHeader header{};
    std::vector<char> response;
    bool ready = sendMsg(CMD_MCRAW_EXTRACT, request.data(), (uint32_t)request.size()) && recvMsg(header, response);
    timeout = 10000;
    setsockopt((SOCKET)m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    if (!ready) { abortTransfer(m_lastError); return false; }
    if (header.cmd == RSP_ERROR) { m_lastError.assign(response.begin(), response.end()); return false; }
    if (header.cmd != RSP_OK || response.size() != sizeof(PullHeader)) {
        abortTransfer("Invalid MCRAW response"); return false;
    }
    memcpy(&outFileSize, response.data(), sizeof(outFileSize));
    return receiveFileStream(localPath, outFileSize, 0, false, progress);
}

bool DeviceClient::pullFile(const std::string& remotePath, const std::string& localPath,
                            uint64_t& outFileSize, ProgressCallback progress) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    MsgHeader header{};
    std::vector<char> response;
    if (!sendMsg(CMD_PULL, remotePath.data(), (uint32_t)remotePath.size()) || !recvMsg(header, response)) return false;
    if (header.cmd == RSP_ERROR) { m_lastError.assign(response.begin(), response.end()); return false; }
    if (header.cmd != RSP_OK || response.size() != sizeof(PullHeader)) {
        abortTransfer("Invalid file response"); return false;
    }
    memcpy(&outFileSize, response.data(), sizeof(outFileSize));
    return receiveFileStream(localPath, outFileSize, 0, true, progress);
}

bool DeviceClient::archiveRemoteDirectory(const std::string& remotePath, const std::string& localTarPath, bool excludeCache,
                                          uint64_t& outFileSize, ProgressCallback progress) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    LOG_INFO("BackupServer", "Archive START: " + remotePath + " -> " + localTarPath);
    std::lock_guard<std::mutex> lk(m_mutex);

    SOCKET sock = (SOCKET)m_socket;
    DWORD longTimeout = 300000;
    DWORD origTimeout = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&longTimeout, sizeof(longTimeout));

    uint32_t exclude = excludeCache ? 1u : 0u;
    std::vector<char> request(4 + remotePath.size());
    memcpy(request.data(), &exclude, 4);
    memcpy(request.data() + 4, remotePath.data(), remotePath.size());
    if (!sendMsg(CMD_ARCHIVE_PATH, request.data(), (uint32_t)request.size())) {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
        return false;
    }

    MsgHeader hdr; std::vector<char> payload;
    if (!recvMsg(hdr, payload)) {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
        return false;
    }
    if (hdr.cmd == RSP_ERROR) {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
        m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    if (hdr.cmd != RSP_OK || payload.size() < sizeof(PullHeader)) {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
        m_lastError = "Invalid archive response";
        return false;
    }

    PullHeader ph;
    memcpy(&ph, payload.data(), sizeof(ph));
    outFileSize = ph.file_size;

    bool success = receiveFileStream(localTarPath, outFileSize, 0, false, progress);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));

    if (success) LOG_INFO("BackupServer", "Archive OK: " + std::to_string(outFileSize) + " bytes");
    else LOG_ERROR("BackupServer", "Archive FAILED: " + m_lastError);
    return success;
}

bool DeviceClient::extractRemoteArchive(const std::string& localTarPath, const std::string& remoteTarget,
                                        ProgressCallback progress) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    std::filesystem::path localPath = toFsPath(localTarPath);
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(localPath, ec);
    if (ec) {
        m_lastError = "Could not read local archive size";
        return false;
    }
    std::string remoteArchive = "/data/local/tmp/afm-restore-" + sanitizePathPart(remoteTarget) + ".tar";
    if (!pushFile(localTarPath, remoteArchive, size, progress)) return false;

    std::lock_guard<std::mutex> lk(m_mutex);
    uint32_t targetLen = (uint32_t)remoteTarget.size();
    uint32_t archiveLen = (uint32_t)remoteArchive.size();
    std::vector<char> payload(8 + targetLen + archiveLen);
    memcpy(payload.data(), &targetLen, 4);
    memcpy(payload.data() + 4, &archiveLen, 4);
    memcpy(payload.data() + 8, remoteTarget.data(), targetLen);
    memcpy(payload.data() + 8 + targetLen, remoteArchive.data(), archiveLen);

    SOCKET sock = (SOCKET)m_socket;
    DWORD longTimeout = 300000;
    DWORD origTimeout = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&longTimeout, sizeof(longTimeout));
    bool sent = sendMsg(CMD_EXTRACT_ARCHIVE, payload.data(), (uint32_t)payload.size());
    MsgHeader hdr{}; std::vector<char> resp;
    bool got = sent && recvMsg(hdr, resp);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
    if (!got) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return false;
    }
    return hdr.cmd == RSP_OK;
}

bool DeviceClient::finishWrite(uint64_t expectedBytes, bool rangeWrite, uint32_t* crc) {
    MsgHeader header{};
    std::vector<char> response;
    if (!sendMsg(RSP_DONE) || !recvMsg(header, response)) { abortTransfer("No write confirmation"); return false; }
    if (header.cmd == RSP_ERROR) { abortTransfer(std::string(response.begin(), response.end())); return false; }
    if (header.cmd != RSP_OK) { abortTransfer("Invalid write confirmation"); return false; }
    if (rangeWrite) {
        uint64_t written = 0;
        if (response.size() >= sizeof(written)) memcpy(&written, response.data(), sizeof(written));
        if (response.size() < sizeof(written) || written != expectedBytes) {
            abortTransfer("Incomplete write confirmation"); return false;
        }
        if (crc) {
            if (response.size() < 12) { abortTransfer("Missing range checksum"); return false; }
            memcpy(crc, response.data() + 8, 4);
        }
    }
    return true;
}

bool DeviceClient::sendFileStream(void* file, uint64_t length, uint64_t progressOffset,
                                  bool calculateCrc, bool rangeWrite, ProgressCallback progress) {
    m_inlineCrcPath.clear();
    if (calculateCrc) inlineCrcReset();
    if (length <= AFM_CHUNK_SIZE) {
        if (progress && !progress(progressOffset, progressOffset + length)) { abortTransfer("Cancelled"); return false; }
        if (length) {
            size_t size = sizeof(MsgHeader) + (size_t)length;
            if (m_transferBuf.size() < size) m_transferBuf.resize(size);
            DWORD count = 0;
            if (!ReadFile(file, m_transferBuf.data() + sizeof(MsgHeader), (DWORD)length, &count, nullptr) || count != length) {
                abortTransfer("Failed to read complete local file range"); return false;
            }
            if (calculateCrc) inlineCrcUpdate(m_transferBuf.data() + sizeof(MsgHeader), count);
            MsgHeader header{RSP_DATA, count};
            memcpy(m_transferBuf.data(), &header, sizeof(header));
            if (!sendAll(m_transferBuf.data(), size)) return false;
            if (progress && !progress(progressOffset + length, progressOffset + length)) { abortTransfer("Cancelled"); return false; }
        }
        if (!finishWrite(length, rangeWrite)) return false;
        if (calculateCrc) m_inlineCrc = ~m_inlineCrc;
        return true;
    }
    std::vector<char> first(sizeof(MsgHeader) + AFM_CHUNK_SIZE), second(first.size());
    char* buffer = first.data();
    TransferWorker sender;
    uint64_t sent = 0;
    uint32_t pending = 0;
    auto finishSend = [&] {
        if (!sender.wait()) return false;
        sent += pending;
        pending = 0;
        if (progress && !progress(progressOffset + sent, progressOffset + length)) {
            m_lastError = "Cancelled"; return false;
        }
        return true;
    };
    while (sent + pending < length) {
        DWORD toRead = (DWORD)std::min<uint64_t>(AFM_CHUNK_SIZE, length - sent - pending);
        DWORD count = 0;
        if (!ReadFile(file, buffer + sizeof(MsgHeader), toRead, &count, nullptr) || count != toRead) {
            interruptBlocking();
            sender.wait();
            abortTransfer("Failed to read complete local file range"); return false;
        }
        if (calculateCrc) inlineCrcUpdate(buffer + sizeof(MsgHeader), count);
        if (!finishSend()) { abortTransfer(m_lastError); return false; }
        MsgHeader header{RSP_DATA, count};
        memcpy(buffer, &header, sizeof(header));
        char* sendBuffer = buffer;
        sender.submit([this, sendBuffer, count] { return sendAll(sendBuffer, sizeof(MsgHeader) + count); });
        pending = count;
        buffer = buffer == first.data() ? second.data() : first.data();
    }
    if (!finishSend()) { abortTransfer(m_lastError); return false; }
    if (sent != length) { abortTransfer("Incomplete upload"); return false; }
    if (!finishWrite(length, rangeWrite)) return false;
    if (calculateCrc) m_inlineCrc = ~m_inlineCrc;
    return true;
}

bool DeviceClient::pushFile(const std::string& localPath, const std::string& remotePath,
                            uint64_t fileSize, ProgressCallback progress) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    auto file = openTransferSource(localPath, 0, fileSize, m_lastError, true);
    if (!file) return false;
    std::vector<char> request(sizeof(PushHeader) + remotePath.size());
    memcpy(request.data(), &fileSize, sizeof(fileSize));
    memcpy(request.data() + sizeof(PushHeader), remotePath.data(), remotePath.size());
    MsgHeader header{};
    std::vector<char> response;
    if (!sendMsg(CMD_PUSH, request.data(), (uint32_t)request.size()) || !recvMsg(header, response)) return false;
    if (header.cmd == RSP_ERROR) { m_lastError.assign(response.begin(), response.end()); return false; }
    if (header.cmd != RSP_OK) { abortTransfer("Invalid upload response"); return false; }
    if (!sendFileStream(file.get(), fileSize, 0, true, false, progress)) return false;
    m_inlineCrcPath = localPath;
    return true;
}

bool DeviceClient::relayFile(DeviceClient& src, DeviceClient& dst,
                             const std::string& srcPath, const std::string& dstPath,
                             uint64_t fileSize, ProgressCallback progress) {
    return relayStream(src, dst, srcPath, dstPath, 0, 0, fileSize, false, progress, nullptr);
}

bool DeviceClient::relayRange(DeviceClient& src, DeviceClient& dst,
                              const std::string& srcPath, const std::string& dstPath,
                              uint64_t srcOffset, uint64_t dstOffset, uint64_t length,
                              ProgressCallback progress, RelayRangeStats* stats) {
    return relayStream(src, dst, srcPath, dstPath, srcOffset, dstOffset, length, true, progress, stats);
}

bool DeviceClient::relayStream(DeviceClient& src, DeviceClient& dst,
                               const std::string& srcPath, const std::string& dstPath,
                               uint64_t srcOffset, uint64_t dstOffset, uint64_t length,
                               bool range, ProgressCallback progress, RelayRangeStats* stats) {
    if (&src == &dst) { src.m_lastError = "Relay requires separate connections"; return false; }
    std::scoped_lock lock(src.m_mutex, dst.m_mutex);
    if (!src.m_connected || !dst.m_connected) { src.m_lastError = "Relay device not connected"; return false; }
    if (range && (srcOffset > INT64_MAX || length > INT64_MAX - srcOffset ||
        dstOffset > INT64_MAX || length > INT64_MAX - dstOffset)) {
        src.m_lastError = "Invalid relay range bounds"; return false;
    }
    if (range && length == 0) return true;
    std::vector<char> request((range ? 16 : 0) + srcPath.size());
    if (range) { memcpy(request.data(), &srcOffset, 8); memcpy(request.data() + 8, &length, 8); }
    memcpy(request.data() + (range ? 16 : 0), srcPath.data(), srcPath.size());
    MsgHeader header{};
    std::vector<char> response;
    if (!src.sendMsg(range ? CMD_READ_RANGE : CMD_PULL, request.data(), (uint32_t)request.size()) ||
        !src.recvMsg(header, response)) return false;
    if (header.cmd == RSP_ERROR) { src.m_lastError.assign(response.begin(), response.end()); return false; }
    if (header.cmd != RSP_OK || response.size() != 8) { src.abortTransfer("Invalid relay response"); return false; }
    uint64_t actualSize = 0;
    memcpy(&actualSize, response.data(), 8);
    if (actualSize > INT64_MAX) { src.abortTransfer("Invalid relay file size"); return false; }
    if (range && actualSize != length) { src.abortTransfer("Source range changed during relay"); return false; }
    request.resize((range ? 16 : 8) + dstPath.size());
    if (range) { memcpy(request.data(), &dstOffset, 8); memcpy(request.data() + 8, &actualSize, 8); }
    else memcpy(request.data(), &actualSize, 8);
    memcpy(request.data() + (range ? 16 : 8), dstPath.data(), dstPath.size());
    if (!dst.sendMsg(range ? CMD_WRITE_RANGE : CMD_PUSH, request.data(), (uint32_t)request.size()) ||
        !dst.recvMsg(header, response) || header.cmd != RSP_OK) {
        if (header.cmd == RSP_ERROR) dst.m_lastError.assign(response.begin(), response.end());
        src.abortTransfer("Destination rejected relay"); return false;
    }
    size_t bufferSize = sizeof(MsgHeader) + (size_t)std::min<uint64_t>(actualSize, AFM_CHUNK_SIZE);
    std::vector<char> first(bufferSize), second(actualSize > AFM_CHUNK_SIZE ? bufferSize : 0);
    char* buffer = first.data();
    std::unique_ptr<TransferWorker> sender;
    if (actualSize > AFM_CHUNK_SIZE) sender = std::make_unique<TransferWorker>();
    uint64_t received = 0, sent = 0;
    uint32_t pending = 0, crc = ~(uint32_t)0;
    bool complete = false;
    auto finishSend = [&] {
        if (sender && !sender->wait()) return false;
        sent += pending;
        pending = 0;
        if (progress && !progress(sent, actualSize)) { src.m_lastError = "Cancelled"; return false; }
        return true;
    };
    while (true) {
        auto start = std::chrono::steady_clock::now();
        if (!src.recvAll(&header, sizeof(header))) break;
        if (header.cmd == RSP_DONE) {
            complete = header.length == 0 && received == actualSize;
            if (!complete) src.m_lastError = "Incomplete relay response";
            break;
        }
        if (header.cmd != RSP_DATA || header.length == 0 || header.length > AFM_CHUNK_SIZE ||
            header.length > actualSize - received) { src.m_lastError = "Invalid relay data length"; break; }
        if (!src.recvAll(buffer + sizeof(MsgHeader), header.length)) break;
        if (stats) stats->srcRecvSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (stats) crc32Update(crc, buffer + sizeof(MsgHeader), header.length);
        if (!finishSend()) break;
        memcpy(buffer, &header, sizeof(header));
        char* sendBuffer = buffer;
        uint32_t count = header.length;
        auto sendChunk = [&dst, sendBuffer, count, stats] {
            auto start = std::chrono::steady_clock::now();
            bool ok = dst.sendAll(sendBuffer, sizeof(MsgHeader) + count);
            if (stats) stats->dstSendSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            return ok;
        };
        if (sender) sender->submit(sendChunk);
        else if (!sendChunk()) break;
        pending = count;
        received += count;
        if (sender) buffer = buffer == first.data() ? second.data() : first.data();
    }
    if (!complete) {
        src.abortTransfer(src.m_lastError.empty() ? "Relay interrupted" : src.m_lastError);
        dst.interruptBlocking();
    }
    if (!finishSend()) complete = false;
    if (!complete || sent != actualSize) {
        src.abortTransfer(src.m_lastError.empty() ? "Relay interrupted" : src.m_lastError);
        dst.abortTransfer(dst.m_lastError.empty() ? "Relay interrupted" : dst.m_lastError);
        return false;
    }
    uint32_t destinationCrc = 0;
    if (!dst.finishWrite(actualSize, range, range && stats ? &destinationCrc : nullptr)) return false;
    if (stats) {
        stats->bytes += sent;
        stats->srcCrc = ~crc;
        stats->hasSrcCrc = true;
        stats->dstCrc = destinationCrc;
        stats->hasDstCrc = range;
    }
    return true;
}

bool DeviceClient::getDiskSpace(uint64_t& totalBytes, uint64_t& freeBytes) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    MsgHeader hdr; std::vector<char> payload;
    if (!runSmallOp(CMD_DISK_SPACE, nullptr, 0, hdr, payload)) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    if (hdr.cmd != RSP_OK || payload.size() < 16) return false;
    memcpy(&totalBytes, payload.data(), 8);
    memcpy(&freeBytes, payload.data() + 8, 8);
    return true;
}

uint64_t DeviceClient::writeRange(const std::string& remotePath, uint64_t offset,
                                   const void* data, uint32_t length) {
    if (!m_connected) { m_lastError = "Not connected"; return 0; }
    if ((!data && length) || offset > INT64_MAX || length > INT64_MAX - offset) {
        m_lastError = "Invalid range bounds"; return 0;
    }
    std::lock_guard<std::mutex> lk(m_mutex);

    // Build payload: [8B offset][8B length][path]
    uint64_t len64 = length;
    std::vector<char> cmdPayload(16 + remotePath.size());
    memcpy(cmdPayload.data(), &offset, 8);
    memcpy(cmdPayload.data() + 8, &len64, 8);
    memcpy(cmdPayload.data() + 16, remotePath.data(), remotePath.size());

    if (!sendMsg(CMD_WRITE_RANGE, cmdPayload.data(), (uint32_t)cmdPayload.size())) return 0;

    // Wait for RSP_OK (ready)
    MsgHeader hdr; std::vector<char> resp;
    if (!recvMsg(hdr, resp)) return 0;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return 0;
    }
    if (hdr.cmd != RSP_OK) return 0;

    // Send data as protocol-sized RSP_DATA chunks. Some servers close the
    // connection when a single data frame is much larger than AFM_CHUNK_SIZE.
    const char* p = (const char*)data;
    uint64_t sent = 0;
    while (sent < len64) {
        uint32_t chunkLen = (uint32_t)std::min<uint64_t>(AFM_CHUNK_SIZE, len64 - sent);
        MsgHeader dataHdr = { RSP_DATA, chunkLen };
        if (!sendAll(&dataHdr, sizeof(dataHdr))) return 0;
        if (!sendAll(p + sent, chunkLen)) return 0;
        sent += chunkLen;
    }

    return finishWrite(len64, true) ? len64 : 0;
}

uint64_t DeviceClient::writeRangeStreaming(const std::string& remotePath, uint64_t remoteOffset,
                                           const std::string& localPath, uint64_t localOffset,
                                           uint64_t length, ProgressCallback progress) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected) { m_lastError = "Not connected"; return 0; }
    if (remoteOffset > INT64_MAX || length > INT64_MAX - remoteOffset) {
        m_lastError = "Invalid range bounds"; return 0;
    }
    auto file = openTransferSource(localPath, localOffset, length, m_lastError);
    if (!file) return 0;
    std::vector<char> request(16 + remotePath.size());
    memcpy(request.data(), &remoteOffset, 8);
    memcpy(request.data() + 8, &length, 8);
    memcpy(request.data() + 16, remotePath.data(), remotePath.size());
    MsgHeader header{};
    std::vector<char> response;
    if (!sendMsg(CMD_WRITE_RANGE, request.data(), (uint32_t)request.size()) || !recvMsg(header, response)) return 0;
    if (header.cmd == RSP_ERROR) { m_lastError.assign(response.begin(), response.end()); return 0; }
    if (header.cmd != RSP_OK) { abortTransfer("Invalid range write response"); return 0; }
    return sendFileStream(file.get(), length, 0, false, true, progress) ? length : 0;
}

bool DeviceClient::createFile(const std::string& remotePath, uint64_t totalSize) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    std::lock_guard<std::mutex> lk(m_mutex);

    std::vector<char> payload(8 + remotePath.size());
    memcpy(payload.data(), &totalSize, 8);
    memcpy(payload.data() + 8, remotePath.data(), remotePath.size());

    if (!sendMsg(CMD_CREATE_FILE, payload.data(), (uint32_t)payload.size())) return false;

    MsgHeader hdr; std::vector<char> resp;
    if (!recvMsg(hdr, resp)) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return false;
    }
    return hdr.cmd == RSP_OK;
}

bool DeviceClient::truncateFile(const std::string& remotePath, uint64_t totalSize) {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    if (totalSize > INT64_MAX) { m_lastError = "Invalid file size"; return false; }
    std::vector<char> request(8 + remotePath.size());
    memcpy(request.data(), &totalSize, 8);
    memcpy(request.data() + 8, remotePath.data(), remotePath.size());
    MsgHeader header{};
    std::vector<char> response;
    if (!runSmallOp(CMD_TRUNCATE_FILE, request.data(), (uint32_t)request.size(), header, response)) return false;
    if (header.cmd == RSP_ERROR) { m_lastError.assign(response.begin(), response.end()); return false; }
    if (header.cmd != RSP_OK || !response.empty()) { m_lastError = "Invalid truncate response"; return false; }
    return true;
}

uint64_t DeviceClient::readRange(const std::string& remotePath, uint64_t offset, uint64_t length,
                                 void* outBuffer) {
    if (!outBuffer && length) { m_lastError = "Missing range buffer"; return 0; }
    return readRangeStreaming(remotePath, offset, length,
        [outBuffer, offset](const void* data, uint32_t count, uint64_t position) {
            memcpy((char*)outBuffer + (size_t)(position - offset), data, count);
            return true;
        });
}

uint64_t DeviceClient::readRangeStreaming(const std::string& remotePath, uint64_t offset, uint64_t length,
                                           ReadRangeCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected) { m_lastError = "Not connected"; return 0; }
    if (length > UINT64_MAX - offset) { m_lastError = "Invalid range bounds"; return 0; }
    std::vector<char> request(16 + remotePath.size());
    memcpy(request.data(), &offset, 8);
    memcpy(request.data() + 8, &length, 8);
    memcpy(request.data() + 16, remotePath.data(), remotePath.size());
    MsgHeader header{};
    std::vector<char> response;
    if (!sendMsg(CMD_READ_RANGE, request.data(), (uint32_t)request.size()) || !recvMsg(header, response)) return 0;
    if (header.cmd == RSP_ERROR) { m_lastError.assign(response.begin(), response.end()); return 0; }
    if (header.cmd != RSP_OK || response.size() != 8) { abortTransfer("Invalid range response"); return 0; }
    uint64_t actual = 0;
    memcpy(&actual, response.data(), 8);
    if (actual > length) { abortTransfer("Range response exceeds requested length"); return 0; }
    if (m_transferBuf.size() < AFM_CHUNK_SIZE) m_transferBuf.resize(AFM_CHUNK_SIZE);
    uint64_t received = 0;
    while (true) {
        if (!recvAll(&header, sizeof(header))) return 0;
        if (header.cmd == RSP_DONE) {
            if (header.length == 0 && received == actual) return received;
            abortTransfer("Incomplete range response"); return 0;
        }
        if (header.cmd != RSP_DATA || header.length == 0 || header.length > AFM_CHUNK_SIZE ||
            header.length > actual - received) { abortTransfer("Invalid range data length"); return 0; }
        if (!recvAll(m_transferBuf.data(), header.length)) return 0;
        if (!callback(m_transferBuf.data(), header.length, offset + received)) {
            abortTransfer("Cancelled"); return 0;
        }
        received += header.length;
    }
}

bool DeviceClient::resumePullFile(const std::string& remotePath, const std::string& localPath,
                                  uint64_t& outFileSize, ProgressCallback progress) {
    std::error_code error;
    uint64_t offset = std::filesystem::file_size(toFsPath(localPath), error);
    if (error || offset == 0) return pullFile(remotePath, localPath, outFileSize, progress);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    m_inlineCrcPath.clear();
    std::vector<char> request(8 + remotePath.size());
    memcpy(request.data(), &offset, 8);
    memcpy(request.data() + 8, remotePath.data(), remotePath.size());
    MsgHeader header{};
    std::vector<char> response;
    if (!sendMsg(CMD_RESUME_PULL, request.data(), (uint32_t)request.size()) || !recvMsg(header, response)) return false;
    if (header.cmd == RSP_ERROR) { m_lastError.assign(response.begin(), response.end()); return false; }
    if (header.cmd != RSP_OK || response.size() != sizeof(PullHeader)) {
        abortTransfer("Invalid resume response"); return false;
    }
    memcpy(&outFileSize, response.data(), 8);
    return receiveFileStream(localPath, outFileSize, offset, false, progress);
}

bool DeviceClient::resumePushFile(const std::string& localPath, const std::string& remotePath,
                                  uint64_t fileSize, ProgressCallback progress) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    auto file = openTransferSource(localPath, 0, fileSize, m_lastError, true);
    if (!file) return false;
    m_inlineCrcPath.clear();
    std::vector<char> request(8 + remotePath.size());
    memcpy(request.data(), &fileSize, 8);
    memcpy(request.data() + 8, remotePath.data(), remotePath.size());
    MsgHeader header{};
    std::vector<char> response;
    if (!sendMsg(CMD_RESUME_PUSH, request.data(), (uint32_t)request.size()) || !recvMsg(header, response)) return false;
    if (header.cmd == RSP_ERROR) { m_lastError.assign(response.begin(), response.end()); return false; }
    if (header.cmd != RSP_OK || response.size() != 8) { abortTransfer("Invalid resume response"); return false; }
    uint64_t offset = 0;
    memcpy(&offset, response.data(), 8);
    if (offset > fileSize) { abortTransfer("Invalid resume offset"); return false; }
    LARGE_INTEGER position{};
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN)) {
        abortTransfer("Failed to seek resume source"); return false;
    }
    return sendFileStream(file.get(), fileSize - offset, offset, false, false, progress);
}

// CRC32 — slicing-by-8 for ~4-8x speedup over byte-by-byte
static uint32_t g_crc32Table[8][256];
static std::once_flag g_crc32InitFlag;

static void initCrc32Table() {
    std::call_once(g_crc32InitFlag, []() {
        // Build base table (slice 0)
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
            g_crc32Table[0][i] = c;
        }
        // Build extended tables (slices 1-7)
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = g_crc32Table[0][i];
            for (int s = 1; s < 8; s++) {
                c = g_crc32Table[0][c & 0xFF] ^ (c >> 8);
                g_crc32Table[s][i] = c;
            }
        }
    });
}

// Slicing-by-8: processes 8 bytes per iteration
static void crc32UpdateSlice8(uint32_t& crc, const void* data, size_t len) {
    initCrc32Table();
    const uint8_t* p = (const uint8_t*)data;

    // Process 8 bytes at a time
    while (len >= 8) {
        uint32_t a = crc ^ *(const uint32_t*)p;
        uint32_t b = *(const uint32_t*)(p + 4);
        crc = g_crc32Table[7][(a      ) & 0xFF] ^
              g_crc32Table[6][(a >>  8) & 0xFF] ^
              g_crc32Table[5][(a >> 16) & 0xFF] ^
              g_crc32Table[4][(a >> 24)       ] ^
              g_crc32Table[3][(b      ) & 0xFF] ^
              g_crc32Table[2][(b >>  8) & 0xFF] ^
              g_crc32Table[1][(b >> 16) & 0xFF] ^
              g_crc32Table[0][(b >> 24)       ];
        p += 8;
        len -= 8;
    }
    // Remaining bytes
    while (len-- > 0)
        crc = g_crc32Table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
}

void DeviceClient::inlineCrcUpdate(const void* data, size_t len) {
    crc32UpdateSlice8(m_inlineCrc, data, len);
}

void crc32Update(uint32_t& crc, const void* data, size_t len) {
    crc32UpdateSlice8(crc, data, len);
}

// Combine two CRC32 values: crc1 covers [0, split), crc2 covers [split, split+len2)
// Uses GF(2) matrix exponentiation — O(log n), instant even for 100 GB
static uint32_t gf2MatrixTimes(const uint32_t* mat, uint32_t vec) {
    uint32_t sum = 0;
    for (int i = 0; vec; i++, vec >>= 1)
        if (vec & 1) sum ^= mat[i];
    return sum;
}
static void gf2MatrixSquare(uint32_t* sq, const uint32_t* mat) {
    for (int n = 0; n < 32; n++) sq[n] = gf2MatrixTimes(mat, mat[n]);
}

uint32_t crc32Combine(uint32_t crc1, uint32_t crc2, uint64_t len2) {
    if (len2 == 0) return crc1;
    uint32_t even[32], odd[32];
    odd[0] = 0xEDB88320; // polynomial
    uint32_t row = 1;
    for (int n = 1; n < 32; n++) { odd[n] = row; row <<= 1; }
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

static bool computeLocalCrc32(const std::string& path, uint32_t& result, std::atomic<float>* progress = nullptr) {
    TransferFileHandle file(CreateFileW(toWide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0) return false;
    std::vector<char> buffer(AFM_CHUNK_SIZE);
    uint64_t processed = 0;
    uint32_t crc = ~(uint32_t)0;
    while (true) {
        DWORD count = 0;
        if (!ReadFile(file.get(), buffer.data(), (DWORD)buffer.size(), &count, nullptr)) return false;
        if (!count) break;
        crc32UpdateSlice8(crc, buffer.data(), count);
        processed += count;
        if (progress && size.QuadPart > 0) progress->store((float)((double)processed / size.QuadPart));
    }
    if (processed != (uint64_t)size.QuadPart) return false;
    if (progress) progress->store(1.0f);
    result = ~crc;
    return true;
}

bool DeviceClient::verifyFileCrc(const std::string& remotePath, const std::string& localPath, std::string& detail,
                                 std::atomic<float>* crcProgress, std::atomic<int>* crcPhase,
                                 double* remoteMs, double* localMs) {
    if (!m_connected) { detail = "Not connected"; return false; }

    // Get local file size to estimate CRC computation time
    std::error_code sizeError;
    uint64_t localFileSize = std::filesystem::file_size(toFsPath(localPath), sizeError);
    if (sizeError) { detail = "Cannot read local file for verification: " + sizeError.message(); return false; }

    // Scale recv timeout: ~200MB/s read speed on device, plus margin
    // Minimum 30s, add 1s per 100MB
    DWORD crcTimeout = 30000 + (DWORD)(localFileSize / (100ULL * 1024 * 1024)) * 1000;
    if (crcTimeout > 300000) crcTimeout = 300000; // cap at 5 minutes
    LOG_INFO("CRC", "verifyFileCrc: " + remotePath + " (size=" + std::to_string(localFileSize) + " timeout=" + std::to_string(crcTimeout) + "ms)");

    if (crcPhase) crcPhase->store(1); // phase 1: server computing
    if (crcProgress) crcProgress->store(0.0f);

    auto remoteStart = std::chrono::steady_clock::now();
    uint32_t remoteCrc = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);

        // Set long timeout for CRC computation on large files
        SOCKET sock = (SOCKET)m_socket;
        DWORD origTimeout = 10000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&crcTimeout, sizeof(crcTimeout));

        if (!sendMsg(CMD_CRC32, remotePath.data(), (uint32_t)remotePath.size())) {
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
            detail = "Failed to request remote CRC";
            LOG_ERROR("CRC", detail);
            return false;
        }
        MsgHeader hdr; std::vector<char> payload;
        if (!recvMsg(hdr, payload)) {
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
            detail = "Failed to receive remote CRC";
            LOG_ERROR("CRC", detail + " (timeout was " + std::to_string(crcTimeout) + "ms)");
            return false;
        }
        // Restore normal timeout
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));

        if (hdr.cmd == RSP_ERROR) {
            detail = "Server error: " + std::string(payload.data(), payload.size());
            LOG_ERROR("CRC", detail);
            return false;
        }
        if (hdr.cmd != RSP_OK || payload.size() < 4) { detail = "Invalid CRC response"; return false; }
        memcpy(&remoteCrc, payload.data(), 4);
    }

    auto remoteEnd = std::chrono::steady_clock::now();
    double remoteDur = std::chrono::duration<double, std::milli>(remoteEnd - remoteStart).count();
    if (remoteMs) *remoteMs = remoteDur;
    LOG_INFO("CRC", "Remote CRC: " + std::to_string(remoteCrc) + " (" + std::to_string((int)remoteDur) + "ms) - checking local CRC...");

    // Phase 2: local CRC — use inline CRC if available (computed during transfer)
    if (crcPhase) crcPhase->store(2);
    if (crcProgress) crcProgress->store(0.0f);
    auto localStart = std::chrono::steady_clock::now();
    uint32_t localCrc;
    if (m_inlineCrcPath == localPath) {
        // Use CRC computed during transfer — zero extra disk I/O
        localCrc = m_inlineCrc;
        if (crcProgress) crcProgress->store(1.0f);
        LOG_INFO("CRC", "Using inline CRC (no re-read needed)");
    } else {
        // Fallback: compute from disk (resume transfers, etc.)
        LOG_INFO("CRC", "No inline CRC — reading file from disk");
        if (!computeLocalCrc32(localPath, localCrc, crcProgress)) {
            detail = "Failed to read the complete local file for verification";
            return false;
        }
    }

    auto localEnd = std::chrono::steady_clock::now();
    double localDur = std::chrono::duration<double, std::milli>(localEnd - localStart).count();
    if (localMs) *localMs = localDur;

    char buf[128];
    snprintf(buf, sizeof(buf), "Local: %08X  Remote: %08X", localCrc, remoteCrc);
    detail = buf;
    LOG_INFO("CRC", std::string(buf) + (localCrc == remoteCrc ? " MATCH" : " MISMATCH") +
             " (remote=" + std::to_string((int)remoteDur) + "ms local=" + std::to_string((int)localDur) + "ms)");

    return localCrc == remoteCrc;
}

bool DeviceClient::getRemoteCrc32(const std::string& remotePath, uint32_t& outCrc, std::string& detail,
                                  uint64_t fileSize, double* remoteMs) {
    if (!m_connected) { detail = "Not connected"; return false; }

    DWORD crcTimeout = 30000 + (DWORD)(fileSize / (100ULL * 1024 * 1024)) * 1000;
    if (crcTimeout > 300000) crcTimeout = 300000;
    LOG_INFO("CRC", "getRemoteCrc32: " + remotePath + " (size=" + std::to_string(fileSize) +
             " timeout=" + std::to_string(crcTimeout) + "ms)");

    auto remoteStart = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        SOCKET sock = (SOCKET)m_socket;
        DWORD origTimeout = 10000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&crcTimeout, sizeof(crcTimeout));

        if (!sendMsg(CMD_CRC32, remotePath.data(), (uint32_t)remotePath.size())) {
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
            detail = "Failed to request remote CRC";
            LOG_ERROR("CRC", detail);
            return false;
        }

        MsgHeader hdr; std::vector<char> payload;
        if (!recvMsg(hdr, payload)) {
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
            detail = "Failed to receive remote CRC";
            LOG_ERROR("CRC", detail + " (timeout was " + std::to_string(crcTimeout) + "ms)");
            return false;
        }
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));

        if (hdr.cmd == RSP_ERROR) {
            detail = "Server error: " + std::string(payload.data(), payload.size());
            LOG_ERROR("CRC", detail);
            return false;
        }
        if (hdr.cmd != RSP_OK || payload.size() < 4) {
            detail = "Invalid CRC response";
            return false;
        }
        memcpy(&outCrc, payload.data(), 4);
    }

    auto remoteEnd = std::chrono::steady_clock::now();
    double remoteDur = std::chrono::duration<double, std::milli>(remoteEnd - remoteStart).count();
    if (remoteMs) *remoteMs = remoteDur;
    detail = "Remote: " + std::to_string(outCrc);
    LOG_INFO("CRC", "Remote CRC: " + std::to_string(outCrc) + " (" + std::to_string((int)remoteDur) + "ms)");
    return true;
}

bool DeviceClient::getRemoteSha256(const std::string& remotePath, std::string& outSha256, std::string& detail,
                                   uint64_t fileSize, double* remoteMs) {
    if (!m_connected) { detail = "Not connected"; return false; }

    DWORD hashTimeout = 30000 + (DWORD)(fileSize / (50ULL * 1024 * 1024)) * 1000;
    if (hashTimeout > 600000) hashTimeout = 600000;
    LOG_INFO("SHA256", "getRemoteSha256: " + remotePath + " (size=" + std::to_string(fileSize) +
             " timeout=" + std::to_string(hashTimeout) + "ms)");

    auto remoteStart = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        SOCKET sock = (SOCKET)m_socket;
        DWORD origTimeout = 10000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&hashTimeout, sizeof(hashTimeout));

        if (!sendMsg(CMD_SHA256, remotePath.data(), (uint32_t)remotePath.size())) {
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
            detail = "Failed to request remote SHA-256";
            LOG_ERROR("SHA256", detail);
            return false;
        }

        MsgHeader hdr; std::vector<char> payload;
        if (!recvMsg(hdr, payload)) {
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));
            detail = "Failed to receive remote SHA-256";
            LOG_ERROR("SHA256", detail + " (timeout was " + std::to_string(hashTimeout) + "ms)");
            return false;
        }
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&origTimeout, sizeof(origTimeout));

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
        for (int i = 0; i < 32; i++) {
            unsigned char b = (unsigned char)payload[i];
            outSha256.push_back(hex[b >> 4]);
            outSha256.push_back(hex[b & 0x0f]);
        }
    }

    auto remoteEnd = std::chrono::steady_clock::now();
    double remoteDur = std::chrono::duration<double, std::milli>(remoteEnd - remoteStart).count();
    if (remoteMs) *remoteMs = remoteDur;
    detail = "Remote SHA-256: " + outSha256;
    LOG_INFO("SHA256", "Remote SHA-256: " + outSha256 + " (" + std::to_string((int)remoteDur) + "ms)");
    return true;
}

bool DeviceClient::deleteFile(const std::string& path) {
    auto perform=[&]() -> bool {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    MsgHeader hdr; std::vector<char> payload;
    if (!runSmallOp(CMD_DELETE, path.data(), (uint32_t)path.size(), hdr, payload)) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    return hdr.cmd == RSP_OK;
    };
    bool ok=perform();
    DebugLog::instance().log(ok?LogLevel::Info:LogLevel::Error,"Files",std::string("Delete ")+(ok?"completed: ":"failed: ")+path+(ok?"":": "+m_lastError));
    return ok;
}

bool DeviceClient::createDirectory(const std::string& path) {
    auto perform=[&]() -> bool {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    MsgHeader hdr; std::vector<char> payload;
    if (!runSmallOp(CMD_MKDIR, path.data(), (uint32_t)path.size(), hdr, payload)) return false;
    if (hdr.cmd == RSP_ERROR) { m_lastError = std::string(payload.data(), payload.size()); return false; }
    return hdr.cmd == RSP_OK;
    };
    bool ok=perform();
    DebugLog::instance().log(ok?LogLevel::Info:LogLevel::Error,"Files",std::string("Create folder ")+(ok?"completed: ":"failed: ")+path+(ok?"":": "+m_lastError));
    return ok;
}

bool DeviceClient::renameFile(const std::string& oldPath, const std::string& newPath) {
    auto perform=[&]() -> bool {
    if (!m_connected) { m_lastError = "Not connected"; return false; }
    uint32_t oldLen = (uint32_t)oldPath.size();
    std::vector<char> payload(4 + oldPath.size() + newPath.size());
    memcpy(payload.data(), &oldLen, 4);
    memcpy(payload.data() + 4, oldPath.data(), oldPath.size());
    memcpy(payload.data() + 4 + oldPath.size(), newPath.data(), newPath.size());
    MsgHeader hdr; std::vector<char> resp;
    if (!runSmallOp(CMD_RENAME, payload.data(), (uint32_t)payload.size(), hdr, resp)) return false;
    if (hdr.cmd == RSP_ERROR) { m_lastError = std::string(resp.data(), resp.size()); return false; }
    return hdr.cmd == RSP_OK;
    };
    bool ok=perform();
    DebugLog::instance().log(ok?LogLevel::Info:LogLevel::Error,"Files",std::string("Rename ")+(ok?"completed: ":"failed: ")+oldPath+" -> "+newPath+(ok?"":": "+m_lastError));
    return ok;
}

uint64_t DeviceClient::getFileSize(const std::string& path) {
    if (!m_connected) return 0;
    MsgHeader hdr; std::vector<char> payload;
    if (!runSmallOp(CMD_STAT, path.data(), (uint32_t)path.size(), hdr, payload)) return 0;
    if (hdr.cmd == RSP_OK && payload.size() >= sizeof(StatResponse)) {
        StatResponse sr;
        memcpy(&sr, payload.data(), sizeof(sr));
        return sr.size;
    }
    return 0;
}
