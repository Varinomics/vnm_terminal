#include <Windows.h>

#include <algorithm>
#include <exception>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr DWORD k_maximum_path_chars = 32768;
constexpr size_t k_maximum_command_chars = 32767;
constexpr std::wstring_view k_codex_config =
    L"-c \"shell_environment_policy.set.TERM='xterm-256color'\"";

struct Launcher_failure
{
    std::wstring message;
};

class Windows_handle
{
public:
    explicit Windows_handle(HANDLE value = nullptr)

    :
        m_value(value)
    {}
    ~Windows_handle()
    {
        if (m_value && m_value != INVALID_HANDLE_VALUE) {
            CloseHandle(m_value);
        }
    }

    Windows_handle(const Windows_handle&) = delete;
    Windows_handle& operator=(const Windows_handle&) = delete;

    HANDLE get() const { return m_value; }

private:
    HANDLE m_value;
};

[[noreturn]] void fail(const std::wstring& message)
{
    throw Launcher_failure{message};
}

[[noreturn]] void fail_windows(const std::wstring& operation)
{
    const DWORD error = GetLastError();
    wchar_t description[1024]{};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, description, (DWORD)std::size(description), nullptr);
    std::wstring message = operation + L" (Windows error " + std::to_wstring(error) + L")";
    if (length) {
        message += L": " + std::wstring(description, length);
    }
    fail(message);
}

std::wstring read_environment(const wchar_t* name)
{
    SetLastError(ERROR_SUCCESS);
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND || GetLastError() == ERROR_SUCCESS) {
            return {};
        }
        fail_windows(L"Could not read the terminal environment");
    }
    std::wstring value(needed, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), needed);
    if (length >= needed) {
        fail(L"The terminal environment changed while preparing Codex.");
    }
    value.resize(length);
    return value;
}

void set_environment(const wchar_t* name, const wchar_t* value)
{
    if (!SetEnvironmentVariableW(name, value)) {
        fail_windows(L"Could not prepare the Codex environment");
    }
}

bool equal_paths(std::wstring_view left, std::wstring_view right)
{
    return CompareStringOrdinal(
        left.data(), (int)left.size(), right.data(), (int)right.size(), TRUE) == CSTR_EQUAL;
}

std::wstring unquote_path(std::wstring_view path)
{
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') {
        path.remove_prefix(1);
        path.remove_suffix(1);
    }
    return std::wstring(path);
}

std::wstring absolute_path(std::wstring_view path)
{
    std::wstring input = unquote_path(path);
    if (input.empty()) {
        input = L".";
    }
    std::wstring resolved(k_maximum_path_chars, L'\0');
    const DWORD length = GetFullPathNameW(input.c_str(), (DWORD)resolved.size(), resolved.data(), nullptr);
    if (!length) {
        fail_windows(L"Could not resolve the Codex command path");
    }
    if (length >= resolved.size()) {
        fail(L"The Codex command path is too long.");
    }
    resolved.resize(length);
    std::replace(resolved.begin(), resolved.end(), L'/', L'\\');
    while (resolved.size() > 3 && resolved.back() == L'\\') {
        resolved.pop_back();
    }
    return resolved;
}

std::vector<std::wstring> split_paths(std::wstring_view paths)
{
    std::vector<std::wstring> entries;
    size_t start = 0;
    while (start <= paths.size()) {
        const size_t end = paths.find(L';', start);
        entries.emplace_back(paths.substr(start, end == paths.npos ? paths.size() - start : end - start));
        if (end == paths.npos) {
            break;
        }
        start = end + 1;
    }
    return entries;
}

std::wstring launcher_module_path()
{
    std::wstring module(k_maximum_path_chars, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), (DWORD)module.size());
    if (!length) {
        fail_windows(L"Could not locate the terminal Codex launcher");
    }
    if (length >= module.size()) {
        fail(L"The terminal Codex launcher path is too long.");
    }
    module.resize(length);
    return module;
}

std::wstring remove_launcher_from_path(const std::wstring& directory)
{
    const std::wstring original_path = read_environment(L"PATH");
    std::wstring child_path;
    bool first_entry = true;
    for (const std::wstring& entry : split_paths(original_path)) {
        if (equal_paths(absolute_path(entry), directory)) {
            continue;
        }
        if (!first_entry) {
            child_path += L';';
        }
        child_path += entry;
        first_entry = false;
    }
    set_environment(L"PATH", child_path.c_str());
    return child_path;
}

bool is_file(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring resolve_cmd_command(
    const wchar_t* name, const std::wstring& path, const std::wstring& launcher_directory)
{
    std::vector<std::wstring> directories;
    if (NeedCurrentDirectoryForExePathW(name)) {
        directories.emplace_back(L".");
    }
    const auto path_entries = split_paths(path);
    directories.insert(directories.end(), path_entries.begin(), path_entries.end());
    std::wstring path_extensions = read_environment(L"PATHEXT");
    if (path_extensions.empty()) {
        path_extensions = L".COM;.EXE;.BAT;.CMD";
    }
    const auto extensions = split_paths(path_extensions);
    for (const std::wstring& directory : directories) {
        const std::wstring resolved_directory = absolute_path(directory);
        if (equal_paths(resolved_directory, launcher_directory)) {
            continue;
        }
        const std::wstring prefix = resolved_directory + L"\\" + name;
        for (const std::wstring& extension : extensions) {
            const std::wstring candidate = prefix + extension;
            if (is_file(candidate)) {
                return candidate;
            }
        }
    }
    fail(std::wstring(name) + L" was not found on the original terminal PATH.");
}

std::wstring read_arguments_file(const wchar_t* path)
{
    Windows_handle file(CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        fail_windows(L"Could not open the Codex arguments file");
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) {
        fail_windows(L"Could not read the Codex arguments file size");
    }
    // A file avoids expanding the argument tail beyond CreateProcessW's command-line limit.
    if (size.QuadPart > (LONGLONG)(k_maximum_command_chars * sizeof(wchar_t))) {
        fail(L"The Codex arguments exceed the Windows process limit.");
    }
    const DWORD byte_count = (DWORD)size.QuadPart;
    if (byte_count % sizeof(wchar_t)) {
        fail(L"The Codex arguments file is not UTF-16LE.");
    }
    std::wstring arguments(byte_count / sizeof(wchar_t), L'\0');
    DWORD bytes_read = 0;
    if (!ReadFile(file.get(), arguments.data(), byte_count, &bytes_read, nullptr)) {
        fail_windows(L"Could not read the Codex arguments file");
    }
    if (bytes_read != byte_count) {
        fail(L"The Codex arguments file changed while it was being read.");
    }
    if (arguments.find(L'\0') != arguments.npos) {
        fail(L"The Codex arguments file contains a NUL character.");
    }
    return arguments;
}

bool is_argument_space(wchar_t character)
{
    return character == L' ' || character == L'\t';
}

std::wstring original_argument_tail()
{
    std::wstring_view command = GetCommandLineW();
    size_t offset = 0;
    while (offset < command.size() && is_argument_space(command[offset])) {
        ++offset;
    }
    // CRT parses argv[0] using quote boundaries without backslash escaping.
    bool quoted = false;
    while (offset < command.size()) {
        const wchar_t character = command[offset];
        if (!quoted && is_argument_space(character)) {
            break;
        }
        if (character == L'"') {
            quoted = !quoted;
        }
        ++offset;
    }
    while (offset < command.size() && is_argument_space(command[offset])) {
        ++offset;
    }
    return std::wstring(command.substr(offset));
}

void prepare_terminal_identity()
{
    constexpr const wchar_t* identity_names[] = {
        L"TERM_PROGRAM", L"TERM_PROGRAM_VERSION", L"GHOSTTY_RESOURCES_DIR",
        L"WEZTERM_VERSION", L"WEZTERM_EXECUTABLE", L"ITERM_SESSION_ID",
        L"ITERM_PROFILE", L"ITERM_PROFILE_NAME", L"TERM_SESSION_ID",
        L"KITTY_WINDOW_ID", L"ALACRITTY_SOCKET", L"KONSOLE_VERSION",
        L"GNOME_TERMINAL_SCREEN", L"VTE_VERSION", L"WT_SESSION",
    };
    for (const wchar_t* name : identity_names) {
        set_environment(name, nullptr);
    }
    set_environment(L"TERM", L"vnm-terminal-sixel");
}

std::wstring quote_program(const std::wstring& program)
{
    // Resolved Windows filenames cannot contain quotes or end with a directory separator.
    return L"\"" + program + L"\"";
}

Windows_handle inherit_standard_handle(DWORD identifier)
{
    const HANDLE original = GetStdHandle(identifier);
    if (!original || original == INVALID_HANDLE_VALUE) {
        return Windows_handle(original);
    }
    HANDLE inherited = nullptr;
    if (!DuplicateHandle(
            GetCurrentProcess(), original, GetCurrentProcess(), &inherited,
            0, TRUE, DUPLICATE_SAME_ACCESS))
    {
        fail_windows(L"Could not inherit the Codex terminal handles");
    }
    return Windows_handle(inherited);
}

BOOL WINAPI keep_launcher_waiting(DWORD event)
{
    // The attached child receives these events itself; the launcher must retain its exit status.
    return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT;
}

DWORD launch_child(const std::wstring& program, const std::wstring& arguments)
{
    const size_t extension_start = program.find_last_of(L'.');
    const std::wstring_view extension = extension_start == program.npos
        ? std::wstring_view{} : std::wstring_view(program).substr(extension_start);
    const bool batch_file = equal_paths(extension, L".cmd") || equal_paths(extension, L".bat");
    std::wstring application = program;
    std::wstring command = quote_program(program);
    if (!arguments.empty()) {
        command += L" " + arguments;
    }
    if (batch_file) {
        application = unquote_path(read_environment(L"COMSPEC"));
        if (application.empty()) {
            fail(L"COMSPEC is missing; the original Codex batch command cannot run.");
        }
        // /s removes only the outer pair. Batch arguments retain cmd's expansion and quoting semantics.
        command = quote_program(application) + L" /d /s /v:off /c \"" + command + L"\"";
    }
    if (command.size() >= k_maximum_command_chars) {
        fail(L"The Codex command line exceeds the Windows process limit.");
    }
    DWORD console_process = 0;
    const bool attached_console = GetConsoleProcessList(&console_process, 1) != 0;
    if (!attached_console && GetLastError() != ERROR_INVALID_HANDLE) {
        fail_windows(L"Could not identify the Codex console attachment");
    }
    if (attached_console && !SetConsoleCtrlHandler(keep_launcher_waiting, TRUE)) {
        fail_windows(L"Could not preserve Codex console control handling");
    }
    Windows_handle input  = inherit_standard_handle(STD_INPUT_HANDLE);
    Windows_handle output = inherit_standard_handle(STD_OUTPUT_HANDLE);
    Windows_handle error  = inherit_standard_handle(STD_ERROR_HANDLE);
    STARTUPINFOW startup{};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = input.get();
    startup.hStdOutput = output.get();
    startup.hStdError  = error.get();
    PROCESS_INFORMATION child{};
    // ConPTY is an attached console. Pipe-only launches must not allocate a console window.
    const DWORD creation_flags = attached_console ? 0 : CREATE_NO_WINDOW;
    if (!CreateProcessW(
            application.c_str(), command.data(), nullptr, nullptr, TRUE,
            creation_flags, nullptr, nullptr, &startup, &child))
    {
        fail_windows(L"Could not start the original Codex command " + program);
    }
    Windows_handle process(child.hProcess);
    Windows_handle thread(child.hThread);
    if (WaitForSingleObject(process.get(), INFINITE) != WAIT_OBJECT_0) {
        fail_windows(L"Could not wait for Codex to finish");
    }
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process.get(), &exit_code)) {
        fail_windows(L"Could not read the Codex exit status");
    }
    return exit_code;
}

void report_failure(const std::wstring& failure)
{
    const std::wstring message = L"Terminal Codex launcher: " + failure + L"\r\n";
    const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    DWORD unused = 0;
    DWORD mode = 0;
    if (GetConsoleMode(error, &mode)) {
        WriteConsoleW(error, message.data(), (DWORD)message.size(), &unused, nullptr);
        return;
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, message.data(), (int)message.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(needed, '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, message.data(), (int)message.size(), utf8.data(), needed, nullptr, nullptr);
    WriteFile(error, utf8.data(), (DWORD)utf8.size(), &unused, nullptr);
}

} // namespace

int wmain(int argument_count, wchar_t** arguments)
{
    try {
        std::wstring program;
        std::wstring child_arguments;
        const std::wstring module = launcher_module_path();
        const size_t separator = module.find_last_of(L"\\/");
        const std::wstring directory = absolute_path(module.substr(0, separator));
        const std::wstring path = remove_launcher_from_path(directory);
        const std::wstring_view name = std::wstring_view(module).substr(separator + 1);
        if (equal_paths(name, L"codex.exe") || equal_paths(name, L"codex-pet.exe")) {
            const wchar_t* codex_name = equal_paths(name, L"codex.exe") ? L"codex" : L"codex-pet";
            program         = resolve_cmd_command(codex_name, path, directory);
            child_arguments = std::wstring(k_codex_config);
            const std::wstring original_tail = original_argument_tail();
            if (!original_tail.empty()) {
                child_arguments += L" " + original_tail;
            }
        }
        else
        if (argument_count == 4 && std::wstring_view(arguments[1]) == L"--arguments-file") {
            program         = absolute_path(arguments[2]);
            child_arguments = read_arguments_file(arguments[3]);
        }
        else {
            fail(L"Expected --arguments-file <command path> <UTF-16LE arguments file>.");
        }
        if (!is_file(program)) {
            fail(L"The original Codex command does not exist: " + program);
        }
        prepare_terminal_identity();
        return (int)launch_child(program, child_arguments);
    }
    catch (const Launcher_failure& failure) {
        report_failure(failure.message);
    }
    catch (const std::exception&) {
        report_failure(L"Could not allocate memory to launch Codex.");
    }
    return 1;
}
