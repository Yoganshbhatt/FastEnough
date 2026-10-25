#pragma once
// Linux single-instance handoff, the counterpart of the Windows
// FindWindow + WM_COPYDATA path in src/main.cpp. The primary instance owns a
// per-user Unix datagram socket; a second launch sends an optional APK path
// (empty means "raise the window") and exits.
#include <functional>
#include <string>

namespace single_instance {

// $XDG_RUNTIME_DIR/fast-enough.sock, or /tmp/fast-enough-<uid>.sock.
std::string socketPath();

// Listening endpoint owned by the primary instance.
class Server {
public:
    Server() = default;
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Creates the socket (replacing a stale one from a crash). False means the
    // app continues without handoff; it is not fatal.
    bool start();
    void stop();

    // Drains pending datagrams on the calling (main) thread. onMessage gets the
    // APK path, or an empty string for a plain raise request.
    void poll(const std::function<void(const std::string&)>& onMessage);

private:
    int m_fd = -1;
    std::string m_path;
};

// Second instance: deliver apkPath (may be empty) to the primary. Retries
// briefly to cover the primary's start-up window. True if delivered.
bool handoff(const std::string& apkPath, int retries = 6);

}  // namespace single_instance
