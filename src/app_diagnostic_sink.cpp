#include "vnm_terminal/app_support/diagnostic_sink.h"

#include <QByteArray>
#include <QString>

#include <cstdio>
#include <mutex>
#include <utility>

namespace vnm_terminal::terminal_app {

namespace {

std::mutex& diagnostic_sink_mutex()
{
    static std::mutex mutex;
    return mutex;
}

Diagnostic_sink& configured_diagnostic_sink()
{
    static Diagnostic_sink sink;
    return sink;
}

const char* diagnostic_level_name(Diagnostic_level level)
{
    switch (level) {
        case Diagnostic_level::WARNING: return "warning";
        case Diagnostic_level::ERROR:   return "error";
    }
    return "error";
}

} // namespace

void set_diagnostic_sink(Diagnostic_sink sink)
{
    const std::lock_guard<std::mutex> lock(diagnostic_sink_mutex());
    configured_diagnostic_sink() = std::move(sink);
}

void clear_diagnostic_sink()
{
    set_diagnostic_sink({});
}

void write_diagnostic(Diagnostic_level level, QStringView message)
{
    Diagnostic_sink sink;
    {
        const std::lock_guard<std::mutex> lock(diagnostic_sink_mutex());
        sink = configured_diagnostic_sink();
    }

    if (sink) {
        try {
            sink(level, message);
            return;
        }
        catch (...) {
        }
    }

    const QByteArray utf8 = message.toString().toUtf8();
    std::fprintf(
        stderr,
        "[vnm_terminal][%s] %s\n",
        diagnostic_level_name(level),
        utf8.constData());
    std::fflush(stderr);
}

} // namespace vnm_terminal::terminal_app
