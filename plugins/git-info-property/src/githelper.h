#pragma once

#include <string>
#include <vector>

#include "gitrunner.h"

namespace gitinfo {

// Entry point for the packaged helper. `arguments` includes the program name at
// index 0 and the parsed options from index 1, matching the host invocation
// `--input <dir> --output <base> --output-dir <request-dir> [--git <exe>]`.
//
// Expected domain outcomes (Git unavailable, not a repository, unsafe ownership,
// unavailable query) still publish valid schema-1 JSON and return 0. A nonzero
// return code means the invocation was malformed or the result could not be
// written.
int runGitInfo(const std::vector<std::wstring> &arguments);

}  // namespace gitinfo
