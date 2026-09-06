#pragma once

#include "vnm_terminal/app_support/app_settings.h"

#include <QVariantMap>

#include <optional>

class QJsonValue;
class QSettings;

namespace vnm_terminal::terminal_app {

QVariantMap terminal_settings_payload(const Terminal_settings_snapshot& settings);

// User edits require a named font and an available palette; malformed edits
// must be rejected before they can enter the shared state or its store.
bool terminal_settings_changes_valid(const QVariantMap& changes);

// Missing fields retain the supplied base, including an absent scrollback
// limit. Empty snapshot fonts and unknown palette names retain the surface's
// own value when applied; they are not accepted as user-edit deltas.
std::optional<Terminal_settings_snapshot> terminal_settings_from_json(
    const QJsonValue& value, Terminal_settings_snapshot base);

// One application owns this state; workers report deltas rather than replacing
// each other's settings with independently captured snapshots. Store selection
// and whether persistence is enabled belong to the embedding application.
class Terminal_display_settings
{
public:
    Terminal_display_settings(bool dark_mode, QSettings* store = nullptr);
    const QVariantMap& values() const;
    bool apply_changes(const QVariantMap& changes, bool source_dark_mode);
    bool set_dark_mode(bool dark_mode);

private:
    QSettings* m_store;
    QVariantMap m_values;
    QString m_dark_scheme = QStringLiteral("Classic");
    QString m_light_scheme = QStringLiteral("Solarized Light");
    bool m_dark_mode;
};

} // namespace vnm_terminal::terminal_app
