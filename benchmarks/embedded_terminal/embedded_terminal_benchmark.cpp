#include "vnm_terminal/internal/backend_contract.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/session_contract.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSaveFile>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QWheelEvent>
#include <QtGlobal>
#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(Q_OS_LINUX)
#include <unistd.h>
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

constexpr int k_default_iterations     = 3;
constexpr int k_default_warmup         = 1;
constexpr int k_default_rows           = 24;
constexpr int k_default_columns        = 80;
constexpr int k_default_window_width   = 800;
constexpr int k_default_window_height  = 480;
constexpr int k_max_grid_rows          = 256;
constexpr int k_max_grid_columns       = 512;
constexpr int k_max_window_axis        = 8192;
constexpr int k_render_timeout_ms      = 3000;
constexpr int k_scroll_burst_snapshots = 4;

constexpr int k_surface_session_wheel_burst_events            = 4;
constexpr int k_surface_session_viewport_pan_wheel_events     = 1;
constexpr int k_surface_session_viewport_pan_steps_per_event  = 6;
constexpr int k_surface_session_plain_wheel_lines_per_event   = 3;
constexpr int k_surface_session_min_output_lines              = 160;
constexpr int k_surface_session_sustained_lines_per_attempt   = 64;
constexpr int k_surface_session_scrollback_limit              = 32;
constexpr int k_surface_session_scrollback_lines_per_attempt  = 96;
constexpr int k_surface_session_alternate_churns_per_attempt  = 4;
constexpr int k_surface_session_paste_lines_per_attempt       = 24;
constexpr int k_surface_session_output_high_water_chunks      = 64;
constexpr int k_surface_session_output_high_water_chunk_bytes = 1024;
constexpr int k_surface_session_write_high_water_bytes        = 64 * 1024;

constexpr int k_schema_version              = 16;
constexpr int k_profile_schema_version      = 2;
constexpr int k_profile_text_format         = 2;
constexpr int k_flat_rect_vertices_per_rect = 6;

const QString k_snapshot_bridge_source_mode    = QStringLiteral("snapshot_bridge");
const QString k_snapshot_bridge_execution_mode = QStringLiteral("renderer_surface_snapshot");
const QString k_surface_session_source_mode    = QStringLiteral("surface_session");
const QString k_surface_session_wheel_execution_mode =
    QStringLiteral("surface_session_wheel_event_burst");
const QString k_surface_session_viewport_pan_execution_mode =
    QStringLiteral("surface_session_viewport_pan");
const QString k_surface_session_sustained_output_execution_mode =
    QStringLiteral("surface_session_sustained_output");
const QString k_surface_session_scrollback_limit_execution_mode = QStringLiteral(
    "surface_session_scrollback_growth_limit");
const QString k_surface_session_alternate_churn_execution_mode = QStringLiteral(
    "surface_session_alternate_screen_churn");
const QString k_surface_session_paste_write_execution_mode =
    QStringLiteral("surface_session_paste_write_reservation");
const QString k_surface_session_output_high_water_execution_mode = QStringLiteral(
    "surface_session_output_high_water_pressure");
const QString k_output_high_water_queue_pressure_semantics = QStringLiteral(
    "output_queue_byte_high_water_observed_by_backend_pause");
const QString k_surface_session_write_high_water_execution_mode = QStringLiteral(
    "surface_session_write_high_water_reservation_pressure");
const QString k_render_expected_semantics = QStringLiteral(
    "scene_graph_render_expected_for_each_completed_measured_attempt");
const QString k_no_render_expected_semantics =
    QStringLiteral("backend_write_only_no_scene_graph_render_expected");
const QString k_latency_semantics = QStringLiteral(
    "observed_action_to_scene_graph_update_completion_excludes_readback");
const QString k_elapsed_semantics = QStringLiteral(
    "observed_attempt_wall_time_includes_benchmark_readback_and_pixel_checks");
const QString k_memory_sample_semantics = QStringLiteral(
    "best_effort_process_resident_memory_bytes_sampled_after_completed_measured_attempts");
const QString k_memory_unsupported_semantics = QStringLiteral(
    "unsupported_platform_or_unavailable_sampler_emits_sample_count_zero");
const QString k_per_consumed_update_counter_semantics = QStringLiteral(
    "counter_total_divided_by_bridge_consumed_updates_delta_when_nonzero");
const QString k_per_consumed_update_timing_semantics = QStringLiteral(
    "scene_graph_update_latency_median_per_attempt_exact_for_single_update_attempts_"
    "approximate_divided_by_mean_consumed_updates_for_multi_update_attempts");
const QString k_latency_normalization_unavailable =
    QStringLiteral("unavailable_no_consumed_updates");
const QString k_latency_normalization_single_update = QStringLiteral(
    "median_per_attempt_is_per_update_single_consumed_update_attempts");
const QString k_latency_normalization_multi_update_approximate = QStringLiteral(
    "approximate_divides_median_per_attempt_by_mean_consumed_updates_per_attempt");
const QString k_profile_time_unit        = QStringLiteral("ns");
const QString k_profile_thread_semantics = QStringLiteral("separate_thread_trees");
const QColor  k_surface_background       = QColor(16, 20, 24);

struct grid_size_t
{
    int                    rows    = k_default_rows;
    int                    columns = k_default_columns;
};

struct sample_summary_t
{
    int                    sample_count = 0;
    qint64                 total        = 0;
    qint64                 min          = 0;
    qint64                 median       = 0;
    qint64                 p95          = 0;
    qint64                 max          = 0;
};

struct memory_summary_t
{
    QString                status    = QStringLiteral("unsupported");
    QString                platform;
    QString                metric    = QStringLiteral("resident_set_bytes");
    QString                semantics = k_memory_sample_semantics;
    sample_summary_t       resident_bytes;
};

struct renderer_totals_t
{
    qint64                 text_content_rebuilds                                = 0;
    qint64                 text_content_reused                                  = 0;
    qint64                 text_content_removed                                 = 0;
    qint64                 text_content_failures                                = 0;
    qint64                 text_leaf_nodes_created                              = 0;
    qint64                 text_cache_entry_child_nodes_cleared_for_replacement = 0;
    qint64                 text_cache_entry_child_nodes_cleared_for_removal     = 0;
    qint64                 text_cache_entry_max_child_nodes_cleared             = 0;
    qint64                 text_clean_reuse_skips                               = 0;
    qint64                 text_resource_descriptor_reuses                      = 0;
    qint64                 text_coalescing_candidate_groups                     = 0;
    qint64                 text_coalescing_enabled_groups                       = 0;
    qint64                 text_resource_runs_before_coalescing                 = 0;
    qint64                 text_resource_runs_after_coalescing                  = 0;
    qint64                 simple_content_cells_considered                      = 0;
    qint64                 simple_content_eligible_cells                        = 0;
    qint64                 simple_content_eligible_after_all_gates_cells        = 0;
    qint64                 simple_content_rows_with_eligible_cells              = 0;
    qint64                 simple_content_styles_with_eligible_cells            = 0;
    qint64                 simple_content_dirty_eligible_cells                  = 0;
    qint64                 simple_content_clean_eligible_cells                  = 0;
    qint64                 simple_content_text_category_empty_cells             = 0;
    qint64                 simple_content_text_category_printable_ascii_cells   = 0;
    qint64                 simple_content_text_category_other_ascii_cells       = 0;
    qint64                 simple_content_text_category_non_ascii_cells         = 0;
    qint64                 simple_content_route_none_cells                      = 0;
    qint64                 simple_content_route_fast_text_cells                 = 0;
    qint64                 simple_content_route_qt_text_layout_cells            = 0;
    qint64                 simple_content_route_graphic_geometry_cells          = 0;
    qint64                 simple_content_route_fallback_cells                  = 0;
    qint64                 simple_content_rejection_none_cells                  = 0;
    qint64                 simple_content_rejection_empty_text_cells            = 0;
    qint64                 simple_content_rejection_invalid_grid_cells          = 0;
    qint64                 simple_content_rejection_invalid_position_cells      = 0;
    qint64                 simple_content_rejection_invalid_style_id_cells      = 0;
    qint64                 simple_content_rejection_wide_continuation_cells     = 0;
    qint64                 simple_content_rejection_invalid_display_width_cells = 0;
    qint64                 simple_content_rejection_invalid_text_encoding_cells = 0;
    qint64                 simple_content_rejection_invalid_text_width_cells    = 0;
    qint64                 simple_content_rejection_multi_cell_text_cells       = 0;
    qint64                 simple_content_rejection_non_printable_ascii_cells   = 0;
    qint64                 simple_content_rejection_non_ascii_text_cells        = 0;
    qint64                 simple_content_rejection_decoration_cells            = 0;
    qint64                 simple_content_rejection_hyperlink_cells             = 0;
    qint64                 simple_content_rejection_terminal_graphic_cells      = 0;
    qint64                 route_fast_text_cells                                = 0;
    qint64                 route_qt_text_layout_runs                            = 0;
    qint64                 route_graphic_geometry_cells                         = 0;
    qint64                 route_fallback_cells                                 = 0;
    qint64                 qt_text_layout_calls                                 = 0;
    qint64                 qsg_nodes_created                                    = 0;
    qint64                 qsg_nodes_replaced                                   = 0;
    qint64                 qsg_nodes_destroyed                                  = 0;
    qint64                 background_qsg_nodes_created                         = 0;
    qint64                 background_qsg_nodes_replaced                        = 0;
    qint64                 background_qsg_nodes_destroyed                       = 0;
    qint64                 text_key_builds                                      = 0;
    std::uint64_t          text_key_bytes                                       = 0U;
    qint64                 rect_key_builds                                      = 0;
    std::uint64_t          rect_key_bytes                                       = 0U;
    qint64                 cache_key_builds                                     = 0;
    std::uint64_t          cache_key_bytes                                      = 0U;
    qint64                 text_cache_entries_created                           = 0;
    qint64                 text_cache_entries_replaced                          = 0;
    qint64                 text_cache_entries_removed                           = 0;
    qint64                 frame_background_rects                               = 0;
    qint64                 frame_selection_rects                                = 0;
    qint64                 frame_graphic_rects                                  = 0;
    qint64                 frame_graphic_arcs                                   = 0;
    qint64                 frame_text_runs                                      = 0;
    qint64                 frame_cursor_text_runs                               = 0;
    qint64                 frame_decorations                                    = 0;
    qint64                 frame_cursors                                        = 0;
    qint64                 frame_cursor_graphic_rects                           = 0;
    qint64                 frame_cursor_graphic_arcs                            = 0;
    qint64                 frame_overlay_rects                                  = 0;
    qint64                 frame_dirty_row_ranges                               = 0;
    qint64                 frame_packed_rows                                    = 0;
    qint64                 frame_packed_text_spans                              = 0;
    qint64                 frame_packed_text_cells                              = 0;
    qint64                 frame_packed_graphic_spans                           = 0;
    qint64                 frame_packed_graphic_cells                           = 0;
    std::uint64_t          frame_packed_payload_bytes                           = 0U;
    qint64                 row_cache_hits                                       = 0;
    qint64                 row_cache_clean_skips                                = 0;
    qint64                 background_row_rects_before_coalescing               = 0;
    qint64                 background_row_rects_after_coalescing                = 0;
    qint64                 background_batched_rects                             = 0;
    qint64                 background_batched_vertices                          = 0;
    qint64                 selection_batched_rects                              = 0;
    qint64                 selection_batched_vertices                           = 0;
    qint64                 graphic_batched_rects                                = 0;
    qint64                 graphic_batched_vertices                             = 0;
    qint64                 decoration_batched_rects                             = 0;
    qint64                 decoration_batched_vertices                          = 0;
    qint64                 background_rows_rebuilt                              = 0;
    qint64                 background_rows_reused                               = 0;
    qint64                 background_row_clean_reuse_skips                     = 0;
    qint64                 background_rows_removed                              = 0;
    qint64                 background_row_cache_fallbacks                       = 0;
    qint64                 selection_rows_rebuilt                               = 0;
    qint64                 selection_rows_reused                                = 0;
    qint64                 selection_row_clean_reuse_skips                      = 0;
    qint64                 selection_rows_removed                               = 0;
    qint64                 selection_row_cache_fallbacks                        = 0;
    qint64                 decoration_rows_rebuilt                              = 0;
    qint64                 decoration_rows_reused                               = 0;
    qint64                 decoration_row_clean_reuse_skips                     = 0;
    qint64                 decoration_rows_removed                              = 0;
    qint64                 decoration_row_cache_fallbacks                       = 0;
    qint64                 graphic_rect_rows_rebuilt                            = 0;
    qint64                 graphic_rect_rows_reused                             = 0;
    qint64                 graphic_rect_row_clean_reuse_skips                   = 0;
    qint64                 graphic_rect_rows_removed                            = 0;
    qint64                 graphic_rect_row_cache_fallbacks                     = 0;
    qint64                 graphic_arc_rows_rebuilt                             = 0;
    qint64                 graphic_arc_rows_reused                              = 0;
    qint64                 graphic_arc_row_clean_reuse_skips                    = 0;
    qint64                 graphic_arc_rows_removed                             = 0;
    qint64                 graphic_arc_row_cache_fallbacks                      = 0;
};

struct bridge_delta_t
{
    std::uint64_t          update_requests    = 0U;
    std::uint64_t          scheduled_updates  = 0U;
    std::uint64_t          coalesced_requests = 0U;
    std::uint64_t          consumed_updates   = 0U;
};

struct lifecycle_delta_t
{
    std::uint64_t          release_resources_calls_delta         = 0U;
    std::uint64_t          item_scene_changes_delta              = 0U;
    std::uint64_t          item_scene_detaches_delta             = 0U;
    std::uint64_t          item_destructions_delta               = 0U;
    std::uint64_t          scene_graph_invalidated_calls_delta   = 0U;
    std::uint64_t          render_node_deletions_in_paint_delta  = 0U;
    std::uint64_t          render_root_nodes_created_delta       = 0U;
    std::uint64_t          render_root_nodes_destroyed_delta     = 0U;
    std::uint64_t          render_text_resources_created_delta   = 0U;
    std::uint64_t          render_text_resources_destroyed_delta = 0U;
    std::uint64_t          render_rect_resources_created_delta   = 0U;
    std::uint64_t          render_rect_resources_destroyed_delta = 0U;
    std::uint64_t          live_root_nodes                       = 0U;
    std::uint64_t          live_text_resources                   = 0U;
    std::uint64_t          live_rect_resources                   = 0U;
};

struct App_options
{
    QStringList            scenario_names;
    int                    iterations        = k_default_iterations;
    int                    warmup            = k_default_warmup;
    QSize                  window_size       = QSize(k_default_window_width, k_default_window_height);
    grid_size_t            grid;
    QString                output_path;
    QString                profile_json_path;
    QString                profile_text_path;
    bool                   quiet             = false;
    bool                   validate_json     = false;
    bool                   software_renderer = false;
    bool                   profile           = false;
    bool                   help_requested    = false;
    bool                   list_scenarios    = false;
};

struct Parse_result
{
    App_options            options;
    QString                error;
};

struct Structural_checks
{
    bool                   snapshot_valid                  = true;
    bool                   last_rendered_snapshot_sequence = true;
    bool                   pending_update_clear            = true;
    bool                   paint_completed                 = true;
    bool                   text_content_failures_zero      = true;
    bool                   text_work_observed              = true;
    bool                   visible_pixels_observed         = true;
    bool                   no_child_qquick_items           = true;
    bool                   scrollback_rows_available       = true;
    bool                   viewport_offset_changed         = true;
    bool                   viewport_offset_expected        = true;
    bool                   viewport_content_expected       = true;
    bool                   rendered_pixels_changed         = true;
    bool                   wheel_events_accepted           = true;
    bool                   backend_errors_zero             = true;
    bool                   scrollback_limit_respected      = true;
    bool                   workload_actions_accepted       = true;
    bool                   queue_pressure_observed         = true;
    QString                snapshot_status                 = QStringLiteral("OK");
};

struct Attempt_result
{
    bool                                              completed                     = false;
    bool                                              snapshot_valid                = true;
    qint64                                            elapsed_ns                    = 0;
    qint64                                            snapshot_prep_ns              = 0;
    qint64                                            session_scroll_ns             = 0;
    qint64                                            workload_action_ns            = 0;
    qint64                                            scene_graph_update_latency_ns = 0;
    qint64                                            scene_graph_render_wait_ns    = 0;
    qint64                                            readback_ns                   = 0;
    std::uint64_t                                     sequence                      = 0U;
    term::terminal_renderer_stats_t                   renderer_stats;
    term::Terminal_surface_render_invalidation_stats_t
                                                      invalidation_stats;
    bool                                              visible_pixels_observed       = false;
    bool                                              rendered_pixels_changed       = true;
    QImage                                            rendered_image;
    QString                                           snapshot_status               = QStringLiteral("OK");
};

struct surface_session_wheel_burst_t
{
    int                    accepted_count        = 0;
    int                    before_offset         = 0;
    int                    after_offset          = 0;
    int                    expected_after_offset = 0;
    int                    expected_delta        = 0;
    int                    final_delta           = 0;
    int                    expected_top_line     = -1;
    int                    final_top_line        = -1;
    bool                   content_expected      = false;
};

struct surface_session_scroll_profile_t
{
    int                    wheel_event_count     = k_surface_session_wheel_burst_events;
    int                    wheel_steps_per_event = 1;
    int                    fixed_direction       = 0;
};

struct backend_measurement_baseline_t
{
    std::size_t            writes_count       = 0U;
    std::size_t            output_pause_count = 0U;
};

struct profile_thread_snapshot_t
{
    QString                     role;
    int                         index                         = 0;
    term::Profile_node_snapshot root;
};

struct Scenario_result
{
    QString                name;
    QString                source_mode                        = k_snapshot_bridge_source_mode;
    QString                execution_mode                     = k_snapshot_bridge_execution_mode;
    QString                status                             = QStringLiteral("ok");
    int                    iterations                         = 0;
    int                    warmup                             = 0;
    int                    completed_frames                   = 0;
    int                    timeout_count                      = 0;
    int                    rows                               = 0;
    int                    columns                            = 0;
    QSize                  window_size;
    sample_summary_t       elapsed_ns;
    sample_summary_t       snapshot_prep_ns;
    sample_summary_t       session_scroll_ns;
    sample_summary_t       workload_action_ns;
    sample_summary_t       scene_graph_update_latency_ns;
    sample_summary_t       scene_graph_render_wait_ns;
    sample_summary_t       readback_ns;
    memory_summary_t       process_memory;
    renderer_totals_t      renderer_totals;
    bridge_delta_t         bridge_delta;
    lifecycle_delta_t      lifecycle_delta;
    Structural_checks      structural_checks;
    bool                   viewport_metrics_applicable        = false;
    int                    viewport_scrollback_rows           = 0;
    int                    viewport_initial_offset_from_tail  = 0;
    int                    viewport_final_offset_from_tail    = 0;
    int                    viewport_expected_offset_from_tail = 0;
    int                    wheel_burst_size                   = 0;
    int                    wheel_steps_per_event              = 1;
    int                    viewport_expected_burst_delta      = 0;
    int                    viewport_final_burst_delta         = 0;
    int                    viewport_expected_top_line         = -1;
    int                    viewport_final_top_line            = -1;
    int                    wheel_events_accepted_count        = 0;
    int                    backend_writes_total               = 0;
    int                    backend_write_bytes_total          = 0;
    int                    backend_errors_total               = 0;
    int                    session_snapshots_observed         = 0;
    int                    scrollback_limit_configured        = 0;
    int                    scrollback_limit_observed          = 0;
    int                    output_pause_requests_total        = 0;
    int                    output_pause_enabled_count         = 0;
    int                    output_pause_disabled_count        = 0;
    bool                   render_expected                    = true;
    bool                   output_high_water_observed         = false;
    bool                   write_high_water_observed          = false;
    QString                render_measurement_semantics       = k_render_expected_semantics;
    QString                queue_pressure_semantics           = QStringLiteral("not_applicable");
    int                    workload_actions_expected_count    = 0;
    int                    workload_actions_accepted_count    = 0;
    QString                dominant_latency_component         = QStringLiteral("no_completed_samples");
    QString                primary_pressure                   = QStringLiteral("no_completed_samples");
    std::vector<profile_thread_snapshot_t>
                           profile_threads;
};

struct Benchmark_context
{
    QGuiApplication&       app;
    QQuickWindow&          window;
    VNM_TerminalSurface&   surface;
    std::uint64_t          next_sequence            = 1000U;
    int                    default_scrollback_limit = 0;
};

QStringList scenario_names()
{
    return {
        QStringLiteral("dense_repaint"),
        QStringLiteral("ascii_full_dirty_reuse_only"),
        QStringLiteral("ascii_full_dirty_same_content"),
        QStringLiteral("styled_ascii_full_dirty_reuse_only"),
        QStringLiteral("mixed_text_full_dirty_reuse_only"),
        QStringLiteral("scroll_burst"),
        QStringLiteral("resize_bounce"),
        QStringLiteral("unicode_wide_row"),
        QStringLiteral("cursor_repaint_toggle"),
        QStringLiteral("single_row_geometry_update"),
        QStringLiteral("surface_session_wheel_scroll"),
        QStringLiteral("surface_session_viewport_pan"),
        QStringLiteral("surface_session_sustained_output"),
        QStringLiteral("surface_session_scrollback_limit"),
        QStringLiteral("surface_session_alternate_churn"),
        QStringLiteral("surface_session_paste_write"),
        QStringLiteral("surface_session_output_high_water"),
        QStringLiteral("surface_session_write_high_water"),
    };
}

bool is_measurement_reuse_only_scenario(const QString& scenario_name)
{
    return
        scenario_name == QStringLiteral("ascii_full_dirty_reuse_only")        ||
        scenario_name == QStringLiteral("styled_ascii_full_dirty_reuse_only") ||
        scenario_name == QStringLiteral("mixed_text_full_dirty_reuse_only");
}

bool is_surface_session_scenario(const QString& scenario_name)
{
    return
        scenario_name == QStringLiteral("surface_session_wheel_scroll")      ||
        scenario_name == QStringLiteral("surface_session_viewport_pan")      ||
        scenario_name == QStringLiteral("surface_session_sustained_output")  ||
        scenario_name == QStringLiteral("surface_session_scrollback_limit")  ||
        scenario_name == QStringLiteral("surface_session_alternate_churn")   ||
        scenario_name == QStringLiteral("surface_session_paste_write")       ||
        scenario_name == QStringLiteral("surface_session_output_high_water") ||
        scenario_name == QStringLiteral("surface_session_write_high_water");
}

QString scenario_source_mode(const QString& scenario_name)
{
    return is_surface_session_scenario(scenario_name)
        ? k_surface_session_source_mode
        : k_snapshot_bridge_source_mode;
}

QString scenario_execution_mode(const QString& scenario_name)
{
    if (scenario_name == QStringLiteral("surface_session_wheel_scroll")) {
        return k_surface_session_wheel_execution_mode;
    }

    if (scenario_name == QStringLiteral("surface_session_viewport_pan")) {
        return k_surface_session_viewport_pan_execution_mode;
    }

    if (scenario_name == QStringLiteral("surface_session_sustained_output")) {
        return k_surface_session_sustained_output_execution_mode;
    }

    if (scenario_name == QStringLiteral("surface_session_scrollback_limit")) {
        return k_surface_session_scrollback_limit_execution_mode;
    }

    if (scenario_name == QStringLiteral("surface_session_alternate_churn")) {
        return k_surface_session_alternate_churn_execution_mode;
    }

    if (scenario_name == QStringLiteral("surface_session_paste_write")) {
        return k_surface_session_paste_write_execution_mode;
    }

    if (scenario_name == QStringLiteral("surface_session_output_high_water")) {
        return k_surface_session_output_high_water_execution_mode;
    }

    if (scenario_name == QStringLiteral("surface_session_write_high_water")) {
        return k_surface_session_write_high_water_execution_mode;
    }

    return k_snapshot_bridge_execution_mode;
}

void print_usage()
{
    std::cout
        << "usage: vnm_terminal_embedded_benchmark [options]\n"
        << "\n"
        << "options:\n"
        << "  --help                    show this help\n"
        << "  --list-scenarios          print available scenario names\n"
        << "  --scenario <name>         run one scenario; may be repeated\n"
        << "  --iterations <n>          measured iterations per scenario\n"
        << "  --warmup <n>              warmup iterations per scenario\n"
        << "  --window-size <WxH>       window size in logical pixels\n"
        << "  --grid <ROWSxCOLS>        terminal grid size\n"
        << "  --output <path>           write JSON to a file\n"
#if VNM_TERMINAL_PROFILING_ENABLED
        << "  --profile                 collect hierarchical GUI/render profile data\n"
        << "  --profile-json <path>     write profile-only JSON to a file\n"
        << "  --profile-text <path>     write profile tree text to a file\n"
#endif
        << "  --quiet                   suppress stdout when --output is used\n"
        << "  --validate-json           validate emitted JSON and scenario status\n"
        << "  --software-renderer       use the Qt software scene graph\n";
}

void print_scenarios()
{
    for (const QString& name : scenario_names()) {
        std::cout << name.toUtf8().constData() << '\n';
    }
}

QStringList raw_arguments(int argc, char** argv)
{
    QStringList arguments;
    for (int index = 0; index < argc; ++index) {
        arguments.push_back(QString::fromLocal8Bit(argv[index]));
    }
    return arguments;
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
    if (!width_ok                   ||
        !height_ok                  ||
        width  <= 0                 ||
        height <= 0                 ||
        width  >  k_max_window_axis ||
        height >  k_max_window_axis)
    {
        return std::nullopt;
    }

    return QSize(width, height);
}

std::optional<grid_size_t> parse_grid_size(const QString& value)
{
    int separator = value.indexOf(QLatin1Char('x'));
    if (separator < 0) {
        separator = value.indexOf(QLatin1Char('X'));
    }

    if (separator <= 0 || separator + 1 >= value.size()) {
        return std::nullopt;
    }

    bool      rows_ok    = false;
    bool      columns_ok = false;
    const int rows       = value.left(separator).toInt(&rows_ok);
    const int columns    = value.mid(separator + 1).toInt(&columns_ok);
    if (!rows_ok                   ||
        !columns_ok                ||
        rows    <= 0               ||
        columns <= 0               ||
        rows    >  k_max_grid_rows ||
        columns >  k_max_grid_columns)
    {
        return std::nullopt;
    }

    return grid_size_t{rows, columns};
}

bool parse_positive_int(
    const QString& value,
    const QString& option_name,
    int*           out_value,
    QString*       out_error)
{
    bool      ok     = false;
    const int parsed = value.toInt(&ok);
    if (!ok || parsed <= 0) {
        *out_error = QStringLiteral("%1 requires a positive integer").arg(option_name);
        return false;
    }

    *out_value = parsed;
    return true;
}

bool parse_nonnegative_int(
    const QString& value,
    const QString& option_name,
    int*           out_value,
    QString*       out_error)
{
    bool      ok     = false;
    const int parsed = value.toInt(&ok);
    if (!ok || parsed < 0) {
        *out_error = QStringLiteral("%1 requires a non-negative integer").arg(option_name);
        return false;
    }

    *out_value = parsed;
    return true;
}

QString normalized_output_path_key(const QString& path)
{
    QString key = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#if defined(Q_OS_WIN)
    key = key.toCaseFolded();
#endif
    return key;
}

bool validate_output_path_collisions(
    const App_options& options,
    QString*           out_error)
{
    struct output_path_option_t
    {
        QString option_name;
        QString path;
    };

    const output_path_option_t output_paths[] = {
        { QStringLiteral("--output"), options.output_path },
        { QStringLiteral("--profile-json"), options.profile_json_path },
        { QStringLiteral("--profile-text"), options.profile_text_path },
    };
    constexpr std::size_t output_path_count =
        sizeof(output_paths) / sizeof(output_paths[0]);

    for (std::size_t left = 0U; left < output_path_count; ++left) {
        if (output_paths[left].path.isEmpty()) {
            continue;
        }

        const QString left_key = normalized_output_path_key(output_paths[left].path);
        for (std::size_t right = left + 1U; right < output_path_count; ++right) {
            if (output_paths[right].path.isEmpty()) {
                continue;
            }

            const QString right_key = normalized_output_path_key(output_paths[right].path);
            if (left_key == right_key) {
                *out_error = QStringLiteral(
                    "%1 and %2 resolve to the same output path: %3")
                    .arg(output_paths[left].option_name)
                    .arg(output_paths[right].option_name)
                    .arg(QDir::cleanPath(QFileInfo(output_paths[left].path).absoluteFilePath()));
                return false;
            }
        }
    }

    return true;
}

bool is_known_scenario(const QString& name)
{
    return scenario_names().contains(name);
}

bool validate_high_water_scenario_preconditions(
    const App_options& options,
    QString*           out_error)
{
    const term::Terminal_session_config default_config;
    if (options.scenario_names.contains(QStringLiteral("surface_session_output_high_water"))) {
        const std::size_t output_high_water_bytes =
            k_surface_session_output_high_water_chunks *
            k_surface_session_output_high_water_chunk_bytes;
        if (output_high_water_bytes <
            default_config.output_queue_limits.high_water_bytes ||
            output_high_water_bytes >=
            default_config.output_queue_limits.hard_limit_bytes ||
            k_surface_session_output_high_water_chunks >=
            static_cast<int>(default_config.output_queue_limits.high_water_commands))
        {
            *out_error = QStringLiteral(
                "surface_session_output_high_water no longer crosses only the output " "byte high-water precondition");
            return false;
        }
    }

    if (options.scenario_names.contains(QStringLiteral("surface_session_write_high_water"))) {
        if (k_surface_session_write_high_water_bytes <
            static_cast<int>(default_config.write_queue_limits.high_water_bytes) ||
            k_surface_session_write_high_water_bytes >=
            static_cast<int>(default_config.write_queue_limits.hard_limit_bytes))
        {
            *out_error = QStringLiteral(
                "surface_session_write_high_water no longer reserves bytes inside the "
                "write high-water/hard-limit window");
            return false;
        }
    }

    return true;
}

Parse_result parse_arguments(const QStringList& arguments)
{
    Parse_result result;

    int index = 1;
    while (index < arguments.size()) {
        const QString argument = arguments[index];

        if (argument_is(argument, "--help") || argument_is(argument, "-h")) {
            result.options.help_requested = true;
            return result;
        }

        if (argument_is(argument, "--list-scenarios")) {
            result.options.list_scenarios = true;
            ++index;
            continue;
        }

        if (argument_is(argument, "--quiet")) {
            result.options.quiet = true;
            ++index;
            continue;
        }

        if (argument_is(argument, "--validate-json")) {
            result.options.validate_json = true;
            ++index;
            continue;
        }

        if (argument_is(argument, "--software-renderer")) {
            result.options.software_renderer = true;
            ++index;
            continue;
        }

        if (argument_is(argument, "--profile")) {
#if VNM_TERMINAL_PROFILING_ENABLED
            result.options.profile = true;
            ++index;
            continue;
#else
            result.error = QStringLiteral(
                "--profile requires VNM_TERMINAL_ENABLE_PROFILING=ON");
            return result;
#endif
        }

        QString value;
        if (argument_is(argument, "--scenario")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }

            if (!is_known_scenario(value)) {
                result.error = QStringLiteral("unknown scenario: %1").arg(value);
                return result;
            }

            result.options.scenario_names.push_back(value);
            continue;
        }

        if (argument_is(argument, "--iterations")) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_positive_int(
                    value, QStringLiteral("--iterations"),
                    &result.options.iterations, &result.error))
            {
                return result;
            }

            continue;
        }

        if (argument_is(argument, "--warmup")) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_nonnegative_int(
                    value, QStringLiteral("--warmup"), &result.options.warmup, &result.error))
            {
                return result;
            }

            continue;
        }

        if (argument_is(argument, "--window-size")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }

            const std::optional<QSize> parsed = parse_window_size(value);
            if (!parsed.has_value()) {
                result.error = QStringLiteral(
                    "--window-size requires WIDTHxHEIGHT with each axis in 1..%1"
                ).arg(k_max_window_axis);
                return result;
            }

            result.options.window_size = *parsed;
            continue;
        }

        if (argument_is(argument, "--grid")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }

            const std::optional<grid_size_t> parsed = parse_grid_size(value);
            if (!parsed.has_value()) {
                result.error = QStringLiteral(
                    "--grid requires ROWSxCOLS within 1..%1 rows and 1..%2 columns"
                ).arg(k_max_grid_rows).arg(k_max_grid_columns);
                return result;
            }

            result.options.grid = *parsed;
            continue;
        }

        if (argument_is(argument, "--output")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }

            result.options.output_path = value;
            continue;
        }

        if (argument_is(argument, "--profile-json")) {
#if VNM_TERMINAL_PROFILING_ENABLED
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }

            result.options.profile           = true;
            result.options.profile_json_path = value;
            continue;
#else
            result.error = QStringLiteral(
                "--profile-json requires VNM_TERMINAL_ENABLE_PROFILING=ON");
            return result;
#endif
        }

        if (argument_is(argument, "--profile-text")) {
#if VNM_TERMINAL_PROFILING_ENABLED
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }

            result.options.profile           = true;
            result.options.profile_text_path = value;
            continue;
#else
            result.error = QStringLiteral(
                "--profile-text requires VNM_TERMINAL_ENABLE_PROFILING=ON");
            return result;
#endif
        }

        result.error = QStringLiteral("unknown option: %1").arg(argument);
        return result;
    }

    if (result.options.scenario_names.isEmpty()) {
        result.options.scenario_names = scenario_names();
    }

    if (!validate_output_path_collisions(result.options, &result.error)) {
        return result;
    }

    return result;
}

QByteArray numbered_scroll_lines(int count)
{
    QByteArray bytes;
    for (int index = 0; index < count; ++index) {
        bytes += QStringLiteral("surface-scroll-line-%1\r\n")
            .arg(index, 4, 10, QLatin1Char('0'))
            .toUtf8();
    }
    return bytes;
}

QByteArray numbered_workload_lines(const QString& prefix, int phase, int count)
{
    QByteArray bytes;
    for (int index = 0; index < count; ++index) {
        bytes += QStringLiteral("%1-%2-%3\r\n")
            .arg(prefix)
            .arg(phase, 4, 10, QLatin1Char('0'))
            .arg(index, 4, 10, QLatin1Char('0'))
            .toUtf8();
    }
    return bytes;
}

QByteArray alternate_screen_churn_payload(int phase)
{
    QByteArray bytes;
    for (int churn = 0; churn < k_surface_session_alternate_churns_per_attempt; ++churn) {
        bytes += QStringLiteral("\x1b[?1049hALT-%1-%2\r\n\x1b[?1049lPRI-%1-%2\r\n")
            .arg(phase, 4, 10, QLatin1Char('0'))
            .arg(churn, 2, 10, QLatin1Char('0'))
            .toUtf8();
    }
    return bytes;
}

QString paste_workload_text(int phase)
{
    QString text;
    for (int index = 0; index < k_surface_session_paste_lines_per_attempt; ++index) {
        text += QStringLiteral("paste-reservation-%1-%2\n")
            .arg(phase, 4, 10, QLatin1Char('0'))
            .arg(index, 4, 10, QLatin1Char('0'));
    }
    return text;
}

QByteArray output_high_water_chunk(int phase, int chunk_index)
{
    QByteArray bytes;
    bytes.reserve(k_surface_session_output_high_water_chunk_bytes);
    bytes += QStringLiteral("ohw%1%2\n")
        .arg(phase % 100, 2, 10, QLatin1Char('0'))
        .arg(chunk_index % 100, 2, 10, QLatin1Char('0'))
        .toUtf8();
    if (bytes.size() < k_surface_session_output_high_water_chunk_bytes) {
        bytes.append(
            k_surface_session_output_high_water_chunk_bytes - bytes.size(),
            '.');
    }
    return bytes;
}

QString write_high_water_paste_text(int phase)
{
    QString text;
    text.reserve(k_surface_session_write_high_water_bytes + 32);
    text += QStringLiteral("write-high-water-%1\n")
        .arg(phase, 4, 10, QLatin1Char('0'));
    while (text.toUtf8().size() < k_surface_session_write_high_water_bytes) {
        text += QStringLiteral("0123456789abcdef0123456789abcdef\n");
    }
    return text;
}

class Scripted_backend final : public term::Terminal_backend
{
public:
    term::Terminal_backend_result start(
        const term::Terminal_launch_config&    config,
        term::Terminal_backend_callbacks       callbacks) override
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
        running = true;
        for (const QByteArray& output : outputs_during_start) {
            m_callbacks.output_received(output);
        }
        return term::backend_accept();
    }

    term::Terminal_backend_result write(QByteArray bytes) override
    {
        writes.push_back(std::move(bytes));
        return term::backend_accept();
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request request) override
    {
        resize_requests.push_back(request);
        if (!term::is_valid_grid_size(request.grid_size)) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::RESIZE_FAILED,
                    QStringLiteral("scripted resize requires a positive grid"));
        }

        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool paused) override
    {
        output_pause_requests.push_back(paused);
        return term::backend_accept();
    }

    bool emit_output(QByteArray output)
    {
        if (!running) {
            return false;
        }

        m_callbacks.output_received(std::move(output));
        return true;
    }

    term::Terminal_backend_result interrupt() override
    {
        if (!running) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::INTERRUPT_FAILED,
                    QStringLiteral("scripted interrupt without process"));
        }

        running = false;
        m_callbacks.process_exited({term::Terminal_exit_reason::INTERRUPTED, 130});
        return term::backend_accept();
    }

    term::Terminal_backend_result terminate() override
    {
        if (!running) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::TERMINATE_FAILED,
                    QStringLiteral("scripted terminate without process"));
        }

        running = false;
        m_callbacks.process_exited({term::Terminal_exit_reason::TERMINATED, 0});
        return term::backend_accept();
    }

    bool                       running = false;
    std::vector<QByteArray>    outputs_during_start;
    std::vector<term::Terminal_backend_resize_request>
                               resize_requests;
    std::vector<QByteArray>    writes;
    std::vector<bool>          output_pause_requests;

private:
    term::Terminal_backend_callbacks m_callbacks;
};

class Surface_session_cleanup
{
public:
    Surface_session_cleanup(
        VNM_TerminalSurface&       surface,
        QMetaObject::Connection    backend_error_connection)
    :
        m_surface(surface),
        m_backend_error_connection(backend_error_connection)
    {}

    ~Surface_session_cleanup()
    {
        cleanup();
    }

    Surface_session_cleanup(const Surface_session_cleanup&)            = delete;
    Surface_session_cleanup& operator=(const Surface_session_cleanup&) = delete;

    void cleanup()
    {
        if (m_cleaned) {
            return;
        }

        if (m_surface.process_state() == VNM_TerminalSurface::Process_state::STARTING ||
            m_surface.process_state() == VNM_TerminalSurface::Process_state::RUNNING)
        {
            (void)m_surface.terminate_process();
        }
        QObject::disconnect(m_backend_error_connection);
        m_cleaned = true;
    }

private:
    VNM_TerminalSurface&       m_surface;
    QMetaObject::Connection    m_backend_error_connection;
    bool                       m_cleaned = false;
};

backend_measurement_baseline_t backend_measurement_baseline(const Scripted_backend& backend)
{
    return {
        backend.writes.size(),
        backend.output_pause_requests.size(),
    };
}

void update_backend_measurement_deltas(
    Scenario_result&                       result,
    const Scripted_backend&                backend,
    const backend_measurement_baseline_t&  baseline)
{
    result.backend_writes_total        = 0;
    result.backend_write_bytes_total   = 0;
    result.output_pause_requests_total = 0;
    result.output_pause_enabled_count  = 0;
    result.output_pause_disabled_count = 0;
    result.output_high_water_observed  = false;
    result.write_high_water_observed   = false;

    for (std::size_t index = baseline.writes_count; index < backend.writes.size(); ++index) {
        const QByteArray& write = backend.writes[index];
        ++result.backend_writes_total;
        result.backend_write_bytes_total += write.size();
        result.write_high_water_observed =
            result.write_high_water_observed ||
            write.size() >= k_surface_session_write_high_water_bytes;
    }

    for (std::size_t index = baseline.output_pause_count;
        index < backend.output_pause_requests.size();
        ++index)
    {
        ++result.output_pause_requests_total;
        if (backend.output_pause_requests[index]) {
            ++result.output_pause_enabled_count;
        }
        else {
            ++result.output_pause_disabled_count;
        }
    }

    result.output_high_water_observed =
        result.output_pause_enabled_count > 0 &&
        result.output_pause_disabled_count > 0;
}

term::Terminal_color_state benchmark_color_state()
{
    term::Terminal_color_state state;
    state.default_foreground_rgba = 0xffd7e3eaU;
    state.default_background_rgba = 0xff101418U;
    state.cursor_rgba             = 0xffffffffU;
    for (std::size_t i = 0; i < state.palette_rgba.size(); ++i) {
        state.palette_rgba[i] = 0xff202830U + static_cast<quint32>(i * 17U);
    }
    return state;
}

term::Terminal_render_snapshot make_base_snapshot(
    grid_size_t    grid,
    std::uint64_t  sequence,
    bool           full_repaint)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = grid.rows;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({grid.rows, grid.columns}, viewport, sequence);
    snapshot.color_state    = benchmark_color_state();
    snapshot.cursor.visible = false;
    snapshot.dirty_row_ranges =
        full_repaint
            ? std::vector<term::Terminal_render_dirty_row_range>{{0, grid.rows}}
            : std::vector<term::Terminal_render_dirty_row_range>{};
    return snapshot;
}

QString ascii_marker(int row, int column, int phase)
{
    const ushort value =
        static_cast<ushort>('A' + (row * 7 + column * 3 + phase) % 26);
    return QString(QChar(value));
}

void append_render_cell(
    term::Terminal_render_snapshot&    snapshot,
    int                                row,
    int                                column,
    const QString&                     text,
    int                                display_width,
    bool                               wide_continuation,
    term::Terminal_style_id            style_id = term::k_default_terminal_style_id)
{
    snapshot.cells.push_back({
        { row, column },
        text,
        0U,
        display_width,
        wide_continuation,
        style_id,
    });
}

QString emoji_marker()
{
    return QString::fromUtf8("\xF0\x9F\x99\x82");
}

std::shared_ptr<const term::Terminal_render_snapshot> make_dense_repaint_snapshot(
    grid_size_t    grid,
    std::uint64_t  sequence,
    int            phase)
{
    term::Terminal_render_snapshot snapshot = make_base_snapshot(grid, sequence, true);
    snapshot.cells.reserve(static_cast<std::size_t>(grid.rows) *
        static_cast<std::size_t>(grid.columns));
    for (int row = 0; row < grid.rows; ++row) {
        for (int column = 0; column < grid.columns; ++column) {
            snapshot.cells.push_back({
                { row, column },
                ascii_marker(row, column, phase),
                0U,
                1,
                false,
                0U,
            });
        }
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot>
make_ascii_full_dirty_same_content_snapshot(
    grid_size_t    grid,
    std::uint64_t  sequence,
    int            phase)
{
    return make_dense_repaint_snapshot(grid, sequence, phase);
}

std::shared_ptr<const term::Terminal_render_snapshot>
make_ascii_full_dirty_reuse_only_snapshot(
    grid_size_t    grid,
    std::uint64_t  sequence)
{
    return make_dense_repaint_snapshot(grid, sequence, 0);
}

std::shared_ptr<const term::Terminal_render_snapshot>
make_styled_ascii_full_dirty_reuse_only_snapshot(
    grid_size_t    grid,
    std::uint64_t  sequence)
{
    term::Terminal_render_snapshot snapshot = make_base_snapshot(grid, sequence, true);

    term::Terminal_text_style accent_style = term::make_default_terminal_text_style();
    accent_style.foreground = term::make_rgb_terminal_color_ref(0xff7cc6ffU);
    snapshot.styles.push_back(accent_style);

    term::Terminal_text_style warning_style = term::make_default_terminal_text_style();
    warning_style.foreground = term::make_rgb_terminal_color_ref(0xffffd166U);
    snapshot.styles.push_back(warning_style);

    snapshot.cells.reserve(static_cast<std::size_t>(grid.rows) *
        static_cast<std::size_t>(grid.columns));
    for (int row = 0; row < grid.rows; ++row) {
        for (int column = 0; column < grid.columns; ++column) {
            const int style_selector = (row * 5 + column * 7) % 3;
            const term::Terminal_style_id style_id =
                style_selector == 0
                    ? term::k_default_terminal_style_id
                    : static_cast<term::Terminal_style_id>(style_selector);
            append_render_cell(
                snapshot,
                row,
                column,
                ascii_marker(row, column, 0),
                1,
                false,
                style_id);
        }
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot>
make_mixed_text_full_dirty_reuse_only_snapshot(
    grid_size_t    grid,
    std::uint64_t  sequence)
{
    term::Terminal_render_snapshot snapshot = make_base_snapshot(grid, sequence, true);
    snapshot.cells.reserve(static_cast<std::size_t>(grid.rows) *
        static_cast<std::size_t>(grid.columns));
    for (int row = 0; row < grid.rows; ++row) {
        int column = 0;
        while (column < grid.columns) {
            const int selector = (row * 11 + column * 7) % 17;
            if (selector == 0 && column + 1 < grid.columns) {
                append_render_cell(
                    snapshot,
                    row,
                    column,
                    QStringLiteral("\u754c"),
                    2,
                    false);
                append_render_cell(snapshot, row, column + 1, {}, 0, true);
                column += 2;
                continue;
            }

            if (selector == 1 && column + 1 < grid.columns) {
                append_render_cell(snapshot, row, column,     emoji_marker(), 2, false);
                append_render_cell(snapshot, row, column + 1, {},             0, true);
                column += 2;
                continue;
            }

            if (selector == 2) {
                append_render_cell(
                    snapshot,
                    row,
                    column,
                    QStringLiteral("e\u0301"),
                    1,
                    false);
                ++column;
                continue;
            }

            append_render_cell(
                snapshot,
                row,
                column,
                ascii_marker(row, column, 0),
                1,
                false);
            ++column;
        }
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_scroll_burst_snapshot(
    grid_size_t    grid,
    std::uint64_t  sequence,
    int            phase)
{
    term::Terminal_render_snapshot snapshot = make_base_snapshot(grid, sequence, true);
    snapshot.viewport.scrollback_rows  = phase;
    snapshot.viewport.offset_from_tail = 0;
    snapshot.cells.reserve(static_cast<std::size_t>(grid.rows) *
        static_cast<std::size_t>(grid.columns));
    for (int row = 0; row < grid.rows; ++row) {
        const int source_row = row + phase;
        for (int column = 0; column < grid.columns; ++column) {
            snapshot.cells.push_back({
                { row, column },
                ascii_marker(source_row, column, phase),
                0U,
                1,
                false,
                0U,
            });
        }
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_resize_bounce_snapshot(
    grid_size_t    grid,
    std::uint64_t  sequence,
    int            phase)
{
    term::Terminal_render_snapshot snapshot = make_base_snapshot(grid, sequence, true);
    snapshot.cursor = {
        { phase % grid.rows, (phase * 5) % grid.columns },
        term::Terminal_cursor_shape::BLOCK,
        true,
        false,
    };
    snapshot.cells.reserve(static_cast<std::size_t>(grid.rows) *
        static_cast<std::size_t>(grid.columns));
    for (int row = 0; row < grid.rows; ++row) {
        for (int column = 0; column < grid.columns; ++column) {
            snapshot.cells.push_back({
                { row, column },
                ascii_marker(row, column, phase + 11),
                0U,
                1,
                false,
                0U,
            });
        }
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_unicode_wide_row_snapshot(
    grid_size_t    grid,
    std::uint64_t  sequence,
    int            phase)
{
    term::Terminal_render_snapshot snapshot = make_base_snapshot(grid, sequence, true);
    snapshot.cells.reserve(static_cast<std::size_t>(grid.columns));

    const int row    = phase % grid.rows;
    int       column = 0;
    while (column < grid.columns) {
        if (column + 1 < grid.columns && (column + phase) % 5 != 0) {
            snapshot.cells.push_back({
                { row, column },
                QStringLiteral("\u754c"),
                0U,
                2,
                false,
                0U,
            });
            snapshot.cells.push_back({
                { row, column + 1 },
                {},
                0U,
                0,
                true,
                0U,
            });
            column += 2;
            continue;
        }

        snapshot.cells.push_back({
            { row, column },
            ascii_marker(row, column, phase + 23),
            0U,
            1,
            false,
            0U,
        });
        ++column;
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_cursor_repaint_snapshot(
    grid_size_t    grid,
    std::uint64_t  sequence,
    int            phase)
{
    term::Terminal_render_snapshot snapshot = make_base_snapshot(grid, sequence, false);
    snapshot.cursor = {
        { phase % grid.rows, (phase * 11) % grid.columns },
        phase % 3 == 0
            ? term::Terminal_cursor_shape::BLOCK
            : (phase % 3 == 1
                ? term::Terminal_cursor_shape::BAR
                : term::Terminal_cursor_shape::UNDERLINE),
        true,
        true,
    };

    const int row = snapshot.cursor.position.row;
    for (int column = 0; column < grid.columns; ++column) {
        snapshot.cells.push_back({
            { row, column },
            ascii_marker(row, column, phase + 31),
            0U,
            1,
            false,
            0U,
        });
    }
    snapshot.dirty_row_ranges = {{row, 1}};
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot>
make_single_row_geometry_update_snapshot(
    grid_size_t    grid,
    std::uint64_t  sequence,
    int            phase,
    bool           dirty)
{
    term::Terminal_render_snapshot snapshot = make_base_snapshot(grid, sequence, false);
    const int row = std::clamp(grid.rows / 2, 0, grid.rows - 1);
    snapshot.dirty_row_ranges =
        dirty
            ? std::vector<term::Terminal_render_dirty_row_range>{{row, 1}}
            : std::vector<term::Terminal_render_dirty_row_range>{};

    auto add_style = [&](term::Terminal_text_style style) {
        snapshot.styles.push_back(style);
        return static_cast<term::Terminal_style_id>(snapshot.styles.size() - 1U);
    };

    const quint32 background_rgba[] = {
        phase % 2 == 0 ? 0xff273f54U : 0xff4b3348U,
        phase % 2 == 0 ? 0xff4b3348U : 0xff273f54U,
        phase % 3 == 0 ? 0xff2e4b36U : 0xff543c27U,
    };
    constexpr std::size_t background_style_count =
        sizeof(background_rgba) / sizeof(background_rgba[0]);
    std::vector<term::Terminal_style_id> background_style_ids;
    background_style_ids.reserve(background_style_count);
    for (const quint32 rgba : background_rgba) {
        term::Terminal_text_style background_style = term::make_default_terminal_text_style();
        background_style.background = term::make_rgb_terminal_color_ref(rgba);
        background_style_ids.push_back(add_style(background_style));
    }

    term::Terminal_text_style decoration_style = term::make_default_terminal_text_style();
    decoration_style.foreground = term::make_rgb_terminal_color_ref(0xffe0c760U);
    decoration_style.attributes =
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::STRIKE);
    const term::Terminal_style_id decoration_style_id = add_style(decoration_style);

    term::Terminal_text_style graphic_rect_style = term::make_default_terminal_text_style();
    graphic_rect_style.foreground = term::make_rgb_terminal_color_ref(0xff72c875U);
    const term::Terminal_style_id graphic_rect_style_id = add_style(graphic_rect_style);

    term::Terminal_text_style graphic_arc_style = term::make_default_terminal_text_style();
    graphic_arc_style.foreground = term::make_rgb_terminal_color_ref(0xff6fa8dcU);
    const term::Terminal_style_id graphic_arc_style_id = add_style(graphic_arc_style);

    std::vector<bool> occupied(static_cast<std::size_t>(grid.columns), false);
    auto push_cell = [&](
        int column,
        const QString& text,
        term::Terminal_style_id style_id) {
        if (column <  0            ||
            column >= grid.columns ||
            occupied[static_cast<std::size_t>(column)])
        {
            return false;
        }

        occupied[static_cast<std::size_t>(column)] = true;
        snapshot.cells.push_back({
            { row, column },
            text,
            0U,
            1,
            false,
            style_id,
        });
        return true;
    };

    auto free_column_near = [&](int preferred) {
        for (int offset = 0; offset < grid.columns; ++offset) {
            const int column = (preferred + offset) % grid.columns;
            if (!occupied[static_cast<std::size_t>(column)]) {
                return column;
            }
        }
        return -1;
    };

    const int background_cell_count = std::max(1, std::min(3, grid.columns));
    for (int index = 0; index < background_cell_count; ++index) {
        const int preferred_column =
            phase * (index + 2) + 1 + index * std::max(2, grid.columns / 5);
        const int column = free_column_near(preferred_column);
        if (column < 0) {
            continue;
        }

        (void)push_cell(
            column,
            ascii_marker(row, column, phase),
            background_style_ids[static_cast<std::size_t>(index) %
                background_style_ids.size()]);
    }

    const int selection_width = std::max(1, std::min(6, grid.columns));
    const int selection_start =
        (phase * 3 + 1) % std::max(1, grid.columns - selection_width + 1);
    snapshot.selection_spans.push_back({
        { {row, selection_start}, {row, selection_start + selection_width}, term::Terminal_selection_mode::NORMAL },
        row,
        selection_start,
        selection_width,
    });

    const int decoration_column = free_column_near(phase * 5 + 7);
    if (decoration_column >= 0) {
        (void)push_cell(
            decoration_column,
            ascii_marker(row, decoration_column, phase + 17),
            decoration_style_id);
    }

    const int graphic_rect_column = free_column_near(phase * 7 + 11);
    if (graphic_rect_column >= 0) {
        (void)push_cell(
            graphic_rect_column,
            QString(QChar(static_cast<char16_t>(0x2584))),
            graphic_rect_style_id);
    }

    const int graphic_arc_column = free_column_near(phase * 11 + 13);
    if (graphic_arc_column >= 0) {
        (void)push_cell(
            graphic_arc_column,
            QString(QChar(static_cast<char16_t>(0x256d))),
            graphic_arc_style_id);
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

QString render_snapshot_status_name(term::Terminal_render_snapshot_status status)
{
    switch (status) {
        case term::Terminal_render_snapshot_status::OK:
            return QStringLiteral("OK");
        case term::Terminal_render_snapshot_status::INVALID_GRID_SIZE:
            return QStringLiteral("INVALID_GRID_SIZE");
        case term::Terminal_render_snapshot_status::INVALID_CELL_POSITION:
            return QStringLiteral("INVALID_CELL_POSITION");
        case term::Terminal_render_snapshot_status::INVALID_CELL_WIDTH:
            return QStringLiteral("INVALID_CELL_WIDTH");
        case term::Terminal_render_snapshot_status::INVALID_CELL_OVERLAP:
            return QStringLiteral("INVALID_CELL_OVERLAP");
        case term::Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION:
            return QStringLiteral("INVALID_WIDE_CELL_CONTINUATION");
        case term::Terminal_render_snapshot_status::INVALID_WIDE_CELL_STYLE:
            return QStringLiteral("INVALID_WIDE_CELL_STYLE");
        case term::Terminal_render_snapshot_status::INVALID_STYLE_ID:
            return QStringLiteral("INVALID_STYLE_ID");
        case term::Terminal_render_snapshot_status::INVALID_CURSOR_POSITION:
            return QStringLiteral("INVALID_CURSOR_POSITION");
        case term::Terminal_render_snapshot_status::INVALID_VIEWPORT:
            return QStringLiteral("INVALID_VIEWPORT");
        case term::Terminal_render_snapshot_status::INVALID_DIRTY_ROW_RANGE:
            return QStringLiteral("INVALID_DIRTY_ROW_RANGE");
        case term::Terminal_render_snapshot_status::INVALID_SELECTION_SPAN:
            return QStringLiteral("INVALID_SELECTION_SPAN");
        case term::Terminal_render_snapshot_status::INVALID_HYPERLINK_METADATA:
            return QStringLiteral("INVALID_HYPERLINK_METADATA");
    }

    return QStringLiteral("UNKNOWN");
}

std::uint64_t take_sequence(Benchmark_context& context)
{
    const std::uint64_t sequence = context.next_sequence;
    ++context.next_sequence;
    return sequence;
}

bool render_completion_reached(
    const VNM_TerminalSurface& surface,
    std::uint64_t              sequence)
{
    const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    const term::terminal_renderer_stats_t renderer_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    return
        invalidation_stats.last_rendered_snapshot_sequence >= sequence &&
        !invalidation_stats.pending_update                             &&
        renderer_stats.paint_completed                                 &&
        renderer_stats.text_content_failures == 0;
}

bool image_has_visible_terminal_pixels(const QImage& image)
{
    if (image.isNull()) {
        return false;
    }

    int visible_pixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            const int distance =
                std::abs(color.red()   - k_surface_background.red()) +
                std::abs(color.green() - k_surface_background.green()) +
                std::abs(color.blue()  - k_surface_background.blue());
            if (distance > 80) {
                ++visible_pixels;
                if (visible_pixels > 20) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool images_have_meaningful_pixel_delta(const QImage& before, const QImage& after)
{
    if (before.isNull()                 ||
        after.isNull()                  ||
        before.size()   != after.size() ||
        before.format() != after.format())
    {
        return false;
    }

    int changed_pixels = 0;
    for (int y = 0; y < before.height(); ++y) {
        for (int x = 0; x < before.width(); ++x) {
            const QColor before_color = before.pixelColor(x, y);
            const QColor after_color  = after.pixelColor(x, y);
            const int    distance     =
                std::abs(before_color.red()   - after_color.red()) +
                std::abs(before_color.green() - after_color.green()) +
                std::abs(before_color.blue()  - after_color.blue());
            if (distance > 40) {
                ++changed_pixels;
                if (changed_pixels > 20) {
                    return true;
                }
            }
        }
    }

    return false;
}

struct render_wait_result_t
{
    bool   completed               = false;
    bool   visible_pixels_observed = false;
    qint64 wait_ns                 = 0;
    qint64 completion_elapsed_ns   = 0;
    qint64 readback_ns             = 0;
    QImage rendered_image;
};

QImage grab_window_with_timing(Benchmark_context& context, qint64* out_readback_ns)
{
    QElapsedTimer readback_timer;
    readback_timer.start();
    QImage image = context.window.grabWindow();
    *out_readback_ns += readback_timer.nsecsElapsed();
    return image;
}

void add_simple_content_stats(
    term::terminal_simple_content_stats_t&         totals,
    const term::terminal_simple_content_stats_t&   stats)
{
    totals.cells_considered               += stats.cells_considered;
    totals.eligible_cells                 += stats.eligible_cells;
    totals.eligible_after_all_gates_cells += stats.eligible_after_all_gates_cells;
    totals.rows_with_eligible_cells       += stats.rows_with_eligible_cells;
    totals.styles_with_eligible_cells     += stats.styles_with_eligible_cells;
    totals.dirty_eligible_cells           += stats.dirty_eligible_cells;
    totals.clean_eligible_cells           += stats.clean_eligible_cells;
    totals.text_category_empty_cells      += stats.text_category_empty_cells;
    totals.text_category_printable_ascii_cells +=
        stats.text_category_printable_ascii_cells;
    totals.text_category_other_ascii_cells  += stats.text_category_other_ascii_cells;
    totals.text_category_non_ascii_cells    += stats.text_category_non_ascii_cells;
    totals.route_none_cells                 += stats.route_none_cells;
    totals.route_fast_text_cells            += stats.route_fast_text_cells;
    totals.route_qt_text_layout_cells       += stats.route_qt_text_layout_cells;
    totals.route_graphic_geometry_cells     += stats.route_graphic_geometry_cells;
    totals.route_fallback_cells             += stats.route_fallback_cells;
    totals.rejection_none_cells             += stats.rejection_none_cells;
    totals.rejection_empty_text_cells       += stats.rejection_empty_text_cells;
    totals.rejection_invalid_grid_cells     += stats.rejection_invalid_grid_cells;
    totals.rejection_invalid_position_cells += stats.rejection_invalid_position_cells;
    totals.rejection_invalid_style_id_cells += stats.rejection_invalid_style_id_cells;
    totals.rejection_wide_continuation_cells   +=
        stats.rejection_wide_continuation_cells;
    totals.rejection_invalid_display_width_cells +=
        stats.rejection_invalid_display_width_cells;
    totals.rejection_invalid_text_encoding_cells +=
        stats.rejection_invalid_text_encoding_cells;
    totals.rejection_invalid_text_width_cells  +=
        stats.rejection_invalid_text_width_cells;
    totals.rejection_multi_cell_text_cells     += stats.rejection_multi_cell_text_cells;
    totals.rejection_non_printable_ascii_cells +=
        stats.rejection_non_printable_ascii_cells;
    totals.rejection_non_ascii_text_cells   += stats.rejection_non_ascii_text_cells;
    totals.rejection_decoration_cells       += stats.rejection_decoration_cells;
    totals.rejection_hyperlink_cells        += stats.rejection_hyperlink_cells;
    totals.rejection_terminal_graphic_cells += stats.rejection_terminal_graphic_cells;
}

void add_simple_content_stats(
    renderer_totals_t&                             totals,
    const term::terminal_simple_content_stats_t&   stats)
{
    totals.simple_content_cells_considered += stats.cells_considered;
    totals.simple_content_eligible_cells   += stats.eligible_cells;
    totals.simple_content_eligible_after_all_gates_cells      +=
        stats.eligible_after_all_gates_cells;
    totals.simple_content_rows_with_eligible_cells            +=
        stats.rows_with_eligible_cells;
    totals.simple_content_styles_with_eligible_cells          +=
        stats.styles_with_eligible_cells;
    totals.simple_content_dirty_eligible_cells += stats.dirty_eligible_cells;
    totals.simple_content_clean_eligible_cells += stats.clean_eligible_cells;
    totals.simple_content_text_category_empty_cells           +=
        stats.text_category_empty_cells;
    totals.simple_content_text_category_printable_ascii_cells +=
        stats.text_category_printable_ascii_cells;
    totals.simple_content_text_category_other_ascii_cells     +=
        stats.text_category_other_ascii_cells;
    totals.simple_content_text_category_non_ascii_cells       +=
        stats.text_category_non_ascii_cells;
    totals.simple_content_route_none_cells      += stats.route_none_cells;
    totals.simple_content_route_fast_text_cells += stats.route_fast_text_cells;
    totals.simple_content_route_qt_text_layout_cells          +=
        stats.route_qt_text_layout_cells;
    totals.simple_content_route_graphic_geometry_cells        +=
        stats.route_graphic_geometry_cells;
    totals.simple_content_route_fallback_cells += stats.route_fallback_cells;
    totals.simple_content_rejection_none_cells += stats.rejection_none_cells;
    totals.simple_content_rejection_empty_text_cells          +=
        stats.rejection_empty_text_cells;
    totals.simple_content_rejection_invalid_grid_cells        +=
        stats.rejection_invalid_grid_cells;
    totals.simple_content_rejection_invalid_position_cells    +=
        stats.rejection_invalid_position_cells;
    totals.simple_content_rejection_invalid_style_id_cells    +=
        stats.rejection_invalid_style_id_cells;
    totals.simple_content_rejection_wide_continuation_cells   +=
        stats.rejection_wide_continuation_cells;
    totals.simple_content_rejection_invalid_display_width_cells +=
        stats.rejection_invalid_display_width_cells;
    totals.simple_content_rejection_invalid_text_encoding_cells +=
        stats.rejection_invalid_text_encoding_cells;
    totals.simple_content_rejection_invalid_text_width_cells  +=
        stats.rejection_invalid_text_width_cells;
    totals.simple_content_rejection_multi_cell_text_cells     +=
        stats.rejection_multi_cell_text_cells;
    totals.simple_content_rejection_non_printable_ascii_cells +=
        stats.rejection_non_printable_ascii_cells;
    totals.simple_content_rejection_non_ascii_text_cells      +=
        stats.rejection_non_ascii_text_cells;
    totals.simple_content_rejection_decoration_cells          +=
        stats.rejection_decoration_cells;
    totals.simple_content_rejection_hyperlink_cells           +=
        stats.rejection_hyperlink_cells;
    totals.simple_content_rejection_terminal_graphic_cells    +=
        stats.rejection_terminal_graphic_cells;
}

void add_renderer_stats(
    term::terminal_renderer_stats_t&       totals,
    const term::terminal_renderer_stats_t& stats)
{
    totals.paint_completed = totals.paint_completed && stats.paint_completed;
    add_simple_content_stats(totals.frame.simple_content, stats.frame.simple_content);
    totals.text_content_rebuilds   += stats.text_content_rebuilds;
    totals.text_content_reused     += stats.text_content_reused;
    totals.text_content_removed    += stats.text_content_removed;
    totals.text_content_failures   += stats.text_content_failures;
    totals.text_leaf_nodes_created += stats.text_leaf_nodes_created;
    totals.text_cache_entry_child_nodes_cleared_for_replacement +=
        stats.text_cache_entry_child_nodes_cleared_for_replacement;
    totals.text_cache_entry_child_nodes_cleared_for_removal +=
        stats.text_cache_entry_child_nodes_cleared_for_removal;
    totals.text_cache_entry_max_child_nodes_cleared  = std::max(
        totals.text_cache_entry_max_child_nodes_cleared,
        stats.text_cache_entry_max_child_nodes_cleared);
    totals.text_clean_reuse_skips                   += stats.text_clean_reuse_skips;
    totals.text_resource_descriptor_reuses +=
        stats.text_resource_descriptor_reuses;
    totals.text_coalescing_candidate_groups += stats.text_coalescing_candidate_groups;
    totals.text_coalescing_enabled_groups   += stats.text_coalescing_enabled_groups;
    totals.text_resource_runs_before_coalescing +=
        stats.text_resource_runs_before_coalescing;
    totals.text_resource_runs_after_coalescing +=
        stats.text_resource_runs_after_coalescing;
    totals.route_fast_text_cells          += stats.route_fast_text_cells;
    totals.route_qt_text_layout_runs      += stats.route_qt_text_layout_runs;
    totals.route_graphic_geometry_cells   += stats.route_graphic_geometry_cells;
    totals.route_fallback_cells           += stats.route_fallback_cells;
    totals.qt_text_layout_calls           += stats.qt_text_layout_calls;
    totals.qsg_nodes_created              += stats.qsg_nodes_created;
    totals.qsg_nodes_replaced             += stats.qsg_nodes_replaced;
    totals.qsg_nodes_destroyed            += stats.qsg_nodes_destroyed;
    totals.background_qsg_nodes_created   += stats.background_qsg_nodes_created;
    totals.background_qsg_nodes_replaced  += stats.background_qsg_nodes_replaced;
    totals.background_qsg_nodes_destroyed += stats.background_qsg_nodes_destroyed;
    totals.text_key_builds                += stats.text_key_builds;
    totals.text_key_bytes                 += stats.text_key_bytes;
    totals.rect_key_builds                += stats.rect_key_builds;
    totals.rect_key_bytes                 += stats.rect_key_bytes;
    totals.cache_key_builds               += stats.cache_key_builds;
    totals.cache_key_bytes                += stats.cache_key_bytes;
    totals.text_cache_entries_created     += stats.text_cache_entries_created;
    totals.text_cache_entries_replaced    += stats.text_cache_entries_replaced;
    totals.text_cache_entries_removed     += stats.text_cache_entries_removed;
    totals.frame_background_rects         += stats.frame_background_rects;
    totals.frame_selection_rects          += stats.frame_selection_rects;
    totals.frame_graphic_rects            += stats.frame_graphic_rects;
    totals.frame_graphic_arcs             += stats.frame_graphic_arcs;
    totals.frame_text_runs                += stats.frame_text_runs;
    totals.frame_cursor_text_runs         += stats.frame_cursor_text_runs;
    totals.frame_decorations              += stats.frame_decorations;
    totals.frame_cursors                  += stats.frame_cursors;
    totals.frame_cursor_graphic_rects     += stats.frame_cursor_graphic_rects;
    totals.frame_cursor_graphic_arcs      += stats.frame_cursor_graphic_arcs;
    totals.frame_overlay_rects            += stats.frame_overlay_rects;
    totals.frame_dirty_row_ranges         += stats.frame_dirty_row_ranges;
    totals.frame_packed_rows              += stats.frame_packed_rows;
    totals.frame_packed_text_spans        += stats.frame_packed_text_spans;
    totals.frame_packed_text_cells        += stats.frame_packed_text_cells;
    totals.frame_packed_graphic_spans     += stats.frame_packed_graphic_spans;
    totals.frame_packed_graphic_cells     += stats.frame_packed_graphic_cells;
    totals.frame_packed_payload_bytes     += stats.frame_packed_payload_bytes;
    totals.row_cache_hits                 += stats.row_cache_hits;
    totals.row_cache_clean_skips          += stats.row_cache_clean_skips;
    totals.background_row_rects_before_coalescing +=
        stats.background_row_rects_before_coalescing;
    totals.background_row_rects_after_coalescing  +=
        stats.background_row_rects_after_coalescing;
    totals.background_batched_rects         += stats.background_batched_rects;
    totals.background_batched_vertices      += stats.background_batched_vertices;
    totals.selection_batched_rects          += stats.selection_batched_rects;
    totals.selection_batched_vertices       += stats.selection_batched_vertices;
    totals.graphic_batched_rects            += stats.graphic_batched_rects;
    totals.graphic_batched_vertices         += stats.graphic_batched_vertices;
    totals.decoration_batched_rects         += stats.decoration_batched_rects;
    totals.decoration_batched_vertices      += stats.decoration_batched_vertices;
    totals.background_rows_rebuilt          += stats.background_rows_rebuilt;
    totals.background_rows_reused           += stats.background_rows_reused;
    totals.background_row_clean_reuse_skips += stats.background_row_clean_reuse_skips;
    totals.background_rows_removed          += stats.background_rows_removed;
    totals.background_row_cache_fallbacks   += stats.background_row_cache_fallbacks;
    totals.selection_rows_rebuilt           += stats.selection_rows_rebuilt;
    totals.selection_rows_reused            += stats.selection_rows_reused;
    totals.selection_row_clean_reuse_skips  += stats.selection_row_clean_reuse_skips;
    totals.selection_rows_removed           += stats.selection_rows_removed;
    totals.selection_row_cache_fallbacks    += stats.selection_row_cache_fallbacks;
    totals.decoration_rows_rebuilt          += stats.decoration_rows_rebuilt;
    totals.decoration_rows_reused           += stats.decoration_rows_reused;
    totals.decoration_row_clean_reuse_skips += stats.decoration_row_clean_reuse_skips;
    totals.decoration_rows_removed          += stats.decoration_rows_removed;
    totals.decoration_row_cache_fallbacks   += stats.decoration_row_cache_fallbacks;
    totals.graphic_rect_rows_rebuilt        += stats.graphic_rect_rows_rebuilt;
    totals.graphic_rect_rows_reused         += stats.graphic_rect_rows_reused;
    totals.graphic_rect_row_clean_reuse_skips +=
        stats.graphic_rect_row_clean_reuse_skips;
    totals.graphic_rect_rows_removed         += stats.graphic_rect_rows_removed;
    totals.graphic_rect_row_cache_fallbacks  += stats.graphic_rect_row_cache_fallbacks;
    totals.graphic_arc_rows_rebuilt          += stats.graphic_arc_rows_rebuilt;
    totals.graphic_arc_rows_reused           += stats.graphic_arc_rows_reused;
    totals.graphic_arc_row_clean_reuse_skips += stats.graphic_arc_row_clean_reuse_skips;
    totals.graphic_arc_rows_removed          += stats.graphic_arc_rows_removed;
    totals.graphic_arc_row_cache_fallbacks   += stats.graphic_arc_row_cache_fallbacks;
}

render_wait_result_t wait_for_render_completion(
    Benchmark_context&     context,
    std::uint64_t          sequence,
    const QElapsedTimer&   elapsed_timer,
    bool                   capture_readback)
{
    VNM_TERMINAL_PROFILE_SCOPE("wait_for_render_completion");

    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < k_render_timeout_ms) {
        context.app.processEvents(QEventLoop::AllEvents, 5);
        if (render_completion_reached(context.surface, sequence)) {
            render_wait_result_t result;
            result.completed             = true;
            result.wait_ns               = timeout.nsecsElapsed();
            result.completion_elapsed_ns = elapsed_timer.nsecsElapsed();
            if (capture_readback) {
                result.rendered_image =
                    grab_window_with_timing(context, &result.readback_ns);
                result.visible_pixels_observed =
                    image_has_visible_terminal_pixels(result.rendered_image);
            }
            return result;
        }
        QThread::msleep(1);
    }

    render_wait_result_t result;
    result.wait_ns               = timeout.nsecsElapsed();
    result.completion_elapsed_ns = elapsed_timer.nsecsElapsed();
    return result;
}

Attempt_result submit_snapshots_and_wait(
    Benchmark_context&     context,
    std::vector<std::shared_ptr<const term::Terminal_render_snapshot>>
                           snapshots,
    const QElapsedTimer&   elapsed_timer)
{
    VNM_TERMINAL_PROFILE_SCOPE("submit_snapshots_and_wait");

    Attempt_result result;
    for (const std::shared_ptr<const term::Terminal_render_snapshot>& snapshot : snapshots) {
        const term::Terminal_render_snapshot_validation validation =
            term::validate_render_snapshot(*snapshot);
        if (validation.status != term::Terminal_render_snapshot_status::OK) {
            result.snapshot_valid  = false;
            result.snapshot_status = render_snapshot_status_name(validation.status);
            result.elapsed_ns      = elapsed_timer.nsecsElapsed();
            return result;
        }
    }

    result.sequence = snapshots.back()->metadata.sequence;
    const qint64 render_start_ns            = elapsed_timer.nsecsElapsed();
    qint64       last_completion_elapsed_ns = render_start_ns;
    result.completed = true;
    result.renderer_stats.paint_completed = true;
    for (std::size_t index = 0; index < snapshots.size(); ++index) {
        const std::shared_ptr<const term::Terminal_render_snapshot>& snapshot =
            snapshots[index];
        term::VNM_TerminalSurface_render_bridge::set_render_snapshot(context.surface, snapshot);
        const bool capture_readback = index + 1U == snapshots.size();
        const render_wait_result_t wait_result = wait_for_render_completion(
            context,
            snapshot->metadata.sequence,
            elapsed_timer,
            capture_readback);
        result.completed                   = result.completed && wait_result.completed;
        result.scene_graph_render_wait_ns += wait_result.wait_ns;
        result.readback_ns                += wait_result.readback_ns;
        result.visible_pixels_observed =
            result.visible_pixels_observed || wait_result.visible_pixels_observed;
        result.rendered_image = wait_result.rendered_image;
        if (wait_result.completed) {
            last_completion_elapsed_ns = wait_result.completion_elapsed_ns;
        }
        add_renderer_stats(
            result.renderer_stats,
            term::VNM_TerminalSurface_render_bridge::last_renderer_stats(context.surface));
        if (!wait_result.completed) {
            break;
        }
    }

    result.elapsed_ns = elapsed_timer.nsecsElapsed();
    result.scene_graph_update_latency_ns =
        result.completed ? last_completion_elapsed_ns - render_start_ns : 0;
    result.invalidation_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(context.surface);
    return result;
}

QString snapshot_row_text(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row)
{
    QString text;
    for (int column = 0; column < snapshot.grid_size.columns; ++column) {
        QString cell_text = QStringLiteral(" ");
        for (const term::Terminal_render_cell& cell : snapshot.cells) {
            if (cell.position.row == row && cell.position.column == column) {
                cell_text = cell.text;
                break;
            }
        }
        text += cell_text;
    }

    while (!text.isEmpty() && text.back() == QChar(u' ')) {
        text.chop(1);
    }
    return text;
}

QString scripted_surface_scroll_line_text(int line_index)
{
    return QStringLiteral("surface-scroll-line-%1")
        .arg(line_index, 4, 10, QLatin1Char('0'));
}

std::optional<int> scripted_surface_scroll_line_index(const QString& text)
{
    const QString prefix = QStringLiteral("surface-scroll-line-");
    if (!text.startsWith(prefix)) {
        return std::nullopt;
    }

    qsizetype digit_end = prefix.size();
    while (digit_end < text.size() && text[digit_end].isDigit()) {
        ++digit_end;
    }
    if (digit_end == prefix.size()) {
        return std::nullopt;
    }

    bool ok = false;
    const int line_index =
        text.mid(prefix.size(), digit_end - prefix.size()).toInt(&ok);
    if (!ok) {
        return std::nullopt;
    }

    return line_index;
}

int expected_surface_scroll_top_line(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    output_line_count,
    int                                    offset_from_tail)
{
    return output_line_count - snapshot.grid_size.rows + 1 - offset_from_tail;
}

bool send_surface_wheel_step(VNM_TerminalSurface& surface, int angle_delta_y)
{
    QWheelEvent event(
        QPointF(8.0, 8.0),
        QPointF(8.0, 8.0),
        QPoint(0, 0),
        QPoint(0, angle_delta_y),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false);
    event.ignore();
    QCoreApplication::sendEvent(&surface, &event);
    return event.isAccepted();
}

int send_surface_wheel_burst(
    VNM_TerminalSurface&   surface,
    int                    angle_delta_y,
    int                    event_count)
{
    int accepted_count = 0;
    for (int event_index = 0; event_index < event_count; ++event_index) {
        if (send_surface_wheel_step(surface, angle_delta_y)) {
            ++accepted_count;
        }
    }

    return accepted_count;
}

Attempt_result wait_for_session_snapshot(
    Benchmark_context&     context,
    std::shared_ptr<const term::Terminal_render_snapshot>
                           snapshot,
    const QElapsedTimer&   elapsed_timer,
    qint64                 action_start_ns)
{
    Attempt_result result;
    if (snapshot == nullptr) {
        result.snapshot_valid  = false;
        result.snapshot_status = QStringLiteral("MISSING_SESSION_SNAPSHOT");
        result.elapsed_ns      = elapsed_timer.nsecsElapsed();
        return result;
    }

    const term::Terminal_render_snapshot_validation validation =
        term::validate_render_snapshot(*snapshot);
    if (validation.status != term::Terminal_render_snapshot_status::OK) {
        result.snapshot_valid  = false;
        result.snapshot_status = render_snapshot_status_name(validation.status);
        result.elapsed_ns      = elapsed_timer.nsecsElapsed();
        return result;
    }

    result.sequence = snapshot->metadata.sequence;
    const qint64 wait_start_ns = elapsed_timer.nsecsElapsed();
    const render_wait_result_t wait_result = wait_for_render_completion(
        context,
        snapshot->metadata.sequence,
        elapsed_timer,
        true);
    result.completed                  = wait_result.completed;
    result.scene_graph_render_wait_ns = wait_result.wait_ns;
    result.readback_ns                = wait_result.readback_ns;
    result.visible_pixels_observed    = wait_result.visible_pixels_observed;
    result.rendered_image             = wait_result.rendered_image;
    result.renderer_stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(context.surface);
    result.invalidation_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(context.surface);
    result.elapsed_ns                    = elapsed_timer.nsecsElapsed();
    result.scene_graph_update_latency_ns = wait_result.completed
        ? wait_result.completion_elapsed_ns - action_start_ns
        : 0;
    result.scene_graph_render_wait_ns    = wait_result.completed
        ? wait_result.completion_elapsed_ns - wait_start_ns
        : result.scene_graph_render_wait_ns;
    return result;
}

sample_summary_t summarize_samples(const std::vector<qint64>& samples)
{
    sample_summary_t summary;
    if (samples.empty()) {
        return summary;
    }

    std::vector<qint64> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    summary.sample_count = static_cast<int>(samples.size());
    summary.total        = std::accumulate(samples.begin(), samples.end(), qint64{0});
    summary.min          = sorted.front();
    summary.max          = sorted.back();
    if (sorted.size() % 2U == 0U) {
        const std::size_t right = sorted.size() / 2U;
        summary.median = (sorted[right - 1U] + sorted[right]) / 2;
    }
    else {
        summary.median = sorted[sorted.size() / 2U];
    }

    const double rank = std::ceil(static_cast<double>(sorted.size()) * 0.95);
    const std::size_t p95_index =
        static_cast<std::size_t>(std::max(1.0, rank)) - 1U;
    summary.p95 = sorted[std::min(p95_index, sorted.size() - 1U)];
    return summary;
}

QString process_memory_platform()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#else
    return QStringLiteral("unsupported");
#endif
}

std::optional<qint64> current_process_resident_memory_bytes()
{
#if defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS counters;
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(), &counters, static_cast<DWORD>(sizeof(counters))))
    {
        return std::nullopt;
    }

    return static_cast<qint64>(counters.WorkingSetSize);
#elif defined(Q_OS_LINUX)
    QFile file(QStringLiteral("/proc/self/statm"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return std::nullopt;
    }

    const QList<QByteArray> fields = file.readAll().simplified().split(' ');
    if (fields.size() < 2) {
        return std::nullopt;
    }

    bool         ok             = false;
    const qint64 resident_pages = fields[1].toLongLong(&ok);
    const long   page_size      = sysconf(_SC_PAGESIZE);
    if (!ok || resident_pages < 0 || page_size <= 0) {
        return std::nullopt;
    }

    return resident_pages * static_cast<qint64>(page_size);
#else
    return std::nullopt;
#endif
}

memory_summary_t summarize_memory_samples(const std::vector<qint64>& samples)
{
    memory_summary_t summary;
    summary.platform       = process_memory_platform();
    summary.resident_bytes = summarize_samples(samples);
    if (!samples.empty()) {
        summary.status = QStringLiteral("sampled");
    }
    else
    if (summary.platform != QStringLiteral("unsupported")) {
        summary.status = QStringLiteral("unavailable");
    }
    return summary;
}

struct Timing_samples
{
    std::vector<qint64>    elapsed;
    std::vector<qint64>    snapshot_prep;
    std::vector<qint64>    session_scroll;
    std::vector<qint64>    workload_action;
    std::vector<qint64>    scene_graph_update_latency;
    std::vector<qint64>    scene_graph_render_wait;
    std::vector<qint64>    readback;
    std::vector<qint64>    process_memory;
};

void reserve_timing_samples(Timing_samples& samples, int count)
{
    const std::size_t sample_count = static_cast<std::size_t>(count);
    samples.elapsed.reserve(sample_count);
    samples.snapshot_prep.reserve(sample_count);
    samples.session_scroll.reserve(sample_count);
    samples.workload_action.reserve(sample_count);
    samples.scene_graph_update_latency.reserve(sample_count);
    samples.scene_graph_render_wait.reserve(sample_count);
    samples.readback.reserve(sample_count);
    samples.process_memory.reserve(sample_count);
}

void append_timing_sample(Timing_samples& samples, const Attempt_result& attempt)
{
    samples.elapsed.push_back(attempt.elapsed_ns);
    samples.snapshot_prep.push_back(attempt.snapshot_prep_ns);
    samples.session_scroll.push_back(attempt.session_scroll_ns);
    samples.workload_action.push_back(attempt.workload_action_ns);
    samples.scene_graph_update_latency.push_back(attempt.scene_graph_update_latency_ns);
    samples.scene_graph_render_wait.push_back(attempt.scene_graph_render_wait_ns);
    samples.readback.push_back(attempt.readback_ns);
    const std::optional<qint64> resident_bytes = current_process_resident_memory_bytes();
    if (resident_bytes.has_value()) {
        samples.process_memory.push_back(*resident_bytes);
    }
}

void summarize_timing_samples(Scenario_result& result, const Timing_samples& samples)
{
    result.elapsed_ns         = summarize_samples(samples.elapsed);
    result.snapshot_prep_ns   = summarize_samples(samples.snapshot_prep);
    result.session_scroll_ns  = summarize_samples(samples.session_scroll);
    result.workload_action_ns = summarize_samples(samples.workload_action);
    result.scene_graph_update_latency_ns =
        summarize_samples(samples.scene_graph_update_latency);
    result.scene_graph_render_wait_ns = summarize_samples(samples.scene_graph_render_wait);
    result.readback_ns                = summarize_samples(samples.readback);
    result.process_memory             = summarize_memory_samples(samples.process_memory);
}

bridge_delta_t bridge_delta(
    const term::Terminal_surface_render_invalidation_stats_t&  before,
    const term::Terminal_surface_render_invalidation_stats_t&  after)
{
    return {
        after.update_requests    - before.update_requests,
        after.scheduled_updates  - before.scheduled_updates,
        after.coalesced_requests - before.coalesced_requests,
        after.consumed_updates   - before.consumed_updates,
    };
}

lifecycle_delta_t lifecycle_delta(
    const term::terminal_renderer_lifecycle_stats_t&   before,
    const term::terminal_renderer_lifecycle_stats_t&   after)
{
    return {
        after.release_resources_calls       - before.release_resources_calls,
        after.item_scene_changes            - before.item_scene_changes,
        after.item_scene_detaches           - before.item_scene_detaches,
        after.item_destructions             - before.item_destructions,
        after.scene_graph_invalidated_calls - before.scene_graph_invalidated_calls,
        after.render_node_deletions_in_paint -
            before.render_node_deletions_in_paint,
        after.render_root_nodes_created     - before.render_root_nodes_created,
        after.render_root_nodes_destroyed   - before.render_root_nodes_destroyed,
        after.render_text_resources_created - before.render_text_resources_created,
        after.render_text_resources_destroyed -
            before.render_text_resources_destroyed,
        after.render_rect_resources_created - before.render_rect_resources_created,
        after.render_rect_resources_destroyed -
            before.render_rect_resources_destroyed,
        after.render_root_nodes_created - after.render_root_nodes_destroyed,
        after.render_text_resources_created - after.render_text_resources_destroyed,
        after.render_rect_resources_created - after.render_rect_resources_destroyed,
    };
}

void add_renderer_stats(
    renderer_totals_t&                                 totals,
    const term::terminal_renderer_stats_t&             stats)
{
    add_simple_content_stats(totals, stats.frame.simple_content);
    totals.text_content_rebuilds   += stats.text_content_rebuilds;
    totals.text_content_reused     += stats.text_content_reused;
    totals.text_content_removed    += stats.text_content_removed;
    totals.text_content_failures   += stats.text_content_failures;
    totals.text_leaf_nodes_created += stats.text_leaf_nodes_created;
    totals.text_cache_entry_child_nodes_cleared_for_replacement +=
        stats.text_cache_entry_child_nodes_cleared_for_replacement;
    totals.text_cache_entry_child_nodes_cleared_for_removal +=
        stats.text_cache_entry_child_nodes_cleared_for_removal;
    totals.text_cache_entry_max_child_nodes_cleared  = std::max(
        totals.text_cache_entry_max_child_nodes_cleared,
        static_cast<qint64>(stats.text_cache_entry_max_child_nodes_cleared));
    totals.text_clean_reuse_skips                   += stats.text_clean_reuse_skips;
    totals.text_resource_descriptor_reuses +=
        stats.text_resource_descriptor_reuses;
    totals.text_coalescing_candidate_groups += stats.text_coalescing_candidate_groups;
    totals.text_coalescing_enabled_groups   += stats.text_coalescing_enabled_groups;
    totals.text_resource_runs_before_coalescing +=
        stats.text_resource_runs_before_coalescing;
    totals.text_resource_runs_after_coalescing +=
        stats.text_resource_runs_after_coalescing;
    totals.route_fast_text_cells          += stats.route_fast_text_cells;
    totals.route_qt_text_layout_runs      += stats.route_qt_text_layout_runs;
    totals.route_graphic_geometry_cells   += stats.route_graphic_geometry_cells;
    totals.route_fallback_cells           += stats.route_fallback_cells;
    totals.qt_text_layout_calls           += stats.qt_text_layout_calls;
    totals.qsg_nodes_created              += stats.qsg_nodes_created;
    totals.qsg_nodes_replaced             += stats.qsg_nodes_replaced;
    totals.qsg_nodes_destroyed            += stats.qsg_nodes_destroyed;
    totals.background_qsg_nodes_created   += stats.background_qsg_nodes_created;
    totals.background_qsg_nodes_replaced  += stats.background_qsg_nodes_replaced;
    totals.background_qsg_nodes_destroyed += stats.background_qsg_nodes_destroyed;
    totals.text_key_builds                += stats.text_key_builds;
    totals.text_key_bytes                 += stats.text_key_bytes;
    totals.rect_key_builds                += stats.rect_key_builds;
    totals.rect_key_bytes                 += stats.rect_key_bytes;
    totals.cache_key_builds               += stats.cache_key_builds;
    totals.cache_key_bytes                += stats.cache_key_bytes;
    totals.text_cache_entries_created     += stats.text_cache_entries_created;
    totals.text_cache_entries_replaced    += stats.text_cache_entries_replaced;
    totals.text_cache_entries_removed     += stats.text_cache_entries_removed;
    totals.frame_background_rects         += stats.frame_background_rects;
    totals.frame_selection_rects          += stats.frame_selection_rects;
    totals.frame_graphic_rects            += stats.frame_graphic_rects;
    totals.frame_graphic_arcs             += stats.frame_graphic_arcs;
    totals.frame_text_runs                += stats.frame_text_runs;
    totals.frame_cursor_text_runs         += stats.frame_cursor_text_runs;
    totals.frame_decorations              += stats.frame_decorations;
    totals.frame_cursors                  += stats.frame_cursors;
    totals.frame_cursor_graphic_rects     += stats.frame_cursor_graphic_rects;
    totals.frame_cursor_graphic_arcs      += stats.frame_cursor_graphic_arcs;
    totals.frame_overlay_rects            += stats.frame_overlay_rects;
    totals.frame_dirty_row_ranges         += stats.frame_dirty_row_ranges;
    totals.frame_packed_rows              += stats.frame_packed_rows;
    totals.frame_packed_text_spans        += stats.frame_packed_text_spans;
    totals.frame_packed_text_cells        += stats.frame_packed_text_cells;
    totals.frame_packed_graphic_spans     += stats.frame_packed_graphic_spans;
    totals.frame_packed_graphic_cells     += stats.frame_packed_graphic_cells;
    totals.frame_packed_payload_bytes     += stats.frame_packed_payload_bytes;
    totals.row_cache_hits                 += stats.row_cache_hits;
    totals.row_cache_clean_skips          += stats.row_cache_clean_skips;
    totals.background_row_rects_before_coalescing +=
        stats.background_row_rects_before_coalescing;
    totals.background_row_rects_after_coalescing  +=
        stats.background_row_rects_after_coalescing;
    totals.background_batched_rects         += stats.background_batched_rects;
    totals.background_batched_vertices      += stats.background_batched_vertices;
    totals.selection_batched_rects          += stats.selection_batched_rects;
    totals.selection_batched_vertices       += stats.selection_batched_vertices;
    totals.graphic_batched_rects            += stats.graphic_batched_rects;
    totals.graphic_batched_vertices         += stats.graphic_batched_vertices;
    totals.decoration_batched_rects         += stats.decoration_batched_rects;
    totals.decoration_batched_vertices      += stats.decoration_batched_vertices;
    totals.background_rows_rebuilt          += stats.background_rows_rebuilt;
    totals.background_rows_reused           += stats.background_rows_reused;
    totals.background_row_clean_reuse_skips += stats.background_row_clean_reuse_skips;
    totals.background_rows_removed          += stats.background_rows_removed;
    totals.background_row_cache_fallbacks   += stats.background_row_cache_fallbacks;
    totals.selection_rows_rebuilt           += stats.selection_rows_rebuilt;
    totals.selection_rows_reused            += stats.selection_rows_reused;
    totals.selection_row_clean_reuse_skips  += stats.selection_row_clean_reuse_skips;
    totals.selection_rows_removed           += stats.selection_rows_removed;
    totals.selection_row_cache_fallbacks    += stats.selection_row_cache_fallbacks;
    totals.decoration_rows_rebuilt          += stats.decoration_rows_rebuilt;
    totals.decoration_rows_reused           += stats.decoration_rows_reused;
    totals.decoration_row_clean_reuse_skips += stats.decoration_row_clean_reuse_skips;
    totals.decoration_rows_removed          += stats.decoration_rows_removed;
    totals.decoration_row_cache_fallbacks   += stats.decoration_row_cache_fallbacks;
    totals.graphic_rect_rows_rebuilt        += stats.graphic_rect_rows_rebuilt;
    totals.graphic_rect_rows_reused         += stats.graphic_rect_rows_reused;
    totals.graphic_rect_row_clean_reuse_skips +=
        stats.graphic_rect_row_clean_reuse_skips;
    totals.graphic_rect_rows_removed         += stats.graphic_rect_rows_removed;
    totals.graphic_rect_row_cache_fallbacks  += stats.graphic_rect_row_cache_fallbacks;
    totals.graphic_arc_rows_rebuilt          += stats.graphic_arc_rows_rebuilt;
    totals.graphic_arc_rows_reused           += stats.graphic_arc_rows_reused;
    totals.graphic_arc_row_clean_reuse_skips += stats.graphic_arc_row_clean_reuse_skips;
    totals.graphic_arc_rows_removed          += stats.graphic_arc_rows_removed;
    totals.graphic_arc_row_cache_fallbacks   += stats.graphic_arc_row_cache_fallbacks;
}

QString dominant_latency_component(const Scenario_result& result)
{
    struct latency_component_t
    {
        QString    name;
        qint64     median = 0;
    };

    const latency_component_t components[] = {
        { QStringLiteral("snapshot_prep"), result.snapshot_prep_ns.median },
        { QStringLiteral("workload_action"), result.workload_action_ns.median },
        { QStringLiteral("scene_graph_update_latency"), result.scene_graph_update_latency_ns.median },
        { QStringLiteral("scene_graph_render_wait"), result.scene_graph_render_wait_ns.median },
        { QStringLiteral("readback"), result.readback_ns.median },
    };

    latency_component_t dominant = components[0];
    for (const latency_component_t& component : components) {
        if (component.median > dominant.median) {
            dominant = component;
        }
    }

    return dominant.median > 0
        ? dominant.name
        : QStringLiteral("no_completed_samples");
}

QString primary_pressure(const Scenario_result& result)
{
    if (result.backend_writes_total > 0) {
        return QStringLiteral("backend_write_path");
    }

    if (result.workload_actions_expected_count > 0) {
        return QStringLiteral("backend_output_ingest");
    }

    if (result.renderer_totals.background_rows_rebuilt   > 0 ||
        result.renderer_totals.selection_rows_rebuilt    > 0 ||
        result.renderer_totals.decoration_rows_rebuilt   > 0 ||
        result.renderer_totals.graphic_rect_rows_rebuilt > 0 ||
        result.renderer_totals.graphic_arc_rows_rebuilt  > 0)
    {
        return QStringLiteral("renderer_geometry_row_cache");
    }

    if (result.renderer_totals.text_content_rebuilds >=
        result.renderer_totals.text_content_reused &&
        result.renderer_totals.text_content_rebuilds > 0)
    {
        return QStringLiteral("renderer_text_rebuild");
    }

    if (result.bridge_delta.coalesced_requests > 0U) {
        return QStringLiteral("render_update_coalescing");
    }

    if (result.lifecycle_delta.render_text_resources_created_delta   > 0U ||
        result.lifecycle_delta.render_text_resources_destroyed_delta > 0U)
    {
        return QStringLiteral("text_resource_lifecycle");
    }

    if (result.lifecycle_delta.render_rect_resources_created_delta   > 0U ||
        result.lifecycle_delta.render_rect_resources_destroyed_delta > 0U)
    {
        return QStringLiteral("rect_resource_lifecycle");
    }

    if (result.completed_frames > 0) {
        return QStringLiteral("scene_graph_render");
    }

    return QStringLiteral("no_completed_samples");
}

void finalize_bottleneck_signals(Scenario_result& result)
{
    result.dominant_latency_component = dominant_latency_component(result);
    result.primary_pressure           = primary_pressure(result);
}

bool text_work_observed_for_scenario(
    const QString&                         scenario_name,
    const term::terminal_renderer_stats_t& stats)
{
    if (!is_measurement_reuse_only_scenario(scenario_name)) {
        return stats.text_leaf_nodes_created > 0;
    }

    return
        stats.text_leaf_nodes_created         > 0 ||
        stats.text_content_reused             > 0 ||
        stats.text_resource_descriptor_reuses > 0;
}

void update_structural_checks(
    Scenario_result&       result,
    const Attempt_result&  attempt)
{
    const bool previous_snapshot_valid = result.structural_checks.snapshot_valid;
    result.structural_checks.snapshot_valid =
        result.structural_checks.snapshot_valid && attempt.snapshot_valid;
    if (previous_snapshot_valid && !attempt.snapshot_valid) {
        result.structural_checks.snapshot_status = attempt.snapshot_status;
    }
    result.structural_checks.last_rendered_snapshot_sequence =
        result.structural_checks.last_rendered_snapshot_sequence &&
        attempt.invalidation_stats.last_rendered_snapshot_sequence == attempt.sequence;
    result.structural_checks.pending_update_clear =
        result.structural_checks.pending_update_clear &&
        !attempt.invalidation_stats.pending_update;
    result.structural_checks.paint_completed =
        result.structural_checks.paint_completed && attempt.renderer_stats.paint_completed;
    result.structural_checks.text_content_failures_zero =
        result.structural_checks.text_content_failures_zero &&
        attempt.renderer_stats.text_content_failures == 0;
    result.structural_checks.text_work_observed =
        result.structural_checks.text_work_observed &&
        text_work_observed_for_scenario(result.name, attempt.renderer_stats);
    result.structural_checks.visible_pixels_observed =
        result.structural_checks.visible_pixels_observed &&
        attempt.visible_pixels_observed;
    result.structural_checks.rendered_pixels_changed =
        result.structural_checks.rendered_pixels_changed &&
        attempt.rendered_pixels_changed;
}

void finish_scenario_status(Scenario_result& result)
{
    const bool common_checks_failed =
        result.timeout_count > 0                             ||
        !result.structural_checks.no_child_qquick_items      ||
        !result.structural_checks.scrollback_limit_respected ||
        !result.structural_checks.workload_actions_accepted  ||
        !result.structural_checks.queue_pressure_observed    ||
        !result.structural_checks.backend_errors_zero;
    const bool render_checks_failed =
        result.render_expected &&
        (!result.structural_checks.snapshot_valid ||
            !result.structural_checks.last_rendered_snapshot_sequence ||
            !result.structural_checks.pending_update_clear ||
            !result.structural_checks.paint_completed ||
            !result.structural_checks.text_content_failures_zero ||
            !result.structural_checks.text_work_observed ||
            !result.structural_checks.visible_pixels_observed);
    const bool viewport_checks_failed =
        result.viewport_metrics_applicable &&
        (!result.structural_checks.rendered_pixels_changed ||
            !result.structural_checks.scrollback_rows_available ||
            !result.structural_checks.viewport_offset_changed ||
            !result.structural_checks.viewport_offset_expected ||
            !result.structural_checks.viewport_content_expected ||
            !result.structural_checks.wheel_events_accepted ||
            !result.structural_checks.backend_errors_zero);

    if (common_checks_failed || render_checks_failed || viewport_checks_failed)
    {
        result.status = result.timeout_count > 0
            ? QStringLiteral("timeout")
            : QStringLiteral("failed");
    }
}

std::vector<std::shared_ptr<const term::Terminal_render_snapshot>> make_scenario_snapshots(
    Benchmark_context& context,
    const QString&     scenario_name,
    grid_size_t        grid,
    int                phase)
{
    if (scenario_name == QStringLiteral("dense_repaint")) {
        return {
            make_dense_repaint_snapshot(grid, take_sequence(context), phase),
        };
    }

    if (scenario_name == QStringLiteral("ascii_full_dirty_reuse_only")) {
        return {
            make_ascii_full_dirty_reuse_only_snapshot(grid, take_sequence(context)),
        };
    }

    if (scenario_name == QStringLiteral("ascii_full_dirty_same_content")) {
        return {
            make_ascii_full_dirty_same_content_snapshot(
                grid,
                take_sequence(context),
                phase),
            make_ascii_full_dirty_same_content_snapshot(
                grid,
                take_sequence(context),
                phase),
        };
    }

    if (scenario_name == QStringLiteral("styled_ascii_full_dirty_reuse_only")) {
        return {
            make_styled_ascii_full_dirty_reuse_only_snapshot(
                grid,
                take_sequence(context)),
        };
    }

    if (scenario_name == QStringLiteral("mixed_text_full_dirty_reuse_only")) {
        return {
            make_mixed_text_full_dirty_reuse_only_snapshot(
                grid,
                take_sequence(context)),
        };
    }

    if (scenario_name == QStringLiteral("scroll_burst")) {
        std::vector<std::shared_ptr<const term::Terminal_render_snapshot>> snapshots;
        snapshots.reserve(k_scroll_burst_snapshots);
        for (int burst = 0; burst < k_scroll_burst_snapshots; ++burst) {
            snapshots.push_back(make_scroll_burst_snapshot(
                grid,
                take_sequence(context),
                phase * k_scroll_burst_snapshots + burst));
        }
        return snapshots;
    }

    if (scenario_name == QStringLiteral("resize_bounce")) {
        return {
            make_resize_bounce_snapshot(grid, take_sequence(context), phase),
        };
    }

    if (scenario_name == QStringLiteral("cursor_repaint_toggle")) {
        return {
            make_cursor_repaint_snapshot(grid, take_sequence(context), phase),
            make_cursor_repaint_snapshot(grid, take_sequence(context), phase + 1),
            make_cursor_repaint_snapshot(grid, take_sequence(context), phase + 2),
        };
    }

    if (scenario_name == QStringLiteral("single_row_geometry_update")) {
        return {
            make_single_row_geometry_update_snapshot(
                grid,
                take_sequence(context),
                phase,
                true),
            make_single_row_geometry_update_snapshot(
                grid,
                take_sequence(context),
                phase,
                false),
        };
    }

    return {
        make_unicode_wide_row_snapshot(grid, take_sequence(context), phase),
    };
}

void prepare_scenario_attempt(
    Benchmark_context& context,
    const App_options& options,
    const QString&     scenario_name,
    int                phase)
{
    if (scenario_name != QStringLiteral("resize_bounce")) {
        return;
    }

    const QSize base_size = options.window_size;
    const QSize sizes[] = {
        base_size,
        QSize(base_size.width() + 32, base_size.height() + 18),
        QSize(std::max(1, base_size.width() - 24), std::max(1, base_size.height() - 16)),
        QSize(base_size.width() + 12, std::max(1, base_size.height() - 10)),
    };
    const QSize target_size = sizes[static_cast<std::size_t>(phase) %
        (sizeof(sizes) / sizeof(sizes[0]))];
    context.window.resize(target_size);
    context.surface.setSize(QSizeF(
        static_cast<qreal>(target_size.width()),
        static_cast<qreal>(target_size.height())));
}

Attempt_result run_snapshot_bridge_attempt(
    Benchmark_context& context,
    const App_options& options,
    const QString&     scenario_name,
    int                phase)
{
    VNM_TERMINAL_PROFILE_SCOPE("run_snapshot_bridge_attempt");

    QElapsedTimer elapsed_timer;
    elapsed_timer.start();
    prepare_scenario_attempt(context, options, scenario_name, phase);

    QElapsedTimer snapshot_prep_timer;
    snapshot_prep_timer.start();
    std::vector<std::shared_ptr<const term::Terminal_render_snapshot>> snapshots = make_scenario_snapshots(
        context,
        scenario_name,
        options.grid,
        phase);
    const qint64 snapshot_prep_ns = snapshot_prep_timer.nsecsElapsed();

    Attempt_result attempt =
        submit_snapshots_and_wait(context, std::move(snapshots), elapsed_timer);
    attempt.snapshot_prep_ns = snapshot_prep_ns;
    return attempt;
}

surface_session_scroll_profile_t surface_session_scroll_profile(const QString& scenario_name)
{
    if (scenario_name == QStringLiteral("surface_session_viewport_pan")) {
        return {
            k_surface_session_viewport_pan_wheel_events,
            k_surface_session_viewport_pan_steps_per_event,
            1,
        };
    }

    return {};
}

int surface_session_output_line_count(
    const Benchmark_context&                   context,
    const App_options&                         options,
    const surface_session_scroll_profile_t&    scroll_profile)
{
    const int visible_rows =
        std::max({1, context.surface.rows(), options.grid.rows});
    const int scroll_lines_per_attempt =
        scroll_profile.wheel_event_count *
        scroll_profile.wheel_steps_per_event *
        k_surface_session_plain_wheel_lines_per_event;
    return
        std::max(
            k_surface_session_min_output_lines,
            visible_rows + (options.warmup + options.iterations) * scroll_lines_per_attempt + 32);
}

void update_surface_session_viewport_checks(
    Scenario_result&                           result,
    const surface_session_wheel_burst_t&       burst)
{
    result.structural_checks.wheel_events_accepted =
        result.structural_checks.wheel_events_accepted &&
        burst.accepted_count == result.wheel_burst_size;
    result.structural_checks.viewport_offset_changed =
        result.structural_checks.viewport_offset_changed &&
        burst.after_offset != burst.before_offset;
    result.structural_checks.viewport_offset_expected =
        result.structural_checks.viewport_offset_expected &&
        burst.after_offset == burst.expected_after_offset;
    result.structural_checks.viewport_content_expected =
        result.structural_checks.viewport_content_expected &&
        burst.content_expected;
}

Attempt_result run_surface_session_scroll_attempt(
    Benchmark_context&                         context,
    const QElapsedTimer&                       elapsed_timer,
    int                                        output_line_count,
    const surface_session_scroll_profile_t&    scroll_profile,
    surface_session_wheel_burst_t*             out_burst)
{
    VNM_TERMINAL_PROFILE_SCOPE("run_surface_session_scroll_attempt");

    const std::shared_ptr<const term::Terminal_render_snapshot> before_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(context.surface);
    if (before_snapshot == nullptr) {
        *out_burst = {};
        return wait_for_session_snapshot(context, nullptr, elapsed_timer, 0);
    }

    const int before_offset = before_snapshot->viewport.offset_from_tail;
    const int scroll_direction = scroll_profile.fixed_direction != 0
        ? scroll_profile.fixed_direction
        : (before_offset > 0 ? -1 : 1);
    const int wheel_delta =
        scroll_direction * 120 * scroll_profile.wheel_steps_per_event;
    const int expected_delta =
        scroll_direction *
        scroll_profile.wheel_event_count *
        scroll_profile.wheel_steps_per_event *
        k_surface_session_plain_wheel_lines_per_event;
    const int expected_after_offset = before_offset + expected_delta;

    qint64 readback_ns = 0;
    const QImage before_rendered_image =
        grab_window_with_timing(context, &readback_ns);
    const qint64 action_start_ns = elapsed_timer.nsecsElapsed();
    QElapsedTimer session_scroll_timer;
    session_scroll_timer.start();
    const int accepted_count = send_surface_wheel_burst(
        context.surface,
        wheel_delta,
        scroll_profile.wheel_event_count);
    const std::shared_ptr<const term::Terminal_render_snapshot> after_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(context.surface);
    const qint64 session_scroll_ns = session_scroll_timer.nsecsElapsed();
    Attempt_result attempt = wait_for_session_snapshot(
        context,
        after_snapshot,
        elapsed_timer,
        action_start_ns);

    const int after_offset = after_snapshot != nullptr
        ? after_snapshot->viewport.offset_from_tail
        : before_offset;

    out_burst->accepted_count        = accepted_count;
    out_burst->before_offset         = before_offset;
    out_burst->after_offset          = after_offset;
    out_burst->expected_after_offset = expected_after_offset;
    out_burst->expected_delta        = expected_delta;
    out_burst->final_delta           = after_offset - before_offset;

    if (after_snapshot != nullptr) {
        const int expected_top_line = expected_surface_scroll_top_line(
            *after_snapshot,
            output_line_count,
            expected_after_offset);
        const QString after_top_text = snapshot_row_text(*after_snapshot, 0);
        const std::optional<int> final_top_line =
            scripted_surface_scroll_line_index(after_top_text);
        out_burst->expected_top_line = expected_top_line;
        out_burst->final_top_line    = final_top_line.value_or(-1);
        out_burst->content_expected =
            final_top_line.has_value()                                            &&
            *final_top_line == expected_top_line                                  &&
            after_top_text == scripted_surface_scroll_line_text(expected_top_line);

        const int row_shift = std::abs(expected_delta);
        if (row_shift < before_snapshot->grid_size.rows &&
            row_shift < after_snapshot->grid_size.rows)
        {
            bool shifted_content_expected = false;
            if (expected_delta > 0) {
                shifted_content_expected =
                    snapshot_row_text(*after_snapshot, row_shift) ==
                    snapshot_row_text(*before_snapshot, 0);
            }
            else {
                shifted_content_expected =
                    snapshot_row_text(*after_snapshot, 0) ==
                    snapshot_row_text(*before_snapshot, row_shift);
            }

            out_burst->content_expected =
                out_burst->content_expected && shifted_content_expected;
        }
    }

    attempt.session_scroll_ns        = session_scroll_ns;
    attempt.workload_action_ns       = session_scroll_ns;
    attempt.readback_ns             += readback_ns;
    attempt.rendered_pixels_changed  = images_have_meaningful_pixel_delta(
        before_rendered_image,
        attempt.rendered_image);
    return attempt;
}

Scenario_result run_surface_session_scroll_scenario(
    Benchmark_context& context,
    const App_options& options,
    const QString&     scenario_name)
{
    const surface_session_scroll_profile_t scroll_profile =
        surface_session_scroll_profile(scenario_name);

    Scenario_result result;
    result.name                        = scenario_name;
    result.source_mode                 = scenario_source_mode(scenario_name);
    result.execution_mode              = scenario_execution_mode(scenario_name);
    result.iterations                  = options.iterations;
    result.warmup                      = options.warmup;
    result.window_size                 = options.window_size;
    result.viewport_metrics_applicable = true;
    result.wheel_burst_size            = scroll_profile.wheel_event_count;
    result.wheel_steps_per_event       = scroll_profile.wheel_steps_per_event;

    context.window.resize(options.window_size);
    context.surface.setSize(QSizeF(
        static_cast<qreal>(options.window_size.width()),
        static_cast<qreal>(options.window_size.height())));
    const int output_line_count =
        surface_session_output_line_count(context, options, scroll_profile);
    context.surface.set_scrollback_limit(output_line_count);
    result.scrollback_limit_configured = output_line_count;

    int backend_error_count = 0;
    const QMetaObject::Connection backend_error_connection = QObject::connect(
        &context.surface,
        &VNM_TerminalSurface::backend_error,
        &context.surface,
        [&backend_error_count](VNM_TerminalSurface::Backend_error_code, const QString&) {
            ++backend_error_count;
        });

    auto backend = std::make_unique<Scripted_backend>();
    backend->outputs_during_start = {
        numbered_scroll_lines(output_line_count),
    };
    Scripted_backend* backend_ptr = backend.get();
    const bool started = term::VNM_TerminalSurface_render_bridge::start_process_with_backend(
        context.surface,
        std::move(backend),
        {QStringLiteral("scripted-terminal")});
    if (!started) {
        result.structural_checks.backend_errors_zero       = backend_error_count == 0;
        result.structural_checks.scrollback_rows_available = false;
        result.status                                      = QStringLiteral("failed");
        QObject::disconnect(backend_error_connection);
        return result;
    }
    Surface_session_cleanup cleanup(context.surface, backend_error_connection);

    const std::shared_ptr<const term::Terminal_render_snapshot> initial_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(context.surface);
    QElapsedTimer setup_timer;
    setup_timer.start();
    Attempt_result setup_attempt =
        wait_for_session_snapshot(context, initial_snapshot, setup_timer, 0);
    update_structural_checks(result, setup_attempt);
    if (!setup_attempt.completed || !setup_attempt.snapshot_valid) {
        ++result.timeout_count;
        finish_scenario_status(result);
        return result;
    }

    result.rows    = initial_snapshot != nullptr ? initial_snapshot->grid_size.rows : 0;
    result.columns = initial_snapshot != nullptr ? initial_snapshot->grid_size.columns : 0;
    result.viewport_scrollback_rows =
        initial_snapshot != nullptr ? initial_snapshot->viewport.scrollback_rows : 0;
    result.viewport_initial_offset_from_tail =
        initial_snapshot != nullptr ? initial_snapshot->viewport.offset_from_tail : 0;
    result.structural_checks.scrollback_rows_available =
        result.viewport_scrollback_rows > 0;

    for (int warmup_index = 0; warmup_index < options.warmup; ++warmup_index) {
        surface_session_wheel_burst_t burst;
        QElapsedTimer elapsed_timer;
        elapsed_timer.start();
        Attempt_result attempt = run_surface_session_scroll_attempt(
            context,
            elapsed_timer,
            output_line_count,
            scroll_profile,
            &burst);
        update_structural_checks(result, attempt);
        update_surface_session_viewport_checks(result, burst);
        if (!attempt.completed) {
            ++result.timeout_count;
        }
        if (!attempt.completed || !attempt.snapshot_valid) {
            finish_scenario_status(result);
            return result;
        }
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> measured_initial_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(context.surface);
    result.viewport_initial_offset_from_tail = measured_initial_snapshot != nullptr
        ? measured_initial_snapshot->viewport.offset_from_tail
        : result.viewport_initial_offset_from_tail;
    const backend_measurement_baseline_t backend_baseline =
        backend_measurement_baseline(*backend_ptr);

    const term::Terminal_surface_render_invalidation_stats_t bridge_before =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(context.surface);
    const term::terminal_renderer_lifecycle_stats_t lifecycle_before =
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(context.surface);

    Timing_samples timing_samples;
    reserve_timing_samples(timing_samples, options.iterations);

    for (int iteration = 0; iteration < options.iterations; ++iteration) {
        surface_session_wheel_burst_t burst;
        QElapsedTimer elapsed_timer;
        elapsed_timer.start();
        Attempt_result attempt = run_surface_session_scroll_attempt(
            context,
            elapsed_timer,
            output_line_count,
            scroll_profile,
            &burst);

        add_renderer_stats(result.renderer_totals, attempt.renderer_stats);
        update_structural_checks(result, attempt);
        update_surface_session_viewport_checks(result, burst);
        result.wheel_events_accepted_count += burst.accepted_count;

        if (!attempt.completed) {
            ++result.timeout_count;
        }
        if (!attempt.completed || !attempt.snapshot_valid) {
            continue;
        }

        ++result.completed_frames;
        ++result.session_snapshots_observed;
        append_timing_sample(timing_samples, attempt);
        result.viewport_final_offset_from_tail    = burst.after_offset;
        result.viewport_expected_offset_from_tail = burst.expected_after_offset;
        result.viewport_expected_burst_delta      = burst.expected_delta;
        result.viewport_final_burst_delta         = burst.final_delta;
        result.viewport_expected_top_line         = burst.expected_top_line;
        result.viewport_final_top_line            = burst.final_top_line;
    }

    const term::Terminal_surface_render_invalidation_stats_t bridge_after =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(context.surface);
    const term::terminal_renderer_lifecycle_stats_t lifecycle_after =
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(context.surface);
    result.bridge_delta    = bridge_delta(bridge_before, bridge_after);
    result.lifecycle_delta = lifecycle_delta(lifecycle_before, lifecycle_after);
    summarize_timing_samples(result, timing_samples);
    result.structural_checks.no_child_qquick_items = context.surface.childItems().isEmpty();
    update_backend_measurement_deltas(result, *backend_ptr, backend_baseline);
    result.backend_errors_total = backend_error_count;
    result.structural_checks.backend_errors_zero = backend_error_count == 0;
    const std::shared_ptr<const term::Terminal_render_snapshot> final_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(context.surface);
    result.scrollback_limit_observed =
        final_snapshot != nullptr ? final_snapshot->viewport.scrollback_rows : 0;
    result.structural_checks.scrollback_limit_respected =
        final_snapshot != nullptr &&
        result.scrollback_limit_observed <= result.scrollback_limit_configured;
    finalize_bottleneck_signals(result);
    finish_scenario_status(result);
    return result;
}

struct surface_session_action_profile_t
{
    int    initial_output_lines       = 8;
    int    output_lines_per_attempt   = 0;
    int    scrollback_limit           = 0;
    bool   validate_scrollback_limit  = false;
    bool   paste_write                = false;
    bool   output_high_water_pressure = false;
    bool   write_high_water_pressure  = false;
    bool   render_expected            = true;
};

surface_session_action_profile_t surface_session_action_profile(const QString& scenario_name)
{
    if (scenario_name == QStringLiteral("surface_session_scrollback_limit")) {
        return {
            4,
            k_surface_session_scrollback_lines_per_attempt,
            k_surface_session_scrollback_limit,
            true,
            false,
            false,
            false,
            true,
        };
    }

    if (scenario_name == QStringLiteral("surface_session_paste_write")) {
        return { 8, 0, 0, false, true,  false, false, false };
    }

    if (scenario_name == QStringLiteral("surface_session_output_high_water")) {
        return { 8, 0, 0, false, false, true,  false, true };
    }

    if (scenario_name == QStringLiteral("surface_session_write_high_water")) {
        return { 8, 0, 0, false, true,  false, true,  false };
    }

    if (scenario_name == QStringLiteral("surface_session_alternate_churn")) {
        return { 4, 0, 0, false, false, false, false, true };
    }

    return {
        8,
        k_surface_session_sustained_lines_per_attempt,
        0,
        false,
        false,
        false,
        false,
        true,
    };
}

QByteArray surface_session_action_payload(const QString& scenario_name, int phase)
{
    if (scenario_name == QStringLiteral("surface_session_alternate_churn")) {
        return alternate_screen_churn_payload(phase);
    }

    if (scenario_name == QStringLiteral("surface_session_scrollback_limit")) {
        return
            numbered_workload_lines(
                QStringLiteral("scrollback-limit"),
                phase,
                k_surface_session_scrollback_lines_per_attempt);
    }

    return
        numbered_workload_lines(
            QStringLiteral("sustained-output"),
            phase,
            k_surface_session_sustained_lines_per_attempt);
}

std::shared_ptr<const term::Terminal_render_snapshot> wait_for_session_snapshot_change(
    Benchmark_context& context,
    std::uint64_t      previous_sequence)
{
    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < k_render_timeout_ms) {
        context.app.processEvents(QEventLoop::AllEvents, 5);
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(context.surface);
        if (snapshot != nullptr && snapshot->metadata.sequence > previous_sequence) {
            return snapshot;
        }
        QThread::msleep(1);
    }

    return term::VNM_TerminalSurface_render_bridge::render_snapshot(context.surface);
}

Attempt_result run_surface_session_action_attempt(
    Benchmark_context&                         context,
    const QElapsedTimer&                       elapsed_timer,
    Scripted_backend&                          backend,
    const QString&                             scenario_name,
    int                                        phase,
    const surface_session_action_profile_t&    profile,
    bool*                                      out_action_accepted)
{
    VNM_TERMINAL_PROFILE_SCOPE("run_surface_session_action_attempt");

    const std::shared_ptr<const term::Terminal_render_snapshot> before_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(context.surface);
    const std::uint64_t previous_sequence =
        before_snapshot != nullptr ? before_snapshot->metadata.sequence : 0U;

    const qint64 action_start_ns = elapsed_timer.nsecsElapsed();
    QElapsedTimer action_timer;
    action_timer.start();
    if (profile.paste_write) {
        const QString text = profile.write_high_water_pressure
            ? write_high_water_paste_text(phase)
            : paste_workload_text(phase);
        *out_action_accepted = context.surface.paste_text(text);
    }
    else
    if (profile.output_high_water_pressure) {
        *out_action_accepted = true;
        for (int chunk_index = 0;
            chunk_index < k_surface_session_output_high_water_chunks;
            ++chunk_index)
        {
            *out_action_accepted =
                backend.emit_output(output_high_water_chunk(phase, chunk_index)) &&
                *out_action_accepted;
        }
    }
    else {
        *out_action_accepted = backend.emit_output(
            surface_session_action_payload(scenario_name, phase));
    }
    const qint64 action_ns = action_timer.nsecsElapsed();

    if (!profile.render_expected) {
        Attempt_result attempt;
        attempt.completed                      = *out_action_accepted;
        attempt.snapshot_valid                 = true;
        attempt.elapsed_ns                     = elapsed_timer.nsecsElapsed();
        attempt.workload_action_ns             = action_ns;
        attempt.renderer_stats.paint_completed = true;
        return attempt;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> after_snapshot = wait_for_session_snapshot_change(
        context,
        previous_sequence);

    Attempt_result attempt = wait_for_session_snapshot(
        context,
        after_snapshot,
        elapsed_timer,
        action_start_ns);
    attempt.workload_action_ns = action_ns;

    if (after_snapshot == nullptr || after_snapshot->metadata.sequence <= previous_sequence)
    {
        attempt.snapshot_valid  = false;
        attempt.snapshot_status = QStringLiteral("MISSING_SESSION_SNAPSHOT_UPDATE");
    }

    return attempt;
}

Scenario_result run_surface_session_action_scenario(
    Benchmark_context& context,
    const App_options& options,
    const QString&     scenario_name)
{
    const surface_session_action_profile_t profile =
        surface_session_action_profile(scenario_name);

    Scenario_result result;
    result.name                         = scenario_name;
    result.source_mode                  = scenario_source_mode(scenario_name);
    result.execution_mode               = scenario_execution_mode(scenario_name);
    result.iterations                   = options.iterations;
    result.warmup                       = options.warmup;
    result.window_size                  = options.window_size;
    result.render_expected              = profile.render_expected;
    result.render_measurement_semantics = profile.render_expected
        ? k_render_expected_semantics
        : k_no_render_expected_semantics;
    if (profile.output_high_water_pressure) {
        result.queue_pressure_semantics =
            k_output_high_water_queue_pressure_semantics;
    }
    else
    if (profile.write_high_water_pressure) {
        result.queue_pressure_semantics = QStringLiteral(
            "write_queue_byte_high_water_reserved_by_accepted_paste_payload");
    }

    context.window.resize(options.window_size);
    context.surface.setSize(QSizeF(
        static_cast<qreal>(options.window_size.width()),
        static_cast<qreal>(options.window_size.height())));
    const int effective_scrollback_limit = profile.scrollback_limit > 0
        ? profile.scrollback_limit
        : context.default_scrollback_limit;
    context.surface.set_scrollback_limit(effective_scrollback_limit);
    result.scrollback_limit_configured = effective_scrollback_limit;

    int backend_error_count = 0;
    const QMetaObject::Connection backend_error_connection = QObject::connect(
        &context.surface,
        &VNM_TerminalSurface::backend_error,
        &context.surface,
        [&backend_error_count](VNM_TerminalSurface::Backend_error_code, const QString&) {
            ++backend_error_count;
        });

    auto backend = std::make_unique<Scripted_backend>();
    if (profile.initial_output_lines > 0) {
        backend->outputs_during_start = {
            numbered_workload_lines(
                QStringLiteral("session-start"),
                0,
                profile.initial_output_lines),
        };
    }
    Scripted_backend* backend_ptr = backend.get();
    const bool started = term::VNM_TerminalSurface_render_bridge::start_process_with_backend(
        context.surface,
        std::move(backend),
        {QStringLiteral("scripted-terminal")});
    if (!started) {
        result.structural_checks.backend_errors_zero = backend_error_count == 0;
        result.status = QStringLiteral("failed");
        QObject::disconnect(backend_error_connection);
        return result;
    }
    Surface_session_cleanup cleanup(context.surface, backend_error_connection);

    const std::shared_ptr<const term::Terminal_render_snapshot> initial_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(context.surface);
    QElapsedTimer setup_timer;
    setup_timer.start();
    Attempt_result setup_attempt =
        wait_for_session_snapshot(context, initial_snapshot, setup_timer, 0);
    if (profile.render_expected) {
        update_structural_checks(result, setup_attempt);
    }
    if (!setup_attempt.completed || !setup_attempt.snapshot_valid) {
        ++result.timeout_count;
        if (!profile.render_expected) {
            result.status = setup_attempt.completed
                ? QStringLiteral("failed")
                : QStringLiteral("timeout");
        }
        finish_scenario_status(result);
        return result;
    }

    result.rows    = initial_snapshot != nullptr ? initial_snapshot->grid_size.rows : 0;
    result.columns = initial_snapshot != nullptr ? initial_snapshot->grid_size.columns : 0;

    for (int warmup_index = 0; warmup_index < options.warmup; ++warmup_index) {
        bool action_accepted = false;
        QElapsedTimer elapsed_timer;
        elapsed_timer.start();
        Attempt_result attempt = run_surface_session_action_attempt(
            context,
            elapsed_timer,
            *backend_ptr,
            scenario_name,
            warmup_index,
            profile,
            &action_accepted);
        if (profile.render_expected) {
            update_structural_checks(result, attempt);
        }
        result.structural_checks.workload_actions_accepted =
            result.structural_checks.workload_actions_accepted && action_accepted;
        if (!attempt.completed) {
            ++result.timeout_count;
        }
        if (!attempt.completed || !attempt.snapshot_valid) {
            finish_scenario_status(result);
            return result;
        }
    }

    const backend_measurement_baseline_t backend_baseline =
        backend_measurement_baseline(*backend_ptr);

    const term::Terminal_surface_render_invalidation_stats_t bridge_before =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(context.surface);
    const term::terminal_renderer_lifecycle_stats_t lifecycle_before =
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(context.surface);

    Timing_samples timing_samples;
    reserve_timing_samples(timing_samples, options.iterations);

    for (int iteration = 0; iteration < options.iterations; ++iteration) {
        const int phase           = options.warmup + iteration;
        bool      action_accepted = false;
        QElapsedTimer elapsed_timer;
        elapsed_timer.start();
        Attempt_result attempt = run_surface_session_action_attempt(
            context,
            elapsed_timer,
            *backend_ptr,
            scenario_name,
            phase,
            profile,
            &action_accepted);

        add_renderer_stats(result.renderer_totals, attempt.renderer_stats);
        if (profile.render_expected) {
            update_structural_checks(result, attempt);
        }
        ++result.workload_actions_expected_count;
        if (action_accepted) {
            ++result.workload_actions_accepted_count;
        }
        result.structural_checks.workload_actions_accepted =
            result.structural_checks.workload_actions_accepted && action_accepted;

        if (!attempt.completed) {
            ++result.timeout_count;
        }
        if (!attempt.completed || !attempt.snapshot_valid) {
            continue;
        }

        ++result.completed_frames;
        if (profile.render_expected) {
            ++result.session_snapshots_observed;
        }
        append_timing_sample(timing_samples, attempt);
    }

    const term::Terminal_surface_render_invalidation_stats_t bridge_after =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(context.surface);
    const term::terminal_renderer_lifecycle_stats_t lifecycle_after =
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(context.surface);
    result.bridge_delta    = bridge_delta(bridge_before, bridge_after);
    result.lifecycle_delta = lifecycle_delta(lifecycle_before, lifecycle_after);
    summarize_timing_samples(result, timing_samples);
    result.structural_checks.no_child_qquick_items = context.surface.childItems().isEmpty();
    update_backend_measurement_deltas(result, *backend_ptr, backend_baseline);
    result.backend_errors_total = backend_error_count;
    result.structural_checks.backend_errors_zero = backend_error_count == 0;

    const std::shared_ptr<const term::Terminal_render_snapshot> final_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(context.surface);
    result.viewport_scrollback_rows =
        final_snapshot != nullptr ? final_snapshot->viewport.scrollback_rows : 0;
    result.scrollback_limit_observed = result.viewport_scrollback_rows;
    if (profile.validate_scrollback_limit) {
        result.structural_checks.scrollback_rows_available =
            result.viewport_scrollback_rows > 0;
    }
    result.structural_checks.scrollback_limit_respected =
        final_snapshot != nullptr &&
        result.viewport_scrollback_rows <= result.scrollback_limit_configured;
    if (profile.output_high_water_pressure) {
        result.structural_checks.queue_pressure_observed =
            result.output_high_water_observed;
        result.structural_checks.text_work_observed =
            result.renderer_totals.text_leaf_nodes_created > 0;
    }
    else
    if (profile.write_high_water_pressure) {
        result.structural_checks.queue_pressure_observed =
            result.write_high_water_observed;
    }

    finalize_bottleneck_signals(result);
    finish_scenario_status(result);
    return result;
}

Scenario_result run_scenario_impl(
    Benchmark_context& context,
    const App_options& options,
    const QString&     scenario_name)
{
    if (scenario_name == QStringLiteral("surface_session_wheel_scroll") ||
        scenario_name == QStringLiteral("surface_session_viewport_pan"))
    {
        return run_surface_session_scroll_scenario(context, options, scenario_name);
    }

    if (is_surface_session_scenario(scenario_name)) {
        return run_surface_session_action_scenario(context, options, scenario_name);
    }

    Scenario_result result;
    result.name           = scenario_name;
    result.source_mode    = scenario_source_mode(scenario_name);
    result.execution_mode = scenario_execution_mode(scenario_name);
    result.iterations     = options.iterations;
    result.warmup         = options.warmup;
    result.rows           = options.grid.rows;
    result.columns        = options.grid.columns;
    result.window_size    = options.window_size;

    for (int warmup_index = 0; warmup_index < options.warmup; ++warmup_index) {
        Attempt_result attempt =
            run_snapshot_bridge_attempt(context, options, scenario_name, warmup_index);
        if (!attempt.completed) {
            ++result.timeout_count;
        }
        if (!attempt.completed || !attempt.snapshot_valid) {
            update_structural_checks(result, attempt);
            finish_scenario_status(result);
            return result;
        }
    }

    const term::Terminal_surface_render_invalidation_stats_t bridge_before =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(context.surface);
    const term::terminal_renderer_lifecycle_stats_t lifecycle_before =
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(context.surface);

    Timing_samples timing_samples;
    reserve_timing_samples(timing_samples, options.iterations);

    for (int iteration = 0; iteration < options.iterations; ++iteration) {
        const int phase = options.warmup + iteration;
        Attempt_result attempt =
            run_snapshot_bridge_attempt(context, options, scenario_name, phase);

        add_renderer_stats(result.renderer_totals, attempt.renderer_stats);
        update_structural_checks(result, attempt);

        if (!attempt.completed) {
            ++result.timeout_count;
        }
        if (!attempt.completed || !attempt.snapshot_valid) {
            continue;
        }

        ++result.completed_frames;
        append_timing_sample(timing_samples, attempt);
    }

    const term::Terminal_surface_render_invalidation_stats_t bridge_after =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(context.surface);
    const term::terminal_renderer_lifecycle_stats_t lifecycle_after =
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(context.surface);
    result.bridge_delta    = bridge_delta(bridge_before, bridge_after);
    result.lifecycle_delta = lifecycle_delta(lifecycle_before, lifecycle_after);
    summarize_timing_samples(result, timing_samples);
    result.structural_checks.no_child_qquick_items = context.surface.childItems().isEmpty();
    finalize_bottleneck_signals(result);
    finish_scenario_status(result);

    context.window.resize(options.window_size);
    context.surface.setSize(QSizeF(
        static_cast<qreal>(options.window_size.width()),
        static_cast<qreal>(options.window_size.height())));
    return result;
}

term::Render_profile_snapshot_t wait_for_render_profile_quiescence(
    Benchmark_context& context)
{
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(context.surface);
    if (snapshot == nullptr) {
        return {};
    }

    QElapsedTimer elapsed_timer;
    elapsed_timer.start();
    (void)wait_for_render_completion(
        context,
        snapshot->metadata.sequence,
        elapsed_timer,
        false);

    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < k_render_timeout_ms) {
        const term::Render_profile_snapshot_t profile_snapshot =
            term::VNM_TerminalSurface_render_bridge::render_profiler_snapshot(
                context.surface);
        if (profile_snapshot.sequence >= snapshot->metadata.sequence) {
            return profile_snapshot;
        }

        context.app.processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }

    return term::VNM_TerminalSurface_render_bridge::render_profiler_snapshot(
        context.surface);
}

Scenario_result run_scenario(
    Benchmark_context& context,
    const App_options& options,
    const QString&     scenario_name)
{
#if VNM_TERMINAL_PROFILING_ENABLED
    if (!options.profile) {
        return run_scenario_impl(context, options, scenario_name);
    }

    auto render_profiler = std::make_shared<term::Hierarchical_profiler>();
    term::VNM_TerminalSurface_render_bridge::set_render_profiler(
        context.surface,
        render_profiler);

    term::Hierarchical_profiler gui_profiler;
    term::Active_profiler_binding binding(&gui_profiler);

    Scenario_result result;
    term::Render_profile_snapshot_t render_profile_snapshot;
    {
        VNM_TERMINAL_PROFILE_SCOPE("run_scenario");
        result = run_scenario_impl(context, options, scenario_name);
        render_profile_snapshot = wait_for_render_profile_quiescence(context);
    }

    term::VNM_TerminalSurface_render_bridge::set_render_profiler(context.surface, {});
    result.profile_threads.push_back({
        QStringLiteral("gui"),
        0,
        gui_profiler.root_snapshot(),
    });
    result.profile_threads.push_back({
        QStringLiteral("render"),
        0,
        std::move(render_profile_snapshot.root),
    });
    return result;
#else
    return run_scenario_impl(context, options, scenario_name);
#endif
}

QJsonObject summary_json(const sample_summary_t& summary)
{
    QJsonObject object;
    object.insert(QStringLiteral("sample_count"), summary.sample_count);
    object.insert(QStringLiteral("total"),        summary.total);
    object.insert(QStringLiteral("min"),          summary.min);
    object.insert(QStringLiteral("median"),       summary.median);
    object.insert(QStringLiteral("p95"),          summary.p95);
    object.insert(QStringLiteral("max"),          summary.max);
    return object;
}

QJsonValue normalized_per_consumed_update_value(qint64 total, std::uint64_t consumed_updates)
{
    if (consumed_updates == 0U) {
        return QJsonValue(QJsonValue::Null);
    }

    return static_cast<double>(total) / static_cast<double>(consumed_updates);
}

QJsonValue normalized_per_completed_update_latency_value(const Scenario_result& result)
{
    if (result.completed_frames <= 0 || result.bridge_delta.consumed_updates == 0U) {
        return QJsonValue(QJsonValue::Null);
    }

    return
        static_cast<double>(result.scene_graph_update_latency_ns.median) *
        static_cast<double>(result.completed_frames)                     /
        static_cast<double>(result.bridge_delta.consumed_updates);
}

QJsonValue consumed_updates_per_completed_attempt_value(const Scenario_result& result)
{
    if (result.completed_frames <= 0) {
        return QJsonValue(QJsonValue::Null);
    }

    return static_cast<double>(result.bridge_delta.consumed_updates) /
        static_cast<double>(result.completed_frames);
}

QString latency_normalization_semantics(const Scenario_result& result)
{
    if (result.completed_frames <= 0 || result.bridge_delta.consumed_updates == 0U) {
        return k_latency_normalization_unavailable;
    }

    return result.bridge_delta.consumed_updates ==
            static_cast<std::uint64_t>(result.completed_frames)
                ? k_latency_normalization_single_update
                : k_latency_normalization_multi_update_approximate;
}

QJsonObject normalized_per_consumed_update_json(const Scenario_result& result)
{
    const std::uint64_t denominator = result.bridge_delta.consumed_updates;

    QJsonObject counters;
    counters.insert(
        QStringLiteral("qt_text_layout_calls"),
        normalized_per_consumed_update_value(
            result.renderer_totals.qt_text_layout_calls,
            denominator));
    counters.insert(
        QStringLiteral("text_leaf_nodes_created"),
        normalized_per_consumed_update_value(
            result.renderer_totals.text_leaf_nodes_created,
            denominator));
    counters.insert(
        QStringLiteral("text_key_builds"),
        normalized_per_consumed_update_value(
            result.renderer_totals.text_key_builds,
            denominator));
    counters.insert(
        QStringLiteral("text_key_bytes"),
        normalized_per_consumed_update_value(
            static_cast<qint64>(result.renderer_totals.text_key_bytes),
            denominator));
    counters.insert(
        QStringLiteral("renderer_text_rebuilds_total"),
        normalized_per_consumed_update_value(
            result.renderer_totals.text_content_rebuilds,
            denominator));
    counters.insert(
        QStringLiteral("renderer_text_reused_total"),
        normalized_per_consumed_update_value(
            result.renderer_totals.text_content_reused,
            denominator));
    counters.insert(
        QStringLiteral("text_resource_descriptor_reuses"),
        normalized_per_consumed_update_value(
            result.renderer_totals.text_resource_descriptor_reuses,
            denominator));
    counters.insert(
        QStringLiteral("qsg_nodes_created"),
        normalized_per_consumed_update_value(
            result.renderer_totals.qsg_nodes_created,
            denominator));
    counters.insert(
        QStringLiteral("qsg_nodes_replaced"),
        normalized_per_consumed_update_value(
            result.renderer_totals.qsg_nodes_replaced,
            denominator));
    counters.insert(
        QStringLiteral("qsg_nodes_destroyed"),
        normalized_per_consumed_update_value(
            result.renderer_totals.qsg_nodes_destroyed,
            denominator));

    QJsonObject timing;
    timing.insert(QStringLiteral("semantics"), k_per_consumed_update_timing_semantics);
    timing.insert(
        QStringLiteral("normalization"),
        latency_normalization_semantics(result));
    timing.insert(
        QStringLiteral("scene_graph_update_latency_ns_median_per_attempt"),
        result.scene_graph_update_latency_ns.median);
    timing.insert(
        QStringLiteral("scene_graph_update_latency_ns_median_per_consumed_update"),
        normalized_per_completed_update_latency_value(result));

    QJsonObject object;
    object.insert(QStringLiteral("available"), denominator != 0U);
    object.insert(
        QStringLiteral("denominator_key"),
        QStringLiteral("bridge_consumed_updates_delta"));
    object.insert(QStringLiteral("denominator"),        static_cast<qint64>(denominator));
    object.insert(QStringLiteral("completed_attempts"), result.completed_frames);
    object.insert(
        QStringLiteral("consumed_updates_per_completed_attempt"),
        consumed_updates_per_completed_attempt_value(result));
    object.insert(QStringLiteral("counter_semantics"), k_per_consumed_update_counter_semantics);
    object.insert(QStringLiteral("counters"),          counters);
    object.insert(QStringLiteral("timing"),            timing);
    return object;
}

QJsonObject structural_checks_json(const Structural_checks& checks)
{
    QJsonObject object;
    object.insert(QStringLiteral("snapshot_valid"),  checks.snapshot_valid);
    object.insert(QStringLiteral("snapshot_status"), checks.snapshot_status);
    object.insert(
        QStringLiteral("last_rendered_snapshot_sequence"),
        checks.last_rendered_snapshot_sequence);
    object.insert(QStringLiteral("pending_update_clear"), checks.pending_update_clear);
    object.insert(QStringLiteral("paint_completed"),      checks.paint_completed);
    object.insert(
        QStringLiteral("text_content_failures_zero"),
        checks.text_content_failures_zero);
    object.insert(QStringLiteral("text_work_observed"),        checks.text_work_observed);
    object.insert(QStringLiteral("visible_pixels_observed"),   checks.visible_pixels_observed);
    object.insert(QStringLiteral("rendered_pixels_changed"),   checks.rendered_pixels_changed);
    object.insert(QStringLiteral("no_child_qquick_items"),     checks.no_child_qquick_items);
    object.insert(QStringLiteral("scrollback_rows_available"), checks.scrollback_rows_available);
    object.insert(QStringLiteral("viewport_offset_changed"),   checks.viewport_offset_changed);
    object.insert(QStringLiteral("viewport_offset_expected"),  checks.viewport_offset_expected);
    object.insert(QStringLiteral("viewport_content_expected"), checks.viewport_content_expected);
    object.insert(QStringLiteral("wheel_events_accepted"),     checks.wheel_events_accepted);
    object.insert(QStringLiteral("backend_errors_zero"),       checks.backend_errors_zero);
    object.insert(
        QStringLiteral("scrollback_limit_respected"),
        checks.scrollback_limit_respected);
    object.insert(
        QStringLiteral("workload_actions_accepted"),
        checks.workload_actions_accepted);
    object.insert(
        QStringLiteral("queue_pressure_observed"),
        checks.queue_pressure_observed);
    return object;
}

QJsonObject lifecycle_delta_json(const lifecycle_delta_t& delta)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("release_resources_calls_delta"),
        static_cast<qint64>(delta.release_resources_calls_delta));
    object.insert(
        QStringLiteral("item_scene_changes_delta"),
        static_cast<qint64>(delta.item_scene_changes_delta));
    object.insert(
        QStringLiteral("item_scene_detaches_delta"),
        static_cast<qint64>(delta.item_scene_detaches_delta));
    object.insert(
        QStringLiteral("item_destructions_delta"),
        static_cast<qint64>(delta.item_destructions_delta));
    object.insert(
        QStringLiteral("scene_graph_invalidated_calls_delta"),
        static_cast<qint64>(delta.scene_graph_invalidated_calls_delta));
    object.insert(
        QStringLiteral("render_node_deletions_in_paint_delta"),
        static_cast<qint64>(delta.render_node_deletions_in_paint_delta));
    object.insert(
        QStringLiteral("render_root_nodes_created_delta"),
        static_cast<qint64>(delta.render_root_nodes_created_delta));
    object.insert(
        QStringLiteral("render_root_nodes_destroyed_delta"),
        static_cast<qint64>(delta.render_root_nodes_destroyed_delta));
    object.insert(
        QStringLiteral("render_text_resources_created_delta"),
        static_cast<qint64>(delta.render_text_resources_created_delta));
    object.insert(
        QStringLiteral("render_text_resources_destroyed_delta"),
        static_cast<qint64>(delta.render_text_resources_destroyed_delta));
    object.insert(
        QStringLiteral("render_rect_resources_created_delta"),
        static_cast<qint64>(delta.render_rect_resources_created_delta));
    object.insert(
        QStringLiteral("render_rect_resources_destroyed_delta"),
        static_cast<qint64>(delta.render_rect_resources_destroyed_delta));
    object.insert(QStringLiteral("live_root_nodes"), static_cast<qint64>(delta.live_root_nodes));
    object.insert(
        QStringLiteral("live_text_resources"),
        static_cast<qint64>(delta.live_text_resources));
    object.insert(
        QStringLiteral("live_rect_resources"),
        static_cast<qint64>(delta.live_rect_resources));
    return object;
}

QJsonObject process_memory_json(const memory_summary_t& memory)
{
    QJsonObject object;
    object.insert(QStringLiteral("status"), memory.status);
    object.insert(
        QStringLiteral("platform"),
        memory.platform.isEmpty() ? process_memory_platform() : memory.platform);
    object.insert(QStringLiteral("metric"),         memory.metric);
    object.insert(QStringLiteral("semantics"),      memory.semantics);
    object.insert(QStringLiteral("resident_bytes"), summary_json(memory.resident_bytes));
    return object;
}

qint64 profile_nanoseconds(std::chrono::nanoseconds duration)
{
    return static_cast<qint64>(duration.count());
}

QJsonObject profile_node_json(const term::Profile_node_snapshot& node)
{
    const qint64 total_ns = profile_nanoseconds(node.total_time);
    const qint64 calls    = static_cast<qint64>(node.call_count);

    QJsonArray children;
    qint64 child_ns = 0;
    for (const term::Profile_node_snapshot& child : node.children) {
        child_ns += profile_nanoseconds(child.total_time);
        children.push_back(profile_node_json(child));
    }

    QJsonObject object;
    object.insert(QStringLiteral("name"),     QString::fromStdString(node.name));
    object.insert(QStringLiteral("calls"),    calls);
    object.insert(QStringLiteral("total_ns"), total_ns);
    object.insert(QStringLiteral("self_ns"),  profile_nanoseconds(node.self_time));
    object.insert(QStringLiteral("child_ns"), child_ns);
    object.insert(QStringLiteral("min_ns"),   profile_nanoseconds(node.min_time));
    object.insert(QStringLiteral("max_ns"),   profile_nanoseconds(node.max_time));
    object.insert(QStringLiteral("mean_ns"),  calls > 0 ? total_ns / calls : 0);
    object.insert(QStringLiteral("children"), children);
    return object;
}

QJsonObject profiling_metadata_json(const App_options& options)
{
    QJsonObject object;
    object.insert(QStringLiteral("enabled"),                options.profile);
    object.insert(QStringLiteral("profile_schema_version"), k_profile_schema_version);
    object.insert(QStringLiteral("time_unit"),              k_profile_time_unit);
    object.insert(QStringLiteral("thread_semantics"),       k_profile_thread_semantics);
    object.insert(QStringLiteral("profile_json_requested"), !options.profile_json_path.isEmpty());
    object.insert(QStringLiteral("profile_text_requested"), !options.profile_text_path.isEmpty());
    return object;
}

QJsonObject profile_thread_json(const profile_thread_snapshot_t& thread_snapshot)
{
    QJsonObject thread;
    thread.insert(QStringLiteral("thread_role"),  thread_snapshot.role);
    thread.insert(QStringLiteral("thread_index"), thread_snapshot.index);
    thread.insert(QStringLiteral("root"),         profile_node_json(thread_snapshot.root));
    return thread;
}

QJsonObject scenario_profile_json(
    const std::vector<profile_thread_snapshot_t>& thread_snapshots)
{
    QJsonArray threads;
    for (const profile_thread_snapshot_t& thread_snapshot : thread_snapshots) {
        threads.push_back(profile_thread_json(thread_snapshot));
    }

    QJsonObject object;
    object.insert(QStringLiteral("profile_schema_version"), k_profile_schema_version);
    object.insert(QStringLiteral("time_unit"),              k_profile_time_unit);
    object.insert(QStringLiteral("thread_semantics"),       k_profile_thread_semantics);
    object.insert(QStringLiteral("threads"),                threads);
    return object;
}

QJsonValue scenario_profile_value(const Scenario_result& result)
{
    return result.profile_threads.empty()
        ? QJsonValue()
        : QJsonValue(scenario_profile_json(result.profile_threads));
}

QJsonObject scenario_profile_summary_json(const Scenario_result& result)
{
    QJsonObject object;
    object.insert(QStringLiteral("name"),           result.name);
    object.insert(QStringLiteral("source_mode"),    result.source_mode);
    object.insert(QStringLiteral("execution_mode"), result.execution_mode);
    object.insert(
        QStringLiteral("background_batched_rects"),
        result.renderer_totals.background_batched_rects);
    object.insert(
        QStringLiteral("selection_batched_rects"),
        result.renderer_totals.selection_batched_rects);
    object.insert(
        QStringLiteral("graphic_batched_rects"),
        result.renderer_totals.graphic_batched_rects);
    object.insert(
        QStringLiteral("decoration_batched_rects"),
        result.renderer_totals.decoration_batched_rects);
    object.insert(QStringLiteral("profile"), scenario_profile_value(result));
    return object;
}

QJsonObject scenario_json(const Scenario_result& result)
{
    QJsonObject window_size;
    window_size.insert(QStringLiteral("width"),  result.window_size.width());
    window_size.insert(QStringLiteral("height"), result.window_size.height());

    QJsonObject object;
    object.insert(QStringLiteral("name"),              result.name);
    object.insert(QStringLiteral("source_mode"),       result.source_mode);
    object.insert(QStringLiteral("execution_mode"),    result.execution_mode);
    object.insert(QStringLiteral("latency_semantics"), k_latency_semantics);
    object.insert(QStringLiteral("elapsed_semantics"), k_elapsed_semantics);
    object.insert(QStringLiteral("status"),            result.status);
    object.insert(QStringLiteral("iterations"),        result.iterations);
    object.insert(QStringLiteral("warmup"),            result.warmup);
    object.insert(QStringLiteral("elapsed_ns"),        summary_json(result.elapsed_ns));
    object.insert(
        QStringLiteral("snapshot_prep_ns"),
        summary_json(result.snapshot_prep_ns));
    object.insert(
        QStringLiteral("session_scroll_ns"),
        summary_json(result.session_scroll_ns));
    object.insert(
        QStringLiteral("workload_action_ns"),
        summary_json(result.workload_action_ns));
    object.insert(
        QStringLiteral("scene_graph_update_latency_ns"),
        summary_json(result.scene_graph_update_latency_ns));
    object.insert(
        QStringLiteral("scene_graph_render_wait_ns"),
        summary_json(result.scene_graph_render_wait_ns));
    object.insert(QStringLiteral("readback_ns"),      summary_json(result.readback_ns));
    object.insert(QStringLiteral("completed_frames"), result.completed_frames);
    object.insert(QStringLiteral("timeout_count"),    result.timeout_count);
    object.insert(
        QStringLiteral("renderer_text_rebuilds_total"),
        result.renderer_totals.text_content_rebuilds);
    object.insert(
        QStringLiteral("renderer_text_reused_total"),
        result.renderer_totals.text_content_reused);
    object.insert(
        QStringLiteral("renderer_text_removed_total"),
        result.renderer_totals.text_content_removed);
    object.insert(
        QStringLiteral("renderer_text_failures_total"),
        result.renderer_totals.text_content_failures);
    object.insert(
        QStringLiteral("text_leaf_nodes_created"),
        result.renderer_totals.text_leaf_nodes_created);
    object.insert(
        QStringLiteral("text_cache_entry_child_nodes_cleared_for_replacement"),
        result.renderer_totals.text_cache_entry_child_nodes_cleared_for_replacement);
    object.insert(
        QStringLiteral("text_cache_entry_child_nodes_cleared_for_removal"),
        result.renderer_totals.text_cache_entry_child_nodes_cleared_for_removal);
    object.insert(
        QStringLiteral("text_cache_entry_max_child_nodes_cleared"),
        result.renderer_totals.text_cache_entry_max_child_nodes_cleared);
    object.insert(
        QStringLiteral("text_clean_reuse_skips"),
        result.renderer_totals.text_clean_reuse_skips);
    object.insert(
        QStringLiteral("text_resource_descriptor_reuses"),
        result.renderer_totals.text_resource_descriptor_reuses);
    object.insert(
        QStringLiteral("text_coalescing_candidate_groups"),
        result.renderer_totals.text_coalescing_candidate_groups);
    object.insert(
        QStringLiteral("text_coalescing_enabled_groups"),
        result.renderer_totals.text_coalescing_enabled_groups);
    object.insert(
        QStringLiteral("text_resource_runs_before_coalescing"),
        result.renderer_totals.text_resource_runs_before_coalescing);
    object.insert(
        QStringLiteral("text_resource_runs_after_coalescing"),
        result.renderer_totals.text_resource_runs_after_coalescing);
    object.insert(
        QStringLiteral("simple_content_cells_considered"),
        result.renderer_totals.simple_content_cells_considered);
    object.insert(
        QStringLiteral("simple_content_eligible_cells"),
        result.renderer_totals.simple_content_eligible_cells);
    object.insert(
        QStringLiteral("simple_content_eligible_after_all_gates_cells"),
        result.renderer_totals.simple_content_eligible_after_all_gates_cells);
    object.insert(
        QStringLiteral("simple_content_rows_with_eligible_cells"),
        result.renderer_totals.simple_content_rows_with_eligible_cells);
    object.insert(
        QStringLiteral("simple_content_styles_with_eligible_cells"),
        result.renderer_totals.simple_content_styles_with_eligible_cells);
    object.insert(
        QStringLiteral("simple_content_dirty_eligible_cells"),
        result.renderer_totals.simple_content_dirty_eligible_cells);
    object.insert(
        QStringLiteral("simple_content_clean_eligible_cells"),
        result.renderer_totals.simple_content_clean_eligible_cells);
    object.insert(
        QStringLiteral("simple_content_text_category_empty_cells"),
        result.renderer_totals.simple_content_text_category_empty_cells);
    object.insert(
        QStringLiteral("simple_content_text_category_printable_ascii_cells"),
        result.renderer_totals.simple_content_text_category_printable_ascii_cells);
    object.insert(
        QStringLiteral("simple_content_text_category_other_ascii_cells"),
        result.renderer_totals.simple_content_text_category_other_ascii_cells);
    object.insert(
        QStringLiteral("simple_content_text_category_non_ascii_cells"),
        result.renderer_totals.simple_content_text_category_non_ascii_cells);
    object.insert(
        QStringLiteral("simple_content_route_none_cells"),
        result.renderer_totals.simple_content_route_none_cells);
    object.insert(
        QStringLiteral("simple_content_route_fast_text_cells"),
        result.renderer_totals.simple_content_route_fast_text_cells);
    object.insert(
        QStringLiteral("simple_content_route_qt_text_layout_cells"),
        result.renderer_totals.simple_content_route_qt_text_layout_cells);
    object.insert(
        QStringLiteral("simple_content_route_graphic_geometry_cells"),
        result.renderer_totals.simple_content_route_graphic_geometry_cells);
    object.insert(
        QStringLiteral("simple_content_route_fallback_cells"),
        result.renderer_totals.simple_content_route_fallback_cells);
    object.insert(
        QStringLiteral("simple_content_rejection_none_cells"),
        result.renderer_totals.simple_content_rejection_none_cells);
    object.insert(
        QStringLiteral("simple_content_rejection_empty_text_cells"),
        result.renderer_totals.simple_content_rejection_empty_text_cells);
    object.insert(
        QStringLiteral("simple_content_rejection_invalid_grid_cells"),
        result.renderer_totals.simple_content_rejection_invalid_grid_cells);
    object.insert(
        QStringLiteral("simple_content_rejection_invalid_position_cells"),
        result.renderer_totals.simple_content_rejection_invalid_position_cells);
    object.insert(
        QStringLiteral("simple_content_rejection_invalid_style_id_cells"),
        result.renderer_totals.simple_content_rejection_invalid_style_id_cells);
    object.insert(
        QStringLiteral("simple_content_rejection_wide_continuation_cells"),
        result.renderer_totals.simple_content_rejection_wide_continuation_cells);
    object.insert(
        QStringLiteral("simple_content_rejection_invalid_display_width_cells"),
        result.renderer_totals.simple_content_rejection_invalid_display_width_cells);
    object.insert(
        QStringLiteral("simple_content_rejection_invalid_text_encoding_cells"),
        result.renderer_totals.simple_content_rejection_invalid_text_encoding_cells);
    object.insert(
        QStringLiteral("simple_content_rejection_invalid_text_width_cells"),
        result.renderer_totals.simple_content_rejection_invalid_text_width_cells);
    object.insert(
        QStringLiteral("simple_content_rejection_multi_cell_text_cells"),
        result.renderer_totals.simple_content_rejection_multi_cell_text_cells);
    object.insert(
        QStringLiteral("simple_content_rejection_non_printable_ascii_cells"),
        result.renderer_totals.simple_content_rejection_non_printable_ascii_cells);
    object.insert(
        QStringLiteral("simple_content_rejection_non_ascii_text_cells"),
        result.renderer_totals.simple_content_rejection_non_ascii_text_cells);
    object.insert(
        QStringLiteral("simple_content_rejection_decoration_cells"),
        result.renderer_totals.simple_content_rejection_decoration_cells);
    object.insert(
        QStringLiteral("simple_content_rejection_hyperlink_cells"),
        result.renderer_totals.simple_content_rejection_hyperlink_cells);
    object.insert(
        QStringLiteral("simple_content_rejection_terminal_graphic_cells"),
        result.renderer_totals.simple_content_rejection_terminal_graphic_cells);
    object.insert(
        QStringLiteral("route_fast_text_cells"),
        result.renderer_totals.route_fast_text_cells);
    object.insert(
        QStringLiteral("route_qt_text_layout_runs"),
        result.renderer_totals.route_qt_text_layout_runs);
    object.insert(
        QStringLiteral("route_graphic_geometry_cells"),
        result.renderer_totals.route_graphic_geometry_cells);
    object.insert(
        QStringLiteral("route_fallback_cells"),
        result.renderer_totals.route_fallback_cells);
    object.insert(
        QStringLiteral("qt_text_layout_calls"),
        result.renderer_totals.qt_text_layout_calls);
    object.insert(
        QStringLiteral("qsg_nodes_created"),
        result.renderer_totals.qsg_nodes_created);
    object.insert(
        QStringLiteral("qsg_nodes_replaced"),
        result.renderer_totals.qsg_nodes_replaced);
    object.insert(
        QStringLiteral("qsg_nodes_destroyed"),
        result.renderer_totals.qsg_nodes_destroyed);
    object.insert(
        QStringLiteral("background_qsg_nodes_created"),
        result.renderer_totals.background_qsg_nodes_created);
    object.insert(
        QStringLiteral("background_qsg_nodes_replaced"),
        result.renderer_totals.background_qsg_nodes_replaced);
    object.insert(
        QStringLiteral("background_qsg_nodes_destroyed"),
        result.renderer_totals.background_qsg_nodes_destroyed);
    object.insert(
        QStringLiteral("text_key_builds"),
        result.renderer_totals.text_key_builds);
    object.insert(
        QStringLiteral("text_key_bytes"),
        static_cast<qint64>(result.renderer_totals.text_key_bytes));
    object.insert(
        QStringLiteral("rect_key_builds"),
        result.renderer_totals.rect_key_builds);
    object.insert(
        QStringLiteral("rect_key_bytes"),
        static_cast<qint64>(result.renderer_totals.rect_key_bytes));
    object.insert(
        QStringLiteral("cache_key_builds"),
        result.renderer_totals.cache_key_builds);
    object.insert(
        QStringLiteral("cache_key_bytes"),
        static_cast<qint64>(result.renderer_totals.cache_key_bytes));
    object.insert(
        QStringLiteral("text_cache_entries_created"),
        result.renderer_totals.text_cache_entries_created);
    object.insert(
        QStringLiteral("text_cache_entries_replaced"),
        result.renderer_totals.text_cache_entries_replaced);
    object.insert(
        QStringLiteral("text_cache_entries_removed"),
        result.renderer_totals.text_cache_entries_removed);
    object.insert(
        QStringLiteral("frame_background_rects"),
        result.renderer_totals.frame_background_rects);
    object.insert(
        QStringLiteral("frame_selection_rects"),
        result.renderer_totals.frame_selection_rects);
    object.insert(
        QStringLiteral("frame_graphic_rects"),
        result.renderer_totals.frame_graphic_rects);
    object.insert(
        QStringLiteral("frame_graphic_arcs"),
        result.renderer_totals.frame_graphic_arcs);
    object.insert(
        QStringLiteral("frame_text_runs"),
        result.renderer_totals.frame_text_runs);
    object.insert(
        QStringLiteral("frame_cursor_text_runs"),
        result.renderer_totals.frame_cursor_text_runs);
    object.insert(
        QStringLiteral("frame_decorations"),
        result.renderer_totals.frame_decorations);
    object.insert(
        QStringLiteral("frame_cursors"),
        result.renderer_totals.frame_cursors);
    object.insert(
        QStringLiteral("frame_cursor_graphic_rects"),
        result.renderer_totals.frame_cursor_graphic_rects);
    object.insert(
        QStringLiteral("frame_cursor_graphic_arcs"),
        result.renderer_totals.frame_cursor_graphic_arcs);
    object.insert(
        QStringLiteral("frame_overlay_rects"),
        result.renderer_totals.frame_overlay_rects);
    object.insert(
        QStringLiteral("frame_dirty_row_ranges"),
        result.renderer_totals.frame_dirty_row_ranges);
    object.insert(
        QStringLiteral("frame_packed_rows"),
        result.renderer_totals.frame_packed_rows);
    object.insert(
        QStringLiteral("frame_packed_text_spans"),
        result.renderer_totals.frame_packed_text_spans);
    object.insert(
        QStringLiteral("frame_packed_text_cells"),
        result.renderer_totals.frame_packed_text_cells);
    object.insert(
        QStringLiteral("frame_packed_graphic_spans"),
        result.renderer_totals.frame_packed_graphic_spans);
    object.insert(
        QStringLiteral("frame_packed_graphic_cells"),
        result.renderer_totals.frame_packed_graphic_cells);
    object.insert(
        QStringLiteral("frame_packed_payload_bytes"),
        static_cast<qint64>(result.renderer_totals.frame_packed_payload_bytes));
    object.insert(
        QStringLiteral("row_cache_hits"),
        result.renderer_totals.row_cache_hits);
    object.insert(
        QStringLiteral("row_cache_clean_skips"),
        result.renderer_totals.row_cache_clean_skips);
    object.insert(
        QStringLiteral("background_row_rects_before_coalescing"),
        result.renderer_totals.background_row_rects_before_coalescing);
    object.insert(
        QStringLiteral("background_row_rects_after_coalescing"),
        result.renderer_totals.background_row_rects_after_coalescing);
    object.insert(
        QStringLiteral("background_batched_rects"),
        result.renderer_totals.background_batched_rects);
    object.insert(
        QStringLiteral("background_batched_vertices"),
        result.renderer_totals.background_batched_vertices);
    object.insert(
        QStringLiteral("selection_batched_rects"),
        result.renderer_totals.selection_batched_rects);
    object.insert(
        QStringLiteral("selection_batched_vertices"),
        result.renderer_totals.selection_batched_vertices);
    object.insert(
        QStringLiteral("graphic_batched_rects"),
        result.renderer_totals.graphic_batched_rects);
    object.insert(
        QStringLiteral("graphic_batched_vertices"),
        result.renderer_totals.graphic_batched_vertices);
    object.insert(
        QStringLiteral("decoration_batched_rects"),
        result.renderer_totals.decoration_batched_rects);
    object.insert(
        QStringLiteral("decoration_batched_vertices"),
        result.renderer_totals.decoration_batched_vertices);
    object.insert(
        QStringLiteral("background_rows_rebuilt"),
        result.renderer_totals.background_rows_rebuilt);
    object.insert(
        QStringLiteral("background_rows_reused"),
        result.renderer_totals.background_rows_reused);
    object.insert(
        QStringLiteral("background_row_clean_reuse_skips"),
        result.renderer_totals.background_row_clean_reuse_skips);
    object.insert(
        QStringLiteral("background_rows_removed"),
        result.renderer_totals.background_rows_removed);
    object.insert(
        QStringLiteral("background_row_cache_fallbacks"),
        result.renderer_totals.background_row_cache_fallbacks);
    object.insert(
        QStringLiteral("selection_rows_rebuilt"),
        result.renderer_totals.selection_rows_rebuilt);
    object.insert(
        QStringLiteral("selection_rows_reused"),
        result.renderer_totals.selection_rows_reused);
    object.insert(
        QStringLiteral("selection_row_clean_reuse_skips"),
        result.renderer_totals.selection_row_clean_reuse_skips);
    object.insert(
        QStringLiteral("selection_rows_removed"),
        result.renderer_totals.selection_rows_removed);
    object.insert(
        QStringLiteral("selection_row_cache_fallbacks"),
        result.renderer_totals.selection_row_cache_fallbacks);
    object.insert(
        QStringLiteral("decoration_rows_rebuilt"),
        result.renderer_totals.decoration_rows_rebuilt);
    object.insert(
        QStringLiteral("decoration_rows_reused"),
        result.renderer_totals.decoration_rows_reused);
    object.insert(
        QStringLiteral("decoration_row_clean_reuse_skips"),
        result.renderer_totals.decoration_row_clean_reuse_skips);
    object.insert(
        QStringLiteral("decoration_rows_removed"),
        result.renderer_totals.decoration_rows_removed);
    object.insert(
        QStringLiteral("decoration_row_cache_fallbacks"),
        result.renderer_totals.decoration_row_cache_fallbacks);
    object.insert(
        QStringLiteral("graphic_rect_rows_rebuilt"),
        result.renderer_totals.graphic_rect_rows_rebuilt);
    object.insert(
        QStringLiteral("graphic_rect_rows_reused"),
        result.renderer_totals.graphic_rect_rows_reused);
    object.insert(
        QStringLiteral("graphic_rect_row_clean_reuse_skips"),
        result.renderer_totals.graphic_rect_row_clean_reuse_skips);
    object.insert(
        QStringLiteral("graphic_rect_rows_removed"),
        result.renderer_totals.graphic_rect_rows_removed);
    object.insert(
        QStringLiteral("graphic_rect_row_cache_fallbacks"),
        result.renderer_totals.graphic_rect_row_cache_fallbacks);
    object.insert(
        QStringLiteral("graphic_arc_rows_rebuilt"),
        result.renderer_totals.graphic_arc_rows_rebuilt);
    object.insert(
        QStringLiteral("graphic_arc_rows_reused"),
        result.renderer_totals.graphic_arc_rows_reused);
    object.insert(
        QStringLiteral("graphic_arc_row_clean_reuse_skips"),
        result.renderer_totals.graphic_arc_row_clean_reuse_skips);
    object.insert(
        QStringLiteral("graphic_arc_rows_removed"),
        result.renderer_totals.graphic_arc_rows_removed);
    object.insert(
        QStringLiteral("graphic_arc_row_cache_fallbacks"),
        result.renderer_totals.graphic_arc_row_cache_fallbacks);
    object.insert(
        QStringLiteral("bridge_update_requests_delta"),
        static_cast<qint64>(result.bridge_delta.update_requests));
    object.insert(
        QStringLiteral("bridge_scheduled_updates_delta"),
        static_cast<qint64>(result.bridge_delta.scheduled_updates));
    object.insert(
        QStringLiteral("bridge_coalesced_requests_delta"),
        static_cast<qint64>(result.bridge_delta.coalesced_requests));
    object.insert(
        QStringLiteral("bridge_consumed_updates_delta"),
        static_cast<qint64>(result.bridge_delta.consumed_updates));
    object.insert(
        QStringLiteral("per_consumed_update"),
        normalized_per_consumed_update_json(result));
    object.insert(
        QStringLiteral("lifecycle_delta"),
        lifecycle_delta_json(result.lifecycle_delta));
    object.insert(QStringLiteral("process_memory"), process_memory_json(result.process_memory));
    object.insert(QStringLiteral("rows"),           result.rows);
    object.insert(QStringLiteral("columns"),        result.columns);
    object.insert(QStringLiteral("window_size"),    window_size);
    object.insert(
        QStringLiteral("viewport_metrics_applicable"),
        result.viewport_metrics_applicable);
    object.insert(
        QStringLiteral("viewport_scrollback_rows"),
        result.viewport_scrollback_rows);
    object.insert(
        QStringLiteral("viewport_initial_offset_from_tail"),
        result.viewport_initial_offset_from_tail);
    object.insert(
        QStringLiteral("viewport_final_offset_from_tail"),
        result.viewport_final_offset_from_tail);
    object.insert(
        QStringLiteral("viewport_expected_offset_from_tail"),
        result.viewport_expected_offset_from_tail);
    object.insert(QStringLiteral("wheel_burst_size"),      result.wheel_burst_size);
    object.insert(QStringLiteral("wheel_steps_per_event"), result.wheel_steps_per_event);
    object.insert(
        QStringLiteral("viewport_expected_burst_delta"),
        result.viewport_expected_burst_delta);
    object.insert(
        QStringLiteral("viewport_final_burst_delta"),
        result.viewport_final_burst_delta);
    object.insert(
        QStringLiteral("viewport_expected_top_line"),
        result.viewport_expected_top_line);
    object.insert(
        QStringLiteral("viewport_final_top_line"),
        result.viewport_final_top_line);
    object.insert(
        QStringLiteral("wheel_events_accepted_count"),
        result.wheel_events_accepted_count);
    object.insert(QStringLiteral("backend_writes_total"), result.backend_writes_total);
    object.insert(
        QStringLiteral("backend_write_bytes_total"),
        result.backend_write_bytes_total);
    object.insert(QStringLiteral("backend_errors_total"), result.backend_errors_total);
    object.insert(
        QStringLiteral("session_snapshots_observed"),
        result.session_snapshots_observed);
    object.insert(
        QStringLiteral("scrollback_limit_configured"),
        result.scrollback_limit_configured);
    object.insert(
        QStringLiteral("scrollback_limit_observed"),
        result.scrollback_limit_observed);
    object.insert(
        QStringLiteral("output_pause_requests_total"),
        result.output_pause_requests_total);
    object.insert(
        QStringLiteral("output_pause_enabled_count"),
        result.output_pause_enabled_count);
    object.insert(
        QStringLiteral("output_pause_disabled_count"),
        result.output_pause_disabled_count);
    object.insert(QStringLiteral("render_expected"), result.render_expected);
    object.insert(
        QStringLiteral("output_high_water_observed"),
        result.output_high_water_observed);
    object.insert(
        QStringLiteral("write_high_water_observed"),
        result.write_high_water_observed);
    object.insert(
        QStringLiteral("render_measurement_semantics"),
        result.render_measurement_semantics);
    object.insert(
        QStringLiteral("queue_pressure_semantics"),
        result.queue_pressure_semantics);
    object.insert(
        QStringLiteral("workload_actions_expected_count"),
        result.workload_actions_expected_count);
    object.insert(
        QStringLiteral("workload_actions_accepted_count"),
        result.workload_actions_accepted_count);
    object.insert(
        QStringLiteral("dominant_latency_component"),
        result.dominant_latency_component);
    object.insert(QStringLiteral("primary_pressure"), result.primary_pressure);
    object.insert(
        QStringLiteral("structural_checks"),
        structural_checks_json(result.structural_checks));
    object.insert(QStringLiteral("profile"), scenario_profile_value(result));
    return object;
}

bool scenario_status_ok(const QJsonObject& object)
{
    return object.value(QStringLiteral("status")).toString() == QStringLiteral("ok");
}

bool required_structural_checks_passed(const QJsonObject& object)
{
    const QJsonObject checks =
        object.value(QStringLiteral("structural_checks")).toObject();
    const bool render_expected =
        object.value(QStringLiteral("render_expected")).toBool(true);
    const bool common_checks_passed =
        checks.value(QStringLiteral("no_child_qquick_items")).toBool()      &&
        checks.value(QStringLiteral("scrollback_limit_respected")).toBool() &&
        checks.value(QStringLiteral("workload_actions_accepted")).toBool()  &&
        checks.value(QStringLiteral("queue_pressure_observed")).toBool()    &&
        checks.value(QStringLiteral("backend_errors_zero")).toBool();
    if (!common_checks_passed) {
        return false;
    }

    if (render_expected &&
        (!checks.value(QStringLiteral("snapshot_valid")).toBool() ||
            !checks.value(QStringLiteral("last_rendered_snapshot_sequence")).toBool() ||
            !checks.value(QStringLiteral("pending_update_clear")).toBool()            ||
            !checks.value(QStringLiteral("paint_completed")).toBool()                 ||
            !checks.value(QStringLiteral("text_content_failures_zero")).toBool()      ||
            !checks.value(QStringLiteral("text_work_observed")).toBool()              ||
            !checks.value(QStringLiteral("visible_pixels_observed")).toBool()))
    {
        return false;
    }

    if (!object.value(QStringLiteral("viewport_metrics_applicable")).toBool()) {
        return true;
    }

    return
        checks.value(QStringLiteral("rendered_pixels_changed")).toBool()   &&
        checks.value(QStringLiteral("scrollback_rows_available")).toBool() &&
        checks.value(QStringLiteral("viewport_offset_changed")).toBool()   &&
        checks.value(QStringLiteral("viewport_offset_expected")).toBool()  &&
        checks.value(QStringLiteral("viewport_content_expected")).toBool() &&
        checks.value(QStringLiteral("wheel_events_accepted")).toBool()     &&
        checks.value(QStringLiteral("backend_errors_zero")).toBool();
}

bool validate_summary_json(
    const QJsonObject& object,
    const QString&     key,
    QString*           out_error)
{
    const QJsonValue value = object.value(key);
    if (!value.isObject()) {
        *out_error = QStringLiteral("scenario JSON key is not an object: %1").arg(key);
        return false;
    }

    const QJsonObject summary = value.toObject();
    const QStringList summary_keys = {
        QStringLiteral("sample_count"),
        QStringLiteral("total"),
        QStringLiteral("min"),
        QStringLiteral("median"),
        QStringLiteral("p95"),
        QStringLiteral("max"),
    };
    for (const QString& summary_key : summary_keys) {
        if (!summary.value(summary_key).isDouble()) {
            *out_error = QStringLiteral("scenario summary key is not numeric: %1.%2")
                .arg(key)
                .arg(summary_key);
            return false;
        }
    }

    const double total        = summary.value(QStringLiteral("total")).toDouble();
    const double sample_count = summary.value(QStringLiteral("sample_count")).toDouble();
    const double min          = summary.value(QStringLiteral("min")).toDouble();
    const double median       = summary.value(QStringLiteral("median")).toDouble();
    const double p95          = summary.value(QStringLiteral("p95")).toDouble();
    const double max          = summary.value(QStringLiteral("max")).toDouble();
    if (!std::isfinite(sample_count) ||
        !std::isfinite(total)        ||
        !std::isfinite(min)          ||
        !std::isfinite(median)       ||
        !std::isfinite(p95)          ||
        !std::isfinite(max)          ||
        sample_count < 0.0           ||
        total        < 0.0           ||
        min          < 0.0           ||
        median       < 0.0           ||
        p95          < 0.0           ||
        max          < 0.0)
    {
        *out_error = QStringLiteral("scenario summary contains invalid values: %1")
            .arg(key);
        return false;
    }

    if (min > median || median > p95 || p95 > max || max > total) {
        *out_error = QStringLiteral("scenario summary values are inconsistent: %1")
            .arg(key);
        return false;
    }

    return true;
}

bool validate_process_memory_json(const QJsonObject& object, QString* out_error)
{
    const QJsonValue value = object.value(QStringLiteral("process_memory"));
    if (!value.isObject()) {
        *out_error = QStringLiteral("scenario process_memory is not an object");
        return false;
    }

    const QJsonObject memory = value.toObject();
    const QStringList string_keys = {
        QStringLiteral("status"),
        QStringLiteral("platform"),
        QStringLiteral("metric"),
        QStringLiteral("semantics"),
    };
    for (const QString& key : string_keys) {
        if (!memory.value(key).isString()) {
            *out_error = QStringLiteral("scenario process_memory key is not a string: %1")
                .arg(key);
            return false;
        }
    }

    if (memory.value(QStringLiteral("metric")).toString() !=
        QStringLiteral("resident_set_bytes") ||
        memory.value(QStringLiteral("semantics")).toString() != k_memory_sample_semantics)
    {
        *out_error = QStringLiteral("scenario process_memory metadata is inconsistent");
        return false;
    }

    if (!validate_summary_json(memory, QStringLiteral("resident_bytes"), out_error)) {
        return false;
    }

    const QString status = memory.value(QStringLiteral("status")).toString();
    const int sample_count = memory.value(QStringLiteral("resident_bytes"))
        .toObject()
        .value(QStringLiteral("sample_count"))
        .toInt();
    if (status == QStringLiteral("sampled")) {
        if (sample_count <= 0) {
            *out_error = QStringLiteral("scenario process_memory sampled without samples");
            return false;
        }
        return true;
    }

    if ((status == QStringLiteral("unsupported") || status == QStringLiteral("unavailable")) &&
        sample_count == 0)
    {
        return true;
    }

    *out_error = QStringLiteral("scenario process_memory status/sample_count mismatch");
    return false;
}

bool validate_window_size_json(const QJsonObject& object, QString* out_error)
{
    const QJsonValue value = object.value(QStringLiteral("window_size"));
    if (!value.isObject()) {
        *out_error = QStringLiteral("scenario window_size is not an object");
        return false;
    }

    const QJsonObject window_size = value.toObject();
    if (!window_size.value(QStringLiteral("width")).isDouble() ||
        !window_size.value(QStringLiteral("height")).isDouble())
    {
        *out_error = QStringLiteral("scenario window_size dimensions are not numeric");
        return false;
    }

    return true;
}

bool validate_nonnegative_integer_json_field(
    const QJsonObject& object,
    const QString&     key,
    const QString&     label,
    QString*           out_error)
{
    const QJsonValue value         = object.value(key);
    const double     numeric_value = value.toDouble(-1.0);
    if (!value.isDouble()                          ||
        !std::isfinite(numeric_value)              ||
        numeric_value             <  0.0           ||
        std::floor(numeric_value) != numeric_value ||
        numeric_value             >  static_cast<double>(std::numeric_limits<qint64>::max()))
    {
        *out_error = QStringLiteral("scenario numeric key is invalid: %1.%2")
            .arg(label)
            .arg(key);
        return false;
    }

    return true;
}

bool validate_lifecycle_json(const QJsonObject& object, QString* out_error)
{
    const QJsonValue value = object.value(QStringLiteral("lifecycle_delta"));
    if (!value.isObject()) {
        *out_error = QStringLiteral("scenario lifecycle_delta is not an object");
        return false;
    }

    const QJsonObject lifecycle = value.toObject();
    const QStringList lifecycle_keys = {
        QStringLiteral("release_resources_calls_delta"),
        QStringLiteral("item_scene_changes_delta"),
        QStringLiteral("item_scene_detaches_delta"),
        QStringLiteral("item_destructions_delta"),
        QStringLiteral("scene_graph_invalidated_calls_delta"),
        QStringLiteral("render_node_deletions_in_paint_delta"),
        QStringLiteral("render_root_nodes_created_delta"),
        QStringLiteral("render_root_nodes_destroyed_delta"),
        QStringLiteral("render_text_resources_created_delta"),
        QStringLiteral("render_text_resources_destroyed_delta"),
        QStringLiteral("render_rect_resources_created_delta"),
        QStringLiteral("render_rect_resources_destroyed_delta"),
        QStringLiteral("live_root_nodes"),
        QStringLiteral("live_text_resources"),
        QStringLiteral("live_rect_resources"),
    };
    for (const QString& lifecycle_key : lifecycle_keys) {
        if (!validate_nonnegative_integer_json_field(
                lifecycle, lifecycle_key, QStringLiteral("lifecycle_delta"), out_error))
        {
            return false;
        }
    }

    return true;
}

bool validate_renderer_counter_json(const QJsonObject& object, QString* out_error)
{
    const QStringList counter_keys = {
        QStringLiteral("text_clean_reuse_skips"),
        QStringLiteral("text_resource_descriptor_reuses"),
        QStringLiteral("text_coalescing_candidate_groups"),
        QStringLiteral("text_coalescing_enabled_groups"),
        QStringLiteral("text_resource_runs_before_coalescing"),
        QStringLiteral("text_resource_runs_after_coalescing"),
        QStringLiteral("simple_content_cells_considered"),
        QStringLiteral("simple_content_eligible_cells"),
        QStringLiteral("simple_content_eligible_after_all_gates_cells"),
        QStringLiteral("simple_content_rows_with_eligible_cells"),
        QStringLiteral("simple_content_styles_with_eligible_cells"),
        QStringLiteral("simple_content_dirty_eligible_cells"),
        QStringLiteral("simple_content_clean_eligible_cells"),
        QStringLiteral("simple_content_text_category_empty_cells"),
        QStringLiteral("simple_content_text_category_printable_ascii_cells"),
        QStringLiteral("simple_content_text_category_other_ascii_cells"),
        QStringLiteral("simple_content_text_category_non_ascii_cells"),
        QStringLiteral("simple_content_route_none_cells"),
        QStringLiteral("simple_content_route_fast_text_cells"),
        QStringLiteral("simple_content_route_qt_text_layout_cells"),
        QStringLiteral("simple_content_route_graphic_geometry_cells"),
        QStringLiteral("simple_content_route_fallback_cells"),
        QStringLiteral("simple_content_rejection_none_cells"),
        QStringLiteral("simple_content_rejection_empty_text_cells"),
        QStringLiteral("simple_content_rejection_invalid_grid_cells"),
        QStringLiteral("simple_content_rejection_invalid_position_cells"),
        QStringLiteral("simple_content_rejection_invalid_style_id_cells"),
        QStringLiteral("simple_content_rejection_wide_continuation_cells"),
        QStringLiteral("simple_content_rejection_invalid_display_width_cells"),
        QStringLiteral("simple_content_rejection_invalid_text_encoding_cells"),
        QStringLiteral("simple_content_rejection_invalid_text_width_cells"),
        QStringLiteral("simple_content_rejection_multi_cell_text_cells"),
        QStringLiteral("simple_content_rejection_non_printable_ascii_cells"),
        QStringLiteral("simple_content_rejection_non_ascii_text_cells"),
        QStringLiteral("simple_content_rejection_decoration_cells"),
        QStringLiteral("simple_content_rejection_hyperlink_cells"),
        QStringLiteral("simple_content_rejection_terminal_graphic_cells"),
        QStringLiteral("route_fast_text_cells"),
        QStringLiteral("route_qt_text_layout_runs"),
        QStringLiteral("route_graphic_geometry_cells"),
        QStringLiteral("route_fallback_cells"),
        QStringLiteral("qt_text_layout_calls"),
        QStringLiteral("qsg_nodes_created"),
        QStringLiteral("qsg_nodes_replaced"),
        QStringLiteral("qsg_nodes_destroyed"),
        QStringLiteral("background_qsg_nodes_created"),
        QStringLiteral("background_qsg_nodes_replaced"),
        QStringLiteral("background_qsg_nodes_destroyed"),
        QStringLiteral("text_key_builds"),
        QStringLiteral("text_key_bytes"),
        QStringLiteral("rect_key_builds"),
        QStringLiteral("rect_key_bytes"),
        QStringLiteral("cache_key_builds"),
        QStringLiteral("cache_key_bytes"),
        QStringLiteral("text_cache_entries_created"),
        QStringLiteral("text_cache_entries_replaced"),
        QStringLiteral("text_cache_entries_removed"),
        QStringLiteral("frame_background_rects"),
        QStringLiteral("frame_selection_rects"),
        QStringLiteral("frame_graphic_rects"),
        QStringLiteral("frame_graphic_arcs"),
        QStringLiteral("frame_text_runs"),
        QStringLiteral("frame_cursor_text_runs"),
        QStringLiteral("frame_decorations"),
        QStringLiteral("frame_cursors"),
        QStringLiteral("frame_cursor_graphic_rects"),
        QStringLiteral("frame_cursor_graphic_arcs"),
        QStringLiteral("frame_overlay_rects"),
        QStringLiteral("frame_dirty_row_ranges"),
        QStringLiteral("frame_packed_rows"),
        QStringLiteral("frame_packed_text_spans"),
        QStringLiteral("frame_packed_text_cells"),
        QStringLiteral("frame_packed_graphic_spans"),
        QStringLiteral("frame_packed_graphic_cells"),
        QStringLiteral("frame_packed_payload_bytes"),
        QStringLiteral("row_cache_hits"),
        QStringLiteral("row_cache_clean_skips"),
        QStringLiteral("background_row_rects_before_coalescing"),
        QStringLiteral("background_row_rects_after_coalescing"),
        QStringLiteral("background_batched_rects"),
        QStringLiteral("background_batched_vertices"),
        QStringLiteral("selection_batched_rects"),
        QStringLiteral("selection_batched_vertices"),
        QStringLiteral("graphic_batched_rects"),
        QStringLiteral("graphic_batched_vertices"),
        QStringLiteral("decoration_batched_rects"),
        QStringLiteral("decoration_batched_vertices"),
        QStringLiteral("background_rows_rebuilt"),
        QStringLiteral("background_rows_reused"),
        QStringLiteral("background_row_clean_reuse_skips"),
        QStringLiteral("background_rows_removed"),
        QStringLiteral("background_row_cache_fallbacks"),
        QStringLiteral("selection_rows_rebuilt"),
        QStringLiteral("selection_rows_reused"),
        QStringLiteral("selection_row_clean_reuse_skips"),
        QStringLiteral("selection_rows_removed"),
        QStringLiteral("selection_row_cache_fallbacks"),
        QStringLiteral("decoration_rows_rebuilt"),
        QStringLiteral("decoration_rows_reused"),
        QStringLiteral("decoration_row_clean_reuse_skips"),
        QStringLiteral("decoration_rows_removed"),
        QStringLiteral("decoration_row_cache_fallbacks"),
        QStringLiteral("graphic_rect_rows_rebuilt"),
        QStringLiteral("graphic_rect_rows_reused"),
        QStringLiteral("graphic_rect_row_clean_reuse_skips"),
        QStringLiteral("graphic_rect_rows_removed"),
        QStringLiteral("graphic_rect_row_cache_fallbacks"),
        QStringLiteral("graphic_arc_rows_rebuilt"),
        QStringLiteral("graphic_arc_rows_reused"),
        QStringLiteral("graphic_arc_row_clean_reuse_skips"),
        QStringLiteral("graphic_arc_rows_removed"),
        QStringLiteral("graphic_arc_row_cache_fallbacks"),
    };
    for (const QString& counter_key : counter_keys) {
        if (!validate_nonnegative_integer_json_field(
                object, counter_key, QStringLiteral("scenario"), out_error))
        {
            return false;
        }
    }

    return true;
}

qint64 json_counter(const QJsonObject& object, const QString& key)
{
    return object.value(key).toInteger();
}

bool validate_renderer_counter_invariants(
    const QJsonObject& object,
    QString*           out_error)
{
    const qint64 simple_cells =
        json_counter(object, QStringLiteral("simple_content_cells_considered"));
    const qint64 text_category_cells =
        json_counter(object, QStringLiteral("simple_content_text_category_empty_cells")) +
        json_counter(
            object,
            QStringLiteral("simple_content_text_category_printable_ascii_cells")) +
        json_counter(object, QStringLiteral("simple_content_text_category_other_ascii_cells")) +
        json_counter(object, QStringLiteral("simple_content_text_category_non_ascii_cells"));
    const qint64 route_cells =
        json_counter(object, QStringLiteral("simple_content_route_none_cells")) +
        json_counter(object, QStringLiteral("simple_content_route_fast_text_cells")) +
        json_counter(object, QStringLiteral("simple_content_route_qt_text_layout_cells")) +
        json_counter(object, QStringLiteral("simple_content_route_graphic_geometry_cells")) +
        json_counter(object, QStringLiteral("simple_content_route_fallback_cells"));
    const qint64 rejection_cells =
        json_counter(object, QStringLiteral("simple_content_rejection_none_cells")) +
        json_counter(object, QStringLiteral("simple_content_rejection_empty_text_cells")) +
        json_counter(object, QStringLiteral("simple_content_rejection_invalid_grid_cells")) +
        json_counter(object, QStringLiteral("simple_content_rejection_invalid_position_cells")) +
        json_counter(object, QStringLiteral("simple_content_rejection_invalid_style_id_cells")) +
        json_counter(object, QStringLiteral("simple_content_rejection_wide_continuation_cells")) +
        json_counter(
            object,
            QStringLiteral("simple_content_rejection_invalid_display_width_cells")) +
        json_counter(
            object,
            QStringLiteral("simple_content_rejection_invalid_text_encoding_cells")) +
        json_counter(object, QStringLiteral("simple_content_rejection_invalid_text_width_cells")) +
        json_counter(object, QStringLiteral("simple_content_rejection_multi_cell_text_cells")) +
        json_counter(
            object,
            QStringLiteral("simple_content_rejection_non_printable_ascii_cells")) +
        json_counter(object, QStringLiteral("simple_content_rejection_non_ascii_text_cells")) +
        json_counter(object, QStringLiteral("simple_content_rejection_decoration_cells")) +
        json_counter(object, QStringLiteral("simple_content_rejection_hyperlink_cells")) +
        json_counter(object, QStringLiteral("simple_content_rejection_terminal_graphic_cells"));

    if (text_category_cells != simple_cells ||
        route_cells         != simple_cells ||
        rejection_cells     != simple_cells)
    {
        *out_error = QStringLiteral("simple-content classifier totals are inconsistent");
        return false;
    }

    const qint64 simple_content_graphic_route_cells = json_counter(
        object,
        QStringLiteral("simple_content_route_graphic_geometry_cells"));
    const qint64 simple_content_graphic_rejection_cells = json_counter(
        object,
        QStringLiteral("simple_content_rejection_terminal_graphic_cells"));
    if (simple_content_graphic_route_cells != simple_content_graphic_rejection_cells) {
        *out_error = QStringLiteral("simple-content graphic-route counters are inconsistent");
        return false;
    }

    const qint64 eligible_cells =
        json_counter(object, QStringLiteral("simple_content_eligible_cells"));
    const qint64 candidate_fast_text_cells = json_counter(
        object,
        QStringLiteral("simple_content_route_fast_text_cells"));
    const qint64 candidate_rejection_none_cells = json_counter(
        object,
        QStringLiteral("simple_content_rejection_none_cells"));
    if (json_counter(
            object,
            QStringLiteral("simple_content_eligible_after_all_gates_cells")) >
            eligible_cells                               ||
        candidate_fast_text_cells      != eligible_cells ||
        candidate_rejection_none_cells != eligible_cells ||
        json_counter(object, QStringLiteral("simple_content_dirty_eligible_cells")) +
            json_counter(object, QStringLiteral("simple_content_clean_eligible_cells")) !=
            eligible_cells)
    {
        *out_error = QStringLiteral("simple-content eligibility counters are inconsistent");
        return false;
    }

    if (json_counter(object, QStringLiteral("route_fast_text_cells")) != 0 ||
        json_counter(object, QStringLiteral("route_graphic_geometry_cells")) <
            simple_content_graphic_route_cells                             ||
        json_counter(object, QStringLiteral("route_fallback_cells"))  != 0 ||
        json_counter(object, QStringLiteral("qt_text_layout_calls")) !=
            json_counter(object, QStringLiteral("route_qt_text_layout_runs")))
    {
        *out_error = QStringLiteral("renderer route counters are inconsistent");
        return false;
    }

    if (json_counter(object, QStringLiteral("background_qsg_nodes_created")) >
        json_counter(object, QStringLiteral("qsg_nodes_created")) ||
        json_counter(object, QStringLiteral("background_qsg_nodes_replaced")) >
        json_counter(object, QStringLiteral("qsg_nodes_replaced")) ||
        json_counter(object, QStringLiteral("background_qsg_nodes_destroyed")) >
        json_counter(object, QStringLiteral("qsg_nodes_destroyed")))
    {
        *out_error = QStringLiteral("background QSG node counters exceed global counters");
        return false;
    }

    if (json_counter(object, QStringLiteral("background_batched_vertices")) !=
        json_counter(object, QStringLiteral("background_batched_rects")) *
            k_flat_rect_vertices_per_rect)
    {
        *out_error = QStringLiteral("background batched-geometry counters are inconsistent");
        return false;
    }
    if (json_counter(object, QStringLiteral("selection_batched_vertices")) !=
        json_counter(object, QStringLiteral("selection_batched_rects")) *
            k_flat_rect_vertices_per_rect)
    {
        *out_error = QStringLiteral("selection batched-geometry counters are inconsistent");
        return false;
    }
    if (json_counter(object, QStringLiteral("graphic_batched_vertices")) !=
        json_counter(object, QStringLiteral("graphic_batched_rects")) *
            k_flat_rect_vertices_per_rect)
    {
        *out_error = QStringLiteral("graphic batched-geometry counters are inconsistent");
        return false;
    }
    if (json_counter(object, QStringLiteral("decoration_batched_vertices")) !=
        json_counter(object, QStringLiteral("decoration_batched_rects")) *
            k_flat_rect_vertices_per_rect)
    {
        *out_error = QStringLiteral("decoration batched-geometry counters are inconsistent");
        return false;
    }

    const qint64 frame_packed_rows =
        json_counter(object, QStringLiteral("frame_packed_rows"));
    if (object.value(QStringLiteral("render_expected")).toBool()) {
        const qint64 expected_packed_rows =
            json_counter(object, QStringLiteral("rows")) *
            json_counter(object, QStringLiteral("bridge_consumed_updates_delta"));
        if (frame_packed_rows != expected_packed_rows) {
            *out_error = QStringLiteral("packed row counters are inconsistent");
            return false;
        }
    }

    const qint64 frame_packed_text_cells =
        json_counter(object, QStringLiteral("frame_packed_text_cells"));
    const qint64 frame_packed_graphic_cells =
        json_counter(object, QStringLiteral("frame_packed_graphic_cells"));
    const qint64 frame_packed_text_spans =
        json_counter(object, QStringLiteral("frame_packed_text_spans"));
    const qint64 frame_packed_graphic_spans =
        json_counter(object, QStringLiteral("frame_packed_graphic_spans"));
    if (frame_packed_text_cells !=
        json_counter(
            object,
            QStringLiteral("simple_content_eligible_after_all_gates_cells")) ||
        frame_packed_graphic_cells > simple_content_graphic_route_cells      ||
        frame_packed_text_spans    > frame_packed_text_cells                 ||
        frame_packed_graphic_spans > frame_packed_graphic_cells)
    {
        *out_error = QStringLiteral("packed span counters are inconsistent");
        return false;
    }

    if ((frame_packed_text_cells   > 0 ||
        frame_packed_graphic_cells > 0 ||
        frame_packed_text_spans    > 0 ||
        frame_packed_graphic_spans > 0)
        &&
        json_counter(object, QStringLiteral("frame_packed_payload_bytes")) <= 0)
    {
        *out_error = QStringLiteral("packed payload byte counter is inconsistent");
        return false;
    }

    if (json_counter(object, QStringLiteral("cache_key_builds")) !=
        json_counter(object, QStringLiteral("text_key_builds")) +
            json_counter(object, QStringLiteral("rect_key_builds")) ||
        json_counter(object, QStringLiteral("cache_key_bytes")) !=
        json_counter(object, QStringLiteral("text_key_bytes")) +
            json_counter(object, QStringLiteral("rect_key_bytes")))
    {
        *out_error = QStringLiteral("renderer cache-key counters are inconsistent");
        return false;
    }

    const qint64 text_row_cache_hits = json_counter(
        object,
        QStringLiteral("renderer_text_reused_total")) - json_counter(object, QStringLiteral("text_clean_reuse_skips"));
    if (json_counter(object, QStringLiteral("text_resource_descriptor_reuses")) >
        text_row_cache_hits)
    {
        *out_error = QStringLiteral(
            "text resource descriptor reuses exceed text row-cache hits");
        return false;
    }

    if (json_counter(object, QStringLiteral("text_coalescing_enabled_groups")) >
        json_counter(object, QStringLiteral("text_coalescing_candidate_groups")) ||
        json_counter(object, QStringLiteral("text_resource_runs_after_coalescing")) >
        json_counter(object, QStringLiteral("text_resource_runs_before_coalescing")))
    {
        *out_error = QStringLiteral("text coalescing counters are inconsistent");
        return false;
    }

    const qint64 row_cache_hits =
        text_row_cache_hits +
        json_counter(object, QStringLiteral("background_rows_reused")) +
        json_counter(object, QStringLiteral("selection_rows_reused")) +
        json_counter(object, QStringLiteral("decoration_rows_reused")) +
        json_counter(object, QStringLiteral("graphic_rect_rows_reused")) +
        json_counter(object, QStringLiteral("graphic_arc_rows_reused"));
    const qint64 row_cache_clean_skips =
        json_counter(object, QStringLiteral("text_clean_reuse_skips")) +
        json_counter(object, QStringLiteral("background_row_clean_reuse_skips")) +
        json_counter(object, QStringLiteral("selection_row_clean_reuse_skips")) +
        json_counter(object, QStringLiteral("decoration_row_clean_reuse_skips")) +
        json_counter(object, QStringLiteral("graphic_rect_row_clean_reuse_skips")) +
        json_counter(object, QStringLiteral("graphic_arc_row_clean_reuse_skips"));
    if (json_counter(object, QStringLiteral("row_cache_hits")) != row_cache_hits ||
        json_counter(object, QStringLiteral("row_cache_clean_skips")) !=
            row_cache_clean_skips)
    {
        *out_error = QStringLiteral("renderer row-cache aggregate counters are inconsistent");
        return false;
    }

    return true;
}

bool json_numbers_match(double observed, double expected)
{
    const double tolerance = std::max(1.0e-9, std::abs(expected) * 1.0e-9);
    return std::abs(observed - expected) <= tolerance;
}

bool validate_normalized_json_number(
    const QJsonValue&  value,
    double             expected,
    const QString&     label,
    QString*           out_error)
{
    const double observed = value.toDouble(-1.0);
    if (!value.isDouble() ||
        !std::isfinite(observed) ||
        observed < 0.0 ||
        !json_numbers_match(observed, expected))
    {
        *out_error = QStringLiteral("scenario normalized value is invalid: %1")
            .arg(label);
        return false;
    }

    return true;
}

bool validate_normalized_json_value(
    const QJsonValue&  value,
    qint64             total,
    qint64             denominator,
    const QString&     label,
    QString*           out_error)
{
    if (denominator == 0) {
        if (!value.isNull()) {
            *out_error = QStringLiteral("scenario normalized value must be null: %1")
                .arg(label);
            return false;
        }
        return true;
    }

    return
        validate_normalized_json_number(
            value,
            static_cast<double>(total) / static_cast<double>(denominator),
            label,
            out_error);
}

QString expected_latency_normalization(
    qint64             completed_frames,
    qint64             consumed_updates)
{
    if (completed_frames <= 0 || consumed_updates == 0) {
        return k_latency_normalization_unavailable;
    }

    return consumed_updates == completed_frames
        ? k_latency_normalization_single_update
        : k_latency_normalization_multi_update_approximate;
}

bool validate_per_consumed_update_json(
    const QJsonObject& object,
    QString*           out_error)
{
    const QJsonValue per_update_value =
        object.value(QStringLiteral("per_consumed_update"));
    if (!per_update_value.isObject()) {
        *out_error = QStringLiteral("scenario per_consumed_update is not an object");
        return false;
    }

    const QJsonObject per_update = per_update_value.toObject();
    const QStringList required_keys = {
        QStringLiteral("available"),
        QStringLiteral("denominator_key"),
        QStringLiteral("denominator"),
        QStringLiteral("completed_attempts"),
        QStringLiteral("consumed_updates_per_completed_attempt"),
        QStringLiteral("counter_semantics"),
        QStringLiteral("counters"),
        QStringLiteral("timing"),
    };
    for (const QString& key : required_keys) {
        if (!per_update.contains(key)) {
            *out_error = QStringLiteral("scenario per_consumed_update missing key: %1")
                .arg(key);
            return false;
        }
    }

    const qint64 consumed_updates =
        json_counter(object, QStringLiteral("bridge_consumed_updates_delta"));
    const qint64 completed_frames =
        object.value(QStringLiteral("completed_frames")).toInteger();
    if (!per_update.value(QStringLiteral("available")).isBool()                                ||
        per_update.value(QStringLiteral("available")).toBool()      != (consumed_updates != 0) ||
        per_update.value(QStringLiteral("denominator_key")).toString() !=
            QStringLiteral("bridge_consumed_updates_delta")                                    ||
        per_update.value(QStringLiteral("denominator")).toInteger() != consumed_updates        ||
        per_update.value(QStringLiteral("completed_attempts")).toInteger() !=
            completed_frames ||
        per_update.value(QStringLiteral("counter_semantics")).toString() !=
            k_per_consumed_update_counter_semantics)
    {
        *out_error = QStringLiteral("scenario per_consumed_update metadata is inconsistent");
        return false;
    }

    const QJsonValue updates_per_attempt =
        per_update.value(QStringLiteral("consumed_updates_per_completed_attempt"));
    if (completed_frames <= 0) {
        if (!updates_per_attempt.isNull()) {
            *out_error = QStringLiteral(
                "scenario consumed_updates_per_completed_attempt must be null");
            return false;
        }
    }
    else
    if (!validate_normalized_json_number(
            updates_per_attempt, static_cast<double>(consumed_updates) / static_cast<double>(completed_frames),
            QStringLiteral("per_consumed_update.consumed_updates_per_completed_attempt"), out_error))
    {
        return false;
    }

    const QJsonValue counters_value = per_update.value(QStringLiteral("counters"));
    if (!counters_value.isObject()) {
        *out_error = QStringLiteral("scenario per_consumed_update.counters is not an object");
        return false;
    }

    const QJsonObject counters = counters_value.toObject();
    const QStringList counter_keys = {
        QStringLiteral("qt_text_layout_calls"),
        QStringLiteral("text_leaf_nodes_created"),
        QStringLiteral("text_key_builds"),
        QStringLiteral("text_key_bytes"),
        QStringLiteral("renderer_text_rebuilds_total"),
        QStringLiteral("renderer_text_reused_total"),
        QStringLiteral("text_resource_descriptor_reuses"),
        QStringLiteral("qsg_nodes_created"),
        QStringLiteral("qsg_nodes_replaced"),
        QStringLiteral("qsg_nodes_destroyed"),
    };
    for (const QString& counter_key : counter_keys) {
        if (!counters.contains(counter_key) ||
            !validate_normalized_json_value(
                counters.value(counter_key), json_counter(object, counter_key), consumed_updates,
                QStringLiteral("per_consumed_update.counters.%1").arg(counter_key), out_error))
        {
            return false;
        }
    }

    const QJsonValue timing_value = per_update.value(QStringLiteral("timing"));
    if (!timing_value.isObject()) {
        *out_error = QStringLiteral("scenario per_consumed_update.timing is not an object");
        return false;
    }

    const QJsonObject timing = timing_value.toObject();
    const QStringList timing_keys = {
        QStringLiteral("semantics"),
        QStringLiteral("normalization"),
        QStringLiteral("scene_graph_update_latency_ns_median_per_attempt"),
        QStringLiteral("scene_graph_update_latency_ns_median_per_consumed_update"),
    };
    for (const QString& key : timing_keys) {
        if (!timing.contains(key)) {
            *out_error = QStringLiteral("scenario per_consumed_update.timing missing key: %1")
                .arg(key);
            return false;
        }
    }

    if (!timing.value(QStringLiteral("semantics")).isString() ||
        !timing.value(QStringLiteral("normalization")).isString())
    {
        *out_error = QStringLiteral("scenario per_consumed_update timing strings are invalid");
        return false;
    }

    const QJsonObject latency_summary =
        object.value(QStringLiteral("scene_graph_update_latency_ns")).toObject();
    const qint64 latency_median = latency_summary.value(QStringLiteral("median")).toInteger();
    const QJsonValue latency_median_value =
        timing.value(QStringLiteral("scene_graph_update_latency_ns_median_per_attempt"));
    const double latency_median_number = latency_median_value.toDouble(-1.0);
    if (!latency_median_value.isDouble()                           ||
        !std::isfinite(latency_median_number)                      ||
        latency_median_number             <  0.0                   ||
        std::floor(latency_median_number) != latency_median_number ||
        latency_median_number             >  static_cast<double>(std::numeric_limits<qint64>::max()))
    {
        *out_error = QStringLiteral(
            "scenario per_consumed_update timing median-per-attempt is invalid");
        return false;
    }

    if (timing.value(QStringLiteral("semantics")).toString() !=
        k_per_consumed_update_timing_semantics ||
        timing.value(QStringLiteral("normalization")).toString() !=
        expected_latency_normalization(completed_frames, consumed_updates) ||
        static_cast<qint64>(latency_median_number) != latency_median)
    {
        *out_error = QStringLiteral("scenario per_consumed_update timing metadata is inconsistent");
        return false;
    }

    const QJsonValue latency_per_update = timing.value(
        QStringLiteral("scene_graph_update_latency_ns_median_per_consumed_update"));
    if (completed_frames <= 0 || consumed_updates == 0) {
        if (!latency_per_update.isNull()) {
            *out_error = QStringLiteral(
                "scenario per-consumed-update latency must be null");
            return false;
        }
        return true;
    }

    return validate_normalized_json_number(
        latency_per_update,
        static_cast<double>(latency_median) *
            static_cast<double>(completed_frames) /
            static_cast<double>(consumed_updates),
        QStringLiteral(
            "per_consumed_update.timing."
            "scene_graph_update_latency_ns_median_per_consumed_update"),
        out_error);
}

bool validate_measurement_reuse_only_json(
    const QJsonObject& object,
    const QString&     scenario_name,
    QString*           out_error)
{
    if (!is_measurement_reuse_only_scenario(scenario_name)) {
        return true;
    }

    const qint64 completed_frames =
        object.value(QStringLiteral("completed_frames")).toInteger();
    const qint64 bridge_update_requests =
        object.value(QStringLiteral("bridge_update_requests_delta")).toInteger();
    const qint64 bridge_scheduled_updates =
        object.value(QStringLiteral("bridge_scheduled_updates_delta")).toInteger();
    const qint64 bridge_coalesced_requests =
        object.value(QStringLiteral("bridge_coalesced_requests_delta")).toInteger();
    const qint64 bridge_consumed_updates =
        object.value(QStringLiteral("bridge_consumed_updates_delta")).toInteger();
    if (bridge_consumed_updates   != completed_frames ||
        bridge_update_requests    != completed_frames ||
        bridge_scheduled_updates  != completed_frames ||
        bridge_coalesced_requests != 0)
    {
        *out_error = QStringLiteral(
            "reuse-only scenario did not isolate one update per measured attempt: %1"
        ).arg(scenario_name);
        return false;
    }

    if (json_counter(object, QStringLiteral("renderer_text_reused_total"))   <= 0 ||
        json_counter(object, QStringLiteral("text_clean_reuse_skips"))       != 0 ||
        json_counter(object, QStringLiteral("renderer_text_rebuilds_total")) != 0 ||
        json_counter(object, QStringLiteral("qt_text_layout_calls"))         != 0 ||
        json_counter(object, QStringLiteral("text_leaf_nodes_created"))      != 0 ||
        json_counter(object, QStringLiteral("qsg_nodes_created"))            != 0 ||
        json_counter(object, QStringLiteral("qsg_nodes_replaced"))           != 0 ||
        json_counter(object, QStringLiteral("qsg_nodes_destroyed"))          != 0 ||
        json_counter(object, QStringLiteral("route_fast_text_cells"))        != 0)
    {
        *out_error = QStringLiteral(
            "reuse-only scenario reported rebuild/layout/node churn: %1")
            .arg(scenario_name);
        return false;
    }

    return true;
}

bool profile_nonnegative_integer_field(
    const QJsonObject& object,
    const QString&     key,
    const QString&     label,
    qint64*            out_value,
    QString*           out_error)
{
    const QJsonValue value         = object.value(key);
    const double     numeric_value = value.toDouble(-1.0);
    if (!value.isDouble()                          ||
        numeric_value             <  0.0           ||
        std::floor(numeric_value) != numeric_value ||
        numeric_value             >  static_cast<double>(std::numeric_limits<qint64>::max()))
    {
        *out_error = QStringLiteral("profile numeric key is invalid: %1.%2")
            .arg(label)
            .arg(key);
        return false;
    }

    *out_value = static_cast<qint64>(numeric_value);
    return true;
}

bool validate_profile_node_json(
    const QJsonValue&  value,
    const QString&     label,
    bool               is_root,
    qint64*            out_total_ns,
    QString*           out_error)
{
    if (!value.isObject()) {
        *out_error = QStringLiteral("profile node is not an object: %1").arg(label);
        return false;
    }

    const QJsonObject node = value.toObject();
    const QStringList required_keys = {
        QStringLiteral("name"),
        QStringLiteral("calls"),
        QStringLiteral("total_ns"),
        QStringLiteral("self_ns"),
        QStringLiteral("child_ns"),
        QStringLiteral("min_ns"),
        QStringLiteral("max_ns"),
        QStringLiteral("mean_ns"),
        QStringLiteral("children"),
    };
    for (const QString& key : required_keys) {
        if (!node.contains(key)) {
            *out_error = QStringLiteral("profile node missing key: %1.%2")
                .arg(label)
                .arg(key);
            return false;
        }
    }

    const QJsonValue name_value = node.value(QStringLiteral("name"));
    if (!name_value.isString() || name_value.toString().isEmpty()) {
        *out_error = QStringLiteral("profile node name is invalid: %1").arg(label);
        return false;
    }

    const QStringList numeric_keys = {
        QStringLiteral("calls"),
        QStringLiteral("total_ns"),
        QStringLiteral("self_ns"),
        QStringLiteral("child_ns"),
        QStringLiteral("min_ns"),
        QStringLiteral("max_ns"),
        QStringLiteral("mean_ns"),
    };
    qint64 calls    = 0;
    qint64 total_ns = 0;
    qint64 self_ns  = 0;
    qint64 child_ns = 0;
    qint64 min_ns   = 0;
    qint64 max_ns   = 0;
    qint64 mean_ns  = 0;
    qint64* const numeric_outputs[] = {
        &calls,
        &total_ns,
        &self_ns,
        &child_ns,
        &min_ns,
        &max_ns,
        &mean_ns,
    };
    for (qsizetype index = 0; index < numeric_keys.size(); ++index) {
        if (!profile_nonnegative_integer_field(
                node, numeric_keys[index], label, numeric_outputs[static_cast<std::size_t>(index)], out_error))
        {
            return false;
        }
    }

    if (!is_root && calls == 0) {
        *out_error = QStringLiteral("non-root profile node has no calls: %1").arg(label);
        return false;
    }

    const qint64 expected_mean_ns = calls > 0 ? total_ns / calls : 0;
    if (mean_ns != expected_mean_ns) {
        *out_error = QStringLiteral("profile node mean is inconsistent: %1")
            .arg(label);
        return false;
    }

    if (calls > 0) {
        if (min_ns > max_ns) {
            *out_error = QStringLiteral("profile node min/max are inconsistent: %1")
                .arg(label);
            return false;
        }

        const long double calls_value = static_cast<long double>(calls);
        const long double total_value = static_cast<long double>(total_ns);
        if (calls_value * static_cast<long double>(min_ns) > total_value ||
            total_value                                    > calls_value * static_cast<long double>(max_ns))
        {
            *out_error = QStringLiteral("profile node timing bounds are inconsistent: %1")
                .arg(label);
            return false;
        }
    }

    const QJsonValue children_value = node.value(QStringLiteral("children"));
    if (!children_value.isArray()) {
        *out_error = QStringLiteral("profile node children is not an array: %1").arg(label);
        return false;
    }

    const QJsonArray children       = children_value.toArray();
    qint64           child_total_ns = 0;
    for (int index = 0; index < children.size(); ++index) {
        qint64 current_child_total_ns = 0;
        if (!validate_profile_node_json(
                children[index], QStringLiteral("%1.children[%2]").arg(label).arg(index),
                false, &current_child_total_ns, out_error))
        {
            return false;
        }

        child_total_ns += current_child_total_ns;
    }

    if (child_ns != child_total_ns) {
        *out_error = QStringLiteral("profile node child accounting is inconsistent: %1")
            .arg(label);
        return false;
    }

    if (self_ns + child_ns != total_ns) {
        *out_error = QStringLiteral("profile node self accounting is inconsistent: %1")
            .arg(label);
        return false;
    }

    *out_total_ns = total_ns;
    return true;
}

QString profile_thread_identity_key(const QString& role, qint64 index)
{
    return role + QChar(0x1f) + QString::number(index);
}

bool profile_root_has_child(const QJsonObject& root, const QString& name)
{
    const QJsonArray children = root.value(QStringLiteral("children")).toArray();
    for (const QJsonValue& child_value : children) {
        if (child_value.isObject() &&
            child_value.toObject().value(QStringLiteral("name")).toString() == name)
        {
            return true;
        }
    }

    return false;
}

bool profile_node_has_descendant(const QJsonObject& node, const QString& name)
{
    if (node.value(QStringLiteral("name")).toString() == name) {
        return true;
    }

    const QJsonArray children = node.value(QStringLiteral("children")).toArray();
    for (const QJsonValue& child_value : children) {
        if (child_value.isObject() &&
            profile_node_has_descendant(child_value.toObject(), name))
        {
            return true;
        }
    }

    return false;
}

bool validate_scenario_profile_value(
    const QJsonValue&  value,
    const QString&     label,
    bool               require_background_batched_update_scope,
    bool               require_selection_batched_update_scope,
    bool               require_graphic_batched_update_scope,
    bool               require_decoration_batched_update_scope,
    QString*           out_error)
{
    if (!value.isObject()) {
        *out_error = QStringLiteral("scenario profile is not an object: %1").arg(label);
        return false;
    }

    const QJsonObject profile = value.toObject();
    const QStringList required_keys = {
        QStringLiteral("profile_schema_version"),
        QStringLiteral("time_unit"),
        QStringLiteral("thread_semantics"),
        QStringLiteral("threads"),
    };
    for (const QString& key : required_keys) {
        if (!profile.contains(key)) {
            *out_error = QStringLiteral("scenario profile missing key: %1.%2")
                .arg(label)
                .arg(key);
            return false;
        }
    }

    if (!profile.value(QStringLiteral("profile_schema_version")).isDouble() ||
        profile.value(QStringLiteral("profile_schema_version")).toInt() !=
            k_profile_schema_version ||
        !profile.value(QStringLiteral("time_unit")).isString() ||
        profile.value(QStringLiteral("time_unit")).toString() != k_profile_time_unit ||
        !profile.value(QStringLiteral("thread_semantics")).isString() ||
        profile.value(QStringLiteral("thread_semantics")).toString() !=
            k_profile_thread_semantics ||
        !profile.value(QStringLiteral("threads")).isArray())
    {
        *out_error = QStringLiteral("scenario profile metadata is inconsistent: %1").arg(label);
        return false;
    }

    const QJsonArray threads = profile.value(QStringLiteral("threads")).toArray();
    if (threads.isEmpty()) {
        *out_error = QStringLiteral("scenario profile contains no threads: %1").arg(label);
        return false;
    }

    QStringList seen_thread_keys;
    bool gui_thread_has_run_scenario                 = false;
    bool render_thread_has_update_paint_node         = false;
    bool profile_has_packed_data_scope               = false;
    bool profile_has_background_row_scope            = false;
    bool profile_has_selection_row_scope             = false;
    bool profile_has_graphic_row_scope               = false;
    bool profile_has_decoration_row_scope            = false;
    bool profile_has_background_batched_update_scope = false;
    bool profile_has_selection_batched_update_scope  = false;
    bool profile_has_graphic_batched_update_scope    = false;
    bool profile_has_decoration_batched_update_scope = false;
    for (int index = 0; index < threads.size(); ++index) {
        if (!threads[index].isObject()) {
            *out_error = QStringLiteral("scenario profile thread is not an object: %1[%2]")
                .arg(label)
                .arg(index);
            return false;
        }

        const QJsonObject thread      = threads[index].toObject();
        const QJsonValue  thread_role = thread.value(QStringLiteral("thread_role"));
        const QJsonValue  root        = thread.value(QStringLiteral("root"));
        if (!thread_role.isString() || thread_role.toString().isEmpty() || !root.isObject())
        {
            *out_error = QStringLiteral("scenario profile thread metadata is invalid: %1[%2]")
                .arg(label)
                .arg(index);
            return false;
        }

        qint64 thread_index_value = 0;
        if (!profile_nonnegative_integer_field(
                thread, QStringLiteral("thread_index"),
                QStringLiteral("%1.threads[%2]").arg(label).arg(index), &thread_index_value, out_error))
        {
            return false;
        }

        const QString thread_key =
            profile_thread_identity_key(thread_role.toString(), thread_index_value);
        if (seen_thread_keys.contains(thread_key)) {
            *out_error = QStringLiteral("scenario profile thread is duplicated: %1[%2]")
                .arg(label)
                .arg(index);
            return false;
        }
        seen_thread_keys.push_back(thread_key);

        qint64 root_total_ns = 0;
        if (!validate_profile_node_json(
                root, QStringLiteral("%1.threads[%2].root").arg(label).arg(index), true, &root_total_ns, out_error))
        {
            return false;
        }

        const QString role = thread_role.toString();
        if (role == QStringLiteral("gui") && thread_index_value == 0) {
            gui_thread_has_run_scenario =
                profile_root_has_child(root.toObject(), QStringLiteral("run_scenario"));
        }
        if (role == QStringLiteral("render") && thread_index_value == 0) {
            render_thread_has_update_paint_node = profile_root_has_child(
                root.toObject(),
                QStringLiteral("VNM_TerminalSurface::updatePaintNode"));
        }
        profile_has_packed_data_scope =
            profile_has_packed_data_scope ||
            profile_node_has_descendant(
                root.toObject(),
                QStringLiteral("build_terminal_render_frame::packed_data"));
        profile_has_background_row_scope =
            profile_has_background_row_scope ||
            profile_node_has_descendant(
                root.toObject(),
                QStringLiteral("sync_background_rect_row_layer"));
        profile_has_selection_row_scope =
            profile_has_selection_row_scope ||
            profile_node_has_descendant(
                root.toObject(),
                QStringLiteral("sync_selection_rect_row_layer"));
        profile_has_graphic_row_scope =
            profile_has_graphic_row_scope ||
            profile_node_has_descendant(
                root.toObject(),
                QStringLiteral("sync_graphic_rect_row_layer"));
        profile_has_decoration_row_scope =
            profile_has_decoration_row_scope ||
            profile_node_has_descendant(
                root.toObject(),
                QStringLiteral("sync_decoration_rect_row_layer"));
        profile_has_background_batched_update_scope =
            profile_has_background_batched_update_scope ||
            profile_node_has_descendant(
                root.toObject(),
                QStringLiteral("update_background_batched_rect_geometry"));
        profile_has_selection_batched_update_scope =
            profile_has_selection_batched_update_scope ||
            profile_node_has_descendant(
                root.toObject(),
                QStringLiteral("update_selection_batched_rect_geometry"));
        profile_has_graphic_batched_update_scope =
            profile_has_graphic_batched_update_scope ||
            profile_node_has_descendant(
                root.toObject(),
                QStringLiteral("update_graphic_batched_rect_geometry"));
        profile_has_decoration_batched_update_scope =
            profile_has_decoration_batched_update_scope ||
            profile_node_has_descendant(
                root.toObject(),
                QStringLiteral("update_decoration_batched_rect_geometry"));
    }

    if (!seen_thread_keys.contains(
            profile_thread_identity_key(QStringLiteral("gui"), 0)) ||
        !seen_thread_keys.contains(
            profile_thread_identity_key(QStringLiteral("render"), 0)))
    {
        *out_error = QStringLiteral("scenario profile required threads are missing: %1")
            .arg(label);
        return false;
    }

    if (!gui_thread_has_run_scenario ||
        !render_thread_has_update_paint_node ||
        !profile_has_packed_data_scope ||
        !profile_has_background_row_scope ||
        !profile_has_selection_row_scope ||
        !profile_has_graphic_row_scope ||
        !profile_has_decoration_row_scope)
    {
        *out_error = QStringLiteral("scenario profile required scopes are missing: %1")
            .arg(label);
        return false;
    }

    if (require_background_batched_update_scope &&
        !profile_has_background_batched_update_scope)
    {
        *out_error = QStringLiteral(
            "scenario profile background batched-geometry scope is missing: %1")
            .arg(label);
        return false;
    }
    if (require_selection_batched_update_scope &&
        !profile_has_selection_batched_update_scope)
    {
        *out_error = QStringLiteral(
            "scenario profile selection batched-geometry scope is missing: %1")
            .arg(label);
        return false;
    }
    if (require_graphic_batched_update_scope &&
        !profile_has_graphic_batched_update_scope)
    {
        *out_error = QStringLiteral(
            "scenario profile graphic batched-geometry scope is missing: %1")
            .arg(label);
        return false;
    }
    if (require_decoration_batched_update_scope &&
        !profile_has_decoration_batched_update_scope)
    {
        *out_error = QStringLiteral(
            "scenario profile decoration batched-geometry scope is missing: %1")
            .arg(label);
        return false;
    }

    return true;
}

bool validate_scenario_profile_json(
    const QJsonObject& object,
    QString*           out_error)
{
    const QJsonValue profile = object.value(QStringLiteral("profile"));
    if (profile.isNull()) {
        return true;
    }

    const bool require_background_batched_update_scope =
        json_counter(object, QStringLiteral("background_batched_rects")) > 0;
    const bool require_selection_batched_update_scope =
        json_counter(object, QStringLiteral("selection_batched_rects")) > 0;
    const bool require_graphic_batched_update_scope =
        json_counter(object, QStringLiteral("graphic_batched_rects")) > 0;
    const bool require_decoration_batched_update_scope =
        json_counter(object, QStringLiteral("decoration_batched_rects")) > 0;
    return
        validate_scenario_profile_value(
            profile,
            QStringLiteral("scenario.profile"),
            require_background_batched_update_scope,
            require_selection_batched_update_scope,
            require_graphic_batched_update_scope,
            require_decoration_batched_update_scope,
            out_error);
}

bool validate_profiling_metadata_json(
    const QJsonObject& root,
    const App_options& options,
    QString*           out_error)
{
    const QJsonValue value = root.value(QStringLiteral("profiling"));
    if (!value.isObject()) {
        *out_error = QStringLiteral("benchmark profiling metadata is not an object");
        return false;
    }

    const QJsonObject profiling = value.toObject();
    const QStringList required_keys = {
        QStringLiteral("enabled"),
        QStringLiteral("profile_schema_version"),
        QStringLiteral("time_unit"),
        QStringLiteral("thread_semantics"),
        QStringLiteral("profile_json_requested"),
        QStringLiteral("profile_text_requested"),
    };
    for (const QString& key : required_keys) {
        if (!profiling.contains(key)) {
            *out_error = QStringLiteral("benchmark profiling metadata missing key: %1")
                .arg(key);
            return false;
        }
    }

    if (!profiling.value(QStringLiteral("enabled")).isBool()                           ||
        profiling.value(QStringLiteral("enabled")).toBool()     != options.profile     ||
        !profiling.value(QStringLiteral("profile_schema_version")).isDouble()          ||
        profiling.value(QStringLiteral("profile_schema_version")).toInt() !=
            k_profile_schema_version                                                   ||
        !profiling.value(QStringLiteral("time_unit")).isString()                       ||
        profiling.value(QStringLiteral("time_unit")).toString() != k_profile_time_unit ||
        !profiling.value(QStringLiteral("thread_semantics")).isString() ||
        profiling.value(QStringLiteral("thread_semantics")).toString() !=
            k_profile_thread_semantics ||
        !profiling.value(QStringLiteral("profile_json_requested")).isBool() ||
        profiling.value(QStringLiteral("profile_json_requested")).toBool() !=
            !options.profile_json_path.isEmpty() ||
        !profiling.value(QStringLiteral("profile_text_requested")).isBool() ||
        profiling.value(QStringLiteral("profile_text_requested")).toBool() !=
            !options.profile_text_path.isEmpty())
    {
        *out_error = QStringLiteral("benchmark profiling metadata changed");
        return false;
    }

    return true;
}

bool validate_scenario_json(
    const QJsonObject& object,
    const App_options& options,
    QString*           out_error)
{
    const QStringList required_keys = {
        QStringLiteral("name"),
        QStringLiteral("source_mode"),
        QStringLiteral("execution_mode"),
        QStringLiteral("latency_semantics"),
        QStringLiteral("elapsed_semantics"),
        QStringLiteral("status"),
        QStringLiteral("iterations"),
        QStringLiteral("warmup"),
        QStringLiteral("elapsed_ns"),
        QStringLiteral("snapshot_prep_ns"),
        QStringLiteral("session_scroll_ns"),
        QStringLiteral("workload_action_ns"),
        QStringLiteral("scene_graph_update_latency_ns"),
        QStringLiteral("scene_graph_render_wait_ns"),
        QStringLiteral("readback_ns"),
        QStringLiteral("completed_frames"),
        QStringLiteral("timeout_count"),
        QStringLiteral("renderer_text_rebuilds_total"),
        QStringLiteral("renderer_text_reused_total"),
        QStringLiteral("renderer_text_removed_total"),
        QStringLiteral("renderer_text_failures_total"),
        QStringLiteral("text_leaf_nodes_created"),
        QStringLiteral("text_cache_entry_child_nodes_cleared_for_replacement"),
        QStringLiteral("text_cache_entry_child_nodes_cleared_for_removal"),
        QStringLiteral("text_cache_entry_max_child_nodes_cleared"),
        QStringLiteral("text_clean_reuse_skips"),
        QStringLiteral("text_resource_descriptor_reuses"),
        QStringLiteral("text_coalescing_candidate_groups"),
        QStringLiteral("text_coalescing_enabled_groups"),
        QStringLiteral("text_resource_runs_before_coalescing"),
        QStringLiteral("text_resource_runs_after_coalescing"),
        QStringLiteral("simple_content_cells_considered"),
        QStringLiteral("simple_content_eligible_cells"),
        QStringLiteral("simple_content_eligible_after_all_gates_cells"),
        QStringLiteral("simple_content_rows_with_eligible_cells"),
        QStringLiteral("simple_content_styles_with_eligible_cells"),
        QStringLiteral("simple_content_dirty_eligible_cells"),
        QStringLiteral("simple_content_clean_eligible_cells"),
        QStringLiteral("simple_content_text_category_empty_cells"),
        QStringLiteral("simple_content_text_category_printable_ascii_cells"),
        QStringLiteral("simple_content_text_category_other_ascii_cells"),
        QStringLiteral("simple_content_text_category_non_ascii_cells"),
        QStringLiteral("simple_content_route_none_cells"),
        QStringLiteral("simple_content_route_fast_text_cells"),
        QStringLiteral("simple_content_route_qt_text_layout_cells"),
        QStringLiteral("simple_content_route_graphic_geometry_cells"),
        QStringLiteral("simple_content_route_fallback_cells"),
        QStringLiteral("simple_content_rejection_none_cells"),
        QStringLiteral("simple_content_rejection_empty_text_cells"),
        QStringLiteral("simple_content_rejection_invalid_grid_cells"),
        QStringLiteral("simple_content_rejection_invalid_position_cells"),
        QStringLiteral("simple_content_rejection_invalid_style_id_cells"),
        QStringLiteral("simple_content_rejection_wide_continuation_cells"),
        QStringLiteral("simple_content_rejection_invalid_display_width_cells"),
        QStringLiteral("simple_content_rejection_invalid_text_encoding_cells"),
        QStringLiteral("simple_content_rejection_invalid_text_width_cells"),
        QStringLiteral("simple_content_rejection_multi_cell_text_cells"),
        QStringLiteral("simple_content_rejection_non_printable_ascii_cells"),
        QStringLiteral("simple_content_rejection_non_ascii_text_cells"),
        QStringLiteral("simple_content_rejection_decoration_cells"),
        QStringLiteral("simple_content_rejection_hyperlink_cells"),
        QStringLiteral("simple_content_rejection_terminal_graphic_cells"),
        QStringLiteral("route_fast_text_cells"),
        QStringLiteral("route_qt_text_layout_runs"),
        QStringLiteral("route_graphic_geometry_cells"),
        QStringLiteral("route_fallback_cells"),
        QStringLiteral("qt_text_layout_calls"),
        QStringLiteral("qsg_nodes_created"),
        QStringLiteral("qsg_nodes_replaced"),
        QStringLiteral("qsg_nodes_destroyed"),
        QStringLiteral("background_qsg_nodes_created"),
        QStringLiteral("background_qsg_nodes_replaced"),
        QStringLiteral("background_qsg_nodes_destroyed"),
        QStringLiteral("text_key_builds"),
        QStringLiteral("text_key_bytes"),
        QStringLiteral("rect_key_builds"),
        QStringLiteral("rect_key_bytes"),
        QStringLiteral("cache_key_builds"),
        QStringLiteral("cache_key_bytes"),
        QStringLiteral("text_cache_entries_created"),
        QStringLiteral("text_cache_entries_replaced"),
        QStringLiteral("text_cache_entries_removed"),
        QStringLiteral("frame_background_rects"),
        QStringLiteral("frame_selection_rects"),
        QStringLiteral("frame_graphic_rects"),
        QStringLiteral("frame_graphic_arcs"),
        QStringLiteral("frame_text_runs"),
        QStringLiteral("frame_cursor_text_runs"),
        QStringLiteral("frame_decorations"),
        QStringLiteral("frame_cursors"),
        QStringLiteral("frame_cursor_graphic_rects"),
        QStringLiteral("frame_cursor_graphic_arcs"),
        QStringLiteral("frame_overlay_rects"),
        QStringLiteral("frame_dirty_row_ranges"),
        QStringLiteral("frame_packed_rows"),
        QStringLiteral("frame_packed_text_spans"),
        QStringLiteral("frame_packed_text_cells"),
        QStringLiteral("frame_packed_graphic_spans"),
        QStringLiteral("frame_packed_graphic_cells"),
        QStringLiteral("frame_packed_payload_bytes"),
        QStringLiteral("row_cache_hits"),
        QStringLiteral("row_cache_clean_skips"),
        QStringLiteral("background_row_rects_before_coalescing"),
        QStringLiteral("background_row_rects_after_coalescing"),
        QStringLiteral("background_batched_rects"),
        QStringLiteral("background_batched_vertices"),
        QStringLiteral("selection_batched_rects"),
        QStringLiteral("selection_batched_vertices"),
        QStringLiteral("graphic_batched_rects"),
        QStringLiteral("graphic_batched_vertices"),
        QStringLiteral("decoration_batched_rects"),
        QStringLiteral("decoration_batched_vertices"),
        QStringLiteral("background_rows_rebuilt"),
        QStringLiteral("background_rows_reused"),
        QStringLiteral("background_row_clean_reuse_skips"),
        QStringLiteral("background_rows_removed"),
        QStringLiteral("background_row_cache_fallbacks"),
        QStringLiteral("selection_rows_rebuilt"),
        QStringLiteral("selection_rows_reused"),
        QStringLiteral("selection_row_clean_reuse_skips"),
        QStringLiteral("selection_rows_removed"),
        QStringLiteral("selection_row_cache_fallbacks"),
        QStringLiteral("decoration_rows_rebuilt"),
        QStringLiteral("decoration_rows_reused"),
        QStringLiteral("decoration_row_clean_reuse_skips"),
        QStringLiteral("decoration_rows_removed"),
        QStringLiteral("decoration_row_cache_fallbacks"),
        QStringLiteral("graphic_rect_rows_rebuilt"),
        QStringLiteral("graphic_rect_rows_reused"),
        QStringLiteral("graphic_rect_row_clean_reuse_skips"),
        QStringLiteral("graphic_rect_rows_removed"),
        QStringLiteral("graphic_rect_row_cache_fallbacks"),
        QStringLiteral("graphic_arc_rows_rebuilt"),
        QStringLiteral("graphic_arc_rows_reused"),
        QStringLiteral("graphic_arc_row_clean_reuse_skips"),
        QStringLiteral("graphic_arc_rows_removed"),
        QStringLiteral("graphic_arc_row_cache_fallbacks"),
        QStringLiteral("bridge_update_requests_delta"),
        QStringLiteral("bridge_scheduled_updates_delta"),
        QStringLiteral("bridge_coalesced_requests_delta"),
        QStringLiteral("bridge_consumed_updates_delta"),
        QStringLiteral("per_consumed_update"),
        QStringLiteral("lifecycle_delta"),
        QStringLiteral("process_memory"),
        QStringLiteral("rows"),
        QStringLiteral("columns"),
        QStringLiteral("window_size"),
        QStringLiteral("viewport_metrics_applicable"),
        QStringLiteral("viewport_scrollback_rows"),
        QStringLiteral("viewport_initial_offset_from_tail"),
        QStringLiteral("viewport_final_offset_from_tail"),
        QStringLiteral("viewport_expected_offset_from_tail"),
        QStringLiteral("wheel_burst_size"),
        QStringLiteral("wheel_steps_per_event"),
        QStringLiteral("viewport_expected_burst_delta"),
        QStringLiteral("viewport_final_burst_delta"),
        QStringLiteral("viewport_expected_top_line"),
        QStringLiteral("viewport_final_top_line"),
        QStringLiteral("wheel_events_accepted_count"),
        QStringLiteral("backend_writes_total"),
        QStringLiteral("backend_write_bytes_total"),
        QStringLiteral("backend_errors_total"),
        QStringLiteral("session_snapshots_observed"),
        QStringLiteral("scrollback_limit_configured"),
        QStringLiteral("scrollback_limit_observed"),
        QStringLiteral("output_pause_requests_total"),
        QStringLiteral("output_pause_enabled_count"),
        QStringLiteral("output_pause_disabled_count"),
        QStringLiteral("render_expected"),
        QStringLiteral("output_high_water_observed"),
        QStringLiteral("write_high_water_observed"),
        QStringLiteral("render_measurement_semantics"),
        QStringLiteral("queue_pressure_semantics"),
        QStringLiteral("workload_actions_expected_count"),
        QStringLiteral("workload_actions_accepted_count"),
        QStringLiteral("dominant_latency_component"),
        QStringLiteral("primary_pressure"),
        QStringLiteral("structural_checks"),
        QStringLiteral("profile"),
    };

    for (const QString& key : required_keys) {
        if (!object.contains(key)) {
            *out_error = QStringLiteral("scenario JSON missing key: %1").arg(key);
            return false;
        }
    }

    if (!validate_summary_json(object, QStringLiteral("elapsed_ns"), out_error)                  ||
        !validate_summary_json(object, QStringLiteral("snapshot_prep_ns"), out_error)            ||
        !validate_summary_json(object, QStringLiteral("session_scroll_ns"), out_error)           ||
        !validate_summary_json(object, QStringLiteral("workload_action_ns"), out_error)          ||
        !validate_summary_json(
            object, QStringLiteral("scene_graph_update_latency_ns"), out_error)                  ||
        !validate_summary_json( object, QStringLiteral("scene_graph_render_wait_ns"), out_error) ||
        !validate_summary_json( object, QStringLiteral("readback_ns"), out_error)                ||
        !validate_process_memory_json(object, out_error)                                         ||
        !validate_window_size_json(object, out_error)                                            ||
        !validate_lifecycle_json(object, out_error)                                              ||
        !validate_renderer_counter_json(object, out_error)                                       ||
        !validate_renderer_counter_invariants(object, out_error)                                 ||
        !validate_per_consumed_update_json(object, out_error)                                    ||
        !validate_scenario_profile_json(object, out_error))
    {
        return false;
    }

    if (!scenario_status_ok(object)) {
        *out_error = QStringLiteral("scenario did not complete: %1")
            .arg(object.value(QStringLiteral("name")).toString());
        return false;
    }

    const QString scenario_name = object.value(QStringLiteral("name")).toString();
    if (!is_known_scenario(scenario_name)) {
        *out_error = QStringLiteral("unknown scenario in benchmark output: %1")
            .arg(scenario_name);
        return false;
    }

    if (object.value(QStringLiteral("source_mode")).toString()       != scenario_source_mode(scenario_name)    ||
        object.value(QStringLiteral("execution_mode")).toString()    != scenario_execution_mode(scenario_name) ||
        object.value(QStringLiteral("latency_semantics")).toString() != k_latency_semantics                    ||
        object.value(QStringLiteral("elapsed_semantics")).toString() != k_elapsed_semantics)
    {
        *out_error = QStringLiteral("scenario metadata is inconsistent: %1")
            .arg(scenario_name);
        return false;
    }

    const bool render_expected = object.value(QStringLiteral("render_expected")).toBool();
    const QString expected_render_semantics = render_expected
        ? k_render_expected_semantics
        : k_no_render_expected_semantics;
    if (object.value(QStringLiteral("render_measurement_semantics")).toString() !=
        expected_render_semantics)
    {
        *out_error = QStringLiteral("scenario render semantics are inconsistent: %1")
            .arg(scenario_name);
        return false;
    }

    if (!required_structural_checks_passed(object)) {
        *out_error = QStringLiteral("scenario structural checks failed: %1")
            .arg(object.value(QStringLiteral("name")).toString());
        return false;
    }

    if (object.value(QStringLiteral("dominant_latency_component")).toString().isEmpty() ||
        object.value(QStringLiteral("primary_pressure")).toString().isEmpty())
    {
        *out_error = QStringLiteral("scenario bottleneck metadata is empty: %1")
            .arg(scenario_name);
        return false;
    }

    const int workload_actions_expected =
        object.value(QStringLiteral("workload_actions_expected_count")).toInt();
    const int workload_actions_accepted =
        object.value(QStringLiteral("workload_actions_accepted_count")).toInt();
    if (workload_actions_expected     <  0                         ||
        workload_actions_accepted     <  0                         ||
        workload_actions_accepted     >  workload_actions_expected ||
        (workload_actions_expected > 0 &&
         workload_actions_accepted != workload_actions_expected))
    {
        *out_error = QStringLiteral("scenario workload action counters failed: %1")
            .arg(scenario_name);
        return false;
    }

    const int completed_frames = object.value(QStringLiteral("completed_frames")).toInt();
    const int iterations       = object.value(QStringLiteral("iterations")).toInt();
    if (json_counter(object, QStringLiteral("renderer_text_failures_total")) != 0 ||
        completed_frames                                                     != iterations)
    {
        *out_error = QStringLiteral("scenario renderer counters failed: %1")
            .arg(object.value(QStringLiteral("name")).toString());
        return false;
    }

    if (options.software_renderer &&
        (json_counter(object, QStringLiteral("graphic_batched_rects")) != 0 ||
         json_counter(object, QStringLiteral("graphic_batched_vertices")) != 0))
    {
        *out_error = QStringLiteral(
            "software renderer reported graphic batched-geometry counters: %1"
        ).arg(scenario_name);
        return false;
    }

    if (render_expected) {
        const qint64 bridge_update_requests =
            object.value(QStringLiteral("bridge_update_requests_delta")).toInteger();
        const qint64 bridge_scheduled_updates =
            object.value(QStringLiteral("bridge_scheduled_updates_delta")).toInteger();
        const qint64 bridge_coalesced_requests =
            object.value(QStringLiteral("bridge_coalesced_requests_delta")).toInteger();
        const qint64 bridge_consumed_updates =
            object.value(QStringLiteral("bridge_consumed_updates_delta")).toInteger();
        const bool text_work_observed = is_measurement_reuse_only_scenario(scenario_name)
            ? json_counter(object, QStringLiteral("renderer_text_reused_total")) > 0 ||
                json_counter(object, QStringLiteral("text_leaf_nodes_created")) > 0
            : json_counter(object, QStringLiteral("text_leaf_nodes_created")) > 0;
        if (!text_work_observed ||
            bridge_update_requests   < completed_frames              ||
            bridge_consumed_updates  < completed_frames              ||
            bridge_update_requests !=
                bridge_scheduled_updates + bridge_coalesced_requests ||
            bridge_scheduled_updates < bridge_consumed_updates)
        {
            *out_error = QStringLiteral("scenario render accounting failed: %1")
                .arg(scenario_name);
            return false;
        }
    }
    else {
        if (json_counter(object, QStringLiteral("text_leaf_nodes_created"))             != 0 ||
            json_counter(object, QStringLiteral("renderer_text_rebuilds_total"))        != 0 ||
            json_counter(object, QStringLiteral("renderer_text_reused_total"))          != 0 ||
            object.value(QStringLiteral("bridge_update_requests_delta")).toInteger()    != 0 ||
            object.value(QStringLiteral("bridge_scheduled_updates_delta")).toInteger()  != 0 ||
            object.value(QStringLiteral("bridge_coalesced_requests_delta")).toInteger() != 0 ||
            object.value(QStringLiteral("bridge_consumed_updates_delta")).toInteger()   != 0 ||
            object.value(QStringLiteral("session_snapshots_observed")).toInt()          != 0)
        {
            *out_error = QStringLiteral("scenario no-render accounting failed: %1")
                .arg(scenario_name);
            return false;
        }
    }

    if (!validate_measurement_reuse_only_json(object, scenario_name, out_error)) {
        return false;
    }

    if (scenario_name == QStringLiteral("ascii_full_dirty_reuse_only")) {
        const qint64 consumed_updates =
            json_counter(object, QStringLiteral("bridge_consumed_updates_delta"));
        const QJsonObject per_update =
            object.value(QStringLiteral("per_consumed_update")).toObject();
        const QJsonObject normalized_counters =
            per_update.value(QStringLiteral("counters")).toObject();
        if (json_counter(object, QStringLiteral("text_resource_descriptor_reuses"))             <= 0                ||
            json_counter(object, QStringLiteral("text_clean_reuse_skips"))                      != 0                ||
            json_counter(object, QStringLiteral("route_fast_text_cells"))                       != 0                ||
            json_counter(object, QStringLiteral("qt_text_layout_calls"))                        != 0                ||
            json_counter(object, QStringLiteral("text_leaf_nodes_created"))                     != 0                ||
            json_counter(object, QStringLiteral("renderer_text_rebuilds_total"))                != 0                ||
            json_counter(object, QStringLiteral("renderer_text_reused_total"))                  <= 0                ||
            json_counter(object, QStringLiteral("qsg_nodes_created"))                           != 0                ||
            json_counter(object, QStringLiteral("qsg_nodes_replaced"))                          != 0                ||
            json_counter(object, QStringLiteral("qsg_nodes_destroyed"))                         != 0                ||
            json_counter(object, QStringLiteral("text_key_builds"))                             != consumed_updates ||
            json_counter(object, QStringLiteral("text_key_bytes"))                              <= 0                ||
            !json_numbers_match(
                normalized_counters.value(QStringLiteral("text_key_builds"        )).toDouble(-1.0),
                1.0)                                                                                                ||
            normalized_counters.value(QStringLiteral("qt_text_layout_calls"   )).toDouble(-1.0) != 0.0              ||
            normalized_counters.value(QStringLiteral("text_leaf_nodes_created")).toDouble(-1.0) != 0.0              ||
            normalized_counters.value(QStringLiteral("qsg_nodes_created"      )).toDouble(-1.0) != 0.0              ||
            normalized_counters.value(QStringLiteral("qsg_nodes_replaced"     )).toDouble(-1.0) != 0.0              ||
            normalized_counters.value(QStringLiteral("qsg_nodes_destroyed"    )).toDouble(-1.0) != 0.0)
        {
            *out_error = QStringLiteral(
                "ASCII reuse-only text-resource descriptor path was not isolated: %1"
            ).arg(scenario_name);
            return false;
        }
    }

    if (scenario_name == QStringLiteral("ascii_full_dirty_same_content")) {
        if (json_counter(object, QStringLiteral("text_resource_descriptor_reuses")) <= 0 ||
            json_counter(object, QStringLiteral("text_clean_reuse_skips"))          != 0 ||
            json_counter(object, QStringLiteral("route_fast_text_cells"))           != 0)
        {
            *out_error = QStringLiteral(
                "text-resource descriptor reuse was not observed: %1")
                .arg(scenario_name);
            return false;
        }
    }

    if (scenario_name == QStringLiteral("styled_ascii_full_dirty_reuse_only")) {
        const qint64 consumed_updates =
            json_counter(object, QStringLiteral("bridge_consumed_updates_delta"));
        if (json_counter(object, QStringLiteral("frame_text_runs")) <= 0                                          ||
            json_counter( object, QStringLiteral("simple_content_text_category_printable_ascii_cells")) <= 0      ||
            json_counter(object, QStringLiteral("simple_content_styles_with_eligible_cells")) <= consumed_updates ||
            json_counter(object, QStringLiteral("text_resource_descriptor_reuses")) <= 0                          ||
            json_counter(object, QStringLiteral("text_key_builds")) != consumed_updates                           ||
            json_counter(object, QStringLiteral("route_fast_text_cells")) != 0)
        {
            *out_error = QStringLiteral(
                "styled ASCII reuse-only descriptor counters were not observed: %1"
            ).arg(scenario_name);
            return false;
        }
    }

    if (scenario_name == QStringLiteral("mixed_text_full_dirty_reuse_only")) {
        const qint64 consumed_updates =
            json_counter(object, QStringLiteral("bridge_consumed_updates_delta"));
        const qint64 non_ascii_rejections = json_counter(
            object,
            QStringLiteral("simple_content_rejection_non_ascii_text_cells")) +
            json_counter(object, QStringLiteral("simple_content_rejection_multi_cell_text_cells"));
        if (json_counter(object, QStringLiteral("frame_text_runs")) <= 0                                     ||
            json_counter( object, QStringLiteral("simple_content_text_category_printable_ascii_cells")) <= 0 ||
            json_counter( object, QStringLiteral("simple_content_text_category_non_ascii_cells")) <= 0       ||
            json_counter( object, QStringLiteral("simple_content_rejection_wide_continuation_cells")) <= 0   ||
            non_ascii_rejections <= 0                                                                        ||
            json_counter(object, QStringLiteral("text_resource_descriptor_reuses")) <= 0                     ||
            json_counter(object, QStringLiteral("text_key_builds")) != consumed_updates                      ||
            json_counter(object, QStringLiteral("route_fast_text_cells")) != 0)
        {
            *out_error = QStringLiteral(
                "mixed text reuse-only descriptor counters were not observed: %1"
            ).arg(scenario_name);
            return false;
        }
    }

    if (scenario_name == QStringLiteral("single_row_geometry_update")) {
        const qint64 graphic_arc_work = json_counter(
            object,
            QStringLiteral("graphic_arc_rows_rebuilt")) +
            json_counter(object, QStringLiteral("graphic_arc_row_cache_fallbacks"));
        const bool all_row_families_skipped_cleanly =
            json_counter(object, QStringLiteral("background_row_clean_reuse_skips"))   > 0 &&
            json_counter(object, QStringLiteral("selection_row_clean_reuse_skips"))    > 0 &&
            json_counter(object, QStringLiteral("decoration_row_clean_reuse_skips"))   > 0 &&
            json_counter(object, QStringLiteral("graphic_rect_row_clean_reuse_skips")) > 0 &&
            json_counter(object, QStringLiteral("graphic_arc_row_clean_reuse_skips"))  > 0;
        if (json_counter(object, QStringLiteral("background_rows_rebuilt"))      <= 0  ||
            (!options.software_renderer &&
                json_counter(object, QStringLiteral("background_batched_rects")) <= 0) ||
            json_counter(object, QStringLiteral("selection_rows_rebuilt"))       <= 0  ||
            (!options.software_renderer &&
                json_counter(object, QStringLiteral("selection_batched_rects"))  <= 0) ||
            json_counter(object, QStringLiteral("decoration_rows_rebuilt"))      <= 0  ||
            (!options.software_renderer &&
                json_counter(object, QStringLiteral("decoration_batched_rects")) <= 0) ||
            json_counter(object, QStringLiteral("graphic_rect_rows_rebuilt"))    <= 0  ||
            (!options.software_renderer &&
                json_counter(object, QStringLiteral("graphic_batched_rects"))    <= 0) ||
            graphic_arc_work                                                     <= 0  ||
            !all_row_families_skipped_cleanly)
        {
            *out_error = QStringLiteral("geometry row-cache counters were not observed: %1")
                .arg(scenario_name);
            return false;
        }
    }

    const QStringList summary_names = {
        QStringLiteral("elapsed_ns"),
        QStringLiteral("snapshot_prep_ns"),
        QStringLiteral("session_scroll_ns"),
        QStringLiteral("workload_action_ns"),
        QStringLiteral("scene_graph_update_latency_ns"),
        QStringLiteral("scene_graph_render_wait_ns"),
        QStringLiteral("readback_ns"),
    };
    for (const QString& summary_name : summary_names) {
        const QJsonObject summary = object.value(summary_name).toObject();
        if (summary.value(QStringLiteral("sample_count")).toInt() != completed_frames) {
            *out_error = QStringLiteral(
                "scenario timing sample count mismatch: %1.%2"
            ).arg(scenario_name).arg(summary_name);
            return false;
        }
    }

    const QJsonObject process_memory = object.value(QStringLiteral("process_memory")).toObject();
    const int memory_sample_count = process_memory
        .value(QStringLiteral("resident_bytes"))
        .toObject()
        .value(QStringLiteral("sample_count"))
        .toInt();
    if (memory_sample_count < 0 || memory_sample_count > completed_frames) {
        *out_error = QStringLiteral("scenario memory sample count mismatch: %1")
            .arg(scenario_name);
        return false;
    }
    if (process_memory.value(QStringLiteral("status")).toString() == QStringLiteral("sampled") &&
        memory_sample_count                                       != completed_frames)
    {
        *out_error = QStringLiteral("scenario memory sample coverage is incomplete: %1")
            .arg(scenario_name);
        return false;
    }

    if (is_surface_session_scenario(scenario_name)) {
        const int configured_limit =
            object.value(QStringLiteral("scrollback_limit_configured")).toInt();
        const int observed_limit =
            object.value(QStringLiteral("scrollback_limit_observed")).toInt();
        if (configured_limit <= 0 ||
            observed_limit   <  0 ||
            observed_limit   >  configured_limit)
        {
            *out_error = QStringLiteral("surface/session scrollback limit leaked: %1")
                .arg(scenario_name);
            return false;
        }
    }
    else
    if (object.value(QStringLiteral("scrollback_limit_configured")).toInt() != 0 ||
        object.value(QStringLiteral("scrollback_limit_observed")).toInt()   != 0)
    {
        *out_error = QStringLiteral("snapshot scenario has session scrollback metadata: %1")
            .arg(scenario_name);
        return false;
    }

    if (scenario_name == QStringLiteral("surface_session_output_high_water")) {
        if (!object.value(QStringLiteral("output_high_water_observed")).toBool()     ||
            object.value(QStringLiteral("output_pause_enabled_count")).toInt()  <= 0 ||
            object.value(QStringLiteral("output_pause_disabled_count")).toInt() <= 0 ||
            object.value(QStringLiteral("queue_pressure_semantics")).toString() !=
                k_output_high_water_queue_pressure_semantics)
        {
            *out_error = QStringLiteral("output high-water pressure was not observed: %1")
                .arg(scenario_name);
            return false;
        }
    }
    else
    if (scenario_name == QStringLiteral("surface_session_write_high_water")) {
        if (!object.value(QStringLiteral("write_high_water_observed")).toBool() ||
            object.value(QStringLiteral("backend_write_bytes_total")).toInt() <
                k_surface_session_write_high_water_bytes ||
            object.value(QStringLiteral("queue_pressure_semantics")).toString().isEmpty())
        {
            *out_error = QStringLiteral("write high-water reservation was not observed: %1")
                .arg(scenario_name);
            return false;
        }
    }

    const QJsonObject lifecycle =
        object.value(QStringLiteral("lifecycle_delta")).toObject();
    if (lifecycle.value(QStringLiteral("live_root_nodes")).toInteger()         >  1 ||
        (render_expected &&
            lifecycle.value(QStringLiteral("live_text_resources")).toInteger() <= 0))
    {
        *out_error = QStringLiteral("scenario lifecycle counters failed: %1")
            .arg(object.value(QStringLiteral("name")).toString());
        return false;
    }

    if (object.value(QStringLiteral("viewport_metrics_applicable")).toBool()) {
        const int wheel_burst_size =
            object.value(QStringLiteral("wheel_burst_size")).toInt();
        const int wheel_steps_per_event =
            object.value(QStringLiteral("wheel_steps_per_event")).toInt();
        const int expected_burst_delta =
            object.value(QStringLiteral("viewport_expected_burst_delta")).toInt();
        const int final_burst_delta =
            object.value(QStringLiteral("viewport_final_burst_delta")).toInt();
        if (object.value(QStringLiteral("viewport_scrollback_rows")).toInt() <= 0                    ||
            object.value(QStringLiteral("viewport_final_offset_from_tail")).toInt() !=
                object.value(QStringLiteral("viewport_expected_offset_from_tail")).toInt()           ||
            wheel_burst_size                                                 <= 0                    ||
            wheel_steps_per_event                                            <= 0                    ||
            std::abs(expected_burst_delta) !=
                wheel_burst_size *
                    wheel_steps_per_event *
                    k_surface_session_plain_wheel_lines_per_event                                    ||
            final_burst_delta                                                != expected_burst_delta ||
            object.value(QStringLiteral("viewport_final_top_line")).toInt() !=
                object.value(QStringLiteral("viewport_expected_top_line")).toInt()                   ||
            object.value(QStringLiteral("wheel_events_accepted_count")).toInt() !=
                object.value(QStringLiteral("iterations")).toInt() * wheel_burst_size                ||
            object.value(QStringLiteral("backend_writes_total")).toInt()     != 0                    ||
            object.value(QStringLiteral("backend_errors_total")).toInt()     != 0                    ||
            object.value(QStringLiteral("session_snapshots_observed")).toInt() !=
                object.value(QStringLiteral("iterations")).toInt())
        {
            *out_error = QStringLiteral("surface/session viewport counters failed: %1")
                .arg(object.value(QStringLiteral("name")).toString());
            return false;
        }
    }

    return true;
}

bool validate_requested_scenarios_json(
    const App_options& options,
    const QJsonArray&  scenarios,
    QString*           out_error)
{
    if (scenarios.size() != options.scenario_names.size()) {
        *out_error = QStringLiteral("scenario count does not match request");
        return false;
    }

    for (int index = 0; index < scenarios.size(); ++index) {
        const QJsonObject scenario     = scenarios[index].toObject();
        const QString     emitted_name = scenario.value(QStringLiteral("name")).toString();
        if (emitted_name != options.scenario_names[index]) {
            *out_error = QStringLiteral("scenario order does not match request at index %1")
                .arg(index);
            return false;
        }
    }

    return true;
}

bool validate_scenario_registry_json(const QJsonArray& registry, QString* out_error)
{
    const QStringList names = scenario_names();
    if (registry.size() != names.size()) {
        *out_error = QStringLiteral("scenario registry count does not match known scenarios");
        return false;
    }

    for (int index = 0; index < registry.size(); ++index) {
        if (!registry[index].isObject()) {
            *out_error = QStringLiteral("scenario registry entry is not an object");
            return false;
        }

        const QString     name     = names[index];
        const QJsonObject scenario = registry[index].toObject();
        if (scenario.value(QStringLiteral("name")).toString()              != name                          ||
            scenario.value(QStringLiteral("source_mode")).toString()       != scenario_source_mode(name)    ||
            scenario.value(QStringLiteral("execution_mode")).toString()    != scenario_execution_mode(name) ||
            scenario.value(QStringLiteral("latency_semantics")).toString() != k_latency_semantics)
        {
            *out_error = QStringLiteral("scenario registry metadata is inconsistent: %1")
                .arg(name);
            return false;
        }
    }

    return true;
}

QString root_source_mode(const App_options& options);
QString root_execution_mode(const App_options& options);

bool validate_json_output(
    const QByteArray&  json,
    const App_options& options,
    QString*           out_error)
{
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        *out_error = QStringLiteral("benchmark output is not valid JSON: %1")
            .arg(parse_error.errorString());
        return false;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("source_mode")).toString()     != root_source_mode(options)    ||
        root.value(QStringLiteral("execution_mode")).toString()  != root_execution_mode(options) ||
        !root.value(QStringLiteral("software_renderer")).isBool()                                ||
        root.value(QStringLiteral("software_renderer")).toBool() != options.software_renderer)
    {
        *out_error = QStringLiteral("benchmark root execution metadata does not match request");
        return false;
    }

    if (root.value(QStringLiteral("schema_version")).toInt() != k_schema_version) {
        *out_error = QStringLiteral("unsupported benchmark schema version");
        return false;
    }

    if (root.value(QStringLiteral("status")).toString() != QStringLiteral("ok")) {
        *out_error = QStringLiteral("benchmark status is not ok");
        return false;
    }

    if (root.contains(QStringLiteral("later_work"))) {
        *out_error = QStringLiteral("benchmark root contains stale later_work field");
        return false;
    }

    if (root.value(QStringLiteral("latency_semantics")).toString() !=
        k_latency_semantics ||
        root.value(QStringLiteral("elapsed_semantics")).toString() !=
        k_elapsed_semantics)
    {
        *out_error = QStringLiteral("benchmark timing semantics metadata changed");
        return false;
    }

    if (root.value(QStringLiteral("memory_sample_semantics")).toString() !=
        k_memory_sample_semantics ||
        root.value(QStringLiteral("memory_unsupported_semantics")).toString() !=
        k_memory_unsupported_semantics ||
        root.value(QStringLiteral("memory_sample_metric")).toString() !=
        QStringLiteral("resident_set_bytes") ||
        root.value(QStringLiteral("memory_sample_platform")).toString().isEmpty())
    {
        *out_error = QStringLiteral("benchmark memory metadata changed");
        return false;
    }

    if (!validate_profiling_metadata_json(root, options, out_error)) {
        return false;
    }

    if (root.value(QStringLiteral("iterations")).toInt() != options.iterations ||
        root.value(QStringLiteral("warmup")).toInt()     != options.warmup     ||
        root.value(QStringLiteral("rows")).toInt()       != options.grid.rows  ||
        root.value(QStringLiteral("columns")).toInt()    != options.grid.columns)
    {
        *out_error = QStringLiteral("benchmark root options do not match request");
        return false;
    }

    const QJsonObject root_window_size =
        root.value(QStringLiteral("window_size")).toObject();
    if (root_window_size.value(QStringLiteral("width")).toInt() !=
        options.window_size.width() ||
        root_window_size.value(QStringLiteral("height")).toInt() !=
        options.window_size.height())
    {
        *out_error = QStringLiteral("benchmark root window size does not match request");
        return false;
    }

    const QJsonArray registry = root.value(QStringLiteral("scenario_registry")).toArray();
    if (registry.isEmpty()) {
        *out_error = QStringLiteral("benchmark output contains no scenario registry");
        return false;
    }

    if (!validate_scenario_registry_json(registry, out_error)) {
        return false;
    }

    const QJsonArray scenarios = root.value(QStringLiteral("scenarios")).toArray();
    if (scenarios.isEmpty()) {
        *out_error = QStringLiteral("benchmark output contains no scenarios");
        return false;
    }

    if (!validate_requested_scenarios_json(options, scenarios, out_error)) {
        return false;
    }

    for (const QJsonValue& value : scenarios) {
        if (!value.isObject()) {
            *out_error = QStringLiteral("scenario entry is not an object");
            return false;
        }

        if (!validate_scenario_json(value.toObject(), options, out_error)) {
            return false;
        }

        if (options.profile &&
            !value.toObject().value(QStringLiteral("profile")).isObject())
        {
            *out_error = QStringLiteral("scenario profile data is missing");
            return false;
        }
    }

    return true;
}

QJsonArray scenario_registry_json()
{
    QJsonArray registry;
    for (const QString& name : scenario_names()) {
        QJsonObject scenario;
        scenario.insert(QStringLiteral("name"),              name);
        scenario.insert(QStringLiteral("source_mode"),       scenario_source_mode(name));
        scenario.insert(QStringLiteral("execution_mode"),    scenario_execution_mode(name));
        scenario.insert(QStringLiteral("latency_semantics"), k_latency_semantics);
        registry.push_back(scenario);
    }
    return registry;
}

QJsonObject make_profile_root_json(
    const App_options&                     options,
    const std::vector<Scenario_result>&    scenario_results,
    bool                                   ok,
    const QString&                         error = {})
{
    QJsonArray scenarios;
    for (const Scenario_result& result : scenario_results) {
        scenarios.push_back(scenario_profile_summary_json(result));
    }

    QJsonObject root;
    root.insert(QStringLiteral("schema_version"),    k_schema_version);
    root.insert(QStringLiteral("benchmark"),         QStringLiteral("vnm_terminal_embedded_benchmark"));
    root.insert(QStringLiteral("profile_format"),    QStringLiteral("hierarchical_profiler"));
    root.insert(QStringLiteral("software_renderer"), options.software_renderer);
    root.insert(QStringLiteral("profiling"),         profiling_metadata_json(options));
    root.insert(QStringLiteral("status"),            ok ? QStringLiteral("ok") : QStringLiteral("failed"));
    root.insert(QStringLiteral("scenarios"),         scenarios);
    if (!error.isEmpty()) {
        root.insert(QStringLiteral("error"), error);
    }
    return root;
}

bool validate_profile_json_output(
    const QByteArray&  json,
    const App_options& options,
    QString*           out_error)
{
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        *out_error = QStringLiteral("profile output is not valid JSON: %1")
            .arg(parse_error.errorString());
        return false;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema_version")).toInt()     != k_schema_version ||
        root.value(QStringLiteral("benchmark")).toString() !=
            QStringLiteral("vnm_terminal_embedded_benchmark")                        ||
        root.value(QStringLiteral("profile_format")).toString() !=
            QStringLiteral("hierarchical_profiler")                                  ||
        !root.value(QStringLiteral("software_renderer")).isBool()                    ||
        root.value(QStringLiteral("software_renderer")).toBool() != options.software_renderer)
    {
        *out_error = QStringLiteral("profile root metadata changed");
        return false;
    }

    if (!validate_profiling_metadata_json(root, options, out_error)) {
        return false;
    }

    const QJsonArray scenarios = root.value(QStringLiteral("scenarios")).toArray();
    if (root.value(QStringLiteral("status")).toString() == QStringLiteral("ok") &&
        scenarios.size()                                != options.scenario_names.size())
    {
        *out_error = QStringLiteral("profile scenario count does not match request");
        return false;
    }

    for (int index = 0; index < scenarios.size(); ++index) {
        if (!scenarios[index].isObject()) {
            *out_error = QStringLiteral("profile scenario entry is not an object");
            return false;
        }

        const QJsonObject scenario                 = scenarios[index].toObject();
        const QString     name                     = scenario.value(QStringLiteral("name")).toString();
        qint64            background_batched_rects = 0;
        qint64            selection_batched_rects  = 0;
        qint64            graphic_batched_rects    = 0;
        qint64            decoration_batched_rects = 0;
        if (!profile_nonnegative_integer_field(
                scenario, QStringLiteral("background_batched_rects"),
                QStringLiteral("profile.scenarios[%1]").arg(index), &background_batched_rects, out_error))
        {
            return false;
        }
        if (!profile_nonnegative_integer_field(
                scenario, QStringLiteral("selection_batched_rects"),
                QStringLiteral("profile.scenarios[%1]").arg(index), &selection_batched_rects, out_error))
        {
            return false;
        }
        if (!profile_nonnegative_integer_field(
                scenario, QStringLiteral("graphic_batched_rects"),
                QStringLiteral("profile.scenarios[%1]").arg(index), &graphic_batched_rects, out_error))
        {
            return false;
        }
        if (!profile_nonnegative_integer_field(
                scenario, QStringLiteral("decoration_batched_rects"),
                QStringLiteral("profile.scenarios[%1]").arg(index), &decoration_batched_rects, out_error))
        {
            return false;
        }

        if (!is_known_scenario(name) ||
            scenario.value(QStringLiteral("source_mode")).toString() != scenario_source_mode(name) ||
            scenario.value(QStringLiteral("execution_mode")).toString() !=
                scenario_execution_mode(name) ||
            !scenario.value(QStringLiteral("profile")).isObject())
        {
            *out_error = QStringLiteral("profile scenario metadata is inconsistent: %1")
                .arg(name);
            return false;
        }

        if (!validate_scenario_profile_value(
                scenario.value(QStringLiteral("profile")), QStringLiteral("profile.scenarios[%1]").arg(index),
                background_batched_rects > 0, selection_batched_rects > 0,
                graphic_batched_rects    > 0, decoration_batched_rects > 0, out_error))
        {
            return false;
        }
    }

    return true;
}

void append_profile_text_node(
    QString&                           text,
    const term::Profile_node_snapshot& node,
    int                                depth)
{
    const qint64 total_ns = profile_nanoseconds(node.total_time);
    const qint64 child_ns = profile_nanoseconds(node.child_time);
    const qint64 calls    = static_cast<qint64>(node.call_count);
    const QString indent(depth * 2, QLatin1Char(' '));
    text += QStringLiteral(
        "%1%2 calls=%3 total_ns=%4 self_ns=%5 child_ns=%6 min_ns=%7 max_ns=%8 mean_ns=%9\n")
        .arg(indent)
        .arg(QString::fromStdString(node.name))
        .arg(calls)
        .arg(total_ns)
        .arg(profile_nanoseconds(node.self_time))
        .arg(child_ns)
        .arg(profile_nanoseconds(node.min_time))
        .arg(profile_nanoseconds(node.max_time))
        .arg(calls > 0 ? total_ns / calls : 0);

    for (const term::Profile_node_snapshot& child : node.children) {
        append_profile_text_node(text, child, depth + 1);
    }
}

QString make_profile_text(const std::vector<Scenario_result>& scenario_results)
{
    QString text = QStringLiteral(
        "profile_text_format=%1\n"
        "vnm_terminal_embedded_benchmark profile\n");
    text = text.arg(k_profile_text_format);
    for (const Scenario_result& result : scenario_results) {
        text += QStringLiteral("\nscenario %1\n").arg(result.name);
        if (!result.profile_threads.empty()) {
            for (const profile_thread_snapshot_t& thread_snapshot : result.profile_threads) {
                text += QStringLiteral("  thread role=%1 index=%2\n")
                    .arg(thread_snapshot.role)
                    .arg(thread_snapshot.index);
                append_profile_text_node(text, thread_snapshot.root, 2);
            }
        }
        else {
            text += QStringLiteral("  profile unavailable\n");
        }
    }
    return text;
}

bool write_output_file(const QString& path, const QByteArray& json, QString* out_error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        *out_error = QStringLiteral("could not open output file: %1").arg(path);
        return false;
    }

    if (file.write(json) != json.size()) {
        *out_error = QStringLiteral("could not write output file: %1").arg(path);
        return false;
    }

    if (!file.commit()) {
        *out_error = QStringLiteral("could not commit output file: %1").arg(path);
        return false;
    }

    return true;
}

QString root_source_mode(const App_options& options)
{
    QString source_mode;
    for (const QString& scenario_name : options.scenario_names) {
        const QString scenario_mode = scenario_source_mode(scenario_name);
        if (source_mode.isEmpty()) {
            source_mode = scenario_mode;
            continue;
        }

        if (source_mode != scenario_mode) {
            return QStringLiteral("mixed");
        }
    }

    return source_mode.isEmpty() ? k_snapshot_bridge_source_mode : source_mode;
}

QString root_execution_mode(const App_options& options)
{
    QString execution_mode;
    for (const QString& scenario_name : options.scenario_names) {
        const QString scenario_mode = scenario_execution_mode(scenario_name);
        if (execution_mode.isEmpty()) {
            execution_mode = scenario_mode;
            continue;
        }

        if (execution_mode != scenario_mode) {
            return QStringLiteral("mixed");
        }
    }

    return execution_mode.isEmpty() ? k_snapshot_bridge_execution_mode : execution_mode;
}

QJsonObject make_root_json(
    const App_options& options,
    const QJsonArray&  scenarios,
    bool               ok,
    const QString&     error = {})
{
    QJsonObject window_size;
    window_size.insert(QStringLiteral("width"),  options.window_size.width());
    window_size.insert(QStringLiteral("height"), options.window_size.height());

    QJsonObject root;

    root.insert(QStringLiteral("schema_version"), k_schema_version);
    root.insert(QStringLiteral("benchmark"), QStringLiteral("vnm_terminal_embedded_benchmark"));
    root.insert(QStringLiteral("source_mode"), root_source_mode(options));
    root.insert(QStringLiteral("execution_mode"), root_execution_mode(options));
    root.insert(QStringLiteral("software_renderer"), options.software_renderer);
    root.insert(QStringLiteral("latency_semantics"), k_latency_semantics);
    root.insert(QStringLiteral("elapsed_semantics"), k_elapsed_semantics);
    root.insert(QStringLiteral("memory_sample_semantics"), k_memory_sample_semantics);
    root.insert(QStringLiteral("memory_unsupported_semantics"), k_memory_unsupported_semantics);
    root.insert(QStringLiteral("memory_sample_metric"), QStringLiteral("resident_set_bytes"));
    root.insert(QStringLiteral("memory_sample_platform"), process_memory_platform());
    root.insert(QStringLiteral("profiling"), profiling_metadata_json(options));
    root.insert(QStringLiteral("status"), ok ? QStringLiteral("ok") : QStringLiteral("failed"));
    root.insert(QStringLiteral("iterations"), options.iterations);
    root.insert(QStringLiteral("warmup"), options.warmup);
    root.insert(QStringLiteral("rows"), options.grid.rows);
    root.insert(QStringLiteral("columns"), options.grid.columns);
    root.insert(QStringLiteral("window_size"), window_size);
    root.insert(QStringLiteral("scenario_registry"), scenario_registry_json());
    root.insert(QStringLiteral("scenarios"), scenarios);
    if (!error.isEmpty()) {
        root.insert(QStringLiteral("error"), error);
    }
    return root;
}

int emit_json_and_status(
    const App_options&                     options,
    const QJsonObject&                     root,
    const std::vector<Scenario_result>&    scenario_results,
    bool                                   ok)
{
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);

    if (options.validate_json) {
        QString validation_error;
        if (!validate_json_output(json, options, &validation_error)) {
            std::cerr << "vnm_terminal_embedded_benchmark: "
                << validation_error.toUtf8().constData() << '\n';
            ok = false;
        }
    }

    if (!options.output_path.isEmpty()) {
        QString output_error;
        if (!write_output_file(options.output_path, json, &output_error)) {
            std::cerr << "vnm_terminal_embedded_benchmark: "
                << output_error.toUtf8().constData() << '\n';
            ok = false;
        }
    }

    if (!options.profile_json_path.isEmpty()) {
        const QJsonObject profile_root = make_profile_root_json(options, scenario_results, ok);
        const QByteArray profile_json =
            QJsonDocument(profile_root).toJson(QJsonDocument::Indented);

        if (options.validate_json) {
            QString validation_error;
            if (!validate_profile_json_output(profile_json, options, &validation_error)) {
                std::cerr << "vnm_terminal_embedded_benchmark: "
                    << validation_error.toUtf8().constData() << '\n';
                ok = false;
            }
        }

        QString output_error;
        if (!write_output_file(options.profile_json_path, profile_json, &output_error)) {
            std::cerr << "vnm_terminal_embedded_benchmark: "
                << output_error.toUtf8().constData() << '\n';
            ok = false;
        }
    }

    if (!options.profile_text_path.isEmpty()) {
        QString output_error;
        if (!write_output_file(
                options.profile_text_path, make_profile_text(scenario_results).toUtf8(), &output_error))
        {
            std::cerr << "vnm_terminal_embedded_benchmark: "
                << output_error.toUtf8().constData() << '\n';
            ok = false;
        }
    }

    if (!(options.quiet && !options.output_path.isEmpty())) {
        std::cout << json.constData();
    }

    return ok ? 0 : 1;
}

bool initialize_surface(Benchmark_context& context)
{
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        make_dense_repaint_snapshot({2, 8}, take_sequence(context), 0);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        context.surface,
        snapshot);
    QElapsedTimer elapsed_timer;
    elapsed_timer.start();
    return wait_for_render_completion(
        context,
        snapshot->metadata.sequence,
        elapsed_timer,
        false).completed;
}

bool window_uses_software_scene_graph(const QQuickWindow& window)
{
    const QSGRendererInterface* renderer_interface = window.rendererInterface();
    return
        renderer_interface                != nullptr &&
        renderer_interface->graphicsApi() == QSGRendererInterface::Software;
}

} // namespace

int main(int argc, char** argv)
{
    const QStringList  arguments    = raw_arguments(argc, argv);
    const Parse_result parse_result = parse_arguments(arguments);
    if (parse_result.options.help_requested) {
        print_usage();
        return 0;
    }

    if (parse_result.options.list_scenarios) {
        print_scenarios();
        return 0;
    }

    if (!parse_result.error.isEmpty()) {
        std::cerr << "vnm_terminal_embedded_benchmark: "
            << parse_result.error.toUtf8().constData() << '\n';
        print_usage();
        return 2;
    }

    App_options options = parse_result.options;
    QString precondition_error;
    if (!validate_high_water_scenario_preconditions(options, &precondition_error)) {
        std::cerr << "vnm_terminal_embedded_benchmark: "
            << precondition_error.toUtf8().constData() << '\n';
        return 1;
    }

    if (options.software_renderer) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }

    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("vnm_terminal_embedded_benchmark"));

    QQuickWindow window;
    window.setTitle(QStringLiteral("vnm_terminal embedded benchmark"));
    window.setColor(QColor(16, 20, 24));
    window.resize(options.window_size);

    VNM_TerminalSurface surface(window.contentItem());
    surface.set_font_family(term::vnm_terminal_default_monospace_font_family());
    surface.set_font_size(term::k_vnm_terminal_default_font_pixel_size);
    surface.setSize(QSizeF(
        static_cast<qreal>(options.window_size.width()),
        static_cast<qreal>(options.window_size.height())));
    (void)term::VNM_TerminalSurface_render_bridge::lifecycle_recorder(surface);

    QObject::connect(
        &window,
        &QQuickWindow::widthChanged,
        &surface,
        &QQuickItem::setWidth);
    QObject::connect(
        &window,
        &QQuickWindow::heightChanged,
        &surface,
        &QQuickItem::setHeight);

    window.show();

    Benchmark_context context{app, window, surface, 1000U, surface.scrollback_limit()};
    if (!initialize_surface(context)) {
        options.software_renderer = window_uses_software_scene_graph(window);
        const QJsonObject root = make_root_json(
            options,
            {},
            false,
            QStringLiteral("initial render did not complete"));
        return emit_json_and_status(options, root, {}, false);
    }
    options.software_renderer = window_uses_software_scene_graph(window);

    QJsonArray scenario_array;
    std::vector<Scenario_result> scenario_results;
    scenario_results.reserve(static_cast<std::size_t>(options.scenario_names.size()));
    bool ok = true;
    for (const QString& scenario_name : options.scenario_names) {
        const Scenario_result result = run_scenario(context, options, scenario_name);
        scenario_array.push_back(scenario_json(result));
        scenario_results.push_back(result);
        ok = ok && result.status == QStringLiteral("ok");
    }

    const QJsonObject root = make_root_json(options, scenario_array, ok);
    return emit_json_and_status(options, root, scenario_results, ok);
}
