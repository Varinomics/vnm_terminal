#pragma once

#include "vnm_terminal/app_support/app_settings.h"

#include <QMap>
#include <QObject>
#include <QString>
#include <QVariant>

#include <functional>
#include <optional>

namespace vnm_terminal::terminal_app {

struct Terminal_setting_delta
{
    QString  key;
    QVariant value;
    quint64  sequence = 0;
    bool     dark_mode = true;
};

// One worker owns this journal for one surface. The embedding worker validates
// its wire envelope and emits the delta; the manager owns durable settings.
class Terminal_settings_reconciler final : public QObject
{
public:
    using Changed = std::function<void(const Terminal_setting_delta&)>;

    Terminal_settings_reconciler(
        VNM_TerminalSurface& surface, bool dark_mode, Changed changed);

    // The incoming snapshot and acknowledgement have already been validated
    // by the worker. A newer local edit wins until its sequence is acknowledged.
    Terminal_settings_snapshot apply_manager_snapshot(
        Terminal_settings_snapshot settings,
        bool                       dark_mode,
        quint64                    acknowledged_change);

    // The picker reports intent before setting the surface. A different family
    // is observed through the surface signal; an unchanged fallback still
    // replaces the unavailable stored preference when explicitly selected.
    void notify_font_family_selection(const QString& family);

private:
    struct Pending_change
    {
        QVariant value;
        quint64  sequence = 0;
        bool     dark_mode = true;
    };

    void record_change(const QString& key, QVariant value);

    VNM_TerminalSurface& m_surface;
    Changed              m_changed;
    QMap<QString, Pending_change> m_pending;
    std::optional<bool> m_copy_on_select_override;
    quint64 m_sequence = 0;
    bool    m_dark_mode;
    bool    m_suppress_changes = false;
};

} // namespace vnm_terminal::terminal_app
