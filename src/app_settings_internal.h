#pragma once

#include <QColor>
#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <optional>

class QSettings;

namespace vnm_terminal::terminal_app::detail {

inline constexpr std::size_t k_bytes_per_mib = 1024U * 1024U;

} // namespace vnm_terminal::terminal_app::detail

namespace vnm_terminal::terminal_app {

// Decoding filters invalid values and maps persisted MSDF to AUTO.
struct Persisted_appearance_settings
{
    std::optional<QString> color_scheme;
    std::optional<QString> font_family;
    std::optional<int>     font_advance_policy;
    std::optional<int>     text_renderer_mode;
    std::optional<int>     lcd_subpixel_order;
    std::optional<bool>    invert_brightness;
    std::optional<bool>    row_timestamp_tooltip;
    std::optional<int>     scrollback_buffer_size_mib;
    std::optional<QColor>  chrome_focused_background;
    std::optional<QColor>  chrome_unfocused_background;
    std::optional<QColor>  chrome_focused_frame_edge;
    std::optional<QColor>  chrome_unfocused_frame_edge;
};

namespace detail {

enum class Missing_scrollback_setting_policy
{
    Leave_unchanged,
    Remove,
};

std::optional<int> settings_int_value(QSettings& settings, const char* key);
std::optional<qreal> settings_font_size(QSettings& settings);
std::optional<int> settings_scrollback_buffer_size_mib(QSettings& settings);
std::optional<bool> settings_bool_value(QSettings& settings, const char* key);
std::optional<QColor> settings_color_value(QSettings& settings, const char* key);

Persisted_appearance_settings load_persisted_appearance_settings(QSettings& settings);
void save_persisted_appearance_settings(
    QSettings&                                  settings,
    const Persisted_appearance_settings&        appearance,
    Missing_scrollback_setting_policy           missing_scrollback_policy);

} // namespace vnm_terminal::terminal_app::detail

} // namespace vnm_terminal::terminal_app
