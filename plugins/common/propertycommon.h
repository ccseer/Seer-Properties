#pragma once

// Shared Win32 path and publication helpers for the packages built from the
// C++ standard library plus the vendored nlohmann/json
// (`git-info-property`, `digital-signature-property`). The image package is
// Qt-based and keeps its own QString equivalents.
//
// Header-only on purpose: every package is a standalone CMake project, so a
// shared translation unit would mean a library target in each of them.
//
// The containment helpers are security relevant: the request directory is
// host-supplied and its contents are not trusted, so the checks live in one
// place instead of being re-derived per package (the per-package copies had
// already drifted: one capped paths at MAX_PATH and rejected longer ones).

#include <windows.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace propertycommon {

using json = nlohmann::json;

inline bool isExistingDirectory(const std::wstring &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline bool isReparsePoint(const std::wstring &absolute)
{
    const DWORD attributes = GetFileAttributesW(absolute.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// Windows path canonicalization does not traverse directory junctions, so a
// reparse point created inside the request directory could otherwise satisfy a
// purely lexical containment check. Only the components below the request
// directory are probed: its ancestors are chosen by the host, and rejecting them
// would fail every query on hosts whose temp root is redirected.
inline bool hasReparsePointBelow(const std::wstring &canonicalDir,
                                 const std::wstring &canonicalFile)
{
    if (canonicalFile.size() <= canonicalDir.size()
        || _wcsnicmp(canonicalFile.c_str(), canonicalDir.c_str(),
                     canonicalDir.size())
               != 0) {
        return true;
    }
    const wchar_t next = canonicalFile[canonicalDir.size()];
    if (next != L'\\' && next != L'/') {
        return true;
    }
    std::size_t position = canonicalDir.size() + 1;
    while (position <= canonicalFile.size()) {
        std::size_t separator = canonicalFile.find_first_of(L"\\/", position);
        if (separator == std::wstring::npos) {
            separator = canonicalFile.size();
        }
        if (isReparsePoint(canonicalFile.substr(0, separator))) {
            return true;
        }
        position = separator + 1;
    }
    return false;
}

// GetFullPathNameW needs a buffer large enough for the result; a fixed
// MAX_PATH buffer would reject longer paths instead of canonicalizing them.
inline std::wstring absolutePath(const std::wstring &path)
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length
            = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()),
                               buffer.data(), nullptr);
        if (length == 0) {
            return path;
        }
        if (length < buffer.size()) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(static_cast<std::size_t>(length) + 1);
    }
}

inline std::wstring stripTrailingSeparators(const std::wstring &path)
{
    std::wstring trimmed = path;
    while (trimmed.size() > 3
           && (trimmed.back() == L'\\' || trimmed.back() == L'/')) {
        trimmed.pop_back();
    }
    return trimmed;
}

inline bool isContainedInDirectory(const std::wstring &filePath,
                                  const std::wstring &dirPath)
{
    if (!isExistingDirectory(dirPath)) {
        return false;
    }
    const std::wstring fileCanonical = absolutePath(filePath);
    const std::wstring dirCanonical
        = stripTrailingSeparators(absolutePath(dirPath));

    if (_wcsicmp(fileCanonical.c_str(), dirCanonical.c_str()) == 0) {
        return false;
    }
    if (fileCanonical.size() <= dirCanonical.size()
        || _wcsnicmp(fileCanonical.c_str(), dirCanonical.c_str(),
                     dirCanonical.size())
               != 0) {
        return false;
    }
    const wchar_t boundary = fileCanonical[dirCanonical.size()];
    if (boundary != L'\\' && boundary != L'/') {
        return false;
    }
    return !hasReparsePointBelow(dirCanonical, fileCanonical);
}

// The host expands ${output_file} to a base name without an extension and
// discovers the result through "<output-base>.*", so the result is published as
// "<output-base>.json".
inline std::wstring withJsonSuffix(const std::wstring &outputBase)
{
    const std::wstring suffix(L".json");
    if (outputBase.size() >= suffix.size()
        && _wcsicmp(outputBase.c_str() + outputBase.size() - suffix.size(),
                    suffix.c_str())
               == 0) {
        return outputBase;
    }
    return outputBase + suffix;
}

// Atomic publication: write a temporary file in the target directory, flush it,
// then move it over the final name so the result JSON is never observed
// half-written. Only the supplied request directory is written.
//
// The temporary name is generated here instead of via GetTempFileNameW: that
// API fails when the parent directory path is longer than MAX_PATH - 14
// characters, which would silently disable long-path output directories, and
// it also creates the file itself, which the exclusive CreateFileW below would
// then have to overwrite.
inline bool writeJson(const std::wstring &path, const json &root)
{
    const std::string bytes = root.dump(2) + "\n";

    const std::wstring parent = [&path]() {
        const std::size_t separator = path.find_last_of(L"\\/");
        return separator == std::wstring::npos ? std::wstring(L".")
                                               : path.substr(0, separator);
    }();

    // A unique name per attempt: process id plus tick count plus an increasing
    // counter, so concurrent helpers in the same directory never collide and a
    // stale leftover file cannot make CREATE_NEW fail forever.
    const unsigned long pid = GetCurrentProcessId();
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        std::wstring temporary = parent;
        if (temporary.empty()
            || (temporary.back() != L'\\' && temporary.back() != L'/')) {
            temporary += L'\\';
        }
        temporary += std::wstring(L"seer") + std::to_wstring(pid) + L"_"
                     + std::to_wstring(GetTickCount64()) + L"_"
                     + std::to_wstring(attempt) + L".tmp";

        const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0,
                                        nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD createError = GetLastError();
            if (createError == ERROR_FILE_EXISTS
                || createError == ERROR_ALREADY_EXISTS) {
                continue;
            }
            return false;
        }
        DWORD written = 0;
        const BOOL ok = WriteFile(file, bytes.data(),
                                  static_cast<DWORD>(bytes.size()), &written,
                                  nullptr)
                        && FlushFileBuffers(file);
        CloseHandle(file);
        if (!ok || written != bytes.size()) {
            DeleteFileW(temporary.c_str());
            return false;
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    }
    return false;
}

}  // namespace propertycommon
