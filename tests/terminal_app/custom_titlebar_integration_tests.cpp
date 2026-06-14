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
#include <QImage>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QTemporaryDir>
#include <QThread>
#include <QVariant>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
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

bool color_nearly_equal(
    const QColor& actual,
    const QColor& expected)
{
    constexpr int k_tolerance = 2;
    return
        std::abs(actual.red()   - expected.red())   <= k_tolerance &&
        std::abs(actual.green() - expected.green()) <= k_tolerance &&
        std::abs(actual.blue()  - expected.blue())  <= k_tolerance &&
        std::abs(actual.alpha() - expected.alpha()) <= k_tolerance;
}

std::string color_string(const QColor& color)
{
    return QStringLiteral("#%1%2%3%4")
        .arg(color.red(),   2, 16, QChar('0'))
        .arg(color.green(), 2, 16, QChar('0'))
        .arg(color.blue(),  2, 16, QChar('0'))
        .arg(color.alpha(), 2, 16, QChar('0'))
        .toStdString();
}

// Walks both the QObject child tree and the visual child-item tree, so it
// reaches Repeater-created delegates (e.g. custom titlebar buttons) that
// plain QObject::findChild does not. Mirrors the app's own find_child_object.
QObject* find_object_recursive(QObject* root, const QString& object_name)
{
    if (root == nullptr) {
        return nullptr;
    }

    if (root->objectName() == object_name) {
        return root;
    }

    const auto object_children = root->children();
    for (QObject* child : object_children) {
        if (QObject* found = find_object_recursive(child, object_name)) {
            return found;
        }
    }

    if (auto* item = qobject_cast<QQuickItem*>(root)) {
        const auto child_items = item->childItems();
        for (QQuickItem* child : child_items) {
            if (QObject* found = find_object_recursive(child, object_name)) {
                return found;
            }
        }
    }

    return nullptr;
}

QQuickItem* find_quick_item_recursive(QObject* root, const QString& object_name)
{
    return qobject_cast<QQuickItem*>(find_object_recursive(root, object_name));
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

bool wait_for_exposed_window(
    QGuiApplication& app,
    QWindow&         window)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        pump_events(app);
        if (window.isExposed()) {
            return true;
        }
        QThread::msleep(10);
    }

    return window.isExposed();
}

QRectF item_rect(const QQuickItem& item)
{
    return QRectF(item.x(), item.y(), item.width(), item.height());
}

QRectF expected_resize_disabled_content_interior_rect(
    const chrome_test::Terminal_qml_chrome& titlebar)
{
    const qreal root_width      = titlebar.root_item()->width();
    const qreal root_height     = titlebar.root_item()->height();
    const qreal titlebar_height = titlebar.titlebar_item()->height();
    const qreal frame_edge      =
        1.0 / vnm_qml_chrome::normalized_device_pixel_ratio(titlebar.device_pixel_ratio());

    return QRectF(
        frame_edge,
        titlebar_height + frame_edge,
        std::max<qreal>(0.0, root_width - 2.0 * frame_edge),
        std::max<qreal>(0.0, root_height - titlebar_height - 2.0 * frame_edge));
}

bool check_content_split_geometry(
    const QRectF&                         content_rect,
    const VNM_TerminalSurface&            surface,
    const chrome_test::Terminal_scrollbar& scrollbar,
    const std::string&                    terminal_message,
    const std::string&                    scrollbar_message)
{
    const qreal scrollbar_width =
        std::min(chrome_test::k_terminal_scrollbar_width, content_rect.width());
    const QRectF terminal_rect(
        content_rect.left(),
        content_rect.top(),
        std::max<qreal>(0.0, content_rect.width() - scrollbar_width),
        content_rect.height());
    const QRectF scrollbar_rect(
        content_rect.right() - scrollbar_width,
        content_rect.top(),
        scrollbar_width,
        content_rect.height());

    bool ok = true;
    ok &= check_rect_equal(item_rect(surface), terminal_rect, terminal_message);
    ok &= check_rect_equal(item_rect(scrollbar), scrollbar_rect, scrollbar_message);
    return ok;
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
    ok &= check(nearly_equal(titlebar.root_item()->property("content_interior_x").toReal(), 5.0),
        "shared frame shell exposes content interior x");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_interior_y").toReal(), 31.0),
        "shared frame shell exposes content interior y");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_interior_width").toReal(), 790.0),
        "shared frame shell exposes content interior width");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_interior_height").toReal(), 444.0),
        "shared frame shell exposes content interior height");

    ok &= check(
        find_object_recursive(
            titlebar.root_item(),
            QStringLiteral("terminal_chrome_native_window_frame")) == nullptr,
        "terminal chrome does not create an app-local native window frame");
    ok &= check(
        find_object_recursive(
            titlebar.root_item(),
            QStringLiteral("terminal_chrome_window_frame_top")) == nullptr,
        "terminal chrome does not create an app-local top window frame strip");
    ok &= check(
        find_object_recursive(
            titlebar.root_item(),
            QStringLiteral("terminal_chrome_window_frame_bottom")) == nullptr,
        "terminal chrome does not create an app-local bottom window frame strip");
    ok &= check(
        find_object_recursive(
            titlebar.root_item(),
            QStringLiteral("terminal_chrome_window_frame_left")) == nullptr,
        "terminal chrome does not create an app-local left window frame strip");
    ok &= check(
        find_object_recursive(
            titlebar.root_item(),
            QStringLiteral("terminal_chrome_window_frame_right")) == nullptr,
        "terminal chrome does not create an app-local right window frame strip");
    ok &= check(
        find_object_recursive(
            titlebar.root_item(),
            QStringLiteral("terminal_chrome_content_border_top")) == nullptr,
        "terminal chrome does not create an app-local top content border strip");
    ok &= check(
        find_object_recursive(
            titlebar.root_item(),
            QStringLiteral("terminal_chrome_content_border_bottom")) == nullptr,
        "terminal chrome does not create an app-local bottom content border strip");
    ok &= check(
        find_object_recursive(
            titlebar.root_item(),
            QStringLiteral("terminal_chrome_content_border_left")) == nullptr,
        "terminal chrome does not create an app-local left content border strip");
    ok &= check(
        find_object_recursive(
            titlebar.root_item(),
            QStringLiteral("terminal_chrome_content_border_right")) == nullptr,
        "terminal chrome does not create an app-local right content border strip");
    ok &= check(
        find_object_recursive(
            titlebar.root_item(),
            QStringLiteral("terminal_chrome_window_frame")) == nullptr,
        "terminal chrome does not create an app-local shared window frame wrapper");

    auto* frame_shell = find_quick_item_recursive(
        titlebar.root_item(),
        QStringLiteral("terminal_chrome_frame_shell"));
    ok &= check(frame_shell != nullptr, "terminal chrome creates the shared frame shell");
    if (frame_shell != nullptr) {
        ok &= check(nearly_equal(frame_shell->property("frame_outer_edge").toReal(), 1.0),
            "frame shell uses a one-pixel outer edge");
        ok &= check(nearly_equal(frame_shell->property("frame_gap").toReal(), 3.0),
            "frame shell preserves the three-pixel frame gap");
        ok &= check(nearly_equal(frame_shell->property("frame_inner_edge").toReal(), 1.0),
            "frame shell uses a one-pixel inner edge");
        ok &= check(nearly_equal(frame_shell->property("edge_resize_extent").toReal(), 4.0),
            "frame shell keeps the resize hit area independent from visible edges");
        ok &= check(frame_shell->property("resize_enabled").toBool(),
            "frame shell starts with resize hit areas enabled");
        ok &= check(
            frame_shell->property("frame_color").value<QColor>() ==
                chrome_test::terminal_chrome_background_color(true),
            "frame shell uses the active terminal frame fill color");
        ok &= check(
            frame_shell->property("frame_inner_edge_color").value<QColor>() ==
                chrome_test::terminal_chrome_frame_edge_color(true),
            "frame shell uses the active terminal inner-edge color");
        ok &= check_rect_equal(
            frame_shell->property("content_interior_rect").toRectF(),
            QRectF(5.0, 31.0, 790.0, 444.0),
            "frame shell reports the terminal content interior");
    }

    auto* shell_left_resize_area = find_quick_item_recursive(
        titlebar.root_item(),
        QStringLiteral("left_resize_area"));
    auto* shell_top_left_resize_area = find_quick_item_recursive(
        titlebar.root_item(),
        QStringLiteral("top_left_resize_area"));
    auto* shell_bottom_resize_area = find_quick_item_recursive(
        titlebar.root_item(),
        QStringLiteral("bottom_resize_area"));
    ok &= check(shell_left_resize_area != nullptr,
        "terminal shell creates a left resize area");
    ok &= check(shell_top_left_resize_area != nullptr,
        "terminal shell creates a top-left resize area");
    ok &= check(shell_bottom_resize_area != nullptr,
        "terminal shell creates a bottom resize area");
    if (shell_left_resize_area != nullptr &&
        shell_top_left_resize_area != nullptr &&
        shell_bottom_resize_area != nullptr)
    {
        ok &= check(shell_left_resize_area->property("enabled").toBool(),
            "terminal shell left resize area starts enabled");
        ok &= check(shell_top_left_resize_area->property("enabled").toBool(),
            "terminal shell top-left resize area starts enabled");
        ok &= check(shell_bottom_resize_area->property("enabled").toBool(),
            "terminal shell bottom resize area starts enabled");
    }

    auto* window_frame = find_quick_item_recursive(
        titlebar.root_item(),
        QStringLiteral("chrome_frame_shell_outer_frame"));
    ok &= check(window_frame != nullptr, "frame shell creates shared window frame overlay");
    if (window_frame != nullptr) {
        ok &= check(nearly_equal(window_frame->property("frame_width").toReal(), 1.0),
            "shared window frame uses the outer edge thickness");
        ok &= check(
            window_frame->property("top_edge_visible").toBool() == false,
            "shared window frame delegates the top edge to the titlebar");
        ok &= check(
            window_frame->property("enabled").toBool() == false,
            "shared window frame overlay is non-interactive");
    }

    auto* frame_top = window_frame == nullptr
        ? nullptr
        : find_quick_item_recursive(
            window_frame,
            QStringLiteral("chrome_window_frame_top"));
    auto* frame_bottom = window_frame == nullptr
        ? nullptr
        : find_quick_item_recursive(
            window_frame,
            QStringLiteral("chrome_window_frame_bottom"));
    auto* frame_left = window_frame == nullptr
        ? nullptr
        : find_quick_item_recursive(
            window_frame,
            QStringLiteral("chrome_window_frame_left"));
    auto* frame_right = window_frame == nullptr
        ? nullptr
        : find_quick_item_recursive(
            window_frame,
            QStringLiteral("chrome_window_frame_right"));
    ok &= check(frame_top    != nullptr, "shared chrome creates top window frame edge");
    ok &= check(frame_bottom != nullptr, "shared chrome creates bottom window frame edge");
    ok &= check(frame_left   != nullptr, "shared chrome creates left window frame edge");
    ok &= check(frame_right  != nullptr, "shared chrome creates right window frame edge");
    if (frame_top != nullptr && frame_bottom != nullptr &&
        frame_left != nullptr && frame_right != nullptr)
    {
        ok &= check(!frame_top->isVisible(),
            "shared window frame top edge is disabled for terminal titlebar ownership");
        ok &= check_rect_equal(item_rect(*frame_bottom), QRectF(0.0, 479.0, 800.0, 1.0),
            "shared bottom window frame overlays the outer bottom edge");
        ok &= check_rect_equal(item_rect(*frame_left), QRectF(0.0, 0.0, 1.0, 480.0),
            "shared left window frame overlays the full outer left edge");
        ok &= check_rect_equal(item_rect(*frame_right), QRectF(799.0, 0.0, 1.0, 480.0),
            "shared right window frame overlays the full outer right edge");
    }

    auto* titlebar_frame_top = find_quick_item_recursive(
        titlebar.titlebar_item(),
        QStringLiteral("titlebar_window_frame_top"));
    ok &= check(titlebar_frame_top != nullptr,
        "shared titlebar creates the top window frame edge");
    if (titlebar_frame_top != nullptr) {
        ok &= check_rect_equal(item_rect(*titlebar_frame_top), QRectF(0.0, 0.0, 800.0, 1.0),
            "shared titlebar top window frame overlays the outer top edge");
        ok &= check(
            titlebar_frame_top->property("color").value<QColor>() ==
                chrome_test::terminal_chrome_frame_edge_color(true),
            "shared titlebar top window frame uses the frame edge color");

        auto* mark = find_quick_item_recursive(
            titlebar.titlebar_item(),
            QStringLiteral("vnm_animated_mark"));
        if (mark != nullptr && mark->parentItem() != nullptr) {
            ok &= check(mark->parentItem()->z() > titlebar_frame_top->z(),
                "titlebar top window frame is below the titlebar content layer");
        }
    }

    if (frame_shell != nullptr && titlebar_frame_top != nullptr) {
        titlebar.set_active(false);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        ok &= check(
            frame_shell->property("frame_color").value<QColor>() ==
                chrome_test::terminal_chrome_background_color(false),
            "frame shell uses the inactive terminal frame fill color");
        ok &= check(
            frame_shell->property("frame_outer_edge_color").value<QColor>() ==
                chrome_test::terminal_chrome_frame_edge_color(false),
            "frame shell uses the inactive terminal outer-edge color");
        ok &= check(
            frame_shell->property("frame_inner_edge_color").value<QColor>() ==
                chrome_test::terminal_chrome_frame_edge_color(false),
            "frame shell uses the inactive terminal inner-edge color");
        ok &= check(
            titlebar_frame_top->property("color").value<QColor>() ==
                chrome_test::terminal_chrome_frame_edge_color(false),
            "shared titlebar top window frame uses the inactive edge color");
        titlebar.set_active(true);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }

    auto* inner_top = find_quick_item_recursive(
        titlebar.root_item(),
        QStringLiteral("chrome_frame_shell_inner_edge_top"));
    auto* inner_bottom = find_quick_item_recursive(
        titlebar.root_item(),
        QStringLiteral("chrome_frame_shell_inner_edge_bottom"));
    auto* inner_left = find_quick_item_recursive(
        titlebar.root_item(),
        QStringLiteral("chrome_frame_shell_inner_edge_left"));
    auto* inner_right = find_quick_item_recursive(
        titlebar.root_item(),
        QStringLiteral("chrome_frame_shell_inner_edge_right"));
    ok &= check(inner_top    != nullptr, "frame shell creates top inner edge");
    ok &= check(inner_bottom != nullptr, "frame shell creates bottom inner edge");
    ok &= check(inner_left   != nullptr, "frame shell creates left inner edge");
    ok &= check(inner_right  != nullptr, "frame shell creates right inner edge");
    if (inner_top != nullptr && inner_bottom != nullptr &&
        inner_left != nullptr && inner_right != nullptr)
    {
        ok &= check_rect_equal(item_rect(*inner_top), QRectF(4.0, 30.0, 792.0, 1.0),
            "frame shell top inner edge spans the content frame");
        ok &= check_rect_equal(item_rect(*inner_bottom), QRectF(4.0, 475.0, 792.0, 1.0),
            "frame shell bottom inner edge spans the content frame");
        ok &= check_rect_equal(item_rect(*inner_left), QRectF(4.0, 30.0, 1.0, 446.0),
            "frame shell left inner edge stays at x=4");
        ok &= check_rect_equal(item_rect(*inner_right), QRectF(795.0, 30.0, 1.0, 446.0),
            "frame shell right inner edge stays at the content boundary");
    }

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
    QQmlEngine hidpi_engine;
    QQuickWindow hidpi_window;
    hidpi_window.resize(1920, 1080);
    chrome_test::Terminal_qml_chrome hidpi_titlebar(hidpi_engine, hidpi_window);
    VNM_TerminalSurface hidpi_surface(hidpi_window.contentItem());
    chrome_test::Terminal_scrollbar hidpi_scrollbar(hidpi_window.contentItem());
    ok &= check(hidpi_titlebar.is_valid(), "fractional-DPR shared frame shell initializes");
    if (!hidpi_titlebar.is_valid()) {
        std::cerr << hidpi_titlebar.error_string().toStdString() << '\n';
        return ok;
    }
    ok &= check(
        hidpi_titlebar.root_item()->setProperty("device_pixel_ratio", hidpi_dpr),
        "fractional-DPR fixture overrides the shell device-pixel ratio");
    apply_terminal_shell_geometry(
        hidpi_window,
        hidpi_surface,
        hidpi_scrollbar,
        &hidpi_titlebar,
        true);

    const QRectF hidpi_content_interior = hidpi_titlebar.content_interior_rect();
    ok &= check_rect_equal(
        hidpi_content_interior,
        QRectF(5.6, 31.2, 1908.8, 1043.2),
        "custom titlebar content interior comes from the shell at fractional DPR");
    ok &= check_rect_equal(
        item_rect(hidpi_surface),
        QRectF(5.6, 31.2, 1896.8, 1043.2),
        "custom titlebar terminal rect rounds to physical pixels at fractional DPR");
    ok &= check_rect_equal(
        item_rect(hidpi_scrollbar),
        QRectF(1902.4, 31.2, 12.0, 1043.2),
        "custom titlebar scrollbar rect rounds to physical pixels at fractional DPR");
    ok &= check(
        vnm_qml_chrome::rect_has_snapped_physical_edges(
            hidpi_content_interior,
            hidpi_dpr),
        "custom titlebar content interior edges are physical-pixel aligned");
    ok &= check(
        vnm_qml_chrome::rect_has_snapped_physical_edges(
            item_rect(hidpi_surface),
            hidpi_dpr),
        "custom titlebar terminal edges are physical-pixel aligned");
    ok &= check(
        vnm_qml_chrome::rect_has_snapped_physical_edges(
            item_rect(hidpi_scrollbar),
            hidpi_dpr),
        "custom titlebar scrollbar edges are physical-pixel aligned");
    ok &= check(
        nearly_equal(
            hidpi_surface.x() + hidpi_surface.width(),
            hidpi_scrollbar.x()),
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
    sync_chrome_window_state(titlebar, window);
    apply_terminal_shell_geometry(window, surface, scrollbar, &titlebar, true);
    const QRectF maximized_content_interior = titlebar.content_interior_rect();
    ok &= check_rect_equal(
        maximized_content_interior,
        expected_resize_disabled_content_interior_rect(titlebar),
        "maximized shared frame shell drops inactive resize border gutters");
    if (frame_shell != nullptr) {
        ok &= check(nearly_equal(frame_shell->property("frame_gap").toReal(), 0.0),
            "maximized shared frame shell collapses the resize-only frame gap");
        ok &= check(!frame_shell->property("resize_enabled").toBool(),
            "maximized shared frame shell disables resize hit areas");
    }
    if (shell_left_resize_area != nullptr &&
        shell_top_left_resize_area != nullptr &&
        shell_bottom_resize_area != nullptr)
    {
        ok &= check(!shell_left_resize_area->property("enabled").toBool(),
            "maximized terminal shell left resize area is disabled");
        ok &= check(!shell_top_left_resize_area->property("enabled").toBool(),
            "maximized terminal shell top-left resize area is disabled");
        ok &= check(!shell_bottom_resize_area->property("enabled").toBool(),
            "maximized terminal shell bottom resize area is disabled");
    }
    ok &= check_content_split_geometry(
        maximized_content_interior,
        surface,
        scrollbar,
        "maximized custom terminal consumes the shell content interior",
        "maximized custom scrollbar remains inside content bounds");

    window.setWindowStates(Qt::WindowFullScreen);
    sync_chrome_window_state(titlebar, window);
    apply_terminal_shell_geometry(window, surface, scrollbar, &titlebar, true);
    ok &= check(titlebar.root_item()->property("fullscreen").toBool(),
        "fullscreen shared frame shell tracks fullscreen state separately");
    ok &= check(!titlebar.root_item()->property("resize_enabled").toBool(),
        "fullscreen shared frame shell disables resize hit areas");
    if (frame_shell != nullptr) {
        ok &= check(nearly_equal(frame_shell->property("frame_outer_edge").toReal(), 0.0),
            "fullscreen shared frame shell hides the outer frame edge");
        ok &= check(nearly_equal(frame_shell->property("frame_gap").toReal(), 0.0),
            "fullscreen shared frame shell removes the resize-only frame gap");
        ok &= check(nearly_equal(frame_shell->property("frame_inner_edge").toReal(), 1.0),
            "fullscreen shared frame shell preserves the inner content edge");
        ok &= check(!frame_shell->property("resize_enabled").toBool(),
            "fullscreen shared frame shell disables resize hit areas");
    }
    if (shell_left_resize_area != nullptr &&
        shell_top_left_resize_area != nullptr &&
        shell_bottom_resize_area != nullptr)
    {
        ok &= check(!shell_left_resize_area->property("enabled").toBool(),
            "fullscreen terminal shell left resize area is disabled");
        ok &= check(!shell_top_left_resize_area->property("enabled").toBool(),
            "fullscreen terminal shell top-left resize area is disabled");
        ok &= check(!shell_bottom_resize_area->property("enabled").toBool(),
            "fullscreen terminal shell bottom resize area is disabled");
    }
    const QRectF fullscreen_content_interior = titlebar.content_interior_rect();
    ok &= check_rect_equal(
        fullscreen_content_interior,
        expected_resize_disabled_content_interior_rect(titlebar),
        "fullscreen shared frame shell uses the resize-disabled content interior");
    ok &= check_content_split_geometry(
        fullscreen_content_interior,
        surface,
        scrollbar,
        "fullscreen custom terminal uses the shell content interior",
        "fullscreen custom scrollbar remains inside shell content bounds");

    window.setWindowStates(Qt::WindowNoState);
    sync_chrome_window_state(titlebar, window);

    window.resize(8, 40);
    apply_terminal_shell_geometry(window, surface, scrollbar, &titlebar, true);
    ok &= check(nearly_equal(titlebar.root_item()->property("content_interior_x").toReal(), 5.0),
        "very narrow shared frame shell keeps the content interior at the shell inset");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_interior_width").toReal(), 0.0),
        "very narrow shared frame shell clamps content interior width");
    ok &= check_rect_equal(item_rect(surface), QRectF(5.0, 31.0, 0.0, 4.0),
        "very narrow custom terminal consumes the shell content interior");
    ok &= check_rect_equal(item_rect(scrollbar), QRectF(5.0, 31.0, 0.0, 4.0),
        "very narrow custom scrollbar clamps inside shell content bounds");

    window.resize(200, 20);
    apply_terminal_shell_geometry(window, surface, scrollbar, &titlebar, true);
    ok &= check_rect_equal(item_rect(*titlebar.titlebar_item()), QRectF(0.0, 0.0, 200.0, 20.0),
        "very short custom titlebar clamps to window height");
    ok &= check(nearly_equal(titlebar.root_item()->property("content_interior_height").toReal(), 0.0),
        "very short shared frame shell clamps content interior height");
    ok &= check_rect_equal(item_rect(surface), QRectF(5.0, 21.0, 178.0, 0.0),
        "very short custom terminal consumes the shell content interior");
    ok &= check_rect_equal(item_rect(scrollbar), QRectF(183.0, 21.0, 12.0, 0.0),
        "very short custom scrollbar clamps nonnegative height");

    window.resize(360, 240);
    apply_terminal_shell_geometry(window, surface, scrollbar, nullptr, false);
    ok &= check_rect_equal(item_rect(surface), QRectF(0.0, 0.0, 348.0, 240.0),
        "native-decoration path reserves scrollbar gutter");
    ok &= check_rect_equal(item_rect(scrollbar), QRectF(348.0, 0.0, 12.0, 240.0),
        "native-decoration path positions scrollbar at right edge");

    return ok;
}

bool test_terminal_chrome_native_outer_edge_pixels(QGuiApplication& app)
{
    if (QGuiApplication::platformName().compare(
            QStringLiteral("offscreen"),
            Qt::CaseInsensitive) == 0)
    {
        std::cout
            << "SKIP: native outer-edge pixel coverage requires a real window platform\n";
        return true;
    }

    const QColor expected_color = chrome_test::terminal_chrome_frame_edge_color(true);
    const std::vector<QSize> render_sizes{
        QSize(492, 418),
        QSize(493, 419),
        QSize(522, 392),
        QSize(554, 488),
        QSize(555, 489),
    };

    bool ok = true;
    for (const QSize& render_size : render_sizes) {
        QQmlEngine engine;
        QQuickWindow window;
        window.setFlags(window.flags() | Qt::FramelessWindowHint);
        window.setColor(chrome_test::terminal_chrome_background_color(true));
        window.setPosition(80, 80);
        window.resize(render_size);

        chrome_test::Terminal_qml_chrome titlebar(engine, window);
        VNM_TerminalSurface surface(window.contentItem());
        chrome_test::Terminal_scrollbar scrollbar(window.contentItem());
        ok &= check(titlebar.is_valid(), "native pixel fixture creates terminal chrome");
        if (!titlebar.is_valid()) {
            std::cerr << titlebar.error_string().toStdString() << '\n';
            return ok;
        }

        titlebar.set_active(true);
        titlebar.set_maximized(false);
        titlebar.set_fullscreen(false);
        titlebar.set_resize_enabled(true);
        titlebar.set_size(QSizeF(render_size));
        apply_terminal_shell_geometry(
            window,
            surface,
            scrollbar,
            &titlebar,
            true);
        auto backend = std::make_unique<Metadata_seed_backend>(
            QByteArrayLiteral("native outer edge pixel fixture\r\n"));
        const bool started = term::VNM_TerminalSurface_render_bridge::start_process_with_backend(
            surface,
            std::move(backend),
            {QStringLiteral("native-outer-edge-pixel-fixture")});
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(surface);
        ok &= check(started, "native pixel fixture seeds terminal surface content");

        window.show();
        window.raise();
        window.requestActivate();
        ok &= check(wait_for_exposed_window(app, window),
            "native pixel fixture exposes the frameless window");
        if (!window.isExposed()) {
            window.close();
            return ok;
        }

        const qreal device_pixel_ratio = window.devicePixelRatio();
        if (nearly_equal(device_pixel_ratio, std::round(device_pixel_ratio))) {
            std::cout
                << "SKIP: native outer-edge pixel coverage requires fractional DPR; "
                << "current DPR is " << device_pixel_ratio << '\n';
            window.close();
            pump_events(app);
            return true;
        }
        ok &= check(
            titlebar.root_item()->setProperty(
                "device_pixel_ratio",
                device_pixel_ratio),
            "native pixel fixture uses the exposed window device-pixel ratio");
        titlebar.set_size(QSizeF(render_size));
        apply_terminal_shell_geometry(
            window,
            surface,
            scrollbar,
            &titlebar,
            true);
        pump_events(app);

        QScreen* screen = window.screen();
        ok &= check(screen != nullptr,
            "native pixel fixture has a screen for visible pixel capture");
        if (screen == nullptr) {
            window.close();
            return ok;
        }

        const QRect frame_geometry = window.frameGeometry();
        const QImage image = screen->grabWindow(
            0,
            frame_geometry.x(),
            frame_geometry.y(),
            frame_geometry.width(),
            frame_geometry.height()).toImage();
        ok &= check(!image.isNull(), "native pixel fixture grabs desktop-composited window pixels");
        if (image.isNull()) {
            window.close();
            return ok;
        }
        const int right_x = image.width() - 1;
        const int bottom_y = image.height() - 1;
        for (int y = 0; y < image.height(); ++y) {
            const QColor actual_color = image.pixelColor(right_x, y);
            if (!color_nearly_equal(actual_color, expected_color)) {
                std::cerr
                    << "FAIL: native terminal chrome right outer edge visible"
                    << " size=(" << render_size.width() << ", "
                    << render_size.height() << ")"
                    << " image=(" << image.width() << ", "
                    << image.height() << ")"
                    << " dpr=" << device_pixel_ratio
                    << " pixel=(" << right_x << ", " << y << ")"
                    << " expected=" << color_string(expected_color)
                    << " actual=" << color_string(actual_color) << '\n';
                ok = false;
                break;
            }
        }

        for (int x = 0; x < image.width(); ++x) {
            const QColor actual_color = image.pixelColor(x, bottom_y);
            if (!color_nearly_equal(actual_color, expected_color)) {
                std::cerr
                    << "FAIL: native terminal chrome bottom outer edge visible"
                    << " size=(" << render_size.width() << ", "
                    << render_size.height() << ")"
                    << " image=(" << image.width() << ", "
                    << image.height() << ")"
                    << " dpr=" << device_pixel_ratio
                    << " pixel=(" << x << ", " << bottom_y << ")"
                    << " expected=" << color_string(expected_color)
                    << " actual=" << color_string(actual_color) << '\n';
                ok = false;
                break;
            }
        }

        window.close();
        pump_events(app);
    }

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

bool test_window_chrome_setup_honors_titlebar_mode()
{
    bool ok = true;
    App_options native_options;
    native_options.custom_titlebar = false;
    native_options.window_size = QSize(321, 234);

    QQmlEngine native_engine;
    QQuickWindow native_window;
    Terminal_window_chrome_setup native_setup =
        setup_terminal_window_chrome(
            native_engine,
            native_window,
            QIcon(),
            native_options);
    ok &= check(native_setup.error.isEmpty(),
        "native-titlebar window chrome setup succeeds");
    ok &= check(native_setup.titlebar == nullptr,
        "native-titlebar mode skips Terminal_qml_chrome construction");
    ok &= check(
        find_object_recursive(
            native_window.contentItem(),
            QStringLiteral("terminal_qml_chrome_root")) == nullptr,
        "native-titlebar mode leaves no terminal QML chrome root");
    ok &= check(!native_window.flags().testFlag(Qt::FramelessWindowHint),
        "native-titlebar mode keeps native window decorations");
    ok &= check(native_window.color() == QColor(9, 12, 16),
        "native-titlebar mode keeps the native window background color");
    ok &= check(native_window.size() == native_options.window_size,
        "window chrome setup applies the requested window size");

#if defined(_WIN32) || defined(__linux__)
    App_options custom_options;
    custom_options.custom_titlebar = true;
    custom_options.window_size = QSize(345, 210);

    QQmlEngine custom_engine;
    QQuickWindow custom_window;
    Terminal_window_chrome_setup custom_setup =
        setup_terminal_window_chrome(
            custom_engine,
            custom_window,
            QIcon(),
            custom_options);
    ok &= check(custom_setup.error.isEmpty(),
        "custom-titlebar window chrome setup succeeds");
    ok &= check(custom_setup.titlebar != nullptr,
        "custom-titlebar mode constructs Terminal_qml_chrome");
    if (custom_setup.titlebar != nullptr) {
        ok &= check(custom_setup.titlebar->is_valid(),
            "custom-titlebar mode creates valid QML chrome");
        ok &= check(
            find_object_recursive(
                custom_window.contentItem(),
                QStringLiteral("terminal_qml_chrome_root")) != nullptr,
            "custom-titlebar mode adds the terminal QML chrome root");
    }
    ok &= check(custom_window.flags().testFlag(Qt::FramelessWindowHint),
        "custom-titlebar mode enables frameless window decorations");
    ok &= check(
        custom_window.color() ==
            chrome_test::terminal_chrome_background_color(custom_window.isActive()),
        "custom-titlebar mode uses the terminal chrome background color");
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

bool test_parse_alternate_wheel_option()
{
    Parse_result default_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result page_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--alternate-wheel"),
        QStringLiteral("page"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result cursor_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--alternate-wheel"),
        QStringLiteral("cursor"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result mixed_case_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--alternate-wheel"),
        QStringLiteral("MOUSE"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result invalid_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--alternate-wheel"),
        QStringLiteral("diagonal"),
    });

    using Wheel_policy = VNM_TerminalSurface::Alternate_screen_wheel_policy;
    bool ok = true;
    ok &= check(default_result.error.isEmpty(), "alternate wheel default parse succeeds");
    ok &= check(
        default_result.options.alternate_screen_wheel_policy ==
            Wheel_policy::MOUSE_REPORTING_FIRST,
        "alternate wheel defaults to mouse-reporting-first");
    ok &= check(page_result.error.isEmpty(), "alternate wheel page option parses");
    ok &= check(
        page_result.options.alternate_screen_wheel_policy == Wheel_policy::PAGE_KEYS,
        "alternate wheel page maps to page keys");
    ok &= check(cursor_result.error.isEmpty(), "alternate wheel cursor option parses");
    ok &= check(
        cursor_result.options.alternate_screen_wheel_policy == Wheel_policy::CURSOR_KEYS,
        "alternate wheel cursor maps to cursor keys");
    ok &= check(mixed_case_result.error.isEmpty(), "alternate wheel mixed-case option parses");
    ok &= check(
        mixed_case_result.options.alternate_screen_wheel_policy ==
            Wheel_policy::MOUSE_REPORTING_FIRST,
        "alternate wheel option is case-insensitive");
    ok &= check(!invalid_result.error.isEmpty(),
        "alternate wheel option rejects invalid values");

    return ok;
}

bool test_parse_osc52_clipboard_option()
{
    Parse_result default_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result allow_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--osc52-clipboard"),
        QStringLiteral("allow"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result deny_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--osc52-clipboard"),
        QStringLiteral("DENY"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result invalid_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--osc52-clipboard"),
        QStringLiteral("ask"),
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(), "osc52 clipboard default parse succeeds");
    ok &= check(
        default_result.options.osc52_clipboard_policy == Osc52_clipboard_policy::DENY,
        "osc52 clipboard defaults to deny");
    ok &= check(allow_result.error.isEmpty(), "osc52 clipboard allow option parses");
    ok &= check(
        allow_result.options.osc52_clipboard_policy == Osc52_clipboard_policy::ALLOW,
        "osc52 clipboard allow maps to allow");
    ok &= check(deny_result.error.isEmpty(), "osc52 clipboard mixed-case deny option parses");
    ok &= check(
        deny_result.options.osc52_clipboard_policy == Osc52_clipboard_policy::DENY,
        "osc52 clipboard option is case-insensitive");
    ok &= check(!invalid_result.error.isEmpty(),
        "osc52 clipboard option rejects invalid values");

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

bool test_parse_metrics_timeline_options()
{
    Parse_result default_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result timeline_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--metrics-timeline-jsonl"),
        QStringLiteral("timeline.jsonl"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result interval_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--metrics-timeline-jsonl"),
        QStringLiteral("timeline.jsonl"),
        QStringLiteral("--metrics-timeline-interval-ms"),
        QStringLiteral("250"),
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });

    Parse_result invalid_interval_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--metrics-timeline-interval-ms"),
        QStringLiteral("0"),
    });

    Parse_result overflowing_interval_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--metrics-timeline-interval-ms"),
        QStringLiteral("2147483648"),
    });

    Parse_result command_result = parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--"),
        QStringLiteral("--metrics-timeline-jsonl"),
        QStringLiteral("timeline.jsonl"),
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(),
        "metrics timeline default parse succeeds");
    ok &= check(default_result.options.metrics_timeline_jsonl_path.isEmpty(),
        "metrics timeline path defaults empty");
    ok &= check(
        default_result.options.metrics_timeline_interval_ms ==
            chrome_test::k_default_metrics_timeline_interval_ms,
        "metrics timeline interval defaults to 5000 ms");
    ok &= check(timeline_result.error.isEmpty(),
        "metrics timeline path option parses");
    ok &= check(
        timeline_result.options.metrics_timeline_jsonl_path == QStringLiteral("timeline.jsonl"),
        "metrics timeline path option stores the requested path");
    ok &= check(
        timeline_result.options.metrics_timeline_interval_ms ==
            chrome_test::k_default_metrics_timeline_interval_ms,
        "metrics timeline path option keeps the default interval");
    ok &= check(interval_result.error.isEmpty(),
        "metrics timeline interval option parses");
    ok &= check(interval_result.options.metrics_timeline_interval_ms == 250,
        "metrics timeline interval option stores the requested interval");
    ok &= check(!invalid_interval_result.error.isEmpty(),
        "metrics timeline interval rejects zero");
    ok &= check(
        invalid_interval_result.error ==
            QStringLiteral("--metrics-timeline-interval-ms requires a positive integer"),
        "metrics timeline interval reports the documented error");
    ok &= check(!overflowing_interval_result.error.isEmpty(),
        "metrics timeline interval rejects values beyond int32");
    ok &= check(command_result.error.isEmpty(),
        "metrics timeline option after command separator parses as command argv");
    ok &= check(command_result.options.metrics_timeline_jsonl_path.isEmpty(),
        "metrics timeline option after command separator leaves default path");
    ok &= check(
        command_result.options.command ==
            QStringList{
                QStringLiteral("--metrics-timeline-jsonl"),
                QStringLiteral("timeline.jsonl"),
            },
        "metrics timeline option after command separator is preserved in command argv");

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "metrics timeline conflict test creates temp directory");
    if (temp_dir.isValid()) {
        const QString shared_path = temp_dir.filePath(QStringLiteral("metrics.json"));
        Parse_result conflict_result = parse_arguments({
            QStringLiteral("vnm_terminal"),
            QStringLiteral("--metrics-json"),
            shared_path,
            QStringLiteral("--metrics-timeline-jsonl"),
            shared_path,
            QStringLiteral("--"),
            QStringLiteral("fixture-command"),
        });

        QString validation_error;
        ok &= check(conflict_result.error.isEmpty(),
            "metrics timeline conflict input parses before path validation");
        ok &= check(!validate_capture_paths(&conflict_result.options, &validation_error),
            "metrics timeline rejects sharing the aggregate metrics path");
        ok &= check(
            validation_error.contains(
                QStringLiteral("--metrics-json and --metrics-timeline-jsonl")),
            "metrics timeline conflict reports the conflicting options");
    }

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
    const QRectF signal_maximized_content_interior = signal_titlebar.content_interior_rect();
    ok &= check_rect_equal(
        signal_maximized_content_interior,
        expected_resize_disabled_content_interior_rect(signal_titlebar),
        "windowStateChanged connection reapplies maximized shell geometry");
    ok &= check_content_split_geometry(
        signal_maximized_content_interior,
        signal_surface,
        signal_scrollbar,
        "windowStateChanged connection reapplies maximized terminal geometry",
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

bool test_clipboard_broker_mode_argument_detection()
{
    using chrome_test::clipboard_broker_mode_requested;

    bool ok = true;
    ok &= check(
        clipboard_broker_mode_requested({
            QStringLiteral("vnm_terminal"),
            QStringLiteral("--vnm-terminal-internal-read-clipboard-text"),
        }),
        "clipboard broker mode accepts the exact internal invocation");
    ok &= check(
        !clipboard_broker_mode_requested({
            QStringLiteral("vnm_terminal"),
            QStringLiteral("--"),
            QStringLiteral("--vnm-terminal-internal-read-clipboard-text"),
        }),
        "clipboard broker mode ignores the internal flag after the command separator");
    ok &= check(
        !clipboard_broker_mode_requested({
            QStringLiteral("vnm_terminal"),
            QStringLiteral("--help"),
            QStringLiteral("--vnm-terminal-internal-read-clipboard-text"),
        }),
        "clipboard broker mode ignores mixed user options");
    return ok;
}

bool test_paste_shortcut_consumes_null_clipboard_reader(QGuiApplication& app)
{
    QQuickWindow window;
    window.resize(360, 240);
    VNM_TerminalSurface surface(window.contentItem());
    surface.set_clipboard_text_reader([]() -> std::optional<QString> {
        return std::nullopt;
    });

    Recording_event_filter key_filter(QEvent::KeyPress);
    Terminal_shortcut_filter shortcut_filter(&surface);
    window.installEventFilter(&key_filter);
    window.installEventFilter(&shortcut_filter);
    window.show();
    pump_events(app);
    surface.forceActiveFocus();
    pump_events(app);

    QKeyEvent paste_event(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
    paste_event.setAccepted(false);
    const bool paste_sent = QCoreApplication::sendEvent(&window, &paste_event);

    bool ok = true;
    ok &= check(paste_sent, "null-reader paste shortcut event is delivered");
    ok &= check(key_filter.recorded_count == 0,
        "null-reader paste shortcut is consumed before reaching the terminal");
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
    if (QCoreApplication::arguments().contains(
            QStringLiteral("--native-outer-edge-pixels")))
    {
        return test_terminal_chrome_native_outer_edge_pixels(app) ? 0 : 1;
    }

    bool ok = true;
    ok &= test_custom_titlebar_geometry();
    ok &= test_terminal_scrollbar_tracks_surface_viewport(app);
    ok &= test_terminal_scrollbar_immediate_public_projection_routes(app);
    ok &= test_title_sync_and_button_rect_offsets(app);
    ok &= test_parse_titlebar_options();
    ok &= test_window_chrome_setup_honors_titlebar_mode();
    ok &= test_parse_selection_trace_option();
    ok &= test_parse_wheel_trace_option();
    ok &= test_parse_synchronized_output_scroll_policy_option();
    ok &= test_parse_disable_primary_repaint_recovery_option();
    ok &= test_parse_text_renderer_option();
    ok &= test_parse_lcd_subpixel_option();
    ok &= test_parse_row_timestamps_option();
    ok &= test_parse_alternate_wheel_option();
    ok &= test_parse_osc52_clipboard_option();
    ok &= test_row_timestamp_tooltip_chrome(app);
    ok &= test_parse_scrollback_limit_option();
    ok &= test_parse_metrics_timeline_options();
    ok &= test_parse_transcript_snapshot_diagnostics_option();
    ok &= test_parse_transcript_timing_diagnostics_option();
    ok &= test_paste_shortcut_should_paste_predicate();
    ok &= test_parse_paste_shortcut_option();
    ok &= test_clipboard_broker_mode_argument_detection();
    ok &= test_paste_shortcut_consumes_null_clipboard_reader(app);
    ok &= test_window_state_sync();
    ok &= test_settings_gear_button_and_window(app);
    ok &= test_settings_shortcut_requests_settings(app);
    ok &= test_renderer_mode_enum_assignment_from_qml(app);
#if defined(Q_OS_MACOS)
    ok &= test_macos_command_shortcuts_are_host_shortcuts(app);
#endif
    return ok ? 0 : 1;
}
