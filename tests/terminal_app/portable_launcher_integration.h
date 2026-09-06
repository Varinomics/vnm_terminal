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
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
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

cleanup:
    if (!ok) {
        fprintf(stderr, "FAIL: packaged launcher integration (Windows error %lu)\n", GetLastError());
    }
    if (runtime[0]) { DeleteFileW(runtime); }
    if (launcher[0]) { DeleteFileW(launcher); }
    if (runtime_directory[0]) { RemoveDirectoryW(runtime_directory); }
    if (directory[0]) { RemoveDirectoryW(directory); }
    if (root[0]) { RemoveDirectoryW(root); }
    LocalFree(argv);
    return ok ? 0 : 1;
}
