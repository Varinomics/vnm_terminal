#include "app_metrics.h"

#include "app_common.h"

#include "vnm_terminal/diagnostics/metrics_json.h"

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QQuickWindow>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace vnm_terminal::terminal_app {

namespace {

template<typename Value>
void insert_json_counter(
    QJsonObject&  object,
    const char*   name,
    Value         value)
{
    object.insert(
        QString::fromLatin1(name),
        QString::number(static_cast<qulonglong>(value)));
}

constexpr const char* k_runtime_frame_rate_elapsed_basis =
    "app_exec_elapsed_ms_including_process_startup_excluding_profile_write";
constexpr const char* k_runtime_metrics_schema = "vnm_terminal_runtime_metrics_v2";
constexpr const char* k_metrics_timeline_sample_schema =
    "vnm_terminal_metrics_timeline_sample_v1";
constexpr const char* k_paint_completed_frame_counter_path =
    "renderer.paint_completed_frames";
constexpr const char* k_qsg_atlas_render_frame_counter_path =
    "qsg_atlas.render_count";
constexpr const char* k_presentation_frame_swapped_counter_path =
    "presentation.frameSwapped.count";
constexpr const char* k_presentation_frame_swapped_counter_source =
    "QQuickWindow::frameSwapped";
constexpr const char* k_presentation_frame_swapped_counter_semantics =
    "qt_frame_swapped_proxy";

struct renderer_frame_evidence_t
{
    const char*   counter_path = k_paint_completed_frame_counter_path;
    std::uint64_t frame_count  = 0U;
};

double frames_per_second(
    std::uint64_t frame_count,
    qint64        elapsed_ms)
{
    return elapsed_ms > 0
        ? static_cast<double>(frame_count) * 1000.0 / static_cast<double>(elapsed_ms)
        : 0.0;
}

renderer_frame_evidence_t renderer_frame_evidence(
    std::uint64_t paint_completed_frame_count,
    std::uint64_t qsg_atlas_render_frame_count)
{
    if (qsg_atlas_render_frame_count > 0U) {
        return {
            k_qsg_atlas_render_frame_counter_path,
            qsg_atlas_render_frame_count,
        };
    }

    return {
        k_paint_completed_frame_counter_path,
        paint_completed_frame_count,
    };
}

const char* metrics_timeline_sample_kind_text(Metrics_timeline_sample_kind kind)
{
    switch (kind) {
        case Metrics_timeline_sample_kind::PERIODIC: return "periodic";
        case Metrics_timeline_sample_kind::FINAL:    return "final";
        default:                                     return "unknown";
    }
}

QJsonObject renderer_frame_evidence_json(
    const renderer_frame_evidence_t& evidence,
    qint64                           elapsed_ms)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("counter_path"),
        QString::fromLatin1(evidence.counter_path));
    insert_json_counter(object, "frame_count", evidence.frame_count);
    object.insert(
        QStringLiteral("frames_per_second"),
        frames_per_second(evidence.frame_count, elapsed_ms));
    object.insert(
        QStringLiteral("elapsed_basis"),
        QString::fromLatin1(k_runtime_frame_rate_elapsed_basis));
    return object;
}

QJsonObject startup_metrics_json(
    const Runtime_state&              state,
    const renderer_frame_evidence_t&  frame_evidence)
{
    QJsonObject object;
    object.insert(QStringLiteral("first_output_elapsed_ms"), state.first_output_elapsed_ms);
    object.insert(QStringLiteral("output_seen"), state.output_seen);
    object.insert(
        QStringLiteral("visible_first_frame_completed"),
        frame_evidence.frame_count > 0U);
    object.insert(
        QStringLiteral("visible_first_frame_counter_path"),
        QString::fromLatin1(frame_evidence.counter_path));
    return object;
}

QJsonObject profiling_measurement_json(const metrics_timing_t& timing)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("compiled"),
        static_cast<bool>(VNM_TERMINAL_PROFILING_ENABLED));
    object.insert(QStringLiteral("profile_text_requested"), timing.profile_text_requested);
    object.insert(QStringLiteral("profile_write_elapsed_ms"), timing.profile_write_elapsed_ms);
    object.insert(QStringLiteral("elapsed_ms_excludes_profile_write"), true);
    return object;
}

QJsonObject presentation_signal_metrics_json(
    const presentation_signal_metrics_t& metrics)
{
    QJsonObject object;
    insert_json_counter(object, "count", metrics.count);
    insert_json_counter(object, "last_interval_ns", metrics.last_interval_ns);
    insert_json_counter(object, "max_interval_ns", metrics.max_interval_ns);
    return object;
}

QJsonObject presentation_metrics_json(
    const Presentation_metrics_recorder&  recorder,
    qint64                                elapsed_ms)
{
    const presentation_metrics_snapshot_t snapshot = recorder.snapshot();
    const presentation_signal_metrics_t& frame_swapped =
        snapshot.stages[static_cast<std::size_t>(Presentation_signal::FRAME_SWAPPED)];

    QJsonObject object;
    object.insert(
        QStringLiteral("primary_counter_path"),
        QString::fromLatin1(k_presentation_frame_swapped_counter_path));
    object.insert(
        QStringLiteral("primary_counter_source"),
        QString::fromLatin1(k_presentation_frame_swapped_counter_source));
    object.insert(
        QStringLiteral("primary_counter_semantics"),
        QString::fromLatin1(k_presentation_frame_swapped_counter_semantics));
    object.insert(QStringLiteral("scanout_verified"), false);
    insert_json_counter(object, "primary_frame_count", frame_swapped.count);
    object.insert(
        QStringLiteral("primary_frames_per_second"),
        frames_per_second(frame_swapped.count, elapsed_ms));
    object.insert(
        QStringLiteral("elapsed_basis"),
        QString::fromLatin1(k_runtime_frame_rate_elapsed_basis));

    for (std::size_t index = 0U; index < snapshot.stages.size(); ++index) {
        const auto signal = static_cast<Presentation_signal>(index);
        if (!presentation_signal_available(signal)) {
            continue;
        }

        object.insert(
            QString::fromLatin1(presentation_signal_json_key(signal)),
            presentation_signal_metrics_json(snapshot.stages[index]));
    }

    return object;
}

QJsonObject surface_geometry_json(const VNM_TerminalSurface& surface)
{
    QJsonObject object;
    object.insert(QStringLiteral("rows"), surface.rows());
    object.insert(QStringLiteral("columns"), surface.columns());
    object.insert(QStringLiteral("surface_width"), surface.width());
    object.insert(QStringLiteral("surface_height"), surface.height());
    object.insert(QStringLiteral("font_family"), surface.font_family());
    object.insert(QStringLiteral("font_size"), surface.font_size());
    if (surface.window() != nullptr) {
        object.insert(QStringLiteral("window_width"), surface.window()->width());
        object.insert(QStringLiteral("window_height"), surface.window()->height());
        object.insert(
            QStringLiteral("device_pixel_ratio"),
            surface.window()->devicePixelRatio());
    }
    return object;
}

QJsonObject terminal_metrics_json(
    const VNM_TerminalSurface&  surface,
    const Presentation_metrics_recorder&
                                presentation_metrics,
    const Runtime_state&        state,
    const metrics_timing_t&     timing,
    std::optional<int>          app_result)
{
    const std::uint64_t paint_completed_frame_count =
        static_cast<std::uint64_t>(surface.paint_completed_frame_count());
    const std::uint64_t qsg_atlas_render_frame_count =
        static_cast<std::uint64_t>(surface.qsg_atlas_render_frame_count());
    const renderer_frame_evidence_t frame_evidence =
        renderer_frame_evidence(paint_completed_frame_count, qsg_atlas_render_frame_count);

    QJsonObject renderer;
    vnm_terminal::diagnostics::append_renderer_metrics_json(surface, renderer);

    QJsonObject qsg_atlas;
    vnm_terminal::diagnostics::append_atlas_metrics_json(surface, qsg_atlas);

    QJsonObject render_invalidation;
    vnm_terminal::diagnostics::append_render_invalidation_metrics_json(
        surface,
        render_invalidation);

    QJsonObject backend_drain;
    vnm_terminal::diagnostics::append_backend_drain_metrics_json(surface, backend_drain);

    QJsonObject root;
    root.insert(QStringLiteral("schema"), QString::fromLatin1(k_runtime_metrics_schema));
    root.insert(QStringLiteral("elapsed_ms"), timing.app_elapsed_ms);
    if (app_result.has_value()) {
        root.insert(QStringLiteral("app_result"), *app_result);
    }
    root.insert(QStringLiteral("process_exit_code"), state.process_exit_code);
    root.insert(QStringLiteral("process_exit_reason"), enum_key(state.process_exit_reason));
    root.insert(QStringLiteral("backend_error_count"), state.backend_error_count);
    root.insert(QStringLiteral("output_seen"), state.output_seen);
    root.insert(QStringLiteral("process_exited"), state.process_exited);
    root.insert(QStringLiteral("timeout_expired"), state.timeout_expired);
    root.insert(
        QStringLiteral("paint_frames_per_second"),
        frames_per_second(paint_completed_frame_count, timing.app_elapsed_ms));
    root.insert(
        QStringLiteral("paint_frames_per_second_elapsed_basis"),
        QString::fromLatin1(k_runtime_frame_rate_elapsed_basis));
    root.insert(
        QStringLiteral("renderer_frame_evidence"),
        renderer_frame_evidence_json(frame_evidence, timing.app_elapsed_ms));
    root.insert(
        QStringLiteral("startup"),
        startup_metrics_json(state, frame_evidence));
    root.insert(QStringLiteral("profiling"), profiling_measurement_json(timing));
    root.insert(
        QStringLiteral("presentation"),
        presentation_metrics_json(presentation_metrics, timing.app_elapsed_ms));
    root.insert(QStringLiteral("surface_geometry"), surface_geometry_json(surface));
    root.insert(QStringLiteral("render_invalidation"), render_invalidation);
    root.insert(QStringLiteral("backend_drain"), backend_drain);
    root.insert(QStringLiteral("renderer"), renderer);
    root.insert(QStringLiteral("qsg_atlas"), qsg_atlas);

    return root;
}

QJsonObject metrics_timeline_sample_json(
    Metrics_timeline_sample_kind kind,
    int                          sample_index,
    const VNM_TerminalSurface&   surface,
    const Presentation_metrics_recorder&
                                 presentation_metrics,
    const Runtime_state&         state,
    const metrics_timing_t&      timing,
    std::optional<int>           app_result,
    int                          interval_ms)
{
    QJsonObject root;
    root.insert(QStringLiteral("schema"), QString::fromLatin1(k_metrics_timeline_sample_schema));
    root.insert(QStringLiteral("sample_index"), sample_index);
    root.insert(QStringLiteral("elapsed_ms"), timing.app_elapsed_ms);
    root.insert(
        QStringLiteral("kind"),
        QString::fromLatin1(metrics_timeline_sample_kind_text(kind)));
    root.insert(QStringLiteral("interval_ms"), interval_ms);
    root.insert(QStringLiteral("app_result_available"), app_result.has_value());
    root.insert(
        QStringLiteral("runtime_metrics"),
        terminal_metrics_json(
            surface,
            presentation_metrics,
            state,
            timing,
            app_result));
    return root;
}

} // namespace

bool Metrics_timeline_jsonl_writer::open(const QString& path, QString* out_error)
{
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *out_error = QStringLiteral("could not write metrics timeline JSONL %1: %2")
            .arg(path, m_file.errorString());
        return false;
    }

    m_sample_index = 0;
    return true;
}

bool Metrics_timeline_jsonl_writer::write_sample(
    Metrics_timeline_sample_kind kind,
    const VNM_TerminalSurface&   surface,
    const Presentation_metrics_recorder&
                                 presentation_metrics,
    const Runtime_state&         state,
    const metrics_timing_t&      timing,
    std::optional<int>           app_result,
    int                          interval_ms,
    QString*                     out_error)
{
    QByteArray json = QJsonDocument(
        metrics_timeline_sample_json(
            kind,
            m_sample_index,
            surface,
            presentation_metrics,
            state,
            timing,
            app_result,
            interval_ms))
            .toJson(QJsonDocument::Compact);
    json.append('\n');

    const qint64 written = m_file.write(json);
    if (written != json.size()) {
        *out_error = QStringLiteral("could not write metrics timeline JSONL %1: %2")
            .arg(m_file.fileName(), m_file.errorString());
        return false;
    }

    if (!m_file.flush()) {
        *out_error = QStringLiteral("could not flush metrics timeline JSONL %1: %2")
            .arg(m_file.fileName(), m_file.errorString());
        return false;
    }

    ++m_sample_index;
    return true;
}

bool write_metrics_json(
    const QString&              path,
    const VNM_TerminalSurface&  surface,
    const Presentation_metrics_recorder&
                                presentation_metrics,
    const Runtime_state&        state,
    const metrics_timing_t&     timing,
    int                         app_result,
    QString*                    out_error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *out_error = QStringLiteral("could not write metrics JSON %1: %2")
            .arg(path, file.errorString());
        return false;
    }

    const QByteArray json = QJsonDocument(
        terminal_metrics_json(
            surface,
            presentation_metrics,
            state,
            timing,
            app_result))
            .toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size()) {
        *out_error = QStringLiteral("could not write metrics JSON %1: %2")
            .arg(path, file.errorString());
        return false;
    }

    return true;
}

} // namespace vnm_terminal::terminal_app
