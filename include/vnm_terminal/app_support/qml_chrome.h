#pragma once

#include <QColor>
#include <QDateTime>
#include <QObject>
#include <QPointF>
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

// The window chrome carries one palette while the window holds focus and a
// dimmer one while it does not. This is the single source for both: the QML
// chrome, the window clear color and the search bar all paint from it, so the
// three stay in step.
struct Terminal_chrome_palette
{
    QColor focused_background;
    QColor unfocused_background;
    QColor focused_frame_edge;
    QColor unfocused_frame_edge;
};

Terminal_chrome_palette default_terminal_chrome_palette();

const Terminal_chrome_palette& terminal_chrome_palette();

// Startup configuration. The QML chrome samples the palette once, when it is
// constructed, so this must run before the terminal window is set up.
void set_terminal_chrome_palette(const Terminal_chrome_palette& palette);

QColor terminal_chrome_background_color(bool active);
QColor terminal_chrome_frame_edge_color(bool active);

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
    QRectF content_interior_rect() const;
    qreal device_pixel_ratio() const;
    void set_title(const QString& title);
    void set_activity_marker_text(const QString& marker_text);
    void set_active(bool active);
    void set_maximized(bool maximized);
    void set_fullscreen(bool fullscreen);
    void set_resize_enabled(bool resize_enabled);
    void pulse_wheel_delivery_indicator();
    void show_row_timestamp_tooltip(const QPointF& position, const QDateTime& timestamp);
    void hide_row_timestamp_tooltip();

signals:
    void settings_requested();
    void title_edit_accepted(const QString& title);

private slots:
    void handle_move_requested();
    void handle_resize_requested(int edges);
    void handle_minimize_requested();
    void handle_maximize_toggle_requested();
    void handle_close_requested();
    void handle_settings_requested();
    void handle_title_edit_accepted(const QString& title);

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
