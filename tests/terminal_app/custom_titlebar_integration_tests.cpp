#define VNM_TERMINAL_APP_NO_MAIN
#include "../../src/main.cpp"
#undef VNM_TERMINAL_APP_NO_MAIN

#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/internal/terminal_transcript.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPointF>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QWheelEvent>

#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace chrome_test = vnm_terminal::terminal_app;
namespace term = vnm_terminal::internal;

namespace {

QString scalar_text(char32_t codepoint)
{
    return QString::fromUcs4(&codepoint, 1);
}

using vnm_terminal::test_helpers::check;

bool nearly_equal(qreal actual, qreal expected)
{
    return std::abs(actual - expected) <= 0.000001;
}

bool check_rect_equal(
    const QRectF&      actual,
    const QRectF&      expected,
    const std::string& message)
{
    if (nearly_equal(actual.x(),      expected.x())     &&
        nearly_equal(actual.y(),      expected.y())     &&
        nearly_equal(actual.width(),  expected.width()) &&
        nearly_equal(actual.height(), expected.height()))
    {
        return true;
    }

    std::cerr << "FAIL: " << message
        << " expected=(" << expected.x() << ", " << expected.y()
        << ", " << expected.width() << ", " << expected.height()
        << ") actual=(" << actual.x() << ", " << actual.y()
        << ", " << actual.width() << ", " << actual.height() << ")\n";
    return false;
}

void pump_events(QGuiApplication& app)
{
    for (int index = 0; index < 5; ++index) {
        app.processEvents(QEventLoop::AllEvents, 20);
    }
}

QRectF item_rect(const QQuickItem& item)
{
    return QRectF(item.x(), item.y(), item.width(), item.height());
}

bool send_item_wheel_event(
    QQuickItem&            item,
    Qt::KeyboardModifiers  modifiers,
    int                    pixel_delta_y,
    int                    angle_delta_y,
    bool                   expected_accepted,
    const std::string&     message)
{
    const QPointF point(item.width() / 2.0, item.height() / 2.0);
    QWheelEvent event(
        point,
        point,
        QPoint(0, pixel_delta_y),
        QPoint(0, angle_delta_y),
        Qt::NoButton,
        modifiers,
        Qt::NoScrollPhase,
        false);
    event.ignore();
    QCoreApplication::sendEvent(&item, &event);
    return check(event.isAccepted() == expected_accepted, message);
}

bool send_item_mouse_event(
    QQuickItem&            item,
    QEvent::Type           type,
    const QPointF&         point,
    Qt::MouseButton        button,
    Qt::MouseButtons       buttons,
    Qt::KeyboardModifiers  modifiers,
    bool                   expected_accepted,
    const std::string&     message)
{
    QMouseEvent event(
        type,
        point,
        point,
        point,
        button,
        buttons,
        modifiers);
    event.ignore();
    QCoreApplication::sendEvent(&item, &event);
    return check(event.isAccepted() == expected_accepted, message);
}

bool send_item_mouse_event(
    QQuickItem&            item,
    QEvent::Type           type,
    const QPointF&         point,
    Qt::MouseButton        button,
    Qt::MouseButtons       buttons,
    bool                   expected_accepted,
    const std::string&     message)
{
    return send_item_mouse_event(
        item,
        type,
        point,
        button,
        buttons,
        Qt::NoModifier,
        expected_accepted,
        message);
}

bool transcript_has_source_event(
    const std::vector<term::Terminal_transcript_event>& events,
    const QString&                                      kind,
    const QString&                                      source)
{
    for (const term::Terminal_transcript_event& event : events) {
        if (event.kind == kind &&
            event.object.value(QStringLiteral("source")).toString() == source)
        {
            return true;
        }
    }

    return false;
}

bool transcript_has_deferred_app_wheel_trace(
    const std::vector<term::Terminal_transcript_event>& events)
{
    for (const term::Terminal_transcript_event& event : events) {
        if (event.kind != QStringLiteral("surface.wheel_trace")) {
            continue;
        }

        const QJsonObject& object = event.object;
        if (object.value(QStringLiteral("source")).toString() ==
                QStringLiteral("app.scrollbar.wheel") &&
            object.value(QStringLiteral("outcome")).toString() ==
                QStringLiteral("deferred_intent_recorded") &&
            object.value(QStringLiteral("deferred_intent_recorded")).toBool() &&
            !object.value(QStringLiteral("local_scroll_applied")).toBool(true) &&
            !object.value(QStringLiteral("visible_scroll_applied")).toBool(true))
        {
            return true;
        }
    }

    return false;
}

bool transcript_has_deferred_scroll_event(
    const std::vector<term::Terminal_transcript_event>& events,
    const QString&                                      source)
{
    for (const term::Terminal_transcript_event& event : events) {
        if (event.kind != QStringLiteral("surface.scroll")) {
            continue;
        }

        const QJsonObject& object = event.object;
        if (object.value(QStringLiteral("source")).toString() == source &&
            object.value(QStringLiteral("action")).toString() ==
                QStringLiteral("deferred_intent_recorded"))
        {
            return true;
        }
    }

    return false;
}

bool rect_geometry_changed(const QRectF& before, const QRectF& after)
{
    return
        !nearly_equal(before.x(),      after.x())     ||
        !nearly_equal(before.y(),      after.y())     ||
        !nearly_equal(before.width(),  after.width()) ||
        !nearly_equal(before.height(), after.height());
}

class Recording_event_filter final : public QObject
{
public:
    explicit Recording_event_filter(QEvent::Type recorded_type)
    :
        m_recorded_type(recorded_type)
    {}

    int            recorded_count = 0;

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event != nullptr && event->type() == m_recorded_type) {
            ++recorded_count;
        }

        return false;
    }

private:
    QEvent::Type   m_recorded_type;
};

class Metadata_seed_backend final : public term::Terminal_backend
{
public:
    explicit Metadata_seed_backend(QByteArray startup_output)
    :
        m_startup_output(std::move(startup_output))
    {}

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
        m_running = true;
        if (!m_startup_output.isEmpty()) {
            m_callbacks.output_received(m_startup_output);
        }
        return term::backend_accept();
    }

    term::Terminal_backend_result write(QByteArray) override
    {
        return term::backend_accept();
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request request) override
    {
        if (!term::is_valid_grid_size(request.grid_size)) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::RESIZE_FAILED,
                    QStringLiteral("metadata seed backend requires positive grid"));
        }
        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool) override
    {
        return term::backend_accept();
    }

    term::Terminal_backend_result interrupt() override
    {
        return finish(term::Terminal_exit_reason::INTERRUPTED, 130);
    }

    term::Terminal_backend_result terminate() override
    {
        return finish(term::Terminal_exit_reason::TERMINATED, 0);
    }

    void emit_output(QByteArray bytes)
    {
        if (m_running) {
            m_callbacks.output_received(std::move(bytes));
        }
    }

private:
    term::Terminal_backend_result finish(term::Terminal_exit_reason reason, int exit_code)
    {
        if (!m_running) {
            return term::backend_accept();
        }

        m_running = false;
        m_callbacks.process_exited({reason, exit_code});
        return term::backend_accept();
    }

    QByteArray                       m_startup_output;
    term::Terminal_backend_callbacks m_callbacks;
    bool                             m_running = false;
};

QByteArray osc1_icon_name_sequence(const QString& icon_name)
{
    QByteArray bytes("\x1b]1;", 4);
    bytes += icon_name.toUtf8();
    bytes += '\a';
    return bytes;
}

QByteArray numbered_scroll_lines(int count)
{
    QByteArray bytes;
    for (int i = 0; i < count; ++i) {
        bytes += QByteArrayLiteral("row-");
        bytes += QByteArray::number(i);
        bytes += QByteArrayLiteral("\r\n");
    }
    return bytes;
}

bool test_custom_titlebar_geometry()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(800, 480);

    chrome_test::Terminal_qml_chrome titlebar(engine, window);
    VNM_TerminalSurface surface(window.contentItem());
    chrome_test::Terminal_scrollbar scrollbar(window.contentItem());

    bool ok = true;
    ok &= check(titlebar.is_valid(), "shared QML titlebar initializes");
    if (!titlebar.is_valid()) {
        std::cerr << titlebar.error_string().toStdString() << '\n';
        return ok;
    }

    apply_terminal_shell_geometry(window, surface, scrollbar, &titlebar, true);

    ok &= check_rect_equal(item_rect(*titlebar.root_item()), QRectF(0.0, 0.0, 800.0, 480.0),
        "shared chrome root covers the window");
    ok &= check_rect_equal(item_rect(*titlebar.titlebar_item()), QRectF(0.0, 0.0, 800.0, 32.0),
        "shared titlebar occupies the top band");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_x").toReal(), 6.0),
        "shared chrome records content border x");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_y").toReal(), 32.0),
        "shared chrome records content border y");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_width").toReal(), 788.0),
        "shared chrome records content border width");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_height").toReal(), 442.0),
        "shared chrome records content border height");
    ok &= check_rect_equal(item_rect(surface), QRectF(7.0, 33.0, 774.0, 440.0),
        "custom terminal is inset inside the content border");
    ok &= check_rect_equal(item_rect(scrollbar), QRectF(781.0, 33.0, 12.0, 440.0),
        "custom scrollbar touches the inner right frame edge");
    ok &= check(surface.y() >= titlebar.titlebar_item()->y() + titlebar.titlebar_item()->height(),
        "terminal top is below titlebar bottom");
    ok &= check(surface.x() >= chrome_test::k_default_frameless_resize_border_width,
        "terminal left edge is inside resize border");
    ok &= check(nearly_equal(surface.x() + surface.width(), scrollbar.x()),
        "terminal right edge is adjacent to scrollbar");
    ok &= check(
        scrollbar.x() + scrollbar.width() <=
            window.width() - chrome_test::k_default_frameless_resize_border_width,
        "scrollbar right edge is inside resize border");
    ok &= check(
        surface.y() + surface.height() <=
            window.height() - chrome_test::k_default_frameless_resize_border_width,
        "terminal bottom edge is inside resize border");

    window.resize(360, 240);
    apply_terminal_shell_geometry(window, surface, scrollbar, &titlebar, true);
    ok &= check_rect_equal(item_rect(*titlebar.root_item()), QRectF(0.0, 0.0, 360.0, 240.0),
        "resized shared chrome root tracks window size");
    ok &= check_rect_equal(item_rect(*titlebar.titlebar_item()), QRectF(0.0, 0.0, 360.0, 32.0),
        "resized custom titlebar tracks window width");
    ok &= check_rect_equal(item_rect(surface), QRectF(7.0, 33.0, 334.0, 200.0),
        "resized custom terminal tracks titlebar and border insets");
    ok &= check_rect_equal(item_rect(scrollbar), QRectF(341.0, 33.0, 12.0, 200.0),
        "resized custom scrollbar remains inside the right frame");

    window.setWindowStates(Qt::WindowMaximized);
    apply_terminal_shell_geometry(window, surface, scrollbar, &titlebar, true);
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_x").toReal(), 0.0),
        "maximized shared chrome drops inactive resize gutter x");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_width").toReal(), 360.0),
        "maximized shared chrome drops inactive resize gutter width");
    ok &= check_rect_equal(item_rect(surface), QRectF(1.0, 33.0, 346.0, 206.0),
        "maximized custom terminal drops inactive resize border gutters");
    ok &= check_rect_equal(item_rect(scrollbar), QRectF(347.0, 33.0, 12.0, 206.0),
        "maximized custom scrollbar remains inside content bounds");
    window.setWindowStates(Qt::WindowNoState);

    window.resize(8, 40);
    apply_terminal_shell_geometry(window, surface, scrollbar, &titlebar, true);
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_x").toReal(), 4.0),
        "very narrow shared chrome clamps horizontal resize insets");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_width").toReal(), 0.0),
        "very narrow shared chrome clamps content border width");
    ok &= check_rect_equal(item_rect(surface), QRectF(4.0, 33.0, 0.0, 0.0),
        "very narrow custom terminal clamps horizontal resize insets");
    ok &= check_rect_equal(item_rect(scrollbar), QRectF(4.0, 33.0, 0.0, 0.0),
        "very narrow custom scrollbar clamps inside horizontal resize insets");

    window.resize(200, 20);
    apply_terminal_shell_geometry(window, surface, scrollbar, &titlebar, true);
    ok &= check_rect_equal(item_rect(*titlebar.titlebar_item()), QRectF(0.0, 0.0, 200.0, 20.0),
        "very short custom titlebar clamps to window height");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_height").toReal(), 0.0),
        "very short shared chrome clamps content border height");
    ok &= check_rect_equal(item_rect(surface), QRectF(7.0, 20.0, 174.0, 0.0),
        "very short custom terminal clamps nonnegative height");
    ok &= check_rect_equal(item_rect(scrollbar), QRectF(181.0, 20.0, 12.0, 0.0),
        "very short custom scrollbar clamps nonnegative height");

    window.resize(360, 240);
    apply_terminal_shell_geometry(window, surface, scrollbar, nullptr, false);
    ok &= check_rect_equal(item_rect(surface), QRectF(0.0, 0.0, 348.0, 240.0),
        "native-decoration path reserves scrollbar gutter");
    ok &= check_rect_equal(item_rect(scrollbar), QRectF(348.0, 0.0, 12.0, 240.0),
        "native-decoration path positions scrollbar at right edge");

    return ok;
}

bool test_terminal_scrollbar_tracks_surface_viewport(QGuiApplication& app)
{
    QQuickWindow window;
    window.resize(360, 240);

    VNM_TerminalSurface surface(window.contentItem());
    chrome_test::Terminal_scrollbar scrollbar(window.contentItem());
    surface.setSize(QSizeF(348.0, 200.0));
    scrollbar.setSize(QSizeF(12.0, 200.0));
    scrollbar.set_surface(&surface);

    auto backend = std::make_unique<Metadata_seed_backend>(numbered_scroll_lines(80));
    const bool started = term::VNM_TerminalSurface_render_bridge::start_process_with_backend(
        surface,
        std::move(backend),
        {QStringLiteral("scrollbar-seed")});
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(surface);
    pump_events(app);

    bool ok = true;
    ok &= check(started, "scrollbar viewport seed backend starts");
    ok &= check(surface.scrollback_rows() > 0,
        "scrollbar test surface has scrollback rows");
    ok &= check(scrollbar.scrollbar_visible(),
        "scrollbar becomes visible when surface has scrollback");

    const QRectF tail_thumb = scrollbar.thumb_rect();
    ok &= check(!tail_thumb.isEmpty(),
        "scrollbar has a nonempty tail thumb");
    ok &= check(nearly_equal(tail_thumb.right(), scrollbar.width()),
        "scrollbar thumb is aligned to the right gutter edge");
    ok &= check(tail_thumb.bottom() <= scrollbar.height(),
        "scrollbar tail thumb stays inside item");

    ok &= check(surface.scroll_to_offset_from_tail(surface.scrollback_rows()),
        "scrollbar test surface scrolls to top");
    pump_events(app);
    const QRectF top_thumb = scrollbar.thumb_rect();
    ok &= check(!top_thumb.isEmpty(),
        "scrollbar has a nonempty top thumb");
    ok &= check(top_thumb.top() < tail_thumb.top(),
        "scrollbar thumb moves upward when viewport moves into scrollback");

    ok &= check(surface.scroll_to_offset_from_tail(0),
        "scrollbar test surface returns to tail");
    pump_events(app);
    const QRectF returned_thumb = scrollbar.thumb_rect();
    ok &= check(nearly_equal(returned_thumb.top(), tail_thumb.top()),
        "scrollbar thumb returns to the tail position");

    ok &= send_item_wheel_event(
        scrollbar,
        Qt::NoModifier,
        0,
        40,
        true,
        "scrollbar first high-resolution wheel fragment is consumed");
    ok &= check(surface.viewport_offset_from_tail() == 0,
        "scrollbar first high-resolution wheel fragment does not scroll");
    ok &= send_item_wheel_event(
        scrollbar,
        Qt::NoModifier,
        0,
        40,
        true,
        "scrollbar second high-resolution wheel fragment is consumed");
    ok &= check(surface.viewport_offset_from_tail() == 0,
        "scrollbar second high-resolution wheel fragment does not scroll");
    ok &= send_item_wheel_event(
        scrollbar,
        Qt::NoModifier,
        0,
        40,
        true,
        "scrollbar third high-resolution wheel fragment is consumed");
    ok &= check(surface.viewport_offset_from_tail() == 3,
        "scrollbar high-resolution wheel fragments accumulate to one step");

    ok &= check(surface.scroll_to_offset_from_tail(0),
        "scrollbar wheel test returns to tail");
    const qreal previous_font_size = surface.font_size();
    ok &= send_item_wheel_event(
        scrollbar,
        Qt::ControlModifier,
        0,
        120,
        true,
        "scrollbar Ctrl+wheel zoom is consumed");
    ok &= check(surface.font_size() > previous_font_size,
        "scrollbar Ctrl+wheel delegates to font zoom behavior");

    (void)surface.scroll_to_offset_from_tail(0);
    ok &= send_item_mouse_event(
        scrollbar,
        QEvent::MouseButtonPress,
        QPointF(scrollbar.width() / 2.0, 4.0),
        Qt::LeftButton,
        Qt::LeftButton,
        true,
        "scrollbar track press is accepted");
    ok &= check(surface.viewport_offset_from_tail() > 0,
        "scrollbar track press moves the viewport");
    ok &= send_item_mouse_event(
        scrollbar,
        QEvent::MouseButtonRelease,
        QPointF(scrollbar.width() / 2.0, 4.0),
        Qt::LeftButton,
        Qt::NoButton,
        false,
        "scrollbar track page release is ignored after non-drag press");
    ok &= check(surface.scroll_to_offset_from_tail(0),
        "scrollbar boundary wheel test returns to tail");
    ok &= send_item_wheel_event(
        scrollbar,
        Qt::NoModifier,
        0,
        40,
        true,
        "scrollbar boundary test stores a partial upward remainder");
    ok &= check(surface.viewport_offset_from_tail() == 0,
        "scrollbar boundary test partial upward remainder does not scroll");
    ok &= send_item_wheel_event(
        scrollbar,
        Qt::NoModifier,
        0,
        -40,
        true,
        "scrollbar boundary opposite wheel fragment is consumed");
    ok &= send_item_wheel_event(
        scrollbar,
        Qt::NoModifier,
        0,
        80,
        true,
        "scrollbar boundary opposite fragment cancels stale remainder");
    ok &= check(surface.viewport_offset_from_tail() == 0,
        "scrollbar boundary stale remainder does not accelerate next scroll");
    ok &= send_item_wheel_event(
        scrollbar,
        Qt::NoModifier,
        0,
        40,
        true,
        "scrollbar boundary remainder does not delay opposite scroll");
    ok &= check(surface.viewport_offset_from_tail() == 3,
        "scrollbar opposite scroll starts from a clean boundary remainder");

    return ok;
}

bool test_terminal_scrollbar_immediate_public_projection_routes(QGuiApplication& app)
{
    bool ok = true;

#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    QTemporaryDir transcript_dir;
    ok &= check(transcript_dir.isValid(), "scrollbar immediate transcript temp dir is valid");
    if (!transcript_dir.isValid()) {
        return ok;
    }
    const QString transcript_path =
        transcript_dir.filePath(QStringLiteral("scrollbar_immediate_routes.ndjson"));
#endif

    {
        QQuickWindow window;
        window.resize(360, 240);
        window.show();

        VNM_TerminalSurface surface(window.contentItem());
        chrome_test::Terminal_scrollbar scrollbar(window.contentItem());
        surface.setSize(QSizeF(348.0, 200.0));
        surface.set_scrollback_limit(200);
        surface.set_synchronized_output_scroll_policy(
            VNM_TerminalSurface::Synchronized_output_scroll_policy::
                IMMEDIATE_PUBLIC_PROJECTION);
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
        surface.set_transcript_capture_path(transcript_path);
        surface.set_wheel_trace_enabled(true);
        scrollbar.set_wheel_trace_enabled(true);
#endif
        scrollbar.setSize(QSizeF(12.0, 200.0));
        scrollbar.set_surface(&surface);

        auto backend = std::make_unique<Metadata_seed_backend>(numbered_scroll_lines(80));
        Metadata_seed_backend* backend_ptr = backend.get();
        const bool started = term::VNM_TerminalSurface_render_bridge::start_process_with_backend(
            surface,
            std::move(backend),
            {QStringLiteral("scrollbar-immediate-routes")});
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(surface);
        pump_events(app);

        ok &= check(started, "scrollbar immediate route backend starts");
        ok &= check(backend_ptr != nullptr, "scrollbar immediate route backend remains observable");
        if (!started || backend_ptr == nullptr) {
            return ok;
        }
        ok &= check(scrollbar.scrollbar_visible(),
            "scrollbar immediate route scrollbar is visible");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(surface);
        pump_events(app);

        ok &= send_item_wheel_event(
            scrollbar,
            Qt::NoModifier,
            0,
            120,
            true,
            "scrollbar immediate wheel route is accepted");
        ok &= check(surface.viewport_offset_from_tail() > 0,
            "scrollbar immediate wheel route moves public projection");
        const std::shared_ptr<const term::Terminal_render_snapshot> wheel_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(surface);
        ok &= check(wheel_snapshot != nullptr &&
            wheel_snapshot->basis == term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION &&
            wheel_snapshot->purpose == term::Terminal_render_snapshot_purpose::SCROLL,
            "scrollbar immediate wheel route publishes public projection scroll");

        const int page_offset_before = surface.viewport_offset_from_tail();
        const QRectF page_thumb_before = scrollbar.thumb_rect();
        ok &= send_item_mouse_event(
            scrollbar,
            QEvent::MouseButtonPress,
            QPointF(scrollbar.width() / 2.0, 4.0),
            Qt::LeftButton,
            Qt::LeftButton,
            true,
            "scrollbar immediate page route press is accepted");
        const int page_offset_after = surface.viewport_offset_from_tail();
        ok &= check(page_offset_after > page_offset_before,
            "scrollbar immediate page route increases viewport offset");
        ok &= check(rect_geometry_changed(page_thumb_before, scrollbar.thumb_rect()),
            "scrollbar immediate page route changes thumb geometry");

        ok &= send_item_mouse_event(
            scrollbar,
            QEvent::MouseButtonPress,
            QPointF(scrollbar.width() / 2.0, scrollbar.height() - 4.0),
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::ControlModifier,
            true,
            "scrollbar immediate Ctrl-track route press is accepted");
        ok &= check(surface.viewport_offset_from_tail() == 0,
            "scrollbar immediate Ctrl-track route jumps to tail");

        const QRectF thumb_before_drag = scrollbar.thumb_rect();
        ok &= check(!thumb_before_drag.isEmpty(),
            "scrollbar immediate thumb route has a thumb to drag");
        const int thumb_offset_before = surface.viewport_offset_from_tail();
        ok &= send_item_mouse_event(
            scrollbar,
            QEvent::MouseButtonPress,
            thumb_before_drag.center(),
            Qt::LeftButton,
            Qt::LeftButton,
            true,
            "scrollbar immediate thumb press is accepted");
        ok &= send_item_mouse_event(
            scrollbar,
            QEvent::MouseMove,
            thumb_before_drag.center() - QPointF(0.0, 40.0),
            Qt::NoButton,
            Qt::LeftButton,
            true,
            "scrollbar immediate thumb drag move is accepted");
        ok &= send_item_mouse_event(
            scrollbar,
            QEvent::MouseButtonRelease,
            thumb_before_drag.center() - QPointF(0.0, 40.0),
            Qt::LeftButton,
            Qt::NoButton,
            true,
            "scrollbar immediate thumb release is accepted");
        ok &= check(surface.viewport_offset_from_tail() != thumb_offset_before,
            "scrollbar immediate thumb route changes viewport offset");
        ok &= check(rect_geometry_changed(thumb_before_drag, scrollbar.thumb_rect()),
            "scrollbar immediate thumb route changes thumb geometry");

        ok &= check(surface.scroll_to_offset_from_tail(surface.scrollback_rows()),
            "scrollbar immediate invalidation setup moves frozen public state to top boundary");
        pump_events(app);

        const int held_scrollback_rows = surface.scrollback_rows();
        const int held_visible_rows = surface.viewport_visible_rows();
        const int held_offset = surface.viewport_offset_from_tail();
        const bool held_at_tail = surface.viewport_at_tail();
        const QRectF held_thumb = scrollbar.thumb_rect();
        ok &= check(held_offset == held_scrollback_rows,
            "scrollbar immediate invalidation setup freezes at top boundary");
        backend_ptr->emit_output(numbered_scroll_lines(5));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(surface);
        pump_events(app);
        ok &= check(surface.scrollback_rows() == held_scrollback_rows,
            "scrollbar immediate hidden growth freezes public scrollback range");
        ok &= check_rect_equal(
            scrollbar.thumb_rect(),
            held_thumb,
            "scrollbar immediate hidden growth freezes thumb geometry");

        term::VNM_TerminalSurface_render_bridge::invalidate_public_projection_for_testing(
            surface,
            term::Terminal_public_projection_disable_reason::PROJECTION_INVALIDATED);
        for (int i = 0; i < 3; ++i) {
            ok &= send_item_wheel_event(
                scrollbar,
                Qt::NoModifier,
                0,
                40,
                true,
                "scrollbar immediate invalidated high-resolution wheel is accepted");
        }
        ok &= check(surface.scrollback_rows() == held_scrollback_rows,
            "scrollbar immediate invalidated wheel keeps public range frozen");
        ok &= check(surface.viewport_visible_rows() == held_visible_rows,
            "scrollbar immediate invalidated wheel keeps public visible rows frozen");
        ok &= check(surface.viewport_offset_from_tail() == held_offset,
            "scrollbar immediate invalidated wheel keeps public offset frozen");
        ok &= check(surface.viewport_at_tail() == held_at_tail,
            "scrollbar immediate invalidated wheel keeps public at-tail flag frozen");
        ok &= check_rect_equal(
            scrollbar.thumb_rect(),
            held_thumb,
            "scrollbar immediate invalidated wheel keeps thumb geometry frozen");

        ok &= send_item_mouse_event(
            scrollbar,
            QEvent::MouseButtonPress,
            QPointF(scrollbar.width() / 2.0, scrollbar.height() - 4.0),
            Qt::LeftButton,
            Qt::LeftButton,
            true,
            "scrollbar immediate invalidated page route press is accepted");
        ok &= send_item_mouse_event(
            scrollbar,
            QEvent::MouseButtonPress,
            QPointF(scrollbar.width() / 2.0, scrollbar.height() - 4.0),
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::ControlModifier,
            true,
            "scrollbar immediate invalidated Ctrl-track route press is accepted");
        ok &= send_item_mouse_event(
            scrollbar,
            QEvent::MouseButtonPress,
            held_thumb.center(),
            Qt::LeftButton,
            Qt::LeftButton,
            true,
            "scrollbar immediate invalidated thumb press is accepted");
        ok &= send_item_mouse_event(
            scrollbar,
            QEvent::MouseMove,
            held_thumb.center() + QPointF(0.0, 40.0),
            Qt::NoButton,
            Qt::LeftButton,
            true,
            "scrollbar immediate invalidated thumb drag move is accepted");
        ok &= send_item_mouse_event(
            scrollbar,
            QEvent::MouseButtonRelease,
            held_thumb.center() + QPointF(0.0, 40.0),
            Qt::LeftButton,
            Qt::NoButton,
            true,
            "scrollbar immediate invalidated thumb release is accepted");
        ok &= check(surface.scrollback_rows() == held_scrollback_rows &&
            surface.viewport_visible_rows() == held_visible_rows &&
            surface.viewport_offset_from_tail() == held_offset &&
            surface.viewport_at_tail() == held_at_tail,
            "scrollbar immediate invalidated app routes keep public viewport frozen");
        ok &= check_rect_equal(
            scrollbar.thumb_rect(),
            held_thumb,
            "scrollbar immediate invalidated app routes keep thumb geometry frozen");

        backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(surface);
        pump_events(app);
        const QRectF released_thumb = scrollbar.thumb_rect();
        ok &= check(surface.scrollback_rows() > held_scrollback_rows,
            "scrollbar immediate release updates public scrollback range");
        ok &= check(rect_geometry_changed(held_thumb, released_thumb),
            "scrollbar immediate release updates thumb geometry");
    }

#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    QString error;
    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(transcript_path, &error);
    ok &= check(events.has_value(), "scrollbar immediate route transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return ok;
    }

    const QStringList sources = {
        QStringLiteral("app.scrollbar.wheel"),
        QStringLiteral("app.scrollbar.page"),
        QStringLiteral("app.scrollbar.track"),
        QStringLiteral("app.scrollbar.thumb"),
    };
    for (const QString& source : sources) {
        const std::string route_name = source.toStdString();
        ok &= check(transcript_has_source_event(
            *events,
            QStringLiteral("surface.scroll_intent"),
            source),
            "scrollbar immediate route records scroll intent source: " + route_name);
        ok &= check(transcript_has_source_event(
            *events,
            QStringLiteral("surface.scroll"),
            source),
            "scrollbar immediate route records scroll source: " + route_name);
    }
    for (const QString& source : sources) {
        const std::string route_name = source.toStdString();
        ok &= check(transcript_has_deferred_scroll_event(*events, source),
            "scrollbar immediate invalidated route records deferred scroll action: " + route_name);
    }
    ok &= check(transcript_has_deferred_app_wheel_trace(*events),
        "scrollbar immediate invalidated wheel trace separates deferred intent from visible scroll");
#endif

    return ok;
}

bool test_title_sync_and_button_rect_offsets(QGuiApplication& app)
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(360, 240);
    chrome_test::Terminal_qml_chrome titlebar(engine, window);
    VNM_TerminalSurface surface(window.contentItem());
    surface.setSize(QSizeF(300.0, 180.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(12.0);
    window.show();
    pump_events(app);

    const QString spinner = scalar_text(chrome_test::k_activity_marker_dingbat_first);
    const QString icon_spinner = scalar_text(chrome_test::k_activity_marker_braille_last);

    bool ok = true;
    ok &= check(titlebar.is_valid(), "shared QML titlebar initializes for metadata sync");
    if (!titlebar.is_valid()) {
        std::cerr << titlebar.error_string().toStdString() << '\n';
        return ok;
    }

    sync_terminal_title(window, &titlebar, QString(), QString());
    ok &= check(window.title() == default_window_title(),
        "empty terminal title uses native fallback title");
    ok &= check(titlebar.root_item()->property("title").toString() == default_window_title(),
        "empty terminal title initializes shared titlebar fallback title");

    sync_terminal_title(window, &titlebar, QStringLiteral("   "), QString());
    ok &= check(window.title() == default_window_title(),
        "whitespace terminal title uses native fallback title");
    ok &= check(titlebar.root_item()->property("title").toString() == default_window_title(),
        "whitespace terminal title uses shared titlebar fallback title");

    sync_terminal_title(
        window,
        &titlebar,
        QStringLiteral("  ") + spinner + QStringLiteral(" build  "),
        QString());
    ok &= check(window.title() == spinner + QStringLiteral(" build"),
        "visible window title is trimmed");
    ok &= check(titlebar.root_item()->property("activity_marker_text").toString() == spinner,
        "shared titlebar extracts activity marker after title trim");
    ok &= check(titlebar.root_item()->property("title").toString() == QStringLiteral("build"),
        "shared titlebar display title strips leading marker and one separator");

    titlebar.set_title(QStringLiteral("sentinel"));
    titlebar.set_activity_marker_text(QStringLiteral("marker-sentinel"));
    window.setTitle(QStringLiteral("sentinel"));
    const QString surface_icon_name = icon_spinner + QStringLiteral("surface-icon");
    auto backend = std::make_unique<Metadata_seed_backend>(
        osc1_icon_name_sequence(surface_icon_name));
    const bool started = term::VNM_TerminalSurface_render_bridge::start_process_with_backend(
        surface,
        std::move(backend),
        {QStringLiteral("metadata-seed")});
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(surface);
    pump_events(app);
    ok &= check(started,
        "metadata seed backend starts before titlebar connection");
    ok &= check(surface.terminal_icon_name() == surface_icon_name,
        "metadata seed backend initializes nonempty surface icon name");

    connect_terminal_metadata_to_chrome(surface, window, &titlebar);
    ok &= check(window.title() == default_window_title(),
        "metadata connection initializes native title fallback");
    ok &= check(titlebar.root_item()->property("title").toString() == default_window_title(),
        "metadata connection initializes shared title fallback");
    ok &= check(
        titlebar.root_item()->property("activity_marker_text").toString() == icon_spinner,
        "metadata connection initializes shared activity marker from current icon name");

    return ok;
}

bool test_parse_titlebar_options()
{
    Parse_result default_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(),
        "default titlebar mode parses");
#if defined(_WIN32) || defined(__linux__)
    ok &= check(default_result.options.custom_titlebar,
        "default titlebar mode enables custom titlebar on validated platforms");
#else
    ok &= check(!default_result.options.custom_titlebar,
        "default titlebar mode keeps native titlebar on unvalidated platforms");
#endif

    Parse_result custom_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--custom-titlebar"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    ok &= check(!custom_result.error.isEmpty(),
        "custom-titlebar option is not a public app option");

    Parse_result native_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--native-titlebar"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

#if defined(_WIN32) || defined(__linux__)
    ok &= check(native_result.error.isEmpty(),
        "native-titlebar option parses on validated platform");
    ok &= check(!native_result.options.custom_titlebar,
        "native-titlebar option disables custom titlebar mode");
#else
    ok &= check(!native_result.error.isEmpty(),
        "native-titlebar option is rejected on unvalidated platforms");
#endif
    return ok;
}

bool test_parse_selection_trace_option()
{
    Parse_result default_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result trace_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--selection-trace"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result command_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("--selection-trace"),
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(),
        "selection trace default parse succeeds");
    ok &= check(!default_result.options.selection_trace_enabled,
        "selection trace defaults off");
    ok &= check(trace_result.error.isEmpty(),
        "selection-trace option parses before command separator");
    ok &= check(trace_result.options.selection_trace_enabled,
        "selection-trace option enables tracing before command separator");
    ok &= check(command_result.error.isEmpty(),
        "selection-trace command argument parses after command separator");
    ok &= check(!command_result.options.selection_trace_enabled,
        "selection-trace after command separator remains a command argument");
    ok &= check(command_result.options.command == QStringList{QStringLiteral("--selection-trace")},
        "selection-trace after command separator is preserved in command argv");
    return ok;
}

bool test_parse_wheel_trace_option()
{
    Parse_result trace_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--wheel-trace"),
        QStringLiteral("--capture-transcript"),
        QStringLiteral("wheel.ndjson"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result missing_capture_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--wheel-trace"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result command_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("--wheel-trace"),
    });

    bool ok = true;
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    ok &= check(trace_result.error.isEmpty(),
        "wheel-trace option parses with transcript capture");
    ok &= check(trace_result.options.wheel_trace_enabled,
        "wheel-trace option enables tracing when transcript capture is active");
    ok &= check(trace_result.options.transcript_capture_path == QStringLiteral("wheel.ndjson"),
        "wheel-trace keeps transcript capture path");
    ok &= check(!missing_capture_result.error.isEmpty(),
        "wheel-trace without transcript capture is rejected");
    ok &= check(!missing_capture_result.options.wheel_trace_enabled,
        "rejected wheel-trace does not remain enabled");
#else
    ok &= check(!trace_result.error.isEmpty(),
        "wheel-trace option is rejected when transcript capture is disabled");
    ok &= check(!trace_result.options.wheel_trace_enabled,
        "disabled transcript build does not enable wheel tracing");
#endif
    ok &= check(command_result.error.isEmpty(),
        "wheel-trace command argument parses after command separator");
    ok &= check(!command_result.options.wheel_trace_enabled,
        "wheel-trace after command separator remains a command argument");
    ok &= check(command_result.options.command == QStringList{QStringLiteral("--wheel-trace")},
        "wheel-trace after command separator is preserved in command argv");
    return ok;
}

bool test_parse_synchronized_output_scroll_policy_option()
{
    using Synchronized_output_scroll_policy =
        VNM_TerminalSurface::Synchronized_output_scroll_policy;

    Parse_result default_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result defer_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--synchronized-output-scroll-policy=defer"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result immediate_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--synchronized-output-scroll-policy=immediate-public"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result mixed_case_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--synchronized-output-scroll-policy=ImMeDiAtE-PuBlIc"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result invalid_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--synchronized-output-scroll-policy=hidden-live"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result empty_value_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--synchronized-output-scroll-policy="),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result missing_value_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--synchronized-output-scroll-policy"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result command_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("--synchronized-output-scroll-policy=immediate-public"),
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(),
        "synchronized-output scroll policy default parse succeeds");
    ok &= check(
        default_result.options.synchronized_output_scroll_policy ==
            Synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION,
        "synchronized-output scroll policy defaults to deferred publication");
    ok &= check(defer_result.error.isEmpty(),
        "synchronized-output scroll policy defer value parses");
    ok &= check(
        defer_result.options.synchronized_output_scroll_policy ==
            Synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION,
        "synchronized-output scroll policy defer value selects deferred publication");
    ok &= check(immediate_result.error.isEmpty(),
        "synchronized-output scroll policy immediate-public value parses");
    ok &= check(
        immediate_result.options.synchronized_output_scroll_policy ==
            Synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION,
        "synchronized-output scroll policy immediate-public value selects immediate projection");
    ok &= check(mixed_case_result.error.isEmpty(),
        "synchronized-output scroll policy mixed-case value parses");
    ok &= check(
        mixed_case_result.options.synchronized_output_scroll_policy ==
            Synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION,
        "synchronized-output scroll policy mixed-case value selects immediate projection");
    ok &= check(!invalid_result.error.isEmpty(),
        "synchronized-output scroll policy rejects invalid values");
    ok &= check(
        invalid_result.options.synchronized_output_scroll_policy ==
            Synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION,
        "rejected synchronized-output scroll policy keeps deferred default");
    ok &= check(!empty_value_result.error.isEmpty(),
        "synchronized-output scroll policy rejects empty values");
    ok &= check(
        empty_value_result.options.synchronized_output_scroll_policy ==
            Synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION,
        "empty synchronized-output scroll policy keeps deferred default");
    ok &= check(!missing_value_result.error.isEmpty(),
        "synchronized-output scroll policy rejects missing values");
    ok &= check(command_result.error.isEmpty(),
        "synchronized-output scroll policy command argument parses after command separator");
    ok &= check(
        command_result.options.synchronized_output_scroll_policy ==
            Synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION,
        "synchronized-output scroll policy after command separator leaves default");
    ok &= check(
        command_result.options.command ==
            QStringList{QStringLiteral("--synchronized-output-scroll-policy=immediate-public")},
        "synchronized-output scroll policy after command separator is preserved in command argv");

    VNM_TerminalSurface default_surface;
    apply_synchronized_output_scroll_policy_option(default_surface, default_result.options);
    ok &= check(
        default_surface.synchronized_output_scroll_policy() ==
            Synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION,
        "synchronized-output scroll policy default reaches surface config");

    VNM_TerminalSurface surface;
    apply_synchronized_output_scroll_policy_option(surface, immediate_result.options);
    ok &= check(
        surface.synchronized_output_scroll_policy() ==
            Synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION,
        "synchronized-output scroll policy immediate value reaches surface config");
    apply_synchronized_output_scroll_policy_option(surface, defer_result.options);
    ok &= check(
        surface.synchronized_output_scroll_policy() ==
            Synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION,
        "synchronized-output scroll policy defer value reaches surface config");

    return ok;
}

bool test_parse_disable_primary_repaint_recovery_option()
{
    Parse_result default_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result disabled_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--disable-primary-repaint-recovery"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result command_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("--disable-primary-repaint-recovery"),
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(),
        "primary repaint recovery default parse succeeds");
    ok &= check(
        !default_result.options.primary_repaint_recovery_enabled.has_value(),
        "primary repaint recovery default leaves surface platform policy unchanged");
    ok &= check(disabled_result.error.isEmpty(),
        "primary repaint recovery disable flag parses");
    ok &= check(
        disabled_result.options.primary_repaint_recovery_enabled.has_value() &&
            !*disabled_result.options.primary_repaint_recovery_enabled,
        "primary repaint recovery disable flag selects disabled policy");
    ok &= check(
        disabled_result.options.command == QStringList{QStringLiteral("fixture-command")},
        "primary repaint recovery disable flag does not consume command argv");
    ok &= check(command_result.error.isEmpty(),
        "primary repaint recovery flag after command separator parses as command argv");
    ok &= check(
        !command_result.options.primary_repaint_recovery_enabled.has_value(),
        "primary repaint recovery flag after command separator leaves default");
    ok &= check(
        command_result.options.command ==
            QStringList{QStringLiteral("--disable-primary-repaint-recovery")},
        "primary repaint recovery flag after command separator is preserved in command argv");

    VNM_TerminalSurface default_surface;
    const bool surface_default = default_surface.primary_repaint_recovery_enabled();
    apply_primary_repaint_recovery_option(default_surface, default_result.options);
    ok &= check(
        default_surface.primary_repaint_recovery_enabled() == surface_default,
        "primary repaint recovery default keeps surface config");

    VNM_TerminalSurface disabled_surface;
    apply_primary_repaint_recovery_option(disabled_surface, disabled_result.options);
    ok &= check(!disabled_surface.primary_repaint_recovery_enabled(),
        "primary repaint recovery disable flag reaches surface config");

    return ok;
}

bool test_parse_scrollback_limit_option()
{
    Parse_result default_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result limit_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--scrollback-limit"),
        QStringLiteral("200"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result zero_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--scrollback-limit"),
        QStringLiteral("0"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result invalid_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--scrollback-limit"),
        QStringLiteral("-1"),
    });

    Parse_result command_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("--scrollback-limit"),
        QStringLiteral("200"),
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(), "scrollback limit default parse succeeds");
    ok &= check(!default_result.options.scrollback_limit.has_value(),
        "scrollback limit default leaves surface policy unchanged");
    ok &= check(limit_result.error.isEmpty(), "scrollback limit option parses");
    ok &= check(
        limit_result.options.scrollback_limit.has_value() &&
            *limit_result.options.scrollback_limit == 200,
        "scrollback limit option selects requested row count");
    ok &= check(
        limit_result.options.command == QStringList{QStringLiteral("fixture-command")},
        "scrollback limit option does not consume command argv");
    ok &= check(zero_result.error.isEmpty(), "scrollback limit accepts zero");
    ok &= check(
        zero_result.options.scrollback_limit.has_value() &&
            *zero_result.options.scrollback_limit == 0,
        "scrollback limit zero disables retained rows");
    ok &= check(!invalid_result.error.isEmpty(),
        "scrollback limit rejects negative values");
    ok &= check(command_result.error.isEmpty(),
        "scrollback limit after command separator parses as command argv");
    ok &= check(!command_result.options.scrollback_limit.has_value(),
        "scrollback limit after command separator leaves default");
    ok &= check(
        command_result.options.command ==
            QStringList{QStringLiteral("--scrollback-limit"), QStringLiteral("200")},
        "scrollback limit after command separator is preserved in command argv");

    VNM_TerminalSurface default_surface;
    const int surface_default = default_surface.scrollback_limit();
    apply_scrollback_limit_option(default_surface, default_result.options);
    ok &= check(default_surface.scrollback_limit() == surface_default,
        "scrollback limit default keeps surface config");

    VNM_TerminalSurface limited_surface;
    apply_scrollback_limit_option(limited_surface, limit_result.options);
    ok &= check(limited_surface.scrollback_limit() == 200,
        "scrollback limit option reaches surface config");

    return ok;
}

bool test_parse_transcript_snapshot_diagnostics_option()
{
    Parse_result snapshot_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--transcript-snapshot-diagnostics"),
        QStringLiteral("--capture-transcript"),
        QStringLiteral("snapshot.ndjson"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result missing_capture_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--transcript-snapshot-diagnostics"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result command_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("--transcript-snapshot-diagnostics"),
    });

    bool ok = true;
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    ok &= check(snapshot_result.error.isEmpty(),
        "transcript-snapshot-diagnostics option parses with transcript capture");
    ok &= check(snapshot_result.options.transcript_snapshot_diagnostics,
        "transcript-snapshot-diagnostics option enables snapshot diagnostics");
    ok &= check(snapshot_result.options.transcript_capture_path == QStringLiteral("snapshot.ndjson"),
        "transcript-snapshot-diagnostics keeps transcript capture path");
    ok &= check(!missing_capture_result.error.isEmpty(),
        "transcript-snapshot-diagnostics without transcript capture is rejected");
    ok &= check(!missing_capture_result.options.transcript_snapshot_diagnostics,
        "rejected transcript-snapshot-diagnostics does not remain enabled");
#else
    ok &= check(!snapshot_result.error.isEmpty(),
        "transcript-snapshot-diagnostics option is rejected when transcript capture is disabled");
    ok &= check(!snapshot_result.options.transcript_snapshot_diagnostics,
        "disabled transcript build does not enable transcript snapshot diagnostics");
#endif
    ok &= check(command_result.error.isEmpty(),
        "transcript-snapshot-diagnostics command argument parses after command separator");
    ok &= check(!command_result.options.transcript_snapshot_diagnostics,
        "transcript-snapshot-diagnostics after command separator remains a command argument");
    ok &= check(
        command_result.options.command ==
            QStringList{QStringLiteral("--transcript-snapshot-diagnostics")},
        "transcript-snapshot-diagnostics after command separator is preserved in command argv");
    return ok;
}

bool test_parse_transcript_timing_diagnostics_option()
{
    Parse_result timing_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--transcript-timing-diagnostics"),
        QStringLiteral("--capture-transcript"),
        QStringLiteral("timing.ndjson"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result missing_capture_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--transcript-timing-diagnostics"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result command_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("--transcript-timing-diagnostics"),
    });

    bool ok = true;
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    ok &= check(timing_result.error.isEmpty(),
        "transcript-timing-diagnostics option parses with transcript capture");
    ok &= check(timing_result.options.transcript_timing_diagnostics,
        "transcript-timing-diagnostics option enables timing diagnostics");
    ok &= check(timing_result.options.transcript_capture_path == QStringLiteral("timing.ndjson"),
        "transcript-timing-diagnostics keeps transcript capture path");
    ok &= check(!missing_capture_result.error.isEmpty(),
        "transcript-timing-diagnostics without transcript capture is rejected");
    ok &= check(!missing_capture_result.options.transcript_timing_diagnostics,
        "rejected transcript-timing-diagnostics does not remain enabled");
#else
    ok &= check(!timing_result.error.isEmpty(),
        "transcript-timing-diagnostics option is rejected when transcript capture is disabled");
    ok &= check(!timing_result.options.transcript_timing_diagnostics,
        "disabled transcript build does not enable transcript timing diagnostics");
#endif
    ok &= check(command_result.error.isEmpty(),
        "transcript-timing-diagnostics command argument parses after command separator");
    ok &= check(!command_result.options.transcript_timing_diagnostics,
        "transcript-timing-diagnostics after command separator remains a command argument");
    ok &= check(
        command_result.options.command ==
            QStringList{QStringLiteral("--transcript-timing-diagnostics")},
        "transcript-timing-diagnostics after command separator is preserved in command argv");
    return ok;
}

bool test_window_state_sync()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    chrome_test::Terminal_qml_chrome titlebar(engine, window);

    bool ok = true;
    ok &= check(titlebar.is_valid(), "shared QML titlebar initializes for state sync");
    if (!titlebar.is_valid()) {
        std::cerr << titlebar.error_string().toStdString() << '\n';
        return ok;
    }

    sync_chrome_window_state(titlebar, window);
    ok &= check(!titlebar.root_item()->property("maximized").toBool(),
        "shared titlebar starts with nonmaximized window state");
    ok &= check(titlebar.root_item()->property("resize_enabled").toBool(),
        "shared titlebar starts with resize enabled");
    ok &= check(window.color() == chrome_test::terminal_chrome_background_color(window.isActive()),
        "custom window border color starts synchronized with titlebar color");

    window.setWindowStates(Qt::WindowMaximized);
    sync_chrome_window_state(titlebar, window);
    ok &= check(titlebar.root_item()->property("maximized").toBool(),
        "shared titlebar tracks maximized window state");
    ok &= check(!titlebar.root_item()->property("resize_enabled").toBool(),
        "shared titlebar disables resize while maximized");

    window.setWindowStates(Qt::WindowFullScreen);
    sync_chrome_window_state(titlebar, window);
    ok &= check(titlebar.root_item()->property("maximized").toBool(),
        "shared titlebar treats fullscreen as restore-capable window state");

    window.setWindowStates(Qt::WindowNoState);
    sync_chrome_window_state(titlebar, window);
    ok &= check(!titlebar.root_item()->property("maximized").toBool(),
        "shared titlebar tracks restored window state");

    QQmlEngine signal_engine;
    QQuickWindow signal_window;
    signal_window.resize(360, 240);
    signal_window.setColor(QColor(180, 16, 16));
    chrome_test::Terminal_qml_chrome signal_titlebar(signal_engine, signal_window);
    VNM_TerminalSurface signal_surface(signal_window.contentItem());
    chrome_test::Terminal_scrollbar signal_scrollbar(signal_window.contentItem());
    ok &= check(signal_titlebar.is_valid(), "shared QML titlebar initializes for signal sync");
    if (!signal_titlebar.is_valid()) {
        std::cerr << signal_titlebar.error_string().toStdString() << '\n';
        return ok;
    }

    QObject::connect(
        &signal_window,
        &QWindow::windowStateChanged,
        &signal_titlebar,
        [&signal_window, &signal_titlebar, &signal_surface, &signal_scrollbar](
            Qt::WindowState)
        {
            sync_chrome_window_state(signal_titlebar, signal_window);
            apply_terminal_shell_geometry(
                signal_window,
                signal_surface,
                signal_scrollbar,
                &signal_titlebar,
                true);
        });
    apply_terminal_shell_geometry(
        signal_window,
        signal_surface,
        signal_scrollbar,
        &signal_titlebar,
        true);
    signal_window.setWindowStates(Qt::WindowMaximized);
    ok &= check(signal_titlebar.root_item()->property("maximized").toBool(),
        "windowStateChanged connection updates shared titlebar state");
    ok &= check(
        signal_window.color() ==
            chrome_test::terminal_chrome_background_color(signal_window.isActive()),
        "windowStateChanged connection synchronizes custom window border color");
    ok &= check_rect_equal(item_rect(signal_surface), QRectF(1.0, 33.0, 346.0, 206.0),
        "windowStateChanged connection reapplies maximized geometry");
    ok &= check_rect_equal(item_rect(signal_scrollbar), QRectF(347.0, 33.0, 12.0, 206.0),
        "windowStateChanged connection reapplies maximized scrollbar geometry");

    return ok;
}

#if defined(Q_OS_MACOS)
bool test_macos_command_shortcuts_are_host_shortcuts(QGuiApplication& app)
{
    QQuickWindow window;
    window.resize(360, 240);
    VNM_TerminalSurface surface(window.contentItem());
    Recording_event_filter key_filter(QEvent::KeyPress);
    Terminal_shortcut_filter shortcut_filter(&surface);

    window.installEventFilter(&key_filter);
    window.installEventFilter(&shortcut_filter);
    window.show();
    pump_events(app);
    surface.forceActiveFocus();
    pump_events(app);

    QKeyEvent paste_event(QEvent::KeyPress, Qt::Key_V, Qt::MetaModifier);
    paste_event.setAccepted(false);
    const bool paste_sent = QCoreApplication::sendEvent(&window, &paste_event);

    QKeyEvent copy_event(QEvent::KeyPress, Qt::Key_C, Qt::MetaModifier);
    copy_event.setAccepted(false);
    const bool copy_sent = QCoreApplication::sendEvent(&window, &copy_event);

    bool ok = true;
    ok &= check(paste_sent, "macOS Command+V event is delivered");
    ok &= check(copy_sent, "macOS Command+C event is delivered");
    ok &= check(key_filter.recorded_count == 0,
        "macOS Command copy/paste shortcuts are consumed by the terminal app");
    return ok;
}
#endif

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    bool ok = true;
    ok &= test_custom_titlebar_geometry();
    ok &= test_terminal_scrollbar_tracks_surface_viewport(app);
    ok &= test_terminal_scrollbar_immediate_public_projection_routes(app);
    ok &= test_title_sync_and_button_rect_offsets(app);
    ok &= test_parse_titlebar_options();
    ok &= test_parse_selection_trace_option();
    ok &= test_parse_wheel_trace_option();
    ok &= test_parse_synchronized_output_scroll_policy_option();
    ok &= test_parse_disable_primary_repaint_recovery_option();
    ok &= test_parse_scrollback_limit_option();
    ok &= test_parse_transcript_snapshot_diagnostics_option();
    ok &= test_parse_transcript_timing_diagnostics_option();
    ok &= test_window_state_sync();
#if defined(Q_OS_MACOS)
    ok &= test_macos_command_shortcuts_are_host_shortcuts(app);
#endif
    return ok ? 0 : 1;
}
