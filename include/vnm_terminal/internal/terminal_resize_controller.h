#pragma once

#include "vnm_terminal/internal/metrics_contract.h"
#include "vnm_terminal/internal/terminal_session.h"
#include <QSizeF>

namespace vnm_terminal::internal {

class Terminal_resize_controller
{
public:
    Terminal_resize_controller(
        Terminal_session&                      session,
        const Terminal_grid_metrics_provider&  metrics_provider);

    Terminal_session_result start_from_geometry(
        Terminal_launch_config launch_config,
        QSizeF                 source_geometry);

    Terminal_session_result resize_from_geometry(
        QSizeF                 source_geometry);

    Terminal_session_result refresh_from_geometry(
        QSizeF                 source_geometry);

private:
    Terminal_session_result resize_or_refresh_from_geometry(QSizeF source_geometry);

    Terminal_session&                     m_session;
    const Terminal_grid_metrics_provider& m_metrics_provider;
};

}
