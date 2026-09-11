#pragma once

#include <QStringView>

#include <functional>

namespace vnm_terminal::terminal_app {

enum class Diagnostic_level
{
    WARNING,
    ERROR,
};

// The message view is valid only for the duration of the call. A missing sink
// uses the component-labelled stderr fallback for standalone terminal hosts.
using Diagnostic_sink = std::function<void(Diagnostic_level, QStringView)>;

void set_diagnostic_sink(Diagnostic_sink sink);
void clear_diagnostic_sink();
void write_diagnostic(Diagnostic_level level, QStringView message);

} // namespace vnm_terminal::terminal_app
