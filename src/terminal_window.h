#pragma once

#include "app_options.h"
#include "qml_chrome.h"
#include "terminal_scrollbar.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QObject>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QtGlobal>

class QEvent;
class QQuickWindow;

namespace vnm_terminal::terminal_app {

constexpr qreal k_custom_titlebar_height             = 32.0;
constexpr qreal k_custom_titlebar_physical_reduction = 2.0;
constexpr qreal k_terminal_scrollbar_width           = 12.0;
constexpr int   k_text_area_resize_max_rows    = 512;
constexpr int   k_text_area_resize_max_columns = 512;

QString default_window_title();

struct Terminal_content_geometry
{
    QRectF             content_interior_rect;
    QRectF             terminal_rect;
    QRectF             scrollbar_rect;
};

class Wheel_delivery_indicator_filter final : public QObject
{
public:
    explicit Wheel_delivery_indicator_filter(Terminal_qml_chrome& titlebar);

protected:
    bool eventFilter(QObject*, QEvent* event) override;

private:
    Terminal_qml_chrome& m_titlebar;
};

void install_wheel_delivery_indicator_filter(
    VNM_TerminalSurface&             surface,
    Terminal_scrollbar&              scrollbar,
    Terminal_qml_chrome*             titlebar,
    bool                             enabled);

void split_terminal_area(
    Terminal_content_geometry& geometry,
    const QRectF&            area);

void snap_terminal_content_geometry(
    Terminal_content_geometry& geometry,
    qreal                    device_pixel_ratio);

Terminal_content_geometry terminal_content_geometry(
    const QRectF& content_interior_rect,
    qreal         device_pixel_ratio);

// True while the window manager, not the application, decides this window's
// geometry: maximized, fullscreen, or minimized. Client-driven geometry changes
// have to be refused in those states.
bool window_geometry_is_window_manager_owned(const QQuickWindow& window);

bool custom_titlebar_resize_border_active(const QQuickWindow& window);

// Applies the text-area resize policy to the surface for the window's current
// state, and keeps it applied as the state changes. While the window manager
// owns the geometry the surface ignores XTWINOPS outright, so no grid change
// and no pty resize happen behind a request this host cannot honor.
void connect_text_area_resize_policy(
    QQuickWindow&                  window,
    VNM_TerminalSurface&           surface);

void apply_terminal_shell_geometry(
    QQuickWindow&                  window,
    VNM_TerminalSurface&           surface,
    Terminal_scrollbar&            scrollbar,
    Terminal_qml_chrome*           titlebar,
    bool                           custom_titlebar);

void apply_synchronized_output_scroll_policy_option(
    VNM_TerminalSurface& surface,
    const App_options&   options);

void apply_primary_repaint_recovery_option(
    VNM_TerminalSurface& surface,
    const App_options&   options);

void apply_scrollback_limit_option(
    VNM_TerminalSurface& surface,
    const App_options&   options);

void apply_retained_history_capacity_option(
    VNM_TerminalSurface& surface,
    const App_options&   options);

bool resize_window_for_text_area_request(
    QQuickWindow&                  window,
    const VNM_TerminalSurface&     surface,
    int                            rows,
    int                            columns);

QString visible_terminal_title(QString terminal_title);

void sync_terminal_title(
    QQuickWindow&                  window,
    Terminal_qml_chrome*           titlebar,
    const QString&                 terminal_title,
    const QString&                 terminal_icon_name);

void connect_terminal_metadata_to_chrome(
    VNM_TerminalSurface&           surface,
    QQuickWindow&                  window,
    Terminal_qml_chrome*           titlebar);

void connect_row_timestamp_tooltip_to_chrome(
    VNM_TerminalSurface&           surface,
    Terminal_qml_chrome*           titlebar);

void sync_chrome_window_state(
    Terminal_qml_chrome&           titlebar,
    QQuickWindow&                  window);

} // namespace vnm_terminal::terminal_app
