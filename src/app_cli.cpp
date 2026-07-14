#include "app_cli.h"

#include "app_clipboard_policy.h"
#include "app_common.h"
#include "app_options.h"

#include "vnm_terminal/vnm_terminal_surface.h"
#include "vnm_terminal/internal/terminal_color_scheme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLatin1Char>
#include <QLatin1String>
#include <QSize>
#include <QString>
#include <QStringList>

#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>

#ifndef VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
#define VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED 0
#endif

namespace vnm_terminal::terminal_app {

namespace {

constexpr std::size_t k_bytes_per_mib = 1024U * 1024U;

std::size_t whole_mib_ceiling(std::size_t byte_count)
{
    return
        byte_count / k_bytes_per_mib +
        (byte_count % k_bytes_per_mib == 0U ? 0U : 1U);
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

bool parse_metrics_timeline_interval_ms(
    const QString&         value,
    int*                   out_interval_ms,
    QString*               out_error)
{
    bool ok = false;
    const qlonglong interval_ms = value.toLongLong(&ok);
    if (!ok ||
        interval_ms <= 0 ||
        interval_ms >  static_cast<qlonglong>(std::numeric_limits<int>::max()))
    {
        *out_error = QStringLiteral(
            "--metrics-timeline-interval-ms requires a positive integer");
        return false;
    }

    *out_interval_ms = static_cast<int>(interval_ms);
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

bool parse_retained_history_capacity_mib(
    const QString&              value,
    std::optional<std::size_t>* out_capacity_bytes,
    QString*                    out_error)
{
    const std::size_t minimum_mib = whole_mib_ceiling(
        VNM_TerminalSurface::minimum_retained_history_capacity_bytes());
    const std::size_t maximum_mib =
        VNM_TerminalSurface::maximum_retained_history_capacity_bytes() /
        k_bytes_per_mib;

    bool ok = false;
    const qulonglong capacity_mib = value.toULongLong(&ok);
    if (!ok || capacity_mib < minimum_mib || capacity_mib > maximum_mib) {
        *out_error = QStringLiteral(
            "--retained-history-capacity-mib requires an integer from %1 to %2")
            .arg(minimum_mib)
            .arg(maximum_mib);
        return false;
    }

    *out_capacity_bytes = static_cast<std::size_t>(capacity_mib) * k_bytes_per_mib;
    return true;
}

// Keyword-valued options parse through one descriptor table per option: the
// table is the single source of truth for the accepted keywords, the mapped
// values, and the derived "supports only ..." reject message, so adding a
// keyword cannot drift the parser and its diagnostics apart. Help text stays
// handwritten in print_usage (its wording and grouping are not table-shaped).
template <typename Value_type>
struct cli_enum_choice_t
{
    const char* keyword;
    Value_type  value;
};

template <typename Value_type, std::size_t Choice_count>
struct cli_enum_option_t
{
    const char*                                             name;
    std::array<cli_enum_choice_t<Value_type>, Choice_count> choices;
};

QString cli_enum_option_error(const char* option_name, const QStringList& keywords)
{
    QString choices_text;
    for (qsizetype i = 0; i < keywords.size(); ++i) {
        if (i > 0) {
            choices_text += i + 1 == keywords.size()
                ? (keywords.size() == 2 ? QStringLiteral(" or ") : QStringLiteral(", or "))
                : QStringLiteral(", ");
        }
        choices_text += keywords[i];
    }

    return QStringLiteral("%1 supports only %2")
        .arg(QLatin1String(option_name), choices_text);
}

template <typename Value_type, std::size_t Choice_count>
bool parse_cli_enum_option(
    const cli_enum_option_t<Value_type, Choice_count>& option,
    const QString&                                     value,
    Value_type*                                        out_value,
    QString*                                           out_error)
{
    const QString normalized = value.trimmed().toLower();
    for (const cli_enum_choice_t<Value_type>& choice : option.choices) {
        if (normalized == QLatin1String(choice.keyword)) {
            *out_value = choice.value;
            return true;
        }
    }

    QStringList keywords;
    for (const cli_enum_choice_t<Value_type>& choice : option.choices) {
        keywords << QLatin1String(choice.keyword);
    }
    *out_error = cli_enum_option_error(option.name, keywords);
    return false;
}

constexpr cli_enum_option_t<VNM_TerminalSurface::Alternate_screen_wheel_policy, 3>
k_alternate_wheel_option{
    "--alternate-wheel",
    {{
        {"page",   VNM_TerminalSurface::Alternate_screen_wheel_policy::PAGE_KEYS},
        {"cursor", VNM_TerminalSurface::Alternate_screen_wheel_policy::CURSOR_KEYS},
        {"mouse",  VNM_TerminalSurface::Alternate_screen_wheel_policy::MOUSE_REPORTING_FIRST},
    }},
};

constexpr cli_enum_option_t<VNM_TerminalSurface::Synchronized_output_scroll_policy, 2>
k_synchronized_output_scroll_policy_option{
    "--synchronized-output-scroll-policy",
    {{
        {"defer",
            VNM_TerminalSurface::Synchronized_output_scroll_policy::
                DEFER_UNTIL_CONTENT_PUBLICATION},
        {"immediate-public",
            VNM_TerminalSurface::Synchronized_output_scroll_policy::
                IMMEDIATE_PUBLIC_PROJECTION},
    }},
};

constexpr cli_enum_option_t<VNM_TerminalSurface::Text_renderer_mode, 3>
k_text_renderer_option{
    "--text-renderer",
    {{
        {"auto",  VNM_TerminalSurface::Text_renderer_mode::AUTO},
        {"msdf",  VNM_TerminalSurface::Text_renderer_mode::MSDF},
        {"glyph", VNM_TerminalSurface::Text_renderer_mode::GLYPH},
    }},
};

constexpr cli_enum_option_t<VNM_TerminalSurface::Lcd_subpixel_order, 6>
k_lcd_subpixel_option{
    "--lcd-subpixel",
    {{
        {"auto", VNM_TerminalSurface::Lcd_subpixel_order::AUTO},
        {"none", VNM_TerminalSurface::Lcd_subpixel_order::NONE},
        {"rgb",  VNM_TerminalSurface::Lcd_subpixel_order::RGB},
        {"bgr",  VNM_TerminalSurface::Lcd_subpixel_order::BGR},
        {"vrgb", VNM_TerminalSurface::Lcd_subpixel_order::VRGB},
        {"vbgr", VNM_TerminalSurface::Lcd_subpixel_order::VBGR},
    }},
};

constexpr cli_enum_option_t<bool, 2> k_row_timestamps_option{
    "--row-timestamps",
    {{
        {"on",  true},
        {"off", false},
    }},
};

constexpr cli_enum_option_t<Osc52_clipboard_policy, 2> k_osc52_clipboard_option{
    "--osc52-clipboard",
    {{
        {"deny",  Osc52_clipboard_policy::DENY},
        {"allow", Osc52_clipboard_policy::ALLOW},
    }},
};

constexpr cli_enum_option_t<Paste_shortcut_policy, 4> k_paste_shortcut_option{
    "--paste-shortcut",
    {{
        {"disabled",                Paste_shortcut_policy::DISABLED},
        {"ctrl-shift-v",            Paste_shortcut_policy::CTRL_SHIFT_V},
        {"ctrl-v-and-ctrl-shift-v", Paste_shortcut_policy::CTRL_V_AND_CTRL_SHIFT_V},
        {"platform-default",        Paste_shortcut_policy::PLATFORM_DEFAULT},
    }},
};

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

struct capture_path_option_t
{
    const char* name;
    QString*    path;
};

struct capture_path_conflict_t
{
    std::size_t first;
    std::size_t second;
};

bool validate_present_capture_path(
    const capture_path_option_t& option,
    QString*                     out_error)
{
    if (option.path->isEmpty()) {
        return true;
    }

    return validate_capture_path(
        QString::fromLatin1(option.name),
        *option.path,
        option.path,
        out_error);
}

bool capture_paths_conflict(
    const capture_path_option_t& first,
    const capture_path_option_t& second)
{
    return
        !first.path->isEmpty() &&
        !second.path->isEmpty() &&
        comparable_capture_path(*first.path) == comparable_capture_path(*second.path);
}

QString capture_path_conflict_error(
    const capture_path_option_t& first,
    const capture_path_option_t& second)
{
    return QStringLiteral("%1 and %2 must use different paths: %3")
        .arg(QString::fromLatin1(first.name), QString::fromLatin1(second.name), *first.path);
}

} // namespace

void print_usage()
{
    const std::size_t minimum_retained_history_mib = whole_mib_ceiling(
        VNM_TerminalSurface::minimum_retained_history_capacity_bytes());
    const std::size_t maximum_retained_history_mib =
        VNM_TerminalSurface::maximum_retained_history_capacity_bytes() /
        k_bytes_per_mib;
    const std::size_t default_retained_history_mib =
        VNM_TerminalSurface::default_retained_history_capacity_bytes() /
        k_bytes_per_mib;

    std::cout
        << "usage: vnm_terminal [options]\n"
        << "       vnm_terminal [options] -- <program> [args...]\n"
        << "\n"
        << "options:\n"
        << "  --shell                         launch the default shell; also the default without --\n"
        << "  --cwd <path>                    launch in a working directory\n"
        << "  --font-family <family>          terminal font family\n"
        << "  --font-size <pixels>            terminal font size in pixels\n"
        << "  --color-scheme <name>           terminal color scheme (e.g. Campbell)\n"
        << "  --scrollback-limit <rows>       maximum retained scrollback rows\n"
        << "  --retained-history-capacity-mib <MiB>\n"
        << "                                  retained-history byte capacity, "
        << minimum_retained_history_mib
        << ".."
        << maximum_retained_history_mib
        << ", default "
        << default_retained_history_mib
        << "\n"
        << "  --window-size <width>x<height>  window size in logical pixels\n"
#if defined(_WIN32) || defined(__linux__)
        << "  --native-titlebar               use the platform titlebar instead of built-in chrome\n"
#endif
        << "  --alternate-wheel <mode>        alternate-screen wheel: mouse(default), cursor, or page\n"
        << "  --text-renderer <mode>          text renderer: auto(default), msdf, or glyph\n"
        << "  --lcd-subpixel <order>          MSDF LCD order: auto(default), none, rgb, bgr, vrgb, or vbgr\n"
        << "  --row-timestamps <mode>         row hover timestamp tooltip: on(default) or off\n"
        << "  --synchronized-output-scroll-policy <policy>\n"
        << "                                  DEC synchronized-output scroll: defer(default) "
        << "or immediate-public (case-insensitive)\n"
        << "  --disable-primary-repaint-recovery\n"
        << "                                  disable primary repaint scrollback recovery "
        << "when enabled\n"
        << "  --capture-output <path>         write raw backend output bytes to a file\n"
        << "  --metrics-json <path>           write lightweight terminal runtime metrics\n"
        << "  --metrics-timeline-jsonl <path> write periodic terminal metrics JSONL samples\n"
        << "  --metrics-timeline-interval-ms <n>\n"
        << "                                  metrics timeline sample interval, default 5000\n"
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
        << "  --paste-shortcut <mode>         paste shortcut: platform-default(default), ctrl-shift-v, ctrl-v-and-ctrl-shift-v, or disabled\n"
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
#if VNM_TERMINAL_PROFILING_ENABLED
    const std::array<capture_path_option_t, 5> capture_paths{{
        {"--capture-output",         &options->backend_output_capture_path},
        {"--capture-transcript",     &options->transcript_capture_path},
        {"--metrics-json",           &options->metrics_json_path},
        {"--metrics-timeline-jsonl", &options->metrics_timeline_jsonl_path},
        {"--profile-text",           &options->profile_text_path},
    }};
#else
    const std::array<capture_path_option_t, 4> capture_paths{{
        {"--capture-output",         &options->backend_output_capture_path},
        {"--capture-transcript",     &options->transcript_capture_path},
        {"--metrics-json",           &options->metrics_json_path},
        {"--metrics-timeline-jsonl", &options->metrics_timeline_jsonl_path},
    }};
#endif

    constexpr std::size_t capture_output_path         = 0;
    constexpr std::size_t capture_transcript_path     = 1;
    constexpr std::size_t metrics_json_path           = 2;
    constexpr std::size_t metrics_timeline_jsonl_path = 3;
#if VNM_TERMINAL_PROFILING_ENABLED
    constexpr std::size_t profile_text_path           = 4;
#endif

    for (const capture_path_option_t& path : capture_paths) {
        if (!validate_present_capture_path(path, out_error)) {
            return false;
        }
    }

#if VNM_TERMINAL_PROFILING_ENABLED
    const std::array<capture_path_conflict_t, 10> conflicts{{
        {capture_output_path,     capture_transcript_path},
        {capture_output_path,     metrics_json_path},
        {capture_output_path,     metrics_timeline_jsonl_path},
        {capture_transcript_path, metrics_json_path},
        {capture_transcript_path, metrics_timeline_jsonl_path},
        {metrics_json_path,       metrics_timeline_jsonl_path},
        {profile_text_path,       capture_output_path},
        {profile_text_path,       capture_transcript_path},
        {profile_text_path,       metrics_json_path},
        {profile_text_path,       metrics_timeline_jsonl_path},
    }};
#else
    const std::array<capture_path_conflict_t, 6> conflicts{{
        {capture_output_path,     capture_transcript_path},
        {capture_output_path,     metrics_json_path},
        {capture_output_path,     metrics_timeline_jsonl_path},
        {capture_transcript_path, metrics_json_path},
        {capture_transcript_path, metrics_timeline_jsonl_path},
        {metrics_json_path,       metrics_timeline_jsonl_path},
    }};
#endif

    for (const capture_path_conflict_t& conflict : conflicts) {
        const capture_path_option_t& first  = capture_paths[conflict.first];
        const capture_path_option_t& second = capture_paths[conflict.second];
        if (capture_paths_conflict(first, second)) {
            *out_error = capture_path_conflict_error(first, second);
            return false;
        }
    }

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

            result.options.font_family          = value;
            result.options.font_family_explicit = true;
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

        if (argument_is(argument, "--color-scheme")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }

            result.options.color_scheme          = value;
            result.options.color_scheme_explicit = true;
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

        if (argument_is(argument, "--retained-history-capacity-mib")) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_retained_history_capacity_mib(
                    value,
                    &result.options.retained_history_capacity_bytes,
                    &result.error))
            {
                return result;
            }

            continue;
        }

        if (argument_is(argument, k_alternate_wheel_option.name)) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_cli_enum_option(
                    k_alternate_wheel_option,
                    value, &result.options.alternate_screen_wheel_policy, &result.error))
            {
                return result;
            }

            continue;
        }

        if (argument_is(argument, k_text_renderer_option.name)) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_cli_enum_option(
                    k_text_renderer_option,
                    value, &result.options.text_renderer_mode, &result.error))
            {
                return result;
            }

            result.options.text_renderer_mode_explicit = true;
            continue;
        }

        if (argument_is(argument, k_lcd_subpixel_option.name)) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_cli_enum_option(
                    k_lcd_subpixel_option,
                    value, &result.options.lcd_subpixel_order, &result.error))
            {
                return result;
            }

            result.options.lcd_subpixel_order_explicit = true;
            continue;
        }

        if (argument_is(argument, k_row_timestamps_option.name)) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_cli_enum_option(
                    k_row_timestamps_option,
                    value, &result.options.row_timestamp_tooltip_enabled, &result.error))
            {
                return result;
            }

            result.options.row_timestamp_tooltip_explicit = true;
            continue;
        }

        if (argument_is(argument, k_osc52_clipboard_option.name)) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_cli_enum_option(
                    k_osc52_clipboard_option,
                    value, &result.options.osc52_clipboard_policy, &result.error))
            {
                return result;
            }

            continue;
        }

        if (argument_is(argument, k_paste_shortcut_option.name)) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_cli_enum_option(
                    k_paste_shortcut_option,
                    value, &result.options.paste_shortcut_policy, &result.error))
            {
                return result;
            }

            continue;
        }

        if (argument_is(argument, k_synchronized_output_scroll_policy_option.name)) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_cli_enum_option(
                    k_synchronized_output_scroll_policy_option,
                    value, &result.options.synchronized_output_scroll_policy, &result.error))
            {
                return result;
            }

            continue;
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

        if (argument_is(argument, "--metrics-timeline-jsonl")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }
            if (value.trimmed().isEmpty()) {
                result.error = QStringLiteral("--metrics-timeline-jsonl requires a non-empty path");
                return result;
            }

            result.options.metrics_timeline_jsonl_path = value;
            continue;
        }

        if (argument_is(argument, "--metrics-timeline-interval-ms")) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_metrics_timeline_interval_ms(
                    value,
                    &result.options.metrics_timeline_interval_ms,
                    &result.error))
            {
                return result;
            }

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

    const QString color_scheme = result.options.color_scheme.trimmed();
    if (color_scheme.isEmpty()) {
        result.error = QStringLiteral("--color-scheme requires a non-empty scheme name");
        return result;
    }

    const vnm_terminal::internal::Terminal_color_scheme* scheme =
        vnm_terminal::internal::find_color_scheme(color_scheme);
    if (scheme == nullptr) {
        result.error =
            QStringLiteral("--color-scheme: unknown scheme name '%1'").arg(color_scheme);
        return result;
    }
    result.options.color_scheme = scheme->name;

    return result;
}

} // namespace vnm_terminal::terminal_app
