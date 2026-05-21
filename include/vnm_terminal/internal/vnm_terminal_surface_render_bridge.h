#pragma once

#include "vnm_terminal/internal/backend_contract.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/qsg_terminal_renderer.h"
#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>

class VNM_TerminalSurface;

namespace vnm_terminal::internal {

struct Terminal_surface_render_invalidation_stats_t
{
    std::uint64_t                           update_requests                 = 0U;
    std::uint64_t                           scheduled_updates               = 0U;
    std::uint64_t                           coalesced_requests              = 0U;
    std::uint64_t                           consumed_updates                = 0U;
    std::uint64_t                           last_rendered_snapshot_sequence = 0U;
    bool                                    pending_update                  = false;
};

struct Render_profile_snapshot_t
{
    std::uint64_t                           sequence                        = 0U;
    Profile_node_snapshot                   root;
    Profile_timeline_snapshot               timeline;
#if VNM_TERMINAL_PROFILING_ENABLED
    terminal_text_layout_slow_diagnostics_t slow_text_layouts;
#endif
};

class VNM_TerminalSurface_render_bridge
{
public:
    // Internal test/render handoff only. Call on the surface GUI thread.
    static void set_render_snapshot(
        VNM_TerminalSurface&       surface,
        std::shared_ptr<const Terminal_render_snapshot>
                                   snapshot);

    static void set_render_profiler(
        VNM_TerminalSurface&                   surface,
        std::shared_ptr<Hierarchical_profiler> profiler);

    static Render_profile_snapshot_t render_profiler_snapshot(
        const VNM_TerminalSurface& surface);

    static void set_dirty_row_stats_enabled(
        VNM_TerminalSurface&       surface,
        bool                       enabled);

    static Terminal_screen_model_dirty_row_stats dirty_row_stats(
        const VNM_TerminalSurface& surface);

    static Terminal_screen_model_dirty_row_timeline dirty_row_timeline(
        const VNM_TerminalSurface& surface);

    static void set_cursor_blink_visible(
        VNM_TerminalSurface&       surface,
        bool                       visible);

    static bool start_process_with_backend(
        VNM_TerminalSurface&                   surface,
        std::unique_ptr<Terminal_backend>      backend,
        QStringList                            argv,
        QString                                working_directory = {});

    static bool backend_callback_drain_queued(
        const VNM_TerminalSurface& surface);

    static void drain_backend_callback_events(
        VNM_TerminalSurface&       surface);

    static std::shared_ptr<const Terminal_render_snapshot> render_snapshot(
        const VNM_TerminalSurface& surface);

    static Ime_preedit_state ime_preedit_state(
        const VNM_TerminalSurface& surface);

    static Terminal_surface_render_invalidation_stats_t invalidation_stats(
        const VNM_TerminalSurface& surface);

    static terminal_renderer_stats_t last_renderer_stats(
        const VNM_TerminalSurface& surface);

    static terminal_renderer_cumulative_stats_t cumulative_renderer_stats(
        const VNM_TerminalSurface& surface);

    static terminal_renderer_lifecycle_stats_t lifecycle_stats(
        const VNM_TerminalSurface& surface);

    static std::shared_ptr<Terminal_renderer_lifecycle_recorder> lifecycle_recorder(
        VNM_TerminalSurface&       surface);

    static void release_resources(
        VNM_TerminalSurface&       surface);

    static void simulate_scene_graph_invalidated(
        VNM_TerminalSurface&       surface);

    static void simulate_stale_scene_graph_invalidated(
        VNM_TerminalSurface&       surface);
};

}
