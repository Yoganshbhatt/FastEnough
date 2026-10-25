#pragma once
// Linux-side filesystem path helpers. app.cpp carries its own Windows
// UTF-8/UTF-16 conversion in its platform block; this header is used by
// Linux translation units that need the same API (device_client_backup_linux).
// Keep this header free of Windows headers so it stays portable.
#include <filesystem>
#include <string>

namespace pathutil {

#if defined(_WIN32)
// Windows conversion lives in app.cpp; including this header there is not
// intended. Present only so the API is consistent if shared code grows.
std::string pathToUtf8(const std::filesystem::path& path);
std::filesystem::path toFsPath(const std::string& utf8);
#else
inline std::string pathToUtf8(const std::filesystem::path& path) { return path.string(); }
inline std::filesystem::path toFsPath(const std::string& utf8) { return std::filesystem::path(utf8); }
#endif

}  // namespace pathutil
