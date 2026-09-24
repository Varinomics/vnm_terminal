#pragma once

#include <QtGlobal>

#include <cstddef>
#include <optional>

class QSettings;

namespace vnm_terminal::terminal_app::detail {

inline constexpr std::size_t k_bytes_per_mib = 1024U * 1024U;

std::optional<int> settings_int_value(QSettings& settings, const char* key);
std::optional<qreal> settings_font_size(QSettings& settings);
std::optional<int> settings_scrollback_buffer_size_mib(QSettings& settings);

} // namespace vnm_terminal::terminal_app::detail
