#include <windows.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "githelper.h"

#ifndef FAKE_GIT_PATH
#define FAKE_GIT_PATH ""
#endif

namespace {
using json = nlohmann::json;

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

void checkEq(const std::string &got, const std::string &expected,
             const char *message)
{
    if (got != expected) {
        std::printf("FAIL: %s (got '%s', expected '%s')\n", message,
                    got.c_str(), expected.c_str());
        ++failures;
    }
}

// ---- parsed-result accessors ------------------------------------------------

const json *member(const json &value, const char *key)
{
    if (!value.is_object()) {
        return nullptr;
    }
    const auto it = value.find(key);
    return it == value.end() ? nullptr : &(*it);
}

std::string str(const json &value, const char *key)
{
    const json *found = member(value, key);
    return (found && found->is_string()) ? found->get<std::string>()
                                         : std::string();
}

// Objects and arrays stay retrievable by path without deep nesting helpers.
const json *path(const json &root, const char *first, const char *second,
                 const char *third)
{
    const json *value = &root;
    for (const char *key : {first, second, third}) {
        if (key == nullptr) {
            break;
        }
        value = member(*value, key);
        if (value == nullptr) {
            return nullptr;
        }
    }
    return value;
}

// The reference host drops non-flat subgroup rows, so every row must be a plain
// string/scalar and no row may be a nested object.
bool everyRowIsFlat(const json &group)
{
    if (!group.is_object()) {
        return false;
    }
    for (const auto &item : group.items()) {
        const json &row = item.value();
        const bool flat = row.is_string() || row.is_number() || row.is_boolean()
                          || row.is_array();
        if (!flat) {
            std::printf("FAIL: subgroup row '%s' is not flat\n",
                        item.key().c_str());
            return false;
        }
    }
    return true;
}

// ---- helpers ----------------------------------------------------------------

// The Git subgroup envelope: present whenever the plugin published the group.
const json *groupEnvelope(const json &document)
{
    return path(document, "data", "Git", nullptr);
}

// The subgroup value is an array of one-key fields in the plugin's own order;
// merge them into an object so the key-based assertions keep working.
json mergedGroup(const json &document)
{
    json merged = json::object();
    const json *value = path(document, "data", "Git", "value");
    if (value == nullptr || !value->is_array()) {
        return merged;
    }
    for (const auto &item : *value) {
        if (!item.is_object()) {
            continue;
        }
        for (auto field = item.begin(); field != item.end(); ++field) {
            merged[field.key()] = field.value();
        }
    }
    return merged;
}


std::string readAll(const std::wstring &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream stream;
    stream << in.rdbuf();
    return stream.str();
}

bool makeDirectory(const std::wstring &path)
{
    return CreateDirectoryW(path.c_str(), nullptr) == TRUE
           || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring tempRoot(const wchar_t *tag)
{
    wchar_t buffer[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buffer);
    const std::wstring root = std::wstring(buffer) + L"gitout_" + tag + L"_d1e9";
    makeDirectory(root);
    return root;
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

std::string toNarrow(const std::wstring &text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr,
                                         0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                            static_cast<int>(text.size()), &narrow[0], size,
                            nullptr, nullptr);
    }
    return narrow;
}

struct HelperRun {
    int exitCode = -1;
    json document;
};

HelperRun runHelper(const std::wstring &inputDirectory,
                    const std::wstring &outputBase,
                    const std::wstring &gitOverride)
{
    HelperRun run;
    const std::size_t separator = outputBase.find_last_of(L"\\/");
    const std::wstring outputDirectory = outputBase.substr(0, separator);
    std::vector<std::wstring> arguments{L"git_info.exe", L"--input",
                                        inputDirectory, L"--output", outputBase,
                                        L"--output-dir", outputDirectory};
    if (!gitOverride.empty()) {
        arguments.push_back(L"--git");
        arguments.push_back(gitOverride);
    }
    run.exitCode = gitinfo::runGitInfo(arguments);
    if (run.exitCode == 0) {
        run.document = json::parse(readAll(outputBase + L".json"), nullptr,
                                   false);
        if (run.document.is_discarded()) {
            std::printf("FAIL: helper output is not valid JSON\n");
            ++failures;
        }
    }
    return run;
}

std::wstring scenarioPath(const std::wstring &root, const wchar_t *name)
{
    const std::wstring path = root + L"\\" + name;
    makeDirectory(path);
    return path;
}

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
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(handle, &data));
        FindClose(handle);
    }
    RemoveDirectoryW(root.c_str());
}

void runScenario(const std::wstring &root, const wchar_t *name,
                 const std::vector<std::pair<const char *, const char *>>
                     &expected)
{
    const std::string label         = toNarrow(name);
    const std::wstring directory    = scenarioPath(root, name);
    const std::wstring outputBase   = root + L"\\" + name + L"_out";

    const HelperRun run = runHelper(directory, outputBase, toWide(FAKE_GIT_PATH));
    if (run.exitCode != 0) {
        std::printf("FAIL: %s scenario returned exit %d\n", label.c_str(),
                    run.exitCode);
        ++failures;
        return;
    }
    const json group = mergedGroup(run.document);
    if (groupEnvelope(run.document) == nullptr) {
        std::printf("FAIL: %s scenario produced no Git group\n", label.c_str());
        ++failures;
        return;
    }
    if (!everyRowIsFlat(group)) {
        std::printf("FAIL: %s scenario has a non-flat row\n", label.c_str());
        ++failures;
    }
    for (const auto &pair : expected) {
        const std::string message = label + " scenario row " + pair.first;
        checkEq(str(group, pair.first), pair.second, message.c_str());
    }
}

}  // namespace

int main()
{
    const std::wstring root           = tempRoot(L"base");
    const std::wstring fakeGit        = toWide(FAKE_GIT_PATH);
    const std::wstring cleanDirectory = scenarioPath(root, L"clean");

    // 1. Git unavailable: valid Property JSON and exit 0 are still required.
    {
        PathGuard guard;
        SetEnvironmentVariableW(L"PATH", L"C:\\Windows\\System32");
        const std::wstring directory = scenarioPath(root, L"nogit");
        const std::wstring outputBase = root + L"\\nogit_out";
        const HelperRun run = runHelper(directory, outputBase, std::wstring());
        check(run.exitCode == 0, "missing git still returns exit 0");
        check(run.document.value("result_schema", 0) == 1,
              "missing git publishes result_schema 1");
        const json group = mergedGroup(run.document);
        check(groupEnvelope(run.document) != nullptr, "missing git publishes the Git subgroup");
        if (groupEnvelope(run.document) != nullptr) {
            check(str(group, "Reason").find("no usable git.exe")
                      != std::string::npos,
                  "missing git explains that no usable git.exe was found");
            check(member(group, "Git executable") == nullptr,
                  "missing git reports no executable row");
            check(member(group, "Git version") == nullptr,
                  "missing git reports no version row");
            check(member(group, "Repository") == nullptr,
                  "missing git does not claim a repository state");
            check(everyRowIsFlat(group), "missing git rows are flat");
        }
    }

    // 2. Explicit override that is not an absolute existing file.
    {
        const std::wstring outputBase = root + L"\\bogus_out";
        const HelperRun run
            = runHelper(cleanDirectory, outputBase, L"not_absolute.exe");
        check(run.exitCode == 0, "invalid override still returns exit 0");
        const json group = mergedGroup(run.document);
        check(str(group, "Reason").find("--git override")
                         != std::string::npos,
              "invalid override explains why it was rejected");
        check(member(group, "Git executable") == nullptr,
              "invalid override reports no executable row");
    }

    // 3. Ordinary non-repository directory.
    runScenario(root, L"notrepo", {{"Repository", "not a git repository"}});

    // 4. Clean repository with an upstream that is in sync.
    runScenario(root, L"clean",
                {{"HEAD State", "attached"},
                 {"Branch", "main"},
                 {"HEAD", "abcdef012345"},
                 {"Upstream", "origin/main"},
                 {"Ahead", "0"},
                 {"Behind", "0"},
                 {"Staged Changes", "0"},
                 {"Worktree Changes", "0"},
                 {"Conflicts", "0"},
                 {"Untracked Entries", "0"},
                 {"Repository Type", "worktree"}});

    // 5. Dirty repository: rename, conflict and a collapsed untracked directory.
    runScenario(root, L"dirty",
                {{"Ahead", "2"},
                 {"Behind", "3"},
                 {"Staged Changes", "1"},
                 {"Worktree Changes", "1"},
                 {"Conflicts", "1"},
                 {"Untracked Entries", "1"}});

    // 6. Unborn branch.
    runScenario(root, L"unborn",
                {{"HEAD State", "unborn"},
                 {"Branch", "main"}});

    // 7. Detached HEAD.
    runScenario(root, L"detached",
                {{"HEAD State", "detached"},
                 {"HEAD", "abcdef012345"}});

    // 8. No upstream: stated explicitly instead of omitted.
    runScenario(root, L"noupstream",
                {{"Branch", "feature/x"},
                 {"Upstream", "(none)"}});

    // 9. Bare repository: valid JSON with an explicitly limited result.
    runScenario(root, L"bare",
                {{"Repository Type", "bare"},
                 {"HEAD State", "unborn"},
                 {"Worktree Statistics",
                  "not applicable: worktree statistics do not apply to a bare "
                  "repository"}});

    // 10. Bare repository that has a commit.
    runScenario(root, L"barecommit",
                {{"Repository Type", "bare"},
                 {"HEAD State", "attached"},
                 {"Branch", "main"}});

    // 11. Linked worktree.
    runScenario(root, L"linkedworktree",
                {{"Repository Type", "linked-worktree"}});

    // 12. Subdirectory of a repository.
    {
        const std::wstring nested = cleanDirectory + L"\\nested";
        makeDirectory(nested);
        const std::wstring outputBase = root + L"\\subdir_out";
        const HelperRun run = runHelper(nested, outputBase, fakeGit);
        const json group = mergedGroup(run.document);
        check(member(group, "Repository") == nullptr,
              "subdirectory publishes no redundant repository row");
        check(str(group, "Repository Type").find("subdirectory")
                         != std::string::npos,
              "subdirectory is identified as a subdirectory");
    }

    // 13. Conflict-only repository.
    runScenario(root, L"conflict", {{"Conflicts", "1"}});

    // 14. Unicode and space-containing untracked paths count as entries.
    runScenario(root, L"unicode",
                {{"Untracked Entries", "2"}});

    // 15. Unsafe ownership is its own state, never "not".
    runScenario(root, L"unsafe", {{"Repository", "unsafe-ownership"}});

    // 16. Permission failure is its own state.
    runScenario(root, L"denied", {{"Repository", "permission"}});

    // 17. Corruption reported by git status is its own state.
    runScenario(root, L"corrupt", {{"Repository", "corrupt"}});

    // 18. Unsupported (old) Git version: repository detection still works, the
    //     machine-readable status query is reported as unavailable, and the
    //     version-specific flag is not passed (the test double rejects it).
    {
        const std::wstring directory = scenarioPath(root, L"clean_oldversion");
        const std::wstring outputBase = root + L"\\oldversion_out";
        const HelperRun run = runHelper(directory, outputBase, fakeGit);
        const json group = mergedGroup(run.document);
        check(member(group, "Repository") == nullptr,
              "old git publishes no redundant repository row");
        check(member(group, "Git version") == nullptr,
              "old git version is not published");
        check(str(group, "Query Error").find("2.11") != std::string::npos,
              "old git reports the porcelain v2 limitation");
        check(member(group, "Staged Changes") == nullptr,
              "old git publishes no change counts");
    }

    // 19. Internal timeout: bounded timeout state, valid JSON, exit 0.
    {
        const std::wstring directory = scenarioPath(root, L"clean_sleeptest");
        const std::wstring outputBase = root + L"\\timeout_out";
        const HelperRun run = runHelper(directory, outputBase, fakeGit);
        check(run.exitCode == 0, "timeout returns exit 0");
        const json group = mergedGroup(run.document);
        check(str(group, "Repository") == "timeout",
              "timeout is reported as a bounded timeout state");
        check(str(group, "Query Error").find("deadline")
                         != std::string::npos,
              "timeout explains that the child was terminated");
    }

    // 20. Escaping: a path with quotes, a backslash and non-ASCII text must
    //     survive the write/parse round trip unchanged. A serializer that
    //     concatenated raw values would emit invalid JSON here.
    {
        const std::wstring directory = scenarioPath(root, L"escape");
        const std::wstring outputBase = root + L"\\escape_out";
        const HelperRun run = runHelper(directory, outputBase, fakeGit);
        check(run.exitCode == 0, "escape: helper exits 0");
        const json group = mergedGroup(run.document);
        check(groupEnvelope(run.document) != nullptr, "escape: group is present");
        if (groupEnvelope(run.document) != nullptr) {
            checkEq(str(group, "Repository Root"),
                    "C:\\\\odd\\\"quoted\\\"\\\\p\xC3\xA4th",
                    "escape: quoted, backslashed and non-ASCII text round-trips");
        }
    }

    // 21. The helper's own executable path and its version are implementation
    //     details of the inspection, not repository facts: neither row may be
    //     published, so the subgroup only carries repository state and
    //     diagnostics.
    {
        const std::wstring outputBase = root + L"\\nofields_out";
        const HelperRun run = runHelper(cleanDirectory, outputBase, fakeGit);
        check(run.exitCode == 0, "no-fields: helper exits 0");
        const json group = mergedGroup(run.document);
        check(member(group, "Repository") == nullptr,
              "no-fields: no redundant repository row");
        if (groupEnvelope(run.document) != nullptr) {
            check(member(group, "Git executable") == nullptr,
                  "no-fields: the executable path is never published");
            check(member(group, "Git version") == nullptr,
                  "no-fields: the git version is never published");
        }
    }

    // 22. The subgroup value is an array of one-key fields; the array order is
    //     the plugin's own render order and is pinned here (the keys would sort
    //     differently).
    {
        const std::wstring outputBase = root + L"\\order_out";
        const HelperRun run = runHelper(cleanDirectory, outputBase, fakeGit);
        check(run.exitCode == 0, "order: helper exits 0");
        const json *value = path(run.document, "data", "Git", "value");
        check(value != nullptr && value->is_array(),
              "order: the subgroup value is an array");
        if (value != nullptr && value->is_array()) {
            std::vector<std::string> keys;
            for (const auto &item : *value) {
                for (auto field = item.begin(); field != item.end(); ++field) {
                    keys.push_back(field.key());
                }
            }
            const std::vector<std::string> expected = {
                "HEAD State",        "Branch",
                "HEAD",              "Upstream",
                "Ahead",             "Behind",
                "Repository Type",   "Repository Root",
                "Staged Changes",    "Worktree Changes",
                "Conflicts",         "Untracked Entries",
                "Untracked Mode",
            };
            if (keys != expected) {
                std::printf("FAIL: order: actual sequence:");
                for (const auto &k : keys) {
                    std::printf(" %s", k.c_str());
                }
                std::printf("\n");
            }
            check(keys == expected, "order: the field sequence is pinned");
        }
    }

    removeTree(root);

    if (failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    return 1;
}
