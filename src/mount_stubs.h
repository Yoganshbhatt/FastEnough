// Linux mount managers.
//
// The device mount is implemented by fs_fuse.cpp when libfuse3 is available
// (AFM_HAVE_FUSE); otherwise mount_stubs.cpp keeps the feature disabled with
// a clear message. The MCRAW manager remains a stub until its own FUSE view
// lands. app.cpp uses this header on Linux only; Windows keeps device_dokan.h
// and mcraw_projfs.h.
#pragma once
#if !defined(_WIN32) && !defined(FASTENOUGH_MCRAW_STUBS)
#define FASTENOUGH_MCRAW_STUBS

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace afm::fuse {
class Server;
}

// Minimal stand-in for the Windows McrawMount record type (mcraw_projfs.h),
// which pulls in ProjFS types. The Linux manager attaches a FUSE server.
class McrawMount {
public:
    const std::string& mcrawPath() const { return m_mcrawPath; }
    const std::string& mountPath() const { return m_mountPath; }
    std::string m_mcrawPath;
    std::string m_mountPath;
    std::shared_ptr<afm::fuse::Server> server;
};

class DeviceClient;  // mount() takes a pointer only
struct DeviceMountImpl;  // defined by fs_fuse.cpp when FUSE is enabled

class DeviceMountManager {
public:
    static DeviceMountManager& instance(int slot = 0);
    static std::string availableMountPoint();
#if defined(__linux__)
    // Pure path computation for a device slot; creates nothing.
    static std::string availableMountPoint(int slot);
#endif
    ~DeviceMountManager();

    // Mount device storage as a private FUSE directory. The manager clones
    // the source connection, so unmounting never touches the GUI connection.
    // Returns true when the mount is live.
    bool mount(DeviceClient* device, const std::string& storageRoot,
               const std::string& mountPoint, bool writable = false);
    void unmount();

    bool isMounted() const { return m_mounted; }
    // Copy under the lock: the GUI reads this every frame while the async
    // worker may be clearing it during unmount.
    std::string mountPoint() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_mountPoint;
    }
    int slot() const { return m_slot; }

    // For health check suppression
    std::atomic<bool> ioActive{false};

private:
    DeviceMountManager() = default;
    int m_slot = 0;
    mutable std::mutex m_mutex;
    std::string m_mountPoint;
    std::atomic<bool> m_mounted{false};
    std::shared_ptr<DeviceMountImpl> m_impl;
};

// McrawMountManager: same surface as the Windows ProjFS version; backed by a
// read-only FUSE view with on-demand materialization on Linux.
class McrawMountManager {
public:
    static McrawMountManager& instance();

    std::string mountMcraw(const std::string& mcrawPath);
    bool isInsideMount(const std::string& path) const;
    void unmountAll();

private:
    McrawMountManager() = default;
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<McrawMount>> m_mounts;
};

#endif  // !_WIN32
