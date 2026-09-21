#pragma once

#include <string>
#include <vector>

#include "signaturequery.h"

// Optional injectable query boundary for deterministic tests. Defaults to the
// real Windows implementation. Accepts an absolute input path; returns a result.
using SignatureQueryFn = SignatureQueryResult (*)(const std::wstring &filePath);

// Internal deadline for the trust query. It stays below the manifest
// timeout_ms (30000) so an abandoned query can still publish a result before
// the host kills the helper and its process tree. 0 means the budget is
// already exhausted (test hook); no value disables the deadline. The same
// convention is used by image-histogram-property.
constexpr unsigned kQueryDeadlineMs = 20000;

// Pure helper entry point. `arguments` includes the program name at index 0
// and the parsed options from index 1, matching the host invocation
// `--input <path> --output <base> --output-dir <request-dir>`. queryFn
// defaults to the real implementation when nullptr.
int runDigitalSignature(const std::vector<std::wstring> &arguments,
                        SignatureQueryFn queryFn = nullptr);

// queryDeadlineMs is the budget in milliseconds. 0 abandons the query at the
// first checkpoint without starting it (test hook); there is no value that
// disables the internal deadline.
int executeDigitalSignature(const std::wstring &inputPath,
                            const std::wstring &outputBasePath,
                            const std::wstring &outputDirPath,
                            SignatureQueryFn queryFn = nullptr,
                            unsigned queryDeadlineMs = kQueryDeadlineMs);
