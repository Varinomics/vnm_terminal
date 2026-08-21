#pragma once

#include "vnm_terminal/font_metrics.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include <QString>
#include <QtGlobal>

#include <optional>

class QSettings;

namespace vnm_terminal::terminal_app {

// Neutral embedded-terminal settings. Product settings scopes and command-line
// provenance stay with their owners; this value contains only portable
// appearance state that can be decoded, applied to a surface, and encoded.
struct Terminal_settings_snapshot
{
    QString color_scheme = QStringLiteral("Classic");
    QString font_family  = vnm_terminal::default_monospace_font_family();
    qreal   font_size    = vnm_terminal::k_default_font_pixel_size;
    int     text_renderer_mode =
        static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::AUTO);
    int     lcd_subpixel_order =
        static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::AUTO);
    bool    row_timestamp_tooltip_enabled = true;
    std::optional<int> scrollback_limit;
};

Terminal_settings_snapshot load_terminal_settings_snapshot(QSettings& settings);
Terminal_settings_snapshot terminal_settings_snapshot(
    const VNM_TerminalSurface& surface);
void save_terminal_settings_snapshot(
    QSettings&                       settings,
    const Terminal_settings_snapshot& snapshot);
void apply_terminal_settings_snapshot(
    const Terminal_settings_snapshot& snapshot,
    VNM_TerminalSurface&              surface);

} // namespace vnm_terminal::terminal_app
