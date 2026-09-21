#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "gitrunner.h"

// Path to the git test double, injected by CMake.
#ifndef FAKE_GIT_PATH
#define FAKE_GIT_PATH ""
#endif

namespace {
int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

std::wstring toWide(const std::string &text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr,
                                         0);
    std::wstring wide(static_cast<std::size_t>(size > 0 ? size - 1 : 0), L'\0');
    if (size > 1) {
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wide[0], size);
    }
    return wide;
}

std::wstring tempScenario(const wchar_t *tag)
{
    wchar_t buffer[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buffer);
    const std::wstring directory = std::wstring(buffer) + L"gitrun_" + tag;
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory;
}

std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        }
        else if (c != '\r') {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

bool contains(const std::vector<std::string> &lines, const std::string &value)
{
    for (const auto &line : lines) {
        if (line == value) {
            return true;
        }
    }
    return false;
}
}  // namespace

int main()
{
    const std::wstring fakeGit = toWide(FAKE_GIT_PATH);

    // 1. A plain query starts, exits 0 and returns the version string.
    {
        const auto result
            = gitinfo::runGit(fakeGit, L"", {L"--version"}, 4096, 4096, 5000);
        check(result.started, "version: process started");
        check(!result.timedOut, "version: no timeout");
        check(result.exitCode == 0, "version: exit 0");
        check(result.stdoutText.find("git version") != std::string::npos,
              "version: reports a version string");
    }

    // 2. Argument round-trip: spaces, quotes and trailing backslashes must
    //    survive the command-line quoting unchanged.
    {
        const std::wstring directory = tempScenario(L"echoargs");
        const std::vector<std::wstring> arguments{
            L"-C", directory, L"--echo", L"plain",
            L"with space",   L"quote\"inside", L"trailing\\",
            L"two\\\\backslashes", L"a b \\\"c\\\""};
        const auto result = gitinfo::runGit(fakeGit, L"", arguments, 65536,
                                            65536, 5000);
        check(result.started, "quoting: process started");
        const auto lines = splitLines(result.stdoutText);
        check(contains(lines, "with space"), "quoting: space preserved");
        check(contains(lines, "quote\"inside"), "quoting: quote preserved");
        check(contains(lines, "trailing\\"), "quoting: trailing backslash kept");
        check(contains(lines, "two\\\\backslashes"),
              "quoting: embedded backslashes kept");
        check(contains(lines, "a b \\\"c\\\""),
              "quoting: mixed spaces, backslashes and quotes kept");
    }

    // 3. Environment sanitization: location, namespace, configuration-content
    //    and helper overrides are dropped, the read-only query variables are
    //    forced (replacing inherited values), and configuration *location*
    //    variables are inherited so safe.directory exceptions keep applying.
    {
        const std::wstring directory = tempScenario(L"echoargs");
        SetEnvironmentVariableW(L"GIT_DIR", L"C:\\redirected.git");
        SetEnvironmentVariableW(L"GIT_WORK_TREE", L"C:\\redirected");
        SetEnvironmentVariableW(L"GIT_INDEX_FILE", L"C:\\redirected\\index");
        SetEnvironmentVariableW(L"GIT_OBJECT_DIRECTORY",
                                L"C:\\redirected\\objects");
        SetEnvironmentVariableW(L"GIT_CONFIG_COUNT", L"1");
        SetEnvironmentVariableW(L"GIT_CONFIG_KEY_0", L"core.fsmonitor");
        SetEnvironmentVariableW(L"GIT_CONFIG_VALUE_0", L"true");
        SetEnvironmentVariableW(L"GIT_CONFIG_PARAMETERS",
                                L"'core.fsmonitor=true'");
        SetEnvironmentVariableW(L"GIT_EXTERNAL_DIFF", L"cmd.exe");
        SetEnvironmentVariableW(L"GIT_TRACE", L"1");
        // An inherited value that the runner must replace, not append to.
        SetEnvironmentVariableW(L"GIT_OPTIONAL_LOCKS", L"1");
        SetEnvironmentVariableW(L"GIT_PAGER", L"less");
        SetEnvironmentVariableW(L"GIT_EDITOR", L"notepad.exe");
        SetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT", L"1");
        // Configuration location: must survive.
        SetEnvironmentVariableW(L"GIT_CONFIG_GLOBAL", L"C:\\my.gitconfig");

        const auto result = gitinfo::runGit(
            fakeGit, L"", {L"-C", directory, L"status"}, 65536, 65536, 5000);

        SetEnvironmentVariableW(L"GIT_DIR", nullptr);
        SetEnvironmentVariableW(L"GIT_WORK_TREE", nullptr);
        SetEnvironmentVariableW(L"GIT_INDEX_FILE", nullptr);
        SetEnvironmentVariableW(L"GIT_OBJECT_DIRECTORY", nullptr);
        SetEnvironmentVariableW(L"GIT_CONFIG_COUNT", nullptr);
        SetEnvironmentVariableW(L"GIT_CONFIG_KEY_0", nullptr);
        SetEnvironmentVariableW(L"GIT_CONFIG_VALUE_0", nullptr);
        SetEnvironmentVariableW(L"GIT_CONFIG_PARAMETERS", nullptr);
        SetEnvironmentVariableW(L"GIT_EXTERNAL_DIFF", nullptr);
        SetEnvironmentVariableW(L"GIT_TRACE", nullptr);
        SetEnvironmentVariableW(L"GIT_OPTIONAL_LOCKS", nullptr);
        SetEnvironmentVariableW(L"GIT_PAGER", nullptr);
        SetEnvironmentVariableW(L"GIT_EDITOR", nullptr);
        SetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT", nullptr);
        SetEnvironmentVariableW(L"GIT_CONFIG_GLOBAL", nullptr);

        check(result.started, "environment: process started");
        const auto lines = splitLines(result.stdoutText);
        check(contains(lines, "GIT_DIR=<unset>"),
              "environment: GIT_DIR is dropped");
        check(contains(lines, "GIT_WORK_TREE=<unset>"),
              "environment: GIT_WORK_TREE is dropped");
        check(contains(lines, "GIT_INDEX_FILE=<unset>"),
              "environment: GIT_INDEX_FILE is dropped");
        check(contains(lines, "GIT_OBJECT_DIRECTORY=<unset>"),
              "environment: GIT_OBJECT_DIRECTORY is dropped");
        check(contains(lines, "GIT_CONFIG_COUNT=<unset>"),
              "environment: injected configuration is dropped");
        check(contains(lines, "GIT_CONFIG_PARAMETERS=<unset>"),
              "environment: GIT_CONFIG_PARAMETERS is dropped");
        check(contains(lines, "GIT_EXTERNAL_DIFF=<unset>"),
              "environment: GIT_EXTERNAL_DIFF is dropped");
        check(contains(lines, "GIT_TRACE=<unset>"),
              "environment: GIT_TRACE is dropped");
        check(contains(lines, "GIT_OPTIONAL_LOCKS=0"),
              "environment: an inherited GIT_OPTIONAL_LOCKS is replaced");
        check(contains(lines, "GIT_TERMINAL_PROMPT=0"),
              "environment: terminal prompts are disabled");
        check(contains(lines, "GIT_PAGER=cat"),
              "environment: an inherited pager is replaced");
        check(contains(lines, "GIT_EDITOR=exit 1"),
              "environment: an inherited editor is replaced");
        check(contains(lines, "GIT_CONFIG_GLOBAL=C:\\my.gitconfig"),
              "environment: configuration location is inherited");
    }

    // 4. Structured arguments reach git unchanged, with no shell involved.
    {
        const std::wstring directory = tempScenario(L"args");
        const std::vector<std::wstring> arguments{
            L"-C", directory, L"--no-optional-locks", L"-c",
            L"core.fsmonitor=false", L"status", L"--porcelain=v2", L"--branch",
            L"-z"};
        const auto result = gitinfo::runGit(fakeGit, L"", arguments, 8192, 4096,
                                            5000);
        check(result.started, "status: started");
        check(result.exitCode == 0, "status: exit 0");
        check(result.stdoutText.find("branch.head") != std::string::npos,
              "status: contains branch header");
    }

    // 5. Internal deadline fires and the child is reaped before returning.
    {
        const std::wstring directory = tempScenario(L"sleeptest");
        const ULONGLONG start = GetTickCount64();
        const auto result = gitinfo::runGit(fakeGit, L"",
                                            {L"-C", directory, L"rev-parse",
                                             L"--git-dir"},
                                            4096, 4096, 500);
        const ULONGLONG elapsed = GetTickCount64() - start;
        check(result.started, "timeout: started");
        check(result.processId != 0, "timeout: child pid is reported");
        check(result.timedOut, "timeout: internal deadline fired");
        check(elapsed < 15000,
              "timeout: the helper returned promptly instead of waiting for "
              "the child");

        // The child must actually be gone: a forced host cancellation must not
        // leave query workers running behind the result.
        const HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, result.processId);
        if (child == nullptr) {
            check(GetLastError() == ERROR_INVALID_PARAMETER,
                  "timeout: the child process no longer exists");
        }
        else {
            const DWORD wait = WaitForSingleObject(child, 0);
            CloseHandle(child);
            check(wait == WAIT_OBJECT_0,
                  "timeout: the child process is terminated and reaped");
        }
    }

    // 6. Output cap is enforced while reading rather than after exit.
    {
        const std::wstring directory = tempScenario(L"dirty");
        const auto result = gitinfo::runGit(fakeGit, L"",
                                            {L"-C", directory, L"status"},
                                            16, 4096, 5000);
        check(result.started, "cap: started");
        check(result.stdoutExceeded, "cap: stdout cap enforced");
        check(result.stdoutText.size() <= 16, "cap: stdout stays within the cap");
    }

    // 7. A missing executable is reported instead of crashing.
    {
        const auto result = gitinfo::runGit(
            L"Z:\\definitely\\missing\\git.exe", L"", {L"--version"}, 4096, 4096,
            2000);
        check(!result.started, "missing executable: not started");
        check(!result.timedOut, "missing executable: no timeout");
    }

    if (failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    return 1;
}
