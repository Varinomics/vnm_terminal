#include "frameless_resize_filter.h"

#include <QCursor>
#include <QEvent>
#include <QMouseEvent>
#include <QWindow>

#include <algorithm>
#include <utility>

namespace chrome = vnm_terminal::terminal_app;

namespace {

bool contains_point(const std::vector<QRectF>& rects, const QPointF& point)
{
    return std::any_of(
        rects.begin(),
        rects.end(),
        [&](const QRectF& rect) {
            return rect.normalized().contains(point);
        });
}

} // namespace

Qt::Edges chrome::frameless_resize_edges_at(
    const QSizeF&              window_size,
    const QPointF&             point,
    qreal                      border_width,
    const std::vector<QRectF>& button_exclusion_rects)
{
    const qreal width  = window_size.width();
    const qreal height = window_size.height();
    if (width <= 0.0 || height <= 0.0 || border_width <= 0.0) {
        return {};
    }

    if (point.x() <  0.0    || point.y() <  0.0 ||
        point.x() >= width  || point.y() >= height)
    {
        return {};
    }

    if (contains_point(button_exclusion_rects, point)) {
        return {};
    }

    const qreal horizontal_border = std::min(border_width, width / 2.0);
    const qreal vertical_border   = std::min(border_width, height / 2.0);

    Qt::Edges edges;
    if (point.x() < horizontal_border) {
        edges |= Qt::LeftEdge;
    }
    else
    if (point.x() >= width - horizontal_border) {
        edges |= Qt::RightEdge;
    }

    if (point.y() < vertical_border) {
        edges |= Qt::TopEdge;
    }
    else
    if (point.y() >= height - vertical_border) {
        edges |= Qt::BottomEdge;
    }

    return edges;
}

Qt::CursorShape chrome::frameless_resize_cursor_shape(Qt::Edges edges)
{
    const bool left   = edges.testFlag(Qt::LeftEdge);
    const bool right  = edges.testFlag(Qt::RightEdge);
    const bool top    = edges.testFlag(Qt::TopEdge);
    const bool bottom = edges.testFlag(Qt::BottomEdge);

    if ((left && top) || (right && bottom)) {
        return Qt::SizeFDiagCursor;
    }

    if ((right && top) || (left && bottom)) {
        return Qt::SizeBDiagCursor;
    }

    if (left || right) {
        return Qt::SizeHorCursor;
    }

    if (top || bottom) {
        return Qt::SizeVerCursor;
    }

    return Qt::ArrowCursor;
}

chrome::Frameless_resize_filter::Frameless_resize_filter(
    QWindow*   window,
    QObject*   parent)
:
    QObject(parent),
    m_window(window)
{}

QWindow* chrome::Frameless_resize_filter::window() const
{
    return m_window.data();
}

qreal chrome::Frameless_resize_filter::resize_border_width() const
{
    return m_resize_border_width;
}

bool chrome::Frameless_resize_filter::has_resize_cursor_override() const
{
    return m_has_resize_cursor_override;
}

Qt::CursorShape chrome::Frameless_resize_filter::resize_cursor_shape() const
{
    return m_resize_cursor_shape;
}

void chrome::Frameless_resize_filter::set_resize_border_width(qreal border_width)
{
    m_resize_border_width = std::max<qreal>(0.0, border_width);
    if (m_resize_border_width <= 0.0) {
        clear_resize_cursor();
    }
}

void chrome::Frameless_resize_filter::set_button_exclusion_rects_provider(
    Frameless_resize_button_rects_provider provider)
{
    m_button_exclusion_rects_provider = std::move(provider);
}

bool chrome::Frameless_resize_filter::resize_enabled() const
{
    const QWindow* target_window = window();
    if (target_window == nullptr) {
        return false;
    }

    const Qt::WindowStates states = target_window->windowStates();
    return
        !states.testFlag(Qt::WindowMaximized) &&
        !states.testFlag(Qt::WindowMinimized) &&
        !states.testFlag(Qt::WindowFullScreen);
}

Qt::Edges chrome::Frameless_resize_filter::resize_edges_at(const QPointF& point) const
{
    const QWindow* target_window = window();
    if (target_window == nullptr || !resize_enabled()) {
        return {};
    }

    return
        frameless_resize_edges_at(
            QSizeF(target_window->width(), target_window->height()),
            point,
            m_resize_border_width,
            button_exclusion_rects());
}

bool chrome::Frameless_resize_filter::eventFilter(QObject* watched, QEvent* event)
{
    QWindow* target_window = window();
    if (target_window == nullptr || watched != target_window || event == nullptr) {
        return false;
    }

    switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseMove:
            return handle_mouse_event(*static_cast<QMouseEvent*>(event));

        case QEvent::Leave:
            clear_resize_cursor();
            return false;

        case QEvent::WindowStateChange:
            if (!resize_enabled()) {
                clear_resize_cursor();
            }
            return false;

        default:
            return false;
    }
}

bool chrome::Frameless_resize_filter::start_system_resize(Qt::Edges edges)
{
    QWindow* target_window = window();
    if (target_window == nullptr) {
        return false;
    }

    return target_window->startSystemResize(edges);
}

std::vector<QRectF> chrome::Frameless_resize_filter::button_exclusion_rects() const
{
    return m_button_exclusion_rects_provider
        ? m_button_exclusion_rects_provider()
        : std::vector<QRectF>{};
}

bool chrome::Frameless_resize_filter::handle_mouse_event(QMouseEvent& event)
{
    const Qt::Edges edges = resize_edges_at(event.position());
    if (edges == Qt::Edges{}) {
        clear_resize_cursor();
        return false;
    }

    set_resize_cursor(frameless_resize_cursor_shape(edges));

    if (event.type() == QEvent::MouseButtonPress) {
        if (event.button() != Qt::LeftButton) {
            return false;
        }

        event.accept();
        // A compositor refusal still consumes the press. The custom chrome
        // must not fall back to synthetic dragging after Qt rejects the resize.
        const bool started = start_system_resize(edges);
        (void)started;
        return true;
    }

    if (event.type() == QEvent::MouseButtonDblClick) {
        if (event.button() != Qt::LeftButton) {
            return false;
        }

        event.accept();
        return true;
    }

    return false;
}

void chrome::Frameless_resize_filter::set_resize_cursor(Qt::CursorShape cursor_shape)
{
    QWindow* target_window = window();
    if (target_window == nullptr) {
        return;
    }

    if (m_has_resize_cursor_override && m_resize_cursor_shape == cursor_shape) {
        return;
    }

    target_window->setCursor(QCursor(cursor_shape));
    m_resize_cursor_shape        = cursor_shape;
    m_has_resize_cursor_override = true;
}

void chrome::Frameless_resize_filter::clear_resize_cursor()
{
    QWindow* target_window = window();
    if (target_window == nullptr || !m_has_resize_cursor_override) {
        return;
    }

    target_window->unsetCursor();
    m_resize_cursor_shape        = Qt::ArrowCursor;
    m_has_resize_cursor_override = false;
}
