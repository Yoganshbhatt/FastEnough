// FUSE plumbing tests: path normalization, errno mapping, mount-point safety,
// and a real synthetic mount (no device and no phone access).
//
// Returns 77 when /dev/fuse is unavailable so CTest reports a skip instead of
// a failure on machines without FUSE.
#include "fuse_common.h"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mount_stubs.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("ok   %s\n", what);
    } else {
        std::printf("FAIL %s\n", what);
        ++g_failures;
    }
}

class SyntheticBackend final : public afm::fuse::Backend {
public:
    SyntheticBackend() {
        m_dirs = {"/", "/dir", "/empty"};
        m_files = {
            {"/hello.txt", "hello from the synthetic FUSE backend\n"},
            {"/dir/nested.bin", std::string(1024 * 1024, 'x')},
        };
    }

    bool stat(const std::string& path, afm::fuse::Node& out, int& error) override {
        if (m_dirs.count(path)) {
            out.type = afm::fuse::NodeType::Directory;
            out.size = 0;
            out.mtime = 1700000000;
            return true;
        }
        const auto it = m_files.find(path);
        if (it == m_files.end()) {
            error = ENOENT;
            return false;
        }
        out.type = afm::fuse::NodeType::File;
        out.size = it->second.size();
        out.mtime = 1700000001;
        return true;
    }

    bool list(const std::string& path, std::vector<afm::fuse::DirEntry>& out,
              int& error) override {
        if (!m_dirs.count(path)) {
            error = ENOTDIR;
            return false;
        }
        out.clear();
        for (const auto& dir : m_dirs) {
            if (dir == path || dir == "/") continue;
            if (dir.rfind(path + "/", 0) == 0 && dir.find('/', path.size() + 1) == std::string::npos) {
                afm::fuse::DirEntry entry;
                entry.name = dir.substr(path.size() + 1);
                entry.node.type = afm::fuse::NodeType::Directory;
                out.push_back(std::move(entry));
            }
        }
        for (const auto& file : m_files) {
            const auto& name = file.first;
            if (name.rfind(path + "/", 0) != 0 && !(path == "/" && name.rfind('/', 1) == std::string::npos))
                continue;
            const size_t slash = name.find_last_of('/');
            if (slash != path.size() && !(path == "/" && slash == 0)) continue;
            afm::fuse::DirEntry entry;
            entry.name = name.substr(slash + 1);
            entry.node.type = afm::fuse::NodeType::File;
            entry.node.size = file.second.size();
            out.push_back(std::move(entry));
        }
        return true;
    }

    bool read(const std::string& path, uint64_t offset, size_t size,
              std::vector<char>& out, int& error) override {
        const auto it = m_files.find(path);
        if (it == m_files.end()) {
            error = ENOENT;
            return false;
        }
        if (offset >= it->second.size()) {
            out.clear();
            return true;
        }
        const size_t available = it->second.size() - static_cast<size_t>(offset);
        const size_t count = std::min(size, available);
        out.assign(it->second.begin() + static_cast<long>(offset),
                   it->second.begin() + static_cast<long>(offset + count));
        return true;
    }

    bool statfs(struct statvfs& out, int&) override {
        std::memset(&out, 0, sizeof(out));
        out.f_bsize = 4096;
        out.f_frsize = 4096;
        out.f_blocks = 1000;
        out.f_bfree = 500;
        out.f_bavail = 500;
        out.f_namemax = 255;
        return true;
    }

private:
    std::set<std::string> m_dirs;
    std::map<std::string, std::string> m_files;
};

bool makePrivateDir(const std::string& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode) && st.st_uid == ::getuid() && (st.st_mode & 077) == 0;
    }
    return ::mkdir(path.c_str(), 0700) == 0;
}

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

}  // namespace

int main() {
    if (::access("/dev/fuse", R_OK | W_OK) != 0) {
        std::printf("SKIP /dev/fuse is not accessible\n");
        return 77;
    }

    std::string normalized;
    int error = 0;
    check(afm::fuse::normalizeFusePath("/", normalized, error) && normalized == "/",
          "normalize root");
    check(afm::fuse::normalizeFusePath("//dir///file", normalized, error) &&
              normalized == "/dir/file",
          "normalize duplicate separators");
    check(!afm::fuse::normalizeFusePath("/../etc", normalized, error) && error == EINVAL,
          "reject parent traversal");
    check(!afm::fuse::normalizeFusePath("relative", normalized, error) && error == EINVAL,
          "reject relative path");

    check(afm::fuse::errnoFromMessage("No such file or directory") == ENOENT,
          "map missing file");
    check(afm::fuse::errnoFromMessage("Permission denied") == EACCES,
          "map permission denied");
    check(afm::fuse::errnoFromMessage("Directory not empty") == ENOTEMPTY,
          "map non-empty directory");
    check(afm::fuse::errnoFromMessage("something unexpected") == EIO,
          "map unknown error");

    check(afm::fuse::isUnderRoot("/run/user/1/fast-enough", "/run/user/1/fast-enough/slot-0"),
          "mount point is under root");
    check(!afm::fuse::isUnderRoot("/run/user/1/fast-enough", "/run/user/1/other"),
          "unrelated path is rejected");
    check(!afm::fuse::isUnderRoot("/run/user/1/fast-enough", "/run/user/1/fast-enough-evil"),
          "prefix confusion is rejected");

    struct stat st{};
    afm::fuse::Node node;
    node.type = afm::fuse::NodeType::File;
    node.size = 513;
    afm::fuse::fillStat(node, st, 1000, 1000);
    check(S_ISREG(st.st_mode) && st.st_size == 513 && st.st_blocks == 2,
          "stat synthesis for a file");

    const std::string root = afm::fuse::runtimeRoot();
    const std::string mountPoint = root + "/test-" + std::to_string(::getpid());
    const std::string outside = "/tmp/fast-enough-test-" + std::to_string(::getpid());
    std::string prepareError;
    check(!afm::fuse::prepareMountPoint(outside, root, prepareError),
          "refuse a mount point outside the runtime root");

    if (!makePrivateDir(root)) {
        std::printf("FAIL cannot create %s\n", root.c_str());
        return 1;
    }
    if (!makePrivateDir(mountPoint)) {
        std::printf("FAIL cannot create %s\n", mountPoint.c_str());
        return 1;
    }
    {
        std::ofstream blocker(mountPoint + "/leftover");
        blocker << "not empty";
    }
    check(!afm::fuse::prepareMountPoint(mountPoint, root, prepareError),
          "refuse a non-empty mount point");
    ::unlink((mountPoint + "/leftover").c_str());
    ::rmdir(mountPoint.c_str());

    {
        const std::string linkPath = root + "/test-link-" + std::to_string(::getpid());
        ::unlink(linkPath.c_str());
        if (::symlink("/tmp", linkPath.c_str()) == 0) {
            check(!afm::fuse::prepareMountPoint(linkPath, root, prepareError),
                  "refuse a symlinked mount point");
            ::unlink(linkPath.c_str());
        }
    }

    {
        auto server = std::make_unique<afm::fuse::Server>(
            std::make_unique<SyntheticBackend>(), mountPoint, "FastEnough-test", true);
        std::string startError;
        if (!server->start(startError)) {
            std::printf("FAIL synthetic mount: %s\n", startError.c_str());
            ++g_failures;
        } else {
            check(server->running(), "synthetic FUSE loop is running");

            if (std::getenv("AFM_FUSE_HOLD")) {
                std::printf("HOLD %s\n", mountPoint.c_str());
                std::fflush(stdout);
                ::sleep(60);
            }

            bool ok = false;
            const std::string hello = readFile(mountPoint + "/hello.txt", ok);
            check(ok && hello == "hello from the synthetic FUSE backend\n",
                  "read a file through the mount");

            bool nestedOk = false;
            const std::string nested = readFile(mountPoint + "/dir/nested.bin", nestedOk);
            check(nestedOk && nested.size() == 1024 * 1024 && nested[0] == 'x',
                  "read a nested file through the mount");

            struct statvfs vfs{};
            check(::statvfs(mountPoint.c_str(), &vfs) == 0 && vfs.f_blocks == 1000,
                  "statfs through the mount");

            struct stat fileStat{};
            check(::stat((mountPoint + "/hello.txt").c_str(), &fileStat) == 0 &&
                      fileStat.st_size == static_cast<off_t>(hello.size()),
                  "stat through the mount");

            server->stop();
            check(!server->running(), "synthetic FUSE loop stopped");
            check(::access(mountPoint.c_str(), F_OK) != 0,
                  "mount point is removed after unmount");
        }
    }

    ::rmdir(outside.c_str());
    ::rmdir((root + "/test-" + std::to_string(::getpid())).c_str());
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all FUSE checks passed\n");
    return 0;
}
