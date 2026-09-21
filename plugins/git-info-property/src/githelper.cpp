#include "githelper.h"

// Shared path and publication helpers; see plugins/common/propertycommon.h.
#include "propertycommon.h"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include "gitdiscovery.h"
#include "gitrunner.h"

namespace gitinfo {

namespace {

using json = nlohmann::json;

// Path and publication helpers are shared with digital-signature-property
// through plugins/common; the per-package copies had started to drift.
using propertycommon::absolutePath;
using propertycommon::isContainedInDirectory;
using propertycommon::isExistingDirectory;
using propertycommon::stripTrailingSeparators;
using propertycommon::withJsonSuffix;
using propertycommon::writeJson;

// Output caps. The internal deadline stays well below the manifest timeout
// (30000 ms) so a normal timeout result can be published before the host
// forcibly terminates the helper and its process tree.
constexpr std::size_t kMaxStdoutBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxStderrBytes = 256 * 1024;

// Per-query caps. The queries run sequentially, so they share one budget: the
// sum of the caps must never exceed what the host permits.
constexpr DWORD kStatusQueryCapMs   = 12000;
constexpr DWORD kPlumbingQueryCapMs = 5000;
constexpr DWORD kTotalBudgetMs      = 25000;

// One budget shared by every git query of a single helper invocation.
class QueryBudget {
public:
    QueryBudget() : startTick_(GetTickCount64()) {}

    // Deadline for one query: its own cap, but never more than what is left of
    // the shared budget. A value of 1 ms fails the query immediately, which
    // keeps the caller on its timeout path.
    DWORD slice(DWORD capMs) const
    {
        const ULONGLONG elapsed = GetTickCount64() - startTick_;
        if (elapsed >= kTotalBudgetMs) {
            return 1;
        }
        const DWORD remaining = kTotalBudgetMs - static_cast<DWORD>(elapsed);
        return capMs < remaining ? capMs : remaining;
    }

private:
    ULONGLONG startTick_;
};

// Minimum Git versions for the read-only machine-readable queries.
constexpr int kMinPorcelainV2Major   = 2;
constexpr int kMinPorcelainV2Minor   = 11;
constexpr int kMinOptionalLocksMajor = 2;
constexpr int kMinOptionalLocksMinor = 15;

// ---------------------------------------------------------------------------
// Text helpers

std::string trim(const std::string &text)
{
    std::size_t begin = 0;
    std::size_t end   = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin
           && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string asciiLower(const std::string &text)
{
    std::string lowered = text;
    for (auto &c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lowered;
}

std::wstring utf8ToWide(const std::string &text)
{
    if (text.empty()) {
        return std::wstring();
    }
    const int length = static_cast<int>(text.size());
    const int size   = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), length,
                                           nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), length, &wide[0], size);
    return wide;
}

bool startsWith(const std::string &text, const char *prefix)
{
    const std::size_t length = std::strlen(prefix);
    return text.size() >= length && text.compare(0, length, prefix) == 0;
}

std::string firstLine(const std::string &text)
{
    const std::size_t newline = text.find('\n');
    return trim(newline == std::string::npos ? text : text.substr(0, newline));
}

std::string nthLine(const std::string &text, int index)
{
    std::size_t position = 0;
    int current          = 0;
    for (;;) {
        const std::size_t newline = text.find('\n', position);
        const std::string line
            = trim(newline == std::string::npos
                       ? text.substr(position)
                       : text.substr(position, newline - position));
        if (current == index) {
            return line;
        }
        if (newline == std::string::npos) {
            break;
        }
        position = newline + 1;
        ++current;
    }
    return std::string();
}

// ---------------------------------------------------------------------------
// Subprocess wrapper

struct QueryResult {
    bool started        = false;
    bool timedOut       = false;
    bool outputExceeded = false;
    int exitCode        = 0;
    std::string out;
    std::string err;
};

QueryResult runGitQuery(const std::wstring &gitExecutable,
                        const std::wstring &workingDirectory,
                        const std::vector<std::wstring> &arguments,
                        const QueryBudget &budget, DWORD capMs)
{
    GitRunResult raw
        = runGit(gitExecutable, workingDirectory, arguments, kMaxStdoutBytes,
                 kMaxStderrBytes, budget.slice(capMs));
    QueryResult result;
    result.started        = raw.started;
    result.timedOut       = raw.timedOut;
    result.outputExceeded = raw.stdoutExceeded || raw.stderrExceeded;
    result.exitCode       = raw.exitCode;
    // The captured output can reach the multi-megabyte caps; move it instead
    // of paying for a deep copy on every query.
    result.out = std::move(raw.stdoutText);
    result.err = std::move(raw.stderrText);
    return result;
}

struct GitVersion {
    bool parsed = false;
    int major   = 0;
    int minor   = 0;

    bool atLeast(int requiredMajor, int requiredMinor) const
    {
        if (!parsed) {
            return false;
        }
        if (major != requiredMajor) {
            return major > requiredMajor;
        }
        return minor >= requiredMinor;
    }
};

GitVersion parseVersion(const std::string &versionLine)
{
    GitVersion version;
    // "git version 2.45.0.windows.1" -> 2.45
    const std::size_t space = versionLine.rfind(' ');
    if (space == std::string::npos) {
        return version;
    }
    const std::string number = versionLine.substr(space + 1);
    std::size_t position     = 0;
    int parts[2]             = {0, 0};
    for (int i = 0; i < 2; ++i) {
        const std::size_t start = position;
        while (position < number.size()
               && std::isdigit(static_cast<unsigned char>(number[position]))) {
            ++position;
        }
        if (position == start) {
            return version;
        }
        parts[i] = std::atoi(number.substr(start, position - start).c_str());
        if (i == 0) {
            if (position >= number.size() || number[position] != '.') {
                return version;
            }
            ++position;
        }
    }
    version.major  = parts[0];
    version.minor  = parts[1];
    version.parsed = true;
    return version;
}

// ---------------------------------------------------------------------------
// Repository classification

struct Classification {
    // Value reported for the "Repository" row. It is a distinct state, never a
    // silent "not".
    std::string repository = "query-error";
    std::string detail;
};

Classification classifyFailure(const QueryResult &result)
{
    Classification classification;
    if (result.timedOut) {
        classification.repository = "timeout";
        classification.detail     = "the query exceeded the internal deadline; "
                                    "the child process was terminated";
        return classification;
    }
    if (result.outputExceeded) {
        classification.repository = "query-error";
        classification.detail     = "the query exceeded the output limit";
        return classification;
    }
    if (!result.started) {
        classification.repository = "query-error";
        classification.detail     = "git could not be started";
        return classification;
    }

    const std::string stderrLower = asciiLower(result.err);
    if (stderrLower.find("dubious ownership") != std::string::npos
        || stderrLower.find("unsafe repository") != std::string::npos) {
        classification.repository = "unsafe-ownership";
        classification.detail
            = "git refused the repository because of unsafe ownership; "
              "safe.directory is never modified automatically";
        return classification;
    }
    if (stderrLower.find("permission denied") != std::string::npos
        || stderrLower.find("access is denied") != std::string::npos) {
        classification.repository = "permission";
        classification.detail
            = "permission denied while reading the repository";
        return classification;
    }
    if (stderrLower.find("not a git repository") != std::string::npos
        || stderrLower.find("does not appear to be a git repository")
               != std::string::npos
        || stderrLower.find("not a repository") != std::string::npos) {
        classification.repository = "not a git repository";
        classification.detail.clear();
        return classification;
    }
    if (stderrLower.find("corrupt") != std::string::npos
        || stderrLower.find("bad object") != std::string::npos
        || stderrLower.find("bad config") != std::string::npos) {
        classification.repository = "corrupt";
        classification.detail
            = "git reports repository, index or configuration corruption";
        return classification;
    }

    classification.repository = "query-error";
    classification.detail     = firstLine(result.err);
    return classification;
}

// ---------------------------------------------------------------------------
// Status parsing

struct StatusSummary {
    bool available  = false;
    std::string headOid;    // raw object id, or "(initial)"
    std::string head;       // branch name, or "(detached)"
    std::string headState;  // attached | detached | unborn, empty when unknown
    std::string upstream;
    bool hasAheadBehind = false;
    long long ahead     = 0;
    long long behind    = 0;
    long long staged    = 0;
    long long worktree  = 0;
    long long conflicts = 0;
    long long untracked = 0;
};

// Parses the machine-readable `git status --porcelain=v2 --branch -z` output.
// Branch information is read from the leading header records only, so a file
// name can never be mistaken for a branch header. Paths stay NUL-delimited, so
// names containing spaces or newlines are never split.
//
// Rename records (`2 ...`) are followed by the original path as its own
// NUL-terminated field. That field must be consumed as data: without this, an
// original path that happens to start with 'u', '1', '2' or '?' would be
// counted as an extra entry.
StatusSummary parseStatusV2(const std::string &output)
{
    StatusSummary summary;
    summary.available = true;

    bool headersFinished  = false;
    bool skipRenameSource = false;
    std::string record;
    const auto handleRecord = [&](const std::string &value) {
        if (skipRenameSource) {
            skipRenameSource = false;
            return;
        }
        if (value.empty()) {
            return;
        }
        if (value[0] == '#' && !headersFinished) {
            if (startsWith(value, "# branch.oid ")) {
                summary.headOid = trim(value.substr(13));
            }
            else if (startsWith(value, "# branch.head ")) {
                summary.head = trim(value.substr(14));
            }
            else if (startsWith(value, "# branch.upstream ")) {
                summary.upstream = trim(value.substr(18));
            }
            else if (startsWith(value, "# branch.ab ")) {
                const std::string fields = trim(value.substr(12));
                const std::size_t space  = fields.find(' ');
                if (!fields.empty() && fields[0] == '+'
                    && space != std::string::npos) {
                    summary.ahead = std::atoll(fields.c_str() + 1);
                    if (space + 1 < fields.size() && fields[space + 1] == '-') {
                        summary.behind
                            = std::atoll(fields.c_str() + space + 2);
                        summary.hasAheadBehind = true;
                    }
                }
            }
            return;
        }
        headersFinished = true;

        if (value[0] == '?') {
            // An untracked entry, which may be a collapsed directory.
            ++summary.untracked;
            return;
        }
        if (value[0] == 'u') {
            ++summary.conflicts;
            return;
        }
        if ((value[0] == '1' || value[0] == '2') && value.size() >= 4
            && value[1] == ' ') {
            const char x = value[2];
            const char y = value[3];
            if (x == 'U' || y == 'U') {
                ++summary.conflicts;
            }
            else {
                if (x != '.') {
                    ++summary.staged;
                }
                if (y != '.') {
                    ++summary.worktree;
                }
            }
            // A rename record is followed by the original path as its own
            // NUL-terminated field (see the comment above).
            if (value[0] == '2') {
                skipRenameSource = true;
            }
        }
    };

    for (const char c : output) {
        if (c == '\0') {
            handleRecord(record);
            record.clear();
        }
        else {
            record.push_back(c);
        }
    }
    handleRecord(record);

    if (summary.headOid == "(initial)") {
        summary.headState = "unborn";
    }
    else if (summary.head == "(detached)") {
        summary.headState = "detached";
    }
    else {
        summary.headState = "attached";
    }
    return summary;
}

// ---------------------------------------------------------------------------
// Result assembly

// The subgroup value is an array of one-key fields, so the Inspector renders the
// rows in the order they are put here (an object would impose key order). Every
// row is a flat string; the host resolves typed chart fields in this array as
// well, which this package does not use.
struct GroupBuilder {
    json value = json::array();

    void put(const char *key, const std::string &text)
    {
        if (!text.empty()) {
            value.push_back(json{{key, text}});
        }
    }
};

// ": <detail>" when a query carries a diagnostic, empty otherwise.
std::string joinDetail(const std::string &detail)
{
    return detail.empty() ? std::string() : (": " + detail);
}

json buildRoot(const GroupBuilder &group)
{
    json data             = json::object();
    data["Git"]           = json{{"value", group.value}};
    json root;
    root["result_schema"] = 1;
    root["data"]          = data;
    return root;
}

// One published result for every "no usable git" outcome. Discovery and the
// version probe share it so the four early exits cannot drift apart. The
// executable path is not reported: the Inspector shows repository facts, and
// this outcome is explained by the reason row.
int publishGitNotFound(const std::wstring &outputJsonPath,
                       const std::string &reason)
{
    GroupBuilder group;
    group.put("Reason", reason);
    return writeJson(outputJsonPath, buildRoot(group)) ? 0 : 1;
}

bool isAbsolutePath(const std::wstring &path)
{
    if (path.size() >= 3 && std::iswalpha(path[0]) && path[1] == L':'
        && (path[2] == L'\\' || path[2] == L'/')) {
        return true;
    }
    return path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
}

// Git reports --git-common-dir relative to the current directory, while other
// paths come back absolute. Resolving against the inspected directory makes the
// two comparable.
std::wstring resolveAgainst(const std::wstring &baseDirectory,
                            const std::wstring &path)
{
    if (path.empty()) {
        return std::wstring();
    }
    if (isAbsolutePath(path)) {
        return absolutePath(path);
    }
    if (baseDirectory.empty()) {
        return absolutePath(path);
    }
    return absolutePath(baseDirectory + L"\\" + path);
}

std::string repositoryType(const std::wstring &inputDirectory,
                           const std::string &workTreeRootUtf8,
                           const std::string &absoluteGitDirectoryUtf8,
                           const std::string &gitCommonDirectoryUtf8,
                           bool bare)
{
    if (bare) {
        return "bare";
    }

    const std::wstring gitDirectory = stripTrailingSeparators(resolveAgainst(
        inputDirectory, utf8ToWide(absoluteGitDirectoryUtf8)));
    const std::wstring commonDirectory = stripTrailingSeparators(resolveAgainst(
        inputDirectory, utf8ToWide(gitCommonDirectoryUtf8)));

    std::string type = "worktree";
    if (!gitDirectory.empty() && !commonDirectory.empty()
        && _wcsicmp(gitDirectory.c_str(), commonDirectory.c_str()) != 0) {
        // A linked worktree keeps its per-worktree git directory inside the
        // common directory of the main repository.
        type = "linked-worktree";
    }

    const std::wstring workTreeRoot = utf8ToWide(workTreeRootUtf8);
    if (!workTreeRoot.empty() && !inputDirectory.empty()) {
        const std::wstring inputCanonical
            = stripTrailingSeparators(absolutePath(inputDirectory));
        const std::wstring rootCanonical
            = stripTrailingSeparators(absolutePath(workTreeRoot));
        if (_wcsicmp(inputCanonical.c_str(), rootCanonical.c_str()) != 0) {
            type += " subdirectory";
        }
    }
    return type;
}

}  // namespace

int runGitInfo(const std::vector<std::wstring> &arguments)
{
    std::wstring input;
    std::wstring output;
    std::wstring outputDir;
    std::wstring explicitGit;
    bool inputSeen     = false;
    bool outputSeen    = false;
    bool outputDirSeen = false;

    for (std::size_t i = 1; i < arguments.size();) {
        const auto &option = arguments[i];
        if (option == L"--git") {
            if (i + 1 >= arguments.size()) {
                return 1;
            }
            explicitGit = arguments[i + 1];
            i += 2;
            continue;
        }
        if (i + 1 >= arguments.size()) {
            return 1;
        }
        const auto &value = arguments[i + 1];
        if (value.empty()) {
            return 1;
        }
        if (option == L"--input" && !inputSeen) {
            input     = value;
            inputSeen = true;
        }
        else if (option == L"--output" && !outputSeen) {
            output     = value;
            outputSeen = true;
        }
        else if (option == L"--output-dir" && !outputDirSeen) {
            outputDir     = value;
            outputDirSeen = true;
        }
        else {
            return 1;
        }
        i += 2;
    }

    if (!inputSeen || !outputSeen || !outputDirSeen) {
        return 1;
    }
    if (!isExistingDirectory(input) || !isExistingDirectory(outputDir)) {
        return 1;
    }

    const std::wstring outputJsonPath = withJsonSuffix(output);
    if (!isContainedInDirectory(outputJsonPath, outputDir)) {
        return 1;
    }

    GroupBuilder group;

    // Non-fatal query problems accumulate: several queries can degrade the
    // result, and a later one must not silently overwrite an earlier one.
    std::string queryError;
    auto noteQueryError = [&queryError](const std::string &text) {
        if (text.empty()) {
            return;
        }
        queryError = queryError.empty() ? text : queryError + "; " + text;
    };

    // Shared deadline for every query of this invocation (see QueryBudget).
    const QueryBudget budget;

    // --- Executable discovery -------------------------------------------------
    std::wstring gitExecutable;
    if (!explicitGit.empty()) {
        if (!isUsableGitOverride(explicitGit, gitExecutable)) {
            return publishGitNotFound(
                outputJsonPath, "the --git override is not an absolute path to "
                                "an existing executable file");
        }
    }
    else {
        gitExecutable = discoverGitFromPath(input);
    }

    if (gitExecutable.empty()) {
        return publishGitNotFound(
            outputJsonPath,
            "no usable git.exe was found in the absolute, non-empty PATH "
            "entries");
    }

    // --- Version --------------------------------------------------------------
    const QueryResult versionQuery
        = runGitQuery(gitExecutable, input,
                      {L"-C", input, L"--version"}, budget,
                      kPlumbingQueryCapMs);
    if (!versionQuery.started || versionQuery.timedOut
        || versionQuery.outputExceeded || versionQuery.exitCode != 0
        || trim(versionQuery.out).empty()) {
        return publishGitNotFound(
            outputJsonPath,
            "the discovered git executable did not report a version");
    }
    const GitVersion version = parseVersion(firstLine(versionQuery.out));
    if (!version.parsed) {
        return publishGitNotFound(
            outputJsonPath, "unrecognized git version string: "
                                + firstLine(versionQuery.out));
    }

    // Read-only query flags. `--no-optional-locks` requires Git 2.15; the
    // equivalent GIT_OPTIONAL_LOCKS=0 environment variable is always set by the
    // runner, so these queries never refresh the index.
    std::vector<std::wstring> base{L"-C", input};
    if (version.atLeast(kMinOptionalLocksMajor, kMinOptionalLocksMinor)) {
        base.emplace_back(L"--no-optional-locks");
    }
    // Disable repository-configured helpers that could execute external
    // programs or change what is inspected.
    base.emplace_back(L"-c");
    base.emplace_back(L"core.fsmonitor=false");
    base.emplace_back(L"-c");
    base.emplace_back(L"diff.external=");
    base.emplace_back(L"-c");
    base.emplace_back(L"credential.helper=");

    // --- Repository probe -----------------------------------------------------
    // `rev-parse --git-dir` is the repository probe: it is pure plumbing, it
    // never writes, and it fails with a stable diagnostic outside a repository.
    std::string gitDirectory;
    {
        std::vector<std::wstring> probeArguments = base;
        probeArguments.emplace_back(L"rev-parse");
        probeArguments.emplace_back(L"--git-dir");
        const QueryResult probe
            = runGitQuery(gitExecutable, input, probeArguments, budget,
                            kPlumbingQueryCapMs);
        if (!probe.started || probe.timedOut || probe.outputExceeded
            || probe.exitCode != 0) {
            const Classification classification = classifyFailure(probe);
            group.put("Repository", classification.repository);
            group.put("Query Error", classification.detail);
            const json root = buildRoot(group);
            return writeJson(outputJsonPath, root) ? 0 : 1;
        }
        gitDirectory = firstLine(probe.out);
    }

    // A healthy repository is implied by the repository rows themselves; the
    // Repository row is reserved for the degraded and not-a-repo states.
    bool bare = false;
    std::string absoluteGitDirectory;
    std::string gitCommonDirectory;
    bool layoutKnown = false;
    {
        // Repository layout. All three options are valid in bare and non-bare
        // repositories, so a single invocation covers both.
        std::vector<std::wstring> layout = base;
        layout.emplace_back(L"rev-parse");
        layout.emplace_back(L"--is-bare-repository");
        layout.emplace_back(L"--absolute-git-dir");
        layout.emplace_back(L"--git-common-dir");
        const QueryResult layoutQuery
            = runGitQuery(gitExecutable, input, layout, budget, kPlumbingQueryCapMs);
        layoutKnown = layoutQuery.started && !layoutQuery.timedOut
                      && !layoutQuery.outputExceeded
                      && layoutQuery.exitCode == 0;
        if (layoutKnown) {
            bare                = firstLine(layoutQuery.out) == "true";
            absoluteGitDirectory = nthLine(layoutQuery.out, 1);
            gitCommonDirectory  = nthLine(layoutQuery.out, 2);
        }
        else {
            // The layout decides whether the repository is bare and which
            // directory is its root, so a failed query is a visible state.
            // Silently keeping bare=false would claim "worktree" without
            // evidence.
            const Classification classification = classifyFailure(layoutQuery);
            noteQueryError("the repository layout query failed"
                           + joinDetail(classification.detail)
                           + "; the repository type and root are not reported");
        }
    }

    StatusSummary status;
    std::string workTreeRoot;

    if (bare) {
        // Worktree statistics do not apply to a bare repository, so branch state
        // is read with plumbing commands instead of `git status`.
        std::vector<std::wstring> symbolic = base;
        symbolic.emplace_back(L"symbolic-ref");
        symbolic.emplace_back(L"-q");
        symbolic.emplace_back(L"--short");
        symbolic.emplace_back(L"HEAD");
        const QueryResult symbolicQuery
            = runGitQuery(gitExecutable, input, symbolic, budget,
                                  kPlumbingQueryCapMs);

        std::vector<std::wstring> head = base;
        head.emplace_back(L"rev-parse");
        head.emplace_back(L"--short");
        head.emplace_back(L"HEAD");
        const QueryResult headQuery
            = runGitQuery(gitExecutable, input, head, budget, kPlumbingQueryCapMs);

        const bool hasBranch = symbolicQuery.started && !symbolicQuery.timedOut
                               && symbolicQuery.exitCode == 0
                               && !trim(symbolicQuery.out).empty();
        const bool hasCommit = headQuery.started && !headQuery.timedOut
                               && headQuery.exitCode == 0
                               && !trim(headQuery.out).empty();

        if (hasCommit) {
            status.available = true;
            status.headOid   = firstLine(headQuery.out);
            status.headState = "detached";
            if (hasBranch) {
                status.head      = firstLine(symbolicQuery.out);
                status.headState = "attached";
            }
        }
        else if (hasBranch) {
            status.available = true;
            status.head      = firstLine(symbolicQuery.out);
            status.headState = "unborn";
        }
    }
    else {
        if (!version.atLeast(kMinPorcelainV2Major, kMinPorcelainV2Minor)) {
            noteQueryError("the installed Git is older than 2.11, so the "
                           "machine-readable porcelain v2 status and the "
                           "change counts are unavailable");
        }
        else {
            std::vector<std::wstring> statusArguments = base;
            statusArguments.emplace_back(L"status");
            statusArguments.emplace_back(L"--porcelain=v2");
            statusArguments.emplace_back(L"--branch");
            statusArguments.emplace_back(L"-z");
            const QueryResult statusQuery
                = runGitQuery(gitExecutable, input, statusArguments,
                                  budget, kStatusQueryCapMs);

            if (statusQuery.timedOut) {
                noteQueryError("git status exceeded the internal deadline and "
                               "the child process was terminated");
            }
            else if (statusQuery.outputExceeded) {
                noteQueryError("git status output exceeded the limit");
            }
            else if (!statusQuery.started || statusQuery.exitCode != 0) {
                const Classification classification
                    = classifyFailure(statusQuery);
                noteQueryError("git status failed"
                               + joinDetail(classification.detail));
                // Repository-state failures are surfaced as distinct states
                // instead of being hidden behind "yes".
                if (classification.repository == "unsafe-ownership"
                    || classification.repository == "permission"
                    || classification.repository == "corrupt") {
                    group.put("Repository", classification.repository);
                }
            }
            else {
                status = parseStatusV2(statusQuery.out);
            }
        }

        std::vector<std::wstring> toplevel = base;
        toplevel.emplace_back(L"rev-parse");
        toplevel.emplace_back(L"--show-toplevel");
        const QueryResult toplevelQuery
            = runGitQuery(gitExecutable, input, toplevel, budget,
                                  kPlumbingQueryCapMs);
        if (toplevelQuery.started && !toplevelQuery.timedOut
            && !toplevelQuery.outputExceeded && toplevelQuery.exitCode == 0) {
            workTreeRoot = firstLine(toplevelQuery.out);
        }
        else {
            // Without the worktree root the result loses the "Repository Root"
            // row and the "subdirectory" qualifier, so the failure is stated.
            const Classification classification = classifyFailure(toplevelQuery);
            noteQueryError("the worktree root query failed"
                           + joinDetail(classification.detail)
                           + "; the repository root is not reported");
        }
    }

    // --- Branch state ---------------------------------------------------------
    if (status.available) {
        if (status.headState == "detached") {
            group.put("HEAD State", "detached");
        }
        else if (status.headState == "unborn") {
            group.put("HEAD State", "unborn");
        }
        else {
            group.put("HEAD State", "attached");
        }

        if (status.headState != "detached" && !status.head.empty()
            && status.head != "(detached)") {
            group.put("Branch", status.head);
        }
        if (status.headState == "detached" && !status.headOid.empty()) {
            group.put("HEAD", status.headOid.substr(0, 12));
        }
        else if (status.headState == "attached" && !status.headOid.empty()
                 && status.headOid != "(initial)") {
            group.put("HEAD", status.headOid.substr(0, 12));
        }

        if (!status.upstream.empty()) {
            group.put("Upstream", status.upstream);
            if (status.hasAheadBehind) {
                group.put("Ahead", std::to_string(status.ahead));
                group.put("Behind", std::to_string(status.behind));
            }
        }
        else if (!bare) {
            group.put("Upstream", "(none)");
        }
    }

    // --- Repository layout ----------------------------------------------------
    // Without the layout query the bare/worktree distinction is unknown, so no
    // type is claimed: "worktree" would be an unsupported assertion.
    if (layoutKnown) {
        group.put("Repository Type",
                  repositoryType(input, workTreeRoot, absoluteGitDirectory,
                                 gitCommonDirectory, bare));
    }
    if (!workTreeRoot.empty()) {
        group.put("Repository Root", workTreeRoot);
    }
    else if (bare && !absoluteGitDirectory.empty()) {
        group.put("Repository Root", absoluteGitDirectory);
    }

    // --- Change counts --------------------------------------------------------
    if (bare) {
        group.put("Worktree Statistics",
                  "not applicable: worktree statistics do not apply to a bare "
                  "repository");
    }
    else if (status.available) {
        group.put("Staged Changes", std::to_string(status.staged));
        group.put("Worktree Changes", std::to_string(status.worktree));
        group.put("Conflicts", std::to_string(status.conflicts));
        group.put("Untracked Entries", std::to_string(status.untracked));
        group.put("Untracked Mode",
                  "entry count; a collapsed untracked directory counts as one "
                  "entry, not as every file below it");
    }

    group.put("Query Error", queryError);

    const json root = buildRoot(group);
    return writeJson(outputJsonPath, root) ? 0 : 1;
}

}  // namespace gitinfo
