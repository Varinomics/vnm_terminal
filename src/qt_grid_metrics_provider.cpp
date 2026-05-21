#include "vnm_terminal/internal/qt_grid_metrics_provider.h"

#include <QFontMetricsF>
#include <QLatin1Char>
#include <cmath>
#include <utility>

namespace vnm_terminal::internal {

namespace {

qreal normalized_device_pixel_ratio(qreal device_pixel_ratio)
{
    if (!std::isfinite(device_pixel_ratio) || device_pixel_ratio <= 0.0) {
        return 1.0;
    }

    return device_pixel_ratio;
}

}

Qt_grid_metrics_provider::Qt_grid_metrics_provider() = default;

Qt_grid_metrics_provider::Qt_grid_metrics_provider(QFont font, qreal device_pixel_ratio)
:
    m_font(std::move(font)),
    m_device_pixel_ratio(normalized_device_pixel_ratio(device_pixel_ratio))
{}

terminal_cell_metrics_t Qt_grid_metrics_provider::cell_metrics() const
{
    // QQuickItem geometry and published grid metrics are logical pixels. DPR is
    // retained only as invalidation/configuration state unless physical-pixel
    // snapping is introduced here.
    const QFontMetricsF metrics(m_font);
    return {
        metrics.horizontalAdvance(QLatin1Char('M')),
        metrics.lineSpacing(),
        metrics.ascent(),
        metrics.descent(),
    };
}

Terminal_metrics_result Qt_grid_metrics_provider::grid_size_for_item_geometry(
    QSizeF geometry) const
{
    return grid_size_for_geometry(geometry, cell_metrics());
}

QFont Qt_grid_metrics_provider::font() const
{
    return m_font;
}

void Qt_grid_metrics_provider::set_font(QFont font)
{
    m_font = std::move(font);
}

qreal Qt_grid_metrics_provider::device_pixel_ratio() const
{
    return m_device_pixel_ratio;
}

void Qt_grid_metrics_provider::set_device_pixel_ratio(qreal device_pixel_ratio)
{
    m_device_pixel_ratio = normalized_device_pixel_ratio(device_pixel_ratio);
}

}
