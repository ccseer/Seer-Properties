#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "signaturehelper.h"

int wmain()
{
    int argc = 0;
    const auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr)
        return 2;

    std::vector<std::wstring> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i)
        arguments.emplace_back(argv[i]);
    LocalFree(argv);

    return runDigitalSignature(arguments);
}
