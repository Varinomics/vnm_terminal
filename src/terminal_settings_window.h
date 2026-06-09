#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

class QQmlEngine;
class QQuickWindow;
class QWindow;

namespace vnm_terminal::terminal_app {

// Frameless, chrome-styled child window that hosts the terminal settings panel.
// It owns a self-contained QML Window (its own VNM_ChromeTitleBar with drag,
// resize, and close) created from the shared chrome runtime, and is shown on
// demand when the user activates the titlebar settings (gear) button.
class Terminal_settings_window final : public QObject
{
    Q_OBJECT

public:
    explicit Terminal_settings_window(QQmlEngine& engine, QObject* parent = nullptr);
    ~Terminal_settings_window() override;

    bool    is_valid() const;
    QString error_string() const;

    void set_transient_parent(QWindow* parent);

public slots:
    void show_window();

private slots:
    void handle_close_requested();
    void handle_move_requested();
    void handle_resize_requested(int edges);

private:
    void center_over_transient_parent();

    std::unique_ptr<QObject> m_root_object;
    QPointer<QQuickWindow>    m_window;
    QString                  m_error_string;
    bool                     m_positioned = false;
};

} // namespace vnm_terminal::terminal_app
