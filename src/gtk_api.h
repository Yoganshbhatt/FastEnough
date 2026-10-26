// Runtime binding for GTK3.
//
// GTK is only needed for the StatusNotifier tray icon. Linking it directly
// makes the whole client refuse to start on systems without the GTK stack,
// even though browsing and transfers do not need it at all. The library is
// therefore loaded with dlopen() on first use, and the handful of types and
// entry points used below are declared here, so neither the GTK development
// headers nor a link-time dependency are required to build the client.
//
// When libgtk-3 is not installed, the tray simply reports itself unavailable
// and closing the window quits the application.
#pragma once

#if defined(__linux__)

#include <string>

// Opaque GTK/GIO types. Only pointers to them are passed around.
struct _GtkWidget;
using GtkWidget = _GtkWidget;
struct _GtkMenuShell;
using GtkMenuShell = _GtkMenuShell;
struct _GtkMenuItem;
using GtkMenuItem = _GtkMenuItem;
struct _GError;
using GError = _GError;
struct _GVariant;
using GVariant = _GVariant;
struct _GDBusConnection;
using GDBusConnection = _GDBusConnection;

namespace afm::gtk {

// Values the tray needs from GTK/GIO headers that are no longer included.
// GBusType: STARTER=-1, NONE=0, SYSTEM=1, SESSION=2.
inline constexpr int kBusTypeSession = 2;
inline constexpr int kDbusCallFlagsNone = 0;
inline constexpr int kSourceRemove = 0;

// Loads libgtk-3 on first use. Returns false and sets loadError() when the
// library is not present; every other function here must only be called after
// this returned true.
bool ensureLoaded();
const std::string& loadError();

// Widget construction and the menu surface used for the tray menu.
GtkWidget* menuNew();
GtkWidget* menuItemNewWithLabel(const char* label);
GtkWidget* separatorMenuItemNew();
void menuShellAppend(GtkMenuShell* shell, GtkWidget* child);
void widgetShowAll(GtkWidget* widget);

// Main loop ownership of the tray thread.
bool initCheck(int* argc, char*** argv);
void mainLoop();
void mainQuit();

// Source and signal plumbing.
void* idleAdd(int (*callback)(void* data), void* data);
void* signalConnect(void* instance, const char* detailedSignal,
                    void (*handler)(GtkMenuItem*, void*), void* data);

// Reference counting and errors.
void objectUnref(void* object);
void errorFree(GError* error);

// D-Bus probe for a StatusNotifier host.
GDBusConnection* busGetSync(int busType, void* cancellable, GError** error);
GVariant* dbusConnectionCallSync(GDBusConnection* connection, const char* busName,
                                  const char* objectPath, const char* interfaceName,
                                  const char* method, GVariant* parameters,
                                  const void* replyType, int flags, int timeoutMsec,
                                  void* cancellable, GError** error);
// Only the two variant shapes the tray needs, so no varargs forwarding is
// involved: build a "(ss)" tuple, and unwrap a "(v)" into its inner variant.
GVariant* variantNewSS(const char* first, const char* second);
void variantGetInner(GVariant* value, GVariant** inner);
int variantGetBoolean(GVariant* value);
void variantUnref(GVariant* value);

}  // namespace afm::gtk

#endif  // defined(__linux__)
