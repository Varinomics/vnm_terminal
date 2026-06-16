#pragma once

#include <QObject>
#include <QStringList>

namespace vnm_terminal::terminal_app {

// Supplies settings-panel helpers QML cannot derive itself.
class Terminal_settings_controller final : public QObject
{
    Q_OBJECT

public:
    explicit Terminal_settings_controller(QObject* parent = nullptr);

    Q_INVOKABLE QStringList available_font_families() const;
};

} // namespace vnm_terminal::terminal_app
