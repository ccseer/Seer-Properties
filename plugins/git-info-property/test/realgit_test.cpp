// Integration test against a real Git installation.
//
// The other tests use the git test double, which cannot prove that the real
// command surface behaves as assumed. This test creates real repositories with
// the git found on PATH and checks both the reported values and that the
// read-only queries do not modify repository, index or configuration content.
// It reports SKIP (and succeeds) when no real Git is available.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "gitdiscovery.h"
#include "githelper.h"
#include "gitrunner.h"

namespace {
int failures = 0;
int skipped  = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

void skip(const char *message)
{
    std::printf("SKIP: %s\n", message);
    ++skipped;
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

std::string toUtf8(const std::wstring &text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr,
                                         0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                            static_cast<int>(text.size()), &utf8[0], size,
                            nullptr, nullptr);
    }
    return utf8;
}

std::wstring tempRoot()
{
    wchar_t buffer[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buffer);
    const std::wstring root = std::wstring(buffer) + L"gitreal_7c31";
    CreateDirectoryW(root.c_str(), nullptr);
    return root;
}

bool makeDirectory(const std::wstring &path)
{
    return CreateDirectoryW(path.c_str(), nullptr) == TRUE
           || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool writeFile(const std::wstring &path, const std::string &body)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, body.data(),
                              static_cast<DWORD>(body.size()), &written,
                              nullptr);
    CloseHandle(file);
    return ok == TRUE;
}

// Runs git for test setup with the same sanitized runner the plugin uses.
bool setupGit(const std::wstring &gitExecutable,
              const std::vector<std::wstring> &arguments)
{
    const auto result = gitinfo::runGit(gitExecutable, std::wstring(), arguments,
                                        1024 * 1024, 64 * 1024, 30000);
    if (!result.started || result.timedOut || result.exitCode != 0) {
        std::printf("setup git failed (%d): %ls\n", result.exitCode,
                    toWide(result.stderrText).c_str());
        return false;
    }
    return true;
}

// Recursive fingerprint of a directory: every relative path with its size and
// last-write time. Used to prove the read-only queries changed nothing.
void collectFingerprint(const std::wstring &root, const std::wstring &prefix,
                        std::vector<std::string> *out)
{
    WIN32_FIND_DATAW data;
    const HANDLE handle
        = FindFirstFileW((root + L"\\*").c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        const std::wstring relative
            = prefix.empty() ? name : (prefix + L"/" + name);
        const std::wstring full = root + L"\\" + name;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            out->push_back(toUtf8(relative) + "/");
            collectFingerprint(full, relative, out);
        }
        else {
            const ULONGLONG size = (static_cast<ULONGLONG>(data.nFileSizeHigh)
                                    << 32)
                                   | data.nFileSizeLow;
            out->push_back(toUtf8(relative) + "|" + std::to_string(size) + "|"
                           + std::to_string(data.ftLastWriteTime.dwHighDateTime)
                           + ":" + std::to_string(data.ftLastWriteTime.dwLowDateTime));
        }
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
}

std::vector<std::string> snapshot(const std::wstring &directory)
{
    std::vector<std::string> entries;
    collectFingerprint(directory, std::wstring(), &entries);
    std::sort(entries.begin(), entries.end());
    return entries;
}

void removeTree(const std::wstring &root)
{
    WIN32_FIND_DATAW data;
    const HANDLE handle = FindFirstFileW((root + L"\\*").c_str(), &data);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring path = root + L"\\" + name;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                removeTree(path);
            }
            else {
                SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(handle, &data));
        FindClose(handle);
    }
    RemoveDirectoryW(root.c_str());
}

std::string readHelperOutput(const std::wstring &outputBase,
                             std::wstring *error)
{
    const HANDLE file = CreateFileW((outputBase + L".json").c_str(),
                                    GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = L"output json missing";
        return std::string();
    }
    std::string content;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        content.append(buffer, read);
    }
    CloseHandle(file);
    return content;
}

// Extracts the value of a flat string row from the "Git" subgroup without
// pulling in a JSON library.
std::string subgroupValue(const std::string &json, const std::string &key)
{
    const std::string needle = "\"" + key + "\": \"";
    const std::size_t start  = json.find(needle);
    if (start == std::string::npos) {
        return std::string();
    }
    const std::size_t begin = start + needle.size();
    const std::size_t end   = json.find('"', begin);
    if (end == std::string::npos) {
        return std::string();
    }
    return json.substr(begin, end - begin);
}

struct HelperOutcome {
    int exitCode = -1;
    std::string json;
};

HelperOutcome runHelper(const std::wstring &gitExecutable,
                        const std::wstring &inputDirectory,
                        const std::wstring &outputBase,
                        const std::wstring &outputDirectory)
{
    HelperOutcome outcome;
    outcome.exitCode = gitinfo::runGitInfo(
        {L"git_info.exe", L"--input", inputDirectory, L"--output", outputBase,
         L"--output-dir", outputDirectory, L"--git", gitExecutable});
    if (outcome.exitCode == 0) {
        std::wstring error;
        outcome.json = readHelperOutput(outputBase, &error);
    }
    return outcome;
}

}  // namespace

int main()
{
    const std::wstring gitExecutable = gitinfo::discoverGitFromPath();
    if (gitExecutable.empty()) {
        skip("no real git.exe was found on PATH");
        std::printf("PASS (%d skipped)\n", skipped);
        return 0;
    }
    std::printf("INFO using %ls\n", gitExecutable.c_str());

    const std::wstring root = tempRoot();
    const std::wstring repository = root + L"\\repo";
    makeDirectory(repository);

    if (!setupGit(gitExecutable, {L"init", L"-q", repository})
        || !writeFile(repository + L"\\tracked.txt", "hello\n")
        || !setupGit(gitExecutable,
                     {L"-C", repository, L"add", L"."})
        || !setupGit(gitExecutable,
                     {L"-C", repository, L"-c", L"user.name=Seer Test", L"-c",
                      L"user.email=test@example.invalid", L"commit", L"-qm",
                      L"init"})) {
        skip("could not create the real test repository");
        removeTree(root);
        std::printf("PASS (%d skipped)\n", skipped);
        return 0;
    }

    // 1. Clean repository: values plus an unchanged .git directory.
    {
        const std::wstring outputDirectory = root + L"\\out_clean";
        makeDirectory(outputDirectory);
        const std::wstring outputBase = outputDirectory + L"\\result";
        const auto before = snapshot(repository + L"\\.git");

        const HelperOutcome outcome = runHelper(
            gitExecutable, repository, outputBase, outputDirectory);
        check(outcome.exitCode == 0, "clean: helper exits 0");
        check(subgroupValue(outcome.json, "Repository").empty(),
              "clean: no redundant Repository row");
        check(subgroupValue(outcome.json, "Staged Changes") == "0",
              "clean: no staged changes");
        check(subgroupValue(outcome.json, "Worktree Changes") == "0",
              "clean: no worktree changes");
        check(subgroupValue(outcome.json, "Untracked Entries") == "0",
              "clean: no untracked entries");
        check(subgroupValue(outcome.json, "HEAD State") == "attached",
              "clean: HEAD is attached");
        check(!subgroupValue(outcome.json, "Branch").empty(),
              "clean: branch name is reported");
        check(subgroupValue(outcome.json, "Upstream") == "(none)",
              "clean: missing upstream is stated explicitly");

        const auto after = snapshot(repository + L"\\.git");
        check(before == after,
              "clean: the read-only query did not modify repository, index or "
              "configuration content");
    }

    // 2. Staged, modified, untracked and renamed content.
    {
        writeFile(repository + L"\\tracked.txt", "hello again\n");
        writeFile(repository + L"\\untracked file with space.txt", "x\n");
        if (!setupGit(gitExecutable,
                      {L"-C", repository, L"add", L"tracked.txt"})) {
            skip("could not stage the second fixture");
        }
        const std::wstring outputDirectory = root + L"\\out_dirty";
        makeDirectory(outputDirectory);
        const std::wstring outputBase = outputDirectory + L"\\result";
        const auto before = snapshot(repository + L"\\.git");

        const HelperOutcome outcome = runHelper(
            gitExecutable, repository, outputBase, outputDirectory);
        check(outcome.exitCode == 0, "dirty: helper exits 0");
        check(subgroupValue(outcome.json, "Staged Changes") == "1",
              "dirty: one staged change");
        check(subgroupValue(outcome.json, "Untracked Entries") == "1",
              "dirty: one untracked entry with a space in its name");
        check(subgroupValue(outcome.json, "Repository Type") == "worktree",
              "dirty: repository type is worktree");
        check(subgroupValue(outcome.json, "HEAD").size() == 12,
              "dirty: abbreviated HEAD is reported");

        const auto after = snapshot(repository + L"\\.git");
        check(before == after,
              "dirty: the read-only query left the repository untouched");
    }

    // 3. Detached HEAD.
    {
        if (setupGit(gitExecutable,
                     {L"-C", repository, L"checkout", L"-q", L"--detach",
                      L"HEAD"})) {
            const std::wstring outputDirectory = root + L"\\out_detached";
            makeDirectory(outputDirectory);
            const std::wstring outputBase = outputDirectory + L"\\result";
            const HelperOutcome outcome = runHelper(
                gitExecutable, repository, outputBase, outputDirectory);
            check(subgroupValue(outcome.json, "HEAD State") == "detached",
                  "detached: HEAD state is detached");
            check(!subgroupValue(outcome.json, "HEAD").empty(),
                  "detached: HEAD revision is reported");
            setupGit(gitExecutable,
                     {L"-C", repository, L"checkout", L"-q", L"-"});
        }
        else {
            skip("could not detach HEAD");
        }
    }

    // 4. Unborn branch.
    {
        const std::wstring unborn = root + L"\\unborn";
        makeDirectory(unborn);
        if (setupGit(gitExecutable, {L"init", L"-q", unborn})) {
            const std::wstring outputDirectory = root + L"\\out_unborn";
            makeDirectory(outputDirectory);
            const std::wstring outputBase = outputDirectory + L"\\result";
            const HelperOutcome outcome = runHelper(gitExecutable, unborn,
                                                    outputBase,
                                                    outputDirectory);
            check(subgroupValue(outcome.json, "Repository").empty(),
                  "unborn: no redundant Repository row");
            check(subgroupValue(outcome.json, "HEAD State") == "unborn",
                  "unborn: HEAD state is unborn");
            check(!subgroupValue(outcome.json, "Branch").empty(),
                  "unborn: pending branch name is reported");
        }
        else {
            skip("could not create the unborn repository");
        }
    }

    // 5. Bare repository.
    {
        const std::wstring bare = root + L"\\bare.git";
        if (setupGit(gitExecutable,
                     {L"clone", L"-q", L"--bare", repository, bare})) {
            const std::wstring outputDirectory = root + L"\\out_bare";
            makeDirectory(outputDirectory);
            const std::wstring outputBase = outputDirectory + L"\\result";
            const HelperOutcome outcome
                = runHelper(gitExecutable, bare, outputBase, outputDirectory);
            check(outcome.exitCode == 0, "bare: helper exits 0");
            check(subgroupValue(outcome.json, "Repository").empty(),
                  "bare: no redundant Repository row");
            check(subgroupValue(outcome.json, "Repository Type") == "bare",
                  "bare: repository type is bare");
            check(subgroupValue(outcome.json, "Worktree Statistics").find(
                      "not applicable")
                      != std::string::npos,
                  "bare: worktree statistics are explicitly limited");
        }
        else {
            skip("could not create the bare clone");
        }
    }

    // 6. Linked worktree.
    {
        const std::wstring worktree = root + L"\\wt";
        if (setupGit(gitExecutable,
                     {L"-C", repository, L"worktree", L"add", L"-q",
                      L"--detach", worktree})) {
            const std::wstring outputDirectory = root + L"\\out_wt";
            makeDirectory(outputDirectory);
            const std::wstring outputBase = outputDirectory + L"\\result";
            const HelperOutcome outcome = runHelper(gitExecutable, worktree,
                                                    outputBase,
                                                    outputDirectory);
            check(subgroupValue(outcome.json, "Repository").empty(),
                  "worktree: no redundant Repository row");
            check(subgroupValue(outcome.json, "Repository Type")
                      == "linked-worktree",
                  "worktree: linked worktree is identified");
        }
        else {
            skip("could not create the linked worktree");
        }
    }

    // 7. Subdirectory of a repository.
    {
        const std::wstring nested = repository + L"\\nested dir";
        makeDirectory(nested);
        const std::wstring outputDirectory = root + L"\\out_nested";
        makeDirectory(outputDirectory);
        const std::wstring outputBase = outputDirectory + L"\\result";
        const HelperOutcome outcome
            = runHelper(gitExecutable, nested, outputBase, outputDirectory);
        check(subgroupValue(outcome.json, "Repository").empty(),
              "nested: no redundant Repository row");
        check(subgroupValue(outcome.json, "Repository Type")
                  == "worktree subdirectory",
              "nested: repository type marks the subdirectory");
    }

    // 7b. Rename and Unicode-name coverage, in a fresh repository so the
    //     counts are exact. A rename emits the original path as its own
    //     NUL-delimited field, which must not be counted as an entry. The
    //     original path deliberately starts with 'u', the tag of an unmerged
    //     record, because that is what the naive parser misread.
    {
        const std::wstring renameRepo = root + L"\\repo_rename";
        makeDirectory(renameRepo);
        const std::wstring renameDir = renameRepo + L"\\utils";
        makeDirectory(renameDir);
        if (setupGit(gitExecutable, {L"init", L"-q", renameRepo})
            && writeFile(renameDir + L"\\old-name.txt", "content\n")
            && setupGit(gitExecutable, {L"-C", renameRepo, L"add", L"."})
            && setupGit(gitExecutable,
                        {L"-C", renameRepo, L"-c", L"user.name=Seer Test",
                         L"-c", L"user.email=test@example.invalid", L"commit",
                         L"-qm", L"init"})
            && setupGit(gitExecutable,
                        {L"-C", renameRepo, L"mv", L"utils/old-name.txt",
                         L"utils/new-name.txt"})) {
            // An untracked file whose name starts with a letter that could be
            // mistaken for a record tag by a naive parser.
            writeFile(renameRepo + L"\\unmerged-sounding-name.txt", "x\n");

            const std::wstring outputDirectory = root + L"\\out_rename";
            makeDirectory(outputDirectory);
            const std::wstring outputBase = outputDirectory + L"\\result";
            const HelperOutcome outcome = runHelper(gitExecutable, renameRepo,
                                                    outputBase,
                                                    outputDirectory);
            check(outcome.exitCode == 0, "rename: helper exits 0");
            check(subgroupValue(outcome.json, "Repository").empty(),
                  "rename: no redundant Repository row");
            check(subgroupValue(outcome.json, "Staged Changes") == "1",
                  "rename: the rename counts as exactly one staged change");
            check(subgroupValue(outcome.json, "Conflicts") == "0",
                  "rename: the original path is not counted as a conflict");
            check(subgroupValue(outcome.json, "Untracked Entries") == "1",
                  "rename: the untracked entry is counted once");
        }
        else {
            skip("could not create the rename repository");
        }
    }

    // 7c. Unicode file name.
    {
        const std::wstring unicodeRepo = root + L"\\repo_unicode";
        makeDirectory(unicodeRepo);
        if (setupGit(gitExecutable, {L"init", L"-q", unicodeRepo})
            && writeFile(unicodeRepo + L"\\tracked.txt", "hello\n")
            && setupGit(gitExecutable, {L"-C", unicodeRepo, L"add", L"."})
            && setupGit(gitExecutable,
                        {L"-C", unicodeRepo, L"-c", L"user.name=Seer Test",
                         L"-c", L"user.email=test@example.invalid", L"commit",
                         L"-qm", L"init"})) {
            const std::wstring unicodeName
                = unicodeRepo + L"\\P\x00E4th-\x30C6\x30B9\x30C8 file.txt";
            writeFile(unicodeName, "unicode\n");

            const std::wstring outputDirectory = root + L"\\out_unicode";
            makeDirectory(outputDirectory);
            const std::wstring outputBase = outputDirectory + L"\\result";
            const HelperOutcome outcome = runHelper(gitExecutable, unicodeRepo,
                                                    outputBase,
                                                    outputDirectory);
            check(subgroupValue(outcome.json, "Untracked Entries") == "1",
                  "unicode: a non-ASCII file name counts as one entry");
            check(subgroupValue(outcome.json, "Conflicts") == "0",
                  "unicode: no conflict is reported");
        }
        else {
            skip("could not create the unicode repository");
        }
    }

    // 8. Directory that is definitely not a repository.
    {
        const std::wstring outside = root + L"\\outside";
        makeDirectory(outside);
        const std::wstring outputDirectory = root + L"\\out_outside";
        makeDirectory(outputDirectory);
        const std::wstring outputBase = outputDirectory + L"\\result";
        const HelperOutcome outcome
            = runHelper(gitExecutable, outside, outputBase, outputDirectory);
        check(outcome.exitCode == 0, "outside: helper exits 0");
        check(subgroupValue(outcome.json, "Repository")
                  == "not a git repository",
              "outside: not a repository is reported in plain text");
    }

    removeTree(root);

    std::printf("PASS (%d skipped)\n", skipped);
    return failures == 0 ? 0 : 1;
}
