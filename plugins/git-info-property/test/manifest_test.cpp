#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
using json = nlohmann::json;

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

void checkEqStr(const std::string &got, const std::string &expected,
                const char *message)
{
    if (got != expected) {
        std::printf("FAIL: %s (got '%s', expected '%s')\n", message,
                    got.c_str(), expected.c_str());
        ++failures;
    }
}

std::string readUtf8(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream stream;
    stream << in.rdbuf();
    return stream.str();
}

std::string readAll(const std::wstring &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream stream;
    stream << in.rdbuf();
    return stream.str();
}

bool pathExists(const std::wstring &path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring widen(const std::string &text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr,
                                         0);
    std::wstring wide(static_cast<std::size_t>(size > 0 ? size - 1 : 0), L'\0');
    if (size > 1) {
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wide[0], size);
    }
    return wide;
}

const json *member(const json &value, const char *key)
{
    if (!value.is_object()) {
        return nullptr;
    }
    const auto it = value.find(key);
    return it == value.end() ? nullptr : &(*it);
}

std::string str(const json &value, const char *key)
{
    const json *found = member(value, key);
    return (found && found->is_string()) ? found->get<std::string>()
                                         : std::string();
}

long long num(const json &value, const char *key)
{
    const json *found = member(value, key);
    return (found && found->is_number()) ? found->get<long long>() : -1;
}

bool runProcess(const std::wstring &executable,
                const std::vector<std::wstring> &arguments, DWORD *exitCode)
{
    std::wstring command = L"\"" + executable + L"\"";
    for (const auto &argument : arguments) {
        command += L" \"" + argument + L"\"";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process)) {
        return false;
    }
    WaitForSingleObject(process.hProcess, 30000);
    GetExitCodeProcess(process.hProcess, exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return true;
}

}  // namespace

int main(int argc, char *argv[])
{
    std::string packageRoot = ".";
    if (argc >= 2) {
        packageRoot = argv[1];
    }

    const std::string manifestPath = packageRoot + "/plugin.json";
    check(pathExists(widen(manifestPath)), "plugin.json exists");

    const json manifest = json::parse(readUtf8(manifestPath), nullptr, false);
    check(!manifest.is_discarded(), "plugin.json parses as JSON");
    if (manifest.is_discarded()) {
        return 1;
    }

    // The host requires schema_version 1 and a valid three-segment version.
    check(num(manifest, "schema_version") == 1, "schema_version is 1");
    checkEqStr(str(manifest, "id"), "io.1218.seer.git-info",
               "id is io.1218.seer.git-info");
    check(str(manifest, "name") == "Git Info", "name is Git Info");
    check(str(manifest, "backend") == "process", "backend is process");
    check(str(manifest, "version") == "1.0.0", "version is 1.0.0");
    check(str(manifest, "appMinVersion") == "4.5.10", "appMinVersion is 4.5.10");

    const json *capabilities = member(manifest, "capabilities");
    check(capabilities && capabilities->is_array() && capabilities->size() == 1
              && capabilities->front().get<std::string>() == "property",
          "capabilities contains only property");

    const json *extensions = member(manifest, "extensions");
    check(extensions && extensions->is_array() && extensions->size() == 1
              && extensions->front().get<std::string>() == "${type_folder}",
          "extensions is ${type_folder}");

    const json *invocations = member(manifest, "invocations");
    const json *invocation
        = invocations ? member(*invocations, "property") : nullptr;
    check(invocation != nullptr, "invocations declares the property capability");
    if (!invocation) {
        return 1;
    }

    // The helper must be the packaged wrapper, never git.exe: a missing Git
    // installation has to stay reportable instead of invalidating the package.
    const std::string command = str(*invocation, "command");
    checkEqStr(command, "git_info.exe", "command is git_info.exe (not git.exe)");
    check(command.find('/') == std::string::npos
              && command.find('\\') == std::string::npos,
          "command is package-relative");
    check(num(*invocation, "result_schema") == 1, "result_schema is 1");
    check(num(*invocation, "timeout_ms") == 30000, "timeout_ms is 30000");

    const json *exitCodes = member(*invocation, "success_exit_codes");
    check(exitCodes && exitCodes->is_array() && exitCodes->size() == 1
              && exitCodes->front().get<long long>() == 0,
          "success_exit_codes contains 0");

    const std::vector<std::string> expectedArguments{"--input", "${input_file}",
                                                     "--output", "${output_file}",
                                                     "--output-dir",
                                                     "${output_dir}"};
    const json *arguments = member(*invocation, "arguments");
    check(arguments != nullptr && arguments->is_array(), "arguments is an array");
    if (arguments && arguments->size() == expectedArguments.size()) {
        for (std::size_t i = 0; i < expectedArguments.size(); ++i) {
            checkEqStr((*arguments)[i].get<std::string>(), expectedArguments[i],
                       "argument token matches");
        }
    }
    else {
        check(false, "arguments use input_file/output_file/output_dir");
    }

    const std::wstring executablePath
        = widen(packageRoot) + L"\\git_info.exe";
    check(pathExists(executablePath), "staged helper exists in the package root");

    if (pathExists(executablePath)) {
        // Run the staged helper on a real folder. Git may be present or absent;
        // either way the helper must publish valid schema-1 JSON with the Git
        // subgroup and exit 0.
        wchar_t buffer[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, buffer);
        const std::wstring directory = std::wstring(buffer) + L"git_manifest_6b";
        CreateDirectoryW(directory.c_str(), nullptr);
        const std::wstring outputBase = directory + L"\\out";

        DWORD exitCode = 999;
        const bool started = runProcess(
            executablePath,
            {L"--input", directory, L"--output", outputBase, L"--output-dir",
             directory},
            &exitCode);
        check(started && exitCode == 0, "staged helper runs and exits 0");

        const std::wstring jsonPath = outputBase + L".json";
        check(pathExists(jsonPath), "out.json is created");
        if (pathExists(jsonPath)) {
            const json document
                = json::parse(readAll(jsonPath), nullptr, false);
            check(!document.is_discarded(),
                  "staged helper output parses as JSON");
            check(num(document, "result_schema") == 1,
                  "staged helper output has result_schema 1");
            const json *data = member(document, "data");
            const json *git = data ? member(*data, "Git") : nullptr;
            check(git != nullptr, "staged helper output has the Git subgroup");
            // The subgroup value is an array of one-key fields in the plugin's
            // own order; merge them so the assertions can look fields up by key.
            json merged = json::object();
            const json *rows = git ? member(*git, "value") : nullptr;
            if (rows != nullptr && rows->is_array()) {
                for (const auto &item : *rows) {
                    if (!item.is_object()) {
                        continue;
                    }
                    for (auto field = item.begin(); field != item.end();
                         ++field) {
                        merged[field.key()] = field.value();
                    }
                }
            }
            {
                bool allFlat = true;
                for (const auto &row : merged.items()) {
                    const json &value = row.value();
                    if (!(value.is_string() || value.is_number()
                          || value.is_boolean() || value.is_array())) {
                        allFlat = false;
                        std::printf("FAIL: subgroup row '%s' is not flat\n",
                                    row.key().c_str());
                    }
                }
                check(allFlat, "every subgroup row is flat");
                check(member(merged, "Git executable") == nullptr
                          && member(merged, "Git version") == nullptr,
                      "the executable path and version are never reported");
            }
        }

        WIN32_FIND_DATAW data;
        const HANDLE handle = FindFirstFileW((directory + L"\\*").c_str(), &data);
        if (handle != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = data.cFileName;
                if (name != L"." && name != L"..") {
                    DeleteFileW((directory + L"\\" + name).c_str());
                }
            } while (FindNextFileW(handle, &data));
            FindClose(handle);
        }
        RemoveDirectoryW(directory.c_str());
    }

    if (failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    return 1;
}
