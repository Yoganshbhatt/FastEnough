// Shared libfuse3 plumbing for the Linux mount backends.
//
// The phone-storage filesystem (fs_fuse.cpp) and the MCRAW view (mcraw_fuse)
// both need the same lifecycle, path validation, stat synthesis and readdir
// behavior. Keeping that here means neither backend can accidentally mount
// outside the private runtime directory or leak a handle on unmount.
#pragma once

#if defined(__linux__)

#include <sys/stat.h>
#include <sys/statvfs.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace afm::fuse {

enum class NodeType { File, Directory };

struct Node {
    NodeType type = NodeType::File;
    uint64_t size = 0;
    int64_t mtime = 0;
};

struct DirEntry {
    std::string name;
    Node node;
};

// Backends are called only from the FUSE loop thread (single-threaded in
// V1/V2). A backend may return a positive errno through `error`; callers map
// it to a negative FUSE return value.
class Backend {
public:
    virtual ~Backend() = default;

    virtual bool stat(const std::string& path, Node& out, int& error) = 0;
    virtual bool list(const std::string& path, std::vector<DirEntry>& out, int& error) = 0;
    virtual bool read(const std::string& path, uint64_t offset, size_t size,
                      std::vector<char>& out, int& error) = 0;
    virtual bool statfs(struct statvfs& out, int& error) = 0;

    // Writable backends override these. The defaults keep read-only
    // backends (the synthetic test backend and MCRAW view) unchanged.
    virtual bool create(const std::string&, mode_t, int, int& error) {
        error = EROFS;
        return false;
    }
    virtual bool write(const std::string&, uint64_t, const char*, size_t, int& error) {
        error = EROFS;
        return false;
    }
    virtual bool truncate(const std::string&, uint64_t, int& error) {
        error = EROFS;
        return false;
    }
    virtual bool unlink(const std::string&, int& error) {
        error = EROFS;
        return false;
    }
    virtual bool mkdir(const std::string&, mode_t, int& error) {
        error = EROFS;
        return false;
    }
    virtual bool rmdir(const std::string&, int& error) {
        error = EROFS;
        return false;
    }
    virtual bool rename(const std::string&, const std::string&, bool, int& error) {
        error = EROFS;
        return false;
    }
    // Flush any buffered writes for `path` to the device. Called on close,
    // fsync and before reads/metadata queries so callers never observe a
    // stale view of a file they just wrote.
    virtual bool flush(const std::string&, int& error) { (void)error; return true; }
    // Timestamps are accepted silently (Dokan's FsSetFileTime does the same);
    // the protocol has no utime command.
    virtual bool utimens(const std::string&, int& error) { error = EROFS; return false; }

    // Called before the loop is asked to exit, from a different thread. It
    // must unblock any in-flight network/socket operation promptly.
    virtual void interrupt() {}
};

// Owns one FUSE mount and its loop thread. Start() is synchronous: it returns
// only after the mount is live (or reports why it failed). Stop() is
// idempotent and waits for the loop to finish.
class Server {
public:
    Server(std::unique_ptr<Backend> backend, std::string mountPoint,
           std::string fsName, bool readOnly);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(std::string& error);
    void stop();

    bool running() const;
    const std::string& mountPoint() const { return m_mountPoint; }
    bool readOnly() const { return m_readOnly; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_mountPoint;
    bool m_readOnly = true;
};

// ${XDG_RUNTIME_DIR}/fast-enough, or /tmp/fast-enough-<uid>. Never creates it.
std::string runtimeRoot();

// Pure path computation; never creates directories.
std::string slotMountPoint(int slot);

// True when `path` is the root itself or a child of it (no ".." tricks).
bool isUnderRoot(const std::string& root, const std::string& path);

// Creates the runtime root and `path`, requiring a private (0700) directory
// owned by this user. `path` must be a strict child of `root` and must be
// empty. On failure `error` explains why.
bool prepareMountPoint(const std::string& path, const std::string& root,
                       std::string& error);

// Maps server error strings to POSIX errno values. Unknown errors become EIO.
int errnoFromMessage(const std::string& message);

// Validates a FUSE path: absolute, no NUL, no "." or ".." components, bounded
// length. Returns the normalized path (root remains "/").
bool normalizeFusePath(const std::string& input, std::string& out, int& error);

// Fills a struct stat from a backend node. Exposed for unit tests.
void fillStat(const Node& node, struct stat& st, uid_t uid, gid_t gid);

}  // namespace afm::fuse

#endif  // defined(__linux__)
