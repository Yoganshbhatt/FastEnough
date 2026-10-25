// Linux implementation of the DeviceClient transfer core (Stage A).
//
// Same class as src/device_client.h, POSIX body: BSD sockets via net_posix.h
// (poll deadlines instead of SO_RCVTIMEO juggling), adb via subprocess.h,
// XDG paths, no wide-char round-trips. Windows behavior is preserved
// verbatim: identical command strings, retry counts, delays, and protocol
// framing — only the OS primitives differ.
//
// Stage A covers: adb discovery/spawn, server lifecycle, TCP connect/verify,
// control channel, browsing ops (storage/list/diskspace/stat), mkdir.
// Stage B adds bulk transfer (pull/push/resume/ranges/CRC); later stages add
// backup/install/archive. Unimplemented methods are simply absent from this
// TU — the linker only resolves what afm-cli references.
#include "device_client.h"
#include "net_posix.h"

#include <limits.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "log_facade.h"
#include "net_posix.h"
#include "subprocess.h"

namespace {
constexpr uintptr_t kInvalid = ~(uintptr_t)0;
constexpr uint32_t kDefaultSendMs = 30000;
constexpr uint32_t kDefaultRecvMs = 10000;

ProcessResult toProcessResult(SubprocessResult sub) {
    ProcessResult out;
    out.exitCode = sub.exitCode < 0 ? 1 : static_cast<uint32_t>(sub.exitCode);
    out.systemError = sub.systemError < 0 ? 1 : static_cast<uint32_t>(sub.systemError);
    out.timedOut = sub.timedOut;
    out.cancelled = sub.cancelled;
    out.outputTruncated = sub.outputTruncated;
    out.standardOutput = std::move(sub.standardOutput);
    out.standardError = std::move(sub.standardError);
    out.errorMessage = std::move(sub.errorMessage);
    out.bytesWritten = sub.bytesWritten;
    return out;
}

// Split an adb argument string ("-s SER shell \"cmd\"") into argv entries.
// Handles double-quote grouping exactly as the call sites construct it
// (quoteCmdArg emits double quotes); single quotes pass through for the
// device shell (su -c '...'). No shell is ever involved locally.
std::vector<std::string> splitAdbArgs(const std::string& args) {
    std::vector<std::string> argv;
    std::string current;
    bool inQuotes = false;
    bool hasToken = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const char c = args[i];
        if (c == '"') {
            inQuotes = !inQuotes;
            hasToken = true;
            continue;
        }
        if (!inQuotes && (c == ' ' || c == '\t')) {
            if (hasToken) {
                argv.push_back(current);
                current.clear();
                hasToken = false;
            }
            continue;
        }
        current.push_back(c);
        hasToken = true;
    }
    if (hasToken) argv.push_back(current);
    return argv;
}

std::string sanitizeAdbOutput(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
    return text;
}

std::string wrapSu(const std::string& innerCmd, bool useRoot) {
    if (!useRoot) return innerCmd;
    return "su -c '" + innerCmd + "'";
}

std::string serverLaunchCommand(int port, bool useRoot) {
    return wrapSu("nohup /data/local/tmp/afm-server " + std::to_string(port) +
                     " > /data/local/tmp/afm-server.log 2>&1 &",
                 useRoot);
}

std::string quoteCmdArg(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}

std::vector<std::string> splitLines(std::string text) {
    std::vector<std::string> lines;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::string exeDir() {
    char buf[PATH_MAX];
    const ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return {};
    buf[len] = '\0';
    return std::filesystem::path(buf).parent_path().string();
}

bool sendFrame(int fd, uint32_t cmd, const void* payload, uint32_t len,
               uint32_t sendMs, uint32_t recvMsUnused, std::string& error) {
    (void)recvMsUnused;
    MsgHeader hdr{cmd, len};
    if (!net::sendAll(fd, &hdr, sizeof(hdr), sendMs, error)) return false;
    if (len > 0 && !net::sendAll(fd, payload, len, sendMs, error)) return false;
    return true;
}

bool recvFrame(int fd, MsgHeader& hdr, std::vector<char>& payload, uint32_t recvMs,
               std::string& error) {
    if (!net::recvAll(fd, &hdr, sizeof(hdr), recvMs, error)) return false;
    const uint32_t maxPayload = AFM_CHUNK_SIZE + 65536;  // same cap as Windows
    if (hdr.length > maxPayload) {
        error = "Protocol error: payload too large (" + std::to_string(hdr.length) + " bytes)";
        return false;
    }
    payload.resize(hdr.length);
    if (hdr.length > 0 && !net::recvAll(fd, payload.data(), hdr.length, recvMs, error))
        return false;
    return true;
}

}  // namespace

// Linux counterpart of process_runner.cpp's definition. Same formatting rules,
// so error strings shown by the shared app.cpp are identical on both platforms.
std::string ProcessResult::diagnostics() const {
    std::string message = errorMessage;
    if (!standardError.empty()) {
        if (!message.empty()) message += '\n';
        message += standardError;
    } else if (!succeeded() && !standardOutput.empty()) {
        if (!message.empty()) message += '\n';
        message += standardOutput;
    }
    if (message.empty() && exitCode != 0)
        message = "Command failed with exit code " + std::to_string(exitCode) + ".";
    return message;
}

DeviceClient::DeviceClient() {
    // findAdb() spawns a process; resolved lazily by the caller like Windows.
}

DeviceClient::~DeviceClient() {
    if (m_ownsServer) stopServer();
    else disconnectTcp();
}

bool DeviceClient::findAdb() {
    auto tryPath = [&](const std::filesystem::path& p) -> bool {
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) {
            m_adbPath = p.string();
            return true;
        }
        return false;
    };
    if (!m_adbPath.empty() && tryPath(m_adbPath)) return true;
    m_adbPath.clear();

    // Bundled ADB next to the binary (release layout).
    const std::string dir = exeDir();
    if (!dir.empty()) {
        const std::filesystem::path exePath(dir);
        if (tryPath(exePath / "platform-tools" / "adb")) return true;
        if (tryPath(exePath / "adb")) return true;
    }
    // SDK locations.
    if (const char* home = ::getenv("HOME")) {
        const std::filesystem::path sdk(home);
        if (tryPath(sdk / "Android" / "Sdk" / "platform-tools" / "adb")) return true;
    }
    if (const char* ah = ::getenv("ANDROID_HOME"))
        if (tryPath(std::filesystem::path(ah) / "platform-tools" / "adb")) return true;
    if (const char* sr = ::getenv("ANDROID_SDK_ROOT"))
        if (tryPath(std::filesystem::path(sr) / "platform-tools" / "adb")) return true;
    // PATH lookup (execvp-equivalent): bare "adb" resolves if executable.
    SubprocessOptions probe;
    probe.executable = "adb";
    probe.arguments = {"version"};
    probe.timeoutMs = 5000;
    if (runSubprocess(probe).succeeded()) {
        m_adbPath = "adb";
        return true;
    }
    return false;
}

std::string DeviceClient::runProcess(const std::string& command) {
    SubprocessOptions options;
    options.executable = m_adbPath.empty() ? "adb" : m_adbPath;
    options.shouldCancel = [this]() { return m_cancelCommands.load(); };
    // runProcess callers pass pre-split argv on Linux (see runAdbCommand);
    // a bare command string runs through PATH lookup of its first token.
    const std::vector<std::string> argv = splitAdbArgs(command);
    if (!argv.empty()) {
        options.executable = argv[0];
        options.arguments.assign(argv.begin() + 1, argv.end());
    }
    const SubprocessResult result = runSubprocess(options);
    if (result.exitCode != 0 || result.systemError != 0 || result.timedOut ||
        result.cancelled || !result.errorMessage.empty()) {
        ProcessResult pr = toProcessResult(result);
        return "error: Command failed. " + pr.diagnostics();
    }
    return result.standardOutput + result.standardError;
}

bool DeviceClient::runProcessToFile(const std::string& command, const std::string& outputPath,
                                    std::string& output, uint32_t timeoutMs,
                                    ProgressCallback progress, uint64_t totalBytes) {
    SubprocessOptions options;
    const std::vector<std::string> argv = splitAdbArgs(command);
    if (argv.empty()) {
        output = "error: empty command.";
        return false;
    }
    options.executable = argv[0];
    options.arguments.assign(argv.begin() + 1, argv.end());
    options.outputPath = outputPath;
    options.timeoutMs = timeoutMs ? timeoutMs : 30 * 60 * 1000;
    options.shouldCancel = [this]() { return m_cancelCommands.load(); };
    if (progress)
        options.progress = [progress, totalBytes](uint64_t bytes) {
            return progress(bytes, totalBytes);
        };
    const ProcessResult result = toProcessResult(runSubprocess(options));
    output = result.diagnostics();
    if (!result.succeeded()) LOG_ERROR("Process", "Streaming command failed: " + output);
    return result.succeeded();
}

bool DeviceClient::runProcessFromFile(const std::string& command, const std::string& inputPath,
                                      std::string& output, uint32_t timeoutMs) {
    SubprocessOptions options;
    const std::vector<std::string> argv = splitAdbArgs(command);
    if (argv.empty()) {
        output = "error: empty command.";
        return false;
    }
    options.executable = argv[0];
    options.arguments.assign(argv.begin() + 1, argv.end());
    options.inputPath = inputPath;
    options.timeoutMs = timeoutMs ? timeoutMs : 30 * 60 * 1000;
    options.shouldCancel = [this]() { return m_cancelCommands.load(); };
    const ProcessResult result = toProcessResult(runSubprocess(options));
    output = result.succeeded() ? result.standardOutput + result.standardError
                                : result.diagnostics();
    return result.succeeded();
}

ProcessResult DeviceClient::runAdbCommandResult(const std::string& args, uint32_t timeoutMs,
                                               std::function<bool()> shouldCancel) {
    const bool pairing = args.starts_with("pair ");
    LOG_DEBUG("ADB", "$ adb " + (pairing ? std::string("pair [redacted]") : args));
    SubprocessOptions options;
    options.executable = m_adbPath.empty() ? "adb" : m_adbPath;
    options.arguments = splitAdbArgs(args);
    options.timeoutMs = timeoutMs;
    options.shouldCancel = [this, shouldCancel]() {
        return m_cancelCommands.load() || (shouldCancel && shouldCancel());
    };
    ProcessResult result = toProcessResult(runSubprocess(options));
    std::string logged = result.succeeded() ? result.standardOutput + result.standardError
                                            : result.diagnostics();
    while (!logged.empty() && (logged.back() == '\r' || logged.back() == '\n'))
        logged.pop_back();
    if (logged.size() > 200) logged = logged.substr(0, 200) + "...";
    if (!logged.empty() && !pairing) LOG_DEBUG("ADB", "  -> " + logged);
    return result;
}

std::string DeviceClient::runAdbCommand(const std::string& args, uint32_t timeoutMs) {
    const ProcessResult result = runAdbCommandResult(args, timeoutMs);
    if (!result.succeeded()) return "error: Command failed. " + result.diagnostics();
    if (result.outputTruncated) return "error: Command output exceeded the capture limit.";
    return result.standardOutput + result.standardError;
}

std::vector<DeviceInfo> DeviceClient::getDevices() {
    if (m_adbPath.empty() && !findAdb()) return {};
    return parseAdbDevices(runAdbCommand("devices -l"));
}

namespace {
// ADB host protocol (host:track-devices-l): 4-hex-digit length + payload,
// "OKAY"/"FAIL" status words. Same wire format as the Windows client.
bool adbProtoSend(int fd, const std::string& cmd) {
    char lenBuf[5];
    ::snprintf(lenBuf, sizeof(lenBuf), "%04x", static_cast<unsigned>(cmd.size()));
    const std::string msg = std::string(lenBuf) + cmd;
    std::string error;
    return net::sendAll(fd, msg.data(), msg.size(), 5000, error);
}

bool adbProtoRecvStatus(int fd) {
    char status[4];
    std::string error;
    if (!net::recvAll(fd, status, sizeof(status), 5000, error)) return false;
    return std::memcmp(status, "OKAY", 4) == 0;
}

std::string adbProtoRecvPayload(int fd) {
    char lenBuf[5] = {};
    std::string error;
    if (!net::recvAll(fd, lenBuf, 4, 0, error)) return {};
    unsigned len = 0;
    const auto [end, ec] = std::from_chars(lenBuf, lenBuf + 4, len, 16);
    if (ec != std::errc{} || end != lenBuf + 4) return {};
    if (len == 0) return " ";  // empty list marker, same as Windows
    std::string payload(len, '\0');
    if (!net::recvAll(fd, payload.data(), len, 0, error)) return {};
    return payload;
}
}  // namespace

uintptr_t DeviceClient::openTrackDevices() {
    std::string error;
    const int fd = net::tcpConnect("127.0.0.1", 5037, 3000, "", error);
    if (fd < 0) {
        LOG_WARN("ADB", "track-devices: cannot connect to ADB server: " + error);
        return kInvalid;
    }
    if (!adbProtoSend(fd, "host:track-devices-l") || !adbProtoRecvStatus(fd)) {
        LOG_WARN("ADB", "track-devices: server rejected");
        ::close(fd);
        return kInvalid;
    }
    LOG_INFO("ADB", "track-devices: connected — waiting for device events");
    return static_cast<uintptr_t>(fd);
}

std::vector<DeviceInfo> DeviceClient::readTrackDevicesUpdate(uintptr_t sockHandle,
                                                             bool* receivedUpdate) {
    std::vector<DeviceInfo> devices;
    if (receivedUpdate) *receivedUpdate = false;
    if (sockHandle == kInvalid) return devices;
    const std::string payload = adbProtoRecvPayload(static_cast<int>(sockHandle));
    if (payload.empty()) return devices;
    if (receivedUpdate) *receivedUpdate = true;
    if (payload == " ") {
        LOG_INFO("ADB", "track-devices: all devices disconnected");
        return devices;
    }
    return parseAdbDevices(payload);
}

void DeviceClient::closeTrackDevices(uintptr_t sock) {
    if (sock != kInvalid) ::close(static_cast<int>(sock));
}

bool DeviceClient::lastTrackWaitTimedOut() {
    return net::lastWaitTimedOut();
}

bool DeviceClient::connectLocalPort(int port) {
    disconnectTcp();
    if (port <= 0) return false;
    if (connectTcp("127.0.0.1", port) && verifyConnection()) {
        m_connected = true;
        m_directConnection = false;
        return true;
    }
    disconnectTcp();
    return false;
}

std::string DeviceClient::getServerBinaryPath() {
    // Beside the executable (dev tree: staged by CMake; release: installed).
    const std::string dir = exeDir();
    if (!dir.empty()) {
        const std::string beside = (std::filesystem::path(dir) / "afm-server").string();
        std::error_code ec;
        if (std::filesystem::exists(beside, ec) && !ec) return beside;
        return beside;  // caller reports "not found", same contract as Windows
    }
    return "afm-server";
}

std::string DeviceClient::detectDeviceIp(const std::string& serial) {
    // USB tethering interfaces only — wlan0 is not a direct USB connection.
    const char* interfaces[] = {"rndis0", "usb0", "ncm0", nullptr};
    for (int i = 0; interfaces[i]; ++i) {
        const std::string out = runAdbCommand("-s " + serial + " shell \"ip -4 addr show " +
                                              interfaces[i] + " 2>/dev/null\"");
        auto inetPos = out.find("inet ");
        if (inetPos == std::string::npos) continue;
        inetPos += 5;
        const auto slashPos = out.find('/', inetPos);
        const auto spacePos = out.find(' ', inetPos);
        const auto endPos = std::min(slashPos, spacePos);
        if (endPos != std::string::npos && endPos > inetPos) {
            const std::string ip = out.substr(inetPos, endPos - inetPos);
            if (ip.find('.') != std::string::npos && ip != "127.0.0.1") return ip;
        }
    }
    return {};
}

bool DeviceClient::tryDirectConnect(const std::string& ip, int port) {
    if (ip.empty()) return false;
    disconnectTcp();
    if (connectTcp(ip, port) && verifyConnection()) {
        m_directConnection = true;
        m_serverPort = port;
        m_deviceIp = ip;
        return true;
    }
    disconnectTcp();
    return false;
}

bool DeviceClient::verifyConnection() {
    const std::lock_guard<std::mutex> lk(m_mutex);
    if (m_socket == kInvalid) {
        m_lastError = "Not connected";
        return false;
    }
    const int fd = static_cast<int>(m_socket);
    LOG_DEBUG("TCP", "Sending PING to verify connection...");
    std::string error;
    // 3s PING deadline so a dead peer never pins m_mutex for 10s.
    if (!sendFrame(fd, CMD_PING, nullptr, 0, 3000, 3000, error)) {
        m_lastError = "PING send failed: " + error;
        LOG_ERROR("TCP", m_lastError);
        return false;
    }
    MsgHeader hdr{};
    std::vector<char> payload;
    if (!recvFrame(fd, hdr, payload, 3000, error)) {
        m_lastError = "PING recv failed: " + error;
        LOG_ERROR("TCP", m_lastError);
        return false;
    }
    if (hdr.cmd != RSP_OK || payload.size() < 4) {
        LOG_ERROR("TCP", "PING bad response: cmd=" + std::to_string(hdr.cmd) +
                              " len=" + std::to_string(payload.size()));
        return false;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, payload.data(), 4);
    if (magic != AFM_MAGIC) {
        LOG_ERROR("TCP", "PING wrong magic: 0x" + std::to_string(magic));
        return false;
    }
    if (payload.size() >= 8) {
        uint32_t serverVersion = 0;
        std::memcpy(&serverVersion, payload.data() + 4, 4);
        if (serverVersion != AFM_VERSION) {
            m_lastError = "Protocol version mismatch: client=" + std::to_string(AFM_VERSION) +
                " server=" + std::to_string(serverVersion);
            LOG_ERROR("TCP", m_lastError);
            return false;
        }
    }
    m_serverIsRoot = false;
    if (payload.size() >= 12) {
        uint32_t effectiveUid = 0;
        std::memcpy(&effectiveUid, payload.data() + 8, 4);
        m_serverIsRoot = effectiveUid == 0;
    }
    LOG_INFO("TCP", "PING OK (payload " + std::to_string(payload.size()) + " bytes, root=" +
                        std::string(m_serverIsRoot ? "Y" : "N") + ")");
    return true;
}

bool DeviceClient::isRootAvailable(const std::string& serial) {
    return runAdbCommand("-s " + serial + " shell \"su -c id 2>/dev/null\"").find("uid=0") !=
        std::string::npos;
}

void DeviceClient::stopDeviceServerProcesses(const std::string& serial) {
    runAdbCommand("-s " + serial +
                  " shell \"killall afm-server 2>/dev/null; su -c 'killall afm-server' 2>/dev/null\"");
    const std::string stopPortOwners =
        "for pid in $(ss -ltnp 2>/dev/null | grep :" + std::to_string(AFM_PORT) +
        " | grep -o pid=[0-9]* | cut -d= -f2); do kill -9 $pid; done";
    runAdbCommand("-s " + serial + " shell \"" + stopPortOwners + " 2>/dev/null\"");
    runAdbCommand("-s " + serial + " shell \"su -c '" + stopPortOwners + "' 2>/dev/null\"");
}

std::vector<InstalledAppEntry> DeviceClient::listInstalledApps(const std::string& serial,
                                                               bool includeSystem) {
    std::vector<InstalledAppEntry> apps;
    if (serial.empty()) return apps;

    auto parseList = [&](const std::string& out, bool isSystem) {
        std::istringstream ss(out);
        std::string line;
        while (std::getline(ss, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            const std::string prefix = "package:";
            if (line.rfind(prefix, 0) != 0) continue;
            const std::string val = line.substr(prefix.size());
            const auto eq = val.rfind('=');
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
    const std::lock_guard<std::mutex> labelLock(labelQueryMutex);
    const std::string dir = exeDir();
    std::string labelsDex;
    if (!dir.empty()) {
        const std::string beside = (std::filesystem::path(dir) / "afm-labels.dex").string();
        std::error_code ec;
        if (std::filesystem::exists(beside, ec) && !ec) labelsDex = beside;
    }
    if (!labelsDex.empty()) {
        const std::string remoteDex = "/data/local/tmp/afm-labels.dex";
        if (runAdbCommand("-s " + serial + " push " + quoteCmdArg(labelsDex) + " " +
                          remoteDex)
                .find("error") == std::string::npos) {
            const std::string labels =
                runAdbCommand("-s " + serial + " shell \"CLASSPATH=" + remoteDex +
                              " app_process /system/bin com.fastenough.tools.AppLabels "
                              "2>/dev/null; rm -f " +
                              remoteDex + "\"");
            std::unordered_map<std::string, std::string> names;
            for (const auto& line : splitLines(labels)) {
                const size_t sep = line.find('\t');
                if (sep == std::string::npos) continue;
                names[line.substr(0, sep)] = line.substr(sep + 1);
            }
            for (auto& app : apps) {
                const auto found = names.find(app.packageName);
                if (found != names.end()) app.appName = found->second;
            }
        }
    }
    std::sort(apps.begin(), apps.end(), [](const InstalledAppEntry& a, const InstalledAppEntry& b) {
        const std::string& aName = a.appName.empty() ? a.packageName : a.appName;
        const std::string& bName = b.appName.empty() ? b.packageName : b.appName;
        const int nameOrder = ::strcasecmp(aName.c_str(), bName.c_str());
        return nameOrder != 0 ? nameOrder < 0
                              : ::strcasecmp(a.packageName.c_str(), b.packageName.c_str()) < 0;
    });
    return apps;
}

bool DeviceClient::startServer(const std::string& serial, bool preferAdbForward, bool useRoot) {
    const std::lock_guard<std::mutex> serverLk(m_serverMutex);
    m_startServerActive = true;
    struct ActivityReset {
        std::atomic<bool>& active;
        ~ActivityReset() { active = false; }
    } reset{m_startServerActive};

    const bool canReuseConnection =
        m_connected && m_serial == serial && (!useRoot || (m_useRoot && m_serverIsRoot));
    m_useRoot = useRoot;
    LOG_INFO("Server", std::string("startServer(serial=") + serial +
                           " prefer=" + (preferAdbForward ? "ADBForward" : "DirectTCP") +
                           " root=" + (useRoot ? "Y" : "N") + ") begin");
    if (canReuseConnection) {
        LOG_INFO("Server", "Already connected to " + serial + " with the requested privilege mode");
        return true;
    }
    if (m_connected && m_serial == serial && useRoot)
        LOG_INFO("Server", "Existing helper is not verified as root, replacing it");
    if (m_useRoot && !isRootAvailable(serial)) {
        m_lastError =
            "Root access is not available or was denied. Check that this device is rooted and grant root access to ADB in Magisk or KernelSU.";
        LOG_ERROR("Server", m_lastError);
        m_statusText = "ERROR: " + m_lastError;
        return false;
    }

    const bool wasConnectedToSameDevice = (!m_connected && m_serial == serial && !m_serial.empty());
    disconnectTcp();
    m_connected = false;
    m_directConnection = false;
    const std::string savedIp = m_deviceIp;
    LOG_DEBUG("Server", "savedIp from tethering: '" + savedIp + "'");
    m_deviceIp.clear();
    m_serial = serial;
    const int launchPort = m_localPort == AFM_PORT ? AFM_PORT : m_localPort;
    m_serverPort = AFM_PORT;

    if (m_useRoot) {
        LOG_INFO("Server", "Root mode requested - restarting helper as root");
        stopDeviceServerProcesses(serial);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (wasConnectedToSameDevice) {
        LOG_INFO("Server", "Reconnecting to same device - killing old server to clear dead sockets");
        stopDeviceServerProcesses(serial);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const std::string serverBin = getServerBinaryPath();
        std::error_code ec;
        if (std::filesystem::exists(serverBin, ec) && !ec) {
            runAdbCommand("-s " + serial + " push \"" + serverBin + "\" /data/local/tmp/afm-server");
            runAdbCommand("-s " + serial + " shell chmod 755 /data/local/tmp/afm-server");
            runAdbCommand("-s " + serial + " shell rm -f /data/local/tmp/afm-server.log");
            const auto launch = runAdbCommandResult(
                "-s " + serial + " shell \"" + serverLaunchCommand(launchPort, m_useRoot) + "\"",
                5000);
            if (launch.cancelled) return false;
            if (!launch.succeeded())
                LOG_WARN("Server", "Helper launch: " + launch.diagnostics());
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            LOG_INFO("Server", "Fresh server started after reconnect");
        }
    }

    // 1. Try an already-running server first (root mode skips: it must restart).
    m_statusText = "Checking for existing server...";
    std::string deviceIp = detectDeviceIp(serial);
    LOG_INFO("Server", "detectDeviceIp -> '" + deviceIp + "'");
    if (deviceIp.empty() && !savedIp.empty()) {
        deviceIp = savedIp;
        LOG_INFO("Server", "Using savedIp: " + savedIp);
    }
    if (m_useRoot) {
        LOG_INFO("Server", "Skipping existing-server probe (root restart required)");
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

    if (!m_useRoot) {
        LOG_INFO("Server", "Step 1b: Trying existing server via ADB Forward");
        m_statusText = "Trying ADB Forward...";
        // Probe the slot's launch port first, then the default port: a server
        // started by another slot/CLI run listens on its own launch port, and
        // blindly forwarding to AFM_PORT misses it (causing a needless push).
        const int candidates[2] = {launchPort, AFM_PORT};
        for (int ci = 0; ci < 2; ++ci) {
            if (ci == 1 && candidates[ci] == candidates[0]) break;
            runAdbCommand("-s " + serial + " forward --remove tcp:" + std::to_string(m_localPort));
            runAdbCommand("-s " + serial + " forward tcp:" + std::to_string(m_localPort) +
                          " tcp:" + std::to_string(candidates[ci]));
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
    }
    m_serverPort = launchPort;

    // 2. Push and start the server.
    m_statusText = "Pushing server binary to device...";
    const std::string serverBin = getServerBinaryPath();
    LOG_INFO("Server", "Step 2: Push server binary: " + serverBin);
    {
        std::error_code ec;
        if (!std::filesystem::exists(serverBin, ec) || ec) {
            m_lastError = "Server binary not found: " + serverBin;
            LOG_ERROR("Server", m_lastError);
            m_statusText = "ERROR: " + m_lastError;
            return false;
        }
    }
    const std::string pushOut =
        runAdbCommand("-s " + serial + " push \"" + serverBin + "\" /data/local/tmp/afm-server");
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
    stopDeviceServerProcesses(serial);
    runAdbCommand("-s " + serial + " shell rm -f /data/local/tmp/afm-server.log");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const auto launch = runAdbCommandResult(
        "-s " + serial + " shell \"" + serverLaunchCommand(launchPort, m_useRoot) + "\"", 5000);
    if (launch.cancelled) return false;
    if (!launch.succeeded()) LOG_WARN("Server", "Helper launch: " + launch.diagnostics());
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    std::string psCheck =
        runAdbCommand("-s " + serial + " shell \"pidof afm-server 2>/dev/null\"");
    bool serverPidFound =
        !psCheck.empty() && psCheck.find_first_of("0123456789") != std::string::npos;
    LOG_INFO("Server", "pidof afm-server: '" + psCheck + "' -> " +
                           std::string(serverPidFound ? "FOUND" : "NOT FOUND"));
    if (!serverPidFound) {
        const std::string startupLog = sanitizeAdbOutput(runAdbCommand(
            "-s " + serial + " shell \"cat /data/local/tmp/afm-server.log 2>/dev/null\""));
        if (!startupLog.empty()) {
            LOG_ERROR("Server", "afm-server startup log: " + startupLog);
            m_lastError = "Device helper failed to start: " + startupLog;
        } else {
            const std::string fileInfo = sanitizeAdbOutput(runAdbCommand(
                "-s " + serial +
                " shell \"ls -l /data/local/tmp/afm-server 2>/dev/null; getprop ro.product.cpu.abi; "
                "getprop ro.build.version.sdk\""));
            if (!fileInfo.empty())
                LOG_ERROR("Server", "afm-server not running, device info: " + fileInfo);
        }
    }

    // 3. Direct TCP with backoff.
    std::string retryIp = detectDeviceIp(serial);
    LOG_INFO("Server", "Step 4: retryIp='" + retryIp + "' savedIp='" + savedIp + "' deviceIp='" +
                           deviceIp + "'");
    if (retryIp.empty()) retryIp = savedIp;
    if (retryIp.empty()) retryIp = deviceIp;

    if (!retryIp.empty() && !preferAdbForward) {
        LOG_INFO("Server", "Step 5: Direct TCP retries to " + retryIp);
        int delayMs = 1000;
        for (int attempt = 0; attempt < 15; ++attempt) {
            LOG_DEBUG("Server", "Direct TCP attempt " + std::to_string(attempt + 1) +
                                     "/15 (delay=" + std::to_string(delayMs) + "ms)");
            m_statusText = "Direct TCP (" + retryIp + ") attempt " + std::to_string(attempt + 1) +
                "/15" + (serverPidFound ? "" : " [server not detected]") + " ...";
            if (tryDirectConnect(retryIp, launchPort)) {
                LOG_INFO("Server", "Direct TCP connected on attempt " + std::to_string(attempt + 1));
                m_statusText = "Connected via Direct TCP";
                m_connected = true;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            if (delayMs < 8000) delayMs = std::min(delayMs * 2, 8000);
            if (attempt % 5 == 4) {
                LOG_DEBUG("Server", "Re-detecting IP and checking server process...");
                const std::string newIp = detectDeviceIp(serial);
                if (!newIp.empty() && newIp != retryIp) {
                    LOG_INFO("Server", "IP changed: " + retryIp + " -> " + newIp);
                    retryIp = newIp;
                }
                psCheck =
                    runAdbCommand("-s " + serial + " shell \"pidof afm-server 2>/dev/null\"");
                serverPidFound =
                    !psCheck.empty() && psCheck.find_first_of("0123456789") != std::string::npos;
                LOG_INFO("Server",
                         std::string("pidof re-check: ") + (serverPidFound ? "FOUND" : "NOT FOUND"));
                if (!serverPidFound) {
                    LOG_WARN("Server", "Server not running - restarting...");
                    m_statusText = "Server not running - restarting...";
                    runAdbCommand("-s " + serial + " shell rm -f /data/local/tmp/afm-server.log");
                    const auto restart = runAdbCommandResult(
                        "-s " + serial + " shell \"" +
                            serverLaunchCommand(launchPort, m_useRoot) + "\"",
                        5000);
                    if (restart.cancelled) return false;
                    if (!restart.succeeded())
                        LOG_WARN("Server", "Helper restart: " + restart.diagnostics());
                    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                }
            }
        }
        LOG_WARN("Server", "All 15 Direct TCP attempts failed");
    } else {
        LOG_WARN("Server", "No IP for Direct TCP retries, skipping");
    }

    // 4. ADB forward fallback.
    LOG_INFO("Server", "Step 6: ADB Forward fallback");
    m_statusText = "Setting up ADB Forward...";
    runAdbCommand("-s " + serial + " forward --remove tcp:" + std::to_string(m_localPort));
    runAdbCommand("-s " + serial + " forward tcp:" + std::to_string(m_localPort) +
                  " tcp:" + std::to_string(launchPort));
    for (int attempt = 0; attempt < 5; ++attempt) {
        LOG_DEBUG("Server", "ADB Forward attempt " + std::to_string(attempt + 1) + "/5");
        m_statusText = "ADB Forward attempt " + std::to_string(attempt + 1) + "/5...";
        if (connectTcp("127.0.0.1", m_localPort) && verifyConnection()) {
            LOG_INFO("Server",
                     "Connected via ADB Forward on attempt " + std::to_string(attempt + 1));
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
        if (!m_serial.empty())
            runAdbCommand("-s " + m_serial + " forward --remove tcp:" + std::to_string(m_localPort));
        return true;
    }
    disconnectTcp();
    return false;
}

bool DeviceClient::connectDirectForSerial(const std::string& serial, const std::string& ip,
                                          int port) {
    if (serial.empty() || ip.empty() || port <= 0) return false;
    const std::lock_guard<std::mutex> serverLk(m_serverMutex);
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
        LOG_INFO("Server", "Connected directly through WiFi failover: " + serial +
                                " on helper port " + std::to_string(port));
        return true;
    }
    disconnectTcp();
    return false;
}

bool DeviceClient::tryEnableUsbTethering(const std::string& serial) {
    const std::string ser = serial.empty() ? m_serial : serial;
    if (ser.empty()) return false;
    LOG_INFO("Tether", "tryEnableUsbTethering(serial=" + ser + ")");
    m_statusText = "Enabling USB tethering...";
    // NOTE: changes the device USB function; ADB drops and reconnects.
    // The GUI calls this only from the Setup Wizard with user consent.
    runAdbCommand("-s " + ser + " shell svc usb setFunctions rndis");
    m_statusText = "Waiting for USB to settle...";
    const char* tetherIfaces[] = {"rndis0", "usb0", "ncm0", nullptr};
    for (int i = 0; i < 15; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        for (auto& d : getDevices()) {
            if (d.state != "device") continue;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            for (int j = 0; tetherIfaces[j]; ++j) {
                const std::string out =
                    runAdbCommand("-s " + d.serial + " shell \"ip -4 addr show " +
                                  tetherIfaces[j] + " 2>/dev/null | grep inet\"");
                const auto inetPos = out.find("inet ");
                if (inetPos == std::string::npos) continue;
                const auto start = inetPos + 5;
                const auto endPos = std::min(out.find('/', start), out.find(' ', start));
                if (endPos != std::string::npos && endPos > start)
                    m_deviceIp = out.substr(start, endPos - start);
                LOG_INFO("Tether", std::string("Tethering enabled! ") + tetherIfaces[j] +
                                       " IP: " + m_deviceIp + " (device serial: " + d.serial + ")");
                m_statusText = "USB tethering enabled";
                return true;
            }
            LOG_DEBUG("Tether", "Device " + d.serial +
                                     " found but no USB tethering IP yet (attempt " +
                                     std::to_string(i) + "/15)");
        }
    }
    LOG_WARN("Tether", "Tethering not available after 15 attempts");
    m_statusText = "Tethering not available";
    return false;
}

bool DeviceClient::tryUpgradeToDirect() {
    if (!m_connected || m_serial.empty() || m_directConnection) return m_directConnection;
    const std::string ip = detectDeviceIp(m_serial);
    if (ip.empty()) return false;

    std::string error;
    const int testFd = net::tcpConnect(ip, AFM_PORT, 1500, "", error);
    if (testFd < 0) return false;
    MsgHeader pingHdr{CMD_PING, 0};
    MsgHeader respHdr{};
    char respBuf[64];
    bool ok = net::sendAll(testFd, &pingHdr, sizeof(pingHdr), 1500, error);
    if (ok) ok = net::recvAll(testFd, &respHdr, sizeof(respHdr), 1500, error);
    if (ok) {
        if (respHdr.cmd != RSP_OK || respHdr.length < 4 || respHdr.length > sizeof(respBuf))
            ok = false;
        else if (!net::recvAll(testFd, respBuf, respHdr.length, 1500, error))
            ok = false;
        else {
            uint32_t magic = 0;
            std::memcpy(&magic, respBuf, 4);
            ok = (magic == AFM_MAGIC);
        }
    }
    if (!ok) {
        ::close(testFd);
        return false;
    }
    {
        const std::lock_guard<std::mutex> lk(m_mutex);
        if (m_socket != kInvalid) ::close(static_cast<int>(m_socket));
        m_socket = static_cast<uintptr_t>(testFd);
        m_directConnection = true;
        m_deviceIp = ip;
        m_lastConnectHost = ip;
        m_lastConnectPort = AFM_PORT;
        m_lastConnectBindIp.clear();
    }
    runAdbCommand("-s " + m_serial + " forward --remove tcp:" + std::to_string(m_localPort));
    return true;
}

void DeviceClient::stopServer() {
    const std::lock_guard<std::mutex> serverLk(m_serverMutex);
    if (m_connected) {
        const std::lock_guard<std::mutex> lk(m_mutex);
        sendMsg(CMD_QUIT);
        MsgHeader hdr{};
        std::vector<char> payload;
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
    std::string error;
    const int fd = net::tcpConnect(host, port, 5000, bindLocalIp, error);
    if (fd < 0) {
        LOG_ERROR("TCP", "tcpConnect failed for " + host + ":" + std::to_string(port) + ": " + error);
        return kInvalid;
    }
    return static_cast<uintptr_t>(fd);
}

bool DeviceClient::connectTcp(const std::string& host, int port, const std::string& bindLocalIp) {
    LOG_DEBUG("TCP", "Connecting to " + host + ":" + std::to_string(port) + "...");
    const uintptr_t sockU = openServerSocket(host, port, bindLocalIp);
    if (sockU == kInvalid) {
        LOG_WARN("TCP", "openServerSocket failed for " + host + ":" + std::to_string(port));
        return false;
    }
    LOG_INFO("TCP", "Connected to " + host + ":" + std::to_string(port));
    m_connected = true;
    m_socket = sockU;
    m_lastConnectHost = host;
    m_lastConnectPort = port;
    m_lastConnectBindIp = bindLocalIp;
    return true;
}

bool DeviceClient::connectParallelFrom(const DeviceClient& source) {
    if (!source.m_connected || source.m_lastConnectHost.empty()) return false;
    m_adbPath = source.m_adbPath;
    m_serial = source.m_serial;
    m_useRoot = source.m_useRoot;
    m_serverIsRoot = source.m_serverIsRoot;
    m_directConnection = source.m_directConnection;
    m_deviceIp = source.m_deviceIp;
    m_serverPort = source.m_serverPort;
    m_ownsServer = false;
    if (!connectTcp(source.m_lastConnectHost, source.m_lastConnectPort,
                    source.m_lastConnectBindIp))
        return false;
    if (!verifyConnection()) {
        disconnectTcp();
        return false;
    }
    return true;
}

bool DeviceClient::sendAllOn(uintptr_t sockU, const void* data, size_t len) {
    if (sockU == kInvalid) return false;
    std::string error;
    return net::sendAll(static_cast<int>(sockU), data, len, kDefaultSendMs, error);
}

bool DeviceClient::recvAllOn(uintptr_t sockU, void* data, size_t len) {
    if (sockU == kInvalid) return false;
    std::string error;
    return net::recvAll(static_cast<int>(sockU), data, len, kDefaultRecvMs, error);
}

bool DeviceClient::sendMsgOn(uintptr_t sock, uint32_t cmd, const void* payload, uint32_t len,
                              uint32_t sendTimeoutMs) {
    if (sock == kInvalid) return false;
    MsgHeader hdr{cmd, len};
    std::string error;
    const uint32_t sendMs = sendTimeoutMs ? sendTimeoutMs : kDefaultSendMs;
    if (!net::sendAll(static_cast<int>(sock), &hdr, sizeof(hdr), sendMs, error)) return false;
    if (len > 0 && !net::sendAll(static_cast<int>(sock), payload, len, sendMs, error)) return false;
    return true;
}

bool DeviceClient::recvMsgOn(uintptr_t sock, MsgHeader& hdr, std::vector<char>& payload,
                              uint32_t recvTimeoutMs) {
    if (sock == kInvalid) return false;
    const uint32_t recvMs = recvTimeoutMs ? recvTimeoutMs : kDefaultRecvMs;
    std::string error;
    if (!net::recvAll(static_cast<int>(sock), &hdr, sizeof(hdr), recvMs, error)) return false;
    const uint32_t maxPayload = AFM_CHUNK_SIZE + 65536;
    if (hdr.length > maxPayload) return false;
    payload.resize(hdr.length);
    if (hdr.length > 0 && !net::recvAll(static_cast<int>(sock), payload.data(), hdr.length, recvMs, error))
        return false;
    return true;
}

bool DeviceClient::ensureControlChannelLocked() {
    if (m_ctlConnected && m_ctlSocket != kInvalid) return true;
    if (!m_connected || m_lastConnectHost.empty()) return false;
    const uintptr_t sockU =
        openServerSocket(m_lastConnectHost, m_lastConnectPort, m_lastConnectBindIp);
    if (sockU == kInvalid) return false;
    m_ctlSocket = sockU;
    m_ctlConnected = true;
    m_ctlInterruptFd.store(static_cast<int>(sockU));
    LOG_INFO("TCP", "Control channel connected");
    return true;
}

void DeviceClient::closeControlChannelLocked() {
    m_ctlInterruptFd.store(-1);
    if (m_ctlSocket != kInvalid) {
        ::close(static_cast<int>(m_ctlSocket));
        m_ctlSocket = kInvalid;
    }
    m_ctlConnected = false;
}

bool DeviceClient::runSmallOp(uint32_t cmd, const void* payload, uint32_t plen, MsgHeader& outHdr,
                              std::vector<char>& outPayload, uint32_t recvTimeoutMs) {
    const uint32_t recvMs = recvTimeoutMs ? recvTimeoutMs : kDefaultRecvMs;
    {
        const std::lock_guard<std::mutex> lk(m_ctlMutex);
        if (ensureControlChannelLocked()) {
            const int fd = static_cast<int>(m_ctlSocket);
            std::string error;
            const bool ok = sendFrame(fd, cmd, payload, plen, kDefaultSendMs, recvMs, error) &&
                recvFrame(fd, outHdr, outPayload, recvMs, error);
            if (ok) return true;
            m_lastError = error;
            closeControlChannelLocked();  // fall back to primary
        }
    }
    const std::lock_guard<std::mutex> lk(m_mutex);
    if (m_socket == kInvalid) {
        m_lastError = "Not connected";
        return false;
    }
    const int fd = static_cast<int>(m_socket);
    std::string error;
    if (!sendFrame(fd, cmd, payload, plen, kDefaultSendMs, recvMs, error) ||
        !recvFrame(fd, outHdr, outPayload, recvMs, error)) {
        m_lastError = error;
        return false;
    }
    return true;
}

void DeviceClient::flushStaleData() {
    if (!m_connected) return;
    const std::lock_guard<std::mutex> lk(m_mutex);
    if (m_socket == kInvalid) return;
    const int fd = static_cast<int>(m_socket);
    char discard[4096];
    int totalFlushed = 0;
    while (totalFlushed <= 1024 * 1024) {
        if (!net::waitReadable(fd, 100)) break;
        const ssize_t n = ::recv(fd, discard, sizeof(discard), 0);
        if (n <= 0) break;
        totalFlushed += static_cast<int>(n);
    }
    if (totalFlushed > 0)
        LOG_INFO("TCP", "Flushed " + std::to_string(totalFlushed) + " bytes of stale data");
}

void DeviceClient::interruptBlocking() {
    // shutdown() unblocks a stuck poll/recv/send from another thread.
    // Same benign fd-reuse race as the Windows version; cancellation only.
    if (m_socket != kInvalid) net::interruptSocket(static_cast<int>(m_socket));
    const int ctlFd = m_ctlInterruptFd.load();
    if (ctlFd >= 0) net::interruptSocket(ctlFd);
}

void DeviceClient::restoreTimeouts() {
    // No-op on Linux: every wait carries an explicit per-call deadline,
    // so there is no socket-wide timeout state to restore.
}

void DeviceClient::disconnectTcp() {
    if (m_socket != kInvalid) {
        ::close(static_cast<int>(m_socket));
        m_socket = kInvalid;
    }
    m_connected = false;
    m_directConnection = false;
    m_serverIsRoot = false;
    {
        const std::lock_guard<std::mutex> lk(m_ctlMutex);
        closeControlChannelLocked();
    }
}

bool DeviceClient::sendAll(const void* data, size_t len) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    std::string error;
    if (!net::sendAll(static_cast<int>(m_socket), data, len, kDefaultSendMs, error)) {
        m_lastError = "Send failed: " + error;
        LOG_ERROR("TCP", m_lastError);
        m_connected = false;
        return false;
    }
    return true;
}

bool DeviceClient::recvAll(void* data, size_t len) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    std::string error;
    if (!net::recvAll(static_cast<int>(m_socket), data, len, kDefaultRecvMs, error)) {
        m_lastError = "Recv failed: " + error;
        LOG_ERROR("TCP", m_lastError);
        m_connected = false;
        return false;
    }
    return true;
}

bool DeviceClient::sendMsg(uint32_t cmd, const void* payload, uint32_t len) {
    MsgHeader hdr{cmd, len};
    if (!sendAll(&hdr, sizeof(hdr))) return false;
    if (len > 0 && !sendAll(payload, len)) return false;
    return true;
}

bool DeviceClient::recvMsg(MsgHeader& hdr, std::vector<char>& payload) {
    if (!recvAll(&hdr, sizeof(hdr))) return false;
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
    MsgHeader hdr{};
    std::vector<char> payload;
    if (!runSmallOp(CMD_STORAGE, nullptr, 0, hdr, payload)) return "/storage/emulated/0";
    if (hdr.cmd == RSP_OK && !payload.empty())
        return std::string(payload.data(), payload.size());
    return "/storage/emulated/0";
}

std::vector<DeviceFileEntry> DeviceClient::listDirectory(const std::string& path) {
    std::vector<DeviceFileEntry> entries;
    if (!m_connected) {
        LOG_ERROR("Files", "List folder failed: " + path + ": device not connected");
        return entries;
    }
    MsgHeader hdr{};
    std::vector<char> payload;
    if (!runSmallOp(CMD_LIST, path.data(), static_cast<uint32_t>(path.size()), hdr, payload)) {
        LOG_ERROR("Files", "List folder failed: " + path + ": " + m_lastError);
        return entries;
    }
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        LOG_ERROR("Files", "List folder failed: " + path + ": " + m_lastError);
        return entries;
    }
    if (hdr.cmd != RSP_OK || payload.size() < 4) {
        LOG_ERROR("Files", "Invalid folder listing response: " + path);
        return entries;
    }
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
    std::sort(entries.begin(), entries.end(),
              [](const DeviceFileEntry& a, const DeviceFileEntry& b) {
                  if (a.isDirectory() != b.isDirectory()) return a.isDirectory();
                  return a.name < b.name;
              });
    return entries;
}

bool DeviceClient::getDiskSpace(uint64_t& totalBytes, uint64_t& freeBytes) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    MsgHeader hdr{};
    std::vector<char> payload;
    if (!runSmallOp(CMD_DISK_SPACE, nullptr, 0, hdr, payload)) return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    if (hdr.cmd != RSP_OK || payload.size() < 16) return false;
    std::memcpy(&totalBytes, payload.data(), 8);
    std::memcpy(&freeBytes, payload.data() + 8, 8);
    return true;
}

bool DeviceClient::deleteFile(const std::string& path) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    MsgHeader hdr{};
    std::vector<char> payload;
    if (!runSmallOp(CMD_DELETE, path.data(), static_cast<uint32_t>(path.size()), hdr, payload))
        return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    return hdr.cmd == RSP_OK;
}

bool DeviceClient::createDirectory(const std::string& path) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    MsgHeader hdr{};
    std::vector<char> payload;
    if (!runSmallOp(CMD_MKDIR, path.data(), static_cast<uint32_t>(path.size()), hdr, payload))
        return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(payload.data(), payload.size());
        return false;
    }
    return hdr.cmd == RSP_OK;
}

bool DeviceClient::renameFile(const std::string& oldPath, const std::string& newPath) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }
    const uint32_t oldLen = static_cast<uint32_t>(oldPath.size());
    std::vector<char> payload(4 + oldPath.size() + newPath.size());
    std::memcpy(payload.data(), &oldLen, 4);
    std::memcpy(payload.data() + 4, oldPath.data(), oldPath.size());
    std::memcpy(payload.data() + 4 + oldPath.size(), newPath.data(), newPath.size());
    MsgHeader hdr{};
    std::vector<char> resp;
    if (!runSmallOp(CMD_RENAME, payload.data(), static_cast<uint32_t>(payload.size()), hdr, resp))
        return false;
    if (hdr.cmd == RSP_ERROR) {
        m_lastError = std::string(resp.data(), resp.size());
        return false;
    }
    return hdr.cmd == RSP_OK;
}

uint64_t DeviceClient::getFileSize(const std::string& path) {
    if (!m_connected) return 0;
    MsgHeader hdr{};
    std::vector<char> payload;
    if (!runSmallOp(CMD_STAT, path.data(), static_cast<uint32_t>(path.size()), hdr, payload))
        return 0;
    if (hdr.cmd == RSP_OK && payload.size() >= sizeof(StatResponse)) {
        StatResponse sr{};
        std::memcpy(&sr, payload.data(), sizeof(sr));
        return sr.size;
    }
    return 0;
}

bool DeviceClient::statPath(const std::string& path, DeviceFileEntry& out,
                            std::string* error) {
    out = DeviceFileEntry{};
    const auto slash = path.find_last_of('/');
    out.name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (!m_connected) {
        if (error) *error = "Not connected";
        return false;
    }
    MsgHeader hdr{};
    std::vector<char> payload;
    if (!runSmallOp(CMD_STAT, path.data(), static_cast<uint32_t>(path.size()), hdr, payload)) {
        if (error) *error = m_lastError.empty() ? "stat failed" : m_lastError;
        return false;
    }
    if (hdr.cmd == RSP_ERROR) {
        const std::string message(payload.data(), payload.size());
        if (error) *error = message;
        return false;
    }
    if (hdr.cmd != RSP_OK || payload.size() < sizeof(StatResponse)) {
        if (error) *error = "malformed stat response";
        return false;
    }
    StatResponse sr{};
    std::memcpy(&sr, payload.data(), sizeof(sr));
    if (sr.type == 255) {
        if (error) *error = "No such file or directory";
        return false;
    }
    out.type = sr.type;
    out.size = sr.size;
    out.mtime = static_cast<int64_t>(sr.mtime);
    return true;
}
