// Helper tests: CLI parsing rules, query-function injection, the
// zero-deadline test hook and the published JSON shape. The query tests in
// pdbquery_test.cpp cover the real parsing; fake query functions are used
// here so the helper's own behavior is checked in isolation.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pdbhelper.h"

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

std::wstring g_receivedPath;
PdbQueryOptions g_receivedOptions;

PdbQueryResult sentinelQuery(const std::wstring &filePath,
                             const PdbQueryOptions &options)
{
    // The path is copied because the reference the query receives lives in
    // the caller's frame and is gone by the time the assertions run.
    g_receivedPath   = filePath;
    g_receivedOptions = options;
    PdbQueryResult result;
    result.status    = "SentinelStatus";
    result.machine   = "SentinelMachine";
    return result;
}

bool g_queryInvoked = false;

PdbQueryResult countingQuery(const std::wstring &filePath,
                             const PdbQueryOptions &options)
{
    (void)filePath;
    (void)options;
    g_queryInvoked = true;
    PdbQueryResult result;
    result.status = "Valid";
    return result;
}

std::string readAll(const std::wstring &path)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    std::ostringstream stream;
    stream << in.rdbuf();
    return stream.str();
}

bool pathExists(const std::wstring &path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
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
    return ok == TRUE && written == body.size();
}

std::vector<std::wstring> arguments(std::initializer_list<std::wstring> items)
{
    std::vector<std::wstring> result;
    result.reserve(items.size() + 1);
    result.push_back(L"pdb_property.exe");
    for (const std::wstring &item : items) {
        result.push_back(item);
    }
    return result;
}

// Merges the one-key fields of the "PDB" subgroup so assertions can look
// values up by row key, and verifies every row is flat on the way.
bool mergePdbRows(const json &document, json *merged)
{
    const auto dataIt = document.find("data");
    if (dataIt == document.end() || !dataIt->is_object()) {
        return false;
    }
    const auto pdbIt = dataIt->find("PDB");
    if (pdbIt == dataIt->end() || !pdbIt->is_object()) {
        return false;
    }
    const auto valueIt = pdbIt->find("value");
    if (valueIt == pdbIt->end() || !valueIt->is_array()) {
        return false;
    }
    *merged = json::object();
    for (const auto &item : *valueIt) {
        if (!item.is_object() || item.size() != 1) {
            return false;
        }
        for (auto field = item.begin(); field != item.end(); ++field) {
            if (!field.value().is_string()) {
                return false;
            }
            (*merged)[field.key()] = field.value();
        }
    }
    return true;
}

std::string str(const json &merged, const char *key)
{
    const auto it = merged.find(key);
    return it != merged.end() && it->is_string() ? it->get<std::string>()
                                                 : std::string();
}

std::wstring tempDirectory(const wchar_t *suffix)
{
    wchar_t buffer[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buffer);
    const std::wstring path = std::wstring(buffer) + suffix;
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

// Removes the suite scratch directory and every file in it, so a run leaves
// nothing behind in %TEMP% (the other packages' suites clean up the same way).
void removeScratchDirectory(const std::wstring &path)
{
    WIN32_FIND_DATAW entry {};
    const HANDLE handle = FindFirstFileW((path + L"\\*").c_str(), &entry);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = entry.cFileName;
            if (name != L"." && name != L"..") {
                DeleteFileW((path + L"\\" + name).c_str());
            }
        } while (FindNextFileW(handle, &entry));
        FindClose(handle);
    }
    RemoveDirectoryW(path.c_str());
}

}  // namespace

int main(int argc, char *argv[])
{
    if (argc < 2) {
        std::printf("FAIL: fixtures directory argument missing\n");
        return 1;
    }
    const std::wstring fixtures
        = std::wstring(argv[1], argv[1] + strlen(argv[1])) + L"\\";
    const std::wstring fullPdb = fixtures + L"full.pdb";
    const std::wstring fastlinkPdb = fixtures + L"fastlink.pdb";

    const std::wstring directory = tempDirectory(L"pdb_helper_7e04");
    const std::wstring otherDirectory = tempDirectory(L"pdb_helper_other");

    // Invocation errors: every malformed command line exits nonzero and
    // writes no output file. All valid-looking options share one output base
    // so the absence of a published file can be checked uniformly.
    {
        const std::wstring outputBase = directory + L"\\invocation";
        struct Case {
            std::vector<std::wstring> args;
            const char *name;
        };
        const std::vector<Case> cases{
            {arguments({}), "no options at all"},
            {arguments({L"--input", fullPdb}), "missing output options"},
            {arguments({L"--input", fullPdb, L"--output", outputBase,
                        L"--output-dir", directory, L"--unknown", L"x"}),
             "unknown option"},
            {arguments({L"--input", fullPdb, L"--input", fullPdb, L"--output",
                        outputBase, L"--output-dir", directory}),
             "repeated option"},
            {arguments({L"--input", fullPdb, L"--output", L"",
                        L"--output-dir", directory}),
             "empty option value"},
            {arguments({L"--input", fullPdb, L"--output", outputBase,
                        L"--output-dir", directory, L"--top"}),
             "trailing option without value"},
            {arguments({L"--input", fullPdb, L"--output", outputBase,
                        L"--output-dir", directory, L"--top", L"0"}),
             "top below the accepted range"},
            {arguments({L"--input", fullPdb, L"--output", outputBase,
                        L"--output-dir", directory, L"--top", L"101"}),
             "top above the accepted range"},
            {arguments({L"--input", fullPdb, L"--output", outputBase,
                        L"--output-dir", directory, L"--top", L"1x"}),
             "top with non-digit characters"},
            {arguments({L"--input", fullPdb, L"--output", outputBase,
                        L"--output-dir", directory, L"--deadline", L"-1"}),
             "deadline with a sign"},
            {arguments({L"--input", fullPdb, L"--output", outputBase,
                        L"--output-dir", otherDirectory}),
             "output base outside the output directory"},
            {arguments({L"--input", fullPdb, L"--output", outputBase,
                        L"--output-dir", directory + L"\\missing"}),
             "nonexistent output directory"},
        };
        for (const Case &item : cases) {
            const int exitCode = runPdbProperty(item.args, &sentinelQuery);
            if (exitCode != 1) {
                checkEq(std::to_string(exitCode), "1", item.name);
            }
            check(!pathExists(outputBase + L".json"),
                  "failed invocation writes no output");
        }
    }

    // A successful run with an injected query: the helper publishes the
    // injected fields and forwards the parsed options.
    {
        const std::wstring outputBase = directory + L"\\sentinel";
        const int exitCode = runPdbProperty(
            arguments({L"--input", fullPdb, L"--output", outputBase,
                       L"--output-dir", directory, L"--top", L"5",
                       L"--deadline", L"12345"}),
            &sentinelQuery);
        checkEq(std::to_string(exitCode), "0", "injected query run exits 0");

        const json document
            = json::parse(readAll(outputBase + L".json"), nullptr, false);
        check(!document.is_discarded(), "injected query output parses");
        if (!document.is_discarded()) {
            check(document.value("result_schema", 0) == 1,
                  "injected query output has result_schema 1");
            json merged;
            check(mergePdbRows(document, &merged),
                  "injected query rows are one-key string fields");
            checkEq(str(merged, "Status"), "SentinelStatus",
                    "injected query status is published");
            checkEq(str(merged, "Machine"), "SentinelMachine",
                    "injected query rows are published");
            check(g_receivedPath == fullPdb,
                  "injected query receives the input path");
            check(g_receivedOptions.wantTop && g_receivedOptions.topN == 5,
                  "injected query receives the top option");
            check(g_receivedOptions.hasDeadline
                      && g_receivedOptions.deadlineMs == 12345,
                  "injected query receives the deadline option");
        }
    }

    // The zero-deadline hook abandons the query before it runs.
    {
        g_queryInvoked = false;
        const std::wstring outputBase = directory + L"\\abandoned";
        const int exitCode = runPdbProperty(
            arguments({L"--input", fullPdb, L"--output", outputBase,
                       L"--output-dir", directory, L"--deadline", L"0"}),
            &countingQuery);
        checkEq(std::to_string(exitCode), "0", "abandoned run exits 0");
        check(!g_queryInvoked, "abandoned run never invokes the query");

        const json document
            = json::parse(readAll(outputBase + L".json"), nullptr, false);
        check(!document.is_discarded(), "abandoned output parses");
        if (!document.is_discarded()) {
            json merged;
            check(mergePdbRows(document, &merged),
                  "abandoned rows are one-key string fields");
            checkEq(str(merged, "Status"), "QueryError",
                    "abandoned run reports QueryError");
            check(str(merged, "Reason").find("abandoned") != std::string::npos,
                  "abandoned run says why");
        }
    }

    // End-to-end against the real fixtures through the helper entry point.
    {
        const std::wstring outputBase = directory + L"\\full";
        const int exitCode = runPdbProperty(
            arguments({L"--input", fullPdb, L"--output", outputBase,
                       L"--output-dir", directory}),
            nullptr);
        checkEq(std::to_string(exitCode), "0", "full.pdb helper run exits 0");

        const json document
            = json::parse(readAll(outputBase + L".json"), nullptr, false);
        check(!document.is_discarded(), "full.pdb output parses");
        if (!document.is_discarded()) {
            json merged;
            check(mergePdbRows(document, &merged),
                  "full.pdb rows are one-key string fields");
            checkEq(str(merged, "Status"), "Valid", "full.pdb status");
            checkEq(str(merged, "Machine"), "x64", "full.pdb machine");
            checkEq(str(merged, "Streams"), "18", "full.pdb stream count");
            checkEq(str(merged, "File Size"), "100.0 KB",
                    "full.pdb file size row");
            check(merged.find("Largest Object 1") == merged.end(),
                  "default run has no contribution rows");
        }
    }

    // The opt-in top-N rows appear with the documented keys.
    {
        const std::wstring outputBase = directory + L"\\top";
        const int exitCode = runPdbProperty(
            arguments({L"--input", fullPdb, L"--output", outputBase,
                       L"--output-dir", directory, L"--top", L"1"}),
            nullptr);
        checkEq(std::to_string(exitCode), "0", "top-N helper run exits 0");

        const json document
            = json::parse(readAll(outputBase + L".json"), nullptr, false);
        if (!document.is_discarded()) {
            json merged;
            check(mergePdbRows(document, &merged),
                  "top-N rows are one-key string fields");
            check(merged.find("Largest Object 1") != merged.end(),
                  "top-N run publishes Largest Object 1");
            check(merged.find("Largest Object 2") == merged.end(),
                  "top-N run respects the requested row count");
        }
        else {
            check(false, "top-N output parses");
        }
    }

    // The FASTLINK fixture keeps exit 0 and the informative classification.
    {
        const std::wstring outputBase = directory + L"\\fastlink";
        const int exitCode = runPdbProperty(
            arguments({L"--input", fastlinkPdb, L"--output", outputBase,
                       L"--output-dir", directory}),
            nullptr);
        checkEq(std::to_string(exitCode), "0", "fastlink helper run exits 0");

        const json document
            = json::parse(readAll(outputBase + L".json"), nullptr, false);
        if (!document.is_discarded()) {
            json merged;
            check(mergePdbRows(document, &merged),
                  "fastlink rows are one-key string fields");
            checkEq(str(merged, "Status"), "FastLink", "fastlink status");
            check(!str(merged, "Note").empty(), "fastlink carries a note");
        }
        else {
            check(false, "fastlink output parses");
        }
    }

    // A garbage file is an informative domain outcome, not a plugin failure.
    {
        const std::wstring garbage = directory + L"\\garbage.pdb";
        check(writeFile(garbage, "not a program database"),
              "garbage sample written");
        const std::wstring outputBase = directory + L"\\garbage-out";
        const int exitCode = runPdbProperty(
            arguments({L"--input", garbage, L"--output", outputBase,
                       L"--output-dir", directory}),
            nullptr);
        checkEq(std::to_string(exitCode), "0", "garbage input exits 0");

        const json document
            = json::parse(readAll(outputBase + L".json"), nullptr, false);
        if (!document.is_discarded()) {
            json merged;
            check(mergePdbRows(document, &merged),
                  "garbage rows are one-key string fields");
            checkEq(str(merged, "Status"), "NotAPdb", "garbage status");
            check(!str(merged, "Reason").empty(), "garbage carries a reason");
        }
        else {
            check(false, "garbage output parses");
        }
    }

    // Nothing stays behind in %TEMP% once the suite is done.
    removeScratchDirectory(directory);
    removeScratchDirectory(otherDirectory);

    if (failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    return 1;
}
