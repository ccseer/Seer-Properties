#include "gitrunner.h"

#include <windows.h>
#include <tlhelp32.h>

#include <set>
#include <string>


namespace gitinfo {

namespace {

// Quotes one argument using the same rules the C runtime uses to split a
// command line: backslashes are doubled when they precede a quote, and a
// trailing backslash is doubled before the closing quote.
std::wstring quoteArgument(const std::wstring &argument)
{
    std::wstring quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back(L'"');

    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(c);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring commandLineFor(const std::wstring &program,
                            const std::vector<std::wstring> &args)
{
    std::wstring commandLine = quoteArgument(program);
    for (const auto &arg : args) {
        commandLine.push_back(L' ');
        commandLine += quoteArgument(arg);
    }
    return commandLine;
}

// Terminates the child together with every descendant. A Job Object kills the
// whole group at once, which a parent-pid snapshot scan cannot guarantee: a pid
// can be reused, and a re-parented descendant is invisible to the scan.
class ProcessGroupGuard {
public:
    ProcessGroupGuard() : job_(CreateJobObjectW(nullptr, nullptr))
    {
        if (job_ == nullptr) {
            return;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags
            = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation,
                                &limits, sizeof(limits));
    }

    // Closing the job handle terminates every process still in the group.
    ~ProcessGroupGuard()
    {
        if (job_ != nullptr) {
            CloseHandle(job_);
        }
    }

    ProcessGroupGuard(const ProcessGroupGuard &)            = delete;
    ProcessGroupGuard &operator=(const ProcessGroupGuard &) = delete;

    bool valid() const { return job_ != nullptr; }

    bool assign(HANDLE process)
    {
        if (job_ == nullptr) {
            return false;
        }
        return AssignProcessToJobObject(job_, process) == TRUE;
    }

private:
    HANDLE job_;
};

// Fallback used only when the job object cannot be assigned (a nested-job
// policy can forbid it). Parent-pid matching is best effort: a reused pid can
// make the scan miss or over-match.
void killProcessTree(DWORD pid)
{
    if (pid == 0) {
        return;
    }
    std::set<DWORD> toKill;
    std::set<DWORD> visited;
    std::set<DWORD> wave;
    wave.insert(pid);
    visited.insert(pid);
    while (!wave.empty()) {
        std::set<DWORD> next;
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (Process32FirstW(snapshot, &entry)) {
                do {
                    if (wave.count(entry.th32ParentProcessID)
                        && !visited.count(entry.th32ProcessID)) {
                        visited.insert(entry.th32ProcessID);
                        next.insert(entry.th32ProcessID);
                    }
                } while (Process32NextW(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }
        toKill.insert(wave.begin(), wave.end());
        wave.swap(next);
    }
    for (auto it = toKill.rbegin(); it != toKill.rend(); ++it) {
        const HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, *it);
        if (handle) {
            TerminateProcess(handle, 1);
            CloseHandle(handle);
        }
    }
}

// Terminates the child and reaps it before returning, so a forced host
// cancellation cannot leave query workers running behind the result. When the
// job assignment succeeded the group takes care of the descendants; otherwise
// the parent-pid scan is used as a best-effort fallback.
void killChild(const PROCESS_INFORMATION &process, bool inJob)
{
    if (!inJob) {
        killProcessTree(process.dwProcessId);
    }
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 5000);
}

bool startsWith(const std::wstring &text, const wchar_t *prefix)
{
    return _wcsnicmp(text.c_str(), prefix, std::wcslen(prefix)) == 0;
}

// Builds a child environment that cannot redirect inspection or trigger external
// helpers. Inheritance is kept for benign variables such as PATH, SystemRoot and
// the user's global configuration (which may legitimately hold safe.directory).
std::vector<wchar_t> makeChildEnvironment()
{
    // Location, namespace and configuration overrides that would redirect which
    // repository, refs, index, objects or configuration are inspected.
    static const wchar_t *kDroppedExactly[] = {
        L"GIT_DIR",
        L"GIT_WORK_TREE",
        L"GIT_COMMON_DIR",
        L"GIT_INDEX_FILE",
        L"GIT_OBJECT_DIRECTORY",
        L"GIT_ALTERNATE_OBJECT_DIRECTORIES",
        L"GIT_NAMESPACE",
        L"GIT_CEILING_DIRECTORIES",
        L"GIT_DISCOVERY_ACROSS_FILESYSTEM",
        L"GIT_SHALLOW_FILE",
        // Configuration *content* injection is dropped. Configuration *location*
        // variables (GIT_CONFIG_GLOBAL, GIT_CONFIG_SYSTEM, GIT_CONFIG_NOSYSTEM)
        // are deliberately kept: they select which configuration the user
        // normally runs with, and dropping them can turn a working setup into an
        // "unsafe ownership" refusal because a safe.directory exception would
        // silently stop applying.
        L"GIT_CONFIG",
        L"GIT_CONFIG_PARAMETERS",
        L"GIT_CONFIG_COUNT",
        // External helper / credential / editor triggers.
        L"GIT_EXEC_PATH",
        L"GIT_EXTERNAL_DIFF",
        L"GIT_DIFF_OPTS",
        L"GIT_SSH",
        L"GIT_SSH_COMMAND",
        L"GIT_SSH_VARIANT",
        L"GIT_ASKPASS",
        L"SSH_ASKPASS",
        L"GIT_SEQUENCE_EDITOR",
        L"GIT_MERGE_AUTOEDIT",
        L"PAGER",
        L"GIT_ALLOW_PROTOCOL",
        L"GIT_PROTOCOL_FROM_USER",
        L"GIT_LITERAL_PATHSPECS",
        L"GIT_GLOB_PATHSPECS",
        L"GIT_NOGLOB_PATHSPECS",
        L"GIT_ICASE_PATHSPECS",
        L"GIT_REFLOG_ACTION",
        L"GIT_CURL_VERBOSE",
        L"GIT_TRACE",
        L"GIT_TRACE_PACKET",
        L"GIT_TRACE_PERFORMANCE",
        L"GIT_TRACE_SETUP",
        L"GIT_FLUSH",
    };

    static const wchar_t *kDroppedPrefixes[] = {
        L"GIT_CONFIG_KEY_",
        L"GIT_CONFIG_VALUE_",
        L"GIT_TRACE_",
    };

    // Variables this helper always sets itself. Inherited entries with the same
    // name are removed so the forced value wins: with duplicates in a Windows
    // environment block the first occurrence wins, and appending alone would
    // leave an inherited value in charge.
    static const wchar_t *kForcedKeys[] = {
        L"GIT_OPTIONAL_LOCKS",
        L"GIT_TERMINAL_PROMPT",
        L"GIT_PAGER",
        L"GIT_EDITOR",
    };

    LPWCH strings = GetEnvironmentStringsW();
    std::vector<std::wstring> kept;
    if (strings) {
        for (LPWCH current = strings; *current;
             current += std::wcslen(current) + 1) {
            const std::wstring entry(current);
            const std::size_t equals = entry.find(L'=');
            const std::wstring key
                = (equals == std::wstring::npos) ? entry
                                                 : entry.substr(0, equals);
            bool drop = false;
            for (const auto *name : kDroppedExactly) {
                if (_wcsicmp(key.c_str(), name) == 0) {
                    drop = true;
                    break;
                }
            }
            if (!drop) {
                for (const auto *prefix : kDroppedPrefixes) {
                    if (startsWith(key, prefix)) {
                        drop = true;
                        break;
                    }
                }
            }
            if (!drop) {
                for (const auto *name : kForcedKeys) {
                    if (_wcsicmp(key.c_str(), name) == 0) {
                        drop = true;
                        break;
                    }
                }
            }
            if (!drop) {
                kept.push_back(entry);
            }
        }
        FreeEnvironmentStringsW(strings);
    }

    // Read-only queries: no optional index refresh, no filesystem monitor, no
    // terminal prompt, no pager and no console window.
    kept.push_back(L"GIT_OPTIONAL_LOCKS=0");
    kept.push_back(L"GIT_TERMINAL_PROMPT=0");
    kept.push_back(L"GIT_PAGER=cat");
    kept.push_back(L"GIT_EDITOR=exit 1");

    std::vector<wchar_t> block;
    for (const auto &entry : kept) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

}  // namespace

GitRunResult runGit(const std::wstring &gitExecutable,
                    const std::wstring &workingDirectory,
                    const std::vector<std::wstring> &arguments,
                    std::size_t maxStdoutBytes,
                    std::size_t maxStderrBytes,
                    DWORD timeoutMs)
{
    GitRunResult outcome;

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength        = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE stdoutRead = nullptr, stdoutWrite = nullptr;
    HANDLE stderrRead = nullptr, stderrWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &attributes, 0)
        || !CreatePipe(&stderrRead, &stderrWrite, &attributes, 0)) {
        if (stdoutRead) CloseHandle(stdoutRead);
        if (stdoutWrite) CloseHandle(stdoutWrite);
        if (stderrRead) CloseHandle(stderrRead);
        if (stderrWrite) CloseHandle(stderrWrite);
        return outcome;
    }
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError  = stderrWrite;
    // The queries never read stdin; a valid handle keeps STARTF_USESTDHANDLES
    // well formed even in a console-less test run.
    startup.hStdInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);

    const std::wstring commandLine = commandLineFor(gitExecutable, arguments);
    std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
    commandBuffer.push_back(L'\0');

    std::vector<wchar_t> environment = makeChildEnvironment();

    std::vector<wchar_t> workingDirectoryBuffer;
    const wchar_t *workingDirectoryArgument = nullptr;
    if (!workingDirectory.empty()) {
        workingDirectoryBuffer.assign(workingDirectory.begin(),
                                     workingDirectory.end());
        workingDirectoryBuffer.push_back(L'\0');
        workingDirectoryArgument = workingDirectoryBuffer.data();
    }
    // lpCurrentDirectory must be NULL or a valid path; an empty string makes
    // CreateProcessW fail.

    ProcessGroupGuard group;

    PROCESS_INFORMATION process{};
    // Created suspended so the job can be assigned before the child can spawn
    // descendants of its own; otherwise a descendant could escape the group.
    const BOOL created = CreateProcessW(
        gitExecutable.c_str(), commandBuffer.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
        environment.data(), workingDirectoryArgument, &startup, &process);

    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);
    if (startup.hStdInput != nullptr
        && startup.hStdInput != INVALID_HANDLE_VALUE) {
        CloseHandle(startup.hStdInput);
    }

    if (!created) {
        CloseHandle(stdoutRead);
        CloseHandle(stderrRead);
        return outcome;
    }
    outcome.started   = true;
    outcome.processId = process.dwProcessId;

    const bool inJob = group.assign(process.hProcess);
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        // A thread that cannot be resumed would hang the read loop, so end the
        // query instead: the group guard still reaps the child.
        outcome.started = false;
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(stdoutRead);
        CloseHandle(stderrRead);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        return outcome;
    }

    // Read both pipes incrementally, enforcing the byte caps while reading
    // rather than after the child exits.
    bool stdoutDone = false, stderrDone = false;
    std::string stdoutData, stderrData;
    std::vector<char> buffer(65536);

    // GetTickCount64: no 49.7-day wrap, so a helper that starts near a system
    // uptime rollover cannot misread its own deadline.
    const ULONGLONG startTick = GetTickCount64();
    const bool hasTimeout     = timeoutMs > 0;

    const auto readPipe = [&](HANDLE pipe, std::string &destination,
                              std::size_t cap, bool &done, bool &exceeded) {
        while (!done) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                done = true;
                break;
            }
            if (available == 0) {
                break;
            }
            DWORD toRead = available;
            if (toRead > buffer.size()) {
                toRead = static_cast<DWORD>(buffer.size());
            }
            DWORD read = 0;
            if (!ReadFile(pipe, buffer.data(), toRead, &read, nullptr)
                || read == 0) {
                done = true;
                break;
            }
            if (destination.size() + read > cap) {
                exceeded = true;
                destination.append(buffer.data(), cap - destination.size());
                done = true;
                break;
            }
            destination.append(buffer.data(), read);
        }
    };

    for (;;) {
        if (!stdoutDone) {
            readPipe(stdoutRead, stdoutData, maxStdoutBytes, stdoutDone,
                     outcome.stdoutExceeded);
        }
        if (!stderrDone) {
            readPipe(stderrRead, stderrData, maxStderrBytes, stderrDone,
                     outcome.stderrExceeded);
        }

        if (outcome.stdoutExceeded || outcome.stderrExceeded) {
            // The caps exist to bound the query, so exceeding one ends it now.
            // Continuing to read until the deadline would leave a child that
            // keeps writing blocked on a full pipe, burning the whole internal
            // deadline for output that is discarded anyway.
            killChild(process, inJob);
            break;
        }

        if (WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0) {
            // Drain anything left before collecting the exit code.
            if (!stdoutDone) {
                readPipe(stdoutRead, stdoutData, maxStdoutBytes, stdoutDone,
                         outcome.stdoutExceeded);
            }
            if (!stderrDone) {
                readPipe(stderrRead, stderrData, maxStderrBytes, stderrDone,
                         outcome.stderrExceeded);
            }
            DWORD exitCode = 0;
            GetExitCodeProcess(process.hProcess, &exitCode);
            outcome.exitCode = static_cast<int>(exitCode);
            break;
        }
        if (hasTimeout
            && (GetTickCount64() - startTick) > timeoutMs) {
            outcome.timedOut = true;
            // Terminate and reap the child (and its tree) before returning so a
            // forced host cancellation cannot leave query workers running.
            killChild(process, inJob);
            break;
        }
    }

    CloseHandle(stdoutRead);
    CloseHandle(stderrRead);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);

    outcome.stdoutText = std::move(stdoutData);
    outcome.stderrText = std::move(stderrData);
    return outcome;
}

}  // namespace gitinfo
