// Unit tests for the pure formatting helpers. No fixture and no PDB parsing
// is involved; every expectation is a literal the implementation must match.
#include <windows.h>

#include <cstdio>
#include <string>

#include "pdbformat.h"

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

void checkEq(const std::string &got, const std::string &expected,
             const char *message)
{
    if (got != expected) {
        std::printf("FAIL: %s (got '%s', expected '%s')\n", message,
                    got.c_str(), expected.c_str());
        ++failures;
    }
}

}  // namespace

int main()
{
    // The GUID keeps its internal byte order and uses uppercase hex digits.
    {
        const unsigned char data4[8] = {0x01, 0x23, 0x45, 0x67,
                                        0x89, 0xAB, 0xCD, 0xEF};
        checkEq(pdbformat::formatGuid(0x12345678u, 0x9ABC, 0xDEF0, data4),
                "{12345678-9ABC-DEF0-0123-456789ABCDEF}", "formatGuid braces");
    }
    {
        const unsigned char data4[8] = {};
        checkEq(pdbformat::formatGuid(0, 0, 0, data4),
                "{00000000-0000-0000-0000-000000000000}", "formatGuid zeroes");
    }

    checkEq(pdbformat::formatMachine(0x014C), "x86", "machine i386");
    checkEq(pdbformat::formatMachine(0x01C0), "ARM", "machine ARM");
    checkEq(pdbformat::formatMachine(0x01C4), "ARM (Thumb)",
            "machine ARM Thumb");
    checkEq(pdbformat::formatMachine(0x8664), "x64", "machine x64");
    checkEq(pdbformat::formatMachine(0xAA64), "ARM64", "machine ARM64");
    checkEq(pdbformat::formatMachine(0), "unknown", "machine zero");
    checkEq(pdbformat::formatMachine(0x1234), "0x1234",
            "unknown machine keeps raw hex");

    checkEq(pdbformat::formatFormatVersion(19941610), "VC2", "version VC2");
    checkEq(pdbformat::formatFormatVersion(19950623), "VC4", "version VC4");
    checkEq(pdbformat::formatFormatVersion(19950814), "VC4.1",
            "version VC4.1");
    checkEq(pdbformat::formatFormatVersion(19960307), "VC5.0",
            "version VC5.0");
    checkEq(pdbformat::formatFormatVersion(19970604), "VC98", "version VC98");
    checkEq(pdbformat::formatFormatVersion(19990604), "VC70Dep",
            "version VC70Dep");
    checkEq(pdbformat::formatFormatVersion(20000404), "VC70", "version VC70");
    checkEq(pdbformat::formatFormatVersion(20030901), "VC80", "version VC80");
    checkEq(pdbformat::formatFormatVersion(20091201), "VC110",
            "version VC110");
    checkEq(pdbformat::formatFormatVersion(20140508), "VC140",
            "version VC140");
    checkEq(pdbformat::formatFormatVersion(19999999), "19999999",
            "unknown version keeps raw number");

    checkEq(pdbformat::formatBytes(0), "0 B", "bytes zero");
    checkEq(pdbformat::formatBytes(512), "512 B", "bytes below one KiB");
    checkEq(pdbformat::formatBytes(1024), "1.0 KB", "bytes one KiB");
    checkEq(pdbformat::formatBytes(102400), "100.0 KB", "bytes one hundred KiB");
    checkEq(pdbformat::formatBytes(1536), "1.5 KB", "bytes fractional KiB");
    checkEq(pdbformat::formatBytes(1048576), "1.0 MB", "bytes one MiB");
    checkEq(pdbformat::formatBytes(1073741824), "1.0 GB", "bytes one GiB");
    checkEq(pdbformat::formatBytes(1099511627776ull), "1.0 TB", "bytes one TiB");

    // A zero timestamp carries no information and stays empty.
    checkEq(pdbformat::formatUnixTimeUtc(0), "", "timestamp zero is empty");
    checkEq(pdbformat::formatUnixTimeUtc(1), "1970-01-01 00:00:01 UTC",
            "timestamp epoch");
    checkEq(pdbformat::formatUnixTimeUtc(0x80000000u),
            "2038-01-19 03:14:08 UTC", "timestamp beyond 32-bit signed");

    // Valid UTF-8 passes through byte for byte, including multibyte shapes.
    checkEq(pdbformat::sanitizeUtf8(""), "", "sanitize empty");
    checkEq(pdbformat::sanitizeUtf8("plain ascii"), "plain ascii",
            "sanitize ascii");
    checkEq(pdbformat::sanitizeUtf8("a\xc3\xa9z"), "a\xc3\xa9z",
            "sanitize two-byte sequence");
    checkEq(pdbformat::sanitizeUtf8("a\xe2\x82\xacz"), "a\xe2\x82\xacz",
            "sanitize three-byte sequence");
    checkEq(pdbformat::sanitizeUtf8("a\xf0\x9f\x92\xa9z"), "a\xf0\x9f\x92\xa9z",
            "sanitize four-byte sequence");

    // Invalid bytes are replaced individually so the rest survives.
    checkEq(pdbformat::sanitizeUtf8("\xff"), "?", "sanitize lone 0xFF");
    checkEq(pdbformat::sanitizeUtf8("a\xc3zb"), "a?zb",
            "sanitize truncated sequence");
    checkEq(pdbformat::sanitizeUtf8("\xc0\x80"), "??",
            "sanitize overlong two-byte form");
    checkEq(pdbformat::sanitizeUtf8("\xe0\x80\x80"), "???",
            "sanitize overlong three-byte form");
    checkEq(pdbformat::sanitizeUtf8("\xf4\x90\x80\x80"), "????",
            "sanitize out-of-Unicode sequence");
    checkEq(pdbformat::sanitizeUtf8("\xed\xa0\x80"), "???",
            "sanitize surrogate half sequence");
    checkEq(pdbformat::sanitizeUtf8("\xe2\x82"), "??",
            "sanitize sequence cut at end of buffer");

    if (failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    return 1;
}
