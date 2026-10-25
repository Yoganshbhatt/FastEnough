// Portable CRT shims for the Win32 bits the shared GUI code uses.
// Windows keeps the native versions; Linux gets equivalents. AGENTS.md:
// shared logic stays unguarded, per-OS code lives behind guards — these are
// the shared declarations the guards route to.
#pragma once
#if !defined(_WIN32)

#include <cerrno>
#include <cstring>
#include <strings.h>

using errno_t = int;

inline int _stricmp(const char* a, const char* b) {
    return ::strcasecmp(a, b);
}

template <size_t N>
inline errno_t strcpy_s(char (&dst)[N], const char* src) {
    if (!src) return EINVAL;
    const size_t len = std::strlen(src);
    if (len >= N) {
        dst[0] = '\0';
        return ERANGE;
    }
    std::memcpy(dst, src, len + 1);
    return 0;
}

inline errno_t strcpy_s(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0 || !src) return EINVAL;
    const size_t len = std::strlen(src);
    if (len >= dstSize) {
        dst[0] = '\0';
        return ERANGE;
    }
    std::memcpy(dst, src, len + 1);
    return 0;
}

#endif  // !_WIN32
