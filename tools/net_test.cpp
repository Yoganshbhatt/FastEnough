// Verifies src/net_posix.cpp against loopback sockets only. No device, no adb.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>

#include "net_posix.h"

namespace {
int failures = 0;
void check(bool ok, const char* name, const std::string& detail = {}) {
    if (ok) {
        std::puts(("  ok: " + std::string(name)).c_str());
    } else {
        ++failures;
        std::puts(("  FAIL: " + std::string(name) +
                   (detail.empty() ? "" : " -- " + detail)).c_str());
    }
}

// Binds 127.0.0.1 on an ephemeral port and returns the listening fd + port.
int listenLoopback(uint16_t& portOut) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return -1;
    if (::listen(fd, 1) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    portOut = ::ntohs(addr.sin_port);
    return fd;
}
}  // namespace

int main() {
    {
        uint16_t port = 0;
        const int listener = listenLoopback(port);
        check(listener >= 0, "loopback listen");
        // Server side: accept in background, echo one 8-byte header back.
        std::thread server([listener] {
            const int peer = ::accept(listener, nullptr, nullptr);
            char buf[8] = {};
            size_t got = 0;
            while (got < sizeof(buf)) {
                const ssize_t n = ::recv(peer, buf + got, sizeof(buf) - got, 0);
                if (n <= 0) break;
                got += static_cast<size_t>(n);
            }
            size_t sent = 0;
            while (sent < got) {
                const ssize_t n = ::send(peer, buf + sent, got - sent, MSG_NOSIGNAL);
                if (n <= 0) break;
                sent += static_cast<size_t>(n);
            }
            ::close(peer);
            ::close(listener);
        });
        std::string error;
        const int fd = net::tcpConnect("127.0.0.1", port, 3000, "", error);
        check(fd >= 0, "tcpConnect loopback", error);
        const char ping[8] = {'A', 'F', 'M', 'S', 3, 0, 0, 0};
        check(net::sendAll(fd, ping, sizeof(ping), 3000, error), "sendAll loopback", error);
        char echo[8] = {};
        check(net::recvAll(fd, echo, sizeof(echo), 3000, error), "recvAll loopback", error);
        check(std::memcmp(ping, echo, sizeof(ping)) == 0, "loopback echo round-trip");
        ::close(fd);
        server.join();
    }
    {
        std::string error;
        // Nothing listens here: must fail fast with an error, not hang.
        const int fd = net::tcpConnect("127.0.0.1", 1, 2000, "", error);
        check(fd < 0 && !error.empty(), "refused connect errors out", error);
    }
    {
        uint16_t port = 0;
        const int listener = listenLoopback(port);
        std::string error;
        const int fd = net::tcpConnect("127.0.0.1", port, 3000, "", error);
        check(fd >= 0, "tcpConnect for readability test", error);
        // Peer sends nothing: waitReadable must time out, not block forever.
        check(!net::waitReadable(fd, 200), "waitReadable times out on idle socket");
        ::close(fd);
        ::close(listener);
    }
    {
        // interruptSocket must unblock a stuck recvAll from another thread.
        int pair[2] = {-1, -1};
        check(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair setup");
        std::string error;
        bool finished = false;
        std::thread stuck([&] {
            char buf[16] = {};
            net::recvAll(pair[0], buf, sizeof(buf), 30000, error);
            finished = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        net::interruptSocket(pair[0]);
        stuck.join();
        check(finished, "interruptSocket unblocks stuck recv");
        ::close(pair[0]);
        ::close(pair[1]);
    }

    if (failures == 0) std::puts("net_test: ALL OK");
    return failures == 0 ? 0 : 1;
}
