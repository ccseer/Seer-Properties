#include "gitdiscovery.h"

// Shared path helpers; see plugins/common/propertycommon.h.
#include "propertycommon.h"

#include <windows.h>

#include <cwctype>
#include <string>
#include <vector>

namespace gitinfo {

namespace {

using propertycommon::absolutePath;
using propertycommon::isExistingDirectory;
using propertycommon::stripTrailingSeparators;

bool pathIsAbsolute(const std::wstring &path)
{
    if (path.size() >= 3 && std::iswalpha(path[0]) && path[1] == L':'
        && (path[2] == L'\\' || path[2] == L'/')) {
        return true;
    }
    // UNC path.
    return path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
}

std::wstring lowerString(const std::wstring &text)
{
    std::wstring lowered = text;
    for (auto &c : lowered) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return lowered;
}

std::wstring joinPath(const std::wstring &directory,
                      const std::wstring &name)
{
    if (directory.empty()) {
        return name;
    }
    if (directory.back() == L'\\' || directory.back() == L'/') {
        return directory + name;
    }
    return directory + L"\\" + name;
}

bool isExistingFile(const std::wstring &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// True when `candidate` is the inspected directory itself or lives inside it.
bool isInsideInspectedDirectory(const std::wstring &candidate,
                               const std::wstring &inspectedLower)
{
    if (inspectedLower.empty()) {
        return false;
    }
    const std::wstring candidateLower = lowerString(candidate);
    if (candidateLower == inspectedLower) {
        return true;
    }
    if (candidateLower.size() <= inspectedLower.size()
        || candidateLower.compare(0, inspectedLower.size(), inspectedLower)
               != 0) {
        return false;
    }
    const wchar_t boundary = candidateLower[inspectedLower.size()];
    return boundary == L'\\' || boundary == L'/';
}

}  // namespace

bool isUsableGitOverride(const std::wstring &candidate, std::wstring &resolved)
{
    if (candidate.empty() || !pathIsAbsolute(candidate)) {
        return false;
    }
    if (!isExistingFile(candidate)) {
        return false;
    }
    resolved = absolutePath(candidate);
    return true;
}

std::wstring discoverGitFromPath(const std::wstring &inspectedDirectory)
{
    const std::wstring inspectedLower
        = inspectedDirectory.empty()
              ? std::wstring()
              : lowerString(stripTrailingSeparators(
                    absolutePath(inspectedDirectory)));

    std::wstring pathValue;
    {
        const DWORD length = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (length == 0) {
            return std::wstring();
        }
        std::vector<wchar_t> buffer(length);
        // The environment block can change between the two calls (another
        // thread or process updating PATH); a short read would leave the
        // buffer with an unterminated value, so a mismatch is treated as
        // "no PATH" instead of scanning garbage.
        const DWORD copied = GetEnvironmentVariableW(L"PATH", buffer.data(),
                                                     length);
        if (copied == 0 || copied >= length) {
            return std::wstring();
        }
        pathValue = buffer.data();
    }

    // Empty and relative entries are skipped: they resolve against the working
    // directory, which is not a trusted place to find an executable.
    std::vector<std::wstring> directories;
    std::size_t start = 0;
    while (start <= pathValue.size()) {
        const std::size_t separator = pathValue.find(L';', start);
        const std::wstring entry
            = pathValue.substr(start, separator == std::wstring::npos
                                          ? std::wstring::npos
                                          : separator - start);
        if (!entry.empty() && pathIsAbsolute(entry) && isExistingDirectory(entry)) {
            directories.push_back(entry);
        }
        if (separator == std::wstring::npos) {
            break;
        }
        start = separator + 1;
    }

    // git.cmd / git.bat cannot be launched through CreateProcessW, so only the
    // real executable is considered.
    for (const auto &directory : directories) {
        const std::wstring candidate = joinPath(directory, L"git.exe");
        if (!isExistingFile(candidate)) {
            continue;
        }
        const std::wstring resolved = absolutePath(candidate);
        if (isInsideInspectedDirectory(resolved, inspectedLower)) {
            continue;
        }
        return resolved;
    }
    return std::wstring();
}

}  // namespace gitinfo
