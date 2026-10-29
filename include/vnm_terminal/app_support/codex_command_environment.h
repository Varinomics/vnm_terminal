#pragma once

#include "vnm_terminal/vnm_terminal_surface.h"

#include <optional>

namespace vnm_terminal::terminal_app {

struct Codex_invocation
{
    // Includes the absolute executable and any fixed arguments such as a Node entry point.
    qsizetype argument_prefix_size;
};

// Prepare a Codex launch for the ordinary terminal backend without creating files.
// Ordinary commands remain unchanged. A failed preparation leaves the request unchanged.
// An explicit invocation identifies a verified Codex executable or native interpreter
// prefix; its fixed arguments remain before the inserted Codex CLI options.
bool prepare_codex_command(
    Terminal_process_start_request& request,
    QString& error,
    std::optional<Codex_invocation> explicit_invocation = std::nullopt);

} // namespace vnm_terminal::terminal_app
