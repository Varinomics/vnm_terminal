#include "portable_launcher_process.h"
#include "portable_launcher_text.h"

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>

#define PORTABLE_LAUNCHER_ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

static void show_error_message(const wchar_t* in_title, const wchar_t* in_message)
{
    MessageBoxW(NULL, in_message, in_title, MB_OK | MB_ICONERROR);
}

static void show_last_error(const wchar_t* in_title, const wchar_t* in_prefix)
{
    wchar_t system_message[1024];
    wchar_t combined[1400];
    DWORD last_error = GetLastError();
    DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD length = FormatMessageW(
        flags,
        NULL,
        last_error,
        0,
        system_message,
        (DWORD)PORTABLE_LAUNCHER_ARRAY_COUNT(system_message),
        NULL);

    if (length > 0 &&
        portable_launcher_join_text(
            in_prefix, L"\n\n", system_message, PORTABLE_LAUNCHER_ARRAY_COUNT(combined), combined))
    {
        show_error_message(in_title, combined);
        return;
    }
    show_error_message(in_title, in_prefix);
}

int portable_launcher_run(const portable_launcher_config_t* in_config)
{
    wchar_t launcher_path[VNM_TERMINAL_MAX_PATH_CHARS + 1];
    wchar_t launcher_dir[ VNM_TERMINAL_MAX_PATH_CHARS + 1];
    wchar_t target_path[  VNM_TERMINAL_MAX_PATH_CHARS + 1];
    wchar_t command_line[ VNM_TERMINAL_MAX_CMDLINE + 1];
    LPWSTR* argv = NULL;
    int argc = 0;
    STARTUPINFOW startup_info;
    PROCESS_INFORMATION process_info;
    DWORD exit_code = 1;
    DWORD launcher_length;

    launcher_length = GetModuleFileNameW(
        NULL,
        launcher_path,
        (DWORD)PORTABLE_LAUNCHER_ARRAY_COUNT(launcher_path));
    if (launcher_length == 0) {
        show_last_error(in_config->title, in_config->locate_error);
        return 1;
    }
    if (!portable_launcher_module_path_is_complete(
            launcher_length,
            PORTABLE_LAUNCHER_ARRAY_COUNT(launcher_path)))
    {
        show_error_message(in_config->title, L"The portable launcher path is too long.");
        return 1;
    }

    for (size_t i = 0; i <= launcher_length; ++i) {
        launcher_dir[i] = launcher_path[i];
    }
    portable_launcher_trim_to_directory(launcher_length, launcher_dir);

    if (!portable_launcher_join_text(
            launcher_dir, L"", in_config->runtime_relative_path,
            PORTABLE_LAUNCHER_ARRAY_COUNT(target_path), target_path))
    {
        show_error_message(in_config->title, in_config->runtime_path_too_long);
        return 1;
    }

    if (GetFileAttributesW(target_path) == INVALID_FILE_ATTRIBUTES) {
        show_error_message(in_config->title, in_config->runtime_missing);
        return 1;
    }

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc <= 0) {
        show_last_error(in_config->title, L"Failed to parse the command line.");
        return 1;
    }

    if (!portable_launcher_build_command_line(
            target_path, argc, argv, PORTABLE_LAUNCHER_ARRAY_COUNT(command_line), command_line))
    {
        show_error_message(in_config->title, in_config->command_line_too_long);
        LocalFree(argv);
        return 1;
    }

    if (in_config->app_user_model_id_or_null) {
        (void)SetCurrentProcessExplicitAppUserModelID(in_config->app_user_model_id_or_null);
    }
    SetCurrentDirectoryW(launcher_dir);

    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    ZeroMemory(&process_info, sizeof(process_info));

    if (!CreateProcessW(
            target_path,
            command_line,
            NULL,
            NULL,
            FALSE,
            0,
            NULL,
            launcher_dir,
            &startup_info,
            &process_info))
    {
        show_last_error(in_config->title, L"Failed to start the packaged application.");
        LocalFree(argv);
        return 1;
    }

    WaitForSingleObject(process_info.hProcess, INFINITE);
    if (!GetExitCodeProcess(process_info.hProcess, &exit_code)) {
        exit_code = 1;
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    LocalFree(argv);

    return (int)exit_code;
}
