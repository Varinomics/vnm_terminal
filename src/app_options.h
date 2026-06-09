#pragma once

#include "app_clipboard_policy.h"
#include "app_shortcuts.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include "vnm_terminal/internal/vnm_terminal_font.h"

#include <QPoint>
#include <QSize>
#include <QString>
#include <QStringList>

#include <optional>

namespace vnm_terminal::terminal_app {

namespace term = vnm_terminal::internal;

#if defined(_WIN32) || defined(__linux__)
constexpr bool k_custom_titlebar_supported_on_platform = true;
#else
constexpr bool k_custom_titlebar_supported_on_platform = false;
#endif

constexpr bool k_custom_titlebar_default_enabled =
    k_custom_titlebar_supported_on_platform;

bool custom_titlebar_supported_on_platform();

QString     environment_or_default(const char* name, const QString& fallback);
QStringList default_shell_argv();

struct App_options
{
    QStringList        command;
    QString            working_directory;
    QString            backend_output_capture_path;
    QString            transcript_capture_path;
    QString            profile_text_path;
    QString            metrics_json_path;
    QString            font_family = term::vnm_terminal_default_monospace_font_family();
    qreal              font_size   = term::k_vnm_terminal_default_font_pixel_size;
    QString            theme       = QStringLiteral("default");
    QSize              window_size = QSize(900, 600);
    std::optional<QPoint> window_position;
    VNM_TerminalSurface::Alternate_screen_wheel_policy alternate_screen_wheel_policy =
        VNM_TerminalSurface::Alternate_screen_wheel_policy::MOUSE_REPORTING_FIRST;
    VNM_TerminalSurface::Synchronized_output_scroll_policy synchronized_output_scroll_policy =
        VNM_TerminalSurface::Synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION;
    VNM_TerminalSurface::Text_renderer_mode text_renderer_mode =
        VNM_TerminalSurface::Text_renderer_mode::AUTO;
    VNM_TerminalSurface::Lcd_subpixel_order lcd_subpixel_order =
        VNM_TerminalSurface::Lcd_subpixel_order::AUTO;
    Osc52_clipboard_policy osc52_clipboard_policy = Osc52_clipboard_policy::DENY;
    Paste_shortcut_policy paste_shortcut_policy = Paste_shortcut_policy::PLATFORM_DEFAULT;
    std::optional<int> timeout_ms;
    std::optional<int> scrollback_limit;
    bool               shell_requested                    = false;
    bool               keep_open_after_process_exits      = false;
    bool               require_output                     = false;
    bool               custom_titlebar                    = k_custom_titlebar_default_enabled;
    bool               selection_trace_enabled            = false;
    bool               transcript_snapshot_diagnostics    = false;
    bool               transcript_timing_diagnostics      = false;
    bool               wheel_trace_enabled                 = false;
    std::optional<bool> primary_repaint_recovery_enabled;
    bool               font_size_explicit                 = false;
    bool               window_size_explicit               = false;
    bool               restore_maximized_window_state     = false;
};

} // namespace vnm_terminal::terminal_app
