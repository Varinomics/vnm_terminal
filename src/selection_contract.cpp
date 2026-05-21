#include "vnm_terminal/internal/selection_contract.h"

#include <QStringList>
#include <algorithm>

namespace vnm_terminal::internal {

namespace {

terminal_grid_position_t normalized_start(const Terminal_selection_range& range)
{
    if (range.start.row < range.end.row) {
        return range.start;
    }

    if (range.start.row > range.end.row) {
        return range.end;
    }

    return range.start.column <= range.end.column ? range.start : range.end;
}

terminal_grid_position_t normalized_end(const Terminal_selection_range& range)
{
    if (range.start.row < range.end.row) {
        return range.end;
    }

    if (range.start.row > range.end.row) {
        return range.start;
    }

    return range.start.column <= range.end.column ? range.end : range.start;
}

int first_selected_row(const Terminal_selection_range& range)
{
    return std::min(range.start.row, range.end.row);
}

int last_selected_row(const Terminal_selection_range& range)
{
    return std::max(range.start.row, range.end.row);
}

}

void Selection_contract_controller::begin(terminal_grid_position_t anchor)
{
    m_range        = {anchor, anchor, Terminal_selection_mode::NORMAL};
    m_has_selection = true;
}

void Selection_contract_controller::extend(terminal_grid_position_t extent)
{
    m_range.end = extent;
}

void Selection_contract_controller::clear()
{
    m_has_selection = false;
    m_range         = {};
}

void Selection_contract_controller::set_range(Terminal_selection_range range)
{
    if (range.mode == Terminal_selection_mode::NONE) {
        clear();
        return;
    }

    m_range         = range;
    m_has_selection = true;
}

bool Selection_contract_controller::apply_scrollback_eviction(int evicted_rows)
{
    if (!m_has_selection || evicted_rows <= 0) {
        return false;
    }

    if (first_selected_row(m_range) < evicted_rows) {
        clear();
        return true;
    }

    m_range.start.row -= evicted_rows;
    m_range.end.row   -= evicted_rows;
    return true;
}

Terminal_selection_result Selection_contract_controller::selected_text(
    std::span<const QString> logical_rows) const
{
    if (!m_has_selection) {
        return {Terminal_selection_result_code::NO_SELECTION, {}};
    }

    const int first_row = first_selected_row(m_range);
    const int last_row  = last_selected_row(m_range);

    if (first_row < 0 || last_row >= static_cast<int>(logical_rows.size())) {
        return {Terminal_selection_result_code::INVALID_RANGE, {}};
    }

    if (first_row == last_row) {
        const int      left  = std::min(m_range.start.column, m_range.end.column);
        const int      right = std::max(m_range.start.column, m_range.end.column);
        const QString& text  = logical_rows[static_cast<std::size_t>(first_row)];

        if (left < 0 || right > text.size()) {
            return {Terminal_selection_result_code::INVALID_RANGE, {}};
        }

        return {Terminal_selection_result_code::OK, text.mid(left, right - left)};
    }

    const terminal_grid_position_t start = normalized_start(m_range);
    const terminal_grid_position_t end   = normalized_end(m_range);
    QStringList selected_rows;

    for (int row = first_row; row <= last_row; ++row) {
        const QString& text = logical_rows[static_cast<std::size_t>(row)];

        if (row == start.row) {
            if (start.column < 0 || start.column > text.size()) {
                return {Terminal_selection_result_code::INVALID_RANGE, {}};
            }

            selected_rows.push_back(text.mid(start.column));
        }
        else
        if (row == end.row) {
            if (end.column < 0 || end.column > text.size()) {
                return {Terminal_selection_result_code::INVALID_RANGE, {}};
            }

            selected_rows.push_back(text.left(end.column));
        }
        else {
            selected_rows.push_back(text);
        }
    }

    return {Terminal_selection_result_code::OK, selected_rows.join(QLatin1Char('\n'))};
}

}
