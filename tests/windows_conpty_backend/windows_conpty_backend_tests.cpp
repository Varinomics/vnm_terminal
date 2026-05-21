#include "helpers/decode_hex.h"
#include "helpers/test_check.h"
#include "vnm_terminal/internal/terminal_canvas_fixture_contract.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "vnm_terminal/internal/windows_conpty_backend.h"

#include <QByteArray>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <cstddef>

namespace term = vnm_terminal::internal;

namespace {

constexpr std::chrono::milliseconds k_wait_timeout(10000);

using vnm_terminal::test_helpers::check;
using vnm_terminal::test_helpers::decode_hex;

std::size_t count_occurrences(const QByteArray& haystack, const QByteArray& needle)
{
    std::size_t count  = 0U;
    qsizetype   offset = 0;
    for (;;) {
        offset = haystack.indexOf(needle, offset);
        if (offset < 0) {
            return count;
        }

        ++count;
        offset += needle.size();
    }
}

bool output_contains_in_order(
    const QByteArray&              output,
    const std::vector<QByteArray>& needles,
    const char*                    message)
{
    qsizetype offset = 0;
    for (const QByteArray& needle : needles) {
        offset = output.indexOf(needle, offset);
        if (offset < 0) {
            std::cerr << "missing ordered output payload: "
                << needle.toHex(' ').constData() << '\n';
            return check(false, message);
        }

        offset += needle.size();
    }

    return true;
}

int diagnostic_count(const term::Terminal_screen_model_result& result)
{
    int count = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::DIAGNOSTIC) {
            ++count;
        }
    }

    return count;
}

bool wait_for_file(const QString& path)
{
    const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
    do {
        if (QFileInfo::exists(path)) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    while (std::chrono::steady_clock::now() < deadline);

    return false;
}

std::optional<DWORD> parse_pid_after_prefix(
    const QByteArray&  output,
    const QByteArray&  prefix)
{
    const qsizetype prefix_offset = output.indexOf(prefix);
    if (prefix_offset < 0) {
        return std::nullopt;
    }

    const qsizetype pid_offset = prefix_offset + prefix.size();
    const qsizetype line_end   = output.indexOf('\n', pid_offset);
    if (line_end < 0) {
        return std::nullopt;
    }

    qsizetype pid_end = line_end;
    if (pid_end > pid_offset && output[pid_end - 1] == '\r') {
        --pid_end;
    }

    if (pid_end <= pid_offset) {
        return std::nullopt;
    }

    unsigned long long pid = 0U;
    for (qsizetype offset = pid_offset; offset < pid_end; ++offset) {
        const char ch = output[offset];
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }

        pid = pid * 10U + static_cast<unsigned long long>(ch - '0');
        if (pid > std::numeric_limits<DWORD>::max()) {
            return std::nullopt;
        }
    }

    if (pid == 0U) {
        return std::nullopt;
    }

    return static_cast<DWORD>(pid);
}

bool test_pid_parser_requires_line_delimiter()
{
    bool ok = true;

    const QByteArray prefix = QByteArrayLiteral("hold-open-pid-no-read ");
    ok &= check(!parse_pid_after_prefix(
        QByteArrayLiteral("hold-open-pid-no-read 1"),
        prefix).has_value(),
        "PID parser waits for a complete line before accepting digits");
    ok &= check(!parse_pid_after_prefix(
        QByteArrayLiteral("hold-open-pid-no-read 123x\n"),
        prefix).has_value(),
        "PID parser rejects non-digit PID line content");

    const std::optional<DWORD> pid = parse_pid_after_prefix(
        QByteArrayLiteral("hold-open-pid-no-read 12345\r\n"),
        prefix);
    ok &= check(pid.has_value() && *pid == 12345U,
        "PID parser accepts a complete CRLF-delimited PID line");

    return ok;
}

class Win32_process_handle
{
public:
    Win32_process_handle() = default;

    explicit Win32_process_handle(HANDLE handle)
    :
        m_handle(handle)
    {}

    ~Win32_process_handle()
    {
        reset();
    }

    Win32_process_handle(const Win32_process_handle&)            = delete;
    Win32_process_handle& operator=(const Win32_process_handle&) = delete;

    Win32_process_handle(Win32_process_handle&& other) noexcept
    :
        m_handle(other.m_handle)
    {
        other.m_handle = nullptr;
    }

    Win32_process_handle& operator=(Win32_process_handle&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }

        return *this;
    }

    void reset(HANDLE handle = nullptr)
    {
        if (m_handle != nullptr) {
            CloseHandle(m_handle);
        }

        m_handle = handle;
    }

    bool   is_valid() const { return m_handle != nullptr; }
    HANDLE get()      const { return m_handle;            }

private:
    HANDLE m_handle = nullptr;
};

Win32_process_handle open_process_handle(DWORD pid)
{
    return Win32_process_handle(
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
}

std::optional<bool> process_handle_is_running(HANDLE process)
{
    const DWORD wait_result = WaitForSingleObject(process, 0U);
    if (wait_result == WAIT_TIMEOUT) {
        return true;
    }

    if (wait_result == WAIT_OBJECT_0) {
        return false;
    }

    std::cerr << "WaitForSingleObject(process, 0) failed: " << GetLastError() << '\n';
    return std::nullopt;
}

bool wait_for_process_exit(HANDLE process)
{
    const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
    do {
        const DWORD wait_result = WaitForSingleObject(process, 10U);
        if (wait_result == WAIT_OBJECT_0) {
            return true;
        }

        if (wait_result == WAIT_FAILED) {
            std::cerr << "WaitForSingleObject(process, 10) failed: " << GetLastError() << '\n';
            return false;
        }
    }
    while (std::chrono::steady_clock::now() < deadline);

    const DWORD wait_result = WaitForSingleObject(process, 0U);
    if (wait_result == WAIT_FAILED) {
        std::cerr << "WaitForSingleObject(process, 0) failed: " << GetLastError() << '\n';
        return false;
    }

    return wait_result == WAIT_OBJECT_0;
}

std::optional<DWORD> process_exit_code(HANDLE process)
{
    DWORD exit_code = 0U;
    if (!GetExitCodeProcess(process, &exit_code)) {
        std::cerr << "GetExitCodeProcess failed: " << GetLastError() << '\n';
        return std::nullopt;
    }

    return exit_code;
}

std::optional<QByteArray> conpty_observable_output_payload(
    const term::terminal_canvas_fixture_record_t& record)
{
    if (record.label == std::string_view("enter-alternate-screen") ||
        record.label == std::string_view("leave-alternate-screen"))
    {
        return std::nullopt;
    }

    if (record.label == std::string_view("prompt")) {
        return QByteArrayLiteral("term>");
    }

    if (record.label == std::string_view("prompt-editing-keys-ack")) {
        return QByteArrayLiteral("input prompt-editing-keys ok");
    }

    if (record.label == std::string_view("resize-report")) {
        return QByteArrayLiteral("resize 33x120");
    }

    if (record.label == std::string_view("bracketed-paste-ack")) {
        return QByteArrayLiteral("input bracketed-paste ok");
    }

    if (record.label == std::string_view("focus-reporting-ack")) {
        return QByteArrayLiteral("input focus-reporting ok");
    }

    if (record.label == std::string_view("mouse-sgr-1006-ack")) {
        return QByteArrayLiteral("input mouse-sgr-1006 ok");
    }

    if (record.label == term::k_terminal_canvas_fixture_enable_input_modes_label) {
        return std::nullopt;
    }

    if (record.label == std::string_view("reply-handling") &&
        record.kind  == term::Terminal_canvas_fixture_record_kind::OUTPUT)
    {
        return std::nullopt;
    }

    if (record.label == std::string_view("high-volume-streaming")) {
        return QByteArrayLiteral("stream-row");
    }

    if (record.kind == term::Terminal_canvas_fixture_record_kind::OUTPUT ||
        record.kind == term::Terminal_canvas_fixture_record_kind::REPEAT_OUTPUT)
    {
        return decode_hex(record.payload_hex);
    }

    return std::nullopt;
}

QByteArray fixture_output_payload(std::string_view label)
{
    for (const term::terminal_canvas_fixture_record_t& record :
        term::terminal_canvas_fixture_contract_script())
    {
        if (record.kind  == term::Terminal_canvas_fixture_record_kind::OUTPUT &&
            record.label == label)
        {
            return decode_hex(record.payload_hex);
        }
    }

    return {};
}

bool scripted_output_is_ordered(const QByteArray& output)
{
    qsizetype offset = 0;
    for (const term::terminal_canvas_fixture_record_t& record :
        term::terminal_canvas_fixture_contract_script())
    {
        const std::optional<QByteArray> payload =
            conpty_observable_output_payload(record);
        if (!payload.has_value()) {
            continue;
        }

        if (record.kind == term::Terminal_canvas_fixture_record_kind::OUTPUT) {
            offset = output.indexOf(*payload, offset);
            if (offset < 0) {
                std::cerr << "missing ordered output record: " << record.label << '\n';
                return false;
            }

            offset += payload->size();
        }
        else
        if (record.kind == term::Terminal_canvas_fixture_record_kind::REPEAT_OUTPUT) {
            for (int i = 0; i < record.repeat_count; ++i) {
                offset = output.indexOf(*payload, offset);
                if (offset < 0) {
                    std::cerr << "missing repeated output record: "
                        << record.label << " at repeat " << i << '\n';
                    return false;
                }

                offset += payload->size();
            }
        }
    }

    return true;
}

class Backend_capture
{
public:
    term::Terminal_backend_callbacks callbacks(
        std::function<void(const QByteArray&)> output_observer = {})
    {
        term::Terminal_backend_callbacks callbacks;
        callbacks.output_received = [this, output_observer](QByteArray bytes) {
            const QByteArray observed_bytes = bytes;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_output.append(bytes);
            }

            if (output_observer) {
                output_observer(observed_bytes);
            }
            m_cv.notify_all();
        };
        callbacks.process_exited = [this](term::Terminal_backend_exit exit) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_exit = exit;
            ++m_exit_count;
            m_cv.notify_all();
        };
        callbacks.error_reported = [this](term::Terminal_backend_error error) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_errors.push_back(std::move(error));
            m_cv.notify_all();
        };
        return callbacks;
    }

    bool wait_until(const std::function<bool()>& predicate)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, k_wait_timeout, predicate);
    }

    bool wait_for_output(const QByteArray& needle)
    {
        return wait_until([&] {
            return m_output.contains(needle);
        });
    }

    bool wait_for_output_within(
        const QByteArray&          needle,
        std::chrono::milliseconds  interval)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, interval, [&] {
            return m_output.contains(needle);
        });
    }

    bool wait_for_output_matching_within(
        const std::function<bool(const QByteArray&)>&
                                   predicate,
        std::chrono::milliseconds  interval)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, interval, [&] {
            return predicate(m_output);
        });
    }

    std::optional<DWORD> wait_for_pid_output(const QByteArray& prefix)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        const bool ready = m_cv.wait_for(lock, k_wait_timeout, [&] {
            return parse_pid_after_prefix(m_output, prefix).has_value();
        });
        if (!ready) {
            return std::nullopt;
        }

        return parse_pid_after_prefix(m_output, prefix);
    }

    bool wait_for_exit()
    {
        return wait_until([&] {
            return m_exit.has_value();
        });
    }

    QByteArray output_snapshot()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_output;
    }

    std::optional<term::Terminal_backend_exit> exit_snapshot()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_exit;
    }

    int exit_count_snapshot()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_exit_count;
    }

    std::vector<term::Terminal_backend_error> errors_snapshot()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_errors;
    }

private:
    std::mutex                 m_mutex;
    std::condition_variable    m_cv;
    QByteArray                 m_output;
    std::optional<term::Terminal_backend_exit>
                               m_exit;
    std::vector<term::Terminal_backend_error>
                               m_errors;
    int                        m_exit_count = 0;
};

const char* backend_error_code_name(term::Terminal_backend_error_code code)
{
    switch (code) {
        case term::Terminal_backend_error_code::INVALID_LAUNCH_CONFIG:         return "INVALID_LAUNCH_CONFIG";
        case term::Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE:     return "INVALID_INITIAL_GRID_SIZE";
        case term::Terminal_backend_error_code::WORKING_DIRECTORY_UNAVAILABLE: return "WORKING_DIRECTORY_UNAVAILABLE";
        case term::Terminal_backend_error_code::START_FAILED:                  return "START_FAILED";
        case term::Terminal_backend_error_code::WRITE_FAILED:                  return "WRITE_FAILED";
        case term::Terminal_backend_error_code::RESIZE_FAILED:                 return "RESIZE_FAILED";
        case term::Terminal_backend_error_code::INTERRUPT_FAILED:              return "INTERRUPT_FAILED";
        case term::Terminal_backend_error_code::TERMINATE_FAILED:              return "TERMINATE_FAILED";
        case term::Terminal_backend_error_code::OUTPUT_OVERFLOW:               return "OUTPUT_OVERFLOW";
        case term::Terminal_backend_error_code::CALLBACK_MISSING:              return "CALLBACK_MISSING";
        case term::Terminal_backend_error_code::READ_FAILED:                   return "READ_FAILED";
    }

    return "UNKNOWN";
}

bool check_no_backend_errors(Backend_capture& capture, const char* message)
{
    const std::vector<term::Terminal_backend_error> errors =
        capture.errors_snapshot();
    if (errors.empty()) {
        return check(true, message);
    }

    std::cerr << message << ": " << errors.size()
        << " backend error(s)\n";
    for (const term::Terminal_backend_error& error : errors) {
        const QByteArray error_message = error.message.toLocal8Bit();
        std::cerr << "  "
            << backend_error_code_name(error.code)
            << ": "
            << error_message.constData()
            << '\n';
    }

    return check(false, message);
}

term::Terminal_launch_config launch_config(
    const QString& fixture_path,
    QStringList    arguments)
{
    term::Terminal_launch_config config;
    config.argv = {fixture_path};
    config.argv.append(arguments);
    config.working_directory  = QFileInfo(fixture_path).absolutePath();
    config.initial_grid_size  = term::terminal_grid_size_t{24, 80};
    config.identity.term      = QStringLiteral("xterm-256color");
    config.identity.colorterm = QStringLiteral("truecolor");
    return config;
}

const std::vector<term::terminal_grid_size_t>& resize_storm_grid_sizes()
{
    static const std::vector<term::terminal_grid_size_t> sizes = {
        { 20, 70  },
        { 35, 132 },
        { 12, 40  },
        { 48, 160 },
        { 24, 80  },
        { 30, 100 },
        { 18, 90  },
        { 42, 120 },
        { 25, 81  },
        { 33, 111 },
        { 16, 132 },
        { 28, 96  },
        { 21, 73  },
        { 37, 140 },
        { 19, 88  },
        { 31, 99  },
    };

    return sizes;
}

int resize_storm_max_rows()
{
    int max_rows = 0;
    for (term::terminal_grid_size_t grid_size : resize_storm_grid_sizes()) {
        if (grid_size.rows > max_rows) {
            max_rows = grid_size.rows;
        }
    }

    return max_rows;
}

int gated_stream_resize_pad_rows()
{
    return resize_storm_max_rows() + 32;
}

QByteArray shell_fixture_command(std::string_view command)
{
    QByteArray bytes(
        command.data(),
        static_cast<qsizetype>(command.size()));
    bytes.append('\r');
    return bytes;
}

QByteArray shell_fixture_prompt()
{
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    QByteArray prompt(
        contract.prompt.data(),
        static_cast<qsizetype>(contract.prompt.size()));
    while (prompt.endsWith(' ')) {
        prompt.chop(1);
    }

    return prompt;
}

QByteArray shell_fixture_size_marker(term::terminal_grid_size_t grid_size)
{
    QByteArray marker = QByteArrayLiteral("size ");
    marker += QByteArray::number(grid_size.rows);
    marker += 'x';
    marker += QByteArray::number(grid_size.columns);
    return marker;
}

bool is_shell_line_delimiter(char ch)
{
    return ch == '\r' || ch == '\n';
}

bool output_contains_shell_size_marker(
    const QByteArray&          output,
    term::terminal_grid_size_t grid_size)
{
    const QByteArray marker = shell_fixture_size_marker(grid_size);
    qsizetype        offset = 0;
    for (;;) {
        offset = output.indexOf(marker, offset);
        if (offset < 0) {
            return false;
        }

        const qsizetype delimiter_offset = offset + marker.size();
        if (delimiter_offset < output.size() &&
            is_shell_line_delimiter(output[delimiter_offset]))
        {
            return true;
        }

        ++offset;
    }
}

std::size_t shell_size_report_count(const QByteArray& output)
{
    return count_occurrences(output, QByteArrayLiteral("size "));
}

QByteArray shell_fixture_stream_command(int count)
{
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    QByteArray bytes(
        contract.stream_command.data(),
        static_cast<qsizetype>(contract.stream_command.size()));
    bytes += ' ';
    bytes += QByteArray::number(count);
    bytes += '\r';
    return bytes;
}

QByteArray shell_fixture_gated_stream_command(
    int                        count,
    term::terminal_grid_size_t resize_gate,
    int                        resize_pad_rows)
{
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    QByteArray bytes(
        contract.gated_stream_command.data(),
        static_cast<qsizetype>(contract.gated_stream_command.size()));
    bytes += ' ';
    bytes += QByteArray::number(count);
    bytes += ' ';
    bytes += QByteArray::number(resize_gate.rows);
    bytes += ' ';
    bytes += QByteArray::number(resize_gate.columns);
    bytes += ' ';
    bytes += QByteArray::number(resize_pad_rows);
    bytes += '\r';
    return bytes;
}

QByteArray shell_fixture_gated_stream_ready_marker()
{
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    return
        QByteArray(
            contract.gated_stream_ready_output.data(),
            static_cast<qsizetype>(contract.gated_stream_ready_output.size()));
}

QByteArray shell_fixture_gated_stream_resized_marker(
    term::terminal_grid_size_t grid_size)
{
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    QByteArray marker(
        contract.gated_stream_resized_prefix.data(),
        static_cast<qsizetype>(contract.gated_stream_resized_prefix.size()));
    marker += QByteArray::number(grid_size.rows);
    marker += 'x';
    marker += QByteArray::number(grid_size.columns);
    return marker;
}

QByteArray shell_fixture_gated_stream_continue_command()
{
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    return shell_fixture_command(contract.gated_stream_continue_command);
}

QByteArray shell_fixture_stream_line(int row)
{
    QByteArray line = QByteArrayLiteral("stream-row-");
    if (row < 100) { line += '0'; }
    if (row < 10)  { line += '0'; }
    line += QByteArray::number(row);
    return line;
}

QByteArray conpty_utf8_payload()
{
    return decode_hex("636f6e7074792d7574663820636166c3a920cea920e4b8ad20e29c930d0a");
}

QByteArray conpty_sync_enter_sequence()
{
    return decode_hex("1b5b3f3230323668");
}

QByteArray conpty_sync_hidden_payload()
{
    return QByteArrayLiteral("sync-hidden-before-resize\r\n");
}

QByteArray conpty_sync_release_sequence()
{
    return decode_hex("1b5b3f323032366c");
}

QByteArray conpty_sync_final_payload()
{
    return QByteArrayLiteral("sync-final-after-resize\r\n");
}

QByteArray conpty_quick_exit_payload()
{
    return QByteArrayLiteral("quick-exit\r\n");
}

bool shell_fixture_stream_rows_are_exactly_ordered(
    const QByteArray&  output,
    int                stream_count,
    const char*        message)
{
    const std::size_t observed_row_count =
        count_occurrences(output, QByteArrayLiteral("stream-row-"));
    bool rows_are_exact = true;
    if (observed_row_count != static_cast<std::size_t>(stream_count)) {
        std::cerr << "expected " << stream_count
            << " stream rows, saw " << observed_row_count << '\n';
        rows_are_exact = false;
    }

    qsizetype search_offset = 0;
    for (int row = 1; row <= stream_count; ++row) {
        const QByteArray  line      = shell_fixture_stream_line(row);
        const std::size_t row_count = count_occurrences(output, line);
        if (row_count != 1U) {
            std::cerr << "expected one occurrence of "
                << line.constData()
                << ", saw " << row_count << '\n';
            rows_are_exact = false;
        }

        const qsizetype row_offset = output.indexOf(line, search_offset);
        if (row_offset < 0) {
            std::cerr << "stream row out of order: " << line.constData() << '\n';
            rows_are_exact = false;
            continue;
        }

        search_offset = row_offset + line.size();
    }

    return rows_are_exact ? true : check(false, message);
}

std::size_t shell_fixture_authored_stream_row_count(const QByteArray& output)
{
    return count_occurrences(output, QByteArrayLiteral("stream-row-"));
}

bool apply_resize_storm(
    term::Terminal_backend&    backend,
    std::uint64_t&             resize_id,
    const char*                message)
{
    for (term::terminal_grid_size_t grid_size : resize_storm_grid_sizes()) {
        const term::Terminal_backend_result resize_result = backend.resize({
            resize_id++,
            grid_size,
        });
        if (!check(resize_result.code == term::Terminal_backend_result_code::ACCEPTED,
            message))
        {
            std::cerr << "resize storm rejected "
                << grid_size.rows << "x" << grid_size.columns << '\n';
            return false;
        }
    }

    return true;
}

bool stop_shell_like_fixture(
    term::Terminal_backend&    backend,
    Backend_capture&           capture)
{
    bool ok = true;

    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    const term::Terminal_backend_result exit_write_result =
        backend.write(shell_fixture_command(contract.exit_command));
    ok &= check(exit_write_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "shell-like fixture accepts exit command");
    ok &= check(capture.wait_for_exit(), "shell-like fixture exits after exit command");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "shell-like fixture reports clean exit");
    ok &= check(capture.errors_snapshot().empty(),
        "shell-like fixture produces no backend errors");

    return ok;
}

bool wait_for_shell_size_report(
    term::Terminal_backend&    backend,
    Backend_capture&           capture,
    term::terminal_grid_size_t grid_size,
    const char*                message)
{
    const QByteArray marker = shell_fixture_size_marker(grid_size);
    if (output_contains_shell_size_marker(capture.output_snapshot(), grid_size)) {
        return true;
    }

    std::size_t observed_size_reports =
        shell_size_report_count(capture.output_snapshot());
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
    do {
        const term::Terminal_backend_result write_result =
            backend.write(shell_fixture_command(contract.size_command));
        if (!check(write_result.code == term::Terminal_backend_result_code::ACCEPTED,
            "shell-like fixture accepts size command"))
        {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }

        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        bool expected_size_reported = false;
        const bool observed_size_response = capture.wait_for_output_matching_within(
            [&](const QByteArray& output) {
                expected_size_reported =
                    output_contains_shell_size_marker(output, grid_size);
                return
                    expected_size_reported ||
                    shell_size_report_count(output) > observed_size_reports;
            },
            remaining);
        if (expected_size_reported) {
            return true;
        }

        const QByteArray output = capture.output_snapshot();
        if (output_contains_shell_size_marker(output, grid_size)) {
            return true;
        }

        observed_size_reports = shell_size_report_count(output);
        if (!observed_size_response) {
            break;
        }
    }
    while (std::chrono::steady_clock::now() < deadline);

    std::cerr << "expected shell size marker: " << marker.constData() << '\n';
    std::cerr << "shell size output hex: "
        << capture.output_snapshot().toHex(' ').constData()
        << '\n';
    return check(false, message);
}

bool test_interactive_canvas_fixture(const QString& fixture_path)
{
    bool ok = true;

    QTemporaryDir checkpoint_dir;
    ok &= check(checkpoint_dir.isValid(), "temporary checkpoint directory is available");
    if (!checkpoint_dir.isValid()) {
        return false;
    }

    const QString checkpoint_path =
        checkpoint_dir.filePath(QStringLiteral("enable-input-modes.checkpoint"));

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config = launch_config(
        fixture_path,
        {
            QStringLiteral("--interactive-scenario"),
            QString::fromLatin1(
                term::terminal_canvas_fixture_scenario_name().data(),
                static_cast<qsizetype>(
                    term::terminal_canvas_fixture_scenario_name().size())),
            QStringLiteral("--expect-initial-size"),
            QStringLiteral("24"),
            QStringLiteral("80"),
            QStringLiteral("--checkpoint-after-enable-input-modes"),
            checkpoint_path,
        });

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend starts interactive fixture");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("term>")),
        "interactive fixture prompt reaches backend output");

    const term::Terminal_backend_result pause_result = backend->set_output_paused(true);
    ok &= check(pause_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts output pause");

    std::uint64_t resize_id             = 1U;
    bool          output_pause_released = false;
    for (const term::terminal_canvas_fixture_record_t& record :
        term::terminal_canvas_fixture_contract_script())
    {
        if (record.kind == term::Terminal_canvas_fixture_record_kind::EXPECT_INPUT) {
            const term::Terminal_backend_result write_result =
                backend->write(decode_hex(record.payload_hex));
            ok &= check(write_result.code == term::Terminal_backend_result_code::ACCEPTED,
                "ConPTY backend accepts scripted input write");
            if (write_result.code != term::Terminal_backend_result_code::ACCEPTED) {
                return false;
            }
        }
        else
        if (record.kind == term::Terminal_canvas_fixture_record_kind::RESIZE) {
            const term::Terminal_backend_result resize_result = backend->resize({
                resize_id++,
                term::terminal_grid_size_t{record.rows, record.columns},
            });
            ok &= check(resize_result.code == term::Terminal_backend_result_code::ACCEPTED,
                "ConPTY backend accepts scripted resize");
            if (resize_result.code != term::Terminal_backend_result_code::ACCEPTED) {
                return false;
            }

            ok &= check(wait_for_file(checkpoint_path),
                "fixture produced post-resize output while backend output was paused");
            const QByteArray enable_modes_payload = fixture_output_payload(
                term::k_terminal_canvas_fixture_enable_input_modes_label);
            ok &= check(!enable_modes_payload.isEmpty(),
                "enable-input-modes payload is available");
            ok &= check(!capture.output_snapshot().contains(enable_modes_payload),
                "paused ConPTY output holds post-resize fixture output");

            const term::Terminal_backend_result resume_result =
                backend->set_output_paused(false);
            ok                    &= check(resume_result.code == term::Terminal_backend_result_code::ACCEPTED,
                "ConPTY backend accepts output resume");
            ok                    &= check(capture.wait_for_output(decode_hex("1b5b3f3230303468")),
                "resumed ConPTY output delivers buffered fixture output");
            output_pause_released  = true;
        }
    }

    ok &= check(output_pause_released, "interactive scenario exercised output pause");
    ok &= check(capture.wait_for_exit(), "interactive fixture exits");
    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "interactive fixture reports clean exit");

    const QByteArray output = capture.output_snapshot();
    ok &= check(output.contains(decode_hex("1b5b3f3230303468")),
        "ConPTY output includes bracketed paste enable");
    ok &= check(output.contains(decode_hex("1b5b3f323030342470")),
        "ConPTY output includes bracketed paste query");
    ok &= check(count_occurrences(output, QByteArrayLiteral("stream-row")) == 4096U,
        "ConPTY output preserves high-volume stream rows");
    ok &= check(scripted_output_is_ordered(output),
        "ConPTY output contains the scripted fixture output in order");
    ok &= check(capture.errors_snapshot().empty(),
        "interactive fixture produces no backend errors");

    return ok;
}

bool test_resize_storm_reports_final_shell_size(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(fixture_path, {QStringLiteral("--shell-like-smoke")}),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "resize-storm shell fixture starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    ok &= check(capture.wait_for_output(shell_fixture_prompt()),
        "resize-storm shell prompt reaches output");

    std::uint64_t resize_id = 1U;
    ok &= apply_resize_storm(
        *backend,
        resize_id,
        "ConPTY backend accepts resize storm step");

    const term::terminal_grid_size_t final_grid = resize_storm_grid_sizes().back();
    ok &= wait_for_shell_size_report(
        *backend,
        capture,
        final_grid,
        "resize-storm shell reports final ConPTY size");

    ok &= stop_shell_like_fixture(*backend, capture);
    return ok;
}

bool test_resize_interleaved_with_shell_output(const QString& fixture_path)
{
    bool ok = true;

    std::uint64_t resize_id = 1U;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(fixture_path, {QStringLiteral("--shell-like-smoke")}),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "output-interleaved resize shell fixture starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    ok &= check(capture.wait_for_output(shell_fixture_prompt()),
        "output-interleaved resize shell prompt reaches output");

    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    const int stream_count = contract.stream_max_count;

    const term::terminal_grid_size_t final_grid = resize_storm_grid_sizes().back();
    const term::Terminal_backend_result stream_write_result =
        backend->write(shell_fixture_gated_stream_command(
            stream_count,
            final_grid,
            gated_stream_resize_pad_rows()));
    ok &= check(stream_write_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "output-interleaved resize shell accepts gated stream command");
    ok &= check(capture.wait_for_output(shell_fixture_stream_line(1)),
        "output-interleaved resize shell starts stream output");
    ok &= check(capture.wait_for_output(shell_fixture_gated_stream_ready_marker()),
        "output-interleaved resize shell pauses with pending stream rows");

    ok &= apply_resize_storm(
        *backend,
        resize_id,
        "ConPTY backend accepts output-interleaved resize storm step");
    ok &= check(capture.wait_for_output(shell_fixture_gated_stream_resized_marker(final_grid)),
        "output-interleaved resize shell observes final resize before completing stream");
    ok &= check(shell_fixture_authored_stream_row_count(capture.output_snapshot()) == 1U,
        "output-interleaved resize shell has exactly one authored stream row before continue");

    const term::Terminal_backend_result continue_write_result =
        backend->write(shell_fixture_gated_stream_continue_command());
    ok &= check(continue_write_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "output-interleaved resize shell accepts gated stream continue command");
    ok &= check(capture.wait_for_output(shell_fixture_stream_line(stream_count)),
        "output-interleaved resize shell reaches final stream row after resize");

    const QByteArray output = capture.output_snapshot();
    ok &= shell_fixture_stream_rows_are_exactly_ordered(
        output,
        stream_count,
        "output-interleaved resize shell preserves authored stream rows exactly once in order");

    ok &= wait_for_shell_size_report(
        *backend,
        capture,
        final_grid,
        "output-interleaved resize shell reports final ConPTY size");

    ok &= stop_shell_like_fixture(*backend, capture);
    return ok;
}

bool test_scroll_region_scrollback_survives_conpty(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config = launch_config(
        fixture_path,
        {
            QStringLiteral("--behavior-smoke"),
            QStringLiteral("primary-scrollback-insert"),
        });
    config.initial_grid_size = term::terminal_grid_size_t{5, 20};

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY starts scroll-region scrollback fixture");
    ok &= check(capture.wait_for_exit(), "scroll-region scrollback fixture exits");
    ok &= check(capture.errors_snapshot().empty(),
        "scroll-region scrollback fixture produces no backend errors");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "scroll-region scrollback fixture reports clean exit");

    term::Terminal_screen_model model(
        {
            term::terminal_grid_size_t{5, 20},
            32,
            8,
            true,
            });
    const term::Terminal_screen_model_result result =
        model.ingest(capture.output_snapshot());
    ok &= check(diagnostic_count(result) == 0,
        "ConPTY-observed scroll-region scrollback has no parser diagnostics");
    if (!check(model.scrollback_size() > 0,
        "ConPTY-observed scroll-region scrollback creates model scrollback"))
    {
        std::cerr
            << "ConPTY scroll-region output hex: "
            << capture.output_snapshot().toHex(' ').constData()
            << '\n';
        ok = false;
    }

    return ok;
}

bool test_utf8_payload_preserves_exact_conpty_bytes(const QString& fixture_path)
{
    bool ok = true;

    const QByteArray expected_payload = conpty_utf8_payload();
    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(fixture_path, {QStringLiteral("--utf8-payload")}),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "UTF-8 payload fixture starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    ok &= check(capture.wait_for_output(expected_payload),
        "ConPTY backend receives deterministic UTF-8 payload bytes");
    ok &= check(capture.wait_for_exit(), "UTF-8 payload fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "UTF-8 payload fixture reports clean exit");
    ok &= check(count_occurrences(capture.output_snapshot(), expected_payload) == 1U,
        "ConPTY backend receives the exact authored UTF-8 payload once");
    ok &= check_no_backend_errors(capture,
        "UTF-8 payload fixture produces no backend errors");

    return ok;
}

bool test_sync_raw_resize_gate_preserves_order(const QString& fixture_path)
{
    bool ok = true;

    QTemporaryDir checkpoint_dir;
    ok &= check(checkpoint_dir.isValid(), "sync raw resize gate checkpoint directory is available");
    if (!checkpoint_dir.isValid()) {
        return false;
    }

    const QString checkpoint_path =
        checkpoint_dir.filePath(QStringLiteral("sync-raw-resize.checkpoint"));

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(
                fixture_path,
                {
                    QStringLiteral("--sync-raw-resize-gate"),
                    checkpoint_path,
                }),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "raw synchronized-output gate fixture starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    ok &= check(wait_for_file(checkpoint_path),
        "raw synchronized-output fixture reaches gate before release write");

    std::uint64_t resize_id = 1U;
    ok &= apply_resize_storm(
        *backend,
        resize_id,
        "ConPTY backend accepts resize storm between synchronized-output writes");

    const term::Terminal_backend_result continue_result =
        backend->write(QByteArrayLiteral("x"));
    ok &= check(continue_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "raw synchronized-output gate fixture accepts continue byte");
    ok &= check(capture.wait_for_output(conpty_sync_final_payload()),
        "raw synchronized-output gate fixture emits final payload after resize storm");
    ok &= check(capture.wait_for_exit(),
        "raw synchronized-output gate fixture exits");

    const QByteArray output = capture.output_snapshot();
    ok &= check(output.contains(conpty_sync_hidden_payload()),
        "ConPTY raw output contains synchronized-output hidden payload");
    // This raw backend stress intentionally depends on ConPTY passing
    // DECSET/DECRST 2026 mode bytes through as output instead of interpreting
    // them inside the backend layer.
    if (!output.contains(conpty_sync_release_sequence())) {
        std::cerr << "raw synchronized-output capture hex: "
            << output.toHex(' ').constData() << '\n';
    }
    ok &= output_contains_in_order(
        output,
        {
            conpty_sync_enter_sequence(),
            conpty_sync_hidden_payload(),
            conpty_sync_release_sequence(),
            conpty_sync_final_payload(),
        },
        "ConPTY raw output preserves gated synchronized-output write order");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "raw synchronized-output gate fixture reports clean exit");
    ok &= check_no_backend_errors(capture,
        "raw synchronized-output gate fixture produces no backend errors");

    return ok;
}

bool test_fast_start_exit_loop(const QString& fixture_path)
{
    bool ok = true;

    constexpr int k_iterations = 12;
    for (int iteration = 0; iteration < k_iterations; ++iteration) {
        Backend_capture capture;
        std::unique_ptr<term::Terminal_backend> backend =
            term::make_windows_conpty_backend();
        const term::Terminal_backend_result start_result =
            backend->start(
                launch_config(fixture_path, {QStringLiteral("--quick-exit")}),
                capture.callbacks());
        ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
            "quick-exit fixture starts");
        if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
            return false;
        }

        ok &= check(capture.wait_for_output(conpty_quick_exit_payload()),
            "quick-exit fixture marker reaches backend output");
        ok &= check(capture.wait_for_exit(), "quick-exit fixture exits");
        ok &= check(capture.exit_count_snapshot() == 1,
            "quick-exit fixture reports one process exit callback");

        const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
        ok &= check(exit.has_value() &&
            exit->reason == term::Terminal_exit_reason::EXITED &&
            exit->exit_code == 0,
            "quick-exit fixture reports clean exit");
        ok &= check(count_occurrences(
            capture.output_snapshot(),
            conpty_quick_exit_payload()) == 1U,
            "quick-exit fixture emits one authored marker");
        ok &= check_no_backend_errors(capture,
            "quick-exit fixture produces no backend errors");
    }

    return ok;
}

bool test_start_resize_terminate_ordering(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config =
        launch_config(fixture_path, {QStringLiteral("--hold-open-no-read")});
    config.termination_policy.graceful_interval = std::chrono::milliseconds(0);
    config.termination_policy.kill_interval     = std::chrono::milliseconds(500);

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "start-resize-terminate fixture starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    std::uint64_t resize_id = 1U;
    ok &= apply_resize_storm(
        *backend,
        resize_id,
        "ConPTY backend accepts resize immediately after start");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open-no-read")),
        "start-resize-terminate fixture reaches hold-open path before terminate");

    const term::Terminal_backend_result terminate_result = backend->terminate();
    ok &= check(terminate_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts terminate after start-time resize storm");
    ok &= check(backend->resize({resize_id++, {24, 80}}).code ==
        term::Terminal_backend_result_code::REJECTED,
        "ConPTY resize rejects after terminate is accepted");
    ok &= check(capture.wait_for_exit(),
        "start-resize-terminate fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::TERMINATED,
        "start-resize-terminate reports typed terminated exit");
    ok &= check_no_backend_errors(capture,
        "start-resize-terminate fixture produces no backend errors");

    return ok;
}

bool test_terminate_after_resize_storm_stops_child_process(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    Win32_process_handle child_process;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config =
        launch_config(fixture_path, {QStringLiteral("--hold-open-pid-no-read")});
    config.termination_policy.graceful_interval = std::chrono::milliseconds(0);
    config.termination_policy.kill_interval     = std::chrono::milliseconds(500);

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "terminate-after-resize-storm fixture starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    const std::optional<DWORD> child_pid =
        capture.wait_for_pid_output(QByteArrayLiteral("hold-open-pid-no-read "));
    ok &= check(child_pid.has_value(),
        "terminate-after-resize-storm fixture reports child pid");
    if (!child_pid.has_value()) {
        return false;
    }

    child_process = open_process_handle(*child_pid);
    ok &= check(child_process.is_valid(),
        "terminate-after-resize-storm child process handle opens");
    if (!child_process.is_valid()) {
        return false;
    }

    const std::optional<bool> child_running =
        process_handle_is_running(child_process.get());
    ok &= check(child_running.has_value() && *child_running,
        "terminate-after-resize-storm child process is alive before terminate");

    std::uint64_t resize_id = 1U;
    ok &= apply_resize_storm(
        *backend,
        resize_id,
        "ConPTY backend accepts lifecycle resize storm step");

    const term::Terminal_backend_result terminate_result = backend->terminate();
    ok &= check(terminate_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts terminate after lifecycle resize storm");
    ok &= check(capture.wait_for_exit(),
        "terminate-after-resize-storm fixture exits");

    const bool child_exited = wait_for_process_exit(child_process.get());
    ok &= check(child_exited,
        "terminate-after-resize-storm child process exits");
    ok &= check(capture.exit_count_snapshot() == 1,
        "terminate-after-resize-storm reports one process exit callback");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::TERMINATED,
        "terminate-after-resize-storm reports typed terminated exit");
    ok &= check(backend->resize({resize_id++, {24, 80}}).code ==
        term::Terminal_backend_result_code::REJECTED,
        "ConPTY resize after lifecycle termination rejects");
    ok &= check(backend->write(QByteArrayLiteral("after")).code ==
        term::Terminal_backend_result_code::REJECTED,
        "ConPTY write after lifecycle termination rejects");
    ok &= check(capture.errors_snapshot().empty(),
        "terminate-after-resize-storm fixture produces no backend errors");

    return ok;
}

bool test_missing_working_directory(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config =
        launch_config(fixture_path, {QStringLiteral("--hold-open")});
    config.working_directory = QFileInfo(fixture_path).absolutePath() +
        QStringLiteral("/missing-directory");

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::REJECTED,
        "missing working directory rejects start");
    ok &= check(start_result.error.has_value() &&
        start_result.error->code ==
            term::Terminal_backend_error_code::WORKING_DIRECTORY_UNAVAILABLE,
        "missing working directory reports typed error");
    ok &= check(!capture.errors_snapshot().empty() &&
        capture.errors_snapshot().front().code ==
            term::Terminal_backend_error_code::WORKING_DIRECTORY_UNAVAILABLE,
        "missing working directory emits backend error callback");

    config.working_directory = QFileInfo(fixture_path).absolutePath();
    const term::Terminal_backend_result retry_result =
        backend->start(config, capture.callbacks());
    ok &= check(retry_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY start can retry after preflight working-directory rejection");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open")),
        "retried working-directory fixture starts");
    ok &= check(backend->terminate().code == term::Terminal_backend_result_code::ACCEPTED,
        "retried working-directory fixture terminates");
    ok &= check(capture.wait_for_exit(), "retried working-directory fixture exits");

    return ok;
}

bool test_failed_executable(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config = launch_config(
        QFileInfo(fixture_path).absolutePath() + QStringLiteral("/missing-fixture.exe"),
        {});
    config.working_directory = QFileInfo(fixture_path).absolutePath();

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::REJECTED,
        "missing executable rejects start");
    ok &= check(start_result.error.has_value() &&
        start_result.error->code == term::Terminal_backend_error_code::START_FAILED,
        "missing executable reports typed start failure");
    ok &= check(!capture.errors_snapshot().empty() &&
        capture.errors_snapshot().front().code ==
            term::Terminal_backend_error_code::START_FAILED,
        "missing executable emits backend error callback");

    return ok;
}

bool test_rejection_paths(const QString& fixture_path)
{
    bool ok = true;

    {
        Backend_capture capture;
        std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
        const term::Terminal_backend_result callback_result =
            backend->start(launch_config(fixture_path, {QStringLiteral("--hold-open")}), {});
        ok &= check(callback_result.code == term::Terminal_backend_result_code::REJECTED &&
            callback_result.error.has_value() &&
            callback_result.error->code ==
                term::Terminal_backend_error_code::CALLBACK_MISSING,
            "ConPTY start rejects missing callbacks");
    }

    {
        std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
        ok &= check(backend->write(QByteArrayLiteral("x")).code ==
            term::Terminal_backend_result_code::REJECTED,
            "ConPTY write before start rejects");
        ok &= check(backend->resize({1U, {24, 80}}).code ==
            term::Terminal_backend_result_code::REJECTED,
            "ConPTY resize before start rejects");
        ok &= check(backend->interrupt().code == term::Terminal_backend_result_code::REJECTED,
            "ConPTY interrupt before start rejects");
        ok &= check(backend->terminate().code == term::Terminal_backend_result_code::REJECTED,
            "ConPTY terminate before start rejects");
    }

    {
        Backend_capture capture;
        std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
        term::Terminal_launch_config config =
            launch_config(fixture_path, {QStringLiteral("--hold-open")});
        config.initial_grid_size = term::terminal_grid_size_t{40000, 80};
        const term::Terminal_backend_result start_result =
            backend->start(config, capture.callbacks());
        ok &= check(start_result.code == term::Terminal_backend_result_code::REJECTED &&
            start_result.error.has_value() &&
            start_result.error->code ==
                term::Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
            "ConPTY start rejects out-of-range initial grid");
    }

    {
        Backend_capture capture;
        std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
        const term::Terminal_backend_result start_result =
            backend->start(
                launch_config(fixture_path, {QStringLiteral("--hold-open")}),
                capture.callbacks());
        ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
            "rejection-path fixture starts");
        ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open")),
            "rejection-path fixture reaches ready marker");

        const term::Terminal_backend_result second_start =
            backend->start(
                launch_config(fixture_path, {QStringLiteral("--hold-open")}),
                capture.callbacks());
        ok &= check(second_start.code == term::Terminal_backend_result_code::REJECTED &&
            second_start.error.has_value() &&
            second_start.error->code == term::Terminal_backend_error_code::START_FAILED,
            "ConPTY second start rejects");
        ok &= check(backend->resize({1U, {0, 80}}).code ==
            term::Terminal_backend_result_code::REJECTED,
            "ConPTY invalid resize rejects");
        ok &= check(backend->write(QByteArray(1024 * 1024 + 1, 'x')).code ==
            term::Terminal_backend_result_code::REJECTED,
            "ConPTY oversized write rejects");
        ok &= check(backend->terminate().code == term::Terminal_backend_result_code::ACCEPTED,
            "rejection-path fixture terminates");
        ok &= check(capture.wait_for_exit(), "rejection-path fixture exits");
        ok &= check(backend->write(QByteArrayLiteral("after")).code ==
            term::Terminal_backend_result_code::REJECTED,
            "ConPTY write after exit rejects");
    }

    return ok;
}

bool test_interrupt(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(fixture_path, {QStringLiteral("--hold-open")}),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "interrupt fixture starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open")),
        "interrupt fixture ready marker reaches output");

    const term::Terminal_backend_result interrupt_result = backend->interrupt();
    ok &= check(interrupt_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts interrupt");
    ok &= check(capture.wait_for_exit(), "interrupt fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::INTERRUPTED &&
        exit->exit_code == 130,
        "interrupt reports typed interrupted exit");
    ok &= check(capture.errors_snapshot().empty(),
        "interrupt fixture produces no backend errors");
    ok &= check(backend->write(QByteArrayLiteral("after")).code ==
        term::Terminal_backend_result_code::REJECTED,
        "ConPTY write after interrupt exit rejects");

    return ok;
}

bool test_interrupt_without_stdin_reader(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(fixture_path, {QStringLiteral("--hold-open-no-read")}),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "no-read interrupt fixture starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open-no-read")),
        "no-read interrupt fixture ready marker reaches output");

    const term::Terminal_backend_result interrupt_result = backend->interrupt();
    ok &= check(interrupt_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts no-read interrupt");
    ok &= check(capture.wait_for_exit(), "no-read interrupt fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::INTERRUPTED &&
        exit->exit_code == 130,
        "no-read interrupt reports typed interrupted exit");
    ok &= check(capture.errors_snapshot().empty(),
        "no-read interrupt fixture produces no backend errors");

    return ok;
}

bool test_terminate(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config =
        launch_config(fixture_path, {QStringLiteral("--hold-open")});
    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "terminate fixture starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open")),
        "terminate fixture ready marker reaches output");

    const term::Terminal_backend_result terminate_result = backend->terminate();
    ok &= check(terminate_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts terminate");
    ok &= check(capture.wait_for_exit(), "terminate fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::TERMINATED,
        "terminate reports typed terminated exit");
    ok &= check(capture.errors_snapshot().empty(),
        "terminate fixture produces no backend errors");
    ok &= check(backend->resize({1U, {24, 80}}).code ==
        term::Terminal_backend_result_code::REJECTED,
        "ConPTY resize after terminate exit rejects");

    return ok;
}

bool test_terminate_accepts_zero_grace_policy(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config =
        launch_config(fixture_path, {QStringLiteral("--hold-open-no-read")});
    config.termination_policy.graceful_interval = std::chrono::milliseconds(0);
    config.termination_policy.kill_interval     = std::chrono::milliseconds(500);

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "zero-grace terminate fixture starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open-no-read")),
        "zero-grace terminate fixture ready marker reaches output");

    const term::Terminal_backend_result terminate_result = backend->terminate();
    ok &= check(terminate_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts zero-grace terminate");
    ok &= check(capture.wait_for_exit(), "zero-grace terminate fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::TERMINATED,
        "zero-grace terminate reports typed terminated exit");
    ok &= check(capture.errors_snapshot().empty(),
        "zero-grace terminate fixture produces no backend errors");

    return ok;
}

bool test_terminate_returns_before_child_exit(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config =
        launch_config(fixture_path, {QStringLiteral("--hold-open-no-read")});
    config.termination_policy.graceful_interval = std::chrono::milliseconds(1000);
    config.termination_policy.kill_interval     = std::chrono::milliseconds(500);

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "nonblocking terminate fixture starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open-no-read")),
        "nonblocking terminate fixture ready marker reaches output");

    const auto started_at = std::chrono::steady_clock::now();

    const term::Terminal_backend_result terminate_result = backend->terminate();

    const auto terminate_elapsed =
        std::chrono::steady_clock::now() - started_at;
    ok &= check(terminate_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts nonblocking terminate");
    ok &= check(
        terminate_elapsed < std::chrono::milliseconds(250),
        "ConPTY terminate returns before waiting for child exit");
    ok &= check(capture.wait_for_exit(),
        "nonblocking terminate fixture exits after worker termination");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::TERMINATED,
        "nonblocking terminate reports typed terminated exit");
    ok &= check(capture.errors_snapshot().empty(),
        "nonblocking terminate fixture produces no backend errors");
    return ok;
}

bool test_destructor_stops_running_process(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    Win32_process_handle child_process;
    {
        std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
        const term::Terminal_backend_result start_result =
            backend->start(
                launch_config(fixture_path, {QStringLiteral("--hold-open-pid-no-read")}),
                capture.callbacks());
        ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
            "destructor fixture starts");
        const std::optional<DWORD> child_pid =
            capture.wait_for_pid_output(QByteArrayLiteral("hold-open-pid-no-read "));
        ok &= check(child_pid.has_value(),
            "destructor fixture reports child pid");
        if (!child_pid.has_value()) {
            return false;
        }

        child_process = open_process_handle(*child_pid);
        ok &= check(child_process.is_valid(),
            "destructor fixture child process handle opens");
        if (!child_process.is_valid()) {
            return false;
        }

        const std::optional<bool> child_running =
            process_handle_is_running(child_process.get());
        ok &= check(child_running.has_value() && *child_running,
            "destructor fixture child process is alive before backend teardown");
    }

    const bool child_exited = wait_for_process_exit(child_process.get());
    ok &= check(child_exited,
        "ConPTY backend destructor stops the running child process");
    if (!child_exited) {
        return false;
    }

    const std::optional<DWORD> child_exit_code = process_exit_code(child_process.get());
    ok &= check(child_exit_code.has_value() && *child_exit_code == 1U,
        "ConPTY backend destructor force-terminates the running child process");
    ok &= check(capture.errors_snapshot().empty(),
        "destructor fixture produces no backend errors");

    return ok;
}

}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: windows_conpty_backend_tests <fixture-executable>\n";
        return 2;
    }

    const QString fixture_path = QString::fromLocal8Bit(argv[1]);

    bool ok = true;
    ok &= test_pid_parser_requires_line_delimiter();
    ok &= test_interactive_canvas_fixture(fixture_path);
    ok &= test_resize_storm_reports_final_shell_size(fixture_path);
    ok &= test_resize_interleaved_with_shell_output(fixture_path);
    ok &= test_scroll_region_scrollback_survives_conpty(fixture_path);
    ok &= test_utf8_payload_preserves_exact_conpty_bytes(fixture_path);
    ok &= test_sync_raw_resize_gate_preserves_order(fixture_path);
    ok &= test_fast_start_exit_loop(fixture_path);
    ok &= test_start_resize_terminate_ordering(fixture_path);
    ok &= test_terminate_after_resize_storm_stops_child_process(fixture_path);
    ok &= test_missing_working_directory(fixture_path);
    ok &= test_failed_executable(fixture_path);
    ok &= test_rejection_paths(fixture_path);
    ok &= test_interrupt(fixture_path);
    ok &= test_interrupt_without_stdin_reader(fixture_path);
    ok &= test_terminate(fixture_path);
    ok &= test_terminate_accepts_zero_grace_policy(fixture_path);
    ok &= test_terminate_returns_before_child_exit(fixture_path);
    ok &= test_destructor_stops_running_process(fixture_path);
    return ok ? 0 : 1;
}
