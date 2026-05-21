#pragma once

namespace vnm_terminal::internal {

enum class Terminal_input_mouse_tracking_mode
{
    NONE,
    X10,
    NORMAL,
    BUTTON_EVENT,
    ANY_EVENT,
};

struct Terminal_input_mode_state
{
    bool                               application_cursor_keys = false;
    bool                               application_keypad      = false;
    Terminal_input_mouse_tracking_mode mouse_tracking =
        Terminal_input_mouse_tracking_mode::NONE;
    bool                               sgr_mouse_encoding      = false;
    bool                               bracketed_paste         = false;
};

}
