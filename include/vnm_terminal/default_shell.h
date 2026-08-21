#pragma once

#include <QStringList>

namespace vnm_terminal {

// Returns the platform's default shell as separated argv. The environment
// value is one executable argument and is never parsed as a command line.
QStringList default_shell_argv();

} // namespace vnm_terminal
