// POSIX implementation of src/net_posix.h. See header for semantics.
#include "net_posix.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>

namespace net {
namespace {

using Clock = std::chrono::steady_clock;

// Set by waitFor() when poll returns 0 (deadline expiry); cleared otherwise.
// Mirrors the Windows build's WSAETIMEDOUT checks in the ADB track loop.
thread_local bool t_lastWaitTimedOut = false;

uint32_t remainingMs(const Clock::time_point& deadline) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (left.count() <= 0) return 0;
    return static_cast<uint32_t>(std::min<int64_t>(left.count(), 3600000));
}

bool setNonBlocking(int fd, std::string& error) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        error = std::string("fcntl failed (errno ") + std::to_string(errno) + ").";
        return false;
    }
    return true;
}

void tuneBuffers(int fd) {
    const int bufsize = 4 * 1024 * 1024;  // same as openServerSocket
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    const int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    const int keepalive = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    const int idle = 5, interval = 1, count = 3;  // same as Windows TCP_KEEP* values
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
}

bool waitFor(int fd, short events, uint32_t timeoutMs, std::string& error) {
    struct pollfd pfd = {fd, events, 0};
    const int ready = ::poll(&pfd, 1, static_cast<int>(timeoutMs));
    if (ready < 0) {
        t_lastWaitTimedOut = false;
        if (errno == EINTR) return false;
        error = std::string("poll failed (errno ") + std::to_string(errno) + ").";
        return false;
    }
    if (ready == 0) {
        t_lastWaitTimedOut = true;
        error = "Timed out waiting for peer.";
        return false;
    }
    t_lastWaitTimedOut = false;
    if (pfd.revents & (POLLERR | POLLNVAL)) {
        error = "Socket error while waiting for peer.";
        return false;
    }
    return true;
}
}  // namespace

bool lastWaitTimedOut() {
    return t_lastWaitTimedOut;
}

void Socket::reset(int fd) {
    if (m_fd >= 0) ::close(m_fd);
    m_fd = fd;
}

int tcpConnect(const std::string& host, int port, uint32_t timeoutMs,
               const std::string& bindLocalIp, std::string& error) {
    if (port <= 0 || port > 65535) {
        error = "Invalid port.";
        return -1;
    }
    Socket sock(::socket(AF_INET, SOCK_STREAM, 0));
    if (!sock.valid()) {
        error = std::string("socket() failed (errno ") + std::to_string(errno) + ").";
        return -1;
    }
    tuneBuffers(sock.get());

    if (!bindLocalIp.empty()) {
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = 0;
        if (::inet_pton(AF_INET, bindLocalIp.c_str(), &bindAddr.sin_addr) != 1) {
            error = "Invalid bind address: " + bindLocalIp;
            return -1;
        }
        if (::bind(sock.get(), reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
            error = std::string("bind() failed (errno ") + std::to_string(errno) + ").";
            return -1;
        }
    }

    if (!setNonBlocking(sock.get(), error)) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        error = "Invalid connect address: " + host;
        return -1;
    }
    const int ret = ::connect(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (ret != 0 && errno != EINPROGRESS) {
        error = std::string("connect() failed (errno ") + std::to_string(errno) + ").";
        return -1;
    }
    if (ret != 0) {
        struct pollfd pfd = {sock.get(), POLLOUT, 0};
        const int ready = ::poll(&pfd, 1,
                                 static_cast<int>(timeoutMs ? timeoutMs : 5000));
        if (ready <= 0) {
            error = ready == 0 ? "Timed out connecting." : "poll() failed while connecting.";
            return -1;
        }
        int sockErr = 0;
        socklen_t optLen = sizeof(sockErr);
        if (::getsockopt(sock.get(), SOL_SOCKET, SO_ERROR, &sockErr, &optLen) != 0 ||
            sockErr != 0) {
            error = std::string("connect() failed (errno ") + std::to_string(sockErr) + ").";
            return -1;
        }
    }
    // Back to blocking: all waits go through poll() with explicit deadlines.
    const int flags = ::fcntl(sock.get(), F_GETFL, 0);
    if (flags >= 0) ::fcntl(sock.get(), F_SETFL, flags & ~O_NONBLOCK);
    return sock.release();
}

bool sendAll(int fd, const void* data, size_t len, uint32_t timeoutMs, std::string& error) {
    const auto deadline = Clock::now() +
        std::chrono::milliseconds(timeoutMs ? timeoutMs : 30000);
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        if (!waitFor(fd, POLLOUT, remainingMs(deadline), error)) return false;
        const ssize_t n =
            ::send(fd, p, std::min(len, static_cast<size_t>(INT32_MAX)), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            error = std::string("send() failed (errno ") + std::to_string(errno) + ").";
            return false;
        }
        if (n == 0) {
            error = "Connection closed while sending.";
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool recvAll(int fd, void* data, size_t len, uint32_t timeoutMs, std::string& error) {
    const auto deadline = Clock::now() +
        std::chrono::milliseconds(timeoutMs ? timeoutMs : 10000);
    char* p = static_cast<char*>(data);
    while (len > 0) {
        if (!waitFor(fd, POLLIN, remainingMs(deadline), error)) return false;
        const ssize_t n =
            ::recv(fd, p, std::min(len, static_cast<size_t>(INT32_MAX)), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            error = std::string("recv() failed (errno ") + std::to_string(errno) + ").";
            return false;
        }
        if (n == 0) {
            error = "Connection closed by peer.";
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool waitReadable(int fd, uint32_t timeoutMs) {
    std::string ignored;
    return waitFor(fd, POLLIN, timeoutMs, ignored);
}

void interruptSocket(int fd) {
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);  // unblocks a stuck poll/recv/send
}

}  // namespace net
