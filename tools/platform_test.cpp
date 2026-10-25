// Unit checks for the Linux platform layer. Runs entirely inside a temporary
// directory under $HOME (same filesystem as the isolated XDG trash), so the
// user's real Trash and files are never touched.
#include "platform_linux.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

static int g_failures = 0;

static void check(bool condition, const std::string& what) {
    std::cout << (condition ? "PASS  " : "FAIL  ") << what << "\n";
    if (!condition) ++g_failures;
}

static void writeFile(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

int main() {
    const fs::path root = fs::path(std::getenv("HOME")) /
        (".cache/afm-platform-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "xdg", ec);
    if (ec) {
        std::cerr << "Could not create test root: " << ec.message() << "\n";
        return 1;
    }
    // Keep the trash inside the test root: same filesystem as the test files,
    // so gio never writes to the real ~/.local/share/Trash.
    ::setenv("XDG_DATA_HOME", (root / "xdg").c_str(), 1);

    // --- Recoverable delete: files and a directory go to Trash --------------
    writeFile(root / "a.txt", "alpha");
    writeFile(root / "b.txt", "beta");
    fs::create_directories(root / "dir", ec);
    writeFile(root / "dir" / "c.txt", "gamma");

    std::string error;
    const bool trashed = platform::deleteLocalFiles(
        {(root / "a.txt").string(), (root / "b.txt").string(), (root / "dir").string()},
        false, error);
    check(trashed, "trash: deleteLocalFiles returns true" + (error.empty() ? "" : " (" + error + ")"));
    check(!fs::exists(root / "a.txt"), "trash: file removed from its original path");
    check(!fs::exists(root / "dir"), "trash: directory removed from its original path");
    check(fs::exists(root / "xdg" / "Trash" / "files" / "a.txt"), "trash: file stored in Trash/files");
    check(fs::exists(root / "xdg" / "Trash" / "info" / "a.txt.trashinfo"), "trash: .trashinfo written");

    std::ifstream info(root / "xdg" / "Trash" / "info" / "a.txt.trashinfo");
    std::string contents((std::istreambuf_iterator<char>(info)), std::istreambuf_iterator<char>());
    check(contents.find("Path=" + (root / "a.txt").string()) != std::string::npos,
          "trash: .trashinfo records the original path");

    // --- Permanent delete: no Trash entry -----------------------------------
    writeFile(root / "gone.txt", "permanent");
    error.clear();
    const bool removed = platform::deleteLocalFiles({(root / "gone.txt").string()}, true, error);
    check(removed, "permanent: deleteLocalFiles returns true" + (error.empty() ? "" : " (" + error + ")"));
    check(!fs::exists(root / "gone.txt"), "permanent: file removed");
    check(!fs::exists(root / "xdg" / "Trash" / "files" / "gone.txt"), "permanent: no Trash entry");

    // --- Fail safe: an unavailable trash must not become a delete ----------
    writeFile(root / "keep.txt", "keep");
    const char* savedPath = std::getenv("PATH");
    const std::string pathValue = savedPath ? savedPath : "";
    ::setenv("PATH", "", 1);
    error.clear();
    const bool refused = !platform::deleteLocalFiles({(root / "keep.txt").string()}, false, error);
    ::setenv("PATH", pathValue.c_str(), 1);
    check(refused, "fail-safe: unavailable gio reports an error instead of deleting");
    check(!error.empty(), "fail-safe: error explains the failure");
    check(fs::exists(root / "keep.txt"), "fail-safe: file is left in place");

    // --- Cleanup: only the isolated test root -------------------------------
    fs::remove_all(root, ec);
    check(!ec, "cleanup: test root removed");

    std::cout << (g_failures == 0 ? "all platform checks passed" : "FAILURES: " + std::to_string(g_failures))
              << "\n";
    return g_failures == 0 ? 0 : 1;
}
