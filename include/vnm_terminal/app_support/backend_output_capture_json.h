#pragma once

#include "vnm_terminal/backend_output_capture.h"

#include <QJsonObject>

#include <optional>

namespace vnm_terminal::terminal_app {

// Encodes the terminal_diagnostics value within a worker startup payload.
QJsonObject backend_output_capture_config_to_json(
    const Backend_output_capture_config& config);

// Rejects malformed values; the caller owns presence and payload errors.
std::optional<Backend_output_capture_config> backend_output_capture_config_from_json(
    const QJsonObject& object);

} // namespace vnm_terminal::terminal_app
