#pragma once

#include "vnm_terminal/vnm_terminal_surface.h"

#if VNM_TERMINAL_PROFILING_ENABLED
#include "vnm_terminal/internal/hierarchical_profiler.h"

#include <QString>
#endif

namespace vnm_terminal::terminal_app {

#if VNM_TERMINAL_PROFILING_ENABLED

bool prepare_profile_text_file(
    const QString& path,
    QString*       out_error);

bool write_profile_text(
    const QString&                                  path,
    VNM_TerminalSurface&                            surface,
    const vnm_terminal::internal::Hierarchical_profiler& gui_profiler,
    QString*                                        out_error);

#endif

} // namespace vnm_terminal::terminal_app
