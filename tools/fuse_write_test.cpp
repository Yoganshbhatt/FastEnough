// End-to-end FUSE writable-mount test against a local AFM server.
// Never talks to a phone: the caller starts server/afm-server on 127.0.0.1
// with EXTERNAL_STORAGE pointing at a scratch directory.
#include "device_client.h"
#include "fuse_common.h"
#include "mount_stubs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool writeFile(const std::string& path, const std::string& data, int flags = 0) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | flags, 0644);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t wrote = ::write(fd, data.data() + done, data.size() - done);
        if (wrote <= 0) {
            ::close(fd);
            return false;
        }
        done += static_cast<size_t>(wrote);
    }
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

std::string readFile(const std::string& path, bool& ok) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        ok = false;
        return {};
    }
    ok = true;
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

int fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s (errno=%d %s)\n", message, errno,
                 std::strerror(errno));
    return 1;
}

// Creates `dir`, exercises every writable operation inside it, and removes it
// again. The parent directory must exist; `dir` itself must not.
int runChecks(const std::string& dir) {
    if (::mkdir(dir.c_str(), 0755) != 0) return fail("mkdir");

    const std::string file = dir + "/payload.bin";
    std::string payload(1024 * 1024 + 17, '\0');
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i * 31u) & 0xffu);
    }
    if (!writeFile(file, payload, O_EXCL)) return fail("create+write");

    struct stat st{};
    if (::stat(file.c_str(), &st) != 0 ||
        st.st_size != static_cast<off_t>(payload.size())) {
        return fail("stat after write");
    }

    bool ok = false;
    const std::string roundTrip = readFile(file, ok);
    if (!ok || roundTrip != payload) return fail("read back");

    // Exclusive create must refuse to overwrite an existing file.
    const int duplicate = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (duplicate >= 0) {
        ::close(duplicate);
        return fail("O_EXCL overwrote an existing file");
    }
    if (errno != EEXIST) return fail("O_EXCL returned the wrong errno");

    // Opening without O_TRUNC must preserve the rest of the file.
    const int overwriteFd = ::open(file.c_str(), O_WRONLY);
    if (overwriteFd < 0) return fail("open without O_TRUNC");
    if (::write(overwriteFd, "XYZ", 3) != 3) {
        ::close(overwriteFd);
        return fail("write without O_TRUNC");
    }
    ::close(overwriteFd);
    const std::string preserved = readFile(file, ok);
    if (!ok || preserved.size() != payload.size() || preserved.compare(0, 3, "XYZ") != 0 ||
        preserved.compare(3, std::string::npos, payload, 3, std::string::npos) != 0) {
        return fail("write without O_TRUNC changed the file size or tail");
    }

    // Truncate shorter, then extend; the extended range must read as zeros.
    if (::truncate(file.c_str(), 4096) != 0) return fail("truncate");
    if (::stat(file.c_str(), &st) != 0 || st.st_size != 4096) {
        return fail("stat after truncate");
    }
    const off_t extendedSize = 4096 + 64;
    if (::truncate(file.c_str(), extendedSize) != 0) return fail("truncate extend");
    if (::stat(file.c_str(), &st) != 0 || st.st_size != extendedSize) {
        return fail("stat after truncate extend");
    }
    const std::string extended = readFile(file, ok);
    if (!ok || extended.size() != static_cast<size_t>(extendedSize) ||
        extended.find_first_not_of('\0', 4096) != std::string::npos) {
        return fail("truncate extend did not zero-fill");
    }
    if (::truncate(file.c_str(), 4096) != 0) return fail("truncate back");

    const int appendFd = ::open(file.c_str(), O_WRONLY | O_APPEND);
    if (appendFd < 0) return fail("open for append");
    const std::string tail = "appended-after-truncate";
    if (::write(appendFd, tail.data(), tail.size()) !=
        static_cast<ssize_t>(tail.size())) {
        ::close(appendFd);
        return fail("append write");
    }
    ::close(appendFd);
    if (::stat(file.c_str(), &st) != 0 ||
        st.st_size != static_cast<off_t>(4096 + tail.size())) {
        return fail("stat after append");
    }

    // Rename, including POSIX replacement semantics.
    const std::string renamed = dir + "/renamed.bin";
    if (::rename(file.c_str(), renamed.c_str()) != 0) return fail("rename");
    if (::access(file.c_str(), F_OK) == 0) return fail("old name still exists");

    const std::string replacement = dir + "/replacement.tmp";
    if (!writeFile(replacement, "replacement")) return fail("replacement write");
    if (::rename(replacement.c_str(), renamed.c_str()) != 0) {
        return fail("rename replace");
    }
    if (readFile(renamed, ok) != "replacement" || !ok) {
        return fail("rename replace content");
    }

    // rmdir on a non-empty directory must fail and must not delete contents.
    if (::rmdir(dir.c_str()) == 0) return fail("rmdir deleted a non-empty directory");
    if (errno != ENOTEMPTY) return fail("rmdir returned the wrong errno");
    if (::access(renamed.c_str(), F_OK) != 0) {
        return fail("rmdir damaged the directory contents");
    }

    // Throughput: a 128 KiB-block writer (cp-style) and a bulk reader.
    const std::string bench = dir + "/bench.bin";
    {
        constexpr size_t kBlock = 128u * 1024u;
        constexpr size_t kBlocks = 64;  // 8 MiB
        const std::vector<char> block(kBlock, 'b');
        const int fd = ::open(bench.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return fail("open bench file");
        const auto started = std::chrono::steady_clock::now();
        for (size_t i = 0; i < kBlocks; ++i) {
            if (::write(fd, block.data(), block.size()) != static_cast<ssize_t>(kBlock)) {
                ::close(fd);
                return fail("bench write");
            }
        }
        if (::fsync(fd) != 0) { ::close(fd); return fail("bench fsync"); }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        ::close(fd);
        const double mb = (kBlock * kBlocks) / (1024.0 * 1024.0);
        std::printf("write 8 MiB in 128 KiB blocks: %lld ms (%.1f MB/s)\n",
                    static_cast<long long>(elapsed),
                    elapsed > 0 ? mb * 1000.0 / static_cast<double>(elapsed) : 0.0);
    }
    {
        const int fd = ::open(bench.c_str(), O_RDONLY);
        if (fd < 0) return fail("open bench for read");
        std::vector<char> buf(1024u * 1024u);
        size_t total = 0;
        const auto started = std::chrono::steady_clock::now();
        while (true) {
            const ssize_t got = ::read(fd, buf.data(), buf.size());
            if (got < 0) { ::close(fd); return fail("bench read"); }
            if (got == 0) break;
            total += static_cast<size_t>(got);
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        ::close(fd);
        const double mb = static_cast<double>(total) / (1024.0 * 1024.0);
        std::printf("read %zu bytes: %lld ms (%.1f MB/s)\n", total,
                    static_cast<long long>(elapsed),
                    elapsed > 0 ? mb * 1000.0 / static_cast<double>(elapsed) : 0.0);
    }
    if (::unlink(bench.c_str()) != 0) return fail("unlink bench");

    // touch(1)/cp -p parity: timestamps are accepted silently (Dokan does the
    // same in FsSetFileTime) instead of failing with ENOSYS.
    if (::utimensat(AT_FDCWD, renamed.c_str(), nullptr, 0) != 0) {
        return fail("utimens on existing file");
    }

    // Delete the file, then the directory.
    if (::unlink(renamed.c_str()) != 0) return fail("unlink");
    if (::rmdir(dir.c_str()) != 0) return fail("rmdir");
    if (::access(dir.c_str(), F_OK) == 0) {
        std::fprintf(stderr, "FAIL: directory survived rmdir\n");
        return 1;
    }
    return 0;
}

// Host mode: an afm-server already listening on 127.0.0.1:5740 with
// EXTERNAL_STORAGE pointing at the directory passed as <remote-root>.
int runHostMode(const std::string& root, const std::string& mountPoint) {
    DeviceClient client;
    if (!client.connectDirect("127.0.0.1")) {
        std::fprintf(stderr, "connect failed: %s\n", client.lastError().c_str());
        return 1;
    }
    if ((client.capabilities() & AFM_CAP_FUSE_WRITE) == 0) {
        std::fprintf(stderr, "server does not advertise FUSE write support\n");
        return 1;
    }

    auto& manager = DeviceMountManager::instance(0);
    if (!manager.mount(&client, root, mountPoint, true)) {
        std::fprintf(stderr, "writable mount failed; see log\n");
        return 1;
    }
    const int rc = runChecks(mountPoint + "/newdir");
    manager.unmount();
    if (rc != 0) return rc;
    if (::access((mountPoint + "/newdir").c_str(), F_OK) == 0) {
        std::fprintf(stderr, "FAIL: directory survived unmount\n");
        return 1;
    }
    std::printf("FUSE writable-mount test passed\n");
    return 0;
}

// Device mode: deploys the helper to the phone, mounts the storage root
// read-write, runs every check inside /sdcard/FastEnoughTest, then removes
// the scratch path, the forward and the helper it started.
int runDeviceMode(const std::string& serial, const std::string& mountPoint) {
    DeviceClient client;
    if (!client.findAdb()) {
        std::fprintf(stderr, "FAIL: adb not found\n");
        return 1;
    }
    client.setLocalPort(5742);
    client.setOwnsServer(false);
    if (!client.startServer(serial)) {
        std::fprintf(stderr, "FAIL: startServer: %s\n", client.lastError().c_str());
        return 1;
    }
    const std::string root = client.detectStoragePath();
    if (root.empty()) {
        std::fprintf(stderr, "FAIL: storage path unknown\n");
        client.runAdbCommand("-s " + serial + " forward --remove tcp:5742");
        return 1;
    }
    if ((client.capabilities() & AFM_CAP_FUSE_WRITE) == 0) {
        std::fprintf(stderr, "FAIL: deployed helper predates the write protocol\n");
        client.runAdbCommand("-s " + serial + " forward --remove tcp:5742");
        return 1;
    }

    const std::string scratchParent = "/sdcard/FastEnoughTest";
    const std::string scratch = scratchParent + "/fuse-write-live";
    client.runAdbCommand("-s " + serial + " shell mkdir -p " + scratchParent);

    auto& manager = DeviceMountManager::instance(0);
    if (!manager.mount(&client, root, mountPoint, true)) {
        std::fprintf(stderr, "FAIL: writable mount failed; see log\n");
        client.runAdbCommand("-s " + serial + " shell rm -rf " + scratch);
        client.runAdbCommand("-s " + serial + " forward --remove tcp:5742");
        return 1;
    }

    const int rc = runChecks(mountPoint + "/FastEnoughTest/fuse-write-live");
    manager.unmount();
    client.runAdbCommand("-s " + serial + " shell rm -rf " + scratch);
    client.runAdbCommand("-s " + serial + " shell rmdir " + scratchParent + " 2>/dev/null");
    client.runAdbCommand("-s " + serial + " shell killall afm-server 2>/dev/null");
    client.runAdbCommand("-s " + serial + " forward --remove tcp:5742");
    if (rc != 0) return rc;
    std::printf("FUSE live writable-mount test passed\n");
    return 0;
}

// Attach mode: use a helper a GUI session already runs (port 5740) through a
// separate adb forward. Never pushes, never kills, never removes the GUI's
// forward; the mount is a second, independent view of the same phone.
int runAttachMode(const std::string& serial, int port, const std::string& mountPoint) {
    DeviceClient client;
    if (!client.findAdb()) {
        std::fprintf(stderr, "FAIL: adb not found\n");
        return 1;
    }
    client.setLocalPort(port);
    client.setOwnsServer(false);
    const std::string forwardPort = std::to_string(port);
    client.runAdbCommand("-s " + serial + " forward --remove tcp:" + forwardPort);
    client.runAdbCommand("-s " + serial + " forward tcp:" + forwardPort + " tcp:5740");
    if (!client.connectLocalPort(port)) {
        std::fprintf(stderr, "FAIL: cannot attach to the running helper: %s\n",
                     client.lastError().c_str());
        client.runAdbCommand("-s " + serial + " forward --remove tcp:" + forwardPort);
        return 1;
    }
    if ((client.capabilities() & AFM_CAP_FUSE_WRITE) == 0) {
        std::fprintf(stderr, "FAIL: the running helper predates the write protocol\n");
        client.runAdbCommand("-s " + serial + " forward --remove tcp:" + forwardPort);
        return 1;
    }
    const std::string root = client.detectStoragePath();
    if (root.empty()) {
        std::fprintf(stderr, "FAIL: storage path unknown\n");
        client.runAdbCommand("-s " + serial + " forward --remove tcp:" + forwardPort);
        return 1;
    }

    auto& manager = DeviceMountManager::instance(0);
    if (!manager.mount(&client, root, mountPoint, true)) {
        std::fprintf(stderr, "FAIL: writable mount failed; see log\n");
        client.runAdbCommand("-s " + serial + " forward --remove tcp:" + forwardPort);
        return 1;
    }
    const std::string scratch = mountPoint + "/FastEnoughTest/afm-rw-probe";
    const int rc = runChecks(scratch);
    manager.unmount();
    client.runAdbCommand("-s " + serial + " shell rm -rf /sdcard/FastEnoughTest/afm-rw-probe");
    client.runAdbCommand("-s " + serial + " forward --remove tcp:" + forwardPort);
    if (rc != 0) return rc;
    std::printf("FUSE attached writable-mount test passed\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::strcmp(argv[1], "--attach") == 0) {
        return runAttachMode(argv[2], std::atoi(argv[3]), argv[4]);
    }
    if (argc == 4 && std::strcmp(argv[1], "--device") == 0) {
        return runDeviceMode(argv[2], argv[3]);
    }
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: %s <remote-root> <mount-point>\n"
                     "       %s --device <serial> <mount-point>\n"
                     "       %s --attach <serial> <local-port> <mount-point>\n",
                     argv[0], argv[0], argv[0]);
        return 2;
    }
    return runHostMode(argv[1], argv[2]);
}
