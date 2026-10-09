#pragma once

#include <cstdint>
#include <string>

// Pure formatting helpers for the pdb-property helper. No Win32 and no raw_pdb
// dependency, so the unit tests exercise them without any fixture.
namespace pdbformat {

// Formats the info-stream GUID as the canonical braces form
// {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX} with uppercase hex digits.
std::string formatGuid(uint32_t data1, uint16_t data2, uint16_t data3,
                       const unsigned char *data4);

// Maps the DBI stream-header machine field to the common architecture names.
// Zero is reported as "unknown"; an unrecognized value keeps its raw hex form.
std::string formatMachine(uint16_t machine);

// Maps the info-stream version field to the toolset names the PDB format
// defines. An unrecognized version falls back to the raw number.
std::string formatFormatVersion(uint32_t version);

// Human-readable byte count, 1024-based with one decimal (B, KB, MB, GB, TB).
std::string formatBytes(uint64_t bytes);

// UTC timestamp text for the info-stream signature field. An empty string is
// returned for 0 because a zero timestamp carries no information.
std::string formatUnixTimeUtc(uint32_t seconds);

// Replaces bytes that do not form valid UTF-8 with '?' so the JSON serializer
// can accept strings read from a PDB. The PDB format stores names as raw
// bytes without promising any encoding, and the serializer rejects invalid
// UTF-8 instead of escaping it.
std::string sanitizeUtf8(const std::string &text);

}  // namespace pdbformat
