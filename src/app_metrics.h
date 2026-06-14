#pragma once

#include "app_common.h"
#include "app_presentation_metrics.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QFile>
#include <QString>

#include <optional>

namespace vnm_terminal::terminal_app {

enum class Metrics_timeline_sample_kind
{
    PERIODIC,
    FINAL,
};

class Metrics_timeline_jsonl_writer
{
public:
    bool open(const QString& path, QString* out_error);

    bool write_sample(
        Metrics_timeline_sample_kind kind,
        const VNM_TerminalSurface&   surface,
        const Presentation_metrics_recorder&
                                     presentation_metrics,
        const Runtime_state&         state,
        const metrics_timing_t&      timing,
        std::optional<int>           app_result,
        int                          interval_ms,
        QString*                     out_error);

private:
    QFile m_file;
    int   m_sample_index = 0;
};

bool write_metrics_json(
    const QString&              path,
    const VNM_TerminalSurface&  surface,
    const Presentation_metrics_recorder&
                                presentation_metrics,
    const Runtime_state&        state,
    const metrics_timing_t&     timing,
    int                         app_result,
    QString*                    out_error);

} // namespace vnm_terminal::terminal_app
