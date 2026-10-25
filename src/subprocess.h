#pragma once
// Shared subprocess API for the Linux port (AGENTS.md: platform_linux.cpp plan).
// Windows keeps src/process_runner.* until device_client is ported; new Linux
// code uses this argv-based, UTF-8, shell-free interface on every platform.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SubprocessOptions {
    std::string executable;              // path or bare name resolved via PATH
    std::vector<std::string> arguments;  // argv[1..]; never passed through a shell
    std::string inputPath;               // stdin source file; empty means /dev/null
    std::string outputPath;              // stdout sink file; empty means capture
    uint32_t timeoutMs = 30000;
    size_t captureLimit = 16 * 1024 * 1024;
    std::function<bool()> shouldCancel;
    std::function<bool(uint64_t)> progress;  // stdout bytes so far (file or captured)
};

struct SubprocessResult {
    int exitCode = 1;  // POSIX exit status; 128+signal when killed by a signal
    int systemError = 0;
    bool timedOut = false;
    bool cancelled = false;
    bool outputTruncated = false;
    std::string standardOutput;
    std::string standardError;
    std::string errorMessage;
    uint64_t bytesWritten = 0;

    bool succeeded() const {
        return exitCode == 0 && !systemError && !timedOut && !cancelled && errorMessage.empty();
    }
    std::string diagnostics() const;
};

SubprocessResult runSubprocess(const SubprocessOptions& options);
