#include "window_chrome.h"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintDevice>
#include <QPen>
#include <QPixmap>
#include <QQuickWindow>
#include <QRectF>
#include <QSize>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace chrome = vnm_terminal::example_terminal;

namespace {

constexpr qreal k_control_icon_size = 10.0;
constexpr qreal k_restore_offset    = 3.0;

QRectF item_rect(const QQuickItem& item)
{
    return QRectF(0.0, 0.0, item.width(), item.height());
}

qreal paint_device_pixel_ratio(const QPaintDevice* device)
{
    if (device == nullptr) {
        return 1.0;
    }

    return std::max<qreal>(1.0, device->devicePixelRatioF());
}

QColor titlebar_separator_color(bool active)
{
    return active ? QColor(42, 49, 60) : QColor(31, 36, 44);
}

QColor title_text_color(bool active)
{
    return active ? QColor(235, 239, 245) : QColor(147, 156, 169);
}

QColor spinner_text_color(bool active)
{
    return active ? QColor(113, 180, 255) : QColor(95, 119, 147);
}

QColor button_icon_color(chrome::Window_chrome_button_state state, bool active)
{
    if (state == chrome::Window_chrome_button_state::DISABLED) {
        return QColor(99, 106, 116);
    }

    return active ? QColor(226, 232, 240) : QColor(142, 151, 163);
}

QColor button_background_color(
    chrome::Window_chrome_button_role  role,
    chrome::Window_chrome_button_state state)
{
    if (state == chrome::Window_chrome_button_state::PRESSED &&
        role  == chrome::Window_chrome_button_role::CLOSE)
    {
        return QColor(150, 34, 42);
    }

    if (state == chrome::Window_chrome_button_state::HOVERED &&
        role  == chrome::Window_chrome_button_role::CLOSE)
    {
        return QColor(198, 48, 58);
    }

    if (state == chrome::Window_chrome_button_state::PRESSED) {
        return QColor(52, 61, 74);
    }

    if (state == chrome::Window_chrome_button_state::HOVERED) {
        return QColor(39, 47, 58);
    }

    return QColor(Qt::transparent);
}

QRectF centered_square(const QRectF& outer_rect, qreal size)
{
    return QRectF(
        outer_rect.center().x() - size / 2.0,
        outer_rect.center().y() - size / 2.0,
        size,
        size);
}

void paint_app_icon(
    QPainter&      painter,
    const QIcon&   icon,
    const QRectF&  rect,
    qreal          device_pixel_ratio)
{
    if (rect.isEmpty() || icon.isNull()) {
        return;
    }

    const QSize pixel_size(
        std::max(1, static_cast<int>(std::round(rect.width() * device_pixel_ratio))),
        std::max(1, static_cast<int>(std::round(rect.height() * device_pixel_ratio))));

    QPixmap pixmap = icon.pixmap(pixel_size);
    if (pixmap.isNull()) {
        return;
    }

    pixmap.setDevicePixelRatio(device_pixel_ratio);
    const QRectF source_rect(
        0.0,
        0.0,
        pixmap.width() / device_pixel_ratio,
        pixmap.height() / device_pixel_ratio);
    painter.drawPixmap(rect, pixmap, source_rect);
}

void paint_minimize_icon(QPainter& painter, const QRectF& rect)
{
    const qreal y = rect.center().y() + 3.0;
    painter.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
}

void paint_maximize_icon(QPainter& painter, const QRectF& rect)
{
    painter.drawRect(rect.adjusted(0.5, 0.5, -0.5, -0.5));
}

void paint_restore_icon(QPainter& painter, const QRectF& rect)
{
    const QRectF rear_rect = rect.translated(k_restore_offset, -k_restore_offset);
    painter.drawRect(rear_rect.adjusted(0.5, 0.5, -0.5, -0.5));
    painter.drawRect(rect.adjusted(0.5, 0.5, -0.5, -0.5));
}

void paint_close_icon(QPainter& painter, const QRectF& rect)
{
    painter.drawLine(rect.topLeft(),    rect.bottomRight());
    painter.drawLine(rect.bottomLeft(), rect.topRight());
}

void paint_button(
    QPainter&                                      painter,
    const chrome::Window_chrome_button_geometry&   button,
    bool                                           active,
    bool                                           window_maximized)
{
    const QColor background =
        button_background_color(button.role, button.state);
    if (background.alpha() > 0) {
        painter.fillRect(button.rect, background);
    }

    QPen icon_pen(button_icon_color(button.state, active));
    icon_pen.setWidthF(1.3);
    icon_pen.setCapStyle(Qt::SquareCap);
    painter.setPen(icon_pen);
    painter.setBrush(Qt::NoBrush);

    const QRectF icon_rect = centered_square(button.rect, k_control_icon_size);
    switch (button.role) {
        case chrome::Window_chrome_button_role::MINIMIZE:
            paint_minimize_icon(painter, icon_rect);
            break;

        case chrome::Window_chrome_button_role::MAXIMIZE_RESTORE:
            if (window_maximized) {
                paint_restore_icon(painter, icon_rect.adjusted(-1.0, 1.0, -1.0, 1.0));
            }
            else {
                paint_maximize_icon(painter, icon_rect);
            }
            break;

        case chrome::Window_chrome_button_role::CLOSE:
            paint_close_icon(painter, icon_rect.adjusted(1.0, 1.0, -1.0, -1.0));
            break;
    }
}

} // namespace

QColor chrome::window_chrome_background_color(bool active)
{
    return active ? QColor(18, 23, 30) : QColor(14, 17, 22);
}

chrome::Example_window_chrome::Example_window_chrome(QQuickItem* parent)
:
    QQuickPaintedItem(parent),
    m_app_icon(QStringLiteral(":/vnm_terminal_example_terminal/vnm_terminal.ico"))
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setAntialiasing(true);
    setActiveFocusOnTab(false);
    setFocusPolicy(Qt::NoFocus);
    setFocus(false);
}

QString chrome::Example_window_chrome::terminal_title() const
{
    return m_terminal_title;
}

QString chrome::Example_window_chrome::terminal_icon_name() const
{
    return m_terminal_icon_name;
}

bool chrome::Example_window_chrome::window_active() const
{
    return m_window_active;
}

bool chrome::Example_window_chrome::window_maximized() const
{
    return m_window_maximized;
}

void chrome::Example_window_chrome::set_terminal_title(const QString& terminal_title)
{
    if (m_terminal_title == terminal_title) {
        return;
    }

    m_terminal_title = terminal_title;
    update();
}

void chrome::Example_window_chrome::set_terminal_icon_name(const QString& terminal_icon_name)
{
    if (m_terminal_icon_name == terminal_icon_name) {
        return;
    }

    m_terminal_icon_name = terminal_icon_name;
    update();
}

void chrome::Example_window_chrome::set_window_active(bool active)
{
    if (m_window_active == active) {
        return;
    }

    m_window_active = active;
    update();
}

void chrome::Example_window_chrome::set_window_maximized(bool maximized)
{
    if (m_window_maximized == maximized) {
        return;
    }

    m_window_maximized = maximized;
    update();
}

chrome::Window_chrome_title_content chrome::Example_window_chrome::title_content() const
{
    return derive_window_chrome_title_content(m_terminal_title, m_terminal_icon_name);
}

chrome::Window_chrome_button_states chrome::Example_window_chrome::button_states() const
{
    Window_chrome_button_states states;
    states.window_maximized = m_window_maximized;

    auto set_button_state = [&](Window_chrome_button_role role, Window_chrome_button_state state) {
        switch (role) {
            case Window_chrome_button_role::MINIMIZE:
                states.minimize = state;
                break;

            case Window_chrome_button_role::MAXIMIZE_RESTORE:
                states.maximize_restore = state;
                break;

            case Window_chrome_button_role::CLOSE:
                states.close = state;
                break;
        }
    };

    if (m_hovered_button.has_value()) {
        set_button_state(*m_hovered_button, Window_chrome_button_state::HOVERED);
    }

    if (m_pressed_button.has_value() && m_pressed_button == m_hovered_button) {
        set_button_state(*m_pressed_button, Window_chrome_button_state::PRESSED);
    }

    return states;
}

chrome::Window_chrome_layout chrome::Example_window_chrome::chrome_layout() const
{
    return calculate_window_chrome_layout(
        QSizeF(width(), height()),
        button_states());
}

std::optional<chrome::Window_chrome_button_role> chrome::Example_window_chrome::button_role_at(
    const QPointF& point) const
{
    if (!item_rect(*this).contains(point)) {
        return std::nullopt;
    }

    const Window_chrome_layout layout = chrome_layout();
    for (const Window_chrome_button_geometry& button : layout.buttons) {
        if (button.rect.contains(point)) {
            return button.role;
        }
    }

    return std::nullopt;
}

bool chrome::Example_window_chrome::is_draggable_titlebar_point(const QPointF& point) const
{
    return item_rect(*this).contains(point) && !button_role_at(point).has_value();
}

chrome::Window_chrome_command chrome::Example_window_chrome::command_for_button(
    Window_chrome_button_role role) const
{
    switch (role) {
        case Window_chrome_button_role::MINIMIZE:
            return Window_chrome_command::MINIMIZE;
        case Window_chrome_button_role::MAXIMIZE_RESTORE:
            return m_window_maximized
                ? Window_chrome_command::RESTORE
                : Window_chrome_command::MAXIMIZE;
        case Window_chrome_button_role::CLOSE:
            return Window_chrome_command::CLOSE;
    }

    return Window_chrome_command::NONE;
}

chrome::Window_chrome_command chrome::Example_window_chrome::button_command_at(
    const QPointF& point) const
{
    const std::optional<Window_chrome_button_role> role = button_role_at(point);
    return role.has_value() ? command_for_button(*role) : Window_chrome_command::NONE;
}

chrome::Window_chrome_command chrome::Example_window_chrome::titlebar_double_click_command_at(
    const QPointF& point) const
{
    if (!is_draggable_titlebar_point(point)) {
        return Window_chrome_command::NONE;
    }

    return m_window_maximized
        ? Window_chrome_command::RESTORE
        : Window_chrome_command::MAXIMIZE;
}

void chrome::Example_window_chrome::paint(QPainter* painter)
{
    if (painter == nullptr) {
        return;
    }

    const QRectF titlebar_rect = item_rect(*this);
    if (titlebar_rect.isEmpty()) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->fillRect(titlebar_rect, window_chrome_background_color(m_window_active));

    const qreal                       device_pixel_ratio = paint_device_pixel_ratio(painter->device());
    const Window_chrome_title_content content            = title_content();
    const Window_chrome_layout        layout             = chrome_layout();

    paint_app_icon(*painter, m_app_icon, layout.icon_rect, device_pixel_ratio);

    QFont text_font = painter->font();
    text_font.setPointSizeF(9.5);
    painter->setFont(text_font);
    const QFontMetricsF metrics(text_font);

    if (content.spinner.has_value()) {
        painter->setPen(spinner_text_color(m_window_active));
        painter->drawText(
            layout.spinner_slot_rect,
            Qt::AlignCenter,
            *content.spinner);
    }

    const QString elided_title = metrics.elidedText(
        content.display_title,
        Qt::ElideRight,
        layout.title_text_rect.width());
    painter->setPen(title_text_color(m_window_active));
    painter->drawText(
        layout.title_text_rect,
        Qt::AlignLeft | Qt::AlignVCenter,
        elided_title);

    for (const Window_chrome_button_geometry& button : layout.buttons) {
        paint_button(*painter, button, m_window_active, layout.window_maximized);
    }

    const qreal separator_width = 1.0 / device_pixel_ratio;
    QPen separator_pen(titlebar_separator_color(m_window_active));
    separator_pen.setWidthF(separator_width);
    painter->setPen(separator_pen);
    painter->drawLine(
        QPointF(titlebar_rect.left(), titlebar_rect.bottom() - separator_width / 2.0),
        QPointF(titlebar_rect.right(), titlebar_rect.bottom() - separator_width / 2.0));

    painter->restore();
}

void chrome::Example_window_chrome::geometryChange(
    const QRectF&  new_geometry,
    const QRectF&  old_geometry)
{
    QQuickPaintedItem::geometryChange(new_geometry, old_geometry);
    update();
}

void chrome::Example_window_chrome::hoverMoveEvent(QHoverEvent* event)
{
    set_hovered_button(button_role_at(event->position()));
    QQuickPaintedItem::hoverMoveEvent(event);
}

void chrome::Example_window_chrome::hoverLeaveEvent(QHoverEvent* event)
{
    set_hovered_button(std::nullopt);
    QQuickPaintedItem::hoverLeaveEvent(event);
}

void chrome::Example_window_chrome::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QQuickPaintedItem::mousePressEvent(event);
        return;
    }

    const std::optional<Window_chrome_button_role> button = button_role_at(event->position());
    if (button.has_value()) {
        set_hovered_button(button);
        set_pressed_button(button);
        event->accept();
        return;
    }

    if (is_draggable_titlebar_point(event->position())) {
        event->accept();
        invoke_window_command(Window_chrome_command::START_SYSTEM_MOVE);
        return;
    }

    QQuickPaintedItem::mousePressEvent(event);
}

void chrome::Example_window_chrome::mouseMoveEvent(QMouseEvent* event)
{
    set_hovered_button(button_role_at(event->position()));
    QQuickPaintedItem::mouseMoveEvent(event);
}

void chrome::Example_window_chrome::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_pressed_button.has_value()) {
        QQuickPaintedItem::mouseReleaseEvent(event);
        return;
    }

    const std::optional<Window_chrome_button_role> released_button =
        button_role_at(event->position());
    const std::optional<Window_chrome_button_role> pressed_button = m_pressed_button;
    set_pressed_button(std::nullopt);
    set_hovered_button(released_button);
    event->accept();

    if (released_button == pressed_button) {
        invoke_window_command(command_for_button(*pressed_button));
    }
}

void chrome::Example_window_chrome::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QQuickPaintedItem::mouseDoubleClickEvent(event);
        return;
    }

    const Window_chrome_command command =
        titlebar_double_click_command_at(event->position());
    if (command == Window_chrome_command::NONE) {
        QQuickPaintedItem::mouseDoubleClickEvent(event);
        return;
    }

    event->accept();
    invoke_window_command(command);
}

void chrome::Example_window_chrome::mouseUngrabEvent()
{
    set_pressed_button(std::nullopt);
    QQuickPaintedItem::mouseUngrabEvent();
}

void chrome::Example_window_chrome::set_hovered_button(
    std::optional<Window_chrome_button_role> role)
{
    if (m_hovered_button == role) {
        return;
    }

    m_hovered_button = role;
    update();
}

void chrome::Example_window_chrome::set_pressed_button(
    std::optional<Window_chrome_button_role> role)
{
    if (m_pressed_button == role) {
        return;
    }

    m_pressed_button = role;
    update();
}

void chrome::Example_window_chrome::invoke_window_command(Window_chrome_command command)
{
    QWindow* target_window = window();
    if (target_window == nullptr) {
        return;
    }

    switch (command) {
        case Window_chrome_command::NONE:              break;
        case Window_chrome_command::START_SYSTEM_MOVE: (void)target_window->startSystemMove(); break;
        case Window_chrome_command::MINIMIZE:          target_window->showMinimized();         break;
        case Window_chrome_command::MAXIMIZE:          target_window->showMaximized();         break;
        case Window_chrome_command::RESTORE:           target_window->showNormal();            break;
        case Window_chrome_command::CLOSE:             target_window->close();                 break;
    }
}
