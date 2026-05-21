#include "window_chrome.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QColor>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QIcon>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QString>

#include <cmath>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace chrome = vnm_terminal::terminal_app;

namespace {

QString scalar_text(char32_t codepoint)
{
    return QString::fromUcs4(&codepoint, 1);
}

std::string utf8_text(const QString& text)
{
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

using vnm_terminal::test_helpers::check;

bool check_qstring_equal(
    const QString&     actual,
    const QString&     expected,
    const std::string& message)
{
    if (actual == expected) {
        return true;
    }

    std::cerr << "FAIL: " << message
        << " expected=[" << utf8_text(expected)
        << "] actual=[" << utf8_text(actual) << "]\n";
    return false;
}

bool check_button_role_equal(
    std::optional<chrome::Window_chrome_button_role>   actual,
    std::optional<chrome::Window_chrome_button_role>   expected,
    const std::string&                                 message)
{
    if (actual == expected) {
        return true;
    }

    std::cerr << "FAIL: " << message
        << " expected=" << (expected.has_value() ? static_cast<int>(*expected) : -1)
        << " actual="   << (actual.has_value() ? static_cast<int>(*actual) : -1)
        << '\n';
    return false;
}

bool check_command_equal(
    chrome::Window_chrome_command      actual,
    chrome::Window_chrome_command      expected,
    const std::string&                 message)
{
    if (actual == expected) {
        return true;
    }

    std::cerr << "FAIL: " << message
        << " expected=" << static_cast<int>(expected)
        << " actual="   << static_cast<int>(actual) << '\n';
    return false;
}

bool check_button_state_equal(
    chrome::Window_chrome_button_state actual,
    chrome::Window_chrome_button_state expected,
    const std::string&                 message)
{
    if (actual == expected) {
        return true;
    }

    std::cerr << "FAIL: " << message
        << " expected=" << static_cast<int>(expected)
        << " actual="   << static_cast<int>(actual) << '\n';
    return false;
}

bool check_qcolor_equal(
    const QColor&      actual,
    const QColor&      expected,
    const std::string& message)
{
    if (actual == expected) {
        return true;
    }

    std::cerr << "FAIL: " << message
        << " expected=(" << expected.red() << ", " << expected.green()
        << ", " << expected.blue() << ", " << expected.alpha()
        << ") actual=(" << actual.red() << ", " << actual.green()
        << ", " << actual.blue() << ", " << actual.alpha() << ")\n";
    return false;
}

bool check_command_log_equal(
    const std::vector<chrome::Window_chrome_command>&      actual,
    std::initializer_list<chrome::Window_chrome_command>   expected,
    const std::string&                                     message)
{
    if (actual.size() != expected.size()) {
        std::cerr << "FAIL: " << message
            << " expected-size=" << expected.size()
            << " actual-size="   << actual.size() << '\n';
        return false;
    }

    bool        ok    = true;
    std::size_t index = 0;
    for (chrome::Window_chrome_command expected_command : expected) {
        ok &= check_command_equal(
            actual[index],
            expected_command,
            message + " command " + std::to_string(index));
        ++index;
    }
    return ok;
}

bool check_spinner_equal(
    const std::optional<QString>&  actual,
    const std::optional<QString>&  expected,
    const std::string&             message)
{
    if (actual.has_value() != expected.has_value()) {
        std::cerr << "FAIL: " << message
            << " expected-has-spinner=" << expected.has_value()
            << " actual-has-spinner="   << actual.has_value() << '\n';
        return false;
    }

    if (!actual.has_value()) {
        return true;
    }

    return check_qstring_equal(*actual, *expected, message);
}

class Recording_window_chrome final : public chrome::Terminal_window_chrome
{
public:
    using chrome::Terminal_window_chrome::Terminal_window_chrome;

    bool send_mouse_press(
        const QPointF&     point,
        Qt::MouseButton    button = Qt::LeftButton)
    {
        QMouseEvent event(
            QEvent::MouseButtonPress,
            point,
            point,
            point,
            button,
            button == Qt::NoButton ? Qt::NoButton : Qt::MouseButtons(button),
            Qt::NoModifier);
        event.setAccepted(false);
        mousePressEvent(&event);
        return event.isAccepted();
    }

    bool send_mouse_move(const QPointF& point, Qt::MouseButtons buttons = Qt::LeftButton)
    {
        QMouseEvent event(
            QEvent::MouseMove,
            point,
            point,
            point,
            Qt::NoButton,
            buttons,
            Qt::NoModifier);
        event.setAccepted(false);
        mouseMoveEvent(&event);
        return event.isAccepted();
    }

    bool send_mouse_release(
        const QPointF&     point,
        Qt::MouseButton    button = Qt::LeftButton)
    {
        QMouseEvent event(
            QEvent::MouseButtonRelease,
            point,
            point,
            point,
            button,
            Qt::NoButton,
            Qt::NoModifier);
        event.setAccepted(false);
        mouseReleaseEvent(&event);
        return event.isAccepted();
    }

    bool send_mouse_double_click(
        const QPointF&     point,
        Qt::MouseButton    button = Qt::LeftButton)
    {
        QMouseEvent event(
            QEvent::MouseButtonDblClick,
            point,
            point,
            point,
            button,
            button == Qt::NoButton ? Qt::NoButton : Qt::MouseButtons(button),
            Qt::NoModifier);
        event.setAccepted(false);
        mouseDoubleClickEvent(&event);
        return event.isAccepted();
    }

    void send_hover_move(const QPointF& point)
    {
        QHoverEvent event(QEvent::HoverMove, point, QPointF(-1.0, -1.0));
        hoverMoveEvent(&event);
    }

    void send_hover_leave()
    {
        QHoverEvent event(QEvent::HoverLeave, QPointF(-1.0, -1.0), QPointF(0.0, 0.0));
        hoverLeaveEvent(&event);
    }

    void send_mouse_ungrab()
    {
        mouseUngrabEvent();
    }

    std::vector<chrome::Window_chrome_command> commands;

protected:
    void invoke_window_command(chrome::Window_chrome_command command) override
    {
        commands.push_back(command);
    }
};

class Exposed_window_chrome final : public chrome::Terminal_window_chrome
{
public:
    using chrome::Terminal_window_chrome::Terminal_window_chrome;

    void send_button_press_release(const QPointF& point)
    {
        QMouseEvent press(
            QEvent::MouseButtonPress,
            point,
            point,
            point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier);
        mousePressEvent(&press);

        QMouseEvent release(
            QEvent::MouseButtonRelease,
            point,
            point,
            point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier);
        mouseReleaseEvent(&release);
    }
};

void size_titlebar(chrome::Terminal_window_chrome& titlebar, qreal width = 360.0)
{
    titlebar.setWidth(width);
    titlebar.setHeight(32.0);
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

bool test_icon_resource_available()
{
    const QIcon app_icon(QStringLiteral(":/vnm_terminal/vnm_terminal.ico"));
    return check(!app_icon.isNull(), "titlebar app icon resource is available to tests");
}

bool test_button_hit_regions()
{
    chrome::Terminal_window_chrome titlebar;
    size_titlebar(titlebar);

    bool ok = true;
    ok &= check_button_role_equal(
        titlebar.button_role_at(
            button_center(titlebar, chrome::Window_chrome_button_role::MINIMIZE)),
        chrome::Window_chrome_button_role::MINIMIZE,
        "minimize button center hits minimize role");
    ok &= check_button_role_equal(
        titlebar.button_role_at(
            button_center(titlebar, chrome::Window_chrome_button_role::MAXIMIZE_RESTORE)),
        chrome::Window_chrome_button_role::MAXIMIZE_RESTORE,
        "maximize button center hits maximize role");
    ok &= check_button_role_equal(
        titlebar.button_role_at(
            button_center(titlebar, chrome::Window_chrome_button_role::CLOSE)),
        chrome::Window_chrome_button_role::CLOSE,
        "close button center hits close role");
    ok &= check_button_role_equal(
        titlebar.button_role_at(QPointF(60.0, 16.0)),
        std::nullopt,
        "title text space does not hit a button");
    ok &= check_button_role_equal(
        titlebar.button_role_at(QPointF(-1.0, 16.0)),
        std::nullopt,
        "point left of titlebar does not hit a button");

    chrome::Terminal_window_chrome narrow_titlebar;
    size_titlebar(narrow_titlebar, 100.0);
    ok &= check_button_role_equal(
        narrow_titlebar.button_role_at(QPointF(96.0, 16.0)),
        chrome::Window_chrome_button_role::CLOSE,
        "narrow titlebar leaves clipped close-button hit region inside item bounds");
    ok &= check_button_role_equal(
        narrow_titlebar.button_role_at(QPointF(50.0, 16.0)),
        chrome::Window_chrome_button_role::MAXIMIZE_RESTORE,
        "narrow titlebar keeps maximize-button hit region inside item bounds");
    ok &= check_button_role_equal(
        narrow_titlebar.button_role_at(QPointF(120.0, 16.0)),
        std::nullopt,
        "button geometry outside the item is not hittable");

    return ok;
}

bool test_button_command_mapping()
{
    chrome::Terminal_window_chrome titlebar;
    size_titlebar(titlebar);

    bool ok = true;
    ok &= check_command_equal(
        titlebar.button_command_at(
            button_center(titlebar, chrome::Window_chrome_button_role::MINIMIZE)),
        chrome::Window_chrome_command::MINIMIZE,
        "minimize button maps to minimize command");
    ok &= check_command_equal(
        titlebar.button_command_at(
            button_center(titlebar, chrome::Window_chrome_button_role::MAXIMIZE_RESTORE)),
        chrome::Window_chrome_command::MAXIMIZE,
        "maximize button maps to maximize command before maximized state");
    ok &= check_command_equal(
        titlebar.button_command_at(
            button_center(titlebar, chrome::Window_chrome_button_role::CLOSE)),
        chrome::Window_chrome_command::CLOSE,
        "close button maps to close command");
    ok &= check_command_equal(
        titlebar.button_command_at(QPointF(60.0, 16.0)),
        chrome::Window_chrome_command::NONE,
        "non-button point maps to no button command");

    titlebar.set_window_maximized(true);
    ok &= check_command_equal(
        titlebar.button_command_at(
            button_center(titlebar, chrome::Window_chrome_button_role::MAXIMIZE_RESTORE)),
        chrome::Window_chrome_command::RESTORE,
        "maximize button maps to restore command while maximized");

    return ok;
}

bool test_draggable_region_mapping()
{
    chrome::Terminal_window_chrome titlebar;
    size_titlebar(titlebar);

    bool ok = true;
    ok &= check(titlebar.is_draggable_titlebar_point(QPointF(60.0, 16.0)),
        "title text space is draggable");
    ok &= check(titlebar.is_draggable_titlebar_point(QPointF(12.0, 16.0)),
        "app icon space is draggable");
    ok &= check(
        !titlebar.is_draggable_titlebar_point(
            button_center(titlebar, chrome::Window_chrome_button_role::MINIMIZE)),
        "button space is not draggable");
    ok &= check(!titlebar.is_draggable_titlebar_point(QPointF(-1.0, 16.0)),
        "point outside titlebar is not draggable");

    return ok;
}

bool test_double_click_command_mapping()
{
    chrome::Terminal_window_chrome titlebar;
    size_titlebar(titlebar);

    bool ok = true;
    ok &= check_command_equal(
        titlebar.titlebar_double_click_command_at(QPointF(60.0, 16.0)),
        chrome::Window_chrome_command::MAXIMIZE,
        "titlebar double-click maps to maximize before maximized state");
    ok &= check_command_equal(
        titlebar.titlebar_double_click_command_at(
            button_center(titlebar, chrome::Window_chrome_button_role::CLOSE)),
        chrome::Window_chrome_command::NONE,
        "button double-click maps to no titlebar command");
    ok &= check_command_equal(
        titlebar.titlebar_double_click_command_at(QPointF(380.0, 16.0)),
        chrome::Window_chrome_command::NONE,
        "outside double-click maps to no titlebar command");

    titlebar.set_window_maximized(true);
    ok &= check_command_equal(
        titlebar.titlebar_double_click_command_at(QPointF(60.0, 16.0)),
        chrome::Window_chrome_command::RESTORE,
        "titlebar double-click maps to restore while maximized");

    return ok;
}

bool test_mouse_button_event_paths()
{
    Recording_window_chrome titlebar;
    size_titlebar(titlebar);

    const QPointF minimize =
        button_center(titlebar, chrome::Window_chrome_button_role::MINIMIZE);
    const QPointF maximize =
        button_center(titlebar, chrome::Window_chrome_button_role::MAXIMIZE_RESTORE);
    const QPointF close =
        button_center(titlebar, chrome::Window_chrome_button_role::CLOSE);

    bool ok = true;
    ok &= check(titlebar.send_mouse_press(minimize),
        "left press on button is accepted");
    ok &= check_button_state_equal(
        titlebar.button_states().minimize,
        chrome::Window_chrome_button_state::PRESSED,
        "left press sets pressed state");
    ok &= check_command_log_equal(titlebar.commands, {},
        "button press does not invoke command before release");
    ok &= check(titlebar.send_mouse_release(minimize),
        "release on pressed button is accepted");
    ok &= check_command_log_equal(
        titlebar.commands,
        { chrome::Window_chrome_command::MINIMIZE },
        "press/release on same button invokes minimize");
    ok &= check_button_state_equal(
        titlebar.button_states().minimize,
        chrome::Window_chrome_button_state::HOVERED,
        "release clears pressed state while preserving hover");

    titlebar.commands.clear();
    ok &= check(titlebar.send_mouse_press(minimize),
        "second button press is accepted");
    ok &= check(titlebar.send_mouse_release(QPointF(60.0, 16.0)),
        "release outside pressed button is accepted");
    ok &= check_command_log_equal(titlebar.commands, {},
        "release outside pressed button cancels command");
    ok &= check_button_state_equal(
        titlebar.button_states().minimize,
        chrome::Window_chrome_button_state::NORMAL,
        "release outside clears pressed button");

    ok &= check(titlebar.send_mouse_press(minimize),
        "third button press is accepted");
    ok &= check(titlebar.send_mouse_release(maximize),
        "release on different button is accepted");
    ok &= check_command_log_equal(titlebar.commands, {},
        "release on different button cancels command");

    ok &= check(!titlebar.send_mouse_press(close, Qt::RightButton),
        "right press falls through");
    ok &= check_command_log_equal(titlebar.commands, {},
        "right press invokes no window command");

    titlebar.set_window_maximized(false);
    ok &= check(titlebar.send_mouse_press(maximize),
        "maximize press is accepted");
    ok &= check(titlebar.send_mouse_release(maximize),
        "maximize release is accepted");
    ok &= check_command_log_equal(
        titlebar.commands,
        { chrome::Window_chrome_command::MAXIMIZE },
        "maximize button invokes maximize command");

    titlebar.commands.clear();
    titlebar.set_window_maximized(true);
    ok &= check(titlebar.send_mouse_press(maximize),
        "restore press is accepted");
    ok &= check(titlebar.send_mouse_release(maximize),
        "restore release is accepted");
    ok &= check_command_log_equal(
        titlebar.commands,
        { chrome::Window_chrome_command::RESTORE },
        "maximize button invokes restore command while maximized");

    return ok;
}

bool test_drag_and_double_click_event_paths()
{
    Recording_window_chrome titlebar;
    size_titlebar(titlebar);

    bool ok = true;
    ok &= check(titlebar.send_mouse_press(QPointF(60.0, 16.0)),
        "draggable titlebar press is accepted");
    ok &= check_command_log_equal(
        titlebar.commands,
        { chrome::Window_chrome_command::START_SYSTEM_MOVE },
        "draggable titlebar press synchronously dispatches start-system-move");

    titlebar.commands.clear();
    titlebar.set_window_maximized(false);
    ok &= check(titlebar.send_mouse_double_click(QPointF(60.0, 16.0)),
        "draggable titlebar double-click is accepted");
    ok &= check_command_log_equal(
        titlebar.commands,
        { chrome::Window_chrome_command::MAXIMIZE },
        "draggable titlebar double-click dispatches maximize");

    titlebar.commands.clear();
    titlebar.set_window_maximized(true);
    ok &= check(titlebar.send_mouse_double_click(QPointF(60.0, 16.0)),
        "maximized titlebar double-click is accepted");
    ok &= check_command_log_equal(
        titlebar.commands,
        { chrome::Window_chrome_command::RESTORE },
        "maximized titlebar double-click dispatches restore");

    titlebar.commands.clear();
    ok &= check(
        !titlebar.send_mouse_double_click(
            button_center(titlebar, chrome::Window_chrome_button_role::CLOSE)),
        "button double-click falls through titlebar maximize handling");
    ok &= check_command_log_equal(titlebar.commands, {},
        "button double-click invokes no titlebar command");

    ok &= check(!titlebar.send_mouse_double_click(QPointF(380.0, 16.0)),
        "outside double-click falls through");
    ok &= check_command_log_equal(titlebar.commands, {},
        "outside double-click invokes no command");

    return ok;
}

bool test_hover_and_ungrab_state_paths()
{
    Recording_window_chrome titlebar;
    size_titlebar(titlebar);

    const QPointF minimize =
        button_center(titlebar, chrome::Window_chrome_button_role::MINIMIZE);

    bool ok = true;
    titlebar.send_hover_move(minimize);
    ok &= check_button_state_equal(
        titlebar.button_states().minimize,
        chrome::Window_chrome_button_state::HOVERED,
        "hover move sets hovered button state");

    titlebar.send_hover_leave();
    ok &= check_button_state_equal(
        titlebar.button_states().minimize,
        chrome::Window_chrome_button_state::NORMAL,
        "hover leave clears hovered button state");

    ok &= check(titlebar.send_mouse_press(minimize),
        "button press before ungrab is accepted");
    ok &= check_button_state_equal(
        titlebar.button_states().minimize,
        chrome::Window_chrome_button_state::PRESSED,
        "button press sets pressed state before ungrab");
    titlebar.send_mouse_ungrab();
    ok &= check_button_state_equal(
        titlebar.button_states().minimize,
        chrome::Window_chrome_button_state::HOVERED,
        "mouse ungrab clears pressed state");

    ok &= check(titlebar.send_mouse_press(minimize),
        "button press before drag-out is accepted");
    (void)titlebar.send_mouse_move(QPointF(60.0, 16.0));
    ok &= check_button_state_equal(
        titlebar.button_states().minimize,
        chrome::Window_chrome_button_state::NORMAL,
        "mouse move away from pressed button clears visible pressed state");

    return ok;
}

bool test_focus_policy()
{
    chrome::Terminal_window_chrome titlebar;

    bool ok = true;
    ok &= check(titlebar.focusPolicy() == Qt::NoFocus,
        "titlebar item has no keyboard focus policy");
    ok &= check(!titlebar.activeFocusOnTab(),
        "titlebar item does not accept tab focus");
    ok &= check(!titlebar.hasFocus(),
        "titlebar item is not constructed with focus");
    ok &= check(titlebar.acceptedMouseButtons() == Qt::LeftButton,
        "titlebar accepts only left mouse button input");

    return ok;
}

bool test_titlebar_background_paint_uses_window_chrome_color()
{
    auto painted_titlebar = [](bool active) {
        chrome::Terminal_window_chrome titlebar;
        titlebar.setSize(QSizeF(64.0, 32.0));
        titlebar.set_window_active(active);

        QImage image(QSize(64, 32), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        titlebar.paint(&painter);
        painter.end();
        return image;
    };

    const QImage active_titlebar   = painted_titlebar(true);
    const QImage inactive_titlebar = painted_titlebar(false);

    bool ok = true;
    ok &= check_qcolor_equal(
        active_titlebar.pixelColor(2, 2),
        chrome::window_chrome_background_color(true),
        "active titlebar background uses the shared chrome background color");
    ok &= check_qcolor_equal(
        inactive_titlebar.pixelColor(2, 2),
        chrome::window_chrome_background_color(false),
        "inactive titlebar background uses the shared chrome background color");
    ok &= check_qcolor_equal(
        active_titlebar.pixelColor(2, 31),
        chrome::window_chrome_background_color(true),
        "active titlebar bottom edge has no separator line");
    ok &= check_qcolor_equal(
        inactive_titlebar.pixelColor(2, 31),
        chrome::window_chrome_background_color(false),
        "inactive titlebar bottom edge has no separator line");
    return ok;
}

bool test_terminal_content_border_paint_uses_border_color()
{
    auto painted_border = [](bool active) {
        chrome::Terminal_content_border border;
        border.setSize(QSizeF(20.0, 12.0));
        border.set_window_active(active);

        QImage image(QSize(20, 12), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        border.paint(&painter);
        painter.end();
        return image;
    };

    const QImage active_border   = painted_border(true);
    const QImage inactive_border = painted_border(false);

    bool ok = true;
    ok &= check_qcolor_equal(
        active_border.pixelColor(0, 0),
        chrome::terminal_content_border_color(true),
        "active content border uses the shared border color");
    ok &= check_qcolor_equal(
        active_border.pixelColor(10, 6),
        chrome::terminal_content_border_color(true),
        "active content border fill uses the shared border color");
    ok &= check_qcolor_equal(
        inactive_border.pixelColor(19, 11),
        chrome::terminal_content_border_color(false),
        "inactive content border uses the shared border color");
    return ok;
}

bool test_focus_survives_titlebar_interactions()
{
    QQuickWindow window;
    Recording_window_chrome titlebar(window.contentItem());
    QQuickItem focus_owner(window.contentItem());
    size_titlebar(titlebar);

    focus_owner.setFlag(QQuickItem::ItemIsFocusScope, true);
    focus_owner.setFocus(true);

    const QPointF minimize =
        button_center(titlebar, chrome::Window_chrome_button_role::MINIMIZE);

    bool ok = true;
    ok &= check(focus_owner.hasFocus(), "focus owner starts with focus");
    ok &= check(titlebar.send_mouse_press(minimize),
        "button press for focus test is accepted");
    ok &= check(titlebar.send_mouse_release(minimize),
        "button release for focus test is accepted");
    ok &= check(!titlebar.hasFocus(), "titlebar does not take focus after button click");
    ok &= check(focus_owner.hasFocus(), "focus owner keeps focus after button click");

    titlebar.commands.clear();
    ok &= check(titlebar.send_mouse_press(QPointF(60.0, 16.0)),
        "drag press for focus test is accepted");
    ok &= check(!titlebar.hasFocus(), "titlebar does not take focus after drag press");
    ok &= check(focus_owner.hasFocus(), "focus owner keeps focus after drag press");

    return ok;
}

bool test_null_window_event_paths_are_safe()
{
    Exposed_window_chrome titlebar;
    size_titlebar(titlebar);
    titlebar.send_button_press_release(
        button_center(titlebar, chrome::Window_chrome_button_role::CLOSE));
    return check(true, "unparented titlebar button event path does not crash");
}

bool test_state_and_title_derivation()
{
    const QString title_marker = scalar_text(chrome::k_activity_marker_dingbat_first);
    const QString icon_marker  = scalar_text(chrome::k_activity_marker_braille_last);

    chrome::Terminal_window_chrome titlebar;
    size_titlebar(titlebar);
    titlebar.set_terminal_title(title_marker + QStringLiteral("compile"));
    titlebar.set_terminal_icon_name(icon_marker + QStringLiteral("icon-frame"));
    titlebar.set_window_active(false);
    titlebar.set_window_maximized(true);

    const chrome::Window_chrome_title_content content = titlebar.title_content();
    const chrome::Window_chrome_button_states states  = titlebar.button_states();
    const chrome::Window_chrome_layout        layout  = titlebar.chrome_layout();

    bool ok = true;
    ok &= check_qstring_equal(
        titlebar.terminal_title(),
        title_marker + QStringLiteral("compile"),
        "titlebar stores raw terminal title");
    ok &= check_qstring_equal(
        titlebar.terminal_icon_name(),
        icon_marker + QStringLiteral("icon-frame"),
        "titlebar stores raw terminal icon name");
    ok &= check_spinner_equal(content.spinner, icon_marker,
        "titlebar derives spinner from icon-name marker first");
    ok &= check_qstring_equal(content.display_title, QStringLiteral("compile"),
        "titlebar derives display title with title marker stripped");
    ok &= check(!titlebar.window_active(),
        "titlebar stores inactive window state");
    ok &= check(titlebar.window_maximized(),
        "titlebar stores maximized window state");
    ok &= check(states.window_maximized,
        "button states carry maximized state");
    ok &= check(layout.window_maximized,
        "layout carries maximized state");

    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    bool ok = true;
    ok &= test_icon_resource_available();
    ok &= test_button_hit_regions();
    ok &= test_button_command_mapping();
    ok &= test_draggable_region_mapping();
    ok &= test_double_click_command_mapping();
    ok &= test_mouse_button_event_paths();
    ok &= test_drag_and_double_click_event_paths();
    ok &= test_hover_and_ungrab_state_paths();
    ok &= test_focus_policy();
    ok &= test_titlebar_background_paint_uses_window_chrome_color();
    ok &= test_terminal_content_border_paint_uses_border_color();
    ok &= test_focus_survives_titlebar_interactions();
    ok &= test_null_window_event_paths_are_safe();
    ok &= test_state_and_title_derivation();
    return ok ? 0 : 1;
}
