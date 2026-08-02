// Oracle for this fixture:
//   - GetModuleFileNameW is documented to return nSize, set ERROR_INSUFFICIENT_BUFFER, and
//     leave a truncated but NUL-terminated buffer behind when the module path does not fit
//     the caller's buffer. That documented contract, not the launcher's current behaviour,
//     decides which lookup results are complete paths, so module_path_is_complete is
//     asserted directly against it.
//
// The launcher translation unit is included with WinMain renamed out of the way so that
// this console test process can link it. The GetModuleFileNameW call itself lives inside
// WinMain and cannot be exercised from a test; only the predicate that classifies its
// result can be.

#include <stdio.h>
#include <windows.h>

#define WinMain vnm_terminal_portable_launcher_WinMain
#include "portable_launcher_win.c"
#undef WinMain

static int check(int condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 0;
    }
    return 1;
}

static int test_module_path_truncation_detection(void)
{
    int ok = 1;

    ok &= check( module_path_is_complete(7, 8), "complete module path was rejected");
    ok &= check(!module_path_is_complete(8, 8), "truncated module path was accepted");
    ok &= check(!module_path_is_complete(0, 8), "failed module-path lookup was accepted");
    return ok;
}

int main(void)
{
    if (!test_module_path_truncation_detection()) {
        fprintf(stderr, "portable_launcher_win_tests: FAILED\n");
        return 1;
    }

    printf("portable_launcher_win_tests: OK\n");
    return 0;
}
