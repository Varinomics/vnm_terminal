#include "vnm_terminal/internal/terminal_canvas_fixture_contract.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace term = vnm_terminal::internal;

namespace {

enum class hex_decode_status_t
{
    OK,
    INVALID_HEX,
};

struct hex_decode_result_t
{
    hex_decode_status_t        status = hex_decode_status_t::OK;
    std::vector<unsigned char> bytes;
};

enum class behavior_smoke_payload_status_t
{
    OK,
    UNKNOWN_NAME,
    EMPTY_PAYLOAD,
    INVALID_HEX,
    INVALID_REPEAT_COUNT,
};

struct behavior_smoke_payload_result_t
{
    behavior_smoke_payload_status_t status =
        behavior_smoke_payload_status_t::UNKNOWN_NAME;
    std::vector<unsigned char> bytes;
};

struct console_size_t
{
    int                        rows    = 0;
    int                        columns = 0;
};

struct gated_stream_arguments_t
{
    int                        count           = 0;
    int                        resize_rows     = 0;
    int                        resize_columns  = 0;
    int                        resize_pad_rows = 0;
};

void print_usage()
{
    std::cout
        << "usage: vnm_terminal_canvas_fixture --list\n"
        << "       vnm_terminal_canvas_fixture --list-behavior-smokes\n"
        << "       vnm_terminal_canvas_fixture --scenario "
        << term::terminal_canvas_fixture_scenario_name() << '\n'
        << "       vnm_terminal_canvas_fixture --behavior-smoke <name>\n"
        << "       vnm_terminal_canvas_fixture --interactive-scenario "
        << term::terminal_canvas_fixture_scenario_name()
        << " [--expect-initial-size <rows> <columns>]"
        << " [--checkpoint-after-enable-input-modes <path>]\n"
        << "       vnm_terminal_canvas_fixture --shell-like-smoke\n"
        << "       vnm_terminal_canvas_fixture --hold-open\n"
        << "       vnm_terminal_canvas_fixture --hold-open-pid\n"
        << "       vnm_terminal_canvas_fixture --hold-open-pid-no-read\n"
        << "       vnm_terminal_canvas_fixture --hold-open-no-read\n"
        << "       vnm_terminal_canvas_fixture --utf8-payload\n"
        << "       vnm_terminal_canvas_fixture --sync-raw-resize-gate <checkpoint-path>\n"
        << "       vnm_terminal_canvas_fixture --quick-exit\n"
        << "       vnm_terminal_canvas_fixture --echo-argv [args...]\n";
}

void print_list()
{
    std::cout << term::terminal_canvas_fixture_scenario_name() << '\n';
}

void print_behavior_smoke_list()
{
    for (const term::terminal_canvas_fixture_behavior_smoke_case_t& smoke_case :
        term::terminal_canvas_fixture_behavior_smoke_cases())
    {
        std::cout << smoke_case.name << '\n';
    }
}

bool print_record(const term::terminal_canvas_fixture_record_t& record)
{
    const std::string_view kind_name = term::terminal_canvas_fixture_kind_name(record.kind);
    if (kind_name.empty()) {
        return false;
    }

    std::cout << "record "
        << kind_name << ' '
        << record.label;

    switch (record.kind) {
        case term::Terminal_canvas_fixture_record_kind::OUTPUT:
        case term::Terminal_canvas_fixture_record_kind::EXPECT_INPUT:
            std::cout << ' ' << record.payload_hex;
            break;
        case term::Terminal_canvas_fixture_record_kind::RESIZE:
            std::cout << ' ' << record.rows << ' ' << record.columns;
            break;
        case term::Terminal_canvas_fixture_record_kind::REPEAT_OUTPUT:
            std::cout << ' ' << record.repeat_count << ' ' << record.payload_hex;
            break;
        case term::Terminal_canvas_fixture_record_kind::EXIT:
            std::cout << ' ' << record.exit_code;
            break;
        case term::Terminal_canvas_fixture_record_kind::CHECKPOINT:
            break;
    }

    std::cout << '\n';
    return true;
}

bool print_scenario()
{
    std::cout << "vnm-fixture-protocol "
        << term::k_terminal_canvas_fixture_protocol_version << '\n';
    std::cout << "scenario " << term::terminal_canvas_fixture_scenario_name() << '\n';

    for (const term::terminal_canvas_fixture_record_t& record :
        term::terminal_canvas_fixture_contract_script())
    {
        if (!print_record(record)) {
            return false;
        }
    }

    return true;
}

int echo_argv(int argc, char** argv)
{
    for (int index = 1; index < argc; ++index) {
        std::cout << "argv[" << index << "]=" << argv[index] << '\n';
    }

    return 0;
}

bool argument_equals(const char* argument, std::string_view expected)
{
    return std::string_view(argument) == expected;
}

std::optional<int> parse_int(const char* argument)
{
    try {
        return std::stoi(std::string(argument));
    }
    catch (const std::exception&) {
        return std::nullopt;
    }
}

hex_decode_result_t decode_hex(std::string_view hex)
{
    hex_decode_result_t result;
    result.bytes.reserve(hex.size() / 2U);

    const auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }

        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }

        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }

        return -1;
    };

    if ((hex.size() % 2U) != 0U) {
        result.status = hex_decode_status_t::INVALID_HEX;
        return result;
    }

    for (std::size_t i = 0U; i < hex.size(); i += 2U) {
        const int high = hex_value(hex[i]);
        const int low  = hex_value(hex[i + 1U]);
        if (high < 0 || low < 0) {
            result.status = hex_decode_status_t::INVALID_HEX;
            result.bytes.clear();
            return result;
        }

        result.bytes.push_back(static_cast<unsigned char>((high << 4) | low));
    }

    return result;
}

behavior_smoke_payload_result_t behavior_smoke_payload(std::string_view name)
{
    for (const term::terminal_canvas_fixture_behavior_smoke_case_t& smoke_case :
        term::terminal_canvas_fixture_behavior_smoke_cases())
    {
        if (smoke_case.name != name) {
            continue;
        }

        if (smoke_case.payload_hex.empty()) {
            return {behavior_smoke_payload_status_t::EMPTY_PAYLOAD, {}};
        }

        if (smoke_case.repeat_count <= 0) {
            return {behavior_smoke_payload_status_t::INVALID_REPEAT_COUNT, {}};
        }

        const hex_decode_result_t unit = decode_hex(smoke_case.payload_hex);
        if (unit.status != hex_decode_status_t::OK) {
            return {behavior_smoke_payload_status_t::INVALID_HEX, {}};
        }

        std::vector<unsigned char> payload;
        payload.reserve(
            unit.bytes.size() * static_cast<std::size_t>(smoke_case.repeat_count));
        for (int i = 0; i < smoke_case.repeat_count; ++i) {
            payload.insert(payload.end(), unit.bytes.begin(), unit.bytes.end());
        }
        return {behavior_smoke_payload_status_t::OK, std::move(payload)};
    }

    return {behavior_smoke_payload_status_t::UNKNOWN_NAME, {}};
}

std::string encode_hex(const std::vector<unsigned char>& bytes)
{
    static constexpr char digits[] = "0123456789abcdef";

    std::string hex;
    hex.reserve(bytes.size() * 2U);
    for (unsigned char byte : bytes) {
        hex.push_back(digits[(byte >> 4) & 0x0fU]);
        hex.push_back(digits[byte & 0x0fU]);
    }

    return hex;
}

bool configure_binary_stdout()
{
#if defined(_WIN32)
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
        std::cerr << "failed to set binary stdout mode\n";
        return false;
    }

    HANDLE output       = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  console_mode = 0U;
    // Defensive for direct console-host runs; exact ConPTY byte checks below
    // come from binary stdout writes, not code-page transcoding.
    if (output != INVALID_HANDLE_VALUE &&
        GetConsoleMode(output, &console_mode) &&
        !SetConsoleOutputCP(CP_UTF8))
    {
        std::cerr << "failed to set UTF-8 console output code page\n";
        return false;
    }
#endif

    return true;
}

bool write_all_stdout(const std::vector<unsigned char>& bytes)
{
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const std::size_t written =
            std::fwrite(bytes.data() + offset, 1U, bytes.size() - offset, stdout);
        if (written == 0U) {
            return false;
        }

        offset += written;
    }

    std::fflush(stdout);
    return true;
}

bool write_all_stdout(std::string_view text)
{
    std::size_t offset = 0U;
    while (offset < text.size()) {
        const std::size_t written =
            std::fwrite(text.data() + offset, 1U, text.size() - offset, stdout);
        if (written == 0U) {
            return false;
        }

        offset += written;
    }

    std::fflush(stdout);
    return true;
}

bool write_stdout_line(std::string_view text)
{
    std::string line(text);
    line += "\r\n";
    return write_all_stdout(line);
}

bool read_exact_stdin(std::vector<unsigned char>& bytes)
{
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const std::size_t count =
            std::fread(bytes.data() + offset, 1U, bytes.size() - offset, stdin);
        if (count == 0U) {
            return false;
        }

        offset += count;
    }

    return true;
}

bool configure_interactive_console()
{
#if defined(_WIN32)
    if (_setmode(_fileno(stdin), _O_BINARY)  == -1 ||
        _setmode(_fileno(stdout), _O_BINARY) == -1)
    {
        std::cerr << "failed to set binary stdio mode\n";
        return false;
    }

    HANDLE input      = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  input_mode = 0U;
    if (input == INVALID_HANDLE_VALUE ||
        !GetConsoleMode(input, &input_mode))
    {
        std::cerr << "failed to read input console mode\n";
        return false;
    }

    input_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    input_mode &= ~ENABLE_LINE_INPUT;
    input_mode &= ~ENABLE_ECHO_INPUT;
    input_mode &= ~ENABLE_PROCESSED_INPUT;
    if (!SetConsoleMode(input, input_mode)) {
        std::cerr << "failed to set input console mode\n";
        return false;
    }

    HANDLE output      = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  output_mode = 0U;
    if (output == INVALID_HANDLE_VALUE ||
        !GetConsoleMode(output, &output_mode))
    {
        std::cerr << "failed to read output console mode\n";
        return false;
    }

    output_mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(output, output_mode)) {
        std::cerr << "failed to set output console mode\n";
        return false;
    }
#else
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::cerr << "interactive fixture requires terminal stdio\n";
        return false;
    }

    termios input_mode{};
    if (tcgetattr(STDIN_FILENO, &input_mode) != 0) {
        std::cerr << "failed to read input terminal mode\n";
        return false;
    }

    input_mode.c_iflag     &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    input_mode.c_lflag     &= ~(ECHO | ICANON | IEXTEN | ISIG);
    input_mode.c_cflag     &= ~(CSIZE | PARENB);
    input_mode.c_cflag     |= CS8;
    input_mode.c_cc[VMIN]   = 1;
    input_mode.c_cc[VTIME]  = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &input_mode) != 0) {
        std::cerr << "failed to set input terminal mode\n";
        return false;
    }

    termios output_mode{};
    if (tcgetattr(STDOUT_FILENO, &output_mode) != 0) {
        std::cerr << "failed to read output terminal mode\n";
        return false;
    }

    output_mode.c_oflag &= ~OPOST;
    if (tcsetattr(STDOUT_FILENO, TCSANOW, &output_mode) != 0) {
        std::cerr << "failed to set output terminal mode\n";
        return false;
    }
#endif

    return true;
}

bool configure_raw_sync_console()
{
#if defined(_WIN32)
    if (_setmode(_fileno(stdin), _O_BINARY)  == -1 ||
        _setmode(_fileno(stdout), _O_BINARY) == -1)
    {
        std::cerr << "failed to set binary stdio mode\n";
        return false;
    }

    HANDLE input      = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  input_mode = 0U;
    if (input == INVALID_HANDLE_VALUE ||
        !GetConsoleMode(input, &input_mode))
    {
        std::cerr << "failed to read input console mode\n";
        return false;
    }

    input_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    input_mode &= ~ENABLE_LINE_INPUT;
    input_mode &= ~ENABLE_ECHO_INPUT;
    input_mode &= ~ENABLE_PROCESSED_INPUT;
    if (!SetConsoleMode(input, input_mode)) {
        std::cerr << "failed to set input console mode\n";
        return false;
    }

    HANDLE output      = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  output_mode = 0U;
    if (output == INVALID_HANDLE_VALUE ||
        !GetConsoleMode(output, &output_mode))
    {
        std::cerr << "failed to read output console mode\n";
        return false;
    }

    output_mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    output_mode &= ~ENABLE_PROCESSED_OUTPUT;
    if (!SetConsoleMode(output, output_mode)) {
        std::cerr << "failed to set raw output console mode\n";
        return false;
    }

    return true;
#else
    return configure_interactive_console();
#endif
}

bool console_size_matches(int rows, int columns)
{
#if defined(_WIN32)
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (output == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(output, &info)) {
        return false;
    }

    const int window_rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    const int window_cols = info.srWindow.Right - info.srWindow.Left + 1;
    if (window_rows   <= 0 || window_cols <= 0 || info.dwSize.Y < window_rows ||
        info.dwSize.X <  window_cols)
    {
        return false;
    }

    return window_rows == rows && window_cols == columns;
#else
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0) {
        return false;
    }

    return
        size.ws_row                   >  0    &&
        size.ws_col                   >  0    &&
        static_cast<int>(size.ws_row) == rows &&
        static_cast<int>(size.ws_col) == columns;
#endif
}

std::optional<console_size_t> current_console_size()
{
#if defined(_WIN32)
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (output == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(output, &info)) {
        return std::nullopt;
    }

    const int window_rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    const int window_cols = info.srWindow.Right - info.srWindow.Left + 1;
    if (window_rows   <= 0 || window_cols <= 0 || info.dwSize.Y < window_rows ||
        info.dwSize.X <  window_cols)
    {
        return std::nullopt;
    }

    return console_size_t{window_rows, window_cols};
#else
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0) {
        return std::nullopt;
    }

    if (size.ws_row == 0 || size.ws_col == 0) {
        return std::nullopt;
    }

    return console_size_t{
        static_cast<int>(size.ws_row),
        static_cast<int>(size.ws_col),
    };
#endif
}

bool wait_for_console_size(int rows, int columns)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        if (console_size_matches(rows, columns)) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    while (std::chrono::steady_clock::now() < deadline);

    return false;
}

bool write_checkpoint_file(const std::string& path)
{
    if (path.empty()) {
        return true;
    }

    std::ofstream file(path, std::ios::binary);
    file << "ready\n";
    return file.good();
}

int run_interactive_scenario(
    int                expected_initial_rows,
    int                expected_initial_columns,
    const std::string& checkpoint_after_enable_input_modes)
{
    if (!configure_interactive_console()) {
        return 19;
    }

    if (expected_initial_rows    > 0 &&
        expected_initial_columns > 0 &&
        !wait_for_console_size(expected_initial_rows, expected_initial_columns))
    {
        std::cerr << "initial terminal size did not match "
            << expected_initial_rows << "x" << expected_initial_columns << '\n';
        return 4;
    }

    for (const term::terminal_canvas_fixture_record_t& record :
        term::terminal_canvas_fixture_contract_script())
    {
        switch (record.kind) {
            case term::Terminal_canvas_fixture_record_kind::CHECKPOINT:
                break;

            case term::Terminal_canvas_fixture_record_kind::OUTPUT:
        {
                                    const hex_decode_result_t decoded = decode_hex(record.payload_hex);
                                    if (decoded.status != hex_decode_status_t::OK ||
                                        decoded.bytes.empty() ||
                                        !write_all_stdout(decoded.bytes))
                                    {
                                        return 5;
                                    }
                                    if (record.label == term::k_terminal_canvas_fixture_enable_input_modes_label &&
                                        !write_checkpoint_file(checkpoint_after_enable_input_modes))
                                    {
                                        return 14;
                                    }
                                    break;
        }

            case term::Terminal_canvas_fixture_record_kind::EXPECT_INPUT:
        {
                                    const hex_decode_result_t expected = decode_hex(record.payload_hex);
                                    if (expected.status != hex_decode_status_t::OK || expected.bytes.empty()) {
                                        return 6;
                                    }

                                    std::vector<unsigned char> actual(expected.bytes.size());
                                    if (!read_exact_stdin(actual) || actual != expected.bytes) {
                                        std::cerr << "input mismatch for " << record.label
                                            << ": expected " << encode_hex(expected.bytes)
                                            << ", got " << encode_hex(actual) << '\n';
                                        return 6;
                                    }
                                    break;
        }

            case term::Terminal_canvas_fixture_record_kind::RESIZE:
                if (!wait_for_console_size(record.rows, record.columns)) {
                    std::cerr << "resize did not reach "
                        << record.rows << "x" << record.columns << '\n';
                    return 7;
                }
                break;

            case term::Terminal_canvas_fixture_record_kind::REPEAT_OUTPUT:
        {
                                    const hex_decode_result_t decoded = decode_hex(record.payload_hex);
                                    if (decoded.status != hex_decode_status_t::OK || decoded.bytes.empty()) {
                                        return 8;
                                    }

                                    for (int i = 0; i < record.repeat_count; ++i) {
                                        if (!write_all_stdout(decoded.bytes)) {
                                            return 9;
                                        }
                                    }
                                    break;
        }

            case term::Terminal_canvas_fixture_record_kind::EXIT:
                return record.exit_code;

            default:
                return 10;
        }
    }

    return 10;
}

#if defined(_WIN32)
BOOL WINAPI control_event_handler(DWORD control_type);
#endif

bool line_starts_with_command(std::string_view line, std::string_view command)
{
    return line == command ||
        (line.size() > command.size() &&
            line.substr(0U, command.size()) == command &&
        line[command.size()] == ' ');
}

constexpr int k_shell_line_ready = -1;

std::string_view command_argument(std::string_view line, std::string_view command)
{
    if (line.size() <= command.size()) {
        return {};
    }

    return line.substr(command.size() + 1U);
}

std::optional<int> parse_bounded_command_count(std::string_view text, int max_count)
{
    try {
        std::size_t parsed_size = 0U;
        const int   value       = std::stoi(std::string(text), &parsed_size);
        if (parsed_size != text.size() || value <= 0 || value > max_count) {
            return std::nullopt;
        }

        return value;
    }
    catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<gated_stream_arguments_t> parse_gated_stream_arguments(
    std::string_view   text,
    int                max_count)
{
    std::istringstream stream{std::string(text)};
    gated_stream_arguments_t arguments;
    std::string trailing_text;
    if (!(stream >> arguments.count
        >> arguments.resize_rows
        >> arguments.resize_columns
        >> arguments.resize_pad_rows) ||
        (stream >> trailing_text))
    {
        return std::nullopt;
    }

    if (arguments.count <= 0       || arguments.count >  max_count  ||
        arguments.resize_rows <= 0 || arguments.resize_columns <= 0 ||
        arguments.resize_pad_rows <= 0)
    {
        return std::nullopt;
    }

    return arguments;
}

bool write_shell_prompt()
{
    return write_all_stdout(term::terminal_canvas_fixture_shell_like_smoke_contract().prompt);
}

bool write_shell_size()
{
    const std::optional<console_size_t> size = current_console_size();
    if (!size.has_value()) {
        return write_stdout_line("size unavailable");
    }

    std::ostringstream line;
    line << term::terminal_canvas_fixture_shell_like_smoke_contract().size_prefix
        << size->rows << 'x' << size->columns;
    return write_stdout_line(line.str());
}

std::string shell_stream_line(int row)
{
    std::ostringstream line;
    line << "stream-row-" << std::setw(3) << std::setfill('0') << row;
    return line.str();
}

bool write_shell_stream_range(int first_row, int last_row)
{
    for (int row = first_row; row <= last_row; ++row) {
        if (!write_stdout_line(shell_stream_line(row))) {
            return false;
        }
    }

    return true;
}

bool write_shell_stream(int count)
{
    return write_shell_stream_range(1, count);
}

bool write_shell_gated_resized_marker(
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t&
                           contract,
    int                    rows,
    int                    columns)
{
    std::ostringstream line;
    line << contract.gated_stream_resized_prefix << rows << 'x' << columns;
    return write_stdout_line(line.str());
}

int read_shell_line(std::string& line, bool& skip_lf_after_cr)
{
    line.clear();
    for (;;) {
        unsigned char     byte  = 0U;
        const std::size_t count = std::fread(&byte, 1U, 1U, stdin);
        if (count == 0U) {
            if (std::feof(stdin)) {
                return 0;
            }

            if (std::ferror(stdin)) {
                return 43;
            }

            return 44;
        }

        if (byte == 0x03U) {
            return 130;
        }

        if (byte == '\n' && skip_lf_after_cr) {
            skip_lf_after_cr = false;
            continue;
        }

        skip_lf_after_cr = false;
        if (byte == '\r' || byte == '\n') {
            if (byte == '\r') {
                skip_lf_after_cr = true;
            }

            return k_shell_line_ready;
        }

        line.push_back(static_cast<char>(byte));
    }
}

int run_shell_gated_stream(
    const gated_stream_arguments_t&    arguments,
    bool&                              skip_lf_after_cr)
{
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();

    if (!write_stdout_line(shell_stream_line(1))) {
        return 49;
    }

    // Keep row 1 out of the visible viewport before the resize storm, so a
    // ConPTY repaint cannot masquerade as another authored stream row.
    for (int row = 0; row < arguments.resize_pad_rows; ++row) {
        if (!write_stdout_line("stream-gated-pad")) {
            return 50;
        }
    }

    if (!write_stdout_line(contract.gated_stream_ready_output)) {
        return 51;
    }

    if (!wait_for_console_size(arguments.resize_rows, arguments.resize_columns)) {
        return 52;
    }

    if (!write_shell_gated_resized_marker(
            contract, arguments.resize_rows, arguments.resize_columns))
    {
        return 53;
    }

    std::string line;
    for (;;) {
        const int read_result = read_shell_line(line, skip_lf_after_cr);
        if (read_result != k_shell_line_ready) {
            return read_result;
        }

        const std::string_view continue_line(line.data(), line.size());
        if (continue_line == contract.gated_stream_continue_command) {
            break;
        }

        if (line.empty()) {
            continue;
        }

        if (!write_stdout_line("stream-gated unexpected-command")) {
            return 54;
        }
        return write_shell_prompt() ? k_shell_line_ready : 55;
    }

    if (!write_shell_stream_range(2, arguments.count)) {
        return 56;
    }

    return write_shell_prompt() ? k_shell_line_ready : 57;
}

int run_shell_wait()
{
    if (!write_stdout_line(term::terminal_canvas_fixture_shell_like_smoke_contract().wait_output)) {
        return 27;
    }

    for (;;) {
        unsigned char     byte  = 0U;
        const std::size_t count = std::fread(&byte, 1U, 1U, stdin);
        if (count == 0U) {
            if (std::feof(stdin)) {
                return 0;
            }

            if (std::ferror(stdin)) {
                return 28;
            }

            return 29;
        }

        if (byte == 0x03U) {
            return 130;
        }
    }
}

int run_shell_command(std::string_view line, bool& skip_lf_after_cr)
{
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();

    if (line_starts_with_command(line, contract.echo_command)) {
        if (!write_stdout_line(command_argument(line, contract.echo_command))) {
            return 30;
        }
        return write_shell_prompt() ? -1 : 31;
    }

    if (line == contract.size_command) {
        if (!write_shell_size()) {
            return 32;
        }
        return write_shell_prompt() ? -1 : 33;
    }

    if (line_starts_with_command(line, contract.stream_command)) {
        const std::optional<int> count = parse_bounded_command_count(
            command_argument(line, contract.stream_command),
            contract.stream_max_count);
        if (!count.has_value()) {
            if (!write_stdout_line("stream invalid-count")) {
                return 34;
            }
            return write_shell_prompt() ? -1 : 35;
        }

        if (!write_shell_stream(*count)) {
            return 36;
        }
        return write_shell_prompt() ? -1 : 37;
    }

    if (line_starts_with_command(line, contract.gated_stream_command)) {
        const std::optional<gated_stream_arguments_t> arguments = parse_gated_stream_arguments(
            command_argument(line, contract.gated_stream_command),
            contract.stream_max_count);
        if (!arguments.has_value()) {
            if (!write_stdout_line("stream-gated invalid-count")) {
                return 58;
            }
            return write_shell_prompt() ? -1 : 59;
        }

        return run_shell_gated_stream(*arguments, skip_lf_after_cr);
    }

    if (line == contract.wait_command) {
        return run_shell_wait();
    }

    if (line == contract.exit_command) {
        return 0;
    }

    if (!line.empty()) {
        std::string message("unknown command: ");
        message.append(line.data(), line.size());
        if (!write_stdout_line(message)) {
            return 38;
        }
    }

    return write_shell_prompt() ? -1 : 39;
}

int run_shell_like_smoke()
{
    if (!configure_interactive_console()) {
        return 40;
    }

#if defined(_WIN32)
    if (!SetConsoleCtrlHandler(control_event_handler, TRUE)) {
        std::cerr << "failed to install console control handler\n";
        return 41;
    }
#endif

    if (!write_shell_prompt()) {
        return 42;
    }

    bool skip_lf_after_cr = false;
    for (;;) {
        std::string line;
        const int read_result = read_shell_line(line, skip_lf_after_cr);
        if (read_result != k_shell_line_ready) {
            return read_result;
        }

        const int command_result = run_shell_command(line, skip_lf_after_cr);
        if (command_result >= 0) {
            return command_result;
        }
    }
}

int run_hold_open_until_input_closes_or_interrupt()
{
    for (;;) {
        unsigned char     byte  = 0U;
        const std::size_t count = std::fread(&byte, 1U, 1U, stdin);
        if (count == 0U) {
            if (std::feof(stdin)) {
                return 12;
            }

            if (std::ferror(stdin)) {
                return 13;
            }

            std::cerr << "stdin returned no bytes without EOF or error\n";
            return 17;
        }

        if (byte == 0x03U) {
            return 130;
        }
    }
}

unsigned long current_process_id()
{
#if defined(_WIN32)
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

int run_hold_open()
{
    if (!configure_interactive_console()) {
        return 20;
    }
    const std::vector<unsigned char> ready = {
        'h', 'o', 'l', 'd', '-', 'o', 'p', 'e', 'n', '\n',
    };
    if (!write_all_stdout(ready)) {
        return 11;
    }

    return run_hold_open_until_input_closes_or_interrupt();
}

int run_hold_open_pid()
{
    if (!configure_interactive_console()) {
        return 45;
    }

    std::ostringstream ready;
    ready << "hold-open-pid " << current_process_id() << '\n';
    if (!write_all_stdout(ready.str())) {
        return 46;
    }

    return run_hold_open_until_input_closes_or_interrupt();
}

#if defined(_WIN32)
BOOL WINAPI control_event_handler(DWORD control_type)
{
    if (control_type == CTRL_C_EVENT) {
        ExitProcess(130U);
    }

    return FALSE;
}
#endif

int run_hold_open_no_read_with_marker(
    std::string_view   ready_marker,
    int                write_failure_code,
    int                timeout_exit_code)
{
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);

    HANDLE input      = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  input_mode = 0U;
    if (input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &input_mode)) {
        input_mode |= ENABLE_PROCESSED_INPUT;
        input_mode &= ~ENABLE_LINE_INPUT;
        input_mode &= ~ENABLE_ECHO_INPUT;
        SetConsoleMode(input, input_mode);
    }

    HANDLE output      = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  output_mode = 0U;
    if (output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &output_mode)) {
        output_mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(output, output_mode);
    }

    if (!SetConsoleCtrlHandler(control_event_handler, TRUE)) {
        std::cerr << "failed to install console control handler\n";
        return 18;
    }
#else
    if (!configure_interactive_console()) {
        return 21;
    }
#endif

    if (!write_all_stdout(ready_marker)) {
        return write_failure_code;
    }

    std::this_thread::sleep_for(std::chrono::seconds(60));
    return timeout_exit_code;
}

int run_hold_open_no_read()
{
    return run_hold_open_no_read_with_marker("hold-open-no-read\n", 15, 16);
}

int run_hold_open_pid_no_read()
{
    std::ostringstream ready;
    ready << "hold-open-pid-no-read " << current_process_id() << '\n';
    const std::string ready_marker = ready.str();
    return run_hold_open_no_read_with_marker(ready_marker, 47, 48);
}

int run_utf8_payload()
{
    if (!configure_binary_stdout()) {
        return 60;
    }

    const std::vector<unsigned char> payload = {
        'c', 'o', 'n', 'p', 't', 'y', '-', 'u',
        't', 'f', '8', ' ', 'c', 'a', 'f', 0xc3U,
        0xa9U, ' ', 0xceU, 0xa9U, ' ', 0xe4U, 0xb8U,
        0xadU, ' ', 0xe2U, 0x9cU, 0x93U, '\r', '\n',
    };
    return write_all_stdout(payload) ? 0 : 61;
}

int run_sync_raw_resize_gate(const std::string& checkpoint_path)
{
    if (!configure_raw_sync_console()) {
        return 62;
    }

    const std::vector<unsigned char> sync_prefix = {
        0x1bU, '[', '?', '2', '0', '2', '6', 'h',
        's', 'y', 'n', 'c', '-', 'h', 'i', 'd',
        'd', 'e', 'n', '-', 'b', 'e', 'f', 'o',
        'r', 'e', '-', 'r', 'e', 's', 'i', 'z',
        'e', '\r', '\n',
    };
    if (!write_all_stdout(sync_prefix)) {
        return 63;
    }

    if (!write_checkpoint_file(checkpoint_path)) {
        return 64;
    }

    unsigned char byte = 0U;
    if (std::fread(&byte, 1U, 1U, stdin) != 1U) {
        return 65;
    }

    if (byte == 0x03U) {
        return 130;
    }

    const std::vector<unsigned char> sync_suffix = {
        0x1bU, '[', '?', '2', '0', '2', '6', 'l',
        's', 'y', 'n', 'c', '-', 'f', 'i', 'n',
        'a', 'l', '-', 'a', 'f', 't', 'e', 'r',
        '-', 'r', 'e', 's', 'i', 'z', 'e', '\r',
        '\n',
    };
    return write_all_stdout(sync_suffix) ? 0 : 66;
}

int run_quick_exit()
{
    if (!configure_binary_stdout()) {
        return 67;
    }

    return write_all_stdout("quick-exit\r\n") ? 0 : 68;
}

}

int main(int argc, char** argv)
{
    if (argc == 2 && argument_equals(argv[1], "--list")) {
        print_list();
        return 0;
    }

    if (argc == 2 && argument_equals(argv[1], "--list-behavior-smokes")) {
        print_behavior_smoke_list();
        return 0;
    }

    if (argc == 3 && argument_equals(argv[1], "--behavior-smoke")) {
        const behavior_smoke_payload_result_t payload = behavior_smoke_payload(argv[2]);
        if (payload.status == behavior_smoke_payload_status_t::UNKNOWN_NAME) {
            std::cerr << "unknown behavior smoke: " << argv[2] << '\n';
            return 2;
        }

        if (payload.status == behavior_smoke_payload_status_t::EMPTY_PAYLOAD) {
            std::cerr << "behavior smoke payload is empty: " << argv[2] << '\n';
            return 24;
        }

        if (payload.status == behavior_smoke_payload_status_t::INVALID_HEX) {
            std::cerr << "behavior smoke payload is not valid hex: " << argv[2] << '\n';
            return 25;
        }

        if (payload.status == behavior_smoke_payload_status_t::INVALID_REPEAT_COUNT) {
            std::cerr << "behavior smoke repeat count is invalid: " << argv[2] << '\n';
            return 26;
        }

        if (!configure_binary_stdout()) {
            return 22;
        }

        return write_all_stdout(payload.bytes) ? 0 : 23;
    }

    if (argc >= 2 && argument_equals(argv[1], "--echo-argv")) {
        return echo_argv(argc, argv);
    }

    if (argc == 2 && argument_equals(argv[1], "--shell-like-smoke")) {
        return run_shell_like_smoke();
    }

    if (argc == 3 &&
        argument_equals(argv[1], "--scenario") &&
        argument_equals(argv[2], term::terminal_canvas_fixture_scenario_name()))
    {
        return print_scenario() ? 0 : 3;
    }

    if (argc >= 3 &&
        argument_equals(argv[1], "--interactive-scenario") &&
        argument_equals(argv[2], term::terminal_canvas_fixture_scenario_name()))
    {
        int expected_initial_rows    = 0;
        int expected_initial_columns = 0;
        std::string checkpoint_after_enable_input_modes;

        int index = 3;
        while (index < argc) {
            if (argument_equals(argv[index], "--expect-initial-size")) {
                if (index + 2 >= argc) {
                    print_usage();
                    return 2;
                }

                const std::optional<int> parsed_rows    = parse_int(argv[index + 1]);
                const std::optional<int> parsed_columns = parse_int(argv[index + 2]);
                if (!parsed_rows.has_value() || !parsed_columns.has_value()) {
                    std::cerr << "--expect-initial-size requires integer rows and columns\n";
                    return 2;
                }

                expected_initial_rows     = *parsed_rows;
                expected_initial_columns  = *parsed_columns;
                index                    += 3;
                continue;
            }

            if (argument_equals(argv[index], "--checkpoint-after-enable-input-modes")) {
                if (index + 1 >= argc) {
                    print_usage();
                    return 2;
                }

                checkpoint_after_enable_input_modes = argv[index + 1];
                index += 2;
                continue;
            }

            {
                print_usage();
                return 2;
            }
        }

        return
            run_interactive_scenario(
                expected_initial_rows,
                expected_initial_columns,
                checkpoint_after_enable_input_modes);
    }

    if (argc == 2 && argument_equals(argv[1], "--hold-open")) {
        return run_hold_open();
    }

    if (argc == 2 && argument_equals(argv[1], "--hold-open-pid")) {
        return run_hold_open_pid();
    }

    if (argc == 2 && argument_equals(argv[1], "--hold-open-pid-no-read")) {
        return run_hold_open_pid_no_read();
    }

    if (argc == 2 && argument_equals(argv[1], "--hold-open-no-read")) {
        return run_hold_open_no_read();
    }

    if (argc == 2 && argument_equals(argv[1], "--utf8-payload")) {
        return run_utf8_payload();
    }

    if (argc == 3 && argument_equals(argv[1], "--sync-raw-resize-gate")) {
        return run_sync_raw_resize_gate(argv[2]);
    }

    if (argc == 2 && argument_equals(argv[1], "--quick-exit")) {
        return run_quick_exit();
    }

    print_usage();
    return 2;
}
