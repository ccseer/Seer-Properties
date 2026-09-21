#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace gitinfo {

// Outcome of one git subprocess invocation.
struct GitRunResult {
    bool started        = false;  // process launched (not FailedToStart)
    bool timedOut       = false;  // internal deadline exceeded
    bool stdoutExceeded = false;  // stdout exceeded the byte cap
    bool stderrExceeded = false;  // stderr exceeded the byte cap
    int exitCode        = 0;
    DWORD processId     = 0;      // child pid, 0 when it never started
    std::string stdoutText;
    std::string stderrText;
};

// Runs `git.exe <arguments...>` with a sanitized environment and an optional
// working directory.
// - Structured arguments: the argument list is quoted for the C runtime and no
//   shell is involved, so no shell interpolation can occur.
// - stdout/stderr are read incrementally and capped at maxStdoutBytes /
//   maxStderrBytes; the cap is enforced while reading, not after exit.
// - timeoutMs is the internal deadline; on expiry the child and its descendants
//   are terminated and reaped before returning.
// - The environment drops inherited Git location, namespace, configuration and
//   helper overrides and disables the filesystem monitor, terminal prompts,
//   pagers and editors. GIT_OPTIONAL_LOCKS=0 keeps these queries from refreshing
//   the index.
GitRunResult runGit(const std::wstring &gitExecutable,
                    const std::wstring &workingDirectory,
                    const std::vector<std::wstring> &arguments,
                    std::size_t maxStdoutBytes,
                    std::size_t maxStderrBytes,
                    DWORD timeoutMs);

}  // namespace gitinfo
