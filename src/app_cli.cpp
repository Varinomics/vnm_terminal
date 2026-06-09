#include "app_cli.h"

#include "app_clipboard_policy.h"
#include "app_common.h"
#include "app_options.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLatin1Char>
#include <QLatin1String>
#include <QSize>
#include <QString>
#include <QStringList>

#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>

#ifndef VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
#define VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED 0
#endif

namespace vnm_terminal::terminal_app {

namespace {

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

} // namespace

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

} // namespace vnm_terminal::terminal_app
