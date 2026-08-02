#include "portable_launcher_text.h"

#include <windows.h>
#include <shellapi.h>

static const wchar_t k_runtime_relative_path[] =
    L"\\vnm_terminal_runtime\\vnm_terminal.exe";

static void show_error_message(const wchar_t* title, const wchar_t* message)
{
    MessageBoxW(NULL, message, title, MB_OK | MB_ICONERROR);
}

static void show_last_error(const wchar_t* title, const wchar_t* prefix)
{
    wchar_t system_message[1024];
    DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageW(
        flags,
        NULL,
        GetLastError(),
        0,
        system_message,
        (DWORD)(sizeof(system_message) / sizeof(system_message[0])),
        NULL);

    // The appends are not checked because there is no better message to fall back to on
    // an error path, and a short message is a complete diagnostic on its own. The buffer
    // is NUL-terminated either way, so a partial assembly is still safe to display.
    wchar_t combined[1400];
    const size_t combined_capacity = sizeof(combined) / sizeof(combined[0]);
    size_t       combined_length   = 0;
    combined[0] = L'\0';
    portable_launcher_append_text(combined, combined_capacity, &combined_length, prefix);
    if (len > 0) {
        portable_launcher_append_text(
            combined, combined_capacity, &combined_length, L"\n\n");
        portable_launcher_append_text(
            combined, combined_capacity, &combined_length, system_message);
    }
    show_error_message(title, combined);
}

// GetModuleFileNameW reports the buffer element count when the module path does not fit:
// it leaves a truncated but NUL-terminated path behind and sets ERROR_INSUFFICIENT_BUFFER
// instead of failing. A truncated launcher path names a different directory, so the
// runtime would be looked for beside the wrong file. Only a length strictly inside the
// buffer is a complete path.
static int module_path_is_complete(DWORD length, size_t capacity)
{
    return length > 0 && (size_t)length < capacity;
}

static void trim_to_directory(wchar_t* path)
{
    int len = lstrlenW(path);
    while (len > 0) {
        wchar_t ch = path[len - 1];
        if (ch == L'\\' || ch == L'/') {
            path[len - 1] = L'\0';
            return;
        }
        len--;
    }
    path[0] = L'\0';
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev_instance, LPSTR cmd_line, int show_cmd)
{
    wchar_t launcher_path[VNM_TERMINAL_MAX_PATH_CHARS + 1];
    wchar_t launcher_dir[ VNM_TERMINAL_MAX_PATH_CHARS + 1];
    wchar_t target_path[  VNM_TERMINAL_MAX_PATH_CHARS + 1];
    wchar_t command_line[ VNM_TERMINAL_MAX_CMDLINE    + 1];
    const size_t launcher_path_capacity = sizeof(launcher_path) / sizeof(launcher_path[0]);
    const size_t launcher_dir_capacity  = sizeof(launcher_dir)  / sizeof(launcher_dir[0]);
    const size_t target_path_capacity   = sizeof(target_path)   / sizeof(target_path[0]);
    const size_t command_line_capacity  = sizeof(command_line)  / sizeof(command_line[0]);
    LPWSTR* argv               = NULL;
    int     argc               = 0;
    size_t  target_path_length = 0;
    size_t  offset             = 0;
    int     assembled          = 0;
    DWORD   launcher_length    = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = 1;

    (void)instance;
    (void)prev_instance;
    (void)cmd_line;

    launcher_length = GetModuleFileNameW(
        NULL, launcher_path, (DWORD)launcher_path_capacity);
    if (launcher_length == 0) {
        show_last_error(L"vnm_terminal", L"Failed to locate vnm_terminal.exe.");
        return 1;
    }
    if (!module_path_is_complete(launcher_length, launcher_path_capacity)) {
        show_error_message(L"vnm_terminal", L"The portable launcher path is too long.");
        return 1;
    }

    // lstrcpynW's count includes the terminating NUL, so it must be the full element count
    // of the destination. The guard above bounds the source below that count, so the copy
    // reproduces the launcher path exactly instead of dropping its last character.
    lstrcpynW(launcher_dir, launcher_path, (int)launcher_dir_capacity);
    trim_to_directory(launcher_dir);

    target_path[0] = L'\0';
    if (!portable_launcher_append_text(
            target_path, target_path_capacity, &target_path_length, launcher_dir) ||
        !portable_launcher_append_text(
            target_path, target_path_capacity, &target_path_length, k_runtime_relative_path))
    {
        show_error_message(
            L"vnm_terminal",
            L"The runtime path is too long to start the packaged application.");
        return 1;
    }

    if (GetFileAttributesW(target_path) == INVALID_FILE_ATTRIBUTES) {
        show_error_message(
            L"vnm_terminal",
            L"Could not find the application runtime.\n\nExpected:\n"
            L"vnm_terminal_runtime\\vnm_terminal.exe");
        return 1;
    }

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc <= 0) {
        show_last_error(L"vnm_terminal", L"Failed to parse the command line.");
        return 1;
    }

    command_line[0] = L'\0';
    offset          = 0;
    assembled       = portable_launcher_append_quoted_arg(
        command_line, command_line_capacity, &offset, target_path);
    for (int i = 1; assembled && i < argc; ++i) {
        assembled =
            portable_launcher_append_text(
                command_line, command_line_capacity, &offset, L" ") &&
            portable_launcher_append_quoted_arg(
                command_line, command_line_capacity, &offset, argv[i]);
    }

    if (!assembled) {
        show_error_message(
            L"vnm_terminal",
            L"The command line is too long to start the packaged application.");
        LocalFree(argv);
        return 1;
    }

    SetCurrentDirectoryW(launcher_dir);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(
            target_path, command_line, NULL, NULL, FALSE, 0, NULL, launcher_dir, &si, &pi))
    {
        show_last_error(L"vnm_terminal", L"Failed to start the packaged application.");
        LocalFree(argv);
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        exit_code = 1;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LocalFree(argv);

    (void)show_cmd;
    return (int)exit_code;
}
