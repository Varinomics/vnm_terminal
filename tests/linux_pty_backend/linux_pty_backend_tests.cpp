#include "helpers/decode_hex.h"
#include "helpers/test_check.h"
#include "vnm_terminal/internal/linux_pty_backend.h"
#include "vnm_terminal/internal/terminal_canvas_fixture_contract.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <signal.h>
#include <sys/types.h>

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

std::optional<QByteArray> linux_pty_observable_output_payload(
    const term::terminal_canvas_fixture_record_t& record)
{
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
            linux_pty_observable_output_payload(record);
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
                ++m_callback_sequence;
                m_output_events.push_back({observed_bytes, m_callback_sequence});
                m_output.append(bytes);
            }

            if (output_observer) {
                output_observer(observed_bytes);
            }
            m_cv.notify_all();
        };
        callbacks.process_exited = [this](term::Terminal_backend_exit exit) {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_callback_sequence;
            m_exit_event_sequence = m_callback_sequence;
            m_exit = exit;
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

    bool wait_for_output_to_stay_absent(
        const QByteArray&          needle,
        std::chrono::milliseconds  interval)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        const bool appeared = m_cv.wait_for(lock, interval, [&] {
            return m_output.contains(needle);
        });
        return !appeared;
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

    std::vector<term::Terminal_backend_error> errors_snapshot()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_errors;
    }

    bool output_precedes_exit(const QByteArray& needle)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_exit_event_sequence.has_value()) {
            return false;
        }

        QByteArray output_prefix;
        for (const Output_event& event : m_output_events) {
            output_prefix += event.bytes;
            if (output_prefix.contains(needle) &&
                event.sequence < *m_exit_event_sequence)
            {
                return true;
            }
        }

        return false;
    }

private:
    struct Output_event
    {
        QByteArray     bytes;
        std::size_t    sequence = 0U;
    };

    std::mutex                 m_mutex;
    std::condition_variable    m_cv;
    QByteArray                 m_output;
    std::optional<term::Terminal_backend_exit>
                               m_exit;
    std::vector<term::Terminal_backend_error>
                               m_errors;
    std::vector<Output_event>  m_output_events;
    std::optional<std::size_t> m_exit_event_sequence;
    std::size_t                m_callback_sequence = 0U;
};

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

term::Terminal_launch_config shell_launch_config(const QString& command)
{
    term::Terminal_launch_config config = launch_config(
        QStringLiteral("/bin/sh"),
        {QStringLiteral("-c"), command});
    config.working_directory = QStringLiteral("/");
    return config;
}

QString shell_quote(const QString& value)
{
    QString out = value;
    out.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
    return QStringLiteral("'") + out + QStringLiteral("'");
}

std::optional<pid_t> read_pid_file(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return std::nullopt;
    }

    bool ok = false;
    const qlonglong pid = QString::fromLocal8Bit(file.readAll()).trimmed().toLongLong(&ok);
    if (!ok || pid <= 0) {
        return std::nullopt;
    }

    return static_cast<pid_t>(pid);
}

bool process_is_alive(pid_t pid)
{
    if (::kill(pid, 0) == 0) {
        return true;
    }

    return errno == EPERM;
}

bool wait_for_process_absent(pid_t pid)
{
    const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
    do {
        if (!process_is_alive(pid)) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    while (std::chrono::steady_clock::now() < deadline);

    return false;
}

bool test_launch_output(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(launch_config(fixture_path, {QStringLiteral("--list")}),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "Linux PTY backend starts list fixture");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("terminal-canvas")),
        "list fixture output reaches backend output");
    ok &= check(capture.wait_for_exit(), "list fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "list fixture reports clean exit");
    ok &= check(capture.errors_snapshot().empty(),
        "list fixture produces no backend errors");
    ok &= check(backend->write(QByteArrayLiteral("after")).code ==
        term::Terminal_backend_result_code::REJECTED,
        "Linux PTY write after list exit rejects");

    return ok;
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
    std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
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
        "Linux PTY backend starts interactive fixture");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("term>")),
        "interactive fixture prompt reaches backend output");

    const term::Terminal_backend_result pause_result = backend->set_output_paused(true);
    ok &= check(pause_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "Linux PTY backend accepts output pause");

    std::uint64_t resize_id             = 1U;
    bool          output_pause_released = false;
    for (const term::terminal_canvas_fixture_record_t& record :
        term::terminal_canvas_fixture_contract_script())
    {
        if (record.kind == term::Terminal_canvas_fixture_record_kind::EXPECT_INPUT) {
            const term::Terminal_backend_result write_result =
                backend->write(decode_hex(record.payload_hex));
            ok &= check(write_result.code == term::Terminal_backend_result_code::ACCEPTED,
                "Linux PTY backend accepts scripted input write");
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
                "Linux PTY backend accepts scripted resize");
            if (resize_result.code != term::Terminal_backend_result_code::ACCEPTED) {
                return false;
            }

            ok &= check(wait_for_file(checkpoint_path),
                "fixture produced post-resize output while backend output was paused");
            const QByteArray enable_modes_payload = fixture_output_payload(
                term::k_terminal_canvas_fixture_enable_input_modes_label);
            ok &= check(!enable_modes_payload.isEmpty(),
                "enable-input-modes payload is available");
            ok &= check(
                capture.wait_for_output_to_stay_absent(
                    enable_modes_payload,
                    std::chrono::milliseconds(1000)),
                "paused Linux PTY output holds post-resize fixture output");

            const term::Terminal_backend_result resume_result =
                backend->set_output_paused(false);
            ok                    &= check(resume_result.code == term::Terminal_backend_result_code::ACCEPTED,
                "Linux PTY backend accepts output resume");
            ok                    &= check(capture.wait_for_output(enable_modes_payload),
                "resumed Linux PTY output delivers buffered fixture output");
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
        "Linux PTY output includes bracketed paste enable");
    ok &= check(output.contains(decode_hex("1b5b3f323030342470")),
        "Linux PTY output includes bracketed paste query");
    ok &= check(count_occurrences(output, QByteArrayLiteral("stream-row")) == 4096U,
        "Linux PTY output preserves high-volume stream rows");
    ok &= check(scripted_output_is_ordered(output),
        "Linux PTY output contains the scripted fixture output in order");
    ok &= check(capture.errors_snapshot().empty(),
        "interactive fixture produces no backend errors");

    return ok;
}

bool test_missing_working_directory(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
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
        "Linux PTY start can retry after preflight working-directory rejection");
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
    std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
    term::Terminal_launch_config config = launch_config(
        QFileInfo(fixture_path).absolutePath() + QStringLiteral("/missing-fixture"),
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

    config.argv = {fixture_path, QStringLiteral("--hold-open")};
    const term::Terminal_backend_result retry_result =
        backend->start(config, capture.callbacks());
    ok &= check(retry_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "Linux PTY start can retry after missing-executable rejection");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open")),
        "retried missing-executable fixture starts");
    ok &= check(backend->terminate().code == term::Terminal_backend_result_code::ACCEPTED,
        "retried missing-executable fixture terminates");
    ok &= check(capture.wait_for_exit(), "retried missing-executable fixture exits");

    return ok;
}

bool test_rejection_paths(const QString& fixture_path)
{
    bool ok = true;

    {
        Backend_capture capture;
        std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
        const term::Terminal_backend_result callback_result =
            backend->start(launch_config(fixture_path, {QStringLiteral("--hold-open")}), {});
        ok &= check(callback_result.code == term::Terminal_backend_result_code::REJECTED &&
            callback_result.error.has_value() &&
            callback_result.error->code ==
                term::Terminal_backend_error_code::CALLBACK_MISSING,
            "Linux PTY start rejects missing callbacks");
    }

    {
        std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
        ok &= check(backend->write(QByteArrayLiteral("x")).code ==
            term::Terminal_backend_result_code::REJECTED,
            "Linux PTY write before start rejects");
        ok &= check(backend->resize({1U, {24, 80}}).code ==
            term::Terminal_backend_result_code::REJECTED,
            "Linux PTY resize before start rejects");
        ok &= check(backend->interrupt().code == term::Terminal_backend_result_code::REJECTED,
            "Linux PTY interrupt before start rejects");
        ok &= check(backend->terminate().code == term::Terminal_backend_result_code::REJECTED,
            "Linux PTY terminate before start rejects");
    }

    {
        Backend_capture capture;
        std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
        term::Terminal_launch_config config =
            launch_config(fixture_path, {QStringLiteral("--hold-open")});
        config.initial_grid_size = term::terminal_grid_size_t{70000, 80};
        const term::Terminal_backend_result start_result =
            backend->start(config, capture.callbacks());
        ok &= check(start_result.code == term::Terminal_backend_result_code::REJECTED &&
            start_result.error.has_value() &&
            start_result.error->code ==
                term::Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
            "Linux PTY start rejects out-of-range initial grid");
    }

    {
        Backend_capture capture;
        std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
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
            "Linux PTY second start rejects");
        ok &= check(backend->resize({1U, {0, 80}}).code ==
            term::Terminal_backend_result_code::REJECTED,
            "Linux PTY invalid resize rejects");
        ok &= check(backend->resize({2U, {24, 70000}}).code ==
            term::Terminal_backend_result_code::REJECTED,
            "Linux PTY out-of-range resize rejects");
        ok &= check(backend->write(QByteArray()).code ==
            term::Terminal_backend_result_code::REJECTED,
            "Linux PTY empty write while running rejects");
        ok &= check(backend->write(QByteArray(1024 * 1024 + 1, 'x')).code ==
            term::Terminal_backend_result_code::REJECTED,
            "Linux PTY oversized write rejects");
        ok &= check(backend->terminate().code == term::Terminal_backend_result_code::ACCEPTED,
            "rejection-path fixture terminates");
        ok &= check(capture.wait_for_exit(), "rejection-path fixture exits");
        ok &= check(backend->write(QByteArrayLiteral("after")).code ==
            term::Terminal_backend_result_code::REJECTED,
            "Linux PTY write after exit rejects");
    }

    return ok;
}

bool test_interrupt(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
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
        "Linux PTY backend accepts interrupt");
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
        "Linux PTY write after interrupt exit rejects");

    return ok;
}

bool test_interrupt_without_stdin_reader(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
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
        "Linux PTY backend accepts no-read interrupt");
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

bool test_interrupt_ignored_child_exits_normally()
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
    const term::Terminal_backend_result start_result = backend->start(
        shell_launch_config(
            QStringLiteral(
                "trap 'echo caught-int; exit 0' INT; "
                "echo ignore-int; "
                "while :; do sleep 1; done")),
        capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "interrupt-ignored shell starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("ignore-int")),
        "interrupt-ignored shell ready marker reaches output");

    const term::Terminal_backend_result interrupt_result = backend->interrupt();
    ok &= check(interrupt_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "Linux PTY backend accepts ignored interrupt");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("caught-int")),
        "interrupt-ignored shell observes delivered SIGINT");
    ok &= check(capture.wait_for_exit(), "interrupt-ignored shell exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "ignored interrupt preserves normal child exit status");
    ok &= check(capture.errors_snapshot().empty(),
        "interrupt-ignored shell produces no backend errors");

    return ok;
}

bool test_process_exit_reports_with_descendant_slave_open()
{
    bool ok = true;

    QTemporaryDir pid_dir;
    ok &= check(pid_dir.isValid(), "descendant-open pid directory is available");
    if (!pid_dir.isValid()) {
        return false;
    }

    const QString pid_path   = pid_dir.filePath(QStringLiteral("descendant.pid"));
    const QString ready_path = pid_dir.filePath(QStringLiteral("descendant.ready"));
    std::optional<pid_t> descendant_pid;
    {
        Backend_capture capture;
        std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
        const term::Terminal_backend_result start_result = backend->start(
            shell_launch_config(
                QStringLiteral(
                    "trap '' HUP TERM INT; "
                    "echo descendant-slave-open; "
                    "(echo ready > %1; sleep 30) & "
                    "echo $! > %2; "
                    "exit 0").arg(shell_quote(ready_path), shell_quote(pid_path))),
            capture.callbacks());
        ok             &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
            "descendant-open shell starts");
        ok             &= check(capture.wait_for_output(QByteArrayLiteral("descendant-slave-open")),
            "descendant-open shell ready marker reaches output");
        ok             &= check(wait_for_file(pid_path),
            "descendant-open shell records descendant pid");
        ok             &= check(wait_for_file(ready_path),
            "descendant-open shell confirms descendant is ready");
        descendant_pid  = read_pid_file(pid_path);
        ok             &= check(descendant_pid.has_value(),
            "descendant-open pid file is readable before backend teardown");
        if (descendant_pid.has_value()) {
            ok &= check(process_is_alive(*descendant_pid),
                "descendant-open process is alive before backend teardown");
        }
        ok &= check(capture.wait_for_exit(),
            "descendant-held PTY slave does not block exit reporting");
        if (descendant_pid.has_value()) {
            ok &= check(wait_for_process_absent(*descendant_pid),
                "descendant-open exit cleanup does not leak descendant before backend teardown");
        }

        const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
        ok &= check(exit.has_value() &&
            exit->reason == term::Terminal_exit_reason::EXITED &&
            exit->exit_code == 0,
            "descendant-open shell reports direct child clean exit");
        ok &= check(capture.errors_snapshot().empty(),
            "descendant-open shell produces no backend errors");
    }

    if (descendant_pid.has_value()) {
        if (process_is_alive(*descendant_pid)) {
            ::kill(*descendant_pid, SIGKILL);
        }
    }

    return ok;
}

bool test_exit_drain_ignores_repause_and_delivers_buffered_output()
{
    bool ok = true;

    const QByteArray marker            = QByteArrayLiteral("paused-exit-drain");
    std::atomic_bool repause_attempted = false;
    std::atomic_bool repause_accepted  = false;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend     = term::make_linux_pty_backend();
    term::Terminal_backend*                 backend_ptr = backend.get();
    const term::Terminal_backend_result start_result = backend->start(
        shell_launch_config(
            QStringLiteral(
                "echo exit-drain-ready; "
                "read line; "
                "echo paused-exit-drain; "
                "(trap 'exit 0' HUP TERM INT; sleep 30) & "
                "exit 0")),
        capture.callbacks([&](const QByteArray& bytes) {
            if (bytes.contains(marker) && !repause_attempted.exchange(true)) {
                const term::Terminal_backend_result pause_result =
                    backend_ptr->set_output_paused(true);
                repause_accepted =
                    pause_result.code == term::Terminal_backend_result_code::ACCEPTED;
            }
        }));
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "paused exit-drain shell starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("exit-drain-ready")),
        "paused exit-drain shell reaches ready marker");

    const term::Terminal_backend_result pause_result = backend->set_output_paused(true);
    ok &= check(pause_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "Linux PTY backend accepts pre-exit output pause");
    ok &= check(backend->write(QByteArrayLiteral("go\n")).code ==
        term::Terminal_backend_result_code::ACCEPTED,
        "paused exit-drain shell receives release input");
    ok &= check(capture.wait_for_output(marker),
        "exit drain delivers output buffered before child exit");
    ok &= check(repause_attempted,
        "exit drain callback attempted to re-pause output");
    ok &= check(repause_accepted,
        "exit drain re-pause request remains accepted");
    ok &= check(capture.wait_for_exit(),
        "exit drain re-pause does not block exit reporting");
    ok &= check(capture.output_precedes_exit(marker),
        "exit drain delivers buffered output before exit callback");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "paused exit-drain shell reports direct child clean exit");
    ok &= check(capture.errors_snapshot().empty(),
        "paused exit-drain shell produces no backend errors");

    return ok;
}

bool test_destructor_from_output_callback_returns()
{
    bool ok = true;

    const QByteArray marker = QByteArrayLiteral("destroy-from-output-callback");
    std::mutex callback_mutex;
    std::condition_variable callback_cv;
    bool callback_returned = false;
    QByteArray callback_output;
    std::atomic_bool delete_started     = false;
    std::atomic_bool callback_timed_out = false;

    // The raw pointer keeps ownership on the callback thread for this destruction test.
    term::Terminal_backend* backend = term::make_linux_pty_backend().release();
    term::Terminal_backend_callbacks callbacks;
    callbacks.output_received = [&](QByteArray bytes) {
        if (callback_timed_out) {
            return;
        }

        callback_output += bytes;
        if (!callback_output.contains(marker) || delete_started.exchange(true)) {
            return;
        }

        delete backend;

        std::lock_guard<std::mutex> lock(callback_mutex);
        callback_returned = true;
        callback_cv.notify_all();
    };
    callbacks.process_exited = [](term::Terminal_backend_exit) {};
    callbacks.error_reported = [](term::Terminal_backend_error) {};

    const term::Terminal_backend_result start_result = backend->start(
        shell_launch_config(
            QStringLiteral("sleep 0.2; echo destroy-from-output-callback; sleep 30")),
        callbacks);
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "destructor-from-callback shell starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        delete backend;
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        const bool returned = callback_cv.wait_for(lock, k_wait_timeout, [&] {
            return callback_returned;
        });
        callback_returned = returned;
        ok &= check(
            returned,
            "backend destructor returns from output callback");
    }

    if (!callback_returned && !delete_started.exchange(true)) {
        callback_timed_out = true;
        delete backend;
    }

    return ok;
}

bool test_terminate_kills_descendant_in_target_group()
{
    bool ok = true;

    QTemporaryDir pid_dir;
    ok &= check(pid_dir.isValid(), "terminate-descendant pid directory is available");
    if (!pid_dir.isValid()) {
        return false;
    }

    const QString pid_path   = pid_dir.filePath(QStringLiteral("descendant.pid"));
    const QString ready_path = pid_dir.filePath(QStringLiteral("descendant.ready"));
    std::optional<pid_t> descendant_pid;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
    const term::Terminal_backend_result start_result = backend->start(
        shell_launch_config(
            QStringLiteral(
                "echo terminate-descendant; "
                "(trap '' TERM; echo ready > %1; sleep 30) & "
                "echo $! > %2; "
                "wait").arg(shell_quote(ready_path), shell_quote(pid_path))),
        capture.callbacks());
    ok             &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "terminate-descendant shell starts");
    ok             &= check(capture.wait_for_output(QByteArrayLiteral("terminate-descendant")),
        "terminate-descendant shell ready marker reaches output");
    ok             &= check(wait_for_file(pid_path),
        "terminate-descendant shell records descendant pid");
    ok             &= check(wait_for_file(ready_path),
        "terminate-descendant shell confirms descendant is ready");
    descendant_pid  = read_pid_file(pid_path);
    ok             &= check(descendant_pid.has_value(),
        "terminate-descendant pid file is readable before terminate");
    if (descendant_pid.has_value()) {
        ok &= check(process_is_alive(*descendant_pid),
            "terminate-descendant process is alive before terminate");
    }

    const term::Terminal_backend_result terminate_result = backend->terminate();
    ok &= check(terminate_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "Linux PTY backend accepts descendant-target terminate");
    ok &= check(capture.wait_for_exit(),
        "terminate-descendant shell exits");

    if (descendant_pid.has_value()) {
        ok &= check(wait_for_process_absent(*descendant_pid),
            "terminate escalation kills descendant target group");
        if (process_is_alive(*descendant_pid)) {
            ::kill(*descendant_pid, SIGKILL);
        }
    }
    ok &= check(capture.errors_snapshot().empty(),
        "terminate-descendant shell produces no backend errors");

    return ok;
}

bool test_terminate(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
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
        "Linux PTY backend accepts terminate");
    ok &= check(capture.wait_for_exit(), "terminate fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::TERMINATED,
        "terminate reports typed terminated exit");
    ok &= check(capture.errors_snapshot().empty(),
        "terminate fixture produces no backend errors");
    ok &= check(backend->resize({1U, {24, 80}}).code ==
        term::Terminal_backend_result_code::REJECTED,
        "Linux PTY resize after terminate exit rejects");

    return ok;
}

bool test_terminate_accepts_zero_grace_policy(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
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
        "Linux PTY backend accepts zero-grace terminate");
    ok &= check(capture.wait_for_exit(), "zero-grace terminate fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::TERMINATED,
        "zero-grace terminate reports typed terminated exit");
    ok &= check(capture.errors_snapshot().empty(),
        "zero-grace terminate fixture produces no backend errors");

    return ok;
}

bool test_destructor_returns_with_running_process(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    {
        std::unique_ptr<term::Terminal_backend> backend = term::make_linux_pty_backend();
        const term::Terminal_backend_result start_result =
            backend->start(
                launch_config(fixture_path, {QStringLiteral("--hold-open")}),
                capture.callbacks());
        ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
            "destructor-return fixture starts");
        ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open")),
            "destructor-return fixture ready marker reaches output");
    }

    return ok;
}

}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: linux_pty_backend_tests <fixture-executable>\n";
        return 2;
    }

    const QString fixture_path = QString::fromLocal8Bit(argv[1]);
    bool ok = true;
    ok &= test_launch_output(fixture_path);
    ok &= test_interactive_canvas_fixture(fixture_path);
    ok &= test_missing_working_directory(fixture_path);
    ok &= test_failed_executable(fixture_path);
    ok &= test_rejection_paths(fixture_path);
    ok &= test_interrupt(fixture_path);
    ok &= test_interrupt_without_stdin_reader(fixture_path);
    ok &= test_interrupt_ignored_child_exits_normally();
    ok &= test_process_exit_reports_with_descendant_slave_open();
    ok &= test_exit_drain_ignores_repause_and_delivers_buffered_output();
    ok &= test_destructor_from_output_callback_returns();
    ok &= test_terminate_kills_descendant_in_target_group();
    ok &= test_terminate(fixture_path);
    ok &= test_terminate_accepts_zero_grace_policy(fixture_path);
    ok &= test_destructor_returns_with_running_process(fixture_path);
    return ok ? 0 : 1;
}
