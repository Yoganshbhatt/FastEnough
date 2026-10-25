// Linux entry point: GLFW + OpenGL3 ImGui shell for the shared App code.
// Mirrors src/main.cpp structure: single instance lock, window-state
// persistence, DPI scaling, crash log, file-drop -> App::handleExternalFileDrop,
// and close-to-tray via StatusNotifier when the user disables close
// confirmation. Windows-only features without a Linux counterpart yet:
// taskbar progress (no-op shims at the bottom) and OLE drag-out (replaced by
// the Android pane's "Export to Folder..." action). APK handoff uses the
// single-instance socket instead of WM_COPYDATA.
#include "app.h"
#include "single_instance_linux.h"
#include "tray_linux.h"
#include "ui_widgets.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

bool g_reallyQuit = false;  // defined in app.cpp (extern); set by the close confirmation

namespace {
// Hide-to-tray state, touched only on the GLFW main thread.
bool g_windowHidden = false;
std::string g_trayIconPath;
App* g_appForClose = nullptr;

std::string exeDirPath() {
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : exe.parent_path().string();
}

// Packaged layouts: tarball (icon under share/ next to the binary),
// installed (~/.local/lib/fast-enough -> ../../share), dev tree (res/).
std::string findTrayIcon() {
    const std::string dir = exeDirPath();
    const std::string rel = "share/icons/hicolor/256x256/apps/fast-enough.png";
    std::vector<std::string> candidates;
    if (dir.empty()) {
        candidates = {"res/app.png"};
    } else {
        candidates = {dir + "/" + rel, dir + "/../" + rel, dir + "/../../" + rel,
                      dir + "/res/app.png", dir + "/../res/app.png",
                      dir + "/../../res/app.png"};
    }
#ifdef AFM_RES_DIR
    candidates.emplace_back(std::string(AFM_RES_DIR) + "/app.png");
#endif
    for (const std::string& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

// SIGTERM/SIGINT (terminal closed, session logout) must unwind through the
// normal shutdown path so the device helper is stopped, not orphaned.
volatile std::sig_atomic_t g_termRequested = 0;
void requestTermination(int) { g_termRequested = 1; }
}  // namespace

// Taskbar progress shims — app.cpp calls these unconditionally.
#if defined(__linux__)
void updateTaskbarProgress(float, bool) {}
void setTaskbarError() {}
void setTaskbarPaused() {}
#endif

// ---- window state persistence (~/.config/FastEnough/window.cfg) ------------
static std::string settingsDir() {
    const char* xdg = ::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else if (const char* home = ::getenv("HOME")) {
        base = std::string(home) + "/.config";
    }
    if (base.empty()) return "";
    return base + "/FastEnough";
}

static std::string windowStatePath() {
    const std::string dir = settingsDir();
    if (dir.empty()) return "";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return ec ? "" : dir + "/window.cfg";
}

static void saveWindowState(GLFWwindow* window) {
    if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) return;
    int w = 0, h = 0, xpos = 0, ypos = 0;
    glfwGetWindowSize(window, &w, &h);
    glfwGetWindowPos(window, &xpos, &ypos);
    if (w < 200 || h < 200) return;
    const std::string path = windowStatePath();
    if (path.empty()) return;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return;
    struct State { int32_t x, y, w, h; } state{xpos, ypos, w, h};
    f.write(reinterpret_cast<const char*>(&state), sizeof(state));
}

static bool loadWindowState(GLFWwindow* window) {
    const std::string path = windowStatePath();
    if (path.empty()) return false;
    std::ifstream f(path, std::ios::binary);
    struct State { int32_t x, y, w, h; } state{};
    if (!f || !f.read(reinterpret_cast<char*>(&state), sizeof(state))) return false;
    if (state.w < 200 || state.h < 200) return false;
    // Keep the window on-screen (a monitor may have been unplugged).
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    bool onScreen = false;
    for (int i = 0; i < count && !onScreen; ++i) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        onScreen = state.x + 100 > mx && state.x < mx + mw && state.y + 60 > my && state.y < my + mh;
    }
    if (!onScreen) return false;
    glfwSetWindowPos(window, state.x, state.y);
    glfwSetWindowSize(window, state.w, state.h);
    return true;
}

// ---- crash log -------------------------------------------------------------
static std::string g_crashLogPath;

static void writeCrashLine(const char* what) {
    const std::string path =
        g_crashLogPath.empty() ? "FastEnough-crash.log" : g_crashLogPath;
    std::ofstream f(path, std::ios::app);
    if (!f) return;
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_r(&now, &tm);
    f << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " [CRASH] " << what << "\n";
}

static void crashSignalHandler(int sig) {
    const char* names[] = {"SIGSEGV", "SIGILL", "SIGFPE", "SIGABRT", "SIGBUS"};
    const char* name = (sig >= 0 && sig <= 4) ? names[sig] : "signal";
    writeCrashLine((std::string("Fatal signal: ") + name).c_str());
    // Restore default and re-raise so core dumps still work.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// ---- launch APK from argv (double-click association) ------------------------
static std::string getLaunchApkPath(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.size() >= 4) {
            std::string ext = arg.substr(arg.size() - 4);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(::tolower(c)); });
            if (ext == ".apk") return arg;
        }
    }
    return "";
}

// ---- single instance (flock on a lock file) ---------------------------------
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>


class InstanceLock {
public:
    InstanceLock() {
        const std::string dir = settingsDir();
        if (!dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            m_fd = ::open((dir + "/instance.lock").c_str(), O_RDWR | O_CREAT, 0666);
            if (m_fd >= 0 && ::flock(m_fd, LOCK_EX | LOCK_NB) != 0) {
                ::close(m_fd);
                m_fd = -1;  // another instance holds it
            }
        }
    }
    ~InstanceLock() {
        if (m_fd >= 0) {
            ::flock(m_fd, LOCK_UN);
            ::close(m_fd);
        }
    }
    bool held() const { return m_fd >= 0; }
private:
    int m_fd = -1;
};

int main(int argc, char** argv) {
    const std::string launchApkPath = getLaunchApkPath(argc, argv);

    InstanceLock instanceLock;
    if (!instanceLock.held()) {
        // Another instance owns the lock: hand it the APK path (or just a
        // raise request) over the Unix socket, mirroring WM_COPYDATA.
        if (single_instance::handoff(launchApkPath)) return 0;
        std::fprintf(stderr,
                     "FastEnough is already running (instance lock held).\n");
        return 0;
    }
    single_instance::Server instanceServer;
    if (!instanceServer.start())
        LOG_WARN("Startup", "Single-instance socket unavailable; second launches will not hand off files");

    {
        const std::string dir = settingsDir();
        std::error_code ec;
        std::filesystem::create_directories(dir + "/Logs", ec);
        g_crashLogPath = dir + "/Logs/crash.log";
    }
    DebugLog::instance().setFilePath(settingsDir() + "/Logs/activity.log");
    ::signal(SIGTERM, requestTermination);
    ::signal(SIGINT, requestTermination);
    ::signal(SIGSEGV, crashSignalHandler);
    ::signal(SIGILL, crashSignalHandler);
    ::signal(SIGFPE, crashSignalHandler);
    ::signal(SIGABRT, crashSignalHandler);
    ::signal(SIGBUS, crashSignalHandler);
    std::signal(SIGPIPE, SIG_IGN);  // sockets use MSG_NOSIGNAL; belt and braces
    LOG_INFO("Startup", "Application starting (Linux/GLFW).");

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    {
        const char* platformName = "unknown";
#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4)
        switch (glfwGetPlatform()) {
            case GLFW_PLATFORM_WAYLAND: platformName = "Wayland"; break;
            case GLFW_PLATFORM_X11:     platformName = "X11"; break;
            case GLFW_PLATFORM_NULL:    platformName = "null"; break;
            default: break;
        }
#endif
        LOG_INFO("Startup", std::string("GLFW window system: ") + platformName);
    }
    // GL 3.2 core is the ImGui OpenGL3 backend baseline.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);  // per-monitor DPI
    // Match the installed .desktop entry so Wayland/X11 associate the window
    // with its icon, name and startup notification (hyprctl class was empty).
#ifdef GLFW_WAYLAND_APP_ID
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "fast-enough");
#endif
#ifdef GLFW_X11_CLASS_NAME
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "fast-enough");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "fast-enough");
#endif

    GLFWwindow* window = glfwCreateWindow(1400, 800, "Fast Enough? - Android File Explorer",
                                          nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(window, 640, 400, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync (matches Present(1,0) on Windows)

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    static std::string imguiIniPath;
    {
        const std::string dir = settingsDir();
        if (!dir.empty()) imguiIniPath = dir + "/imgui.ini";
    }
    if (!imguiIniPath.empty()) io.IniFilename = imguiIniPath.c_str();

    // DPI: each axis scale averaged; Hyprland reports fractional scale.
    float xScale = 1.0f, yScale = 1.0f;
    glfwGetWindowContentScale(window, &xScale, &yScale);
    const float dpiScale = (xScale + yScale) * 0.5f;
    const float fontSize = 14.0f * dpiScale;

    ImFontConfig fontCfg;
    fontCfg.OversampleH = 3;
    fontCfg.OversampleV = 3;
    fontCfg.PixelSnapH = true;
    fontCfg.RasterizerDensity = 1.0f;

    static const ImWchar glyphRanges[] = {
        0x0020, 0x00FF,  // Basic Latin + Latin Supplement
        0x0100, 0x024F,  // Latin Extended-A + B
        0x0370, 0x03FF,  // Greek and Coptic
        0x0400, 0x04FF,  // Cyrillic
        0x2000, 0x206F,  // General Punctuation
        0x2100, 0x214F,  // Letterlike Symbols
        0,
    };

    // fontconfig candidates, most common Arch/CachyOS fonts first.
    ImFont* mainFont = nullptr;
    const char* fontCandidates[] = {
        "/usr/share/fonts/noto-fonts/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/cantarell/Cantarell-Regular.ttf",
        "/usr/share/fonts/adobe-source-sans-fonts/SourceSans3-Regular.otf",
    };
    for (const char* candidate : fontCandidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            mainFont = io.Fonts->AddFontFromFileTTF(candidate, fontSize, &fontCfg, glyphRanges);
            if (mainFont) break;
        }
    }
    if (!mainFont) {
        fontCfg.SizePixels = fontSize;
        mainFont = io.Fonts->AddFontDefault(&fontCfg);
    }
    // Semibold companion (App::applyScale reloads this file when the user
    // changes scale; ui::semiboldFont may be null — panels fall back).
    ui::semiboldFont = nullptr;
    for (const char* candidate : {"noto-fonts/NotoSans-Bold.ttf", "TTF/DejaVuSans-Bold.ttf",
                                  "dejavu/DejaVuSans-Bold.ttf"}) {
        const std::string path = std::string("/usr/share/fonts/") + candidate;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            ui::semiboldFont = io.Fonts->AddFontFromFileTTF(path.c_str(), fontSize, &fontCfg, glyphRanges);
            if (ui::semiboldFont) break;
        }
    }
    io.Fonts->Build();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    App app;
    app.m_systemDpiScale = dpiScale;
    app.applyScale(app.m_theme.userScale);
    if (!launchApkPath.empty()) app.requestApkInstall(launchApkPath);
    if (!loadWindowState(window)) glfwSetWindowSize(window, 1400, 800);

    // Close behavior mirrors main.cpp: with "confirm before closing" on, the
    // shared App dialog handles it; otherwise hide to the tray. If no tray
    // host exists, the window closes normally rather than becoming invisible.
    g_appForClose = &app;
    g_trayIconPath = findTrayIcon();
    glfwSetWindowCloseCallback(window, [](GLFWwindow* win) {
        if (g_reallyQuit || g_termRequested || !g_appForClose) return;
        if (g_appForClose->m_prefs.confirmOnClose) {
            glfwSetWindowShouldClose(win, GLFW_FALSE);
            g_appForClose->m_showCloseConfirm = true;
            return;
        }
        if (tray::start(g_trayIconPath)) {
            glfwSetWindowShouldClose(win, GLFW_FALSE);
            tray::setVisible(true);
            glfwHideWindow(win);
            g_windowHidden = true;
            LOG_INFO("Tray", "Window hidden to the system tray");
        }
    });

    // External file drop -> App (paths are bytes on Linux). Single-window app:
    // the free callback reaches App through a static pointer set here.
    static App* g_appForDrop = &app;
    glfwSetDropCallback(window, [](GLFWwindow* win, int count, const char** paths) {
        std::vector<std::string> files;
        for (int i = 0; i < count; ++i) files.emplace_back(paths[i]);
        // GLFW has no drop position; the cursor is the best match and keeps
        // the shared panel hit-test working (0,0 used to mean "outside").
        double cx = 0.0, cy = 0.0;
        glfwGetCursorPos(win, &cx, &cy);
        if (g_appForDrop)
            g_appForDrop->handleExternalFileDrop(files, static_cast<int>(cx), static_cast<int>(cy));
    });

    // Second-instance handoff: raise the window and optionally install an APK.
    auto handleInstanceMessage = [&](const std::string& apkPath) {
        if (g_windowHidden) {
            glfwShowWindow(window);
            tray::setVisible(false);
            g_windowHidden = false;
        }
        glfwShowWindow(window);
        glfwFocusWindow(window);
        glfwRequestWindowAttention(window);
        LOG_INFO("Startup", apkPath.empty()
            ? "Second launch raised the window"
            : "Second launch handed off APK: " + apkPath);
        if (!apkPath.empty()) app.requestApkInstall(apkPath);
    };

    while (!glfwWindowShouldClose(window)) {
        instanceServer.poll(handleInstanceMessage);
        if (g_windowHidden) {
            // Idle in the tray: wake on menu actions or signals only.
            glfwWaitEventsTimeout(0.25);
            if (tray::consumeShowRequest()) {
                glfwShowWindow(window);
                glfwFocusWindow(window);
                tray::setVisible(false);
                g_windowHidden = false;
                int fw = 0, fh = 0;
                glfwGetFramebufferSize(window, &fw, &fh);
                LOG_INFO("Tray", "Window restored from the system tray (visible=" +
                    std::to_string(glfwGetWindowAttrib(window, GLFW_VISIBLE)) +
                    ", fb=" + std::to_string(fw) + "x" + std::to_string(fh) + ")");
            }
            if (tray::consumeExitRequest()) g_reallyQuit = true;
            if (g_reallyQuit || g_termRequested)
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            continue;
        }

        glfwPollEvents();
        // A StatusNotifier host keeps the menu alive even while the window is
        // visible; honor Exit from either state.
        if (tray::consumeExitRequest()) g_reallyQuit = true;
        (void)tray::consumeShowRequest();  // already visible

        if (g_reallyQuit || g_termRequested) glfwSetWindowShouldClose(window, GLFW_TRUE);

        if (app.m_pendingScale > 0) {
            app.applyScale(app.m_pendingScale);
            app.m_scalePreview = app.m_theme.userScale;
            app.m_theme.save();
            app.m_pendingScale = -1.0f;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app.render();
        ImGui::Render();

        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        glViewport(0, 0, fbW, fbH);
        const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        if (!app.wantsHighFps()) {
            // Match the Windows loop's idle throttle (30fps idle ceiling).
            struct timespec ts{0, 33 * 1000 * 1000};
            ::nanosleep(&ts, nullptr);
        }
    }

    saveWindowState(window);
    instanceServer.stop();
    tray::stop();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
