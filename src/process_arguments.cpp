#include "process_arguments.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <memory>
#endif

namespace vnm_terminal::terminal_app {

#if defined(_WIN32)
Process_arguments windows_command_line_arguments(const wchar_t* command_line)
{
    int count = 0;
    wchar_t** native_arguments = CommandLineToArgvW(command_line, &count);
    const DWORD error = native_arguments ? ERROR_SUCCESS : GetLastError();
    const std::unique_ptr<wchar_t*, decltype(&LocalFree)> owner(native_arguments, &LocalFree);
    if (!owner) {
        return {{}, QStringLiteral("Failed to parse the Windows command line (error %1).").arg(error)};
    }

    Process_arguments result;
    result.arguments.reserve(count);
    for (int index = 0; index < count; ++index) {
        result.arguments.append(QString::fromWCharArray(native_arguments[index]));
    }
    return result;
}
#endif

Process_arguments process_arguments(int argc, char** argv)
{
#if defined(_WIN32)
    (void)argc;
    (void)argv;
    // The CRT's narrow argv can already have replaced Unicode with '?'. Use the
    // same Unicode parser as the portable launcher's argument transport.
    return windows_command_line_arguments(GetCommandLineW());
#else
    Process_arguments result;
    result.arguments.reserve(argc);
    for (int index = 0; index < argc; ++index) {
        result.arguments.append(QString::fromLocal8Bit(argv[index]));
    }
    return result;
#endif
}

} // namespace vnm_terminal::terminal_app
