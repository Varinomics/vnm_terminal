#include "vnm_terminal/internal/metrics_contract.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vnm_terminal::internal {

Terminal_metric_invalidation invalidation_for_surface_property(
    Terminal_surface_property property)
{
    switch (property) {
        case Terminal_surface_property::FONT_FAMILY:          return Terminal_metric_invalidation::METRICS;
        case Terminal_surface_property::FONT_SIZE:            return Terminal_metric_invalidation::METRICS;
        case Terminal_surface_property::COLOR_THEME:          return Terminal_metric_invalidation::RENDER_STYLE;
        case Terminal_surface_property::CURSOR_STYLE:         return Terminal_metric_invalidation::RENDER_STYLE;
        case Terminal_surface_property::CURSOR_BLINK_ENABLED: return Terminal_metric_invalidation::RENDER_STYLE;
        case Terminal_surface_property::VISUAL_BELL_POLICY:   return Terminal_metric_invalidation::RENDER_STYLE;
        case Terminal_surface_property::SCROLLBACK_LIMIT:     return Terminal_metric_invalidation::SCROLLBACK_RETENTION;

        case Terminal_surface_property::MOUSE_REPORTING_POLICY:
        case Terminal_surface_property::BRACKETED_PASTE_POLICY:
        case Terminal_surface_property::AUDIBLE_BELL_POLICY:
            return Terminal_metric_invalidation::FUTURE_SNAPSHOTS;
    }

    return Terminal_metric_invalidation::NONE;
}

Terminal_metrics_result grid_size_for_geometry(
    QSizeF                     geometry,
    terminal_cell_metrics_t    metrics)
{
    if (!is_valid_cell_metrics(metrics)) {
        return {Terminal_metrics_status::INVALID_CELL_METRICS, {}};
    }

    if (!std::isfinite(geometry.width())  ||
        !std::isfinite(geometry.height()) ||
        geometry.width()  <= 0.0          ||
        geometry.height() <= 0.0)
    {
        return {Terminal_metrics_status::INVALID_GEOMETRY, {}};
    }

    const qreal rows     = geometry.height() / metrics.height;
    const qreal columns  = geometry.width()  / metrics.width;
    const qreal max_axis = static_cast<qreal>(std::numeric_limits<int>::max());
    if (!std::isfinite(rows)    ||
        !std::isfinite(columns) ||
        rows    > max_axis      ||
        columns > max_axis)
    {
        return {Terminal_metrics_status::INVALID_GEOMETRY, {}};
    }

    return {
        Terminal_metrics_status::OK,
        {
            std::max(1, static_cast<int>(rows)),
            std::max(1, static_cast<int>(columns)),
        },
    };
}

}
