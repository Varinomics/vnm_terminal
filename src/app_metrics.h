#pragma once

#include "app_common.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QString>

namespace vnm_terminal::terminal_app {

bool write_metrics_json(
    const QString&              path,
    const VNM_TerminalSurface&  surface,
    const Runtime_state&        state,
    const metrics_timing_t&     timing,
    int                         app_result,
    QString*                    out_error);

} // namespace vnm_terminal::terminal_app
