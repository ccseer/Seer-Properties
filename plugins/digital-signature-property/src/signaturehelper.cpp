#include "signaturehelper.h"

#include "propertycommon.h"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <thread>

namespace {

using json = nlohmann::json;

// Ordered rows for the "Digital Signature" subgroup. Values are flat strings:
// the reference host parses a subgroup value object as a list of flat
// string/scalar rows and drops anything else.
json buildSignatureGroup(const SignatureQueryResult &result)
{
    // The subgroup value is an array of one-key fields, so the Inspector renders
    // the rows in the order they are put here (an object would impose key
    // order). Every row is a flat string.
    json value = json::array();

    auto put = [&value](const char *key, const std::string &text) {
        if (!text.empty()) {
            value.push_back(json{{key, text}});
        }
    };

    put("Status", result.status);
    put("Verification Scope", result.scope);
    put("Signature Source", result.source);
    put("Signature Type", result.signatureKind);
    put("Signers Examined", result.signersExamined);
    put("Catalog Signature", result.catalogStatus);
    put("Publisher", result.publisher);
    put("Issuer", result.issuer);
    if (!result.thumbprint.empty()) {
        const std::string combined
            = result.thumbprintAlgorithm.empty()
                  ? result.thumbprint
                  : result.thumbprintAlgorithm + " " + result.thumbprint;
        put("Certificate Fingerprint", combined);
    }
    put("Signing Digest Algorithm", result.signingDigestAlgorithm);
    if (!result.validFrom.empty() && !result.validTo.empty()) {
        put("Certificate Validity", result.validFrom + " to " + result.validTo);
    }
    if (result.hasTimestamp) {
        put("Timestamp", result.timestampStatus.empty()
                             ? std::string("present")
                             : result.timestampStatus);
    }
    else {
        put("Timestamp", "none found on the examined signer");
    }
    put("Reason", result.reason);

    return json{{"value", value}};
}

// The deadline convention is shared with image-histogram-property: 0 means the
// budget is already exhausted (a test hook), and no value disables it.
SignatureQueryResult abandonedQuery(DWORD deadlineMs)
{
    SignatureQueryResult result;
    result.status = "QueryError";
    result.reason = "the signature query exceeded the internal deadline ("
                    + std::to_string(deadlineMs)
                    + " ms) and was abandoned; the file is not reported as "
                      "unsigned";
    return result;
}

// WinVerifyTrust and the catalog hash cannot be interrupted, so the query runs
// on a worker thread and the caller waits with a deadline. An abandoned worker
// is left running detached on purpose: publishing a result before the host
// kills the process tree matters more than reclaiming the thread, and the
// event handle is kept alive by the worker itself.
SignatureQueryResult runQueryWithDeadline(const std::wstring &inputPath,
                                          SignatureQueryFn queryFn,
                                          DWORD deadlineMs)
{
    if (!queryFn) {
        queryFn = &queryFileSignature;
    }
    if (deadlineMs == 0) {
        return abandonedQuery(deadlineMs);
    }

    auto rawHandle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (rawHandle == nullptr) {
        return queryFn(inputPath);
    }
    const std::shared_ptr<void> finished(rawHandle, [](void *handle) {
        CloseHandle(handle);
    });

    auto slot = std::make_shared<SignatureQueryResult>();
    std::thread worker([slot, inputPath, queryFn, finished]() {
        *slot = queryFn(inputPath);
        SetEvent(finished.get());
    });
    worker.detach();

    if (WaitForSingleObject(finished.get(), deadlineMs) == WAIT_OBJECT_0) {
        return *slot;
    }
    return abandonedQuery(deadlineMs);
}

}  // namespace

int executeDigitalSignature(const std::wstring &inputPath,
                            const std::wstring &outputBasePath,
                            const std::wstring &outputDirPath,
                            SignatureQueryFn queryFn,
                            unsigned queryDeadlineMs)
{
    if (!propertycommon::isExistingDirectory(outputDirPath)) {
        return 1;
    }

    const std::wstring outputJsonPath
        = propertycommon::withJsonSuffix(outputBasePath);
    if (!propertycommon::isContainedInDirectory(outputJsonPath, outputDirPath)) {
        return 1;
    }

    // A missing or inaccessible input is a domain outcome, not an invocation
    // error: the query classifies it as ReadError and a valid result is
    // published, so the Inspector shows an explanation instead of nothing.
    // The injected query boundary keeps status/output tests deterministic.
    const SignatureQueryResult result
        = runQueryWithDeadline(inputPath, queryFn,
                               static_cast<DWORD>(queryDeadlineMs));

    json data                 = json::object();
    data["Digital Signature"] = buildSignatureGroup(result);

    json root;
    root["result_schema"] = 1;
    root["data"]          = data;

    return propertycommon::writeJson(outputJsonPath, root) ? 0 : 1;
}

int runDigitalSignature(const std::vector<std::wstring> &arguments,
                        SignatureQueryFn queryFn)
{
    std::wstring input;
    std::wstring output;
    std::wstring outputDir;
    bool inputSeen     = false;
    bool outputSeen    = false;
    bool outputDirSeen = false;

    // Every option consumes the next token, so a trailing option without a
    // value is an invocation error instead of being ignored silently.
    for (std::size_t i = 1; i < arguments.size();) {
        if (i + 1 >= arguments.size()) {
            return 1;
        }
        const auto &option = arguments[i];
        const auto &value  = arguments[i + 1];
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

    return executeDigitalSignature(input, output, outputDir, queryFn);
}
