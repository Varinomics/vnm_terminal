#include "vnm_terminal/app_support/terminal_settings_model.h"

#include "vnm_terminal/app_support/terminal_display_settings.h"
#include "vnm_terminal/terminal_canvas_appearance.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include <QLatin1String>

#include <cstddef>

namespace vnm_terminal::terminal_app {

namespace {

struct setting_name_t
{
    const char* property;
    const char* key;
};

constexpr setting_name_t k_setting_names[] = {
    {"colorScheme",                "color_scheme"},
    {"fontFamily",                 "font_family"},
    {"fontSize",                   "font_size"},
    {"fontAdvancePolicy",          "font_advance_policy"},
    {"textRendererMode",           "text_renderer_mode"},
    {"lcdSubpixelOrder",           "lcd_subpixel_order"},
    {"invertBrightness",           "invert_brightness"},
    {"rowTimestampTooltipEnabled", "row_timestamp_tooltip_enabled"},
    {"scrollbackBufferSizeMiB",     "scrollback_buffer_size_mib"},
};

constexpr std::size_t k_bytes_per_mib = 1024U * 1024U;

QString setting_key(const QString& property)
{
    for (const auto& name : k_setting_names) {
        if (property == QLatin1String(name.property)) {
            return QString::fromLatin1(name.key);
        }
    }
    return {};
}

} // namespace

Terminal_settings_model::Terminal_settings_model(QObject* parent)
:
    QQmlPropertyMap(this, parent)
{
    insert(QStringLiteral("minimumScrollbackBufferSizeMiB"), static_cast<int>(
        (VNM_TerminalSurface::minimum_retained_history_capacity_bytes() +
            k_bytes_per_mib - 1U) / k_bytes_per_mib));
    insert(QStringLiteral("maximumScrollbackBufferSizeMiB"), static_cast<int>(
        VNM_TerminalSurface::maximum_retained_history_capacity_bytes() / k_bytes_per_mib));
    set_values(terminal_settings_payload(Terminal_settings_snapshot{}));
    freeze();

    QObject::connect(
        this,
        &QQmlPropertyMap::valueChanged,
        this,
        [
            this
        ](
            const QString& property,
            const QVariant& value)
        {
            const QString key = setting_key(property);
            const QVariantMap changes{{key, value}};
            if (!key.isEmpty() && value != m_values.value(property) &&
                terminal_settings_changes_valid(changes))
            {
                emit changes_requested(changes);
            }
        });
}

void Terminal_settings_model::set_values(const QVariantMap& values)
{
    const QVariantMap defaults = terminal_settings_payload(Terminal_settings_snapshot{});
    for (const auto& name : k_setting_names) {
        const QString key = QString::fromLatin1(name.key);
        const QVariant fallback = key == QStringLiteral("scrollback_buffer_size_mib")
            ? QVariant(static_cast<int>(
                  VNM_TerminalSurface::default_retained_history_capacity_bytes() / k_bytes_per_mib))
            : defaults.value(key);
        const QString property = QString::fromLatin1(name.property);
        const QVariant value   = values.value(key, fallback);
        m_values.insert(property, value);
        insert(property, value);
    }
}

QVariant Terminal_settings_model::updateValue(const QString& property, const QVariant& input)
{
    const QString key = setting_key(property);
    if (key.isEmpty() || !terminal_settings_changes_valid({{key, input}})) {
        return value(property);
    }
    return input;
}

QStringList Terminal_settings_model::available_color_schemes() const
{
    return terminal_canvas_color_scheme_names();
}

QVariantMap Terminal_settings_model::color_scheme_preview(const QString& scheme_name) const
{
    return terminal_color_scheme_preview(scheme_name);
}

} // namespace vnm_terminal::terminal_app
