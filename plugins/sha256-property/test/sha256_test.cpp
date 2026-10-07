#include "sha256.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {
int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path tempDirectory()
{
    wchar_t buffer[MAX_PATH]{};
    GetTempPathW(MAX_PATH, buffer);
    const auto path = std::filesystem::path(buffer) / L"Seer Sha256 P\u00E4th-\u30C6\u30B9\u30C8";
    std::filesystem::create_directories(path);
    return path;
}

std::filesystem::path writeFile(const std::filesystem::path& path, const std::string& data)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    return path;
}

std::string bytesHex(const std::vector<std::uint8_t>& digest)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string value;
    value.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        value.push_back(digits[byte >> 4]);
        value.push_back(digits[byte & 0x0f]);
    }
    return value;
}

const HashDigest* findDigest(const HashFileResult& result, HashAlgorithm algorithm)
{
    for (const auto& entry : result.digests) {
        if (entry.algorithm == algorithm)
            return &entry;
    }
    return nullptr;
}

void checkVector(const HashFileResult& result, HashAlgorithm algorithm,
                 const char* expected, const char* message)
{
    const auto* entry = findDigest(result, algorithm);
    check(entry != nullptr && bytesHex(entry->digest) == expected, message);
}

std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Reference digests for the shared fixtures, cross-checked against Python
// hashlib/zlib and the reference xxhash/blake3 packages.
constexpr const char* kAbcSha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr const char* kAbcCrc32 = "352441c2";
constexpr const char* kAbcBlake3 = "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85";
constexpr const char* kAbcSha512 =
    "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f";
constexpr const char* kAbcXxHash = "44bc2cf5ad770999";
constexpr const char* kAbcSha3_256 =
    "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532";
constexpr const char* kAbcCrc64 = "2cd8094a1a277627";
constexpr const char* kAbcSha384 =
    "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
    "8086072ba1e7cc2358baeca134c825a7";

// Grouped shape for "abc" with every algorithm, in canonical priority order.
constexpr const char* kAbcGroupedJson =
    R"json({"result_schema":1,"data":{"Hashes":{"value":[)json"
    R"json({"SHA-256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},)json"
    R"json({"CRC32":"352441c2"},)json"
    R"json({"BLAKE3":"6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85"},)json"
    R"json({"SHA-512":"ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"},)json"
    R"json({"xxHash":"44bc2cf5ad770999"},)json"
    R"json({"SHA-3-256":"3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"},)json"
    R"json({"CRC64":"2cd8094a1a277627"},)json"
    R"json({"SHA-384":"cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"})json"
    R"json(]}}})json";
}

int main()
{
    const auto directory = tempDirectory();
    const auto empty = writeFile(directory / L"empty.bin", "");
    const auto abc = writeFile(directory / L"abc.txt", "abc");
    const auto check9 = writeFile(directory / L"check9.txt", "123456789");
    const auto binary = writeFile(directory / L"binary.bin", std::string("\0\x01\x7f\xff", 4));
    const auto unicode = writeFile(directory / L"P\u00E4th-\u30C6\u30B9\u30C8 file.txt", "unicode");

    const auto& all = allHashAlgorithms();
    check(all.size() == 8, "eight algorithms are registered");
    check(hashAlgorithmLabel(all.front()) == L"SHA-256", "SHA-256 has the top priority");
    check(hashAlgorithmLabel(all.back()) == L"SHA-384", "SHA-384 has the lowest priority");

    // Every algorithm in one pass over the same file; digests arrive in the
    // canonical priority order.
    const auto emptyResult = hashFile(empty.wstring(), all);
    check(emptyResult.ok && emptyResult.bytes == 0, "empty file hashes");
    check(emptyResult.digests.size() == all.size(), "one digest per algorithm");
    bool canonicalOrder = true;
    for (std::size_t index = 0; index < emptyResult.digests.size(); ++index) {
        if (emptyResult.digests[index].algorithm != all[index])
            canonicalOrder = false;
    }
    check(canonicalOrder, "digests follow the canonical algorithm order");
    checkVector(emptyResult, HashAlgorithm::Sha256,
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                "empty SHA-256 vector");
    checkVector(emptyResult, HashAlgorithm::Crc32, "00000000", "empty CRC32 vector");
    checkVector(emptyResult, HashAlgorithm::Blake3,
                "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262",
                "empty BLAKE3 vector");
    checkVector(emptyResult, HashAlgorithm::Sha512,
                "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
                "empty SHA-512 vector");
    checkVector(emptyResult, HashAlgorithm::XxHash, "ef46db3751d8e999",
                "empty xxHash vector");
    checkVector(emptyResult, HashAlgorithm::Sha3_256,
                "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a",
                "empty SHA-3-256 vector");
    checkVector(emptyResult, HashAlgorithm::Crc64, "0000000000000000",
                "empty CRC64 vector");
    checkVector(emptyResult, HashAlgorithm::Sha384,
                "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da"
                "274edebfe76f65fbd51ad2f14898b95b",
                "empty SHA-384 vector");

    const auto abcResult = hashFile(abc.wstring(), all);
    check(abcResult.ok && abcResult.bytes == 3, "abc file hashes");
    checkVector(abcResult, HashAlgorithm::Sha256, kAbcSha256, "abc SHA-256 vector");
    checkVector(abcResult, HashAlgorithm::Crc32, kAbcCrc32, "abc CRC32 vector");
    checkVector(abcResult, HashAlgorithm::Blake3, kAbcBlake3, "abc BLAKE3 vector");
    checkVector(abcResult, HashAlgorithm::Sha512, kAbcSha512, "abc SHA-512 vector");
    checkVector(abcResult, HashAlgorithm::XxHash, kAbcXxHash, "abc xxHash vector");
    checkVector(abcResult, HashAlgorithm::Sha3_256, kAbcSha3_256, "abc SHA-3-256 vector");
    checkVector(abcResult, HashAlgorithm::Crc64, kAbcCrc64, "abc CRC64 vector");
    checkVector(abcResult, HashAlgorithm::Sha384, kAbcSha384, "abc SHA-384 vector");

    // Classic single-algorithm check values for the checksum algorithms.
    const auto check9Result = hashFile(check9.wstring(), all);
    check(check9Result.ok, "check9 file hashes");
    checkVector(check9Result, HashAlgorithm::Crc32, "cbf43926", "CRC32 check value");
    checkVector(check9Result, HashAlgorithm::Crc64, "995dc9bbdf1939fa",
                "CRC64/XZ check value");
    checkVector(check9Result, HashAlgorithm::XxHash, "8cb841db40e6ae83",
                "xxHash check vector");

    const auto binaryResult = hashFile(binary.wstring(), all);
    check(binaryResult.ok && binaryResult.bytes == 4, "binary fixture hashes");
    checkVector(binaryResult, HashAlgorithm::Sha256,
                "9beb9b4fbb3161c1c60d01c253b504f0dd2ea909f764fd3d7c8213fa1580ae94",
                "binary SHA-256 vector");
    checkVector(binaryResult, HashAlgorithm::Crc32, "a5233f9f", "binary CRC32 vector");
    checkVector(binaryResult, HashAlgorithm::Blake3,
                "056a2ac59f71a153a4a86c0026c24dfb821f6b96269aa0d102f42510aea24200",
                "binary BLAKE3 vector");

    const auto unicodeResult = hashFile(unicode.wstring(), all);
    check(unicodeResult.ok && unicodeResult.bytes == 7, "Unicode path hashes");

    const auto large = directory / L"larger-than-buffer.bin";
    {
        std::ofstream output(large, std::ios::binary | std::ios::trunc);
        const std::string block(1024 * 1024, 'x');
        for (int index = 0; index < 3; ++index)
            output.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    const auto largeResult = hashFile(large.wstring(), all);
    check(largeResult.ok && largeResult.bytes == 3 * 1024 * 1024
              && largeResult.digests.size() == all.size(),
          "file larger than read buffer hashes with every algorithm");
    checkVector(largeResult, HashAlgorithm::Sha256,
                "3bea8a9a07c1e8dcaa4c1b816815c35a29b4fb585ba6ecc70ea44840a794cfb3",
                "large-file SHA-256 vector");
    checkVector(largeResult, HashAlgorithm::Sha3_256,
                "8433444632952710fdd6f7d16517454dbd04adb56a5a63088fbcee486d3b051c",
                "large-file SHA-3-256 vector");
    checkVector(largeResult, HashAlgorithm::Blake3,
                "e8b7e60f5520dab1baac40abb3a934ef43e04640c09dd7d423e989fd665a3df0",
                "large-file BLAKE3 vector");
    checkVector(largeResult, HashAlgorithm::XxHash, "2b13b122ad0b4715",
                "large-file xxHash vector");
    checkVector(largeResult, HashAlgorithm::Crc64, "1a8570f5a9c424e7",
                "large-file CRC64 vector");
    checkVector(largeResult, HashAlgorithm::Crc32, "becc1448", "large-file CRC32 vector");

    const auto missingResult = hashFile((directory / L"missing.bin").wstring(),
                                        {HashAlgorithm::Sha256});
    check(!missingResult.ok && !missingResult.error.empty(), "input-open failure is reported");

    const auto deleteRace = directory / L"delete-race.bin";
    {
        std::ofstream output(deleteRace, std::ios::binary | std::ios::trunc);
        const std::string block(1024 * 1024, 'd');
        for (int index = 0; index < 256; ++index)
            output.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    std::atomic<bool> deleteRaceFinished = false;
    HashFileResult deleteRaceResult;
    std::thread deleteRaceHasher([&] {
        deleteRaceResult = hashFile(deleteRace.wstring(), {HashAlgorithm::Sha256});
        deleteRaceFinished = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool deletionBlocked = !deleteRaceFinished && DeleteFileW(deleteRace.c_str()) == FALSE;
    deleteRaceHasher.join();
    check(deletionBlocked && deleteRaceResult.ok,
          "hashFile blocks deletion while reading its input");

    const auto outputBase = directory / L"result";
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output", outputBase.wstring()})
              == 0,
          "process succeeds for valid input");
    const auto outputPath = outputBase.wstring() + L".json";
    const auto outputText = readText(outputPath);
    const std::string expectedDefault =
        std::string(R"json({"result_schema":1,"data":{"SHA-256":")json") + kAbcSha256
                    + R"json("}})json" + "\n";
    check(outputText == expectedDefault,
          "default output is the schema-1 envelope with one flat row");

    const auto lowerBase = directory / L"result-lower";
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output", lowerBase.wstring(),
               L"--case", L"lower"})
              == 0,
          "process succeeds with explicit lower case");
    const auto lowerText = readText(lowerBase.wstring() + L".json");
    check(lowerText == outputText, "explicit lower matches default lower output");

    const auto upperBase = directory / L"result-upper";
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output", upperBase.wstring(),
               L"--case", L"upper"})
              == 0,
          "process succeeds with explicit upper case");
    const std::string expectedUpper =
        std::string(R"json({"result_schema":1,"data":{"SHA-256":")json")
        + "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"
        + R"json("}})json" + "\n";
    check(readText(upperBase.wstring() + L".json") == expectedUpper,
          "upper output produces exact ASCII uppercase digest");

    const auto upperOrderBase = directory / L"result-upper-order";
    check(run({L"sha256_property.exe", L"--case", L"upper", L"--input", abc.wstring(),
               L"--output", upperOrderBase.wstring()})
              == 0,
          "process succeeds with case option placed first");
    check(readText(upperOrderBase.wstring() + L".json") == expectedUpper,
          "case option ordering independence produces matching output");

    const auto crc32Base = directory / L"result-crc32";
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output", crc32Base.wstring(),
               L"--algorithms", L"crc32"})
              == 0,
          "process succeeds with a single non-default algorithm");
    const std::string expectedCrc32 =
        std::string(R"json({"result_schema":1,"data":{"CRC32":")json") + kAbcCrc32
                    + R"json("}})json" + "\n";
    check(readText(crc32Base.wstring() + L".json") == expectedCrc32,
          "single selected algorithm stays a flat one-key row");

    const auto crc32UpperNameBase = directory / L"result-crc32-uppername";
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output",
               crc32UpperNameBase.wstring(), L"--algorithms", L"CRC32"})
              == 0,
          "algorithm names are case-insensitive");
    check(readText(crc32UpperNameBase.wstring() + L".json") == expectedCrc32,
          "uppercase algorithm name matches lowercase output");

    const auto crc32UpperBase = directory / L"result-crc32-upper";
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output",
               crc32UpperBase.wstring(), L"--algorithms", L"crc32", L"--case", L"upper"})
              == 0,
          "case option applies to the selected algorithm");
    check(readText(crc32UpperBase.wstring() + L".json")
              == std::string(R"json({"result_schema":1,"data":{"CRC32":")json")
                             + "352441C2"
                             + R"json("}})json" + "\n",
          "upper case applies to the CRC32 digest");

    const auto groupBase = directory / L"result-group";
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output", groupBase.wstring(),
               L"--algorithms", L"blake3,crc32"})
              == 0,
          "process succeeds with several algorithms");
    const std::string expectedGroup =
        std::string(R"json({"result_schema":1,"data":{"Hashes":{"value":[{"CRC32":")json")
                    + kAbcCrc32 + R"json("},{"BLAKE3":")json" + kAbcBlake3
                    + R"json("}]}}})json" + "\n";
    check(readText(groupBase.wstring() + L".json") == expectedGroup,
          "several algorithms are grouped in canonical order");

    // Documented behaviour: names may carry surrounding spaces.
    const auto spacedBase = directory / L"result-spaced";
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output",
               spacedBase.wstring(), L"--algorithms", L" blake3 , crc32 "})
              == 0,
          "process tolerates spaces around algorithm names");
    check(readText(spacedBase.wstring() + L".json") == expectedGroup,
          "spaced algorithm list matches the compact grouped output");

    const auto allBase = directory / L"result-all";
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output", allBase.wstring(),
               L"--algorithms", L"all"})
              == 0,
          "process succeeds with the all selection");
    check(readText(allBase.wstring() + L".json") == std::string(kAbcGroupedJson) + "\n",
          "all selection publishes every algorithm in canonical order");

    const auto allMixedCaseBase = directory / L"result-all-mixed";
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output",
               allMixedCaseBase.wstring(), L"--algorithms", L"ALL"})
              == 0,
          "the all selection is case-insensitive");
    check(readText(allMixedCaseBase.wstring() + L".json")
              == std::string(kAbcGroupedJson) + "\n",
          "mixed-case all selection matches the canonical grouped output");

    const auto dedupBase = directory / L"result-dedup";
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output", dedupBase.wstring(),
               L"--algorithms", L"sha256,sha256"})
              == 0,
          "duplicate algorithm names collapse into one selection");
    check(readText(dedupBase.wstring() + L".json") == expectedDefault,
          "deduplicated single selection stays a flat row");

    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output", outputBase.wstring(),
               L"--algorithms", L"unknown"})
              == 2,
          "run returns exit code 2 on unknown algorithm name");
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output", outputBase.wstring(),
               L"--algorithms", L"crc32,,blake3"})
              == 2,
          "run returns exit code 2 on an empty algorithm list item");
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output", outputBase.wstring(),
               L"--algorithms", L"all,crc32"})
              == 2,
          "run returns exit code 2 when all is mixed with names");
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output", outputBase.wstring(),
               L"--algorithms", L""})
              == 2,
          "run returns exit code 2 on an empty algorithms value");
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output", outputBase.wstring(),
               L"--algorithms", L"crc32", L"--algorithms", L"blake3"})
              == 2,
          "run returns exit code 2 on duplicate algorithms option");
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output", outputBase.wstring(),
               L"--algorithms"})
              == 2,
          "run returns exit code 2 on a trailing algorithms option");
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output", outputBase.wstring(),
               L"--case", L"invalid"})
              == 2,
          "run returns exit code 2 on unknown case value");
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output", outputBase.wstring(),
               L"--case", L"lower", L"--case", L"upper"})
              == 2,
          "run returns exit code 2 on duplicate case option");

    const auto suffixedBase = directory / L"result-suffixed.json";
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output",
               suffixedBase.wstring()})
              == 0,
          "process accepts an output base ending in .json");
    check(std::filesystem::exists(suffixedBase)
              && !std::filesystem::exists(suffixedBase.wstring() + L".json"),
          "output suffix is appended exactly once");
    check(run({L"sha256_property.exe", L"--input", abc.wstring(), L"--output",
               (directory / L"missing-dir" / L"result").wstring()})
              != 0,
          "output-create failure is nonzero");
    check(run({L"sha256_property.exe", L"--input", (directory / L"missing.bin").wstring(),
               L"--output", (directory / L"missing-result").wstring()})
              != 0,
          "input-open process failure is nonzero");

    std::wstring parsedInput;
    std::wstring parsedOutput;
    OutputCase parsedCase = OutputCase::Upper;
    std::vector<HashAlgorithm> parsedAlgorithms;
    check(parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json"},
                         parsedInput, parsedOutput, parsedCase, parsedAlgorithms)
              && parsedInput == L"in.bin" && parsedOutput == L"out.json"
              && parsedCase == OutputCase::Lower
                  && parsedAlgorithms == std::vector<HashAlgorithm>{HashAlgorithm::Sha256},
          "parseArguments default case is Lower and default algorithm is SHA-256");

    check(parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                          L"--case", L"lower"},
                         parsedInput, parsedOutput, parsedCase, parsedAlgorithms)
              && parsedCase == OutputCase::Lower,
          "parseArguments explicit lower");

    check(parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                          L"--case", L"upper"},
                         parsedInput, parsedOutput, parsedCase, parsedAlgorithms)
              && parsedCase == OutputCase::Upper,
          "parseArguments explicit upper");

    check(parseArguments({L"sha256_property.exe", L"--case", L"upper", L"--input", L"in.bin",
                          L"--output", L"out.json"},
                         parsedInput, parsedOutput, parsedCase, parsedAlgorithms)
              && parsedCase == OutputCase::Upper,
          "parseArguments case before required options");

    check(parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--case", L"upper",
                          L"--output", L"out.json"},
                         parsedInput, parsedOutput, parsedCase, parsedAlgorithms)
              && parsedCase == OutputCase::Upper,
          "parseArguments case between required options");

    check(parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                          L"--algorithms", L"crc32,blake3"},
                         parsedInput, parsedOutput, parsedCase, parsedAlgorithms)
              && parsedAlgorithms
                     == std::vector<HashAlgorithm>{HashAlgorithm::Crc32, HashAlgorithm::Blake3},
          "parseArguments keeps the canonical algorithm order");

    check(parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                          L"--algorithms", L"blake3,crc32"},
                         parsedInput, parsedOutput, parsedCase, parsedAlgorithms)
              && parsedAlgorithms
                     == std::vector<HashAlgorithm>{HashAlgorithm::Crc32, HashAlgorithm::Blake3},
          "parseArguments reorders a reversed list into canonical order");

    check(parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                          L"--algorithms", L" crc32 , blake3 "},
                         parsedInput, parsedOutput, parsedCase, parsedAlgorithms)
              && parsedAlgorithms
                     == std::vector<HashAlgorithm>{HashAlgorithm::Crc32, HashAlgorithm::Blake3},
          "parseArguments tolerates spaces around algorithm names");

    check(parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                          L"--algorithms", L"crc32,crc32"},
                         parsedInput, parsedOutput, parsedCase, parsedAlgorithms)
              && parsedAlgorithms == std::vector<HashAlgorithm>{HashAlgorithm::Crc32},
          "parseArguments deduplicates repeated names");

    check(parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                          L"--algorithms", L"ALL"},
                         parsedInput, parsedOutput, parsedCase, parsedAlgorithms)
              && parsedAlgorithms == allHashAlgorithms(),
          "parseArguments expands all into every algorithm");

    check(!parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                           L"--case", L"lower", L"--case", L"upper"},
                          parsedInput, parsedOutput, parsedCase, parsedAlgorithms),
          "parseArguments rejects duplicate --case");

    check(!parseArguments({L"sha256_property.exe", L"--input", L"in1.bin", L"--input", L"in2.bin",
                           L"--output", L"out.json"},
                          parsedInput, parsedOutput, parsedCase, parsedAlgorithms),
          "parseArguments rejects duplicate --input");

    check(!parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out1.json",
                           L"--output", L"out2.json"},
                          parsedInput, parsedOutput, parsedCase, parsedAlgorithms),
          "parseArguments rejects duplicate --output");

    check(!parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                           L"--algorithms", L"crc32", L"--algorithms", L"blake3"},
                          parsedInput, parsedOutput, parsedCase, parsedAlgorithms),
          "parseArguments rejects duplicate --algorithms");

    check(!parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                           L"--case", L"UPPER"},
                          parsedInput, parsedOutput, parsedCase, parsedAlgorithms),
          "parseArguments rejects case value with uppercase option string");

    check(!parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                           L"--case", L"unknown"},
                          parsedInput, parsedOutput, parsedCase, parsedAlgorithms),
          "parseArguments rejects unknown case value");

    check(!parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                           L"--algorithms", L"unknown"},
                          parsedInput, parsedOutput, parsedCase, parsedAlgorithms),
          "parseArguments rejects unknown algorithm names");

    check(!parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                           L"--algorithms", L"crc32,,blake3"},
                          parsedInput, parsedOutput, parsedCase, parsedAlgorithms),
          "parseArguments rejects empty algorithm list items");

    check(!parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                           L"--algorithms", L"all,crc32"},
                          parsedInput, parsedOutput, parsedCase, parsedAlgorithms),
          "parseArguments rejects all mixed with algorithm names");

    check(!parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                           L"--algorithms", L""},
                          parsedInput, parsedOutput, parsedCase, parsedAlgorithms),
          "parseArguments rejects an empty algorithms value");

    check(!parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                           L"--case"},
                          parsedInput, parsedOutput, parsedCase, parsedAlgorithms),
          "parseArguments rejects missing option value");

    check(!parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                           L"--case", L""},
                          parsedInput, parsedOutput, parsedCase, parsedAlgorithms),
          "parseArguments rejects empty case value");

    check(!parseArguments({L"sha256_property.exe", L"--input", L"in.bin", L"--output", L"out.json",
                           L"--unknown", L"val"},
                          parsedInput, parsedOutput, parsedCase, parsedAlgorithms),
          "parseArguments rejects unknown option");

    const auto changed = directory / L"changed.bin";
    {
        std::ofstream output(changed, std::ios::binary | std::ios::trunc);
        const std::string block(1024 * 1024, 'y');
        for (int index = 0; index < 256; ++index)
            output.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    std::thread modifier([&changed] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::ofstream output(changed, std::ios::binary | std::ios::app);
        for (int index = 0; index < 32 && output; ++index) {
            output.put('z');
            output.flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    const auto changedResult = hashFile(changed.wstring(), {HashAlgorithm::Sha256});
    modifier.join();
    check(!changedResult.ok, "changed input is rejected");

    return failures == 0 ? 0 : 1;
}
