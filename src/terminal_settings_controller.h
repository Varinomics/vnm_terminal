#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace vnm_terminal::terminal_app {

// Supplies settings-panel helpers QML cannot derive itself.
class Terminal_settings_controller final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString buildProvenanceText READ build_provenance_text CONSTANT)

public:
    explicit Terminal_settings_controller(QObject* parent = nullptr);

    QString build_provenance_text() const;

    Q_INVOKABLE QStringList available_font_families() const;
};

} // namespace vnm_terminal::terminal_app
