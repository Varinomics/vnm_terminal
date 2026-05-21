#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <Qt>
#include <QObject>
#include <QPointer>

#include <functional>
#include <vector>

class QEvent;
class QMouseEvent;
class QWindow;

namespace vnm_terminal::terminal_app {

constexpr qreal k_default_frameless_resize_border_width = 6.0;

// Exclusion rectangles are in window-local logical coordinates.
using Frameless_resize_button_rects_provider = std::function<std::vector<QRectF>()>;

Qt::Edges frameless_resize_edges_at(
    const QSizeF&              window_size,
    const QPointF&             point,
    qreal                      border_width,
    const std::vector<QRectF>& button_exclusion_rects = {});

Qt::CursorShape frameless_resize_cursor_shape(
    Qt::Edges                  edges);

class Frameless_resize_filter : public QObject
{
public:
    explicit Frameless_resize_filter(QWindow* window, QObject* parent = nullptr);

    QWindow* window() const;
    qreal resize_border_width() const;
    bool has_resize_cursor_override() const;
    Qt::CursorShape resize_cursor_shape() const;

    void set_resize_border_width(
        qreal          border_width);

    void set_button_exclusion_rects_provider(
        Frameless_resize_button_rects_provider
                       provider);

    bool resize_enabled() const;
    Qt::Edges resize_edges_at(const QPointF& point) const;

    bool eventFilter(QObject* watched, QEvent* event) override;

protected:
    virtual bool start_system_resize(Qt::Edges edges);

private:
    std::vector<QRectF> button_exclusion_rects() const;
    bool handle_mouse_event(QMouseEvent& event);
    void set_resize_cursor(Qt::CursorShape cursor_shape);
    void clear_resize_cursor();

    QPointer<QWindow>                       m_window;
    qreal                                   m_resize_border_width        = k_default_frameless_resize_border_width;
    Frameless_resize_button_rects_provider  m_button_exclusion_rects_provider;
    bool                                    m_has_resize_cursor_override = false;
    Qt::CursorShape                         m_resize_cursor_shape        = Qt::ArrowCursor;
};

} // namespace vnm_terminal::terminal_app
