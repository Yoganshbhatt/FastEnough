// POSIX implementation of src/subprocess.h: fork+exec, pipe/poll,
// timeout/kill semantics mirroring src/process_runner.cpp (Windows).
//
// Safety notes: argv only (no shell), child gets its own process group so a
// timeout kills the whole tree like the Windows Job Object does, stdout
// streaming counts bytes for progress, every syscall failure lands in the
// result with errno captured at the failure site.
#include "subprocess.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <system_error>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <string>
#include <vector>

namespace {
class Fd {
public:
    explicit Fd(int fd = -1) : m_fd(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : m_fd(other.m_fd) { other.m_fd = -1; }
    int get() const { return m_fd; }
    bool valid() const { return m_fd >= 0; }
    void reset(int fd = -1) {
        if (m_fd >= 0) ::close(m_fd);
        m_fd = fd;
    }
private:
    int m_fd;
};

bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::string errnoText(int code, const char* operation) {
    // std::error_code is thread-safe and portable; strerror_r has two
    // incompatible return types across libc implementations.
    const std::string detail =
        std::error_code(code, std::generic_category()).message();
    return std::string(operation) + " (errno " + std::to_string(code) + ": " +
           detail + ").";
}
}  // namespace

std::string SubprocessResult::diagnostics() const {
    std::string message = errorMessage;
    if (!standardError.empty()) {
        if (!message.empty()) message += '\n';
        message += standardError;
    } else if (!succeeded() && !standardOutput.empty()) {
        if (!message.empty()) message += '\n';
        message += standardOutput;
    }
    if (message.empty() && exitCode != 0)
        message = "Command failed with exit code " + std::to_string(exitCode) + ".";
    return message;
}

namespace {
// Child side: report the captured errno to the parent. Best effort — the child
// is about to _exit, and only async-signal-safe calls are allowed here.
void reportErrno(int fd, int code) {
    const char* cursor = reinterpret_cast<const char*>(&code);
    size_t remaining = sizeof(code);
    while (remaining > 0) {
        const ssize_t written = ::write(fd, cursor, remaining);
        if (written > 0) {
            cursor += written;
            remaining -= static_cast<size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return;
        }
    }
}

// Child side: wire fds then exec. Only async-signal-safe calls; on failure
// write errno to the error pipe and _exit(127).
void runChild(const std::vector<char*>& argv, int devNull, int inputFd,
              int outWrite, int errWrite, int errReport) {
    if (::setpgid(0, 0) != 0) {
        const int code = errno;
        reportErrno(errReport, code);
        ::_exit(127);
    }
    const int inFd = inputFd >= 0 ? inputFd : devNull;
    if (::dup2(inFd, STDIN_FILENO) < 0 || ::dup2(outWrite, STDOUT_FILENO) < 0 ||
        ::dup2(errWrite, STDERR_FILENO) < 0) {
        const int code = errno;
        reportErrno(errReport, code);
        ::_exit(127);
    }
    ::close(devNull);
    if (inputFd >= 0) ::close(inputFd);
    ::close(outWrite);
    ::close(errWrite);
    // NOTE: errReport stays open across execvp on purpose: success closes it via
    // O_CLOEXEC (parent sees EOF), failure writes errno below.
    ::execvp(argv[0], argv.data());
    const int code = errno;
    reportErrno(errReport, code);
    ::_exit(127);
}
}  // namespace

SubprocessResult runSubprocess(const SubprocessOptions& options) {
    SubprocessResult result;
    auto fail = [&](const char* operation, int code) {
        result.systemError = code;
        result.errorMessage = errnoText(code, operation);
    };
    if (options.shouldCancel && options.shouldCancel()) {
        result.cancelled = true;
        result.errorMessage = "Operation cancelled.";
        return result;
    }
    if (options.executable.empty()) {
        result.errorMessage = "No executable specified.";
        return result;
    }

    std::vector<std::string> args;
    args.reserve(options.arguments.size() + 1);
    args.push_back(options.executable);
    args.insert(args.end(), options.arguments.begin(), options.arguments.end());
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    Fd devNull(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (!devNull.valid()) {
        fail("Could not open /dev/null", errno);
        return result;
    }
    Fd inputFd;
    if (!options.inputPath.empty()) {
        inputFd.reset(::open(options.inputPath.c_str(), O_RDONLY | O_CLOEXEC));
        if (!inputFd.valid()) {
            fail("Could not open command input", errno);
            return result;
        }
    }
    Fd outFile;
    if (!options.outputPath.empty()) {
        outFile.reset(::open(options.outputPath.c_str(),
                             O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666));
        if (!outFile.valid()) {
            fail("Could not create output file", errno);
            return result;
        }
    }

    int outPipe[2] = {-1, -1}, errPipe[2] = {-1, -1}, reportPipe[2] = {-1, -1};
    if (::pipe2(outPipe, O_CLOEXEC) != 0 || ::pipe2(errPipe, O_CLOEXEC) != 0 ||
        ::pipe2(reportPipe, O_CLOEXEC) != 0) {
        const int code = errno;
        for (int fd : {outPipe[0], outPipe[1], errPipe[0], errPipe[1], reportPipe[0], reportPipe[1]})
            if (fd >= 0) ::close(fd);
        fail("Could not create command output pipes", code);
        return result;
    }
    Fd outRead(outPipe[0]), outWrite(outPipe[1]), errRead(errPipe[0]), errWrite(errPipe[1]);
    Fd reportRead(reportPipe[0]), reportWrite(reportPipe[1]);

    const pid_t pid = ::fork();
    if (pid < 0) {
        fail("Could not start command", errno);
        return result;
    }
    if (pid == 0) {
        runChild(argv, devNull.get(), inputFd.get(), outWrite.get(), errWrite.get(), reportWrite.get());
        ::_exit(127);  // unreachable
    }
    outWrite.reset();
    errWrite.reset();
    reportWrite.reset();
    inputFd.reset();
    devNull.reset();

    // exec/dup2 failures arrive here; success closes the write end (CLOEXEC).
    int childErrno = 0;
    ssize_t reportBytes = ::read(reportRead.get(), &childErrno, sizeof(childErrno));
    reportRead.reset();
    if (reportBytes == static_cast<ssize_t>(sizeof(childErrno))) {
        ::waitpid(pid, nullptr, 0);
        fail("Could not start command", childErrno);
        return result;
    }

    if (!setNonBlocking(outRead.get()) || !setNonBlocking(errRead.get())) {
        fail("Could not configure command output pipes", errno);
        ::kill(-pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        return result;
    }

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() +
        std::chrono::milliseconds(options.timeoutMs ? options.timeoutMs : 30000);
    auto nextProgress = Clock::now();
    std::array<char, 64 * 1024> buffer{};
    bool outEof = false, errEof = false, exited = false;
    int status = 0;
    bool aborted = false;

    auto drain = [&](Fd& pipe, std::string& captured, bool toFile, bool& eof) {
        if (eof) return true;
        for (;;) {
            const ssize_t count = ::read(pipe.get(), buffer.data(), buffer.size());
            if (count > 0) {
                const auto n = static_cast<size_t>(count);
                if (toFile) {
                    size_t off = 0;
                    while (off < n) {
                        const ssize_t written =
                            ::write(outFile.get(), buffer.data() + off, n - off);
                        if (written < 0) {
                            if (errno == EINTR) continue;
                            fail("Could not write output file", errno);
                            return false;
                        }
                        off += static_cast<size_t>(written);
                    }
                    result.bytesWritten += n;
                } else {
                    const size_t room = options.captureLimit > captured.size()
                        ? options.captureLimit - captured.size()
                        : 0;
                    const size_t take = n < room ? n : room;
                    captured.append(buffer.data(), take);
                    if (take != n) result.outputTruncated = true;
                }
                continue;
            }
            if (count == 0) {
                eof = true;
                pipe.reset();
                return true;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            fail("Could not read command output", errno);
            return false;
        }
    };

    for (;;) {
        const auto now = Clock::now();
        bool cancel = options.shouldCancel && options.shouldCancel();
        if (!cancel && options.progress && now >= nextProgress) {
            const uint64_t produced = outFile.valid()
                ? result.bytesWritten
                : result.standardOutput.size();
            cancel = !options.progress(produced);
            nextProgress = now + std::chrono::milliseconds(50);
        }
        if (cancel || now >= deadline) {
            result.cancelled = cancel;
            result.timedOut = !cancel;
            result.errorMessage = cancel ? "Operation cancelled." : "Command timed out.";
            aborted = true;
            break;
        }
        if (!drain(outRead, result.standardOutput, outFile.valid(), outEof) ||
            !drain(errRead, result.standardError, false, errEof)) {
            aborted = true;
            break;
        }
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited < 0 && errno != EINTR) {
            fail("Could not wait for command", errno);
            aborted = true;
            break;
        }
        if (waited == pid) exited = true;
        if (exited && outEof && errEof) break;
        struct pollfd fds[2] = {{-1, 0, 0}, {-1, 0, 0}};
        int nfds = 0;
        if (!outEof) fds[nfds++] = {outRead.get(), POLLIN, 0};
        if (!errEof) fds[nfds++] = {errRead.get(), POLLIN, 0};
        if (nfds == 0) {
            if (exited) break;
            // No pipes left but child alive: wait briefly for the exit.
            struct pollfd none[1] = {{-1, 0, 0}};
            ::poll(none, 0, 1);
            continue;
        }
        const int ready = ::poll(fds, static_cast<nfds_t>(nfds), exited ? 0 : 1);
        if (ready < 0 && errno != EINTR) {
            fail("Could not wait for command output", errno);
            aborted = true;
            break;
        }
    }

    if (aborted) {
        ::kill(-pid, SIGKILL);  // whole process group, like the Windows Job Object
        ::waitpid(pid, &status, 0);
    } else if (!exited) {
        ::waitpid(pid, &status, 0);
    }
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
        if (!aborted) result.errorMessage = "Command killed by signal " +
            std::to_string(WTERMSIG(status)) + ".";
    }
    if (!aborted && options.progress) {
        const uint64_t produced =
            outFile.valid() ? result.bytesWritten : result.standardOutput.size();
        if (!options.progress(produced)) {
            result.cancelled = true;
            result.errorMessage = "Operation cancelled.";
        }
    }
    return result;
}
