#pragma once

#include "hashing.h"

#include <cstdint>
#include <string>
#include <vector>

enum class OutputCase {
    Lower,
    Upper
};

struct HashDigest {
    HashAlgorithm algorithm = HashAlgorithm::Sha256;
    std::vector<std::uint8_t> digest;
};

struct HashFileResult {
    bool ok = false;
    std::uint64_t bytes = 0;
    std::wstring error;
    std::vector<HashDigest> digests;
};

// Hashes the input file with a single read pass that feeds every selected
// algorithm; digests come back in the canonical algorithm order.
HashFileResult hashFile(const std::wstring& inputPath,
                        const std::vector<HashAlgorithm>& algorithms);
bool parseArguments(const std::vector<std::wstring>& arguments, std::wstring& input,
                    std::wstring& output, OutputCase& outputCase,
                    std::vector<HashAlgorithm>& algorithms);
int run(const std::vector<std::wstring>& arguments);
