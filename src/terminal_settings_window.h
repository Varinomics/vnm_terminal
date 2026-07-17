#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

class QQmlEngine;
class QQuickWindow;
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
    void set_fallback_anchor_window_title(const QString& title);

public slots:
    void show_window();

private slots:
    void handle_close_requested();
    void handle_move_requested();
    void handle_resize_requested(int edges);

private:
    void place_within_transient_parent();

    std::unique_ptr<QObject> m_root_object;
    QPointer<QQuickWindow>   m_window;
    QString                  m_error_string;
    QString                  m_fallback_anchor_window_title;
    bool                     m_positioned = false;
};

} // namespace vnm_terminal::terminal_app
