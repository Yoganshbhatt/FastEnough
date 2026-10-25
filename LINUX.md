# Fast Enough on Linux

The client in this repository builds natively on Linux from the same sources as the
Windows build. The Android helper (`server/afm-server`) is unchanged and shared.

This page covers Linux-specific installation, behaviour and quirks. The general
feature description in [README.md](README.md) applies unchanged.

## What works

Everything the Windows client does, with the platform substitutions listed
[below](#platform-substitutions): ADB discovery over USB and wireless debugging,
dual-pane browsing, push/pull, resume, ranged reads, CRC32 and SHA-256
verification, device-to-device relay, archive/extract, APK install, backup and
restore, themes and scaling, activity log, system tray, Trash and packaging.

## Requirements

Runtime:

* A desktop with OpenGL 3.2 (Wayland or X11).
* `adb` on `PATH`, in `~/Android/Sdk/platform-tools/`, or bundled with the
  release tarball (looked up next to the executable).
* `fusermount3` and access to `/dev/fuse` for the virtual drive (FUSE).
* Optional: GTK3 for the tray icon, `zenity` for native file dialogs,
  `gio` for Trash.

Build:

* CMake 3.20+, a C++20 compiler (GCC 13+ or Clang 16+), `pkg-config`.
* GLFW 3.4+, OpenGL and OpenSSL headers.
* `libfuse3` headers (optional; without them the mount action reports a clean
  error), GTK3 headers (optional; enables the tray).

On Arch/CachyOS:

```bash
sudo pacman -S --needed cmake gcc pkgconf glfw openssl gtk3 fuse3 zenity
```

On Debian/Ubuntu:

```bash
sudo apt install build-essential cmake pkg-config libglfw3-dev libgl1-mesa-dev \
                 libssl-dev libfuse3-dev libgtk-3-dev zenity
```

## Build and run

```bash
./setup.sh                                   # fetch Dear ImGui into thirdparty/imgui
cmake -S . -B build                          # afm-cli + test suites
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

cmake -S . -B build-gui -DAFM_BUILD_GUI=ON   # GUI
cmake --build build-gui -j"$(nproc)"
./build-gui/fast-enough
```

`afm-cli` is a headless companion to the GUI: it talks to the same helper over
its own adb forward and is useful for scripted transfers and for verifying a
connection without the GUI.

## Install

Release tarballs contain the GUI, `afm-cli`, the ARM64 helper, the bundled
Android platform tools, a `.desktop` entry and the icon.

```bash
tar -xzf fast-enough-<version>-linux-x86_64.tar.gz
cd fast-enough-<version>-linux-x86_64
./install.sh                 # installs into ~/.local
./install.sh /usr/local      # or system-wide (with sudo)
```

Installing creates `~/.local/bin/fast-enough` and `~/.local/bin/afm-cli`, keeps
the real binaries in `~/.local/lib/fast-enough/` so the bundled `platform-tools`
stays next to the executable, and registers the desktop entry. The app then
appears in the launcher as **Fast Enough?** and can also be started from a
terminal. Running the binary directly from the extracted directory works too.

To build a tarball yourself:

```bash
./scripts/package-release.sh --version v1.0.16
```

## The phone drive (FUSE)

Windows mounts the phone as a drive letter through Dokan. Linux mounts it as a
directory through FUSE, because there are no drive letters:

```
$XDG_RUNTIME_DIR/fast-enough/slot-0
```

Mount it from **Connection → Mount …**. It mounts read-only by default; a
separate **read-write** entry is offered as a deliberate opt-in. The mount is
private to your user (no root, no `allow_other`), and the mount point is removed
when you unmount or quit the application.

Unmount from **Connection → Unmount**, or by quitting the app. If a process
crashes while files are open, the mount is detached automatically; the manual
recovery is:

```bash
fusermount3 -u "$XDG_RUNTIME_DIR/fast-enough/slot-0"
```

Deleting files through the mount is permanent — Android has no Trash for this
path. Inside the app's own file panes, Delete uses the XDG Trash by default
(Shift+Delete is permanent).

## MCRAW recordings

A MotionCam `.mcraw` file is exposed as a read-only virtual folder when opened
from the app: `metadata.json`, one `frame_NNNNNN.dng` per frame, and `audio.wav`
when the recording contains audio. Frames are decoded on first access and cached
in `~/.cache/FastEnough/`; the folder is unmounted when you leave it.

This replaces Windows' ProjFS-based view; the user-visible result is the same.

## Platform substitutions

| Windows | Linux | Notes |
|---|---|---|
| Dokan virtual drive (`P:\`) | FUSE mount point | same read/write file operations |
| ProjFS virtual folder | FUSE virtual folder | frames decoded on demand |
| OLE drag-and-drop out | **Export to Folder…** | context menu action; see limitations |
| `ITaskbarList3` progress | — | no desktop-neutral equivalent |
| Explorer integration | `.desktop` entry + `xdg-open` | APK files open with the app |
| Registry file associations | XDG MIME association | created on first run |
| `ShellExecute` | `xdg-open` | |
| Win32 file dialogs | zenity | fails closed when zenity is absent |
| `SHGetFolderPath` | XDG base directories | |
| Taskbar tray (`Shell_NotifyIcon`) | StatusNotifier item | needs a StatusNotifier host |
| Recycle Bin | XDG Trash (`gio trash`) | |

## Known limitations

* **Dragging files out** of the app into a file manager is not implemented.
  XDND has no equivalent of Windows' virtual-file streaming, so the app offers
  **Export to Folder…** instead, which copies the selection through the normal
  transfer pipeline (with checksum verification) and then opens the target.
* **Taskbar progress** is not shown; there is no portable Linux API for it.
* **Window position** is not restored on Wayland (size is); compositors own
  window placement.
* **Global hotkeys** are not available.
* **Wireless debugging pairing** uses `adb pair`; the legacy `adb tcpip` path
  is only used when the device is not paired.
* **USB and wireless** both work; the app keeps ADB forwarding as the default
  transport because it rides the authenticated adb channel and does not expose
  the helper's port on the network. Direct TCP is available for cases where the
  LAN route is faster, but the helper's port is not authenticated, so only use
  it on a trusted network.
* **Filenames** containing `"` are rejected by Android itself, on both platforms.

## Troubleshooting

* *Mount fails with "helper must be rebuilt"* — the helper binary on the device
  predates the writable-mount commands. Reconnect so the app pushes the bundled
  helper, or push `server/afm-server` manually.
* *Mount fails with a permission error* — check that `/dev/fuse` exists and that
  `fusermount3` is installed.
* *No tray icon* — the desktop needs a StatusNotifier host (KDE, GNOME with the
  AppIndicator extension, Waybar with a tray module). Without one, the app
  behaves normally and simply closes on exit.
* *No native dialogs* — install `zenity`; the app reports the failure instead of
  silently using a fallback.
* *Wrong icon or window grouping* — the binary must be built against GLFW 3.4 or
  newer for the Wayland application id to be set.
