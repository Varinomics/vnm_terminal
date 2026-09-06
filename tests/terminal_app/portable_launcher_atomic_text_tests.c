#include "portable_launcher_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

static int check(int condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

static int is_filled_with(const wchar_t* text, size_t count, wchar_t expected)
{
    for (size_t i = 0; i < count; ++i) {
        if (text[i] != expected) {
            return 0;
        }
    }
    return 1;
}

static int test_atomic_capacity(void)
{
    // Preserve the products' unchanged-output contract at every boundary, including
    // escapes expanding before the closing quote and a NUL that does not fit.
    wchar_t argument[] = L"space " L"\\" L"\"";
    wchar_t* arguments[] = {L"launcher", argument, L"", L"\x03a9\x4e2d"};
    const wchar_t expected[] = L"T \"space " L"\\\\\\" L"\"\" \"\" \x03a9\x4e2d";
    wchar_t output[128];
    int ok = 1;
    for (size_t capacity = 0; capacity <= ARRAY_COUNT(expected); ++capacity) {
        wmemset(output, L'Q', ARRAY_COUNT(output));
        int built = portable_launcher_build_command_line(
            L"T", (int)ARRAY_COUNT(arguments), arguments, capacity, output);
        if (capacity < ARRAY_COUNT(expected)) {
            ok &= check(!built, "undersized command must fail");
            ok &= check(is_filled_with(output, ARRAY_COUNT(output), L'Q'),
                "rejected command must leave the entire buffer unchanged");
        }
        else {
            ok &= check(built && wcscmp(output, expected) == 0, "exact command capacity must succeed");
            ok &= check(output[capacity] == L'Q', "command must stay inside capacity");
        }
    }

    const wchar_t joined[] = L"root\\runtime\\app.exe";
    for (size_t capacity = 0; capacity <= ARRAY_COUNT(joined); ++capacity) {
        wmemset(output, L'Q', ARRAY_COUNT(output));
        int built = portable_launcher_join_text(L"root", L"\\", L"runtime\\app.exe", capacity, output);
        if (capacity < ARRAY_COUNT(joined)) {
            ok &= check(!built && is_filled_with(output, ARRAY_COUNT(output), L'Q'),
                "rejected path must leave the entire buffer unchanged");
        }
        else {
            ok &= check(built && wcscmp(output, joined) == 0, "exact path capacity must succeed");
            ok &= check(output[capacity] == L'Q', "path must stay inside capacity");
        }
    }
    ok &= check(portable_launcher_join_text(L"", L"", L"", 1, output) && output[0] == 0,
        "empty joined text requires exactly its NUL");
    return ok;
}

static int test_maximum_command_line(void)
{
    const size_t slash_count = 16380;
    wchar_t* argument = malloc((slash_count + 2) * sizeof(wchar_t));
    wchar_t* output = malloc((VNM_TERMINAL_MAX_CMDLINE + 1) * sizeof(wchar_t));
    if (!argument || !output) {
        free(argument);
        free(output);
        return check(0, "could not allocate maximum command fixture");
    }
    wmemset(argument, L'\\', slash_count);
    argument[slash_count] = L'"';
    argument[slash_count + 1] = 0;
    wchar_t* arguments[] = {L"launcher", argument};
    int ok = check(portable_launcher_build_command_line(
        L"T", 2, arguments, VNM_TERMINAL_MAX_CMDLINE + 1, output),
        "CreateProcessW inclusive 32767-character limit must fit");
    if (ok) {
        ok &= check(wcslen(output) == VNM_TERMINAL_MAX_CMDLINE, "maximum encoded size must match");
    }
    wmemset(output, L'Q', VNM_TERMINAL_MAX_CMDLINE + 1);
    ok &= check(!portable_launcher_build_command_line(
        L"T", 2, arguments, VNM_TERMINAL_MAX_CMDLINE, output), "missing final NUL must fail");
    ok &= check(is_filled_with(output, VNM_TERMINAL_MAX_CMDLINE + 1, L'Q'),
        "maximum command rejection must remain atomic");
    free(argument);
    free(output);
    return ok;
}

static int test_maximum_target_path(void)
{
    const wchar_t suffix[] = L"\\runtime\\app.exe";
    const size_t directory_length = VNM_TERMINAL_MAX_PATH_CHARS - wcslen(suffix);
    wchar_t* directory = malloc((directory_length + 2) * sizeof(wchar_t));
    wchar_t* output = malloc((VNM_TERMINAL_MAX_PATH_CHARS + 1) * sizeof(wchar_t));
    if (!directory || !output) {
        free(directory);
        free(output);
        return check(0, "could not allocate maximum path fixture");
    }
    wmemset(directory, L'P', directory_length);
    directory[directory_length] = 0;
    int ok = check(portable_launcher_join_text(
        directory, L"", suffix, VNM_TERMINAL_MAX_PATH_CHARS + 1, output),
        "maximum target path must fit");
    if (ok) {
        ok &= check(wcslen(output) == VNM_TERMINAL_MAX_PATH_CHARS, "maximum path length must match");
    }
    directory[directory_length] = L'P';
    directory[directory_length + 1] = 0;
    wmemset(output, L'Q', VNM_TERMINAL_MAX_PATH_CHARS + 1);
    ok &= check(!portable_launcher_join_text(
        directory, L"", suffix, VNM_TERMINAL_MAX_PATH_CHARS + 1, output), "oversized target path must fail");
    ok &= check(is_filled_with(output, VNM_TERMINAL_MAX_PATH_CHARS + 1, L'Q'),
        "oversized target path must leave its output unchanged");
    free(directory);
    free(output);
    return ok;
}

static int test_directory_paths(void)
{
    wchar_t windows_path[] = L"C:\\portable path\\launcher.exe";
    wchar_t slash_path[] = L"C:/portable path/launcher.exe";
    wchar_t bare_path[] = L"launcher.exe";
    portable_launcher_trim_to_directory(wcslen(windows_path), windows_path);
    portable_launcher_trim_to_directory(wcslen(slash_path), slash_path);
    portable_launcher_trim_to_directory(wcslen(bare_path), bare_path);
    int ok = check(wcscmp(windows_path, L"C:\\portable path") == 0, "Windows directory trim");
    ok &= check(wcscmp(slash_path, L"C:/portable path") == 0, "slash directory trim");
    ok &= check(bare_path[0] == 0, "bare executable has no directory");
    ok &= check(portable_launcher_module_path_is_complete(7, 8), "complete module path");
    ok &= check(!portable_launcher_module_path_is_complete(8, 8), "truncated module path");
    ok &= check(!portable_launcher_module_path_is_complete(0, 8), "failed module lookup");
    return ok;
}

int main(void)
{
    int ok = test_atomic_capacity();
    ok &= test_maximum_command_line();
    ok &= test_maximum_target_path();
    ok &= test_directory_paths();
    return ok ? 0 : 1;
}
