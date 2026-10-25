// End-to-end FUSE device-mount test against a locally running AFM server.
// This never talks to a phone: the caller starts server/afm-server on
// 127.0.0.1 with EXTERNAL_STORAGE pointing at a scratch directory.
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "device_client.h"
#include "fuse_common.h"
#include "mount_stubs.h"

namespace {

std::string readFile(const std::string& path, bool& ok) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        ok = false;
        return {};
    }
    const std::string data((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    ok = true;
    return data;
}

// Live read-only mount against a phone. Uses its own adb forward (5742), never
// sends CMD_QUIT, and removes the forward on exit. The caller supplies a file
// path relative to the storage root and an output file for the read bytes.
int runDeviceMode(const std::string& serial, const std::string& mountPoint,
                  const std::string& remoteRel, const std::string& outFile) {
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
        return 1;
    }
    std::printf("device root: %s\n", root.c_str());

    auto& manager = DeviceMountManager::instance(0);
    if (!manager.mount(&client, root, mountPoint)) {
        std::fprintf(stderr, "FAIL: mount failed (see log)\n");
        return 1;
    }

    {
        DIR* dir = ::opendir(mountPoint.c_str());
        if (!dir) {
            std::fprintf(stderr, "FAIL: cannot open the mount root\n");
            manager.unmount();
            return 1;
        }
        size_t entries = 0;
        std::string first;
        while (struct dirent* entry = ::readdir(dir)) {
            if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
                continue;
            if (first.empty()) first = entry->d_name;
            ++entries;
        }
        ::closedir(dir);
        if (entries == 0) {
            std::fprintf(stderr, "FAIL: mount root is empty\n");
            manager.unmount();
            return 1;
        }
        std::printf("root entries: %zu (first: %s)\n", entries, first.c_str());
    }

    bool ok = false;
    const std::string content = readFile(mountPoint + "/" + remoteRel, ok);
    if (!ok) {
        std::fprintf(stderr, "FAIL: cannot read %s through the mount\n", remoteRel.c_str());
        manager.unmount();
        return 1;
    }
    {
        std::ofstream out(outFile, std::ios::binary);
        if (!out) {
            std::fprintf(stderr, "FAIL: cannot write %s\n", outFile.c_str());
            manager.unmount();
            return 1;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    std::printf("read %zu bytes via FUSE\n", content.size());

    struct statvfs vfs{};
    if (::statvfs(mountPoint.c_str(), &vfs) != 0 || vfs.f_blocks == 0) {
        std::fprintf(stderr, "FAIL: statfs through the mount\n");
        manager.unmount();
        return 1;
    }
    std::printf("statfs: %llu blocks of %lu bytes\n",
                static_cast<unsigned long long>(vfs.f_blocks),
                static_cast<unsigned long>(vfs.f_bsize));

    errno = 0;
    const int writeFd = ::open((mountPoint + "/" + remoteRel).c_str(), O_WRONLY);
    if (writeFd >= 0) {
        ::close(writeFd);
        std::fprintf(stderr, "FAIL: read-only mount accepted O_WRONLY\n");
        manager.unmount();
        return 1;
    }
    if (errno != EROFS) {
        std::fprintf(stderr, "FAIL: write open errno=%d (want EROFS)\n", errno);
        manager.unmount();
        return 1;
    }

    manager.unmount();
    if (::access(mountPoint.c_str(), F_OK) == 0 || manager.isMounted()) {
        std::fprintf(stderr, "FAIL: mount point survived unmount\n");
        return 1;
    }

    // A helper without the write protocol must refuse a writable mount instead
    // of exposing writes that would fail mid-operation.
    const std::string writeMount = mountPoint + "-rw";
    if (manager.mount(&client, root, writeMount, true)) {
        std::fprintf(stderr, "FAIL: writable mount succeeded against an old helper\n");
        manager.unmount();
        return 1;
    }
    std::printf("writable mount correctly refused (helper capability)\n");

    client.runAdbCommand("-s " + serial + " forward --remove tcp:5742");
    std::printf("FUSE live read-only test passed\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 6 && std::strcmp(argv[1], "--device") == 0)
        return runDeviceMode(argv[2], argv[3], argv[4], argv[5]);
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: %s <remote-root> <mount-point> <expected>\n"
                     "       %s --device <serial> <mount-point> <remote-rel-path> <out>\n",
                     argv[0], argv[0]);
        return 2;
    }
    const std::string root = argv[1];
    const std::string mountPoint = argv[2];
    const std::string expected = argv[3];

    DeviceClient client;
    if (!client.connectDirect("127.0.0.1")) {
        std::fprintf(stderr, "connect failed: %s\n", client.lastError().c_str());
        return 1;
    }

    auto& manager = DeviceMountManager::instance(0);
    if (!manager.mount(&client, root, mountPoint)) {
        std::fprintf(stderr, "mount failed; see log\n");
        return 1;
    }

    bool ok = false;
    const std::string content = readFile(mountPoint + "/hello.txt", ok);
    if (!ok || content != expected) {
        std::fprintf(stderr, "read mismatch: ok=%d size=%zu expected=%zu\n",
                     ok ? 1 : 0, content.size(), expected.size());
        manager.unmount();
        return 1;
    }

    // A read-only mount must refuse every write path at the FUSE layer.
    errno = 0;
    const int writeFd = ::open((mountPoint + "/hello.txt").c_str(), O_WRONLY);
    if (writeFd >= 0) {
        ::close(writeFd);
        std::fprintf(stderr, "read-only mount accepted O_WRONLY\n");
        manager.unmount();
        return 1;
    }
    if (errno != EROFS) {
        std::fprintf(stderr, "read-only open failed with errno=%d (want EROFS)\n", errno);
        manager.unmount();
        return 1;
    }

    struct stat st{};
    if (::stat((mountPoint + "/hello.txt").c_str(), &st) != 0 ||
        st.st_size != static_cast<off_t>(expected.size())) {
        std::fprintf(stderr, "stat mismatch\n");
        manager.unmount();
        return 1;
    }

    // Directory listing through the mount.
    DIR* dir = ::opendir((mountPoint + "/sub").c_str());
    if (!dir) {
        std::fprintf(stderr, "opendir failed\n");
        manager.unmount();
        return 1;
    }
    bool sawNested = false;
    while (struct dirent* entry = ::readdir(dir)) {
        if (std::strcmp(entry->d_name, "nested.txt") == 0) sawNested = true;
    }
    ::closedir(dir);
    if (!sawNested) {
        std::fprintf(stderr, "nested entry missing\n");
        manager.unmount();
        return 1;
    }

    // Offset read through the mount (exercises readRange at a non-zero offset).
    {
        const int fd = ::open((mountPoint + "/hello.txt").c_str(), O_RDONLY);
        if (fd < 0) {
            std::fprintf(stderr, "open through mount failed\n");
            manager.unmount();
            return 1;
        }
        char buffer[8] = {};
        const ssize_t got = ::pread(fd, buffer, 5, 6);
        ::close(fd);
        if (got != 5 || std::string(buffer, 5) != "from ") {
            std::fprintf(stderr, "offset read mismatch: %zd\n", got);
            manager.unmount();
            return 1;
        }
    }

    // A file larger than one FUSE read, with a byte pattern.
    {
        const std::string largePath = mountPoint + "/large.bin";
        std::ifstream large(largePath, std::ios::binary);
        if (!large) {
            std::fprintf(stderr, "large file open failed\n");
            manager.unmount();
            return 1;
        }
        std::vector<char> data((std::istreambuf_iterator<char>(large)),
                               std::istreambuf_iterator<char>());
        if (data.size() != 2 * 1024 * 1024 ||
            data[0] != 'A' || data[data.size() - 1] != 'Z') {
            std::fprintf(stderr, "large file mismatch: %zu\n", data.size());
            manager.unmount();
            return 1;
        }
    }

    manager.unmount();
    if (::access(mountPoint.c_str(), F_OK) == 0 || manager.isMounted()) {
        std::fprintf(stderr, "mount point or mount state survived unmount\n");
        return 1;
    }
    std::printf("FUSE device-mount test passed\n");
    return 0;
}
