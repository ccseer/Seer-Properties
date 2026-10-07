#include <windows.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct JsonValue {
    enum class Kind { String, Array, Object, Number, Boolean, Null };

    Kind kind;
    std::string stringValue;
    std::vector<JsonValue> arrayValue;
    std::map<std::string, JsonValue> objectValue;
};

class JsonParser {
public:
    explicit JsonParser(std::string input) : m_input(std::move(input)) {}

    JsonValue parse()
    {
        auto result = parseValue();
        skipWhitespace();
        if (m_position != m_input.size())
            throw std::runtime_error("unexpected trailing JSON input");
        return result;
    }

private:
    JsonValue parseValue()
    {
        skipWhitespace();
        if (m_position == m_input.size())
            throw std::runtime_error("unexpected end of JSON input");
        switch (m_input[m_position]) {
        case '"':
            return {JsonValue::Kind::String, parseString(), {}, {}};
        case '[':
            return parseArray();
        case '{':
            return parseObject();
        case 't':
            expectLiteral("true");
            return {JsonValue::Kind::Boolean, {}, {}, {}};
        case 'f':
            expectLiteral("false");
            return {JsonValue::Kind::Boolean, {}, {}, {}};
        case 'n':
            expectLiteral("null");
            return {JsonValue::Kind::Null, {}, {}, {}};
        default:
            return parseNumber();
        }
    }

    JsonValue parseArray()
    {
        expect('[');
        JsonValue result{JsonValue::Kind::Array, {}, {}, {}};
        skipWhitespace();
        if (consume(']'))
            return result;
        while (true) {
            result.arrayValue.push_back(parseValue());
            skipWhitespace();
            if (consume(']'))
                return result;
            expect(',');
        }
    }

    JsonValue parseObject()
    {
        expect('{');
        JsonValue result{JsonValue::Kind::Object, {}, {}, {}};
        skipWhitespace();
        if (consume('}'))
            return result;
        while (true) {
            skipWhitespace();
            if (m_position == m_input.size() || m_input[m_position] != '"')
                throw std::runtime_error("expected object key");
            const auto key = parseString();
            expect(':');
            const auto value = parseValue();
            if (!result.objectValue.emplace(key, value).second)
                throw std::runtime_error("duplicate JSON object key");
            skipWhitespace();
            if (consume('}'))
                return result;
            expect(',');
        }
    }

    JsonValue parseNumber()
    {
        const auto begin = m_position;
        while (m_position < m_input.size()
               && (std::isdigit(static_cast<unsigned char>(m_input[m_position]))
                   || m_input[m_position] == '-' || m_input[m_position] == '+'))
            ++m_position;
        if (begin == m_position)
            throw std::runtime_error("invalid JSON value");
        return {JsonValue::Kind::Number,
                m_input.substr(begin, m_position - begin),
                {},
                {}};
    }

    std::string parseString()
    {
        expect('"');
        std::string result;
        while (m_position < m_input.size()) {
            const auto character = m_input[m_position++];
            if (character == '"')
                return result;
            if (character == '\\') {
                if (m_position == m_input.size())
                    throw std::runtime_error("incomplete JSON escape");
                const auto escaped = m_input[m_position++];
                switch (escaped) {
                case '"':
                    result += '"';
                    break;
                case '\\':
                    result += '\\';
                    break;
                case '/':
                    result += '/';
                    break;
                case 'b':
                    result += '\b';
                    break;
                case 'f':
                    result += '\f';
                    break;
                case 'n':
                    result += '\n';
                    break;
                case 'r':
                    result += '\r';
                    break;
                case 't':
                    result += '\t';
                    break;
                default:
                    throw std::runtime_error("unsupported JSON escape");
                }
            }
            else {
                result += character;
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }

    void expect(char expected)
    {
        skipWhitespace();
        if (m_position == m_input.size() || m_input[m_position] != expected)
            throw std::runtime_error("unexpected JSON character");
        ++m_position;
    }

    void expectLiteral(const char *expected)
    {
        while (*expected != '\0') {
            if (m_position == m_input.size()
                || m_input[m_position] != *expected)
                throw std::runtime_error("unexpected JSON literal");
            ++m_position;
            ++expected;
        }
    }

    bool consume(char expected)
    {
        if (m_position < m_input.size() && m_input[m_position] == expected) {
            ++m_position;
            return true;
        }
        return false;
    }

    void skipWhitespace()
    {
        while (m_position < m_input.size()
               && std::isspace(static_cast<unsigned char>(m_input[m_position])))
            ++m_position;
    }

    std::string m_input;
    size_t m_position = 0;
};

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

const JsonValue *member(const JsonValue &object, const char *name)
{
    if (object.kind != JsonValue::Kind::Object)
        return nullptr;
    const auto found = object.objectValue.find(name);
    return found == object.objectValue.end() ? nullptr : &found->second;
}

bool exactStringArray(const JsonValue *value,
                      const std::vector<std::string> &expected)
{
    if (!value || value->kind != JsonValue::Kind::Array
        || value->arrayValue.size() != expected.size())
        return false;
    for (size_t index = 0; index < expected.size(); ++index) {
        if (value->arrayValue[index].kind != JsonValue::Kind::String
            || value->arrayValue[index].stringValue != expected[index])
            return false;
    }
    return true;
}

bool exactNumberArray(const JsonValue *value,
                      const std::vector<std::string> &expected)
{
    if (!value || value->kind != JsonValue::Kind::Array
        || value->arrayValue.size() != expected.size())
        return false;
    for (size_t index = 0; index < expected.size(); ++index) {
        if (value->arrayValue[index].kind != JsonValue::Kind::Number
            || value->arrayValue[index].stringValue != expected[index])
            return false;
    }
    return true;
}

bool rejectsDuplicateKeys()
{
    try {
        JsonParser(R"({"backend":"process","backend":42})").parse();
    }
    catch (const std::exception &) {
        return true;
    }
    return false;
}

bool isWithin(const std::filesystem::path &root,
              const std::filesystem::path &path)
{
    auto rootPart = root.begin();
    auto pathPart = path.begin();
    while (rootPart != root.end()) {
        if (pathPart == path.end() || *rootPart != *pathPart)
            return false;
        ++rootPart;
        ++pathPart;
    }
    return true;
}

std::filesystem::path tempDirectory()
{
    wchar_t buffer[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, buffer) == 0)
        throw std::runtime_error("unable to resolve temporary directory");
    wchar_t uniquePath[MAX_PATH]{};
    if (GetTempFileNameW(buffer, L"sha", 0, uniquePath) == 0)
        throw std::runtime_error("unable to create unique temporary path");
    DeleteFileW(uniquePath);
    const auto path = std::filesystem::path(uniquePath);
    if (!std::filesystem::create_directory(path))
        throw std::runtime_error("unable to create unique temporary directory");
    return path;
}

bool runHelper(const std::filesystem::path &executable,
               const std::filesystem::path &input,
               const std::filesystem::path &outputBase,
               const std::wstring &outputCase = {},
               const std::wstring &algorithms = {})
{
    std::wstring command = L"\"" + executable.wstring() + L"\" --input \""
                           + input.wstring() + L"\" --output \""
                           + outputBase.wstring() + L"\"";
    if (!outputCase.empty())
        command += L" --case " + outputCase;
    if (!algorithms.empty())
        command += L" --algorithms " + algorithms;
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const auto created = CreateProcessW(
        nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, executable.parent_path().c_str(), &startup, &process);
    if (!created)
        return false;
    WaitForSingleObject(process.hProcess, 30000);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exitCode == 0;
}

std::string readFile(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
}  // namespace

int main(int argc, char *argv[])
{
    if (argc != 2) {
        std::cerr << "usage: sha256_property_manifest_test <staging-root>\n";
        return 2;
    }

    try {
        check(rejectsDuplicateKeys(),
              "JSON parser rejects duplicate object keys");
        const auto stagingRoot    = std::filesystem::weakly_canonical(argv[1]);
        const auto manifestPath   = stagingRoot / "plugin.json";
        const auto executablePath = stagingRoot / "sha256_property.exe";
        check(std::filesystem::is_regular_file(manifestPath),
              "staged plugin.json exists");
        check(std::filesystem::is_regular_file(executablePath),
              "staged helper exists");
        const auto readmePath   = stagingRoot / "README.md";
        const bool readmePresent
            = std::filesystem::is_regular_file(readmePath);
        check(readmePresent, "staged README.md exists");
        if (readmePresent) {
            std::ifstream readmeFile(readmePath, std::ios::binary);
            const std::string readmeText(
                (std::istreambuf_iterator<char>(readmeFile)), {});
            check(!readmeText.empty(), "staged README.md is non-empty");
        }
        check(isWithin(stagingRoot,
                       std::filesystem::weakly_canonical(executablePath)),
              "staged helper remains inside package");

        std::ifstream manifestFile(manifestPath, std::ios::binary);
        if (!manifestFile)
            throw std::runtime_error("unable to read staged plugin.json");
        const auto root
            = JsonParser({std::istreambuf_iterator<char>(manifestFile), {}})
                  .parse();
        check(member(root, "schema_version")
                  && member(root, "schema_version")->kind
                         == JsonValue::Kind::Number
                  && member(root, "schema_version")->stringValue == "1",
              "schema version is 1");
        check(member(root, "id")
                  && member(root, "id")->kind == JsonValue::Kind::String
                  && member(root, "id")->stringValue
                         == "io.1218.seer.sha256-property",
              "manifest id is fixed");
        check(member(root, "name")
                  && member(root, "name")->kind == JsonValue::Kind::String
                  && member(root, "name")->stringValue == "File Hashes",
              "manifest name is File Hashes");
        check(member(root, "version")
                  && member(root, "version")->kind == JsonValue::Kind::String
                  && member(root, "version")->stringValue == "1.1.0",
              "manifest version is 1.1.0");
        check(member(root, "appMinVersion")
                  && member(root, "appMinVersion")->kind
                         == JsonValue::Kind::String
                  && member(root, "appMinVersion")->stringValue == "4.5.10",
              "minimum Seer version is 4.5.10");
        check(member(root, "backend")
                  && member(root, "backend")->stringValue == "process",
              "backend is process");
        check(exactStringArray(member(root, "capabilities"), {"property"}),
              "capabilities contains only property");
        check(exactStringArray(member(root, "extensions"), {"${type_file}"}),
              "extensions contains the file type token");

        const auto invocations = member(root, "invocations");
        check(invocations && invocations->kind == JsonValue::Kind::Object,
              "invocations object is present");
        const auto property = member(*invocations, "property");
        check(property && property->kind == JsonValue::Kind::Object,
              "property invocation is present");
        const auto command = member(*property, "command");
        check(command && command->kind == JsonValue::Kind::String
                  && command->stringValue == "sha256_property.exe",
              "command names sha256_property.exe");
        if (command && command->kind == JsonValue::Kind::String) {
            const std::filesystem::path commandPath(command->stringValue);
            check(!commandPath.is_absolute() && !commandPath.has_parent_path(),
                  "command is package-relative");
        }
        check(exactStringArray(
                  member(*property, "arguments"),
                  {"--input", "${input_file}", "--output", "${output_file}"}),
              "arguments use input and output base tokens");
        check(member(*property, "result_schema")
                  && member(*property, "result_schema")->kind
                         == JsonValue::Kind::Number
                  && member(*property, "result_schema")->stringValue == "1",
              "result schema is 1");
        check(member(*property, "timeout_ms")
                  && member(*property, "timeout_ms")->kind == JsonValue::Kind::Number
                  && member(*property, "timeout_ms")->stringValue == "120000",
              "timeout is 120000 milliseconds");
        check(exactNumberArray(member(*property, "success_exit_codes"), {"0"}),
              "success exit codes contains only zero");

        const auto directory  = tempDirectory();
        const auto input      = directory / L"manifest input.txt";
        const auto outputBase = directory / L"base";
        std::ofstream(input, std::ios::binary) << "abc";
        const auto outputPath = outputBase.wstring() + L".json";
        check(!std::filesystem::exists(outputPath),
              "output is absent before helper run");
        check(runHelper(executablePath, input, outputBase),
              "staged helper runs successfully");
        check(std::filesystem::is_regular_file(outputPath),
              "helper produces <base-path>.json");
        check(readFile(outputPath)
                  == "{\"result_schema\":1,\"data\":{\"SHA-256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}}\n",
              "helper output is the schema-1 envelope with one flat row");
        for (const auto outputCase : {L"lower", L"upper"}) {
            check(runHelper(executablePath, input, outputBase, outputCase),
                  "packaged helper accepts the case option");
            const auto expected = std::wstring(outputCase) == L"upper"
                ? "{\"result_schema\":1,\"data\":{\"SHA-256\":\"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD\"}}\n"
                : "{\"result_schema\":1,\"data\":{\"SHA-256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}}\n";
            check(readFile(outputPath) == expected,
                  "packaged helper produces the requested case");
        }
        const auto groupOutputBase = directory / L"grouped";
        check(runHelper(executablePath, input, groupOutputBase, {},
                        L"blake3,crc32"),
              "packaged helper accepts the algorithms option");
        check(readFile(groupOutputBase.wstring() + L".json")
                  == "{\"result_schema\":1,\"data\":{\"Hashes\":{\"value\":[{\"CRC32\":\"352441c2\"},{\"BLAKE3\":\"6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85\"}]}}}\n",
              "several algorithms are grouped into one ordered subgroup");
        std::filesystem::remove_all(directory);
    }
    catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
