#pragma once

#include "vnm_terminal/vnm_terminal_surface.h"

#include <optional>
#include <span>
#include <vector>

namespace vnm_terminal::terminal_app {

std::vector<Terminal_environment_entry>
capture_standalone_ambient_environment();

std::optional<std::vector<Terminal_environment_entry>>
sanitize_standalone_base_environment(
    std::span<const Terminal_environment_entry> captured_environment);

} // namespace vnm_terminal::terminal_app
