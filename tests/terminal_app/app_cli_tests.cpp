#include "app_cli.h"

#include "helpers/test_check.h"

#include <QGuiApplication>
#include <QLatin1String>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <initializer_list>

namespace chrome = vnm_terminal::terminal_app;

namespace {

using vnm_terminal::test_helpers::check;

QStringList arguments(std::initializer_list<const char*> values)
{
    QStringList out;
    for (const char* value : values) {
        out << QLatin1String(value);
    }
    return out;
}

chrome::Parse_result parse(std::initializer_list<const char*> values)
{
    return chrome::parse_arguments(arguments(values));
}

template <typename Value_type>
struct enum_option_case_t
{
    const char* value;
    Value_type  expected;
};

template <typename Value_type, typename Getter>
bool test_enum_option(
    const char* option_name,
    Getter getter,
    Value_type default_value,
    std::initializer_list<enum_option_case_t<Value_type>> cases,
    const char* invalid_value,
    const char* invalid_error)
{
    chrome::Parse_result default_result = parse({
        "vnm_terminal",
        "--",
        "fixture-command",
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(), "enum option default parse succeeds");
    ok &= check(getter(default_result.options) == default_value,
        "enum option default value matches");

    for (const enum_option_case_t<Value_type>& test_case : cases) {
        chrome::Parse_result result = parse({
            "vnm_terminal",
            option_name,
            test_case.value,
            "--",
            "fixture-command",
        });
        ok &= check(result.error.isEmpty(), "enum option value parses");
        ok &= check(getter(result.options) == test_case.expected,
            "enum option value maps to expected enum");
    }

    chrome::Parse_result invalid_result = parse({
        "vnm_terminal",
        option_name,
        invalid_value,
    });
    ok &= check(!invalid_result.error.isEmpty(), "enum option rejects invalid value");
    ok &= check(invalid_result.error == QLatin1String(invalid_error),
        "enum option invalid value reports documented error");
    ok &= check(getter(invalid_result.options) == default_value,
        "rejected enum option keeps default");

    const enum_option_case_t<Value_type>& first_case = *cases.begin();
    chrome::Parse_result command_result = parse({
        "vnm_terminal",
        "--",
        option_name,
        first_case.value,
    });
    ok &= check(command_result.error.isEmpty(),
        "enum option after command separator parses as command argv");
    ok &= check(getter(command_result.options) == default_value,
        "enum option after command separator leaves default");
    ok &= check(command_result.options.command == arguments({option_name, first_case.value}),
        "enum option after command separator is preserved in command argv");
    return ok;
}

bool test_parse_titlebar_options()
{
    chrome::Parse_result default_result = parse({
        "vnm_terminal",
        "--",
        "fixture-command",
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(),
        "default titlebar mode parses");
#if defined(_WIN32) || defined(__linux__)
    ok &= check(default_result.options.custom_titlebar,
        "default titlebar mode enables custom titlebar on validated platforms");
#else
    ok &= check(!default_result.options.custom_titlebar,
        "default titlebar mode keeps native titlebar on unvalidated platforms");
#endif

    chrome::Parse_result custom_result = parse({
        "vnm_terminal",
        "--custom-titlebar",
        "--",
        "fixture-command",
    });
    ok &= check(!custom_result.error.isEmpty(),
        "custom-titlebar option is not a public app option");

    chrome::Parse_result native_result = parse({
        "vnm_terminal",
        "--native-titlebar",
        "--",
        "fixture-command",
    });

#if defined(_WIN32) || defined(__linux__)
    ok &= check(native_result.error.isEmpty(),
        "native-titlebar option parses on validated platform");
    ok &= check(!native_result.options.custom_titlebar,
        "native-titlebar option disables custom titlebar mode");
#else
    ok &= check(!native_result.error.isEmpty(),
        "native-titlebar option is rejected on unvalidated platforms");
#endif
    return ok;
}

bool test_parse_selection_trace_option()
{
    chrome::Parse_result default_result = parse({
        "vnm_terminal",
        "--",
        "fixture-command",
    });
    chrome::Parse_result trace_result = parse({
        "vnm_terminal",
        "--selection-trace",
        "--",
        "fixture-command",
    });
    chrome::Parse_result command_result = parse({
        "vnm_terminal",
        "--",
        "--selection-trace",
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(), "selection trace default parse succeeds");
    ok &= check(!default_result.options.selection_trace_enabled,
        "selection trace defaults off");
    ok &= check(trace_result.error.isEmpty(),
        "selection-trace option parses before command separator");
    ok &= check(trace_result.options.selection_trace_enabled,
        "selection-trace option enables tracing before command separator");
    ok &= check(command_result.error.isEmpty(),
        "selection-trace command argument parses after command separator");
    ok &= check(!command_result.options.selection_trace_enabled,
        "selection-trace after command separator remains a command argument");
    ok &= check(command_result.options.command == arguments({"--selection-trace"}),
        "selection-trace after command separator is preserved in command argv");
    return ok;
}

bool test_parse_wheel_trace_option()
{
    chrome::Parse_result trace_result = parse({
        "vnm_terminal",
        "--wheel-trace",
        "--capture-transcript",
        "wheel.ndjson",
        "--",
        "fixture-command",
    });
    chrome::Parse_result missing_capture_result = parse({
        "vnm_terminal",
        "--wheel-trace",
        "--",
        "fixture-command",
    });
    chrome::Parse_result command_result = parse({
        "vnm_terminal",
        "--",
        "--wheel-trace",
    });

    bool ok = true;
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    ok &= check(trace_result.error.isEmpty(),
        "wheel-trace option parses with transcript capture");
    ok &= check(trace_result.options.wheel_trace_enabled,
        "wheel-trace option enables tracing when transcript capture is active");
    ok &= check(trace_result.options.transcript_capture_path == QStringLiteral("wheel.ndjson"),
        "wheel-trace keeps transcript capture path");
    ok &= check(!missing_capture_result.error.isEmpty(),
        "wheel-trace without transcript capture is rejected");
    ok &= check(!missing_capture_result.options.wheel_trace_enabled,
        "rejected wheel-trace does not remain enabled");
#else
    ok &= check(!trace_result.error.isEmpty(),
        "wheel-trace option is rejected when transcript capture is disabled");
    ok &= check(!trace_result.options.wheel_trace_enabled,
        "disabled transcript build does not enable wheel tracing");
#endif
    ok &= check(command_result.error.isEmpty(),
        "wheel-trace command argument parses after command separator");
    ok &= check(!command_result.options.wheel_trace_enabled,
        "wheel-trace after command separator remains a command argument");
    ok &= check(command_result.options.command == arguments({"--wheel-trace"}),
        "wheel-trace after command separator is preserved in command argv");
    return ok;
}

bool test_parse_synchronized_output_scroll_policy_option()
{
    using Policy = VNM_TerminalSurface::Synchronized_output_scroll_policy;

    bool ok = test_enum_option<Policy>(
        "--synchronized-output-scroll-policy",
        [](const chrome::App_options& options) {
            return options.synchronized_output_scroll_policy;
        },
        Policy::DEFER_UNTIL_CONTENT_PUBLICATION,
        {
            {"defer",             Policy::DEFER_UNTIL_CONTENT_PUBLICATION},
            {"immediate-public",  Policy::IMMEDIATE_PUBLIC_PROJECTION},
            {"ImMeDiAtE-PuBlIc",  Policy::IMMEDIATE_PUBLIC_PROJECTION},
        },
        "hidden-live",
        "--synchronized-output-scroll-policy supports only defer or immediate-public");

    chrome::Parse_result missing_value_result = parse({
        "vnm_terminal",
        "--synchronized-output-scroll-policy",
    });
    ok &= check(!missing_value_result.error.isEmpty(),
        "synchronized-output scroll policy rejects missing values");
    return ok;
}

bool test_parse_disable_primary_repaint_recovery_option()
{
    chrome::Parse_result default_result = parse({
        "vnm_terminal",
        "--",
        "fixture-command",
    });
    chrome::Parse_result disabled_result = parse({
        "vnm_terminal",
        "--disable-primary-repaint-recovery",
        "--",
        "fixture-command",
    });
    chrome::Parse_result command_result = parse({
        "vnm_terminal",
        "--",
        "--disable-primary-repaint-recovery",
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(),
        "primary repaint recovery default parse succeeds");
    ok &= check(!default_result.options.primary_repaint_recovery_enabled.has_value(),
        "primary repaint recovery default leaves surface platform policy unchanged");
    ok &= check(disabled_result.error.isEmpty(),
        "primary repaint recovery disable flag parses");
    ok &= check(
        disabled_result.options.primary_repaint_recovery_enabled.has_value() &&
            !*disabled_result.options.primary_repaint_recovery_enabled,
        "primary repaint recovery disable flag selects disabled policy");
    ok &= check(command_result.error.isEmpty(),
        "primary repaint recovery flag after command separator parses as command argv");
    ok &= check(command_result.options.command == arguments({"--disable-primary-repaint-recovery"}),
        "primary repaint recovery flag after command separator is preserved in command argv");
    return ok;
}

bool test_parse_text_renderer_option()
{
    using Mode = VNM_TerminalSurface::Text_renderer_mode;

    bool ok = test_enum_option<Mode>(
        "--text-renderer",
        [](const chrome::App_options& options) {
            return options.text_renderer_mode;
        },
        Mode::AUTO,
        {
            {"auto",   Mode::AUTO},
            {"AUTO",   Mode::AUTO},
            {"msdf",   Mode::MSDF},
            {"glyph",  Mode::GLYPH},
        },
        "invalid",
        "--text-renderer supports only auto, msdf, or glyph");

    chrome::Parse_result msdf_result = parse({
        "vnm_terminal",
        "--text-renderer",
        "msdf",
        "--",
        "fixture-command",
    });
    ok &= check(msdf_result.options.text_renderer_mode_explicit,
        "text renderer option is marked explicit");
    return ok;
}

bool test_parse_lcd_subpixel_option()
{
    using Order = VNM_TerminalSurface::Lcd_subpixel_order;

    bool ok = test_enum_option<Order>(
        "--lcd-subpixel",
        [](const chrome::App_options& options) {
            return options.lcd_subpixel_order;
        },
        Order::AUTO,
        {
            {"auto",  Order::AUTO},
            {"AUTO",  Order::AUTO},
            {"none",  Order::NONE},
            {"rgb",   Order::RGB},
            {"bgr",   Order::BGR},
            {"vrgb",  Order::VRGB},
            {"vbgr",  Order::VBGR},
        },
        "invalid",
        "--lcd-subpixel supports only auto, none, rgb, bgr, vrgb, or vbgr");

    chrome::Parse_result rgb_result = parse({
        "vnm_terminal",
        "--lcd-subpixel",
        "rgb",
        "--",
        "fixture-command",
    });
    ok &= check(rgb_result.options.lcd_subpixel_order_explicit,
        "LCD subpixel option is marked explicit");
    return ok;
}

bool test_parse_row_timestamps_option()
{
    bool ok = test_enum_option<bool>(
        "--row-timestamps",
        [](const chrome::App_options& options) {
            return options.row_timestamp_tooltip_enabled;
        },
        true,
        {
            {"on",   true},
            {"off",  false},
            {"OFF",  false},
        },
        "sometimes",
        "--row-timestamps supports only on or off");

    chrome::Parse_result off_result = parse({
        "vnm_terminal",
        "--row-timestamps",
        "off",
        "--",
        "fixture-command",
    });
    ok &= check(off_result.options.row_timestamp_tooltip_explicit,
        "row timestamps option is marked explicit");
    return ok;
}

bool test_parse_alternate_wheel_option()
{
    using Policy = VNM_TerminalSurface::Alternate_screen_wheel_policy;

    return test_enum_option<Policy>(
        "--alternate-wheel",
        [](const chrome::App_options& options) {
            return options.alternate_screen_wheel_policy;
        },
        Policy::MOUSE_REPORTING_FIRST,
        {
            {"mouse",   Policy::MOUSE_REPORTING_FIRST},
            {"MOUSE",   Policy::MOUSE_REPORTING_FIRST},
            {"page",    Policy::PAGE_KEYS},
            {"cursor",  Policy::CURSOR_KEYS},
        },
        "diagonal",
        "--alternate-wheel supports only page, cursor, or mouse");
}

bool test_parse_osc52_clipboard_option()
{
    return test_enum_option<chrome::Osc52_clipboard_policy>(
        "--osc52-clipboard",
        [](const chrome::App_options& options) {
            return options.osc52_clipboard_policy;
        },
        chrome::Osc52_clipboard_policy::DENY,
        {
            {"deny",   chrome::Osc52_clipboard_policy::DENY},
            {"DENY",   chrome::Osc52_clipboard_policy::DENY},
            {"allow",  chrome::Osc52_clipboard_policy::ALLOW},
        },
        "ask",
        "--osc52-clipboard supports only deny or allow");
}

bool test_parse_scrollback_limit_option()
{
    chrome::Parse_result default_result = parse({
        "vnm_terminal",
        "--",
        "fixture-command",
    });
    chrome::Parse_result limit_result = parse({
        "vnm_terminal",
        "--scrollback-limit",
        "200",
        "--",
        "fixture-command",
    });
    chrome::Parse_result zero_result = parse({
        "vnm_terminal",
        "--scrollback-limit",
        "0",
        "--",
        "fixture-command",
    });
    chrome::Parse_result invalid_result = parse({
        "vnm_terminal",
        "--scrollback-limit",
        "-1",
    });
    chrome::Parse_result command_result = parse({
        "vnm_terminal",
        "--",
        "--scrollback-limit",
        "200",
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(), "scrollback limit default parse succeeds");
    ok &= check(!default_result.options.scrollback_limit.has_value(),
        "scrollback limit default leaves surface policy unchanged");
    ok &= check(limit_result.error.isEmpty(), "scrollback limit option parses");
    ok &= check(
        limit_result.options.scrollback_limit.has_value() &&
            *limit_result.options.scrollback_limit == 200,
        "scrollback limit option selects requested row count");
    ok &= check(zero_result.error.isEmpty(), "scrollback limit accepts zero");
    ok &= check(
        zero_result.options.scrollback_limit.has_value() &&
            *zero_result.options.scrollback_limit == 0,
        "scrollback limit zero disables retained rows");
    ok &= check(!invalid_result.error.isEmpty(),
        "scrollback limit rejects negative values");
    ok &= check(command_result.error.isEmpty(),
        "scrollback limit after command separator parses as command argv");
    ok &= check(command_result.options.command == arguments({"--scrollback-limit", "200"}),
        "scrollback limit after command separator is preserved in command argv");
    return ok;
}

bool test_parse_metrics_timeline_options()
{
    chrome::Parse_result default_result = parse({
        "vnm_terminal",
        "--",
        "fixture-command",
    });
    chrome::Parse_result timeline_result = parse({
        "vnm_terminal",
        "--metrics-timeline-jsonl",
        "timeline.jsonl",
        "--",
        "fixture-command",
    });
    chrome::Parse_result interval_result = parse({
        "vnm_terminal",
        "--metrics-timeline-jsonl",
        "timeline.jsonl",
        "--metrics-timeline-interval-ms",
        "250",
        "--",
        "fixture-command",
    });
    chrome::Parse_result invalid_interval_result = parse({
        "vnm_terminal",
        "--metrics-timeline-interval-ms",
        "0",
    });
    chrome::Parse_result overflowing_interval_result = parse({
        "vnm_terminal",
        "--metrics-timeline-interval-ms",
        "2147483648",
    });
    chrome::Parse_result command_result = parse({
        "vnm_terminal",
        "--",
        "--metrics-timeline-jsonl",
        "timeline.jsonl",
    });

    bool ok = true;
    ok &= check(default_result.error.isEmpty(), "metrics timeline default parse succeeds");
    ok &= check(default_result.options.metrics_timeline_jsonl_path.isEmpty(),
        "metrics timeline path defaults empty");
    ok &= check(
        default_result.options.metrics_timeline_interval_ms ==
            chrome::k_default_metrics_timeline_interval_ms,
        "metrics timeline interval defaults to 5000 ms");
    ok &= check(timeline_result.error.isEmpty(), "metrics timeline path option parses");
    ok &= check(
        timeline_result.options.metrics_timeline_jsonl_path ==
            QStringLiteral("timeline.jsonl"),
        "metrics timeline path option stores the requested path");
    ok &= check(interval_result.error.isEmpty(),
        "metrics timeline interval option parses");
    ok &= check(interval_result.options.metrics_timeline_interval_ms == 250,
        "metrics timeline interval option stores the requested interval");
    ok &= check(!invalid_interval_result.error.isEmpty(),
        "metrics timeline interval rejects zero");
    ok &= check(
        invalid_interval_result.error ==
            QStringLiteral("--metrics-timeline-interval-ms requires a positive integer"),
        "metrics timeline interval reports the documented error");
    ok &= check(!overflowing_interval_result.error.isEmpty(),
        "metrics timeline interval rejects values beyond int32");
    ok &= check(command_result.error.isEmpty(),
        "metrics timeline option after command separator parses as command argv");
    ok &= check(command_result.options.command == arguments({
            "--metrics-timeline-jsonl",
            "timeline.jsonl",
        }),
        "metrics timeline option after command separator is preserved in command argv");

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "metrics timeline conflict test creates temp directory");
    if (temp_dir.isValid()) {
        const QString shared_path = temp_dir.filePath(QStringLiteral("metrics.json"));
        chrome::Parse_result conflict_result = chrome::parse_arguments({
            QStringLiteral("vnm_terminal"),
            QStringLiteral("--metrics-json"),
            shared_path,
            QStringLiteral("--metrics-timeline-jsonl"),
            shared_path,
            QStringLiteral("--"),
            QStringLiteral("fixture-command"),
        });

        QString validation_error;
        ok &= check(conflict_result.error.isEmpty(),
            "metrics timeline conflict input parses before path validation");
        ok &= check(!chrome::validate_capture_paths(&conflict_result.options, &validation_error),
            "metrics timeline rejects sharing the aggregate metrics path");
        ok &= check(
            validation_error.contains(
                QStringLiteral("--metrics-json and --metrics-timeline-jsonl")),
            "metrics timeline conflict reports the conflicting options");
    }
    return ok;
}

#if VNM_TERMINAL_PROFILING_ENABLED
bool test_profile_text_capture_path_conflicts()
{
    QTemporaryDir temp_dir;
    bool ok = check(temp_dir.isValid(),
        "profile-text conflict test creates temp directory");
    if (!temp_dir.isValid()) {
        return ok;
    }

    const QString shared_path = temp_dir.filePath(QStringLiteral("shared.txt"));
    chrome::Parse_result output_conflict_result = chrome::parse_arguments({
        QStringLiteral("vnm_terminal"),
        QStringLiteral("--profile-text"),
        shared_path,
        QStringLiteral("--capture-output"),
        shared_path,
        QStringLiteral("--"),
        QStringLiteral("fixture-command"),
    });
    QString validation_error;
    ok &= check(output_conflict_result.error.isEmpty(),
        "profile-text/capture-output conflict input parses before path validation");
    ok &= check(
        !chrome::validate_capture_paths(
            &output_conflict_result.options,
            &validation_error),
        "profile-text rejects sharing the capture-output path");
    ok &= check(
        validation_error.contains(
            QStringLiteral("--profile-text and --capture-output")),
        "profile-text/capture-output conflict reports the conflicting options");

    chrome::App_options transcript_conflict_options;
    transcript_conflict_options.profile_text_path = shared_path;
    transcript_conflict_options.transcript_capture_path = shared_path;
    validation_error.clear();
    ok &= check(
        !chrome::validate_capture_paths(
            &transcript_conflict_options,
            &validation_error),
        "profile-text rejects sharing the capture-transcript path");
    ok &= check(
        validation_error.contains(
            QStringLiteral("--profile-text and --capture-transcript")),
        "profile-text/capture-transcript conflict reports the conflicting options");

    return ok;
}
#endif

bool test_parse_transcript_snapshot_diagnostics_option()
{
    chrome::Parse_result snapshot_result = parse({
        "vnm_terminal",
        "--transcript-snapshot-diagnostics",
        "--capture-transcript",
        "snapshot.ndjson",
        "--",
        "fixture-command",
    });
    chrome::Parse_result missing_capture_result = parse({
        "vnm_terminal",
        "--transcript-snapshot-diagnostics",
        "--",
        "fixture-command",
    });
    chrome::Parse_result command_result = parse({
        "vnm_terminal",
        "--",
        "--transcript-snapshot-diagnostics",
    });

    bool ok = true;
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    ok &= check(snapshot_result.error.isEmpty(),
        "transcript-snapshot-diagnostics option parses with transcript capture");
    ok &= check(snapshot_result.options.transcript_snapshot_diagnostics,
        "transcript-snapshot-diagnostics option enables snapshot diagnostics");
    ok &= check(snapshot_result.options.transcript_capture_path == QStringLiteral("snapshot.ndjson"),
        "transcript-snapshot-diagnostics keeps transcript capture path");
    ok &= check(!missing_capture_result.error.isEmpty(),
        "transcript-snapshot-diagnostics without transcript capture is rejected");
    ok &= check(!missing_capture_result.options.transcript_snapshot_diagnostics,
        "rejected transcript-snapshot-diagnostics does not remain enabled");
#else
    ok &= check(!snapshot_result.error.isEmpty(),
        "transcript-snapshot-diagnostics option is rejected when transcript capture is disabled");
    ok &= check(!snapshot_result.options.transcript_snapshot_diagnostics,
        "disabled transcript build does not enable transcript snapshot diagnostics");
#endif
    ok &= check(command_result.error.isEmpty(),
        "transcript-snapshot-diagnostics command argument parses after command separator");
    ok &= check(command_result.options.command == arguments({"--transcript-snapshot-diagnostics"}),
        "transcript-snapshot-diagnostics after command separator is preserved in command argv");
    return ok;
}

bool test_parse_transcript_timing_diagnostics_option()
{
    chrome::Parse_result timing_result = parse({
        "vnm_terminal",
        "--transcript-timing-diagnostics",
        "--capture-transcript",
        "timing.ndjson",
        "--",
        "fixture-command",
    });
    chrome::Parse_result missing_capture_result = parse({
        "vnm_terminal",
        "--transcript-timing-diagnostics",
        "--",
        "fixture-command",
    });
    chrome::Parse_result command_result = parse({
        "vnm_terminal",
        "--",
        "--transcript-timing-diagnostics",
    });

    bool ok = true;
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    ok &= check(timing_result.error.isEmpty(),
        "transcript-timing-diagnostics option parses with transcript capture");
    ok &= check(timing_result.options.transcript_timing_diagnostics,
        "transcript-timing-diagnostics option enables timing diagnostics");
    ok &= check(timing_result.options.transcript_capture_path == QStringLiteral("timing.ndjson"),
        "transcript-timing-diagnostics keeps transcript capture path");
    ok &= check(!missing_capture_result.error.isEmpty(),
        "transcript-timing-diagnostics without transcript capture is rejected");
    ok &= check(!missing_capture_result.options.transcript_timing_diagnostics,
        "rejected transcript-timing-diagnostics does not remain enabled");
#else
    ok &= check(!timing_result.error.isEmpty(),
        "transcript-timing-diagnostics option is rejected when transcript capture is disabled");
    ok &= check(!timing_result.options.transcript_timing_diagnostics,
        "disabled transcript build does not enable transcript timing diagnostics");
#endif
    ok &= check(command_result.error.isEmpty(),
        "transcript-timing-diagnostics command argument parses after command separator");
    ok &= check(command_result.options.command == arguments({"--transcript-timing-diagnostics"}),
        "transcript-timing-diagnostics after command separator is preserved in command argv");
    return ok;
}

bool test_parse_paste_shortcut_option()
{
    bool ok = test_enum_option<chrome::Paste_shortcut_policy>(
        "--paste-shortcut",
        [](const chrome::App_options& options) {
            return options.paste_shortcut_policy;
        },
        chrome::Paste_shortcut_policy::PLATFORM_DEFAULT,
        {
            {"platform-default",         chrome::Paste_shortcut_policy::PLATFORM_DEFAULT},
            {"PLATFORM-DEFAULT",         chrome::Paste_shortcut_policy::PLATFORM_DEFAULT},
            {"disabled",                 chrome::Paste_shortcut_policy::DISABLED},
            {"ctrl-shift-v",             chrome::Paste_shortcut_policy::CTRL_SHIFT_V},
            {"ctrl-v-and-ctrl-shift-v",
                chrome::Paste_shortcut_policy::CTRL_V_AND_CTRL_SHIFT_V},
        },
        "bogus",
        "--paste-shortcut supports only disabled, ctrl-shift-v, "
        "ctrl-v-and-ctrl-shift-v, or platform-default");

    chrome::Parse_result missing_value_result = parse({
        "vnm_terminal",
        "--paste-shortcut",
    });
    ok &= check(!missing_value_result.error.isEmpty(),
        "paste shortcut option rejects missing values");
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    bool ok = true;
    ok &= test_parse_titlebar_options();
    ok &= test_parse_selection_trace_option();
    ok &= test_parse_wheel_trace_option();
    ok &= test_parse_synchronized_output_scroll_policy_option();
    ok &= test_parse_disable_primary_repaint_recovery_option();
    ok &= test_parse_text_renderer_option();
    ok &= test_parse_lcd_subpixel_option();
    ok &= test_parse_row_timestamps_option();
    ok &= test_parse_alternate_wheel_option();
    ok &= test_parse_osc52_clipboard_option();
    ok &= test_parse_scrollback_limit_option();
    ok &= test_parse_metrics_timeline_options();
#if VNM_TERMINAL_PROFILING_ENABLED
    ok &= test_profile_text_capture_path_conflicts();
#endif
    ok &= test_parse_transcript_snapshot_diagnostics_option();
    ok &= test_parse_transcript_timing_diagnostics_option();
    ok &= test_parse_paste_shortcut_option();
    return ok ? 0 : 1;
}
