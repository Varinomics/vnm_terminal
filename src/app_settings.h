#pragma once

#include "app_options.h"
#include "qml_chrome.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QColor>
#include <QPoint>
#include <QSize>
#include <QString>

#include <optional>

class QSettings;
class QWindow;

namespace vnm_terminal::terminal_app {

constexpr int k_persisted_window_min_axis = 1;

constexpr char k_window_settings_group[]     = "window";
constexpr char k_window_settings_font_size[] = "font_size";
constexpr char k_window_settings_height[]    = "height";
constexpr char k_window_settings_maximized[] = "maximized";
constexpr char k_window_settings_width[]     = "width";
constexpr char k_window_settings_x[]         = "x";
constexpr char k_window_settings_y[]         = "y";

constexpr char k_appearance_settings_group[]        = "appearance";
constexpr char k_appearance_color_scheme[]          = "color_scheme";
constexpr char k_appearance_font_family[]           = "font_family";
constexpr char k_appearance_text_renderer_mode[]    = "text_renderer_mode";
constexpr char k_appearance_lcd_subpixel_order[]    = "lcd_subpixel_order";
constexpr char k_appearance_row_timestamp_tooltip[] = "row_timestamp_tooltip";
constexpr char k_appearance_scrollback_limit[]      = "scrollback_limit";

// Window-chrome colors, as color names or "#rrggbb" strings. Each key that is
// absent or unreadable keeps its default from Terminal_chrome_palette. The app
// never writes these back, so a stored value is the user's alone.
constexpr char k_appearance_chrome_focused_background[]   = "chrome_focused_background";
constexpr char k_appearance_chrome_unfocused_background[] = "chrome_unfocused_background";
constexpr char k_appearance_chrome_focused_frame_edge[]   = "chrome_focused_frame_edge";
constexpr char k_appearance_chrome_unfocused_frame_edge[] = "chrome_unfocused_frame_edge";

constexpr char k_interaction_settings_group[] = "interaction";
constexpr char k_interaction_copy_on_select[] = "copy_on_select";

struct Persisted_terminal_window_state
{
    std::optional<QPoint> position;
    std::optional<QSize>  size;
    std::optional<qreal>  font_size;
    bool                  maximized = false;
};

struct Persisted_appearance_settings
{
    std::optional<QString> color_scheme;
    std::optional<QString> font_family;
    std::optional<int>     text_renderer_mode;
    std::optional<int>     lcd_subpixel_order;
    std::optional<bool>    row_timestamp_tooltip;
    std::optional<int>     scrollback_limit;
    std::optional<QColor>  chrome_focused_background;
    std::optional<QColor>  chrome_unfocused_background;
    std::optional<QColor>  chrome_focused_frame_edge;
    std::optional<QColor>  chrome_unfocused_frame_edge;
};

struct Persisted_interaction_settings
{
    std::optional<bool> copy_on_select;
};

// The settings this run was forced to from the command line, as the surface
// normalized them. A forced value is deliberately transient: writing it back
// would replace a preference the user chose in the settings panel and never
// asked to change. Each engaged field therefore suppresses its own write while
// the live setting still holds it.
//
// The record is mutable and outlives a single save: the first save that finds
// a field somewhere other than its forced value clears that field for good,
// which is what lets a user who moves a forced setting and moves it back store
// that choice. Pass the same instance to every save of a run.
//
// The geometry fields are latched later than the rest. Every other forced
// setting has a normalized value to read back from the surface at startup, but
// a forced window size is given to the window system, which answers with the
// geometry it is willing to grant - normalized, clamped, decorated, or moved to
// fit. Latching the requested size would treat that answer as a user decision
// and release the latch on the run's very first save; latching the granted
// geometry instead keeps geometry on the same rule as everything else. Until
// the first save observes it, `window_geometry_latch_pending` stands in for the
// three geometry fields.
struct Command_line_setting_overrides
{
    std::optional<qreal>   font_size;
    std::optional<QString> color_scheme;
    std::optional<QString> font_family;
    std::optional<int>     text_renderer_mode;
    std::optional<int>     lcd_subpixel_order;
    std::optional<bool>    row_timestamp_tooltip;
    std::optional<int>     scrollback_limit;
    std::optional<QSize>   window_size;
    std::optional<QPoint>  window_position;
    std::optional<bool>    maximized;
    bool                   window_geometry_latch_pending = false;
};

// Snapshots the forced settings from the surface, so call this once the startup
// options have been applied to it. Taking the values from the surface rather
// than from App_options is what makes the later comparison exact: the surface
// canonicalizes a color scheme name and rounds a font size, so the raw
// command-line text does not compare equal to the setting it produced.
Command_line_setting_overrides command_line_setting_overrides(
    const App_options&         options,
    const VNM_TerminalSurface& surface);

bool persisted_window_axis_is_valid(int value);

std::optional<int>    settings_int_value(QSettings& settings, const char* key);
std::optional<bool>   settings_bool_value(QSettings& settings, const char* key);
std::optional<QColor> settings_color_value(QSettings& settings, const char* key);
std::optional<qreal>  settings_font_size(QSettings& settings);
std::optional<QSize>  settings_window_size(QSettings& settings);
std::optional<QPoint> settings_window_position(QSettings& settings);

Persisted_terminal_window_state load_persisted_terminal_window_state(
    QSettings& settings);

void save_persisted_terminal_window_state(
    QSettings&                             settings,
    const Persisted_terminal_window_state& state,
    Command_line_setting_overrides&        overrides);

Persisted_appearance_settings load_persisted_appearance_settings(QSettings& settings);

void save_persisted_appearance_settings(
    QSettings&                      settings,
    const VNM_TerminalSurface&      surface,
    Command_line_setting_overrides& overrides);

void apply_persisted_appearance_settings(
    const Persisted_appearance_settings& state,
    App_options*                         options);

// The default chrome palette with whatever the appearance settings override.
Terminal_chrome_palette persisted_terminal_chrome_palette(
    const Persisted_appearance_settings& state);

Persisted_interaction_settings load_persisted_interaction_settings(
    QSettings& settings);

void save_persisted_interaction_settings(
    QSettings& settings,
    bool       copy_on_select);

void apply_persisted_interaction_settings(
    const Persisted_interaction_settings& state,
    App_options*                          options);

bool terminal_window_persistence_enabled();

// How much of a restored window has to land on an available screen for the
// stored position to be worth honoring. A single intersecting pixel satisfies
// the geometry but not the user: after a monitor is removed or the desktop is
// rearranged, a window with a sliver on screen is not reachable by pointer.
// These are enough to grab and drag - roughly a titlebar corner - and they are
// fixed logical pixels rather than a titlebar measurement so the rule stays
// deterministic across platforms and themes. A window smaller than the
// threshold is held to its own size instead.
constexpr int k_min_restored_visible_width  = 64;
constexpr int k_min_restored_visible_height = 32;

bool window_geometry_has_useful_visible_area(
    const QPoint& position,
    const QSize&  size);

void apply_persisted_terminal_window_state(
    const Persisted_terminal_window_state& state,
    App_options*                           options);

std::optional<Persisted_terminal_window_state> restorable_terminal_window_state(
    const QWindow&             window,
    const VNM_TerminalSurface& surface);

} // namespace vnm_terminal::terminal_app
