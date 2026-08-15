#include "terminal_window.h"

#include "app_common.h"
#include "terminal_title_metadata.h"

#include "vnm_qml_chrome/vnm_chrome_geometry.h"

#include "vnm_terminal/font_metrics.h"

#include <vnm_qt_dispatch/vnm_qt_dispatch.h>

#include <QDateTime>
#include <QEvent>
#include <QPointF>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSize>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

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
    Terminal_content_geometry& geometry,
    const QRectF&            area)
{
    const qreal scrollbar_width = std::min(k_terminal_scrollbar_width, area.width());
    geometry.content_interior_rect = area;
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

void snap_terminal_content_geometry(
    Terminal_content_geometry& geometry,
    qreal                    device_pixel_ratio)
{
    const qreal dpr =
        vnm_qml_chrome::normalized_device_pixel_ratio(device_pixel_ratio);
    geometry.content_interior_rect = vnm_qml_chrome::snapped_logical_rect(
        geometry.content_interior_rect,
        dpr);
    geometry.terminal_rect = vnm_qml_chrome::snapped_logical_rect(
        geometry.terminal_rect,
        dpr);
    geometry.scrollbar_rect = vnm_qml_chrome::snapped_logical_rect(
        geometry.scrollbar_rect,
        dpr);
}

Terminal_content_geometry terminal_content_geometry(
    const QRectF& content_interior_rect,
    qreal         device_pixel_ratio)
{
    Terminal_content_geometry geometry;
    split_terminal_area(geometry, content_interior_rect);
    snap_terminal_content_geometry(geometry, device_pixel_ratio);
    return geometry;
}

bool window_geometry_is_window_manager_owned(const QQuickWindow& window)
{
    const Qt::WindowStates states = window.windowStates();
    return
        states.testFlag(Qt::WindowMaximized) ||
        states.testFlag(Qt::WindowMinimized) ||
        states.testFlag(Qt::WindowFullScreen);
}

bool custom_titlebar_resize_border_active(const QQuickWindow& window)
{
    return !window_geometry_is_window_manager_owned(window);
}

void apply_terminal_shell_geometry(
    QQuickWindow&                  window,
    VNM_TerminalSurface&           surface,
    Terminal_scrollbar&            scrollbar,
    Terminal_qml_chrome*           titlebar,
    bool                           custom_titlebar)
{
    QRectF content_interior_rect(0.0, 0.0, window.width(), window.height());
    qreal device_pixel_ratio = window.devicePixelRatio();
    if (titlebar != nullptr) {
        titlebar->set_size(QSizeF(window.width(), window.height()));
        if (QQuickItem* root_item = titlebar->root_item()) {
            root_item->setZ(10000.0);
            root_item->setVisible(custom_titlebar);
        }
        if (custom_titlebar) {
            content_interior_rect = titlebar->content_interior_rect();
            device_pixel_ratio = titlebar->device_pixel_ratio();
        }
    }

    const Terminal_content_geometry geometry =
        terminal_content_geometry(content_interior_rect, device_pixel_ratio);

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

void apply_retained_history_capacity_option(
    VNM_TerminalSurface& surface,
    const App_options&   options)
{
    if (options.retained_history_capacity_bytes.has_value()) {
        surface.set_retained_history_capacity_bytes(
            *options.retained_history_capacity_bytes);
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

    // The window manager owns the geometry in these states, so honoring the
    // request would drag the window off its maximized fill, or rewrite the
    // restore geometry of a minimized window. This mirrors the DECCOLM stance
    // recorded as dec-private-3 in the surface's terminal_sequence_matrix.md:
    // geometry stays host controlled and the grid follows the item.
    // connect_text_area_resize_policy() normally stops the request reaching the
    // host at all in these states; this keeps the helper correct on its own.
    if (window_geometry_is_window_manager_owned(window)) {
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

void connect_text_area_resize_policy(
    QQuickWindow&                  window,
    VNM_TerminalSurface&           surface)
{
    const auto apply_policy = [&window, &surface] {
        surface.set_text_area_resize_policy(
            window_geometry_is_window_manager_owned(window)
                ? VNM_TerminalSurface::Text_area_resize_policy::DISABLED
                : VNM_TerminalSurface::Text_area_resize_policy::APPLICATION_CONTROLLED);
    };

    QObject::connect(
        &window,
        &QWindow::windowStateChanged,
        &surface,
        [apply_policy](Qt::WindowState) {
            apply_policy();
        });
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::text_area_resize_requested,
        &window,
        [&window, &surface](int rows, int columns) {
            if (resize_window_for_text_area_request(window, surface, rows, columns)) {
                // The window was resized, so a geometry change is on its way and
                // VNM_TerminalSurface::geometryChange re-derives the grid when it
                // lands. Reconciling here instead would be wrong, not redundant:
                // QWindowsGuiEventDispatcher::sendPostedEvents drains Qt posted
                // events before window-system events, and a plain resize does not
                // flush its geometry change synchronously, so a posted refresh
                // runs while the item is still the old size. It would re-derive
                // the pre-request grid, resize the pty backwards, and let the
                // real geometry change resize it forwards again.
                return;
            }

            // Declined, so no geometry change is coming, and the model has
            // already moved the grid. This is the only thing that can put it
            // back. Deferred so the resize does not nest inside the session
            // notification delivery this handler was called from.
            if (surface.width() <= 0.0 || surface.height() <= 0.0) {
                return;
            }

            const vnm::qt::Post_result post_result =
                vnm::qt::post(&surface, [&surface] {
                    surface.refresh_grid_from_item_geometry();
                });
            if (post_result != vnm::qt::Post_result::QUEUED) {
                // Dropping the reconciliation would leave the grid off the item
                // for good. Nesting is the lesser cost.
                surface.refresh_grid_from_item_geometry();
            }
        });

    apply_policy();
}

QString visible_terminal_title(QString terminal_title)
{
    terminal_title = terminal_title.trimmed();
    return terminal_title.isEmpty() ? default_window_title() : terminal_title;
}

namespace {

void apply_window_title(
    QQuickWindow& window,
    Terminal_qml_chrome* titlebar,
    const QString& window_title,
    const Terminal_title_content& titlebar_content)
{
    window.setTitle(window_title);
    if (titlebar != nullptr) {
        titlebar->set_title(titlebar_content.display_title);
        titlebar->set_activity_marker_text(activity_marker_text(titlebar_content));
    }
}

} // namespace

void sync_terminal_title(
    QQuickWindow&                  window,
    Terminal_qml_chrome*           titlebar,
    const QString&                 terminal_title,
    const QString&                 terminal_icon_name)
{
    const QString visible_title = visible_terminal_title(terminal_title);
    apply_window_title(
        window,
        titlebar,
        visible_title,
        derive_terminal_title_content(visible_title, terminal_icon_name));
}

void connect_terminal_metadata_to_chrome(
    VNM_TerminalSurface&           surface,
    QQuickWindow&                  window,
    Terminal_qml_chrome*           titlebar)
{
    auto user_title = std::make_shared<std::optional<QString>>();
    auto sync_metadata = [titlebar, &window, &surface, user_title] {
        if (user_title->has_value()) {
            Terminal_title_content content = derive_terminal_title_content(
                visible_terminal_title(surface.terminal_title()),
                surface.terminal_icon_name());
            content.display_title = **user_title;
            apply_window_title(
                window,
                titlebar,
                **user_title,
                content);
            return;
        }

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
    if (titlebar != nullptr) {
        QObject::connect(
            titlebar,
            &Terminal_qml_chrome::title_edit_accepted,
            &window,
            [user_title, sync_metadata](const QString& title) {
                *user_title = title;
                sync_metadata();
            });
    }
    sync_metadata();
}

void connect_row_timestamp_tooltip_to_chrome(
    VNM_TerminalSurface&           surface,
    Terminal_qml_chrome*           titlebar)
{
    // Without the built-in chrome there is no overlay layer to host the
    // tooltip, so the surface's hover signals stay unconsumed.
    if (titlebar == nullptr) {
        return;
    }

    QObject::connect(
        &surface,
        &VNM_TerminalSurface::row_timestamp_tooltip_requested,
        titlebar,
        [titlebar, &surface](qreal x, qreal y, const QDateTime& timestamp) {
            // The surface reports the pointer in its own item coordinates;
            // the chrome root spans the window, so map before anchoring.
            titlebar->show_row_timestamp_tooltip(
                surface.mapToItem(titlebar->root_item(), QPointF(x, y)),
                timestamp);
        });
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::row_timestamp_tooltip_dismissed,
        titlebar,
        [titlebar] {
            titlebar->hide_row_timestamp_tooltip();
        });
}

void sync_chrome_window_state(
    Terminal_qml_chrome&           titlebar,
    QQuickWindow&                  window)
{
    const Qt::WindowStates states = window.windowStates();
    titlebar.set_active(window.isActive());
    titlebar.set_maximized(
        states.testFlag(Qt::WindowMaximized) ||
        states.testFlag(Qt::WindowFullScreen));
    titlebar.set_fullscreen(states.testFlag(Qt::WindowFullScreen));
    titlebar.set_resize_enabled(custom_titlebar_resize_border_active(window));
    window.setColor(terminal_chrome_background_color(window.isActive()));
}

} // namespace vnm_terminal::terminal_app
