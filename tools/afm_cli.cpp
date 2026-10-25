// afm-cli: headless exerciser for the Linux transfer core (Stage A).
//
// Safety contract (AGENTS.md): read-only by default. The only device writes
// in Stage A are `mkdir` (explicit user path) and the server push/forward
// that `startServer` performs. This tool NEVER sends CMD_QUIT, never runs
// killall, and never removes a forward port it did not create: it marks
// itself non-owning (setOwnsServer(false)) and cleans up only its own
// forward on tcp:5741 — a port the Windows GUI never uses.
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "device_client.h"
#include "log_facade.h"

namespace {
constexpr int kCliLocalPort = 5741;  // GUI uses AFM_PORT; never collide with it
bool g_preferDirect = false;        // --direct: reconnect over WiFi instead of ADB forward

void usage() {
    std::puts("usage: afm-cli [-s SERIAL] <command> [args]\n"
              "\n"
              "  devices                 list adb devices (read-only)\n"
              "  list [REMOTE_PATH]      browse a device directory (default: storage root)\n"
              "  storage                 print the device storage root (read-only)\n"
              "  diskspace               print total/free bytes (read-only)\n"
              "  size REMOTE_PATH        stat a remote file (read-only)\n"
              "  mkdir REMOTE_PATH       create a remote directory (explicit write)\n"
              "  apps                    list installed packages (pushes+removes a tmp dex)\n"
              "  pull REMOTE LOCAL       download a file (verifies inline CRC state)\n"
              "  push LOCAL REMOTE       upload a file\n"
              "  mpull REMOTE LOCAL [N]  download over N parallel connections (default 4)\n"
              "  mpush LOCAL REMOTE [N]  upload over N parallel connections (default 4)\n"
              "  resume-pull REMOTE LOCAL  resume an interrupted download\n"
              "  resume-push LOCAL REMOTE  resume an interrupted upload\n"
              "  crc REMOTE LOCAL        compare remote vs local CRC32 (read-only)\n"
              "  range REMOTE OFF LEN LOCAL  fetch a byte range into LOCAL (read-only)\n"
              "  relay SRC DST           device-to-device copy, no disk IO (scratch only)\n"
              "  archive REMOTE LOCAL_TAR  tar a remote dir to a local file (read-only src)\n"
              "  extract LOCAL_TAR REMOTE_TARGET  push a tar and unpack it (scratch only)\n"
              "  mcraw-list MCRAW_PATH    list virtual DNG/WAV entries (read-only)\n"
              "\n"
              "All transfer tests must target a scratch dir (e.g. /sdcard/FastEnoughTest).");
}

// Connect flow shared by every device command: findAdb -> startServer.
// Returns false with lastError logged.
bool connectClient(DeviceClient& client, const std::string& serial) {
    if (!client.findAdb()) {
        LOG_ERROR("CLI", "adb not found (PATH, ~/Android/Sdk, or beside the binary)");
        return false;
    }
    client.setLocalPort(kCliLocalPort);
    client.setOwnsServer(false);  // never QUIT/killall on exit
    if (!client.startServer(serial)) {
        LOG_ERROR("CLI", "startServer failed: " + client.lastError());
        return false;
    }
    if (g_preferDirect) {
        // WiFi serials are host:port; USB/mDNS serials fall back to detection.
        const size_t colon = serial.rfind(':');
        std::string ip = (colon != std::string::npos) ? serial.substr(0, colon) : std::string();
        if (ip.empty()) ip = client.detectDeviceIp(serial);
        // The helper runs on our local port, not the GUI's 5740.
        if (ip.empty() || !client.connectDirectForSerial(serial, ip, kCliLocalPort))
            LOG_WARN("CLI", "direct TCP unavailable; keeping the ADB forward");
        else
            LOG_INFO("CLI", "direct TCP to " + ip + ":" + std::to_string(kCliLocalPort));
    }
    return true;
}

void cleanupForward(DeviceClient& client, const std::string& serial) {
    // Remove ONLY our own forward port. No CMD_QUIT, no killall — the device
    // server stays up for other sessions.
    client.disconnectTcp();
    client.runAdbCommand("-s " + serial + " forward --remove tcp:" +
                         std::to_string(kCliLocalPort));
}

std::string pickSerial(DeviceClient& client, const std::string& flag) {
    if (!flag.empty()) return flag;
    const std::vector<DeviceInfo> devices = client.getDevices();
    for (const auto& d : devices)
        if (d.state == "device") return d.serial;
    return {};
}

int cmdDevices(DeviceClient& client) {
    if (!client.findAdb()) {
        LOG_ERROR("CLI", "adb not found");
        return 1;
    }
    const std::vector<DeviceInfo> devices = client.getDevices();
    for (const auto& d : devices)
        std::printf("%-22s %-8s model=%s transport=%llu\n", d.serial.c_str(), d.state.c_str(),
                    d.model.c_str(), static_cast<unsigned long long>(d.transportId));
    std::printf("%zu device(s)\n", devices.size());
    return 0;
}

int cmdList(DeviceClient& client, const std::string& serial, const std::string& pathArg) {
    if (!connectClient(client, serial)) return 1;
    const std::string path = pathArg.empty() ? client.detectStoragePath() : pathArg;
    std::printf("listing: %s  (via %s)\n", path.c_str(),
                client.isDirectConnection() ? "direct" : "adb-forward");
    const std::vector<DeviceFileEntry> entries = client.listDirectory(path);
    for (const auto& e : entries)
        std::printf("%c %12llu  %s\n", e.isDirectory() ? 'd' : (e.isSymlink() ? 'l' : 'f'),
                    static_cast<unsigned long long>(e.size), e.name.c_str());
    std::printf("%zu entrie(s)\n", entries.size());
    const bool connected = client.isServerRunning();
    cleanupForward(client, serial);
    return connected ? 0 : 1;
}

int cmdStorage(DeviceClient& client, const std::string& serial) {
    if (!connectClient(client, serial)) return 1;
    std::printf("%s\n", client.detectStoragePath().c_str());
    cleanupForward(client, serial);
    return 0;
}

int cmdDiskspace(DeviceClient& client, const std::string& serial) {
    if (!connectClient(client, serial)) return 1;
    uint64_t total = 0, free = 0;
    if (!client.getDiskSpace(total, free)) {
        LOG_ERROR("CLI", "diskspace failed: " + client.lastError());
        cleanupForward(client, serial);
        return 1;
    }
    std::printf("total=%llu free=%llu\n", static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(free));
    cleanupForward(client, serial);
    return 0;
}

int cmdSize(DeviceClient& client, const std::string& serial, const std::string& path) {
    if (!connectClient(client, serial)) return 1;
    std::printf("%llu\n", static_cast<unsigned long long>(client.getFileSize(path)));
    cleanupForward(client, serial);
    return 0;
}

int cmdMkdir(DeviceClient& client, const std::string& serial, const std::string& path) {
    if (!connectClient(client, serial)) return 1;
    if (!client.createDirectory(path)) {
        LOG_ERROR("CLI", "mkdir failed: " + client.lastError());
        cleanupForward(client, serial);
        return 1;
    }
    std::printf("created: %s\n", path.c_str());
    cleanupForward(client, serial);
    return 0;
}

int cmdApps(DeviceClient& client, const std::string& serial) {
    if (!client.findAdb()) {
        LOG_ERROR("CLI", "adb not found");
        return 1;
    }
    const std::vector<InstalledAppEntry> apps = client.listInstalledApps(serial, false);
    for (const auto& a : apps)
        std::printf("%s  %s\n", a.packageName.c_str(),
                    a.appName.empty() ? a.apkPath.c_str() : a.appName.c_str());
    std::printf("%zu app(s)\n", apps.size());
    return 0;
}

bool cliProgress(uint64_t done, uint64_t total) {
    if (total == 0) return true;
    std::fprintf(stderr, "\r  %llu / %llu bytes (%.1f%%)", static_cast<unsigned long long>(done),
                 static_cast<unsigned long long>(total),
                 100.0 * static_cast<double>(done) / static_cast<double>(total));
    return true;
}

int cmdPull(DeviceClient& client, const std::string& serial, const std::string& remote,
            const std::string& local) {
    if (!connectClient(client, serial)) return 1;
    uint64_t size = 0;
    const bool ok = client.pullFile(remote, local, size, cliProgress);
    std::fputs("\n", stderr);
    if (!ok) {
        LOG_ERROR("CLI", "pull failed: " + client.lastError());
        cleanupForward(client, serial);
        return 1;
    }
    std::printf("pulled %llu bytes -> %s\n", static_cast<unsigned long long>(size), local.c_str());
    cleanupForward(client, serial);
    return 0;
}

int cmdPush(DeviceClient& client, const std::string& serial, const std::string& local,
            const std::string& remote) {
    if (!connectClient(client, serial)) return 1;
    std::error_code ec;
    const uint64_t size = static_cast<uint64_t>(std::filesystem::file_size(local, ec));
    if (ec) {
        LOG_ERROR("CLI", "cannot stat local file: " + local);
        return 1;
    }
    if (!client.pushFile(local, remote, size, cliProgress)) {
        std::fputs("\n", stderr);
        LOG_ERROR("CLI", "push failed: " + client.lastError());
        cleanupForward(client, serial);
        return 1;
    }
    std::fputs("\n", stderr);
    std::printf("pushed %llu bytes -> %s\n", static_cast<unsigned long long>(size), remote.c_str());
    cleanupForward(client, serial);
    return 0;
}

struct Slice {
    uint64_t offset = 0;
    uint64_t length = 0;
};

std::vector<Slice> splitRange(uint64_t total, int count) {
    std::vector<Slice> slices;
    const uint64_t per = total / static_cast<uint64_t>(count);
    const uint64_t remainder = total % static_cast<uint64_t>(count);
    uint64_t offset = 0;
    for (int i = 0; i < count; ++i) {
        const uint64_t length = per + (static_cast<uint64_t>(i) < remainder ? 1 : 0);
        if (length == 0) continue;
        slices.push_back({offset, length});
        offset += length;
    }
    return slices;
}

int parseChannels(const std::vector<std::string>& args, size_t index) {
    if (args.size() <= index) return 4;  // matches the GUI's default WiFi pipe count
    int channels = std::atoi(args[index].c_str());
    if (channels < 1) channels = 1;
    if (channels > 4) channels = 4;
    return channels;
}

int cmdMultiPull(DeviceClient& client, const std::string& serial, const std::string& remote,
                 const std::string& local, int channels) {
    if (!connectClient(client, serial)) return 1;
    const uint64_t total = client.getFileSize(remote);
    if (total == 0) {
        LOG_ERROR("CLI", "cannot determine remote size (empty file or stat failed): " + remote);
        cleanupForward(client, serial);
        return 1;
    }
    int fd = ::open(local.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        LOG_ERROR("CLI", "cannot create local file: " + local);
        cleanupForward(client, serial);
        return 1;
    }
    if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
        LOG_ERROR("CLI", "cannot size local file: " + local);
        ::close(fd);
        cleanupForward(client, serial);
        return 1;
    }

    std::vector<DeviceClient*> workers{&client};
    std::vector<std::unique_ptr<DeviceClient>> extras;
    for (int i = 1; i < channels; ++i) {
        auto extra = std::make_unique<DeviceClient>();
        if (!extra->connectParallelFrom(client)) {
            LOG_ERROR("CLI", "parallel connect failed for channel " + std::to_string(i + 1));
            ::close(fd);
            cleanupForward(client, serial);
            return 1;
        }
        extras.push_back(std::move(extra));
        workers.push_back(extras.back().get());
    }

    const std::vector<Slice> slices = splitRange(total, static_cast<int>(workers.size()));
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> moved{0};
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (size_t i = 0; i < slices.size(); ++i) {
        threads.emplace_back([&, i]() {
            DeviceClient* channel = workers[i];
            const Slice slice = slices[i];
            const uint64_t got = channel->readRangeStreaming(
                remote, slice.offset, slice.length,
                [&](const void* data, uint32_t len, uint64_t offset) {
                    if (::pwrite(fd, data, len, static_cast<off_t>(offset)) !=
                        static_cast<ssize_t>(len)) {
                        failed = true;
                        return false;
                    }
                    moved += len;
                    return true;
                });
            if (got < slice.length) failed = true;
        });
    }
    for (auto& thread : threads) thread.join();
    ::close(fd);
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    if (failed) {
        LOG_ERROR("CLI", "mpull failed: " + client.lastError());
        cleanupForward(client, serial);
        return 1;
    }

    std::string detail;
    const bool match = client.verifyFileCrc(remote, local, detail);
    std::printf("mpull %llu bytes in %.2fs (%.2f MB/s, %zu channels) CRC %s: %s\n",
                static_cast<unsigned long long>(moved.load()), seconds,
                static_cast<double>(total) / 1048576.0 / seconds,
                slices.size(), match ? "MATCH" : "MISMATCH", detail.c_str());
    cleanupForward(client, serial);
    return match ? 0 : 1;
}

int cmdMultiPush(DeviceClient& client, const std::string& serial, const std::string& local,
                 const std::string& remote, int channels) {
    if (!connectClient(client, serial)) return 1;
    std::error_code ec;
    const uint64_t total = static_cast<uint64_t>(std::filesystem::file_size(local, ec));
    if (ec) {
        LOG_ERROR("CLI", "cannot stat local file: " + local);
        cleanupForward(client, serial);
        return 1;
    }
    if (!client.createFile(remote, total)) {
        LOG_ERROR("CLI", "cannot create remote file: " + client.lastError());
        cleanupForward(client, serial);
        return 1;
    }

    std::vector<DeviceClient*> workers{&client};
    std::vector<std::unique_ptr<DeviceClient>> extras;
    for (int i = 1; i < channels; ++i) {
        auto extra = std::make_unique<DeviceClient>();
        if (!extra->connectParallelFrom(client)) {
            LOG_ERROR("CLI", "parallel connect failed for channel " + std::to_string(i + 1));
            cleanupForward(client, serial);
            return 1;
        }
        extras.push_back(std::move(extra));
        workers.push_back(extras.back().get());
    }

    const std::vector<Slice> slices = splitRange(total, static_cast<int>(workers.size()));
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> moved{0};
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (size_t i = 0; i < slices.size(); ++i) {
        threads.emplace_back([&, i]() {
            const Slice slice = slices[i];
            auto progress = [&](uint64_t done, uint64_t) {
                moved += done;
                return !failed.load();
            };
            const uint64_t written = workers[i]->writeRangeStreaming(
                remote, slice.offset, local, slice.offset, slice.length, progress);
            if (written < slice.length) failed = true;
        });
    }
    for (auto& thread : threads) thread.join();
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    if (failed) {
        LOG_ERROR("CLI", "mpush failed: " + client.lastError());
        cleanupForward(client, serial);
        return 1;
    }

    std::string detail;
    const bool match = client.verifyFileCrc(remote, local, detail);
    std::printf("mpush %llu bytes in %.2fs (%.2f MB/s, %zu channels) CRC %s: %s\n",
                static_cast<unsigned long long>(total), seconds,
                static_cast<double>(total) / 1048576.0 / seconds,
                slices.size(), match ? "MATCH" : "MISMATCH", detail.c_str());
    cleanupForward(client, serial);
    return match ? 0 : 1;
}

int cmdCrc(DeviceClient& client, const std::string& serial, const std::string& remote,
           const std::string& local) {
    if (!connectClient(client, serial)) return 1;
    std::string detail;
    const bool match = client.verifyFileCrc(remote, local, detail);
    std::printf("%s %s\n", match ? "MATCH" : "MISMATCH", detail.c_str());
    cleanupForward(client, serial);
    return match ? 0 : 1;
}

int cmdResumePull(DeviceClient& client, const std::string& serial, const std::string& remote,
                  const std::string& local) {
    if (!connectClient(client, serial)) return 1;
    uint64_t size = 0;
    const bool ok = client.resumePullFile(remote, local, size, cliProgress);
    std::fputs("\n", stderr);
    if (!ok) {
        LOG_ERROR("CLI", "resume-pull failed: " + client.lastError());
        cleanupForward(client, serial);
        return 1;
    }
    std::printf("resume-pulled %llu bytes -> %s\n", static_cast<unsigned long long>(size),
                local.c_str());
    cleanupForward(client, serial);
    return 0;
}

int cmdResumePush(DeviceClient& client, const std::string& serial, const std::string& local,
                  const std::string& remote) {
    if (!connectClient(client, serial)) return 1;
    std::error_code ec;
    const uint64_t size = static_cast<uint64_t>(std::filesystem::file_size(local, ec));
    if (ec) {
        LOG_ERROR("CLI", "cannot stat local file: " + local);
        return 1;
    }
    if (!client.resumePushFile(local, remote, size, cliProgress)) {
        std::fputs("\n", stderr);
        LOG_ERROR("CLI", "resume-push failed: " + client.lastError());
        cleanupForward(client, serial);
        return 1;
    }
    std::fputs("\n", stderr);
    std::printf("resume-pushed %llu bytes -> %s\n", static_cast<unsigned long long>(size),
                remote.c_str());
    cleanupForward(client, serial);
    return 0;
}

int cmdRange(DeviceClient& client, const std::string& serial, const std::string& remote,
             uint64_t offset, uint64_t length, const std::string& local) {
    if (!connectClient(client, serial)) return 1;
    FILE* out = std::fopen(local.c_str(), "wb");
    if (!out) {
        LOG_ERROR("CLI", "cannot create local file: " + local);
        return 1;
    }
    uint64_t delivered = 0;
    const uint64_t got = client.readRangeStreaming(
        remote, offset, length,
        [&](const void* data, uint32_t len, uint64_t chunkOffset) {
            if (std::fwrite(data, 1, len, out) != len) return false;
            delivered += len;
            (void)chunkOffset;
            return cliProgress(offset + delivered, offset + length);
        });
    std::fclose(out);
    std::fputs("\n", stderr);
    std::printf("range GOT %llu bytes -> %s\n", static_cast<unsigned long long>(got), local.c_str());
    cleanupForward(client, serial);
    return got > 0 ? 0 : 1;
}

int cmdRelay(const std::string& serial, const std::string& src, const std::string& dst) {
    DeviceClient srcClient;
    if (!connectClient(srcClient, serial)) return 1;
    DeviceClient dstClient;
    if (!dstClient.connectParallelFrom(srcClient)) {
        LOG_ERROR("CLI", "parallel connect failed");
        cleanupForward(srcClient, serial);
        return 1;
    }
    const bool ok = DeviceClient::relayFile(srcClient, dstClient, src, dst, 0, cliProgress);
    std::fputs("\n", stderr);
    std::printf("%s relay %s -> %s\n", ok ? "OK" : "FAIL", src.c_str(), dst.c_str());
    dstClient.disconnectTcp();
    cleanupForward(srcClient, serial);
    return ok ? 0 : 1;
}

int cmdArchive(DeviceClient& client, const std::string& serial, const std::string& remote,
               const std::string& local) {
    if (!connectClient(client, serial)) return 1;
    uint64_t size = 0;
    const bool ok = client.archiveRemoteDirectory(remote, local, true, size, cliProgress);
    std::fputs("\n", stderr);
    if (!ok) {
        LOG_ERROR("CLI", "archive failed: " + client.lastError());
        cleanupForward(client, serial);
        return 1;
    }
    std::printf("archived %llu bytes -> %s\n", static_cast<unsigned long long>(size), local.c_str());
    cleanupForward(client, serial);
    return 0;
}

int cmdExtract(DeviceClient& client, const std::string& serial, const std::string& local,
               const std::string& target) {
    if (!connectClient(client, serial)) return 1;
    if (!client.extractRemoteArchive(local, target, cliProgress)) {
        std::fputs("\n", stderr);
        LOG_ERROR("CLI", "extract failed: " + client.lastError());
        cleanupForward(client, serial);
        return 1;
    }
    std::fputs("\n", stderr);
    std::printf("extracted %s -> %s\n", local.c_str(), target.c_str());
    cleanupForward(client, serial);
    return 0;
}

int cmdMcrawList(DeviceClient& client, const std::string& serial, const std::string& path) {
    if (!connectClient(client, serial)) return 1;
    const std::vector<DeviceFileEntry> entries = client.listMcraw(path);
    for (const auto& e : entries)
        std::printf("%c %12llu  %s\n", e.isDirectory() ? 'd' : 'f',
                    static_cast<unsigned long long>(e.size), e.name.c_str());
    std::printf("%zu entrie(s)\n", entries.size());
    if (!entries.empty()) {
        // Error text when the server rejected the path.
        std::printf("lastError: %s\n", client.lastError().c_str());
    }
    cleanupForward(client, serial);
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);  // sends report errors; a dead peer must not kill us
    LinuxLog::instance().setMinLevel(LinuxLogLevel::Info);

    std::string serial;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-s" && i + 1 < argc) {
            serial = argv[++i];
        } else if (std::string(argv[i]) == "--direct") {
            g_preferDirect = true;
        } else {
            args.emplace_back(argv[i]);
        }
    }
    if (args.empty()) {
        usage();
        return 2;
    }

    DeviceClient client;
    const std::string cmd = args[0];
    if (cmd == "devices") return cmdDevices(client);
    if (cmd == "list") {
        const std::string resolved =
            serial.empty() ? pickSerial(client, serial) : serial;
        if (resolved.empty()) {
            LOG_ERROR("CLI", "no connected device");
            return 1;
        }
        return cmdList(client, resolved, args.size() > 1 ? args[1] : "");
    }
    if (cmd == "storage" || cmd == "diskspace" || cmd == "size" || cmd == "mkdir" ||
        cmd == "apps") {
        const std::string resolved = serial.empty() ? pickSerial(client, serial) : serial;
        if (resolved.empty()) {
            LOG_ERROR("CLI", "no connected device (use -s SERIAL)");
            return 1;
        }
        if (cmd == "storage") return cmdStorage(client, resolved);
        if (cmd == "diskspace") return cmdDiskspace(client, resolved);
        if (cmd == "apps") return cmdApps(client, resolved);
        if (args.size() < 2) {
            usage();
            return 2;
        }
        if (cmd == "size") return cmdSize(client, resolved, args[1]);
        return cmdMkdir(client, resolved, args[1]);
    }
    if (cmd == "pull" || cmd == "push" || cmd == "mpull" || cmd == "mpush" ||
        cmd == "crc" || cmd == "resume-pull" || cmd == "resume-push") {
        const std::string resolved = serial.empty() ? pickSerial(client, serial) : serial;
        if (resolved.empty()) {
            LOG_ERROR("CLI", "no connected device (use -s SERIAL)");
            return 1;
        }
        if (args.size() < 3) {
            usage();
            return 2;
        }
        if (cmd == "pull") return cmdPull(client, resolved, args[1], args[2]);
        if (cmd == "push") return cmdPush(client, resolved, args[1], args[2]);
        if (cmd == "mpull") return cmdMultiPull(client, resolved, args[1], args[2],
                                                parseChannels(args, 3));
        if (cmd == "mpush") return cmdMultiPush(client, resolved, args[1], args[2],
                                                parseChannels(args, 3));
        if (cmd == "resume-pull") return cmdResumePull(client, resolved, args[1], args[2]);
        if (cmd == "resume-push") return cmdResumePush(client, resolved, args[1], args[2]);
        return cmdCrc(client, resolved, args[1], args[2]);
    }
    if (cmd == "range") {
        const std::string resolved = serial.empty() ? pickSerial(client, serial) : serial;
        if (resolved.empty()) {
            LOG_ERROR("CLI", "no connected device (use -s SERIAL)");
            return 1;
        }
        if (args.size() < 5) {
            usage();
            return 2;
        }
        return cmdRange(client, resolved, args[1], std::stoull(args[2]),
                        std::stoull(args[3]), args[4]);
    }
    if (cmd == "relay" || cmd == "archive" || cmd == "extract") {
        const std::string resolved = serial.empty() ? pickSerial(client, serial) : serial;
        if (resolved.empty()) {
            LOG_ERROR("CLI", "no connected device (use -s SERIAL)");
            return 1;
        }
        if (args.size() < 3) {
            usage();
            return 2;
        }
        if (cmd == "relay") return cmdRelay(resolved, args[1], args[2]);
        if (cmd == "archive") return cmdArchive(client, resolved, args[1], args[2]);
        return cmdExtract(client, resolved, args[1], args[2]);
    }
    if (cmd == "mcraw-list") {
        const std::string resolved = serial.empty() ? pickSerial(client, serial) : serial;
        if (resolved.empty()) {
            LOG_ERROR("CLI", "no connected device (use -s SERIAL)");
            return 1;
        }
        if (args.size() < 2) {
            usage();
            return 2;
        }
        return cmdMcrawList(client, resolved, args[1]);
    }
    usage();
    return 2;
}
