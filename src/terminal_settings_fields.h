#pragma once

#include "vnm_terminal/app_support/app_settings.h"

namespace vnm_terminal::terminal_app::detail {

template<typename Visitor>
void for_each_terminal_setting_field(Visitor&& visit)
{
    using Settings = Terminal_settings_snapshot;
    visit("color_scheme",                  &Settings::color_scheme);
    visit("font_family",                   &Settings::font_family);
    visit("font_size",                     &Settings::font_size);
    visit("font_advance_policy",           &Settings::font_advance_policy);
    visit("text_renderer_mode",            &Settings::text_renderer_mode);
    visit("lcd_subpixel_order",            &Settings::lcd_subpixel_order);
    visit("invert_brightness",             &Settings::invert_brightness);
    visit("row_timestamp_tooltip_enabled", &Settings::row_timestamp_tooltip_enabled);
    visit("copy_on_select",                &Settings::copy_on_select);
}

} // namespace vnm_terminal::terminal_app::detail
