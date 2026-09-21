#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "signaturequery.h"

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

void checkEq(const std::string &got, const std::string &expected,
             const char *message)
{
    if (got != expected) {
        std::printf("FAIL: %s (got '%s', expected '%s')\n", message,
                    got.c_str(), expected.c_str());
        ++failures;
    }
}

void skip(const char *message)
{
    std::printf("SKIP: %s\n", message);
    ++skipped;
}

bool fileExists(const std::wstring &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring tempDirectory()
{
    wchar_t buffer[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buffer);
    const std::wstring path = std::wstring(buffer) + L"dsig_query_9b21";
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
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

bool appendBytes(const std::wstring &path, const std::string &body)
{
    const HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                    FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
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

// Flips one byte inside the Authenticode hashed range without touching the
// headers Windows parses. The signature stays locatable, so the digest mismatch
// is what the trust policy reports.
bool flipByteInBody(const std::wstring &path, LONGLONG offset)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER position{};
    position.QuadPart = offset;
    BYTE value = 0;
    DWORD transferred = 0;
    bool ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
              && ReadFile(file, &value, 1, &transferred, nullptr)
              && transferred == 1;
    if (ok) {
        value = static_cast<BYTE>(value ^ 0xFF);
        ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
             && WriteFile(file, &value, 1, &transferred, nullptr)
             && transferred == 1;
    }
    CloseHandle(file);
    return ok;
}

bool copyFile(const std::wstring &from, const std::wstring &to)
{
    return CopyFileW(from.c_str(), to.c_str(), FALSE) == TRUE;
}

bool isKnownStatus(const std::string &status)
{
    static const char *kKnown[] = {
        "Valid",           "Unsigned",      "HashMismatch",
        "UntrustedChain",  "Revoked",       "RevocationUnavailable",
        "UnsupportedOrMalformed", "ReadError", "QueryError",
    };
    for (const auto *known : kKnown) {
        if (status == known) {
            return true;
        }
    }
    return false;
}

void removeFile(const std::wstring &path)
{
    DeleteFileW(path.c_str());
}
}  // namespace

int main()
{
    // 1. Empty path is a query error.
    {
        const auto result = queryFileSignature(L"");
        checkEq(result.status, "QueryError", "empty path -> QueryError");
    }

    // 2. Non-existent path is a read error.
    {
        const auto result =
            queryFileSignature(L"Z:\\definitely\\missing\\file_9d8f.dll");
        checkEq(result.status, "ReadError", "missing file -> ReadError");
    }

    // 3. A directory is rejected as a read error.
    {
        const auto result = queryFileSignature(L"C:\\Windows");
        checkEq(result.status, "ReadError", "directory -> ReadError");
    }

    const std::wstring temp = tempDirectory();
    check(!temp.empty(), "temp directory is available");

    // 4. Malformed input: a .dll that is not a PE image must not be reported as
    //    "unsigned", because Authenticode cannot apply to it at all.
    {
        const std::wstring path = temp + L"\\malformed.dll";
        check(writeFile(path, "this is not a portable executable"),
              "malformed fixture written");
        const auto result = queryFileSignature(path);
        checkEq(result.status, "UnsupportedOrMalformed",
                "non-PE input -> UnsupportedOrMalformed");
        checkEq(result.source, "none", "non-PE input has no signature source");
        check(!result.reason.empty(), "non-PE input has a reason");
        removeFile(path);
    }

    // 5. Unsigned but well-formed: this test executable.
    {
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const auto result = queryFileSignature(self);
        check(!result.status.empty(), "self query has a status");
        check(isKnownStatus(result.status), "self query status is a known state");
        check(!result.scope.empty(), "self query describes the verification scope");
        check(!result.source.empty(), "self query has a source");
        check(!result.signersExamined.empty(),
              "self query states what was examined");
        check(result.scope.find("cache-only") != std::string::npos,
              "scope states the offline/cache-only limitation");
        if (result.source == "none") {
            checkEq(result.status, "Unsigned",
                    "no signature source implies the Unsigned status");
        }
    }

    // 6. A real, catalog-and-embedded signed system binary.
    const std::wstring signedBinary = L"C:\\Windows\\System32\\kernel32.dll";
    if (!fileExists(signedBinary)) {
        skip("C:\\Windows\\System32\\kernel32.dll is unavailable");
    }
    else {
        const auto result = queryFileSignature(signedBinary);
        check(isKnownStatus(result.status), "system binary status is known");
        checkEq(result.status, "Valid", "system binary verifies as Valid");
        check(result.source == "embedded" || result.source == "catalog",
              "system binary reports a real signature source");
        check(result.scope.find("GENERIC_VERIFY_V2") != std::string::npos,
              "scope names the verification policy");
        if (result.source == "embedded") {
            check(!result.publisher.empty(),
                  "embedded signature reports a publisher");
            check(!result.thumbprint.empty(),
                  "embedded signature reports a fingerprint");
            check(!result.thumbprintAlgorithm.empty(),
                  "fingerprint algorithm is identified");
            check(!result.signingDigestAlgorithm.empty(),
                  "signing digest algorithm is identified");
            check(!result.validFrom.empty() && !result.validTo.empty(),
                  "certificate validity interval is reported");
            check(!result.signersExamined.empty(),
                  "signer scope is reported");
            if (result.hasTimestamp) {
                check(result.timestampStatus.find("not verified")
                          != std::string::npos,
                      "timestamp is not claimed as verified");
            }
        }

        // 7. Modified copy: the byte flip stays inside the Authenticode hashed
        //    range, so the digest mismatch must be reported while certificate
        //    metadata is still available.
        const std::wstring modified = temp + L"\\modified.dll";
        if (copyFile(signedBinary, modified) && flipByteInBody(modified, 0x40)) {
            const auto modifiedResult = queryFileSignature(modified);
            check(modifiedResult.status != "Valid",
                  "modified copy is not reported as Valid");
            checkEq(modifiedResult.status, "HashMismatch",
                    "modified copy reports a hash mismatch");
            check(!modifiedResult.reason.empty(),
                  "modified copy has a reason");
            check(!modifiedResult.publisher.empty(),
                  "publisher metadata survives a failing verdict");
            removeFile(modified);
        }
        else {
            skip("could not create the modified-copy fixture");
        }

        // 7b. Signature that no longer terminates the file: Windows cannot
        //     locate the embedded signature any more. This must be reported as
        //     a malformed/unsupported signature, not as "unsigned".
        const std::wstring appended = temp + L"\\appended.dll";
        if (copyFile(signedBinary, appended)
            && appendBytes(appended, "\r\nappended by the test\r\n")) {
            const auto appendedResult = queryFileSignature(appended);
            checkEq(appendedResult.status, "UnsupportedOrMalformed",
                    "appended copy reports an unusable embedded signature");
            checkEq(appendedResult.source, "embedded",
                    "appended copy still reports the embedded source");
            removeFile(appended);
        }
        else {
            skip("could not create the appended-copy fixture");
        }
    }

    // 8. Catalog-signed coverage: system binaries without an embedded signature
    //    exercise the catalog path on real data.
    {
        static const wchar_t *kCandidates[] = {
            L"C:\\Windows\\System32\\winhttp.dll",
            L"C:\\Windows\\System32\\d3d11.dll",
            L"C:\\Windows\\System32\\dxgi.dll",
            L"C:\\Windows\\System32\\uxtheme.dll",
            L"C:\\Windows\\System32\\bcrypt.dll",
            L"C:\\Windows\\System32\\msvcrt.dll",
            L"C:\\Windows\\System32\\winmm.dll",
        };
        bool foundCatalog = false;
        for (const auto *candidate : kCandidates) {
            if (!fileExists(candidate)) {
                continue;
            }
            const auto result = queryFileSignature(candidate);
            if (result.source == "catalog") {
                foundCatalog = true;
                checkEq(result.status, "Valid",
                        "catalog-signed binary verifies as Valid");
                checkEq(result.signatureKind, "local system catalog member",
                        "catalog result identifies the signature source");
                check(result.catalogStatus.find("present") != std::string::npos,
                      "catalog lookup result is reported");
                break;
            }
        }
        if (!foundCatalog) {
            skip("no catalog-only signed binary found in this environment");
        }
    }

    RemoveDirectoryW(temp.c_str());

    if (failures == 0) {
        std::printf("PASS (%d skipped)\n", skipped);
        return 0;
    }
    return 1;
}
