// Unit checks for the Linux single-instance handoff. Runs in an isolated
// XDG_RUNTIME_DIR so the real user's socket and app are never touched.
#include "single_instance_linux.h"

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static int g_failures = 0;

static void check(bool condition, const std::string& what) {
    std::cout << (condition ? "PASS  " : "FAIL  ") << what << "\n";
    if (!condition) ++g_failures;
}

int main() {
    const fs::path root =
        fs::path("/tmp") / ("afm-single-instance-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    if (ec) {
        std::cerr << "Could not create test root: " << ec.message() << "\n";
        return 1;
    }
    ::setenv("XDG_RUNTIME_DIR", root.c_str(), 1);

    const std::string path = single_instance::socketPath();
    check(path.rfind(root.string(), 0) == 0, "socket lives under XDG_RUNTIME_DIR");

    single_instance::Server server;
    check(server.start(), "server binds the socket");
    struct stat st{};
    check(::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode), "socket file exists");
    check((st.st_mode & 0777) == 0600, "socket is owner-only (0600)");

    std::vector<std::string> messages;
    auto drain = [&]() {
        messages.clear();
        server.poll([&](const std::string& message) { messages.push_back(message); });
    };

    check(single_instance::handoff(""), "empty handoff is delivered");
    drain();
    check(messages.size() == 1 && messages[0].empty(), "raise request arrives");

    const std::string apk = "/tmp/Example App.apk";
    check(single_instance::handoff(apk), "APK handoff is delivered");
    drain();
    check(messages.size() == 1 && messages[0] == apk, "APK path round-trips exactly");

    check(single_instance::handoff("a"), "back-to-back handoff is delivered");
    check(single_instance::handoff("b"), "second back-to-back handoff is delivered");
    drain();
    check(messages.size() == 2 && messages[0] == "a" && messages[1] == "b",
          "queued datagrams are drained in order");

    drain();
    check(messages.empty(), "idle poll yields nothing");

    // Stale socket from a crash must be replaceable.
    server.stop();
    check(::access(path.c_str(), F_OK) != 0, "stop removes the socket");
    check(!single_instance::handoff("x", 1), "handoff fails when no primary listens");
    {
        FILE* stale = std::fopen(path.c_str(), "w");
        if (stale) std::fclose(stale);
    }
    check(server.start(), "server replaces a stale socket file");
    check(single_instance::handoff("after-restart"), "handoff works after restart");
    drain();
    check(messages.size() == 1 && messages[0] == "after-restart",
          "message after restart arrives");

    server.stop();
    fs::remove_all(root, ec);
    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES: " + std::to_string(g_failures))
              << "\n";
    return g_failures == 0 ? 0 : 1;
}
