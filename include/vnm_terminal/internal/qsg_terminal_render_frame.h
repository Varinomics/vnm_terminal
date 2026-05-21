#pragma once

#include "vnm_terminal/internal/cell_stable_shaping.h"
#include "vnm_terminal/internal/render_snapshot.h"
#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vnm_terminal::internal {

enum class Terminal_render_decoration_kind
{
    UNDERLINE,
    STRIKE,
    PREEDIT_CARET,
    HYPERLINK_UNDERLINE,
};

enum class Terminal_render_arc_kind
{
    DOWN_RIGHT,
    DOWN_LEFT,
    UP_LEFT,
    UP_RIGHT,
};

struct Terminal_render_options
{
    QColor                     default_background   = QColor(9,   12,  16);
    QColor                     default_foreground   = QColor(196, 230, 201);
    QColor                     selection_background = QColor(48, 96, 160, 190);
    QColor                     cursor_color         = QColor(230, 240, 220);
    QColor                     preedit_background   = QColor(96,  96,  96,  120);
    QColor                     visual_bell_color    = QColor(255, 255, 255, 70);
    std::optional<Terminal_cursor_shape>
                               cursor_shape_override;
    std::optional<bool>        cursor_blink_enabled_override;
    bool                       visual_bell_enabled  = true;
    bool                       underline_hyperlinks = false;
};

struct Terminal_render_rect
{
    QRectF                     rect;
    QColor                     color;
    bool                       antialias            = false;
};

struct Terminal_render_arc
{
    Terminal_render_arc_kind   kind   = Terminal_render_arc_kind::DOWN_RIGHT;
    QRectF                     rect;
    QColor                     color;
    qreal                      stroke = 1.0;
};

struct Terminal_render_text_run
{
    int                        row          = 0;
    int                        logical_row  = 0;
    int                        column       = 0;
    QRectF                     rect;
    QRectF                     clip_rect;
    QPointF                    baseline_origin;
    QString                    text;
    QColor                     foreground;
    QColor                     background;
    Terminal_style_id          style_id     = k_default_terminal_style_id;
    std::uint64_t              hyperlink_id = 0U;
    bool                       underline    = false;
    bool                       strike       = false;
};

struct Terminal_render_decoration
{
    Terminal_render_decoration_kind kind          = Terminal_render_decoration_kind::UNDERLINE;
    QRectF                          rect;
    QColor                          color;
};

struct Terminal_render_cursor_primitive
{
    Terminal_cursor_shape      kind               = Terminal_cursor_shape::BLOCK;
    QRectF                     rect;
    QColor                     color;
};

struct terminal_packed_render_row_t
{
    Terminal_buffer_id         active_buffer      = Terminal_buffer_id::PRIMARY;
    int                        viewport_row       = 0;
    int                        logical_row        = 0;
    std::uint32_t              first_text_span    = 0U;
    std::uint32_t              text_span_count    = 0U;
    std::uint32_t              first_graphic_span = 0U;
    std::uint32_t              graphic_span_count = 0U;
};

struct terminal_packed_text_span_t
{
    int                        first_column    = 0;
    int                        column_count    = 0;
    Terminal_style_id          style_id        = k_default_terminal_style_id;
    std::uint32_t              foreground_rgba = 0U;
    std::uint32_t              background_rgba = 0U;
    std::uint32_t              text_offset     = 0U;
    std::uint32_t              text_length     = 0U;
};

struct terminal_packed_graphic_span_t
{
    int                        first_column     = 0;
    int                        column_count     = 0;
    Terminal_style_id          style_id         = k_default_terminal_style_id;
    std::uint32_t              foreground_rgba  = 0U;
    std::uint32_t              background_rgba  = 0U;
    std::uint32_t              codepoint_offset = 0U;
    std::uint32_t              codepoint_count  = 0U;
};

enum class Terminal_simple_content_text_category
{
    EMPTY,
    PRINTABLE_ASCII,
    OTHER_ASCII,
    NON_ASCII,
};

enum class Terminal_simple_content_route
{
    NONE,
    FAST_TEXT,
    QT_TEXT_LAYOUT,
    GRAPHIC_GEOMETRY,
    FALLBACK,
};

enum class Terminal_simple_content_rejection_reason
{
    NONE,
    EMPTY_TEXT,
    INVALID_GRID,
    INVALID_POSITION,
    INVALID_STYLE_ID,
    WIDE_CONTINUATION,
    INVALID_DISPLAY_WIDTH,
    INVALID_TEXT_ENCODING,
    INVALID_TEXT_WIDTH,
    MULTI_CELL_TEXT,
    NON_PRINTABLE_ASCII,
    NON_ASCII_TEXT,
    DECORATION,
    HYPERLINK,
    TERMINAL_GRAPHIC,
};

struct terminal_simple_content_classification_t
{
    Terminal_simple_content_text_category    text_category =
        Terminal_simple_content_text_category::EMPTY;
    Terminal_simple_content_route            route =
        Terminal_simple_content_route::NONE;
    Terminal_simple_content_rejection_reason rejection_reason =
        Terminal_simple_content_rejection_reason::EMPTY_TEXT;
    int                row                 = 0;
    Terminal_style_id  style_id            = k_default_terminal_style_id;
    bool               dirty_row           = false;
    bool               valid_terminal_cell = false;
    bool               fast_text_eligible  = false;
};

struct terminal_simple_content_stats_t
{
    int                cells_considered                      = 0;
    int                eligible_cells                        = 0;
    int                eligible_after_all_gates_cells        = 0;
    int                rows_with_eligible_cells              = 0;
    int                styles_with_eligible_cells            = 0;
    int                dirty_eligible_cells                  = 0;
    int                clean_eligible_cells                  = 0;
    int                text_category_empty_cells             = 0;
    int                text_category_printable_ascii_cells   = 0;
    int                text_category_other_ascii_cells       = 0;
    int                text_category_non_ascii_cells         = 0;
    int                route_none_cells                      = 0;
    int                route_fast_text_cells                 = 0;
    int                route_qt_text_layout_cells            = 0;
    int                route_graphic_geometry_cells          = 0;
    int                route_fallback_cells                  = 0;
    int                rejection_none_cells                  = 0;
    int                rejection_empty_text_cells            = 0;
    int                rejection_invalid_grid_cells          = 0;
    int                rejection_invalid_position_cells      = 0;
    int                rejection_invalid_style_id_cells      = 0;
    int                rejection_wide_continuation_cells     = 0;
    int                rejection_invalid_display_width_cells = 0;
    int                rejection_invalid_text_encoding_cells = 0;
    int                rejection_invalid_text_width_cells    = 0;
    int                rejection_multi_cell_text_cells       = 0;
    int                rejection_non_printable_ascii_cells   = 0;
    int                rejection_non_ascii_text_cells        = 0;
    int                rejection_decoration_cells            = 0;
    int                rejection_hyperlink_cells             = 0;
    int                rejection_terminal_graphic_cells      = 0;
};

struct terminal_simple_content_cumulative_stats_t
{
    std::uint64_t      cells_considered                      = 0U;
    std::uint64_t      eligible_cells                        = 0U;
    std::uint64_t      eligible_after_all_gates_cells        = 0U;
    std::uint64_t      rows_with_eligible_cells              = 0U;
    std::uint64_t      styles_with_eligible_cells            = 0U;
    std::uint64_t      dirty_eligible_cells                  = 0U;
    std::uint64_t      clean_eligible_cells                  = 0U;
    std::uint64_t      text_category_empty_cells             = 0U;
    std::uint64_t      text_category_printable_ascii_cells   = 0U;
    std::uint64_t      text_category_other_ascii_cells       = 0U;
    std::uint64_t      text_category_non_ascii_cells         = 0U;
    std::uint64_t      route_none_cells                      = 0U;
    std::uint64_t      route_fast_text_cells                 = 0U;
    std::uint64_t      route_qt_text_layout_cells            = 0U;
    std::uint64_t      route_graphic_geometry_cells          = 0U;
    std::uint64_t      route_fallback_cells                  = 0U;
    std::uint64_t      rejection_none_cells                  = 0U;
    std::uint64_t      rejection_empty_text_cells            = 0U;
    std::uint64_t      rejection_invalid_grid_cells          = 0U;
    std::uint64_t      rejection_invalid_position_cells      = 0U;
    std::uint64_t      rejection_invalid_style_id_cells      = 0U;
    std::uint64_t      rejection_wide_continuation_cells     = 0U;
    std::uint64_t      rejection_invalid_display_width_cells = 0U;
    std::uint64_t      rejection_invalid_text_encoding_cells = 0U;
    std::uint64_t      rejection_invalid_text_width_cells    = 0U;
    std::uint64_t      rejection_multi_cell_text_cells       = 0U;
    std::uint64_t      rejection_non_printable_ascii_cells   = 0U;
    std::uint64_t      rejection_non_ascii_text_cells        = 0U;
    std::uint64_t      rejection_decoration_cells            = 0U;
    std::uint64_t      rejection_hyperlink_cells             = 0U;
    std::uint64_t      rejection_terminal_graphic_cells      = 0U;
};

terminal_simple_content_classification_t classify_terminal_simple_content_cell(
    const Terminal_render_cell&        cell,
    terminal_grid_size_t               grid_size,
    std::size_t                        style_count,
    bool                               has_decoration,
    bool                               dirty_row,
    Terminal_shaped_presentation_mode  presentation_mode = Terminal_shaped_presentation_mode::DEFAULT_TEXT);

struct terminal_render_frame_stats_t
{
    terminal_simple_content_stats_t                simple_content;
    int                                            cells_considered                = 0;
    int                                            cells_skipped_invalid           = 0;
    int                                            cells_skipped_wide_continuation = 0;
    int                                            cells_rendered                  = 0;
    int                                            text_cells_empty                = 0;
    int                                            text_cells_rendered_as_text     = 0;
    int                                            text_cells_rendered_as_graphic  = 0;
    int                                            text_cells_printable_ascii      = 0;
    int                                            text_cells_other_ascii          = 0;
    int                                            text_cells_non_ascii            = 0;
    int                                            text_cells_simple_ascii         = 0;
    int                                            text_cells_single_width         = 0;
    int                                            text_cells_multi_width          = 0;
    int                                            text_cells_with_decorations     = 0;
    int                                            text_cells_with_hyperlink       = 0;
    int                                            text_style_changes              = 0;
    int                                            text_distinct_styles            = 0;
    int                                            background_rects_emitted        = 0;
    int                                            selection_rects_emitted         = 0;
    int                                            graphic_rects_emitted           = 0;
    int                                            graphic_arcs_emitted            = 0;
    int                                            text_runs_emitted               = 0;
    int                                            cursor_text_runs_emitted        = 0;
    int                                            decoration_rects_emitted        = 0;
    int                                            cursor_rects_emitted            = 0;
    int                                            cursor_graphic_rects_emitted    = 0;
    int                                            cursor_graphic_arcs_emitted     = 0;
    int                                            overlay_rects_emitted           = 0;
    int                                            packed_rows                     = 0;
    int                                            packed_text_spans               = 0;
    int                                            packed_text_cells               = 0;
    int                                            packed_graphic_spans            = 0;
    int                                            packed_graphic_cells            = 0;
    std::uint64_t                                  packed_payload_bytes            = 0U;
};

struct terminal_render_frame_cumulative_stats_t
{
    terminal_simple_content_cumulative_stats_t     simple_content;
    std::uint64_t                                  cells_considered                = 0U;
    std::uint64_t                                  cells_skipped_invalid           = 0U;
    std::uint64_t                                  cells_skipped_wide_continuation = 0U;
    std::uint64_t                                  cells_rendered                  = 0U;
    std::uint64_t                                  text_cells_empty                = 0U;
    std::uint64_t                                  text_cells_rendered_as_text     = 0U;
    std::uint64_t                                  text_cells_rendered_as_graphic  = 0U;
    std::uint64_t                                  text_cells_printable_ascii      = 0U;
    std::uint64_t                                  text_cells_other_ascii          = 0U;
    std::uint64_t                                  text_cells_non_ascii            = 0U;
    std::uint64_t                                  text_cells_simple_ascii         = 0U;
    std::uint64_t                                  text_cells_single_width         = 0U;
    std::uint64_t                                  text_cells_multi_width          = 0U;
    std::uint64_t                                  text_cells_with_decorations     = 0U;
    std::uint64_t                                  text_cells_with_hyperlink       = 0U;
    std::uint64_t                                  text_style_changes              = 0U;
    std::uint64_t                                  text_distinct_styles            = 0U;
    std::uint64_t                                  background_rects_emitted        = 0U;
    std::uint64_t                                  selection_rects_emitted         = 0U;
    std::uint64_t                                  graphic_rects_emitted           = 0U;
    std::uint64_t                                  graphic_arcs_emitted            = 0U;
    std::uint64_t                                  text_runs_emitted               = 0U;
    std::uint64_t                                  cursor_text_runs_emitted        = 0U;
    std::uint64_t                                  decoration_rects_emitted        = 0U;
    std::uint64_t                                  cursor_rects_emitted            = 0U;
    std::uint64_t                                  cursor_graphic_rects_emitted    = 0U;
    std::uint64_t                                  cursor_graphic_arcs_emitted     = 0U;
    std::uint64_t                                  overlay_rects_emitted           = 0U;
    std::uint64_t                                  packed_rows                     = 0U;
    std::uint64_t                                  packed_text_spans               = 0U;
    std::uint64_t                                  packed_text_cells               = 0U;
    std::uint64_t                                  packed_graphic_spans            = 0U;
    std::uint64_t                                  packed_graphic_cells            = 0U;
    std::uint64_t                                  packed_payload_bytes            = 0U;
};

struct Terminal_render_frame
{
    QSizeF                                         logical_size;
    terminal_grid_size_t                           grid_size;
    Terminal_viewport_state                        viewport;
    terminal_cell_metrics_t                        cell_metrics;
    std::vector<Terminal_render_rect>              background_rects;
    std::vector<Terminal_render_rect>              selection_rects;
    std::vector<Terminal_render_rect>              graphic_rects;
    std::vector<Terminal_render_arc>               graphic_arcs;
    std::vector<Terminal_render_text_run>          text_runs;
    std::vector<Terminal_render_text_run>          cursor_text_runs;
    std::vector<Terminal_render_decoration>        decorations;
    std::vector<Terminal_render_cursor_primitive>  cursors;
    std::vector<Terminal_render_rect>              cursor_graphic_rects;
    std::vector<Terminal_render_arc>               cursor_graphic_arcs;
    std::vector<Terminal_render_rect>              overlay_rects;
    std::vector<Terminal_render_dirty_row_range>   dirty_row_ranges;
    std::vector<terminal_packed_render_row_t>      packed_rows;
    std::vector<terminal_packed_text_span_t>       packed_text_spans;
    std::vector<char>                              packed_text_bytes;
    std::vector<terminal_packed_graphic_span_t>    packed_graphic_spans;
    std::vector<std::uint32_t>                     packed_graphic_codepoints;
    terminal_render_frame_stats_t                  stats;
};

Terminal_render_frame build_terminal_render_frame(
    const Terminal_render_snapshot*    snapshot,
    QSizeF                             logical_size,
    terminal_cell_metrics_t            cell_metrics,
    const Terminal_render_options&     options,
    bool                               cursor_blink_visible,
    const Ime_preedit_state*           ime_preedit_override = nullptr);

}
