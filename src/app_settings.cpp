#include "vnm_terminal/app_support/app_settings.h"

#include "app_settings_keys.h"
#include "app_settings_internal.h"

#include "vnm_terminal/internal/terminal_color_scheme.h"

#include <QLatin1String>
#include <QSettings>

#include <cmath>

namespace vnm_terminal::terminal_app {

namespace detail {

std::optional<int> settings_int_value(QSettings& settings, const char* key)
{
    if (!settings.contains(QLatin1String(key))) {
        return std::nullopt;
    }

    bool      ok    = false;
    const int value = settings.value(QLatin1String(key)).toInt(&ok);
    return ok ? std::optional<int>(value) : std::nullopt;
}

std::optional<int> settings_scrollback_buffer_size_mib(QSettings& settings)
{
    const std::optional<int> size_mib = settings_int_value(
        settings,
        k_appearance_scrollback_buffer_size_mib);
    if (!size_mib.has_value()) {
        return std::nullopt;
    }

    const int minimum_mib = static_cast<int>(
        (VNM_TerminalSurface::minimum_retained_history_capacity_bytes() +
            k_bytes_per_mib - 1U) /
        k_bytes_per_mib);
    const int maximum_mib = static_cast<int>(
        VNM_TerminalSurface::maximum_retained_history_capacity_bytes() /
        k_bytes_per_mib);
    if (*size_mib < minimum_mib || *size_mib > maximum_mib) {
        return std::nullopt;
    }

    return size_mib;
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

std::optional<bool> settings_bool_value(QSettings& settings, const char* key)
{
    if (!settings.contains(QLatin1String(key))) {
        return std::nullopt;
    }
    return settings.value(QLatin1String(key)).toBool();
}

std::optional<QColor> settings_color_value(QSettings& settings, const char* key)
{
    const QString text = settings.value(QLatin1String(key)).toString().trimmed();
    if (text.isEmpty()) {
        return std::nullopt;
    }

    const QColor color = QColor::fromString(text);
    return color.isValid() ? std::optional<QColor>(color) : std::nullopt;
}

namespace {

std::optional<int> settings_int_range(
    QSettings& settings,
    const char* key,
    int         minimum,
    int         maximum)
{
    const std::optional<int> value = settings_int_value(settings, key);
    return value.has_value() && *value >= minimum && *value <= maximum
        ? value
        : std::nullopt;
}

template <typename Value_t>
void apply_if_present(const std::optional<Value_t>& value, Value_t& target)
{
    if (value.has_value()) {
        target = *value;
    }
}

} // namespace

Persisted_appearance_settings load_persisted_appearance_settings(QSettings& settings)
{
    Persisted_appearance_settings appearance;
    settings.beginGroup(QLatin1String(k_appearance_settings_group));

    const QString color_scheme =
        settings.value(QLatin1String(k_appearance_color_scheme)).toString().trimmed();
    if (!color_scheme.isEmpty() &&
        vnm_terminal::internal::find_color_scheme(color_scheme) != nullptr)
    {
        appearance.color_scheme = color_scheme;
    }

    const QString font_family =
        settings.value(QLatin1String(k_appearance_font_family)).toString().trimmed();
    if (!font_family.isEmpty()) {
        appearance.font_family = font_family;
    }

    appearance.font_advance_policy = settings_int_range(
        settings,
        k_appearance_font_advance_policy,
        static_cast<int>(vnm_terminal::Font_advance_policy::ADJUST_FONT_SIZE),
        static_cast<int>(vnm_terminal::Font_advance_policy::SNAP_ADVANCE_NEAREST));
    appearance.text_renderer_mode = settings_int_range(
        settings,
        k_appearance_text_renderer_mode,
        static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::AUTO),
        static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH));
    if (appearance.text_renderer_mode ==
        static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::MSDF))
    {
        appearance.text_renderer_mode =
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::AUTO);
    }
    appearance.lcd_subpixel_order = settings_int_range(
        settings,
        k_appearance_lcd_subpixel_order,
        static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::AUTO),
        static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::VBGR));
    appearance.invert_brightness =
        settings_bool_value(settings, k_appearance_invert_brightness);
    appearance.row_timestamp_tooltip =
        settings_bool_value(settings, k_appearance_row_timestamp_tooltip);
    appearance.scrollback_buffer_size_mib =
        settings_scrollback_buffer_size_mib(settings);
    appearance.chrome_focused_background =
        settings_color_value(settings, k_appearance_chrome_focused_background);
    appearance.chrome_unfocused_background =
        settings_color_value(settings, k_appearance_chrome_unfocused_background);
    appearance.chrome_focused_frame_edge =
        settings_color_value(settings, k_appearance_chrome_focused_frame_edge);
    appearance.chrome_unfocused_frame_edge =
        settings_color_value(settings, k_appearance_chrome_unfocused_frame_edge);

    settings.endGroup();
    return appearance;
}

void save_persisted_appearance_settings(
    QSettings&                           settings,
    const Persisted_appearance_settings& appearance,
    Missing_scrollback_setting_policy    missing_scrollback_policy)
{
    settings.beginGroup(QLatin1String(k_appearance_settings_group));
    if (appearance.color_scheme.has_value() &&
        vnm_terminal::internal::find_color_scheme(*appearance.color_scheme) != nullptr)
    {
        settings.setValue(QLatin1String(k_appearance_color_scheme), *appearance.color_scheme);
    }
    if (appearance.font_family.has_value() && !appearance.font_family->trimmed().isEmpty()) {
        settings.setValue(QLatin1String(k_appearance_font_family), *appearance.font_family);
    }

    const auto write_int = [&settings](const char* key, const std::optional<int>& value,
                              int minimum, int maximum) {
        if (value.has_value() && *value >= minimum && *value <= maximum) {
            settings.setValue(QLatin1String(key), *value);
        }
    };
    write_int(
        k_appearance_font_advance_policy,
        appearance.font_advance_policy,
        static_cast<int>(vnm_terminal::Font_advance_policy::ADJUST_FONT_SIZE),
        static_cast<int>(vnm_terminal::Font_advance_policy::SNAP_ADVANCE_NEAREST));
    // MSDF is a transient runtime diagnostic, not a stored preference.
    if (appearance.text_renderer_mode !=
        static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::MSDF))
    {
        write_int(
            k_appearance_text_renderer_mode,
            appearance.text_renderer_mode,
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::AUTO),
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH));
    }
    write_int(
        k_appearance_lcd_subpixel_order,
        appearance.lcd_subpixel_order,
        static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::AUTO),
        static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::VBGR));

    if (appearance.invert_brightness.has_value()) {
        settings.setValue(
            QLatin1String(k_appearance_invert_brightness),
            *appearance.invert_brightness);
    }
    if (appearance.row_timestamp_tooltip.has_value()) {
        settings.setValue(
            QLatin1String(k_appearance_row_timestamp_tooltip),
            *appearance.row_timestamp_tooltip);
    }
    if (appearance.scrollback_buffer_size_mib.has_value()) {
        settings.setValue(
            QLatin1String(k_appearance_scrollback_buffer_size_mib),
            *appearance.scrollback_buffer_size_mib);
    } else if (missing_scrollback_policy == Missing_scrollback_setting_policy::Remove) {
        settings.remove(QLatin1String(k_appearance_scrollback_buffer_size_mib));
    }
    settings.remove(QLatin1String(k_appearance_scrollback_limit));
    settings.endGroup();
    settings.sync();
}

} // namespace detail

Terminal_settings_snapshot load_terminal_settings_snapshot(QSettings& settings)
{
    Terminal_settings_snapshot snapshot;

    settings.beginGroup(QLatin1String(k_window_settings_group));
    if (const std::optional<qreal> font_size = detail::settings_font_size(settings)) {
        snapshot.font_size = *font_size;
    }
    settings.endGroup();

    const Persisted_appearance_settings appearance =
        detail::load_persisted_appearance_settings(settings);
    detail::apply_if_present(appearance.color_scheme, snapshot.color_scheme);
    detail::apply_if_present(appearance.font_family, snapshot.font_family);
    detail::apply_if_present(appearance.text_renderer_mode, snapshot.text_renderer_mode);
    detail::apply_if_present(appearance.font_advance_policy, snapshot.font_advance_policy);
    detail::apply_if_present(appearance.lcd_subpixel_order, snapshot.lcd_subpixel_order);
    detail::apply_if_present(appearance.invert_brightness, snapshot.invert_brightness);
    detail::apply_if_present(
        appearance.row_timestamp_tooltip,
        snapshot.row_timestamp_tooltip_enabled);
    snapshot.scrollback_buffer_size_mib = appearance.scrollback_buffer_size_mib;
    return snapshot;
}

Terminal_settings_snapshot terminal_settings_snapshot(
    const VNM_TerminalSurface& surface)
{
    Terminal_settings_snapshot snapshot;
    snapshot.color_scheme = surface.color_scheme();
    snapshot.font_family  = surface.font_family();
    snapshot.font_size    = surface.font_size();
    snapshot.font_advance_policy = surface.font_advance_policy_value();
    snapshot.text_renderer_mode = static_cast<int>(surface.text_renderer_mode());
    snapshot.lcd_subpixel_order = static_cast<int>(surface.lcd_subpixel_order());
    snapshot.invert_brightness = surface.invert_brightness();
    snapshot.row_timestamp_tooltip_enabled =
        surface.row_timestamp_tooltip_enabled();
    snapshot.scrollback_buffer_size_mib = surface.scrollback_buffer_size_mib();
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

    Persisted_appearance_settings appearance;
    appearance.color_scheme = snapshot.color_scheme;
    appearance.font_family = snapshot.font_family;
    appearance.font_advance_policy = snapshot.font_advance_policy;
    appearance.text_renderer_mode = snapshot.text_renderer_mode;
    appearance.lcd_subpixel_order = snapshot.lcd_subpixel_order;
    appearance.invert_brightness = snapshot.invert_brightness;
    appearance.row_timestamp_tooltip = snapshot.row_timestamp_tooltip_enabled;
    appearance.scrollback_buffer_size_mib = snapshot.scrollback_buffer_size_mib;
    detail::save_persisted_appearance_settings(
        settings,
        appearance,
        detail::Missing_scrollback_setting_policy::Remove);
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
    const int minimum_policy =
        static_cast<int>(vnm_terminal::Font_advance_policy::ADJUST_FONT_SIZE);
    const int maximum_policy =
        static_cast<int>(vnm_terminal::Font_advance_policy::SNAP_ADVANCE_NEAREST);
    if (snapshot.font_advance_policy >= minimum_policy &&
        snapshot.font_advance_policy <= maximum_policy)
    {
        surface.set_font_advance_policy_value(snapshot.font_advance_policy);
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

    surface.set_invert_brightness(snapshot.invert_brightness);

    surface.set_row_timestamp_tooltip_enabled(
        snapshot.row_timestamp_tooltip_enabled);
    if (snapshot.scrollback_buffer_size_mib.has_value()) {
        surface.set_scrollback_buffer_size_mib(*snapshot.scrollback_buffer_size_mib);
    }
}

} // namespace vnm_terminal::terminal_app
