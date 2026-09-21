#pragma once

#include <string>

namespace gitinfo {

// Accepts only an absolute path to an existing executable file. Returns true and
// sets resolved to the absolute path. Relative names and bare file names are
// rejected so discovery never depends on the current directory.
bool isUsableGitOverride(const std::wstring &candidate, std::wstring &resolved);

// Discovers git.exe in the environment PATH.
//
// Only absolute, existing, non-empty PATH entries are considered and only the
// executable name "git.exe" is accepted, because the resolved path is later
// passed to CreateProcessW. Entries that resolve to the inspected directory or
// live inside it are skipped, so a repository cannot supply the executable used
// to inspect it. Candidates are resolved to absolute paths before use.
// Returns an empty string when nothing usable is found.
std::wstring discoverGitFromPath(const std::wstring &inspectedDirectory = {});

}  // namespace gitinfo
