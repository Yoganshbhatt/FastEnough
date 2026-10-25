// FUSE backend for the Android device filesystem (Linux counterpart of
// device_dokan.cpp). Read-only in V1: every operation is served on demand via
// the existing range protocol; no local cache and no device writes.
//
// The mount owns a dedicated DeviceClient clone. That keeps unmount's
// interrupt() away from the GUI connection and avoids sharing transfer
// buffers with browsing/transfers.
#include "mount_stubs.h"

#if defined(AFM_HAVE_FUSE)

#include "fuse_common.h"

#include <sys/statvfs.h>

#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include "device_client.h"
#include "log_facade.h"

namespace {

class IoGuard {
public:
    explicit IoGuard(std::atomic<bool>& active) : m_active(active) {
        m_active.store(true);
    }
    ~IoGuard() { m_active.store(false); }
    IoGuard(const IoGuard&) = delete;
    IoGuard& operator=(const IoGuard&) = delete;

private:
    std::atomic<bool>& m_active;
};

std::string trimTrailingSlashes(std::string value) {
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

}  // namespace

class DeviceFuseBackend final : public afm::fuse::Backend {
public:
    DeviceFuseBackend(DeviceClient& source, std::string storageRoot,
                      std::atomic<bool>& ioActive)
        : m_root(trimTrailingSlashes(std::move(storageRoot))), m_ioActive(ioActive) {
        m_device = std::make_unique<DeviceClient>();
        if (!m_device->connectParallelFrom(source)) {
            m_error = source.lastError();
            if (m_error.empty()) m_error = "could not open a dedicated connection";
            m_device.reset();
            return;
        }
        m_valid = true;
        m_canWrite = (m_device->capabilities() & AFM_CAP_FUSE_WRITE) != 0;
    }

    bool valid() const { return m_valid; }
    bool canWrite() const { return m_canWrite; }
    const std::string& error() const { return m_error; }

    bool stat(const std::string& path, afm::fuse::Node& out, int& error) override {
        IoGuard guard(m_ioActive);
        if (!flushPath(path, error)) return false;
        DeviceFileEntry entry;
        std::string message;
        if (!m_device || !m_device->statPath(remotePath(path), entry, &message)) {
            error = afm::fuse::errnoFromMessage(message);
            return false;
        }
        out.type = entry.isDirectory() ? afm::fuse::NodeType::Directory
                                       : afm::fuse::NodeType::File;
        out.size = entry.size;
        out.mtime = entry.mtime;
        return true;
    }

    bool list(const std::string& path, std::vector<afm::fuse::DirEntry>& out,
              int& error) override {
        IoGuard guard(m_ioActive);
        if (!m_device) {
            error = EIO;
            return false;
        }
        if (!flushAll(error)) return false;
        const auto entries = m_device->listDirectory(remotePath(path));
        if (entries.empty()) {
            // An empty directory and a failed list both return no entries.
            // Verify the directory itself so a failed request is not shown
            // as an empty folder.
            DeviceFileEntry entry;
            std::string message;
            if (!m_device->statPath(remotePath(path), entry, &message) || !entry.isDirectory()) {
                error = afm::fuse::errnoFromMessage(message);
                return false;
            }
            out.clear();
            return true;
        }
        out.clear();
        out.reserve(entries.size());
        for (const auto& entry : entries) {
            if (entry.name.empty() || entry.name == "." || entry.name == "..") continue;
            if (entry.name.find('/') != std::string::npos) continue;
            afm::fuse::DirEntry item;
            item.name = entry.name;
            item.node.type = entry.isDirectory() ? afm::fuse::NodeType::Directory
                                                 : afm::fuse::NodeType::File;
            item.node.size = entry.size;
            item.node.mtime = entry.mtime;
            out.push_back(std::move(item));
        }
        return true;
    }

    bool read(const std::string& path, uint64_t offset, size_t size,
              std::vector<char>& out, int& error) override {
        IoGuard guard(m_ioActive);
        if (!m_device) {
            error = EIO;
            return false;
        }
        if (size == 0) {
            out.clear();
            return true;
        }
        if (!flushPath(path, error)) return false;
        // The protocol's range request is 32-bit bounded; the FUSE layer caps
        // requests at max_read=1 MiB, so this is defensive only.
        constexpr size_t kMaxRead = 4u * 1024u * 1024u;
        if (size > kMaxRead) size = kMaxRead;
        out.resize(size);
        bool readOk = false;
        const uint64_t got = m_device->readRangeWithStatus(remotePath(path), offset,
                                                           static_cast<uint64_t>(size), out.data(),
                                                           readOk);
        if (!readOk) {
            out.clear();
            error = afm::fuse::errnoFromMessage(m_device->lastError());
            return false;
        }
        if (got > size) {
            out.clear();
            error = EIO;
            return false;
        }
        if (got == 0) {
            // Clean EOF. FUSE expects 0 bytes, not an error, when a file
            // shrank or the kernel read past the current end.
            out.clear();
            return true;
        }
        out.resize(static_cast<size_t>(got));
        return true;
    }

    bool statfs(struct statvfs& out, int& error) override {
        IoGuard guard(m_ioActive);
        uint64_t total = 0;
        uint64_t freeBytes = 0;
        if (!m_device || !m_device->getDiskSpace(total, freeBytes)) {
            error = EIO;
            return false;
        }
        std::memset(&out, 0, sizeof(out));
        constexpr uint64_t kBlock = 4096;
        out.f_bsize = static_cast<unsigned long>(kBlock);
        out.f_frsize = static_cast<unsigned long>(kBlock);
        out.f_blocks = static_cast<fsblkcnt_t>(total / kBlock);
        out.f_bfree = static_cast<fsblkcnt_t>(freeBytes / kBlock);
        out.f_bavail = out.f_bfree;
        out.f_files = 0;
        out.f_ffree = 0;
        out.f_namemax = 255;
        return true;
    }

    bool truncate(const std::string& path, uint64_t size, int& error) override {
        IoGuard guard(m_ioActive);
        if (!m_device) {
            error = EIO;
            return false;
        }
        dropPath(path);
        if (!m_device->truncateFile(remotePath(path), size)) {
            error = afm::fuse::errnoFromMessage(m_device->lastError());
            return false;
        }
        return true;
    }

    bool create(const std::string& path, mode_t, int flags, int& error) override {
        IoGuard guard(m_ioActive);
        if (!m_device) {
            error = EIO;
            return false;
        }
        // Honor the caller's open(2) flags. O_CREAT without O_TRUNC on an
        // existing file must keep its contents; O_EXCL must still refuse.
        const bool truncate = (static_cast<unsigned>(flags) & O_TRUNC) != 0;
        const bool exclusive = (static_cast<unsigned>(flags) & O_EXCL) != 0;
        if (truncate) {
            dropPath(path);
        } else if (!flushPath(path, error)) {
            return false;
        }
        if (!m_device->createFileEx(remotePath(path), 0, truncate, exclusive)) {
            error = afm::fuse::errnoFromMessage(m_device->lastError());
            return false;
        }
        return true;
    }

    bool write(const std::string& path, uint64_t offset, const char* data, size_t size,
               int& error) override {
        IoGuard guard(m_ioActive);
        if (!m_device) {
            error = EIO;
            return false;
        }
        // Coalesce sequential writes into one device request. Without this a
        // 128 KiB-at-a-time writer pays a full round trip per block (~1 MB/s
        // on WiFi ADB); with it the device sees one request per 4 MiB.
        constexpr size_t kFlushThreshold = 4u * 1024u * 1024u;
        auto& pending = m_pending[path];
        const bool sequential = !pending.data.empty() &&
                                offset == pending.start + pending.data.size();
        if (!pending.data.empty() && !sequential) {
            if (!flushPath(path, error)) return false;
        }
        if (pending.data.empty()) pending.start = offset;
        pending.data.append(data, size);
        if (pending.data.size() >= kFlushThreshold) {
            if (!flushPath(path, error)) return false;
        }
        return true;
    }

    bool unlink(const std::string& path, int& error) override {
        IoGuard guard(m_ioActive);
        dropPath(path);
        if (!m_device || !m_device->unlinkFile(remotePath(path))) {
            error = m_device ? afm::fuse::errnoFromMessage(m_device->lastError()) : EIO;
            return false;
        }
        return true;
    }

    bool mkdir(const std::string& path, mode_t, int& error) override {
        IoGuard guard(m_ioActive);
        if (!m_device || !m_device->createDirectory(remotePath(path))) {
            error = m_device ? afm::fuse::errnoFromMessage(m_device->lastError()) : EIO;
            return false;
        }
        return true;
    }

    bool rmdir(const std::string& path, int& error) override {
        IoGuard guard(m_ioActive);
        if (!m_device || !m_device->rmdirDirectory(remotePath(path))) {
            error = m_device ? afm::fuse::errnoFromMessage(m_device->lastError()) : EIO;
            return false;
        }
        return true;
    }

    bool rename(const std::string& oldPath, const std::string& newPath, bool replace,
                int& error) override {
        IoGuard guard(m_ioActive);
        dropPath(oldPath);
        dropPath(newPath);
        if (!m_device ||
            !m_device->renameFileEx(remotePath(oldPath), remotePath(newPath), replace)) {
            error = m_device ? afm::fuse::errnoFromMessage(m_device->lastError()) : EIO;
            return false;
        }
        return true;
    }

    bool flush(const std::string& path, int& error) override {
        IoGuard guard(m_ioActive);
        return flushPath(path, error);
    }

    // Dokan's FsSetFileTime accepts timestamps silently; the wire protocol has
    // no utime command, so mirror that instead of failing touch(1)/cp -p.
    bool utimens(const std::string&, int& error) override {
        (void)error;
        return true;
    }

    void interrupt() override {
        if (m_device) m_device->interruptBlocking();
    }

private:
    struct PendingWrite {
        uint64_t start = 0;
        std::string data;
    };

    bool flushPath(const std::string& path, int& error) {
        auto it = m_pending.find(path);
        if (it == m_pending.end()) return true;
        const PendingWrite pending = std::move(it->second);
        m_pending.erase(it);
        if (pending.data.empty()) return true;
        if (!m_device) {
            error = EIO;
            return false;
        }
        constexpr size_t kMaxWrite = 4u * 1024u * 1024u;
        size_t done = 0;
        while (done < pending.data.size()) {
            const size_t chunk = std::min(pending.data.size() - done, kMaxWrite);
            const uint64_t written = m_device->writeRange(
                remotePath(path), pending.start + done, pending.data.data() + done,
                static_cast<uint32_t>(chunk));
            if (written != chunk) {
                error = afm::fuse::errnoFromMessage(m_device->lastError());
                return false;
            }
            done += chunk;
        }
        return true;
    }

    bool flushAll(int& error) {
        while (!m_pending.empty()) {
            const std::string path = m_pending.begin()->first;
            if (!flushPath(path, error)) return false;
        }
        return true;
    }

    void dropPath(const std::string& path) { m_pending.erase(path); }

    std::string remotePath(const std::string& path) const {
        if (path == "/" || path.empty()) return m_root;
        if (m_root == "/") return path;
        return m_root + path;
    }

    std::string m_root;
    std::atomic<bool>& m_ioActive;
    std::unique_ptr<DeviceClient> m_device;
    std::unordered_map<std::string, PendingWrite> m_pending;
    std::string m_error;
    bool m_valid = false;
    bool m_canWrite = false;
};

struct DeviceMountImpl {
    std::unique_ptr<afm::fuse::Server> server;
};

DeviceMountManager& DeviceMountManager::instance(int slot) {
    static std::mutex s_mutex;
    static std::vector<std::unique_ptr<DeviceMountManager>> s_instances;
    const std::lock_guard<std::mutex> lock(s_mutex);
    const size_t index = static_cast<size_t>(slot < 0 ? 0 : slot);
    if (s_instances.size() <= index) s_instances.resize(index + 1);
    auto& slotRef = s_instances[index];
    if (!slotRef) {
        slotRef.reset(new DeviceMountManager());
        slotRef->m_slot = slot;
    }
    return *slotRef;
}

DeviceMountManager::~DeviceMountManager() {
    unmount();
}

std::string DeviceMountManager::availableMountPoint() {
    return "";  // no drive letters on Linux
}

std::string DeviceMountManager::availableMountPoint(int slot) {
    return afm::fuse::slotMountPoint(slot);
}

bool DeviceMountManager::mount(DeviceClient* device, const std::string& storageRoot,
                               const std::string& mountPoint, bool writable) {
    if (!device) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_mounted.load() || m_impl) return false;
    if (storageRoot.empty() || mountPoint.empty()) return false;

    auto backend = std::make_unique<DeviceFuseBackend>(*device, storageRoot, ioActive);
    if (!backend->valid()) {
        LOG_ERROR("FUSE", "Cannot create device backend: " + backend->error());
        return false;
    }
    if (writable && !backend->canWrite()) {
        LOG_ERROR("FUSE", "Write mount requested, but the device helper predates "
                          "the write protocol (rebuild afm-server)");
        return false;
    }

    auto server = std::make_unique<afm::fuse::Server>(
        std::move(backend), mountPoint,
        "FastEnough-" + std::to_string(m_slot < 0 ? 0 : m_slot), !writable);
    std::string error;
    if (!server->start(error)) {
        LOG_ERROR("FUSE", "Cannot mount " + mountPoint + ": " + error);
        return false;
    }

    auto impl = std::make_shared<DeviceMountImpl>();
    impl->server = std::move(server);
    m_impl = std::move(impl);
    m_mountPoint = mountPoint;
    m_mounted.store(true);
    LOG_INFO("FUSE", "Mounted " + m_mountPoint + (writable ? " (read-write)" : " (read-only)"));
    return true;
}

void DeviceMountManager::unmount() {
    std::shared_ptr<DeviceMountImpl> impl;
    std::string mountPoint;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        impl = std::move(m_impl);
        mountPoint = std::move(m_mountPoint);
        m_mounted.store(false);
        m_mountPoint.clear();
    }
    if (impl && impl->server) {
        impl->server->stop();
        LOG_INFO("FUSE", "Unmounted " + mountPoint);
    }
}

#endif  // AFM_HAVE_FUSE
