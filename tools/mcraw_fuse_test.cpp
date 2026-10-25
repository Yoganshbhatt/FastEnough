// End-to-end test of the Linux MCRAW FUSE view against a real .mcraw file.
// The file is only ever read; the decode cache and mount point live under
// $XDG_RUNTIME_DIR and are removed by unmountAll().
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "mount_stubs.h"

namespace {

bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::vector<std::string> listDir(const std::string& path) {
    std::vector<std::string> names;
    DIR* dir = ::opendir(path.c_str());
    if (!dir) return names;
    while (struct dirent* entry = ::readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
            names.emplace_back(entry->d_name);
    }
    ::closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

bool readPrefix(const std::string& path, size_t size, std::string& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    out.resize(size);
    stream.read(out.data(), static_cast<std::streamsize>(size));
    out.resize(static_cast<size_t>(stream.gcount()));
    return true;
}

// A DNG is a TIFF container: "II" + 42 (LE) or "MM" + 42 (BE).
bool isDng(const std::string& bytes) {
    if (bytes.size() < 4) return false;
    const auto* raw = reinterpret_cast<const unsigned char*>(bytes.data());
    if (raw[0] == 'I' && raw[1] == 'I') return raw[2] == 42 || (raw[2] == 0 && raw[3] == 42);
    if (raw[0] == 'M' && raw[1] == 'M') return (raw[2] == 0 && raw[3] == 42) || raw[2] == 42;
    return false;
}

int fail(const char* what) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    McrawMountManager::instance().unmountAll();
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <file.mcraw>\n", argv[0]);
        return 2;
    }
    const std::string mcraw = argv[1];
    struct stat source{};
    if (::stat(mcraw.c_str(), &source) != 0 || !S_ISREG(source.st_mode)) {
        std::fprintf(stderr, "FAIL: %s is not a regular file\n", mcraw.c_str());
        return 2;
    }

    auto& manager = McrawMountManager::instance();
    const std::string mountPath = manager.mountMcraw(mcraw);
    if (mountPath.empty()) return fail("mountMcraw returned no mount point");
    std::printf("mounted %s at %s\n", mcraw.c_str(), mountPath.c_str());

    const std::vector<std::string> names = listDir(mountPath);
    if (names.empty()) return fail("mount is empty");

    bool sawMetadata = false;
    size_t frameCount = 0;
    std::string firstFrame;
    for (const auto& name : names) {
        if (name == "metadata.json") sawMetadata = true;
        if (startsWith(name, "frame_") && name.size() > 4 &&
            name.compare(name.size() - 4, 4, ".dng") == 0) {
            ++frameCount;
            if (firstFrame.empty()) firstFrame = name;
        }
    }
    std::printf("entries: %zu (metadata=%d frames=%zu)\n", names.size(), sawMetadata ? 1 : 0,
                frameCount);
    if (!sawMetadata) return fail("metadata.json missing");
    if (frameCount == 0) return fail("no frame_NNNNNN.dng entries");

    std::string metadata;
    if (!readPrefix(mountPath + "/metadata.json", 64, metadata)) return fail("metadata unreadable");
    if (metadata.empty() || metadata.front() != '{') return fail("metadata is not JSON");

    std::string dng;
    if (!readPrefix(mountPath + "/" + firstFrame, 4096, dng)) return fail("frame unreadable");
    if (!isDng(dng)) return fail("frame does not start with a TIFF/DNG header");
    std::printf("frame %s: %zu bytes read, DNG header ok\n", firstFrame.c_str(), dng.size());

    // Second read must be byte-identical (served from the decode cache).
    std::string dngAgain;
    if (!readPrefix(mountPath + "/" + firstFrame, 4096, dngAgain)) return fail("frame re-read failed");
    if (dngAgain != dng) return fail("cached frame differs from the first decode");

    struct stat mounted{};
    if (::stat((mountPath + "/" + firstFrame).c_str(), &mounted) != 0 || mounted.st_size <= 0)
        return fail("mounted frame has no size");
    std::printf("frame size after decode: %lld\n", static_cast<long long>(mounted.st_size));

    // The MCRAW view is read-only at the FUSE layer.
    errno = 0;
    const int writeFd = ::open((mountPath + "/" + firstFrame).c_str(), O_WRONLY);
    if (writeFd >= 0) {
        ::close(writeFd);
        return fail("read-only MCRAW mount accepted O_WRONLY");
    }
    if (errno != EROFS) {
        std::fprintf(stderr, "FAIL: write open errno=%d (want EROFS=%d)\n", errno, EROFS);
        manager.unmountAll();
        return 1;
    }

    manager.unmountAll();
    if (::access(mountPath.c_str(), F_OK) == 0) return fail("mount point survived unmountAll");
    if (::access((mountPath + ".cache").c_str(), F_OK) == 0) return fail("decode cache survived");
    std::printf("MCRAW FUSE test passed\n");
    return 0;
}
