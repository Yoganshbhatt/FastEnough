// Verifies src/subprocess_posix.cpp against local commands only.
// No device interaction: every case runs against /bin or PATH binaries.
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "subprocess.h"

namespace {
int failures = 0;
void check(bool ok, const char* name, const std::string& detail = {}) {
    if (ok) {
        std::puts(("  ok: " + std::string(name)).c_str());
    } else {
        ++failures;
        std::puts(("  FAIL: " + std::string(name) +
                   (detail.empty() ? "" : " -- " + detail)).c_str());
    }
}

SubprocessOptions base() {
    SubprocessOptions opts;
    opts.timeoutMs = 10000;
    return opts;
}
}  // namespace

int main() {
    {
        auto opts = base();
        opts.executable = "echo";
        opts.arguments = {"hello"};
        const SubprocessResult r = runSubprocess(opts);
        check(r.succeeded(), "echo exit 0", r.diagnostics());
        check(r.standardOutput == "hello\n", "echo stdout", r.standardOutput);
    }
    {
        auto opts = base();
        opts.executable = "false";
        const SubprocessResult r = runSubprocess(opts);
        check(!r.succeeded() && r.exitCode != 0, "false exit non-zero");
    }
    {
        auto opts = base();
        opts.executable = "afm-no-such-binary-xyz";
        const SubprocessResult r = runSubprocess(opts);
        check(!r.succeeded() && r.systemError != 0 && !r.errorMessage.empty(),
              "missing binary reports systemError", r.diagnostics());
    }
    {
        auto opts = base();
        opts.executable = "sleep";
        opts.arguments = {"5"};
        opts.timeoutMs = 200;
        const SubprocessResult r = runSubprocess(opts);
        check(r.timedOut && !r.succeeded(), "sleep 5 killed at 200ms timeout");
    }
    {
        auto opts = base();
        opts.executable = "sh";
        opts.arguments = {"-c", "echo out; echo err >&2; exit 3"};
        const SubprocessResult r = runSubprocess(opts);
        check(r.exitCode == 3, "exit code passthrough", std::to_string(r.exitCode));
        check(r.standardOutput == "out\n", "stdout capture", r.standardOutput);
        check(r.standardError == "err\n", "stderr capture", r.standardError);
    }
    {
        const std::string path = "/tmp/afm-subprocess-test.bin";
        auto opts = base();
        opts.executable = "echo";
        opts.arguments = {"filedata"};
        opts.outputPath = path;
        const SubprocessResult r = runSubprocess(opts);
        std::ifstream in(path, std::ios::binary);
        std::ostringstream content;
        content << in.rdbuf();
        ::remove(path.c_str());
        check(r.succeeded(), "output-to-file exit 0", r.diagnostics());
        check(content.str() == "filedata\n", "output file contents", content.str());
        check(r.bytesWritten == content.str().size(), "bytesWritten counted");
        check(r.standardOutput.empty(), "nothing captured when streaming to file");
    }
    {
        auto opts = base();
        opts.executable = "sleep";
        opts.arguments = {"5"};
        opts.shouldCancel = [] { return true; };
        const SubprocessResult r = runSubprocess(opts);
        check(r.cancelled && !r.succeeded(), "immediate cancel");
    }
    {
        // Informational: proves we can spawn the real adb from PATH. adb is an
        // optional runtime dependency, so a host without it skips, not fails.
        auto opts = base();
        opts.executable = "adb";
        opts.arguments = {"version"};
        const SubprocessResult r = runSubprocess(opts);
        if (r.systemError == ENOENT) {
            std::puts("  skip: adb not installed on this host");
        } else {
            check(r.succeeded(), "adb version spawns", r.diagnostics());
            std::puts(("  info: " + r.standardOutput.substr(0, r.standardOutput.find('\n'))).c_str());
        }
    }

    if (failures == 0) std::puts("subprocess_test: ALL OK");
    return failures == 0 ? 0 : 1;
}
