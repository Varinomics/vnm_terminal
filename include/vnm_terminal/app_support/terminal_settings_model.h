#pragma once

#include <QQmlPropertyMap>
#include <QStringList>
#include <QVariantMap>

namespace vnm_terminal::terminal_app {

// Edits application-owned terminal preferences without starting a terminal.
// The owner applies requested deltas and refreshes this model from its canonical state.
class Terminal_settings_model final : public QQmlPropertyMap
{
    Q_OBJECT

public:
    explicit Terminal_settings_model(QObject* parent = nullptr);

    void set_values(const QVariantMap& values);

    Q_INVOKABLE QStringList available_color_schemes() const;
    Q_INVOKABLE QVariantMap color_scheme_preview(const QString& scheme_name) const;

signals:
    void changes_requested(const QVariantMap& changes);

protected:
    QVariant updateValue(const QString& key, const QVariant& input) override;

private:
    QVariantMap m_values;
};

} // namespace vnm_terminal::terminal_app
