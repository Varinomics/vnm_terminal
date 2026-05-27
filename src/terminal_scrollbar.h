#pragma once

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QMetaObject>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QRectF>

class QHoverEvent;
class QMouseEvent;
class QPainter;
class QWheelEvent;

namespace vnm_terminal::terminal_app {

class Terminal_scrollbar final : public QQuickPaintedItem
{
public:
    explicit Terminal_scrollbar(QQuickItem* parent = nullptr);

    VNM_TerminalSurface* surface() const;
    void set_surface(VNM_TerminalSurface* surface);

    bool wheel_trace_enabled() const;
    void set_wheel_trace_enabled(bool enabled);

    bool scrollbar_visible() const;
    QRectF thumb_rect() const;

    void paint(QPainter* painter) override;

protected:
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void wheelEvent(QWheelEvent* event) override;

private:
    QRectF track_rect() const;

    qreal  thumb_height(
        qreal                  track_height) const;

    int    offset_from_thumb_top(
        qreal                  thumb_top) const;

    bool   apply_offset_from_position(
        qreal                  position_y,
        qreal                  grab_offset_y);

    int    vertical_wheel_steps(
        const QWheelEvent&     event,
        qreal                  pixel_step_size,
        qreal&                 angle_remainder,
        qreal&                 pixel_remainder);

    bool   zoom_surface_from_wheel(
        QWheelEvent*           event);

    bool   scroll_surface_from_wheel(
        QWheelEvent*           event);

    void   record_wheel_trace_event(
        const QWheelEvent&     event,
        const QString&         route,
        const QString&         outcome,
        bool                   accepted,
        int                    wheel_steps,
        int                    effective_line_delta,
        qreal                  angle_remainder,
        qreal                  pixel_remainder,
        int                    backend_drain_calls = 0,
        qint64                 backend_drain_elapsed_ns = 0,
        bool                   local_scroll_intent_recorded = false,
        const QString&         local_scroll_block_reason = {},
        const QString&         scroll_action = {},
        int                    applied_line_delta = 0) const;

    void   set_drag_active(
        bool                   active);

    void   set_hovered(
        bool                   hovered);

    void sync_from_surface();

    void   set_viewport_state(
        int                    scrollback_rows,
        int                    visible_rows,
        int                    offset_from_tail);

    QPointer<VNM_TerminalSurface>  m_surface;
    QMetaObject::Connection        m_viewport_connection;
    QMetaObject::Connection        m_grid_connection;
    QMetaObject::Connection        m_theme_connection;
    QMetaObject::Connection        m_destroyed_connection;
    int                            m_scrollback_rows              = 0;
    int                            m_visible_rows                 = 0;
    int                            m_offset_from_tail             = 0;
    qreal                          m_drag_grab_offset_y           = 0.0;
    qreal                          m_wheel_scroll_angle_remainder = 0.0;
    qreal                          m_wheel_scroll_pixel_remainder = 0.0;
    qreal                          m_wheel_zoom_angle_remainder   = 0.0;
    qreal                          m_wheel_zoom_pixel_remainder   = 0.0;
    bool                           m_drag_active                  = false;
    bool                           m_hovered                      = false;
    bool                           m_wheel_trace_enabled          = false;
};

} // namespace vnm_terminal::terminal_app
