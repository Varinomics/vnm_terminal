#pragma once

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QString>
#include <QtGlobal>

namespace vnm_terminal::terminal_app {

enum class Osc52_clipboard_policy
{
    DENY,
    ALLOW,
};

// Bound how much a terminal program may push to the system clipboard in a single
// OSC 52 write even when the session is explicitly trusted (--osc52-clipboard allow).
constexpr qsizetype k_osc52_clipboard_max_payload_bytes = 1024 * 1024;

void handle_clipboard_write_request(
    VNM_TerminalSurface&     surface,
    quint64                  request_id,
    const QString&           target_selection,
    qsizetype                payload_size,
    Osc52_clipboard_policy   policy);

} // namespace vnm_terminal::terminal_app
