// Path, mount-point and errno helpers for the Linux FUSE backends.
#include "fuse_common.h"

#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <string>

namespace afm::fuse {

namespace {

bool ensurePrivateDirectory(const std::string& path, std::string& error) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        if (errno != ENOENT) {
            error = std::string("lstat(") + path + "): " + std::strerror(errno);
            return false;
        }
        const std::filesystem::path fsPath(path);
        const std::string parent = fsPath.parent_path().string();
        if (!parent.empty() && parent != path) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                error = "cannot create parent " + parent + ": " + ec.message();
                return false;
            }
        }
        // Create the leaf explicitly with 0700; create_directories() would
        // follow the process umask and can produce 0755, which we reject.
        if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
            error = std::string("mkdir(") + path + "): " + std::strerror(errno);
            return false;
        }
        if (::lstat(path.c_str(), &st) != 0) {
            error = std::string("lstat(") + path + "): " + std::strerror(errno);
            return false;
        }
    }
    if (!S_ISDIR(st.st_mode)) {
        error = path + " exists but is not a directory";
        return false;
    }
    if (st.st_uid != ::getuid()) {
        error = path + " is not owned by this user";
        return false;
    }
    if ((st.st_mode & 077) != 0) {
        error = path + " must be private (mode 0700)";
        return false;
    }
    return true;
}

bool directoryIsEmpty(const std::string& path, bool& empty, std::string& error) {
    DIR* dir = ::opendir(path.c_str());
    if (!dir) {
        error = std::string("opendir(") + path + "): " + std::strerror(errno);
        return false;
    }
    empty = true;
    errno = 0;
    while (struct dirent* entry = ::readdir(dir)) {
        const char* name = entry->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) continue;
        empty = false;
        break;
    }
    const int saved = errno;
    ::closedir(dir);
    if (!empty) return true;
    if (saved != 0) {
        error = std::string("readdir(") + path + "): " + std::strerror(saved);
        return false;
    }
    return true;
}

}  // namespace

std::string runtimeRoot() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] == '/' && std::strlen(xdg) < 4000) {
        std::string root(xdg);
        while (root.size() > 1 && root.back() == '/') root.pop_back();
        if (root != "/") return root + "/fast-enough";
    }
    return "/tmp/fast-enough-" + std::to_string(::getuid());
}

std::string slotMountPoint(int slot) {
    return runtimeRoot() + "/slot-" + std::to_string(slot < 0 ? 0 : slot);
}

bool isUnderRoot(const std::string& root, const std::string& path) {
    if (root.empty() || path.empty() || root[0] != '/' || path[0] != '/') return false;
    std::string base = root;
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    if (path == base) return true;
    if (path.size() <= base.size()) return false;
    return path.compare(0, base.size(), base) == 0 && path[base.size()] == '/';
}

bool prepareMountPoint(const std::string& path, const std::string& root,
                       std::string& error) {
    if (!isUnderRoot(root, path) || path == root) {
        error = "mount point is outside the private runtime directory";
        return false;
    }
    if (path.find("/../") != std::string::npos ||
        (path.size() >= 3 && path.compare(path.size() - 3, 3, "/..") == 0)) {
        error = "mount point must not contain parent traversal";
        return false;
    }
    if (!ensurePrivateDirectory(root, error)) return false;
    if (!ensurePrivateDirectory(path, error)) return false;
    bool empty = false;
    if (!directoryIsEmpty(path, empty, error)) return false;
    if (!empty) {
        error = "mount point is not empty: " + path;
        return false;
    }
    return true;
}

int errnoFromMessage(const std::string& message) {
    std::string lower;
    lower.reserve(message.size());
    for (const unsigned char c : message) lower.push_back(static_cast<char>(std::tolower(c)));
    auto has = [&lower](const char* needle) { return lower.find(needle) != std::string::npos; };
    if (has("no such file") || has("not found")) return ENOENT;
    if (has("operation not permitted")) return EPERM;
    if (has("permission denied") || has("denied")) return EACCES;
    if (has("is a directory")) return EISDIR;
    if (has("not a directory")) return ENOTDIR;
    if (has("directory not empty") || has("not empty")) return ENOTEMPTY;
    if (has("already exists") || has("file exists")) return EEXIST;
    if (has("no space")) return ENOSPC;
    if (has("read-only")) return EROFS;
    if (has("too long")) return ENAMETOOLONG;
    if (has("invalid argument")) return EINVAL;
    if (has("too many open files")) return EMFILE;
    if (has("device or resource busy") || has("busy")) return EBUSY;
    if (has("too many levels of symbolic links")) return ELOOP;
    if (has("cross-device link")) return EXDEV;
    if (has("input/output error")) return EIO;
    return EIO;
}

bool normalizeFusePath(const std::string& input, std::string& out, int& error) {
    if (input.empty() || input[0] != '/') {
        error = EINVAL;
        return false;
    }
    if (input.size() >= PATH_MAX) {
        error = ENAMETOOLONG;
        return false;
    }
    if (input.find('\0') != std::string::npos) {
        error = EINVAL;
        return false;
    }
    if (input == "/") {
        out = "/";
        return true;
    }
    std::string normalized = "/";
    size_t start = 1;
    while (start < input.size()) {
        const size_t slash = input.find('/', start);
        const size_t end = slash == std::string::npos ? input.size() : slash;
        const std::string part = input.substr(start, end - start);
        if (part.empty() || part == ".") {
            // Collapse duplicate separators and "." components.
        } else if (part == "..") {
            error = EINVAL;
            return false;
        } else {
            if (normalized.size() > 1) normalized.push_back('/');
            normalized += part;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (normalized.empty()) normalized = "/";
    out = std::move(normalized);
    return true;
}

void fillStat(const Node& node, struct stat& st, uid_t uid, gid_t gid) {
    std::memset(&st, 0, sizeof(st));
    const bool dir = node.type == NodeType::Directory;
    st.st_mode = (dir ? S_IFDIR | 0755 : S_IFREG | 0644);
    st.st_nlink = dir ? 2 : 1;
    st.st_uid = uid;
    st.st_gid = gid;
    st.st_size = dir ? 0 : static_cast<off_t>(node.size);
    st.st_blksize = 4096;
    st.st_blocks = dir ? 0 : static_cast<blkcnt_t>((node.size + 511) / 512);
    st.st_mtime = static_cast<time_t>(node.mtime);
    st.st_atime = st.st_mtime;
    st.st_ctime = st.st_mtime;
}

}  // namespace afm::fuse

