#include "single_instance_linux.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace single_instance {
namespace {

sockaddr_un makeAddress(const std::string& path, bool& ok) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ok = path.size() < sizeof(addr.sun_path);
    if (ok) std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return addr;
}

}  // namespace

std::string socketPath() {
    const char* runtime = ::getenv("XDG_RUNTIME_DIR");
    std::string dir = (runtime && *runtime) ? runtime : "/tmp";
    return dir + "/fast-enough-" + std::to_string(::getuid()) + ".sock";
}

Server::~Server() { stop(); }

bool Server::start() {
    stop();
    const std::string path = socketPath();
    bool ok = false;
    const sockaddr_un addr = makeAddress(path, ok);
    if (!ok) return false;

    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    // A stale socket from a crashed instance would make bind fail with
    // EADDRINUSE. The flock already guarantees no live primary owns it.
    ::unlink(path.c_str());
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }
    ::chmod(path.c_str(), 0600);
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        ::close(fd);
        ::unlink(path.c_str());
        return false;
    }
    m_fd = fd;
    m_path = path;
    return true;
}

void Server::stop() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    if (!m_path.empty()) {
        ::unlink(m_path.c_str());
        m_path.clear();
    }
}

void Server::poll(const std::function<void(const std::string&)>& onMessage) {
    if (m_fd < 0) return;
    char buffer[8192];
    for (;;) {
        const ssize_t n = ::recvfrom(m_fd, buffer, sizeof(buffer) - 1, 0, nullptr, nullptr);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;  // EAGAIN/EWOULDBLOCK: drained
        }
        buffer[n] = '\0';
        if (onMessage) onMessage(std::string(buffer, static_cast<size_t>(n)));
    }
}

bool handoff(const std::string& apkPath, int retries) {
    bool ok = false;
    const std::string path = socketPath();
    const sockaddr_un addr = makeAddress(path, ok);
    if (!ok) return false;

    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    bool delivered = false;
    for (int attempt = 0; attempt < retries && !delivered; ++attempt) {
        const ssize_t sent = ::sendto(fd, apkPath.data(), apkPath.size(), MSG_NOSIGNAL,
                                      reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        if (sent == static_cast<ssize_t>(apkPath.size())) {
            delivered = true;
            break;
        }
        // No listener yet (primary still starting): wait briefly and retry.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::close(fd);
    return delivered;
}

}  // namespace single_instance
