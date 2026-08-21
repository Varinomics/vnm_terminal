#include "vnm_terminal/app_support/app_settings.h"

#include "app_settings_keys.h"

#include "vnm_terminal/internal/terminal_color_scheme.h"

#include <QLatin1String>
#include <QSettings>

#include <cmath>

namespace vnm_terminal::terminal_app {

namespace {

std::optional<int> settings_int_value(QSettings& settings, const char* key)
{
    if (!settings.contains(QLatin1String(key))) {
        return std::nullopt;
    }

    bool      ok    = false;
    const int value = settings.value(QLatin1String(key)).toInt(&ok);
    return ok ? std::optional<int>(value) : std::nullopt;
}

std::optional<qreal> settings_font_size(QSettings& settings)
{
    if (!settings.contains(QLatin1String(k_window_settings_font_size))) {
        return std::nullopt;
    }

    bool         ok        = false;
    const double font_size =
        settings.value(QLatin1String(k_window_settings_font_size)).toDouble(&ok);
    if (!ok || !std::isfinite(font_size) || font_size <= 0.0) {
        return std::nullopt;
    }
    return static_cast<qreal>(font_size);
}

} // namespace

Terminal_settings_snapshot load_terminal_settings_snapshot(QSettings& settings)
{
    Terminal_settings_snapshot snapshot;

    settings.beginGroup(QLatin1String(k_window_settings_group));
    if (const std::optional<qreal> font_size = settings_font_size(settings)) {
        snapshot.font_size = *font_size;
    }
    settings.endGroup();

    settings.beginGroup(QLatin1String(k_appearance_settings_group));
    const QString color_scheme =
        settings.value(QLatin1String(k_appearance_color_scheme)).toString().trimmed();
    if (!color_scheme.isEmpty() &&
        vnm_terminal::internal::find_color_scheme(color_scheme) != nullptr)
    {
        snapshot.color_scheme = color_scheme;
    }

    const QString font_family =
        settings.value(QLatin1String(k_appearance_font_family)).toString().trimmed();
    if (!font_family.isEmpty()) {
        snapshot.font_family = font_family;
    }

    if (const std::optional<int> mode =
            settings_int_value(settings, k_appearance_text_renderer_mode))
    {
        const int minimum =
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::AUTO);
        const int maximum =
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH);
        if (*mode == static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::MSDF)) {
            snapshot.text_renderer_mode = minimum;
        } else if (*mode >= minimum && *mode <= maximum) {
            snapshot.text_renderer_mode = *mode;
        }
    }

    if (const std::optional<int> order =
            settings_int_value(settings, k_appearance_lcd_subpixel_order))
    {
        const int minimum =
            static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::AUTO);
        const int maximum =
            static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::VBGR);
        if (*order >= minimum && *order <= maximum) {
            snapshot.lcd_subpixel_order = *order;
        }
    }

    if (settings.contains(QLatin1String(k_appearance_row_timestamp_tooltip))) {
        snapshot.row_timestamp_tooltip_enabled = settings.value(
            QLatin1String(k_appearance_row_timestamp_tooltip)).toBool();
    }

    if (const std::optional<int> scrollback_limit =
            settings_int_value(settings, k_appearance_scrollback_limit);
        scrollback_limit.has_value() && *scrollback_limit >= 0)
    {
        snapshot.scrollback_limit = scrollback_limit;
    }
    settings.endGroup();
    return snapshot;
}

Terminal_settings_snapshot terminal_settings_snapshot(
    const VNM_TerminalSurface& surface)
{
    Terminal_settings_snapshot snapshot;
    snapshot.color_scheme = surface.color_scheme();
    snapshot.font_family  = surface.font_family();
    snapshot.font_size    = surface.font_size();
    snapshot.text_renderer_mode = static_cast<int>(surface.text_renderer_mode());
    snapshot.lcd_subpixel_order = static_cast<int>(surface.lcd_subpixel_order());
    snapshot.row_timestamp_tooltip_enabled =
        surface.row_timestamp_tooltip_enabled();
    snapshot.scrollback_limit = surface.scrollback_limit();
    return snapshot;
}

void save_terminal_settings_snapshot(
    QSettings&                        settings,
    const Terminal_settings_snapshot& snapshot)
{
    settings.beginGroup(QLatin1String(k_window_settings_group));
    if (std::isfinite(snapshot.font_size) && snapshot.font_size > 0.0) {
        settings.setValue(
            QLatin1String(k_window_settings_font_size),
            snapshot.font_size);
    }
    settings.endGroup();

    settings.beginGroup(QLatin1String(k_appearance_settings_group));
    if (vnm_terminal::internal::find_color_scheme(snapshot.color_scheme) != nullptr) {
        settings.setValue(
            QLatin1String(k_appearance_color_scheme),
            snapshot.color_scheme);
    }
    if (!snapshot.font_family.trimmed().isEmpty()) {
        settings.setValue(
            QLatin1String(k_appearance_font_family),
            snapshot.font_family);
    }

    const int minimum_renderer =
        static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::AUTO);
    const int maximum_renderer =
        static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH);
    // Forced MSDF is a runtime diagnostic and must not replace the user's
    // persisted AUTO or GLYPH preference when another appearance value changes.
    // An embedded host can select it without a command line, so this stays a
    // suppression of the value itself.
    const int transient_msdf_renderer =
        static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::MSDF);
    if (snapshot.text_renderer_mode >= minimum_renderer &&
        snapshot.text_renderer_mode <= maximum_renderer &&
        snapshot.text_renderer_mode != transient_msdf_renderer)
    {
        settings.setValue(
            QLatin1String(k_appearance_text_renderer_mode),
            snapshot.text_renderer_mode);
    }

    const int minimum_subpixel =
        static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::AUTO);
    const int maximum_subpixel =
        static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::VBGR);
    if (snapshot.lcd_subpixel_order >= minimum_subpixel &&
        snapshot.lcd_subpixel_order <= maximum_subpixel)
    {
        settings.setValue(
            QLatin1String(k_appearance_lcd_subpixel_order),
            snapshot.lcd_subpixel_order);
    }

    settings.setValue(
        QLatin1String(k_appearance_row_timestamp_tooltip),
        snapshot.row_timestamp_tooltip_enabled);
    if (snapshot.scrollback_limit.has_value() && *snapshot.scrollback_limit >= 0) {
        settings.setValue(
            QLatin1String(k_appearance_scrollback_limit),
            *snapshot.scrollback_limit);
    } else {
        settings.remove(QLatin1String(k_appearance_scrollback_limit));
    }
    settings.endGroup();
    settings.sync();
}

void apply_terminal_settings_snapshot(
    const Terminal_settings_snapshot& snapshot,
    VNM_TerminalSurface&              surface)
{
    surface.set_color_scheme(snapshot.color_scheme);
    if (!snapshot.font_family.trimmed().isEmpty()) {
        surface.set_font_family(snapshot.font_family);
    }
    if (std::isfinite(snapshot.font_size) && snapshot.font_size > 0.0) {
        surface.set_font_size(snapshot.font_size);
    }

    const int minimum_renderer =
        static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::AUTO);
    const int maximum_renderer =
        static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH);
    if (snapshot.text_renderer_mode >= minimum_renderer &&
        snapshot.text_renderer_mode <= maximum_renderer)
    {
        surface.set_text_renderer_mode(
            static_cast<VNM_TerminalSurface::Text_renderer_mode>(
                snapshot.text_renderer_mode));
    }

    const int minimum_subpixel =
        static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::AUTO);
    const int maximum_subpixel =
        static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::VBGR);
    if (snapshot.lcd_subpixel_order >= minimum_subpixel &&
        snapshot.lcd_subpixel_order <= maximum_subpixel)
    {
        surface.set_lcd_subpixel_order(
            static_cast<VNM_TerminalSurface::Lcd_subpixel_order>(
                snapshot.lcd_subpixel_order));
    }

    surface.set_row_timestamp_tooltip_enabled(
        snapshot.row_timestamp_tooltip_enabled);
    if (snapshot.scrollback_limit.has_value() && *snapshot.scrollback_limit >= 0) {
        surface.set_scrollback_limit(*snapshot.scrollback_limit);
    }
}

} // namespace vnm_terminal::terminal_app
