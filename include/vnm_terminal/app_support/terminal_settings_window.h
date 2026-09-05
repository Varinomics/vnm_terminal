#pragma once

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QtGlobal>

#include <functional>
#include <memory>

class QQmlEngine;
class QQuickWindow;
class QScreen;
class QWindow;
class VNM_TerminalSurface;

namespace vnm_terminal::terminal_app {

class Terminal_settings_controller;

// Frameless, chrome-styled child window that hosts the terminal settings panel.
// It owns a self-contained QML Window (its own VNM_ChromeTitleBar with drag,
// resize, and close) created from the shared chrome runtime, and is shown on
// demand when the user activates the titlebar settings (gear) button. The
// panel binds its controls to the live surface and the settings controller,
// which are exposed to the QML as context properties.
class Terminal_settings_window final : public QObject
{
    Q_OBJECT

public:
    using Native_anchor_id_provider = std::function<quintptr()>;

    Terminal_settings_window(
        QQmlEngine&                   engine,
        VNM_TerminalSurface&          surface,
        Terminal_settings_controller& controller,
        bool                          interaction_diagnostics_unlocked = false,
        QObject*                      parent = nullptr);
    ~Terminal_settings_window() override;

    bool    is_valid() const;
    QString error_string() const;

    void set_transient_parent(QWindow* parent);
    void set_dark_mode(bool dark_mode);
    void set_fallback_anchor_window_title(const QString& title);
    // On Windows the provider is copied and called once at the start of each
    // show. Its native id is used only when it names a visible top-level
    // window. Other platforms retain their existing placement behavior.
    void set_native_anchor_id_provider(Native_anchor_id_provider provider);

public slots:
    void show_window();

private slots:
    void handle_close_requested();
    void handle_move_requested();
    void handle_resize_requested(int edges);

private:
    void apply_available_geometry(const QRect& available_geometry);
    void clamp_to_available_geometry(const QRect& available_geometry);
    void place_within_anchor(const QRect& native_anchor_geometry = {});
    void watch_available_geometry(QScreen* screen);

    std::unique_ptr<QObject> m_root_object;
    QPointer<QQuickWindow>   m_window;
    QPointer<QScreen>        m_available_geometry_screen;
    QMetaObject::Connection  m_available_geometry_connection;
    QMetaObject::Connection  m_transient_parent_screen_connection;
    QString                  m_error_string;
    QString                   m_fallback_anchor_window_title;
    Native_anchor_id_provider m_native_anchor_id_provider;
    bool                      m_positioned = false;
};

} // namespace vnm_terminal::terminal_app
