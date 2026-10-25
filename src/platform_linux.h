// Linux platform layer for the shared GUI code (app.cpp).
// Windows keeps its inline Win32 code (guarded by #if defined(_WIN32) at the
// call sites); this header declares the Linux replacements with the same
// signatures/semantics so call sites stay one-line switches.
#pragma once
#if defined(__linux__)

#include <cstdint>
#include <string>
#include <vector>

#include "app.h"  // DetectedNic (shared struct)

// NIC enumeration for the pairing panel (getifaddrs-based); uses the shared
// DetectedNic struct from app.h.

namespace platform {

// OS light/dark preference. Reads the gsettings color-scheme key (freedesktop
// "prefer-dark"); falls back to dark (matches ImGui default and GTK default).
bool systemUsesLightAppTheme();

// SHA-256 of a local file (OpenSSL-backed streaming hash).
bool computeLocalSha256(const std::string& path, std::string& outSha256, std::string& detail);

// Local-file deletion. permanent=false moves files to the freedesktop Trash
// via `gio trash` (recoverable); permanent=true unlinks directly. Returns
// false with error text on failure; a failed trash move leaves the file in
// place rather than falling back to deletion.
bool deleteLocalFiles(const std::vector<std::string>& paths, bool permanent, std::string& error);

// Refresh the desktop-entry/MIME cache after writing a .desktop file.
// argv-based; a missing update-desktop-database is logged, not fatal.
bool refreshDesktopDatabase(const std::string& applicationsDir);

// Shell open: xdg-open for files/URLs; directories open in the file manager.
bool shellOpen(const std::string& pathOrUri, std::string& error);

// One-shot 1s TCP reachability probe (non-blocking connect + poll).
bool tcpProbe(const std::string& ip, int port);

// Enumerate non-loopback IPv4 interfaces (Ethernet/WLAN) for the pairing UI.
std::vector<DetectedNic> enumerateNics();

// Reachability of the loopback ADB server is always true; kept for parity.
bool adbServerProbe();

// 1s TCP probe used by the multi-channel upgrade path.
bool probeReachable(const std::string& ip, int port);

}  // namespace platform
#endif  // __linux__
