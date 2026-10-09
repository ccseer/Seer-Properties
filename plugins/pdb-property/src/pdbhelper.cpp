#include "pdbhelper.h"

#include "propertycommon.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace {

using json = nlohmann::json;

// Ordered rows for the "PDB" subgroup. Values are flat strings: the host
// drops anything else inside a subgroup value. An empty field means the
// information was not available; the row is omitted instead of guessing.
json buildPdbGroup(const PdbQueryResult &result)
{
    json value = json::array();

    auto put = [&value](const char *key, const std::string &text) {
        if (!text.empty()) {
            value.push_back(json{{key, text}});
        }
    };

    put("Status", result.status);
    put("Reason", result.reason);
    put("Link Type", result.linkType);
    put("Note", result.note);
    put("File Size", result.fileSize);
    put("PDB Format", result.formatVersion);
    put("Built", result.built);
    put("Age", result.age);
    put("GUID", result.guid);
    put("Machine", result.machine);
    put("Streams", result.streamCount);
    put("Modules", result.modules);
    put("Public Symbols", result.publicSymbols);
    put("Global Symbols", result.globalSymbols);
    put("Types (TPI)", result.typeRecords);
    put("Inlinees (IPI)", result.inlineRecords);
    put("Source Files", result.sourceFiles);
    put("Contributions", result.contributions);
    put("Line Info", result.lineInfo);
    for (std::size_t i = 0; i < result.topContributions.size(); ++i) {
        const PdbContribution &row = result.topContributions[i];
        const std::string key = "Largest Object " + std::to_string(i + 1);
        put(key.c_str(), row.name + " (" + row.sizeText + ")");
    }

    return json{{"value", value}};
}

// Digits only, no sign and no whitespace: anything else is an invocation
// error rather than a silently truncated number.
bool parseUnsigned(const std::wstring &text, uint64_t limit, uint32_t *value)
{
    if (text.empty()) {
        return false;
    }
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            return false;
        }
    }
    uint64_t parsed = 0;
    for (const wchar_t character : text) {
        parsed = parsed * 10 + static_cast<uint64_t>(character - L'0');
        if (parsed > limit) {
            return false;
        }
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
}

// The zero-deadline test hook shared with the other property packages: the
// budget is already exhausted, so the query is abandoned without running and
// the result says so instead of implying success.
PdbQueryResult abandonedQuery()
{
    PdbQueryResult result;
    result.status = "QueryError";
    result.reason
        = "the query was abandoned at the first checkpoint because the "
          "internal deadline (0 ms) budget is already exhausted";
    return result;
}

}  // namespace

int executePdbProperty(const std::wstring &inputPath,
                       const std::wstring &outputBasePath,
                       const std::wstring &outputDirPath,
                       const PdbQueryOptions &options, PdbQueryFn queryFn)
{
    if (!propertycommon::isExistingDirectory(outputDirPath)) {
        return 1;
    }

    const std::wstring outputJsonPath
        = propertycommon::withJsonSuffix(outputBasePath);
    if (!propertycommon::isContainedInDirectory(outputJsonPath,
                                                outputDirPath)) {
        return 1;
    }

    PdbQueryResult result;
    if (options.hasDeadline && options.deadlineMs == 0) {
        result = abandonedQuery();
    }
    else {
        result = queryFn ? queryFn(inputPath, options)
                         : queryPdbFile(inputPath, options);
    }

    json data    = json::object();
    data["PDB"]  = buildPdbGroup(result);

    json root;
    root["result_schema"] = 1;
    root["data"]          = data;

    return propertycommon::writeJson(outputJsonPath, root) ? 0 : 1;
}

int runPdbProperty(const std::vector<std::wstring> &arguments,
                   PdbQueryFn queryFn)
{
    std::wstring input;
    std::wstring output;
    std::wstring outputDir;
    std::wstring topText;
    std::wstring deadlineText;
    bool inputSeen     = false;
    bool outputSeen    = false;
    bool outputDirSeen = false;
    bool topSeen       = false;
    bool deadlineSeen  = false;

    // Every option consumes the next token, so a trailing option without a
    // value is an invocation error instead of being ignored silently. The
    // same rules as the other property packages: a repeated option, an
    // unknown option, an empty value or a missing value is rejected with a
    // nonzero exit code and no output.
    for (std::size_t i = 1; i < arguments.size();) {
        if (i + 1 >= arguments.size()) {
            return 1;
        }
        const std::wstring &option = arguments[i];
        const std::wstring &value  = arguments[i + 1];
        if (value.empty()) {
            return 1;
        }
        if (option == L"--input" && !inputSeen) {
            input     = value;
            inputSeen = true;
        }
        else if (option == L"--output" && !outputSeen) {
            output     = value;
            outputSeen = true;
        }
        else if (option == L"--output-dir" && !outputDirSeen) {
            outputDir     = value;
            outputDirSeen = true;
        }
        else if (option == L"--top" && !topSeen) {
            topText = value;
            topSeen = true;
        }
        else if (option == L"--deadline" && !deadlineSeen) {
            deadlineText = value;
            deadlineSeen = true;
        }
        else {
            return 1;
        }
        i += 2;
    }

    if (!inputSeen || !outputSeen || !outputDirSeen) {
        return 1;
    }

    PdbQueryOptions options;
    if (topSeen) {
        // Accepted values are 1..100; anything else is an invocation error.
        uint32_t top = 0;
        if (!parseUnsigned(topText, 100, &top) || top == 0) {
            return 1;
        }
        options.wantTop = true;
        options.topN    = top;
    }
    if (deadlineSeen) {
        uint32_t deadline = 0;
        if (!parseUnsigned(deadlineText, 0xFFFFFFFFull, &deadline)) {
            return 1;
        }
        options.hasDeadline = true;
        options.deadlineMs  = deadline;
    }

    return executePdbProperty(input, output, outputDir, options, queryFn);
}
