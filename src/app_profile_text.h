#pragma once

#include "vnm_terminal/vnm_terminal_surface.h"

#if VNM_TERMINAL_PROFILING_ENABLED
// Privileged first-party use: the app owns its GUI-thread Hierarchical_profiler
// (profiling builds only). See vnm_terminal_surface docs/public_surface.md,
// "Internal Headers And Privileged First-Party Consumers".
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
