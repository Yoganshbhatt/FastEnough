// Linux implementations for the shared GUI platform calls (see
// platform_linux.h). OpenSSL for SHA-256, getifaddrs for NICs, xdg-open for
// shell open, non-blocking connect probes for reachability.
#include "platform_linux.h"

#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include <arpa/inet.h>
#include <net/if.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "log_facade.h"
#include "subprocess.h"

namespace platform {

namespace {
std::string firstLine(const std::string& text) {
    const auto pos = text.find('\n');
    std::string line = pos == std::string::npos ? text : text.substr(0, pos);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}
}

bool systemUsesLightAppTheme() {
    // gsettings follows the freedesktop color-scheme; empty output = unset.
    FILE* pipe = ::popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null",
                         "r");
    if (!pipe) return false;
    char buf[128] = {};
    const size_t n = ::fread(buf, 1, sizeof(buf) - 1, pipe);
    const int rc = ::pclose(pipe);
    if (rc != 0 || n == 0) return false;
    return std::strstr(buf, "light") != nullptr || std::strstr(buf, "default") != nullptr;
}

bool computeLocalSha256(const std::string& path, std::string& outSha256, std::string& detail) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        detail = "Could not read local file";
        return false;
    }
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = ctx != nullptr && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1;
    std::vector<char> buf(4 * 1024 * 1024);
    while (ok) {
        ssize_t n;
        do {
            n = ::read(fd, buf.data(), buf.size());
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
            detail = "Could not read local file";
            ok = false;
            break;
        }
        if (n == 0) break;
        ok = EVP_DigestUpdate(ctx, buf.data(), static_cast<size_t>(n)) == 1;
    }
    if (ok) ok = EVP_DigestFinal_ex(ctx, digest, &digestLen) == 1 && digestLen == 32;
    EVP_MD_CTX_free(ctx);
    ::close(fd);
    if (!ok) {
        if (detail.empty()) detail = "Could not finish local hash";
        return false;
    }
    static const char* hex = "0123456789abcdef";
    outSha256.clear();
    outSha256.reserve(64);
    for (int i = 0; i < 32; ++i) {
        outSha256.push_back(hex[digest[i] >> 4]);
        outSha256.push_back(hex[digest[i] & 0x0F]);
    }
    return true;
}

bool deleteLocalFiles(const std::vector<std::string>& paths, bool permanent, std::string& error) {
    if (!permanent) {
        // Freedesktop trash, same user-visible semantics as the Windows
        // Recycle Bin path (Shift+Delete still requests permanent removal).
        // gio owns the XDG trash layout, cross-mount handling and .trashinfo;
        // argv only, no shell. A failure is reported, never downgraded to a
        // permanent delete: the caller asked for a recoverable operation.
        SubprocessOptions options;
        options.executable = "gio";
        options.arguments = {"trash", "--"};
        options.arguments.insert(options.arguments.end(), paths.begin(), paths.end());
        options.timeoutMs = 120000;
        const SubprocessResult result = runSubprocess(options);
        if (result.succeeded()) return true;

        std::string detail = firstLine(result.standardError);
        if (detail.empty()) detail = result.errorMessage;
        error = "Could not move to Trash";
        if (!detail.empty()) error += ": " + detail;
        if (detail.find("not supported") != std::string::npos ||
            detail.find("trash directory") != std::string::npos)
            error += " (use Shift+Delete to remove permanently)";
        LOG_ERROR("Activity", error);
        return false;
    }

    for (const auto& path : paths) {
        std::error_code ec;
        const std::filesystem::path p(path);
        if (std::filesystem::is_directory(p, ec) && !ec) {
            std::filesystem::remove_all(p, ec);
        } else {
            std::filesystem::remove(p, ec);
        }
        if (ec) {
            error = "Delete failed for " + path + ": " + ec.message();
            LOG_ERROR("Activity", error);
            return false;
        }
    }
    return true;
}

bool shellOpen(const std::string& pathOrUri, std::string& error) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        error = "fork failed";
        return false;
    }
    if (pid == 0) {
        ::setsid();
        ::execlp("xdg-open", "xdg-open", pathOrUri.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "xdg-open failed for " + pathOrUri;
        LOG_WARN("Activity", error);
        return false;
    }
    return true;
}

bool refreshDesktopDatabase(const std::string& applicationsDir) {
    SubprocessOptions options;
    options.executable = "update-desktop-database";
    options.arguments = {applicationsDir};
    options.timeoutMs = 10000;
    const SubprocessResult result = runSubprocess(options);
    if (!result.succeeded()) {
        // Not fatal: the entry still works after the next desktop-cache refresh
        // or login; only automatic MIME discovery is delayed.
        LOG_WARN("Platform", "update-desktop-database failed: " + result.diagnostics());
        return false;
    }
    return true;
}

bool tcpProbe(const std::string& ip, int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return false;
    }
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    bool reachable = rc == 0;
    if (!reachable && (errno == EINPROGRESS)) {
        pollfd pfd = {fd, POLLOUT, 0};
        reachable = ::poll(&pfd, 1, 1000) > 0 && !(pfd.revents & (POLLERR | POLLHUP));
    }
    ::close(fd);
    return reachable;
}

bool adbServerProbe() {
    return tcpProbe("127.0.0.1", 5037);
}

bool probeReachable(const std::string& ip, int port) {
    return tcpProbe(ip, port);
}

std::vector<DetectedNic> enumerateNics() {
    std::vector<DetectedNic> result;
    ifaddrs* ifas = nullptr;
    if (::getifaddrs(&ifas) != 0) return result;
    for (ifaddrs* ifa = ifas; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        const auto* sa = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        char ipStr[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));
        std::string ip(ipStr);
        if (ip == "127.0.0.1" || ip.rfind("169.254.", 0) == 0) continue;
        DetectedNic nic;
        nic.adapterName = ifa->ifa_name ? ifa->ifa_name : "?";
        nic.ipAddress = ip;
        nic.description = nic.adapterName;
        nic.speed = 0;  // ethtool/ioctl link-speed queries are a follow-up
        result.push_back(std::move(nic));
    }
    ::freeifaddrs(ifas);
    // One entry per address; keep list stable and ordered by name.
    std::sort(result.begin(), result.end(),
              [](const DetectedNic& a, const DetectedNic& b) { return a.adapterName < b.adapterName; });
    return result;
}

}  // namespace platform
