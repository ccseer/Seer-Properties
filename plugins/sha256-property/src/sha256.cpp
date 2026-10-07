#include "sha256.h"

#include <windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr DWORD kReadBufferSize = 1024 * 1024;

// Subgroup title used when several algorithms are published in one group.
const char* const kHashGroupTitle = "Hashes";

std::wstring win32Error(const wchar_t* operation, DWORD code = GetLastError())
{
    wchar_t* message = nullptr;
    const auto flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                       | FORMAT_MESSAGE_IGNORE_INSERTS;
    const auto length = FormatMessageW(flags, nullptr, code, 0,
                                       reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result = operation;
    result += L" failed";
    if (length != 0 && message != nullptr) {
        result += L": ";
        result.append(message, length);
        LocalFree(message);
    }
    return result;
}

bool sameFileState(HANDLE file, const LARGE_INTEGER& sizeBefore, const FILETIME& timeBefore)
{
    LARGE_INTEGER sizeAfter{};
    FILETIME creation{}, access{}, write{};
    return GetFileSizeEx(file, &sizeAfter) && GetFileTime(file, &creation, &access, &write)
           && sizeAfter.QuadPart == sizeBefore.QuadPart
           && CompareFileTime(&write, &timeBefore) == 0;
}

bool writeAll(HANDLE file, const char* data, DWORD length)
{
    while (length != 0) {
        DWORD written = 0;
        if (!WriteFile(file, data, length, &written, nullptr) || written == 0)
            return false;
        data += written;
        length -= written;
    }
    return true;
}

std::wstring outputPathFor(const std::wstring& base)
{
    constexpr wchar_t suffix[] = L".json";
    if (base.size() >= 5 && _wcsicmp(base.c_str() + base.size() - 5, suffix) == 0)
        return base;
    return base + suffix;
}

// Parses the comma-separated algorithm list. Names are case-insensitive and
// may carry surrounding spaces; the special name "all" is only accepted as
// the single token of the list. Duplicate names collapse into one selection,
// and the resulting order is always the canonical algorithm order.
bool parseAlgorithmList(const std::wstring& value, std::vector<HashAlgorithm>& selected)
{
    std::set<HashAlgorithm> chosen;
    bool sawAll = false;
    std::size_t tokenCount = 0;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t comma = value.find(L',', begin);
        const std::wstring token = value.substr(
            begin, comma == std::wstring::npos ? std::wstring::npos : comma - begin);
        std::size_t first = 0;
        std::size_t last = token.size();
        while (first < last && (token[first] == L' ' || token[first] == L'\t'))
            ++first;
        while (last > first && (token[last - 1] == L' ' || token[last - 1] == L'\t'))
            --last;
        const std::wstring trimmed = token.substr(first, last - first);
        if (trimmed.empty())
            return false;
        if (lowerAscii(trimmed) == L"all") {
            if (tokenCount > 0)
                return false;
            sawAll = true;
        } else {
            HashAlgorithm parsed = HashAlgorithm::Sha256;
            if (sawAll || !hashAlgorithmFromName(trimmed, parsed))
                return false;
            chosen.insert(parsed);
        }
        ++tokenCount;
        if (comma == std::wstring::npos)
            break;
        begin = comma + 1;
    }

    selected.clear();
    if (sawAll) {
        selected = allHashAlgorithms();
        return true;
    }
    for (const HashAlgorithm algorithm : allHashAlgorithms()) {
        if (chosen.count(algorithm) != 0)
            selected.push_back(algorithm);
    }
    return !selected.empty();
}

// Algorithm labels are ASCII, so narrowing for the JSON serializer is safe.
std::string narrowLabel(const std::wstring& label)
{
    std::string narrow;
    narrow.reserve(label.size());
    for (const wchar_t character : label)
        narrow.push_back(static_cast<char>(character));
    return narrow;
}

}

bool parseArguments(const std::vector<std::wstring>& arguments, std::wstring& input,
                    std::wstring& output, OutputCase& outputCase,
                    std::vector<HashAlgorithm>& algorithms)
{
    outputCase = OutputCase::Lower;
    if (arguments.size() < 5 || arguments.size() % 2 == 0)
        return false;
    bool inputSeen = false;
    bool outputSeen = false;
    bool caseSeen = false;
    bool algorithmsSeen = false;
    for (size_t index = 1; index + 1 < arguments.size(); index += 2) {
        const auto& option = arguments[index];
        const auto& value = arguments[index + 1];
        if (value.empty())
            return false;
        if (option == L"--input" && !inputSeen) {
            input = value;
            inputSeen = true;
        } else if (option == L"--output" && !outputSeen) {
            output = value;
            outputSeen = true;
        } else if (option == L"--case" && !caseSeen) {
            if (value == L"lower") {
                outputCase = OutputCase::Lower;
            } else if (value == L"upper") {
                outputCase = OutputCase::Upper;
            } else {
                return false;
            }
            caseSeen = true;
        } else if (option == L"--algorithms" && !algorithmsSeen) {
            if (!parseAlgorithmList(value, algorithms))
                return false;
            algorithmsSeen = true;
        } else {
            return false;
        }
    }
    if (!inputSeen || !outputSeen)
        return false;
    if (!algorithmsSeen)
        algorithms.assign(1, HashAlgorithm::Sha256);
    return true;
}

HashFileResult hashFile(const std::wstring& inputPath,
                        const std::vector<HashAlgorithm>& algorithms)
{
    HashFileResult result;
    const auto file = CreateFileW(inputPath.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.error = win32Error(L"CreateFileW");
        return result;
    }

    LARGE_INTEGER sizeBefore{};
    FILETIME creation{}, access{}, timeBefore{};
    if (!GetFileSizeEx(file, &sizeBefore) || !GetFileTime(file, &creation, &access, &timeBefore)) {
        result.error = win32Error(L"Read file metadata");
        CloseHandle(file);
        return result;
    }

    std::vector<std::unique_ptr<HashSink>> sinks;
    sinks.reserve(algorithms.size());
    for (const HashAlgorithm algorithm : algorithms) {
        std::unique_ptr<HashSink> sink;
        std::wstring error;
        if (!createHashSink(algorithm, sink, error)) {
            result.error = error;
            CloseHandle(file);
            return result;
        }
        sinks.push_back(std::move(sink));
    }

    std::vector<char> buffer(kReadBufferSize);
    bool readOk = true;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), kReadBufferSize, &read, nullptr)) {
            readOk = false;
            result.error = win32Error(L"ReadFile");
            break;
        }
        if (read == 0)
            break;
        for (const auto& sink : sinks) {
            if (!sink->update(buffer.data(), static_cast<std::size_t>(read), result.error)) {
                readOk = false;
                break;
            }
        }
        if (!readOk)
            break;
        result.bytes += read;
    }

    if (readOk && !sameFileState(file, sizeBefore, timeBefore)) {
        readOk = false;
        result.error = L"Input file changed while hashing";
    }
    if (readOk) {
        for (std::size_t index = 0; index < sinks.size() && readOk; ++index) {
            HashDigest entry;
            entry.algorithm = algorithms[index];
            if (!sinks[index]->finalize(entry.digest, result.error)) {
                readOk = false;
            } else {
                result.digests.push_back(std::move(entry));
            }
        }
    }

    CloseHandle(file);
    result.ok = readOk;
    if (!result.ok) {
        result.digests.clear();
        result.bytes = 0;
    }
    return result;
}

int run(const std::vector<std::wstring>& arguments)
{
    std::wstring input;
    std::wstring outputBase;
    OutputCase outputCase = OutputCase::Lower;
    std::vector<HashAlgorithm> algorithms;
    if (!parseArguments(arguments, input, outputBase, outputCase, algorithms))
        return 2;

    const auto result = hashFile(input, algorithms);
    if (!result.ok) {
        std::wcerr << result.error << L'\n';
        return 3;
    }

    const char* digits = (outputCase == OutputCase::Upper) ? "0123456789ABCDEF"
                                                           : "0123456789abcdef";
    const auto toHex = [digits](const std::vector<std::uint8_t>& digest) {
        std::string text;
        text.reserve(digest.size() * 2);
        for (const auto byte : digest) {
            text.push_back(digits[byte >> 4]);
            text.push_back(digits[byte & 0x0f]);
        }
        return text;
    };

    // One algorithm stays a flat single row; several algorithms are grouped
    // into one subgroup whose value is an ordered array of one-key fields.
    // ordered_json keeps the documented key order in the published bytes.
    nlohmann::ordered_json data;
    if (result.digests.size() == 1) {
        const auto& entry = result.digests.front();
        data[narrowLabel(hashAlgorithmLabel(entry.algorithm))] = toHex(entry.digest);
    } else {
        nlohmann::ordered_json rows = nlohmann::ordered_json::array();
        for (const auto& entry : result.digests) {
            rows.push_back(
                nlohmann::ordered_json{{narrowLabel(hashAlgorithmLabel(entry.algorithm)),
                                        toHex(entry.digest)}});
        }
        nlohmann::ordered_json group;
        group["value"] = std::move(rows);
        data[kHashGroupTitle] = std::move(group);
    }
    nlohmann::ordered_json envelope;
    envelope["result_schema"] = 1;
    envelope["data"] = std::move(data);
    const std::string json = envelope.dump() + "\n";

    const auto outputPath = outputPathFor(outputBase);
    const auto parent = std::filesystem::path(outputPath).parent_path();
    const auto directory = parent.empty() ? std::filesystem::path(L".") : parent;
    wchar_t temporary[MAX_PATH]{};
    if (GetTempFileNameW(directory.c_str(), L"sha", 0, temporary) == 0) {
        std::wcerr << L"failed to create temporary output file\n";
        return 4;
    }
    const auto temporaryPath = std::wstring(temporary);
    const auto file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::wcerr << win32Error(L"CreateFileW for output") << L'\n';
        DeleteFileW(temporaryPath.c_str());
        return 4;
    }
    const bool written = writeAll(file, json.data(), static_cast<DWORD>(json.size()))
                         && FlushFileBuffers(file);
    CloseHandle(file);
    if (!written || !MoveFileExW(temporaryPath.c_str(), outputPath.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::wcerr << win32Error(L"Publish output") << L'\n';
        DeleteFileW(temporaryPath.c_str());
        return 4;
    }
    return 0;
}
