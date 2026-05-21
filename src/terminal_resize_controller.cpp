#include "vnm_terminal/internal/terminal_resize_controller.h"

#include <QString>
#include <optional>
#include <utility>

namespace vnm_terminal::internal {

namespace {

bool same_grid_size(terminal_grid_size_t lhs, terminal_grid_size_t rhs)
{
    return lhs.rows == rhs.rows && lhs.columns == rhs.columns;
}

QString metrics_failure_message(Terminal_metrics_status status)
{
    switch (status) {
        case Terminal_metrics_status::INVALID_CELL_METRICS:
            return QStringLiteral("terminal cell metrics must be positive before resize");
        case Terminal_metrics_status::INVALID_GEOMETRY:
            return QStringLiteral("terminal item geometry must be positive before resize");
        case Terminal_metrics_status::OK:
            break;
    }

    return QStringLiteral("terminal metrics failed before resize");
}

Terminal_session_result make_controller_rejection(
    Terminal_backend_error_code    error_code,
    QString                        message)
{
    return {
        Terminal_session_result_code::INVALID_ARGUMENT,
        0U,
        false,
        Terminal_backend_error{error_code, std::move(message)},
    };
}

Terminal_session_result make_controller_accept()
{
    return {
        Terminal_session_result_code::ACCEPTED,
        0U,
        false,
        std::nullopt,
    };
}

}

Terminal_resize_controller::Terminal_resize_controller(
    Terminal_session&                      session,
    const Terminal_grid_metrics_provider&  metrics_provider)
:
    m_session(session),
    m_metrics_provider(metrics_provider)
{}

Terminal_session_result Terminal_resize_controller::start_from_geometry(
    Terminal_launch_config launch_config,
    QSizeF                 source_geometry)
{
    const Terminal_metrics_result metrics_result =
        m_metrics_provider.grid_size_for_item_geometry(source_geometry);
    if (metrics_result.status != Terminal_metrics_status::OK) {
        return
            make_controller_rejection(
                Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
                metrics_failure_message(metrics_result.status));
    }

    launch_config.initial_grid_size = metrics_result.grid_size;
    return m_session.start(std::move(launch_config));
}

Terminal_session_result Terminal_resize_controller::resize_from_geometry(QSizeF source_geometry)
{
    return resize_or_refresh_from_geometry(source_geometry);
}

Terminal_session_result Terminal_resize_controller::refresh_from_geometry(QSizeF source_geometry)
{
    return resize_or_refresh_from_geometry(source_geometry);
}

Terminal_session_result Terminal_resize_controller::resize_or_refresh_from_geometry(
    QSizeF source_geometry)
{
    const Terminal_metrics_result metrics_result =
        m_metrics_provider.grid_size_for_item_geometry(source_geometry);
    if (metrics_result.status != Terminal_metrics_status::OK) {
        return
            make_controller_rejection(
                Terminal_backend_error_code::RESIZE_FAILED,
                metrics_failure_message(metrics_result.status));
    }

    if (m_session.process_state() == Terminal_process_state::RUNNING &&
        m_session.backend_ready() &&
        m_session.backend_geometry_in_sync() &&
        same_grid_size(metrics_result.grid_size, m_session.grid_size()))
    {
        return make_controller_accept();
    }

    return m_session.resize(source_geometry, metrics_result.grid_size);
}

}
