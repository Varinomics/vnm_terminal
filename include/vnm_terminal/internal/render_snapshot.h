#pragma once

#include "vnm_terminal/internal/ime_contract.h"
#include "vnm_terminal/internal/metrics_contract.h"
#include "vnm_terminal/internal/selection_contract.h"
#include "vnm_terminal/internal/terminal_style.h"
#include "vnm_terminal/internal/viewport_contract.h"
#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <algorithm>
#include <set>
#include <vector>
#include <cstdint>

namespace vnm_terminal::internal {

enum class Terminal_cursor_shape
{
    BLOCK,
    BAR,
    UNDERLINE,
};

enum class Terminal_mouse_tracking_mode
{
    NONE,
    BUTTON,
    DRAG,
    ANY,
};

struct Terminal_mode_state
{
    bool                         application_cursor_keys = false;
    bool                         reverse_video           = false;
    bool                         origin_mode             = false;
    bool                         autowrap                = true;
    bool                         cursor_visible          = true;
    Terminal_mouse_tracking_mode mouse_tracking          = Terminal_mouse_tracking_mode::NONE;
    bool                         focus_reporting         = false;
    bool                         sgr_mouse_encoding      = false;
    bool                         alternate_scroll        = false;
    bool                         bracketed_paste         = false;
    bool                         synchronized_output     = false;
};

enum class Terminal_render_snapshot_status
{
    OK,
    INVALID_GRID_SIZE,
    INVALID_CELL_POSITION,
    INVALID_CELL_WIDTH,
    INVALID_CELL_OVERLAP,
    INVALID_WIDE_CELL_CONTINUATION,
    INVALID_WIDE_CELL_STYLE,
    INVALID_STYLE_ID,
    INVALID_CURSOR_POSITION,
    INVALID_VIEWPORT,
    INVALID_DIRTY_ROW_RANGE,
    INVALID_SELECTION_SPAN,
    INVALID_HYPERLINK_METADATA,
};

struct Terminal_render_cell
{
    terminal_grid_position_t   position;
    QString                    text;
    std::uint64_t              hyperlink_id      = 0U;
    int                        display_width     = 1;
    bool                       wide_continuation = false;
    Terminal_style_id          style_id          = k_default_terminal_style_id;
};

struct Terminal_render_cursor
{
    terminal_grid_position_t   position;
    Terminal_cursor_shape      shape         = Terminal_cursor_shape::BLOCK;
    bool                       visible       = true;
    bool                       blink_enabled = true;
};

struct Terminal_render_dirty_row_range
{
    int                        first_row = 0;
    int                        row_count = 0;
};

struct Terminal_render_selection_span
{
    Terminal_selection_range   source_range;
    int                        row          = 0;
    int                        first_column = 0;
    int                        column_count = 0;
};

struct Terminal_render_hyperlink_metadata
{
    std::uint64_t              hyperlink_id                 = 0U;
    QByteArray                 identity_key;
    QByteArray                 uri;
};

struct Terminal_render_metadata
{
    std::uint64_t              sequence                     = 0U;
    bool                       backend_geometry_in_sync     = true;
    bool                       visual_bell_active           = false;
    bool                       mouse_reporting_mode_changed = false;
};

struct Terminal_render_snapshot_request
{
    std::uint64_t                         sequence = 0U;
    Terminal_viewport_state               viewport;
    std::vector<int>                      dirty_rows;
    bool                                  viewport_changed = false;
    Terminal_cursor_shape                 cursor_shape = Terminal_cursor_shape::BLOCK;
    bool                                  cursor_blink_enabled = true;
    Ime_preedit_state                     ime_preedit;
    std::vector<Terminal_selection_range> selections;
    bool                                  backend_geometry_in_sync = true;
    bool                                  visual_bell_active = false;
    bool                                  mouse_reporting_mode_changed = false;
};

struct Terminal_render_snapshot
{
    terminal_grid_size_t                               grid_size;
    Terminal_viewport_state                            viewport;
    Terminal_color_state                               color_state;
    std::vector<Terminal_text_style>                   styles;
    std::vector<Terminal_render_cell>                  cells;
    std::vector<Terminal_render_dirty_row_range>       dirty_row_ranges;
    std::vector<Terminal_render_hyperlink_metadata>    hyperlinks;
    Terminal_render_cursor                             cursor;
    Ime_preedit_state                                  ime_preedit;
    std::vector<Terminal_render_selection_span>        selection_spans;
    Terminal_render_metadata                           metadata;
    Terminal_mode_state                                modes;
};

struct Terminal_render_snapshot_validation
{
    Terminal_render_snapshot_status status = Terminal_render_snapshot_status::OK;
};

inline Terminal_render_snapshot make_empty_render_snapshot(
    terminal_grid_size_t       grid_size,
    Terminal_viewport_state    viewport,
    std::uint64_t              sequence)
{
    Terminal_render_snapshot snapshot;
    snapshot.grid_size             = grid_size;
    snapshot.viewport              = viewport;
    snapshot.viewport.visible_rows = grid_size.rows > 0 ? grid_size.rows : viewport.visible_rows;
    snapshot.metadata.sequence     = sequence;
    snapshot.styles.push_back(make_default_terminal_text_style());
    return snapshot;
}

inline std::vector<Terminal_render_dirty_row_range> compact_dirty_row_ranges(
    std::vector<int>           dirty_rows,
    int                        visible_rows,
    bool                       full_repaint)
{
    if (visible_rows <= 0) {
        return {};
    }

    if (full_repaint) {
        return {{0, visible_rows}};
    }

    std::sort(dirty_rows.begin(), dirty_rows.end());
    dirty_rows.erase(std::unique(dirty_rows.begin(), dirty_rows.end()), dirty_rows.end());

    std::vector<Terminal_render_dirty_row_range> ranges;
    for (int row : dirty_rows) {
        if (row < 0 || row >= visible_rows) {
            continue;
        }

        if (!ranges.empty() &&
            ranges.back().first_row + ranges.back().row_count == row)
        {
            ++ranges.back().row_count;
            continue;
        }

        ranges.push_back({row, 1});
    }

    return ranges;
}

inline Terminal_render_snapshot_validation validate_render_snapshot(
    const Terminal_render_snapshot& snapshot)
{
    if (snapshot.grid_size.rows <= 0 || snapshot.grid_size.columns <= 0) {
        return {Terminal_render_snapshot_status::INVALID_GRID_SIZE};
    }

    if (snapshot.styles.empty()) {
        return {Terminal_render_snapshot_status::INVALID_STYLE_ID};
    }

    if (!(snapshot.styles.front() == make_default_terminal_text_style())) {
        return {Terminal_render_snapshot_status::INVALID_STYLE_ID};
    }

    if (snapshot.viewport.visible_rows             <= 0                                 ||
        snapshot.viewport.visible_rows             != snapshot.grid_size.rows           ||
        snapshot.viewport.scrollback_rows          <  0                                 ||
        snapshot.viewport.offset_from_tail         <  0                                 ||
        snapshot.viewport.offset_from_tail         >  snapshot.viewport.scrollback_rows ||
        (snapshot.viewport.active_buffer == Terminal_buffer_id::ALTERNATE &&
            (snapshot.viewport.scrollback_rows != 0 ||
                snapshot.viewport.offset_from_tail != 0)))
    {
        return {Terminal_render_snapshot_status::INVALID_VIEWPORT};
    }

    int next_dirty_row = 0;
    for (const Terminal_render_dirty_row_range& range : snapshot.dirty_row_ranges) {
        if (range.first_row < 0 || range.row_count <= 0 || range.first_row < next_dirty_row ||
            range.first_row + range.row_count > snapshot.grid_size.rows)
        {
            return {Terminal_render_snapshot_status::INVALID_DIRTY_ROW_RANGE};
        }
        next_dirty_row = range.first_row + range.row_count;
    }

    std::set<std::uint64_t> hyperlink_ids;
    for (const Terminal_render_hyperlink_metadata& hyperlink : snapshot.hyperlinks) {
        if (hyperlink.hyperlink_id                     == 0U ||
            hyperlink_ids.find(hyperlink.hyperlink_id) != hyperlink_ids.end())
        {
            return {Terminal_render_snapshot_status::INVALID_HYPERLINK_METADATA};
        }
        hyperlink_ids.insert(hyperlink.hyperlink_id);
    }

    for (std::size_t i = 0; i < snapshot.cells.size(); ++i) {
        const Terminal_render_cell& cell = snapshot.cells[i];
        if (cell.position.row    < 0  || cell.position.row    >= snapshot.grid_size.rows ||
            cell.position.column < 0  || cell.position.column >= snapshot.grid_size.columns)
        {
            return {Terminal_render_snapshot_status::INVALID_CELL_POSITION};
        }

        if (static_cast<std::size_t>(cell.style_id) >= snapshot.styles.size()) {
            return {Terminal_render_snapshot_status::INVALID_STYLE_ID};
        }

        for (std::size_t j = 0; j < i; ++j) {
            const Terminal_render_cell& previous = snapshot.cells[j];
            if (previous.position.row    == cell.position.row &&
                previous.position.column == cell.position.column)
            {
                return {Terminal_render_snapshot_status::INVALID_CELL_OVERLAP};
            }
        }

        if (cell.wide_continuation) {
            if (!cell.text.isEmpty() || cell.display_width != 0 || cell.position.column == 0) {
                return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
            }
            continue;
        }

        if (cell.display_width <= 0 ||
            cell.display_width >  snapshot.grid_size.columns - cell.position.column)
        {
            return {Terminal_render_snapshot_status::INVALID_CELL_WIDTH};
        }
    }

    for (const Terminal_render_cell& cell : snapshot.cells) {
        if (cell.wide_continuation) {
            const Terminal_render_cell* covering_base       = nullptr;
            int                         covering_base_count = 0;
            for (const Terminal_render_cell& base_cell : snapshot.cells) {
                if (base_cell.wide_continuation || base_cell.position.row != cell.position.row) {
                    continue;
                }

                if (base_cell.position.column                        < cell.position.column &&
                    cell.position.column - base_cell.position.column < base_cell.display_width)
                {
                    covering_base = &base_cell;
                    ++covering_base_count;
                }
            }

            if (covering_base_count != 1) {
                return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
            }

            if (covering_base->style_id     != cell.style_id ||
                covering_base->hyperlink_id != cell.hyperlink_id)
            {
                return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_STYLE};
            }
            continue;
        }

        int continuation_count = 0;
        for (const Terminal_render_cell& covered_cell : snapshot.cells) {
            if (covered_cell.position.row                           != cell.position.row    ||
                covered_cell.position.column                        <= cell.position.column ||
                covered_cell.position.column - cell.position.column >= cell.display_width)
            {
                continue;
            }

            if (!covered_cell.wide_continuation) {
                return {Terminal_render_snapshot_status::INVALID_CELL_OVERLAP};
            }

            if (covered_cell.wide_continuation) {
                ++continuation_count;
            }
        }

        if (continuation_count != cell.display_width - 1) {
            return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
        }
    }

    for (const Terminal_render_cell& cell : snapshot.cells) {
        if (cell.hyperlink_id                     != 0U &&
            hyperlink_ids.find(cell.hyperlink_id) == hyperlink_ids.end())
        {
            return {Terminal_render_snapshot_status::INVALID_HYPERLINK_METADATA};
        }
    }

    if (snapshot.cursor.visible &&
        (snapshot.cursor.position.row < 0 || snapshot.cursor.position.row >= snapshot.grid_size.rows ||
         snapshot.cursor.position.column <  0 ||
         snapshot.cursor.position.column >= snapshot.grid_size.columns))
    {
        return {Terminal_render_snapshot_status::INVALID_CURSOR_POSITION};
    }

    for (const Terminal_render_selection_span& span : snapshot.selection_spans) {
        if (span.row < 0          || span.row >= snapshot.grid_size.rows ||
            span.first_column < 0 || span.column_count <= 0              ||
            span.first_column + span.column_count > snapshot.grid_size.columns)
        {
            return {Terminal_render_snapshot_status::INVALID_SELECTION_SPAN};
        }
    }

    return {};
}

}
