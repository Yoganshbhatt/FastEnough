#pragma once
// POSIX socket primitives for the Linux port (AGENTS.md).
// Mirrors DeviceClient::openServerSocket semantics: 4MB socket buffers,
// TCP_NODELAY, keepalive, non-blocking connect with timeout. All waits use
// poll() with explicit deadlines — no SO_RCVTIMEO juggling. send() uses
// MSG_NOSIGNAL so a dead peer surfaces as an error, never a SIGPIPE kill.
#include <cstddef>
#include <cstdint>
#include <string>

namespace net {

constexpr uintptr_t kInvalidSocket = ~(uintptr_t)0;

class Socket {
public:
    explicit Socket(int fd = -1) : m_fd(fd) {}
    ~Socket() { reset(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : m_fd(other.m_fd) { other.m_fd = -1; }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset();
            m_fd = other.m_fd;
            other.m_fd = -1;
        }
        return *this;
    }
    int get() const { return m_fd; }
    bool valid() const { return m_fd >= 0; }
    int release() {
        const int fd = m_fd;
        m_fd = -1;
        return fd;
    }
    void reset(int fd = -1);
private:
    int m_fd;
};

// Connect with timeout. Returns fd or -1 with errno text in error.
int tcpConnect(const std::string& host, int port, uint32_t timeoutMs,
               const std::string& bindLocalIp, std::string& error);

// Blocking-exact send/recv bounded by a total deadline (0 = 30s default).
bool sendAll(int fd, const void* data, size_t len, uint32_t timeoutMs, std::string& error);
bool recvAll(int fd, void* data, size_t len, uint32_t timeoutMs, std::string& error);
bool waitReadable(int fd, uint32_t timeoutMs);

// True when the last waitFor/sendAll/recvAll failure on this thread was a
// deadline expiry (poll returned 0). ADB track loop uses this the way the
// Windows build uses WSAETIMEDOUT. Thread-local; check right after failure.
bool lastWaitTimedOut();

// Unblock a thread stuck in sendAll/recvAll (shutdown from another thread).
// Equivalent of DeviceClient::interruptBlocking().
void interruptSocket(int fd);

}  // namespace net
