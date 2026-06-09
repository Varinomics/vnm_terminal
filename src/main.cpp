#include "app_clipboard_policy.h"
#include "app_common.h"
#include "app_metrics.h"
#include "app_options.h"
#include "app_profile_text.h"
#include "app_settings.h"
#include "app_shortcuts.h"
#include "qml_chrome.h"
#include "terminal_scrollbar.h"
#include "terminal_window.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QIODevice>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QRect>
#include <QSettings>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWindow>
#include <QtGlobal>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#ifndef VNM_TERMINAL_VERSION_STRING
#define VNM_TERMINAL_VERSION_STRING "0.0.0"
#endif

#ifndef VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
#define VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED 0
#endif

namespace {

namespace term   = vnm_terminal::internal;
namespace chrome = vnm_terminal::terminal_app;

using chrome::App_options;
using chrome::apply_persisted_terminal_window_state;
using chrome::apply_primary_repaint_recovery_option;
using chrome::apply_scrollback_limit_option;
using chrome::apply_synchronized_output_scroll_policy_option;
using chrome::apply_terminal_shell_geometry;
using chrome::connect_terminal_metadata_to_chrome;
using chrome::custom_titlebar_resize_border_active;
using chrome::custom_titlebar_supported_on_platform;
using chrome::default_shell_argv;
using chrome::default_window_title;
using chrome::enum_key;
using chrome::environment_or_default;
using chrome::handle_clipboard_write_request;
using chrome::install_wheel_delivery_indicator_filter;
using chrome::k_custom_titlebar_default_enabled;
using chrome::k_custom_titlebar_height;
using chrome::k_custom_titlebar_physical_reduction;
using chrome::k_custom_titlebar_supported_on_platform;
using chrome::k_exit_no_output;
using chrome::k_exit_process_failed;
using chrome::k_exit_start_failed;
using chrome::k_exit_timeout;
using chrome::k_exit_usage_error;
using chrome::k_osc52_clipboard_max_payload_bytes;
using chrome::k_persisted_window_min_axis;
using chrome::k_terminal_scrollbar_width;
using chrome::k_text_area_resize_max_columns;
using chrome::k_text_area_resize_max_rows;
using chrome::k_text_area_resize_max_window_axis;
using chrome::k_timeout_force_exit_grace_ms;
using chrome::k_window_settings_font_size;
using chrome::k_window_settings_group;
using chrome::k_window_settings_height;
using chrome::k_window_settings_maximized;
using chrome::k_window_settings_width;
using chrome::k_window_settings_x;
using chrome::k_window_settings_y;
using chrome::load_persisted_terminal_window_state;
using chrome::logical_device_pixel_width;
using chrome::metrics_timing_t;
using chrome::Osc52_clipboard_policy;
using chrome::Persisted_terminal_window_state;
using chrome::persisted_window_axis_is_valid;
#if VNM_TERMINAL_PROFILING_ENABLED
using chrome::prepare_profile_text_file;
#endif
using chrome::print_error;
using chrome::reduced_chrome_span;
using chrome::reduced_custom_titlebar_height;
using chrome::reduced_frameless_resize_border_width;
using chrome::resize_window_for_text_area_request;
using chrome::restorable_terminal_window_state;
using chrome::Runtime_state;
using chrome::save_persisted_terminal_window_state;
using chrome::settings_font_size;
using chrome::settings_int_value;
using chrome::settings_window_position;
using chrome::settings_window_size;
using chrome::snap_terminal_shell_geometry;
using chrome::split_terminal_area;
using chrome::sync_chrome_window_state;
using chrome::sync_terminal_title;
using chrome::Terminal_shell_geometry;
using chrome::terminal_shell_geometry;
using chrome::Terminal_shortcut_filter;
using chrome::terminal_window_persistence_enabled;
using chrome::visible_terminal_title;
using chrome::Wheel_delivery_indicator_filter;
using chrome::window_geometry_intersects_available_screen;
using chrome::write_metrics_json;
#if VNM_TERMINAL_PROFILING_ENABLED
using chrome::write_profile_text;
#endif

struct Parse_result
{
    App_options        options;
    QString            error;
    bool               help_requested                = false;
};

void print_usage()
{
    std::cout
        << "usage: vnm_terminal [options]\n"
        << "       vnm_terminal [options] -- <program> [args...]\n"
        << "\n"
        << "options:\n"
        << "  --shell                         launch the default shell; also the default without --\n"
        << "  --cwd <path>                    launch in a working directory\n"
        << "  --font-family <family>          terminal font family\n"
        << "  --font-size <pixels>            terminal font size in pixels\n"
        << "  --theme <name>                  terminal color theme\n"
        << "  --scrollback-limit <rows>       maximum retained scrollback rows\n"
        << "  --window-size <width>x<height>  window size in logical pixels\n"
#if defined(_WIN32) || defined(__linux__)
        << "  --native-titlebar               use the platform titlebar instead of built-in chrome\n"
#endif
        << "  --alternate-wheel <mode>        alternate-screen wheel: mouse(default), cursor, or page\n"
        << "  --text-renderer <mode>          text renderer: auto(default), msdf, or glyph\n"
        << "  --lcd-subpixel <order>          MSDF LCD order: auto(default), none, rgb, bgr, vrgb, or vbgr\n"
        << "  --synchronized-output-scroll-policy=<policy>\n"
        << "                                  DEC synchronized-output scroll: defer(default) "
        << "or immediate-public (case-insensitive)\n"
        << "  --disable-primary-repaint-recovery\n"
        << "                                  disable primary repaint scrollback recovery "
        << "when enabled\n"
        << "  --capture-output <path>         write raw backend output bytes to a file\n"
        << "  --metrics-json <path>           write lightweight terminal runtime metrics\n"
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
        << "  --capture-transcript <path>     write sensitive NDJSON replay transcript\n"
        << "  --transcript-snapshot-diagnostics include visible row text/provenance snapshots\n"
        << "  --transcript-timing-diagnostics include thresholded transcript hot-path timings\n"
        << "  --wheel-trace                   include diagnostic wheel routing events in transcript\n"
#endif
        << "  --selection-trace               write selection diagnostics to stderr\n"
#if VNM_TERMINAL_PROFILING_ENABLED
        << "  --profile-text <path>           write profile and dirty-row diagnostics\n"
#endif
        << "  --keep-open-after-process-exits leave the window open after the child exits\n"
        << "  --timeout-ms <n>                fail if the run is still active after n ms\n"
        << "  --require-output                fail if no terminal output activity is observed\n"
        << "  --osc52-clipboard <policy>      OSC 52 clipboard write policy: deny(default) or allow\n"
        << "  --help                          show this help\n"
        << "\n"
        << "interactions:\n"
        << "  mouse-reporting apps receive unmodified mouse drags; Shift-drag selects locally\n"
#if defined(Q_OS_MACOS)
        << "  Command+C copies selected text; Command+V pastes clipboard text; "
        << "Ctrl+C sends terminal input\n"
#else
        << "  Ctrl+C copies selected text, otherwise sends Ctrl+C; "
        << "Ctrl+V/Ctrl+Shift+V paste clipboard text\n"
#endif
        << "  OSC 52 clipboard writes are denied by default; --osc52-clipboard allow permits "
        << "writes to target c/clipboard\n";
}

bool argument_is(const QString& argument, const char* expected)
{
    return argument == QLatin1String(expected);
}

bool take_option_value(
    const QStringList& arguments,
    int&               index,
    QString*           out_value,
    QString*           out_error)
{
    if (index + 1 >= arguments.size()) {
        *out_error = QStringLiteral("%1 requires a value").arg(arguments[index]);
        return false;
    }

    *out_value = arguments[index + 1];
    index += 2;
    return true;
}

std::optional<QSize> parse_window_size(const QString& value)
{
    int separator = value.indexOf(QLatin1Char('x'));
    if (separator < 0) {
        separator = value.indexOf(QLatin1Char('X'));
    }

    if (separator <= 0 || separator + 1 >= value.size()) {
        return std::nullopt;
    }

    bool      width_ok  = false;
    bool      height_ok = false;
    const int width     = value.left(separator).toInt(&width_ok);
    const int height    = value.mid(separator + 1).toInt(&height_ok);
    if (!width_ok || !height_ok || width <= 0 || height <= 0) {
        return std::nullopt;
    }

    return QSize(width, height);
}

bool parse_font_size(
    const QString&         value,
    qreal*                 out_font_size,
    QString*               out_error)
{
    bool         ok        = false;
    const double font_size = value.toDouble(&ok);
    if (!ok || !std::isfinite(font_size) || font_size <= 0.0) {
        *out_error = QStringLiteral("--font-size requires a positive pixel size");
        return false;
    }

    *out_font_size = static_cast<qreal>(font_size);
    return true;
}

bool parse_timeout_ms(
    const QString&         value,
    std::optional<int>*    out_timeout_ms,
    QString*               out_error)
{
    bool ok = false;
    const qlonglong timeout_ms = value.toLongLong(&ok);
    if (!ok ||
        timeout_ms <= 0 ||
        timeout_ms >  static_cast<qlonglong>(std::numeric_limits<int>::max()))
    {
        *out_error = QStringLiteral("--timeout-ms requires a positive integer");
        return false;
    }

    *out_timeout_ms = static_cast<int>(timeout_ms);
    return true;
}

bool parse_scrollback_limit(
    const QString&         value,
    std::optional<int>*    out_scrollback_limit,
    QString*               out_error)
{
    bool ok = false;
    const qlonglong limit = value.toLongLong(&ok);
    if (!ok || limit < 0 || limit > static_cast<qlonglong>(std::numeric_limits<int>::max())) {
        *out_error = QStringLiteral("--scrollback-limit requires a non-negative integer");
        return false;
    }

    *out_scrollback_limit = static_cast<int>(limit);
    return true;
}

bool parse_alternate_wheel_policy(
    const QString&         value,
    VNM_TerminalSurface::Alternate_screen_wheel_policy*
                           out_policy,
    QString*               out_error)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("page")) {
        *out_policy = VNM_TerminalSurface::Alternate_screen_wheel_policy::PAGE_KEYS;
        return true;
    }
    if (normalized == QStringLiteral("cursor")) {
        *out_policy = VNM_TerminalSurface::Alternate_screen_wheel_policy::CURSOR_KEYS;
        return true;
    }
    if (normalized == QStringLiteral("mouse")) {
        *out_policy =
            VNM_TerminalSurface::Alternate_screen_wheel_policy::MOUSE_REPORTING_FIRST;
        return true;
    }

    *out_error = QStringLiteral("--alternate-wheel supports only page, cursor, or mouse");
    return false;
}

bool parse_synchronized_output_scroll_policy(
    const QString&         value,
    VNM_TerminalSurface::Synchronized_output_scroll_policy*
                           out_policy,
    QString*               out_error)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("defer")) {
        *out_policy = VNM_TerminalSurface::Synchronized_output_scroll_policy::
            DEFER_UNTIL_CONTENT_PUBLICATION;
        return true;
    }
    if (normalized == QStringLiteral("immediate-public")) {
        *out_policy = VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION;
        return true;
    }

    *out_error = QStringLiteral(
        "--synchronized-output-scroll-policy supports only defer or immediate-public");
    return false;
}

bool parse_text_renderer_mode(
    const QString&                         value,
    VNM_TerminalSurface::Text_renderer_mode*
                                           out_mode,
    QString*                               out_error)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("auto")) {
        *out_mode = VNM_TerminalSurface::Text_renderer_mode::AUTO;
        return true;
    }
    if (normalized == QStringLiteral("msdf")) {
        *out_mode = VNM_TerminalSurface::Text_renderer_mode::MSDF;
        return true;
    }
    if (normalized == QStringLiteral("glyph")) {
        *out_mode = VNM_TerminalSurface::Text_renderer_mode::GLYPH;
        return true;
    }

    *out_error = QStringLiteral("--text-renderer supports only auto, msdf, or glyph");
    return false;
}

bool parse_lcd_subpixel_order(
    const QString&                         value,
    VNM_TerminalSurface::Lcd_subpixel_order*
                                           out_order,
    QString*                               out_error)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("auto")) {
        *out_order = VNM_TerminalSurface::Lcd_subpixel_order::AUTO;
        return true;
    }
    if (normalized == QStringLiteral("none")) {
        *out_order = VNM_TerminalSurface::Lcd_subpixel_order::NONE;
        return true;
    }
    if (normalized == QStringLiteral("rgb")) {
        *out_order = VNM_TerminalSurface::Lcd_subpixel_order::RGB;
        return true;
    }
    if (normalized == QStringLiteral("bgr")) {
        *out_order = VNM_TerminalSurface::Lcd_subpixel_order::BGR;
        return true;
    }
    if (normalized == QStringLiteral("vrgb")) {
        *out_order = VNM_TerminalSurface::Lcd_subpixel_order::VRGB;
        return true;
    }
    if (normalized == QStringLiteral("vbgr")) {
        *out_order = VNM_TerminalSurface::Lcd_subpixel_order::VBGR;
        return true;
    }

    *out_error = QStringLiteral(
        "--lcd-subpixel supports only auto, none, rgb, bgr, vrgb, or vbgr");
    return false;
}

bool parse_osc52_clipboard_policy(
    const QString&          value,
    Osc52_clipboard_policy* out_policy,
    QString*                out_error)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("deny")) {
        *out_policy = Osc52_clipboard_policy::DENY;
        return true;
    }
    if (normalized == QStringLiteral("allow")) {
        *out_policy = Osc52_clipboard_policy::ALLOW;
        return true;
    }

    *out_error = QStringLiteral("--osc52-clipboard supports only deny or allow");
    return false;
}

bool take_synchronized_output_scroll_policy_value(
    const QString&  argument,
    QString*        out_value,
    QString*        out_error)
{
    if (argument_is(argument, "--synchronized-output-scroll-policy")) {
        *out_error = QStringLiteral(
            "--synchronized-output-scroll-policy requires =defer or =immediate-public");
        return false;
    }

    const QString prefix =
        QStringLiteral("--synchronized-output-scroll-policy=");
    if (!argument.startsWith(prefix)) {
        return false;
    }

    *out_value = argument.mid(prefix.size());
    return true;
}

QString comparable_capture_path(QString path)
{
    path = QDir::cleanPath(std::move(path));
#if defined(Q_OS_WIN)
    path = path.toCaseFolded();
#endif
    return path;
}

bool validate_capture_path(
    const QString& option_name,
    const QString& path,
    QString*       out_absolute_path,
    QString*       out_error)
{
    if (path.trimmed().isEmpty()) {
        *out_error = QStringLiteral("%1 requires a non-empty path").arg(option_name);
        return false;
    }

    const QFileInfo file_info(path);
    const QDir parent_dir = file_info.absoluteDir();
    if (!parent_dir.exists()) {
        *out_error = QStringLiteral("%1 parent directory does not exist: %2")
            .arg(option_name, parent_dir.absolutePath());
        return false;
    }
    if (file_info.exists() && file_info.isDir()) {
        *out_error = QStringLiteral("%1 points to a directory: %2")
            .arg(option_name, file_info.absoluteFilePath());
        return false;
    }

    *out_absolute_path = file_info.absoluteFilePath();
    return true;
}

bool validate_capture_paths(App_options* options, QString* out_error)
{
    if (!options->backend_output_capture_path.isEmpty() &&
        !validate_capture_path(
            QStringLiteral("--capture-output"),
            options->backend_output_capture_path,
            &options->backend_output_capture_path,
            out_error))
    {
        return false;
    }

    if (!options->transcript_capture_path.isEmpty() &&
        !validate_capture_path(
            QStringLiteral("--capture-transcript"),
            options->transcript_capture_path,
            &options->transcript_capture_path,
            out_error))
    {
        return false;
    }

    if (!options->metrics_json_path.isEmpty() &&
        !validate_capture_path(
            QStringLiteral("--metrics-json"),
            options->metrics_json_path,
            &options->metrics_json_path,
            out_error))
    {
        return false;
    }

#if VNM_TERMINAL_PROFILING_ENABLED
    if (!options->profile_text_path.isEmpty() &&
        !validate_capture_path(
            QStringLiteral("--profile-text"),
            options->profile_text_path,
            &options->profile_text_path,
            out_error))
    {
        return false;
    }
#endif

    if (!options->backend_output_capture_path.isEmpty() &&
        !options->transcript_capture_path.isEmpty() &&
        comparable_capture_path(options->backend_output_capture_path) ==
            comparable_capture_path(options->transcript_capture_path))
    {
        *out_error = QStringLiteral(
            "--capture-output and --capture-transcript must use different paths: %1")
            .arg(options->backend_output_capture_path);
        return false;
    }

    if (!options->backend_output_capture_path.isEmpty() &&
        !options->metrics_json_path.isEmpty() &&
        comparable_capture_path(options->backend_output_capture_path) ==
            comparable_capture_path(options->metrics_json_path))
    {
        *out_error = QStringLiteral(
            "--capture-output and --metrics-json must use different paths: %1")
            .arg(options->backend_output_capture_path);
        return false;
    }

    if (!options->transcript_capture_path.isEmpty() &&
        !options->metrics_json_path.isEmpty() &&
        comparable_capture_path(options->transcript_capture_path) ==
            comparable_capture_path(options->metrics_json_path))
    {
        *out_error = QStringLiteral(
            "--capture-transcript and --metrics-json must use different paths: %1")
            .arg(options->transcript_capture_path);
        return false;
    }

#if VNM_TERMINAL_PROFILING_ENABLED
    if (!options->profile_text_path.isEmpty() &&
        !options->metrics_json_path.isEmpty() &&
        comparable_capture_path(options->profile_text_path) ==
            comparable_capture_path(options->metrics_json_path))
    {
        *out_error = QStringLiteral(
            "--profile-text and --metrics-json must use different paths: %1")
            .arg(options->profile_text_path);
        return false;
    }
#endif

    return true;
}

bool prepare_capture_file(
    const QString& option_name,
    const QString& path,
    QString*       out_error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *out_error = QStringLiteral("%1 could not open %2: %3")
            .arg(option_name, path, file.errorString());
        return false;
    }

    return true;
}

Parse_result parse_arguments(const QStringList& arguments)
{
    Parse_result result;
    bool explicit_command_separator = false;

    int index = 1;
    while (index < arguments.size()) {
        const QString argument = arguments[index];

        if (argument_is(argument, "--")) {
            explicit_command_separator = true;
            result.options.command = arguments.mid(index + 1);
            break;
        }

        if (argument_is(argument, "--help") || argument_is(argument, "-h")) {
            result.help_requested = true;
            return result;
        }

        if (argument_is(argument, "--shell")) {
            result.options.shell_requested = true;
            ++index;
            continue;
        }

        if (argument_is(argument, "--exit-when-process-exits")) {
            result.options.keep_open_after_process_exits = false;
            ++index;
            continue;
        }

        if (argument_is(argument, "--keep-open-after-process-exits")) {
            result.options.keep_open_after_process_exits = true;
            ++index;
            continue;
        }

        if (argument_is(argument, "--require-output")) {
            result.options.require_output = true;
            ++index;
            continue;
        }

        if (argument_is(argument, "--disable-primary-repaint-recovery")) {
            result.options.primary_repaint_recovery_enabled = false;
            ++index;
            continue;
        }

        if (argument_is(argument, "--selection-trace")) {
            result.options.selection_trace_enabled = true;
            ++index;
            continue;
        }

        if (argument_is(argument, "--native-titlebar")) {
            if (!custom_titlebar_supported_on_platform()) {
                result.error = QStringLiteral(
                    "--native-titlebar is supported only after platform validation");
                return result;
            }

            result.options.custom_titlebar = false;
            ++index;
            continue;
        }

        QString value;
        if (argument_is(argument, "--cwd")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }

            result.options.working_directory = value;
            continue;
        }

        if (argument_is(argument, "--font-family")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }

            result.options.font_family = value;
            continue;
        }

        if (argument_is(argument, "--font-size")) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_font_size(value, &result.options.font_size, &result.error))
            {
                return result;
            }

            result.options.font_size_explicit = true;
            continue;
        }

        if (argument_is(argument, "--theme")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }

            result.options.theme = value;
            continue;
        }

        if (argument_is(argument, "--window-size")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }

            const std::optional<QSize> window_size = parse_window_size(value);
            if (!window_size.has_value()) {
                result.error = QStringLiteral(
                    "--window-size requires <positive-width>x<positive-height>");
                return result;
            }

            result.options.window_size = *window_size;
            result.options.window_size_explicit = true;
            continue;
        }

        if (argument_is(argument, "--scrollback-limit")) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_scrollback_limit(value, &result.options.scrollback_limit, &result.error))
            {
                return result;
            }

            continue;
        }

        if (argument_is(argument, "--alternate-wheel")) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_alternate_wheel_policy(
                    value, &result.options.alternate_screen_wheel_policy, &result.error))
            {
                return result;
            }

            continue;
        }

        if (argument_is(argument, "--text-renderer")) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_text_renderer_mode(
                    value, &result.options.text_renderer_mode, &result.error))
            {
                return result;
            }

            continue;
        }

        if (argument_is(argument, "--lcd-subpixel")) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_lcd_subpixel_order(
                    value, &result.options.lcd_subpixel_order, &result.error))
            {
                return result;
            }

            continue;
        }

        if (argument_is(argument, "--osc52-clipboard")) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_osc52_clipboard_policy(
                    value, &result.options.osc52_clipboard_policy, &result.error))
            {
                return result;
            }

            continue;
        }

        if (take_synchronized_output_scroll_policy_value(
                argument, &value, &result.error))
        {
            if (!parse_synchronized_output_scroll_policy(
                    value, &result.options.synchronized_output_scroll_policy, &result.error))
            {
                return result;
            }

            ++index;
            continue;
        }
        if (!result.error.isEmpty()) {
            return result;
        }

        if (argument_is(argument, "--capture-output")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }
            if (value.trimmed().isEmpty()) {
                result.error = QStringLiteral("--capture-output requires a non-empty path");
                return result;
            }

            result.options.backend_output_capture_path = value;
            continue;
        }

        if (argument_is(argument, "--capture-transcript")) {
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }
            if (value.trimmed().isEmpty()) {
                result.error = QStringLiteral("--capture-transcript requires a non-empty path");
                return result;
            }

            result.options.transcript_capture_path = value;
            continue;
#else
            result.error = QStringLiteral(
                "--capture-transcript is unavailable because transcript capture/replay is disabled in this build");
            return result;
#endif
        }

        if (argument_is(argument, "--transcript-snapshot-diagnostics")) {
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
            result.options.transcript_snapshot_diagnostics = true;
            ++index;
            continue;
#else
            result.error = QStringLiteral(
                "--transcript-snapshot-diagnostics is unavailable because transcript capture/replay is disabled in this build");
            return result;
#endif
        }

        if (argument_is(argument, "--transcript-timing-diagnostics")) {
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
            result.options.transcript_timing_diagnostics = true;
            ++index;
            continue;
#else
            result.error = QStringLiteral(
                "--transcript-timing-diagnostics is unavailable because transcript capture/replay is disabled in this build");
            return result;
#endif
        }

        if (argument_is(argument, "--wheel-trace")) {
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
            result.options.wheel_trace_enabled = true;
            ++index;
            continue;
#else
            result.error = QStringLiteral(
                "--wheel-trace is unavailable because transcript capture/replay is disabled in this build");
            return result;
#endif
        }

        if (argument_is(argument, "--profile-text")) {
#if VNM_TERMINAL_PROFILING_ENABLED
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }
            if (value.trimmed().isEmpty()) {
                result.error = QStringLiteral("--profile-text requires a non-empty path");
                return result;
            }

            result.options.profile_text_path = value;
            continue;
#else
            result.error = QStringLiteral(
                "--profile-text requires VNM_TERMINAL_ENABLE_PROFILING=ON");
            return result;
#endif
        }

        if (argument_is(argument, "--metrics-json")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }
            if (value.trimmed().isEmpty()) {
                result.error = QStringLiteral("--metrics-json requires a non-empty path");
                return result;
            }

            result.options.metrics_json_path = value;
            continue;
        }

        if (argument_is(argument, "--timeout-ms")) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_timeout_ms(value, &result.options.timeout_ms, &result.error))
            {
                return result;
            }

            continue;
        }

        result.error = QStringLiteral("unexpected argument '%1'; use -- before a command")
            .arg(argument);
        return result;
    }

#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (result.options.wheel_trace_enabled &&
        result.options.transcript_capture_path.isEmpty())
    {
        result.options.wheel_trace_enabled = false;
        result.error = QStringLiteral(
            "--wheel-trace requires --capture-transcript <path>");
        return result;
    }
    if (result.options.transcript_timing_diagnostics &&
        result.options.transcript_capture_path.isEmpty())
    {
        result.options.transcript_timing_diagnostics = false;
        result.error = QStringLiteral(
            "--transcript-timing-diagnostics requires --capture-transcript <path>");
        return result;
    }
    if (result.options.transcript_snapshot_diagnostics &&
        result.options.transcript_capture_path.isEmpty())
    {
        result.options.transcript_snapshot_diagnostics = false;
        result.error = QStringLiteral(
            "--transcript-snapshot-diagnostics requires --capture-transcript <path>");
        return result;
    }
#endif

    if (explicit_command_separator) {
        if (result.options.command.isEmpty()) {
            result.error = QStringLiteral("explicit command after -- must name a program");
            return result;
        }

        if (result.options.shell_requested) {
            result.error = QStringLiteral("--shell cannot be combined with an explicit command");
            return result;
        }
    }
    else {
        result.options.command = default_shell_argv();
        if (result.options.command.isEmpty()) {
            result.error = QStringLiteral("no default shell is available on this platform");
            return result;
        }
    }

    if (result.options.font_family.trimmed().isEmpty()) {
        result.error = QStringLiteral("--font-family requires a non-empty family name");
        return result;
    }

    const QString theme = result.options.theme.trimmed();
    if (theme.isEmpty()) {
        result.error = QStringLiteral("--theme requires a non-empty theme name");
        return result;
    }

    if (theme.compare(QStringLiteral("default"), Qt::CaseInsensitive) != 0 &&
        theme.compare(QStringLiteral("light"), Qt::CaseInsensitive)   != 0)
    {
        result.error = QStringLiteral("--theme supports only 'default' or 'light'");
        return result;
    }
    result.options.theme = theme;

    return result;
}

QStringList raw_arguments(int argc, char** argv)
{
    QStringList arguments;
    for (int index = 0; index < argc; ++index) {
        arguments.push_back(QString::fromLocal8Bit(argv[index]));
    }
    return arguments;
}

struct Qt_arguments
{
    std::vector<QByteArray>    storage;
    std::vector<char*>         argv;
    int                        argc = 0;
};

Qt_arguments make_qt_arguments(int argc, char** argv)
{
    Qt_arguments arguments;
    for (int index = 0; index < argc; ++index) {
        if (index > 0 && QByteArray(argv[index]) == QByteArrayLiteral("--")) {
            break;
        }

        arguments.storage.push_back(QByteArray(argv[index]));
    }

    arguments.argv.reserve(arguments.storage.size());
    for (QByteArray& argument : arguments.storage) {
        arguments.argv.push_back(argument.data());
    }
    arguments.argc = static_cast<int>(arguments.storage.size());
    arguments.argv.push_back(nullptr);
    return arguments;
}

void request_vsync_surface_format()
{
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);
}

int process_exit_status(VNM_TerminalSurface::Exit_reason reason, int exit_code)
{
    switch (reason) {
        case VNM_TerminalSurface::Exit_reason::EXITED:
            return exit_code;
        case VNM_TerminalSurface::Exit_reason::INTERRUPTED:
        case VNM_TerminalSurface::Exit_reason::TERMINATED:
            return exit_code != 0 ? exit_code : k_exit_process_failed;
        case VNM_TerminalSurface::Exit_reason::FAILED_TO_START:
            return k_exit_start_failed;
    }

    return k_exit_process_failed;
}

int app_status_after_process_exit(
    const App_options&     options,
    const Runtime_state&   state)
{
    const int status = process_exit_status(
        state.process_exit_reason,
        state.process_exit_code);

    if (status != 0) {
        print_error(QStringLiteral("process exited with %1, code %2")
            .arg(enum_key(state.process_exit_reason))
            .arg(state.process_exit_code));
        return status;
    }

    if (options.require_output && !state.output_seen) {
        print_error(QStringLiteral("required terminal output activity was not observed"));
        return k_exit_no_output;
    }

    return 0;
}

}

#ifndef VNM_TERMINAL_APP_NO_MAIN
int main(int argc, char** argv)
{
    const QStringList arguments = raw_arguments(argc, argv);
    request_vsync_surface_format();

    Qt_arguments qt_arguments = make_qt_arguments(argc, argv);
    QGuiApplication app(qt_arguments.argc, qt_arguments.argv.data());
    QCoreApplication::setOrganizationName(QStringLiteral("Varinomics"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("varinomics.com"));
    QCoreApplication::setApplicationName(QStringLiteral("vnm_terminal"));
    QCoreApplication::setApplicationVersion(QStringLiteral(VNM_TERMINAL_VERSION_STRING));
    const QIcon app_icon(
        QStringLiteral(
            ":/vnm_terminal/vnm_terminal.ico"));
    QGuiApplication::setWindowIcon(app_icon);

    Parse_result parse_result = parse_arguments(arguments);
    if (parse_result.help_requested) {
        print_usage();
        return 0;
    }

    if (!parse_result.error.isEmpty()) {
        print_error(parse_result.error);
        print_usage();
        return k_exit_usage_error;
    }

    App_options options = std::move(parse_result.options);
    if (!validate_capture_paths(&options, &parse_result.error)) {
        print_error(parse_result.error);
        return k_exit_usage_error;
    }

    if (!options.backend_output_capture_path.isEmpty() &&
        !prepare_capture_file(
            QStringLiteral("--capture-output"),
            options.backend_output_capture_path, &parse_result.error))
    {
        print_error(parse_result.error);
        return k_exit_usage_error;
    }
    if (!options.transcript_capture_path.isEmpty() &&
        !prepare_capture_file(
            QStringLiteral("--capture-transcript"),
            options.transcript_capture_path, &parse_result.error))
    {
        print_error(parse_result.error);
        return k_exit_usage_error;
    }
    if (!options.metrics_json_path.isEmpty() &&
        !prepare_capture_file(
            QStringLiteral("--metrics-json"),
            options.metrics_json_path, &parse_result.error))
    {
        print_error(parse_result.error);
        return k_exit_usage_error;
    }
#if VNM_TERMINAL_PROFILING_ENABLED
    if (!options.profile_text_path.isEmpty() &&
        !prepare_profile_text_file(options.profile_text_path, &parse_result.error))
    {
        print_error(parse_result.error);
        return k_exit_usage_error;
    }
#endif

    const bool persistence_enabled = terminal_window_persistence_enabled();
    if (persistence_enabled) {
        QSettings settings;
        apply_persisted_terminal_window_state(
            load_persisted_terminal_window_state(settings),
            &options);
    }

    QQmlEngine chrome_engine;

    QQuickWindow window;
    window.setTitle(default_window_title());
    window.setIcon(app_icon);
    window.setColor(options.custom_titlebar
        ? chrome::terminal_chrome_background_color(window.isActive())
        : QColor(9, 12, 16));
    window.resize(options.window_size);
    if (options.window_position.has_value()) {
        window.setPosition(*options.window_position);
    }
    if (options.custom_titlebar) {
        window.setFlags(window.flags() | Qt::FramelessWindowHint);
    }

    std::unique_ptr<chrome::Terminal_qml_chrome> titlebar;
    if (options.custom_titlebar) {
        titlebar = std::make_unique<chrome::Terminal_qml_chrome>(chrome_engine, window);
        if (!titlebar->is_valid()) {
            print_error(QStringLiteral("failed to create shared window chrome: %1")
                .arg(titlebar->error_string()));
            return k_exit_start_failed;
        }
    }
    auto* titlebar_ptr = titlebar.get();

    auto* surface = new VNM_TerminalSurface(window.contentItem());
    term::VNM_TerminalSurface_render_bridge::set_selection_trace_enabled(
        *surface,
        options.selection_trace_enabled);
#if VNM_TERMINAL_PROFILING_ENABLED
    std::unique_ptr<term::Hierarchical_profiler> gui_profiler;
    std::unique_ptr<term::Active_profiler_binding> gui_profiler_binding;
    std::shared_ptr<term::Hierarchical_profiler> render_profiler;
    if (!options.profile_text_path.isEmpty()) {
        gui_profiler = std::make_unique<term::Hierarchical_profiler>();
        gui_profiler_binding =
            std::make_unique<term::Active_profiler_binding>(gui_profiler.get());
        render_profiler = std::make_shared<term::Hierarchical_profiler>();
        term::VNM_TerminalSurface_render_bridge::set_render_profiler(
            *surface,
            render_profiler);
        term::VNM_TerminalSurface_render_bridge::set_dirty_row_stats_enabled(
            *surface,
            true);
    }
#endif
    auto* scrollbar = new chrome::Terminal_scrollbar(window.contentItem());
    scrollbar->set_surface(surface);
    scrollbar->set_wheel_trace_enabled(options.wheel_trace_enabled);
    surface->set_font_family(options.font_family);
    surface->set_font_size(options.font_size);
    surface->set_color_theme(options.theme);
    surface->set_wheel_event_policy(
        VNM_TerminalSurface::Wheel_event_policy::LOCAL_SCROLLBACK_FIRST);
    apply_scrollback_limit_option(*surface, options);
#if defined(Q_OS_MACOS)
    surface->set_copy_shortcut_policy(VNM_TerminalSurface::Copy_shortcut_policy::TERMINAL_INPUT);
#endif
    surface->set_alternate_screen_wheel_policy(options.alternate_screen_wheel_policy);
    surface->set_text_renderer_mode(options.text_renderer_mode);
    surface->set_lcd_subpixel_order(options.lcd_subpixel_order);
    apply_synchronized_output_scroll_policy_option(*surface, options);
    apply_primary_repaint_recovery_option(*surface, options);
    surface->set_backend_output_capture_path(options.backend_output_capture_path);
    surface->set_transcript_capture_path(options.transcript_capture_path);
    surface->set_transcript_snapshot_diagnostics(options.transcript_snapshot_diagnostics);
    surface->set_transcript_timing_diagnostics(options.transcript_timing_diagnostics);
    surface->set_wheel_trace_enabled(options.wheel_trace_enabled);
    install_wheel_delivery_indicator_filter(
        *surface,
        *scrollbar,
        titlebar_ptr,
        options.wheel_trace_enabled);
    const bool custom_titlebar_enabled = options.custom_titlebar;
    std::optional<Persisted_terminal_window_state> latest_restorable_window_state =
        restorable_terminal_window_state(window, *surface);
    const auto remember_restorable_window_state =
        [&window, surface, &latest_restorable_window_state] {
            const std::optional<Persisted_terminal_window_state> state =
                restorable_terminal_window_state(window, *surface);
            if (state.has_value()) {
                latest_restorable_window_state = *state;
            }
        };

    apply_terminal_shell_geometry(
        window,
        *surface,
        *scrollbar,
        titlebar_ptr,
        custom_titlebar_enabled);
    window.installEventFilter(new Terminal_shortcut_filter(surface));

    connect_terminal_metadata_to_chrome(*surface, window, titlebar_ptr);
    QObject::connect(
        surface,
        &VNM_TerminalSurface::clipboard_write_requested,
        surface,
        [surface, policy = options.osc52_clipboard_policy](
            quint64 request_id,
            const QString& target_selection,
            const QByteArray& payload)
        {
            // Respond during the signal delivery so the single pending host
            // request slot in VNM_TerminalSurface cannot be superseded.
            handle_clipboard_write_request(
                *surface,
                request_id,
                target_selection,
                payload.size(),
                policy);
        });
    QObject::connect(
        surface,
        &VNM_TerminalSurface::text_area_resize_requested,
        &window,
        [&window, surface](int rows, int columns) {
            (void)resize_window_for_text_area_request(
                window,
                *surface,
                rows,
                columns);
        });

    QObject::connect(
        &window,
        &QQuickWindow::widthChanged,
        surface,
        [
            titlebar_ptr,
            &window,
            surface,
            scrollbar,
            custom_titlebar_enabled,
            remember_restorable_window_state
        ] {
            apply_terminal_shell_geometry(
                window,
                *surface,
                *scrollbar,
                titlebar_ptr,
                custom_titlebar_enabled);
            remember_restorable_window_state();
        });
    QObject::connect(
        &window,
        &QQuickWindow::heightChanged,
        surface,
        [
            titlebar_ptr,
            &window,
            surface,
            scrollbar,
            custom_titlebar_enabled,
            remember_restorable_window_state
        ] {
            apply_terminal_shell_geometry(
                window,
                *surface,
                *scrollbar,
                titlebar_ptr,
                custom_titlebar_enabled);
            remember_restorable_window_state();
        });
    QObject::connect(
        &window,
        &QWindow::xChanged,
        surface,
        [remember_restorable_window_state](int) {
            remember_restorable_window_state();
        });
    QObject::connect(
        &window,
        &QWindow::yChanged,
        surface,
        [remember_restorable_window_state](int) {
            remember_restorable_window_state();
        });

    if (titlebar_ptr != nullptr) {
        auto sync_titlebar_state = [titlebar_ptr, &window] {
            sync_chrome_window_state(*titlebar_ptr, window);
        };
        auto sync_titlebar_state_and_geometry =
            [
                titlebar_ptr,
                &window,
                surface,
                scrollbar,
                custom_titlebar_enabled,
                remember_restorable_window_state
            ] {
                sync_chrome_window_state(*titlebar_ptr, window);
                apply_terminal_shell_geometry(
                    window,
                    *surface,
                    *scrollbar,
                    titlebar_ptr,
                    custom_titlebar_enabled);
                remember_restorable_window_state();
            };
        QObject::connect(
            &window,
            &QWindow::activeChanged,
            titlebar_ptr,
            [titlebar_ptr, &window] {
                sync_chrome_window_state(*titlebar_ptr, window);
            });
        QObject::connect(
            &window,
            &QWindow::windowStateChanged,
            titlebar_ptr,
            [sync_titlebar_state_and_geometry](Qt::WindowState) {
                sync_titlebar_state_and_geometry();
            });
        sync_titlebar_state();
    }
    else {
        QObject::connect(
            &window,
            &QWindow::windowStateChanged,
            surface,
            [remember_restorable_window_state](Qt::WindowState) {
                remember_restorable_window_state();
            });
    }

    QObject::connect(
        &app,
        &QCoreApplication::aboutToQuit,
        surface,
        [
            persistence_enabled,
            surface,
            &window,
            &latest_restorable_window_state
        ] {
            if (!persistence_enabled) {
                return;
            }

            const std::optional<Persisted_terminal_window_state> current_state =
                restorable_terminal_window_state(window, *surface);
            Persisted_terminal_window_state state =
                current_state.value_or(
                    latest_restorable_window_state.value_or(
                        Persisted_terminal_window_state{}));
            state.font_size = surface->font_size();
            state.maximized = window.windowStates().testFlag(Qt::WindowMaximized);

            QSettings settings;
            save_persisted_terminal_window_state(settings, state);
        });

    Runtime_state state;
    QElapsedTimer startup_elapsed_timer;
    startup_elapsed_timer.start();

    QObject::connect(
        surface,
        &VNM_TerminalSurface::backend_error,
        surface,
        [&state](VNM_TerminalSurface::Backend_error_code code, const QString& message) {
            ++state.backend_error_count;
            print_error(QStringLiteral("backend error [%1]: %2")
                .arg(enum_key(code), message));
        });
    QObject::connect(
        surface,
        &VNM_TerminalSurface::output_activity,
        surface,
        [&state, &startup_elapsed_timer] {
            if (!state.output_seen) {
                state.first_output_elapsed_ms = startup_elapsed_timer.elapsed();
            }
            state.output_seen = true;
        });
    QObject::connect(
        surface,
        &VNM_TerminalSurface::process_exited,
        surface,
        [&options, &state, &window](
            VNM_TerminalSurface::Exit_reason reason,
            int exit_code)
        {
            state.process_exited      = true;
            state.process_exit_reason = reason;
            state.process_exit_code   = exit_code;

            if (!options.keep_open_after_process_exits) {
                QCoreApplication::exit(
                    state.timeout_expired
                        ? k_exit_timeout
                        : app_status_after_process_exit(options, state));
            }
        });

    QTimer timeout_timer(&app);
    QTimer timeout_force_exit_timer(&app);
    timeout_timer.setSingleShot(true);
    timeout_force_exit_timer.setSingleShot(true);
    QObject::connect(
        &timeout_timer,
        &QTimer::timeout,
        &app,
        [&options, &state, surface, &timeout_force_exit_timer] {
            if (state.process_exited) {
                QCoreApplication::exit(app_status_after_process_exit(options, state));
                return;
            }

            state.timeout_expired = true;
            print_error(QStringLiteral("timeout after %1 ms").arg(*options.timeout_ms));
            if (!surface->terminate_process()) {
                QCoreApplication::exit(k_exit_timeout);
                return;
            }

            timeout_force_exit_timer.start(k_timeout_force_exit_grace_ms);
        });
    QObject::connect(
        &timeout_force_exit_timer,
        &QTimer::timeout,
        &app,
        [] {
            QCoreApplication::exit(k_exit_timeout);
        });

    window.show();
    if (options.restore_maximized_window_state) {
        window.setWindowState(Qt::WindowMaximized);
    }
    surface->forceActiveFocus();

    QTimer::singleShot(0, &app, [&options, &state, surface, &timeout_timer] {
        if (!surface->start_process(options.command, options.working_directory)) {
            if (state.backend_error_count == 0) {
                print_error(QStringLiteral("failed to start terminal process"));
            }

            QCoreApplication::exit(k_exit_start_failed);
            return;
        }

        if (options.timeout_ms.has_value()) {
            timeout_timer.start(*options.timeout_ms);
        }
    });

    QElapsedTimer app_elapsed_timer;
    app_elapsed_timer.start();
    int app_result = app.exec();
    metrics_timing_t metrics_timing;
    metrics_timing.app_elapsed_ms         = app_elapsed_timer.elapsed();
    metrics_timing.profile_text_requested = !options.profile_text_path.isEmpty();
#if VNM_TERMINAL_PROFILING_ENABLED
    if (!options.profile_text_path.isEmpty() && gui_profiler != nullptr) {
        QString profile_error;
        QElapsedTimer profile_write_timer;
        profile_write_timer.start();
        if (!write_profile_text(
                options.profile_text_path, *surface, *gui_profiler, &profile_error))
        {
            print_error(profile_error);
            if (app_result == 0) {
                app_result = k_exit_usage_error;
            }
        }
        metrics_timing.profile_write_elapsed_ms = profile_write_timer.elapsed();
    }
#endif

    if (!options.metrics_json_path.isEmpty()) {
        QString metrics_error;
        if (!write_metrics_json(
                options.metrics_json_path,
                *surface,
                state,
                metrics_timing,
                app_result,
                &metrics_error))
        {
            print_error(metrics_error);
            if (app_result == 0) {
                app_result = k_exit_usage_error;
            }
        }
    }

    if (app_result == 0 && options.require_output && !state.output_seen) {
        print_error(QStringLiteral("required terminal output activity was not observed"));
        return k_exit_no_output;
    }

    return app_result;
}
#endif
