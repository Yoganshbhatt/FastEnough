// Linux fallback implementations for the mount managers (see mount_stubs.h).
// With FUSE enabled, fs_fuse.cpp provides the device manager; this file keeps
// only the MCRAW placeholder until its FUSE view lands.
#include "mount_stubs.h"

#include <unistd.h>

#include <cstdlib>
#include <memory>
#include <string>

#include "log_facade.h"

#if !defined(AFM_HAVE_FUSE)
DeviceMountManager& DeviceMountManager::instance(int slot) {
    static std::mutex s_mutex;
    static std::vector<std::unique_ptr<DeviceMountManager>> s_instances;
    const std::lock_guard<std::mutex> lock(s_mutex);
    if (s_instances.size() <= static_cast<size_t>(slot < 0 ? 0 : slot))
        s_instances.resize(static_cast<size_t>(slot < 0 ? 0 : slot) + 1);
    auto& slotRef = s_instances[static_cast<size_t>(slot < 0 ? 0 : slot)];
    if (!slotRef) {
        slotRef.reset(new DeviceMountManager());
        slotRef->m_slot = slot;
    }
    return *slotRef;
}

DeviceMountManager::~DeviceMountManager() = default;

std::string DeviceMountManager::availableMountPoint() {
    return "";  // no drive letters on Linux
}

std::string DeviceMountManager::availableMountPoint(int slot) {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    std::string base;
    if (xdg && xdg[0] == '/') {
        base = xdg;
        while (base.size() > 1 && base.back() == '/') base.pop_back();
        base += "/fast-enough";
    } else {
        base = "/tmp/fast-enough-" + std::to_string(::getuid());
    }
    return base + "/slot-" + std::to_string(slot < 0 ? 0 : slot);
}

bool DeviceMountManager::mount(DeviceClient*, const std::string&, const std::string&,
                               bool) {
    LOG_WARN("FUSE", "FUSE backend not built; install libfuse3 and rebuild the GUI");
    return false;
}

void DeviceMountManager::unmount() {
    m_mounted = false;
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_mountPoint.clear();
}
#endif  // !AFM_HAVE_FUSE

#if !defined(AFM_HAVE_FUSE)
McrawMountManager& McrawMountManager::instance() {
    static McrawMountManager manager;
    return manager;
}

std::string McrawMountManager::mountMcraw(const std::string& mcrawPath) {
    LOG_WARN("MCRAW", "ProjFS mount not yet available on Linux; use Extract instead ("
                          + mcrawPath + ")");
    return "";
}

bool McrawMountManager::isInsideMount(const std::string&) const {
    return false;
}

void McrawMountManager::unmountAll() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_mounts.clear();
}
#endif  // !AFM_HAVE_FUSE
