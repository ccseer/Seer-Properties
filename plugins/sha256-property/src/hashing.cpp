#include "hashing.h"

#include <windows.h>
#include <bcrypt.h>

#include "blake3.h"

// The vendored sha3.h is plain C without an extern "C" guard.
extern "C" {
#include "sha3.h"
}

// Single translation unit that emits the xxHash implementation; the static
// linking opt-in exposes the state struct used by the streaming API.
#define XXH_STATIC_LINKING_ONLY 1
#define XXH_IMPLEMENTATION 1
#include "xxhash.h"

#include <array>
#include <utility>

namespace {

void appendBigEndian(std::vector<std::uint8_t>& out, std::uint64_t value, int width)
{
    for (int shift = (width - 1) * 8; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
}

// SHA-256, SHA-384 and SHA-512 come from the Windows CNG
// provider, which supports incremental hashing on every Windows 10+ system.
class CngHashSink final : public HashSink {
public:
    CngHashSink(const wchar_t* algorithmId, std::size_t digestLength)
        : m_digestLength(digestLength)
    {
        NTSTATUS status = BCryptOpenAlgorithmProvider(&m_algorithm, algorithmId, nullptr, 0);
        if (status == 0) {
            DWORD objectLength = 0;
            DWORD bytesReturned = 0;
            status = BCryptGetProperty(m_algorithm, BCRYPT_OBJECT_LENGTH,
                                       reinterpret_cast<PUCHAR>(&objectLength),
                                       sizeof(objectLength), &bytesReturned, 0);
            if (status == 0) {
                m_object.resize(objectLength);
                status = BCryptCreateHash(m_algorithm, &m_hash, m_object.data(),
                                          objectLength, nullptr, 0, 0);
            }
        }
        m_failed = status != 0;
    }

    ~CngHashSink() override
    {
        if (m_hash != nullptr)
            BCryptDestroyHash(m_hash);
        if (m_algorithm != nullptr)
            BCryptCloseAlgorithmProvider(m_algorithm, 0);
    }

    // Handle-owning sink; instances only ever live behind unique_ptr.
    CngHashSink(const CngHashSink&) = delete;
    CngHashSink& operator=(const CngHashSink&) = delete;

    bool valid() const { return !m_failed; }

    bool update(const void* data, std::size_t length, std::wstring& error) override
    {
        const NTSTATUS status = BCryptHashData(
            m_hash, reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
            static_cast<ULONG>(length), 0);
        if (status != 0) {
            error = L"Windows CNG hashing failed";
            return false;
        }
        return true;
    }

    bool finalize(std::vector<std::uint8_t>& digest, std::wstring& error) override
    {
        digest.assign(m_digestLength, 0);
        const NTSTATUS status = BCryptFinishHash(m_hash, digest.data(),
                                                 static_cast<ULONG>(digest.size()), 0);
        if (status != 0) {
            error = L"Windows CNG finalization failed";
            return false;
        }
        return true;
    }

private:
    BCRYPT_ALG_HANDLE m_algorithm = nullptr;
    BCRYPT_HASH_HANDLE m_hash = nullptr;
    std::vector<UCHAR> m_object;
    std::size_t m_digestLength = 0;
    bool m_failed = false;
};

// Standard CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320), the variant
// used by zip/gzip and reported by common checksum tools.
class Crc32HashSink final : public HashSink {
public:
    bool update(const void* data, std::size_t length, std::wstring&) override
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        const std::uint32_t* table = crc32Table();
        std::uint32_t state = m_state;
        for (std::size_t index = 0; index < length; ++index)
            state = table[(state ^ bytes[index]) & 0xFFu] ^ (state >> 8);
        m_state = state;
        return true;
    }

    bool finalize(std::vector<std::uint8_t>& digest, std::wstring&) override
    {
        digest.clear();
        digest.reserve(4);
        appendBigEndian(digest, m_state ^ 0xFFFFFFFFu, 4);
        return true;
    }

private:
    static const std::uint32_t* crc32Table()
    {
        static const std::array<std::uint32_t, 256> table = [] {
            std::array<std::uint32_t, 256> generated{};
            for (std::uint32_t index = 0; index < 256; ++index) {
                std::uint32_t value = index;
                for (int bit = 0; bit < 8; ++bit)
                    value = (value & 1u) != 0 ? 0xEDB88320u ^ (value >> 1) : (value >> 1);
                generated[index] = value;
            }
            return generated;
        }();
        return table.data();
    }

    std::uint32_t m_state = 0xFFFFFFFFu;
};

// CRC-64/XZ (reflected ECMA-182 polynomial 0xC96C5795D7870F42 with all-ones
// init and final xor), the variant reported by the xz tools.
class Crc64HashSink final : public HashSink {
public:
    bool update(const void* data, std::size_t length, std::wstring&) override
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        const std::uint64_t* table = crc64Table();
        std::uint64_t state = m_state;
        for (std::size_t index = 0; index < length; ++index)
            state = table[(state ^ bytes[index]) & 0xFFu] ^ (state >> 8);
        m_state = state;
        return true;
    }

    bool finalize(std::vector<std::uint8_t>& digest, std::wstring&) override
    {
        digest.clear();
        digest.reserve(8);
        appendBigEndian(digest, m_state ^ 0xFFFFFFFFFFFFFFFFull, 8);
        return true;
    }

private:
    static const std::uint64_t* crc64Table()
    {
        static const std::array<std::uint64_t, 256> table = [] {
            std::array<std::uint64_t, 256> generated{};
            for (std::uint64_t index = 0; index < 256; ++index) {
                std::uint64_t value = index;
                for (int bit = 0; bit < 8; ++bit)
                    value = (value & 1u) != 0 ? 0xC96C5795D7870F42ull ^ (value >> 1)
                                              : (value >> 1);
                generated[index] = value;
            }
            return generated;
        }();
        return table.data();
    }

    std::uint64_t m_state = 0xFFFFFFFFFFFFFFFFull;
};

// xxHash, 64-bit variant (XXH64), the default of the xxhsum tool.
class XxHash64Sink final : public HashSink {
public:
    XxHash64Sink() : m_state(XXH64_createState())
    {
        m_failed = m_state == nullptr
                   || XXH64_reset(m_state, 0) != XXH_OK;
    }

    ~XxHash64Sink() override
    {
        if (m_state != nullptr)
            XXH64_freeState(m_state);
    }

    // Handle-owning sink; instances only ever live behind unique_ptr.
    XxHash64Sink(const XxHash64Sink&) = delete;
    XxHash64Sink& operator=(const XxHash64Sink&) = delete;

    bool valid() const { return !m_failed; }

    bool update(const void* data, std::size_t length, std::wstring& error) override
    {
        if (XXH64_update(m_state, data, length) != XXH_OK) {
            error = L"xxHash streaming failed";
            return false;
        }
        return true;
    }

    bool finalize(std::vector<std::uint8_t>& digest, std::wstring&) override
    {
        digest.clear();
        digest.reserve(8);
        appendBigEndian(digest, XXH64_digest(m_state), 8);
        return true;
    }

private:
    XXH64_state_t* m_state = nullptr;
    bool m_failed = false;
};

// BLAKE3, vendored official C reference implementation (portable code path).
class Blake3HashSink final : public HashSink {
public:
    Blake3HashSink()
    {
        blake3_hasher_init(&m_hasher);
    }

    bool update(const void* data, std::size_t length, std::wstring&) override
    {
        blake3_hasher_update(&m_hasher, data, length);
        return true;
    }

    bool finalize(std::vector<std::uint8_t>& digest, std::wstring&) override
    {
        digest.assign(BLAKE3_OUT_LEN, 0);
        blake3_hasher_finalize(&m_hasher, digest.data(), digest.size());
        return true;
    }

private:
    blake3_hasher m_hasher;
};

// SHA-3-256 (FIPS 202), vendored public-domain-style reference implementation.
class Sha3_256HashSink final : public HashSink {
public:
    Sha3_256HashSink()
    {
        sha3_init(&m_context, 32);
    }

    bool update(const void* data, std::size_t length, std::wstring&) override
    {
        sha3_update(&m_context, data, length);
        return true;
    }

    bool finalize(std::vector<std::uint8_t>& digest, std::wstring&) override
    {
        digest.assign(32, 0);
        sha3_final(digest.data(), &m_context);
        return true;
    }

private:
    sha3_ctx_t m_context;
};

bool openCngSink(const wchar_t* algorithmId, std::size_t digestLength,
                 const std::wstring& label, std::unique_ptr<HashSink>& sink,
                 std::wstring& error)
{
    auto created = std::make_unique<CngHashSink>(algorithmId, digestLength);
    if (!created->valid()) {
        error = L"Windows CNG initialization failed for " + label;
        return false;
    }
    sink = std::move(created);
    return true;
}

}

const std::vector<HashAlgorithm>& allHashAlgorithms()
{
    static const std::vector<HashAlgorithm> algorithms{
        HashAlgorithm::Sha256, HashAlgorithm::Crc32,  HashAlgorithm::Blake3,
        HashAlgorithm::Sha512, HashAlgorithm::XxHash, HashAlgorithm::Sha3_256,
        HashAlgorithm::Crc64,  HashAlgorithm::Sha384};
    return algorithms;
}

std::wstring hashAlgorithmLabel(HashAlgorithm algorithm)
{
    switch (algorithm) {
    case HashAlgorithm::Sha256:
        return L"SHA-256";
    case HashAlgorithm::Crc32:
        return L"CRC32";
    case HashAlgorithm::Blake3:
        return L"BLAKE3";
    case HashAlgorithm::Sha512:
        return L"SHA-512";
    case HashAlgorithm::XxHash:
        return L"xxHash";
    case HashAlgorithm::Sha3_256:
        return L"SHA-3-256";
    case HashAlgorithm::Crc64:
        return L"CRC64";
    case HashAlgorithm::Sha384:
        return L"SHA-384";
    }
    return L"unknown";
}

std::wstring lowerAscii(const std::wstring& text)
{
    std::wstring lowered;
    lowered.reserve(text.size());
    for (const wchar_t character : text) {
        lowered.push_back(character >= L'A' && character <= L'Z'
                              ? static_cast<wchar_t>(character - L'A' + L'a')
                              : character);
    }
    return lowered;
}

bool hashAlgorithmFromName(const std::wstring& name, HashAlgorithm& parsed)
{
    const std::wstring lowered = lowerAscii(name);
    static const std::array<std::pair<const wchar_t*, HashAlgorithm>, 8> names{{
        {L"sha256", HashAlgorithm::Sha256}, {L"crc32", HashAlgorithm::Crc32},
        {L"blake3", HashAlgorithm::Blake3}, {L"sha512", HashAlgorithm::Sha512},
        {L"xxhash", HashAlgorithm::XxHash}, {L"sha3-256", HashAlgorithm::Sha3_256},
        {L"crc64", HashAlgorithm::Crc64},   {L"sha384", HashAlgorithm::Sha384},
    }};
    for (const auto& entry : names) {
        if (lowered == entry.first) {
            parsed = entry.second;
            return true;
        }
    }
    return false;
}

bool createHashSink(HashAlgorithm algorithm, std::unique_ptr<HashSink>& sink,
                    std::wstring& error)
{
    switch (algorithm) {
    case HashAlgorithm::Sha256:
        return openCngSink(BCRYPT_SHA256_ALGORITHM, 32, hashAlgorithmLabel(algorithm),
                           sink, error);
    case HashAlgorithm::Sha512:
        return openCngSink(BCRYPT_SHA512_ALGORITHM, 64, hashAlgorithmLabel(algorithm),
                           sink, error);
    case HashAlgorithm::Sha384:
        return openCngSink(BCRYPT_SHA384_ALGORITHM, 48, hashAlgorithmLabel(algorithm),
                           sink, error);
    case HashAlgorithm::Crc32:
        sink = std::make_unique<Crc32HashSink>();
        return true;
    case HashAlgorithm::Crc64:
        sink = std::make_unique<Crc64HashSink>();
        return true;
    case HashAlgorithm::XxHash: {
        auto created = std::make_unique<XxHash64Sink>();
        if (!created->valid()) {
            error = L"xxHash initialization failed";
            return false;
        }
        sink = std::move(created);
        return true;
    }
    case HashAlgorithm::Blake3:
        sink = std::make_unique<Blake3HashSink>();
        return true;
    case HashAlgorithm::Sha3_256:
        sink = std::make_unique<Sha3_256HashSink>();
        return true;
    }
    error = L"Unknown hash algorithm";
    return false;
}
