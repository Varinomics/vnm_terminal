#pragma once

#if defined(_WIN32)
#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>

#include <cstdio>
#endif

extern "C" __declspec(dllimport) unsigned int __stdcall SetErrorMode(unsigned int mode);

namespace vnm_terminal::tests {

#if defined(_MSC_VER)
inline int __cdecl test_crt_report_hook(int report_type, char* message, int* return_value)
{
    (void)report_type;

    if (message != nullptr) {
        std::fputs(message, stderr);
    }

    if (return_value != nullptr) {
        *return_value = 0;
    }

    return 1;
}
#endif

inline void install_test_process_handlers()
{
    static const bool installed = [] {
        constexpr unsigned int sem_failcriticalerrors = 0x0001U;
        constexpr unsigned int sem_nogpfaultbox       = 0x0002U;
        constexpr unsigned int sem_noopenfileerrorbox = 0x8000U;

        SetErrorMode(sem_failcriticalerrors | sem_nogpfaultbox | sem_noopenfileerrorbox);

#if defined(_MSC_VER)
        _set_error_mode(_OUT_TO_STDERR);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportHook(test_crt_report_hook);
#endif

        return true;
    }();

    (void)installed;
}

} // namespace vnm_terminal::tests

namespace {

const bool vnm_terminal_test_process_handlers_installed = [] {
    vnm_terminal::tests::install_test_process_handlers();
    return true;
}();

} // namespace
#endif
