#include "app_settings.h"

#include "app_common.h"

#include "vnm_terminal/internal/terminal_color_scheme.h"

#include <QColor>
#include <QGuiApplication>
#include <QLatin1String>
#include <QRect>
#include <QScreen>
#include <QSettings>
#include <QWindow>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace vnm_terminal::terminal_app {

namespace {

// True while a setting the command line forced is still untouched, which is the
// only state in which its write must be suppressed. The comparison is exact by
// construction: both sides are the surface's own normalized value, one
// snapshotted at startup and one read now.
//
// The first save that sees the setting anywhere else releases the field for
// good, so a user who moves a forced setting and then returns it to the forced
// value still gets that final choice stored. Comparing without releasing would
// leave half the reachable states of a two-valued setting unpersistable for the
// whole session.
template <typename Value_t>
bool command_line_override_still_holds(
    std::optional<Value_t>& forced_value,
    const Value_t&          live_value)
{
    if (!forced_value.has_value()) {
        return false;
    }

    if (*forced_value == live_value) {
        return true;
    }

    forced_value.reset();
    return false;
}

} // namespace

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

std::optional<QColor> settings_color_value(QSettings& settings, const char* key)
{
    const QString text = settings.value(QLatin1String(key)).toString().trimmed();
    if (text.isEmpty()) {
        return std::nullopt;
    }

    const QColor color = QColor::fromString(text);
    if (!color.isValid()) {
        return std::nullopt;
    }

    return color;
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

Command_line_setting_overrides command_line_setting_overrides(
    const App_options&         options,
    const VNM_TerminalSurface& surface)
{
    Command_line_setting_overrides overrides;
    if (options.font_size_explicit) {
        overrides.font_size = surface.font_size();
    }

    if (options.color_scheme_explicit) {
        overrides.color_scheme = surface.color_scheme();
    }

    if (options.font_family_explicit) {
        overrides.font_family = surface.font_family();
    }

    if (options.text_renderer_mode_explicit) {
        overrides.text_renderer_mode = static_cast<int>(surface.text_renderer_mode());
    }

    if (options.lcd_subpixel_order_explicit) {
        overrides.lcd_subpixel_order = static_cast<int>(surface.lcd_subpixel_order());
    }

    if (options.row_timestamp_tooltip_explicit) {
        overrides.row_timestamp_tooltip = surface.row_timestamp_tooltip_enabled();
    }

    if (options.scrollback_limit_explicit) {
        overrides.scrollback_limit = surface.scrollback_limit();
    }

    // The window has no normalizing setter to read back, so unlike every other
    // forced setting there is nothing to snapshot here. The requested size is
    // the wrong thing to latch: the window system may normalize, clamp,
    // decorate, or move the window before anything is saved, and the first save
    // would then find a size that never equalled the request, release the latch,
    // and store geometry the user never chose. Latch what the run was actually
    // granted instead, at the first save that observes it.
    //
    // Position and maximized state ride along for the same run. An explicit size
    // forces the maximized state, because apply_persisted_terminal_window_state()
    // drops the stored maximized restore for it: the run cannot honor a size and
    // a maximized window at once. Position is not forced, but a window resized
    // to a size the desktop has to accommodate can be moved to fit, and that
    // move is the window system's decision rather than the user's.
    overrides.window_geometry_latch_pending = options.window_size_explicit;

    return overrides;
}

void save_persisted_terminal_window_state(
    QSettings&                             settings,
    const Persisted_terminal_window_state& state,
    Command_line_setting_overrides&        overrides)
{
    settings.beginGroup(QLatin1String(k_window_settings_group));
    if (state.font_size.has_value() &&
        std::isfinite(*state.font_size) &&
        *state.font_size > 0.0 &&
        !command_line_override_still_holds(overrides.font_size, *state.font_size))
    {
        settings.setValue(QLatin1String(k_window_settings_font_size), *state.font_size);
    }

    // The geometry the run was granted, taken the first time a save can see it.
    // From here the geometry fields behave like every other latched setting: the
    // save that finds one of them moved stores it and releases the latch for
    // good, so a resize, a move, or a maximize the user performs still becomes
    // their preference.
    if (overrides.window_geometry_latch_pending && state.size.has_value()) {
        overrides.window_size     = state.size;
        overrides.window_position = state.position;
        overrides.maximized       = state.maximized;
        overrides.window_geometry_latch_pending = false;
    }

    if (state.size.has_value() &&
        persisted_window_axis_is_valid(state.size->width()) &&
        persisted_window_axis_is_valid(state.size->height()) &&
        !command_line_override_still_holds(overrides.window_size, *state.size))
    {
        settings.setValue(QLatin1String(k_window_settings_width),  state.size->width());
        settings.setValue(QLatin1String(k_window_settings_height), state.size->height());
    }

    if (state.position.has_value() &&
        !command_line_override_still_holds(overrides.window_position, *state.position))
    {
        settings.setValue(QLatin1String(k_window_settings_x), state.position->x());
        settings.setValue(QLatin1String(k_window_settings_y), state.position->y());
    }

    if (!command_line_override_still_holds(overrides.maximized, state.maximized)) {
        settings.setValue(QLatin1String(k_window_settings_maximized), state.maximized);
    }

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

    state.chrome_focused_background =
        settings_color_value(settings, k_appearance_chrome_focused_background);
    state.chrome_unfocused_background =
        settings_color_value(settings, k_appearance_chrome_unfocused_background);
    state.chrome_focused_frame_edge =
        settings_color_value(settings, k_appearance_chrome_focused_frame_edge);
    state.chrome_unfocused_frame_edge =
        settings_color_value(settings, k_appearance_chrome_unfocused_frame_edge);

    // Forced MSDF was once exposed by the settings UI, but it deliberately
    // disables glyph fallback. Preserve fallback when adopting that stored value.
    if (state.text_renderer_mode.has_value() &&
        *state.text_renderer_mode ==
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::MSDF))
    {
        state.text_renderer_mode =
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::AUTO);
    }

    settings.endGroup();
    return state;
}

void save_persisted_appearance_settings(
    QSettings&                      settings,
    const VNM_TerminalSurface&      surface,
    Command_line_setting_overrides& overrides)
{
    settings.beginGroup(QLatin1String(k_appearance_settings_group));
    const QString color_scheme = surface.color_scheme();
    if (!command_line_override_still_holds(overrides.color_scheme, color_scheme)) {
        settings.setValue(QLatin1String(k_appearance_color_scheme), color_scheme);
    }

    const QString font_family = surface.font_family();
    if (!command_line_override_still_holds(overrides.font_family, font_family)) {
        settings.setValue(QLatin1String(k_appearance_font_family), font_family);
    }

    // Forced MSDF is a runtime diagnostic and must not replace the user's
    // persisted AUTO or GLYPH preference when another appearance value changes.
    // An embedded host can select it without a command line, so this stays a
    // suppression of the value itself, independent of the override rule.
    const int text_renderer_mode = static_cast<int>(surface.text_renderer_mode());
    if (surface.text_renderer_mode() != VNM_TerminalSurface::Text_renderer_mode::MSDF &&
        !command_line_override_still_holds(overrides.text_renderer_mode, text_renderer_mode))
    {
        settings.setValue(QLatin1String(k_appearance_text_renderer_mode), text_renderer_mode);
    }

    const int lcd_subpixel_order = static_cast<int>(surface.lcd_subpixel_order());
    if (!command_line_override_still_holds(overrides.lcd_subpixel_order, lcd_subpixel_order)) {
        settings.setValue(QLatin1String(k_appearance_lcd_subpixel_order), lcd_subpixel_order);
    }

    const bool row_timestamp_tooltip = surface.row_timestamp_tooltip_enabled();
    if (!command_line_override_still_holds(
            overrides.row_timestamp_tooltip,
            row_timestamp_tooltip))
    {
        settings.setValue(
            QLatin1String(k_appearance_row_timestamp_tooltip),
            row_timestamp_tooltip);
    }

    const int scrollback_limit = surface.scrollback_limit();
    if (!command_line_override_still_holds(overrides.scrollback_limit, scrollback_limit)) {
        settings.setValue(QLatin1String(k_appearance_scrollback_limit), scrollback_limit);
    }

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

    // scrollback_limit_explicit is the single provenance signal, matching the
    // other six fields. The optional says only whether a limit is known, which
    // this very function then fills in.
    if (!options->scrollback_limit_explicit &&
        state.scrollback_limit.has_value()  &&
        *state.scrollback_limit >= 0)
    {
        options->scrollback_limit = *state.scrollback_limit;
    }
}

Terminal_chrome_palette persisted_terminal_chrome_palette(
    const Persisted_appearance_settings& state)
{
    Terminal_chrome_palette palette = default_terminal_chrome_palette();
    if (state.chrome_focused_background.has_value()) {
        palette.focused_background = *state.chrome_focused_background;
    }

    if (state.chrome_unfocused_background.has_value()) {
        palette.unfocused_background = *state.chrome_unfocused_background;
    }

    if (state.chrome_focused_frame_edge.has_value()) {
        palette.focused_frame_edge = *state.chrome_focused_frame_edge;
    }

    if (state.chrome_unfocused_frame_edge.has_value()) {
        palette.unfocused_frame_edge = *state.chrome_unfocused_frame_edge;
    }

    return palette;
}

Persisted_interaction_settings load_persisted_interaction_settings(
    QSettings& settings)
{
    Persisted_interaction_settings state;
    settings.beginGroup(QLatin1String(k_interaction_settings_group));
    state.copy_on_select = settings_bool_value(settings, k_interaction_copy_on_select);
    settings.endGroup();
    return state;
}

void save_persisted_interaction_settings(
    QSettings& settings,
    bool       copy_on_select)
{
    settings.beginGroup(QLatin1String(k_interaction_settings_group));
    settings.setValue(QLatin1String(k_interaction_copy_on_select), copy_on_select);
    settings.endGroup();
    settings.sync();
}

void apply_persisted_interaction_settings(
    const Persisted_interaction_settings& state,
    App_options*                          options)
{
    if (state.copy_on_select.has_value()) {
        options->copy_on_select = *state.copy_on_select;
    }
}

// A positive opt-out for automated runs. The offscreen platform stays a
// sufficient condition because some suites deliberately run on the real
// platform plugin, and those must not write the user's settings either.
constexpr char k_settings_no_persist_env[] = "VNM_TERMINAL_SETTINGS_NO_PERSIST";

bool terminal_window_persistence_enabled()
{
    return
        !qEnvironmentVariableIsSet(k_settings_no_persist_env) &&
        QGuiApplication::platformName() != QStringLiteral("offscreen");
}

bool window_geometry_has_useful_visible_area(
    const QPoint& position,
    const QSize&  size)
{
    if (size.width() <= 0 || size.height() <= 0) {
        return false;
    }

    // It is the top of the window that has to be reachable. Measuring the whole
    // rectangle lets a window almost entirely above the desktop pass merely
    // because a strip of its bottom edge remains visible, even though no
    // titlebar or drag surface can be reached. Restrict the proof to a logical
    // titlebar-sized strip at the top instead.
    const int grab_height = std::min(size.height(), k_min_restored_visible_height);
    const QRect grab_strip(position, QSize(size.width(), grab_height));

    // Measured per screen rather than across the whole desktop: a strip split
    // across a seam with a sliver on each side is no easier to grab than one
    // with a single sliver, so one screen has to show enough of it on its own.
    const int required_visible_width =
        std::min(size.width(), k_min_restored_visible_width);
    for (const QScreen* screen : QGuiApplication::screens()) {
        const QRect visible = screen->availableGeometry().intersected(grab_strip);
        if (visible.width()  >= required_visible_width &&
            visible.height() >= grab_height)
        {
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
        window_geometry_has_useful_visible_area(
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
