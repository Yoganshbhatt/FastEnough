#pragma once
// Linux system tray (StatusNotifierItem via libayatana-appindicator).
// Mirrors the Windows tray behavior: shown only while the window is hidden,
// with "Show" and "Exit" menu items. All GTK work runs on its own thread;
// menu actions post requests that the GLFW main loop consumes.
#include <string>

namespace tray {

// Starts the tray thread and returns true once an item is registered with a
// StatusNotifierWatcher. False when GTK/appindicator is unavailable or no
// tray host is running, so callers can fall back to a normal window close.
bool start(const std::string& iconFile);

// Display or withdraw the icon (does not stop the thread).
void setVisible(bool visible);

// Stops the GTK loop and joins the thread. Safe to call when not started.
void stop();

// Menu actions, consumed by the main loop.
bool consumeShowRequest();
bool consumeExitRequest();

}  // namespace tray
