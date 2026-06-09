#include "terminal_window.h"

#include "app_common.h"
#include "terminal_title_metadata.h"

#include "vnm_qml_chrome/vnm_chrome_geometry.h"

#include "vnm_terminal/font_metrics.h"

#include <QEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSize>
#include <QWindow>
#include <algorithm>
#include <cmath>

namespace vnm_terminal::terminal_app {

QString default_window_title()
{
    return QStringLiteral("vnm_terminal example terminal");
}

Wheel_delivery_indicator_filter::Wheel_delivery_indicator_filter(Terminal_qml_chrome& titlebar)
:
    QObject(&titlebar),
    m_titlebar(titlebar)
{}

bool Wheel_delivery_indicator_filter::eventFilter(QObject*, QEvent* event)
{
    if (event->type() == QEvent::Wheel) {
        m_titlebar.pulse_wheel_delivery_indicator();
    }

    return false;
}

void install_wheel_delivery_indicator_filter(
    VNM_TerminalSurface&             surface,
    Terminal_scrollbar&              scrollbar,
    Terminal_qml_chrome*             titlebar,
    bool                             enabled)
{
    if (!enabled || titlebar == nullptr) {
        return;
    }

    auto* filter = new Wheel_delivery_indicator_filter(*titlebar);
    surface.installEventFilter(filter);
    scrollbar.installEventFilter(filter);
}

void split_terminal_area(
    Terminal_shell_geometry& geometry,
    const QRectF&            area)
{
    const qreal scrollbar_width = std::min(k_terminal_scrollbar_width, area.width());
    geometry.terminal_rect  = QRectF(
        area.left(),
        area.top(),
        std::max<qreal>(0.0, area.width() - scrollbar_width),
        area.height());
    geometry.scrollbar_rect = QRectF(
        area.right() - scrollbar_width,
        area.top(),
        scrollbar_width,
        area.height());
}

void snap_terminal_shell_geometry(
    Terminal_shell_geometry& geometry,
    qreal                    device_pixel_ratio)
{
    const qreal dpr =
        vnm_qml_chrome::normalized_device_pixel_ratio(device_pixel_ratio);
    geometry.content_border_rect = vnm_qml_chrome::snapped_logical_rect(
        geometry.content_border_rect,
        dpr);
    geometry.terminal_rect = vnm_qml_chrome::snapped_logical_rect(
        geometry.terminal_rect,
        dpr);
    geometry.scrollbar_rect = vnm_qml_chrome::snapped_logical_rect(
        geometry.scrollbar_rect,
        dpr);
}

qreal reduced_chrome_span(
    qreal logical_span,
    qreal physical_reduction,
    qreal device_pixel_ratio)
{
    const qreal dpr = vnm_qml_chrome::normalized_device_pixel_ratio(device_pixel_ratio);
    const qreal snapped_logical_span =
        vnm_qml_chrome::snapped_logical_edge(logical_span, dpr);
    const qreal logical_reduction = physical_reduction / dpr;
    return std::max<qreal>(
        0.0,
        snapped_logical_span - logical_reduction);
}

qreal reduced_frameless_resize_border_width(qreal device_pixel_ratio)
{
    return reduced_chrome_span(
        k_default_frameless_resize_border_width,
        k_frameless_resize_border_physical_reduction,
        device_pixel_ratio);
}

qreal reduced_custom_titlebar_height(qreal device_pixel_ratio)
{
    return reduced_chrome_span(
        k_custom_titlebar_height,
        k_custom_titlebar_physical_reduction,
        device_pixel_ratio);
}

Terminal_shell_geometry terminal_shell_geometry(
    const QSizeF&  window_size,
    bool           custom_titlebar,
    bool           resize_border_active,
    qreal          content_border_width,
    qreal          device_pixel_ratio)
{
    const qreal width  = std::max<qreal>(0.0, window_size.width());
    const qreal height = std::max<qreal>(0.0, window_size.height());

    Terminal_shell_geometry geometry;
    if (!custom_titlebar) {
        split_terminal_area(geometry, QRectF(0.0, 0.0, width, height));
        return geometry;
    }

    const qreal border = resize_border_active
        ? reduced_frameless_resize_border_width(device_pixel_ratio)
        : 0.0;
    const qreal titlebar_height =
        std::min(reduced_custom_titlebar_height(device_pixel_ratio), height);
    const qreal horizontal_inset = std::min(border, width / 2.0);
    const qreal terminal_width_available =
        std::max<qreal>(0.0, width - horizontal_inset * 2.0);
    const qreal terminal_height_available = std::max<qreal>(0.0, height - titlebar_height);
    const qreal bottom_inset              = std::min(border, terminal_height_available);
    const qreal frame_border =
        std::isfinite(content_border_width)
            ? std::max<qreal>(0.0, content_border_width)
            : 0.0;

    geometry.chrome_rect = QRectF(0.0, 0.0, width, titlebar_height);
    geometry.content_border_rect = QRectF(
        horizontal_inset,
        titlebar_height,
        terminal_width_available,
        std::max<qreal>(0.0, terminal_height_available - bottom_inset));
    const qreal content_horizontal_inset =
        std::min(frame_border, geometry.content_border_rect.width() / 2.0);
    const qreal content_vertical_inset =
        std::min(frame_border, geometry.content_border_rect.height() / 2.0);
    split_terminal_area(
        geometry,
        geometry.content_border_rect.adjusted(
            content_horizontal_inset,
            content_vertical_inset,
            -content_horizontal_inset,
            -content_vertical_inset));
    snap_terminal_shell_geometry(geometry, device_pixel_ratio);
    return geometry;
}

qreal logical_device_pixel_width(const QQuickWindow& window)
{
    const qreal device_pixel_ratio = window.devicePixelRatio();
    if (!std::isfinite(device_pixel_ratio) || device_pixel_ratio <= 0.0) {
        return 1.0;
    }

    return 1.0 / device_pixel_ratio;
}

bool custom_titlebar_resize_border_active(const QQuickWindow& window)
{
    const Qt::WindowStates states = window.windowStates();
    return
        !states.testFlag(Qt::WindowMaximized) &&
        !states.testFlag(Qt::WindowMinimized) &&
        !states.testFlag(Qt::WindowFullScreen);
}

void apply_terminal_shell_geometry(
    QQuickWindow&                  window,
    VNM_TerminalSurface&           surface,
    Terminal_scrollbar&            scrollbar,
    Terminal_qml_chrome*           titlebar,
    bool                           custom_titlebar)
{
    const qreal content_border_width = titlebar != nullptr
        ? logical_device_pixel_width(window)
        : 0.0;
    const Terminal_shell_geometry geometry = terminal_shell_geometry(
        QSizeF(window.width(), window.height()),
        custom_titlebar,
        custom_titlebar_resize_border_active(window),
        content_border_width,
        window.devicePixelRatio());

    if (titlebar != nullptr) {
        titlebar->set_size(QSizeF(window.width(), window.height()));
        titlebar->set_content_border_rect(
            geometry.content_border_rect,
            content_border_width);
        if (QQuickItem* root_item = titlebar->root_item()) {
            root_item->setVisible(custom_titlebar);
        }
    }

    surface.setPosition(geometry.terminal_rect.topLeft());
    surface.setSize(geometry.terminal_rect.size());
    scrollbar.setPosition(geometry.scrollbar_rect.topLeft());
    scrollbar.setSize(geometry.scrollbar_rect.size());
}

void apply_synchronized_output_scroll_policy_option(
    VNM_TerminalSurface& surface,
    const App_options&   options)
{
    surface.set_synchronized_output_scroll_policy(
        options.synchronized_output_scroll_policy);
}

void apply_primary_repaint_recovery_option(
    VNM_TerminalSurface& surface,
    const App_options&   options)
{
    if (options.primary_repaint_recovery_enabled.has_value()) {
        surface.set_primary_repaint_recovery_enabled(
            *options.primary_repaint_recovery_enabled);
    }
}

void apply_scrollback_limit_option(
    VNM_TerminalSurface& surface,
    const App_options&   options)
{
    if (options.scrollback_limit.has_value()) {
        surface.set_scrollback_limit(*options.scrollback_limit);
    }
}

bool resize_window_for_text_area_request(
    QQuickWindow&                  window,
    const VNM_TerminalSurface&     surface,
    int                            rows,
    int                            columns)
{
    if (rows <= 0                            || columns <= 0                              ||
        rows >  k_text_area_resize_max_rows  || columns >  k_text_area_resize_max_columns ||
        surface.rows() <= 0                  || surface.columns() <= 0                    ||
        surface.width() <= 0.0               || surface.height() <= 0.0)
    {
        return false;
    }

    const vnm_terminal::Cell_metrics cell_metrics = vnm_terminal::cell_metrics_for_font(
        surface.font_family(),
        surface.font_size(),
        window.devicePixelRatio());
    if (!vnm_terminal::cell_metrics_valid(cell_metrics)) {
        return false;
    }

    const qreal requested_surface_width =
        cell_metrics.width * static_cast<qreal>(columns);
    const qreal requested_surface_height =
        cell_metrics.height * static_cast<qreal>(rows);
    const qreal requested_window_width = std::clamp<qreal>(
        static_cast<qreal>(window.width()) + requested_surface_width - surface.width(),
        1.0,
        k_text_area_resize_max_window_axis);
    const qreal requested_window_height = std::clamp<qreal>(
        static_cast<qreal>(window.height()) + requested_surface_height - surface.height(),
        1.0,
        k_text_area_resize_max_window_axis);

    const QSize requested_size(
        static_cast<int>(std::round(requested_window_width)),
        static_cast<int>(std::round(requested_window_height)));
    if (requested_size == window.size()) {
        return false;
    }

    window.resize(requested_size);
    return true;
}

QString visible_terminal_title(QString terminal_title)
{
    terminal_title = terminal_title.trimmed();
    return terminal_title.isEmpty() ? default_window_title() : terminal_title;
}

void sync_terminal_title(
    QQuickWindow&                  window,
    Terminal_qml_chrome*           titlebar,
    const QString&                 terminal_title,
    const QString&                 terminal_icon_name)
{
    const QString visible_title = visible_terminal_title(terminal_title);
    window.setTitle(visible_title);
    if (titlebar != nullptr) {
        const Terminal_title_content content =
            derive_terminal_title_content(visible_title, terminal_icon_name);
        titlebar->set_title(content.display_title);
        titlebar->set_activity_marker_text(activity_marker_text(content));
    }
}

void connect_terminal_metadata_to_chrome(
    VNM_TerminalSurface&           surface,
    QQuickWindow&                  window,
    Terminal_qml_chrome*           titlebar)
{
    auto sync_metadata = [titlebar, &window, &surface] {
        sync_terminal_title(
            window,
            titlebar,
            surface.terminal_title(),
            surface.terminal_icon_name());
    };

    QObject::connect(
        &surface,
        &VNM_TerminalSurface::terminal_title_changed,
        &window,
        sync_metadata);
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::terminal_icon_name_changed,
        &window,
        sync_metadata);
    sync_metadata();
}

void sync_chrome_window_state(
    Terminal_qml_chrome&           titlebar,
    QQuickWindow&                  window)
{
    titlebar.set_active(window.isActive());
    titlebar.set_maximized(
        window.windowStates().testFlag(Qt::WindowMaximized) ||
        window.windowStates().testFlag(Qt::WindowFullScreen));
    titlebar.set_resize_enabled(custom_titlebar_resize_border_active(window));
    window.setColor(terminal_chrome_background_color(window.isActive()));
}

} // namespace vnm_terminal::terminal_app
