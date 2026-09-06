#include "portable_launcher_process.h"

#include <windows.h>

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous_instance, LPSTR raw_command_line, int show_command)
{
    const portable_launcher_config_t config = {
        L"vnm_terminal",
        L"\\vnm_terminal_runtime\\vnm_terminal.exe",
        NULL,
        L"Failed to locate vnm_terminal.exe.",
        L"The runtime path is too long to start the packaged application.",
        L"Could not find the application runtime.\n\nExpected:\n"
        L"vnm_terminal_runtime\\vnm_terminal.exe",
        L"The command line is too long to start the packaged application."
    };
    (void)instance;
    (void)previous_instance;
    (void)raw_command_line;
    (void)show_command;
    return portable_launcher_run(&config);
}
