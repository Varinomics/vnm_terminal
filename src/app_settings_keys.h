#pragma once

namespace vnm_terminal::terminal_app {

constexpr int k_persisted_window_min_axis = 1;

constexpr char k_window_settings_group[]     = "window";
constexpr char k_window_settings_font_size[] = "font_size";
constexpr char k_window_settings_height[]    = "height";
constexpr char k_window_settings_maximized[] = "maximized";
constexpr char k_window_settings_width[]     = "width";
constexpr char k_window_settings_x[]         = "x";
constexpr char k_window_settings_y[]         = "y";

constexpr char k_appearance_settings_group[]        = "appearance";
constexpr char k_appearance_color_scheme[]          = "color_scheme";
constexpr char k_appearance_font_family[]           = "font_family";
constexpr char k_appearance_text_renderer_mode[]    = "text_renderer_mode";
constexpr char k_appearance_lcd_subpixel_order[]    = "lcd_subpixel_order";
constexpr char k_appearance_row_timestamp_tooltip[] = "row_timestamp_tooltip";
constexpr char k_appearance_scrollback_limit[]      = "scrollback_limit";

constexpr char k_appearance_chrome_focused_background[] =
    "chrome_focused_background";
constexpr char k_appearance_chrome_unfocused_background[] =
    "chrome_unfocused_background";
constexpr char k_appearance_chrome_focused_frame_edge[] =
    "chrome_focused_frame_edge";
constexpr char k_appearance_chrome_unfocused_frame_edge[] =
    "chrome_unfocused_frame_edge";

constexpr char k_interaction_settings_group[] = "interaction";
constexpr char k_interaction_copy_on_select[] = "copy_on_select";

} // namespace vnm_terminal::terminal_app
