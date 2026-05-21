#pragma once

#include "vnm_terminal/internal/metrics_contract.h"
#include <QFont>

namespace vnm_terminal::internal {

class Qt_grid_metrics_provider final : public Terminal_grid_metrics_provider
{
public:
    Qt_grid_metrics_provider();
    Qt_grid_metrics_provider(QFont font, qreal device_pixel_ratio);

    terminal_cell_metrics_t cell_metrics() const override;
    Terminal_metrics_result grid_size_for_item_geometry(QSizeF geometry) const override;

    QFont font() const;
    void set_font(QFont font);

    qreal device_pixel_ratio() const;
    void set_device_pixel_ratio(qreal device_pixel_ratio);

private:
    QFont  m_font;
    qreal  m_device_pixel_ratio = 1.0;
};

}
