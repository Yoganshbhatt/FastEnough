// Linux system tray implementation. GTK3 is the only link-time dependency;
// libayatana-appindicator (or Fedora's libappindicator) is loaded at runtime,
// so a missing tray library degrades to a normal window close instead of
// preventing the GUI from starting. Without GTK3 headers at build time the
// stubs at the bottom keep the GUI linkable.
#include "tray_linux.h"

#if defined(AFM_HAVE_GTK)

#include <gtk/gtk.h>

#include <GLFW/glfw3.h>
#include <dlfcn.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <thread>

#include "log_facade.h"

// Opaque StatusNotifier types and the two enum values used here. Declared
// locally so neither libayatana headers nor a link-time dependency are needed.
struct _AppIndicator;
using AppIndicator = _AppIndicator;
static constexpr int kCategoryApplicationStatus = 0;
static constexpr int kStatusPassive = 0;
static constexpr int kStatusActive = 1;

namespace tray {
namespace {

using NewWithPathFn = AppIndicator* (*)(const gchar*, const gchar*, int, const gchar*);
using SetStatusFn = void (*)(AppIndicator*, int);
using SetMenuFn = void (*)(AppIndicator*, GtkMenu*);
using SetTitleFn = void (*)(AppIndicator*, const gchar*);

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

void onShow(GtkMenuItem*, gpointer) {
    g_showRequested.store(true);
    glfwPostEmptyEvent();
}

void onExit(GtkMenuItem*, gpointer) {
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
    GError* error = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!bus) {
        if (error) g_error_free(error);
        return false;
    }
    GVariant* reply = g_dbus_connection_call_sync(
        bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.kde.StatusNotifierWatcher",
                      "IsStatusNotifierHostRegistered"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
    bool present = false;
    if (reply) {
        GVariant* inner = nullptr;
        g_variant_get(reply, "(v)", &inner);
        if (inner) {
            present = g_variant_get_boolean(inner);
            g_variant_unref(inner);
        }
        g_variant_unref(reply);
    }
    if (error) g_error_free(error);
    g_object_unref(bus);
    return present;
}

gboolean setStatusOnGtkThread(gpointer data) {
    if (g_indicator)
        g_api.setStatus(g_indicator, GPOINTER_TO_INT(data) ? kStatusActive : kStatusPassive);
    return G_SOURCE_REMOVE;
}

}  // namespace

bool start(const std::string& iconFile) {
    if (g_running.load()) return true;
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
        if (!gtk_init_check(&argc, &argv)) {
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

        g_menu = gtk_menu_new();
        GtkWidget* showItem = gtk_menu_item_new_with_label("Show");
        GtkWidget* separator = gtk_separator_menu_item_new();
        GtkWidget* exitItem = gtk_menu_item_new_with_label("Exit");
        g_signal_connect(showItem, "activate", G_CALLBACK(onShow), nullptr);
        g_signal_connect(exitItem, "activate", G_CALLBACK(onExit), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), showItem);
        gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), separator);
        gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), exitItem);
        gtk_widget_show_all(g_menu);
        g_api.setMenu(g_indicator, GTK_MENU(g_menu));
        g_api.setStatus(g_indicator, kStatusActive);
        g_running.store(true);
        promise->set_value(true);

        gtk_main();

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
    g_idle_add(setStatusOnGtkThread, GINT_TO_POINTER(visible ? 1 : 0));
}

void stop() {
    if (!g_thread.joinable()) return;
    g_idle_add([](gpointer) -> gboolean {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
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

#else  // no GTK3 at build time

namespace tray {
bool start(const std::string&) { return false; }
void setVisible(bool) {}
void stop() {}
bool consumeShowRequest() { return false; }
bool consumeExitRequest() { return false; }
}  // namespace tray

#endif
