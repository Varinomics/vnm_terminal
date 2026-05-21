#pragma once

#include <QSizeF>
#include <cmath>

namespace vnm_terminal::internal {

enum class Terminal_metrics_status
{
    OK,
    INVALID_CELL_METRICS,
    INVALID_GEOMETRY,
};

enum class Terminal_metric_invalidation
{
    NONE,
    METRICS,
    RENDER_STYLE,
    SCROLLBACK_RETENTION,
    FUTURE_SNAPSHOTS,
};

enum class Terminal_surface_property
{
    FONT_FAMILY,
    FONT_SIZE,
    COLOR_THEME,
    CURSOR_STYLE,
    CURSOR_BLINK_ENABLED,
    SCROLLBACK_LIMIT,
    MOUSE_REPORTING_POLICY,
    BRACKETED_PASTE_POLICY,
    AUDIBLE_BELL_POLICY,
    VISUAL_BELL_POLICY,
};

struct terminal_grid_size_t
{
    int                        rows    = 0;
    int                        columns = 0;
};

struct terminal_cell_metrics_t
{
    qreal                      width   = 0.0;
    qreal                      height  = 0.0;
    qreal                      ascent  = 0.0;
    qreal                      descent = 0.0;
};

struct Terminal_metrics_result
{
    Terminal_metrics_status    status  = Terminal_metrics_status::OK;
    terminal_grid_size_t       grid_size;
};

inline bool is_valid_cell_metrics(terminal_cell_metrics_t metrics)
{
    return
        std::isfinite(metrics.width)   &&
        std::isfinite(metrics.height)  &&
        std::isfinite(metrics.ascent)  &&
        std::isfinite(metrics.descent) &&
        metrics.width > 0.0            &&
        metrics.height > 0.0;
}

Terminal_metric_invalidation invalidation_for_surface_property(
    Terminal_surface_property  property);

Terminal_metrics_result grid_size_for_geometry(
    QSizeF                     geometry,
    terminal_cell_metrics_t    metrics);

class Terminal_grid_metrics_provider
{
public:
    virtual ~Terminal_grid_metrics_provider() = default;

    virtual terminal_cell_metrics_t cell_metrics() const = 0;
    virtual Terminal_metrics_result grid_size_for_item_geometry(QSizeF geometry) const = 0;
};

class Fake_terminal_grid_metrics_provider final : public Terminal_grid_metrics_provider
{
public:
    terminal_cell_metrics_t cell_metrics() const override { return m_cell_metrics; }

    Terminal_metrics_result grid_size_for_item_geometry(QSizeF geometry) const override
    {
        return grid_size_for_geometry(geometry, m_cell_metrics);
    }

    void set_cell_metrics(terminal_cell_metrics_t metrics) { m_cell_metrics = metrics; }

private:
    terminal_cell_metrics_t m_cell_metrics{8.0, 16.0, 12.0, 4.0};
};

}
