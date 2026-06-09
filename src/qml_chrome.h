#pragma once

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QTimer>
#include <QtGlobal>

#include <memory>

class QQmlEngine;
class QQuickItem;
class QQuickWindow;
class QVariant;

namespace vnm_terminal::terminal_app {

constexpr qreal k_default_frameless_resize_border_width      = 6.0;
constexpr qreal k_frameless_resize_border_physical_reduction = 2.0;

QColor terminal_chrome_background_color(bool active);
QColor terminal_chrome_content_border_color(bool active);

class Terminal_qml_chrome final : public QObject
{
    Q_OBJECT

public:
    Terminal_qml_chrome(QQmlEngine& engine, QQuickWindow& window);
    ~Terminal_qml_chrome() override;

    bool is_valid() const;
    QString error_string() const;

    QQuickItem* root_item() const;
    QQuickItem* titlebar_item() const;

    void set_size(const QSizeF& size);
    void set_content_border_rect(const QRectF& rect, qreal border_width);
    void set_title(const QString& title);
    void set_activity_marker_text(const QString& marker_text);
    void set_active(bool active);
    void set_maximized(bool maximized);
    void set_resize_enabled(bool resize_enabled);
    void pulse_wheel_delivery_indicator();

signals:
    void settings_requested();

private slots:
    void handle_move_requested();
    void handle_resize_requested(int edges);
    void handle_minimize_requested();
    void handle_maximize_toggle_requested();
    void handle_close_requested();
    void handle_settings_requested();

private:
    void connect_window_commands();
    void set_property(const char* property_name, const QVariant& value);
    void set_wheel_delivery_indicator_visible(bool visible);
    void toggle_window_maximized();

    QPointer<QQuickWindow> m_window;
    std::unique_ptr<QObject> m_root_object;
    QQuickItem* m_root_item     = nullptr;
    QQuickItem* m_titlebar_item = nullptr;
    QString     m_error_string;
    QTimer      m_wheel_delivery_indicator_timer;
};

} // namespace vnm_terminal::terminal_app
