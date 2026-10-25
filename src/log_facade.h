#pragma once
// Linux-only logging facade for the port (AGENTS.md).
// Same call syntax as activity_log.h (LOG_DEBUG/INFO/WARN/ERROR with tag + message)
// so transfer-core code reads identically on both platforms. The GUI phase will
// adopt the full activity_log.h (needs only a localtime_s shim); this facade
// stays as the afm-cli sink: thread-safe, stderr by default, optional file.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

enum class LinuxLogLevel { Debug, Info, Warn, Error };

class LinuxLog {
public:
    static LinuxLog& instance() {
        static LinuxLog logger;
        return logger;
    }

    void setMinLevel(LinuxLogLevel level) { m_minLevel = level; }
    void setFile(FILE* file) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_file = file;
    }

    void log(LinuxLogLevel level, const char* tag, const std::string& message) {
        if (level < m_minLevel.load()) return;
        const std::lock_guard<std::mutex> lock(m_mutex);
        static const char* names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
        const auto now = std::chrono::system_clock::now();
        const std::time_t clock = std::chrono::system_clock::to_time_t(now);
        std::tm broken{};
        ::localtime_r(&clock, &broken);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &broken);
        std::string line = std::string(stamp) + " [" + names[static_cast<int>(level)] +
            "] [" + tag + "] " + singleLine(message) + "\n";
        std::fwrite(line.data(), 1, line.size(), stderr);
        if (m_file) std::fwrite(line.data(), 1, line.size(), m_file);
    }

private:
    static std::string singleLine(const std::string& value) {
        std::string text;
        text.reserve(value.size());
        for (const char c : value) {
            if (c == '\n') text += " | ";
            else if (c != '\r' && c != '\0') text += c;
        }
        return text;
    }

    std::mutex m_mutex;
    std::atomic<LinuxLogLevel> m_minLevel{LinuxLogLevel::Debug};
    FILE* m_file = nullptr;  // not owned; caller manages lifetime
};

#define LINUX_LOG_DEBUG(tag, msg) LinuxLog::instance().log(LinuxLogLevel::Debug, tag, msg)
#define LINUX_LOG_INFO(tag, msg) LinuxLog::instance().log(LinuxLogLevel::Info, tag, msg)
#define LINUX_LOG_WARN(tag, msg) LinuxLog::instance().log(LinuxLogLevel::Warn, tag, msg)
#define LINUX_LOG_ERROR(tag, msg) LinuxLog::instance().log(LinuxLogLevel::Error, tag, msg)

// DeviceClient code uses LOG_* on Windows; map the same names here so the
// Linux port of that code needs no syntactic changes. Include-guarded: when
// activity_log.h's real macros are already defined (GUI code), they win —
// never redefine a macro (that is UB and trips -Werror).
#ifndef LOG_DEBUG
#define LOG_DEBUG LINUX_LOG_DEBUG
#endif
#ifndef LOG_INFO
#define LOG_INFO LINUX_LOG_INFO
#endif
#ifndef LOG_WARN
#define LOG_WARN LINUX_LOG_WARN
#endif
#ifndef LOG_ERROR
#define LOG_ERROR LINUX_LOG_ERROR
#endif
