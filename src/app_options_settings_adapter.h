#pragma once

#include "app_settings_keys.h"

#include "vnm_terminal/app_support/app_settings.h"
#include "vnm_terminal/app_support/qml_chrome.h"

#include <QColor>
#include <QPoint>
#include <QSize>

#include <optional>

class QSettings;
class QWindow;

namespace vnm_terminal::terminal_app {

struct App_options;

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

// Standalone-only command-line provenance. Each forced setting suppresses its
// own persistence while its normalized value still holds. Window geometry is
// suppressed as one unit until the platform-granted geometry is observed.
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
    bool                   window_geometry_settlement_pending = false;
};

Command_line_setting_overrides command_line_setting_overrides(
    const App_options&         options,
    const VNM_TerminalSurface& surface);

void apply_persisted_appearance_settings(
    const Persisted_appearance_settings& state,
    App_options*                         options);

void apply_persisted_interaction_settings(
    const Persisted_interaction_settings& state,
    App_options*                          options);

void apply_persisted_terminal_window_state(
    const Persisted_terminal_window_state& state,
    App_options*                           options);

bool persisted_window_axis_is_valid(int value);

std::optional<int>    settings_int_value(QSettings& settings, const char* key);
std::optional<bool>   settings_bool_value(QSettings& settings, const char* key);
std::optional<QColor> settings_color_value(QSettings& settings, const char* key);
std::optional<qreal>  settings_font_size(QSettings& settings);
std::optional<QSize>  settings_window_size(QSettings& settings);
std::optional<QPoint> settings_window_position(QSettings& settings);

Persisted_terminal_window_state load_persisted_terminal_window_state(
    QSettings& settings);

bool settle_command_line_window_geometry(
    const Persisted_terminal_window_state& state,
    Command_line_setting_overrides&        overrides);

void save_persisted_terminal_window_state(
    QSettings&                             settings,
    const Persisted_terminal_window_state& state,
    Command_line_setting_overrides&        overrides);

Persisted_appearance_settings load_persisted_appearance_settings(
    QSettings& settings);

void save_persisted_appearance_settings(
    QSettings&                      settings,
    const VNM_TerminalSurface&      surface,
    Command_line_setting_overrides& overrides);

Terminal_chrome_palette persisted_terminal_chrome_palette(
    const Persisted_appearance_settings& state);

Persisted_interaction_settings load_persisted_interaction_settings(
    QSettings& settings);

void save_persisted_interaction_settings(
    QSettings& settings,
    bool       copy_on_select);

bool terminal_window_persistence_enabled();

constexpr int k_min_restored_visible_width  = 64;
constexpr int k_min_restored_visible_height = 32;

bool window_geometry_has_useful_visible_area(
    const QPoint& position,
    const QSize&  size);

std::optional<Persisted_terminal_window_state> restorable_terminal_window_state(
    const QWindow&             window,
    const VNM_TerminalSurface& surface);

} // namespace vnm_terminal::terminal_app
