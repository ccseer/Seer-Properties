// Test double for git.exe. It emulates the small, read-only Git surface used by
// the git-info plugin so output, timeout and limit tests stay deterministic
// without a real Git installation.
//
// The scenario is selected by the directory passed through "-C <dir>": a folder
// whose name contains a scenario token drives the replies. Replies are emitted
// through the redirected pipes exactly as git would, including NUL-delimited
// porcelain v2 records.
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char *kLongOid = "abcdef0123456789abcdef0123456789abcdef01";

bool findValue(const std::vector<std::wstring> &arguments, const wchar_t *key,
               std::wstring *value)
{
    for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
        if (_wcsicmp(arguments[i].c_str(), key) == 0) {
            *value = arguments[i + 1];
            return true;
        }
    }
    return false;
}

bool hasToken(const std::vector<std::wstring> &arguments, const wchar_t *token)
{
    for (const auto &argument : arguments) {
        if (_wcsicmp(argument.c_str(), token) == 0) {
            return true;
        }
    }
    return false;
}

void writeStdout(const std::string &text)
{
    fwrite(text.data(), 1, text.size(), stdout);
    fflush(stdout);
}

void writeStderr(const std::string &text)
{
    fwrite(text.data(), 1, text.size(), stderr);
    fflush(stderr);
}

std::string toUtf8(const std::wstring &text)
{
    const int length = static_cast<int>(text.size());
    const int size   = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), length,
                                           nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), length, &utf8[0], size,
                            nullptr, nullptr);
    }
    return utf8;
}

std::string lowerAscii(const std::wstring &text)
{
    std::string lowered = toUtf8(text);
    for (auto &c : lowered) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return lowered;
}

// NUL-terminated records, matching `git status --porcelain=v2 -z`.
std::string join(const std::vector<std::string> &parts)
{
    std::string joined;
    for (const auto &part : parts) {
        joined += part;
        joined.push_back('\0');
    }
    return joined;
}

}  // namespace

int wmain()
{
    int argc = 0;
    const auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return 2;
    }
    std::vector<std::wstring> arguments;
    for (int i = 0; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }
    LocalFree(argv);

    std::wstring workDirectory;
    findValue(arguments, L"-C", &workDirectory);
    const std::string scenario = lowerAscii(workDirectory);

    const auto has = [&scenario](const char *token) {
        return scenario.find(token) != std::string::npos;
    };

    if (hasToken(arguments, L"--version")) {
        writeStdout(has("oldversion") ? "git version 2.10.4.msysgit.0\n"
                                      : "git version 2.45.0.windows.1\n");
        return 0;
    }

    // Echoes the received argument list and a few environment values so the
    // runner's quoting and environment sanitization can be asserted directly.
    if (has("echoargs")) {
        for (std::size_t i = 1; i < arguments.size(); ++i) {
            writeStdout(toUtf8(arguments[i]) + "\n");
        }
        const auto report = [](const char *name) {
            const char *value = std::getenv(name);
            return std::string(name) + "="
                   + (value ? value : "<unset>") + "\n";
        };
        // Location, namespace and helper overrides: must be unset.
        writeStdout(report("GIT_DIR"));
        writeStdout(report("GIT_WORK_TREE"));
        writeStdout(report("GIT_INDEX_FILE"));
        writeStdout(report("GIT_OBJECT_DIRECTORY"));
        writeStdout(report("GIT_CONFIG_COUNT"));
        writeStdout(report("GIT_CONFIG_PARAMETERS"));
        writeStdout(report("GIT_EXTERNAL_DIFF"));
        writeStdout(report("GIT_TRACE"));
        // Forced read-only values: must be exactly what the runner sets.
        writeStdout(report("GIT_OPTIONAL_LOCKS"));
        writeStdout(report("GIT_TERMINAL_PROMPT"));
        writeStdout(report("GIT_PAGER"));
        writeStdout(report("GIT_EDITOR"));
        // Configuration location: deliberately inherited, because dropping it
        // could invalidate the user's safe.directory exceptions.
        writeStdout(report("GIT_CONFIG_GLOBAL"));
        return 0;
    }

    // `git --version` is always fast; every other query stalls so the caller's
    // internal deadline and process termination can be exercised.
    if (has("sleeptest")) {
        Sleep(60000);
        return 0;
    }

    // Older Git did not understand --no-optional-locks. Reject it so a test can
    // prove the helper stops passing the flag on such versions.
    if (has("oldversion") && hasToken(arguments, L"--no-optional-locks")) {
        writeStderr("error: unknown option `no-optional-locks'\n");
        return 129;
    }

    if (hasToken(arguments, L"--git-dir")) {
        if (has("notrepo")) {
            writeStderr("fatal: not a git repository (or any of the parent "
                        "directories): .git\n");
            return 128;
        }
        if (has("unsafe")) {
            writeStderr("fatal: detected dubious ownership in repository at "
                        "'C:/repo'\n"
                        "To add an exception for this directory, call:\n"
                        "\tgit config --global --add safe.directory C:/repo\n");
            return 128;
        }
        if (has("denied")) {
            writeStderr("fatal: unable to read repository: Permission "
                        "denied\n");
            return 128;
        }
        writeStdout(has("bare") ? ".\n" : ".git\n");
        return 0;
    }

    if (hasToken(arguments, L"--is-bare-repository")) {
        const std::string bare = has("bare") ? "true" : "false";
        std::string common     = ".git";
        if (has("bare")) {
            common = ".";
        }
        else if (has("linkedworktree")) {
            common = "main.git";
        }
        writeStdout(bare + "\n" + toUtf8(workDirectory) + "\\.git\n" + common
                    + "\n");
        return 0;
    }

    if (hasToken(arguments, L"--show-toplevel")) {
        std::wstring root = workDirectory;
        if (has("nested")) {
            const std::size_t separator = root.find_last_of(L"\\/");
            if (separator != std::wstring::npos) {
                root = root.substr(0, separator);
            }
        }
        if (has("escape")) {
            // A path that needs JSON escaping: quotes, a backslash and non-ASCII
            // text. The plugin must publish it without breaking the JSON, so the
            // value can round-trip through the parser unchanged.
            writeStdout("C:\\\\odd\\\"quoted\\\"\\\\p\xC3\xA4th\n");
            return 0;
        }
        writeStdout(toUtf8(root) + "\n");
        return 0;
    }

    if (hasToken(arguments, L"--absolute-git-dir")) {
        writeStdout(toUtf8(workDirectory) + "\n");
        return 0;
    }

    if (hasToken(arguments, L"symbolic-ref")) {
        if (has("detached") && !has("bare")) {
            return 1;
        }
        writeStdout("main\n");
        return 0;
    }

    if (hasToken(arguments, L"status")) {
        if (has("bare")) {
            writeStderr("fatal: this operation must be run in a work tree\n");
            return 128;
        }
        if (has("corrupt")) {
            writeStderr("fatal: index file corrupt\n");
            return 128;
        }

        std::vector<std::string> records;
        if (has("unborn")) {
            records.push_back("# branch.oid (initial)");
            records.push_back("# branch.head main");
        }
        else if (has("detached")) {
            records.push_back(std::string("# branch.oid ") + kLongOid);
            records.push_back("# branch.head (detached)");
        }
        else if (has("noupstream")) {
            records.push_back(std::string("# branch.oid ") + kLongOid);
            records.push_back("# branch.head feature/x");
        }
        else {
            records.push_back(std::string("# branch.oid ") + kLongOid);
            records.push_back("# branch.head main");
            records.push_back("# branch.upstream origin/main");
            records.push_back(has("dirty") ? "# branch.ab +2 -3"
                                           : "# branch.ab +0 -0");
        }

        if (has("dirty")) {
            records.push_back("1 .M N... 100644 100644 100644 1234567 7654321 "
                              "file one.txt");
            // The rename record is followed by the original path as its own
            // NUL-terminated field. It deliberately starts with 'u' so a parser
            // that treats it as a record would count a false conflict.
            records.push_back(std::string("2 R. N... 100644 100644 100644 "
                                          "aaaaaaa bbbbbbb old.txt")
                              + '\0' + "updated notes.txt");
            records.push_back("u UU N... 100644 100644 100644 ccccccc ddddddd "
                              "conflict.txt");
            records.push_back("? untracked_dir/");
        }
        if (has("unicode")) {
            records.push_back("? dir with space/");
            records.push_back("? unicode-\xE6\xB5\x8B\xE8\xAF\x95.txt");
        }
        if (has("conflict")) {
            records.push_back("u UU N... 100644 100644 100644 ccccccc ddddddd "
                              "conflict.txt");
        }
        writeStdout(join(records));
        return 0;
    }

    if (hasToken(arguments, L"rev-parse")) {
        if (has("bare") && !has("barecommit")) {
            // A bare repository whose branch is still unborn.
            writeStderr("fatal: Needed a single revision\n");
            return 128;
        }
        writeStdout(std::string(kLongOid) + "\n");
        return 0;
    }

    if (hasToken(arguments, L"--is-inside-work-tree")) {
        writeStdout(has("bare") ? "false\n" : "true\n");
        return 0;
    }

    return 128;
}
