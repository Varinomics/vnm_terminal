// Oracles for these fixtures:
//   - CommandLineToArgvW is the Windows parser the launcher's escaping must satisfy, so the
//     round-trip fixtures use the OS parser itself as the correctness oracle.
//   - CreateProcessW documents lpCommandLine as at most 32,767 wchar_t including the
//     terminating NUL. VNM_TERMINAL_MAX_CMDLINE encodes that limit, and the capacity
//     fixtures assert the assembly refuses to exceed it instead of writing past the buffer.
//   - show_last_error assembles a message with these appends and hands the buffer to
//     MessageBoxW without inspecting the return value, so NUL termination on the rejected
//     path is a requirement of the caller, not a stylistic preference.

#include "portable_launcher_text.h"
#include "helpers/test_check.h"

#include <windows.h>
#include <shellapi.h>

#include <cwchar>
#include <iostream>
#include <string>
#include <vector>

using vnm_terminal::test_helpers::check;

namespace {

std::string utf8_text(const std::wstring& text)
{
    if (text.empty()) {
        return std::string();
    }

    const int byte_count = WideCharToMultiByte(
        CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<std::size_t>(byte_count), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        bytes.data(),
        byte_count,
        nullptr,
        nullptr);
    return bytes;
}

// Assemble a command line the same way WinMain does: the resolved target as argv[0],
// then every forwarded argument separated by a single space.
bool assemble_command_line(
    const std::wstring&               target,
    const std::vector<std::wstring>&  arguments,
    std::vector<wchar_t>&             buffer,
    std::size_t&                      length)
{
    const std::size_t capacity = buffer.size();
    buffer[0] = L'\0';
    length    = 0;
    if (!portable_launcher_append_quoted_arg(
            buffer.data(), capacity, &length, target.c_str()))
    {
        return false;
    }

    for (const std::wstring& argument : arguments) {
        if (!portable_launcher_append_text(buffer.data(), capacity, &length, L" ") ||
            !portable_launcher_append_quoted_arg(
                buffer.data(), capacity, &length, argument.c_str()))
        {
            return false;
        }
    }

    return true;
}

bool check_round_trip(const std::vector<std::wstring>& arguments, const std::string& id)
{
    const std::wstring target = L"C:\\Program Files\\vnm\\vnm_terminal_runtime\\vnm_terminal.exe";

    std::vector<wchar_t> buffer(VNM_TERMINAL_MAX_CMDLINE + 1, L'\0');
    std::size_t          length = 0;
    if (!check(assemble_command_line(target, arguments, buffer, length),
        id + ": assembly must fit"))
    {
        return false;
    }

    int     parsed_count = 0;
    LPWSTR* parsed       = CommandLineToArgvW(buffer.data(), &parsed_count);
    if (!check(parsed != nullptr, id + ": CommandLineToArgvW must parse the assembled line")) {
        return false;
    }

    bool ok = check(
        parsed_count == static_cast<int>(arguments.size()) + 1,
        id + ": parsed argument count must match");
    if (ok) {
        ok &= check(std::wstring(parsed[0]) == target, id + ": argv[0] must round-trip");
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const std::wstring recovered(parsed[i + 1]);
            if (recovered != arguments[i]) {
                std::cerr << "FAIL: " << id << ": argument " << i << " must round-trip"
                    << " expected=[" << utf8_text(arguments[i])
                    << "] actual=["   << utf8_text(recovered) << "]\n";
                ok = false;
            }
        }
    }

    LocalFree(parsed);
    return ok;
}

bool test_round_trip_arguments()
{
    bool ok = true;
    ok &= check_round_trip({}, "no_arguments");
    ok &= check_round_trip({L"--rows", L"40"}, "plain_arguments");
    ok &= check_round_trip({L"C:\\path with spaces\\file.txt"}, "spaces");
    ok &= check_round_trip({L"C:\\trailing\\dir\\"}, "trailing_backslash");
    ok &= check_round_trip({L"C:\\trailing\\dir with space\\"}, "quoted_trailing_backslash");
    ok &= check_round_trip({L"--title=say \"hello\""}, "embedded_quotes");
    ok &= check_round_trip({L"a\\\\\\\"b"}, "backslashes_before_quote");
    ok &= check_round_trip({L""}, "empty_argument");
    ok &= check_round_trip({L"--tab", L"", L"x y"}, "mixed_arguments");
    return ok;
}

// The trailing-backslash run of a quoted argument is doubled, so an argument that is
// itself far shorter than the buffer can still overflow it. This is the case that used to
// write past the end of WinMain's command_line.
bool test_expansion_heavy_argument_is_rejected()
{
    const std::size_t backslash_count = (VNM_TERMINAL_MAX_CMDLINE / 2) + 100U;
    std::wstring      argument        = L"x ";
    argument.append(backslash_count, L'\\');

    std::vector<wchar_t> buffer(VNM_TERMINAL_MAX_CMDLINE + 1, L'\0');
    const wchar_t        sentinel = L'\x2603';
    buffer.push_back(sentinel);

    std::size_t length = 0;
    buffer[0] = L'\0';
    const bool appended = portable_launcher_append_quoted_arg(
        buffer.data(), VNM_TERMINAL_MAX_CMDLINE + 1, &length, argument.c_str()) != 0;

    bool ok = true;
    ok &= check(
        argument.size() < VNM_TERMINAL_MAX_CMDLINE,
        "expansion_heavy: the raw argument must fit, only its escaped form must not");
    ok &= check(!appended, "expansion_heavy: doubled trailing backslashes must be rejected");
    ok &= check(
        length <= VNM_TERMINAL_MAX_CMDLINE,
        "expansion_heavy: the write offset must stay inside the buffer");
    ok &= check(
        buffer.back() == sentinel,
        "expansion_heavy: no write may pass the end of the buffer");
    return ok;
}

// GetModuleFileNameW can report a path up to VNM_TERMINAL_MAX_PATH_CHARS characters, and
// the runtime suffix is appended to its directory, so the composed target path can exceed
// the path buffer. It must fail closed rather than truncate to a different file.
bool test_max_path_target_composition()
{
    const std::wstring runtime_suffix = L"\\vnm_terminal_runtime\\vnm_terminal.exe";
    const std::size_t  capacity       = VNM_TERMINAL_MAX_PATH_CHARS + 1;

    std::vector<wchar_t> buffer(capacity, L'\0');
    const wchar_t        sentinel = L'\x2603';
    buffer.push_back(sentinel);

    const std::wstring longest_directory(VNM_TERMINAL_MAX_PATH_CHARS - 1U, L'a');
    std::size_t        length = 0;
    buffer[0] = L'\0';
    const bool longest_appended =
        portable_launcher_append_text(
            buffer.data(), capacity, &length, longest_directory.c_str()) != 0 &&
        portable_launcher_append_text(
            buffer.data(), capacity, &length, runtime_suffix.c_str()) != 0;

    bool ok = true;
    ok &= check(!longest_appended, "max_path: an over-long runtime path must be rejected");
    ok &= check(buffer.back() == sentinel, "max_path: no write may pass the end of the buffer");

    // The longest directory that still leaves room for the suffix and the NUL.
    const std::wstring fitting_directory(
        VNM_TERMINAL_MAX_PATH_CHARS - runtime_suffix.size(), L'a');
    length    = 0;
    buffer[0] = L'\0';
    const bool fitting_appended =
        portable_launcher_append_text(
            buffer.data(), capacity, &length, fitting_directory.c_str()) != 0 &&
        portable_launcher_append_text(
            buffer.data(), capacity, &length, runtime_suffix.c_str()) != 0;

    ok &= check(fitting_appended, "max_path: a runtime path that exactly fits must be accepted");
    ok &= check(
        length == VNM_TERMINAL_MAX_PATH_CHARS,
        "max_path: the accepted path must fill the buffer exactly");
    ok &= check(
        std::wstring(buffer.data()) == fitting_directory + runtime_suffix,
        "max_path: the accepted path must be the exact composition");
    ok &= check(buffer.back() == sentinel, "max_path: no write may pass the end of the buffer");
    return ok;
}

// One character past the capacity must be refused, and the refusal must not corrupt the
// buffer that the caller is about to abandon.
bool test_capacity_boundary()
{
    const std::size_t    capacity = 8U;
    std::vector<wchar_t> buffer(capacity, L'\0');
    const wchar_t        sentinel = L'\x2603';
    buffer.push_back(sentinel);

    std::size_t length = 0;
    buffer[0] = L'\0';
    bool ok = true;
    ok &= check(
        portable_launcher_append_text(buffer.data(), capacity, &length, L"1234567") != 0,
        "boundary: capacity minus one characters must be accepted");
    ok &= check(length == capacity - 1U, "boundary: the buffer must be full");
    ok &= check(
        std::wstring(buffer.data()) == L"1234567",
        "boundary: the accepted text must be intact and terminated");
    ok &= check(
        portable_launcher_append_text(buffer.data(), capacity, &length, L"8") == 0,
        "boundary: one character past capacity must be rejected");
    ok &= check(buffer.back() == sentinel, "boundary: no write may pass the end of the buffer");
    return ok;
}

// A rejected append must still leave a readable string behind, because show_last_error
// displays the buffer without inspecting the return value.
bool test_rejected_append_leaves_the_buffer_terminated()
{
    const std::size_t    capacity = 8U;
    std::vector<wchar_t> buffer(capacity, L'\x2603');
    std::size_t          length = 0;
    buffer[0] = L'\0';

    bool ok = true;
    ok &= check(
        portable_launcher_append_text(buffer.data(), capacity, &length, L"123456789") == 0,
        "terminated: an over-long text must be rejected");
    ok &= check(
        std::wstring(buffer.data()) == L"1234567",
        "terminated: the rejected text append must leave the accepted prefix terminated");
    ok &= check(
        length == std::wcslen(buffer.data()),
        "terminated: the offset must index the terminator after a rejected text append");

    buffer.assign(capacity, L'\x2603');
    length    = 0;
    buffer[0] = L'\0';
    ok &= check(
        portable_launcher_append_quoted_arg(buffer.data(), capacity, &length, L"a b c d e") == 0,
        "terminated: an over-long quoted argument must be rejected");
    ok &= check(
        length == std::wcslen(buffer.data()),
        "terminated: the offset must index the terminator after a rejected quoted append");
    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_round_trip_arguments();
    ok &= test_expansion_heavy_argument_is_rejected();
    ok &= test_max_path_target_composition();
    ok &= test_capacity_boundary();
    ok &= test_rejected_append_leaves_the_buffer_terminated();

    if (!ok) {
        std::cerr << "portable_launcher_text_tests: FAILED\n";
        return 1;
    }

    std::cout << "portable_launcher_text_tests: OK\n";
    return 0;
}
