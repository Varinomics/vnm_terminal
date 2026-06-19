#include "app_metrics.h"
#include "app_presentation_metrics.h"

#include "vnm_terminal/internal/backend_contract.h"
#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QQuickWindow>
#include <QScreen>
#include <QSGRendererInterface>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace term   = vnm_terminal::internal;
namespace chrome = vnm_terminal::terminal_app;

namespace {

enum class Evidence_mode
{
    OFFSCREEN_CI,
    REAL_WINDOW_PRESENTATION,
};

struct Probe_options
{
    QString metrics_timeline_jsonl_path =
        QDir::temp().filePath(QStringLiteral("vnm_terminal_input_echo_timing_metrics.jsonl"));
    QString evidence_jsonl_path =
        QDir::temp().filePath(QStringLiteral("vnm_terminal_input_echo_timing_evidence.jsonl"));
    int           metrics_timeline_interval_ms  = 50;
    int           burst_count                   = 5;
    int           burst_length                  = 18;
    int           burst_spacing_ms              = 75;
    int           timeout_ms                    = 2500;
    int           max_echo_ms                   = 750;
    int           max_frame_ms                  = 1500;
    int           echo_delay_ms                 = 2;
    QSize         window_size                   = QSize(800, 480);
    qreal         font_size                     = 10.0;
    Evidence_mode evidence_mode                 = Evidence_mode::OFFSCREEN_CI;
    bool          allow_missing_presentation_frame = false;
    bool          allow_missing_rendered_snapshot  = false;
    bool          enforce_latency_limits           = false;
};

struct Parse_result
{
    Probe_options options;
    QString       error;
    bool          help_requested = false;
};

struct Write_observation
{
    int        index      = 0;
    qint64     elapsed_ms = 0;
    QByteArray bytes;
};

struct Backend_output_observation
{
    int        index      = 0;
    qint64     elapsed_ms = 0;
    QByteArray bytes;
};

struct Evidence_requirements
{
    bool require_qsg_capture        = true;
    bool require_rendered_snapshot  = false;
    bool require_presentation_frame = false;
};

struct Drain_delta
{
    std::uint64_t total_calls               = 0U;
    std::uint64_t posted_calls              = 0U;
    std::uint64_t frame_pending_calls       = 0U;
    std::uint64_t total_elapsed_ns          = 0U;
    std::uint64_t session_processing_calls  = 0U;
    std::uint64_t sync_from_session_calls   = 0U;
    std::uint64_t pending_after_drain       = 0U;
    std::uint64_t backpressure_after_drain  = 0U;
};

struct Presentation_counts
{
    std::uint64_t frame_swapped = 0U;
};

struct Render_counts
{
    std::uint64_t paint_completed                       = 0U;
    std::uint64_t qsg_atlas_render                      = 0U;
    std::uint64_t qsg_atlas_capture                     = 0U;
    std::uint64_t qsg_captured_snapshot                 = 0U;
    std::uint64_t qsg_rendered_snapshot                 = 0U;
    std::uint64_t last_rendered_snapshot                = 0U;
    std::uint64_t backend_callback_frame_deferrals      = 0U;
    std::uint64_t backend_callback_event_epoch          = 0U;
    std::uint64_t backend_callback_frame_boundary_epoch = 0U;
    bool          qsg_drew                              = false;
    bool          render_target_non_null                = false;
    bool          rhi_non_null                          = false;
};

struct Presentation_environment
{
    QString qpa_platform;
    QString qt_qpa_platform_env;
    QString qsg_render_loop_env;
    QString qsg_rhi_backend_env;
    QString qt_quick_backend_env;
    QString scene_graph_backend;
    QString static_graphics_api;
    int     static_graphics_api_value        = 0;
    bool    static_graphics_api_rhi_based    = false;
    bool    static_graphics_api_software_or_null = false;
    QString graphics_api;
    int     graphics_api_value               = 0;
    bool    graphics_api_rhi_based           = false;
    bool    graphics_api_software_or_null    = false;
    bool    renderer_interface_non_null      = false;
    bool    software_or_null_env_requested   = false;
    bool    window_visible                   = false;
    bool    window_exposed                   = false;
    bool    window_minimized                 = false;
    QString window_visibility;
    QString window_state;
    int     window_width                     = 0;
    int     window_height                    = 0;
    qreal   device_pixel_ratio               = 0.0;
    bool    screen_non_null                  = false;
    QString screen_name;
    int     screen_width                     = 0;
    int     screen_height                    = 0;
    bool    qsg_drew                         = false;
    bool    render_target_non_null           = false;
    bool    rhi_non_null                     = false;
    bool    qpa_platform_real_window_valid   = false;
    bool    window_state_real_window_valid   = false;
    bool    renderer_real_window_valid       = false;
    bool    real_window_claim_valid          = false;
};

struct Snapshot_match
{
    std::uint64_t sequence      = 0U;
    int           row           = -1;
    int           column        = -1;
    int           payload_start = -1;
    QString       row_text;
    bool          cursor_fresh  = false;
};

struct Frame_evidence
{
    bool strict_rendered    = false;
    bool qsg_captured       = false;
    bool presentation_frame = false;
};

void print_usage()
{
    std::cout
        << "usage: vnm_terminal_input_echo_timing_probe [options]\n"
        << "\n"
        << "options:\n"
        << "  --metrics-timeline-jsonl <path>  app runtime metrics timeline output\n"
        << "  --evidence-jsonl <path>          per-burst input echo timing output\n"
        << "  --metrics-timeline-interval-ms <n>  metrics sample interval, default 50\n"
        << "  --burst-count <n>                key bursts to send, default 5\n"
        << "  --burst-length <n>               printable chars per burst, default 18\n"
        << "  --burst-spacing-ms <n>           settle time between bursts, default 75\n"
        << "  --timeout-ms <n>                 per-stage wait timeout, default 2500\n"
        << "  --max-echo-ms <n>                max input-to-fresh-cursor latency, default 750\n"
        << "  --max-frame-ms <n>               max input-to-rendered-frame latency, default 1500\n"
        << "  --enforce-latency-limits         fail when max echo/frame limits are exceeded\n"
        << "  --echo-delay-ms <n>              deterministic backend echo delay, default 2\n"
        << "  --window-size <width>x<height>   Qt Quick window size, default 800x480\n"
        << "  --evidence-mode <mode>           offscreen-ci(default) or real-window-presentation\n"
        << "  --allow-missing-presentation-frame\n"
        << "                                  allow offscreen runs without frameSwapped\n"
        << "  --allow-missing-rendered-snapshot\n"
        << "                                  allow offscreen QSG capture without draw completion\n"
        << "  --help                          show this help\n";
}

QString evidence_mode_key(Evidence_mode mode)
{
    switch (mode) {
        case Evidence_mode::OFFSCREEN_CI:
            return QStringLiteral("offscreen-ci");
        case Evidence_mode::REAL_WINDOW_PRESENTATION:
            return QStringLiteral("real-window-presentation");
    }

    return QStringLiteral("offscreen-ci");
}

std::optional<Evidence_mode> parse_evidence_mode(const QString& text)
{
    if (text == QStringLiteral("offscreen-ci")) {
        return Evidence_mode::OFFSCREEN_CI;
    }
    if (text == QStringLiteral("real-window-presentation")) {
        return Evidence_mode::REAL_WINDOW_PRESENTATION;
    }

    return std::nullopt;
}

Evidence_requirements evidence_requirements(const Probe_options& options)
{
    Evidence_requirements requirements;
    if (options.evidence_mode == Evidence_mode::REAL_WINDOW_PRESENTATION) {
        requirements.require_rendered_snapshot  = true;
        requirements.require_presentation_frame = true;
    }

    if (options.allow_missing_rendered_snapshot) {
        requirements.require_rendered_snapshot = false;
    }
    if (options.allow_missing_presentation_frame) {
        requirements.require_presentation_frame = false;
    }

    return requirements;
}

bool parse_positive_int(const QString& text, int* out_value)
{
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok || value <= 0) {
        return false;
    }

    *out_value = value;
    return true;
}

std::optional<QSize> parse_window_size(const QString& text)
{
    const qsizetype separator = text.indexOf(QChar(u'x'));
    if (separator <= 0 || separator >= text.size() - 1) {
        return std::nullopt;
    }

    int width  = 0;
    int height = 0;
    if (!parse_positive_int(text.left(separator), &width) ||
        !parse_positive_int(text.mid(separator + 1), &height))
    {
        return std::nullopt;
    }

    return QSize(width, height);
}

bool take_option_value(
    const QStringList& arguments,
    int*               index,
    QString*           out_value,
    QString*           out_error)
{
    if (*index + 1 >= arguments.size()) {
        *out_error = QStringLiteral("%1 requires a value").arg(arguments[*index]);
        return false;
    }

    *out_value = arguments[*index + 1];
    *index += 2;
    return true;
}

Parse_result parse_arguments(const QStringList& arguments)
{
    Parse_result result;

    int index = 1;
    while (index < arguments.size()) {
        const QString argument = arguments[index];
        if (argument == QStringLiteral("--help") || argument == QStringLiteral("-h")) {
            result.help_requested = true;
            return result;
        }
        if (argument == QStringLiteral("--allow-missing-presentation-frame")) {
            result.options.allow_missing_presentation_frame = true;
            ++index;
            continue;
        }
        if (argument == QStringLiteral("--allow-missing-rendered-snapshot")) {
            result.options.allow_missing_rendered_snapshot = true;
            ++index;
            continue;
        }
        if (argument == QStringLiteral("--enforce-latency-limits")) {
            result.options.enforce_latency_limits = true;
            ++index;
            continue;
        }

        QString value;
        if (argument == QStringLiteral("--metrics-timeline-jsonl")) {
            if (!take_option_value(arguments, &index, &value, &result.error)) {
                return result;
            }
            result.options.metrics_timeline_jsonl_path = value;
            continue;
        }
        if (argument == QStringLiteral("--evidence-jsonl")) {
            if (!take_option_value(arguments, &index, &value, &result.error)) {
                return result;
            }
            result.options.evidence_jsonl_path = value;
            continue;
        }
        if (argument == QStringLiteral("--window-size")) {
            if (!take_option_value(arguments, &index, &value, &result.error)) {
                return result;
            }
            const std::optional<QSize> size = parse_window_size(value);
            if (!size.has_value()) {
                result.error = QStringLiteral(
                    "--window-size requires <positive-width>x<positive-height>");
                return result;
            }
            result.options.window_size = *size;
            continue;
        }
        if (argument == QStringLiteral("--evidence-mode")) {
            if (!take_option_value(arguments, &index, &value, &result.error)) {
                return result;
            }
            const std::optional<Evidence_mode> mode = parse_evidence_mode(value);
            if (!mode.has_value()) {
                result.error = QStringLiteral(
                    "--evidence-mode requires offscreen-ci or real-window-presentation");
                return result;
            }
            result.options.evidence_mode = *mode;
            continue;
        }

        int* numeric_target = nullptr;
        if (argument == QStringLiteral("--metrics-timeline-interval-ms")) {
            numeric_target = &result.options.metrics_timeline_interval_ms;
        }
        else
        if (argument == QStringLiteral("--burst-count")) {
            numeric_target = &result.options.burst_count;
        }
        else
        if (argument == QStringLiteral("--burst-length")) {
            numeric_target = &result.options.burst_length;
        }
        else
        if (argument == QStringLiteral("--burst-spacing-ms")) {
            numeric_target = &result.options.burst_spacing_ms;
        }
        else
        if (argument == QStringLiteral("--timeout-ms")) {
            numeric_target = &result.options.timeout_ms;
        }
        else
        if (argument == QStringLiteral("--max-echo-ms")) {
            numeric_target = &result.options.max_echo_ms;
        }
        else
        if (argument == QStringLiteral("--max-frame-ms")) {
            numeric_target = &result.options.max_frame_ms;
        }
        else
        if (argument == QStringLiteral("--echo-delay-ms")) {
            numeric_target = &result.options.echo_delay_ms;
        }

        if (numeric_target != nullptr) {
            if (!take_option_value(arguments, &index, &value, &result.error)) {
                return result;
            }
            if (!parse_positive_int(value, numeric_target)) {
                result.error = QStringLiteral("%1 requires a positive integer").arg(argument);
                return result;
            }
            continue;
        }

        result.error = QStringLiteral("unexpected argument '%1'").arg(argument);
        return result;
    }

    return result;
}

bool ensure_parent_directory(const QString& path, QString* out_error)
{
    const QFileInfo info(path);
    const QDir parent = info.absoluteDir();
    if (!parent.exists()) {
        *out_error = QStringLiteral("parent directory does not exist: %1")
            .arg(parent.absolutePath());
        return false;
    }

    return true;
}

std::uint64_t counter_delta(std::uint64_t before, std::uint64_t after)
{
    return after >= before ? after - before : 0U;
}

Drain_delta drain_delta(
    const term::Terminal_surface_backend_drain_stats_t& before,
    const term::Terminal_surface_backend_drain_stats_t& after)
{
    return {
        counter_delta(before.total_drain_calls, after.total_drain_calls),
        counter_delta(before.posted_drain_calls, after.posted_drain_calls),
        counter_delta(
            before.posted_frame_pending_small_budget_calls,
            after.posted_frame_pending_small_budget_calls),
        counter_delta(before.total_elapsed_ns, after.total_elapsed_ns),
        counter_delta(before.session_processing_calls, after.session_processing_calls),
        counter_delta(before.sync_from_session_calls, after.sync_from_session_calls),
        counter_delta(before.pending_callback_after_drain, after.pending_callback_after_drain),
        counter_delta(
            before.output_backpressure_after_drain,
            after.output_backpressure_after_drain),
    };
}

Presentation_counts presentation_counts(const chrome::Presentation_metrics_recorder& recorder)
{
    const chrome::presentation_metrics_snapshot_t snapshot = recorder.snapshot();
    return {
        snapshot.stages[static_cast<std::size_t>(chrome::Presentation_signal::FRAME_SWAPPED)].count,
    };
}

Render_counts render_counts(const VNM_TerminalSurface& surface)
{
    const term::Terminal_surface_render_invalidation_stats_t invalidation =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    const term::Qsg_atlas_frame_report qsg_atlas =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    return {
        static_cast<std::uint64_t>(surface.paint_completed_frame_count()),
        static_cast<std::uint64_t>(surface.qsg_atlas_render_frame_count()),
        qsg_atlas.capture_count,
        qsg_atlas.captured_snapshot_sequence,
        qsg_atlas.render_snapshot_sequence,
        invalidation.last_rendered_snapshot_sequence,
        invalidation.backend_callback_frame_deferrals,
        invalidation.backend_callback_event_epoch,
        invalidation.backend_callback_frame_boundary_epoch,
        qsg_atlas.drew,
        qsg_atlas.render_target_non_null,
        qsg_atlas.rhi_non_null,
    };
}

QString environment_value_or_unset(const char* name)
{
    if (!qEnvironmentVariableIsSet(name)) {
        return QStringLiteral("<unset>");
    }

    return qEnvironmentVariable(name);
}

bool environment_backend_requests_software_or_null(const QString& value)
{
    const QString lower = value.toLower();
    return lower.contains(QStringLiteral("software")) ||
        lower.contains(QStringLiteral("null"));
}

bool qpa_platform_is_headless(const QString& platform)
{
    const QString lower = platform.toLower();
    return lower.startsWith(QStringLiteral("offscreen")) ||
        lower.startsWith(QStringLiteral("minimal"))      ||
        lower.startsWith(QStringLiteral("minimalegl"))   ||
        lower.startsWith(QStringLiteral("vnc"));
}

bool qpa_env_requests_headless_platform(const QString& value)
{
    const QString lower = value.toLower();
    return qpa_platform_is_headless(lower);
}

QString graphics_api_key(QSGRendererInterface::GraphicsApi api)
{
    switch (api) {
        case QSGRendererInterface::Unknown:
            return QStringLiteral("unknown");
        case QSGRendererInterface::Software:
            return QStringLiteral("software");
        case QSGRendererInterface::OpenVG:
            return QStringLiteral("openvg");
        case QSGRendererInterface::OpenGL:
            return QStringLiteral("opengl");
        case QSGRendererInterface::Direct3D11:
            return QStringLiteral("direct3d11");
        case QSGRendererInterface::Vulkan:
            return QStringLiteral("vulkan");
        case QSGRendererInterface::Metal:
            return QStringLiteral("metal");
        case QSGRendererInterface::Null:
            return QStringLiteral("null");
        case QSGRendererInterface::Direct3D12:
            return QStringLiteral("direct3d12");
    }

    return QStringLiteral("unknown");
}

QString window_visibility_key(QWindow::Visibility visibility)
{
    switch (visibility) {
        case QWindow::Hidden:
            return QStringLiteral("hidden");
        case QWindow::AutomaticVisibility:
            return QStringLiteral("automatic");
        case QWindow::Windowed:
            return QStringLiteral("windowed");
        case QWindow::Minimized:
            return QStringLiteral("minimized");
        case QWindow::Maximized:
            return QStringLiteral("maximized");
        case QWindow::FullScreen:
            return QStringLiteral("fullscreen");
    }

    return QStringLiteral("unknown");
}

QString window_state_key(Qt::WindowStates states)
{
    QStringList parts;
    if (states.testFlag(Qt::WindowMinimized)) {
        parts.push_back(QStringLiteral("minimized"));
    }
    if (states.testFlag(Qt::WindowMaximized)) {
        parts.push_back(QStringLiteral("maximized"));
    }
    if (states.testFlag(Qt::WindowFullScreen)) {
        parts.push_back(QStringLiteral("fullscreen"));
    }
    if (states.testFlag(Qt::WindowActive)) {
        parts.push_back(QStringLiteral("active"));
    }
    if (parts.isEmpty()) {
        return QStringLiteral("none");
    }

    return parts.join(QStringLiteral("|"));
}

Presentation_environment presentation_environment(
    const QQuickWindow&   window,
    const Render_counts&  render)
{
    const QSGRendererInterface* renderer_interface = window.rendererInterface();
    const QSGRendererInterface::GraphicsApi graphics_api =
        renderer_interface == nullptr
            ? QSGRendererInterface::Unknown
            : renderer_interface->graphicsApi();
    const QSGRendererInterface::GraphicsApi static_graphics_api =
        QQuickWindow::graphicsApi();
    const QScreen* screen = window.screen();

    Presentation_environment environment;
    environment.qpa_platform = QGuiApplication::platformName();
    environment.qt_qpa_platform_env = environment_value_or_unset("QT_QPA_PLATFORM");
    environment.qsg_render_loop_env = environment_value_or_unset("QSG_RENDER_LOOP");
    environment.qsg_rhi_backend_env = environment_value_or_unset("QSG_RHI_BACKEND");
    environment.qt_quick_backend_env = environment_value_or_unset("QT_QUICK_BACKEND");
    environment.scene_graph_backend = QQuickWindow::sceneGraphBackend();
    environment.static_graphics_api = graphics_api_key(static_graphics_api);
    environment.static_graphics_api_value = static_cast<int>(static_graphics_api);
    environment.static_graphics_api_rhi_based =
        QSGRendererInterface::isApiRhiBased(static_graphics_api);
    environment.static_graphics_api_software_or_null =
        static_graphics_api == QSGRendererInterface::Software ||
        static_graphics_api == QSGRendererInterface::Null;
    environment.graphics_api = graphics_api_key(graphics_api);
    environment.graphics_api_value = static_cast<int>(graphics_api);
    environment.graphics_api_rhi_based =
        QSGRendererInterface::isApiRhiBased(graphics_api);
    environment.graphics_api_software_or_null =
        graphics_api == QSGRendererInterface::Software ||
        graphics_api == QSGRendererInterface::Null;
    environment.renderer_interface_non_null = renderer_interface != nullptr;
    environment.software_or_null_env_requested =
        environment_backend_requests_software_or_null(
            environment.qsg_rhi_backend_env) ||
        environment_backend_requests_software_or_null(
            environment.qt_quick_backend_env);
    environment.window_visible = window.isVisible();
    environment.window_exposed = window.isExposed();
    environment.window_minimized =
        window.windowStates().testFlag(Qt::WindowMinimized) ||
        window.visibility() == QWindow::Minimized;
    environment.window_visibility = window_visibility_key(window.visibility());
    environment.window_state = window_state_key(window.windowStates());
    environment.window_width = window.width();
    environment.window_height = window.height();
    environment.device_pixel_ratio = window.devicePixelRatio();
    environment.screen_non_null = screen != nullptr;
    if (screen != nullptr) {
        environment.screen_name = screen->name();
        environment.screen_width = screen->geometry().width();
        environment.screen_height = screen->geometry().height();
    }
    environment.qsg_drew = render.qsg_drew;
    environment.render_target_non_null = render.render_target_non_null;
    environment.rhi_non_null = render.rhi_non_null;
    environment.qpa_platform_real_window_valid =
        !qpa_platform_is_headless(environment.qpa_platform) &&
        !qpa_env_requests_headless_platform(environment.qt_qpa_platform_env);
    environment.window_state_real_window_valid =
        environment.window_visible   &&
        environment.window_exposed   &&
        !environment.window_minimized &&
        environment.screen_non_null;
    environment.renderer_real_window_valid =
        environment.renderer_interface_non_null   &&
        graphics_api != QSGRendererInterface::Unknown &&
        !environment.graphics_api_software_or_null &&
        !environment.static_graphics_api_software_or_null &&
        !environment.software_or_null_env_requested &&
        environment.qsg_drew                       &&
        environment.render_target_non_null         &&
        environment.rhi_non_null;
    environment.real_window_claim_valid =
        environment.qpa_platform_real_window_valid &&
        environment.window_state_real_window_valid &&
        environment.renderer_real_window_valid;
    return environment;
}

QString snapshot_row_text(const term::Terminal_render_snapshot& snapshot, int row)
{
    const term::Terminal_render_snapshot_row_content_view rows(snapshot);
    return rows.row_text(row, 0, rows.column_count(), true);
}

std::optional<Snapshot_match> find_snapshot_match(
    const VNM_TerminalSurface& surface,
    const QString&             payload)
{
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(surface);
    if (snapshot == nullptr) {
        return std::nullopt;
    }

    for (int row = 0; row < snapshot->grid_size.rows; ++row) {
        const QString row_text = snapshot_row_text(*snapshot, row);
        const qsizetype payload_start = row_text.indexOf(payload);
        if (payload_start < 0) {
            continue;
        }

        const int expected_cursor_column =
            static_cast<int>(payload_start) + payload.size();
        const bool cursor_fresh =
            snapshot->cursor.position.row    == row                    &&
            snapshot->cursor.position.column >= expected_cursor_column;
        return Snapshot_match{
            snapshot->metadata.sequence,
            row,
            snapshot->cursor.position.column,
            static_cast<int>(payload_start),
            row_text,
            cursor_fresh,
        };
    }

    return std::nullopt;
}

bool wait_until(
    QGuiApplication&        app,
    int                     timeout_ms,
    const std::function<bool()>& predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms) {
        app.processEvents(QEventLoop::AllEvents, 20);
        if (predicate()) {
            return true;
        }
        QThread::msleep(2);
    }

    app.processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}

void pump_for(QGuiApplication& app, int duration_ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < duration_ms) {
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
}

Frame_evidence wait_for_frame_evidence(
    QGuiApplication&                              app,
    QQuickWindow&                                 window,
    VNM_TerminalSurface&                          surface,
    const chrome::Presentation_metrics_recorder&  presentation,
    std::uint64_t                                 presentation_frame_before,
    std::uint64_t                                 snapshot_sequence,
    int                                           timeout_ms,
    const Evidence_requirements&                  requirements)
{
    Frame_evidence evidence;
    const auto refresh_evidence = [&] {
        const Presentation_counts presentation_now =
            presentation_counts(presentation);
        const term::Terminal_surface_render_invalidation_stats_t invalidation =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::Qsg_atlas_frame_report qsg_atlas =
            term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
        evidence.strict_rendered =
            invalidation.last_rendered_snapshot_sequence >= snapshot_sequence;
        evidence.qsg_captured =
            qsg_atlas.captured_snapshot_sequence >= snapshot_sequence;
        evidence.presentation_frame =
            counter_delta(
                presentation_frame_before,
                presentation_now.frame_swapped) > 0U;
    };
    const auto requirements_met = [&] {
        return
            (!requirements.require_qsg_capture || evidence.qsg_captured) &&
            (!requirements.require_rendered_snapshot || evidence.strict_rendered) &&
            (!requirements.require_presentation_frame || evidence.presentation_frame);
    };

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms) {
        surface.update();
        window.requestUpdate();
        app.processEvents(QEventLoop::AllEvents, 20);

        refresh_evidence();
        if (requirements_met()) {
            return evidence;
        }

        QThread::msleep(2);
    }

    refresh_evidence();
    return evidence;
}

int key_for_payload_char(QChar ch)
{
    if (ch.isDigit()) {
        return ch.unicode();
    }
    if (ch.isLetter()) {
        return ch.toUpper().unicode();
    }

    switch (ch.unicode()) {
        case ' ': return Qt::Key_Space;
        default:  return Qt::Key_unknown;
    }
}

bool send_key(VNM_TerminalSurface& surface, int key, const QString& text)
{
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier, text);
    QCoreApplication::sendEvent(&surface, &event);
    return event.isAccepted();
}

bool send_payload(VNM_TerminalSurface& surface, const QString& payload)
{
    for (const QChar ch : payload) {
        const int key = key_for_payload_char(ch);
        if (key == Qt::Key_unknown || !send_key(surface, key, QString(ch))) {
            return false;
        }
    }

    return true;
}

bool send_return(VNM_TerminalSurface& surface)
{
    return send_key(surface, Qt::Key_Return, QStringLiteral("\r"));
}

QString burst_payload(int burst_index, int burst_length)
{
    QString payload = QStringLiteral("probe%1").arg(
        burst_index,
        2,
        10,
        QLatin1Char('0'));

    const QString alphabet = QStringLiteral("abcdefghijklmnopqrstuvwxyz");
    int offset = burst_index % alphabet.size();
    while (payload.size() < burst_length) {
        payload.append(alphabet[offset]);
        offset = (offset + 1) % alphabet.size();
    }

    return payload.left(burst_length);
}

QByteArray joined_write_bytes(
    const std::vector<Write_observation>& writes,
    std::size_t                           first,
    std::size_t                           last)
{
    QByteArray bytes;
    const std::size_t bounded_last = std::min(last, writes.size());
    for (std::size_t index = first; index < bounded_last; ++index) {
        bytes += writes[index].bytes;
    }
    return bytes;
}

QByteArray joined_backend_output_bytes(
    const std::vector<Backend_output_observation>& outputs,
    std::size_t                                    first,
    std::size_t                                    last)
{
    QByteArray bytes;
    const std::size_t bounded_last = std::min(last, outputs.size());
    for (std::size_t index = first; index < bounded_last; ++index) {
        bytes += outputs[index].bytes;
    }
    return bytes;
}

QJsonObject drain_delta_json(const Drain_delta& delta)
{
    QJsonObject object;
    object.insert(QStringLiteral("total_calls"), static_cast<qint64>(delta.total_calls));
    object.insert(QStringLiteral("posted_calls"), static_cast<qint64>(delta.posted_calls));
    object.insert(
        QStringLiteral("frame_pending_calls"),
        static_cast<qint64>(delta.frame_pending_calls));
    object.insert(
        QStringLiteral("total_elapsed_ns"),
        static_cast<qint64>(delta.total_elapsed_ns));
    object.insert(
        QStringLiteral("session_processing_calls"),
        static_cast<qint64>(delta.session_processing_calls));
    object.insert(
        QStringLiteral("sync_from_session_calls"),
        static_cast<qint64>(delta.sync_from_session_calls));
    object.insert(
        QStringLiteral("pending_after_drain"),
        static_cast<qint64>(delta.pending_after_drain));
    object.insert(
        QStringLiteral("backpressure_after_drain"),
        static_cast<qint64>(delta.backpressure_after_drain));
    return object;
}

QJsonObject evidence_requirements_json(
    Evidence_mode                  mode,
    const Evidence_requirements&   requirements)
{
    QJsonObject object;
    object.insert(QStringLiteral("mode"), evidence_mode_key(mode));
    object.insert(QStringLiteral("requires_input_accepted"), true);
    object.insert(QStringLiteral("requires_backend_input_write"), true);
    object.insert(QStringLiteral("requires_backend_echo_emitted"), true);
    object.insert(QStringLiteral("requires_surface_fresh_snapshot"), true);
    object.insert(QStringLiteral("requires_qsg_fresh_capture"), requirements.require_qsg_capture);
    object.insert(
        QStringLiteral("requires_qsg_fresh_render"),
        requirements.require_rendered_snapshot);
    object.insert(
        QStringLiteral("requires_frame_swapped"),
        requirements.require_presentation_frame);
    if (mode == Evidence_mode::REAL_WINDOW_PRESENTATION) {
        object.insert(QStringLiteral("requires_real_window_qpa"), true);
        object.insert(QStringLiteral("requires_visible_non_minimized_window"), true);
        object.insert(QStringLiteral("requires_non_software_non_null_renderer"), true);
        object.insert(QStringLiteral("requires_render_target_non_null"), true);
        object.insert(QStringLiteral("requires_rhi_non_null"), true);
    }
    return object;
}

QJsonObject presentation_delta_json(
    const Presentation_counts& before,
    const Presentation_counts& after)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("frameSwapped_before"),
        static_cast<qint64>(before.frame_swapped));
    object.insert(
        QStringLiteral("frameSwapped_after"),
        static_cast<qint64>(after.frame_swapped));
    object.insert(
        QStringLiteral("frameSwapped_delta"),
        static_cast<qint64>(counter_delta(before.frame_swapped, after.frame_swapped)));
    return object;
}

QJsonObject render_delta_json(const Render_counts& before, const Render_counts& after)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("paint_completed_delta"),
        static_cast<qint64>(counter_delta(before.paint_completed, after.paint_completed)));
    object.insert(
        QStringLiteral("qsg_atlas_render_delta"),
        static_cast<qint64>(counter_delta(before.qsg_atlas_render, after.qsg_atlas_render)));
    object.insert(
        QStringLiteral("qsg_atlas_capture_delta"),
        static_cast<qint64>(counter_delta(before.qsg_atlas_capture, after.qsg_atlas_capture)));
    object.insert(
        QStringLiteral("qsg_captured_snapshot_before"),
        static_cast<qint64>(before.qsg_captured_snapshot));
    object.insert(
        QStringLiteral("qsg_captured_snapshot_after"),
        static_cast<qint64>(after.qsg_captured_snapshot));
    object.insert(
        QStringLiteral("qsg_rendered_snapshot_before"),
        static_cast<qint64>(before.qsg_rendered_snapshot));
    object.insert(
        QStringLiteral("qsg_rendered_snapshot_after"),
        static_cast<qint64>(after.qsg_rendered_snapshot));
    object.insert(QStringLiteral("qsg_drew_after"), after.qsg_drew);
    object.insert(
        QStringLiteral("render_target_non_null_after"),
        after.render_target_non_null);
    object.insert(QStringLiteral("rhi_non_null_after"), after.rhi_non_null);
    object.insert(
        QStringLiteral("last_rendered_snapshot_before"),
        static_cast<qint64>(before.last_rendered_snapshot));
    object.insert(
        QStringLiteral("last_rendered_snapshot_after"),
        static_cast<qint64>(after.last_rendered_snapshot));
    object.insert(
        QStringLiteral("backend_callback_frame_deferrals_delta"),
        static_cast<qint64>(counter_delta(
            before.backend_callback_frame_deferrals,
            after.backend_callback_frame_deferrals)));
    object.insert(
        QStringLiteral("backend_callback_frame_deferrals_after"),
        static_cast<qint64>(after.backend_callback_frame_deferrals));
    object.insert(
        QStringLiteral("backend_callback_event_epoch_after"),
        static_cast<qint64>(after.backend_callback_event_epoch));
    object.insert(
        QStringLiteral("backend_callback_frame_boundary_epoch_after"),
        static_cast<qint64>(after.backend_callback_frame_boundary_epoch));
    return object;
}

QJsonObject presentation_environment_json(
    const Presentation_environment& environment)
{
    QJsonObject object;
    object.insert(QStringLiteral("qpa_platform"), environment.qpa_platform);
    object.insert(
        QStringLiteral("qt_qpa_platform_env"),
        environment.qt_qpa_platform_env);
    object.insert(
        QStringLiteral("qsg_render_loop_env"),
        environment.qsg_render_loop_env);
    object.insert(
        QStringLiteral("qsg_rhi_backend_env"),
        environment.qsg_rhi_backend_env);
    object.insert(
        QStringLiteral("qt_quick_backend_env"),
        environment.qt_quick_backend_env);
    object.insert(
        QStringLiteral("scene_graph_backend"),
        environment.scene_graph_backend);
    object.insert(
        QStringLiteral("static_graphics_api"),
        environment.static_graphics_api);
    object.insert(
        QStringLiteral("static_graphics_api_value"),
        environment.static_graphics_api_value);
    object.insert(
        QStringLiteral("static_graphics_api_rhi_based"),
        environment.static_graphics_api_rhi_based);
    object.insert(
        QStringLiteral("static_graphics_api_software_or_null"),
        environment.static_graphics_api_software_or_null);
    object.insert(QStringLiteral("graphics_api"), environment.graphics_api);
    object.insert(
        QStringLiteral("graphics_api_value"),
        environment.graphics_api_value);
    object.insert(
        QStringLiteral("graphics_api_rhi_based"),
        environment.graphics_api_rhi_based);
    object.insert(
        QStringLiteral("graphics_api_software_or_null"),
        environment.graphics_api_software_or_null);
    object.insert(
        QStringLiteral("renderer_interface_non_null"),
        environment.renderer_interface_non_null);
    object.insert(
        QStringLiteral("software_or_null_env_requested"),
        environment.software_or_null_env_requested);
    object.insert(QStringLiteral("window_visible"), environment.window_visible);
    object.insert(QStringLiteral("window_exposed"), environment.window_exposed);
    object.insert(
        QStringLiteral("window_minimized"),
        environment.window_minimized);
    object.insert(
        QStringLiteral("window_visibility"),
        environment.window_visibility);
    object.insert(QStringLiteral("window_state"), environment.window_state);
    object.insert(QStringLiteral("window_width"), environment.window_width);
    object.insert(QStringLiteral("window_height"), environment.window_height);
    object.insert(
        QStringLiteral("device_pixel_ratio"),
        environment.device_pixel_ratio);
    object.insert(QStringLiteral("screen_non_null"), environment.screen_non_null);
    object.insert(QStringLiteral("screen_name"), environment.screen_name);
    object.insert(QStringLiteral("screen_width"), environment.screen_width);
    object.insert(QStringLiteral("screen_height"), environment.screen_height);
    object.insert(QStringLiteral("qsg_drew"), environment.qsg_drew);
    object.insert(
        QStringLiteral("render_target_non_null"),
        environment.render_target_non_null);
    object.insert(QStringLiteral("rhi_non_null"), environment.rhi_non_null);
    object.insert(
        QStringLiteral("qpa_platform_real_window_valid"),
        environment.qpa_platform_real_window_valid);
    object.insert(
        QStringLiteral("window_state_real_window_valid"),
        environment.window_state_real_window_valid);
    object.insert(
        QStringLiteral("renderer_real_window_valid"),
        environment.renderer_real_window_valid);
    object.insert(
        QStringLiteral("real_window_claim_valid"),
        environment.real_window_claim_valid);
    return object;
}

QJsonObject snapshot_match_json(const Snapshot_match& match)
{
    QJsonObject object;
    object.insert(QStringLiteral("sequence"), static_cast<qint64>(match.sequence));
    object.insert(QStringLiteral("row"), match.row);
    object.insert(QStringLiteral("cursor_column"), match.column);
    object.insert(QStringLiteral("payload_start_column"), match.payload_start);
    object.insert(QStringLiteral("cursor_fresh"), match.cursor_fresh);
    object.insert(QStringLiteral("row_text"), match.row_text);
    return object;
}

QJsonObject stage_evidence_json(
    bool                   input_accepted,
    bool                   backend_input_write_observed,
    bool                   backend_echo_emitted,
    bool                   surface_published_fresh_snapshot,
    const Frame_evidence&  frame_evidence)
{
    QJsonObject object;
    object.insert(QStringLiteral("input_accepted"), input_accepted);
    object.insert(
        QStringLiteral("backend_input_write_observed"),
        backend_input_write_observed);
    object.insert(QStringLiteral("backend_echo_emitted"), backend_echo_emitted);
    object.insert(
        QStringLiteral("surface_published_fresh_snapshot"),
        surface_published_fresh_snapshot);
    object.insert(
        QStringLiteral("qsg_captured_fresh_snapshot"),
        frame_evidence.qsg_captured);
    object.insert(
        QStringLiteral("qsg_rendered_fresh_snapshot"),
        frame_evidence.strict_rendered);
    object.insert(
        QStringLiteral("frame_presented_after_input"),
        frame_evidence.presentation_frame);
    return object;
}

bool write_jsonl_record(QFile& file, const QJsonObject& object, QString* out_error)
{
    QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact);
    line.append('\n');

    if (file.write(line) != line.size()) {
        *out_error = QStringLiteral("could not write evidence JSONL %1: %2")
            .arg(file.fileName(), file.errorString());
        return false;
    }
    if (!file.flush()) {
        *out_error = QStringLiteral("could not flush evidence JSONL %1: %2")
            .arg(file.fileName(), file.errorString());
        return false;
    }

    return true;
}

class Echo_backend final : public term::Terminal_backend
{
public:
    Echo_backend(QElapsedTimer& app_timer, int echo_delay_ms)
    :
        m_app_timer(app_timer),
        m_echo_delay_ms(echo_delay_ms)
    {}

    term::Terminal_backend_result start(
        const term::Terminal_launch_config& config,
        term::Terminal_backend_callbacks    callbacks) override
    {
        const term::Terminal_backend_result callback_result =
            term::validate_backend_callbacks(callbacks);
        if (term::is_backend_rejection(callback_result)) {
            return callback_result;
        }

        const term::Terminal_backend_result config_result =
            term::validate_launch_config(config);
        if (term::is_backend_rejection(config_result)) {
            return config_result;
        }

        m_callbacks = std::move(callbacks);
        m_running   = true;
        schedule_output(QByteArrayLiteral("input-echo-probe-ready\r\n"));
        return term::backend_accept();
    }

    term::Terminal_backend_result write(QByteArray bytes) override
    {
        m_writes.push_back({
            static_cast<int>(m_writes.size()),
            m_app_timer.elapsed(),
            bytes,
        });

        QByteArray output;
        for (char byte : bytes) {
            if (byte == '\r') {
                output += QByteArrayLiteral("\r\n");
            }
            else {
                output += byte;
            }
        }
        if (!output.isEmpty()) {
            schedule_output(std::move(output));
        }

        return term::backend_accept();
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request request) override
    {
        if (!term::is_valid_grid_size(request.grid_size)) {
            return term::backend_reject(
                term::Terminal_backend_error_code::RESIZE_FAILED,
                QStringLiteral("input echo probe resize requires a positive grid"));
        }

        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool) override
    {
        return term::backend_accept();
    }

    term::Terminal_backend_result interrupt() override
    {
        if (m_running) {
            finish({term::Terminal_exit_reason::INTERRUPTED, 130});
        }
        return term::backend_accept();
    }

    term::Terminal_backend_result terminate() override
    {
        if (m_running) {
            finish({term::Terminal_exit_reason::TERMINATED, 0});
        }
        return term::backend_accept();
    }

    void finish(term::Terminal_backend_exit exit)
    {
        if (!m_running) {
            return;
        }

        m_running = false;
        term::Terminal_backend_callbacks callbacks = m_callbacks;
        QTimer::singleShot(0, QCoreApplication::instance(), [callbacks, exit] {
            callbacks.process_exited(exit);
        });
    }

    const std::vector<Write_observation>& writes() const { return m_writes; }
    const std::vector<Backend_output_observation>& outputs() const { return m_outputs; }

private:
    void schedule_output(QByteArray output)
    {
        if (output.isEmpty()) {
            return;
        }

        m_pending_outputs.push_back(std::move(output));
        if (m_output_flush_queued) {
            return;
        }

        m_output_flush_queued = true;
        QTimer::singleShot(
            m_echo_delay_ms,
            QCoreApplication::instance(),
            [this] {
                flush_pending_outputs();
            });
    }

    void flush_pending_outputs()
    {
        m_output_flush_queued = false;
        term::Terminal_backend_callbacks callbacks = m_callbacks;
        while (!m_pending_outputs.empty()) {
            QByteArray output = std::move(m_pending_outputs.front());
            m_pending_outputs.pop_front();
            m_outputs.push_back({
                static_cast<int>(m_outputs.size()),
                m_app_timer.elapsed(),
                output,
            });
            callbacks.output_received(std::move(output));
        }
    }

    QElapsedTimer&                                  m_app_timer;
    int                                             m_echo_delay_ms = 0;
    bool                                            m_running = false;
    bool                                            m_output_flush_queued = false;
    term::Terminal_backend_callbacks                m_callbacks;
    std::deque<QByteArray>                          m_pending_outputs;
    std::vector<Write_observation>                  m_writes;
    std::vector<Backend_output_observation>         m_outputs;
};

void print_burst_failure_diagnostics(
    const VNM_TerminalSurface&             surface,
    const std::vector<Write_observation>&  writes,
    const std::vector<Backend_output_observation>& outputs,
    std::size_t                            first_write,
    std::size_t                            first_output,
    std::size_t                            expected_write_count,
    std::size_t                            expected_output_count,
    const QString&                         payload)
{
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(surface);
    const term::Terminal_surface_backend_drain_stats_t drain =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(surface);
    const Render_counts render = render_counts(surface);
    const QByteArray joined_writes =
        joined_write_bytes(writes, first_write, writes.size());
    const QByteArray joined_outputs =
        joined_backend_output_bytes(outputs, first_output, outputs.size());

    std::cerr
        << "  payload=" << payload.toStdString()
        << " writes_observed=" << writes.size()
        << " expected_writes=" << expected_write_count
        << " first_write_index=" << first_write
        << " joined_write_hex="
        << QString::fromLatin1(joined_writes.toHex()).toStdString()
        << '\n';
    std::cerr
        << "  backend_outputs_observed=" << outputs.size()
        << " expected_outputs=" << expected_output_count
        << " first_output_index=" << first_output
        << " joined_output_hex="
        << QString::fromLatin1(joined_outputs.toHex()).toStdString()
        << '\n';
    std::cerr
        << "  backend_drain total=" << drain.total_drain_calls
        << " posted=" << drain.posted_drain_calls
        << " session_processing=" << drain.session_processing_calls
        << " sync_from_session=" << drain.sync_from_session_calls
        << " pending_after_drain=" << drain.pending_callback_after_drain
        << " backpressure_after_drain=" << drain.output_backpressure_after_drain
        << '\n';
    std::cerr
        << "  render paint_completed=" << render.paint_completed
        << " qsg_atlas_render=" << render.qsg_atlas_render
        << " qsg_atlas_capture=" << render.qsg_atlas_capture
        << " qsg_captured_snapshot=" << render.qsg_captured_snapshot
        << " qsg_rendered_snapshot=" << render.qsg_rendered_snapshot
        << " last_rendered_snapshot=" << render.last_rendered_snapshot
        << " backend_callback_frame_deferrals="
        << render.backend_callback_frame_deferrals
        << " backend_callback_event_epoch=" << render.backend_callback_event_epoch
        << " backend_callback_frame_boundary_epoch="
        << render.backend_callback_frame_boundary_epoch
        << " qsg_drew=" << (render.qsg_drew ? "true" : "false")
        << '\n';

    if (snapshot == nullptr) {
        std::cerr << "  snapshot=null\n";
        return;
    }

    const term::Terminal_render_snapshot_row_content_view rows(*snapshot);
    std::cerr
        << "  snapshot sequence=" << snapshot->metadata.sequence
        << " grid=" << snapshot->grid_size.columns << 'x' << snapshot->grid_size.rows
        << " cursor=" << snapshot->cursor.position.row
        << ',' << snapshot->cursor.position.column
        << " row_count=" << rows.row_count()
        << '\n';
    const int rows_to_print = std::min(rows.row_count(), 8);
    for (int row = 0; row < rows_to_print; ++row) {
        const QString text = rows.row_text(row, 0, rows.column_count(), true);
        std::cerr
            << "  row[" << row << "]="
            << text.toStdString()
            << '\n';
    }
}

bool open_output_file(QFile& file, const QString& path, QString* out_error)
{
    if (!ensure_parent_directory(path, out_error)) {
        return false;
    }

    file.setFileName(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *out_error = QStringLiteral("could not write %1: %2")
            .arg(path, file.errorString());
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    const Parse_result parsed = parse_arguments(QCoreApplication::arguments());
    if (parsed.help_requested) {
        print_usage();
        return 0;
    }
    if (!parsed.error.isEmpty()) {
        std::cerr << "ERROR: " << parsed.error.toStdString() << '\n';
        print_usage();
        return 2;
    }

    const Probe_options options = parsed.options;
    const Evidence_requirements requirements = evidence_requirements(options);
    QString error;
    QFile evidence_file;
    if (!open_output_file(evidence_file, options.evidence_jsonl_path, &error)) {
        std::cerr << "ERROR: " << error.toStdString() << '\n';
        return 2;
    }

    chrome::Metrics_timeline_jsonl_writer metrics_writer;
    if (!ensure_parent_directory(options.metrics_timeline_jsonl_path, &error) ||
        !metrics_writer.open(options.metrics_timeline_jsonl_path, &error))
    {
        std::cerr << "ERROR: " << error.toStdString() << '\n';
        return 2;
    }

    QElapsedTimer app_timer;
    app_timer.start();

    chrome::Runtime_state runtime_state;
    chrome::Presentation_metrics_recorder presentation_metrics;
    QQuickWindow window;
    window.resize(options.window_size);
    chrome::connect_presentation_metrics_recorder(window, presentation_metrics);

    VNM_TerminalSurface surface(window.contentItem());
    surface.setSize(QSizeF(
        static_cast<qreal>(options.window_size.width()),
        static_cast<qreal>(options.window_size.height())));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(options.font_size);
    surface.forceActiveFocus();

    QObject::connect(
        &surface,
        &VNM_TerminalSurface::output_activity,
        &surface,
        [&runtime_state, &app_timer] {
            if (!runtime_state.output_seen) {
                runtime_state.first_output_elapsed_ms = app_timer.elapsed();
            }
            runtime_state.output_seen = true;
        });
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::backend_error,
        &surface,
        [&runtime_state](VNM_TerminalSurface::Backend_error_code, const QString&) {
            ++runtime_state.backend_error_count;
        });
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::process_exited,
        &surface,
        [&runtime_state](VNM_TerminalSurface::Exit_reason reason, int exit_code) {
            runtime_state.process_exited      = true;
            runtime_state.process_exit_reason = reason;
            runtime_state.process_exit_code   = exit_code;
        });

    window.show();
    pump_for(app, 50);

    auto backend = std::make_unique<Echo_backend>(app_timer, options.echo_delay_ms);
    Echo_backend* backend_ptr = backend.get();
    const bool started = term::VNM_TerminalSurface_render_bridge::start_process_with_backend(
        surface,
        std::move(backend),
        {QStringLiteral("input-echo-probe")});
    if (!started) {
        std::cerr << "ERROR: input echo probe backend did not start\n";
        return 3;
    }

    int periodic_metrics_samples = 0;
    const auto write_periodic_metrics = [&](QString* out_error) {
        chrome::metrics_timing_t timing;
        timing.app_elapsed_ms = app_timer.elapsed();
        if (!metrics_writer.write_sample(
                chrome::Metrics_timeline_sample_kind::PERIODIC,
                surface,
                presentation_metrics,
                runtime_state,
                timing,
                std::nullopt,
                options.metrics_timeline_interval_ms,
                out_error))
        {
            return false;
        }

        ++periodic_metrics_samples;
        return true;
    };
    const auto write_final_metrics = [&](int app_result, QString* out_error) {
        chrome::metrics_timing_t timing;
        timing.app_elapsed_ms = app_timer.elapsed();
        return metrics_writer.write_sample(
            chrome::Metrics_timeline_sample_kind::FINAL,
            surface,
            presentation_metrics,
            runtime_state,
            timing,
            app_result,
            options.metrics_timeline_interval_ms,
            out_error);
    };

    QTimer metrics_timer;
    metrics_timer.setInterval(options.metrics_timeline_interval_ms);
    QObject::connect(
        &metrics_timer,
        &QTimer::timeout,
        &metrics_timer,
        [&write_periodic_metrics, &error] {
            QString write_error;
            if (!write_periodic_metrics(&write_error) && error.isEmpty()) {
                error = write_error;
            }
        });
    metrics_timer.start();

    bool ok = wait_until(app, options.timeout_ms, [&surface] {
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(surface);
        return snapshot != nullptr;
    });
    if (!ok) {
        std::cerr << "ERROR: initial render snapshot was not published\n";
        return 4;
    }

    int exceeded_echo_limit = 0;
    int exceeded_frame_limit = 0;
    int missing_backend_echo = 0;
    int missing_qsg_capture = 0;
    int missing_presentation_frame = 0;
    int missing_backend_drain = 0;
    int missing_rendered_snapshot = 0;
    int missing_frame_evidence = 0;
    int invalid_real_window_environment = 0;

    for (int burst_index = 0; burst_index < options.burst_count; ++burst_index) {
        const QString payload = burst_payload(burst_index, options.burst_length);

        const term::Terminal_surface_backend_drain_stats_t drain_before =
            term::VNM_TerminalSurface_render_bridge::backend_drain_stats(surface);
        const Presentation_counts presentation_before =
            presentation_counts(presentation_metrics);
        const Render_counts render_before = render_counts(surface);
        const std::size_t first_write = backend_ptr->writes().size();
        const std::size_t first_output = backend_ptr->outputs().size();

        const qint64 send_elapsed_ms = app_timer.elapsed();
        const bool input_accepted = send_payload(surface, payload);
        if (!input_accepted) {
            std::cerr << "ERROR: key burst " << burst_index << " was not accepted\n";
            return 5;
        }
        const std::size_t expected_write_count =
            first_write + static_cast<std::size_t>(payload.size());
        const std::size_t expected_output_count =
            first_output + static_cast<std::size_t>(payload.size());

        std::optional<Snapshot_match> match;
        ok = wait_until(app, options.timeout_ms, [&] {
            if (backend_ptr->writes().size() < expected_write_count) {
                return false;
            }
            if (backend_ptr->outputs().size() < expected_output_count) {
                return false;
            }

            match = find_snapshot_match(surface, payload);
            return match.has_value() && match->cursor_fresh;
        });

        const qint64 cursor_fresh_elapsed_ms = app_timer.elapsed() - send_elapsed_ms;
        if (!ok || !match.has_value()) {
            std::cerr << "ERROR: key burst " << burst_index
                << " did not reach a fresh cursor snapshot for payload "
                << payload.toStdString() << '\n';
            print_burst_failure_diagnostics(
                surface,
                backend_ptr->writes(),
                backend_ptr->outputs(),
                first_write,
                first_output,
                expected_write_count,
                expected_output_count,
                payload);
            return 6;
        }

        const Frame_evidence frame_evidence = wait_for_frame_evidence(
            app,
            window,
            surface,
            presentation_metrics,
            presentation_before.frame_swapped,
            match->sequence,
            options.timeout_ms,
            requirements);
        const qint64 frame_evidence_elapsed_ms = app_timer.elapsed() - send_elapsed_ms;

        const term::Terminal_surface_backend_drain_stats_t drain_after =
            term::VNM_TerminalSurface_render_bridge::backend_drain_stats(surface);
        const Presentation_counts presentation_after =
            presentation_counts(presentation_metrics);
        const Render_counts render_after = render_counts(surface);
        const Presentation_environment environment =
            presentation_environment(window, render_after);
        const Drain_delta drain = drain_delta(drain_before, drain_after);
        const QByteArray input_bytes = joined_write_bytes(
            backend_ptr->writes(),
            first_write,
            expected_write_count);
        const QByteArray backend_echo_bytes = joined_backend_output_bytes(
            backend_ptr->outputs(),
            first_output,
            expected_output_count);
        const bool backend_input_write_observed =
            backend_ptr->writes().size() >= expected_write_count;
        const bool backend_echo_emitted =
            backend_ptr->outputs().size() >= expected_output_count;
        const bool surface_published_fresh_snapshot =
            match.has_value() && match->cursor_fresh;

        const std::uint64_t presentation_delta =
            counter_delta(presentation_before.frame_swapped, presentation_after.frame_swapped);
        if (cursor_fresh_elapsed_ms > options.max_echo_ms) {
            ++exceeded_echo_limit;
        }
        if (frame_evidence_elapsed_ms > options.max_frame_ms) {
            ++exceeded_frame_limit;
        }
        if (!backend_echo_emitted) {
            ++missing_backend_echo;
        }
        if (!frame_evidence.qsg_captured) {
            ++missing_qsg_capture;
        }
        if (requirements.require_presentation_frame && !frame_evidence.presentation_frame) {
            ++missing_presentation_frame;
        }
        if (drain.total_calls == 0U) {
            ++missing_backend_drain;
        }
        if (requirements.require_rendered_snapshot && !frame_evidence.strict_rendered) {
            ++missing_rendered_snapshot;
        }
        if (requirements.require_qsg_capture && !frame_evidence.qsg_captured) {
            ++missing_frame_evidence;
        }
        if (options.evidence_mode == Evidence_mode::REAL_WINDOW_PRESENTATION &&
            !environment.real_window_claim_valid)
        {
            ++invalid_real_window_environment;
        }

        QJsonObject record;
        record.insert(
            QStringLiteral("schema"),
            QStringLiteral("vnm_terminal_input_echo_timing_burst_v2"));
        record.insert(QStringLiteral("burst_index"), burst_index);
        record.insert(QStringLiteral("payload"), payload);
        record.insert(QStringLiteral("payload_utf8_bytes"), payload.toUtf8().size());
        record.insert(QStringLiteral("input_write_count"), static_cast<int>(
            expected_write_count - first_write));
        record.insert(QStringLiteral("input_bytes_hex"), QString::fromLatin1(input_bytes.toHex()));
        record.insert(
            QStringLiteral("backend_echo_output_count"),
            static_cast<int>(expected_output_count - first_output));
        record.insert(
            QStringLiteral("backend_echo_bytes_hex"),
            QString::fromLatin1(backend_echo_bytes.toHex()));
        record.insert(QStringLiteral("send_elapsed_ms"), send_elapsed_ms);
        if (backend_ptr->writes().size() >= expected_write_count) {
            record.insert(
                QStringLiteral("first_input_write_elapsed_ms"),
                backend_ptr->writes()[first_write].elapsed_ms);
            record.insert(
                QStringLiteral("last_input_write_elapsed_ms"),
                backend_ptr->writes()[expected_write_count - 1U].elapsed_ms);
        }
        if (backend_ptr->outputs().size() >= expected_output_count) {
            record.insert(
                QStringLiteral("first_backend_echo_elapsed_ms"),
                backend_ptr->outputs()[first_output].elapsed_ms);
            record.insert(
                QStringLiteral("last_backend_echo_elapsed_ms"),
                backend_ptr->outputs()[expected_output_count - 1U].elapsed_ms);
        }
        record.insert(QStringLiteral("cursor_fresh_elapsed_ms"), cursor_fresh_elapsed_ms);
        record.insert(QStringLiteral("frame_evidence_elapsed_ms"), frame_evidence_elapsed_ms);
        record.insert(QStringLiteral("rendered_snapshot_observed"), frame_evidence.strict_rendered);
        record.insert(QStringLiteral("qsg_capture_observed"), frame_evidence.qsg_captured);
        record.insert(
            QStringLiteral("presentation_frame_observed"),
            frame_evidence.presentation_frame);
        record.insert(
            QStringLiteral("stages"),
            stage_evidence_json(
                input_accepted,
                backend_input_write_observed,
                backend_echo_emitted,
                surface_published_fresh_snapshot,
                frame_evidence));
        record.insert(
            QStringLiteral("decision_criteria"),
            evidence_requirements_json(options.evidence_mode, requirements));
        record.insert(
            QStringLiteral("presentation_environment"),
            presentation_environment_json(environment));
        record.insert(QStringLiteral("backend_drain"), drain_delta_json(drain));
        record.insert(
            QStringLiteral("presentation"),
            presentation_delta_json(presentation_before, presentation_after));
        record.insert(QStringLiteral("render"), render_delta_json(render_before, render_after));
        record.insert(QStringLiteral("snapshot"), snapshot_match_json(*match));

        QJsonObject limits;
        limits.insert(QStringLiteral("max_echo_ms"), options.max_echo_ms);
        limits.insert(QStringLiteral("max_frame_ms"), options.max_frame_ms);
        limits.insert(QStringLiteral("timeout_ms"), options.timeout_ms);
        limits.insert(
            QStringLiteral("latency_limits_enforced"),
            options.enforce_latency_limits);
        limits.insert(QStringLiteral("evidence_mode"), evidence_mode_key(options.evidence_mode));
        record.insert(QStringLiteral("limits"), limits);

        if (!write_jsonl_record(evidence_file, record, &error)) {
            std::cerr << "ERROR: " << error.toStdString() << '\n';
            return 2;
        }

        std::cout
            << "input echo burst " << burst_index
            << " writes=" << (expected_write_count - first_write)
            << " backend_echo_outputs=" << (expected_output_count - first_output)
            << " echo_ms=" << cursor_fresh_elapsed_ms
            << " frame_ms=" << frame_evidence_elapsed_ms
            << " drain_delta=" << drain.total_calls
            << " frameSwapped_delta=" << presentation_delta
            << " qsg_capture=" << (frame_evidence.qsg_captured ? "yes" : "no")
            << " strict_render=" << (frame_evidence.strict_rendered ? "yes" : "no")
            << " presented=" << (frame_evidence.presentation_frame ? "yes" : "no")
            << " real_window_env="
            << (environment.real_window_claim_valid ? "yes" : "no")
            << " snapshot=" << match->sequence
            << " cursor=" << match->row << ',' << match->column
            << '\n';

        if (!send_return(surface)) {
            std::cerr << "ERROR: separator Return after burst " << burst_index
                << " was not accepted\n";
            return 7;
        }
        pump_for(app, options.burst_spacing_ms);
        if (!error.isEmpty()) {
            std::cerr << "ERROR: " << error.toStdString() << '\n';
            return 2;
        }
    }

    backend_ptr->finish({term::Terminal_exit_reason::EXITED, 0});
    (void)wait_until(app, options.timeout_ms, [&runtime_state] {
        return runtime_state.process_exited;
    });
    metrics_timer.stop();

    int app_result = 0;
    if ((options.enforce_latency_limits &&
            (exceeded_echo_limit > 0 || exceeded_frame_limit > 0)) ||
        missing_backend_echo > 0 ||
        missing_backend_drain > 0 ||
        missing_frame_evidence > 0 ||
        invalid_real_window_environment > 0 ||
        (requirements.require_qsg_capture && missing_qsg_capture > 0) ||
        (requirements.require_rendered_snapshot && missing_rendered_snapshot > 0) ||
        (requirements.require_presentation_frame && missing_presentation_frame > 0))
    {
        app_result = 8;
    }

    if (!write_final_metrics(app_result, &error)) {
        std::cerr << "ERROR: " << error.toStdString() << '\n';
        return 2;
    }

    std::cout
        << "input echo timing summary"
        << " bursts=" << options.burst_count
        << " periodic_metrics_samples=" << periodic_metrics_samples
        << " exceeded_echo_limit=" << exceeded_echo_limit
        << " exceeded_frame_limit=" << exceeded_frame_limit
        << " latency_limits_enforced="
        << (options.enforce_latency_limits ? "yes" : "no")
        << " evidence_mode=" << evidence_mode_key(options.evidence_mode).toStdString()
        << " missing_backend_echo=" << missing_backend_echo
        << " missing_backend_drain=" << missing_backend_drain
        << " missing_qsg_capture=" << missing_qsg_capture
        << " missing_presentation_frame=" << missing_presentation_frame
        << " missing_rendered_snapshot=" << missing_rendered_snapshot
        << " missing_frame_evidence=" << missing_frame_evidence
        << " invalid_real_window_environment=" << invalid_real_window_environment
        << " metrics_timeline=" << options.metrics_timeline_jsonl_path.toStdString()
        << " evidence=" << options.evidence_jsonl_path.toStdString()
        << '\n';

    if (app_result != 0) {
        std::cerr
            << "ERROR: input echo timing probe did not meet configured evidence "
               "requirements\n";
    }

    return app_result;
}
