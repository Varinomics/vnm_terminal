#pragma once

#include "vnm_terminal/internal/qsg_terminal_render_frame.h"
#include <QFont>
#include <QString>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#if !defined(VNM_TERMINAL_PROFILING_ENABLED)
#define VNM_TERMINAL_PROFILING_ENABLED 0
#endif

class QQuickWindow;
class QSGNode;

namespace vnm_terminal::internal {

struct Terminal_render_text_run;

#if VNM_TERMINAL_PROFILING_ENABLED
struct terminal_text_layout_slow_diagnostic_t
{
    std::uint64_t  duration_ns                = 0U;
    std::uint64_t  text_hash                  = 0U;
    int            text_utf16_units           = 0;
    int            text_codepoints            = 0;
    int            row                        = 0;
    int            logical_row                = 0;
    int            column                     = 0;
    int            style_id                   = 0;
    std::uint64_t  hyperlink_id               = 0U;
    qreal          rect_width                 = 0.0;
    qreal          rect_height                = 0.0;
    qreal          font_point_size            = 0.0;
    int            font_pixel_size            = 0;
    int            font_weight                = 0;
    bool           ascii_only                 = false;
    bool           printable_ascii_only       = false;
    bool           has_control_codepoint      = false;
    bool           clipped                    = false;
    bool           force_blended_order        = false;
    bool           ascii_layout_font          = false;
    bool           line_has_text              = false;
    bool           text_preview_truncated     = false;
    bool           font_italic                = false;
    QString        font_family;
    QString        font_style_name;
    QString        resolved_font_family;
    QString        resolved_font_style_name;
    QString        text_preview;
    QString        codepoint_sample;
};

struct terminal_text_layout_slow_diagnostics_t
{
    std::uint64_t  threshold_ns    = 0U;
    std::uint64_t  slow_call_count = 0U;
    std::vector<terminal_text_layout_slow_diagnostic_t>
                   samples;
};

class Terminal_text_layout_slow_diagnostics_recorder final
{
public:
    void reset();

    void record_layout(
        std::uint64_t                      duration_ns,
        const QFont&                       font,
        const Terminal_render_text_run&    run,
        bool                               clipped,
        bool                               force_blended_order,
        bool                               ascii_layout_font,
        bool                               line_has_text);

    terminal_text_layout_slow_diagnostics_t snapshot() const;

private:
    mutable std::mutex                                  m_mutex;
    std::uint64_t                                       m_slow_call_count = 0U;
    std::vector<terminal_text_layout_slow_diagnostic_t> m_samples;
};
#endif

struct terminal_renderer_stats_t
{
    bool           paint_completed                                      = false;
    terminal_render_frame_stats_t
                   frame;
    int            text_content_rebuilds                                = 0;
    int            text_content_reused                                  = 0;
    int            text_content_removed                                 = 0;
    int            text_content_failures                                = 0;
    int            text_leaf_nodes_created                              = 0;
    int            text_cache_entry_child_nodes_cleared_for_replacement = 0;
    int            text_cache_entry_child_nodes_cleared_for_removal     = 0;
    int            text_cache_entry_max_child_nodes_cleared             = 0;
    int            route_fast_text_cells                                = 0;
    int            route_qt_text_layout_runs                            = 0;
    int            route_graphic_geometry_cells                         = 0;
    int            route_fallback_cells                                 = 0;
    int            qt_text_layout_calls                                 = 0;
    int            qsg_nodes_created                                    = 0;
    int            qsg_nodes_replaced                                   = 0;
    int            qsg_nodes_destroyed                                  = 0;
    int            background_qsg_nodes_created                         = 0;
    int            background_qsg_nodes_replaced                        = 0;
    int            background_qsg_nodes_destroyed                       = 0;
    int            text_groups_considered                               = 0;
    int            text_groups_dirty                                    = 0;
    int            text_groups_clean                                    = 0;
    int            text_clean_reuse_skips                               = 0;
    int            text_resource_descriptor_reuses                      = 0;
    int            text_key_builds                                      = 0;
    std::uint64_t  text_key_bytes                                       = 0U;
    int            rect_key_builds                                      = 0;
    std::uint64_t  rect_key_bytes                                       = 0U;
    int            cache_key_builds                                     = 0;
    std::uint64_t  cache_key_bytes                                      = 0U;
    int            text_dirty_row_ranges                                = 0;
    int            text_dirty_rows                                      = 0;
    int            text_runs_considered                                 = 0;
    int            text_coalescing_candidate_groups                     = 0;
    int            text_coalescing_enabled_groups                       = 0;
    int            text_resource_runs_before_coalescing                 = 0;
    int            text_resource_runs_after_coalescing                  = 0;
    int            rect_resource_rects_before_coalescing                = 0;
    int            rect_resource_rects_after_coalescing                 = 0;
    int            background_row_rects_before_coalescing               = 0;
    int            background_row_rects_after_coalescing                = 0;
    int            background_batched_rects                             = 0;
    int            background_batched_vertices                          = 0;
    int            selection_batched_rects                              = 0;
    int            selection_batched_vertices                           = 0;
    int            graphic_batched_rects                                = 0;
    int            graphic_batched_vertices                             = 0;
    int            decoration_batched_rects                             = 0;
    int            decoration_batched_vertices                          = 0;
    int            text_cache_entries_created                           = 0;
    int            text_cache_entries_replaced                          = 0;
    int            text_cache_entries_removed                           = 0;
    int            frame_background_rects                               = 0;
    int            frame_selection_rects                                = 0;
    int            frame_graphic_rects                                  = 0;
    int            frame_graphic_arcs                                   = 0;
    int            frame_text_runs                                      = 0;
    int            frame_cursor_text_runs                               = 0;
    int            frame_decorations                                    = 0;
    int            frame_cursors                                        = 0;
    int            frame_cursor_graphic_rects                           = 0;
    int            frame_cursor_graphic_arcs                            = 0;
    int            frame_overlay_rects                                  = 0;
    int            frame_dirty_row_ranges                               = 0;
    int            frame_packed_rows                                    = 0;
    int            frame_packed_text_spans                              = 0;
    int            frame_packed_text_cells                              = 0;
    int            frame_packed_graphic_spans                           = 0;
    int            frame_packed_graphic_cells                           = 0;
    std::uint64_t  frame_packed_payload_bytes                           = 0U;
    int            row_cache_hits                                       = 0;
    int            row_cache_clean_skips                                = 0;
    bool           text_wrapper_order_rebuilt                           = false;
    bool           background_layer_rebuilt                             = false;
    bool           selection_layer_rebuilt                              = false;
    bool           graphic_layer_rebuilt                                = false;
    bool           decoration_layer_rebuilt                             = false;
    bool           cursor_layer_rebuilt                                 = false;
    bool           cursor_graphic_layer_rebuilt                         = false;
    bool           cursor_text_layer_rebuilt                            = false;
    bool           overlay_layer_rebuilt                                = false;
    int            background_rows_rebuilt                              = 0;
    int            background_rows_reused                               = 0;
    int            background_row_clean_reuse_skips                     = 0;
    int            background_rows_removed                              = 0;
    int            background_row_cache_fallbacks                       = 0;
    int            selection_rows_rebuilt                               = 0;
    int            selection_rows_reused                                = 0;
    int            selection_row_clean_reuse_skips                      = 0;
    int            selection_rows_removed                               = 0;
    int            selection_row_cache_fallbacks                        = 0;
    int            decoration_rows_rebuilt                              = 0;
    int            decoration_rows_reused                               = 0;
    int            decoration_row_clean_reuse_skips                     = 0;
    int            decoration_rows_removed                              = 0;
    int            decoration_row_cache_fallbacks                       = 0;
    int            graphic_rect_rows_rebuilt                            = 0;
    int            graphic_rect_rows_reused                             = 0;
    int            graphic_rect_row_clean_reuse_skips                   = 0;
    int            graphic_rect_rows_removed                            = 0;
    int            graphic_rect_row_cache_fallbacks                     = 0;
    int            graphic_arc_rows_rebuilt                             = 0;
    int            graphic_arc_rows_reused                              = 0;
    int            graphic_arc_row_clean_reuse_skips                    = 0;
    int            graphic_arc_rows_removed                             = 0;
    int            graphic_arc_row_cache_fallbacks                      = 0;
    bool           root_reused                                          = false;
};

struct terminal_renderer_cumulative_stats_t
{
    std::uint64_t  frames_published                                     = 0U;
    std::uint64_t  paint_completed_frames                               = 0U;
    std::uint64_t  root_reused_frames                                   = 0U;
    terminal_render_frame_cumulative_stats_t
                   frame;
    std::uint64_t  text_content_rebuilds                                = 0U;
    std::uint64_t  text_content_reused                                  = 0U;
    std::uint64_t  text_content_removed                                 = 0U;
    std::uint64_t  text_content_failures                                = 0U;
    std::uint64_t  text_leaf_nodes_created                              = 0U;
    std::uint64_t  text_cache_entry_child_nodes_cleared_for_replacement = 0U;
    std::uint64_t  text_cache_entry_child_nodes_cleared_for_removal     = 0U;
    std::uint64_t  text_cache_entry_max_child_nodes_cleared             = 0U;
    std::uint64_t  route_fast_text_cells                                = 0U;
    std::uint64_t  route_qt_text_layout_runs                            = 0U;
    std::uint64_t  route_graphic_geometry_cells                         = 0U;
    std::uint64_t  route_fallback_cells                                 = 0U;
    std::uint64_t  qt_text_layout_calls                                 = 0U;
    std::uint64_t  qsg_nodes_created                                    = 0U;
    std::uint64_t  qsg_nodes_replaced                                   = 0U;
    std::uint64_t  qsg_nodes_destroyed                                  = 0U;
    std::uint64_t  background_qsg_nodes_created                         = 0U;
    std::uint64_t  background_qsg_nodes_replaced                        = 0U;
    std::uint64_t  background_qsg_nodes_destroyed                       = 0U;
    std::uint64_t  text_groups_considered                               = 0U;
    std::uint64_t  text_groups_dirty                                    = 0U;
    std::uint64_t  text_groups_clean                                    = 0U;
    std::uint64_t  text_clean_reuse_skips                               = 0U;
    std::uint64_t  text_resource_descriptor_reuses                      = 0U;
    std::uint64_t  text_key_builds                                      = 0U;
    std::uint64_t  text_key_bytes                                       = 0U;
    std::uint64_t  rect_key_builds                                      = 0U;
    std::uint64_t  rect_key_bytes                                       = 0U;
    std::uint64_t  cache_key_builds                                     = 0U;
    std::uint64_t  cache_key_bytes                                      = 0U;
    std::uint64_t  text_dirty_row_ranges                                = 0U;
    std::uint64_t  text_dirty_rows                                      = 0U;
    std::uint64_t  text_runs_considered                                 = 0U;
    std::uint64_t  text_coalescing_candidate_groups                     = 0U;
    std::uint64_t  text_coalescing_enabled_groups                       = 0U;
    std::uint64_t  text_resource_runs_before_coalescing                 = 0U;
    std::uint64_t  text_resource_runs_after_coalescing                  = 0U;
    std::uint64_t  rect_resource_rects_before_coalescing                = 0U;
    std::uint64_t  rect_resource_rects_after_coalescing                 = 0U;
    std::uint64_t  background_row_rects_before_coalescing               = 0U;
    std::uint64_t  background_row_rects_after_coalescing                = 0U;
    std::uint64_t  background_batched_rects                             = 0U;
    std::uint64_t  background_batched_vertices                          = 0U;
    std::uint64_t  selection_batched_rects                              = 0U;
    std::uint64_t  selection_batched_vertices                           = 0U;
    std::uint64_t  graphic_batched_rects                                = 0U;
    std::uint64_t  graphic_batched_vertices                             = 0U;
    std::uint64_t  decoration_batched_rects                             = 0U;
    std::uint64_t  decoration_batched_vertices                          = 0U;
    std::uint64_t  text_cache_entries_created                           = 0U;
    std::uint64_t  text_cache_entries_replaced                          = 0U;
    std::uint64_t  text_cache_entries_removed                           = 0U;
    std::uint64_t  frame_background_rects                               = 0U;
    std::uint64_t  frame_selection_rects                                = 0U;
    std::uint64_t  frame_graphic_rects                                  = 0U;
    std::uint64_t  frame_graphic_arcs                                   = 0U;
    std::uint64_t  frame_text_runs                                      = 0U;
    std::uint64_t  frame_cursor_text_runs                               = 0U;
    std::uint64_t  frame_decorations                                    = 0U;
    std::uint64_t  frame_cursors                                        = 0U;
    std::uint64_t  frame_cursor_graphic_rects                           = 0U;
    std::uint64_t  frame_cursor_graphic_arcs                            = 0U;
    std::uint64_t  frame_overlay_rects                                  = 0U;
    std::uint64_t  frame_dirty_row_ranges                               = 0U;
    std::uint64_t  frame_packed_rows                                    = 0U;
    std::uint64_t  frame_packed_text_spans                              = 0U;
    std::uint64_t  frame_packed_text_cells                              = 0U;
    std::uint64_t  frame_packed_graphic_spans                           = 0U;
    std::uint64_t  frame_packed_graphic_cells                           = 0U;
    std::uint64_t  frame_packed_payload_bytes                           = 0U;
    std::uint64_t  row_cache_hits                                       = 0U;
    std::uint64_t  row_cache_clean_skips                                = 0U;
    std::uint64_t  text_wrapper_order_rebuilds                          = 0U;
    std::uint64_t  background_layer_rebuilds                            = 0U;
    std::uint64_t  selection_layer_rebuilds                             = 0U;
    std::uint64_t  graphic_layer_rebuilds                               = 0U;
    std::uint64_t  decoration_layer_rebuilds                            = 0U;
    std::uint64_t  cursor_layer_rebuilds                                = 0U;
    std::uint64_t  cursor_graphic_layer_rebuilds                        = 0U;
    std::uint64_t  cursor_text_layer_rebuilds                           = 0U;
    std::uint64_t  overlay_layer_rebuilds                               = 0U;
    std::uint64_t  background_rows_rebuilt                              = 0U;
    std::uint64_t  background_rows_reused                               = 0U;
    std::uint64_t  background_row_clean_reuse_skips                     = 0U;
    std::uint64_t  background_rows_removed                              = 0U;
    std::uint64_t  background_row_cache_fallbacks                       = 0U;
    std::uint64_t  selection_rows_rebuilt                               = 0U;
    std::uint64_t  selection_rows_reused                                = 0U;
    std::uint64_t  selection_row_clean_reuse_skips                      = 0U;
    std::uint64_t  selection_rows_removed                               = 0U;
    std::uint64_t  selection_row_cache_fallbacks                        = 0U;
    std::uint64_t  decoration_rows_rebuilt                              = 0U;
    std::uint64_t  decoration_rows_reused                               = 0U;
    std::uint64_t  decoration_row_clean_reuse_skips                     = 0U;
    std::uint64_t  decoration_rows_removed                              = 0U;
    std::uint64_t  decoration_row_cache_fallbacks                       = 0U;
    std::uint64_t  graphic_rect_rows_rebuilt                            = 0U;
    std::uint64_t  graphic_rect_rows_reused                             = 0U;
    std::uint64_t  graphic_rect_row_clean_reuse_skips                   = 0U;
    std::uint64_t  graphic_rect_rows_removed                            = 0U;
    std::uint64_t  graphic_rect_row_cache_fallbacks                     = 0U;
    std::uint64_t  graphic_arc_rows_rebuilt                             = 0U;
    std::uint64_t  graphic_arc_rows_reused                              = 0U;
    std::uint64_t  graphic_arc_row_clean_reuse_skips                    = 0U;
    std::uint64_t  graphic_arc_rows_removed                             = 0U;
    std::uint64_t  graphic_arc_row_cache_fallbacks                      = 0U;
};

class Terminal_renderer_stats_publisher final
{
public:
    // publish() runs from the scene graph update path; snapshot() is used by
    // GUI-thread diagnostics/tests and must remain safe across that boundary.
    void publish(const terminal_renderer_stats_t& stats);
    terminal_renderer_stats_t snapshot() const;
    terminal_renderer_cumulative_stats_t cumulative_snapshot() const;

private:
    mutable std::mutex         m_mutex;
    terminal_renderer_stats_t  m_stats;
    terminal_renderer_cumulative_stats_t
                               m_cumulative_stats;
};

struct terminal_renderer_lifecycle_stats_t
{
    std::uint64_t release_resources_calls              = 0U;
    std::uint64_t item_scene_changes                   = 0U;
    std::uint64_t item_scene_detaches                  = 0U;
    std::uint64_t item_destructions                    = 0U;
    std::uint64_t scene_graph_invalidated_calls        = 0U;
    std::uint64_t render_node_deletions_in_paint       = 0U;
    std::uint64_t render_root_nodes_created            = 0U;
    std::uint64_t render_root_nodes_destroyed          = 0U;
    std::uint64_t render_text_resources_created        = 0U;
    std::uint64_t render_text_resources_destroyed      = 0U;
    std::uint64_t render_rect_resources_created        = 0U;
    std::uint64_t render_rect_resources_destroyed      = 0U;
};

class Terminal_renderer_lifecycle_recorder final
{
public:
    void record_release_resources()
    {
        increment(&terminal_renderer_lifecycle_stats_t::release_resources_calls);
    }

    void record_item_scene_change()
    {
        increment(&terminal_renderer_lifecycle_stats_t::item_scene_changes);
    }

    void record_item_scene_detach()
    {
        increment(&terminal_renderer_lifecycle_stats_t::item_scene_detaches);
    }

    void record_item_destruction()
    {
        increment(&terminal_renderer_lifecycle_stats_t::item_destructions);
    }

    void record_scene_graph_invalidated()
    {
        increment(&terminal_renderer_lifecycle_stats_t::scene_graph_invalidated_calls);
    }

    void record_render_node_deleted()
    {
        increment(&terminal_renderer_lifecycle_stats_t::render_node_deletions_in_paint);
    }

    void record_root_node_created()
    {
        increment(&terminal_renderer_lifecycle_stats_t::render_root_nodes_created);
    }

    void record_root_node_destroyed()
    {
        increment(&terminal_renderer_lifecycle_stats_t::render_root_nodes_destroyed);
    }

    void record_text_resource_created()
    {
        increment(&terminal_renderer_lifecycle_stats_t::render_text_resources_created);
    }

    void record_text_resource_destroyed()
    {
        increment(&terminal_renderer_lifecycle_stats_t::render_text_resources_destroyed);
    }

    void record_rect_resource_created()
    {
        increment(&terminal_renderer_lifecycle_stats_t::render_rect_resources_created);
    }

    void record_rect_resource_destroyed()
    {
        increment(&terminal_renderer_lifecycle_stats_t::render_rect_resources_destroyed);
    }

    terminal_renderer_lifecycle_stats_t snapshot() const;

private:
    void increment(std::uint64_t terminal_renderer_lifecycle_stats_t::* counter)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        ++(m_stats.*counter);
    }

    mutable std::mutex                    m_mutex;
    terminal_renderer_lifecycle_stats_t   m_stats;
};

class Qsg_terminal_renderer final
{
public:
#if VNM_TERMINAL_PROFILING_ENABLED
    QSGNode* update_node(
        QSGNode*                   old_node,
        QQuickWindow*              window,
        const Terminal_render_frame&
                                   frame,
        const QFont&               font,
        qreal                      device_pixel_ratio,
        std::shared_ptr<Terminal_renderer_lifecycle_recorder>
                                   lifecycle_recorder,
        terminal_renderer_stats_t& out_stats,
        std::shared_ptr<Terminal_text_layout_slow_diagnostics_recorder>
                                   slow_text_layout_recorder = {});
#else
    QSGNode* update_node(
        QSGNode*                   old_node,
        QQuickWindow*              window,
        const Terminal_render_frame&
                                   frame,
        const QFont&               font,
        qreal                      device_pixel_ratio,
        std::shared_ptr<Terminal_renderer_lifecycle_recorder>
                                   lifecycle_recorder,
        terminal_renderer_stats_t& out_stats);
#endif
};

}
