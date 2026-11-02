#pragma once
#include "windows_file_entry.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <filesystem>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

inline std::vector<WindowsFileEntry> listWindowsDirectory(const std::filesystem::path& directory,
    const std::function<bool()>& cancelled = {}) {
    WIN32_FIND_DATAW data{};
    auto pattern = directory / L"*";
    HANDLE search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
        FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) return {};
        throw std::system_error((int)error, std::system_category(), "Cannot list folder");
    }
    struct SearchHandle { HANDLE value; ~SearchHandle() { FindClose(value); } } handle{search};
    std::vector<WindowsFileEntry> entries;
    for (;;) {
        if (cancelled && cancelled()) return {};
        if (wcscmp(data.cFileName, L".") != 0 && wcscmp(data.cFileName, L"..") != 0) {
            WindowsFileEntry entry;
            int count = WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, nullptr, 0, nullptr, nullptr);
            std::string name(count, '\0');
            WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, name.data(), count, nullptr, nullptr);
            if (!name.empty()) name.pop_back();
            entry.name = std::move(name);
            entry.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            entry.isHidden = (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
            if (!entry.isDirectory) entry.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            FILETIME local{};
            SYSTEMTIME date{};
            if (FileTimeToLocalFileTime(&data.ftLastWriteTime, &local) && FileTimeToSystemTime(&local, &date)) {
                char formatted[32];
                snprintf(formatted, sizeof(formatted), "%04u-%02u-%02u %02u:%02u",
                    date.wYear, date.wMonth, date.wDay, date.wHour, date.wMinute);
                entry.dateModified = formatted;
            }
            entries.push_back(std::move(entry));
        }
        if (!FindNextFileW(search, &data)) {
            DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_FILES)
                throw std::system_error((int)error, std::system_category(), "Cannot finish listing folder");
            break;
        }
    }
    return entries;
}
