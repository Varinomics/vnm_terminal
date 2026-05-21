#include "terminal_scrollbar.h"

#include <QColor>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace scrollbar = vnm_terminal::terminal_app;

namespace {

constexpr qreal k_track_vertical_inset        = 3.0;
constexpr qreal k_track_width                 = 4.0;
constexpr qreal k_thumb_min_height            = 24.0;
constexpr qreal k_thumb_radius                = 2.0;
constexpr qreal k_angle_delta_per_wheel_step  = 120.0;
constexpr qreal k_font_zoom_min_pixel_size    = 6.0;
constexpr qreal k_font_zoom_max_pixel_size    = 72.0;
constexpr qreal k_font_zoom_wheel_step        = 1.0;
constexpr int   k_scroll_lines_per_wheel_step = 3;

QColor track_color(bool active)
{
    return active
        ? QColor(78, 88, 101, 90)
        : QColor(48, 56, 66, 70);
}

QColor thumb_color(bool active)
{
    return active
        ? QColor(183, 192, 206, 205)
        : QColor(132, 143, 158, 170);
}

QColor terminal_background_color(const VNM_TerminalSurface* surface)
{
    if (surface                                                                      != nullptr &&
        surface->color_theme().compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0)
    {
        return QColor(246, 247, 242);
    }

    return QColor(0, 0, 0);
}

bool has_vertical_wheel_delta(const QWheelEvent& event)
{
    return event.angleDelta().y() != 0 || event.pixelDelta().y() != 0;
}

} // namespace

scrollbar::Terminal_scrollbar::Terminal_scrollbar(QQuickItem* parent)
:
    QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setAntialiasing(true);
}

VNM_TerminalSurface* scrollbar::Terminal_scrollbar::surface() const
{
    return m_surface;
}

void scrollbar::Terminal_scrollbar::set_surface(VNM_TerminalSurface* surface)
{
    if (m_surface == surface) {
        return;
    }

    QObject::disconnect(m_viewport_connection);
    QObject::disconnect(m_grid_connection);
    QObject::disconnect(m_theme_connection);
    QObject::disconnect(m_destroyed_connection);
    m_surface                      = surface;
    m_wheel_scroll_angle_remainder = 0.0;
    m_wheel_scroll_pixel_remainder = 0.0;
    m_wheel_zoom_angle_remainder   = 0.0;
    m_wheel_zoom_pixel_remainder   = 0.0;

    if (m_surface != nullptr) {
        m_viewport_connection = QObject::connect(
            m_surface,
            &VNM_TerminalSurface::viewport_changed,
            this,
            [this] {
                sync_from_surface();
            });
        m_grid_connection = QObject::connect(
            m_surface,
            &VNM_TerminalSurface::grid_geometry_changed,
            this,
            [this] {
                sync_from_surface();
            });
        m_theme_connection = QObject::connect(
            m_surface,
            &VNM_TerminalSurface::color_theme_changed,
            this,
            [this] {
                update();
            });
        m_destroyed_connection = QObject::connect(
            m_surface,
            &QObject::destroyed,
            this,
            [this] {
                m_surface = nullptr;
                set_drag_active(false);
                set_viewport_state(0, 0, 0);
            });
    }

    sync_from_surface();
}

bool scrollbar::Terminal_scrollbar::scrollbar_visible() const
{
    return
        m_scrollback_rows > 0   &&
        m_visible_rows    > 0   &&
        width()           > 0.0 &&
        height()          > 0.0;
}

QRectF scrollbar::Terminal_scrollbar::thumb_rect() const
{
    if (!scrollbar_visible()) {
        return {};
    }

    const QRectF track              = track_rect();
    const qreal  thumb_height_value = thumb_height(track.height());
    const qreal  travel             = std::max<qreal>(0.0, track.height() - thumb_height_value);
    const int    bounded_offset     =
        std::clamp(m_offset_from_tail, 0, std::max(0, m_scrollback_rows));
    const qreal top_ratio = m_scrollback_rows > 0
        ? static_cast<qreal>(m_scrollback_rows - bounded_offset) /
            static_cast<qreal>(m_scrollback_rows)
        : 1.0;

    return QRectF(
        track.left(),
        track.top() + top_ratio * travel,
        track.width(),
        thumb_height_value);
}

void scrollbar::Terminal_scrollbar::paint(QPainter* painter)
{
    if (painter == nullptr) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->fillRect(boundingRect(), terminal_background_color(m_surface));

    if (!scrollbar_visible()) {
        painter->restore();
        return;
    }

    const bool   active = m_hovered || m_drag_active;
    const QRectF track  = track_rect();
    painter->setBrush(track_color(active));
    painter->drawRoundedRect(track, k_thumb_radius, k_thumb_radius);

    painter->setBrush(thumb_color(active));
    painter->drawRoundedRect(thumb_rect(), k_thumb_radius, k_thumb_radius);
    painter->restore();
}

void scrollbar::Terminal_scrollbar::geometryChange(
    const QRectF&  new_geometry,
    const QRectF&  old_geometry)
{
    QQuickPaintedItem::geometryChange(new_geometry, old_geometry);
    update();
}

void scrollbar::Terminal_scrollbar::hoverMoveEvent(QHoverEvent* event)
{
    set_hovered(scrollbar_visible() && boundingRect().contains(event->position()));
    QQuickPaintedItem::hoverMoveEvent(event);
}

void scrollbar::Terminal_scrollbar::hoverLeaveEvent(QHoverEvent* event)
{
    set_hovered(false);
    QQuickPaintedItem::hoverLeaveEvent(event);
}

void scrollbar::Terminal_scrollbar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !scrollbar_visible()) {
        event->ignore();
        return;
    }

    const QRectF thumb = thumb_rect();
    const qreal grab_offset = thumb.contains(event->position())
        ? event->position().y() - thumb.top()
        : thumb.height() / 2.0;

    set_drag_active(true);
    m_drag_grab_offset_y = std::clamp(grab_offset, 0.0, thumb.height());
    (void)apply_offset_from_position(event->position().y(), m_drag_grab_offset_y);
    event->accept();
}

void scrollbar::Terminal_scrollbar::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_drag_active) {
        event->ignore();
        return;
    }

    (void)apply_offset_from_position(event->position().y(), m_drag_grab_offset_y);
    event->accept();
}

void scrollbar::Terminal_scrollbar::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_drag_active) {
        event->ignore();
        return;
    }

    (void)apply_offset_from_position(event->position().y(), m_drag_grab_offset_y);
    set_drag_active(false);
    event->accept();
}

void scrollbar::Terminal_scrollbar::mouseUngrabEvent()
{
    set_drag_active(false);
    QQuickPaintedItem::mouseUngrabEvent();
}

void scrollbar::Terminal_scrollbar::wheelEvent(QWheelEvent* event)
{
    if (m_surface == nullptr) {
        event->ignore();
        return;
    }

    if ((event->modifiers() & Qt::ControlModifier) != 0) {
        if (zoom_surface_from_wheel(event)) {
            event->accept();
        }
        else {
            event->ignore();
        }
        return;
    }

    if (!scrollbar_visible()) {
        event->ignore();
        return;
    }

    if (scroll_surface_from_wheel(event)) {
        event->accept();
    }
    else {
        event->ignore();
    }
}

QRectF scrollbar::Terminal_scrollbar::track_rect() const
{
    const qreal track_width = std::min(k_track_width, std::max<qreal>(0.0, width()));
    const qreal left        = std::max<qreal>(0.0, width() - track_width);
    const qreal top         = std::min(k_track_vertical_inset, std::max<qreal>(0.0, height() / 2.0));
    return QRectF(
        left,
        top,
        track_width,
        std::max<qreal>(0.0, height() - top * 2.0));
}

qreal scrollbar::Terminal_scrollbar::thumb_height(qreal track_height) const
{
    if (track_height <= 0.0 || m_visible_rows <= 0) {
        return 0.0;
    }

    const qreal total_rows =
        static_cast<qreal>(std::max(1, m_scrollback_rows + m_visible_rows));
    const qreal proportional_height =
        track_height * static_cast<qreal>(m_visible_rows) / total_rows;
    return std::clamp(
        proportional_height,
        std::min(k_thumb_min_height, track_height),
        track_height);
}

int scrollbar::Terminal_scrollbar::offset_from_thumb_top(qreal thumb_top) const
{
    if (!scrollbar_visible()) {
        return 0;
    }

    const QRectF track = track_rect();
    const qreal travel =
        std::max<qreal>(0.0, track.height() - thumb_height(track.height()));
    if (travel <= 0.0 || m_scrollback_rows <= 0) {
        return 0;
    }

    const qreal clamped_top = std::clamp(
        thumb_top,
        track.top(),
        track.top() + travel);
    const qreal top_ratio = (clamped_top - track.top()) / travel;
    const qreal target_offset =
        static_cast<qreal>(m_scrollback_rows) * (1.0 - top_ratio);
    return std::clamp(
        static_cast<int>(std::llround(target_offset)),
        0,
        m_scrollback_rows);
}

bool scrollbar::Terminal_scrollbar::apply_offset_from_position(
    qreal              position_y,
    qreal              grab_offset_y)
{
    if (m_surface == nullptr) {
        return false;
    }

    const int  target_offset = offset_from_thumb_top(position_y - grab_offset_y);
    const bool moved         = m_surface->scroll_to_offset_from_tail(target_offset);
    sync_from_surface();
    return moved;
}

int scrollbar::Terminal_scrollbar::vertical_wheel_steps(
    const QWheelEvent& event,
    qreal              pixel_step_size,
    qreal&             angle_remainder,
    qreal&             pixel_remainder)
{
    const auto steps_from_delta = [](int delta, qreal step_size, qreal& remainder) {
        if (delta == 0 || !std::isfinite(step_size) || step_size <= 0.0) {
            return 0;
        }

        remainder += static_cast<qreal>(delta);
        const int steps = static_cast<int>(std::trunc(remainder / step_size));
        remainder -= static_cast<qreal>(steps) * step_size;
        return steps;
    };

    const int angle_delta = event.angleDelta().y();
    if (angle_delta != 0) {
        pixel_remainder = 0.0;
        return steps_from_delta(
            angle_delta,
            k_angle_delta_per_wheel_step,
            angle_remainder);
    }

    const int pixel_delta = event.pixelDelta().y();
    if (pixel_delta == 0) {
        return 0;
    }

    angle_remainder = 0.0;
    return steps_from_delta(pixel_delta, pixel_step_size, pixel_remainder);
}

bool scrollbar::Terminal_scrollbar::zoom_surface_from_wheel(QWheelEvent* event)
{
    if (!has_vertical_wheel_delta(*event)) {
        return false;
    }

    const int steps = vertical_wheel_steps(
        *event,
        k_angle_delta_per_wheel_step,
        m_wheel_zoom_angle_remainder,
        m_wheel_zoom_pixel_remainder);
    if (steps == 0) {
        return true;
    }

    const qreal requested_font_size = std::clamp(
        m_surface->font_size() + static_cast<qreal>(steps) * k_font_zoom_wheel_step,
        k_font_zoom_min_pixel_size,
        k_font_zoom_max_pixel_size);
    m_surface->set_font_size(requested_font_size);
    return true;
}

bool scrollbar::Terminal_scrollbar::scroll_surface_from_wheel(QWheelEvent* event)
{
    if (!has_vertical_wheel_delta(*event)) {
        return false;
    }

    const int raw_delta = event->angleDelta().y() != 0
        ? event->angleDelta().y()
        : event->pixelDelta().y();
    if ((raw_delta > 0 && m_offset_from_tail >= m_scrollback_rows) ||
        (raw_delta < 0 && m_offset_from_tail <= 0))
    {
        m_wheel_scroll_angle_remainder = 0.0;
        m_wheel_scroll_pixel_remainder = 0.0;
        return false;
    }

    const qreal pixel_step_size = m_visible_rows > 0
        ? std::max<qreal>(1.0, height() / static_cast<qreal>(m_visible_rows))
        : 1.0;
    const int wheel_steps = vertical_wheel_steps(
        *event,
        pixel_step_size,
        m_wheel_scroll_angle_remainder,
        m_wheel_scroll_pixel_remainder);
    if (wheel_steps == 0) {
        return true;
    }

    const int line_delta = event->angleDelta().y() != 0
        ? wheel_steps * k_scroll_lines_per_wheel_step
        : wheel_steps;
    const bool moved = m_surface->scroll_viewport_lines(line_delta);
    sync_from_surface();
    return moved;
}

void scrollbar::Terminal_scrollbar::set_drag_active(bool active)
{
    if (m_drag_active == active) {
        return;
    }

    m_drag_active = active;
    setKeepMouseGrab(active);
    update();
}

void scrollbar::Terminal_scrollbar::set_hovered(bool hovered)
{
    if (m_hovered == hovered) {
        return;
    }

    m_hovered = hovered;
    update();
}

void scrollbar::Terminal_scrollbar::sync_from_surface()
{
    if (m_surface == nullptr) {
        set_viewport_state(0, 0, 0);
        return;
    }

    set_viewport_state(
        m_surface->scrollback_rows(),
        m_surface->viewport_visible_rows(),
        m_surface->viewport_offset_from_tail());
}

void scrollbar::Terminal_scrollbar::set_viewport_state(
    int    scrollback_rows,
    int    visible_rows,
    int    offset_from_tail)
{
    scrollback_rows  = std::max(0, scrollback_rows);
    visible_rows     = std::max(0, visible_rows);
    offset_from_tail = std::clamp(offset_from_tail, 0, scrollback_rows);

    if (m_scrollback_rows  == scrollback_rows &&
        m_visible_rows     == visible_rows    &&
        m_offset_from_tail == offset_from_tail)
    {
        return;
    }

    m_scrollback_rows  = scrollback_rows;
    m_visible_rows     = visible_rows;
    m_offset_from_tail = offset_from_tail;
    if (!scrollbar_visible()) {
        set_drag_active(false);
    }
    update();
}
