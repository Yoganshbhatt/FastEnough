#define FUSE_USE_VERSION 35

#include "fuse_common.h"

#include <fuse.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <system_error>
#include <thread>

#include "log_facade.h"

namespace afm::fuse {
namespace {

struct FuseCtx {
    Backend* backend = nullptr;
    bool readOnly = true;
};

struct Handle {
    std::string path;
};

bool normalizePath(const char* raw, std::string& out, int& error) {
    if (!raw) {
        error = EINVAL;
        return false;
    }
    return normalizeFusePath(std::string(raw), out, error);
}

void fillReaddirStat(const Node& node, struct stat& st) {
    fillStat(node, st, ::getuid(), ::getgid());
}

int cbGetattr(const char* path, struct stat* st, struct fuse_file_info*) {
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    std::string normalized;
    int error = 0;
    if (!normalizePath(path, normalized, error)) return -error;
    Node node;
    if (!ctx->backend->stat(normalized, node, error)) return -error;
    std::memset(st, 0, sizeof(*st));
    fillStat(node, *st, ::getuid(), ::getgid());
    return 0;
}

int cbReadlink(const char*, char*, size_t) {
    return -EOPNOTSUPP;
}

int cbReaddir(const char* path, void* buffer, fuse_fill_dir_t filler, off_t offset,
              struct fuse_file_info*, enum fuse_readdir_flags flags) {
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    std::string normalized;
    int error = 0;
    if (!normalizePath(path, normalized, error)) return -error;

    Node dirNode;
    if (!ctx->backend->stat(normalized, dirNode, error)) return -error;
    std::vector<DirEntry> entries;
    if (!ctx->backend->list(normalized, entries, error)) return -error;

    // Cookies: "." = 1, ".." = 2, entries start at 3. A resumed call receives
    // the last cookie; entries before it are skipped.
    const bool withStat = (flags & FUSE_READDIR_PLUS) != 0;
    const fuse_fill_dir_flags fillFlags =
        withStat ? FUSE_FILL_DIR_PLUS : static_cast<fuse_fill_dir_flags>(0);
    struct stat st{};
    if (withStat) {
        fillReaddirStat(dirNode, st);
        if (offset == 0 && filler(buffer, ".", &st, 1, fillFlags) != 0) return 0;
        if (offset <= 1 && filler(buffer, "..", &st, 2, fillFlags) != 0) return 0;
    } else {
        if (offset == 0 && filler(buffer, ".", nullptr, 1, fillFlags) != 0) return 0;
        if (offset <= 1 && filler(buffer, "..", nullptr, 2, fillFlags) != 0) return 0;
    }

    const size_t start = offset <= 2 ? 0 : static_cast<size_t>(offset - 2);
    for (size_t i = start; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const off_t next = static_cast<off_t>(i + 3);
        if (withStat) {
            fillReaddirStat(entry.node, st);
            if (filler(buffer, entry.name.c_str(), &st, next, fillFlags) != 0) return 0;
        } else {
            if (filler(buffer, entry.name.c_str(), nullptr, next, fillFlags) != 0) return 0;
        }
    }
    return 0;
}

int cbOpen(const char* path, struct fuse_file_info* fi) {
    if (!fi) return -EINVAL;
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    std::string normalized;
    int error = 0;
    if (!normalizePath(path, normalized, error)) return -error;

    Node node;
    if (!ctx->backend->stat(normalized, node, error)) return -error;
    if (node.type == NodeType::Directory) return -EISDIR;

    const int flags = static_cast<int>(static_cast<unsigned>(fi->flags) & O_ACCMODE);
    if (ctx->readOnly && flags != O_RDONLY) return -EROFS;
    if (!ctx->readOnly && (static_cast<unsigned>(fi->flags) & O_TRUNC) && flags != O_RDONLY) {
        int truncateError = 0;
        if (!ctx->backend->truncate(normalized, 0, truncateError)) return -truncateError;
    }

    auto* handle = new (std::nothrow) Handle;
    if (!handle) return -ENOMEM;
    handle->path = std::move(normalized);
    fi->fh = reinterpret_cast<uint64_t>(handle);
    fi->keep_cache = 0;
    return 0;
}

int cbRead(const char* path, char* buffer, size_t size, off_t offset,
           struct fuse_file_info* fi) {
    if (!fi) return -EINVAL;
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    Handle* handle = reinterpret_cast<Handle*>(fi->fh);
    std::string normalized;
    int error = 0;
    if (handle) {
        normalized = handle->path;
    } else if (!normalizePath(path, normalized, error)) {
        return -error;
    }
    if (offset < 0) return -EINVAL;

    const uint64_t start = static_cast<uint64_t>(offset);
    const size_t want = size;
    if (want == 0) return 0;

    std::vector<char> data;
    if (!ctx->backend->read(normalized, start, want, data, error)) return -error;
    if (data.empty()) return 0;  // clean EOF; backend reports failures above
    if (data.size() > want) return -EIO;
    std::memcpy(buffer, data.data(), data.size());
    return static_cast<int>(data.size());
}

int cbRelease(const char* path, struct fuse_file_info* fi) {
    if (!fi) return 0;
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    Handle* handle = reinterpret_cast<Handle*>(fi->fh);
    if (handle) {
        int error = 0;
        if (!ctx->backend->flush(handle->path, error)) {
            delete handle;
            fi->fh = 0;
            return -error;
        }
    }
    delete handle;
    fi->fh = 0;
    (void)path;
    return 0;
}

int cbCreate(const char* path, mode_t mode, struct fuse_file_info* fi) {
    if (!fi) return -EINVAL;
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    if (ctx->readOnly) return -EROFS;
    std::string normalized;
    int error = 0;
    if (!normalizePath(path, normalized, error)) return -error;
    if (normalized == "/") return -EBUSY;
    if (!ctx->backend->create(normalized, mode, fi->flags, error)) return -error;

    auto* handle = new (std::nothrow) Handle;
    if (!handle) return -ENOMEM;
    handle->path = normalized;
    fi->fh = reinterpret_cast<uint64_t>(handle);
    fi->keep_cache = 0;
    return 0;
}

int cbWrite(const char* path, const char* buffer, size_t size, off_t offset,
            struct fuse_file_info* fi) {
    if (!fi) return -EINVAL;
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    if (ctx->readOnly) return -EROFS;
    if (offset < 0) return -EINVAL;
    Handle* handle = reinterpret_cast<Handle*>(fi->fh);
    std::string normalized;
    int error = 0;
    if (handle) {
        normalized = handle->path;
    } else if (!normalizePath(path, normalized, error)) {
        return -error;
    }
    if (!ctx->backend->write(normalized, static_cast<uint64_t>(offset), buffer, size,
                             error)) {
        return -error;
    }
    return static_cast<int>(size);
}

int cbTruncate(const char* path, off_t size, struct fuse_file_info* fi) {
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    if (ctx->readOnly) return -EROFS;
    if (size < 0) return -EINVAL;
    Handle* handle = fi ? reinterpret_cast<Handle*>(fi->fh) : nullptr;
    std::string normalized;
    int error = 0;
    if (handle) {
        normalized = handle->path;
    } else if (!normalizePath(path, normalized, error)) {
        return -error;
    }
    if (normalized == "/") return -EBUSY;
    if (!ctx->backend->truncate(normalized, static_cast<uint64_t>(size), error)) {
        return -error;
    }
    return 0;
}

int cbUnlink(const char* path) {
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    if (ctx->readOnly) return -EROFS;
    std::string normalized;
    int error = 0;
    if (!normalizePath(path, normalized, error)) return -error;
    if (normalized == "/") return -EBUSY;
    return ctx->backend->unlink(normalized, error) ? 0 : -error;
}

int cbMkdir(const char* path, mode_t mode) {
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    if (ctx->readOnly) return -EROFS;
    std::string normalized;
    int error = 0;
    if (!normalizePath(path, normalized, error)) return -error;
    if (normalized == "/") return -EBUSY;
    return ctx->backend->mkdir(normalized, mode, error) ? 0 : -error;
}

int cbRmdir(const char* path) {
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    if (ctx->readOnly) return -EROFS;
    std::string normalized;
    int error = 0;
    if (!normalizePath(path, normalized, error)) return -error;
    if (normalized == "/") return -EBUSY;
    return ctx->backend->rmdir(normalized, error) ? 0 : -error;
}

int cbRename(const char* from, const char* to, unsigned int flags) {
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    if (ctx->readOnly) return -EROFS;
    if (flags & ~static_cast<unsigned int>(RENAME_NOREPLACE)) return -EINVAL;
    std::string oldPath;
    std::string newPath;
    int error = 0;
    if (!normalizePath(from, oldPath, error) || !normalizePath(to, newPath, error)) {
        return -error;
    }
    if (oldPath == "/" || newPath == "/") return -EBUSY;
    const bool replace = (flags & RENAME_NOREPLACE) == 0;
    return ctx->backend->rename(oldPath, newPath, replace, error) ? 0 : -error;
}

int cbFlush(const char* path, struct fuse_file_info* fi) {
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    Handle* handle = fi ? reinterpret_cast<Handle*>(fi->fh) : nullptr;
    std::string normalized;
    int error = 0;
    if (handle) {
        normalized = handle->path;
    } else if (!normalizePath(path, normalized, error)) {
        return -error;
    }
    return ctx->backend->flush(normalized, error) ? 0 : -error;
}

int cbFsync(const char* path, int, struct fuse_file_info* fi) {
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    Handle* handle = fi ? reinterpret_cast<Handle*>(fi->fh) : nullptr;
    std::string normalized;
    int error = 0;
    if (handle) {
        normalized = handle->path;
    } else if (!normalizePath(path, normalized, error)) {
        return -error;
    }
    return ctx->backend->flush(normalized, error) ? 0 : -error;
}

int cbUtimens(const char* path, const struct timespec[2], struct fuse_file_info* fi) {
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    if (ctx->readOnly) return -EROFS;
    Handle* handle = fi ? reinterpret_cast<Handle*>(fi->fh) : nullptr;
    std::string normalized;
    int error = 0;
    if (handle) {
        normalized = handle->path;
    } else if (!normalizePath(path, normalized, error)) {
        return -error;
    }
    if (normalized == "/") return -EBUSY;
    return ctx->backend->utimens(normalized, error) ? 0 : -error;
}

int cbStatfs(const char*, struct statvfs* st) {
    auto* ctx = static_cast<FuseCtx*>(fuse_get_context()->private_data);
    int error = 0;
    if (!ctx->backend->statfs(*st, error)) return -error;
    if (ctx->readOnly) st->f_flag |= ST_RDONLY;
    return 0;
}

void* cbInit(struct fuse_conn_info* conn, struct fuse_config* cfg) {
    cfg->kernel_cache = 0;
    cfg->attr_timeout = 1.0;
    cfg->entry_timeout = 1.0;
    cfg->negative_timeout = 0.0;
    cfg->direct_io = 0;
    // libfuse requires the same max_read in the session (mount option) and
    // here; a mismatch aborts the session during init.
    conn->max_read = 1048576;
    // Larger requests let a 1 MiB-at-a-time writer reach the device in one op.
    conn->max_write = 1048576;
    return fuse_get_context()->private_data;
}

void cbDestroy(void*) {
    // The backend is owned by Server; do not delete it here. fuse_destroy()
    // calls this while the loop is no longer running.
}

const fuse_operations& fuseOps() {
    static const fuse_operations ops = [] {
        fuse_operations value{};
        value.getattr = cbGetattr;
        value.readlink = cbReadlink;
        value.readdir = cbReaddir;
        value.open = cbOpen;
        value.read = cbRead;
        value.create = cbCreate;
        value.write = cbWrite;
        value.truncate = cbTruncate;
        value.unlink = cbUnlink;
        value.mkdir = cbMkdir;
        value.rmdir = cbRmdir;
        value.rename = cbRename;
        value.flush = cbFlush;
        value.fsync = cbFsync;
        value.utimens = cbUtimens;
        value.release = cbRelease;
        value.statfs = cbStatfs;
        value.init = cbInit;
        value.destroy = cbDestroy;
        return value;
    }();
    return ops;
}


}  // namespace


struct Server::Impl {
    std::unique_ptr<Backend> backend;
    std::string fsName;
    bool readOnly = true;
    struct fuse* fuse = nullptr;
    std::thread loop;
    std::mutex loopIdMutex;
    std::thread::id loopId;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    FuseCtx ctx;
    bool mountPointExisted = false;
    bool prepared = false;
};

Server::Server(std::unique_ptr<Backend> backend, std::string mountPoint,
               std::string fsName, bool readOnly)
    : m_impl(std::make_unique<Impl>()),
      m_mountPoint(std::move(mountPoint)),
      m_readOnly(readOnly) {
    m_impl->backend = std::move(backend);
    m_impl->fsName = std::move(fsName);
    m_impl->readOnly = readOnly;
    m_impl->ctx.backend = m_impl->backend.get();
    m_impl->ctx.readOnly = readOnly;
}

Server::~Server() {
    stop();
}

bool Server::running() const {
    return m_impl && m_impl->running.load();
}

bool Server::start(std::string& error) {
    if (!m_impl) {
        error = "FUSE server is not initialized";
        return false;
    }
    if (m_impl->fuse || m_impl->loop.joinable()) {
        error = "FUSE server is already started";
        return false;
    }
    if (!m_impl->backend) {
        error = "FUSE backend is missing";
        return false;
    }

    struct stat before{};
    m_impl->mountPointExisted = ::lstat(m_mountPoint.c_str(), &before) == 0;
    if (!prepareMountPoint(m_mountPoint, runtimeRoot(), error)) return false;
    m_impl->prepared = true;
    auto cleanupMountPoint = [this]() {
        if (m_impl->prepared && !m_impl->mountPointExisted && !m_mountPoint.empty()) {
            if (::rmdir(m_mountPoint.c_str()) != 0 && errno != ENOENT && errno != ENOTEMPTY) {
                LOG_WARN("FUSE", std::string("rmdir(") + m_mountPoint + "): " +
                                     std::strerror(errno));
            }
        }
        m_impl->prepared = false;
    };

    fuse_args args = FUSE_ARGS_INIT(0, nullptr);
    if (fuse_opt_add_arg(&args, "fast-enough") != 0 ||
        fuse_opt_add_arg(&args, "-o") != 0) {
        fuse_opt_free_args(&args);
        error = "cannot allocate FUSE arguments";
        cleanupMountPoint();
        return false;
    }

    std::string options =
        "fsname=" + m_impl->fsName +
        ",subtype=fast-enough,default_permissions,auto_unmount,"
        "attr_timeout=1,entry_timeout=1,negative_timeout=0,max_read=1048576,"
        "nosuid,nodev";
    if (m_readOnly) options += ",ro";
    if (fuse_opt_add_arg(&args, options.c_str()) != 0) {
        fuse_opt_free_args(&args);
        error = "cannot allocate FUSE options";
        cleanupMountPoint();
        return false;
    }

    m_impl->fuse = fuse_new(&args, &fuseOps(), sizeof(fuse_operations), &m_impl->ctx);
    fuse_opt_free_args(&args);
    if (!m_impl->fuse) {
        error = std::string("fuse_new failed: ") + std::strerror(errno);
        cleanupMountPoint();
        return false;
    }
    if (fuse_mount(m_impl->fuse, m_mountPoint.c_str()) != 0) {
        error = std::string("fuse_mount(") + m_mountPoint + "): " + std::strerror(errno);
        fuse_destroy(m_impl->fuse);
        m_impl->fuse = nullptr;
        cleanupMountPoint();
        return false;
    }

    m_impl->running.store(true);
    m_impl->stopping.store(false);
    try {
        m_impl->loop = std::thread([impl = m_impl.get()]() {
            {
                const std::lock_guard<std::mutex> lock(impl->loopIdMutex);
                impl->loopId = std::this_thread::get_id();
            }
            const int rc = fuse_loop(impl->fuse);
            if (rc != 0) {
                LOG_WARN("FUSE", "fuse_loop ended with " + std::to_string(rc) +
                                     ": " + std::strerror(errno));
            }
            impl->running.store(false);
        });
    } catch (const std::system_error& e) {
        m_impl->running.store(false);
        fuse_unmount(m_impl->fuse);
        fuse_destroy(m_impl->fuse);
        m_impl->fuse = nullptr;
        cleanupMountPoint();
        error = std::string("cannot start FUSE loop thread: ") + e.what();
        return false;
    }
    return true;
}

void Server::stop() {
    if (!m_impl) return;
    {
        const std::lock_guard<std::mutex> lock(m_impl->loopIdMutex);
        if (m_impl->loopId == std::this_thread::get_id()) {
            // A FUSE callback must not join its own loop; callers stop from the
            // GUI/main thread. Leave the mount alone rather than deadlock.
            LOG_ERROR("FUSE", "stop() called from the FUSE loop thread; ignoring");
            return;
        }
    }
    if (m_impl->stopping.exchange(true)) return;

    if (m_impl->ctx.backend) m_impl->ctx.backend->interrupt();
    if (m_impl->fuse) {
        // fuse_exit() only sets a flag; the loop is normally blocked in
        // read(/dev/fuse). Unmounting closes the channel and wakes it. The
        // loop owns no resources that unmount needs, so this ordering is safe.
        fuse_exit(m_impl->fuse);
        fuse_unmount(m_impl->fuse);
    }
    if (m_impl->loop.joinable()) m_impl->loop.join();

    if (m_impl->fuse) {
        fuse_destroy(m_impl->fuse);
        m_impl->fuse = nullptr;
    }

    m_impl->ctx.backend = nullptr;
    m_impl->backend.reset();
    m_impl->running.store(false);

    if (m_impl->prepared && !m_impl->mountPointExisted && !m_mountPoint.empty()) {
        if (::rmdir(m_mountPoint.c_str()) != 0 && errno != ENOENT && errno != ENOTEMPTY) {
            LOG_WARN("FUSE", std::string("rmdir(") + m_mountPoint + "): " +
                                 std::strerror(errno));
        }
    }
    m_impl->prepared = false;
}

}  // namespace afm::fuse
