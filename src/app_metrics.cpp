#include "app_metrics.h"

#include "app_common.h"

#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/qsg_terminal_renderer.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QQuickWindow>
#include <QString>

#include <algorithm>
#include <cstdint>

namespace vnm_terminal::terminal_app {

namespace {

namespace term = vnm_terminal::internal;

template<typename Stats>
std::uint64_t renderer_text_resource_dirty_row_metric_value(const Stats& stats)
{
    if constexpr (requires { stats.text_resource_dirty_row_lookups; }) {
        return static_cast<std::uint64_t>(stats.text_resource_dirty_row_lookups);
    }
    else {
        return static_cast<std::uint64_t>(stats.text_resource_dirty_row_probes);
    }
}

template<typename Stats>
const char* renderer_text_resource_dirty_row_metric_name(const Stats&)
{
    if constexpr (requires(Stats stats) { stats.text_resource_dirty_row_lookups; }) {
        return "text_resource_dirty_row_lookups";
    }
    else {
        return "text_resource_dirty_row_probes";
    }
}

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
constexpr const char* k_paint_completed_frame_counter_path =
    "renderer.paint_completed_frames";
constexpr const char* k_qsg_atlas_render_frame_counter_path =
    "qsg_atlas.render_count";

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
    const term::terminal_renderer_cumulative_stats_t& cumulative_stats,
    const term::Qsg_atlas_frame_report&               atlas_report)
{
    if (atlas_report.render_count > 0U) {
        return {
            k_qsg_atlas_render_frame_counter_path,
            atlas_report.render_count,
        };
    }

    return {
        k_paint_completed_frame_counter_path,
        cumulative_stats.paint_completed_frames,
    };
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

QJsonObject atlas_buffer_summary_json(
    const term::Qsg_atlas_buffer_update_summary& summary)
{
    QJsonObject object;
    insert_json_counter(object, "rhi_frames_in_flight", summary.rhi_frames_in_flight);
    insert_json_counter(object, "rhi_frame_slot", summary.rhi_frame_slot);
    insert_json_counter(object, "instance_count", summary.instance_count);
    insert_json_counter(
        object,
        "active_instance_count",
        summary.active_instance_count);
    insert_json_counter(object, "instance_bytes", summary.instance_bytes);
    insert_json_counter(object, "buffer_bytes", summary.buffer_bytes);
    insert_json_counter(object, "dirty_rows", summary.dirty_rows);
    insert_json_counter(object, "seeded_slots", summary.seeded_slots);
    insert_json_counter(object, "full_uploads", summary.full_uploads);
    insert_json_counter(object, "partial_uploads", summary.partial_uploads);
    insert_json_counter(object, "uploaded_bytes", summary.uploaded_bytes);
    object.insert(QStringLiteral("full_upload"), summary.full_upload);
    object.insert(QStringLiteral("partial_upload"), summary.partial_upload);
    object.insert(QStringLiteral("skipped_upload"), summary.skipped_upload);
    object.insert(
        QStringLiteral("rotating_slot_seed_upload"),
        summary.rotating_slot_seed_upload);
    object.insert(
        QStringLiteral("buffer_recreated_upload"),
        summary.buffer_recreated_upload);
    object.insert(
        QStringLiteral("instance_layout_changed_upload"),
        summary.instance_layout_changed_upload);
    object.insert(QStringLiteral("full_repaint_upload"), summary.full_repaint_upload);
    object.insert(QStringLiteral("non_dirty_state_upload"), summary.non_dirty_state_upload);
    object.insert(QStringLiteral("row_stable_layout"), summary.row_stable_layout);
    return object;
}

QJsonObject glyph_coverage_counts_json(
    const term::Qsg_atlas_frame_build_summary& summary)
{
    const term::Glyph_coverage_counts& counts = summary.glyph_coverage;

    QJsonObject object;
    insert_json_counter(object, "grayscale_masks", counts.grayscale_masks);
    insert_json_counter(object, "lcd_rgb_masks", counts.lcd_rgb_masks);
    insert_json_counter(object, "lcd_bgr_masks", counts.lcd_bgr_masks);
    insert_json_counter(object, "color_images", counts.color_images);
    insert_json_counter(object, "ambiguous_images", counts.ambiguous_images);
    insert_json_counter(object, "unsupported_images", counts.unsupported_images);
    insert_json_counter(object, "missed_images", counts.missed_images);
    return object;
}

QJsonObject atlas_first_glyph_miss_json(
    const term::Qsg_atlas_glyph_miss_diagnostic& miss)
{
    QJsonObject object;
    object.insert(QStringLiteral("valid"), miss.valid);
    object.insert(
        QStringLiteral("cause"),
        QString::fromLatin1(term::qsg_atlas_glyph_miss_cause_name(miss.cause)));
    object.insert(
        QStringLiteral("coverage_kind"),
        QString::fromLatin1(
            term::qsg_atlas_glyph_coverage_kind_name(
                miss.image.coverage_kind)));
    object.insert(
        QStringLiteral("presentation"),
        QString::fromLatin1(
            term::qsg_atlas_glyph_image_presentation_name(
                miss.image.presentation)));
    object.insert(
        QStringLiteral("source_format"),
        static_cast<int>(miss.image.source_format));
    object.insert(QStringLiteral("source_width"), miss.image.source_size.width());
    object.insert(QStringLiteral("source_height"), miss.image.source_size.height());
    insert_json_counter(object, "glyph_index", miss.image.glyph_index);
    object.insert(QStringLiteral("fallback_face_id"), miss.image.fallback_face_id);
    insert_json_counter(object, "text_run_index", miss.image.text_run_index);
    insert_json_counter(object, "glyph_run_index", miss.image.glyph_run_index);
    insert_json_counter(
        object,
        "glyph_index_in_run",
        miss.image.glyph_index_in_run);
    insert_json_counter(
        object,
        "source_string_start",
        miss.image.source_string_start);
    insert_json_counter(object, "source_string_end", miss.image.source_string_end);
    object.insert(QStringLiteral("tile_width"), miss.tile_size.width());
    object.insert(QStringLiteral("tile_height"), miss.tile_size.height());
    insert_json_counter(object, "tile_bytes_per_line", miss.tile_bytes_per_line);
    insert_json_counter(object, "atlas_page_count", miss.atlas_page_count);
    insert_json_counter(object, "atlas_page_budget", miss.atlas_page_budget);
    object.insert(QStringLiteral("atlas_page_width"), miss.atlas_page_size.width());
    object.insert(
        QStringLiteral("atlas_page_height"),
        miss.atlas_page_size.height());
    return object;
}

QJsonObject atlas_capabilities_json(const term::Qsg_atlas_render_summary& summary)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("glyph_shader_package_available"),
        summary.glyph_shader_package_available);
    object.insert(
        QStringLiteral("dual_source_probe_shader_package_available"),
        summary.dual_source_probe_shader_package_available);
    object.insert(
        QStringLiteral("dual_source_blend_factors_available"),
        summary.dual_source_blend_factors_available);
    object.insert(
        QStringLiteral("dual_source_blend_factors_runtime_probe"),
        summary.dual_source_blend_factors_runtime_probe);
    return object;
}

QJsonObject atlas_render_summary_json(
    const term::Qsg_atlas_render_summary& summary)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("rect_buffer"),
        atlas_buffer_summary_json(summary.rect_buffer));
    object.insert(
        QStringLiteral("glyph_buffer"),
        atlas_buffer_summary_json(summary.glyph_buffer));
    insert_json_counter(
        object,
        "shaped_text_runs",
        summary.shaped_text_runs);
    insert_json_counter(
        object,
        "shaped_glyph_records",
        summary.shaped_glyph_records);
    insert_json_counter(
        object,
        "shaped_missing_string_indexes",
        summary.shaped_missing_string_indexes);
    insert_json_counter(
        object,
        "shaped_invalid_string_indexes",
        summary.shaped_invalid_string_indexes);
    insert_json_counter(
        object,
        "glyph_buffer_instances",
        summary.glyph_buffer_instances);
    insert_json_counter(
        object,
        "glyph_text_row_capacity",
        summary.glyph_text_row_capacity);
    insert_json_counter(
        object,
        "glyph_cursor_text_row_capacity",
        summary.glyph_cursor_text_row_capacity);
    insert_json_counter(
        object,
        "background_rects_before_coalescing",
        summary.background_rects_before_coalescing);
    insert_json_counter(
        object,
        "background_rects_after_coalescing",
        summary.background_rects_after_coalescing);
    insert_json_counter(
        object,
        "background_rects_coalesced",
        summary.background_rects_coalesced);
    insert_json_counter(object, "rect_draw_calls", summary.rect_draw_calls);
    insert_json_counter(object, "glyph_draw_calls", summary.glyph_draw_calls);
    insert_json_counter(
        object,
        "msdf_text_draw_calls",
        summary.msdf_text_draw_calls);
    insert_json_counter(object, "draw_calls", summary.draw_calls);
    object.insert(
        QStringLiteral("text_renderer_policy"),
        QString::fromLatin1(
            term::qsg_atlas_text_renderer_policy_name(
                summary.text_renderer_policy)));
    object.insert(
        QStringLiteral("effective_text_renderer"),
        QString::fromLatin1(
            term::qsg_atlas_text_renderer_kind_name(
                summary.effective_text_renderer)));
    object.insert(
        QStringLiteral("text_renderer_fallback_allowed"),
        summary.text_renderer_fallback_allowed);
    object.insert(
        QStringLiteral("text_renderer_fallback_used"),
        summary.text_renderer_fallback_used);
    object.insert(
        QStringLiteral("msdf_lcd_subpixel_order"),
        QString::fromLatin1(
            term::qsg_atlas_lcd_subpixel_order_name(
                summary.msdf_lcd_subpixel_order)));
    object.insert(
        QStringLiteral("msdf_lcd_text_enabled"),
        summary.msdf_lcd_text_enabled);
    insert_json_counter(object, "atlas_page_count", summary.atlas_page_count);
    insert_json_counter(object, "atlas_page_budget", summary.atlas_page_budget);
    insert_json_counter(object, "atlas_page_bytes", summary.atlas_page_bytes);
    insert_json_counter(object, "atlas_allocated_bytes", summary.atlas_allocated_bytes);
    insert_json_counter(object, "atlas_budget_bytes", summary.atlas_budget_bytes);
    insert_json_counter(object, "atlas_used_bytes", summary.atlas_used_bytes);
    insert_json_counter(object, "atlas_failed_inserts", summary.atlas_failed_inserts);
    object.insert(QStringLiteral("atlas_page_pressure"), summary.atlas_page_pressure);
    object.insert(
        QStringLiteral("coverage_texture_uploaded"),
        summary.coverage_texture_uploaded);
    object.insert(
        QStringLiteral("coverage_texture_skipped"),
        summary.coverage_texture_skipped);
    object.insert(
        QStringLiteral("full_dirty_range_reupload"),
        summary.full_dirty_range_reupload);
    object.insert(
        QStringLiteral("public_projection_full_reupload"),
        summary.public_projection_full_reupload);
    object.insert(QStringLiteral("scroll_full_reupload"), summary.scroll_full_reupload);
    object.insert(
        QStringLiteral("non_dirty_selection_invalidation"),
        summary.non_dirty_selection_invalidation);
    object.insert(
        QStringLiteral("non_dirty_cursor_invalidation"),
        summary.non_dirty_cursor_invalidation);
    object.insert(
        QStringLiteral("non_dirty_preedit_invalidation"),
        summary.non_dirty_preedit_invalidation);
    object.insert(
        QStringLiteral("non_dirty_options_invalidation"),
        summary.non_dirty_options_invalidation);
    object.insert(
        QStringLiteral("non_dirty_visual_bell_invalidation"),
        summary.non_dirty_visual_bell_invalidation);
    object.insert(QStringLiteral("font_epoch_invalidation"), summary.font_epoch_invalidation);
    return object;
}

QJsonObject atlas_producer_summary_json(
    const term::Qsg_atlas_producer_summary& summary)
{
    QJsonObject object;
    insert_json_counter(object, "text_runs_considered", summary.text_runs_considered);
    insert_json_counter(object, "text_runs_empty", summary.text_runs_empty);
    insert_json_counter(object, "shape_cache_lookups", summary.shape_cache_lookups);
    insert_json_counter(object, "shape_cache_hits", summary.shape_cache_hits);
    insert_json_counter(object, "shape_cache_misses", summary.shape_cache_misses);
    insert_json_counter(object, "shape_cache_inserts", summary.shape_cache_inserts);
    insert_json_counter(object, "shape_cache_pruned", summary.shape_cache_pruned);
    insert_json_counter(object, "shape_cache_entries", summary.shape_cache_entries);
    insert_json_counter(object, "shaped_runs_built", summary.shaped_runs_built);
    insert_json_counter(object, "shaped_runs_reused", summary.shaped_runs_reused);
    insert_json_counter(
        object,
        "shaped_glyph_records_built",
        summary.shaped_glyph_records_built);
    insert_json_counter(
        object,
        "shaped_glyph_records_reused",
        summary.shaped_glyph_records_reused);
    insert_json_counter(
        object,
        "presentation_run_scans",
        summary.presentation_run_scans);
    insert_json_counter(
        object,
        "presentation_source_scans",
        summary.presentation_source_scans);
    insert_json_counter(
        object,
        "presentation_fast_text_runs",
        summary.presentation_fast_text_runs);
    insert_json_counter(
        object,
        "presentation_emoji_runs",
        summary.presentation_emoji_runs);
    insert_json_counter(object, "slot_resolutions_built", summary.slot_resolutions_built);
    insert_json_counter(object, "slot_resolutions_reused", summary.slot_resolutions_reused);
    insert_json_counter(object, "simple_path_attempts", summary.simple_path_attempts);
    insert_json_counter(object, "simple_path_used", summary.simple_path_used);
    insert_json_counter(object, "simple_path_fallbacks", summary.simple_path_fallbacks);
    return object;
}

QJsonObject atlas_warm_lazy_summary_json(
    const term::Qsg_atlas_warm_lazy_summary& summary)
{
    QJsonObject object;
    object.insert(QStringLiteral("warm_completed"), summary.warm_completed);
    insert_json_counter(object, "warm_epoch", summary.warm_epoch);
    insert_json_counter(object, "warm_seed_strings", summary.warm_seed_strings);
    insert_json_counter(
        object,
        "warm_shaped_glyph_records",
        summary.warm_shaped_glyph_records);
    insert_json_counter(
        object,
        "warm_covered_glyph_records",
        summary.warm_covered_glyph_records);
    insert_json_counter(
        object,
        "warm_skipped_glyph_records",
        summary.warm_skipped_glyph_records);
    insert_json_counter(
        object,
        "warm_environment_skipped_glyph_records",
        summary.warm_environment_skipped_glyph_records);
    insert_json_counter(
        object,
        "warm_failed_glyph_records",
        summary.warm_failed_glyph_records);
    insert_json_counter(
        object,
        "warm_missing_string_indexes",
        summary.warm_missing_string_indexes);
    insert_json_counter(
        object,
        "warm_invalid_string_indexes",
        summary.warm_invalid_string_indexes);
    insert_json_counter(
        object,
        "warm_unsupported_images",
        summary.warm_unsupported_images);
    insert_json_counter(object, "warm_cache_hits", summary.warm_cache_hits);
    insert_json_counter(
        object,
        "warm_insert_attempts",
        summary.warm_insert_attempts);
    insert_json_counter(object, "warm_inserts", summary.warm_inserts);
    insert_json_counter(
        object,
        "warm_failed_inserts",
        summary.warm_failed_inserts);
    object.insert(QStringLiteral("warm_elapsed_ms"), summary.warm_elapsed_ms);
    object.insert(QStringLiteral("warm_page_pressure"), summary.warm_page_pressure);
    insert_json_counter(
        object,
        "lazy_insert_attempts",
        summary.lazy_insert_attempts);
    insert_json_counter(object, "lazy_inserts", summary.lazy_inserts);
    insert_json_counter(
        object,
        "lazy_failed_inserts",
        summary.lazy_failed_inserts);
    object.insert(QStringLiteral("lazy_elapsed_ms"), summary.lazy_elapsed_ms);
    insert_json_counter(
        object,
        "lazy_max_insert_us",
        summary.lazy_max_insert_us);
    insert_json_counter(object, "lazy_frames", summary.lazy_frames);
    insert_json_counter(object, "incomplete_frames", summary.incomplete_frames);
    return object;
}

QJsonObject atlas_msdf_text_metrics_json(
    const term::Qsg_atlas_render_summary& summary)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("renderer_enabled"),
        summary.msdf_text_renderer_enabled);
    object.insert(
        QStringLiteral("renderer_active"),
        summary.msdf_text_renderer_active);
    object.insert(
        QStringLiteral("atlas_built"), summary.msdf_text_atlas_built);
    object.insert(
        QStringLiteral("atlas_ready"), summary.msdf_text_atlas_ready);
    insert_json_counter(
        object, "draw_pixel_height", summary.msdf_text_pixel_height);
    insert_json_counter(
        object, "baked_pixel_height", summary.msdf_text_baked_pixel_height);
    insert_json_counter(object, "atlas_size", summary.msdf_text_atlas_size);
    object.insert(
        QStringLiteral("px_range"),
        static_cast<double>(summary.msdf_text_px_range));
    insert_json_counter(
        object, "atlas_generation", summary.msdf_text_atlas_generation);
    // Per-frame event flags.
    object.insert(QStringLiteral("cache_hit"), summary.msdf_text_cache_hit);
    object.insert(QStringLiteral("cache_miss"), summary.msdf_text_cache_miss);
    object.insert(
        QStringLiteral("baked_atlas_reused"),
        summary.msdf_text_baked_atlas_reused);
    object.insert(
        QStringLiteral("atlas_build_attempted"),
        summary.msdf_text_atlas_build_attempted);
    object.insert(
        QStringLiteral("atlas_build_succeeded"),
        summary.msdf_text_atlas_build_succeeded);
    object.insert(
        QStringLiteral("texture_uploaded"),
        summary.msdf_text_texture_uploaded);
    // Lifetime-cumulative counters: a zoom gesture needs these so a single
    // build/upload cannot be hidden by a later clean frame before capture.
    insert_json_counter(
        object,
        "atlas_build_attempts_total",
        summary.msdf_text_atlas_build_attempts_total);
    insert_json_counter(
        object,
        "atlas_build_successes_total",
        summary.msdf_text_atlas_build_successes_total);
    insert_json_counter(
        object,
        "atlas_texture_uploads_total",
        summary.msdf_text_atlas_texture_uploads_total);
    insert_json_counter(
        object,
        "baked_cache_hits_total",
        summary.msdf_text_baked_cache_hits_total);
    insert_json_counter(
        object,
        "baked_cache_misses_total",
        summary.msdf_text_baked_cache_misses_total);
    return object;
}

QJsonObject qsg_atlas_metrics_json(const term::Qsg_atlas_frame_report& report)
{
    QJsonObject object;
    object.insert(QStringLiteral("renderer"), QStringLiteral("atlas"));
    object.insert(
        QStringLiteral("text_renderer_policy"),
        QString::fromLatin1(
            term::qsg_atlas_text_renderer_policy_name(
                report.text_renderer_policy)));
    object.insert(
        QStringLiteral("effective_text_renderer"),
        QString::fromLatin1(
            term::qsg_atlas_text_renderer_kind_name(
                report.effective_text_renderer)));
    object.insert(
        QStringLiteral("text_renderer_fallback_allowed"),
        report.text_renderer_fallback_allowed);
    object.insert(
        QStringLiteral("text_renderer_fallback_used"),
        report.text_renderer_fallback_used);
    object.insert(
        QStringLiteral("msdf_lcd_subpixel_order"),
        QString::fromLatin1(
            term::qsg_atlas_lcd_subpixel_order_name(
                report.msdf_lcd_subpixel_order)));
    object.insert(
        QStringLiteral("msdf_lcd_text_enabled"),
        report.msdf_lcd_text_enabled);

    insert_json_counter(object, "capture_count", report.capture_count);
    insert_json_counter(object, "prepare_count", report.prepare_count);
    insert_json_counter(object, "render_count", report.render_count);
    insert_json_counter(object, "capture_sequence", report.capture_sequence);
    insert_json_counter(
        object,
        "captured_snapshot_sequence",
        report.captured_snapshot_sequence);
    insert_json_counter(object, "captured_font_epoch", report.captured_font_epoch);
    object.insert(QStringLiteral("command_buffer_non_null"), report.command_buffer_non_null);
    object.insert(QStringLiteral("render_target_non_null"), report.render_target_non_null);
    object.insert(QStringLiteral("rhi_non_null"), report.rhi_non_null);
    object.insert(QStringLiteral("drew"), report.drew);
    object.insert(QStringLiteral("coverage_texture_created"), report.coverage_texture_created);
    object.insert(QStringLiteral("coverage_upload_recorded"), report.coverage_upload_recorded);
    object.insert(QStringLiteral("raw_font_rasterized"), report.raw_font_rasterized);
    insert_json_counter(object, "rasterized_glyphs", report.rasterized_glyphs);
    insert_json_counter(object, "atlas_page_count", report.atlas_page_count);
    insert_json_counter(
        object,
        "max_glyph_instance_page",
        std::max(0, report.frame_build.max_glyph_instance_page));
    insert_json_counter(
        object,
        "snapped_origin_failures",
        report.frame_build.snapped_origin_failures);
    insert_json_counter(
        object,
        "glyph_missed_instances",
        report.frame_build.glyph_missed_instances);
    insert_json_counter(
        object,
        "glyph_coverage_failures",
        report.frame_build.glyph_coverage_failures);
    insert_json_counter(
        object,
        "glyph_atlas_insert_failures",
        report.frame_build.glyph_atlas_insert_failures);
    object.insert(
        QStringLiteral("coverage"),
        glyph_coverage_counts_json(report.frame_build));
    object.insert(
        QStringLiteral("first_glyph_miss"),
        atlas_first_glyph_miss_json(report.frame_build.first_glyph_miss));
    object.insert(
        QStringLiteral("sampler_mode"),
        QString::fromLatin1(
            term::qsg_atlas_sampler_mode_name(report.render.glyph_sampler_mode)));
    object.insert(
        QStringLiteral("capabilities"),
        atlas_capabilities_json(report.render));
    object.insert(
        QStringLiteral("producer"),
        atlas_producer_summary_json(report.producer));
    object.insert(
        QStringLiteral("warm_lazy"),
        atlas_warm_lazy_summary_json(report.warm_lazy));
    object.insert(QStringLiteral("buffer_upload"), atlas_render_summary_json(report.render));
    object.insert(
        QStringLiteral("msdf_text"),
        atlas_msdf_text_metrics_json(report.render));
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

template<typename Simple_content_stats>
void insert_renderer_simple_content_stats(
    QJsonObject&                  object,
    const Simple_content_stats&   stats)
{
    insert_json_counter(object, "cells_considered", stats.cells_considered);
    insert_json_counter(object, "eligible_cells", stats.eligible_cells);
    insert_json_counter(
        object,
        "eligible_after_all_gates_cells",
        stats.eligible_after_all_gates_cells);
    insert_json_counter(object, "rows_with_eligible_cells", stats.rows_with_eligible_cells);
    insert_json_counter(object, "styles_with_eligible_cells", stats.styles_with_eligible_cells);
    insert_json_counter(object, "dirty_eligible_cells", stats.dirty_eligible_cells);
    insert_json_counter(object, "clean_eligible_cells", stats.clean_eligible_cells);
    insert_json_counter(object, "text_category_empty_cells", stats.text_category_empty_cells);
    insert_json_counter(
        object,
        "text_category_printable_ascii_cells",
        stats.text_category_printable_ascii_cells);
    insert_json_counter(
        object,
        "text_category_other_ascii_cells",
        stats.text_category_other_ascii_cells);
    insert_json_counter(
        object,
        "text_category_non_ascii_cells",
        stats.text_category_non_ascii_cells);
    insert_json_counter(object, "route_none_cells", stats.route_none_cells);
    insert_json_counter(object, "route_fast_text_cells", stats.route_fast_text_cells);
    insert_json_counter(
        object,
        "route_qt_text_layout_cells",
        stats.route_qt_text_layout_cells);
    insert_json_counter(object, "route_fallback_cells", stats.route_fallback_cells);
    insert_json_counter(object, "rejection_none_cells", stats.rejection_none_cells);
    insert_json_counter(
        object,
        "rejection_empty_text_cells",
        stats.rejection_empty_text_cells);
    insert_json_counter(
        object,
        "rejection_invalid_grid_cells",
        stats.rejection_invalid_grid_cells);
    insert_json_counter(
        object,
        "rejection_invalid_position_cells",
        stats.rejection_invalid_position_cells);
    insert_json_counter(
        object,
        "rejection_invalid_style_id_cells",
        stats.rejection_invalid_style_id_cells);
    insert_json_counter(
        object,
        "rejection_wide_continuation_cells",
        stats.rejection_wide_continuation_cells);
    insert_json_counter(
        object,
        "rejection_invalid_display_width_cells",
        stats.rejection_invalid_display_width_cells);
    insert_json_counter(
        object,
        "rejection_invalid_text_encoding_cells",
        stats.rejection_invalid_text_encoding_cells);
    insert_json_counter(
        object,
        "rejection_invalid_text_width_cells",
        stats.rejection_invalid_text_width_cells);
    insert_json_counter(
        object,
        "rejection_multi_cell_text_cells",
        stats.rejection_multi_cell_text_cells);
    insert_json_counter(
        object,
        "rejection_non_printable_ascii_cells",
        stats.rejection_non_printable_ascii_cells);
    insert_json_counter(
        object,
        "rejection_non_ascii_text_cells",
        stats.rejection_non_ascii_text_cells);
    insert_json_counter(
        object,
        "rejection_decoration_cells",
        stats.rejection_decoration_cells);
    insert_json_counter(object, "rejection_hyperlink_cells", stats.rejection_hyperlink_cells);
}

template<typename Frame_stats>
void insert_renderer_frame_stats(
    QJsonObject&        object,
    const Frame_stats&  stats)
{
    QJsonObject simple_content;
    insert_renderer_simple_content_stats(simple_content, stats.simple_content);

    insert_json_counter(object, "visible_rows", stats.visible_rows);
    insert_json_counter(object, "dirty_rows", stats.dirty_rows);
    insert_json_counter(object, "full_dirty_rows", stats.full_dirty_rows);
    insert_json_counter(object, "cell_pass_input_cells", stats.cell_pass_input_cells);
    insert_json_counter(
        object,
        "cell_pass_classification_calls",
        stats.cell_pass_classification_calls);
    insert_json_counter(object, "dirty_row_lookup_count", stats.dirty_row_lookup_count);
    insert_json_counter(object, "cells_considered", stats.cells_considered);
    insert_json_counter(object, "cells_skipped_invalid", stats.cells_skipped_invalid);
    insert_json_counter(
        object,
        "cells_skipped_wide_continuation",
        stats.cells_skipped_wide_continuation);
    insert_json_counter(object, "cells_rendered", stats.cells_rendered);
    insert_json_counter(object, "text_cells_empty", stats.text_cells_empty);
    insert_json_counter(
        object,
        "text_cells_rendered_as_text",
        stats.text_cells_rendered_as_text);
    insert_json_counter(
        object,
        "text_cells_printable_ascii",
        stats.text_cells_printable_ascii);
    insert_json_counter(object, "text_cells_other_ascii", stats.text_cells_other_ascii);
    insert_json_counter(object, "text_cells_non_ascii", stats.text_cells_non_ascii);
    insert_json_counter(object, "text_cells_simple_ascii", stats.text_cells_simple_ascii);
    insert_json_counter(object, "text_cells_single_width", stats.text_cells_single_width);
    insert_json_counter(object, "text_cells_multi_width", stats.text_cells_multi_width);
    insert_json_counter(
        object,
        "text_cells_with_decorations",
        stats.text_cells_with_decorations);
    insert_json_counter(object, "text_cells_with_hyperlink", stats.text_cells_with_hyperlink);
    insert_json_counter(object, "text_style_changes", stats.text_style_changes);
    insert_json_counter(object, "text_distinct_styles", stats.text_distinct_styles);
    insert_json_counter(object, "background_rects_emitted", stats.background_rects_emitted);
    insert_json_counter(object, "selection_rects_emitted", stats.selection_rects_emitted);
    insert_json_counter(object, "graphic_rects_emitted", stats.graphic_rects_emitted);
    insert_json_counter(object, "graphic_arcs_emitted", stats.graphic_arcs_emitted);
    insert_json_counter(object, "text_runs_emitted", stats.text_runs_emitted);
    insert_json_counter(
        object,
        "cursor_text_runs_emitted",
        stats.cursor_text_runs_emitted);
    insert_json_counter(
        object,
        "decoration_rects_emitted",
        stats.decoration_rects_emitted);
    insert_json_counter(object, "cursor_rects_emitted", stats.cursor_rects_emitted);
    insert_json_counter(object, "overlay_rects_emitted", stats.overlay_rects_emitted);
    object.insert(QStringLiteral("simple_content"), simple_content);
}

void insert_text_layout_stats_json(
    QJsonObject&                                      object,
    const term::terminal_renderer_cumulative_stats_t& stats)
{
    insert_json_counter(object, "qt_text_layout_calls", stats.qt_text_layout_calls);
    insert_json_counter(
        object,
        "text_layout_runs_single_code_unit",
        stats.text_layout_runs_single_code_unit);
    insert_json_counter(
        object,
        "text_layout_runs_multi_code_unit",
        stats.text_layout_runs_multi_code_unit);
    insert_json_counter(
        object,
        "text_layout_runs_all_space",
        stats.text_layout_runs_all_space);
    insert_json_counter(
        object,
        "text_layout_runs_printable_ascii",
        stats.text_layout_runs_printable_ascii);
    insert_json_counter(
        object,
        "text_layout_runs_printable_ascii_with_space",
        stats.text_layout_runs_printable_ascii_with_space);
    insert_json_counter(
        object,
        "text_layout_runs_other_ascii",
        stats.text_layout_runs_other_ascii);
    insert_json_counter(
        object,
        "text_layout_runs_non_ascii",
        stats.text_layout_runs_non_ascii);
    insert_json_counter(
        object,
        "text_layout_runs_clipped",
        stats.text_layout_runs_clipped);
    insert_json_counter(
        object,
        "text_layout_runs_ascii_layout_font",
        stats.text_layout_runs_ascii_layout_font);
    insert_json_counter(
        object,
        "text_layout_runs_force_blended_order",
        stats.text_layout_runs_force_blended_order);
    insert_json_counter(
        object,
        "text_layout_runs_with_hyperlink",
        stats.text_layout_runs_with_hyperlink);
    insert_json_counter(
        object,
        "text_layout_runs_with_decoration",
        stats.text_layout_runs_with_decoration);
    insert_json_counter(
        object,
        "text_layout_runs_mixed_ascii_non_ascii",
        stats.text_layout_runs_mixed_ascii_non_ascii);
    insert_json_counter(
        object,
        "text_layout_runs_pure_non_ascii",
        stats.text_layout_runs_pure_non_ascii);
    insert_json_counter(
        object,
        "text_layout_runs_plain_unclipped",
        stats.text_layout_runs_plain_unclipped);
    insert_json_counter(
        object,
        "text_layout_runs_plain_unclipped_ascii_font",
        stats.text_layout_runs_plain_unclipped_ascii_font);
    insert_json_counter(
        object,
        "text_layout_runs_all_space_plain_unclipped",
        stats.text_layout_runs_all_space_plain_unclipped);
    insert_json_counter(
        object,
        "text_layout_runs_printable_ascii_plain_unclipped",
        stats.text_layout_runs_printable_ascii_plain_unclipped);
    insert_json_counter(
        object,
        "text_layout_runs_non_ascii_plain_unclipped",
        stats.text_layout_runs_non_ascii_plain_unclipped);
    insert_json_counter(
        object,
        "text_layout_runs_mixed_ascii_non_ascii_plain_unclipped",
        stats.text_layout_runs_mixed_ascii_non_ascii_plain_unclipped);
    insert_json_counter(
        object,
        "text_layout_runs_pure_non_ascii_plain_unclipped",
        stats.text_layout_runs_pure_non_ascii_plain_unclipped);
    insert_json_counter(
        object,
        "text_layout_runs_fast_space_candidate",
        stats.text_layout_runs_fast_space_candidate);
    insert_json_counter(
        object,
        "text_layout_runs_fast_ascii_candidate",
        stats.text_layout_runs_fast_ascii_candidate);
    insert_json_counter(
        object,
        "text_layout_runs_fast_ascii_no_space_candidate",
        stats.text_layout_runs_fast_ascii_no_space_candidate);
    insert_json_counter(
        object,
        "text_layout_runs_fast_ascii_single_candidate",
        stats.text_layout_runs_fast_ascii_single_candidate);
    insert_json_counter(
        object,
        "text_layout_runs_fast_ascii_multi_candidate",
        stats.text_layout_runs_fast_ascii_multi_candidate);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_screened",
        stats.text_ascii_replacement_runs_screened);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_eligible",
        stats.text_ascii_replacement_runs_eligible);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_attempted",
        stats.text_ascii_replacement_runs_attempted);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_trusted_fast_path",
        stats.text_ascii_replacement_runs_trusted_fast_path);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_succeeded",
        stats.text_ascii_replacement_runs_succeeded);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_all_space_succeeded",
        stats.text_ascii_replacement_runs_all_space_succeeded);
    if constexpr (requires { stats.text_ascii_replacement_add_glyphs_calls; }) {
        insert_json_counter(
            object,
            "text_ascii_replacement_add_glyphs_calls",
            stats.text_ascii_replacement_add_glyphs_calls);
    }
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_fallback",
        stats.text_ascii_replacement_runs_fallback);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_clipped",
        stats.text_ascii_replacement_runs_rejected_clipped);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_force_blended_order",
        stats.text_ascii_replacement_runs_rejected_force_blended_order);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_decoration",
        stats.text_ascii_replacement_runs_rejected_decoration);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_hyperlink",
        stats.text_ascii_replacement_runs_rejected_hyperlink);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_non_printable_ascii",
        stats.text_ascii_replacement_runs_rejected_non_printable_ascii);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_non_ascii",
        stats.text_ascii_replacement_runs_rejected_non_ascii);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_geometry",
        stats.text_ascii_replacement_runs_rejected_geometry);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_unsupported_font",
        stats.text_ascii_replacement_runs_rejected_unsupported_font);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_internal_node",
        stats.text_ascii_replacement_runs_rejected_internal_node);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_glyph_mapping",
        stats.text_ascii_replacement_runs_rejected_glyph_mapping);
    insert_json_counter(object, "text_layout_code_units", stats.text_layout_code_units);
    insert_json_counter(
        object,
        "text_layout_space_code_units",
        stats.text_layout_space_code_units);
    insert_json_counter(
        object,
        "text_layout_printable_ascii_code_units",
        stats.text_layout_printable_ascii_code_units);
    insert_json_counter(
        object,
        "text_layout_other_ascii_code_units",
        stats.text_layout_other_ascii_code_units);
    insert_json_counter(
        object,
        "text_layout_non_ascii_code_units",
        stats.text_layout_non_ascii_code_units);
    insert_json_counter(
        object,
        "text_layout_plain_unclipped_code_units",
        stats.text_layout_plain_unclipped_code_units);
    insert_json_counter(
        object,
        "text_layout_all_space_plain_unclipped_code_units",
        stats.text_layout_all_space_plain_unclipped_code_units);
    insert_json_counter(
        object,
        "text_layout_printable_ascii_plain_unclipped_code_units",
        stats.text_layout_printable_ascii_plain_unclipped_code_units);
    insert_json_counter(
        object,
        "text_layout_non_ascii_plain_unclipped_code_units",
        stats.text_layout_non_ascii_plain_unclipped_code_units);
    insert_json_counter(
        object,
        "text_layout_fast_space_candidate_code_units",
        stats.text_layout_fast_space_candidate_code_units);
    insert_json_counter(
        object,
        "text_layout_fast_ascii_candidate_code_units",
        stats.text_layout_fast_ascii_candidate_code_units);
    insert_json_counter(
        object,
        "text_ascii_replacement_code_units_screened",
        stats.text_ascii_replacement_code_units_screened);
    insert_json_counter(
        object,
        "text_ascii_replacement_code_units_eligible",
        stats.text_ascii_replacement_code_units_eligible);
    insert_json_counter(
        object,
        "text_ascii_replacement_code_units_attempted",
        stats.text_ascii_replacement_code_units_attempted);
    insert_json_counter(
        object,
        "text_ascii_replacement_code_units_trusted_fast_path",
        stats.text_ascii_replacement_code_units_trusted_fast_path);
    insert_json_counter(
        object,
        "text_ascii_replacement_code_units_succeeded",
        stats.text_ascii_replacement_code_units_succeeded);
    insert_json_counter(
        object,
        "text_ascii_replacement_code_units_fallback",
        stats.text_ascii_replacement_code_units_fallback);
}

QJsonObject terminal_metrics_json(
    const VNM_TerminalSurface&  surface,
    const Runtime_state&        state,
    const metrics_timing_t&     timing,
    int                         app_result)
{
    const term::terminal_renderer_cumulative_stats_t cumulative_stats =
        term::VNM_TerminalSurface_render_bridge::cumulative_renderer_stats(surface);
    const term::Qsg_atlas_frame_report atlas_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const renderer_frame_evidence_t frame_evidence =
        renderer_frame_evidence(cumulative_stats, atlas_report);

    QJsonObject frame;
    insert_renderer_frame_stats(frame, cumulative_stats.frame);

    QJsonObject renderer;
    insert_json_counter(renderer, "frames_published", cumulative_stats.frames_published);
    insert_json_counter(
        renderer,
        "paint_completed_frames",
        cumulative_stats.paint_completed_frames);
    insert_json_counter(renderer, "root_reused_frames", cumulative_stats.root_reused_frames);
    insert_json_counter(renderer, "text_content_rebuilds", cumulative_stats.text_content_rebuilds);
    insert_json_counter(renderer, "text_content_reused", cumulative_stats.text_content_reused);
    insert_json_counter(renderer, "text_content_removed", cumulative_stats.text_content_removed);
    insert_json_counter(renderer, "text_content_failures", cumulative_stats.text_content_failures);
    insert_json_counter(
        renderer,
        "atlas_work_created",
        cumulative_stats.atlas_work_created);
    if constexpr (requires { cumulative_stats.atlas_work_reused; }) {
        insert_json_counter(
            renderer,
            "atlas_work_reused",
            cumulative_stats.atlas_work_reused);
    }
    insert_json_counter(
        renderer,
        "text_cache_entry_child_nodes_cleared_for_replacement",
        cumulative_stats.text_cache_entry_child_nodes_cleared_for_replacement);
    insert_json_counter(
        renderer,
        "text_cache_entry_child_nodes_cleared_for_removal",
        cumulative_stats.text_cache_entry_child_nodes_cleared_for_removal);
    insert_json_counter(
        renderer,
        "text_cache_entry_max_child_nodes_cleared",
        cumulative_stats.text_cache_entry_max_child_nodes_cleared);
    insert_json_counter(renderer, "route_fast_text_cells", cumulative_stats.route_fast_text_cells);
    insert_json_counter(
        renderer,
        "route_qt_text_layout_runs",
        cumulative_stats.route_qt_text_layout_runs);
    insert_json_counter(renderer, "route_fallback_cells", cumulative_stats.route_fallback_cells);
    insert_text_layout_stats_json(renderer, cumulative_stats);
    if constexpr (requires { cumulative_stats.text_resource_descriptor_builds; }) {
        insert_json_counter(
            renderer,
            "text_resource_descriptor_builds",
            cumulative_stats.text_resource_descriptor_builds);
    }
    if constexpr (requires { cumulative_stats.text_resource_descriptor_builds_avoided; }) {
        insert_json_counter(
            renderer,
            "text_resource_descriptor_builds_avoided",
            cumulative_stats.text_resource_descriptor_builds_avoided);
    }
    insert_json_counter(renderer, "qsg_nodes_created", cumulative_stats.qsg_nodes_created);
    insert_json_counter(renderer, "qsg_nodes_replaced", cumulative_stats.qsg_nodes_replaced);
    insert_json_counter(renderer, "qsg_nodes_destroyed", cumulative_stats.qsg_nodes_destroyed);
    insert_json_counter(
        renderer,
        "background_qsg_nodes_created",
        cumulative_stats.background_qsg_nodes_created);
    insert_json_counter(
        renderer,
        "background_qsg_nodes_replaced",
        cumulative_stats.background_qsg_nodes_replaced);
    insert_json_counter(
        renderer,
        "background_qsg_nodes_destroyed",
        cumulative_stats.background_qsg_nodes_destroyed);
    insert_json_counter(
        renderer,
        "text_groups_considered",
        cumulative_stats.text_groups_considered);
    insert_json_counter(renderer, "text_groups_dirty", cumulative_stats.text_groups_dirty);
    insert_json_counter(renderer, "text_groups_clean", cumulative_stats.text_groups_clean);
    insert_json_counter(
        renderer,
        "text_clean_reuse_skips",
        cumulative_stats.text_clean_reuse_skips);
    insert_json_counter(
        renderer,
        "text_resource_descriptor_reuses",
        cumulative_stats.text_resource_descriptor_reuses);
    insert_json_counter(renderer, "text_key_match_reuses", cumulative_stats.text_key_match_reuses);
    insert_json_counter(renderer, "text_key_builds", cumulative_stats.text_key_builds);
    insert_json_counter(renderer, "text_key_bytes", cumulative_stats.text_key_bytes);
    insert_json_counter(renderer, "rect_key_builds", cumulative_stats.rect_key_builds);
    insert_json_counter(renderer, "rect_key_bytes", cumulative_stats.rect_key_bytes);
    insert_json_counter(renderer, "cache_key_builds", cumulative_stats.cache_key_builds);
    insert_json_counter(renderer, "cache_key_bytes", cumulative_stats.cache_key_bytes);
    insert_json_counter(
        renderer,
        "text_dirty_row_ranges",
        cumulative_stats.text_dirty_row_ranges);
    insert_json_counter(renderer, "text_dirty_rows", cumulative_stats.text_dirty_rows);
    renderer.insert(
        QString::fromLatin1(
            renderer_text_resource_dirty_row_metric_name(cumulative_stats)),
        QString::number(
            static_cast<qulonglong>(
                renderer_text_resource_dirty_row_metric_value(cumulative_stats))));
    insert_json_counter(
        renderer,
        "text_runs_considered",
        cumulative_stats.text_runs_considered);
    insert_json_counter(
        renderer,
        "text_coalescing_candidate_groups",
        cumulative_stats.text_coalescing_candidate_groups);
    insert_json_counter(
        renderer,
        "text_coalescing_enabled_groups",
        cumulative_stats.text_coalescing_enabled_groups);
    insert_json_counter(
        renderer,
        "text_resource_rows_with_runs",
        cumulative_stats.text_resource_rows_with_runs);
    insert_json_counter(
        renderer,
        "text_resource_max_runs_after_coalescing_per_row",
        cumulative_stats.text_resource_max_runs_after_coalescing_per_row);
    insert_json_counter(
        renderer,
        "text_resource_runs_before_coalescing",
        cumulative_stats.text_resource_runs_before_coalescing);
    insert_json_counter(
        renderer,
        "text_resource_runs_after_coalescing",
        cumulative_stats.text_resource_runs_after_coalescing);
    insert_json_counter(
        renderer,
        "text_dirty_descriptor_identical_rows",
        cumulative_stats.text_dirty_descriptor_identical_rows);
    insert_json_counter(
        renderer,
        "text_dirty_rows_rebuilt",
        cumulative_stats.text_dirty_rows_rebuilt);
    insert_json_counter(
        renderer,
        "text_clean_rows_rebuilt",
        cumulative_stats.text_clean_rows_rebuilt);
    insert_json_counter(
        renderer,
        "rect_resource_rects_before_coalescing",
        cumulative_stats.rect_resource_rects_before_coalescing);
    insert_json_counter(
        renderer,
        "rect_resource_rects_after_coalescing",
        cumulative_stats.rect_resource_rects_after_coalescing);
    insert_json_counter(
        renderer,
        "background_row_rects_before_coalescing",
        cumulative_stats.background_row_rects_before_coalescing);
    insert_json_counter(
        renderer,
        "background_row_rects_after_coalescing",
        cumulative_stats.background_row_rects_after_coalescing);
    insert_json_counter(
        renderer,
        "background_batched_rects",
        cumulative_stats.background_batched_rects);
    insert_json_counter(
        renderer,
        "background_batched_vertices",
        cumulative_stats.background_batched_vertices);
    insert_json_counter(
        renderer,
        "selection_batched_rects",
        cumulative_stats.selection_batched_rects);
    insert_json_counter(
        renderer,
        "selection_batched_vertices",
        cumulative_stats.selection_batched_vertices);
    insert_json_counter(
        renderer,
        "graphic_batched_rects",
        cumulative_stats.graphic_batched_rects);
    insert_json_counter(
        renderer,
        "graphic_batched_vertices",
        cumulative_stats.graphic_batched_vertices);
    insert_json_counter(
        renderer,
        "decoration_batched_rects",
        cumulative_stats.decoration_batched_rects);
    insert_json_counter(
        renderer,
        "decoration_batched_vertices",
        cumulative_stats.decoration_batched_vertices);
    insert_json_counter(
        renderer,
        "text_cache_entries_created",
        cumulative_stats.text_cache_entries_created);
    insert_json_counter(
        renderer,
        "text_cache_entries_replaced",
        cumulative_stats.text_cache_entries_replaced);
    insert_json_counter(
        renderer,
        "text_cache_entries_removed",
        cumulative_stats.text_cache_entries_removed);
    insert_json_counter(
        renderer,
        "text_wrapper_order_rebuilds",
        cumulative_stats.text_wrapper_order_rebuilds);
    insert_json_counter(
        renderer,
        "background_layer_rebuilds",
        cumulative_stats.background_layer_rebuilds);
    insert_json_counter(
        renderer,
        "selection_layer_rebuilds",
        cumulative_stats.selection_layer_rebuilds);
    insert_json_counter(
        renderer,
        "graphic_layer_rebuilds",
        cumulative_stats.graphic_layer_rebuilds);
    insert_json_counter(
        renderer,
        "decoration_layer_rebuilds",
        cumulative_stats.decoration_layer_rebuilds);
    insert_json_counter(
        renderer,
        "cursor_layer_rebuilds",
        cumulative_stats.cursor_layer_rebuilds);
    insert_json_counter(
        renderer,
        "cursor_text_layer_rebuilds",
        cumulative_stats.cursor_text_layer_rebuilds);
    insert_json_counter(renderer, "overlay_layer_rebuilds", cumulative_stats.overlay_layer_rebuilds);
    insert_json_counter(renderer, "row_cache_hits", cumulative_stats.row_cache_hits);
    insert_json_counter(
        renderer,
        "row_cache_clean_skips",
        cumulative_stats.row_cache_clean_skips);
    insert_json_counter(
        renderer,
        "background_rows_rebuilt",
        cumulative_stats.background_rows_rebuilt);
    insert_json_counter(
        renderer,
        "background_rows_reused",
        cumulative_stats.background_rows_reused);
    insert_json_counter(
        renderer,
        "background_row_clean_reuse_skips",
        cumulative_stats.background_row_clean_reuse_skips);
    insert_json_counter(
        renderer,
        "background_rows_removed",
        cumulative_stats.background_rows_removed);
    insert_json_counter(
        renderer,
        "background_row_cache_fallbacks",
        cumulative_stats.background_row_cache_fallbacks);
    insert_json_counter(
        renderer,
        "selection_rows_rebuilt",
        cumulative_stats.selection_rows_rebuilt);
    insert_json_counter(
        renderer,
        "selection_rows_reused",
        cumulative_stats.selection_rows_reused);
    insert_json_counter(
        renderer,
        "selection_row_clean_reuse_skips",
        cumulative_stats.selection_row_clean_reuse_skips);
    insert_json_counter(
        renderer,
        "selection_rows_removed",
        cumulative_stats.selection_rows_removed);
    insert_json_counter(
        renderer,
        "selection_row_cache_fallbacks",
        cumulative_stats.selection_row_cache_fallbacks);
    insert_json_counter(
        renderer,
        "decoration_rows_rebuilt",
        cumulative_stats.decoration_rows_rebuilt);
    insert_json_counter(
        renderer,
        "decoration_rows_reused",
        cumulative_stats.decoration_rows_reused);
    insert_json_counter(
        renderer,
        "decoration_row_clean_reuse_skips",
        cumulative_stats.decoration_row_clean_reuse_skips);
    insert_json_counter(
        renderer,
        "decoration_rows_removed",
        cumulative_stats.decoration_rows_removed);
    insert_json_counter(
        renderer,
        "decoration_row_cache_fallbacks",
        cumulative_stats.decoration_row_cache_fallbacks);
    insert_json_counter(
        renderer,
        "graphic_rect_rows_rebuilt",
        cumulative_stats.graphic_rect_rows_rebuilt);
    insert_json_counter(
        renderer,
        "graphic_rect_rows_reused",
        cumulative_stats.graphic_rect_rows_reused);
    insert_json_counter(
        renderer,
        "graphic_rect_row_clean_reuse_skips",
        cumulative_stats.graphic_rect_row_clean_reuse_skips);
    insert_json_counter(
        renderer,
        "graphic_rect_rows_removed",
        cumulative_stats.graphic_rect_rows_removed);
    insert_json_counter(
        renderer,
        "graphic_rect_row_cache_fallbacks",
        cumulative_stats.graphic_rect_row_cache_fallbacks);
    insert_json_counter(
        renderer,
        "graphic_arc_rows_rebuilt",
        cumulative_stats.graphic_arc_rows_rebuilt);
    insert_json_counter(
        renderer,
        "graphic_arc_rows_reused",
        cumulative_stats.graphic_arc_rows_reused);
    insert_json_counter(
        renderer,
        "graphic_arc_row_clean_reuse_skips",
        cumulative_stats.graphic_arc_row_clean_reuse_skips);
    insert_json_counter(
        renderer,
        "graphic_arc_rows_removed",
        cumulative_stats.graphic_arc_rows_removed);
    insert_json_counter(
        renderer,
        "graphic_arc_row_cache_fallbacks",
        cumulative_stats.graphic_arc_row_cache_fallbacks);
    renderer.insert(QStringLiteral("frame"), frame);

    QJsonObject root;
    root.insert(QStringLiteral("schema"), QStringLiteral("vnm_terminal_runtime_metrics_v2"));
    root.insert(QStringLiteral("elapsed_ms"), timing.app_elapsed_ms);
    root.insert(QStringLiteral("app_result"), app_result);
    root.insert(QStringLiteral("process_exit_code"), state.process_exit_code);
    root.insert(QStringLiteral("process_exit_reason"), enum_key(state.process_exit_reason));
    root.insert(QStringLiteral("backend_error_count"), state.backend_error_count);
    root.insert(QStringLiteral("output_seen"), state.output_seen);
    root.insert(QStringLiteral("process_exited"), state.process_exited);
    root.insert(QStringLiteral("timeout_expired"), state.timeout_expired);
    root.insert(
        QStringLiteral("paint_frames_per_second"),
        frames_per_second(cumulative_stats.paint_completed_frames, timing.app_elapsed_ms));
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
    root.insert(QStringLiteral("surface_geometry"), surface_geometry_json(surface));
    root.insert(QStringLiteral("renderer"), renderer);
    root.insert(QStringLiteral("qsg_atlas"), qsg_atlas_metrics_json(atlas_report));

    return root;
}

} // namespace

bool write_metrics_json(
    const QString&              path,
    const VNM_TerminalSurface&  surface,
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
