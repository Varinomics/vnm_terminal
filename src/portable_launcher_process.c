#include "portable_launcher_process.h"
#include "portable_launcher_text.h"

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <stdio.h>

#define PORTABLE_LAUNCHER_ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

static void show_error_message(BOOL in_stdio, const wchar_t* in_title, const wchar_t* in_message)
{
    if (in_stdio) {
        fwprintf(stderr, L"%ls\n", in_message);
    } else {
        MessageBoxW(NULL, in_message, in_title, MB_OK | MB_ICONERROR);
    }
}

static void show_last_error(BOOL in_stdio, const wchar_t* in_title, const wchar_t* in_prefix)
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
        show_error_message(in_stdio, in_title, combined);
        return;
    }
    show_error_message(in_stdio, in_title, in_prefix);
}

static HANDLE inherit_standard_stream(DWORD in_stream)
{
    HANDLE stream = GetStdHandle(in_stream);
    if (stream == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }
    if (!stream) {
        // STARTF_USESTDHANDLES requires three valid handles even for an absent stream.
        SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
        return CreateFileW(L"NUL", in_stream == STD_INPUT_HANDLE ? GENERIC_READ : GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    HANDLE copy;
    if (!DuplicateHandle(GetCurrentProcess(), stream, GetCurrentProcess(), &copy,
                         0, TRUE, DUPLICATE_SAME_ACCESS)) {
        return INVALID_HANDLE_VALUE;
    }
    return copy;
}

static BOOL start_runtime(const wchar_t* in_path, wchar_t* in_command, const wchar_t* in_directory,
                          BOOL in_stdio, PROCESS_INFORMATION* out_process)
{
    STARTUPINFOEXW startup = {0};
    startup.StartupInfo.cb = sizeof(STARTUPINFOW);
    if (!in_stdio) {
        return CreateProcessW(in_path, in_command, NULL, NULL, FALSE, 0, NULL, in_directory,
                              &startup.StartupInfo, out_process);
    }

    const DWORD streams[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    HANDLE handles[3] = {0};
    SIZE_T size = 0;
    BOOL initialized = FALSE;
    BOOL started = FALSE;
    DWORD error;
    for (size_t i = 0; i < PORTABLE_LAUNCHER_ARRAY_COUNT(handles); ++i) {
        handles[i] = inherit_standard_stream(streams[i]);
        if (handles[i] == INVALID_HANDLE_VALUE) {
            goto cleanup;
        }
    }
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    startup.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, size);
    if (!startup.lpAttributeList) {
        SetLastError(ERROR_OUTOFMEMORY);
        goto cleanup;
    }
    initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size);
    if (!initialized || !UpdateProcThreadAttribute(startup.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), NULL, NULL)) {
        goto cleanup;
    }

    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = handles[0];
    startup.StartupInfo.hStdOutput = handles[1];
    startup.StartupInfo.hStdError = handles[2];
    started = CreateProcessW(in_path, in_command, NULL, NULL, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, NULL, in_directory, &startup.StartupInfo, out_process);

cleanup:
    error = GetLastError();
    if (initialized) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    }
    if (startup.lpAttributeList) {
        HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    }
    for (size_t i = 0; i < PORTABLE_LAUNCHER_ARRAY_COUNT(handles); ++i) {
        if (handles[i] && handles[i] != INVALID_HANDLE_VALUE) {
            CloseHandle(handles[i]);
        }
    }
    SetLastError(error);
    return started;
}

static int launch_runtime(const portable_launcher_config_t* in_config, int argc, wchar_t** argv)
{
    wchar_t launcher_path[VNM_TERMINAL_MAX_PATH_CHARS + 1];
    wchar_t launcher_dir[ VNM_TERMINAL_MAX_PATH_CHARS + 1];
    wchar_t target_path[  VNM_TERMINAL_MAX_PATH_CHARS + 1];
    wchar_t command_line[ VNM_TERMINAL_MAX_CMDLINE + 1];
    const BOOL stdio = argc > 1 && in_config->stdio_argument_or_null &&
                       wcscmp(argv[1], in_config->stdio_argument_or_null) == 0;
    PROCESS_INFORMATION process_info = {0};
    DWORD exit_code = 1;
    DWORD launcher_length;

    launcher_length = GetModuleFileNameW(
        NULL,
        launcher_path,
        (DWORD)PORTABLE_LAUNCHER_ARRAY_COUNT(launcher_path));
    if (launcher_length == 0) {
        show_last_error(stdio, in_config->title, in_config->locate_error);
        return 1;
    }
    if (!portable_launcher_module_path_is_complete(
            launcher_length,
            PORTABLE_LAUNCHER_ARRAY_COUNT(launcher_path)))
    {
        show_error_message(stdio, in_config->title, L"The portable launcher path is too long.");
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
        show_error_message(stdio, in_config->title, in_config->runtime_path_too_long);
        return 1;
    }

    if (GetFileAttributesW(target_path) == INVALID_FILE_ATTRIBUTES) {
        show_error_message(stdio, in_config->title, in_config->runtime_missing);
        return 1;
    }

    if (!portable_launcher_build_command_line(
            target_path, argc, argv, PORTABLE_LAUNCHER_ARRAY_COUNT(command_line), command_line))
    {
        show_error_message(stdio, in_config->title, in_config->command_line_too_long);
        return 1;
    }

    if (in_config->app_user_model_id_or_null) {
        (void)SetCurrentProcessExplicitAppUserModelID(in_config->app_user_model_id_or_null);
    }
    SetCurrentDirectoryW(launcher_dir);

    if (!start_runtime(target_path, command_line, launcher_dir, stdio, &process_info)) {
        show_last_error(stdio, in_config->title, L"Failed to start the packaged application.");
        return 1;
    }

    WaitForSingleObject(process_info.hProcess, INFINITE);
    if (!GetExitCodeProcess(process_info.hProcess, &exit_code)) {
        exit_code = 1;
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return (int)exit_code;
}

int portable_launcher_run(const portable_launcher_config_t* in_config)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        show_last_error(FALSE, in_config->title, L"Failed to parse the command line.");
        return 1;
    }
    int result = launch_runtime(in_config, argc, argv);
    LocalFree(argv);
    return result;
}
