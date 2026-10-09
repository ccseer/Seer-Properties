#pragma once

#include <string>
#include <vector>

#include "pdbquery.h"

// Pure helper entry point. `arguments` includes the program name at index 0
// and the parsed options from index 1, matching the host invocation
// `--input <path> --output <base> --output-dir <request-dir>` plus the
// optional `--top <N>` and `--deadline <ms>`. queryFn defaults to the real
// implementation when nullptr.
int runPdbProperty(const std::vector<std::wstring> &arguments,
                   PdbQueryFn queryFn = nullptr);

// queryOptions carries the optional analysis switches and the internal
// deadline, which follows the convention shared by the property packages: a
// positive value is the budget in milliseconds, 0 means the budget is already
// exhausted (the query is abandoned at the first checkpoint without running),
// and hasDeadline == false disables the deadline so the host timeout governs.
int executePdbProperty(const std::wstring &inputPath,
                       const std::wstring &outputBasePath,
                       const std::wstring &outputDirPath,
                       const PdbQueryOptions &options = {},
                       PdbQueryFn queryFn = nullptr);
