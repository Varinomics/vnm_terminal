#include "vnm_terminal/internal/terminal_canvas_fixture_contract.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/vnm_terminal_surface.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQuickWindow>
#include <QString>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

constexpr quint32 k_red_rgba = 0xffcd0000U;

struct command_result_t
{
    bool               started   = false;
    bool               timed_out = false;
    int                exit_code = -1;
    QByteArray         stdout_bytes;
    QByteArray         stderr_bytes;
    QString            error_string;
};

struct smoke_payload_t
{
    std::string_view   name;
    QByteArray         bytes;
};

class Surface_smoke_backend final : public term::Terminal_backend
{
public:
    term::Terminal_backend_result start(
        const term::Terminal_launch_config&    config,
        term::Terminal_backend_callbacks       callbacks) override
    {
        const term::Terminal_backend_result callback_result =
            term::validate_backend_callbacks(callbacks);
        if (term::is_backend_rejection(callback_result)) {
            return callback_result;
        }

        const term::Terminal_backend_result config_result =
            term::validate_launch_config(config);
        if (term::is_backend_rejection(config_result)) {
            return config_result;
        }

        m_callbacks = std::move(callbacks);
        running = true;
        for (const QByteArray& output : outputs_during_start) {
            m_callbacks.output_received(output);
        }

        return term::backend_accept();
    }

    term::Terminal_backend_result write(QByteArray bytes) override
    {
        writes.push_back(std::move(bytes));
        return term::backend_accept();
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request request) override
    {
        resize_requests.push_back(request);
        if (!term::is_valid_grid_size(request.grid_size)) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::RESIZE_FAILED,
                    QStringLiteral("surface smoke resize requires a valid grid"));
        }

        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool) override
    {
        return term::backend_accept();
    }

    term::Terminal_backend_result interrupt() override
    {
        if (!running) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::INTERRUPT_FAILED,
                    QStringLiteral("surface smoke interrupt requires a running process"));
        }

        running = false;
        m_callbacks.process_exited({term::Terminal_exit_reason::INTERRUPTED, 130});
        return term::backend_accept();
    }

    term::Terminal_backend_result terminate() override
    {
        if (!running) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::TERMINATE_FAILED,
                    QStringLiteral("surface smoke terminate requires a running process"));
        }

        running = false;
        m_callbacks.process_exited({term::Terminal_exit_reason::TERMINATED, 0});
        return term::backend_accept();
    }

    bool                       running = false;
    std::vector<QByteArray>    outputs_during_start;
    std::vector<QByteArray>    writes;
    std::vector<term::Terminal_backend_resize_request>
                               resize_requests;

private:
    term::Terminal_backend_callbacks m_callbacks;
};

struct Surface_fixture
{
    QQuickWindow           window;
    VNM_TerminalSurface    surface;

    Surface_fixture()
    {
        window.resize(900, 520);
        surface.setParentItem(window.contentItem());
        surface.setSize(QSizeF(800.0, 400.0));
        surface.set_font_family(QStringLiteral("monospace"));
        surface.set_font_size(12.0);
        window.show();
    }
};

using vnm_terminal::test_helpers::check;

void pump_events(QGuiApplication& app, int rounds = 8)
{
    for (int i = 0; i < rounds; ++i) {
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
}

bool argument_equals(const char* argument, const char* expected)
{
    return std::string_view(argument) == expected;
}

QByteArray byte_array_from_view(std::string_view text)
{
    return QByteArray(text.data(), static_cast<qsizetype>(text.size()));
}

bool is_non_empty_hex(std::string_view text)
{
    if (text.empty() || (text.size() % 2U) != 0U) {
        return false;
    }

    for (char ch : text) {
        const bool digit = ch >= '0' && ch <= '9';
        const bool lower = ch >= 'a' && ch <= 'f';
        const bool upper = ch >= 'A' && ch <= 'F';
        if (!digit && !lower && !upper) {
            return false;
        }
    }

    return true;
}

QByteArray bytes_from_hex(std::string_view hex)
{
    return QByteArray::fromHex(byte_array_from_view(hex));
}

QString text_from_hex(std::string_view hex)
{
    return QString::fromUtf8(bytes_from_hex(hex));
}

QByteArray expected_payload(
    const term::terminal_canvas_fixture_behavior_smoke_case_t& smoke_case)
{
    // Keep this independent from the fixture's decoder so the test cross-checks
    // the fixture output instead of sharing its implementation.
    const QByteArray unit = bytes_from_hex(smoke_case.payload_hex);

    QByteArray payload;
    payload.reserve(unit.size() * smoke_case.repeat_count);
    for (int i = 0; i < smoke_case.repeat_count; ++i) {
        payload.append(unit);
    }
    return payload;
}

QProcessEnvironment host_process_environment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"),             QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_SCALE_FACTOR"),             QStringLiteral("1"));
    environment.insert(QStringLiteral("QT_SCREEN_SCALE_FACTORS"),     QString());
    environment.insert(QStringLiteral("QT_AUTO_SCREEN_SCALE_FACTOR"), QStringLiteral("0"));
    environment.insert(QStringLiteral("QT_DEVICE_PIXEL_RATIO"),       QString());
    environment.insert(
        QStringLiteral("QT_SCALE_FACTOR_ROUNDING_POLICY"),
        QStringLiteral("PassThrough"));
    return environment;
}

command_result_t run_process(
    const QString&     program,
    const QStringList& arguments,
    int                timeout_ms,
    bool               use_host_environment = false)
{
    command_result_t result;

    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    if (use_host_environment) {
        process.setProcessEnvironment(host_process_environment());
    }
    process.start();

    if (!process.waitForStarted(5000)) {
        result.error_string = process.errorString();
        return result;
    }
    result.started = true;

    if (!process.waitForFinished(timeout_ms)) {
        process.kill();
        process.waitForFinished(5000);
        result.timed_out    = true;
        result.stdout_bytes = process.readAllStandardOutput();
        result.stderr_bytes = process.readAllStandardError();
        result.error_string = process.errorString();
        return result;
    }

    result.exit_code    = process.exitCode();
    result.stdout_bytes = process.readAllStandardOutput();
    result.stderr_bytes = process.readAllStandardError();
    result.error_string = process.errorString();
    return result;
}

bool check_process_success(const command_result_t& result, const std::string& label)
{
    bool ok = true;
    ok &= check(result.started, label + " starts");
    ok &= check(!result.timed_out, label + " finishes before timeout");
    ok &= check(result.exit_code == 0, label + " exits successfully");

    if (!ok) {
        if (!result.error_string.isEmpty()) {
            std::cerr << "process error: "
                << result.error_string.toLocal8Bit().constData() << '\n';
        }
        if (!result.stderr_bytes.isEmpty()) {
            std::cerr << "stderr:\n" << result.stderr_bytes.constData() << '\n';
        }
    }

    return ok;
}

std::vector<QByteArray> non_empty_lines(const QByteArray& bytes)
{
    std::vector<QByteArray> lines;
    for (QByteArray line : bytes.split('\n')) {
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (!line.isEmpty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

bool validate_behavior_smoke_contract()
{
    bool ok = true;
    const std::vector<term::terminal_canvas_fixture_behavior_smoke_case_t>& smoke_cases =
        term::terminal_canvas_fixture_behavior_smoke_cases();
    std::set<std::string_view> names;

    for (const term::terminal_canvas_fixture_behavior_smoke_case_t& smoke_case : smoke_cases) {
        ok &= check(!smoke_case.name.empty(),
            "behavior smoke contract case has a name");
        ok &= check(names.insert(smoke_case.name).second,
            "behavior smoke contract case name is unique: " +
                std::string(smoke_case.name));
        ok &= check(is_non_empty_hex(smoke_case.payload_hex),
            std::string("behavior smoke payload is non-empty valid hex: ") +
                std::string(smoke_case.name));
        ok &= check(smoke_case.repeat_count > 0,
            std::string("behavior smoke repeat count is positive: ") +
                std::string(smoke_case.name));
    }

    return ok;
}

std::vector<smoke_payload_t> fixture_behavior_outputs(
    const QString& fixture_path,
    bool&          ok)
{
    std::vector<smoke_payload_t> payloads;
    const std::vector<term::terminal_canvas_fixture_behavior_smoke_case_t>& smoke_cases =
        term::terminal_canvas_fixture_behavior_smoke_cases();

    const command_result_t list_result = run_process(
        fixture_path,
        { QStringLiteral("--list-behavior-smokes") },
        5000);
    ok &= check_process_success(list_result, "fixture behavior smoke list");
    ok &= check(list_result.stderr_bytes.isEmpty(), "behavior smoke list has no stderr");

    const std::vector<QByteArray> lines = non_empty_lines(list_result.stdout_bytes);
    ok &= check(lines.size() == smoke_cases.size(),
        "behavior smoke list has expected case count");
    if (lines.size() == smoke_cases.size()) {
        for (std::size_t i = 0U; i < smoke_cases.size(); ++i) {
            const QByteArray expected = byte_array_from_view(smoke_cases[i].name);
            ok &= check(lines[i] == expected,
                "behavior smoke list entry " + std::to_string(i) +
                " matches shared contract order: expected " +
                    expected.toStdString() + ", got " + lines[i].toStdString());
        }
    }

    payloads.reserve(smoke_cases.size());
    for (const term::terminal_canvas_fixture_behavior_smoke_case_t& smoke_case : smoke_cases) {
        const command_result_t result = run_process(
            fixture_path,
            {
                QStringLiteral("--behavior-smoke"),
                QString::fromLatin1(
                    smoke_case.name.data(),
                    static_cast<qsizetype>(smoke_case.name.size())),
            },
            5000);
        const QByteArray expected = expected_payload(smoke_case);
        const std::string label =
            std::string("fixture behavior smoke: ") + std::string(smoke_case.name);

        ok &= check_process_success(result, label);
        ok &= check(result.stderr_bytes.isEmpty(), label + " has no stderr");
        ok &= check(result.stdout_bytes == expected, label + " emits expected bytes");
        payloads.push_back({smoke_case.name, result.stdout_bytes});
    }

    const command_result_t missing_result = run_process(
        fixture_path,
        {
            QStringLiteral("--behavior-smoke"),
            QStringLiteral("missing"),
        },
        5000);
    ok &= check(missing_result.started, "fixture unknown behavior smoke starts");
    ok &= check(!missing_result.timed_out, "fixture unknown behavior smoke finishes");
    ok &= check(missing_result.exit_code == 2, "fixture rejects unknown behavior smoke");
    ok &= check(missing_result.stdout_bytes.isEmpty(),
        "fixture unknown behavior smoke emits no stdout");
    ok &= check(missing_result.stderr_bytes.contains("unknown behavior smoke"),
        "fixture unknown behavior smoke reports stderr diagnostic");

    return payloads;
}

const QByteArray* payload_by_name(
    const std::vector<smoke_payload_t>&    payloads,
    std::string_view                       name,
    bool&                                  ok)
{
    for (const smoke_payload_t& payload : payloads) {
        if (payload.name == name) {
            return &payload.bytes;
        }
    }

    ok &= check(false, std::string("expected cached behavior smoke payload: ") +
        std::string(name));
    return nullptr;
}

const term::terminal_canvas_fixture_behavior_smoke_case_t* smoke_case_by_name(
    std::string_view   name,
    bool&              ok)
{
    for (const term::terminal_canvas_fixture_behavior_smoke_case_t& smoke_case :
        term::terminal_canvas_fixture_behavior_smoke_cases())
    {
        if (smoke_case.name == name) {
            return &smoke_case;
        }
    }

    ok &= check(false, std::string("expected behavior smoke contract case: ") +
        std::string(name));
    return nullptr;
}

QString snapshot_row_text(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row)
{
    QString text;
    for (int column = 0; column < snapshot.grid_size.columns; ++column) {
        QString cell_text = QStringLiteral(" ");
        for (const term::Terminal_render_cell& cell : snapshot.cells) {
            if (cell.position.row == row && cell.position.column == column) {
                cell_text = cell.text;
                break;
            }
        }
        text += cell_text;
    }

    while (!text.isEmpty() && text.back() == QChar(u' ')) {
        text.chop(1);
    }
    return text;
}

bool snapshot_contains_text(
    const term::Terminal_render_snapshot&  snapshot,
    const QString&                         text)
{
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        if (snapshot_row_text(snapshot, row).contains(text)) {
            return true;
        }
    }

    return false;
}

term::Terminal_screen_model make_model(
    int    rows,
    int    columns,
    int    scrollback_limit = 16)
{
    return term::Terminal_screen_model({
        term::terminal_grid_size_t{rows, columns},
        scrollback_limit,
        4,
    });
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

bool snapshot_valid(const term::Terminal_screen_model& model, std::uint64_t sequence)
{
    return term::validate_render_snapshot(model.render_snapshot(sequence)).status ==
        term::Terminal_render_snapshot_status::OK;
}

const term::Terminal_render_cell* find_cell(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    column)
{
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row == row && cell.position.column == column) {
            return &cell;
        }
    }

    return nullptr;
}

const term::Terminal_text_style* style_for_cell(
    const term::Terminal_render_snapshot&  snapshot,
    const term::Terminal_render_cell&      cell)
{
    const std::size_t style_index = static_cast<std::size_t>(cell.style_id);
    if (style_index >= snapshot.styles.size()) {
        return nullptr;
    }

    return &snapshot.styles[style_index];
}

bool check_cell(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    column,
    const QString&                         text,
    int                                    display_width,
    bool                                   wide_continuation,
    const char*                            label)
{
    bool ok = true;
    const term::Terminal_render_cell* cell = find_cell(snapshot, row, column);
    ok &= check(cell != nullptr, label);
    if (cell == nullptr) {
        return false;
    }

    ok &= check(cell->text == text, label);
    ok &= check(cell->display_width == display_width, label);
    ok &= check(cell->wide_continuation == wide_continuation, label);
    return ok;
}

bool check_no_cell(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    column,
    const char*                            label)
{
    return check(find_cell(snapshot, row, column) == nullptr, label);
}

bool check_cell_colors(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    column,
    quint32                                foreground,
    quint32                                background,
    const char*                            label)
{
    bool ok = true;
    const term::Terminal_render_cell* cell = find_cell(snapshot, row, column);
    ok &= check(cell != nullptr, label);
    if (cell == nullptr) {
        return false;
    }

    const term::Terminal_text_style* style = style_for_cell(snapshot, *cell);
    ok &= check(style != nullptr, label);
    if (style == nullptr) {
        return false;
    }

    ok &= check(
        term::resolve_terminal_color_ref(style->foreground, snapshot.color_state, true) ==
            foreground,
        label);
    ok &= check(
        term::resolve_terminal_color_ref(style->background, snapshot.color_state, false) ==
            background,
        label);
    return ok;
}

bool check_cell_attribute(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    column,
    term::Terminal_style_attribute         attribute,
    bool                                   expected,
    const char*                            label)
{
    bool ok = true;
    const term::Terminal_render_cell* cell = find_cell(snapshot, row, column);
    ok &= check(cell != nullptr, label);
    if (cell == nullptr) {
        return false;
    }

    const term::Terminal_text_style* style = style_for_cell(snapshot, *cell);
    ok &= check(style != nullptr, label);
    if (style == nullptr) {
        return false;
    }

    ok &= check(term::terminal_style_has_attribute(*style, attribute) == expected, label);
    return ok;
}

bool ingest_chunks(
    term::Terminal_screen_model&           model,
    const QByteArray&                      bytes,
    const std::vector<int>&                chunk_sizes,
    const char*                            label)
{
    bool ok          = true;
    int  offset      = 0;
    int  chunk_index = 0;
    for (int chunk_size : chunk_sizes) {
        if (offset >= bytes.size()) {
            break;
        }

        const int remaining          = static_cast<int>(bytes.size()) - offset;
        const int bounded_chunk_size = std::min(chunk_size, remaining);
        const term::Terminal_screen_model_result result =
            model.ingest(bytes.mid(offset, bounded_chunk_size));
        ok &= check(diagnostic_count(result) == 0,
            std::string(label) + " chunk " + std::to_string(chunk_index) +
            " at offset " + std::to_string(offset));
        offset += bounded_chunk_size;
        ++chunk_index;
    }

    if (offset < bytes.size()) {
        const term::Terminal_screen_model_result result = model.ingest(bytes.mid(offset));
        ok &= check(diagnostic_count(result) == 0,
            std::string(label) + " final chunk at offset " + std::to_string(offset));
    }

    return ok;
}

bool test_cursor_addressing_smoke(const QByteArray& bytes)
{
    bool                                     ok     = true;
    term::Terminal_screen_model              model  = make_model(4, 8);
    const term::Terminal_screen_model_result result = model.ingest(bytes);

    ok &= check(diagnostic_count(result) == 0, "cursor smoke has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("CD"), "cursor smoke final row 0");
    ok &= check(model.row_text(1) == QStringLiteral("  B"), "cursor smoke final row 1");
    ok &= check(model.cursor_position().row == 0 && model.cursor_position().column == 2,
        "cursor smoke final cursor");
    ok &= check(snapshot_valid(model, 1U), "cursor smoke snapshot validates");
    return ok;
}

bool test_cursor_addressing_split_smoke(const QByteArray& bytes)
{
    bool ok = true;
    term::Terminal_screen_model model = make_model(4, 8);
    ok &= ingest_chunks(model, bytes, {4, 2, 1, 4, 5}, "split cursor smoke chunks");
    ok &= check(model.row_text(0) == QStringLiteral("CD"),
        "split cursor smoke final row 0");
    ok &= check(model.row_text(1) == QStringLiteral("  B"),
        "split cursor smoke final row 1");
    ok &= check(model.cursor_position().row == 0 && model.cursor_position().column == 2,
        "split cursor smoke final cursor");
    ok &= check(snapshot_valid(model, 11U), "split cursor smoke snapshot validates");
    return ok;
}

bool test_erase_line_screen_smoke(const QByteArray& bytes)
{
    bool                                     ok     = true;
    term::Terminal_screen_model              model  = make_model(2, 8);
    const term::Terminal_screen_model_result result = model.ingest(bytes);

    ok &= check(diagnostic_count(result) == 0, "erase smoke has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("ab"), "erase smoke final row 0");
    ok &= check(model.row_text(1) == QStringLiteral("    56"), "erase smoke final row 1");
    ok &= check(snapshot_valid(model, 2U), "erase smoke snapshot validates");
    return ok;
}

bool test_alternate_screen_smoke(const QByteArray& bytes)
{
    bool                                     ok     = true;
    term::Terminal_screen_model              model  = make_model(3, 8);
    const term::Terminal_screen_model_result result = model.ingest(bytes);

    ok &= check(diagnostic_count(result) == 0, "1049 smoke has no diagnostics");
    ok &= check(model.active_buffer_id() == term::Terminal_buffer_id::PRIMARY,
        "1049 smoke returns to primary");
    ok &= check(model.row_text(0) == QStringLiteral("P"), "1049 smoke final row 0");
    ok &= check(model.row_text(1) == QStringLiteral("   B"), "1049 smoke final row 1");

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(3U);
    ok &= check(snapshot.viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
        "1049 smoke snapshot reports primary");
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "1049 smoke snapshot validates");
    return ok;
}

bool check_unicode_width_snapshot(
    const term::Terminal_render_snapshot&  snapshot,
    const char*                            label);

bool test_unicode_width_smoke(const QByteArray& bytes)
{
    bool                                     ok     = true;
    term::Terminal_screen_model              model  = make_model(1, 24);
    const term::Terminal_screen_model_result result = model.ingest(bytes);

    ok &= check(diagnostic_count(result) == 0, "unicode smoke has no diagnostics");
    ok &= check(model.cursor_position().row == 0 && model.cursor_position().column == 16,
        "unicode smoke final cursor width");

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(4U);
    ok &= check_unicode_width_snapshot(snapshot, "unicode smoke");
    return ok;
}

bool check_unicode_width_snapshot(
    const term::Terminal_render_snapshot&  snapshot,
    const char*                            label)
{
    bool ok = true;
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        std::string(label) + " snapshot validates");
    ok &= check_cell(snapshot, 0, 0, QStringLiteral("A"), 1, false,
        "unicode smoke ASCII A cell");
    ok &= check_cell(snapshot, 0, 1, text_from_hex("e7958c"), 2, false,
        "unicode smoke CJK wide base");
    ok &= check_cell(snapshot, 0, 2, QString(), 0, true,
        "unicode smoke CJK continuation");
    ok &= check_cell(snapshot, 0, 3, QStringLiteral("B"), 1, false,
        "unicode smoke ASCII B cell");
    ok &= check_cell(snapshot, 0, 4, text_from_hex("65cc81"), 1, false,
        "unicode smoke combining mark stays with base");
    ok &= check_cell(snapshot, 0, 5, QStringLiteral("C"), 1, false,
        "unicode smoke ASCII C cell");
    ok &= check_cell(snapshot, 0, 6, text_from_hex("e29da4efb88e"), 1, false,
        "unicode smoke text variation heart is narrow");
    ok &= check_cell(snapshot, 0, 7, QStringLiteral("D"), 1, false,
        "unicode smoke ASCII D cell");
    ok &= check_cell(snapshot, 0, 8, text_from_hex("e29da4efb88f"), 2, false,
        "unicode smoke emoji variation heart is wide");
    ok &= check_cell(snapshot, 0, 9, QString(), 0, true,
        "unicode smoke emoji variation continuation");
    ok &= check_cell(snapshot, 0, 10, QStringLiteral("E"), 1, false,
        "unicode smoke ASCII E cell");
    ok &= check_cell(snapshot, 0, 11, text_from_hex("f09f9880"), 2, false,
        "unicode smoke default emoji is wide");
    ok &= check_cell(snapshot, 0, 12, QString(), 0, true,
        "unicode smoke default emoji continuation");
    ok &= check_cell(snapshot, 0, 13, QStringLiteral("F"), 1, false,
        "unicode smoke ASCII F cell");
    ok &= check_cell(snapshot, 0, 14, text_from_hex("cea9"), 1, false,
        "unicode smoke ambiguous omega is narrow");
    ok &= check_cell(snapshot, 0, 15, QStringLiteral("G"), 1, false,
        "unicode smoke trailing ASCII G cell");
    for (int column = 16; column < 24; ++column) {
        ok &= check_no_cell(snapshot, 0, column, "unicode smoke trailing blank column");
    }
    return ok;
}

bool test_unicode_width_split_smoke(const QByteArray& bytes)
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(1, 24);
    ok &= ingest_chunks(
        model,
        bytes,
        { 2, 2, 1, 1, 2, 3, 1, 4, 1, 2 },
        "split unicode smoke chunks");
    ok &= check(model.cursor_position().row == 0 && model.cursor_position().column == 16,
        "split unicode smoke final cursor width");

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(14U);
    ok &= check_unicode_width_snapshot(snapshot, "split unicode smoke");
    return ok;
}

bool test_output_burst_smoke(
    const QByteArray&      bytes,
    const term::terminal_canvas_fixture_behavior_smoke_case_t&
                           smoke_case)
{
    bool                                     ok      = true;
    term::Terminal_screen_model              model   = make_model(3, 16, 8);
    const term::Terminal_screen_model_result result  = model.ingest(bytes);
    const QString                            pattern = text_from_hex(smoke_case.payload_hex);

    ok &= check(diagnostic_count(result) == 0, "burst smoke has no diagnostics");
    ok &= check(model.row_text(0) == pattern, "burst smoke final row 0");
    ok &= check(model.row_text(1) == pattern, "burst smoke final row 1");
    ok &= check(model.row_text(2) == pattern, "burst smoke final row 2");
    ok &= check(model.scrollback_size() == 8, "burst smoke keeps bounded scrollback");
    ok &= check(snapshot_valid(model, 5U), "burst smoke snapshot validates");
    return ok;
}

bool test_output_burst_split_smoke(
    const QByteArray&      bytes,
    const term::terminal_canvas_fixture_behavior_smoke_case_t&
                           smoke_case)
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(3, 16, 8);
    ok &= ingest_chunks(
        model,
        bytes,
        { 1, 17, 64, 511, 1024, 2048, 4096 },
        "split burst smoke chunks");

    const QString pattern = text_from_hex(smoke_case.payload_hex);
    ok &= check(model.row_text(0) == pattern, "split burst smoke final row 0");
    ok &= check(model.row_text(1) == pattern, "split burst smoke final row 1");
    ok &= check(model.row_text(2) == pattern, "split burst smoke final row 2");
    ok &= check(model.scrollback_size() == 8, "split burst smoke keeps bounded scrollback");
    ok &= check(snapshot_valid(model, 15U), "split burst smoke snapshot validates");
    return ok;
}

bool test_sgr_reset_interactions_smoke(const QByteArray& bytes)
{
    bool                                     ok     = true;
    term::Terminal_screen_model              model  = make_model(1, 8);
    const term::Terminal_screen_model_result result = model.ingest(bytes);
    ok &= check(diagnostic_count(result) == 0, "SGR smoke has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("ABCDEF"), "SGR smoke row text");

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(6U);
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "SGR smoke snapshot validates");
    ok &= check_cell_colors(
        snapshot,
        0,
        0,
        k_red_rgba,
        term::k_terminal_default_background_rgba,
        "SGR smoke A captures red foreground");
    ok &= check_cell_attribute(
        snapshot,
        0,
        1,
        term::Terminal_style_attribute::BOLD,
        true,
        "SGR smoke B captures bold");
    ok &= check_cell_attribute(
        snapshot,
        0,
        2,
        term::Terminal_style_attribute::UNDERLINE,
        true,
        "SGR smoke C captures underline");
    ok &= check_cell_attribute(
        snapshot,
        0,
        3,
        term::Terminal_style_attribute::BOLD,
        false,
        "SGR smoke D has bold reset");
    ok &= check_cell_colors(
        snapshot,
        0,
        4,
        term::k_terminal_default_foreground_rgba,
        term::k_terminal_default_background_rgba,
        "SGR smoke E has foreground reset");
    ok &= check_cell_attribute(
        snapshot,
        0,
        4,
        term::Terminal_style_attribute::UNDERLINE,
        true,
        "SGR smoke E keeps underline after foreground reset");
    ok &= check_cell_colors(
        snapshot,
        0,
        5,
        term::k_terminal_default_foreground_rgba,
        term::k_terminal_default_background_rgba,
        "SGR smoke F has full reset colors");
    ok &= check_cell_attribute(
        snapshot,
        0,
        5,
        term::Terminal_style_attribute::UNDERLINE,
        false,
        "SGR smoke F has underline reset");
    return ok;
}

bool test_decstbm_scroll_region_smoke(const QByteArray& bytes)
{
    bool                                     ok     = true;
    term::Terminal_screen_model              model  = make_model(4, 5);
    const term::Terminal_screen_model_result result = model.ingest(bytes);
    ok &= check(diagnostic_count(result) == 0, "DECSTBM smoke has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("111"),
        "DECSTBM smoke preserves row above region");
    ok &= check(model.row_text(1) == QStringLiteral("333"),
        "DECSTBM smoke scrolls middle row up");
    ok &= check(model.row_text(2).isEmpty(),
        "DECSTBM smoke clears region bottom row");
    ok &= check(model.row_text(3) == QStringLiteral("444"),
        "DECSTBM smoke preserves row below region");
    ok &= check(model.scrollback_size() == 0,
        "DECSTBM smoke does not append region scrollback");
    ok &= check(snapshot_valid(model, 7U), "DECSTBM smoke snapshot validates");
    return ok;
}

bool test_resize_output_ordering_smoke()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(2, 6, 8);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("12\x1b[2;5HAB"));
    ok &= check(diagnostic_count(result) == 0, "resize smoke setup has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("12"), "resize smoke setup row 0");
    ok &= check(model.row_text(1) == QStringLiteral("    AB"), "resize smoke setup row 1");

    result  = model.resize({3, 8});
    ok     &= check(result.viewport_changed, "resize smoke resize reports viewport change");
    ok     &= check(model.row_text(0) == QStringLiteral("12"),
        "resize smoke preserves row 0 after resize");
    ok     &= check(model.row_text(1) == QStringLiteral("    AB"),
        "resize smoke preserves row 1 after resize");

    result  = model.ingest(QByteArrayLiteral("\x1b[3;6HCD"));
    ok     &= check(diagnostic_count(result) == 0, "resize smoke post-output has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("12"),
        "resize smoke final row 0 ordering");
    ok     &= check(model.row_text(1) == QStringLiteral("    AB"),
        "resize smoke final row 1 ordering");
    ok     &= check(model.row_text(2) == QStringLiteral("     CD"),
        "resize smoke final row 2 ordering");
    ok     &= check(model.cursor_position().row == 2 && model.cursor_position().column == 7,
        "resize smoke final cursor");
    ok     &= check(snapshot_valid(model, 8U), "resize smoke snapshot validates");
    return ok;
}

bool run_host_behavior_smokes(const QString& fixture_path, const QString& example_path)
{
    bool ok = true;

    for (const term::terminal_canvas_fixture_behavior_smoke_case_t& smoke_case :
        term::terminal_canvas_fixture_behavior_smoke_cases())
    {
        const command_result_t result = run_process(
            example_path,
            {
                QStringLiteral("--software-renderer"),
                QStringLiteral("--window-size"),
                QStringLiteral("640x320"),
                QStringLiteral("--timeout-ms"),
                QStringLiteral("10000"),
                QStringLiteral("--require-output"),
                QStringLiteral("--"),
                fixture_path,
                QStringLiteral("--behavior-smoke"),
                QString::fromLatin1(
                    smoke_case.name.data(),
                    static_cast<qsizetype>(smoke_case.name.size())),
            },
            20000,
            true);
        ok &= check_process_success(
            result,
            std::string("example host behavior smoke: ") + std::string(smoke_case.name));
        ok &= check(!result.stderr_bytes.contains("vnm_terminal_example_terminal:"),
            "example host behavior smoke has no example error: " +
                std::string(smoke_case.name));
    }

    return ok;
}

bool text_is_repeated_pattern_row(const QString& text, const QString& pattern)
{
    if (text.isEmpty()) {
        return false;
    }

    for (int offset = 0; offset < pattern.size(); ++offset) {
        bool matches = true;
        for (int i = 0; i < text.size(); ++i) {
            if (text[i] != pattern[(offset + i) % pattern.size()]) {
                matches = false;
                break;
            }
        }

        if (matches) {
            return true;
        }
    }

    return false;
}

bool check_surface_snapshot_for_smoke(
    std::string_view                       name,
    const term::Terminal_render_snapshot&  snapshot)
{
    bool ok = true;
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "surface behavior smoke snapshot validates: " + std::string(name));
    ok &= check(snapshot.grid_size.rows >= 5 && snapshot.grid_size.columns >= 24,
        "surface behavior smoke grid is large enough for assertions: " +
            std::string(name));
    if (snapshot.grid_size.rows < 5 || snapshot.grid_size.columns < 24) {
        return false;
    }

    if (name == "cursor-addressing") {
        ok &= check(snapshot_row_text(snapshot, 0) == QStringLiteral("CD"),
            "surface cursor smoke final row 0");
        ok &= check(snapshot_row_text(snapshot, 1) == QStringLiteral("  B"),
            "surface cursor smoke final row 1");
        ok &= check(snapshot.cursor.position.row == 0 &&
            snapshot.cursor.position.column == 2,
            "surface cursor smoke final cursor");
        return ok;
    }

    if (name == "erase-line-screen") {
        ok &= check(snapshot_row_text(snapshot, 0) == QStringLiteral("ab"),
            "surface erase smoke final row 0");
        ok &= check(snapshot_row_text(snapshot, 1) == QStringLiteral("    56"),
            "surface erase smoke final row 1");
        return ok;
    }

    if (name == "alternate-screen-1049") {
        ok &= check(snapshot.viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
            "surface 1049 smoke returns to primary");
        ok &= check(snapshot_row_text(snapshot, 0) == QStringLiteral("P"),
            "surface 1049 smoke final row 0");
        ok &= check(snapshot_row_text(snapshot, 1) == QStringLiteral("   B"),
            "surface 1049 smoke final row 1");
        return ok;
    }

    if (name == "unicode-width") {
        ok &= check(snapshot.cursor.position.row == 0 &&
            snapshot.cursor.position.column == 16,
            "surface unicode smoke final cursor width");
        ok &= check_unicode_width_snapshot(snapshot, "surface unicode smoke");
        return ok;
    }

    if (name == "output-burst") {
        const QString pattern     = QStringLiteral("0123456789abcdef");
        const int     sample_rows = std::min(snapshot.grid_size.rows, 3);
        for (int row = 0; row < sample_rows; ++row) {
            const QString text = snapshot_row_text(snapshot, row);
            ok &= check(text.size() == snapshot.grid_size.columns,
                "surface burst smoke sampled row is full width");
            ok &= check(text_is_repeated_pattern_row(text, pattern),
                "surface burst smoke sampled row is repeated pattern");
        }
        ok &= check(snapshot.viewport.scrollback_rows > 0,
            "surface burst smoke produces scrollback");
        return ok;
    }

    if (name == "sgr-reset-interactions") {
        ok &= check(snapshot_row_text(snapshot, 0) == QStringLiteral("ABCDEF"),
            "surface SGR smoke row text");
        ok &= check_cell_colors(
            snapshot,
            0,
            0,
            k_red_rgba,
            term::k_terminal_default_background_rgba,
            "surface SGR smoke A captures red foreground");
        ok &= check_cell_attribute(
            snapshot,
            0,
            1,
            term::Terminal_style_attribute::BOLD,
            true,
            "surface SGR smoke B captures bold");
        ok &= check_cell_attribute(
            snapshot,
            0,
            2,
            term::Terminal_style_attribute::UNDERLINE,
            true,
            "surface SGR smoke C captures underline");
        ok &= check_cell_attribute(
            snapshot,
            0,
            5,
            term::Terminal_style_attribute::UNDERLINE,
            false,
            "surface SGR smoke F has underline reset");
        return ok;
    }

    if (name == "decstbm-scroll-region") {
        ok &= check(snapshot_row_text(snapshot, 0) == QStringLiteral("111"),
            "surface DECSTBM smoke preserves row above region");
        ok &= check(snapshot_row_text(snapshot, 1) == QStringLiteral("333"),
            "surface DECSTBM smoke scrolls middle row up");
        ok &= check(snapshot_row_text(snapshot, 2).isEmpty(),
            "surface DECSTBM smoke clears region bottom row");
        ok &= check(snapshot_row_text(snapshot, 3) == QStringLiteral("444"),
            "surface DECSTBM smoke preserves row below region");
        return ok;
    }

    if (name == "codex-scrollback-insert") {
        ok &= check(snapshot.viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
            "surface Codex scrollback insert stays on primary");
        ok &= check(snapshot.viewport.scrollback_rows == 1,
            "surface Codex scrollback insert produces one scrollback row");
        ok &= check(snapshot_row_text(snapshot, 0) == QStringLiteral("top-two"),
            "surface Codex scrollback insert shifts second row to viewport top");
        ok &= check(snapshot_row_text(snapshot, 1) == QStringLiteral("view"),
            "surface Codex scrollback insert keeps view row");
        ok &= check(snapshot_row_text(snapshot, 2) == QStringLiteral("HIST"),
            "surface Codex scrollback insert keeps first inserted row");
        ok &= check(snapshot_row_text(snapshot, 3) == QStringLiteral("NEXT"),
            "surface Codex scrollback insert writes overflowing inserted row");
        ok &= check(snapshot_row_text(snapshot, 4) == QStringLiteral("below"),
            "surface Codex scrollback insert preserves row below insert region");
        ok &= check(snapshot.cursor.position.row == 4 &&
            snapshot.cursor.position.column == 0,
            "surface Codex scrollback insert final cursor");
        return ok;
    }

    ok &= check(false, "surface behavior smoke has snapshot expectation: " +
        std::string(name));
    return ok;
}

bool run_surface_behavior_smokes(QGuiApplication& app)
{
    bool ok = true;

    for (const term::terminal_canvas_fixture_behavior_smoke_case_t& smoke_case :
        term::terminal_canvas_fixture_behavior_smoke_cases())
    {
        Surface_fixture fixture;
        pump_events(app);

        int activity_count = 0;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::output_activity,
            &fixture.surface,
            [&activity_count] {
                ++activity_count;
            });

        auto backend = std::make_unique<Surface_smoke_backend>();
        backend->outputs_during_start = {expected_payload(smoke_case)};
        Surface_smoke_backend* backend_ptr = backend.get();

        const bool started = term::VNM_TerminalSurface_render_bridge::start_process_with_backend(
            fixture.surface,
            std::move(backend),
            {QStringLiteral("surface-behavior-smoke")});
        pump_events(app);
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);

        ok &= check(started,
            "surface behavior smoke starts: " + std::string(smoke_case.name));
        ok &= check(backend_ptr->running,
            "surface behavior smoke backend remains running: " + std::string(smoke_case.name));
        ok &= check(activity_count >= 1,
            "surface behavior smoke emits output activity: " + std::string(smoke_case.name));
        ok &= check(snapshot != nullptr,
            "surface behavior smoke publishes snapshot: " + std::string(smoke_case.name));
        if (snapshot != nullptr) {
            ok &= check_surface_snapshot_for_smoke(smoke_case.name, *snapshot);
        }
    }

    return ok;
}

bool wait_for_process_exit(QGuiApplication& app, bool& process_exited, int timeout_ms)
{
    int elapsed_ms = 0;
    while (!process_exited && elapsed_ms < timeout_ms) {
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
        elapsed_ms += 60;
    }

    return process_exited;
}

bool run_native_surface_behavior_smokes(QGuiApplication& app, const QString& fixture_path)
{
    bool ok = true;

    for (const term::terminal_canvas_fixture_behavior_smoke_case_t& smoke_case :
        term::terminal_canvas_fixture_behavior_smoke_cases())
    {
        Surface_fixture fixture;
        pump_events(app);

        int backend_error_count = 0;
        QStringList backend_errors;
        bool process_exited    = false;
        int  process_exit_code = -1;

        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString& message) {
                ++backend_error_count;
                backend_errors.push_back(
                    QStringLiteral("%1: %2")
                        .arg(static_cast<int>(code))
                        .arg(message));
            });
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::process_exited,
            &fixture.surface,
            [&](VNM_TerminalSurface::Exit_reason, int exit_code) {
                process_exited = true;
                process_exit_code = exit_code;
            });

        const QString smoke_name = QString::fromLatin1(
            smoke_case.name.data(),
            static_cast<qsizetype>(smoke_case.name.size()));
        const bool started = fixture.surface.start_process({
            fixture_path,
            QStringLiteral("--behavior-smoke"),
            smoke_name,
        });
        ok &= check(started,
            "native surface behavior smoke starts: " + std::string(smoke_case.name));
        if (!started) {
            continue;
        }

        if (!wait_for_process_exit(app, process_exited, 10000)) {
            (void)fixture.surface.terminate_process();
            pump_events(app);
            ok &= check(false,
                "native surface behavior smoke exits before timeout: " +
                    std::string(smoke_case.name));
            continue;
        }
        pump_events(app);

        for (const QString& backend_error : backend_errors) {
            std::cerr << "native surface backend error: "
                << backend_error.toLocal8Bit().constData() << '\n';
        }

        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(process_exit_code == 0,
            "native surface behavior smoke exits successfully: " +
                std::string(smoke_case.name));
        ok &= check(backend_error_count == 0,
            "native surface behavior smoke has no backend errors: " +
                std::string(smoke_case.name));
        ok &= check(snapshot != nullptr,
            "native surface behavior smoke publishes snapshot: " +
                std::string(smoke_case.name));
        if (snapshot != nullptr) {
            ok &= check_surface_snapshot_for_smoke(smoke_case.name, *snapshot);
        }
    }

    return ok;
}

}

int main(int argc, char** argv)
{
    if (argc == 2 && argument_equals(argv[1], "--surface-host")) {
        QGuiApplication app(argc, argv);
        if (!validate_behavior_smoke_contract()) {
            return 1;
        }

        return run_surface_behavior_smokes(app) ? 0 : 1;
    }

    if (argc == 3 && argument_equals(argv[1], "--native-surface-host")) {
        QGuiApplication app(argc, argv);
        if (!validate_behavior_smoke_contract()) {
            return 1;
        }

        const QString fixture_path = QString::fromLocal8Bit(argv[2]);
        return run_native_surface_behavior_smokes(app, fixture_path) ? 0 : 1;
    }

    QCoreApplication app(argc, argv);

    if (argc == 4 && argument_equals(argv[1], "--host")) {
        const QString fixture_path = QString::fromLocal8Bit(argv[2]);
        const QString example_path = QString::fromLocal8Bit(argv[3]);
        return run_host_behavior_smokes(fixture_path, example_path) ? 0 : 1;
    }

    if (argc != 2) {
        std::cerr << "usage: behavior_smoke_tests <fixture-executable>\n"
            << "       behavior_smoke_tests --host <fixture-executable> "
            << "<example-terminal-executable>\n";
        return 2;
    }

    const QString fixture_path = QString::fromLocal8Bit(argv[1]);

    bool ok = true;
    ok &= validate_behavior_smoke_contract();
    if (!ok) {
        return 1;
    }

    const std::vector<smoke_payload_t> payloads =
        fixture_behavior_outputs(fixture_path, ok);

    const QByteArray* cursor_addressing =
        payload_by_name(payloads, "cursor-addressing", ok);
    if (cursor_addressing != nullptr) {
        ok &= test_cursor_addressing_smoke(*cursor_addressing);
        ok &= test_cursor_addressing_split_smoke(*cursor_addressing);
    }

    const QByteArray* erase_line_screen =
        payload_by_name(payloads, "erase-line-screen", ok);
    if (erase_line_screen != nullptr) {
        ok &= test_erase_line_screen_smoke(*erase_line_screen);
    }

    const QByteArray* alternate_screen =
        payload_by_name(payloads, "alternate-screen-1049", ok);
    if (alternate_screen != nullptr) {
        ok &= test_alternate_screen_smoke(*alternate_screen);
    }

    const QByteArray* unicode_width = payload_by_name(payloads, "unicode-width", ok);
    if (unicode_width != nullptr) {
        ok &= test_unicode_width_smoke(*unicode_width);
        ok &= test_unicode_width_split_smoke(*unicode_width);
    }

    const QByteArray* output_burst = payload_by_name(payloads, "output-burst", ok);
    const term::terminal_canvas_fixture_behavior_smoke_case_t* output_burst_case = smoke_case_by_name(
        "output-burst",
        ok);
    if (output_burst != nullptr && output_burst_case != nullptr) {
        ok &= test_output_burst_smoke(*output_burst, *output_burst_case);
        ok &= test_output_burst_split_smoke(*output_burst, *output_burst_case);
    }

    const QByteArray* sgr_reset_interactions =
        payload_by_name(payloads, "sgr-reset-interactions", ok);
    if (sgr_reset_interactions != nullptr) {
        ok &= test_sgr_reset_interactions_smoke(*sgr_reset_interactions);
    }

    const QByteArray* decstbm_scroll_region =
        payload_by_name(payloads, "decstbm-scroll-region", ok);
    if (decstbm_scroll_region != nullptr) {
        ok &= test_decstbm_scroll_region_smoke(*decstbm_scroll_region);
    }

    ok &= test_resize_output_ordering_smoke();

    return ok ? 0 : 1;
}
