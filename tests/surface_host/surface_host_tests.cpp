#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/vnm_terminal_surface.h"
#include "helpers/test_check.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QThread>
#include <QVariant>
#include <QWheelEvent>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

bool check_bytes_equal(const QByteArray& actual, const QByteArray& expected, const char* message)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << message
            << " expected=" << expected.toHex(' ').constData()
            << " actual="   << actual.toHex(' ').constData() << '\n';
        return false;
    }

    return true;
}

bool check_int_equal(int actual, int expected, const char* message)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << message
            << " expected=" << expected
            << " actual="   << actual << '\n';
        return false;
    }

    return true;
}

bool check_write_chunks_equal(
    const std::vector<QByteArray>& writes,
    std::size_t                    first_index,
    const std::vector<QByteArray>& expected,
    const char*                    message)
{
    if (writes.size() - std::min(writes.size(), first_index) != expected.size()) {
        std::cerr << "FAIL: " << message
            << " expected chunk count=" << expected.size()
            << " actual chunk count="   << writes.size() - std::min(writes.size(), first_index)
            << '\n';
        return false;
    }

    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (writes[first_index + i] != expected[i]) {
            std::cerr << "FAIL: " << message
                << " chunk="    << i
                << " expected=" << expected[i].toHex(' ').constData()
                << " actual="   << writes[first_index + i].toHex(' ').constData()
                << '\n';
            return false;
        }
    }

    return true;
}

bool nearly_equal(qreal lhs, qreal rhs, qreal tolerance = 0.01)
{
    return std::abs(lhs - rhs) <= tolerance;
}

bool check_rect_near(
    const QRectF&  actual,
    const QRectF&  expected,
    const char*    message)
{
    if (!nearly_equal(actual.left(),   expected.left())  ||
        !nearly_equal(actual.top(),    expected.top())   ||
        !nearly_equal(actual.width(),  expected.width()) ||
        !nearly_equal(actual.height(), expected.height()))
    {
        std::cerr << "FAIL: " << message
            << " expected=(" << expected.left() << ',' << expected.top()
            << ',' << expected.width() << ',' << expected.height()
            << ") actual=(" << actual.left() << ',' << actual.top()
            << ',' << actual.width() << ',' << actual.height() << ")\n";
        return false;
    }

    return true;
}

QByteArray bytes_from_hex(const char* hex)
{
    return QByteArray::fromHex(QByteArray(hex));
}

QByteArray osc52_write_sequence(const char* target_selection, const QByteArray& payload)
{
    QByteArray bytes = QByteArrayLiteral("\x1b]52;");
    bytes += target_selection;
    bytes += ';';
    bytes += payload.toBase64();
    bytes += '\a';
    return bytes;
}

struct Clipboard_write_observation
{
    quint64    request_id = 0U;
    QString    target_selection;
    QByteArray payload;
};

class Clipboard_text_guard
{
public:
    Clipboard_text_guard()
    :
        m_clipboard(QGuiApplication::clipboard()),
        m_original_text(m_clipboard->text(QClipboard::Clipboard))
    {}

    ~Clipboard_text_guard()
    {
        m_clipboard->setText(m_original_text, QClipboard::Clipboard);
    }

private:
    QClipboard*    m_clipboard = nullptr;
    QString        m_original_text;
};

void observe_backend_errors(VNM_TerminalSurface& surface, int& error_count)
{
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::backend_error,
        &surface,
        [&error_count](VNM_TerminalSurface::Backend_error_code, const QString&) {
            ++error_count;
        });
}

void observe_backend_error_codes(
    VNM_TerminalSurface&                                   surface,
    std::vector<VNM_TerminalSurface::Backend_error_code>&  error_codes)
{
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::backend_error,
        &surface,
        [&error_codes](VNM_TerminalSurface::Backend_error_code code, const QString&) {
            error_codes.push_back(code);
        });
}

void observe_clipboard_write_requests(
    VNM_TerminalSurface&                                   surface,
    std::vector<Clipboard_write_observation>&              requests)
{
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::clipboard_write_requested,
        &surface,
        [&requests](
            quint64 request_id,
            const QString& target_selection,
            const QByteArray& payload) {
            requests.push_back({request_id, target_selection, payload});
        });
}

void pump_events(QGuiApplication& app, int rounds = 8)
{
    for (int i = 0; i < rounds; ++i) {
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
}

template <typename Predicate>
bool pump_until(QGuiApplication& app, Predicate predicate, int rounds = 20)
{
    for (int i = 0; i < rounds; ++i) {
        app.processEvents(QEventLoop::AllEvents, 50);
        if (predicate()) {
            return true;
        }
        QThread::msleep(10);
    }

    return predicate();
}

bool window_render_matches(
    QGuiApplication&               app,
    QQuickWindow&                  window,
    const std::function<bool()>&   predicate)
{
    for (int i = 0; i < 30; ++i) {
        pump_events(app, 1);
        const QImage image = window.grabWindow();
        if (!image.isNull() && predicate()) {
            return true;
        }
    }

    return false;
}

bool render_surface_sequence(
    QGuiApplication&               app,
    QQuickWindow&                  window,
    const VNM_TerminalSurface&     surface,
    std::uint64_t                  sequence)
{
    return window_render_matches(app, window, [&] {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t renderer_stats =
            term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
        return
            invalidation_stats.last_rendered_snapshot_sequence == sequence &&
            !invalidation_stats.pending_update                             &&
            renderer_stats.paint_completed                                 &&
            renderer_stats.text_content_failures == 0;
    });
}

std::uint64_t live_root_node_count(
    const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return stats.render_root_nodes_created - stats.render_root_nodes_destroyed;
}

std::uint64_t live_text_resource_count(
    const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return stats.render_text_resources_created - stats.render_text_resources_destroyed;
}

bool has_valid_lifecycle_resource_counts(
    const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return
        stats.render_root_nodes_created     >= stats.render_root_nodes_destroyed &&
        stats.render_text_resources_created >= stats.render_text_resources_destroyed;
}

bool has_live_render_tree(const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return
        has_valid_lifecycle_resource_counts(stats) &&
        live_root_node_count(stats) == 1U          &&
        live_text_resource_count(stats) > 0U;
}

bool has_no_live_render_resources(const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return
        has_valid_lifecycle_resource_counts(stats) &&
        live_root_node_count(stats) == 0U          &&
        live_text_resource_count(stats) == 0U;
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

bool snapshot_dirty_ranges_contain_row(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row)
{
    for (const term::Terminal_render_dirty_row_range& range : snapshot.dirty_row_ranges) {
        if (row >= range.first_row && row < range.first_row + range.row_count) {
            return true;
        }
    }

    return false;
}

bool snapshot_has_selection_span(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    first_column,
    int                                    column_count)
{
    for (const term::Terminal_render_selection_span& span : snapshot.selection_spans) {
        if (span.row          == row          &&
            span.first_column == first_column &&
            span.column_count == column_count)
        {
            return true;
        }
    }

    return false;
}

QByteArray numbered_scroll_lines(int count)
{
    QByteArray bytes;
    for (int i = 0; i < count; ++i) {
        bytes += QStringLiteral("scroll-line-%1\r\n")
            .arg(i, 3, 10, QLatin1Char('0'))
            .toUtf8();
    }
    return bytes;
}

struct Scripted_backend_lifecycle_state
{
    std::atomic<int> destructed_count{0};
    std::atomic<int> terminate_count{0};
    std::atomic<int> worker_callback_attempts{0};
    std::atomic<int> worker_callback_completions{0};
};

class Scripted_backend final : public term::Terminal_backend
{
public:
    ~Scripted_backend() override
    {
        join_worker();
        if (lifecycle_state != nullptr) {
            ++lifecycle_state->destructed_count;
        }
    }

    term::Terminal_backend_result start(
        const term::Terminal_launch_config&    config,
        term::Terminal_backend_callbacks       callbacks) override
    {
        if (start_attempt_observer != nullptr) {
            ++(*start_attempt_observer);
        }

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
        start_configs.push_back(config);
        if (start_config_observer != nullptr) {
            start_config_observer->push_back(config);
        }
        running = true;
        if (on_start != nullptr) {
            on_start();
        }
        for (const QByteArray& output : outputs_during_start) {
            m_callbacks.output_received(output);
        }
        return term::backend_accept();
    }

    term::Terminal_backend_result write(QByteArray bytes) override
    {
        writes.push_back(std::move(bytes));
        if (reject_writes) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("scripted write rejection"));
        }
        return term::backend_accept();
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request request) override
    {
        resize_requests.push_back(request);
        if (!term::is_valid_grid_size(request.grid_size)) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::RESIZE_FAILED,
                    QStringLiteral("scripted resize requires a positive grid"));
        }

        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool paused) override
    {
        output_pause_requests.push_back(paused);
        output_paused = paused;
        return term::backend_accept();
    }

    term::Terminal_backend_result interrupt() override
    {
        if (!running) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::INTERRUPT_FAILED,
                    QStringLiteral("scripted interrupt without process"));
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
                    QStringLiteral("scripted terminate without process"));
        }

        if (lifecycle_state != nullptr) {
            ++lifecycle_state->terminate_count;
        }
        running = false;
        m_callbacks.process_exited({term::Terminal_exit_reason::TERMINATED, 0});
        return term::backend_accept();
    }

    void emit_output(QByteArray bytes)
    {
        m_callbacks.output_received(std::move(bytes));
    }

    void emit_exit(term::Terminal_backend_exit exit)
    {
        running = false;
        m_callbacks.process_exited(exit);
    }

    void emit_output_from_worker(QByteArray bytes)
    {
        join_worker();

        term::Terminal_backend_callbacks callbacks = m_callbacks;
        std::shared_ptr<Scripted_backend_lifecycle_state> state = lifecycle_state;
        worker = std::thread([callbacks, state, bytes = std::move(bytes)]() mutable {
            if (state != nullptr) {
                ++state->worker_callback_attempts;
            }
            callbacks.output_received(std::move(bytes));
            if (state != nullptr) {
                ++state->worker_callback_completions;
            }
        });
    }

    void join_worker()
    {
        if (worker.joinable()) {
            worker.join();
        }
    }

    bool                       running                = false;
    bool                       reject_writes          = false;
    bool                       output_paused          = false;
    std::vector<QByteArray>    outputs_during_start;
    std::vector<term::Terminal_launch_config>
                               start_configs;
    std::vector<term::Terminal_backend_resize_request>
                               resize_requests;
    std::vector<QByteArray>    writes;
    std::vector<bool>          output_pause_requests;
    std::vector<term::Terminal_launch_config>*
                               start_config_observer  = nullptr;
    std::function<void()>      on_start;
    std::shared_ptr<Scripted_backend_lifecycle_state>
                               lifecycle_state;
    int*                       start_attempt_observer = nullptr;
    std::thread                worker;

private:
    term::Terminal_backend_callbacks m_callbacks;
};

struct Surface_fixture
{
    QQuickWindow           window;
    VNM_TerminalSurface    surface;

    Surface_fixture()
    {
        window.resize(640, 320);
        surface.setParentItem(window.contentItem());
        surface.setSize(QSizeF(520.0, 240.0));
        surface.set_font_family(QStringLiteral("monospace"));
        surface.set_font_size(12.0);
        window.show();
    }
};

Scripted_backend* start_surface_with_backend(
    VNM_TerminalSurface&               surface,
    std::unique_ptr<Scripted_backend>  backend,
    QStringList                        argv,
    bool*                              out_started)
{
    Scripted_backend* backend_ptr = backend.get();
    *out_started = term::VNM_TerminalSurface_render_bridge::start_process_with_backend(
        surface,
        std::move(backend),
        std::move(argv));
    return backend_ptr;
}

bool send_key(
    VNM_TerminalSurface&   surface,
    int                    key,
    Qt::KeyboardModifiers  modifiers,
    const QString&         text,
    const char*            message)
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted(), message);
}

bool send_window_key_and_expect_write(
    VNM_TerminalSurface&   surface,
    QQuickWindow&          window,
    Scripted_backend&      backend,
    int                    key,
    Qt::KeyboardModifiers  modifiers,
    const QString&         text,
    const QByteArray&      expected,
    const char*            message)
{
    surface.forceActiveFocus();
    const std::size_t write_count = backend.writes.size();
    QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
    QCoreApplication::sendEvent(&window, &event);
    bool ok  = check(event.isAccepted(), message);
    ok      &= check(backend.writes.size() == write_count + 1U, message);
    if (backend.writes.size() > write_count) {
        ok &= check_bytes_equal(backend.writes.back(), expected, message);
    }
    return ok;
}

bool send_key_and_expect_write(
    VNM_TerminalSurface&   surface,
    Scripted_backend&      backend,
    int                    key,
    Qt::KeyboardModifiers  modifiers,
    const QString&         text,
    const QByteArray&      expected,
    const char*            message)
{
    const std::size_t write_count = backend.writes.size();
    bool ok = send_key(surface, key, modifiers, text, message);
    ok &= check(backend.writes.size() == write_count + 1U, message);
    if (backend.writes.size() > write_count) {
        ok &= check_bytes_equal(backend.writes.back(), expected, message);
    }
    return ok;
}

bool send_ime_event(
    VNM_TerminalSurface&   surface,
    const QString&         commit_text,
    const QString&         preedit_text,
    int                    cursor_position,
    int                    replacement_start,
    int                    replacement_length,
    const char*            message)
{
    QList<QInputMethodEvent::Attribute> attributes;
    if (!preedit_text.isEmpty()) {
        attributes.push_back(QInputMethodEvent::Attribute(
            QInputMethodEvent::Cursor,
            cursor_position,
            1,
            QVariant()));
    }

    QInputMethodEvent event(preedit_text, attributes);
    if (!commit_text.isEmpty()) {
        event.setCommitString(commit_text, replacement_start, replacement_length);
    }
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted(), message);
}

bool send_ime_preedit(
    VNM_TerminalSurface&   surface,
    const QString&         text,
    int                    cursor_position,
    const char*            message)
{
    return send_ime_event(surface, {}, text, cursor_position, 0, 0, message);
}

bool send_ime_commit(
    VNM_TerminalSurface&   surface,
    const QString&         text,
    const char*            message)
{
    QInputMethodEvent event;
    event.setCommitString(text);
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted(), message);
}

bool send_empty_ime_commit(VNM_TerminalSurface& surface, const char* message)
{
    QInputMethodEvent event;
    event.setCommitString(QString());
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted(), message);
}

bool send_empty_ime_event(VNM_TerminalSurface& surface, const char* message)
{
    QInputMethodEvent event;
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted(), message);
}

bool send_wheel_event(
    VNM_TerminalSurface&   surface,
    Qt::KeyboardModifiers  modifiers,
    QPointF                position,
    int                    pixel_delta_y,
    int                    angle_delta_y,
    bool                   expected_accepted,
    const char*            message)
{
    QWheelEvent event(
        position,
        position,
        QPoint(0, pixel_delta_y),
        QPoint(0, angle_delta_y),
        Qt::NoButton,
        modifiers,
        Qt::NoScrollPhase,
        false);
    event.ignore();
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted() == expected_accepted, message);
}

bool send_wheel_event(
    VNM_TerminalSurface&   surface,
    Qt::KeyboardModifiers  modifiers,
    int                    pixel_delta_y,
    int                    angle_delta_y,
    bool                   expected_accepted,
    const char*            message)
{
    return
        send_wheel_event(
            surface,
            modifiers,
            QPointF(8.0, 8.0),
            pixel_delta_y,
            angle_delta_y,
            expected_accepted,
            message);
}

bool send_wheel_event(
    VNM_TerminalSurface&   surface,
    Qt::KeyboardModifiers  modifiers,
    int                    angle_delta_y,
    bool                   expected_accepted,
    const char*            message)
{
    return send_wheel_event(
        surface,
        modifiers,
        0,
        angle_delta_y,
        expected_accepted,
        message);
}

bool send_mouse_event(
    VNM_TerminalSurface&   surface,
    QEvent::Type           type,
    QPointF                position,
    Qt::MouseButton        button,
    Qt::MouseButtons       buttons,
    Qt::KeyboardModifiers  modifiers,
    bool                   expected_accepted,
    const char*            message)
{
    QMouseEvent event(type, position, position, position, button, buttons, modifiers);
    event.ignore();
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted() == expected_accepted, message);
}

bool send_hover_move(
    VNM_TerminalSurface&   surface,
    QPointF                position,
    Qt::KeyboardModifiers  modifiers,
    bool                   expected_accepted,
    const char*            message)
{
    QHoverEvent event(QEvent::HoverMove, position, position, modifiers);
    event.ignore();
    QCoreApplication::sendEvent(&surface, &event);
    return check(event.isAccepted() == expected_accepted, message);
}

QByteArray joined_writes_since(
    const std::vector<QByteArray>& writes,
    std::size_t                    first_index)
{
    QByteArray joined;
    for (std::size_t i = first_index; i < writes.size(); ++i) {
        joined.append(writes[i]);
    }
    return joined;
}

QByteArray framed_paste(QByteArray body)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[200~");
    bytes += body;
    bytes += QByteArrayLiteral("\x1b[201~");
    return bytes;
}

term::terminal_cell_metrics_t current_cell_metrics(const VNM_TerminalSurface& surface)
{
    const QFont font = term::vnm_terminal_font(surface.font_family(), surface.font_size());
    const QFontMetricsF metrics(font);
    return {
        metrics.horizontalAdvance(QLatin1Char('M')),
        metrics.lineSpacing(),
        metrics.ascent(),
        metrics.descent(),
    };
}

QPointF point_in_grid_cell(
    const VNM_TerminalSurface& surface,
    int                        row,
    int                        column)
{
    const term::terminal_cell_metrics_t metrics = current_cell_metrics(surface);
    return {
        (static_cast<qreal>(column) + 0.5) * metrics.width,
        (static_cast<qreal>(row) + 0.5) * metrics.height,
    };
}

QByteArray sgr_mouse_report(int code, int row, int column, char final_byte)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[<");
    bytes += QByteArray::number(code);
    bytes += ';';
    bytes += QByteArray::number(column + 1);
    bytes += ';';
    bytes += QByteArray::number(row + 1);
    bytes += final_byte;
    return bytes;
}

bool test_start_maps_output_to_snapshot(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int started_count  = 0;
    int activity_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_started,
        &fixture.surface,
        [&started_count] {
            ++started_count;
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::output_activity,
        &fixture.surface,
        [&activity_count] {
            ++activity_count;
        });

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("hello from start")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);

    ok &= check(started, "surface scripted start succeeds");
    ok &= check(backend_ptr->start_configs.size() == 1U,
        "surface forwards one launch config to backend");
    ok &= check(started_count == 1, "surface emits process_started");
    ok &= check(activity_count == 1, "surface emits output_activity for start output");
    ok &= check(fixture.surface.process_state() == VNM_TerminalSurface::Process_state::RUNNING,
        "surface process state becomes running");
    ok &= check(fixture.surface.backend_ready(), "surface backend ready becomes true");
    ok &= check(snapshot != nullptr && snapshot_contains_text(*snapshot, QStringLiteral("hello")),
        "surface publishes session render snapshot for start output");

    return ok;
}

bool test_surface_session_snapshot_burst_coalesces_to_latest_render(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("sync-burst-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface sync burst starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("sync-burst-baseline")),
        "surface sync burst publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(render_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "surface sync burst renders baseline before rapid updates");
    const term::Terminal_surface_render_invalidation_stats_t baseline_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);

    const std::vector<QByteArray> burst_outputs = {
        QByteArrayLiteral("\r\nsync-burst-row-0"),
        QByteArrayLiteral("\r\nsync-burst-row-1"),
        QByteArrayLiteral("\r\nsync-burst-row-2"),
        QByteArrayLiteral("\r\nsync-burst-row-3"),
        QByteArrayLiteral("\r\nsync-burst-row-4"),
    };

    std::uint64_t previous_snapshot_sequence = baseline_snapshot->metadata.sequence;
    std::uint64_t latest_snapshot_sequence   = previous_snapshot_sequence;
    for (std::size_t i = 0; i < burst_outputs.size(); ++i) {
        backend_ptr->emit_output(burst_outputs[i]);
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);

        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            snapshot->metadata.sequence > previous_snapshot_sequence,
            "surface sync burst advances snapshot sequence for each drained output");
        ok &= check(snapshot != nullptr &&
            snapshot_contains_text(
                *snapshot,
                QStringLiteral("sync-burst-row-%1").arg(static_cast<int>(i))),
            "surface sync burst latest session snapshot contains drained output");
        if (snapshot != nullptr) {
            previous_snapshot_sequence = snapshot->metadata.sequence;
            latest_snapshot_sequence   = snapshot->metadata.sequence;
        }

        const term::Terminal_surface_render_invalidation_stats_t stats_after_drain =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
        ok &= check(stats_after_drain.pending_update,
            "surface sync burst leaves render update pending while paint is delayed");

        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        const term::Terminal_surface_render_invalidation_stats_t stats_after_noop_drain =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
        ok &= check(stats_after_noop_drain.update_requests ==
            stats_after_drain.update_requests &&
            stats_after_noop_drain.scheduled_updates ==
                stats_after_drain.scheduled_updates &&
            stats_after_noop_drain.coalesced_requests ==
                stats_after_drain.coalesced_requests,
            "surface sync burst generation gate avoids duplicate render requests");
    }

    const term::Terminal_surface_render_invalidation_stats_t burst_pending_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(burst_pending_stats.update_requests ==
        baseline_stats.update_requests + burst_outputs.size(),
        "surface sync burst records one render request per fresh snapshot");
    ok &= check(burst_pending_stats.scheduled_updates ==
        baseline_stats.scheduled_updates + 1U,
        "surface sync burst schedules one render update for rapid session snapshots");
    ok &= check(burst_pending_stats.coalesced_requests ==
        baseline_stats.coalesced_requests + burst_outputs.size() - 1U,
        "surface sync burst coalesces pending session snapshot renders");
    ok &= check(burst_pending_stats.pending_update,
        "surface sync burst has one pending render before the delayed paint");

    ok &= check(render_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        latest_snapshot_sequence),
        "surface sync burst renders the newest coalesced session snapshot");
    const term::Terminal_surface_render_invalidation_stats_t rendered_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(rendered_stats.last_rendered_snapshot_sequence == latest_snapshot_sequence &&
        !rendered_stats.pending_update,
        "surface sync burst reports the latest rendered snapshot sequence");

    return ok;
}

bool test_surface_session_single_drain_coalesces_dirty_rows(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("single-drain-baseline")};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface single-drain dirty coalescing starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> baseline_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(baseline_snapshot != nullptr &&
        snapshot_contains_text(*baseline_snapshot, QStringLiteral("single-drain-baseline")),
        "surface single-drain dirty coalescing publishes a baseline snapshot");
    if (baseline_snapshot == nullptr) {
        return ok;
    }

    ok &= check(render_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        baseline_snapshot->metadata.sequence),
        "surface single-drain dirty coalescing renders baseline");
    const term::Terminal_surface_render_invalidation_stats_t baseline_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[1;1Hsession-row-0"));
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[2;1Hsession-row-1"));
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[3;1Hsession-row-2"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(snapshot != nullptr &&
        snapshot_contains_text(*snapshot, QStringLiteral("session-row-0")) &&
        snapshot_contains_text(*snapshot, QStringLiteral("session-row-1")) &&
        snapshot_contains_text(*snapshot, QStringLiteral("session-row-2")),
        "surface single-drain dirty coalescing latest snapshot contains every row");
    if (snapshot == nullptr) {
        return ok;
    }

    ok &= check(snapshot_dirty_ranges_contain_row(*snapshot, 0) &&
        snapshot_dirty_ranges_contain_row(*snapshot, 1) &&
        snapshot_dirty_ranges_contain_row(*snapshot, 2),
        "surface single-drain dirty coalescing carries skipped dirty rows");

    const term::Terminal_surface_render_invalidation_stats_t pending_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(pending_stats.update_requests == baseline_stats.update_requests + 1U,
        "surface single-drain dirty coalescing schedules one surface snapshot");

    ok &= check(render_surface_sequence(
        app,
        fixture.window,
        fixture.surface,
        snapshot->metadata.sequence),
        "surface single-drain dirty coalescing renders latest snapshot");
    const term::terminal_renderer_stats_t render_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        fixture.surface);
    ok &= check(render_stats.text_content_rebuilds >= 3,
        "surface single-drain dirty coalescing rebuilds all skipped changed rows");

    return ok;
}

bool test_osc52_clipboard_write_signal_and_deny(QGuiApplication& app)
{
    bool ok = true;
    Clipboard_text_guard clipboard_guard;
    QGuiApplication::clipboard()->setText(QStringLiteral("deny-sentinel"), QClipboard::Clipboard);

    Surface_fixture fixture;
    pump_events(app);

    std::vector<Clipboard_write_observation> requests;
    std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
    observe_clipboard_write_requests(fixture.surface, requests);
    observe_backend_error_codes(fixture.surface, error_codes);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "OSC 52 deny surface starts");

    const QByteArray payload = QByteArrayLiteral("surface deny payload");
    backend_ptr->emit_output(osc52_write_sequence("c", payload));
    pump_events(app);

    ok &= check(requests.size() == 1U, "OSC 52 write emits one surface clipboard signal");
    if (!requests.empty()) {
        ok &= check(requests[0].request_id == 1U, "OSC 52 surface signal carries request id");
        ok &= check(requests[0].target_selection == QStringLiteral("c"),
            "OSC 52 surface signal carries target selection");
        ok &= check_bytes_equal(
            requests[0].payload,
            payload,
            "OSC 52 surface signal carries decoded payload");
        ok &= check(fixture.surface.respond_clipboard_write(
            requests[0].request_id,
            VNM_TerminalSurface::Clipboard_response_decision::DENY),
            "OSC 52 deny response consumes pending request");
    }

    ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
        QStringLiteral("deny-sentinel"),
        "OSC 52 deny response leaves clipboard unchanged");
    ok &= check(error_codes.empty(), "OSC 52 deny response emits no backend error");

    return ok;
}

bool test_osc52_clipboard_wrong_duplicate_and_replacement(QGuiApplication& app)
{
    bool ok = true;
    Clipboard_text_guard clipboard_guard;
    QGuiApplication::clipboard()->setText(QStringLiteral("replacement-sentinel"),
        QClipboard::Clipboard);

    Surface_fixture fixture;
    pump_events(app);

    std::vector<Clipboard_write_observation> requests;
    std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
    observe_clipboard_write_requests(fixture.surface, requests);
    observe_backend_error_codes(fixture.surface, error_codes);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "OSC 52 response policy surface starts");

    backend_ptr->emit_output(
        osc52_write_sequence("c", QByteArrayLiteral("first response payload")));
    pump_events(app);

    ok &= check(requests.size() == 1U, "OSC 52 first request reaches surface");
    if (!requests.empty()) {
        const quint64 first_request_id = requests[0].request_id;
        ok &= check(!fixture.surface.respond_clipboard_write(
            first_request_id + 100U,
            VNM_TerminalSurface::Clipboard_response_decision::DENY),
            "OSC 52 wrong response id fails");
        ok &= check(fixture.surface.respond_clipboard_write(
            first_request_id,
            VNM_TerminalSurface::Clipboard_response_decision::DENY),
            "OSC 52 correct response after wrong id still succeeds");
        ok &= check(!fixture.surface.respond_clipboard_write(
            first_request_id,
            VNM_TerminalSurface::Clipboard_response_decision::DENY),
            "OSC 52 duplicate response fails");
    }

    backend_ptr->emit_output(
        osc52_write_sequence("c", QByteArrayLiteral("late response payload")));
    backend_ptr->emit_output(
        osc52_write_sequence("p", QByteArrayLiteral("replacement payload")));
    pump_events(app);

    ok &= check(requests.size() == 3U, "OSC 52 replacement requests reach surface");
    if (requests.size() >= 3U) {
        ok &= check(requests[1].target_selection == QStringLiteral("c"),
            "OSC 52 late request carries original target");
        ok &= check_bytes_equal(
            requests[1].payload,
            QByteArrayLiteral("late response payload"),
            "OSC 52 late request carries original payload");
        ok &= check(requests[2].target_selection == QStringLiteral("p"),
            "OSC 52 replacement request carries replacement target");
        ok &= check_bytes_equal(
            requests[2].payload,
            QByteArrayLiteral("replacement payload"),
            "OSC 52 replacement request carries replacement payload");
        ok &= check(!fixture.surface.respond_clipboard_write(
            requests[1].request_id,
            VNM_TerminalSurface::Clipboard_response_decision::ALLOW),
            "OSC 52 replaced older response is late");
        ok &= check(fixture.surface.respond_clipboard_write(
            requests[2].request_id,
            VNM_TerminalSurface::Clipboard_response_decision::DENY),
            "OSC 52 replacement request remains pending after late response");
    }

    ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
        QStringLiteral("replacement-sentinel"),
        "OSC 52 wrong duplicate and late responses leave clipboard unchanged");
    ok &= check(error_codes.size() == 3U,
        "OSC 52 wrong duplicate and late responses emit backend errors");
    for (VNM_TerminalSurface::Backend_error_code code : error_codes) {
        ok &= check(code == VNM_TerminalSurface::Backend_error_code::CALLBACK_MISSING,
            "OSC 52 failed response emits CALLBACK_MISSING");
    }

    return ok;
}

bool test_osc52_clipboard_late_exit_restart_and_targets(QGuiApplication& app)
{
    bool ok = true;
    Clipboard_text_guard clipboard_guard;
    QGuiApplication::clipboard()->setText(QStringLiteral("lifecycle-sentinel"),
        QClipboard::Clipboard);

    Surface_fixture fixture;
    pump_events(app);

    std::vector<Clipboard_write_observation> requests;
    std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
    observe_clipboard_write_requests(fixture.surface, requests);
    observe_backend_error_codes(fixture.surface, error_codes);

    auto first_backend = std::make_unique<Scripted_backend>();
    bool first_started = false;
    Scripted_backend* first_backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(first_backend),
        { QStringLiteral("scripted-terminal") },
        &first_started);
    ok &= check(first_started, "OSC 52 lifecycle first surface starts");

    first_backend_ptr->emit_output(
        osc52_write_sequence("c", QByteArrayLiteral("exit-stale payload")));
    pump_events(app);
    ok &= check(requests.size() == 1U, "OSC 52 lifecycle request reaches surface");
    const quint64 stale_request_id = requests.empty() ? 0U : requests[0].request_id;

    first_backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
    pump_events(app);
    ok &= check(!fixture.surface.respond_clipboard_write(
        stale_request_id,
        VNM_TerminalSurface::Clipboard_response_decision::ALLOW),
        "OSC 52 response after process exit is late");

    auto second_backend = std::make_unique<Scripted_backend>();
    bool second_started = false;
    Scripted_backend* second_backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(second_backend),
        { QStringLiteral("scripted-terminal") },
        &second_started);
    ok &= check(second_started, "OSC 52 lifecycle second surface starts");

    second_backend_ptr->emit_output(
        osc52_write_sequence("p", QByteArrayLiteral("selection-target payload")));
    pump_events(app);
    ok &= check(requests.size() == 2U, "OSC 52 second lifecycle request reaches surface");
    if (requests.size() >= 2U) {
        ok &= check(requests[1].request_id != stale_request_id,
            "OSC 52 surface request ids are not reused across restart");
        ok &= check(requests[1].target_selection == QStringLiteral("p"),
            "OSC 52 non-clipboard target is reported to host");
        ok &= check_bytes_equal(
            requests[1].payload,
            QByteArrayLiteral("selection-target payload"),
            "OSC 52 non-clipboard payload is reported to host");
        ok &= check(!fixture.surface.respond_clipboard_write(
            stale_request_id,
            VNM_TerminalSurface::Clipboard_response_decision::ALLOW),
            "OSC 52 stale old id cannot approve a new request");
        ok &= check(!fixture.surface.respond_clipboard_write(
            requests[1].request_id,
            VNM_TerminalSurface::Clipboard_response_decision::ALLOW),
            "OSC 52 unsupported target cannot be allowed as clipboard");
    }

    ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
        QStringLiteral("lifecycle-sentinel"),
        "OSC 52 late and unsupported responses leave clipboard unchanged");
    ok &= check(error_codes.size() == 3U,
        "OSC 52 lifecycle failures emit backend errors");
    for (VNM_TerminalSurface::Backend_error_code code : error_codes) {
        ok &= check(code == VNM_TerminalSurface::Backend_error_code::CALLBACK_MISSING,
            "OSC 52 lifecycle failure emits CALLBACK_MISSING");
    }

    return ok;
}

bool test_osc52_clipboard_allow_writes_clipboard(QGuiApplication& app)
{
    bool ok = true;
    Clipboard_text_guard clipboard_guard;
    QGuiApplication::clipboard()->setText(QStringLiteral("allow-sentinel"), QClipboard::Clipboard);

    Surface_fixture fixture;
    pump_events(app);

    std::vector<Clipboard_write_observation> requests;
    std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
    observe_clipboard_write_requests(fixture.surface, requests);
    observe_backend_error_codes(fixture.surface, error_codes);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "OSC 52 allow surface starts");

    const QByteArray payload = QByteArrayLiteral("allowed clipboard text");
    backend_ptr->emit_output(osc52_write_sequence("c", payload));
    pump_events(app);

    ok &= check(requests.size() == 1U, "OSC 52 allow request reaches surface");
    if (!requests.empty()) {
        ok &= check(fixture.surface.respond_clipboard_write(
            requests[0].request_id,
            VNM_TerminalSurface::Clipboard_response_decision::ALLOW),
            "OSC 52 allow response consumes pending request");
    }

    ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
        QString::fromUtf8(payload),
        "OSC 52 allow response writes decoded payload to clipboard");
    ok &= check(!requests.empty() &&
        !fixture.surface.respond_clipboard_write(
            requests[0].request_id,
            VNM_TerminalSurface::Clipboard_response_decision::ALLOW),
        "OSC 52 duplicate allow response fails");
    ok &= check(error_codes.size() == 1U &&
        error_codes[0] == VNM_TerminalSurface::Backend_error_code::CALLBACK_MISSING,
        "OSC 52 duplicate allow response emits CALLBACK_MISSING");

    return ok;
}

bool test_keyboard_printable_controls_and_prompt_path(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "keyboard printable surface starts");

    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_L, Qt::NoModifier,
        QString::fromUtf8("\xce\xbb"), bytes_from_hex("cebb"),
        "printable UTF-8 writes text bytes");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Return, Qt::NoModifier,
        QStringLiteral("\r"), bytes_from_hex("0d"),
        "Return writes carriage return");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Tab, Qt::NoModifier,
        QStringLiteral("\t"), bytes_from_hex("09"),
        "Tab writes horizontal tab");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Backspace, Qt::NoModifier,
        {}, bytes_from_hex("7f"),
        "Backspace writes DEL");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Escape, Qt::NoModifier,
        {}, bytes_from_hex("1b"),
        "Escape writes ESC");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_A, Qt::ControlModifier,
        {}, bytes_from_hex("01"),
        "Ctrl+A writes C0 SOH");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_C, Qt::ControlModifier,
        {}, bytes_from_hex("03"),
        "Ctrl+C without selection writes C0 ETX");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_K, Qt::ControlModifier,
        {}, bytes_from_hex("0b"),
        "Ctrl+K writes C0 VT");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_V, Qt::ControlModifier,
        {}, bytes_from_hex("16"),
        "Ctrl+V writes C0 SYN through the reusable surface");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Z, Qt::ControlModifier,
        {}, bytes_from_hex("1a"),
        "Ctrl+Z writes C0 SUB");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_BracketLeft, Qt::ControlModifier,
        {}, bytes_from_hex("1b"),
        "Ctrl+[ writes ESC");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Backslash, Qt::ControlModifier,
        {}, bytes_from_hex("1c"),
        "Ctrl+Backslash writes FS");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_BracketRight, Qt::ControlModifier,
        {}, bytes_from_hex("1d"),
        "Ctrl+] writes GS");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_AsciiCircum, Qt::ControlModifier,
        {}, bytes_from_hex("1e"),
        "Ctrl+^ writes RS");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Underscore, Qt::ControlModifier,
        {}, bytes_from_hex("1f"),
        "Ctrl+_ writes US");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Space, Qt::ControlModifier,
        {}, bytes_from_hex("00"),
        "Ctrl+Space writes NUL");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_X, Qt::AltModifier,
        QStringLiteral("x"), bytes_from_hex("1b78"),
        "Alt+x prefixes printable text with ESC");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_C,
        Qt::ControlModifier | Qt::AltModifier,
        {}, bytes_from_hex("1b03"),
        "Ctrl+Alt+C prefixes control byte with ESC");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_At,
        Qt::ControlModifier | Qt::AltModifier,
        QStringLiteral("@"), bytes_from_hex("40"),
        "Ctrl+Alt printable text preserves layout bytes without ESC prefix");

    const std::size_t ignored_write_count = backend_ptr->writes.size();
    QKeyEvent ignored_event(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, {});
    QCoreApplication::sendEvent(&fixture.surface, &ignored_event);
    ok &= check(!ignored_event.isAccepted(), "unencoded key event is ignored");
    ok &= check(backend_ptr->writes.size() == ignored_write_count,
        "unencoded key event does not write backend bytes");

    const std::size_t prompt_write_index = backend_ptr->writes.size();
    ok &= send_key(fixture.surface, Qt::Key_Left, Qt::NoModifier, {},
        "prompt path Left is accepted");
    ok &= send_key(fixture.surface, Qt::Key_Backspace, Qt::NoModifier, {},
        "prompt path Backspace is accepted");
    for (const QChar ch : QStringLiteral("term")) {
        ok &= send_key(
            fixture.surface,
            ch.toUpper().unicode(),
            Qt::NoModifier,
            QString(ch),
            "prompt path printable key is accepted");
    }
    ok &= send_key(fixture.surface, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"),
        "prompt path Return is accepted");
    ok &= check_bytes_equal(
        joined_writes_since(backend_ptr->writes, prompt_write_index),
        bytes_from_hex("1b5b447f7465726d0d"),
        "prompt editing path writes exact byte stream");
    ok &= check(backend_error_count == 0,
        "keyboard printable success path emits no backend_error");

    return ok;
}

bool test_copy_shortcut_policy(QGuiApplication& app)
{
    bool ok = true;
    Clipboard_text_guard clipboard_guard;

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "copy shortcut surface starts");

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "copy shortcut selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "copy shortcut selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "copy shortcut selection release is accepted");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "copy shortcut fixture has selected text");

        QGuiApplication::clipboard()->setText(QStringLiteral("copy-sentinel"),
            QClipboard::Clipboard);
        const std::size_t copy_write_count = backend_ptr->writes.size();
        ok &= send_key(
            fixture.surface,
            Qt::Key_C,
            Qt::ControlModifier,
            {},
            "Ctrl+C with selection is accepted by copy policy");
        ok &= check(backend_ptr->writes.size() == copy_write_count,
            "Ctrl+C with selection writes no ETX under copy policy");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("lpha"),
            "Ctrl+C with selection copies selected text to clipboard");

        fixture.surface.clear_selection();
        QGuiApplication::clipboard()->setText(QStringLiteral("copy-fallback"),
            QClipboard::Clipboard);
        ok &= send_key_and_expect_write(
            fixture.surface, *backend_ptr, Qt::Key_C, Qt::ControlModifier,
            {}, bytes_from_hex("03"),
            "Ctrl+C without selection falls through to terminal input");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("copy-fallback"),
            "Ctrl+C fallback leaves clipboard unchanged");

        const int last_column = std::max(0, fixture.surface.columns() - 1);
        ok &= check(last_column > 5,
            "copy shortcut empty-selection fixture has enough columns");
        const QPointF empty_start = point_in_grid_cell(fixture.surface, 0, 5);
        const QPointF empty_end   = point_in_grid_cell(fixture.surface, 0, last_column);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            empty_start,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "copy shortcut empty selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            empty_end,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "copy shortcut empty selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            empty_end,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "copy shortcut empty selection release is accepted");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE,
            "copy shortcut empty selection remains active");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "copy shortcut empty selection has empty selected text");

        QGuiApplication::clipboard()->setText(QStringLiteral("empty-selection-sentinel"),
            QClipboard::Clipboard);
        const std::size_t empty_selection_write_count = backend_ptr->writes.size();
        ok &= send_key(
            fixture.surface,
            Qt::Key_C,
            Qt::ControlModifier,
            {},
            "Ctrl+C with empty active selection is accepted by copy policy");
        ok &= check(backend_ptr->writes.size() == empty_selection_write_count,
            "Ctrl+C with empty active selection writes no ETX");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard).isEmpty(),
            "Ctrl+C with empty active selection copies empty text");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "visible-selection copy shortcut surface starts");

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "visible-selection copy shortcut press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "visible-selection copy shortcut drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "visible-selection copy shortcut release is accepted");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "visible-selection copy shortcut fixture has selected text");

        backend_ptr->emit_output(numbered_scroll_lines(80));
        QGuiApplication::clipboard()->setText(QStringLiteral("visible-selection-sentinel"),
            QClipboard::Clipboard);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_key(
            fixture.surface,
            Qt::Key_C,
            Qt::ControlModifier,
            {},
            "Ctrl+C copies the visible selection before draining queued output");
        ok &= check(backend_ptr->writes.size() == write_count,
            "visible-selection Ctrl+C writes no backend bytes");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("lpha"),
            "visible-selection Ctrl+C copies the selection visible at key time");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "scrollback-spanning copy shortcut surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const int line_last_column = QStringLiteral("scroll-line-000").size() - 1;
        const bool usable_tail_snapshot =
            tail_snapshot                           != nullptr          &&
            tail_snapshot->grid_size.rows           >  5                &&
            tail_snapshot->grid_size.columns        >  line_last_column &&
            tail_snapshot->viewport.scrollback_rows >  0;
        ok &= check(usable_tail_snapshot,
            "scrollback-spanning copy shortcut fixture has usable scrollback");

        if (usable_tail_snapshot) {
            const int     anchor_row  = tail_snapshot->grid_size.rows - 2;
            const QString anchor_text = snapshot_row_text(*tail_snapshot, anchor_row);
            ok &= check(!anchor_text.isEmpty(),
                "scrollback-spanning copy shortcut anchor row has text");

            const QPointF anchor_point =
                point_in_grid_cell(fixture.surface, anchor_row, line_last_column);
            const QPointF top_point = point_in_grid_cell(fixture.surface, 0, 0);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                anchor_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "scrollback-spanning copy shortcut press is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                top_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "scrollback-spanning copy shortcut initial drag is accepted");
            ok &= send_wheel_event(
                fixture.surface,
                Qt::NoModifier,
                120,
                true,
                "scrollback-spanning copy shortcut drag scrolls upward");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                top_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "scrollback-spanning copy shortcut scrolled drag is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonRelease,
                top_point,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                true,
                "scrollback-spanning copy shortcut release is accepted");

            const QString expected_text = fixture.surface.selected_text();
            const std::shared_ptr<const term::Terminal_render_snapshot> selection_snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            ok &= check(!expected_text.isEmpty() && expected_text.contains(anchor_text),
                "scrollback-spanning copy shortcut selected_text includes offscreen text");
            ok &= check(selection_snapshot != nullptr &&
                !selection_snapshot->selection_spans.empty(),
                "scrollback-spanning copy shortcut publishes visible selection spans");
            ok &= check(selection_snapshot != nullptr &&
                !snapshot_contains_text(*selection_snapshot, anchor_text),
                "scrollback-spanning copy shortcut anchor text is outside the visible snapshot");

            QGuiApplication::clipboard()->setText(
                QStringLiteral("scrollback-spanning-sentinel"),
                QClipboard::Clipboard);
            const std::size_t write_count = backend_ptr->writes.size();
            ok &= send_key(
                fixture.surface,
                Qt::Key_C,
                Qt::ControlModifier,
                {},
                "Ctrl+C copies a scrollback-spanning selection");
            ok &= check(backend_ptr->writes.size() == write_count,
                "scrollback-spanning Ctrl+C writes no backend bytes");
            ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
                expected_text,
                "Ctrl+C copies the complete logical selection across offscreen rows");
        }
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "terminal-input copy policy surface starts");

        fixture.surface.set_copy_shortcut_policy(
            VNM_TerminalSurface::Copy_shortcut_policy::TERMINAL_INPUT);
        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "terminal-input policy selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "terminal-input policy selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "terminal-input policy selection release is accepted");

        QGuiApplication::clipboard()->setText(QStringLiteral("terminal-input-sentinel"),
            QClipboard::Clipboard);
        ok &= send_key_and_expect_write(
            fixture.surface, *backend_ptr, Qt::Key_C, Qt::ControlModifier,
            {}, bytes_from_hex("03"),
            "terminal-input copy policy sends Ctrl+C to backend despite selection");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("terminal-input-sentinel"),
            "terminal-input copy policy leaves clipboard unchanged");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "copy-or-ignore policy surface starts");

        fixture.surface.set_copy_shortcut_policy(
            VNM_TerminalSurface::Copy_shortcut_policy::COPY_SELECTION_OR_IGNORE);
        QGuiApplication::clipboard()->setText(QStringLiteral("copy-or-ignore-sentinel"),
            QClipboard::Clipboard);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_key(
            fixture.surface,
            Qt::Key_C,
            Qt::ControlModifier,
            {},
            "copy-or-ignore policy consumes Ctrl+C without selection");
        ok &= check(backend_ptr->writes.size() == write_count,
            "copy-or-ignore policy with no selection writes no backend bytes");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("copy-or-ignore-sentinel"),
            "copy-or-ignore policy with no selection leaves clipboard unchanged");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "copy-or-ignore selection policy surface starts");

        fixture.surface.set_copy_shortcut_policy(
            VNM_TerminalSurface::Copy_shortcut_policy::COPY_SELECTION_OR_IGNORE);
        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "copy-or-ignore selection policy press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "copy-or-ignore selection policy drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "copy-or-ignore selection policy release is accepted");

        QGuiApplication::clipboard()->setText(QStringLiteral("copy-selection-sentinel"),
            QClipboard::Clipboard);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_key(
            fixture.surface,
            Qt::Key_C,
            Qt::ControlModifier,
            {},
            "copy-or-ignore policy consumes Ctrl+C with selection");
        ok &= check(backend_ptr->writes.size() == write_count,
            "copy-or-ignore policy with selection writes no backend bytes");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("lpha"),
            "copy-or-ignore policy with selection copies selected text");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "Ctrl+Shift+C pass-through surface starts");

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "Ctrl+Shift+C pass-through selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "Ctrl+Shift+C pass-through selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "Ctrl+Shift+C pass-through selection release is accepted");

        QGuiApplication::clipboard()->setText(QStringLiteral("shift-copy-sentinel"),
            QClipboard::Clipboard);
        ok &= send_key_and_expect_write(
            fixture.surface,
            *backend_ptr,
            Qt::Key_C,
            Qt::ControlModifier | Qt::ShiftModifier,
            {},
            bytes_from_hex("03"),
            "Ctrl+Shift+C is terminal input at the reusable surface");
        ok &= check(QGuiApplication::clipboard()->text(QClipboard::Clipboard) ==
            QStringLiteral("shift-copy-sentinel"),
            "Ctrl+Shift+C reusable surface path leaves clipboard unchanged");
    }

    return ok;
}

bool test_control_wheel_font_zoom(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int font_size_changed_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::font_size_changed,
        &fixture.surface,
        [&font_size_changed_count] {
            ++font_size_changed_count;
        });

    const qreal initial_font_size = fixture.surface.font_size();
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        120,
        false,
        "plain wheel with no session remains unconsumed");
    ok &= check(fixture.surface.font_size() == initial_font_size,
        "plain wheel leaves font size unchanged");

    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        40,
        true,
        "first high-resolution Ctrl+wheel fragment is retained");
    ok &= check(fixture.surface.font_size() == initial_font_size,
        "first high-resolution Ctrl+wheel fragment does not zoom alone");
    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        40,
        true,
        "second high-resolution Ctrl+wheel fragment is retained");
    ok &= check(fixture.surface.font_size() == initial_font_size,
        "second high-resolution Ctrl+wheel fragment does not zoom alone");
    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        40,
        true,
        "third high-resolution Ctrl+wheel fragment completes one zoom step");
    ok &= check(fixture.surface.font_size() == initial_font_size + 1.0,
        "three high-resolution Ctrl+wheel fragments zoom by one pixel");

    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        -120,
        true,
        "Ctrl+wheel-down after high-resolution fragments is consumed");
    ok &= check(fixture.surface.font_size() == initial_font_size,
        "Ctrl+wheel-down after high-resolution fragments restores font size");

    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        120,
        true,
        "Ctrl+wheel-up is consumed by font zoom");
    ok &= check(fixture.surface.font_size() == initial_font_size + 1.0,
        "Ctrl+wheel-up increases font size by one pixel");

    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        -120,
        true,
        "Ctrl+wheel-down is consumed by font zoom");
    ok &= check(fixture.surface.font_size() == initial_font_size,
        "Ctrl+wheel-down decreases font size by one pixel");

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "Ctrl+wheel zoom surface starts");
    const std::size_t write_count = backend_ptr->writes.size();
    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        120,
        true,
        "Ctrl+wheel zoom is consumed while a session is active");
    ok &= check(backend_ptr->writes.size() == write_count,
        "Ctrl+wheel zoom does not write terminal input bytes");
    ok &= check(font_size_changed_count == 5,
        "Ctrl+wheel zoom emits one font-size signal per actual change");

    fixture.surface.set_font_size(72.0);
    std::vector<bool> backpressure_states;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::output_backpressure_changed,
        &fixture.surface,
        [&backpressure_states](bool active) {
            backpressure_states.push_back(active);
        });

    QByteArray zoom_drain_output(70 * 1024, 'x');
    zoom_drain_output += QByteArrayLiteral("\r\nzoom-drain-output");
    backend_ptr->emit_output_from_worker(std::move(zoom_drain_output));
    backend_ptr->join_worker();
    const std::shared_ptr<const term::Terminal_render_snapshot> pre_zoom_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(pre_zoom_snapshot != nullptr &&
        !snapshot_contains_text(*pre_zoom_snapshot, QStringLiteral("zoom-drain")),
        "deferred backend output is still pending before clamped Ctrl+wheel");
    ok &= send_wheel_event(
        fixture.surface,
        Qt::ControlModifier,
        120,
        true,
        "clamped Ctrl+wheel drains deferred backend output");
    const std::shared_ptr<const term::Terminal_render_snapshot> post_zoom_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(post_zoom_snapshot != nullptr &&
        snapshot_contains_text(*post_zoom_snapshot, QStringLiteral("zoom-drain")),
        "clamped Ctrl+wheel exposes deferred backend output immediately");
    ok &= check(fixture.surface.font_size() == 72.0,
        "clamped Ctrl+wheel leaves maximum font size unchanged");
    ok &= check(!backend_ptr->output_paused,
        "clamped Ctrl+wheel drain resumes backend output");
    ok &= check(!backpressure_states.empty() && !backpressure_states.back(),
        "clamped Ctrl+wheel drain publishes final backpressure release");
    const std::size_t post_zoom_write_count = backend_ptr->writes.size();
    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_A,
        Qt::NoModifier,
        QStringLiteral("a"),
        QByteArrayLiteral("a"),
        "typing after clamped Ctrl+wheel drain remains writable");
    ok &= check(backend_ptr->writes.size() == post_zoom_write_count + 1U,
        "typing after clamped Ctrl+wheel drain reaches backend once");

    {
        Surface_fixture clamped_fixture;
        pump_events(app);

        clamped_fixture.surface.set_font_size(6.0);
        ok &= send_wheel_event(
            clamped_fixture.surface,
            Qt::ControlModifier,
            -120,
            true,
            "Ctrl+wheel at the minimum font size is still consumed");
        ok &= check(clamped_fixture.surface.font_size() == 6.0,
            "Ctrl+wheel at the minimum font size leaves zoom clamped");
    }

    return ok;
}

bool test_plain_wheel_scrolls_primary_scrollback(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {numbered_scroll_lines(80)};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "plain-wheel scrollback surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(tail_snapshot != nullptr,
        "plain-wheel scrollback has an initial render snapshot");
    if (tail_snapshot == nullptr) {
        return ok;
    }

    const QString tail_first_row = snapshot_row_text(*tail_snapshot, 0);
    ok &= check(tail_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
        "plain-wheel scrollback starts on the primary screen");
    ok &= check(tail_snapshot->viewport.scrollback_rows > 0,
        "plain-wheel scrollback fixture has scrollback rows");
    ok &= check(tail_snapshot->viewport.offset_from_tail == 0,
        "plain-wheel scrollback starts at the tail");

    const std::size_t write_count = backend_ptr->writes.size();
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        120,
        true,
        "plain wheel up is consumed by primary scrollback");
    const std::shared_ptr<const term::Terminal_render_snapshot> scrolled_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(scrolled_snapshot != nullptr,
        "plain wheel up publishes a scrolled render snapshot");
    if (scrolled_snapshot != nullptr) {
        ok &= check(scrolled_snapshot->viewport.offset_from_tail == 3,
            "plain wheel up moves the viewport three rows from tail");
        ok &= check(snapshot_row_text(*scrolled_snapshot, 3) == tail_first_row,
            "plain wheel up shifts the previous first tail row down by three");
    }
    ok &= check(backend_ptr->writes.size() == write_count,
        "plain wheel up does not write backend bytes");

    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        -120,
        true,
        "plain wheel down is consumed when returning toward tail");
    const std::shared_ptr<const term::Terminal_render_snapshot> returned_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(returned_snapshot != nullptr,
        "plain wheel down publishes a returned render snapshot");
    if (returned_snapshot != nullptr) {
        ok &= check(returned_snapshot->viewport.offset_from_tail == 0,
            "plain wheel down returns the viewport to the tail");
        ok &= check(snapshot_row_text(*returned_snapshot, 0) == tail_first_row,
            "plain wheel down restores tail snapshot text");
    }
    ok &= check(backend_ptr->writes.size() == write_count,
        "plain wheel down does not write backend bytes");

    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        40,
        true,
        "first high-resolution plain-wheel fragment is retained");
    std::shared_ptr<const term::Terminal_render_snapshot> high_res_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(high_res_snapshot != nullptr &&
        high_res_snapshot->viewport.offset_from_tail == 0,
        "first high-resolution plain-wheel fragment does not scroll alone");
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        40,
        true,
        "second high-resolution plain-wheel fragment is retained");
    high_res_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(high_res_snapshot != nullptr &&
        high_res_snapshot->viewport.offset_from_tail == 0,
        "second high-resolution plain-wheel fragment does not scroll alone");
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        40,
        true,
        "third high-resolution plain-wheel fragment completes one scroll step");
    high_res_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(high_res_snapshot != nullptr &&
        high_res_snapshot->viewport.offset_from_tail == 3,
        "three high-resolution plain-wheel fragments scroll by one angle step");
    ok &= check(backend_ptr->writes.size() == write_count,
        "high-resolution plain-wheel fragments do not write backend bytes");

    const int first_pixel_fragment = std::max(
        1,
        static_cast<int>(std::floor(current_cell_metrics(fixture.surface).height / 2.0)));
    const int second_pixel_fragment = std::max(
        1,
        static_cast<int>(
            std::ceil(current_cell_metrics(fixture.surface).height -
                static_cast<qreal>(first_pixel_fragment))));
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        first_pixel_fragment,
        0,
        true,
        "first plain-wheel pixel fragment is retained");
    std::shared_ptr<const term::Terminal_render_snapshot> pixel_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(pixel_snapshot != nullptr &&
        pixel_snapshot->viewport.offset_from_tail == 3,
        "first plain-wheel pixel fragment does not scroll alone");
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        second_pixel_fragment,
        0,
        true,
        "second plain-wheel pixel fragment completes one scroll line");
    pixel_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(pixel_snapshot != nullptr &&
        pixel_snapshot->viewport.offset_from_tail == 4,
        "plain-wheel pixel fragments scroll after one cell-height step");
    ok &= check(backend_ptr->writes.size() == write_count,
        "plain-wheel pixel fragments do not write backend bytes");

    return ok;
}

bool test_public_viewport_scroll_api(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    pump_events(app);

    int viewport_change_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::viewport_changed,
        &fixture.surface,
        [&viewport_change_count] {
            ++viewport_change_count;
        });

    ok &= check(!fixture.surface.scroll_viewport_lines(1),
        "public viewport scroll rejects missing session");
    ok &= check(!fixture.surface.scroll_to_offset_from_tail(1),
        "public viewport offset rejects missing session");
    ok &= check(fixture.surface.scrollback_rows() == 0,
        "public viewport starts with no scrollback rows");
    ok &= check(fixture.surface.viewport_offset_from_tail() == 0,
        "public viewport starts at tail");
    ok &= check(fixture.surface.viewport_at_tail(),
        "public viewport reports initial tail state");

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {numbered_scroll_lines(80)};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "public viewport API surface starts");
    ok &= check(backend_ptr != nullptr, "public viewport API retains backend");

    const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(tail_snapshot != nullptr,
        "public viewport API has an initial render snapshot");
    if (tail_snapshot == nullptr) {
        return ok;
    }

    ok &= check_int_equal(
        fixture.surface.scrollback_rows(),
        tail_snapshot->viewport.scrollback_rows,
        "public viewport exposes rendered scrollback rows");
    ok &= check_int_equal(
        fixture.surface.viewport_visible_rows(),
        tail_snapshot->viewport.visible_rows,
        "public viewport exposes rendered visible rows");
    ok &= check_int_equal(
        fixture.surface.viewport_offset_from_tail(),
        0,
        "public viewport starts at rendered tail");
    ok &= check(fixture.surface.viewport_at_tail(),
        "public viewport at-tail property starts true");

    const int initial_viewport_change_count = viewport_change_count;
    ok &= check(fixture.surface.scroll_viewport_lines(4),
        "public viewport line scroll moves viewport");
    const std::shared_ptr<const term::Terminal_render_snapshot> line_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(line_snapshot != nullptr,
        "public viewport line scroll publishes snapshot");
    if (line_snapshot != nullptr) {
        ok &= check_int_equal(
            fixture.surface.viewport_offset_from_tail(),
            line_snapshot->viewport.offset_from_tail,
            "public viewport line scroll property matches snapshot");
        ok &= check_int_equal(
            fixture.surface.viewport_offset_from_tail(),
            4,
            "public viewport line scroll reaches requested offset");
        ok &= check(!fixture.surface.viewport_at_tail(),
            "public viewport reports detached state");
    }
    ok &= check(viewport_change_count > initial_viewport_change_count,
        "public viewport line scroll emits viewport_changed");

    ok &= check(fixture.surface.scroll_to_offset_from_tail(100000),
        "public viewport offset scroll clamps to top");
    const std::shared_ptr<const term::Terminal_render_snapshot> top_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(top_snapshot != nullptr,
        "public viewport offset scroll publishes snapshot");
    if (top_snapshot != nullptr) {
        ok &= check_int_equal(
            fixture.surface.viewport_offset_from_tail(),
            top_snapshot->viewport.scrollback_rows,
            "public viewport offset clamps to maximum scrollback");
    }

    ok &= check(fixture.surface.scroll_to_offset_from_tail(0),
        "public viewport offset scroll returns to tail");
    ok &= check_int_equal(
        fixture.surface.viewport_offset_from_tail(),
        0,
        "public viewport offset returns to zero");
    ok &= check(fixture.surface.viewport_at_tail(),
        "public viewport reports tail after offset zero");
    ok &= check(!fixture.surface.scroll_to_offset_from_tail(0),
        "public viewport offset no-op reports no movement");

    const int pre_sync_viewport_change_count = viewport_change_count;
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
    ok &= check(!fixture.surface.scroll_viewport_lines(1),
        "public viewport scroll rejects hidden synchronized output");
    ok &= check_int_equal(
        fixture.surface.viewport_offset_from_tail(),
        0,
        "public viewport stays at visible tail while synchronized output is hidden");
    ok &= check(viewport_change_count == pre_sync_viewport_change_count,
        "public viewport hidden synchronized scroll emits no viewport_changed");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> sync_released_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(sync_released_snapshot != nullptr &&
        sync_released_snapshot->viewport.offset_from_tail == 0,
        "public viewport synchronized output release remains at tail");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1049halternate"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> alternate_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(alternate_snapshot != nullptr &&
        alternate_snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "public viewport API fixture enters alternate screen");
    const int pre_alternate_scroll_change_count = viewport_change_count;
    ok &= check(!fixture.surface.scroll_viewport_lines(1),
        "public viewport line scroll rejects alternate screen");
    ok &= check(!fixture.surface.scroll_to_offset_from_tail(1),
        "public viewport offset scroll rejects alternate screen");
    ok &= check(viewport_change_count == pre_alternate_scroll_change_count,
        "public viewport alternate-screen scroll emits no viewport_changed");

    return ok;
}

bool test_plain_wheel_scrolls_scroll_region_primary_scrollback(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("\x1b[1;1Htop-one"
            "\x1b[2;1Htop-two"
            "\x1b[3;1Hview"
            "\x1b[4;1Hbelow"
            "\x1b[5;1Hprompt"
            "\x1b[4;5r\x1b[4;1H\x1b" "M"
            "\x1b[r"
            "\x1b[1;4r\x1b[3;1H\r\nHIST"
            "\x1b[r\x1b[5;1H"),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "scroll-region scrollback surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(tail_snapshot != nullptr,
        "scroll-region scrollback has a render snapshot");
    if (tail_snapshot == nullptr) {
        return ok;
    }

    ok &= check(tail_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
        "scroll-region scrollback remains on primary screen");
    ok &= check(tail_snapshot->viewport.scrollback_rows == 0,
        "scroll-region first insert does not create scrollback before overflow");
    ok &= check(tail_snapshot->viewport.offset_from_tail == 0,
        "scroll-region scrollback starts at tail");
    ok &= check(snapshot_row_text(*tail_snapshot, 3) == QStringLiteral("HIST"),
        "scroll-region first insert writes history row");

    const std::size_t write_count = backend_ptr->writes.size();
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        120,
        false,
        "plain wheel up has no scroll-region scrollback before overflow");
    ok &= check(backend_ptr->writes.size() == write_count,
        "scroll-region pre-overflow wheel writes no backend bytes");

    backend_ptr->emit_output(
        QByteArrayLiteral("\x1b[1;4r\x1b[4;1H\r\nNEXT"
            "\x1b[r\x1b[5;1H"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> overflow_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(overflow_snapshot != nullptr,
        "scroll-region overflow publishes a snapshot");
    if (overflow_snapshot != nullptr) {
        ok &= check(overflow_snapshot->viewport.scrollback_rows == 1,
            "scroll-region overflow creates one primary scrollback row");
        ok &= check(snapshot_row_text(*overflow_snapshot, 0) == QStringLiteral("top-two"),
            "scroll-region overflow tail shows shifted primary content");
    }

    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        120,
        true,
        "plain wheel up scrolls overflowing scroll-region primary scrollback");
    const std::shared_ptr<const term::Terminal_render_snapshot> scrolled_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(scrolled_snapshot != nullptr,
        "scroll-region overflowing wheel scroll publishes a snapshot");
    if (scrolled_snapshot != nullptr) {
        ok &= check(scrolled_snapshot->viewport.offset_from_tail == 1,
            "scroll-region overflowing wheel scroll moves to the available scrollback row");
        ok &= check(snapshot_row_text(*scrolled_snapshot, 0) == QStringLiteral("top-one"),
            "scroll-region overflowing wheel scroll reveals the inserted scrollback row");
    }
    ok &= check(backend_ptr->writes.size() == write_count,
        "scroll-region overflowing wheel scroll writes no backend bytes");

    return ok;
}

bool test_plain_wheel_scrolls_csi_scroll_up_primary_scrollback(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("\x1b[1;1H111"
            "\x1b[2;1H222"
            "\x1b[3;1H333"
            "\x1b[4;1H444"
            "\x1b[1;3r\x1b[2S\x1b[r"),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "CSI SU scrollback surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(tail_snapshot != nullptr, "CSI SU scrollback has a render snapshot");
    if (tail_snapshot == nullptr) {
        return ok;
    }

    ok &= check(tail_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
        "CSI SU scrollback remains on primary screen");
    ok &= check(tail_snapshot->viewport.scrollback_rows == 2,
        "CSI SU creates primary scrollback rows");
    ok &= check(snapshot_row_text(*tail_snapshot, 0) == QStringLiteral("333"),
        "CSI SU tail shows shifted content");

    const std::size_t write_count = backend_ptr->writes.size();
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        120,
        true,
        "plain wheel up scrolls CSI SU primary scrollback");
    const std::shared_ptr<const term::Terminal_render_snapshot> scrolled_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(scrolled_snapshot != nullptr,
        "CSI SU wheel scroll publishes a snapshot");
    if (scrolled_snapshot != nullptr) {
        ok &= check(scrolled_snapshot->viewport.offset_from_tail > 0,
            "CSI SU wheel scroll moves into scrollback");
        ok &= check(snapshot_row_text(*scrolled_snapshot, 0) == QStringLiteral("111"),
            "CSI SU wheel scroll reveals earliest scrolled row");
    }
    ok &= check(backend_ptr->writes.size() == write_count,
        "CSI SU wheel scroll writes no backend bytes");

    return ok;
}

bool test_page_keys_scroll_primary_scrollback(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {numbered_scroll_lines(80)};

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "page-key scrollback surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> tail_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(tail_snapshot != nullptr,
        "page-key scrollback has an initial render snapshot");
    if (tail_snapshot == nullptr) {
        return ok;
    }

    ok &= check(tail_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
        "page-key scrollback starts on the primary screen");
    ok &= check(tail_snapshot->viewport.scrollback_rows > 0,
        "page-key scrollback fixture has scrollback rows");
    ok &= check(tail_snapshot->viewport.offset_from_tail == 0,
        "page-key scrollback starts at the tail");

    std::size_t write_count = backend_ptr->writes.size();
    ok &= send_key(
        fixture.surface,
        Qt::Key_PageDown,
        Qt::NoModifier,
        {},
        "PageDown at tail is terminal-owned when scrollback exists");
    std::shared_ptr<const term::Terminal_render_snapshot> boundary_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(boundary_snapshot != nullptr &&
        boundary_snapshot->viewport.offset_from_tail == 0,
        "PageDown at tail leaves the viewport at the tail");
    ok &= check(backend_ptr->writes.size() == write_count,
        "PageDown at tail writes no backend bytes when scrollback exists");

    write_count = backend_ptr->writes.size();
    ok &= send_key(
        fixture.surface,
        Qt::Key_PageUp,
        Qt::NoModifier,
        {},
        "PageUp is consumed by primary scrollback");
    std::shared_ptr<const term::Terminal_render_snapshot> scrolled_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(scrolled_snapshot != nullptr,
        "PageUp publishes a scrolled render snapshot");
    if (scrolled_snapshot != nullptr) {
        ok &= check(scrolled_snapshot->viewport.offset_from_tail ==
            tail_snapshot->viewport.visible_rows,
            "PageUp moves the viewport by one visible page");
    }
    ok &= check(backend_ptr->writes.size() == write_count,
        "PageUp scrollback writes no backend bytes");

    ok &= send_key(
        fixture.surface,
        Qt::Key_PageDown,
        Qt::NoModifier,
        {},
        "PageDown is consumed when returning toward tail");
    scrolled_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(scrolled_snapshot != nullptr &&
        scrolled_snapshot->viewport.offset_from_tail == 0,
        "PageDown returns the viewport to the tail");
    ok &= check(backend_ptr->writes.size() == write_count,
        "PageDown scrollback writes no backend bytes");

    write_count = backend_ptr->writes.size();
    bool reached_top = false;
    for (int i = 0; i < tail_snapshot->viewport.scrollback_rows + 2; ++i) {
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        if (snapshot                            == nullptr ||
            snapshot->viewport.offset_from_tail >= snapshot->viewport.scrollback_rows)
        {
            reached_top = snapshot != nullptr &&
                snapshot->viewport.offset_from_tail >= snapshot->viewport.scrollback_rows;
            break;
        }

        ok &= send_key(
            fixture.surface,
            Qt::Key_PageUp,
            Qt::NoModifier,
            {},
            "PageUp advances toward the top of primary scrollback");
    }
    ok &= check(reached_top, "PageUp loop reaches the top of primary scrollback");
    ok &= check(backend_ptr->writes.size() == write_count,
        "PageUp toward top writes no backend bytes");
    boundary_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    const int top_offset =
        boundary_snapshot != nullptr
            ? boundary_snapshot->viewport.offset_from_tail
            : -1;
    ok &= send_key(
        fixture.surface,
        Qt::Key_PageUp,
        Qt::NoModifier,
        {},
        "PageUp at scrollback top is terminal-owned");
    boundary_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(boundary_snapshot != nullptr &&
        boundary_snapshot->viewport.offset_from_tail == top_offset,
        "PageUp at scrollback top leaves the viewport at the top");
    ok &= check(backend_ptr->writes.size() == write_count,
        "PageUp at scrollback top writes no backend bytes");

    return ok;
}

bool test_page_keys_fall_through_on_alternate_screen(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    fixture.surface.set_scrollback_limit(200);
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        QByteArrayLiteral("primary-before\r\n\x1b[?1049halternate-screen"),
    };

    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "alternate Page-key surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(snapshot != nullptr &&
        snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "alternate Page-key fixture enters the alternate screen");

    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_PageUp,
        Qt::NoModifier,
        {},
        QByteArrayLiteral("\x1b[5~"),
        "PageUp on alternate screen writes terminal input");
    ok &= send_key_and_expect_write(
        fixture.surface,
        *backend_ptr,
        Qt::Key_PageDown,
        Qt::NoModifier,
        {},
        QByteArrayLiteral("\x1b[6~"),
        "PageDown on alternate screen writes terminal input");

    return ok;
}

bool test_plain_wheel_boundaries_and_alternate_input(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "no-scrollback wheel boundary surface starts");

        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            false,
            "plain wheel with no scrollback remains unconsumed");
        ok &= check(backend_ptr->writes.size() == write_count,
            "plain wheel with no scrollback writes no backend bytes");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1049halternate-screen"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "alternate-screen wheel input surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
            "alternate-screen wheel boundary fixture enters the alternate screen");

        const std::size_t wheel_up_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "plain wheel up on alternate screen falls back to terminal input");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            wheel_up_index,
            {
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
            },
            "plain wheel up on alternate screen writes cursor-up input");

        const std::size_t partial_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            40,
            true,
            "first alternate-screen high-resolution wheel fragment is accepted");
        ok &= check(backend_ptr->writes.size() == partial_wheel_index,
            "first alternate-screen high-resolution wheel fragment writes no input");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            40,
            true,
            "second alternate-screen high-resolution wheel fragment is accepted");
        ok &= check(backend_ptr->writes.size() == partial_wheel_index,
            "second alternate-screen high-resolution wheel fragment writes no input");
        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1007h"));
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            80,
            true,
            "alternate-scroll toggle clears partial wheel fragment");
        ok &= check(backend_ptr->writes.size() == partial_wheel_index,
            "alternate-scroll toggle prevents stale fragment input");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            40,
            true,
            "post-toggle high-resolution wheel fragment completes a fresh step");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            partial_wheel_index,
            {
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
            },
            "alternate-screen high-resolution wheel fragments write after a fresh full step");

        const std::size_t wheel_down_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            -120,
            true,
            "plain wheel down on alternate screen sends terminal input");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            wheel_down_index,
            {
                QByteArrayLiteral("\x1b[B"),
                QByteArrayLiteral("\x1b[B"),
                QByteArrayLiteral("\x1b[B"),
            },
            "plain wheel down on alternate screen writes cursor-down input");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1h"));
        const std::size_t application_cursor_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "alternate-screen wheel applies pending application-cursor mode before input");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            application_cursor_index,
            {
                QByteArrayLiteral("\x1bOA"),
                QByteArrayLiteral("\x1bOA"),
                QByteArrayLiteral("\x1bOA"),
            },
            "alternate-screen wheel input honors application-cursor mode");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1007l"));
        const std::size_t alternate_scroll_reset_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "plain wheel on alternate screen after DEC 1007 reset still uses fallback input");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            alternate_scroll_reset_index,
            {
                QByteArrayLiteral("\x1bOA"),
                QByteArrayLiteral("\x1bOA"),
                QByteArrayLiteral("\x1bOA"),
            },
            "plain wheel after DEC 1007 reset writes application-cursor input");

        fixture.surface.set_wheel_event_policy(
            VNM_TerminalSurface::Wheel_event_policy::LOCAL_SCROLLBACK_ONLY);
        const std::size_t local_only_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            false,
            "local-only wheel policy leaves alternate-screen wheel unconsumed");
        ok &= check(backend_ptr->writes.size() == local_only_index,
            "local-only wheel policy writes no alternate-screen input");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1049halternate-screen"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "synchronized alternate-scroll surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026h\x1b[?1007h"));
        const std::size_t synchronized_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "synchronized hidden alternate-scroll wheel is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            synchronized_wheel_index,
            {
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
            },
            "alternate-scroll wheel uses live synchronized-output mode");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1049h\x1b[?1007halternate-screen"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "local-first alternate-screen wheel surface starts");

        fixture.surface.set_wheel_event_policy(
            VNM_TerminalSurface::Wheel_event_policy::LOCAL_SCROLLBACK_FIRST);
        const std::size_t partial_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            40,
            true,
            "first local-first alternate-screen wheel fragment is accepted");
        ok &= check(backend_ptr->writes.size() == partial_wheel_index,
            "first local-first alternate-screen wheel fragment writes no input");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            40,
            true,
            "second local-first alternate-screen wheel fragment is accepted");
        ok &= check(backend_ptr->writes.size() == partial_wheel_index,
            "second local-first alternate-screen wheel fragment writes no input");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            40,
            true,
            "third local-first alternate-screen wheel fragment writes input");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            partial_wheel_index,
            {
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
                QByteArrayLiteral("\x1b[A"),
            },
            "local-first alternate-screen fragments are not lost to local scroll");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1049halternate-screen"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "alternate-screen page-wheel policy surface starts");

        fixture.surface.set_alternate_screen_wheel_policy(
            VNM_TerminalSurface::Alternate_screen_wheel_policy::PAGE_KEYS);
        const std::size_t wheel_up_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "alternate-screen page-wheel policy accepts wheel up");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            wheel_up_index,
            { QByteArrayLiteral("\x1b[5~") },
            "alternate-screen page-wheel policy writes PageUp input");

        const std::size_t wheel_down_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            -120,
            true,
            "alternate-screen page-wheel policy accepts wheel down");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            wheel_down_index,
            { QByteArrayLiteral("\x1b[6~") },
            "alternate-screen page-wheel policy writes PageDown input");

        const int first_pixel_fragment = std::max(
            1,
            static_cast<int>(std::floor(current_cell_metrics(fixture.surface).height / 2.0)));
        const int second_pixel_fragment = std::max(
            1,
            static_cast<int>(
                std::ceil(current_cell_metrics(fixture.surface).height -
                    static_cast<qreal>(first_pixel_fragment))));
        const std::size_t pixel_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            first_pixel_fragment,
            0,
            true,
            "first alternate-screen page-wheel pixel fragment is accepted");
        ok &= check(backend_ptr->writes.size() == pixel_wheel_index,
            "first alternate-screen page-wheel pixel fragment writes no input");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            second_pixel_fragment,
            0,
            true,
            "second alternate-screen page-wheel pixel fragment completes one page key");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            pixel_wheel_index,
            { QByteArrayLiteral("\x1b[5~") },
            "alternate-screen page-wheel pixel fragments write after one cell-height step");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1049h\x1b[?1000hlegacy-mouse-alt"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "alternate-screen legacy mouse wheel surface starts");

        const std::size_t wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "legacy non-SGR mouse wheel is accepted by unsupported mouse mode");
        ok &= check(backend_ptr->writes.size() == wheel_index,
            "legacy non-SGR mouse wheel writes no alternate fallback input by default");

        fixture.surface.set_alternate_screen_wheel_policy(
            VNM_TerminalSurface::Alternate_screen_wheel_policy::PAGE_KEYS);
        const std::size_t page_policy_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "page-policy legacy non-SGR mouse wheel is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            page_policy_wheel_index,
            { QByteArrayLiteral("\x1b[5~") },
            "page-policy legacy non-SGR mouse wheel writes PageUp input");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1000h") + numbered_scroll_lines(80),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "primary legacy mouse wheel surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY &&
            snapshot->viewport.scrollback_rows > 0 &&
            snapshot->viewport.offset_from_tail == 0,
            "primary legacy mouse wheel fixture starts at scrollback tail");

        const std::size_t wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "primary legacy non-SGR mouse wheel is terminal-owned");
        const std::shared_ptr<const term::Terminal_render_snapshot> wheel_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(wheel_snapshot != nullptr &&
            wheel_snapshot->viewport.offset_from_tail == 0,
            "primary legacy non-SGR mouse wheel does not scroll locally");
        ok &= check(backend_ptr->writes.size() == wheel_index,
            "primary legacy non-SGR mouse wheel writes no unsupported bytes");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("\x1b[?1000hlegacy-ready")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "primary no-scrollback legacy mouse wheel surface starts");

        const std::size_t wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "primary no-scrollback legacy non-SGR mouse wheel is terminal-owned");
        ok &= check(backend_ptr->writes.size() == wheel_index,
            "primary no-scrollback legacy non-SGR mouse wheel writes no unsupported bytes");
    }

    return ok;
}

bool test_mouse_reporting_surface_events(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1000;1006h") + numbered_scroll_lines(80),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "mouse reporting surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> initial_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(initial_snapshot != nullptr,
            "mouse reporting surface has a render snapshot");
        if (initial_snapshot == nullptr) {
            return ok;
        }

        const int report_row =
            std::min(2, std::max(0, initial_snapshot->grid_size.rows - 1));
        const int report_column =
            std::min(3, std::max(0, initial_snapshot->grid_size.columns - 1));
        const QPointF report_point =
            point_in_grid_cell(fixture.surface, report_row, report_column);
        ok &= check(report_row != 0 || report_column != 0,
            "mouse reporting fixture uses a nonzero grid cell");

        QQuickItem other_item;
        other_item.setParentItem(fixture.window.contentItem());
        other_item.forceActiveFocus();
        ok &= check(pump_until(app, [&] { return !fixture.surface.hasActiveFocus(); }),
            "mouse reporting starts with another item focused");

        const std::size_t click_index = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            report_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "active mouse press is accepted");
        ok &= check(fixture.surface.hasActiveFocus(),
            "accepted mouse press restores active terminal focus");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            report_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "active mouse release is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            click_index,
            {
                sgr_mouse_report(0, report_row, report_column, 'M'),
                sgr_mouse_report(0, report_row, report_column, 'm'),
            },
            "active click and release write SGR bytes");

        fixture.surface.set_mouse_reporting_policy(
            VNM_TerminalSurface::Mouse_reporting_policy::DISABLED);
        const std::size_t disabled_write_count = backend_ptr->writes.size();
        other_item.forceActiveFocus();
        ok &= check(pump_until(app, [&] { return !fixture.surface.hasActiveFocus(); }),
            "disabled mouse reporting focus fixture moves focus away");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            QPointF(1.0, 1.0),
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "disabled mouse reporting routes left press to selection");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            QPointF(1.0, 1.0),
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "disabled mouse reporting routes left release to selection");
        ok &= check(fixture.surface.hasActiveFocus(),
            "disabled mouse reporting press still restores terminal focus");
        ok &= check(backend_ptr->writes.size() == disabled_write_count,
            "disabled mouse reporting writes nothing");

        fixture.surface.set_mouse_reporting_policy(
            VNM_TerminalSurface::Mouse_reporting_policy::APPLICATION_CONTROLLED);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            QPointF(fixture.surface.width() + 10.0, fixture.surface.height() + 10.0),
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            false,
            "out-of-bounds mouse press remains unaccepted");
        ok &= check(backend_ptr->writes.size() == disabled_write_count,
            "out-of-bounds mouse press writes nothing");

        ok &= check(initial_snapshot->viewport.offset_from_tail == 0,
            "mouse wheel reporting starts at scrollback tail");
        const std::size_t wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            240,
            true,
            "active mouse wheel is accepted by terminal reporting");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            wheel_index,
            {
                sgr_mouse_report(64, report_row, report_column, 'M'),
                sgr_mouse_report(64, report_row, report_column, 'M'),
            },
            "active mouse wheel writes one SGR report per completed step");
        const std::shared_ptr<const term::Terminal_render_snapshot> wheel_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(wheel_snapshot != nullptr &&
            wheel_snapshot->viewport.offset_from_tail == 0,
            "active mouse wheel does not scroll scrollback");

        const std::size_t small_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "first small mouse wheel delta is retained");
        ok &= check(backend_ptr->writes.size() == small_wheel_index,
            "first small mouse wheel delta writes no SGR report");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "second small mouse wheel delta is retained");
        ok &= check(backend_ptr->writes.size() == small_wheel_index,
            "second small mouse wheel delta writes no SGR report");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "third small mouse wheel delta completes one report");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            small_wheel_index,
            { sgr_mouse_report(64, report_row, report_column, 'M') },
            "small mouse wheel deltas write only after a full step");

        const int first_pixel_fragment = std::max(
            1,
            static_cast<int>(std::floor(current_cell_metrics(fixture.surface).height / 2.0)));
        const int second_pixel_fragment = std::max(
            1,
            static_cast<int>(
                std::ceil(current_cell_metrics(fixture.surface).height -
                    static_cast<qreal>(first_pixel_fragment))));
        const std::size_t pixel_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            first_pixel_fragment,
            0,
            true,
            "first mouse-reporting pixel wheel fragment is retained");
        ok &= check(backend_ptr->writes.size() == pixel_wheel_index,
            "first mouse-reporting pixel wheel fragment writes no SGR report");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            second_pixel_fragment,
            0,
            true,
            "second mouse-reporting pixel wheel fragment completes one report");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            pixel_wheel_index,
            { sgr_mouse_report(64, report_row, report_column, 'M') },
            "mouse-reporting pixel wheel fragments write after one cell-height step");

        const std::size_t mode_boundary_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "mouse wheel partial step is retained before mode reset");
        ok &= check(backend_ptr->writes.size() == mode_boundary_wheel_index,
            "mouse wheel partial step before mode reset writes no SGR report");
        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000;1006l"));
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            120,
            true,
            "mouse DECRST drains and clears partial wheel remainder");
        ok &= check(backend_ptr->writes.size() == mode_boundary_wheel_index,
            "mouse DECRST after partial wheel writes no stale SGR report");
        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000;1006h"));
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            80,
            true,
            "mouse DECSET starts a fresh wheel remainder after reset");
        ok &= check(backend_ptr->writes.size() == mode_boundary_wheel_index,
            "mouse DECSET after reset does not reuse stale wheel remainder");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "fresh mouse wheel remainder completes after new partials");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            mode_boundary_wheel_index,
            { sgr_mouse_report(64, report_row, report_column, 'M') },
            "fresh mouse wheel remainder writes only after a new full step");

        const std::size_t collapsed_boundary_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "mouse wheel partial step is retained before collapsed mode toggle");
        ok &= check(backend_ptr->writes.size() == collapsed_boundary_wheel_index,
            "mouse wheel partial step before collapsed toggle writes no SGR report");
        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000;1006l\x1b[?1000;1006h"));
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            80,
            true,
            "collapsed mouse mode toggle starts a fresh wheel remainder");
        ok &= check(backend_ptr->writes.size() == collapsed_boundary_wheel_index,
            "collapsed mouse mode toggle does not reuse stale wheel remainder");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "collapsed mouse mode toggle allows new full wheel step");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            collapsed_boundary_wheel_index,
            { sgr_mouse_report(64, report_row, report_column, 'M') },
            "collapsed mouse mode toggle writes only after a new full step");

        const std::size_t ordinary_output_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            40,
            true,
            "mouse wheel partial step is retained before ordinary output");
        ok &= check(backend_ptr->writes.size() == ordinary_output_wheel_index,
            "mouse wheel partial step before ordinary output writes no SGR report");
        backend_ptr->emit_output(QByteArrayLiteral("ordinary-output"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            80,
            true,
            "ordinary output preserves mouse wheel remainder");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            ordinary_output_wheel_index,
            { sgr_mouse_report(64, report_row, report_column, 'M') },
            "ordinary output snapshot does not clear mouse wheel remainder");

        const qreal       font_size_before_zoom  = fixture.surface.font_size();
        const std::size_t ctrl_wheel_write_count = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::ControlModifier,
            report_point,
            0,
            120,
            true,
            "Ctrl+wheel keeps zoom priority while mouse reporting is active");
        ok &= check(fixture.surface.font_size() == font_size_before_zoom + 1.0,
            "Ctrl+wheel zoom still updates font size");
        ok &= check(backend_ptr->writes.size() == ctrl_wheel_write_count,
            "Ctrl+wheel zoom writes no mouse reporting bytes");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000;1006l"));
        const std::size_t pending_reset_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            120,
            true,
            "pending mouse DECRST is drained before wheel routing");
        ok &= check(backend_ptr->writes.size() == pending_reset_wheel_index,
            "pending mouse DECRST suppresses stale SGR wheel bytes");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "pending mouse DECSET wheel surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr,
            "pending mouse DECSET wheel surface has a render snapshot");
        if (snapshot == nullptr) {
            return ok;
        }

        const QPointF report_point = point_in_grid_cell(fixture.surface, 0, 0);
        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000;1006h"));
        const std::size_t pending_set_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            120,
            true,
            "pending mouse DECSET is drained before wheel routing");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            pending_set_wheel_index,
            { QByteArrayLiteral("\x1b[<64;1;1M") },
            "pending mouse DECSET routes wheel to SGR reporting");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1049h\x1b[?1000;1006halt-mouse-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "alternate-screen mouse wheel precedence surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
            "alternate-screen mouse wheel precedence fixture enters alternate screen");

        const QPointF     report_point = point_in_grid_cell(fixture.surface, 0, 0);
        const std::size_t wheel_index  = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            120,
            true,
            "mouse-reporting alternate-screen wheel is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            wheel_index,
            { QByteArrayLiteral("\x1b[<64;1;1M") },
            "mouse-reporting alternate-screen wheel prefers SGR over cursor keys");

        fixture.surface.set_alternate_screen_wheel_policy(
            VNM_TerminalSurface::Alternate_screen_wheel_policy::PAGE_KEYS);
        const std::size_t page_policy_wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            report_point,
            0,
            120,
            true,
            "page-policy alternate-screen mouse wheel is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            page_policy_wheel_index,
            { QByteArrayLiteral("\x1b[5~") },
            "page-policy alternate-screen wheel sends PageUp before SGR mouse");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1002;1006hdrag-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "drag mouse reporting surface starts");

        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr,
            "drag mouse reporting surface has a render snapshot");
        if (snapshot == nullptr) {
            return ok;
        }

        const int report_row =
            std::min(2, std::max(0, snapshot->grid_size.rows - 1));
        const int report_column =
            std::min(3, std::max(0, snapshot->grid_size.columns - 1));
        const QPointF report_point =
            point_in_grid_cell(fixture.surface, report_row, report_column);
        const QPointF outside_point(
            fixture.surface.width() + 10.0,
            fixture.surface.height() + 10.0);

        const std::size_t drag_index = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            report_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "button-event mouse press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            outside_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "button-event grabbed drag outside the grid is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            outside_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "button-event grabbed release outside the grid is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            drag_index,
            {
                sgr_mouse_report(0,  report_row, report_column, 'M'),
                sgr_mouse_report(32, report_row, report_column, 'M'),
                sgr_mouse_report(0,  report_row, report_column, 'm'),
            },
            "button-event grabbed drag and release preserve the last in-grid cell");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1003;1006hmotion-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "all-motion mouse reporting surface starts");

        const std::size_t motion_index = backend_ptr->writes.size();
        ok &= send_hover_move(
            fixture.surface,
            QPointF(1.0, 1.0),
            Qt::NoModifier,
            true,
            "all-motion hover move is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            motion_index,
            { QByteArrayLiteral("\x1b[<35;1;1M") },
            "all-motion hover writes SGR passive-motion bytes");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("wheel-ready")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "synchronized wheel mouse reporting surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026h\x1b[?1000;1006h"));
        const std::size_t wheel_index = backend_ptr->writes.size();
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "synchronized hidden mouse-reporting wheel is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            wheel_index,
            { sgr_mouse_report(64, 0, 1, 'M') },
            "wheel reporting uses live synchronized-output mouse mode");
    }

    return ok;
}

bool test_selection_drag_and_selected_text(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "selection surface starts");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "selected_text is empty before selection");

        int selection_changed_count = 0;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::selection_changed,
            &fixture.surface,
            [&selection_changed_count] {
                ++selection_changed_count;
            });

        const QPointF     start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF     end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "plain left press starts selection");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "plain left press alone does not activate selection");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "plain left drag extends selection");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "plain left release finishes selection");

        const std::shared_ptr<const term::Terminal_render_snapshot> selection_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(backend_ptr->writes.size() == write_count,
            "plain selection drag writes no mouse reporting bytes");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE,
            "plain selection drag activates public selection state");
        ok &= check_int_equal(
            selection_changed_count,
            1,
            "plain selection drag emits selection_changed for the ACTIVE transition");
        ok &= check(selection_snapshot != nullptr &&
            selection_snapshot->selection_spans.size() == 1U &&
            snapshot_has_selection_span(*selection_snapshot, 0, 1, 4),
            "plain selection drag publishes a visible selection span");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "plain selection drag exposes selected_text");

        const QPointF replacement_start = point_in_grid_cell(fixture.surface, 1, 0);
        const QPointF replacement_end   = point_in_grid_cell(fixture.surface, 1, 1);
        ok                             &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            replacement_start,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "new left press starts replacement selection");
        ok                             &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            replacement_end,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "new left release finishes replacement selection");

        const std::shared_ptr<const term::Terminal_render_snapshot> replaced_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check_int_equal(
            selection_changed_count,
            3,
            "replacement selection emits clear and activate selection_changed transitions");
        ok &= check(replaced_snapshot != nullptr &&
            replaced_snapshot->selection_spans.size() == 1U &&
            snapshot_has_selection_span(*replaced_snapshot, 1, 0, 2),
            "new selection replaces old selection span");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("be"),
            "new selection replaces old selected text");

        const QPointF single_click_point = point_in_grid_cell(fixture.surface, 0, 0);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            single_click_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "single left press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            single_click_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "single left release is accepted");

        const std::shared_ptr<const term::Terminal_render_snapshot> click_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check_int_equal(
            selection_changed_count,
            4,
            "single left click emits selection_changed for the clear transition");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "single left click clears selection state");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "single left click leaves selected_text empty");
        ok &= check(click_snapshot != nullptr && click_snapshot->selection_spans.empty(),
            "single left click publishes no selection spans");

        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            replacement_start,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "clear_selection fixture starts another selection");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            replacement_end,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "clear_selection fixture finishes another selection");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE &&
            !fixture.surface.selected_text().isEmpty(),
            "clear_selection fixture has an active selection before clearing");
        ok &= check_int_equal(
            selection_changed_count,
            5,
            "press-release replacement emits selection_changed for reactivation");
        fixture.surface.clear_selection();
        const std::shared_ptr<const term::Terminal_render_snapshot> cleared_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "clear_selection clears public selection state");
        ok &= check_int_equal(
            selection_changed_count,
            6,
            "clear_selection emits selection_changed for the NONE transition");
        ok &= check(cleared_snapshot != nullptr && cleared_snapshot->selection_spans.empty(),
            "clear_selection publishes a snapshot without selection spans");

        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "multi-row selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            replacement_end,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "multi-row selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            replacement_end,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "multi-row selection release is accepted");
        const std::shared_ptr<const term::Terminal_render_snapshot> multi_row_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const int multi_row_tail_columns =
            multi_row_snapshot != nullptr ? multi_row_snapshot->grid_size.columns - 1 : 0;
        ok &= check(multi_row_snapshot != nullptr &&
            multi_row_snapshot->selection_spans.size() == 2U &&
            snapshot_has_selection_span(
                *multi_row_snapshot, 0, 1, multi_row_tail_columns) &&
            snapshot_has_selection_span(*multi_row_snapshot, 1, 0, 2),
            "surface multi-row drag publishes row-boundary selection spans");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha\nbe"),
            "surface multi-row drag exposes selected text across rows");

        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            end_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "same-row reverse selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            start_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "same-row reverse selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            start_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "same-row reverse selection release is accepted");
        const std::shared_ptr<const term::Terminal_render_snapshot> reverse_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(reverse_snapshot != nullptr &&
            reverse_snapshot->selection_spans.size() == 1U &&
            snapshot_has_selection_span(*reverse_snapshot, 0, 1, 4),
            "same-row reverse drag publishes the expected selection span");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "same-row reverse drag exposes selected text");

        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            replacement_end,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "multi-row reverse selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            start_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "multi-row reverse selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            start_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "multi-row reverse selection release is accepted");
        const std::shared_ptr<const term::Terminal_render_snapshot> reverse_multi_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(reverse_multi_snapshot != nullptr &&
            reverse_multi_snapshot->selection_spans.size() == 2U &&
            snapshot_has_selection_span(
                *reverse_multi_snapshot, 0, 1, multi_row_tail_columns) &&
            snapshot_has_selection_span(*reverse_multi_snapshot, 1, 0, 2),
            "multi-row reverse drag publishes row-boundary selection spans");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha\nbe"),
            "multi-row reverse drag exposes selected text across rows");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "buffer-switch selection surface starts");

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "buffer-switch selection press is accepted");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1049halt"));
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "buffer-switch selection drag is cancelled");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "buffer switch during selection drag leaves no active selection");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "buffer switch during selection drag leaves no selected text");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1002;1006hmouse-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "mouse-reporting selection suppression surface starts");

        const QPointF     point       = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF     drag_point  = point_in_grid_cell(fixture.surface, 0, 3);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "mouse-reporting left press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            drag_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "mouse-reporting left drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            drag_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "mouse-reporting left release is accepted");

        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_count,
            {
                sgr_mouse_report(0, 0, 1, 'M'),
                sgr_mouse_report(32, 0, 3, 'M'),
                sgr_mouse_report(0, 0, 3, 'm'),
            },
            "mouse-reporting drag writes SGR bytes");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "mouse-reporting drag keeps public selection state empty");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "mouse-reporting drag does not start selection text");
        ok &= check(snapshot != nullptr && snapshot->selection_spans.empty(),
            "mouse-reporting drag does not publish selection spans");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1002;1006hmouse-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "multi-button mouse-reporting surface starts");

        const QPointF     left_point  = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF     right_point = point_in_grid_cell(fixture.surface, 0, 2);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            left_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "multi-button left press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            right_point,
            Qt::RightButton,
            Qt::LeftButton | Qt::RightButton,
            Qt::NoModifier,
            true,
            "multi-button right press is accepted while left remains down");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            left_point,
            Qt::LeftButton,
            Qt::RightButton,
            Qt::NoModifier,
            true,
            "multi-button left release is accepted while right remains down");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            right_point,
            Qt::RightButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "multi-button right final release is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_count,
            {
                sgr_mouse_report(0, 0, 1, 'M'),
                sgr_mouse_report(2, 0, 2, 'M'),
                sgr_mouse_report(0, 0, 1, 'm'),
                sgr_mouse_report(2, 0, 2, 'm'),
            },
            "multi-button reporting preserves per-button press/release pairing");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1002;1006hmouse-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "unhandled mouse move position surface starts");

        const QPointF press_point         = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF disabled_move_point = point_in_grid_cell(fixture.surface, 0, 3);
        const QPointF out_of_bounds_point =
            QPointF(fixture.surface.width() + 10.0, fixture.surface.height() + 10.0);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            press_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "unhandled-position press is accepted");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1002;1006l"));
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            disabled_move_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            false,
            "disabled-mode mouse move is unhandled");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1002;1006h"));
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            out_of_bounds_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "reenabled out-of-bounds release uses last handled position");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_count,
            {
                sgr_mouse_report(0, 0, 1, 'M'),
                sgr_mouse_report(0, 0, 1, 'm'),
            },
            "unhandled mouse move does not replace last reported position");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "mouse-reporting clears existing selection surface starts");

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "pre-reporting selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "pre-reporting selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "pre-reporting selection release is accepted");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE,
            "pre-reporting drag creates a selection");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000;1006h"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "mouse-reporting takeover press is accepted");
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_count,
            { sgr_mouse_report(0, 0, 1, 'M') },
            "mouse-reporting takeover writes the SGR press");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "mouse-reporting takeover clears existing selection state");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "mouse-reporting takeover clears selected text");
        ok &= check(snapshot != nullptr && snapshot->selection_spans.empty(),
            "mouse-reporting takeover clears selection spans");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            start_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "mouse-reporting takeover release is accepted");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_count,
            {
                sgr_mouse_report(0, 0, 1, 'M'),
                sgr_mouse_report(0, 0, 1, 'm'),
            },
            "mouse-reporting takeover preserves press/release pairing");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1003;1006halpha\r\nbeta"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "shift-selection mouse-reporting surface starts");

        const QPointF     start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF     end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::ShiftModifier,
            true,
            "shift left press starts local selection under mouse reporting");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::ShiftModifier,
            true,
            "shift left drag extends local selection under mouse reporting");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::ShiftModifier,
            true,
            "shift left release finishes local selection under mouse reporting");
        ok &= check(backend_ptr->writes.size() == write_count,
            "shift local selection writes no mouse reporting bytes");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE,
            "shift local selection activates public selection state");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "shift local selection exposes selected text");
        ok &= send_hover_move(
            fixture.surface,
            end_point,
            Qt::NoModifier,
            true,
            "passive mouse reporting hover is accepted after shift selection");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_count,
            { sgr_mouse_report(35, 0, 4, 'M') },
            "passive hover after shift selection writes SGR bytes");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::ACTIVE,
            "passive hover after shift selection preserves selection state");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "passive hover after shift selection preserves selected text");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {
            QByteArrayLiteral("\x1b[?1002;1006hmouse-ready"),
        };

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "shift-mid-drag mouse-reporting surface starts");

        const QPointF     start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF     drag_point  = point_in_grid_cell(fixture.surface, 0, 3);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "terminal-owned drag press is accepted before Shift modifier");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            drag_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::ShiftModifier,
            true,
            "terminal-owned drag continues reporting after Shift modifier");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            drag_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::ShiftModifier,
            true,
            "terminal-owned drag release is reported after Shift modifier");
        ok &= check_write_chunks_equal(
            backend_ptr->writes,
            write_count,
            {
                sgr_mouse_report(0,  0, 1, 'M'),
                sgr_mouse_report(36, 0, 3, 'M'),
                sgr_mouse_report(4,  0, 3, 'm'),
            },
            "terminal-owned Shift drag preserves press-drag-release reporting");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "detached scrollback selection surface starts");
        ok &= send_wheel_event(
            fixture.surface,
            Qt::NoModifier,
            120,
            true,
            "detached scrollback selection scrolls up");
        const std::shared_ptr<const term::Terminal_render_snapshot> scrolled_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        if (scrolled_snapshot != nullptr) {
            const int first_visible_logical_row =
                scrolled_snapshot->viewport.scrollback_rows -
                scrolled_snapshot->viewport.offset_from_tail;
            const QString expected_text =
                QStringLiteral("scroll-line-%1")
                    .arg(first_visible_logical_row + 1, 3, 10, QLatin1Char('0'));
            ok &= check(snapshot_row_text(*scrolled_snapshot, 1) == expected_text,
                "detached scrollback selection fixture independently shows row text");
            const QPointF start_point = point_in_grid_cell(fixture.surface, 1, 0);
            const QPointF end_point   = point_in_grid_cell(fixture.surface, 1, 14);
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonPress,
                start_point,
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "detached scrollback selection press is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseMove,
                end_point,
                Qt::NoButton,
                Qt::LeftButton,
                Qt::NoModifier,
                true,
                "detached scrollback selection drag is accepted");
            ok &= send_mouse_event(
                fixture.surface,
                QEvent::MouseButtonRelease,
                end_point,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                true,
                "detached scrollback selection release is accepted");
            ok &= check(fixture.surface.selected_text() == expected_text,
                "detached scrollback selection uses logical scrollback coordinates");
        }
        else {
            ok &= check(false, "detached scrollback selection snapshot is available");
        }
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(200);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {numbered_scroll_lines(80)};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "pending-output selection surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("scroll-line-080\r\n"));
        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 14);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "pending-output selection press drains callbacks before anchoring");
        const std::shared_ptr<const term::Terminal_render_snapshot> post_press_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        const int queued_line_count = 81;
        const int expected_first_line = post_press_snapshot != nullptr
            ? queued_line_count - post_press_snapshot->grid_size.rows + 1
            : -1;
        const QString expected_text =
            QStringLiteral("scroll-line-%1")
                .arg(expected_first_line, 3, 10, QLatin1Char('0'));
        ok &= check(post_press_snapshot != nullptr &&
            snapshot_row_text(*post_press_snapshot, 0) == expected_text &&
            snapshot_contains_text(*post_press_snapshot, QStringLiteral("scroll-line-080")),
            "pending-output selection press drains queued output before selecting");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "pending-output selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "pending-output selection release is accepted");
        ok &= check(!expected_text.isEmpty() &&
            fixture.surface.selected_text() == expected_text,
            "pending-output selection anchors against the drained snapshot");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "synchronized-output local selection surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hbravo"));
        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 0);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            false,
            "local selection press is ignored while synchronized output is hidden");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            false,
            "local selection drag is ignored while synchronized output is hidden");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            false,
            "local selection release is ignored while synchronized output is hidden");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "hidden synchronized output does not create local selection state");
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "hidden synchronized output does not expose hidden selected text");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_scrollback_limit(0);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("alpha\r\nbeta")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "selected_text drain surface starts");

        const QPointF start_point = point_in_grid_cell(fixture.surface, 0, 1);
        const QPointF end_point   = point_in_grid_cell(fixture.surface, 0, 4);
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            start_point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "selected_text drain selection press is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseMove,
            end_point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            "selected_text drain selection drag is accepted");
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonRelease,
            end_point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            true,
            "selected_text drain selection release is accepted");
        ok &= check(fixture.surface.selected_text() == QStringLiteral("lpha"),
            "selected_text drain fixture starts with selected text");

        backend_ptr->emit_output(numbered_scroll_lines(80));
        ok &= check(fixture.surface.selected_text().isEmpty(),
            "selected_text drains queued output before reading evicted selection");
        ok &= check(fixture.surface.selection_state() ==
            VNM_TerminalSurface::Selection_state::NONE,
            "selected_text drain updates public selection state after eviction");
    }

    return ok;
}

bool test_stale_synchronized_output_recovery(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        fixture.surface.set_synchronized_output_stale_timeout_ms(25);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("visible ")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "stale synchronized-output recovery surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);

        std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            snapshot_contains_text(*snapshot, QStringLiteral("visible")) &&
            !snapshot_contains_text(*snapshot, QStringLiteral("held")),
            "stale synchronized-output recovery keeps held output hidden initially");
        const std::uint64_t hidden_sequence =
            snapshot != nullptr ? snapshot->metadata.sequence : 0U;

        ok &= check(pump_until(app, [&] {
            snapshot =
                term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
            return
                snapshot != nullptr                                       &&
                snapshot_contains_text(*snapshot, QStringLiteral("held")) &&
                snapshot->metadata.sequence > hidden_sequence;
        }),
            "stale synchronized-output recovery force releases held output after timeout");
    }

    {
        Surface_fixture fixture;
        fixture.surface.set_synchronized_output_stale_timeout_ms(25);
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("visible ")};

        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "explicit synchronized-output release surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);

        std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            !snapshot_contains_text(*snapshot, QStringLiteral("held")),
            "explicit synchronized-output release keeps held output hidden initially");
        const std::uint64_t hidden_sequence =
            snapshot != nullptr ? snapshot->metadata.sequence : 0U;

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
            fixture.surface);

        snapshot = term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            snapshot_contains_text(*snapshot, QStringLiteral("held")) &&
            snapshot->metadata.sequence > hidden_sequence,
            "explicit synchronized-output release publishes held output");
        const std::uint64_t released_sequence =
            snapshot != nullptr ? snapshot->metadata.sequence : 0U;

        pump_events(app, 8);
        snapshot = term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
        ok &= check(snapshot != nullptr &&
            snapshot->metadata.sequence == released_sequence &&
            snapshot_contains_text(*snapshot, QStringLiteral("held")),
            "explicit synchronized-output release does not publish again after stale timeout");
    }

    return ok;
}

bool test_paste_text_public_method_and_policy(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        ok &= check(!fixture.surface.paste_text(QStringLiteral("no-session")),
            "no-session paste_text returns false");
        ok &= check(error_codes.empty(),
            "no-session paste_text emits no backend_error");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "paste policy surface starts");

        const std::size_t initial_write_index = backend_ptr->writes.size();
        ok &= check(fixture.surface.paste_text(QStringLiteral("plain\r\ntext")),
            "application-controlled paste without DECSET returns true");
        ok &= check(joined_writes_since(backend_ptr->writes, initial_write_index) ==
            QByteArrayLiteral("plain\ntext"),
            "application-controlled paste without DECSET writes unframed sanitized text");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2004h"));
        const std::size_t framed_write_index = backend_ptr->writes.size();
        ok &= check(fixture.surface.paste_text(QStringLiteral("mode")),
            "application-controlled paste after pending DECSET returns true");
        ok &= check(joined_writes_since(backend_ptr->writes, framed_write_index) ==
            framed_paste(QByteArrayLiteral("mode")),
            "surface paste_text drains pending DECSET before policy mapping");

        fixture.surface.set_bracketed_paste_policy(
            VNM_TerminalSurface::Bracketed_paste_policy::DISABLED);
        const std::size_t disabled_write_index = backend_ptr->writes.size();
        ok &= check(fixture.surface.paste_text(QStringLiteral("disabled")),
            "disabled surface paste returns true");
        ok &= check(joined_writes_since(backend_ptr->writes, disabled_write_index) ==
            QByteArrayLiteral("disabled"),
            "disabled surface paste stays unframed even in terminal bracketed mode");

        fixture.surface.set_bracketed_paste_policy(
            VNM_TerminalSurface::Bracketed_paste_policy::ENABLED);
        const std::size_t enabled_write_index = backend_ptr->writes.size();
        ok &= check(fixture.surface.paste_text(QStringLiteral("enabled")),
            "enabled surface paste returns true");
        ok &= check(joined_writes_since(backend_ptr->writes, enabled_write_index) ==
            framed_paste(QByteArrayLiteral("enabled")),
            "enabled surface paste always frames");

        const std::size_t empty_write_index = backend_ptr->writes.size();
        ok &= check(!fixture.surface.paste_text(QString(QChar(0x001b))),
            "surface paste_text returns false when sanitization removes the body");
        ok &= check(backend_ptr->writes.size() == empty_write_index,
            "surface paste_text sends no frame for sanitized-empty paste");
        ok &= check(error_codes.empty(),
            "surface paste success path emits no backend_error");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "post-exit paste surface starts");

        backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
        pump_events(app);
        ok &= check(error_codes.empty(),
            "post-exit setup emits no backend_error before paste");

        const std::size_t write_count = backend_ptr->writes.size();
        ok &= check(!fixture.surface.paste_text(QStringLiteral("after")),
            "post-exit paste_text returns false");
        ok &= check(backend_ptr->writes.size() == write_count,
            "post-exit paste_text does not reach backend write");
        ok &= check(error_codes.size() == 1U &&
            error_codes.front() == VNM_TerminalSurface::Backend_error_code::WRITE_FAILED,
            "post-exit paste_text reports WRITE_FAILED");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        auto backend = std::make_unique<Scripted_backend>();
        backend->reject_writes = true;
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "write-rejected paste surface starts");

        fixture.surface.set_bracketed_paste_policy(
            VNM_TerminalSurface::Bracketed_paste_policy::DISABLED);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= check(!fixture.surface.paste_text(QStringLiteral("rejected")),
            "backend-rejected paste_text returns false");
        ok &= check(backend_ptr->writes.size() == write_count + 1U,
            "backend-rejected paste_text reaches backend write");
        ok &= check(error_codes.size() == 1U &&
            error_codes.front() == VNM_TerminalSurface::Backend_error_code::WRITE_FAILED,
            "backend-rejected paste_text reports WRITE_FAILED");
    }

    return ok;
}

bool test_right_click_paste_and_mouse_reporting_precedence(QGuiApplication& app)
{
    bool ok = true;
    Clipboard_text_guard clipboard_guard;
    QClipboard* clipboard = QGuiApplication::clipboard();
    clipboard->setText(QStringLiteral("right-paste"), QClipboard::Clipboard);

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "right-click paste surface starts");

        fixture.surface.set_bracketed_paste_policy(
            VNM_TerminalSurface::Bracketed_paste_policy::DISABLED);
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            point_in_grid_cell(fixture.surface, 0, 0),
            Qt::RightButton,
            Qt::RightButton,
            Qt::NoModifier,
            true,
            "right-click paste press is accepted");
        ok &= check(backend_ptr->writes.size() == write_count + 1U,
            "right-click paste writes clipboard text");
        if (backend_ptr->writes.size() > write_count) {
            ok &= check_bytes_equal(
                backend_ptr->writes.back(),
                QByteArrayLiteral("right-paste"),
                "right-click paste writes unframed clipboard text");
        }
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "right-click mouse-reporting surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000;1006h"));
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            point_in_grid_cell(fixture.surface, 0, 0),
            Qt::RightButton,
            Qt::RightButton,
            Qt::NoModifier,
            true,
            "right-click mouse-reporting press is accepted");
        ok &= check(backend_ptr->writes.size() == write_count + 1U,
            "right-click mouse reporting writes one event");
        if (backend_ptr->writes.size() > write_count) {
            ok &= check_bytes_equal(
                backend_ptr->writes.back(),
                sgr_mouse_report(2, 0, 0, 'M'),
                "right-click mouse reporting takes precedence over paste");
        }
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "legacy right-click mouse-reporting surface starts");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1000h"));
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_mouse_event(
            fixture.surface,
            QEvent::MouseButtonPress,
            point_in_grid_cell(fixture.surface, 0, 0),
            Qt::RightButton,
            Qt::RightButton,
            Qt::NoModifier,
            true,
            "legacy mouse-reporting right-click is terminal-owned");
        ok &= check(backend_ptr->writes.size() == write_count,
            "legacy mouse-reporting right-click does not paste clipboard text");
    }

    return ok;
}

bool test_focus_reporting_writes_mode_bytes_and_preserves_ime_cancel(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::backend_error,
        &fixture.surface,
        [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
            error_codes.push_back(code);
        });

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "focus reporting surface starts");

    QQuickItem other_item;
    other_item.setParentItem(fixture.window.contentItem());
    other_item.setFocus(true);

    const auto focus_surface = [&]() {
        fixture.surface.forceActiveFocus();
        return pump_until(app, [&] { return fixture.surface.hasActiveFocus(); });
    };
    const auto focus_other = [&]() {
        other_item.forceActiveFocus();
        return pump_until(app, [&] { return !fixture.surface.hasActiveFocus(); });
    };

    ok &= check(focus_other(),
        "focus reporting fixture starts from inactive surface focus");

    const std::size_t disabled_write_count = backend_ptr->writes.size();
    ok &= check(focus_surface(),
        "disabled focus reporting surface receives focus");
    ok &= check(backend_ptr->writes.size() == disabled_write_count,
        "disabled focus-in reporting writes no backend bytes");
    ok &= check(focus_other(),
        "disabled focus reporting surface loses focus");
    ok &= check(backend_ptr->writes.size() == disabled_write_count,
        "disabled focus-out reporting writes no backend bytes");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1004h"));
    const std::size_t focus_in_index = backend_ptr->writes.size();
    ok &= check(focus_surface(),
        "enabled focus reporting surface receives focus");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        focus_in_index,
        { QByteArrayLiteral("\x1b[I") },
        "enabled surface focus-in writes CSI I");

    backend_ptr->emit_output(numbered_scroll_lines(80));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
        fixture.surface);
    ok &= send_wheel_event(
        fixture.surface,
        Qt::NoModifier,
        120,
        true,
        "focus reporting scrollback fixture detaches viewport");
    std::shared_ptr<const term::Terminal_render_snapshot> detached_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(detached_snapshot != nullptr &&
        detached_snapshot->viewport.offset_from_tail == 3,
        "focus reporting fixture starts from detached surface viewport");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("pending"),
        4,
        "focus reporting IME preedit event is accepted");
    term::Ime_preedit_state ime_preedit =
        term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok &= check(ime_preedit.active,
        "focus reporting IME preedit starts active");

    const std::size_t focus_out_index = backend_ptr->writes.size();
    const std::shared_ptr<const term::Terminal_render_snapshot> before_focus_out_snapshot =
        detached_snapshot;
    ok &= check(focus_other(),
        "enabled focus reporting surface loses focus");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        focus_out_index,
        { QByteArrayLiteral("\x1b[O") },
        "enabled surface focus-out writes CSI O");
    ime_preedit =
        term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "focus-out reporting still cancels IME preedit");
    detached_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(detached_snapshot == before_focus_out_snapshot,
        "surface focus-out report does not publish a replacement snapshot");
    ok &= check(detached_snapshot != nullptr &&
        detached_snapshot->viewport.offset_from_tail == 3,
        "surface focus-out report preserves detached viewport");

    const std::size_t detached_focus_in_index = backend_ptr->writes.size();
    const std::shared_ptr<const term::Terminal_render_snapshot> before_focus_in_snapshot =
        detached_snapshot;
    ok &= check(focus_surface(),
        "enabled focus reporting surface regains focus while viewport is detached");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        detached_focus_in_index,
        { QByteArrayLiteral("\x1b[I") },
        "enabled detached surface focus-in writes CSI I");
    detached_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(detached_snapshot == before_focus_in_snapshot,
        "surface focus-in report does not publish a replacement snapshot");
    ok &= check(detached_snapshot != nullptr &&
        detached_snapshot->viewport.offset_from_tail == 3,
        "surface focus-in report preserves detached viewport");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1004l"));
    const std::size_t reset_write_count = backend_ptr->writes.size();
    ok &= check(focus_other(),
        "reset focus reporting surface loses focus");
    ok &= check(backend_ptr->writes.size() == reset_write_count,
        "surface focus-out after DECRST writes no backend bytes");
    ok &= check(focus_surface(),
        "reset focus reporting surface receives focus");
    ok &= check(backend_ptr->writes.size() == reset_write_count,
        "surface focus-in after DECRST writes no backend bytes");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1004h"));
    backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
    pump_events(app);
    const std::size_t exited_write_count = backend_ptr->writes.size();
    ok &= check(focus_other(),
        "post-exit focus reporting surface loses focus");
    ok &= check(backend_ptr->writes.size() == exited_write_count,
        "post-exit focus reporting writes no backend bytes");
    ok &= check(error_codes.empty(),
        "focus reporting success and no-op paths emit no backend_error");

    {
        Surface_fixture rejected_fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> rejected_error_codes;
        QObject::connect(
            &rejected_fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &rejected_fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                rejected_error_codes.push_back(code);
            });

        auto rejected_backend = std::make_unique<Scripted_backend>();
        rejected_backend->reject_writes = true;
        bool rejected_started = false;
        Scripted_backend* rejected_backend_ptr = start_surface_with_backend(
            rejected_fixture.surface,
            std::move(rejected_backend),
            { QStringLiteral("scripted-terminal") },
            &rejected_started);
        ok &= check(rejected_started, "rejected focus reporting surface starts");

        QQuickItem rejected_other_item;
        rejected_other_item.setParentItem(rejected_fixture.window.contentItem());
        rejected_other_item.setFocus(true);

        rejected_backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1004h"));
        const std::size_t rejected_focus_index = rejected_backend_ptr->writes.size();
        rejected_fixture.surface.forceActiveFocus();
        ok &= check(pump_until(app, [&] { return rejected_fixture.surface.hasActiveFocus(); }),
            "backend-rejected focus reporting surface receives focus");
        ok &= check_write_chunks_equal(
            rejected_backend_ptr->writes,
            rejected_focus_index,
            { QByteArrayLiteral("\x1b[I") },
            "backend-rejected focus reporting reaches backend write");
        ok &= check(rejected_error_codes.size() == 1U &&
            rejected_error_codes.front() ==
                VNM_TerminalSurface::Backend_error_code::WRITE_FAILED,
            "backend-rejected focus reporting reports one WRITE_FAILED backend_error");
    }

    return ok;
}

bool test_ime_preedit_updates_overlay_without_snapshot_churn(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME preedit surface starts");

    backend_ptr->emit_output(QByteArrayLiteral("\r\norder-before-ime"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot_before =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    const term::Terminal_surface_render_invalidation_stats_t stats_before =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    const std::size_t write_count = backend_ptr->writes.size();
    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("compose"),
        3,
        "IME preedit event is accepted");

    const term::Ime_preedit_state ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
        fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    const term::Terminal_surface_render_invalidation_stats_t stats_after =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(ime_preedit.active &&
        ime_preedit.text == QStringLiteral("compose") &&
        ime_preedit.cursor_position == 3,
        "IME preedit appears in surface overlay state");
    ok &= check(snapshot == snapshot_before,
        "IME preedit update does not replace the latest render snapshot");
    ok &= check(stats_after.update_requests > stats_before.update_requests,
        "IME preedit update requests a render update");
    ok &= check(snapshot != nullptr &&
        snapshot_contains_text(*snapshot, QStringLiteral("order-before-ime")),
        "terminal output snapshot remains available after IME preedit");
    ok &= check(snapshot != nullptr &&
        !snapshot_contains_text(*snapshot, QStringLiteral("compose")),
        "IME preedit does not mutate screen model cells");
    ok &= check(backend_ptr->writes.size() == write_count,
        "IME preedit update produces no backend writes");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("replacement"),
        5,
        "IME replacement preedit event is accepted");
    const term::Ime_preedit_state replacement_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
        fixture.surface);
    ok &= check(replacement_preedit.active &&
        replacement_preedit.text == QStringLiteral("replacement") &&
        replacement_preedit.cursor_position == 5,
        "IME preedit replacement updates text and cursor");
    ok &= check(term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface) ==
        snapshot_before,
        "IME preedit replacement still keeps the latest render snapshot");
    ok &= check(backend_ptr->writes.size() == write_count,
        "IME preedit replacement produces no backend writes");

    backend_ptr->emit_output(QByteArrayLiteral("\r\nmodel-after-preedit"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot_with_preedit =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(snapshot_with_preedit != nullptr &&
        snapshot_with_preedit != snapshot_before &&
        snapshot_contains_text(*snapshot_with_preedit, QStringLiteral("model-after-preedit")),
        "terminal output still publishes a model snapshot while IME preedit is active");
    ok &= check(snapshot_with_preedit != nullptr &&
        snapshot_with_preedit->ime_preedit.active,
        "model snapshots may carry current IME preedit for compatibility");

    const term::Terminal_surface_render_invalidation_stats_t cancel_stats_before =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= send_empty_ime_event(
        fixture.surface,
        "IME preedit cancel event is accepted");
    const term::Ime_preedit_state canceled_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
        fixture.surface);
    const term::Terminal_surface_render_invalidation_stats_t cancel_stats_after =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(!canceled_preedit.active && canceled_preedit.text.isEmpty(),
        "IME cancel clears authoritative overlay state");
    ok &= check(term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface) ==
        snapshot_with_preedit,
        "IME cancel does not replace a model snapshot that carried preedit compatibility state");
    ok &= check(cancel_stats_after.update_requests > cancel_stats_before.update_requests,
        "IME cancel requests a render update");

    return ok;
}

bool test_ime_commit_writes_utf8_and_clears_preedit(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME commit surface starts");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("pending"),
        2,
        "IME preedit before commit is accepted");

    const std::size_t write_count     = backend_ptr->writes.size();
    const QByteArray  expected_commit = bytes_from_hex("e7958c");
    ok &= send_ime_commit(
        fixture.surface,
        QString::fromUtf8(expected_commit),
        "IME commit event is accepted");
    ok &= check(backend_ptr->writes.size() == write_count + 1U,
        "IME commit writes one backend chunk");
    if (backend_ptr->writes.size() > write_count) {
        ok &= check_bytes_equal(
            backend_ptr->writes.back(),
            expected_commit,
            "IME commit writes UTF-8 bytes");
    }

    const term::Ime_preedit_state commit_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
        fixture.surface);
    ok &= check(!commit_preedit.active && commit_preedit.text.isEmpty(),
        "IME commit clears preedit overlay state");

    const std::size_t empty_commit_write_count = backend_ptr->writes.size();
    ok &= send_empty_ime_commit(fixture.surface, "empty IME commit event is accepted");
    ok &= check(backend_ptr->writes.size() == empty_commit_write_count,
        "empty IME commit produces no backend writes");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("to-cancel"),
        4,
        "IME preedit before empty event is accepted");
    const std::size_t empty_event_write_count = backend_ptr->writes.size();
    ok &= send_empty_ime_event(fixture.surface, "empty IME event is accepted");
    const term::Ime_preedit_state empty_event_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
        fixture.surface);
    ok &= check(backend_ptr->writes.size() == empty_event_write_count,
        "empty IME event produces no backend writes");
    ok &= check(!empty_event_preedit.active &&
        empty_event_preedit.text.isEmpty(),
        "empty IME event cancels active preedit");
    ok &= check(backend_error_count == 0,
        "IME commit success path emits no backend_error");

    return ok;
}

bool test_ime_no_session_overlay_and_later_start_clear(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    const term::Terminal_surface_render_invalidation_stats_t stats_before =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("local"),
        3,
        "no-session IME preedit event is accepted");
    term::Ime_preedit_state ime_preedit =
        term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    const term::Terminal_surface_render_invalidation_stats_t stats_after =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    ok &= check(ime_preedit.active &&
        ime_preedit.text == QStringLiteral("local") &&
        ime_preedit.cursor_position == 3,
        "no-session IME preedit updates local overlay");
    ok &= check(stats_after.update_requests > stats_before.update_requests,
        "no-session IME preedit requests a render update");

    ok          &= send_ime_commit(
        fixture.surface,
        QStringLiteral("ignored"),
        "no-session IME commit event is accepted");
    ime_preedit  = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok          &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "no-session IME commit clears local overlay");

    ok          &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("empty-commit"),
        6,
        "no-session empty-commit preedit event is accepted");
    ok          &= send_empty_ime_commit(
        fixture.surface,
        "no-session empty IME commit event is accepted");
    ime_preedit  = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok          &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "no-session empty commit clears local overlay");

    ok          &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("plain-empty"),
        5,
        "no-session plain-empty preedit event is accepted");
    ok          &= send_empty_ime_event(
        fixture.surface,
        "no-session plain empty IME event is accepted");
    ime_preedit  = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok          &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "no-session plain empty event clears local overlay");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("stray"),
        2,
        "no-session stray preedit event is accepted");

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    bool started = false;
    (void)start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "no-session IME later start succeeds");
    ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "later start clears no-session IME overlay");
    ok &= check(snapshot != nullptr &&
        !snapshot_contains_text(*snapshot, QStringLiteral("stray")),
        "later start has no stray no-session preedit in model snapshot");
    ok &= check(backend_error_count == 0,
        "no-session IME events emit no backend_error");

    return ok;
}

bool test_ime_startup_commit_clears_preedit_without_backend_error(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    backend->on_start             = [&] {
        ok &= send_ime_preedit(
            fixture.surface,
            QStringLiteral("startup"),
            4,
            "startup IME preedit event is accepted");
        ok &= send_ime_commit(
            fixture.surface,
            QStringLiteral("ignored"),
            "startup IME commit event is accepted");
    };
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "startup IME surface starts");

    const term::Ime_preedit_state ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
        fixture.surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(backend_ptr->writes.empty(),
        "startup IME commit writes no backend bytes");
    ok &= check(backend_error_count == 0,
        "startup IME commit reports no backend_error");
    ok &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "startup IME commit does not ghost an old preedit");
    ok &= check(snapshot != nullptr &&
        !snapshot_contains_text(*snapshot, QStringLiteral("startup")),
        "startup IME preedit does not leak into model snapshot");

    return ok;
}

bool test_ime_multi_codepoint_commit_writes_utf8(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME multi-codepoint commit surface starts");

    const QByteArray  expected_commit = bytes_from_hex("41e7958cf09f9880");
    const std::size_t write_count     = backend_ptr->writes.size();
    ok &= send_ime_commit(
        fixture.surface,
        QString::fromUtf8(expected_commit),
        "IME multi-codepoint commit event is accepted");
    ok &= check(backend_ptr->writes.size() == write_count + 1U,
        "IME multi-codepoint commit writes one backend chunk");
    if (backend_ptr->writes.size() > write_count) {
        ok &= check_bytes_equal(
            backend_ptr->writes.back(),
            expected_commit,
            "IME multi-codepoint commit writes exact UTF-8");
    }

    return ok;
}

bool test_ime_combined_commit_and_preedit(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME combined commit/preedit surface starts");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("previous"),
        1,
        "IME combined previous preedit is accepted");

    const std::size_t write_count = backend_ptr->writes.size();
    ok &= send_ime_event(
        fixture.surface,
        QStringLiteral("go"),
        QStringLiteral("next"),
        2,
        0,
        0,
        "IME combined commit/preedit event is accepted");
    ok &= check(backend_ptr->writes.size() == write_count + 1U,
        "IME combined event writes commit bytes");
    if (backend_ptr->writes.size() > write_count) {
        ok &= check_bytes_equal(
            backend_ptr->writes.back(),
            QByteArrayLiteral("go"),
            "IME combined event writes exact commit bytes");
    }

    const term::Ime_preedit_state ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
        fixture.surface);
    ok &= check(ime_preedit.active &&
        ime_preedit.text == QStringLiteral("next") &&
        ime_preedit.cursor_position == 2,
        "IME combined event leaves replacement preedit active");

    return ok;
}

bool test_ime_replacement_range_is_stream_commit(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME replacement-range surface starts");

    const std::size_t write_count = backend_ptr->writes.size();
    ok &= send_ime_event(
        fixture.surface,
        QStringLiteral("range"),
        {},
        0,
        -2,
        3,
        "IME replacement-range commit event is accepted");
    ok &= check(backend_ptr->writes.size() == write_count + 1U,
        "IME replacement-range commit writes one backend chunk");
    ok &= check_write_chunks_equal(
        backend_ptr->writes,
        write_count,
        { QByteArrayLiteral("range") },
        "IME replacement range is treated as stream commit text");

    return ok;
}

bool test_ime_commit_failure_preserves_preedit(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("prompt")};
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "IME post-exit failure surface starts");

        ok &= send_ime_preedit(
            fixture.surface,
            QStringLiteral("pending"),
            3,
            "IME post-exit failure preedit is accepted");
        backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
        pump_events(app);

        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_ime_commit(
            fixture.surface,
            QStringLiteral("x"),
            "IME post-exit commit event is accepted");
        const term::Ime_preedit_state ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
            fixture.surface);
        ok &= check(backend_ptr->writes.size() == write_count,
            "IME post-exit commit does not reach backend write");
        ok &= check(error_codes.size() == 1U &&
            error_codes.front() == VNM_TerminalSurface::Backend_error_code::WRITE_FAILED,
            "IME post-exit commit reports WRITE_FAILED");
        ok &= check(ime_preedit.active &&
            ime_preedit.text == QStringLiteral("pending"),
            "IME post-exit commit preserves active preedit");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        auto backend = std::make_unique<Scripted_backend>();
        backend->outputs_during_start = {QByteArrayLiteral("prompt")};
        backend->reject_writes        = true;
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "IME write-rejection failure surface starts");

        ok &= send_ime_preedit(
            fixture.surface,
            QStringLiteral("pending"),
            3,
            "IME write-rejection failure preedit is accepted");
        const std::size_t write_count = backend_ptr->writes.size();
        ok &= send_ime_commit(
            fixture.surface,
            QStringLiteral("x"),
            "IME write-rejection commit event is accepted");
        const term::Ime_preedit_state ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
            fixture.surface);
        ok &= check(backend_ptr->writes.size() == write_count + 1U,
            "IME write-rejection commit reaches backend write");
        ok &= check(error_codes.size() == 1U &&
            error_codes.front() == VNM_TerminalSurface::Backend_error_code::WRITE_FAILED,
            "IME write-rejection commit reports WRITE_FAILED");
        ok &= check(ime_preedit.active &&
            ime_preedit.text == QStringLiteral("pending"),
            "IME write-rejection commit preserves active preedit");
    }

    return ok;
}

bool test_ime_synchronized_output_overlay(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("A")};
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME synchronized-output surface starts");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hB"));
    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("sync"),
        2,
        "IME synchronized-output preedit event is accepted");

    std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    term::Ime_preedit_state ime_preedit =
        term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok &= check(ime_preedit.active &&
        ime_preedit.text == QStringLiteral("sync"),
        "IME preedit is visible during synchronized output");
    ok &= check(snapshot != nullptr &&
        snapshot_row_text(*snapshot, 0) == QStringLiteral("A"),
        "IME preedit during synchronized output does not expose held output");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026lC"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
    snapshot     = term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ime_preedit  = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok          &= check(snapshot != nullptr &&
        snapshot_row_text(*snapshot, 0) == QStringLiteral("ABC") &&
        ime_preedit.active &&
        ime_preedit.text == QStringLiteral("sync"),
        "synchronized output release preserves active IME overlay");

    ok          &= send_empty_ime_event(
        fixture.surface,
        "IME synchronized-output empty event is accepted");
    snapshot     = term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ime_preedit  = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok          &= check(snapshot != nullptr &&
        !ime_preedit.active &&
        snapshot_row_text(*snapshot, 0) == QStringLiteral("ABC"),
        "IME cancel after synchronized output release clears overlay");

    return ok;
}

bool test_ime_focus_loss_cancels_preedit(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("prompt")};
    bool started = false;
    (void)start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME focus-loss surface starts");

    fixture.surface.forceActiveFocus();
    ok &= check(pump_until(app, [&] { return fixture.surface.hasActiveFocus(); }),
        "IME focus-loss surface receives active focus");
    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("pending"),
        4,
        "IME focus-loss preedit event is accepted");
    term::Ime_preedit_state ime_preedit =
        term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok &= check(ime_preedit.active,
        "IME focus-loss test starts with active preedit");

    QQuickItem other_item;
    other_item.setParentItem(fixture.window.contentItem());
    other_item.setFocus(true);
    other_item.forceActiveFocus();
    ok &= check(pump_until(app, [&] { return !fixture.surface.hasActiveFocus(); }),
        "IME focus-loss surface loses active focus");

    ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    ok &= check(!ime_preedit.active &&
        ime_preedit.text.isEmpty(),
        "IME focus loss cancels active preedit");

    return ok;
}

bool test_ime_cursor_rectangle_query_tracks_snapshot_cursor(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("A")};
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "IME cursor-rectangle surface starts");
    ok &= check(fixture.surface.inputMethodQuery(Qt::ImEnabled).toBool(),
        "IME cursor rectangle surface reports input method enabled");

    const QRectF first_rect =
        fixture.surface.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    ok &= check(!first_rect.isEmpty(),
        "IME cursor rectangle query returns a non-empty rect");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[3;6H"));
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);

    const QRectF moved_rect =
        fixture.surface.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    ok &= check(!moved_rect.isEmpty(),
        "IME moved cursor rectangle query returns a non-empty rect");
    ok &= check(!nearly_equal(moved_rect.left(), first_rect.left()) ||
        !nearly_equal(moved_rect.top(), first_rect.top()),
        "IME cursor rectangle changes after cursor movement");

    const qreal cell_width  = moved_rect.width();
    const qreal cell_height = moved_rect.height();
    ok &= check_rect_near(
        moved_rect,
        QRectF(5.0 * cell_width, 2.0 * cell_height, cell_width, cell_height),
        "IME cursor rectangle matches CUP 3;6 cell geometry");

    ok &= send_ime_preedit(
        fixture.surface,
        QString::fromUtf8("A\xe7\x95\x8c" "B"),
        2,
        "IME cursor rectangle preedit offset event is accepted");
    const QRectF preedit_rect =
        fixture.surface.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    ok &= check_rect_near(
        preedit_rect,
        QRectF(8.0 * cell_width, 2.0 * cell_height, cell_width, cell_height),
        "IME cursor rectangle includes active preedit display width");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("abc"),
        -20,
        "IME negative cursor preedit event is accepted");
    term::Ime_preedit_state ime_preedit =
        term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    const QRectF negative_clamped_rect =
        fixture.surface.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    ok &= check(ime_preedit.cursor_position == 0,
        "IME negative preedit cursor position clamps to start");
    ok &= check_rect_near(
        negative_clamped_rect,
        QRectF(5.0 * cell_width, 2.0 * cell_height, cell_width, cell_height),
        "IME negative preedit cursor clamp keeps rectangle at terminal cursor");

    ok &= send_ime_preedit(
        fixture.surface,
        QStringLiteral("abc"),
        200,
        "IME over-length cursor preedit event is accepted");
    ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    const QRectF over_length_rect =
        fixture.surface.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    ok &= check(ime_preedit.cursor_position == 3,
        "IME over-length preedit cursor position clamps to text length");
    ok &= check_rect_near(
        over_length_rect,
        QRectF(8.0 * cell_width, 2.0 * cell_height, cell_width, cell_height),
        "IME over-length preedit cursor clamp places rectangle after text");

    ok &= send_empty_ime_event(
        fixture.surface,
        "IME cursor rectangle clear event is accepted");
    ime_preedit = term::VNM_TerminalSurface_render_bridge::ime_preedit_state(fixture.surface);
    const QRectF cleared_rect =
        fixture.surface.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    ok &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "IME cursor rectangle clear resets overlay state");
    ok &= check_rect_near(
        cleared_rect,
        QRectF(5.0 * cell_width, 2.0 * cell_height, cell_width, cell_height),
        "IME cursor rectangle clear resets to terminal cursor");

    return ok;
}

bool test_keyboard_cursor_modes(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "keyboard cursor mode surface starts");

    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Up, Qt::NoModifier,
        {}, bytes_from_hex("1b5b41"),
        "normal Up writes CSI A");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Down, Qt::NoModifier,
        {}, bytes_from_hex("1b5b42"),
        "normal Down writes CSI B");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Right, Qt::NoModifier,
        {}, bytes_from_hex("1b5b43"),
        "normal Right writes CSI C");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Left, Qt::NoModifier,
        {}, bytes_from_hex("1b5b44"),
        "normal Left writes CSI D");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1h"));
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Left, Qt::NoModifier,
        {}, bytes_from_hex("1b4f44"),
        "application cursor Left drains pending mode output and writes SS3 D");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Left, Qt::ShiftModifier,
        {}, bytes_from_hex("1b5b313b3244"),
        "modified Left ignores application cursor mode");
    ok &= check(backend_error_count == 0,
        "keyboard cursor success path emits no backend_error");

    return ok;
}

bool test_keyboard_navigation_keys(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "keyboard navigation surface starts");

    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Home, Qt::NoModifier,
        {}, bytes_from_hex("1b5b48"),
        "Home writes CSI H");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_End, Qt::NoModifier,
        {}, bytes_from_hex("1b5b46"),
        "End writes CSI F");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Insert, Qt::NoModifier,
        {}, bytes_from_hex("1b5b327e"),
        "Insert writes CSI 2 tilde");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Delete, Qt::NoModifier,
        {}, bytes_from_hex("1b5b337e"),
        "Delete writes CSI 3 tilde");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_PageUp, Qt::NoModifier,
        {}, bytes_from_hex("1b5b357e"),
        "PageUp writes CSI 5 tilde");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_PageDown, Qt::NoModifier,
        {}, bytes_from_hex("1b5b367e"),
        "PageDown writes CSI 6 tilde");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Backtab, Qt::ShiftModifier,
        {}, bytes_from_hex("1b5b5a"),
        "Shift+Tab writes CSI Z");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Tab, Qt::ShiftModifier,
        QStringLiteral("\t"), bytes_from_hex("1b5b5a"),
        "Shift+Tab through Key_Tab writes CSI Z");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Home,
        Qt::ShiftModifier | Qt::ControlModifier,
        {}, bytes_from_hex("1b5b313b3648"),
        "modified Home writes CSI 1;6 H");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_End, Qt::AltModifier,
        {}, bytes_from_hex("1b5b313b3346"),
        "modified End writes CSI 1;3 F");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Delete,
        Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier,
        {}, bytes_from_hex("1b5b333b387e"),
        "modified Delete writes CSI 3;8 tilde");
    ok &= check(backend_error_count == 0,
        "keyboard navigation success path emits no backend_error");

    return ok;
}

bool test_keyboard_function_keys(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "keyboard function surface starts");

    const struct {
        int         key;
        const char* expected_hex;
        const char* message;
    } unmodified_cases[] = {
        { Qt::Key_F1,  "1b4f50",     "F1 writes SS3 P"         },
        { Qt::Key_F2,  "1b4f51",     "F2 writes SS3 Q"         },
        { Qt::Key_F3,  "1b4f52",     "F3 writes SS3 R"         },
        { Qt::Key_F4,  "1b4f53",     "F4 writes SS3 S"         },
        { Qt::Key_F5,  "1b5b31357e", "F5 writes CSI 15 tilde"  },
        { Qt::Key_F6,  "1b5b31377e", "F6 writes CSI 17 tilde"  },
        { Qt::Key_F7,  "1b5b31387e", "F7 writes CSI 18 tilde"  },
        { Qt::Key_F8,  "1b5b31397e", "F8 writes CSI 19 tilde"  },
        { Qt::Key_F9,  "1b5b32307e", "F9 writes CSI 20 tilde"  },
        { Qt::Key_F10, "1b5b32317e", "F10 writes CSI 21 tilde" },
        { Qt::Key_F11, "1b5b32337e", "F11 writes CSI 23 tilde" },
        { Qt::Key_F12, "1b5b32347e", "F12 writes CSI 24 tilde" },
    };
    for (const auto& key_case : unmodified_cases) {
        ok &= send_key_and_expect_write(
            fixture.surface, *backend_ptr, key_case.key, Qt::NoModifier,
            {}, bytes_from_hex(key_case.expected_hex),
            key_case.message);
    }

    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_F1, Qt::ShiftModifier,
        {}, bytes_from_hex("1b5b313b3250"),
        "modified F1 writes CSI 1;2 P");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_F5,
        Qt::ControlModifier | Qt::AltModifier,
        {}, bytes_from_hex("1b5b31353b377e"),
        "modified F5 writes CSI 15;7 tilde");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_F12,
        Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier,
        {}, bytes_from_hex("1b5b32343b387e"),
        "modified F12 writes CSI 24;8 tilde");
    ok &= check(backend_error_count == 0,
        "keyboard function success path emits no backend_error");

    return ok;
}

bool test_keyboard_keypad_modes(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "keyboard keypad surface starts");

    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_5, Qt::KeypadModifier,
        QStringLiteral("5"), bytes_from_hex("35"),
        "normal keypad digit writes text");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Plus, Qt::KeypadModifier,
        QStringLiteral("+"), bytes_from_hex("2b"),
        "normal keypad plus writes text");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Enter, Qt::KeypadModifier,
        QStringLiteral("\r"), bytes_from_hex("0d"),
        "normal keypad Enter writes carriage return");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b="));

    const struct {
        int         key;
        QString     text;
        const char* expected_hex;
        const char* message;
    } application_cases[] = {
        { Qt::Key_0,        QStringLiteral("0"),  "1b4f70", "application keypad 0"        },
        { Qt::Key_9,        QStringLiteral("9"),  "1b4f79", "application keypad 9"        },
        { Qt::Key_Period,   QStringLiteral("."),  "1b4f6e", "application keypad decimal"  },
        { Qt::Key_Minus,    QStringLiteral("-"),  "1b4f6d", "application keypad minus"    },
        { Qt::Key_Comma,    QStringLiteral(","),  "1b4f6c", "application keypad comma"    },
        { Qt::Key_Plus,     QStringLiteral("+"),  "1b4f6b", "application keypad plus"     },
        { Qt::Key_Asterisk, QStringLiteral("*"),  "1b4f6a", "application keypad multiply" },
        { Qt::Key_Slash,    QStringLiteral("/"),  "1b4f6f", "application keypad divide"   },
        { Qt::Key_Enter,    QStringLiteral("\r"), "1b4f4d", "application keypad enter"    },
        { Qt::Key_Equal,    QStringLiteral("="),  "1b4f58", "application keypad equal"    },
    };
    for (const auto& key_case : application_cases) {
        ok &= send_key_and_expect_write(
            fixture.surface, *backend_ptr, key_case.key, Qt::KeypadModifier,
            key_case.text, bytes_from_hex(key_case.expected_hex),
            key_case.message);
    }

    backend_ptr->emit_output(QByteArrayLiteral("\x1b>"));
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_5, Qt::KeypadModifier,
        QStringLiteral("5"), bytes_from_hex("35"),
        "ESC > resets keypad digit to normal text");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Plus, Qt::KeypadModifier,
        QStringLiteral("+"), bytes_from_hex("2b"),
        "ESC > resets keypad operator to normal text");
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_Enter, Qt::KeypadModifier,
        QStringLiteral("\r"), bytes_from_hex("0d"),
        "ESC > resets keypad Enter to carriage return");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?66h"));
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_5, Qt::KeypadModifier,
        QStringLiteral("5"), bytes_from_hex("1b4f75"),
        "DECNKM set makes keypad digit application encoded");
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?66l"));
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_5, Qt::KeypadModifier,
        QStringLiteral("5"), bytes_from_hex("35"),
        "DECNKM reset restores keypad digit text");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026h\x1b[?66h"));
    ok &= send_key_and_expect_write(
        fixture.surface, *backend_ptr, Qt::Key_5, Qt::KeypadModifier,
        QStringLiteral("5"), bytes_from_hex("1b4f75"),
        "DECNKM under synchronized output still applies immediately to input");

    ok &= check(backend_error_count == 0,
        "keyboard keypad success path emits no backend_error");

    return ok;
}

bool test_keyboard_focus_routed_delivery(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);
    int backend_error_count = 0;
    observe_backend_errors(fixture.surface, backend_error_count);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "focus-routed keyboard surface starts");

    ok &= send_window_key_and_expect_write(
        fixture.surface,
        fixture.window,
        *backend_ptr,
        Qt::Key_Left,
        Qt::NoModifier,
        {},
        bytes_from_hex("1b5b44"),
        "focus-routed window special key event writes through active terminal item");
    ok &= check(backend_error_count == 0,
        "focus-routed keyboard success path emits no backend_error");

    return ok;
}

bool test_keyboard_no_session_and_post_exit_semantics(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        pump_events(app);
        int backend_error_count = 0;
        observe_backend_errors(fixture.surface, backend_error_count);

        QKeyEvent event(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        QCoreApplication::sendEvent(&fixture.surface, &event);
        ok &= check(event.isAccepted(),
            "no-session mapped key event is accepted");
        ok &= check(backend_error_count == 0,
            "no-session mapped key event emits no backend_error");

        QKeyEvent ignored_event(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, {});
        QCoreApplication::sendEvent(&fixture.surface, &ignored_event);
        ok &= check(!ignored_event.isAccepted(),
            "no-session unmapped key event is ignored");
        ok &= check(backend_error_count == 0,
            "no-session unmapped key event emits no backend_error");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "post-exit keyboard surface starts");

        backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
        pump_events(app);

        const std::size_t write_count = backend_ptr->writes.size();
        QKeyEvent event(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        QCoreApplication::sendEvent(&fixture.surface, &event);
        ok &= check(event.isAccepted(),
            "post-exit mapped key event remains accepted");
        ok &= check(backend_ptr->writes.size() == write_count,
            "post-exit mapped key event does not reach backend write");
        ok &= check(error_codes.size() == 1U &&
            error_codes.front() == VNM_TerminalSurface::Backend_error_code::WRITE_FAILED,
            "post-exit mapped key event reports one WRITE_FAILED backend_error");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        auto backend = std::make_unique<Scripted_backend>();
        backend->reject_writes = true;
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);
        ok &= check(started, "write-rejection keyboard surface starts");

        const std::size_t write_count = backend_ptr->writes.size();
        QKeyEvent event(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        QCoreApplication::sendEvent(&fixture.surface, &event);
        ok &= check(event.isAccepted(),
            "backend-rejected mapped key event remains accepted");
        ok &= check(backend_ptr->writes.size() == write_count + 1U,
            "backend-rejected mapped key event reaches backend write");
        ok &= check(error_codes.size() == 1U &&
            error_codes.front() == VNM_TerminalSurface::Backend_error_code::WRITE_FAILED,
            "backend-rejected mapped key event reports one WRITE_FAILED backend_error");
    }

    return ok;
}

bool test_keyboard_unhandled_key_skips_backend_drain(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "unhandled-key drain surface starts");

    backend_ptr->emit_output(QByteArrayLiteral("ignored-key-output"));

    QKeyEvent event(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, {});
    QCoreApplication::sendEvent(&fixture.surface, &event);
    ok &= check(!event.isAccepted(),
        "unhandled key event is ignored while backend output is pending");

    const std::shared_ptr<const term::Terminal_render_snapshot> pre_pump_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(pre_pump_snapshot == nullptr ||
        !snapshot_contains_text(*pre_pump_snapshot, QStringLiteral("ignored-key-output")),
        "unhandled key event does not drain pending backend output");

    pump_events(app);
    const std::shared_ptr<const term::Terminal_render_snapshot> post_pump_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(post_pump_snapshot != nullptr &&
        snapshot_contains_text(*post_pump_snapshot, QStringLiteral("ignored-key-output")),
        "pending backend output still drains through the GUI event path");

    return ok;
}

bool test_worker_callback_drains_on_gui_thread(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int  activity_count         = 0;
    bool activity_on_gui_thread = false;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::output_activity,
        &fixture.surface,
        [&] {
            ++activity_count;
            activity_on_gui_thread =
                QThread::currentThread() == fixture.surface.thread();
        },
        Qt::DirectConnection);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "worker-callback surface starts");

    backend_ptr->emit_output_from_worker(QByteArrayLiteral("worker-output"));
    backend_ptr->join_worker();
    ok &= check(activity_count == 0,
        "worker callback is not drained inline on the worker thread");
    const std::shared_ptr<const term::Terminal_render_snapshot> pre_gui_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(pre_gui_snapshot == nullptr ||
        !snapshot_contains_text(*pre_gui_snapshot, QStringLiteral("worker")),
        "worker callback output does not mutate the snapshot before GUI events");

    pump_events(app);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    ok &= check(activity_count == 1, "worker callback drains after GUI events");
    ok &= check(activity_on_gui_thread, "worker callback notification emits on GUI thread");
    ok &= check(snapshot != nullptr && snapshot_contains_text(*snapshot, QStringLiteral("worker")),
        "worker callback output reaches render snapshot");

    return ok;
}

bool test_heap_surface_destroy_closes_queued_worker_callback(QGuiApplication& app)
{
    bool ok = true;

    auto window = std::make_unique<QQuickWindow>();
    window->resize(640, 320);

    auto surface = std::make_unique<VNM_TerminalSurface>();
    QPointer<VNM_TerminalSurface> surface_guard(surface.get());
    surface->setParentItem(window->contentItem());
    surface->setSize(QSizeF(520.0, 240.0));
    surface->set_font_family(QStringLiteral("monospace"));
    surface->set_font_size(12.0);
    std::shared_ptr<term::Terminal_renderer_lifecycle_recorder> lifecycle_recorder =
        term::VNM_TerminalSurface_render_bridge::lifecycle_recorder(*surface);
    window->show();
    pump_events(app);

    int output_activity_count = 0;
    int exited_count          = 0;
    int backend_error_count   = 0;
    QObject::connect(
        surface.get(),
        &VNM_TerminalSurface::output_activity,
        &app,
        [&] {
            ++output_activity_count;
        });
    QObject::connect(
        surface.get(),
        &VNM_TerminalSurface::process_exited,
        &app,
        [&](VNM_TerminalSurface::Exit_reason, int) {
            ++exited_count;
        });
    QObject::connect(
        surface.get(),
        &VNM_TerminalSurface::backend_error,
        &app,
        [&](VNM_TerminalSurface::Backend_error_code, const QString&) {
            ++backend_error_count;
        });

    auto state   = std::make_shared<Scripted_backend_lifecycle_state>();
    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("heap-lifecycle-output")};
    backend->lifecycle_state      = state;
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        *surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "heap-destroy lifecycle surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(*surface);
    ok &= check(snapshot != nullptr &&
        snapshot_contains_text(*snapshot, QStringLiteral("heap-lifecycle")),
        "heap-destroy lifecycle has a live session snapshot before destruction");
    if (snapshot != nullptr) {
        ok &= check(render_surface_sequence(
            app,
            *window,
            *surface,
            snapshot->metadata.sequence),
            "heap-destroy lifecycle renders session resources before destruction");
    }
    ok &= check(pump_until(app, [&] {
        return has_live_render_tree(lifecycle_recorder->snapshot());
    }),
        "heap-destroy lifecycle starts with live QSG render resources");
    ok &= check(!term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(*surface),
        "heap-destroy lifecycle starts worker callback check with no queued GUI drain");

    const int pre_worker_output_activity_count = output_activity_count;
    backend_ptr->emit_output_from_worker(QByteArrayLiteral("queued-before-delete"));
    backend_ptr->join_worker();
    ok &= check(state->worker_callback_attempts.load() == 1 &&
        state->worker_callback_completions.load() == 1,
        "heap-destroy lifecycle queues one worker callback before deletion");
    ok &= check(term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(*surface),
        "heap-destroy lifecycle has a queued GUI drain callback before deletion");
    ok &= check(output_activity_count == pre_worker_output_activity_count,
        "heap-destroy lifecycle does not drain worker output before deletion");

    const int pre_destroy_signal_count =
        output_activity_count + exited_count + backend_error_count;
    surface.reset();
    ok &= check(surface_guard.isNull(), "heap-destroy lifecycle deletes the surface object");
    ok &= check(state->terminate_count.load() == 1,
        "heap-destroy lifecycle terminates the running backend during destruction");
    ok &= check(state->destructed_count.load() == 1,
        "heap-destroy lifecycle destroys the backend during surface destruction");

    ok &= check(pump_until(app, [&] {
        return has_no_live_render_resources(lifecycle_recorder->snapshot());
    }),
        "heap-destroy lifecycle releases QSG resources after surface destruction");
    ok &= check(state->terminate_count.load() == 1 &&
        state->destructed_count.load() == 1,
        "heap-destroy lifecycle cleanup is complete before post-delete event pumping");
    ok &= check(output_activity_count + exited_count + backend_error_count ==
        pre_destroy_signal_count,
        "heap-destroy lifecycle emits no public signal during or after surface deletion");

    return ok;
}

bool test_window_destroy_keeps_surface_session_until_surface_destroy(QGuiApplication& app)
{
    bool ok = true;

    auto window = std::make_unique<QQuickWindow>();
    window->resize(640, 320);

    VNM_TerminalSurface* surface = new VNM_TerminalSurface();
    QPointer<VNM_TerminalSurface> surface_guard(surface);
    surface->setParentItem(window->contentItem());
    surface->setSize(QSizeF(520.0, 240.0));
    surface->set_font_family(QStringLiteral("monospace"));
    surface->set_font_size(12.0);

    int output_activity_count = 0;
    QObject::connect(
        surface,
        &VNM_TerminalSurface::output_activity,
        surface,
        [&] {
            ++output_activity_count;
        });

    std::shared_ptr<term::Terminal_renderer_lifecycle_recorder> lifecycle_recorder =
        term::VNM_TerminalSurface_render_bridge::lifecycle_recorder(*surface);
    auto state   = std::make_shared<Scripted_backend_lifecycle_state>();
    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {QByteArrayLiteral("window-lifecycle-output")};
    backend->lifecycle_state      = state;
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        *surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "window-destroy lifecycle surface starts");

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(*surface);
    ok &= check(snapshot != nullptr &&
        snapshot_contains_text(*snapshot, QStringLiteral("window-lifecycle")),
        "window-destroy lifecycle has a live session snapshot before rendering");

    window->show();
    if (snapshot != nullptr) {
        ok &= check(render_surface_sequence(
            app,
            *window,
            *surface,
            snapshot->metadata.sequence),
            "window-destroy lifecycle renders session resources before window destruction");
    }

    const term::terminal_renderer_lifecycle_stats_t setup_stats =
        lifecycle_recorder->snapshot();
    ok &= check(has_live_render_tree(setup_stats),
        "window-destroy lifecycle starts with live QSG render resources");

    window.reset();
    ok &= check(pump_until(app, [&] {
        return surface_guard.isNull() || surface_guard->window() == nullptr;
    }),
        "window-destroy lifecycle reaches a post-window-destruction state");
    ok &= check(!surface_guard.isNull(),
        "window-destroy lifecycle keeps the surface alive after window destruction");
    if (surface_guard.isNull()) {
        return ok;
    }
    ok &= check(surface_guard->window() == nullptr,
        "window-destroy lifecycle detaches the surface from the destroyed window");

    ok &= check(pump_until(app, [&] {
        return has_no_live_render_resources(lifecycle_recorder->snapshot());
    }),
        "window-destroy lifecycle releases QSG resources after window destruction");
    const term::terminal_renderer_lifecycle_stats_t window_destroy_stats =
        lifecycle_recorder->snapshot();
    const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(*surface_guard.data());
    ok &= check(window_destroy_stats.item_scene_detaches >
        setup_stats.item_scene_detaches,
        "window-destroy lifecycle records the item scene detach");
    ok &= check(!invalidation_stats.pending_update,
        "window-destroy lifecycle clears pending render invalidation state");
    ok &= check(has_no_live_render_resources(window_destroy_stats),
        "window-destroy lifecycle leaves no live QSG resources after convergence");

    ok &= check(state->destructed_count.load() == 0,
        "window-destroy lifecycle keeps the backend owned while the surface lives");
    ok &= check(surface_guard->process_state() == VNM_TerminalSurface::Process_state::RUNNING &&
        surface_guard->backend_ready(),
        "window-destroy lifecycle keeps the session active until surface destruction");

    const int post_detach_activity_count = output_activity_count;
    backend_ptr->emit_output(QByteArrayLiteral("after-window-destroy"));
    pump_events(app);
    const std::shared_ptr<const term::Terminal_render_snapshot> post_detach_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(*surface_guard.data());
    ok &= check(output_activity_count == post_detach_activity_count + 1,
        "window-destroy lifecycle still emits output activity after window destruction");
    ok &= check(post_detach_snapshot != nullptr &&
        snapshot_contains_text(*post_detach_snapshot, QStringLiteral("after-window-destroy")),
        "window-destroy lifecycle still updates the session snapshot after window destruction");

    delete surface;
    surface  = nullptr;
    ok      &= check(surface_guard.isNull(), "window-destroy lifecycle deletes the surface explicitly");
    ok      &= check(state->terminate_count.load() == 1,
        "window-destroy lifecycle terminates the backend when the surface is destroyed");
    ok      &= check(state->destructed_count.load() == 1,
        "window-destroy lifecycle destroys the backend with the surface");

    return ok;
}

bool test_geometry_change_resizes_session(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "geometry-resize surface starts");
    ok &= check(backend_ptr->resize_requests.empty(),
        "start does not issue a separate resize request");

    const std::size_t initial_resize_count = backend_ptr->resize_requests.size();
    const QSizeF candidates[] = {
        QSizeF(800.0, 360.0),
        QSizeF(900.0, 420.0),
        QSizeF(360.0, 180.0),
    };
    for (QSizeF size : candidates) {
        fixture.surface.setSize(size);
        pump_events(app, 2);
        if (backend_ptr->resize_requests.size() > initial_resize_count) {
            break;
        }
    }

    ok &= check(backend_ptr->resize_requests.size() > initial_resize_count,
        "geometry change sends a resize to the backend");
    if (backend_ptr->resize_requests.size() > initial_resize_count) {
        const term::terminal_grid_size_t grid_size =
            backend_ptr->resize_requests.back().grid_size;
        ok &= check(grid_size.rows == fixture.surface.rows() &&
            grid_size.columns == fixture.surface.columns(),
            "surface rows and columns mirror resized session grid");
        ok &= check(term::is_valid_grid_size(grid_size),
            "geometry resize sends a valid backend grid");
    }

    return ok;
}

bool test_backend_exit_updates_surface(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int exited_count = 0;
    VNM_TerminalSurface::Exit_reason exit_reason =
        VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
    int exit_code = -1;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_exited,
        &fixture.surface,
        [&](VNM_TerminalSurface::Exit_reason reason, int code) {
            ++exited_count;
            exit_reason = reason;
            exit_code = code;
        });

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "exit surface starts");

    backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 7});
    pump_events(app);

    ok &= check(fixture.surface.process_state() == VNM_TerminalSurface::Process_state::EXITED,
        "backend exit updates surface process state");
    ok &= check(!fixture.surface.backend_ready(),
        "backend exit clears surface backend readiness");
    ok &= check(exited_count == 1 &&
        exit_reason == VNM_TerminalSurface::Exit_reason::EXITED &&
        exit_code == 7,
        "backend exit emits public process_exited signal");

    return ok;
}

bool test_public_lifecycle_methods(QGuiApplication& app)
{
    bool ok = true;

    {
        Surface_fixture fixture;
        pump_events(app);

        std::vector<VNM_TerminalSurface::Backend_error_code> error_codes;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                error_codes.push_back(code);
            });

        ok &= check(!fixture.surface.interrupt_process(),
            "no-session interrupt returns false");
        ok &= check(!fixture.surface.terminate_process(),
            "no-session terminate returns false");
        ok &= check(error_codes.size() == 2U &&
            error_codes[0] ==
                VNM_TerminalSurface::Backend_error_code::INTERRUPT_FAILED &&
            error_codes[1] ==
                VNM_TerminalSurface::Backend_error_code::TERMINATE_FAILED,
            "no-session lifecycle calls emit typed public backend errors");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        int exited_count = 0;
        VNM_TerminalSurface::Exit_reason exit_reason =
            VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
        int exit_code = -1;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::process_exited,
            &fixture.surface,
            [&](VNM_TerminalSurface::Exit_reason reason, int code) {
                ++exited_count;
                exit_reason = reason;
                exit_code = code;
            });

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);

        ok &= check(started, "interrupt lifecycle surface starts");
        ok &= check(fixture.surface.interrupt_process(),
            "public interrupt succeeds through scripted backend");
        pump_events(app);
        ok &= check(!backend_ptr->running,
            "public interrupt stops scripted backend");
        ok &= check(fixture.surface.process_state() ==
            VNM_TerminalSurface::Process_state::EXITED &&
            !fixture.surface.backend_ready(),
            "public interrupt syncs exited surface properties");
        ok &= check(exited_count == 1 &&
            exit_reason == VNM_TerminalSurface::Exit_reason::INTERRUPTED &&
            exit_code == 130,
            "public interrupt emits process_exited signal");
    }

    {
        Surface_fixture fixture;
        pump_events(app);

        int exited_count = 0;
        VNM_TerminalSurface::Exit_reason exit_reason =
            VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
        int exit_code = -1;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::process_exited,
            &fixture.surface,
            [&](VNM_TerminalSurface::Exit_reason reason, int code) {
                ++exited_count;
                exit_reason = reason;
                exit_code = code;
            });

        auto backend = std::make_unique<Scripted_backend>();
        bool started = false;
        Scripted_backend* backend_ptr = start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            { QStringLiteral("scripted-terminal") },
            &started);

        ok &= check(started, "terminate lifecycle surface starts");
        ok &= check(fixture.surface.terminate_process(),
            "public terminate succeeds through scripted backend");
        pump_events(app);
        ok &= check(!backend_ptr->running,
            "public terminate stops scripted backend");
        ok &= check(fixture.surface.process_state() ==
            VNM_TerminalSurface::Process_state::EXITED &&
            !fixture.surface.backend_ready(),
            "public terminate syncs exited surface properties");
        ok &= check(exited_count == 1 &&
            exit_reason == VNM_TerminalSurface::Exit_reason::TERMINATED &&
            exit_code == 0,
            "public terminate emits process_exited signal");
    }

    return ok;
}

bool test_terminal_icon_name_notifications(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int title_changed_count     = 0;
    int icon_name_changed_count = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::terminal_title_changed,
        &fixture.surface,
        [&] {
            ++title_changed_count;
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::terminal_icon_name_changed,
        &fixture.surface,
        [&] {
            ++icon_name_changed_count;
        });

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "terminal-icon-name surface starts");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b]1;icon-only\a"));
    pump_events(app);
    ok &= check(icon_name_changed_count == 1,
        "OSC 1 emits surface icon name notification");
    ok &= check(title_changed_count == 0,
        "OSC 1 does not emit surface title notification");
    ok &= check(fixture.surface.terminal_icon_name() == QStringLiteral("icon-only"),
        "OSC 1 updates surface terminal icon name");
    ok &= check(fixture.surface.terminal_title().isEmpty(),
        "OSC 1 preserves empty surface terminal title");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b]0;combined\a"));
    pump_events(app);
    ok &= check(icon_name_changed_count == 2,
        "OSC 0 emits surface icon name notification");
    ok &= check(title_changed_count == 1,
        "OSC 0 emits surface title notification");
    ok &= check(fixture.surface.terminal_icon_name() == QStringLiteral("combined"),
        "OSC 0 updates surface terminal icon name");
    ok &= check(fixture.surface.terminal_title() == QStringLiteral("combined"),
        "OSC 0 updates surface terminal title");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b]2;title-only\a"));
    pump_events(app);
    ok &= check(icon_name_changed_count == 2,
        "OSC 2 does not emit surface icon name notification");
    ok &= check(title_changed_count == 2,
        "OSC 2 emits surface title notification");
    ok &= check(fixture.surface.terminal_icon_name() == QStringLiteral("combined"),
        "OSC 2 preserves surface terminal icon name");
    ok &= check(fixture.surface.terminal_title() == QStringLiteral("title-only"),
        "OSC 2 updates surface terminal title");

    return ok;
}

bool test_notification_burst_uses_durable_channel(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int title_changed_count = 0;
    int exited_count        = 0;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::terminal_title_changed,
        &fixture.surface,
        [&] {
            ++title_changed_count;
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_exited,
        &fixture.surface,
        [&](VNM_TerminalSurface::Exit_reason, int) {
            ++exited_count;
        });

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "notification-burst surface starts");

    constexpr int k_title_notification_count = 1105;
    QByteArray burst;
    for (int i = 0; i < k_title_notification_count; ++i) {
        burst.append(QByteArrayLiteral("\x1b]2;"));
        burst.append(QStringLiteral("burst-title-%1").arg(i).toUtf8());
        burst.append('\a');
    }

    backend_ptr->emit_output(std::move(burst));
    backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 5});
    pump_events(app);

    ok &= check(title_changed_count == 1,
        "durable notification drain coalesces title burst for GUI delivery");
    ok &= check(fixture.surface.terminal_title() ==
        QStringLiteral("burst-title-%1").arg(k_title_notification_count - 1),
        "title burst leaves the final terminal title visible");
    ok &= check(exited_count == 1,
        "critical process_exited notification is delivered after a notification burst");

    return ok;
}

bool test_surface_overflow_reports_error_and_exit(QGuiApplication& app)
{
    bool ok = true;
    Surface_fixture fixture;
    pump_events(app);

    int overflow_error_count = 0;
    int exited_count         = 0;

    VNM_TerminalSurface::Exit_reason exit_reason =
        VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::backend_error,
        &fixture.surface,
        [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
            if (code == VNM_TerminalSurface::Backend_error_code::OUTPUT_OVERFLOW) {
                ++overflow_error_count;
            }
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_exited,
        &fixture.surface,
        [&](VNM_TerminalSurface::Exit_reason reason, int) {
            ++exited_count;
            exit_reason = reason;
        });

    auto backend = std::make_unique<Scripted_backend>();
    bool started = false;
    Scripted_backend* backend_ptr = start_surface_with_backend(
        fixture.surface,
        std::move(backend),
        { QStringLiteral("scripted-terminal") },
        &started);
    ok &= check(started, "surface overflow test starts");

    backend_ptr->emit_output(QByteArray(300000, 'x'));
    pump_events(app);

    ok &= check(overflow_error_count == 1,
        "surface overflow emits one public overflow backend_error");
    ok &= check(exited_count == 1 &&
        exit_reason == VNM_TerminalSurface::Exit_reason::TERMINATED,
        "surface overflow termination emits public process_exited");
    ok &= check(fixture.surface.process_state() == VNM_TerminalSurface::Process_state::EXITED,
        "surface overflow reaches exited process state");
    ok &= check(!fixture.surface.backend_ready(),
        "surface overflow clears backend readiness");

    return ok;
}

bool test_invalid_argv_reports_backend_error(QGuiApplication& app)
{
    bool ok = true;

    const std::vector<QStringList> invalid_argv_cases = {
        QStringList{},
        QStringList{QStringLiteral("   ")},
    };
    for (const QStringList& argv : invalid_argv_cases) {
        {
            Surface_fixture fixture;
            pump_events(app);

            int error_count = 0;
            std::optional<VNM_TerminalSurface::Backend_error_code> error_code;
            QObject::connect(
                &fixture.surface,
                &VNM_TerminalSurface::backend_error,
                &fixture.surface,
                [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                    ++error_count;
                    error_code = code;
                });

            ok &= check(!fixture.surface.start_process(argv),
                "public invalid argv start returns false");
            ok &= check(error_count == 1 &&
                error_code ==
                    VNM_TerminalSurface::Backend_error_code::INVALID_LAUNCH_CONFIG,
                "public invalid argv emits typed backend_error");
            ok &= check(fixture.surface.process_state() ==
                VNM_TerminalSurface::Process_state::FAILED,
                "public invalid argv marks surface process state failed");
        }

        Surface_fixture fixture;
        pump_events(app);

        int error_count = 0;
        std::optional<VNM_TerminalSurface::Backend_error_code> error_code;
        QObject::connect(
            &fixture.surface,
            &VNM_TerminalSurface::backend_error,
            &fixture.surface,
            [&](VNM_TerminalSurface::Backend_error_code code, const QString&) {
                ++error_count;
                error_code = code;
            });

        std::vector<term::Terminal_launch_config> observed_start_configs;
        int  observed_start_attempts = 0;
        auto backend                 = std::make_unique<Scripted_backend>();
        backend->start_config_observer  = &observed_start_configs;
        backend->start_attempt_observer = &observed_start_attempts;
        bool started = false;
        (void)start_surface_with_backend(
            fixture.surface,
            std::move(backend),
            argv,
            &started);

        ok &= check(!started, "invalid argv start returns false");
        ok &= check(observed_start_attempts == 0,
            "invalid argv does not attempt backend start");
        ok &= check(observed_start_configs.empty(),
            "invalid argv does not invoke backend start");
        ok &= check(error_count == 1 &&
            error_code ==
                VNM_TerminalSurface::Backend_error_code::INVALID_LAUNCH_CONFIG,
            "invalid argv emits public backend_error");
        ok &= check(fixture.surface.process_state() ==
            VNM_TerminalSurface::Process_state::FAILED,
            "invalid argv marks surface process state failed");
    }

    return ok;
}

}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    bool ok = true;
    ok &= test_start_maps_output_to_snapshot(app);
    ok &= test_surface_session_snapshot_burst_coalesces_to_latest_render(app);
    ok &= test_surface_session_single_drain_coalesces_dirty_rows(app);
    ok &= test_osc52_clipboard_write_signal_and_deny(app);
    ok &= test_osc52_clipboard_wrong_duplicate_and_replacement(app);
    ok &= test_osc52_clipboard_late_exit_restart_and_targets(app);
    ok &= test_osc52_clipboard_allow_writes_clipboard(app);
    ok &= test_keyboard_printable_controls_and_prompt_path(app);
    ok &= test_copy_shortcut_policy(app);
    ok &= test_control_wheel_font_zoom(app);
    ok &= test_plain_wheel_scrolls_primary_scrollback(app);
    ok &= test_public_viewport_scroll_api(app);
    ok &= test_plain_wheel_scrolls_scroll_region_primary_scrollback(app);
    ok &= test_plain_wheel_scrolls_csi_scroll_up_primary_scrollback(app);
    ok &= test_page_keys_scroll_primary_scrollback(app);
    ok &= test_page_keys_fall_through_on_alternate_screen(app);
    ok &= test_plain_wheel_boundaries_and_alternate_input(app);
    ok &= test_mouse_reporting_surface_events(app);
    ok &= test_selection_drag_and_selected_text(app);
    ok &= test_stale_synchronized_output_recovery(app);
    ok &= test_paste_text_public_method_and_policy(app);
    ok &= test_right_click_paste_and_mouse_reporting_precedence(app);
    ok &= test_focus_reporting_writes_mode_bytes_and_preserves_ime_cancel(app);
    ok &= test_ime_preedit_updates_overlay_without_snapshot_churn(app);
    ok &= test_ime_commit_writes_utf8_and_clears_preedit(app);
    ok &= test_ime_no_session_overlay_and_later_start_clear(app);
    ok &= test_ime_startup_commit_clears_preedit_without_backend_error(app);
    ok &= test_ime_multi_codepoint_commit_writes_utf8(app);
    ok &= test_ime_combined_commit_and_preedit(app);
    ok &= test_ime_replacement_range_is_stream_commit(app);
    ok &= test_ime_commit_failure_preserves_preedit(app);
    ok &= test_ime_synchronized_output_overlay(app);
    ok &= test_ime_focus_loss_cancels_preedit(app);
    ok &= test_ime_cursor_rectangle_query_tracks_snapshot_cursor(app);
    ok &= test_keyboard_cursor_modes(app);
    ok &= test_keyboard_navigation_keys(app);
    ok &= test_keyboard_function_keys(app);
    ok &= test_keyboard_keypad_modes(app);
    ok &= test_keyboard_focus_routed_delivery(app);
    ok &= test_keyboard_no_session_and_post_exit_semantics(app);
    ok &= test_keyboard_unhandled_key_skips_backend_drain(app);
    ok &= test_worker_callback_drains_on_gui_thread(app);
    ok &= test_heap_surface_destroy_closes_queued_worker_callback(app);
    ok &= test_window_destroy_keeps_surface_session_until_surface_destroy(app);
    ok &= test_geometry_change_resizes_session(app);
    ok &= test_backend_exit_updates_surface(app);
    ok &= test_public_lifecycle_methods(app);
    ok &= test_terminal_icon_name_notifications(app);
    ok &= test_notification_burst_uses_durable_channel(app);
    ok &= test_surface_overflow_reports_error_and_exit(app);
    ok &= test_invalid_argv_reports_backend_error(app);
    return ok ? 0 : 1;
}
