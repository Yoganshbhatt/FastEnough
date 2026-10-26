// Linux system tray implementation.
//
// Neither library is a link-time dependency: libgtk-3 is resolved through
// src/gtk_api.cpp and libayatana-appindicator (or Fedora's libappindicator)
// through dlopen below. A system without either library still runs the GUI;
// the tray simply reports itself unavailable and closing the window quits.
#include "tray_linux.h"

#if defined(__linux__)

#include <dlfcn.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

#include <GLFW/glfw3.h>

#include "gtk_api.h"
#include "log_facade.h"

// Opaque StatusNotifier types and the enum values used here, declared locally
// so neither libayatana headers nor a link-time dependency are needed.
struct _AppIndicator;
using AppIndicator = _AppIndicator;
static constexpr int kCategoryApplicationStatus = 0;
static constexpr int kStatusPassive = 0;
static constexpr int kStatusActive = 1;

namespace tray {
namespace {

using NewWithPathFn = AppIndicator* (*)(const char*, const char*, int, const char*);
using SetStatusFn = void (*)(AppIndicator*, int);
using SetMenuFn = void (*)(AppIndicator*, void*);
using SetTitleFn = void (*)(AppIndicator*, const char*);

struct Api {
    void* handle = nullptr;
    NewWithPathFn newWithPath = nullptr;
    SetStatusFn setStatus = nullptr;
    SetMenuFn setMenu = nullptr;
    SetTitleFn setTitle = nullptr;
};

Api g_api;
AppIndicator* g_indicator = nullptr;
GtkWidget* g_menu = nullptr;
std::thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<bool> g_showRequested{false};
std::atomic<bool> g_exitRequested{false};

// Ubuntu/Debian and Arch ship libayatana; Fedora ships the older soname.
bool loadApi() {
    const char* candidates[] = {
        "libayatana-appindicator3.so.1",
        "libayatana-appindicator3.so.0",
        "libappindicator3.so.1",
    };
    for (const char* name : candidates) {
        void* handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (!handle) continue;
        g_api.handle = handle;
        g_api.newWithPath = reinterpret_cast<NewWithPathFn>(
            dlsym(handle, "app_indicator_new_with_path"));
        g_api.setStatus = reinterpret_cast<SetStatusFn>(
            dlsym(handle, "app_indicator_set_status"));
        g_api.setMenu = reinterpret_cast<SetMenuFn>(
            dlsym(handle, "app_indicator_set_menu"));
        g_api.setTitle = reinterpret_cast<SetTitleFn>(
            dlsym(handle, "app_indicator_set_title"));
        if (g_api.newWithPath && g_api.setStatus && g_api.setMenu && g_api.setTitle)
            return true;
        dlclose(handle);
        g_api = Api{};
    }
    return false;
}

void onShow(GtkMenuItem*, void*) {
    g_showRequested.store(true);
    glfwPostEmptyEvent();
}

void onExit(GtkMenuItem*, void*) {
    g_exitRequested.store(true);
    glfwPostEmptyEvent();
}

// The appindicator theme path needs a directory containing an icon named
// fast-enough.png. The packaged icon may live anywhere, so materialize it in
// the user cache once per start.
std::string prepareIcon(const std::string& source) {
    if (source.empty()) return {};
    const char* cacheHome = std::getenv("XDG_CACHE_HOME");
    std::string base = (cacheHome && *cacheHome)
        ? cacheHome
        : std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.cache";
    if (base.empty()) return {};
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::path(base) / "FastEnough" / "icons";
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};
    const std::filesystem::path dest = dir / "fast-enough.png";
    std::filesystem::copy_file(source, dest,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return {};
    return dir.string();
}

// Hiding a window with no icon would strand the user: require a registered
// StatusNotifier host before reporting success.
bool trayHostPresent() {
    _GError* error = nullptr;
    GDBusConnection* bus = afm::gtk::busGetSync(afm::gtk::kBusTypeSession, nullptr, &error);
    if (bus == nullptr) {
        if (error) afm::gtk::errorFree(error);
        return false;
    }
    _GVariant* params = afm::gtk::variantNewSS("org.kde.StatusNotifierWatcher",
                                               "IsStatusNotifierHostRegistered");
    _GVariant* reply = afm::gtk::dbusConnectionCallSync(
        bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.freedesktop.DBus.Properties", "Get", params,
        "(v)" /* G_VARIANT_TYPE("(v)") */, afm::gtk::kDbusCallFlagsNone, 2000, nullptr,
        &error);
    bool present = false;
    if (reply != nullptr) {
        _GVariant* inner = nullptr;
        afm::gtk::variantGetInner(reply, &inner);
        if (inner != nullptr) {
            present = afm::gtk::variantGetBoolean(inner) != 0;
            afm::gtk::variantUnref(inner);
        }
        afm::gtk::variantUnref(reply);
    }
    if (error) afm::gtk::errorFree(error);
    afm::gtk::objectUnref(bus);
    return present;
}

int setStatusOnGtkThread(void* data) {
    if (g_indicator)
        g_api.setStatus(g_indicator,
                        reinterpret_cast<intptr_t>(data) != 0 ? kStatusActive : kStatusPassive);
    return afm::gtk::kSourceRemove;
}

}  // namespace

bool start(const std::string& iconFile) {
    if (g_running.load()) return true;
    if (!afm::gtk::ensureLoaded()) {
        LOG_INFO("Tray", afm::gtk::loadError());
        return false;
    }
    if (!trayHostPresent()) {
        LOG_INFO("Tray", "No StatusNotifier host; close will quit normally");
        return false;
    }
    if (!loadApi()) {
        LOG_INFO("Tray", "libayatana-appindicator not found; close will quit normally");
        return false;
    }
    const std::string iconDir = prepareIcon(iconFile);
    auto promise = std::make_shared<std::promise<bool>>();
    std::future<bool> ready = promise->get_future();
    g_thread = std::thread([promise, iconDir]() {
        int argc = 0;
        char** argv = nullptr;
        if (!afm::gtk::initCheck(&argc, &argv)) {
            promise->set_value(false);
            return;
        }
        g_indicator = g_api.newWithPath(
            "fast-enough", "fast-enough", kCategoryApplicationStatus,
            iconDir.empty() ? nullptr : iconDir.c_str());
        if (!g_indicator) {
            promise->set_value(false);
            return;
        }
        g_api.setTitle(g_indicator, "Fast Enough? - Android File Explorer");

        g_menu = afm::gtk::menuNew();
        GtkWidget* showItem = afm::gtk::menuItemNewWithLabel("Show");
        GtkWidget* separator = afm::gtk::separatorMenuItemNew();
        GtkWidget* exitItem = afm::gtk::menuItemNewWithLabel("Exit");
        afm::gtk::signalConnect(showItem, "activate", onShow, nullptr);
        afm::gtk::signalConnect(exitItem, "activate", onExit, nullptr);
        afm::gtk::menuShellAppend(reinterpret_cast<GtkMenuShell*>(g_menu), showItem);
        afm::gtk::menuShellAppend(reinterpret_cast<GtkMenuShell*>(g_menu), separator);
        afm::gtk::menuShellAppend(reinterpret_cast<GtkMenuShell*>(g_menu), exitItem);
        afm::gtk::widgetShowAll(g_menu);
        g_api.setMenu(g_indicator, g_menu);
        g_api.setStatus(g_indicator, kStatusActive);
        g_running.store(true);
        promise->set_value(true);

        afm::gtk::mainLoop();

        g_running.store(false);
        g_indicator = nullptr;
        g_menu = nullptr;
    });

    const bool ok = ready.get();
    if (!ok) {
        if (g_thread.joinable()) g_thread.join();
        LOG_WARN("Tray", "Could not start the system tray; close will quit normally");
    }
    return ok;
}

void setVisible(bool visible) {
    if (!g_running.load()) return;
    afm::gtk::idleAdd(setStatusOnGtkThread, reinterpret_cast<void*>(visible ? 1 : 0));
}

void stop() {
    if (!g_thread.joinable()) return;
    afm::gtk::idleAdd([](void*) -> int {
        afm::gtk::mainQuit();
        return afm::gtk::kSourceRemove;
    }, nullptr);
    g_thread.join();
    if (g_api.handle) {
        dlclose(g_api.handle);
        g_api = Api{};
    }
}

bool consumeShowRequest() { return g_showRequested.exchange(false); }
bool consumeExitRequest() { return g_exitRequested.exchange(false); }

}  // namespace tray

#endif  // defined(__linux__)
