#pragma once

#include "portable_launcher_text.h"

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

#define LAUNCHER_TEST_CAPACITY 32768
#define LAUNCHER_TEST_SUCCESS 73

static int launcher_test_child(int argc, wchar_t** argv)
{
    wchar_t directory[LAUNCHER_TEST_CAPACITY];
    const wchar_t* forwarded[] = {
        L"", L"two words", L"quote\"inside", L"space and trailing slash\\",
        L"\x03a9\x4e2d", L"tab\tline\nbreak", L"two\\\\before\"quote"
    };
    if (argc != 11 || wcscmp(argv[0], argv[3]) != 0 ||
        !GetCurrentDirectoryW(LAUNCHER_TEST_CAPACITY, directory) ||
        wcscmp(directory, argv[2]) != 0)
    {
        fprintf(stderr, "FAIL: configured runtime executable or working directory changed\n");
        return 1;
    }
    for (int i = 0; i < 7; ++i) {
        if (wcscmp(argv[i + 4], forwarded[i]) != 0) {
            fprintf(stderr, "FAIL: forwarded argument %d changed\n", i);
            return 1;
        }
    }
    return LAUNCHER_TEST_SUCCESS;
}

struct launcher_error_dialog_t
{
    DWORD          process_id;
    const wchar_t* message;
    int            found;
    HWND           button;
};

static BOOL CALLBACK launcher_test_find_message(HWND window, LPARAM parameter)
{
    struct launcher_error_dialog_t* dialog = (struct launcher_error_dialog_t*)parameter;
    wchar_t text[LAUNCHER_TEST_CAPACITY];
    GetWindowTextW(window, text, LAUNCHER_TEST_CAPACITY);
    wchar_t class_name[128];
    GetClassNameW(window, class_name, 128);
    if (wcscmp(class_name, L"Button") == 0) {
        dialog->button = window;
    }
    if (wcsncmp(text, dialog->message, wcslen(dialog->message)) == 0) {
        dialog->found = 1;
    }
    return TRUE;
}

static BOOL CALLBACK launcher_test_close_error(HWND window, LPARAM parameter)
{
    struct launcher_error_dialog_t* dialog = (struct launcher_error_dialog_t*)parameter;
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != dialog->process_id) {
        return TRUE;
    }
    wchar_t class_name[128];
    GetClassNameW(window, class_name, 128);
    if (wcscmp(class_name, L"#32770") != 0) {
        return TRUE;
    }
    EnumChildWindows(window, launcher_test_find_message, parameter);
    if (dialog->found && dialog->button) {
        // Windows may give the sole MB_OK button IDCANCEL rather than IDOK.
        PostMessageW(window, WM_COMMAND,
            MAKEWPARAM(GetDlgCtrlID(dialog->button), BN_CLICKED), (LPARAM)dialog->button);
    }
    return TRUE;
}

// Inspect and dismiss only the error dialog belonging to this test's launcher.
static int launcher_test_error(
    const wchar_t* in_launcher,
    const wchar_t* in_directory,
    wchar_t*       in_desktop_name,
    HDESK          in_desktop,
    const wchar_t* in_message)
{
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    startup.lpDesktop = in_desktop_name;
    if (!CreateProcessW(in_launcher, NULL, NULL, NULL, FALSE, 0,
        NULL, in_directory, &startup, &process))
    {
        return 0;
    }
    struct launcher_error_dialog_t dialog = {
        process.dwProcessId, in_message, 0, NULL
    };
    DWORD waited = WAIT_TIMEOUT;
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (waited == WAIT_TIMEOUT && GetTickCount64() < deadline) {
        EnumDesktopWindows(in_desktop, launcher_test_close_error, (LPARAM)&dialog);
        waited = WaitForSingleObject(process.hProcess, 20);
    }
    DWORD exit_code = 0;
    int ok = waited == WAIT_OBJECT_0 && dialog.found &&
             GetExitCodeProcess(process.hProcess, &exit_code) && exit_code == 1;
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!ok) {
        fwprintf(stderr, L"FAIL: launcher error dialog or failure exit changed: message=%ls, "
            L"wait=%lu, found=%d, exit=%lu\n", in_message, waited, dialog.found, exit_code);
    }
    return ok;
}

// Exercises the compiled launcher against a runtime probe placed at the product's
// packaged relative path. The child verifies its real argv and working directory.
static int portable_launcher_integration_main(const wchar_t* in_runtime_suffix)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return 1;
    }
    if (argc > 1 && wcscmp(argv[1], L"--portable-child") == 0) {
        int result = launcher_test_child(argc, argv);
        LocalFree(argv);
        return result;
    }
    if (argc != 2) {
        fprintf(stderr, "FAIL: expected the compiled launcher path\n");
        LocalFree(argv);
        return 1;
    }

    wchar_t temp[LAUNCHER_TEST_CAPACITY];
    wchar_t root[LAUNCHER_TEST_CAPACITY];
    wchar_t directory[LAUNCHER_TEST_CAPACITY];
    wchar_t runtime[LAUNCHER_TEST_CAPACITY];
    wchar_t runtime_directory[LAUNCHER_TEST_CAPACITY];
    wchar_t launcher[LAUNCHER_TEST_CAPACITY];
    wchar_t self[LAUNCHER_TEST_CAPACITY];
    wchar_t command[LAUNCHER_TEST_CAPACITY];
    wchar_t desktop_name[128];
    HDESK desktop = NULL;
    int ok = 0;
    directory[0] = runtime[0] = runtime_directory[0] = launcher[0] = root[0] = 0;
    if (!GetTempPathW(LAUNCHER_TEST_CAPACITY, temp) ||
        !GetTempFileNameW(temp, L"vpl", 0, root))
    {
        goto cleanup;
    }
    if (!DeleteFileW(root) || !CreateDirectoryW(root, NULL) ||
        !portable_launcher_join_text(root, L"\\", L"portable path \x03a9", LAUNCHER_TEST_CAPACITY, directory) ||
        !CreateDirectoryW(directory, NULL) ||
        !portable_launcher_join_text(directory, L"", in_runtime_suffix, LAUNCHER_TEST_CAPACITY, runtime) ||
        !portable_launcher_join_text(directory, L"\\", L"launcher.exe", LAUNCHER_TEST_CAPACITY, launcher))
    {
        goto cleanup;
    }
    wcscpy(runtime_directory, runtime);
    portable_launcher_trim_to_directory(wcslen(runtime_directory), runtime_directory);
    if (!CreateDirectoryW(runtime_directory, NULL) ||
        !portable_launcher_module_path_is_complete(
            GetModuleFileNameW(NULL, self, LAUNCHER_TEST_CAPACITY), LAUNCHER_TEST_CAPACITY) ||
        !CopyFileW(self, runtime, TRUE) || !CopyFileW(argv[1], launcher, TRUE))
    {
        goto cleanup;
    }
    wchar_t* child_arguments[] = {
        launcher, L"--portable-child", directory, runtime,
        L"", L"two words", L"quote\"inside", L"space and trailing slash\\",
        L"\x03a9\x4e2d", L"tab\tline\nbreak", L"two\\\\before\"quote"
    };
    if (!portable_launcher_build_command_line(
            launcher, 11, child_arguments, LAUNCHER_TEST_CAPACITY, command))
    {
        goto cleanup;
    }
    // Even an unexpected launcher failure must not put a dialog on the user's desktop.
    swprintf(desktop_name, 128, L"vnm_launcher_test_%lu_%llu",
        GetCurrentProcessId(), GetTickCount64());
    desktop = CreateDesktopW(desktop_name, NULL, NULL, 0, GENERIC_ALL, NULL);
    if (!desktop) {
        fprintf(stderr, "FAIL: creating isolated launcher-test desktop: %lu\n", GetLastError());
        goto cleanup;
    }
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    startup.lpDesktop = desktop_name;
    if (!CreateProcessW(launcher, command, NULL, NULL, FALSE, 0, NULL, temp, &startup, &process)) {
        goto cleanup;
    }
    DWORD exit_code = 1;
    DWORD waited = WaitForSingleObject(process.hProcess, 20000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    ok = waited == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exit_code) &&
         exit_code == LAUNCHER_TEST_SUCCESS;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (!ok || !DeleteFileW(runtime)) {
        ok = 0;
        goto cleanup;
    }
    wchar_t missing_message[LAUNCHER_TEST_CAPACITY];
    if (!portable_launcher_join_text(
            L"Could not find the application runtime.\n\nExpected:\n", L"",
            in_runtime_suffix + 1, LAUNCHER_TEST_CAPACITY, missing_message))
    {
        ok = 0;
        goto cleanup;
    }
    ok = launcher_test_error(launcher, temp, desktop_name, desktop, missing_message);
    if (!ok) {
        goto cleanup;
    }
    HANDLE invalid_runtime = CreateFileW(
        runtime, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (invalid_runtime == INVALID_HANDLE_VALUE) {
        ok = 0;
        goto cleanup;
    }
    CloseHandle(invalid_runtime);
    ok = launcher_test_error(
        launcher, temp, desktop_name, desktop, L"Failed to start the packaged application.");

cleanup:
    if (!ok) {
        fprintf(stderr, "FAIL: packaged launcher integration (Windows error %lu)\n", GetLastError());
    }
    if (desktop) { CloseDesktop(desktop); }
    if (runtime[0]) { DeleteFileW(runtime); }
    if (launcher[0]) { DeleteFileW(launcher); }
    if (runtime_directory[0]) { RemoveDirectoryW(runtime_directory); }
    if (directory[0]) { RemoveDirectoryW(directory); }
    if (root[0]) { RemoveDirectoryW(root); }
    LocalFree(argv);
    return ok ? 0 : 1;
}
