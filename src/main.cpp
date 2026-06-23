#include "app_cli.h"
#include "app_clipboard_reader.h"
#include "app_clipboard_policy.h"
#include "app_common.h"
#include "app_metrics.h"
#include "app_options.h"
#include "app_profile_text.h"
#include "app_settings.h"
#include "app_shortcuts.h"
#include "qml_chrome.h"
#include "terminal_scrollbar.h"
#include "terminal_settings_controller.h"
#include "terminal_settings_window.h"
#include "terminal_window.h"

#include "vnm_terminal/vnm_terminal_surface.h"

// Privileged first-party use of surface internal headers for profiler wiring
// (render-profiler attachment + the app's own GUI-thread profiler; profiling
// builds only). vnm_terminal builds the surface in-tree and is a documented
// privileged consumer -- see vnm_terminal_surface docs/public_surface.md,
// "Internal Headers And Privileged First-Party Consumers". Installed embedders
// cannot include these headers.
#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QGuiApplication>
#include <QIcon>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QRect>
#include <QScreen>
#include <QSettings>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWindow>
#include <QtGlobal>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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
using chrome::apply_persisted_appearance_settings;
using chrome::apply_persisted_terminal_window_state;
using chrome::apply_primary_repaint_recovery_option;
using chrome::apply_scrollback_limit_option;
using chrome::apply_synchronized_output_scroll_policy_option;
using chrome::apply_terminal_shell_geometry;
using chrome::clipboard_broker_mode_requested;
using chrome::connect_presentation_metrics_recorder;
using chrome::connect_row_timestamp_tooltip_to_chrome;
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
using chrome::k_appearance_color_scheme;
using chrome::k_appearance_font_family;
using chrome::k_appearance_lcd_subpixel_order;
using chrome::k_appearance_row_timestamp_tooltip;
using chrome::k_appearance_scrollback_limit;
using chrome::k_appearance_settings_group;
using chrome::k_appearance_text_renderer_mode;
using chrome::k_window_settings_font_size;
using chrome::k_window_settings_group;
using chrome::k_window_settings_height;
using chrome::k_window_settings_maximized;
using chrome::k_window_settings_width;
using chrome::k_window_settings_x;
using chrome::k_window_settings_y;
using chrome::load_persisted_appearance_settings;
using chrome::load_persisted_terminal_window_state;
using chrome::Metrics_timeline_jsonl_writer;
using chrome::Metrics_timeline_sample_kind;
using chrome::metrics_timing_t;
using chrome::Osc52_clipboard_policy;
using chrome::Persisted_appearance_settings;
using chrome::Persisted_terminal_window_state;
using chrome::persisted_window_axis_is_valid;
using chrome::Presentation_metrics_recorder;
#if VNM_TERMINAL_PROFILING_ENABLED
using chrome::prepare_profile_text_file;
#endif
using chrome::print_error;
using chrome::read_clipboard_text_with_broker;
using chrome::resize_window_for_text_area_request;
using chrome::restorable_terminal_window_state;
using chrome::save_persisted_appearance_settings;
using chrome::Runtime_state;
using chrome::save_persisted_terminal_window_state;
using chrome::settings_font_size;
using chrome::settings_int_value;
using chrome::settings_window_position;
using chrome::settings_window_size;
using chrome::split_terminal_area;
using chrome::sync_chrome_window_state;
using chrome::sync_terminal_title;
using chrome::Terminal_shortcut_filter;
using chrome::terminal_window_persistence_enabled;
using chrome::visible_terminal_title;
using chrome::Wheel_delivery_indicator_filter;
using chrome::window_geometry_intersects_available_screen;
using chrome::write_metrics_json;
#if VNM_TERMINAL_PROFILING_ENABLED
using chrome::write_profile_text;
#endif

using chrome::parse_arguments;
using chrome::Parse_result;
using chrome::prepare_capture_file;
using chrome::print_usage;
using chrome::validate_capture_paths;

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

void request_terminal_bell()
{
#if defined(Q_OS_WIN)
    (void)MessageBeep(MB_OK);
#endif
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

struct Terminal_window_chrome_setup
{
    std::unique_ptr<chrome::Terminal_qml_chrome> titlebar;
    QString                                     error;
};

Terminal_window_chrome_setup setup_terminal_window_chrome(
    QQmlEngine&        chrome_engine,
    QQuickWindow&      window,
    const QIcon&       app_icon,
    const App_options& options)
{
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

    Terminal_window_chrome_setup setup;
    if (options.custom_titlebar) {
        setup.titlebar = std::make_unique<chrome::Terminal_qml_chrome>(
            chrome_engine,
            window);
        if (!setup.titlebar->is_valid()) {
            setup.error = QStringLiteral("failed to create shared window chrome: %1")
                .arg(setup.titlebar->error_string());
            setup.titlebar.reset();
        }
    }

    return setup;
}

}

#ifndef VNM_TERMINAL_APP_NO_MAIN
int main(int argc, char** argv)
{
    const QStringList arguments = raw_arguments(argc, argv);
    if (clipboard_broker_mode_requested(arguments)) {
        return chrome::run_clipboard_text_broker(argc, argv);
    }

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
    if (!options.metrics_timeline_jsonl_path.isEmpty() &&
        !prepare_capture_file(
            QStringLiteral("--metrics-timeline-jsonl"),
            options.metrics_timeline_jsonl_path, &parse_result.error))
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

    Metrics_timeline_jsonl_writer metrics_timeline_writer;
    if (!options.metrics_timeline_jsonl_path.isEmpty() &&
        !metrics_timeline_writer.open(options.metrics_timeline_jsonl_path, &parse_result.error))
    {
        print_error(parse_result.error);
        return k_exit_usage_error;
    }

    const bool persistence_enabled = terminal_window_persistence_enabled();
    if (persistence_enabled) {
        QSettings settings;
        apply_persisted_terminal_window_state(
            load_persisted_terminal_window_state(settings),
            &options);
        apply_persisted_appearance_settings(
            load_persisted_appearance_settings(settings),
            &options);
    }

    QQmlEngine chrome_engine;
    Presentation_metrics_recorder presentation_metrics;
    QQuickWindow window;
    const bool presentation_metrics_requested =
        !options.metrics_json_path.isEmpty() ||
        !options.metrics_timeline_jsonl_path.isEmpty();
    if (presentation_metrics_requested) {
        connect_presentation_metrics_recorder(window, presentation_metrics);
    }
    Terminal_window_chrome_setup chrome_setup =
        setup_terminal_window_chrome(chrome_engine, window, app_icon, options);
    if (!chrome_setup.error.isEmpty()) {
        print_error(chrome_setup.error);
        return k_exit_start_failed;
    }
    std::unique_ptr<chrome::Terminal_qml_chrome> titlebar =
        std::move(chrome_setup.titlebar);
    auto* titlebar_ptr = titlebar.get();

    auto* surface = new VNM_TerminalSurface(window.contentItem());
    surface->set_clipboard_text_reader(read_clipboard_text_with_broker);
    surface->set_selection_trace_enabled(options.selection_trace_enabled);
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
        surface->set_dirty_row_stats_enabled(true);
    }
#endif
    auto* scrollbar = new chrome::Terminal_scrollbar(window.contentItem());
    scrollbar->set_surface(surface);
    scrollbar->set_wheel_trace_enabled(options.wheel_trace_enabled);
    surface->set_font_family(options.font_family);
    surface->set_font_size(options.font_size);
    surface->set_color_scheme(options.color_scheme);
    surface->set_wheel_event_policy(
        VNM_TerminalSurface::Wheel_event_policy::LOCAL_SCROLLBACK_FIRST);
    apply_scrollback_limit_option(*surface, options);
#if defined(Q_OS_MACOS)
    surface->set_copy_shortcut_policy(VNM_TerminalSurface::Copy_shortcut_policy::TERMINAL_INPUT);
#endif
    surface->set_alternate_screen_wheel_policy(options.alternate_screen_wheel_policy);
    surface->set_text_renderer_mode(options.text_renderer_mode);
    surface->set_lcd_subpixel_order(options.lcd_subpixel_order);
    surface->set_row_timestamp_tooltip_enabled(options.row_timestamp_tooltip_enabled);
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
    const auto persist_window_state =
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
            if (current_state.has_value()) {
                latest_restorable_window_state = *current_state;
            }

            Persisted_terminal_window_state state =
                current_state.value_or(
                    latest_restorable_window_state.value_or(
                        Persisted_terminal_window_state{}));
            state.font_size = surface->font_size();
            state.maximized = window.windowStates().testFlag(Qt::WindowMaximized);

            QSettings settings;
            save_persisted_terminal_window_state(settings, state);
        };
    const auto persist_appearance = [persistence_enabled, surface] {
        if (!persistence_enabled) {
            return;
        }

        QSettings settings;
        save_persisted_appearance_settings(settings, *surface);
    };

    apply_terminal_shell_geometry(
        window,
        *surface,
        *scrollbar,
        titlebar_ptr,
        custom_titlebar_enabled);
    auto* shortcut_filter =
        new Terminal_shortcut_filter(surface, options.paste_shortcut_policy);
    window.installEventFilter(shortcut_filter);

    auto settings_controller =
        std::make_unique<chrome::Terminal_settings_controller>();
    auto settings_window = std::make_unique<chrome::Terminal_settings_window>(
        chrome_engine,
        *surface,
        *settings_controller);
    if (!settings_window->is_valid()) {
        print_error(QStringLiteral("failed to create settings window: %1")
            .arg(settings_window->error_string()));
        settings_window.reset();
        settings_controller.reset();
    }
    else {
        settings_window->set_transient_parent(&window);
        // Settings are reachable everywhere via Ctrl+, / Cmd+, and, when the
        // built-in chrome is active, via the titlebar gear button.
        QObject::connect(
            shortcut_filter,
            &Terminal_shortcut_filter::settings_requested,
            settings_window.get(),
            &chrome::Terminal_settings_window::show_window);
        if (titlebar_ptr != nullptr) {
            QObject::connect(
                titlebar_ptr,
                &chrome::Terminal_qml_chrome::settings_requested,
                settings_window.get(),
                &chrome::Terminal_settings_window::show_window);
        }
    }

    connect_terminal_metadata_to_chrome(*surface, window, titlebar_ptr);
    connect_row_timestamp_tooltip_to_chrome(*surface, titlebar_ptr);
    QObject::connect(
        surface,
        &VNM_TerminalSurface::bell_requested,
        &app,
        [] {
            request_terminal_bell();
        });
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
            persist_window_state
        ] {
            apply_terminal_shell_geometry(
                window,
                *surface,
                *scrollbar,
                titlebar_ptr,
                custom_titlebar_enabled);
            persist_window_state();
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
            persist_window_state
        ] {
            apply_terminal_shell_geometry(
                window,
                *surface,
                *scrollbar,
                titlebar_ptr,
                custom_titlebar_enabled);
            persist_window_state();
        });
    QObject::connect(
        &window,
        &QWindow::xChanged,
        surface,
        [persist_window_state](int) {
            persist_window_state();
        });
    QObject::connect(
        &window,
        &QWindow::yChanged,
        surface,
        [persist_window_state](int) {
            persist_window_state();
        });
    QObject::connect(
        &window,
        &QWindow::screenChanged,
        surface,
        [
            titlebar_ptr,
            &window,
            surface,
            scrollbar,
            custom_titlebar_enabled,
            persist_window_state
        ](QScreen*) {
            if (titlebar_ptr != nullptr) {
                sync_chrome_window_state(*titlebar_ptr, window);
            }
            apply_terminal_shell_geometry(
                window,
                *surface,
                *scrollbar,
                titlebar_ptr,
                custom_titlebar_enabled);
            persist_window_state();
        });
    QObject::connect(
        surface,
        &VNM_TerminalSurface::font_size_changed,
        surface,
        persist_window_state);
    QObject::connect(
        surface,
        &VNM_TerminalSurface::color_scheme_changed,
        surface,
        persist_appearance);
    QObject::connect(
        surface,
        &VNM_TerminalSurface::font_family_changed,
        surface,
        persist_appearance);
    QObject::connect(
        surface,
        &VNM_TerminalSurface::text_renderer_mode_changed,
        surface,
        persist_appearance);
    QObject::connect(
        surface,
        &VNM_TerminalSurface::lcd_subpixel_order_changed,
        surface,
        persist_appearance);
    QObject::connect(
        surface,
        &VNM_TerminalSurface::row_timestamp_tooltip_enabled_changed,
        surface,
        persist_appearance);
    QObject::connect(
        surface,
        &VNM_TerminalSurface::scrollback_limit_changed,
        surface,
        persist_appearance);

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
                persist_window_state
            ] {
                sync_chrome_window_state(*titlebar_ptr, window);
                apply_terminal_shell_geometry(
                    window,
                    *surface,
                    *scrollbar,
                    titlebar_ptr,
                    custom_titlebar_enabled);
                persist_window_state();
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
            [persist_window_state](Qt::WindowState) {
                persist_window_state();
            });
    }

    QObject::connect(
        &app,
        &QCoreApplication::aboutToQuit,
        surface,
        [persist_window_state] {
            persist_window_state();
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
    QTimer metrics_timeline_timer(&app);
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

    QElapsedTimer app_elapsed_timer;
    bool metrics_timeline_error_seen = false;
    if (!options.metrics_timeline_jsonl_path.isEmpty()) {
        metrics_timeline_timer.setInterval(options.metrics_timeline_interval_ms);
        QObject::connect(
            &metrics_timeline_timer,
            &QTimer::timeout,
            &app,
            [
                &app_elapsed_timer,
                &metrics_timeline_error_seen,
                &metrics_timeline_timer,
                &metrics_timeline_writer,
                &options,
                &presentation_metrics,
                &state,
                surface
            ] {
                metrics_timing_t sample_timing;
                sample_timing.app_elapsed_ms         = app_elapsed_timer.elapsed();
                sample_timing.profile_text_requested = !options.profile_text_path.isEmpty();

                QString metrics_error;
                if (!metrics_timeline_writer.write_sample(
                        Metrics_timeline_sample_kind::PERIODIC,
                        *surface,
                        presentation_metrics,
                        state,
                        sample_timing,
                        std::nullopt,
                        options.metrics_timeline_interval_ms,
                        &metrics_error))
                {
                    metrics_timeline_error_seen = true;
                    metrics_timeline_timer.stop();
                    print_error(metrics_error);
                }
            });
    }

    window.show();
    if (options.restore_maximized_window_state) {
        window.setWindowState(Qt::WindowMaximized);
    }
    if (titlebar_ptr != nullptr) {
        sync_chrome_window_state(*titlebar_ptr, window);
    }
    apply_terminal_shell_geometry(
        window,
        *surface,
        *scrollbar,
        titlebar_ptr,
        custom_titlebar_enabled);
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

    app_elapsed_timer.start();
    if (!options.metrics_timeline_jsonl_path.isEmpty()) {
        metrics_timeline_timer.start();
    }
    int app_result = app.exec();
    metrics_timeline_timer.stop();
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

    if (metrics_timeline_error_seen && app_result == 0) {
        app_result = k_exit_usage_error;
    }

    if (!options.metrics_json_path.isEmpty()) {
        QString metrics_error;
        if (!write_metrics_json(
                options.metrics_json_path,
                *surface,
                presentation_metrics,
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

    if (!options.metrics_timeline_jsonl_path.isEmpty()) {
        QString metrics_error;
        if (!metrics_timeline_writer.write_sample(
                Metrics_timeline_sample_kind::FINAL,
                *surface,
                presentation_metrics,
                state,
                metrics_timing,
                app_result,
                options.metrics_timeline_interval_ms,
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
