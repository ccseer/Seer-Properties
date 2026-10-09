// Query tests against the committed fixtures in test/fixtures (see
// fixtures/README.md for how they were produced and what they contain).
// The expected values below are the probe-verified contents of full.pdb;
// fastlink.pdb is only asserted on its classification because it is a
// byte-patched variant of the same file.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pdbquery.h"

namespace {

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

bool copyFile(const std::wstring &from, const std::wstring &to)
{
    return CopyFileW(from.c_str(), to.c_str(), FALSE) == TRUE;
}

// Cuts the file down so it is smaller than its own MSF header claims,
// which must be reported as a corrupt PDB rather than a read failure.
bool truncateFile(const std::wstring &path, uint64_t newSize)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER position {};
    position.QuadPart = static_cast<LONGLONG>(newSize);
    const BOOL ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
                    && SetEndOfFile(file);
    CloseHandle(file);
    return ok == TRUE;
}

std::wstring tempDirectory()
{
    wchar_t buffer[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buffer);
    const std::wstring path = std::wstring(buffer) + L"pdb_query_3d17";
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
    const std::wstring fixtures = std::wstring(argv[1], argv[1] + strlen(argv[1]))
                                  + L"\\";
    const std::wstring fullPdb = fixtures + L"full.pdb";
    const std::wstring fastlinkPdb = fixtures + L"fastlink.pdb";

    // The primary fixture: a full, valid PDB with probed contents.
    {
        const PdbQueryResult result = queryPdbFile(fullPdb, {});
        checkEq(result.status, "Valid", "full.pdb is valid");
        checkEq(result.linkType, "Full", "full.pdb is a full PDB");
        check(result.note.empty(), "full.pdb carries no note");
        checkEq(result.fileSize, "100.0 KB", "full.pdb file size");
        checkEq(result.formatVersion, "VC70", "full.pdb format version");
        checkEq(result.age, "1", "full.pdb age");
        check(result.guid.size() == 38 && result.guid.front() == '{'
                  && result.guid.back() == '}',
              "full.pdb GUID has the braces form");
        checkEq(result.machine, "x64", "full.pdb machine");
        checkEq(result.streamCount, "18", "full.pdb stream count");
        checkEq(result.modules, "2", "full.pdb module count");
        checkEq(result.publicSymbols, "1", "full.pdb public symbol count");
        checkEq(result.globalSymbols, "1", "full.pdb global symbol count");
        checkEq(result.contributions, "6", "full.pdb contribution count");
        checkEq(result.typeRecords, "2", "full.pdb TPI record count");
        checkEq(result.inlineRecords, "10", "full.pdb IPI record count");
        check(!result.built.empty(), "full.pdb build timestamp present");
        check(result.lineInfo == "Yes" || result.lineInfo == "No",
              "full.pdb line info is a yes/no answer");
    }

    // Opt-in top-N analysis: two modules hold contributions, so N=2 yields
    // two deterministic rows.
    {
        PdbQueryOptions options;
        options.wantTop = true;
        options.topN = 2;
        const PdbQueryResult result = queryPdbFile(fullPdb, options);
        checkEq(result.status, "Valid", "top-N query stays valid");
        if (result.topContributions.size() != 2) {
            check(false, "top-N query reports two rows");
        }
        else {
            for (std::size_t i = 0; i < 2; ++i) {
                const PdbContribution &row = result.topContributions[i];
                check(!row.name.empty(), "top-N row has a module name");
                check(!row.sizeText.empty(), "top-N row has a size");
            }
        }
    }

    // Without the opt-in the contribution rows stay out of the output.
    {
        const PdbQueryResult result = queryPdbFile(fullPdb, {});
        check(result.topContributions.empty(),
              "default query omits contribution rows");
    }

    // The FASTLINK fixture: only the classification is asserted because the
    // fixture is a byte-patched variant of full.pdb.
    {
        const PdbQueryResult result = queryPdbFile(fastlinkPdb, {});
        checkEq(result.status, "FastLink", "fastlink.pdb is classified FastLink");
        checkEq(result.linkType, "/DEBUG:FASTLINK",
                "fastlink.pdb link type");
        check(!result.note.empty(),
                "fastlink.pdb explains the fast-link limitation");
        check(!result.streamCount.empty(),
              "fastlink.pdb still exposes the stream count");
    }

    // A missing file is a read error with a plain-language reason.
    {
        const PdbQueryResult result
            = queryPdbFile(L"C:\\no\\such\\file\\missing.pdb", {});
        checkEq(result.status, "ReadError", "missing file is a read error");
        checkEq(result.reason, "the file does not exist",
                "missing file reason");
    }

    // An empty file is not a PDB file.
    {
        const std::wstring directory = tempDirectory();
        const std::wstring emptyFile = directory + L"\\empty.pdb";
        check(writeFile(emptyFile, ""), "empty sample written");
        const PdbQueryResult result = queryPdbFile(emptyFile, {});
        checkEq(result.status, "NotAPdb", "empty file is not a PDB");
        checkEq(result.reason, "the file is empty", "empty file reason");
        checkEq(result.fileSize, "0 B", "empty file size");
        DeleteFileW(emptyFile.c_str());
    }

    // A file without the PDB magic is not a PDB, not a corrupt PDB.
    {
        const std::wstring directory = tempDirectory();
        const std::wstring garbage = directory + L"\\garbage.pdb";
        check(writeFile(garbage, "not a program database at all"),
              "garbage sample written");
        const PdbQueryResult result = queryPdbFile(garbage, {});
        checkEq(result.status, "NotAPdb", "garbage file is not a PDB");
        check(result.reason.find("PDB magic") != std::string::npos,
              "garbage file reason names the PDB magic");
        DeleteFileW(garbage.c_str());
    }

    // A truncated copy of the fixture is a corrupt PDB.
    {
        const std::wstring directory = tempDirectory();
        const std::wstring truncated = directory + L"\\truncated.pdb";
        check(copyFile(fullPdb, truncated), "truncation source copied");
        check(truncateFile(truncated, 101400), "truncation applied");
        const PdbQueryResult result = queryPdbFile(truncated, {});
        checkEq(result.status, "CorruptPdb", "truncated file is corrupt");
        check(result.reason.find("truncated") != std::string::npos,
              "truncated file reason mentions truncation");
        DeleteFileW(truncated.c_str());
    }

    // A zero budget is already exhausted: the query reports the abandonment
    // instead of running.
    {
        PdbQueryOptions options;
        options.hasDeadline = true;
        options.deadlineMs = 0;
        const PdbQueryResult result = queryPdbFile(fullPdb, options);
        checkEq(result.status, "QueryError", "zero budget abandons the query");
        check(result.reason.find("deadline") != std::string::npos,
              "zero budget reason mentions the deadline");
    }

    // Nothing stays behind in %TEMP% once the suite is done.
    removeScratchDirectory(tempDirectory());

    if (failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    return 1;
}
