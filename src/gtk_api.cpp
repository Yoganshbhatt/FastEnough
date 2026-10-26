#include "gtk_api.h"

#if defined(__linux__)

#include <dlfcn.h>

#include <cstdlib>
#include <mutex>

namespace {

// GTK types, spelled out exactly as the ABI expects. gboolean/gint are int,
// gpointer is void*, GCallback is a generic function pointer.
using GtkWidgetPtr = ::GtkWidget*;
using GtkMenuShellPtr = ::GtkMenuShell*;
using GtkMenuItemPtr = ::GtkMenuItem*;

struct Table {
    // GTK
    int (*initCheck)(int*, char***) = nullptr;
    GtkWidgetPtr (*menuNew)() = nullptr;
    GtkWidgetPtr (*menuItemNewWithLabel)(const char*) = nullptr;
    GtkWidgetPtr (*separatorMenuItemNew)() = nullptr;
    void (*menuShellAppend)(GtkMenuShellPtr, GtkWidgetPtr) = nullptr;
    void (*widgetShowAll)(GtkWidgetPtr) = nullptr;
    void (*mainLoop)() = nullptr;
    void (*mainQuit)() = nullptr;

    // GObject / GLib (reached through libgtk's dependencies)
    unsigned (*idleAdd)(int (*)(void*), void*) = nullptr;
    void* (*signalConnectData)(void*, const char*, void (*)(void), void*, void*, int) = nullptr;
    void (*objectUnref)(void*) = nullptr;
    void (*errorFree)(GError*) = nullptr;

    // GIO / GLib
    GDBusConnection* (*busGetSync)(int, void*, GError**) = nullptr;
    GVariant* (*connectionCallSync)(GDBusConnection*, const char*, const char*,
                                      const char*, const char*, GVariant*, const void*,
                                      int, int, void*, GError**) = nullptr;
    GVariant* (*variantNew)(const char*, ...) = nullptr;
    void (*variantGet)(GVariant*, const char*, ...) = nullptr;
    int (*variantGetBoolean)(GVariant*) = nullptr;
    void (*variantUnref)(GVariant*) = nullptr;
};

Table g_table;
void* g_handle = nullptr;
std::string g_error;
bool g_ready = false;
std::once_flag g_once;

template <typename Slot>
bool bindSymbol(Slot& slot, const char* name) {
    void* symbol = ::dlsym(g_handle, name);
    if (symbol == nullptr) {
        g_error = std::string("libgtk-3 is missing symbol ") + name;
        return false;
    }
    slot = reinterpret_cast<Slot>(symbol);
    return true;
}

bool bindAll() {
    // dlsym on the libgtk handle also resolves symbols from its dependencies
    // (libgobject, libglib, libgio), which is why one handle is enough.
    return bindSymbol(g_table.initCheck, "gtk_init_check") &&
           bindSymbol(g_table.menuNew, "gtk_menu_new") &&
           bindSymbol(g_table.menuItemNewWithLabel, "gtk_menu_item_new_with_label") &&
           bindSymbol(g_table.separatorMenuItemNew, "gtk_separator_menu_item_new") &&
           bindSymbol(g_table.menuShellAppend, "gtk_menu_shell_append") &&
           bindSymbol(g_table.widgetShowAll, "gtk_widget_show_all") &&
           bindSymbol(g_table.mainLoop, "gtk_main") &&
           bindSymbol(g_table.mainQuit, "gtk_main_quit") &&
           bindSymbol(g_table.idleAdd, "g_idle_add") &&
           bindSymbol(g_table.signalConnectData, "g_signal_connect_data") &&
           bindSymbol(g_table.objectUnref, "g_object_unref") &&
           bindSymbol(g_table.errorFree, "g_error_free") &&
           bindSymbol(g_table.busGetSync, "g_bus_get_sync") &&
           bindSymbol(g_table.connectionCallSync, "g_dbus_connection_call_sync") &&
           bindSymbol(g_table.variantNew, "g_variant_new") &&
           bindSymbol(g_table.variantGet, "g_variant_get") &&
           bindSymbol(g_table.variantGetBoolean, "g_variant_get_boolean") &&
           bindSymbol(g_table.variantUnref, "g_variant_unref");
}

std::string candidate() {
    // Test hook: a missing path simulates a system without GTK.
    if (const char* override = std::getenv("AFM_GTK_LIB");
        override != nullptr && *override != '\0') {
        return override;
    }
    return "libgtk-3.so.0";
}

void load() {
    const std::string name = candidate();
    void* handle = ::dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        g_error = "libgtk-3 not found (" + name +
                  "); the tray icon is unavailable and closing quits the app";
        return;
    }
    g_handle = handle;
    if (bindAll()) {
        g_ready = true;
        return;
    }
    ::dlclose(handle);
    g_handle = nullptr;
}

}  // namespace

namespace afm::gtk {

bool ensureLoaded() {
    std::call_once(g_once, load);
    return g_ready;
}

const std::string& loadError() { return g_error; }

GtkWidget* menuNew() { return g_table.menuNew(); }

GtkWidget* menuItemNewWithLabel(const char* label) {
    return g_table.menuItemNewWithLabel(label);
}

GtkWidget* separatorMenuItemNew() { return g_table.separatorMenuItemNew(); }

void menuShellAppend(GtkMenuShell* shell, GtkWidget* child) {
    g_table.menuShellAppend(shell, child);
}

void widgetShowAll(GtkWidget* widget) { g_table.widgetShowAll(widget); }

bool initCheck(int* argc, char*** argv) { return g_table.initCheck(argc, argv) != 0; }

void mainLoop() { g_table.mainLoop(); }

void mainQuit() { g_table.mainQuit(); }

void* idleAdd(int (*callback)(void* data), void* data) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(g_table.idleAdd(callback, data)));
}

void* signalConnect(void* instance, const char* detailedSignal,
                    void (*handler)(GtkMenuItem*, void*), void* data) {
    return g_table.signalConnectData(instance, detailedSignal,
                                     reinterpret_cast<void (*)(void)>(handler), data, nullptr,
                                     0);
}

void objectUnref(void* object) { g_table.objectUnref(object); }

void errorFree(GError* error) { g_table.errorFree(error); }

GDBusConnection* busGetSync(int busType, void* cancellable, GError** error) {
    return g_table.busGetSync(busType, cancellable, error);
}

GVariant* dbusConnectionCallSync(GDBusConnection* connection, const char* busName,
                                  const char* objectPath, const char* interfaceName,
                                  const char* method, GVariant* parameters,
                                  const void* replyType, int flags, int timeoutMsec,
                                  void* cancellable, GError** error) {
    return g_table.connectionCallSync(connection, busName, objectPath, interfaceName, method,
                                      parameters, replyType, flags, timeoutMsec, cancellable,
                                      error);
}

GVariant* variantNewSS(const char* first, const char* second) {
    return g_table.variantNew("(ss)", first, second);
}

void variantGetInner(GVariant* value, GVariant** inner) {
    g_table.variantGet(value, "(v)", inner);
}

int variantGetBoolean(GVariant* value) { return g_table.variantGetBoolean(value); }

void variantUnref(GVariant* value) { g_table.variantUnref(value); }

}  // namespace afm::gtk

#endif  // defined(__linux__)
