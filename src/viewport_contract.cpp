#include "vnm_terminal/internal/viewport_contract.h"

#include <algorithm>
#include <limits>

namespace vnm_terminal::internal {

namespace {

int state_max_offset_from_tail(const Terminal_viewport_state& state)
{
    return std::max(0, state.scrollback_rows);
}

void clamp_state_offset(Terminal_viewport_state& state)
{
    state.offset_from_tail =
        std::clamp(state.offset_from_tail, 0, state_max_offset_from_tail(state));
    state.follow_tail = state.offset_from_tail == 0;
}

void apply_scrollback_rows(Terminal_viewport_state& state, int rows)
{
    const int old_rows = state.scrollback_rows;
    state.scrollback_rows = std::max(0, rows);

    if (!state.follow_tail && state.scrollback_rows > old_rows) {
        state.offset_from_tail += state.scrollback_rows - old_rows;
    }

    if (state.follow_tail) {
        state.offset_from_tail = 0;
    }

    clamp_state_offset(state);
}

void apply_scrollback_sync(
    Terminal_viewport_state&   state,
    int                        rows,
    int                        evicted_rows)
{
    const int old_first_visible_row =
        state.scrollback_rows - state.offset_from_tail;

    state.scrollback_rows = std::max(0, rows);

    if (state.follow_tail) {
        state.offset_from_tail = 0;
        clamp_state_offset(state);
        return;
    }

    const int bounded_evicted_rows = std::max(0, evicted_rows);
    const int preserved_first_row =
        old_first_visible_row < bounded_evicted_rows
            ? 0
            : old_first_visible_row - bounded_evicted_rows;

    state.offset_from_tail = state.scrollback_rows - preserved_first_row;
    clamp_state_offset(state);
}

}

Terminal_viewport_result Terminal_viewport_controller::set_visible_rows(int rows)
{
    if (rows <= 0) {
        return {Terminal_viewport_result_code::INVALID_VISIBLE_ROWS};
    }

    m_state.visible_rows = rows;
    m_primary_state.visible_rows = rows;
    clamp_state_offset(m_state);
    clamp_state_offset(m_primary_state);
    return {};
}

void Terminal_viewport_controller::set_scrollback_rows(int rows)
{
    if (m_state.active_buffer == Terminal_buffer_id::ALTERNATE) {
        apply_scrollback_rows(m_primary_state, rows);
        return;
    }

    apply_scrollback_rows(m_state, rows);
}

void Terminal_viewport_controller::sync_scrollback_rows(int rows, int evicted_rows)
{
    if (m_state.active_buffer == Terminal_buffer_id::ALTERNATE) {
        apply_scrollback_sync(m_primary_state, rows, evicted_rows);
        return;
    }

    apply_scrollback_sync(m_state, rows, evicted_rows);
}

Terminal_viewport_scroll_result Terminal_viewport_controller::scroll_lines(int line_delta)
{
    if (m_state.active_buffer == Terminal_buffer_id::ALTERNATE) {
        if (m_state.alternate_screen_scroll_policy ==
            Terminal_alternate_screen_scroll_policy::WHEEL_TO_TERMINAL_INPUT)
        {
            return {Terminal_viewport_scroll_action::TERMINAL_INPUT, 0};
        }

        return {};
    }

    const int previous_offset = m_state.offset_from_tail;
    const long long requested_offset =
        static_cast<long long>(previous_offset) + line_delta;
    m_state.offset_from_tail = static_cast<int>(
        std::clamp(requested_offset, 0LL, static_cast<long long>(state_max_offset_from_tail(m_state))));
    m_state.follow_tail      = m_state.offset_from_tail == 0;

    if (m_state.offset_from_tail == previous_offset) {
        return {};
    }

    return {
        Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        m_state.offset_from_tail - previous_offset,
    };
}

Terminal_viewport_scroll_result Terminal_viewport_controller::scroll_pages(int page_delta)
{
    const long long requested_delta =
        static_cast<long long>(page_delta) * m_state.visible_rows;
    const int line_delta = static_cast<int>(std::clamp(
        requested_delta,
        static_cast<long long>(std::numeric_limits<int>::min()),
        static_cast<long long>(std::numeric_limits<int>::max())));

    return scroll_lines(line_delta);
}

void Terminal_viewport_controller::return_to_tail()
{
    m_state.offset_from_tail = 0;
    m_state.follow_tail      = true;
}

void Terminal_viewport_controller::notify_user_input()
{
    return_to_tail();
}

void Terminal_viewport_controller::enter_alternate_screen()
{
    if (m_state.active_buffer == Terminal_buffer_id::ALTERNATE) {
        return;
    }

    m_primary_state          = m_state;
    m_state.active_buffer    = Terminal_buffer_id::ALTERNATE;
    m_state.scrollback_rows  = 0;
    m_state.offset_from_tail = 0;
    m_state.follow_tail      = true;
}

void Terminal_viewport_controller::leave_alternate_screen()
{
    if (m_state.active_buffer == Terminal_buffer_id::PRIMARY) {
        clamp_state_offset(m_state);
        return;
    }

    const int visible_rows = m_state.visible_rows;
    const Terminal_alternate_screen_scroll_policy alternate_policy =
        m_state.alternate_screen_scroll_policy;

    m_state                                = m_primary_state;
    m_state.active_buffer                  = Terminal_buffer_id::PRIMARY;
    m_state.visible_rows                   = visible_rows;
    m_state.alternate_screen_scroll_policy = alternate_policy;
    clamp_state_offset(m_state);
}

void Terminal_viewport_controller::set_alternate_screen_scroll_policy(
    Terminal_alternate_screen_scroll_policy policy)
{
    m_state.alternate_screen_scroll_policy = policy;
    m_primary_state.alternate_screen_scroll_policy = policy;
}

int Terminal_viewport_controller::max_offset_from_tail() const
{
    return state_max_offset_from_tail(m_state);
}

int Terminal_viewport_controller::first_visible_logical_row() const
{
    if (m_state.active_buffer == Terminal_buffer_id::ALTERNATE) {
        return 0;
    }

    return m_state.scrollback_rows - m_state.offset_from_tail;
}

Terminal_viewport_coordinate_result
Terminal_viewport_controller::viewport_row_to_logical_row(int viewport_row) const
{
    if (viewport_row < 0 || viewport_row >= m_state.visible_rows) {
        return {Terminal_viewport_coordinate_result_code::INVALID_VIEWPORT_ROW, 0};
    }

    return {
        Terminal_viewport_coordinate_result_code::OK,
        first_visible_logical_row() + viewport_row,
    };
}

}
