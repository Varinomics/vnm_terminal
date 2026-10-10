#include "vnm_terminal/app_support/terminal_settings_reconciler.h"

#include <QLatin1String>
#include <QScopedValueRollback>

#include <utility>

namespace vnm_terminal::terminal_app {

namespace {

bool mode_specific_setting(const QString& key)
{
    return key == QStringLiteral("color_scheme") ||
        key == QStringLiteral("invert_brightness");
}

void apply_pending_change(
    Terminal_settings_snapshot& settings,
    const QString&              key,
    const QVariant&             value)
{
    if (key == QStringLiteral("color_scheme")) {
        settings.color_scheme = value.toString();
    }
    else
    if (key == QStringLiteral("font_family")) {
        settings.font_family = value.toString();
    }
    else
    if (key == QStringLiteral("font_size")) {
        settings.font_size = value.value<qreal>();
    }
    else
    if (key == QStringLiteral("font_advance_policy")) {
        settings.font_advance_policy = value.toInt();
    }
    else
    if (key == QStringLiteral("text_renderer_mode")) {
        settings.text_renderer_mode = value.toInt();
    }
    else
    if (key == QStringLiteral("lcd_subpixel_order")) {
        settings.lcd_subpixel_order = value.toInt();
    }
    else
    if (key == QStringLiteral("invert_brightness")) {
        settings.invert_brightness = value.toBool();
    }
    else
    if (key == QStringLiteral("row_timestamp_tooltip_enabled")) {
        settings.row_timestamp_tooltip_enabled = value.toBool();
    }
    else
    if (key == QStringLiteral("scrollback_buffer_size_mib")) {
        settings.scrollback_buffer_size_mib = value.toInt();
    }
    else {
        Q_ASSERT_X(false, "Terminal_settings_reconciler", "unknown observed setting");
    }
}

} // namespace

Terminal_settings_reconciler::Terminal_settings_reconciler(
    VNM_TerminalSurface& surface, bool dark_mode, Changed changed)
:
    QObject(&surface),
    m_surface(surface),
    m_changed(std::move(changed)),
    m_dark_mode(dark_mode)
{
    const auto connect_setting = [this](auto signal, const char* key, auto member) {
        QObject::connect(&m_surface, signal, this, [this, key, member] {
            const Terminal_settings_snapshot settings =
                terminal_settings_snapshot(m_surface);
            record_change(QLatin1String(key), QVariant::fromValue(settings.*member));
        });
    };
    connect_setting(&VNM_TerminalSurface::font_size_changed,
        "font_size", &Terminal_settings_snapshot::font_size);
    connect_setting(&VNM_TerminalSurface::font_family_changed,
        "font_family", &Terminal_settings_snapshot::font_family);
    connect_setting(&VNM_TerminalSurface::font_advance_policy_changed,
        "font_advance_policy", &Terminal_settings_snapshot::font_advance_policy);
    connect_setting(&VNM_TerminalSurface::color_scheme_changed,
        "color_scheme", &Terminal_settings_snapshot::color_scheme);
    connect_setting(&VNM_TerminalSurface::invert_brightness_changed,
        "invert_brightness", &Terminal_settings_snapshot::invert_brightness);
    connect_setting(&VNM_TerminalSurface::text_renderer_mode_changed,
        "text_renderer_mode", &Terminal_settings_snapshot::text_renderer_mode);
    connect_setting(&VNM_TerminalSurface::lcd_subpixel_order_changed,
        "lcd_subpixel_order", &Terminal_settings_snapshot::lcd_subpixel_order);
    connect_setting(&VNM_TerminalSurface::row_timestamp_tooltip_enabled_changed,
        "row_timestamp_tooltip_enabled",
        &Terminal_settings_snapshot::row_timestamp_tooltip_enabled);
    QObject::connect(
        &m_surface, &VNM_TerminalSurface::copy_on_select_changed,
        this, [this] {
            if (!m_suppress_changes) {
                m_copy_on_select_override = m_surface.copy_on_select();
            }
        });
    QObject::connect(
        &m_surface, &VNM_TerminalSurface::scrollback_buffer_size_mib_changed,
        this, [this] {
            record_change(QStringLiteral("scrollback_buffer_size_mib"),
                m_surface.scrollback_buffer_size_mib());
        });
}

void Terminal_settings_reconciler::notify_font_family_selection(const QString& family)
{
    if (family == m_surface.font_family()) {
        record_change(QStringLiteral("font_family"), family);
    }
}

void Terminal_settings_reconciler::record_change(
    const QString& key, QVariant value)
{
    if (m_suppress_changes) {
        return;
    }
    const quint64 sequence = ++m_sequence;
    m_pending.insert(key, {value, sequence, m_dark_mode});
    if (m_changed) {
        m_changed({key, std::move(value), sequence, m_dark_mode});
    }
}

Terminal_settings_snapshot Terminal_settings_reconciler::apply_manager_snapshot(
    Terminal_settings_snapshot settings,
    bool                       dark_mode,
    quint64                    acknowledged_change)
{
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->sequence <= acknowledged_change) {
            it = m_pending.erase(it);
            continue;
        }
        if (!mode_specific_setting(it.key()) || it->dark_mode == dark_mode) {
            apply_pending_change(settings, it.key(), it->value);
        }
        ++it;
    }
    QScopedValueRollback suppress_changes(m_suppress_changes, true);
    m_dark_mode = dark_mode;
    if (m_copy_on_select_override.has_value()) {
        settings.copy_on_select = *m_copy_on_select_override;
    }
    apply_terminal_settings_snapshot(settings, m_surface);
    return settings;
}

} // namespace vnm_terminal::terminal_app
