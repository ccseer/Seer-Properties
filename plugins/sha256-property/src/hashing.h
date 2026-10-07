#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// All algorithms the helper can compute, with the canonical priority order
// used both for CLI name parsing and for the published row order.
// MD5 and SHA-1 are intentionally absent: the host already reports both by
// default, so this package does not publish a second copy of them.
enum class HashAlgorithm {
    Sha256,
    Crc32,
    Blake3,
    Sha512,
    XxHash,
    Sha3_256,
    Crc64,
    Sha384
};

// Every supported algorithm in the canonical display/priority order.
const std::vector<HashAlgorithm>& allHashAlgorithms();

// Row label published in the result JSON (exactly as the Inspector shows it).
std::wstring hashAlgorithmLabel(HashAlgorithm algorithm);

// ASCII case folding for CLI tokens; only the A-Z range is mapped.
std::wstring lowerAscii(const std::wstring& text);

// Case-insensitive match of a CLI token such as L"sha3-256"; accepts "all"
// only through the dedicated list handling, not here. Returns false for
// unknown names.
bool hashAlgorithmFromName(const std::wstring& name, HashAlgorithm& parsed);

// One incremental hasher per selected algorithm; all sinks are fed the same
// byte stream from a single read pass over the input file.
class HashSink {
public:
    virtual ~HashSink() = default;

    // Feeds one chunk; returns false and sets error on failure.
    virtual bool update(const void* data, std::size_t length, std::wstring& error) = 0;

    // Produces the final digest; returns false and sets error on failure.
    virtual bool finalize(std::vector<std::uint8_t>& digest, std::wstring& error) = 0;
};

// Creates the incremental sink for one algorithm; returns false with an error
// message when the algorithm cannot be initialized in this environment.
bool createHashSink(HashAlgorithm algorithm, std::unique_ptr<HashSink>& sink,
                    std::wstring& error);
