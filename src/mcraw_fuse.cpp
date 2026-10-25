// MCRAW virtual folder on Linux: a read-only FUSE view of a local MotionCam
// container. Entries are listed without decoding; each frame/audio/metadata
// file is materialized into a private cache only when read.
#include "mount_stubs.h"

#if defined(AFM_HAVE_FUSE)

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "fuse_common.h"
#include "mcraw_local.h"
#include "log_facade.h"

namespace {

class McrawBackend final : public afm::fuse::Backend {
public:
    McrawBackend(std::string mcrawPath, std::string cacheDir)
        : m_mcrawPath(std::move(mcrawPath)), m_cacheDir(std::move(cacheDir)) {
        struct stat st{};
        if (::stat(m_mcrawPath.c_str(), &st) == 0) m_mtime = static_cast<int64_t>(st.st_mtime);
        for (const auto& entry : listLocalMcraw(m_mcrawPath)) {
            Entry value;
            value.size = entry.size;
            // DNG size is an estimate until the frame is decoded. Reserve a
            // generous margin so the kernel never truncates a larger file.
            if (entry.name.rfind("frame_", 0) == 0 && entry.name.size() > 10 &&
                entry.name.compare(entry.name.size() - 4, 4, ".dng") == 0) {
                value.size += 1024 * 1024;
            }
            m_entries[entry.name] = value;
        }
    }

    bool hasEntries() const { return !m_entries.empty(); }

    bool stat(const std::string& path, afm::fuse::Node& out, int& error) override {
        if (path == "/") {
            out.type = afm::fuse::NodeType::Directory;
            out.size = 0;
            out.mtime = m_mtime;
            return true;
        }
        auto it = m_entries.find(path.substr(1));
        if (it == m_entries.end()) {
            error = ENOENT;
            return false;
        }
        out.type = afm::fuse::NodeType::File;
        out.size = actualSize(it->first, it->second.size);
        out.mtime = m_mtime;
        return true;
    }

    bool list(const std::string& path, std::vector<afm::fuse::DirEntry>& out,
              int& error) override {
        if (path != "/") {
            error = ENOTDIR;
            return false;
        }
        for (const auto& [name, entry] : m_entries) {
            afm::fuse::DirEntry item;
            item.name = name;
            item.node.type = afm::fuse::NodeType::File;
            item.node.size = actualSize(name, entry.size);
            item.node.mtime = m_mtime;
            out.push_back(std::move(item));
        }
        return true;
    }

    bool read(const std::string& path, uint64_t offset, size_t size,
              std::vector<char>& out, int& error) override {
        const std::string name = path.substr(1);
        if (name.empty() || name.find('/') != std::string::npos) {
            error = ENOENT;
            return false;
        }
        if (m_entries.find(name) == m_entries.end()) {
            error = ENOENT;
            return false;
        }
        const std::string cachePath = materialize(name, error);
        if (cachePath.empty()) return false;
        touch(name);

        struct stat st{};
        if (::stat(cachePath.c_str(), &st) != 0) {
            error = EIO;
            return false;
        }
        const uint64_t fileSize = static_cast<uint64_t>(st.st_size);
        if (offset >= fileSize) {
            out.clear();
            return true;
        }
        const size_t wanted = static_cast<size_t>(
            std::min<uint64_t>(size, fileSize - offset));
        std::ifstream stream(cachePath, std::ios::binary);
        if (!stream || !stream.seekg(static_cast<std::streamoff>(offset))) {
            error = EIO;
            return false;
        }
        out.resize(wanted);
        stream.read(out.data(), static_cast<std::streamsize>(wanted));
        if (static_cast<size_t>(stream.gcount()) != wanted) {
            error = EIO;
            return false;
        }
        return true;
    }

    bool statfs(struct statvfs& out, int& error) override {
        if (::statvfs(m_cacheDir.c_str(), &out) != 0) {
            error = errno;
            return false;
        }
        return true;
    }

private:
    struct Entry {
        uint64_t size = 0;
    };
    struct CacheEntry {
        uint64_t size = 0;
        uint64_t lastUse = 0;
    };

    static constexpr uint64_t kCacheLimitBytes = 256ull * 1024ull * 1024ull;

    uint64_t actualSize(const std::string& name, uint64_t estimate) const {
        const std::string cachePath = m_cacheDir + "/" + name;
        struct stat st{};
        if (::stat(cachePath.c_str(), &st) == 0 && st.st_size >= 0)
            return static_cast<uint64_t>(st.st_size);
        return estimate;
    }

    void touch(const std::string& name) {
        auto it = m_cache.find(name);
        if (it != m_cache.end()) it->second.lastUse = ++m_useClock;
    }

    void forget(const std::string& name) {
        auto it = m_cache.find(name);
        if (it == m_cache.end()) return;
        m_cacheBytes = it->second.size > m_cacheBytes ? 0 : m_cacheBytes - it->second.size;
        m_cache.erase(it);
    }

    // Drop least-recently-used materialized entries until the cache is under
    // its byte budget. The entry currently being read is never evicted.
    void evict(const std::string& keep) {
        while (m_cacheBytes > kCacheLimitBytes) {
            auto victim = m_cache.end();
            for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
                if (it->first == keep) continue;
                if (victim == m_cache.end() || it->second.lastUse < victim->second.lastUse)
                    victim = it;
            }
            if (victim == m_cache.end()) break;
            const std::string victimPath = m_cacheDir + "/" + victim->first;
            if (::unlink(victimPath.c_str()) == 0) {
                LOG_INFO("MCRAW", "Evicted decoded frame from cache: " + victim->first);
            }
            forget(victim->first);
        }
    }

    std::string materialize(const std::string& name, int& error) {
        const std::string cachePath = m_cacheDir + "/" + name;
        struct stat st{};
        if (::stat(cachePath.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            auto it = m_cache.find(name);
            if (it == m_cache.end()) {
                m_cache[name] = CacheEntry{static_cast<uint64_t>(st.st_size), ++m_useClock};
                m_cacheBytes += static_cast<uint64_t>(st.st_size);
            }
            touch(name);
            return cachePath;
        }
        if (::stat(cachePath.c_str(), &st) == 0) ::unlink(cachePath.c_str());
        forget(name);

        // Extract to a private temp name and publish with rename(), so an
        // interrupted decode can never be mistaken for a complete frame.
        const std::string tempPath = cachePath + ".part-" + std::to_string(::getpid()) +
                                     "-" + std::to_string(m_useClock++);
        if (!extractLocalMcrawItem(m_mcrawPath, name, tempPath)) {
            ::unlink(tempPath.c_str());
            error = EIO;
            return {};
        }
        const int fd = ::open(tempPath.c_str(), O_RDONLY);
        if (fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
        if (::rename(tempPath.c_str(), cachePath.c_str()) != 0) {
            const int saved = errno;
            ::unlink(tempPath.c_str());
            error = saved;
            return {};
        }
        if (::stat(cachePath.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            ::unlink(cachePath.c_str());
            error = EIO;
            return {};
        }
        const uint64_t size = static_cast<uint64_t>(st.st_size);
        m_cache[name] = CacheEntry{size, ++m_useClock};
        m_cacheBytes += size;
        evict(name);
        return cachePath;
    }

    std::string m_mcrawPath;
    std::string m_cacheDir;
    int64_t m_mtime = 0;
    std::map<std::string, Entry> m_entries;
    std::map<std::string, CacheEntry> m_cache;
    uint64_t m_cacheBytes = 0;
    uint64_t m_useClock = 0;
};

std::string uniqueMountPoint() {
    const std::string root = afm::fuse::runtimeRoot();
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::string candidate =
            root + "/mcraw-" + std::to_string(::getpid()) + "-" + std::to_string(attempt);
        std::string error;
        if (afm::fuse::prepareMountPoint(candidate, root, error)) return candidate;
        if (error.find("not empty") == std::string::npos) return {};
    }
    return {};
}

}  // namespace

McrawMountManager& McrawMountManager::instance() {
    static McrawMountManager manager;
    return manager;
}

std::string McrawMountManager::mountMcraw(const std::string& mcrawPath) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& mount : m_mounts) {
        if (mount && mount->m_mcrawPath == mcrawPath) return mount->m_mountPath;
    }

    const std::string mountPath = uniqueMountPoint();
    if (mountPath.empty()) {
        LOG_WARN("MCRAW", "could not create a private mount point");
        return {};
    }

    const std::string cacheDir = mountPath + ".cache";
    std::error_code ec;
    if (::mkdir(cacheDir.c_str(), 0700) != 0 && errno != EEXIST) {
        ::rmdir(mountPath.c_str());
        LOG_WARN("MCRAW", std::string("could not create the decode cache: ") +
                             std::strerror(errno));
        return {};
    }
    struct stat cacheStat{};
    if (::lstat(cacheDir.c_str(), &cacheStat) != 0 || !S_ISDIR(cacheStat.st_mode) ||
        cacheStat.st_uid != ::getuid() || (cacheStat.st_mode & 077) != 0) {
        std::filesystem::remove_all(cacheDir, ec);
        ::rmdir(mountPath.c_str());
        LOG_WARN("MCRAW", "decode cache is not a private directory");
        return {};
    }

    auto backend = std::make_unique<McrawBackend>(mcrawPath, cacheDir);
    if (!backend->hasEntries()) {
        std::filesystem::remove_all(cacheDir, ec);
        ::rmdir(mountPath.c_str());
        LOG_WARN("MCRAW", "not a readable MotionCam container: " + mcrawPath);
        return {};
    }
    auto server = std::make_shared<afm::fuse::Server>(
        std::move(backend), mountPath, "FastEnough-mcraw", true);
    std::string startError;
    if (!server->start(startError)) {
        std::filesystem::remove_all(cacheDir, ec);
        ::rmdir(mountPath.c_str());
        LOG_WARN("MCRAW", "FUSE mount failed: " + startError);
        return {};
    }

    auto mount = std::make_shared<McrawMount>();
    mount->m_mcrawPath = mcrawPath;
    mount->m_mountPath = mountPath;
    mount->server = server;
    m_mounts.push_back(mount);
    LOG_INFO("MCRAW", "Mounted " + mcrawPath + " at " + mountPath);
    return mountPath;
}

bool McrawMountManager::isInsideMount(const std::string& path) const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& mount : m_mounts) {
        if (mount && !mount->m_mountPath.empty() &&
            afm::fuse::isUnderRoot(mount->m_mountPath, path))
            return true;
    }
    return false;
}

void McrawMountManager::unmountAll() {
    std::vector<std::shared_ptr<McrawMount>> mounts;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        mounts.swap(m_mounts);
    }
    for (auto& mount : mounts) {
        if (mount && mount->server) mount->server->stop();
        if (mount) {
            std::error_code ec;
            std::filesystem::remove_all(mount->m_mountPath + ".cache", ec);
            ::rmdir(mount->m_mountPath.c_str());
        }
    }
}

#endif  // AFM_HAVE_FUSE
