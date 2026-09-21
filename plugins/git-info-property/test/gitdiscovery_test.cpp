#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "gitdiscovery.h"

namespace {
int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

// Saves and restores PATH so the test cannot leak into other tests.
class PathGuard {
public:
    PathGuard()
    {
        const DWORD length = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (length > 0) {
            std::vector<wchar_t> buffer(length);
            GetEnvironmentVariableW(L"PATH", buffer.data(), length);
            saved_ = buffer.data();
        }
    }
    ~PathGuard() { SetEnvironmentVariableW(L"PATH", saved_.c_str()); }

private:
    std::wstring saved_;
};

std::wstring tempDirectory(const wchar_t *tag)
{
    wchar_t buffer[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buffer);
    const std::wstring path = std::wstring(buffer) + L"gitdisco_" + tag;
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

void writeDummyFile(const std::wstring &path)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, "x", 1, &written, nullptr);
        CloseHandle(file);
    }
}
}  // namespace

int main()
{
    PathGuard guard;

    // 1. Overrides must be absolute paths to an existing file.
    {
        std::wstring resolved;
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        check(gitinfo::isUsableGitOverride(self, resolved),
              "absolute existing path is accepted");
        check(gitinfo::isUsableGitOverride(L"git.exe", resolved) == false,
              "bare file name is rejected");
        check(gitinfo::isUsableGitOverride(L"tools\\git.exe", resolved) == false,
              "relative path is rejected");
        check(gitinfo::isUsableGitOverride(L"", resolved) == false,
              "empty path is rejected");
        check(gitinfo::isUsableGitOverride(L"Z:\\missing\\git.exe", resolved)
                  == false,
              "missing file is rejected");
    }

    // 2. Empty and relative PATH entries are skipped; the first usable absolute
    //    entry wins and is resolved to an absolute path.
    {
        const std::wstring directory = tempDirectory(L"a7f3");
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const std::wstring fake = directory + L"\\git.exe";
        CopyFileW(self, fake.c_str(), FALSE);

        const std::wstring missing = tempDirectory(L"missing_0000");
        RemoveDirectoryW(missing.c_str());
        const std::wstring pathValue
            = std::wstring(L";relative;..\\relative2;") + directory + L";"
              + missing;
        SetEnvironmentVariableW(L"PATH", pathValue.c_str());

        const std::wstring found = gitinfo::discoverGitFromPath();
        check(!found.empty(), "discovery found an absolute candidate");
        check(found == fake, "discovery resolved the absolute fake git.exe");

        DeleteFileW(fake.c_str());
        RemoveDirectoryW(directory.c_str());
    }

    // 3. Only git.exe is accepted: a batch shim cannot be launched through
    //    CreateProcessW and must not be selected.
    {
        const std::wstring directory = tempDirectory(L"cmd_only_5b1c");
        writeDummyFile(directory + L"\\git.cmd");
        SetEnvironmentVariableW(L"PATH", directory.c_str());
        check(gitinfo::discoverGitFromPath().empty(),
              "git.cmd alone is not selected");
        DeleteFileW((directory + L"\\git.cmd").c_str());
        RemoveDirectoryW(directory.c_str());
    }

    // 4. The inspected directory never supplies the executable used to inspect
    //    it, even when it is on PATH.
    {
        const std::wstring inspected = tempDirectory(L"repo_4e77");
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const std::wstring localGit = inspected + L"\\git.exe";
        CopyFileW(self, localGit.c_str(), FALSE);

        SetEnvironmentVariableW(L"PATH", inspected.c_str());
        check(gitinfo::discoverGitFromPath(inspected).empty(),
              "git.exe inside the inspected directory is skipped");
        check(gitinfo::discoverGitFromPath() == localGit,
              "the exclusion is caller-driven: without the inspected directory "
              "the entry is still usable");

        DeleteFileW(localGit.c_str());
        RemoveDirectoryW(inspected.c_str());
    }

    // 5. Nothing usable on PATH yields an empty result.
    {
        SetEnvironmentVariableW(L"PATH", L"C:\\Windows\\System32");
        check(gitinfo::discoverGitFromPath().empty(),
              "no git.exe on PATH yields an empty result");
    }

    if (failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    return 1;
}
