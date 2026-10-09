#include "pdbformat.h"

#include <cstdio>
#include <ctime>

namespace pdbformat {
namespace {

constexpr uint64_t kKib = 1024ull;
constexpr uint64_t kMib = 1024ull * kKib;
constexpr uint64_t kGib = 1024ull * kMib;
constexpr uint64_t kTib = 1024ull * kGib;

}  // namespace

std::string formatGuid(uint32_t data1, uint16_t data2, uint16_t data3,
                       const unsigned char *data4)
{
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer),
                  "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", data1,
                  data2, data3, data4[0], data4[1], data4[2], data4[3],
                  data4[4], data4[5], data4[6], data4[7]);
    return buffer;
}

std::string formatMachine(uint16_t machine)
{
    switch (machine) {
        case 0x014C:
            return "x86";
        case 0x01C0:
            return "ARM";
        case 0x01C4:
            return "ARM (Thumb)";
        case 0x8664:
            return "x64";
        case 0xAA64:
            return "ARM64";
        default:
            break;
    }
    if (machine == 0) {
        return "unknown";
    }
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "0x%04X", machine);
    return buffer;
}

std::string formatFormatVersion(uint32_t version)
{
    switch (version) {
        case 19941610:
            return "VC2";
        case 19950623:
            return "VC4";
        case 19950814:
            return "VC4.1";
        case 19960307:
            return "VC5.0";
        case 19970604:
            return "VC98";
        case 19990604:
            return "VC70Dep";
        case 20000404:
            return "VC70";
        case 20030901:
            return "VC80";
        case 20091201:
            return "VC110";
        case 20140508:
            return "VC140";
        default:
            return std::to_string(version);
    }
}

std::string formatBytes(uint64_t bytes)
{
    char buffer[32];
    if (bytes >= kTib) {
        std::snprintf(buffer, sizeof(buffer), "%.1f TB",
                      static_cast<double>(bytes) / static_cast<double>(kTib));
    }
    else if (bytes >= kGib) {
        std::snprintf(buffer, sizeof(buffer), "%.1f GB",
                      static_cast<double>(bytes) / static_cast<double>(kGib));
    }
    else if (bytes >= kMib) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                      static_cast<double>(bytes) / static_cast<double>(kMib));
    }
    else if (bytes >= kKib) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB",
                      static_cast<double>(bytes) / static_cast<double>(kKib));
    }
    else {
        std::snprintf(buffer, sizeof(buffer), "%llu B",
                      static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

std::string formatUnixTimeUtc(uint32_t seconds)
{
    if (seconds == 0) {
        return {};
    }
    const std::time_t time = static_cast<std::time_t>(seconds);
    std::tm utc {};
    if (gmtime_s(&utc, &time) != 0) {
        return {};
    }
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &utc)
        == 0) {
        return {};
    }
    return buffer;
}

std::string sanitizeUtf8(const std::string &text)
{
    std::string out;
    out.reserve(text.size());

    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead
            = static_cast<unsigned char>(text[index]);
        if (lead < 0x80) {
            out += text[index];
            ++index;
            continue;
        }

        // RFC 3629 sequence shapes. The leading-byte bounds already exclude
        // overlong and out-of-Unicode encodings, so only the continuation
        // ranges need checking here.
        std::size_t length = 0;
        unsigned char firstLow = 0x80;
        unsigned char firstHigh = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        }
        else if (lead == 0xE0) {
            length = 3;
            firstLow = 0xA0;
        }
        else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE
                 || lead == 0xEF) {
            length = 3;
        }
        else if (lead == 0xED) {
            length = 3;
            firstHigh = 0x9F;
        }
        else if (lead == 0xF0) {
            length = 4;
            firstLow = 0x90;
        }
        else if (lead >= 0xF1 && lead <= 0xF3) {
            length = 4;
        }
        else if (lead == 0xF4) {
            length = 4;
            firstHigh = 0x8F;
        }

        bool valid = length != 0 && index + length <= text.size();
        for (std::size_t offset = 1; valid && offset < length; ++offset) {
            const unsigned char continuation = static_cast<unsigned char>(
                text[index + offset]);
            const unsigned char low = offset == 1 ? firstLow : 0x80;
            const unsigned char high = offset == 1 ? firstHigh : 0xBF;
            valid = continuation >= low && continuation <= high;
        }

        if (valid) {
            out.append(text, index, length);
            index += length;
        }
        else {
            out += '?';
            ++index;
        }
    }
    return out;
}

}  // namespace pdbformat
