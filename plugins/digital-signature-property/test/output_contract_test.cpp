#include <windows.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "signaturehelper.h"

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

// The subgroup value is an array of one-key fields in the plugin's own order;
// merge them into an object so the key-based assertions keep working.
json signatureGroup(const json &document)
{
    json merged = json::object();
    const json *data = member(document, "data");
    const json *value = data ? member(*data, "Digital Signature") : nullptr;
    const json *array = value ? member(*value, "value") : nullptr;
    if (array == nullptr || !array->is_array()) {
        return merged;
    }
    for (const auto &item : *array) {
        if (!item.is_object()) {
            continue;
        }
        for (auto field = item.begin(); field != item.end(); ++field) {
            merged[field.key()] = field.value();
        }
    }
    return merged;
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

// Deterministic query boundary: the helper never touches the real file.
SignatureQueryResult fakeResult(const std::wstring &path)
{
    SignatureQueryResult result;
    result.status                 = "Unsigned";
    result.source                 = "none";
    result.scope                  = "mocked policy";
    result.reason                 = "no embedded signature";
    result.catalogStatus          = "absent";
    result.signersExamined        = "none";
    result.publisher              = "Mock Publisher";
    result.issuer                 = "Mock Issuer";
    result.thumbprintAlgorithm    = "SHA-256";
    result.thumbprint             = "ABCDEF";
    result.signingDigestAlgorithm = "sha256RSA";
    result.validFrom              = "2020-01-01 00:00:00";
    result.validTo                = "2030-01-01 00:00:00";
    (void)path;
    return result;
}

// Fields containing quotes, backslashes, tabs and non-ASCII text must survive a
// write/parse round trip; the parser would fail on a broken serializer.
SignatureQueryResult escapingResult(const std::wstring &path)
{
    SignatureQueryResult result;
    result.status    = "UntrustedChain";
    result.source    = "embedded";
    result.scope     = "policy with \"quotes\" and \\backslash\\ and \xC3\xA4";
    result.reason    = "reason\twith\ttabs and \"quotes\"";
    result.publisher = "Publisher \"quoted\" \\ name";
    (void)path;
    return result;
}

// A query that outlives the injected deadline: the helper must abandon it and
// still publish a result instead of waiting for the host to kill the process.
SignatureQueryResult slowResult(const std::wstring &path)
{
    Sleep(5000);
    return fakeResult(path);
}

std::wstring fileIn(const std::wstring &directory, const wchar_t *name)
{
    return directory + L"\\" + name;
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

std::string readAll(const std::wstring &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream stream;
    stream << in.rdbuf();
    return stream.str();
}

bool parseOutput(const std::wstring &path, json *document)
{
    *document = json::parse(readAll(path), nullptr, false);
    if (document->is_discarded()) {
        std::printf("FAIL: output is not valid JSON\n");
        ++failures;
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    wchar_t tempPath[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tempPath) == 0) {
        return 1;
    }
    const std::wstring base = fileIn(std::wstring(tempPath),
                                     L"dsig_out_test_3c91");
    CreateDirectoryW(base.c_str(), nullptr);

    const std::wstring input = fileIn(base, L"input.dll");
    if (!writeFile(input, "not really a dll, but a readable file")) {
        std::printf("FAIL: could not create the input fixture\n");
        return 1;
    }

    // 1. A namespaced "Digital Signature" subgroup with flat rows.
    {
        const std::wstring outputBase = fileIn(base, L"result");
        check(executeDigitalSignature(input, outputBase, base, &fakeResult) == 0,
              "execute returns 0 with valid JSON");

        json document;
        if (parseOutput(outputBase + L".json", &document)) {
            check(document.value("result_schema", 0) == 1,
                  "output has result_schema 1");
            const json group = signatureGroup(document);
            check(!group.empty(), "output has the Digital Signature subgroup");
            if (!group.empty()) {
                check(everyRowIsFlat(group),
                      "every Digital Signature row is flat");
                checkEq(str(group, "Status"), "Unsigned",
                        "mocked status is visible");
                checkEq(str(group, "Publisher"), "Mock Publisher",
                        "publisher metadata is published");
                checkEq(str(group, "Signing Digest Algorithm"), "sha256RSA",
                        "signing digest algorithm is published");
                checkEq(str(group, "Certificate Fingerprint"),
                        "SHA-256 ABCDEF",
                        "fingerprint identifies its hash algorithm");
                checkEq(str(group, "Verification Scope"), "mocked policy",
                        "verification scope is published");
                checkEq(str(group, "Signature Source"), "none",
                        "signature source is published");
                checkEq(str(group, "Catalog Signature"), "absent",
                        "catalog lookup outcome is published");
                check(str(group, "Timestamp").find("none found")
                          != std::string::npos,
                      "absent timestamp is stated explicitly");
                check(str(group, "Certificate Validity").find("2020-01-01")
                          != std::string::npos,
                      "certificate validity is published");
                check(member(group, "Repository") == nullptr,
                      "no unrelated Repository key (no git collision)");
            }
            const json *data = member(document, "data");
            check(data != nullptr && member(*data, "Status") == nullptr,
                  "rows stay inside the subgroup instead of leaking to data");
        }
    }

    // 2. Escaping: quotes, backslashes, tabs and non-ASCII text.
    {
        const std::wstring outputBase = fileIn(base, L"escape");
        check(executeDigitalSignature(input, outputBase, base, &escapingResult)
                  == 0,
              "escape: execute returns 0");
        json document;
        if (parseOutput(outputBase + L".json", &document)) {
            const json group = signatureGroup(document);
            check(!group.empty(), "escape: subgroup is present");
            if (!group.empty()) {
                checkEq(str(group, "Publisher"),
                        "Publisher \"quoted\" \\ name",
                        "escape: quotes and backslashes round-trip");
                checkEq(str(group, "Verification Scope"),
                        "policy with \"quotes\" and \\backslash\\ and \xC3\xA4",
                        "escape: non-ASCII text round-trips");
                check(str(group, "Reason").find('\t') != std::string::npos,
                      "escape: tab control characters round-trip");
            }
        }
    }

    // 3. Output must stay inside the request directory.
    {
        const std::wstring outsideBase = base + L"\\..\\escape_out";
        check(executeDigitalSignature(input, outsideBase, base, &fakeResult) == 1,
              "traversal output is rejected");
        check(GetFileAttributesW((base + L"\\..\\escape_out.json").c_str())
                  == INVALID_FILE_ATTRIBUTES,
              "no file is written outside the request directory");
    }

    // 4. A missing input is a domain outcome: the real query classifies it as
    //    ReadError and a valid result is published with exit 0, so the
    //    Inspector shows an explanation instead of nothing.
    {
        const std::wstring outputBase = fileIn(base, L"missing_input");
        check(executeDigitalSignature(fileIn(base, L"missing_input.dll"),
                                      outputBase, base, nullptr)
                  == 0,
              "missing input still publishes a result");
        json document;
        if (parseOutput(outputBase + L".json", &document)) {
            const json group = signatureGroup(document);
            check(!group.empty(), "missing input: subgroup is present");
            if (!group.empty()) {
                checkEq(str(group, "Status"), "ReadError",
                        "missing input reports ReadError");
                check(str(group, "Reason").find("exist") != std::string::npos
                          || str(group, "Reason").find("open") != std::string::npos,
                      "missing input explains why");
                check(everyRowIsFlat(group),
                      "missing input rows are flat");
            }
        }
    }

    // 4b. Infrastructure failures still exit nonzero: no request directory.
    {
        check(executeDigitalSignature(input, fileIn(base, L"nodir"),
                                      fileIn(base, L"does_not_exist"),
                                      &fakeResult)
                  == 1,
              "missing request directory returns an error");
    }

    // 5. CLI parsing.
    {
        const std::wstring outputBase = fileIn(base, L"cli");
        check(runDigitalSignature({L"digital_signature.exe", L"--input", input,
                                   L"--output", outputBase, L"--output-dir",
                                   base},
                                  &fakeResult)
                  == 0,
              "cli with all options returns 0");
        check(GetFileAttributesW((outputBase + L".json").c_str())
                  != INVALID_FILE_ATTRIBUTES,
              "cli writes the output json");
        check(runDigitalSignature({L"digital_signature.exe", L"--input", input},
                                  &fakeResult)
                  == 1,
              "cli missing required options is rejected");
        check(runDigitalSignature({L"digital_signature.exe", L"--input", input,
                                   L"--output", outputBase, L"--bogus", L"x"},
                                  &fakeResult)
                  == 1,
              "cli unknown option is rejected");
        check(runDigitalSignature({L"digital_signature.exe", L"--input", input,
                                   L"--output", outputBase, L"--output-dir",
                                   base, L"--input", input},
                                  &fakeResult)
                  == 1,
              "cli duplicate option is rejected");
        check(runDigitalSignature({L"digital_signature.exe", L"--input", input,
                                   L"--output", outputBase, L"--output-dir",
                                   base, L"--bogus"},
                                  &fakeResult)
                  == 1,
              "cli trailing option without a value is rejected");
    }

    // 6. Internal deadline: an abandoned query publishes a QueryError result
    //    and never claims the file is unsigned.
    {
        const std::wstring outputBase = fileIn(base, L"deadline");
        check(executeDigitalSignature(input, outputBase, base, &slowResult, 100)
                  == 0,
              "an abandoned query still publishes a result with exit 0");
        json document;
        if (parseOutput(outputBase + L".json", &document)) {
            const json group = signatureGroup(document);
            check(!group.empty(), "deadline: subgroup is present");
            if (!group.empty()) {
                checkEq(str(group, "Status"), "QueryError",
                        "deadline: abandoned query reports QueryError");
                check(str(group, "Reason").find("deadline")
                          != std::string::npos,
                      "deadline: the reason names the internal deadline");
                check(everyRowIsFlat(group), "deadline: rows are flat");
            }
        }

        // 0 is the shared "budget already exhausted" test hook, the same
        // convention image-histogram-property uses: the query is abandoned at
        // the first checkpoint instead of running without a deadline.
        const std::wstring zeroBase = fileIn(base, L"deadline-zero");
        check(executeDigitalSignature(input, zeroBase, base, &fakeResult, 0)
                  == 0,
              "a zero deadline still publishes a result with exit 0");
        json zeroDocument;
        if (parseOutput(zeroBase + L".json", &zeroDocument)) {
            const json group = signatureGroup(zeroDocument);
            check(!group.empty(), "zero deadline: subgroup is present");
            if (!group.empty()) {
                checkEq(str(group, "Status"), "QueryError",
                        "zero deadline: the query is abandoned immediately");
            }
        }
    }

    // Cleanup.
    {
        WIN32_FIND_DATAW data;
        const HANDLE handle = FindFirstFileW((base + L"\\*").c_str(), &data);
        if (handle != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = data.cFileName;
                if (name != L"." && name != L"..") {
                    DeleteFileW((base + L"\\" + name).c_str());
                }
            } while (FindNextFileW(handle, &data));
            FindClose(handle);
        }
        RemoveDirectoryW(base.c_str());
    }

    if (failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    return 1;
}
