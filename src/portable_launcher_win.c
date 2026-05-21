#include <windows.h>
#include <shellapi.h>

#define VNM_TERMINAL_MAX_CMDLINE   32767
#define VNM_TERMINAL_MAX_PATH_CHARS 32767

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

    wchar_t combined[1400];
    if (len > 0) {
        wsprintfW(combined, L"%s\n\n%s", prefix, system_message);
    }
    else {
        lstrcpynW(combined, prefix, (int)(sizeof(combined) / sizeof(combined[0])));
    }
    show_error_message(title, combined);
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

static size_t append_quoted_arg(wchar_t* dst, size_t offset, const wchar_t* arg)
{
    int needs_quotes = arg[0] == L'\0';
    for (const wchar_t* p = arg; *p; ++p) {
        if (*p == L' ' || *p == L'\t' || *p == L'\n' || *p == L'\v' || *p == L'"') {
            needs_quotes = 1;
            break;
        }
    }

    if (!needs_quotes) {
        while (*arg) {
            dst[offset++] = *arg++;
        }
        dst[offset] = L'\0';
        return offset;
    }

    dst[offset++] = L'"';
    {
        unsigned backslashes = 0;
        for (const wchar_t* p = arg; *p; ++p) {
            if (*p == L'\\') {
                backslashes++;
                continue;
            }
            if (*p == L'"') {
                for (unsigned i = 0; i < backslashes * 2 + 1; ++i) {
                    dst[offset++] = L'\\';
                }
                dst[offset++] = L'"';
                backslashes = 0;
                continue;
            }
            while (backslashes > 0) {
                dst[offset++] = L'\\';
                backslashes--;
            }
            dst[offset++] = *p;
        }
        while (backslashes > 0) {
            dst[offset++] = L'\\';
            dst[offset++] = L'\\';
            backslashes--;
        }
    }
    dst[offset++] = L'"';
    dst[offset] = L'\0';
    return offset;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev_instance, LPSTR cmd_line, int show_cmd)
{
    wchar_t launcher_path[VNM_TERMINAL_MAX_PATH_CHARS + 1];
    wchar_t launcher_dir[ VNM_TERMINAL_MAX_PATH_CHARS + 1];
    wchar_t target_path[  VNM_TERMINAL_MAX_PATH_CHARS + 1];
    wchar_t command_line[ VNM_TERMINAL_MAX_CMDLINE    + 1];
    LPWSTR* argv   = NULL;
    int     argc   = 0;
    size_t  offset = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = 1;

    (void)instance;
    (void)prev_instance;
    (void)cmd_line;

    if (GetModuleFileNameW(NULL, launcher_path, VNM_TERMINAL_MAX_PATH_CHARS) == 0) {
        show_last_error(L"vnm_terminal", L"Failed to locate vnm_terminal.exe.");
        return 1;
    }

    lstrcpynW(launcher_dir, launcher_path, VNM_TERMINAL_MAX_PATH_CHARS);
    trim_to_directory(launcher_dir);

    if (wsprintfW(
            target_path,
            L"%s\\vnm_terminal_runtime\\vnm_terminal.exe",
            launcher_dir) <= 0)
    {
        show_error_message(L"vnm_terminal", L"Failed to prepare the runtime path.");
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
    offset = append_quoted_arg(command_line, 0, target_path);
    for (int i = 1; i < argc; ++i) {
        command_line[offset++] = L' ';
        command_line[offset]   = L'\0';
        offset                 = append_quoted_arg(command_line, offset, argv[i]);
    }

    SetEnvironmentVariableW(L"VNM_TERMINAL_PORTABLE_ROOT", launcher_dir);
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
