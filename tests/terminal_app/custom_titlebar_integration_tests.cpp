#define VNM_TERMINAL_APP_NO_MAIN
#include "../../src/main.cpp"
#undef VNM_TERMINAL_APP_NO_MAIN

#include "terminal_title_metadata.h"

#include "vnm_qml_chrome/vnm_chrome_geometry.h"

#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/internal/terminal_transcript.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QVariant>
#include <QWheelEvent>
#include <QWindow>

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

// Walks both the QObject child tree and the visual child-item tree, so it
// reaches Repeater-created delegates (e.g. custom titlebar buttons) that
// plain QObject::findChild does not. Mirrors the app's own find_child_object.
QQuickItem* find_quick_item_recursive(QObject* root, const QString& object_name)
{
    if (root == nullptr) {
        return nullptr;
    }

    if (root->objectName() == object_name) {
        if (auto* item = qobject_cast<QQuickItem*>(root)) {
            return item;
        }
    }

    const auto object_children = root->children();
    for (QObject* child : object_children) {
        if (QQuickItem* found = find_quick_item_recursive(child, object_name)) {
            return found;
        }
    }

    if (auto* item = qobject_cast<QQuickItem*>(root)) {
        const auto child_items = item->childItems();
        for (QQuickItem* child : child_items) {
            if (QQuickItem* found = find_quick_item_recursive(child, object_name)) {
                return found;
            }
        }
    }

    return nullptr;
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
    ok &= check_rect_equal(item_rect(*titlebar.titlebar_item()), QRectF(0.0, 0.0, 800.0, 30.0),
        "shared titlebar occupies the top band");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_x").toReal(), 4.0),
        "shared chrome records content border x");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_y").toReal(), 30.0),
        "shared chrome records content border y");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_width").toReal(), 792.0),
        "shared chrome records content border width");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_height").toReal(), 446.0),
        "shared chrome records content border height");
    ok &= check_rect_equal(item_rect(surface), QRectF(5.0, 31.0, 778.0, 444.0),
        "custom terminal is inset inside the content border");
    ok &= check_rect_equal(item_rect(scrollbar), QRectF(783.0, 31.0, 12.0, 444.0),
        "custom scrollbar touches the inner right frame edge");
    ok &= check(surface.y() >= titlebar.titlebar_item()->y() + titlebar.titlebar_item()->height(),
        "terminal top is below titlebar bottom");
    constexpr qreal expected_resize_border_width =
        chrome_test::k_default_frameless_resize_border_width -
        chrome_test::k_frameless_resize_border_physical_reduction;
    ok &= check(surface.x() >= expected_resize_border_width,
        "terminal left edge is inside resize border");
    ok &= check(nearly_equal(surface.x() + surface.width(), scrollbar.x()),
        "terminal right edge is adjacent to scrollbar");
    ok &= check(
        scrollbar.x() + scrollbar.width() <=
            window.width() - expected_resize_border_width,
        "scrollbar right edge is inside resize border");
    ok &= check(
        surface.y() + surface.height() <=
            window.height() - expected_resize_border_width,
        "terminal bottom edge is inside resize border");

    constexpr qreal hidpi_dpr = 1.25;
    const Terminal_shell_geometry hidpi_geometry = terminal_shell_geometry(
        QSizeF(1920.0, 1080.0),
        true,
        true,
        1.0 / hidpi_dpr,
        hidpi_dpr);
    ok &= check_rect_equal(
        hidpi_geometry.content_border_rect,
        QRectF(4.8, 30.4, 1910.4, 1044.8),
        "custom titlebar content border snaps to physical pixels at fractional DPR");
    ok &= check_rect_equal(
        hidpi_geometry.terminal_rect,
        QRectF(5.6, 31.2, 1896.8, 1043.2),
        "custom titlebar terminal rect rounds to physical pixels at fractional DPR");
    ok &= check_rect_equal(
        hidpi_geometry.scrollbar_rect,
        QRectF(1902.4, 31.2, 12.0, 1043.2),
        "custom titlebar scrollbar rect rounds to physical pixels at fractional DPR");
    ok &= check(
        vnm_qml_chrome::rect_has_snapped_physical_edges(
            hidpi_geometry.content_border_rect,
            hidpi_dpr),
        "custom titlebar content border edges are physical-pixel aligned");
    ok &= check(
        vnm_qml_chrome::rect_has_snapped_physical_edges(
            hidpi_geometry.terminal_rect,
            hidpi_dpr),
        "custom titlebar terminal edges are physical-pixel aligned");
    ok &= check(
        vnm_qml_chrome::rect_has_snapped_physical_edges(
            hidpi_geometry.scrollbar_rect,
            hidpi_dpr),
        "custom titlebar scrollbar edges are physical-pixel aligned");
    ok &= check(
        nearly_equal(
            hidpi_geometry.terminal_rect.right(),
            hidpi_geometry.scrollbar_rect.left()),
        "custom titlebar fractional-DPR terminal and scrollbar remain adjacent");

    window.resize(360, 240);
    apply_terminal_shell_geometry(window, surface, scrollbar, &titlebar, true);
    ok &= check_rect_equal(item_rect(*titlebar.root_item()), QRectF(0.0, 0.0, 360.0, 240.0),
        "resized shared chrome root tracks window size");
    ok &= check_rect_equal(item_rect(*titlebar.titlebar_item()), QRectF(0.0, 0.0, 360.0, 30.0),
        "resized custom titlebar tracks window width");
    ok &= check_rect_equal(item_rect(surface), QRectF(5.0, 31.0, 338.0, 204.0),
        "resized custom terminal tracks titlebar and border insets");
    ok &= check_rect_equal(item_rect(scrollbar), QRectF(343.0, 31.0, 12.0, 204.0),
        "resized custom scrollbar remains inside the right frame");

    window.setWindowStates(Qt::WindowMaximized);
    apply_terminal_shell_geometry(window, surface, scrollbar, &titlebar, true);
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_x").toReal(), 0.0),
        "maximized shared chrome drops inactive resize gutter x");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_width").toReal(), 360.0),
        "maximized shared chrome drops inactive resize gutter width");
    ok &= check_rect_equal(item_rect(surface), QRectF(1.0, 31.0, 346.0, 208.0),
        "maximized custom terminal drops inactive resize border gutters");
    ok &= check_rect_equal(item_rect(scrollbar), QRectF(347.0, 31.0, 12.0, 208.0),
        "maximized custom scrollbar remains inside content bounds");
    window.setWindowStates(Qt::WindowNoState);

    window.resize(8, 40);
    apply_terminal_shell_geometry(window, surface, scrollbar, &titlebar, true);
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_x").toReal(), 4.0),
        "very narrow shared chrome clamps horizontal resize insets");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_width").toReal(), 0.0),
        "very narrow shared chrome clamps content border width");
    ok &= check_rect_equal(item_rect(surface), QRectF(4.0, 31.0, 0.0, 4.0),
        "very narrow custom terminal clamps horizontal resize insets");
    ok &= check_rect_equal(item_rect(scrollbar), QRectF(4.0, 31.0, 0.0, 4.0),
        "very narrow custom scrollbar clamps inside horizontal resize insets");

    window.resize(200, 20);
    apply_terminal_shell_geometry(window, surface, scrollbar, &titlebar, true);
    ok &= check_rect_equal(item_rect(*titlebar.titlebar_item()), QRectF(0.0, 0.0, 200.0, 20.0),
        "very short custom titlebar clamps to window height");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_border_height").toReal(), 0.0),
        "very short shared chrome clamps content border height");
    ok &= check_rect_equal(item_rect(surface), QRectF(5.0, 20.0, 178.0, 0.0),
        "very short custom terminal clamps nonnegative height");
    ok &= check_rect_equal(item_rect(scrollbar), QRectF(183.0, 20.0, 12.0, 0.0),
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
        QStringLiteral("--synchronized-output-scroll-policy"),
        QStringLiteral("defer"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result immediate_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--synchronized-output-scroll-policy"),
        QStringLiteral("immediate-public"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result mixed_case_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--synchronized-output-scroll-policy"),
        QStringLiteral("ImMeDiAtE-PuBlIc"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result invalid_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--synchronized-output-scroll-policy"),
        QStringLiteral("hidden-live"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result empty_value_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--synchronized-output-scroll-policy"),
        QStringLiteral(""),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result missing_value_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--synchronized-output-scroll-policy"),
    });

    Parse_result command_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("--synchronized-output-scroll-policy"),
        QStringLiteral("immediate-public"),
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
            QStringList{
                QStringLiteral("--synchronized-output-scroll-policy"),
                QStringLiteral("immediate-public"),
            },
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

bool test_parse_text_renderer_option()
{
    using Text_renderer_mode = VNM_TerminalSurface::Text_renderer_mode;

    Parse_result default_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result msdf_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--text-renderer"),
        QStringLiteral("msdf"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result glyph_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--text-renderer"),
        QStringLiteral("glyph"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result auto_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--text-renderer"),
        QStringLiteral("AUTO"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result invalid_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--text-renderer"),
        QStringLiteral("invalid"),
    });

    Parse_result command_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("--text-renderer"),
        QStringLiteral("glyph"),
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(), "text renderer default parse succeeds");
    ok &= check(default_result.options.text_renderer_mode == Text_renderer_mode::AUTO,
        "text renderer default selects auto");
    ok &= check(msdf_result.error.isEmpty(), "text renderer msdf option parses");
    ok &= check(msdf_result.options.text_renderer_mode == Text_renderer_mode::MSDF,
        "text renderer msdf option selects MSDF");
    ok &= check(glyph_result.error.isEmpty(), "text renderer glyph option parses");
    ok &= check(glyph_result.options.text_renderer_mode == Text_renderer_mode::GLYPH,
        "text renderer glyph option selects glyph");
    ok &= check(auto_result.error.isEmpty(), "text renderer auto option parses");
    ok &= check(auto_result.options.text_renderer_mode == Text_renderer_mode::AUTO,
        "text renderer option is case-insensitive");
    ok &= check(!invalid_result.error.isEmpty(),
        "text renderer option rejects invalid values");
    ok &= check(command_result.error.isEmpty(),
        "text renderer option after command separator parses as command argv");
    ok &= check(command_result.options.text_renderer_mode == Text_renderer_mode::AUTO,
        "text renderer option after command separator leaves default");
    ok &= check(
        command_result.options.command ==
            QStringList{
                QStringLiteral("--text-renderer"),
                QStringLiteral("glyph"),
            },
        "text renderer option after command separator is preserved in command argv");

    VNM_TerminalSurface surface;
    surface.set_text_renderer_mode(msdf_result.options.text_renderer_mode);
    ok &= check(surface.text_renderer_mode() == Text_renderer_mode::MSDF,
        "text renderer option reaches surface config");
    surface.set_text_renderer_mode(glyph_result.options.text_renderer_mode);
    ok &= check(surface.text_renderer_mode() == Text_renderer_mode::GLYPH,
        "text renderer glyph option reaches surface config");

    return ok;
}

bool test_parse_lcd_subpixel_option()
{
    using Lcd_subpixel_order = VNM_TerminalSurface::Lcd_subpixel_order;

    Parse_result default_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result rgb_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--lcd-subpixel"),
        QStringLiteral("rgb"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result bgr_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--lcd-subpixel"),
        QStringLiteral("bgr"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result vrgb_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--lcd-subpixel"),
        QStringLiteral("vrgb"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result vbgr_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--lcd-subpixel"),
        QStringLiteral("vbgr"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result none_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--lcd-subpixel"),
        QStringLiteral("none"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result auto_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--lcd-subpixel"),
        QStringLiteral("AUTO"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result invalid_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--lcd-subpixel"),
        QStringLiteral("invalid"),
    });

    Parse_result command_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("--lcd-subpixel"),
        QStringLiteral("rgb"),
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(), "LCD subpixel default parse succeeds");
    ok &= check(default_result.options.lcd_subpixel_order == Lcd_subpixel_order::AUTO,
        "LCD subpixel default selects auto");
    ok &= check(rgb_result.error.isEmpty(), "LCD subpixel rgb option parses");
    ok &= check(rgb_result.options.lcd_subpixel_order == Lcd_subpixel_order::RGB,
        "LCD subpixel rgb option selects RGB");
    ok &= check(bgr_result.error.isEmpty(), "LCD subpixel bgr option parses");
    ok &= check(bgr_result.options.lcd_subpixel_order == Lcd_subpixel_order::BGR,
        "LCD subpixel bgr option selects BGR");
    ok &= check(vrgb_result.error.isEmpty(), "LCD subpixel vrgb option parses");
    ok &= check(vrgb_result.options.lcd_subpixel_order == Lcd_subpixel_order::VRGB,
        "LCD subpixel vrgb option selects VRGB");
    ok &= check(vbgr_result.error.isEmpty(), "LCD subpixel vbgr option parses");
    ok &= check(vbgr_result.options.lcd_subpixel_order == Lcd_subpixel_order::VBGR,
        "LCD subpixel vbgr option selects VBGR");
    ok &= check(none_result.error.isEmpty(), "LCD subpixel none option parses");
    ok &= check(none_result.options.lcd_subpixel_order == Lcd_subpixel_order::NONE,
        "LCD subpixel none option disables LCD sampling");
    ok &= check(auto_result.error.isEmpty(), "LCD subpixel auto option parses");
    ok &= check(auto_result.options.lcd_subpixel_order == Lcd_subpixel_order::AUTO,
        "LCD subpixel option is case-insensitive");
    ok &= check(!invalid_result.error.isEmpty(),
        "LCD subpixel option rejects invalid values");
    ok &= check(command_result.error.isEmpty(),
        "LCD subpixel option after command separator parses as command argv");
    ok &= check(command_result.options.lcd_subpixel_order == Lcd_subpixel_order::AUTO,
        "LCD subpixel option after command separator leaves default");
    ok &= check(
        command_result.options.command ==
            QStringList{
                QStringLiteral("--lcd-subpixel"),
                QStringLiteral("rgb"),
            },
        "LCD subpixel option after command separator is preserved in command argv");

    VNM_TerminalSurface surface;
    surface.set_lcd_subpixel_order(rgb_result.options.lcd_subpixel_order);
    ok &= check(surface.lcd_subpixel_order() == Lcd_subpixel_order::RGB,
        "LCD subpixel rgb option reaches surface config");
    surface.set_lcd_subpixel_order(vbgr_result.options.lcd_subpixel_order);
    ok &= check(surface.lcd_subpixel_order() == Lcd_subpixel_order::VBGR,
        "LCD subpixel vbgr option reaches surface config");

    return ok;
}

bool test_parse_row_timestamps_option()
{
    Parse_result default_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result on_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--row-timestamps"),
        QStringLiteral("on"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result off_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--row-timestamps"),
        QStringLiteral("off"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result mixed_case_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--row-timestamps"),
        QStringLiteral("OFF"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result invalid_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--row-timestamps"),
        QStringLiteral("sometimes"),
    });

    Parse_result command_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("--row-timestamps"),
        QStringLiteral("off"),
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(), "row timestamps default parse succeeds");
    ok &= check(default_result.options.row_timestamp_tooltip_enabled,
        "row timestamps default leaves the tooltip enabled");
    ok &= check(!default_result.options.row_timestamp_tooltip_explicit,
        "row timestamps default is not marked explicit");
    ok &= check(on_result.error.isEmpty(), "row timestamps on option parses");
    ok &= check(on_result.options.row_timestamp_tooltip_enabled,
        "row timestamps on option keeps the tooltip enabled");
    ok &= check(on_result.options.row_timestamp_tooltip_explicit,
        "row timestamps on option is marked explicit");
    ok &= check(off_result.error.isEmpty(), "row timestamps off option parses");
    ok &= check(!off_result.options.row_timestamp_tooltip_enabled,
        "row timestamps off option disables the tooltip");
    ok &= check(off_result.options.row_timestamp_tooltip_explicit,
        "row timestamps off option is marked explicit");
    ok &= check(mixed_case_result.error.isEmpty(),
        "row timestamps mixed-case option parses");
    ok &= check(!mixed_case_result.options.row_timestamp_tooltip_enabled,
        "row timestamps option is case-insensitive");
    ok &= check(!invalid_result.error.isEmpty(),
        "row timestamps option rejects invalid values");
    ok &= check(command_result.error.isEmpty(),
        "row timestamps option after command separator parses as command argv");
    ok &= check(command_result.options.row_timestamp_tooltip_enabled,
        "row timestamps option after command separator leaves default");
    ok &= check(
        command_result.options.command ==
            QStringList{
                QStringLiteral("--row-timestamps"),
                QStringLiteral("off"),
            },
        "row timestamps option after command separator is preserved in command argv");

    VNM_TerminalSurface surface;
    surface.set_row_timestamp_tooltip_enabled(off_result.options.row_timestamp_tooltip_enabled);
    ok &= check(!surface.row_timestamp_tooltip_enabled(),
        "row timestamps off option reaches surface config");

    return ok;
}

bool test_row_timestamp_tooltip_chrome(QGuiApplication& app)
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(800, 480);

    chrome_test::Terminal_qml_chrome titlebar(engine, window);
    VNM_TerminalSurface surface(window.contentItem());
    chrome_test::Terminal_scrollbar scrollbar(window.contentItem());

    bool ok = true;
    ok &= check(titlebar.is_valid(), "row timestamp tooltip chrome initializes");
    if (!titlebar.is_valid()) {
        std::cerr << titlebar.error_string().toStdString() << '\n';
        return ok;
    }

    apply_terminal_shell_geometry(window, surface, scrollbar, &titlebar, true);
    connect_row_timestamp_tooltip_to_chrome(surface, &titlebar);
    window.show();
    pump_events(app);

    auto* tooltip = find_quick_item_recursive(
        titlebar.root_item(), QStringLiteral("row_timestamp_tooltip"));
    ok &= check(tooltip != nullptr, "chrome builds the row timestamp tooltip");
    if (tooltip == nullptr) {
        return ok;
    }

    ok &= check(!tooltip->isVisible(), "row timestamp tooltip starts hidden");
    ok &= check(!tooltip->isEnabled(),
        "row timestamp tooltip never accepts pointer input");

    // Drive the real wiring: emitting the surface signal must place the
    // tooltip in chrome coordinates. The surface sits at (5, 31) in this
    // 800x480 fixture (see test_custom_titlebar_geometry), and the tooltip
    // anchors 12px down-right of the pointer.
    const QDateTime timestamp(QDate(2026, 6, 10), QTime(14, 30, 5));
    QMetaObject::invokeMethod(
        &surface,
        "row_timestamp_tooltip_requested",
        Q_ARG(qreal, 40.0),
        Q_ARG(qreal, 20.0),
        Q_ARG(QDateTime, timestamp));
    pump_events(app);

    ok &= check(
        titlebar.root_item()->property("row_timestamp_tooltip_visible").toBool(),
        "tooltip request raises the chrome visibility flag");
    ok &= check(nearly_equal(tooltip->x(), 40.0 + 5.0 + 12.0),
        "tooltip anchors right of the reported pointer position");
    ok &= check(nearly_equal(tooltip->y(), 20.0 + 31.0 + 12.0),
        "tooltip anchors below the reported pointer position");

    auto* tooltip_text = find_quick_item_recursive(
        tooltip, QStringLiteral("row_timestamp_tooltip_text"));
    ok &= check(tooltip_text != nullptr, "tooltip exposes its timestamp text");
    if (tooltip_text != nullptr) {
        ok &= check(
            tooltip_text->property("text").toString() ==
                QStringLiteral("2026-06-10 14:30:05"),
            "tooltip formats the timestamp as a concrete local date and time");
    }

    titlebar.show_row_timestamp_tooltip(QPointF(795.0, 475.0), timestamp);
    pump_events(app);
    ok &= check(nearly_equal(tooltip->x(), 800.0 - tooltip->width()  - 4.0),
        "tooltip clamps to the right window edge");
    ok &= check(nearly_equal(tooltip->y(), 480.0 - tooltip->height() - 4.0),
        "tooltip clamps to the bottom window edge");

    QMetaObject::invokeMethod(&surface, "row_timestamp_tooltip_dismissed");
    pump_events(app);
    ok &= check(
        !titlebar.root_item()->property("row_timestamp_tooltip_visible").toBool(),
        "tooltip dismissal clears the chrome visibility flag");

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
    ok &= check_rect_equal(item_rect(signal_surface), QRectF(1.0, 31.0, 346.0, 208.0),
        "windowStateChanged connection reapplies maximized geometry");
    ok &= check_rect_equal(item_rect(signal_scrollbar), QRectF(347.0, 31.0, 12.0, 208.0),
        "windowStateChanged connection reapplies maximized scrollbar geometry");

    return ok;
}

bool test_paste_shortcut_should_paste_predicate()
{
    using chrome_test::Paste_shortcut_policy;
    using chrome_test::paste_shortcut_should_paste;

    const Qt::KeyboardModifiers ctrl = Qt::ControlModifier;
    const Qt::KeyboardModifiers ctrl_shift = Qt::ControlModifier | Qt::ShiftModifier;
    const Qt::KeyboardModifiers no_mods = Qt::NoModifier;
    const Qt::KeyboardModifiers shift_only = Qt::ShiftModifier;

    bool ok = true;

    // DISABLED never pastes for any modifier combination.
    ok &= check(
        !paste_shortcut_should_paste(Paste_shortcut_policy::DISABLED, Qt::Key_V, ctrl),
        "disabled paste policy ignores Ctrl+V");
    ok &= check(
        !paste_shortcut_should_paste(Paste_shortcut_policy::DISABLED, Qt::Key_V, ctrl_shift),
        "disabled paste policy ignores Ctrl+Shift+V");
    ok &= check(
        !paste_shortcut_should_paste(Paste_shortcut_policy::DISABLED, Qt::Key_V, no_mods),
        "disabled paste policy ignores bare V");
    ok &= check(
        !paste_shortcut_should_paste(Paste_shortcut_policy::DISABLED, Qt::Key_V, shift_only),
        "disabled paste policy ignores Shift+V");

    // CTRL_SHIFT_V pastes only on the exact Ctrl+Shift+V combination.
    ok &= check(
        !paste_shortcut_should_paste(Paste_shortcut_policy::CTRL_SHIFT_V, Qt::Key_V, ctrl),
        "ctrl-shift-v paste policy rejects plain Ctrl+V");
    ok &= check(
        paste_shortcut_should_paste(Paste_shortcut_policy::CTRL_SHIFT_V, Qt::Key_V, ctrl_shift),
        "ctrl-shift-v paste policy accepts Ctrl+Shift+V");
    ok &= check(
        !paste_shortcut_should_paste(Paste_shortcut_policy::CTRL_SHIFT_V, Qt::Key_V, no_mods),
        "ctrl-shift-v paste policy rejects bare V");
    ok &= check(
        !paste_shortcut_should_paste(Paste_shortcut_policy::CTRL_SHIFT_V, Qt::Key_V, shift_only),
        "ctrl-shift-v paste policy rejects Shift+V");

    // CTRL_V_AND_CTRL_SHIFT_V accepts either Ctrl combination.
    ok &= check(
        paste_shortcut_should_paste(
            Paste_shortcut_policy::CTRL_V_AND_CTRL_SHIFT_V, Qt::Key_V, ctrl),
        "ctrl-v-and-ctrl-shift-v paste policy accepts Ctrl+V");
    ok &= check(
        paste_shortcut_should_paste(
            Paste_shortcut_policy::CTRL_V_AND_CTRL_SHIFT_V, Qt::Key_V, ctrl_shift),
        "ctrl-v-and-ctrl-shift-v paste policy accepts Ctrl+Shift+V");
    ok &= check(
        !paste_shortcut_should_paste(
            Paste_shortcut_policy::CTRL_V_AND_CTRL_SHIFT_V, Qt::Key_V, no_mods),
        "ctrl-v-and-ctrl-shift-v paste policy rejects bare V");
    ok &= check(
        !paste_shortcut_should_paste(
            Paste_shortcut_policy::CTRL_V_AND_CTRL_SHIFT_V, Qt::Key_V, shift_only),
        "ctrl-v-and-ctrl-shift-v paste policy rejects Shift+V");

    // PLATFORM_DEFAULT accepts the Ctrl combinations on every platform.
    ok &= check(
        paste_shortcut_should_paste(Paste_shortcut_policy::PLATFORM_DEFAULT, Qt::Key_V, ctrl),
        "platform-default paste policy accepts Ctrl+V");
    ok &= check(
        paste_shortcut_should_paste(
            Paste_shortcut_policy::PLATFORM_DEFAULT, Qt::Key_V, ctrl_shift),
        "platform-default paste policy accepts Ctrl+Shift+V");
    ok &= check(
        !paste_shortcut_should_paste(Paste_shortcut_policy::PLATFORM_DEFAULT, Qt::Key_V, no_mods),
        "platform-default paste policy rejects bare V");
    ok &= check(
        !paste_shortcut_should_paste(
            Paste_shortcut_policy::PLATFORM_DEFAULT, Qt::Key_V, shift_only),
        "platform-default paste policy rejects Shift+V");

    // A non-V key never pastes regardless of policy or modifiers.
    ok &= check(
        !paste_shortcut_should_paste(Paste_shortcut_policy::PLATFORM_DEFAULT, Qt::Key_C, ctrl),
        "platform-default paste policy ignores non-V keys");
    ok &= check(
        !paste_shortcut_should_paste(
            Paste_shortcut_policy::CTRL_V_AND_CTRL_SHIFT_V, Qt::Key_C, ctrl_shift),
        "ctrl-v-and-ctrl-shift-v paste policy ignores non-V keys");

#if defined(Q_OS_MACOS)
    // Only PLATFORM_DEFAULT honors the macOS Cmd+V shortcut; the explicit
    // Ctrl-combo policies are literal overrides that drop it.
    const Qt::KeyboardModifiers meta = Qt::MetaModifier;
    ok &= check(
        paste_shortcut_should_paste(Paste_shortcut_policy::PLATFORM_DEFAULT, Qt::Key_V, meta),
        "platform-default paste policy accepts macOS Cmd+V");
    ok &= check(
        !paste_shortcut_should_paste(Paste_shortcut_policy::CTRL_SHIFT_V, Qt::Key_V, meta),
        "ctrl-shift-v paste policy rejects macOS Cmd+V");
    ok &= check(
        !paste_shortcut_should_paste(
            Paste_shortcut_policy::CTRL_V_AND_CTRL_SHIFT_V, Qt::Key_V, meta),
        "ctrl-v-and-ctrl-shift-v paste policy rejects macOS Cmd+V");
    ok &= check(
        !paste_shortcut_should_paste(Paste_shortcut_policy::DISABLED, Qt::Key_V, meta),
        "disabled paste policy rejects macOS Cmd+V");
#endif

    return ok;
}

bool test_parse_paste_shortcut_option()
{
    using chrome_test::Paste_shortcut_policy;

    Parse_result default_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result disabled_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--paste-shortcut"),
        QStringLiteral("disabled"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result ctrl_shift_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--paste-shortcut"),
        QStringLiteral("ctrl-shift-v"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result ctrl_v_and_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--paste-shortcut"),
        QStringLiteral("ctrl-v-and-ctrl-shift-v"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result platform_default_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--paste-shortcut"),
        QStringLiteral("PLATFORM-DEFAULT"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result invalid_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--paste-shortcut"),
        QStringLiteral("bogus"),
    });

    Parse_result missing_value_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--paste-shortcut"),
    });

    Parse_result command_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("--paste-shortcut"),
        QStringLiteral("disabled"),
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(), "paste shortcut default parse succeeds");
    ok &= check(
        default_result.options.paste_shortcut_policy == Paste_shortcut_policy::PLATFORM_DEFAULT,
        "paste shortcut defaults to platform-default");
    ok &= check(disabled_result.error.isEmpty(), "paste shortcut disabled option parses");
    ok &= check(
        disabled_result.options.paste_shortcut_policy == Paste_shortcut_policy::DISABLED,
        "paste shortcut disabled option selects disabled policy");
    ok &= check(ctrl_shift_result.error.isEmpty(), "paste shortcut ctrl-shift-v option parses");
    ok &= check(
        ctrl_shift_result.options.paste_shortcut_policy == Paste_shortcut_policy::CTRL_SHIFT_V,
        "paste shortcut ctrl-shift-v option selects Ctrl+Shift+V policy");
    ok &= check(ctrl_v_and_result.error.isEmpty(),
        "paste shortcut ctrl-v-and-ctrl-shift-v option parses");
    ok &= check(
        ctrl_v_and_result.options.paste_shortcut_policy ==
            Paste_shortcut_policy::CTRL_V_AND_CTRL_SHIFT_V,
        "paste shortcut ctrl-v-and-ctrl-shift-v option selects both Ctrl combinations");
    ok &= check(platform_default_result.error.isEmpty(),
        "paste shortcut platform-default option parses");
    ok &= check(
        platform_default_result.options.paste_shortcut_policy ==
            Paste_shortcut_policy::PLATFORM_DEFAULT,
        "paste shortcut platform-default option is case-insensitive");
    ok &= check(!invalid_result.error.isEmpty(),
        "paste shortcut option rejects invalid values");
    ok &= check(
        invalid_result.error ==
            QStringLiteral(
                "--paste-shortcut supports only disabled, ctrl-shift-v, "
                "ctrl-v-and-ctrl-shift-v, or platform-default"),
        "paste shortcut invalid value reports the documented error");
    ok &= check(
        invalid_result.options.paste_shortcut_policy == Paste_shortcut_policy::PLATFORM_DEFAULT,
        "rejected paste shortcut keeps platform-default");
    ok &= check(!missing_value_result.error.isEmpty(),
        "paste shortcut option rejects missing values");
    ok &= check(command_result.error.isEmpty(),
        "paste shortcut option after command separator parses as command argv");
    ok &= check(
        command_result.options.paste_shortcut_policy == Paste_shortcut_policy::PLATFORM_DEFAULT,
        "paste shortcut option after command separator leaves default");
    ok &= check(
        command_result.options.command ==
            QStringList{
                QStringLiteral("--paste-shortcut"),
                QStringLiteral("disabled"),
            },
        "paste shortcut option after command separator is preserved in command argv");

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

bool test_settings_gear_button_and_window(QGuiApplication& app)
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(800, 480);

    chrome_test::Terminal_qml_chrome titlebar(engine, window);
    VNM_TerminalSurface surface(window.contentItem());
    chrome_test::Terminal_scrollbar scrollbar(window.contentItem());

    bool ok = true;
    ok &= check(titlebar.is_valid(), "settings gear titlebar initializes");
    if (!titlebar.is_valid()) {
        std::cerr << titlebar.error_string().toStdString() << '\n';
        return ok;
    }

    apply_terminal_shell_geometry(window, surface, scrollbar, &titlebar, true);
    window.show();
    pump_events(app);

    auto* gear = find_quick_item_recursive(
        titlebar.root_item(), QStringLiteral("settings_button"));
    ok &= check(gear != nullptr, "titlebar exposes a settings gear button");
    if (gear == nullptr) {
        return ok;
    }
    ok &= check(gear->isVisible(), "settings gear button is visible by default");
    ok &= check(gear->width() > 0.0 && gear->height() > 0.0,
        "settings gear button has a nonzero hit area");

    int settings_requests = 0;
    QObject::connect(
        &titlebar,
        &chrome_test::Terminal_qml_chrome::settings_requested,
        &titlebar,
        [&settings_requests] {
            ++settings_requests;
        });

    const QPointF gear_center(gear->width() / 2.0, gear->height() / 2.0);
    ok &= send_item_mouse_event(
        *gear,
        QEvent::MouseButtonPress,
        gear_center,
        Qt::LeftButton,
        Qt::LeftButton,
        true,
        "settings gear press is accepted");
    ok &= send_item_mouse_event(
        *gear,
        QEvent::MouseButtonRelease,
        gear_center,
        Qt::LeftButton,
        Qt::NoButton,
        true,
        "settings gear release is accepted");
    pump_events(app);
    ok &= check(settings_requests == 1,
        "clicking the settings gear emits exactly one settings request");

    chrome_test::Terminal_settings_controller settings_controller(surface);
    chrome_test::Terminal_settings_window settings_window(engine, surface, settings_controller);
    ok &= check(settings_window.is_valid(), "settings window initializes");
    if (!settings_window.is_valid()) {
        std::cerr << settings_window.error_string().toStdString() << '\n';
        return ok;
    }

    settings_window.set_transient_parent(&window);
    settings_window.show_window();
    pump_events(app);

    QQuickWindow* settings_qml_window = nullptr;
    const auto top_level_windows = QGuiApplication::topLevelWindows();
    for (QWindow* top_level : top_level_windows) {
        if (top_level->objectName() == QStringLiteral("terminal_settings_window")) {
            settings_qml_window = qobject_cast<QQuickWindow*>(top_level);
            break;
        }
    }
    ok &= check(settings_qml_window != nullptr,
        "settings window registers a top-level window");
    if (settings_qml_window != nullptr) {
        ok &= check(settings_qml_window->isVisible(),
            "settings window becomes visible when shown");
        ok &= check(
            settings_qml_window->findChild<QQuickItem*>(
                QStringLiteral("settings_window_titlebar")) != nullptr,
            "settings window builds its chrome titlebar");

        auto* scheme_list = settings_qml_window->findChild<QQuickItem*>(
            QStringLiteral("scheme_list"));
        ok &= check(scheme_list != nullptr,
            "settings panel builds the color-scheme picker");
        if (scheme_list != nullptr) {
            ok &= check(
                scheme_list->property("count").toInt() ==
                    surface.available_color_schemes().size(),
                "color-scheme picker lists every bundled scheme");
        }

        ok &= check(
            settings_qml_window->findChild<QQuickItem*>(
                QStringLiteral("row_timestamp_switch")) != nullptr,
            "settings panel builds the row-timestamp switch");
    }

    ok &= check(surface.color_scheme() == QStringLiteral("Campbell"),
        "surface defaults to the Campbell color scheme");
    ok &= check(surface.available_color_schemes().size() == 17,
        "surface exposes the bundled color schemes");
    surface.set_color_scheme(QStringLiteral("Tango Dark"));
    ok &= check(surface.color_scheme() == QStringLiteral("Tango Dark"),
        "selecting a bundled scheme updates the surface live");

    return ok;
}

bool test_settings_shortcut_requests_settings(QGuiApplication& app)
{
    QQuickWindow window;
    window.resize(360, 240);
    VNM_TerminalSurface surface(window.contentItem());

    Recording_event_filter   key_filter(QEvent::KeyPress);
    Terminal_shortcut_filter shortcut_filter(&surface);
    window.installEventFilter(&key_filter);
    window.installEventFilter(&shortcut_filter);
    window.show();
    pump_events(app);
    surface.forceActiveFocus();
    pump_events(app);

    int settings_requests = 0;
    QObject::connect(
        &shortcut_filter,
        &Terminal_shortcut_filter::settings_requested,
        &shortcut_filter,
        [&settings_requests] {
            ++settings_requests;
        });

#if defined(Q_OS_MACOS)
    const Qt::KeyboardModifiers settings_modifier = Qt::MetaModifier;
#else
    const Qt::KeyboardModifiers settings_modifier = Qt::ControlModifier;
#endif

    QKeyEvent settings_event(QEvent::KeyPress, Qt::Key_Comma, settings_modifier);
    settings_event.setAccepted(false);
    QCoreApplication::sendEvent(&window, &settings_event);

    bool ok = true;
    ok &= check(settings_requests == 1,
        "the settings shortcut requests the panel exactly once");
    ok &= check(key_filter.recorded_count == 0,
        "the settings shortcut is consumed before reaching the terminal");

    QKeyEvent plain_comma(QEvent::KeyPress, Qt::Key_Comma, Qt::NoModifier);
    plain_comma.setAccepted(false);
    QCoreApplication::sendEvent(&window, &plain_comma);

    ok &= check(settings_requests == 1,
        "a plain comma key press does not request the settings panel");
    ok &= check(key_filter.recorded_count == 1,
        "a plain comma key press is delivered to the terminal");
    return ok;
}

bool test_renderer_mode_enum_assignment_from_qml(QGuiApplication& app)
{
    QQmlEngine engine;
    QQuickWindow window;
    VNM_TerminalSurface surface(window.contentItem());
    surface.set_text_renderer_mode(VNM_TerminalSurface::Text_renderer_mode::AUTO);

    QQmlComponent component(&engine);
    component.setData(
        QByteArrayLiteral(
            "import QtQuick\n"
            "Item {\n"
            "    property var target\n"
            "    function apply(value) { target.textRendererMode = value }\n"
            "}\n"),
        QUrl());
    std::unique_ptr<QObject> holder(component.create());

    bool ok = true;
    ok &= check(holder != nullptr, "renderer-binding QML holder compiles");
    if (holder == nullptr) {
        std::cerr << component.errorString().toStdString() << '\n';
        return ok;
    }

    holder->setProperty("target", QVariant::fromValue<QObject*>(&surface));

    QMetaObject::invokeMethod(holder.get(), "apply", Q_ARG(QVariant, QVariant(2)));
    pump_events(app);
    ok &= check(
        surface.text_renderer_mode() == VNM_TerminalSurface::Text_renderer_mode::GLYPH,
        "assigning int 2 to textRendererMode from QML selects GLYPH");

    QMetaObject::invokeMethod(holder.get(), "apply", Q_ARG(QVariant, QVariant(0)));
    pump_events(app);
    ok &= check(
        surface.text_renderer_mode() == VNM_TerminalSurface::Text_renderer_mode::AUTO,
        "assigning int 0 to textRendererMode from QML selects AUTO");
    return ok;
}

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
    ok &= test_parse_text_renderer_option();
    ok &= test_parse_lcd_subpixel_option();
    ok &= test_parse_row_timestamps_option();
    ok &= test_row_timestamp_tooltip_chrome(app);
    ok &= test_parse_scrollback_limit_option();
    ok &= test_parse_transcript_snapshot_diagnostics_option();
    ok &= test_parse_transcript_timing_diagnostics_option();
    ok &= test_paste_shortcut_should_paste_predicate();
    ok &= test_parse_paste_shortcut_option();
    ok &= test_window_state_sync();
    ok &= test_settings_gear_button_and_window(app);
    ok &= test_settings_shortcut_requests_settings(app);
    ok &= test_renderer_mode_enum_assignment_from_qml(app);
#if defined(Q_OS_MACOS)
    ok &= test_macos_command_shortcuts_are_host_shortcuts(app);
#endif
    return ok ? 0 : 1;
}
