#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

#include <memory>

class QQmlApplicationEngine;

namespace vnm_terminal::terminal_app {

class Terminal_settings_model;
class Terminal_settings_window;

// Keeps application preferences editable independently of terminal lifetimes.
// The application owns persistence and applies the requested deltas.
class Terminal_settings_dialog final : public QObject
{
    Q_OBJECT

public:
    Terminal_settings_dialog(
        QQmlApplicationEngine* engine, QString anchor_window_title, QObject* parent = nullptr);
    ~Terminal_settings_dialog() override;

    bool show_window(const QVariantMap& values, bool dark_mode);
    void refresh(const QVariantMap& values, bool dark_mode);
    QString error_string() const;

signals:
    void changes_requested(const QVariantMap& changes);

private:
    QPointer<QQmlApplicationEngine> m_engine;
    QString m_anchor_window_title;
    QString m_error_string;
    std::unique_ptr<Terminal_settings_model> m_model;
    std::unique_ptr<Terminal_settings_window> m_window;
};

} // namespace vnm_terminal::terminal_app
