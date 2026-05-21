#include "frameless_resize_filter.h"
#include "window_chrome.h"
#include "helpers/test_check.h"

#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>
#include <QQuickWindow>
#include <QRectF>
#include <QSizeF>
#include <QWindow>
#include <QWheelEvent>

#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

namespace chrome = vnm_terminal::terminal_app;

namespace {

using vnm_terminal::test_helpers::check;

bool check_edges_equal(
    Qt::Edges          actual,
    Qt::Edges          expected,
    const std::string& message)
{
    if (actual == expected) {
        return true;
    }

    std::cerr << "FAIL: " << message
        << " expected=" << static_cast<int>(expected)
        << " actual="   << static_cast<int>(actual) << '\n';
    return false;
}

bool check_cursor_shape_equal(
    Qt::CursorShape    actual,
    Qt::CursorShape    expected,
    const std::string& message)
{
    if (actual == expected) {
        return true;
    }

    std::cerr << "FAIL: " << message
        << " expected=" << static_cast<int>(expected)
        << " actual="   << static_cast<int>(actual) << '\n';
    return false;
}

bool check_edges_log_equal(
    const std::vector<Qt::Edges>&      actual,
    std::initializer_list<Qt::Edges>   expected,
    const std::string&                 message)
{
    if (actual.size() != expected.size()) {
        std::cerr << "FAIL: " << message
            << " expected-size=" << expected.size()
            << " actual-size="   << actual.size() << '\n';
        return false;
    }

    bool        ok    = true;
    std::size_t index = 0;
    for (Qt::Edges expected_edges : expected) {
        ok &= check_edges_equal(
            actual[index],
            expected_edges,
            message + " edges " + std::to_string(index));
        ++index;
    }
    return ok;
}

class Recording_resize_filter final : public chrome::Frameless_resize_filter
{
public:
    using chrome::Frameless_resize_filter::Frameless_resize_filter;

    std::vector<Qt::Edges> resize_edges_log;
    bool                   resize_return_value = true;

protected:
    bool start_system_resize(Qt::Edges edges) override
    {
        resize_edges_log.push_back(edges);
        return resize_return_value;
    }
};

class Recording_window_chrome final : public chrome::Terminal_window_chrome
{
public:
    using chrome::Terminal_window_chrome::Terminal_window_chrome;

    bool send_mouse_press(const QPointF& point)
    {
        QMouseEvent event(
            QEvent::MouseButtonPress,
            point,
            point,
            point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier);
        event.setAccepted(false);
        mousePressEvent(&event);
        return event.isAccepted();
    }

    bool send_mouse_double_click(const QPointF& point)
    {
        QMouseEvent event(
            QEvent::MouseButtonDblClick,
            point,
            point,
            point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier);
        event.setAccepted(false);
        mouseDoubleClickEvent(&event);
        return event.isAccepted();
    }

    std::vector<chrome::Window_chrome_command> commands;

protected:
    void invoke_window_command(chrome::Window_chrome_command command) override
    {
        commands.push_back(command);
    }
};

bool send_mouse_event(
    chrome::Frameless_resize_filter&   filter,
    QWindow&                           window,
    QEvent::Type                       type,
    const QPointF&                     point,
    Qt::MouseButton                    button = Qt::NoButton,
    Qt::MouseButtons                   buttons = Qt::NoButton)
{
    QMouseEvent event(
        type,
        point,
        point,
        point,
        button,
        buttons,
        Qt::NoModifier);
    event.setAccepted(false);
    return filter.eventFilter(&window, &event);
}

struct Mouse_filter_result
{
    bool filter_result    = false;
    bool event_accepted   = false;
};

Mouse_filter_result send_observed_mouse_event(
    chrome::Frameless_resize_filter&   filter,
    QWindow&                           window,
    QEvent::Type                       type,
    const QPointF&                     point,
    Qt::MouseButton                    button = Qt::NoButton,
    Qt::MouseButtons                   buttons = Qt::NoButton)
{
    QMouseEvent event(
        type,
        point,
        point,
        point,
        button,
        buttons,
        Qt::NoModifier);
    event.setAccepted(false);
    const bool filter_result = filter.eventFilter(&window, &event);
    return {filter_result, event.isAccepted()};
}

bool check_mouse_filter_result(
    Mouse_filter_result                result,
    bool                               expected_filter_result,
    bool                               expected_event_accepted,
    const std::string&                 message)
{
    bool ok = true;
    ok &= check(result.filter_result == expected_filter_result,
        message + " filter result");
    ok &= check(result.event_accepted == expected_event_accepted,
        message + " event accepted");
    return ok;
}

bool send_left_press(
    chrome::Frameless_resize_filter&   filter,
    QWindow&                           window,
    const QPointF&                     point)
{
    return send_mouse_event(
        filter,
        window,
        QEvent::MouseButtonPress,
        point,
        Qt::LeftButton,
        Qt::LeftButton);
}

bool send_left_double_click(
    chrome::Frameless_resize_filter&   filter,
    QWindow&                           window,
    const QPointF&                     point)
{
    return send_mouse_event(
        filter,
        window,
        QEvent::MouseButtonDblClick,
        point,
        Qt::LeftButton,
        Qt::LeftButton);
}

void size_titlebar(chrome::Terminal_window_chrome& titlebar, qreal width = 360.0)
{
    titlebar.setWidth(width);
    titlebar.setHeight(32.0);
}

std::vector<QRectF> button_rects(const chrome::Terminal_window_chrome& titlebar)
{
    std::vector<QRectF> rects;
    for (const chrome::Window_chrome_button_geometry& button :
        titlebar.chrome_layout().buttons)
    {
        rects.push_back(button.rect);
    }
    return rects;
}

QPointF button_center(
    const chrome::Terminal_window_chrome&   titlebar,
    chrome::Window_chrome_button_role      role)
{
    const chrome::Window_chrome_layout layout = titlebar.chrome_layout();
    for (const chrome::Window_chrome_button_geometry& button : layout.buttons) {
        if (button.role == role) {
            return button.rect.center();
        }
    }

    return {};
}

bool test_edge_and_corner_hit_mapping()
{
    const QSizeF size(100.0, 80.0);
    constexpr qreal border = 6.0;

    struct hit_case_t
    {
        QPointF            point;
        Qt::Edges          expected_edges;
        Qt::CursorShape    expected_cursor;
        const char*        label = "";
    };

    const std::vector<hit_case_t> cases = {
        { QPointF(3.0, 40.0), Qt::LeftEdge, Qt::SizeHorCursor, "left edge" },
        { QPointF(97.0, 40.0), Qt::RightEdge, Qt::SizeHorCursor, "right edge" },
        { QPointF(50.0, 3.0), Qt::TopEdge, Qt::SizeVerCursor, "top edge" },
        { QPointF(50.0, 77.0), Qt::BottomEdge, Qt::SizeVerCursor, "bottom edge" },
        { QPointF(3.0, 3.0), Qt::LeftEdge | Qt::TopEdge, Qt::SizeFDiagCursor, "top-left corner" },
        { QPointF(97.0, 3.0), Qt::RightEdge | Qt::TopEdge, Qt::SizeBDiagCursor, "top-right corner" },
        { QPointF(3.0, 77.0), Qt::LeftEdge | Qt::BottomEdge, Qt::SizeBDiagCursor, "bottom-left corner" },
        {
            QPointF(97.0, 77.0),
            Qt::RightEdge | Qt::BottomEdge,
            Qt::SizeFDiagCursor,
            "bottom-right corner",
        },
        { QPointF(6.0, 40.0),  {}, Qt::ArrowCursor, "left border outer edge" },
        { QPointF(50.0, 6.0),  {}, Qt::ArrowCursor, "top border outer edge"  },
        { QPointF(-1.0, 3.0),  {}, Qt::ArrowCursor, "outside left"           },
        { QPointF(100.0, 3.0), {}, Qt::ArrowCursor, "outside right"          },
    };

    bool ok = true;
    for (const hit_case_t& test_case : cases) {
        const Qt::Edges edges = chrome::frameless_resize_edges_at(
            size,
            test_case.point,
            border);
        ok &= check_edges_equal(
            edges,
            test_case.expected_edges,
            std::string(test_case.label) + " hit edges");
        ok &= check_cursor_shape_equal(
            chrome::frameless_resize_cursor_shape(edges),
            test_case.expected_cursor,
            std::string(test_case.label) + " cursor");
    }

    ok &= check_edges_equal(
        chrome::frameless_resize_edges_at(QSizeF(8.0, 20.0), QPointF(1.0, 10.0), border),
        Qt::LeftEdge,
        "narrow window maps left half to left edge only");
    ok &= check_edges_equal(
        chrome::frameless_resize_edges_at(QSizeF(8.0, 20.0), QPointF(7.0, 10.0), border),
        Qt::RightEdge,
        "narrow window maps right half to right edge only");
    ok &= check_edges_equal(
        chrome::frameless_resize_edges_at(size, QPointF(50.0, 73.9), border),
        {},
        "bottom resize hit starts only before bottom threshold");
    ok &= check_edges_equal(
        chrome::frameless_resize_edges_at(size, QPointF(50.0, 74.0), border),
        Qt::BottomEdge,
        "bottom resize hit includes exact bottom threshold");
    ok &= check_edges_equal(
        chrome::frameless_resize_edges_at(size, QPointF(94.0, 40.0), border),
        Qt::RightEdge,
        "right resize hit includes exact right threshold");
    ok &= check_edges_equal(
        chrome::frameless_resize_edges_at(size, QPointF(50.0, 80.0), border),
        {},
        "point exactly on bottom outer boundary is outside");
    ok &= check_edges_equal(
        chrome::frameless_resize_edges_at(size, QPointF(3.0, 3.0), 0.0),
        {},
        "zero border width disables resize hits");
    ok &= check_edges_equal(
        chrome::frameless_resize_edges_at(size, QPointF(3.0, 3.0), -1.0),
        {},
        "negative border width disables resize hits");
    ok &= check_edges_equal(
        chrome::frameless_resize_edges_at(QSizeF(0.0, 80.0), QPointF(0.0, 3.0), border),
        {},
        "zero width disables resize hits");

    return ok;
}

bool test_maximized_and_fullscreen_disable_resize()
{
    QWindow window;
    window.resize(200, 120);
    Recording_resize_filter filter(&window);

    bool ok = true;
    ok &= check(send_left_press(filter, window, QPointF(2.0, 50.0)),
        "normal window border press is consumed");
    ok &= check_edges_log_equal(
        filter.resize_edges_log,
        { Qt::LeftEdge },
        "normal window starts resize");

    filter.resize_edges_log.clear();
    window.setWindowStates(Qt::WindowMaximized);
    ok &= check(!send_left_press(filter, window, QPointF(2.0, 50.0)),
        "maximized window border press falls through");
    ok &= check_edges_log_equal(filter.resize_edges_log, {},
        "maximized window does not start resize");

    window.setWindowStates(Qt::WindowFullScreen);
    ok &= check(!send_left_press(filter, window, QPointF(2.0, 50.0)),
        "fullscreen window border press falls through");
    ok &= check_edges_log_equal(filter.resize_edges_log, {},
        "fullscreen window does not start resize");

    window.setWindowStates(Qt::WindowMinimized);
    ok &= check(!send_left_press(filter, window, QPointF(2.0, 50.0)),
        "minimized window border press falls through");
    ok &= check_edges_log_equal(filter.resize_edges_log, {},
        "minimized window does not start resize");

    window.setWindowStates(Qt::WindowNoState);
    ok &= check(send_left_press(filter, window, QPointF(2.0, 50.0)),
        "normal state re-enables resize hit testing");

    return ok;
}

bool test_cursor_reset_policy()
{
    QWindow window;
    window.resize(200, 120);
    Recording_resize_filter filter(&window);

    bool ok = true;
    ok &= check(!send_mouse_event(filter, window, QEvent::MouseMove, QPointF(2.0, 50.0)),
        "mouse move over resize border is passed through");
    ok &= check(filter.has_resize_cursor_override(),
        "mouse move over resize border sets resize cursor");
    ok &= check_cursor_shape_equal(
        filter.resize_cursor_shape(),
        Qt::SizeHorCursor,
        "left border uses horizontal resize cursor");
    ok &= check_cursor_shape_equal(
        window.cursor().shape(),
        Qt::SizeHorCursor,
        "window cursor is set to horizontal resize cursor");

    ok &= check(!send_mouse_event(filter, window, QEvent::MouseMove, QPointF(80.0, 50.0)),
        "mouse move outside resize border is passed through");
    ok &= check(!filter.has_resize_cursor_override(),
        "leaving resize border clears resize cursor");
    ok &= check_cursor_shape_equal(
        filter.resize_cursor_shape(),
        Qt::ArrowCursor,
        "cleared resize cursor records arrow shape");
    ok &= check_cursor_shape_equal(
        window.cursor().shape(),
        Qt::ArrowCursor,
        "window cursor is cleared to arrow cursor");

    (void)send_mouse_event(filter, window, QEvent::MouseMove, QPointF(2.0, 50.0));
    ok &= check(filter.has_resize_cursor_override(),
        "second border hover sets cursor for leave test");
    QEvent leave_event(QEvent::Leave);
    ok &= check(!filter.eventFilter(&window, &leave_event),
        "leave event is passed through");
    ok &= check(!filter.has_resize_cursor_override(),
        "leave event clears resize cursor");

    (void)send_mouse_event(filter, window, QEvent::MouseMove, QPointF(2.0, 50.0));
    window.setWindowStates(Qt::WindowMaximized);
    QEvent state_event(QEvent::WindowStateChange);
    ok &= check(!filter.eventFilter(&window, &state_event),
        "window-state event is passed through");
    ok &= check(!filter.has_resize_cursor_override(),
        "disabled resize state clears resize cursor");

    window.setWindowStates(Qt::WindowNoState);
    (void)send_mouse_event(filter, window, QEvent::MouseMove, QPointF(2.0, 50.0));
    ok &= check(filter.has_resize_cursor_override(),
        "border hover sets cursor before border-width disable");
    filter.set_resize_border_width(0.0);
    ok &= check(!filter.has_resize_cursor_override(),
        "setting zero border width clears resize cursor");
    ok &= check(!send_mouse_event(filter, window, QEvent::MouseMove, QPointF(2.0, 50.0)),
        "zero border width passes through later mouse move");

    return ok;
}

bool test_event_filter_cursor_shape_mapping()
{
    QWindow window;
    window.resize(200, 120);
    Recording_resize_filter filter(&window);

    struct cursor_case_t
    {
        QPointF            point;
        Qt::CursorShape    expected_cursor;
        const char*        label = "";
    };

    const std::vector<cursor_case_t> cases = {
        { QPointF(2.0, 50.0),    Qt::SizeHorCursor,   "left edge"           },
        { QPointF(50.0, 2.0),    Qt::SizeVerCursor,   "top edge"            },
        { QPointF(2.0, 2.0),     Qt::SizeFDiagCursor, "top-left corner"     },
        { QPointF(198.0, 2.0),   Qt::SizeBDiagCursor, "top-right corner"    },
        { QPointF(2.0, 118.0),   Qt::SizeBDiagCursor, "bottom-left corner"  },
        { QPointF(198.0, 118.0), Qt::SizeFDiagCursor, "bottom-right corner" },
    };

    bool ok = true;
    for (const cursor_case_t& test_case : cases) {
        ok &= check(!send_mouse_event(filter, window, QEvent::MouseMove, test_case.point),
            std::string(test_case.label) + " mouse move is passed through");
        ok &= check_cursor_shape_equal(
            filter.resize_cursor_shape(),
            test_case.expected_cursor,
            std::string(test_case.label) + " filter cursor shape");
        ok &= check_cursor_shape_equal(
            window.cursor().shape(),
            test_case.expected_cursor,
            std::string(test_case.label) + " window cursor shape");
    }

    return ok;
}

bool test_non_mouse_events_pass_through()
{
    QWindow window;
    window.resize(200, 120);
    Recording_resize_filter filter(&window);

    QKeyEvent key_event(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
    key_event.setAccepted(false);

    bool ok = true;
    ok &= check(!filter.eventFilter(&window, &key_event),
        "keyboard event is passed through");
    ok &= check_edges_log_equal(filter.resize_edges_log, {},
        "keyboard event does not start resize");
    ok &= check(!filter.has_resize_cursor_override(),
        "keyboard event does not set resize cursor");

    QWheelEvent wheel_event(
        QPointF(2.0, 50.0),
        QPointF(2.0, 50.0),
        QPoint(0, 0),
        QPoint(0, 120),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false);
    wheel_event.setAccepted(false);
    ok &= check(!filter.eventFilter(&window, &wheel_event),
        "wheel event on watched window is passed through");

    ok &= check(!send_left_press(filter, window, QPointF(80.0, 50.0)),
        "mouse press outside resize border is passed through");
    ok &= check_edges_log_equal(filter.resize_edges_log, {},
        "mouse press outside resize border does not start resize");

    QObject other_object;
    QEvent generic_event(QEvent::User);
    ok &= check(!filter.eventFilter(&other_object, &generic_event),
        "events for unrelated objects pass through");

    return ok;
}

bool test_button_exclusion()
{
    QWindow window;
    window.resize(200, 120);
    Recording_resize_filter filter(&window);
    std::vector<QRectF> exclusions = {QRectF(90.0, 0.0, 110.0, 32.0)};
    filter.set_button_exclusion_rects_provider([&]() {
        return exclusions;
    });

    bool ok = true;
    ok &= check_edges_equal(
        filter.resize_edges_at(QPointF(196.0, 3.0)),
        {},
        "button exclusion suppresses top-right resize hit");
    ok &= check(!send_left_press(filter, window, QPointF(196.0, 3.0)),
        "press inside excluded button rect falls through");
    ok &= check_edges_log_equal(filter.resize_edges_log, {},
        "excluded button rect does not start resize");

    ok &= check(!send_mouse_event(filter, window, QEvent::MouseMove, QPointF(2.0, 50.0)),
        "border move before exclusion reset is passed through");
    ok &= check(filter.has_resize_cursor_override(),
        "border move before exclusion reset sets cursor");
    ok &= check(!send_mouse_event(filter, window, QEvent::MouseMove, QPointF(196.0, 3.0)),
        "move into excluded button rect is passed through");
    ok &= check(!filter.has_resize_cursor_override(),
        "move into excluded button rect clears resize cursor");

    exclusions.clear();
    ok &= check_edges_equal(
        filter.resize_edges_at(QPointF(196.0, 3.0)),
        Qt::RightEdge | Qt::TopEdge,
        "current exclusion provider allows resize after buttons move away");

    return ok;
}

bool test_mouse_event_side_effects()
{
    QWindow window;
    window.resize(200, 120);
    Recording_resize_filter filter(&window);

    bool ok = true;
    Mouse_filter_result right_press = send_observed_mouse_event(
        filter,
        window,
        QEvent::MouseButtonPress,
        QPointF(2.0, 50.0),
        Qt::RightButton,
        Qt::RightButton);
    ok &= check_mouse_filter_result(
        right_press,
        false,
        false,
        "right press on resize border falls through");
    ok &= check(filter.has_resize_cursor_override(),
        "right press on resize border still applies resize cursor");
    ok &= check_edges_log_equal(filter.resize_edges_log, {},
        "right press on resize border does not start resize");

    Mouse_filter_result right_double_click = send_observed_mouse_event(
        filter,
        window,
        QEvent::MouseButtonDblClick,
        QPointF(2.0, 2.0),
        Qt::RightButton,
        Qt::RightButton);
    ok &= check_mouse_filter_result(
        right_double_click,
        false,
        false,
        "right double-click on resize border falls through");
    ok &= check_edges_log_equal(filter.resize_edges_log, {},
        "right double-click on resize border does not start resize");

    ok &= check(!send_mouse_event(
        filter,
        window,
        QEvent::MouseMove,
        QPointF(2.0, 50.0),
        Qt::NoButton,
        Qt::LeftButton),
        "button-held mouse move over resize border is passed through");
    ok &= check(filter.has_resize_cursor_override(),
        "button-held mouse move over resize border keeps resize cursor");

    ok &= check(!send_mouse_event(
        filter,
        window,
        QEvent::MouseButtonRelease,
        QPointF(2.0, 50.0),
        Qt::LeftButton,
        Qt::NoButton),
        "mouse release over resize border is passed through");
    ok &= check(filter.has_resize_cursor_override(),
        "mouse release over resize border keeps resize cursor");

    return ok;
}

bool test_refused_resize_press_is_still_consumed()
{
    QQuickWindow window;
    window.resize(360, 240);

    Recording_window_chrome titlebar(window.contentItem());
    size_titlebar(titlebar);

    Recording_resize_filter filter(&window);
    filter.resize_return_value = false;

    const QPointF top_titlebar_point(60.0, 3.0);
    const Mouse_filter_result result = send_observed_mouse_event(
        filter,
        window,
        QEvent::MouseButtonPress,
        top_titlebar_point,
        Qt::LeftButton,
        Qt::LeftButton);

    bool ok = true;
    ok &= check_mouse_filter_result(
        result,
        true,
        true,
        "refused resize press is still consumed");
    ok &= check(filter.has_resize_cursor_override(),
        "refused resize press still applies resize cursor");
    ok &= check_edges_log_equal(
        filter.resize_edges_log,
        { Qt::TopEdge },
        "refused resize press still attempts top-edge resize once");
    ok &= check(titlebar.commands.empty(),
        "refused resize press does not fall back to titlebar move");

    return ok;
}

bool test_resize_consumes_titlebar_overlap()
{
    QQuickWindow window;
    window.resize(360, 240);

    Recording_window_chrome titlebar(window.contentItem());
    size_titlebar(titlebar);

    Recording_resize_filter filter(&window);
    filter.set_button_exclusion_rects_provider([&]() {
        return button_rects(titlebar);
    });

    const QPointF top_titlebar_point(60.0, 3.0);
    const QPointF top_left_corner_point(3.0, 3.0);
    bool ok = true;
    ok &= check(titlebar.is_draggable_titlebar_point(top_titlebar_point),
        "top border point is also draggable titlebar space");
    const Mouse_filter_result resize_press = send_observed_mouse_event(
        filter,
        window,
        QEvent::MouseButtonPress,
        top_titlebar_point,
        Qt::LeftButton,
        Qt::LeftButton);
    ok &= check_mouse_filter_result(
        resize_press,
        true,
        true,
        "resize press in titlebar overlap is consumed");
    ok &= check_edges_log_equal(
        filter.resize_edges_log,
        { Qt::TopEdge },
        "titlebar-overlap press starts top-edge resize");
    ok &= check(titlebar.commands.empty(),
        "consumed resize press does not reach titlebar start-system-move handling");

    filter.resize_edges_log.clear();
    ok &= check(titlebar.is_draggable_titlebar_point(top_left_corner_point),
        "top-left border point is also draggable titlebar space");
    ok &= check_mouse_filter_result(
        send_observed_mouse_event(
            filter,
            window,
            QEvent::MouseButtonPress,
            top_left_corner_point,
            Qt::LeftButton,
            Qt::LeftButton),
        true,
        true,
        "top-left resize press in titlebar overlap is consumed");
    ok &= check_edges_log_equal(
        filter.resize_edges_log,
        { Qt::LeftEdge | Qt::TopEdge },
        "top-left overlap press starts corner resize");

    titlebar.commands.clear();
    filter.resize_edges_log.clear();
    ok &= check(
        titlebar.titlebar_double_click_command_at(top_titlebar_point) ==
            chrome::Window_chrome_command::MAXIMIZE,
        "top border point would otherwise maximize on titlebar double-click");
    ok &= check_mouse_filter_result(
        send_observed_mouse_event(
            filter,
            window,
            QEvent::MouseButtonDblClick,
            top_titlebar_point,
            Qt::LeftButton,
            Qt::LeftButton),
        true,
        true,
        "resize double-click in titlebar overlap is consumed");
    ok &= check(titlebar.commands.empty(),
        "consumed resize double-click does not reach titlebar maximize handling");
    ok &= check_edges_log_equal(
        filter.resize_edges_log,
        {},
        "resize double-click consumption does not start another system resize");

    ok &= check_mouse_filter_result(
        send_observed_mouse_event(
            filter,
            window,
            QEvent::MouseButtonDblClick,
            top_left_corner_point,
            Qt::LeftButton,
            Qt::LeftButton),
        true,
        true,
        "top-left resize double-click in titlebar overlap is consumed");
    const QPointF close_button_point = QPointF(
        button_center(titlebar, chrome::Window_chrome_button_role::CLOSE).x(),
        3.0);
    ok &= check(titlebar.button_role_at(close_button_point).has_value(),
        "close button point is a current button exclusion rectangle");
    ok &= check(!send_left_press(filter, window, close_button_point),
        "button exclusion beats resize even on top-right border");
    ok &= check(!send_left_double_click(filter, window, close_button_point),
        "button exclusion beats resize double-click on top-right border");

    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    bool ok = true;
    ok &= test_edge_and_corner_hit_mapping();
    ok &= test_maximized_and_fullscreen_disable_resize();
    ok &= test_cursor_reset_policy();
    ok &= test_event_filter_cursor_shape_mapping();
    ok &= test_non_mouse_events_pass_through();
    ok &= test_button_exclusion();
    ok &= test_mouse_event_side_effects();
    ok &= test_refused_resize_press_is_still_consumed();
    ok &= test_resize_consumes_titlebar_overlap();
    return ok ? 0 : 1;
}
