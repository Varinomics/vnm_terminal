#include "app_settings.h"

#include "app_common.h"

#include "vnm_terminal/internal/terminal_color_scheme.h"

#include <QGuiApplication>
#include <QLatin1String>
#include <QRect>
#include <QScreen>
#include <QSettings>
#include <QWindow>

#include <cmath>

namespace vnm_terminal::terminal_app {

bool persisted_window_axis_is_valid(int value)
{
    return
        value >= k_persisted_window_min_axis &&
        value <= static_cast<int>(k_text_area_resize_max_window_axis);
}

std::optional<int> settings_int_value(QSettings& settings, const char* key)
{
    if (!settings.contains(QLatin1String(key))) {
        return std::nullopt;
    }

    bool      ok    = false;
    const int value = settings.value(QLatin1String(key)).toInt(&ok);
    if (!ok) {
        return std::nullopt;
    }

    return value;
}

std::optional<bool> settings_bool_value(QSettings& settings, const char* key)
{
    if (!settings.contains(QLatin1String(key))) {
        return std::nullopt;
    }

    return settings.value(QLatin1String(key)).toBool();
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

std::optional<QSize> settings_window_size(QSettings& settings)
{
    const std::optional<int> width  = settings_int_value(settings, k_window_settings_width);
    const std::optional<int> height = settings_int_value(settings, k_window_settings_height);
    if (!width.has_value() || !height.has_value()) {
        return std::nullopt;
    }

    if (!persisted_window_axis_is_valid(*width) ||
        !persisted_window_axis_is_valid(*height))
    {
        return std::nullopt;
    }

    return QSize(*width, *height);
}

std::optional<QPoint> settings_window_position(QSettings& settings)
{
    const std::optional<int> x = settings_int_value(settings, k_window_settings_x);
    const std::optional<int> y = settings_int_value(settings, k_window_settings_y);
    if (!x.has_value() || !y.has_value()) {
        return std::nullopt;
    }

    return QPoint(*x, *y);
}

Persisted_terminal_window_state load_persisted_terminal_window_state(
    QSettings& settings)
{
    Persisted_terminal_window_state state;
    settings.beginGroup(QLatin1String(k_window_settings_group));
    state.font_size = settings_font_size(settings);
    state.size      = settings_window_size(settings);
    state.position  = settings_window_position(settings);
    state.maximized =
        settings.value(QLatin1String(k_window_settings_maximized), false).toBool();
    settings.endGroup();
    return state;
}

void save_persisted_terminal_window_state(
    QSettings& settings,
    const Persisted_terminal_window_state& state)
{
    settings.beginGroup(QLatin1String(k_window_settings_group));
    if (state.font_size.has_value() &&
        std::isfinite(*state.font_size) &&
        *state.font_size > 0.0)
    {
        settings.setValue(QLatin1String(k_window_settings_font_size), *state.font_size);
    }

    if (state.size.has_value() &&
        persisted_window_axis_is_valid(state.size->width()) &&
        persisted_window_axis_is_valid(state.size->height()))
    {
        settings.setValue(QLatin1String(k_window_settings_width),  state.size->width());
        settings.setValue(QLatin1String(k_window_settings_height), state.size->height());
    }

    if (state.position.has_value()) {
        settings.setValue(QLatin1String(k_window_settings_x), state.position->x());
        settings.setValue(QLatin1String(k_window_settings_y), state.position->y());
    }

    settings.setValue(QLatin1String(k_window_settings_maximized), state.maximized);
    settings.endGroup();
    settings.sync();
}

Persisted_appearance_settings load_persisted_appearance_settings(QSettings& settings)
{
    Persisted_appearance_settings state;
    settings.beginGroup(QLatin1String(k_appearance_settings_group));

    const QString color_scheme =
        settings.value(QLatin1String(k_appearance_color_scheme)).toString().trimmed();
    if (!color_scheme.isEmpty()) {
        state.color_scheme = color_scheme;
    }

    const QString font_family =
        settings.value(QLatin1String(k_appearance_font_family)).toString().trimmed();
    if (!font_family.isEmpty()) {
        state.font_family = font_family;
    }

    state.text_renderer_mode    = settings_int_value(settings, k_appearance_text_renderer_mode);
    state.lcd_subpixel_order    = settings_int_value(settings, k_appearance_lcd_subpixel_order);
    state.row_timestamp_tooltip = settings_bool_value(settings, k_appearance_row_timestamp_tooltip);
    state.scrollback_limit      = settings_int_value(settings, k_appearance_scrollback_limit);

    settings.endGroup();
    return state;
}

void save_persisted_appearance_settings(
    QSettings&                  settings,
    const VNM_TerminalSurface&  surface)
{
    settings.beginGroup(QLatin1String(k_appearance_settings_group));
    settings.setValue(QLatin1String(k_appearance_color_scheme), surface.color_scheme());
    settings.setValue(QLatin1String(k_appearance_font_family),  surface.font_family());
    settings.setValue(
        QLatin1String(k_appearance_text_renderer_mode),
        static_cast<int>(surface.text_renderer_mode()));
    settings.setValue(
        QLatin1String(k_appearance_lcd_subpixel_order),
        static_cast<int>(surface.lcd_subpixel_order()));
    settings.setValue(
        QLatin1String(k_appearance_row_timestamp_tooltip),
        surface.row_timestamp_tooltip_enabled());
    settings.setValue(QLatin1String(k_appearance_scrollback_limit), surface.scrollback_limit());
    settings.endGroup();
    settings.sync();
}

void apply_persisted_appearance_settings(
    const Persisted_appearance_settings& state,
    App_options*                         options)
{
    if (!options->color_scheme_explicit &&
        state.color_scheme.has_value() &&
        vnm_terminal::internal::find_color_scheme(*state.color_scheme) != nullptr)
    {
        options->color_scheme = *state.color_scheme;
    }

    if (!options->font_family_explicit && state.font_family.has_value()) {
        options->font_family = *state.font_family;
    }

    if (!options->text_renderer_mode_explicit && state.text_renderer_mode.has_value()) {
        const int mode = *state.text_renderer_mode;
        if (mode >= static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::AUTO) &&
            mode <= static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH))
        {
            options->text_renderer_mode =
                static_cast<VNM_TerminalSurface::Text_renderer_mode>(mode);
        }
    }

    if (!options->lcd_subpixel_order_explicit && state.lcd_subpixel_order.has_value()) {
        const int order = *state.lcd_subpixel_order;
        if (order >= static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::AUTO) &&
            order <= static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::VBGR))
        {
            options->lcd_subpixel_order =
                static_cast<VNM_TerminalSurface::Lcd_subpixel_order>(order);
        }
    }

    if (!options->row_timestamp_tooltip_explicit && state.row_timestamp_tooltip.has_value()) {
        options->row_timestamp_tooltip_enabled = *state.row_timestamp_tooltip;
    }

    if (!options->scrollback_limit.has_value() &&
        state.scrollback_limit.has_value()     &&
        *state.scrollback_limit >= 0)
    {
        options->scrollback_limit = *state.scrollback_limit;
    }
}

bool terminal_window_persistence_enabled()
{
    return QGuiApplication::platformName() != QStringLiteral("offscreen");
}

bool window_geometry_intersects_available_screen(
    const QPoint& position,
    const QSize&  size)
{
    const QRect window_rect(position, size);
    for (const QScreen* screen : QGuiApplication::screens()) {
        if (screen->availableGeometry().intersects(window_rect)) {
            return true;
        }
    }

    return false;
}

void apply_persisted_terminal_window_state(
    const Persisted_terminal_window_state& state,
    App_options*                           options)
{
    if (!options->font_size_explicit && state.font_size.has_value()) {
        options->font_size = *state.font_size;
    }

    if (!options->window_size_explicit && state.size.has_value()) {
        options->window_size = *state.size;
    }

    if (state.position.has_value() &&
        window_geometry_intersects_available_screen(
            *state.position, options->window_size))
    {
        options->window_position = *state.position;
    }

    options->restore_maximized_window_state =
        state.maximized && !options->window_size_explicit;
}

std::optional<Persisted_terminal_window_state> restorable_terminal_window_state(
    const QWindow&              window,
    const VNM_TerminalSurface&  surface)
{
    const Qt::WindowStates window_states = window.windowStates();
    if (window_states.testFlag(Qt::WindowMinimized) ||
        window_states.testFlag(Qt::WindowMaximized) ||
        window_states.testFlag(Qt::WindowFullScreen))
    {
        return std::nullopt;
    }

    const QSize size = window.size();
    if (!persisted_window_axis_is_valid(size.width()) ||
        !persisted_window_axis_is_valid(size.height()))
    {
        return std::nullopt;
    }

    Persisted_terminal_window_state state;
    state.position  = window.position();
    state.size      = size;
    state.font_size = surface.font_size();
    state.maximized = false;
    return state;
}

} // namespace vnm_terminal::terminal_app
