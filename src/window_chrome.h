#pragma once

#include "window_chrome_model.h"

#include <QColor>
#include <QIcon>
#include <QPointF>
#include <QQuickPaintedItem>
#include <QString>

#include <optional>

namespace vnm_terminal::terminal_app {

enum class Window_chrome_command
{
    NONE,
    START_SYSTEM_MOVE,
    MINIMIZE,
    MAXIMIZE,
    RESTORE,
    CLOSE,
};

QColor window_chrome_background_color(bool active);

class Terminal_window_chrome : public QQuickPaintedItem
{
public:
    explicit Terminal_window_chrome(QQuickItem* parent = nullptr);

    QString terminal_title() const;
    QString terminal_icon_name() const;
    bool window_active() const;
    bool window_maximized() const;

    void set_terminal_title(const QString& terminal_title);
    void set_terminal_icon_name(const QString& terminal_icon_name);
    void set_window_active(bool active);
    void set_window_maximized(bool maximized);

    Window_chrome_title_content title_content() const;
    Window_chrome_button_states button_states() const;
    Window_chrome_layout chrome_layout() const;

    std::optional<Window_chrome_button_role> button_role_at(const QPointF& point) const;
    bool is_draggable_titlebar_point(const QPointF& point) const;

    Window_chrome_command command_for_button(Window_chrome_button_role role) const;
    Window_chrome_command button_command_at(const QPointF& point) const;
    Window_chrome_command titlebar_double_click_command_at(const QPointF& point) const;

    void paint(QPainter* painter) override;

protected:
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;

    virtual void invoke_window_command(Window_chrome_command command);

private:
    void set_hovered_button(std::optional<Window_chrome_button_role> role);
    void set_pressed_button(std::optional<Window_chrome_button_role> role);

    QString                                    m_terminal_title;
    QString                                    m_terminal_icon_name;
    bool                                       m_window_active    = true;
    bool                                       m_window_maximized = false;

    std::optional<Window_chrome_button_role>   m_hovered_button;
    std::optional<Window_chrome_button_role>   m_pressed_button;
    QIcon                                      m_app_icon;
};

} // namespace vnm_terminal::terminal_app
