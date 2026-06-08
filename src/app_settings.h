#pragma once

#include "app_options.h"

#include "vnm_terminal/vnm_terminal_surface.h"

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

struct Persisted_terminal_window_state
{
    std::optional<QPoint> position;
    std::optional<QSize>  size;
    std::optional<qreal>  font_size;
    bool                  maximized = false;
};

bool persisted_window_axis_is_valid(int value);

std::optional<int>    settings_int_value(QSettings& settings, const char* key);
std::optional<qreal>  settings_font_size(QSettings& settings);
std::optional<QSize>  settings_window_size(QSettings& settings);
std::optional<QPoint> settings_window_position(QSettings& settings);

Persisted_terminal_window_state load_persisted_terminal_window_state(
    QSettings& settings);

void save_persisted_terminal_window_state(
    QSettings&                             settings,
    const Persisted_terminal_window_state& state);

bool terminal_window_persistence_enabled();

bool window_geometry_intersects_available_screen(
    const QPoint& position,
    const QSize&  size);

void apply_persisted_terminal_window_state(
    const Persisted_terminal_window_state& state,
    App_options*                           options);

std::optional<Persisted_terminal_window_state> restorable_terminal_window_state(
    const QWindow&             window,
    const VNM_TerminalSurface& surface);

} // namespace vnm_terminal::terminal_app
