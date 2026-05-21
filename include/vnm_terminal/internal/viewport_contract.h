#pragma once

namespace vnm_terminal::internal {

enum class Terminal_buffer_id
{
    PRIMARY,
    ALTERNATE,
};

enum class Terminal_viewport_result_code
{
    ACCEPTED,
    INVALID_VISIBLE_ROWS,
};

enum class Terminal_alternate_screen_scroll_policy
{
    KEEP_AT_TAIL,
    WHEEL_TO_TERMINAL_INPUT,
};

enum class Terminal_viewport_scroll_action
{
    VIEWPORT_MOVED,
    AT_BOUNDARY,
    TERMINAL_INPUT,
};

enum class Terminal_viewport_coordinate_result_code
{
    OK,
    INVALID_VIEWPORT_ROW,
};

struct Terminal_viewport_state
{
    Terminal_buffer_id active_buffer    = Terminal_buffer_id::PRIMARY;
    int                scrollback_rows  = 0;
    int                visible_rows     = 1;
    int                offset_from_tail = 0;
    bool               follow_tail      = true;
    Terminal_alternate_screen_scroll_policy alternate_screen_scroll_policy =
        Terminal_alternate_screen_scroll_policy::KEEP_AT_TAIL;
};

struct Terminal_viewport_result
{
    Terminal_viewport_result_code code = Terminal_viewport_result_code::ACCEPTED;
};

struct Terminal_viewport_scroll_result
{
    Terminal_viewport_scroll_action action = Terminal_viewport_scroll_action::AT_BOUNDARY;
    int                             applied_line_delta = 0;
};

struct Terminal_viewport_coordinate_result
{
    Terminal_viewport_coordinate_result_code code = Terminal_viewport_coordinate_result_code::OK;
    int logical_row = 0;
};

class Terminal_viewport_controller
{
public:
    const Terminal_viewport_state& state() const { return m_state; }

    Terminal_viewport_result set_visible_rows(int rows);

    void set_scrollback_rows(int rows);
    void sync_scrollback_rows(int rows, int evicted_rows);

    Terminal_viewport_scroll_result scroll_lines(int line_delta);
    Terminal_viewport_scroll_result scroll_pages(int page_delta);

    void return_to_tail();
    void notify_user_input();

    void enter_alternate_screen();
    void leave_alternate_screen();

    void set_alternate_screen_scroll_policy(Terminal_alternate_screen_scroll_policy policy);

    int max_offset_from_tail() const;
    int first_visible_logical_row() const;

    Terminal_viewport_coordinate_result viewport_row_to_logical_row(int viewport_row) const;

private:
    Terminal_viewport_state m_state;
    Terminal_viewport_state m_primary_state;
};

}
