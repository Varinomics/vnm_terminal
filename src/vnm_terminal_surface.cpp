#include "vnm_terminal/vnm_terminal_surface.h"

#include "vnm_terminal/internal/backend_contract.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/linux_pty_backend.h"
#include "vnm_terminal/internal/qsg_terminal_render_frame.h"
#include "vnm_terminal/internal/qsg_terminal_renderer.h"
#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/terminal_input_encoder.h"
#include "vnm_terminal/internal/terminal_resize_controller.h"
#include "vnm_terminal/internal/terminal_session.h"
#include "vnm_terminal/internal/unicode_width.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/internal/windows_conpty_backend.h"
#include <QColor>
#include <QClipboard>
#include <QFont>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPointer>
#include <QQuickWindow>
#include <QScreen>
#include <QStringList>
#include <QThread>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

// The trace remains diagnostic; public signal delivery drains the session's
// durable notification channel during GUI-thread sync.
constexpr std::size_t k_surface_notification_trace_limit         = 1024U;
constexpr qreal       k_font_zoom_min_pixel_size                 = 6.0;
constexpr qreal       k_font_zoom_max_pixel_size                 = 72.0;
constexpr qreal       k_font_zoom_wheel_step                     = 1.0;
constexpr qreal       k_angle_delta_per_wheel_step               = 120.0;
constexpr int         k_plain_scroll_lines_per_angle_step        = 3;
constexpr int         k_min_synchronized_output_stale_timeout_ms = 1;

bool same_grid_size(term::terminal_grid_size_t left, term::terminal_grid_size_t right)
{
    return left.rows == right.rows && left.columns == right.columns;
}

bool same_viewport_row_identity_space(
    const term::Terminal_render_snapshot&  left,
    const term::Terminal_render_snapshot&  right)
{
    return
        same_grid_size(left.grid_size, right.grid_size)                  &&
        left.viewport.active_buffer    == right.viewport.active_buffer   &&
        left.viewport.visible_rows     == right.viewport.visible_rows    &&
        left.viewport.scrollback_rows  == right.viewport.scrollback_rows &&
        left.viewport.offset_from_tail == right.viewport.offset_from_tail;
}

void append_dirty_range_rows(
    std::vector<int>&      rows,
    const std::vector<term::Terminal_render_dirty_row_range>&
                           ranges)
{
    for (const term::Terminal_render_dirty_row_range& range : ranges) {
        for (int row = range.first_row; row < range.first_row + range.row_count; ++row) {
            rows.push_back(row);
        }
    }
}

std::shared_ptr<const term::Terminal_render_snapshot> snapshot_with_coalesced_dirty_rows(
    const term::Terminal_render_snapshot&  pending_snapshot,
    const term::Terminal_render_snapshot&  current_snapshot)
{
    term::Terminal_render_snapshot snapshot = current_snapshot;

    if (!same_viewport_row_identity_space(pending_snapshot, current_snapshot)) {
        snapshot.dirty_row_ranges =
            term::compact_dirty_row_ranges({}, snapshot.grid_size.rows, true);
        return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
    }

    std::vector<int> dirty_rows;
    append_dirty_range_rows(dirty_rows, pending_snapshot.dirty_row_ranges);
    append_dirty_range_rows(dirty_rows, current_snapshot.dirty_row_ranges);
    snapshot.dirty_row_ranges =
        term::compact_dirty_row_ranges(std::move(dirty_rows), snapshot.grid_size.rows, false);
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

bool same_property_value(qreal lhs, qreal rhs)
{
    return lhs == rhs || (std::isnan(lhs) && std::isnan(rhs));
}

qreal normalized_font_pixel_size(qreal font_size)
{
    if (!std::isfinite(font_size) || font_size <= 0.0) {
        return font_size;
    }

    return static_cast<qreal>(std::max(1, static_cast<int>(std::round(font_size))));
}

qreal finite_positive_font_size_or_default(qreal font_size)
{
    if (!std::isfinite(font_size) || font_size <= 0.0) {
        return term::k_vnm_terminal_default_font_pixel_size;
    }

    return font_size;
}

int wheel_steps_from_delta(int delta, qreal step_size, qreal& remainder)
{
    if (delta == 0 || !std::isfinite(step_size) || step_size <= 0.0) {
        return 0;
    }

    remainder += static_cast<qreal>(delta);
    const int steps = static_cast<int>(std::trunc(remainder / step_size));
    remainder -= static_cast<qreal>(steps) * step_size;
    return steps;
}

int vertical_wheel_steps(
    const QWheelEvent& event,
    qreal              pixel_step_size,
    qreal&             angle_remainder,
    qreal&             pixel_remainder)
{
    const int angle_delta = event.angleDelta().y();
    if (angle_delta != 0) {
        pixel_remainder = 0.0;
        return wheel_steps_from_delta(
            angle_delta,
            k_angle_delta_per_wheel_step,
            angle_remainder);
    }

    const int pixel_delta = event.pixelDelta().y();
    if (pixel_delta == 0) {
        return 0;
    }

    angle_remainder = 0.0;
    return wheel_steps_from_delta(pixel_delta, pixel_step_size, pixel_remainder);
}

bool has_vertical_wheel_delta(const QWheelEvent& event)
{
    return event.angleDelta().y() != 0 || event.pixelDelta().y() != 0;
}

int vertical_wheel_direction(const QWheelEvent& event)
{
    const int delta = event.angleDelta().y() != 0
        ? event.angleDelta().y()
        : event.pixelDelta().y();

    if (delta > 0) {
        return 1;
    }

    if (delta < 0) {
        return -1;
    }

    return 0;
}

bool is_plain_copy_shortcut(const QKeyEvent& event)
{
    const Qt::KeyboardModifiers modifiers =
        event.modifiers() &
        (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);

    return event.key() == Qt::Key_C && modifiers == Qt::ControlModifier;
}

bool is_unmodified_page_scroll_key(const QKeyEvent& event)
{
    const Qt::KeyboardModifiers modifiers =
        event.modifiers() &
        (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);

    return modifiers == Qt::NoModifier &&
        (event.key() == Qt::Key_PageUp || event.key() == Qt::Key_PageDown);
}

bool snapshot_has_sgr_mouse_reporting(
    const std::shared_ptr<const term::Terminal_render_snapshot>& snapshot)
{
    return
        snapshot != nullptr                &&
        snapshot->modes.sgr_mouse_encoding &&
        snapshot->modes.mouse_tracking != term::Terminal_mouse_tracking_mode::NONE;
}

bool snapshot_has_terminal_mouse_tracking(
    const std::shared_ptr<const term::Terminal_render_snapshot>& snapshot)
{
    return
        snapshot                       != nullptr &&
        snapshot->modes.mouse_tracking != term::Terminal_mouse_tracking_mode::NONE;
}

bool viewport_state_can_scroll_locally(
    const term::Terminal_viewport_state&   viewport,
    int                                    scroll_direction)
{
    if (viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE) {
        return false;
    }

    if (scroll_direction > 0) {
        return viewport.offset_from_tail < viewport.scrollback_rows;
    }

    if (scroll_direction < 0) {
        return viewport.offset_from_tail > 0;
    }

    return false;
}

QString selected_text_from_visible_snapshot(
    const std::shared_ptr<const term::Terminal_render_snapshot>& snapshot)
{
    if (snapshot == nullptr || snapshot->selection_spans.empty()) {
        return {};
    }

    std::vector<term::Terminal_render_selection_span> spans = snapshot->selection_spans;
    std::sort(
        spans.begin(),
        spans.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.row < rhs.row ||
                (lhs.row == rhs.row && lhs.first_column < rhs.first_column);
        });

    QStringList rows;
    for (const term::Terminal_render_selection_span& span : spans) {
        const int first_column = std::clamp(span.first_column, 0, snapshot->grid_size.columns);
        const int end_column =
            std::clamp(span.first_column + span.column_count, 0, snapshot->grid_size.columns);
        QString row_text;

        for (int column = first_column; column < end_column; ++column) {
            auto cell_it = std::find_if(
                snapshot->cells.begin(),
                snapshot->cells.end(),
                [span, column](const term::Terminal_render_cell& cell) {
                    return cell.position.row == span.row && cell.position.column == column;
                });
            if (cell_it == snapshot->cells.end()) {
                row_text += QLatin1Char(' ');
                continue;
            }

            if (!cell_it->wide_continuation) {
                row_text += cell_it->text;
            }
        }

        if (end_column == snapshot->grid_size.columns) {
            while (!row_text.isEmpty() && row_text.back() == QChar(u' ')) {
                row_text.chop(1);
            }
        }
        rows.push_back(row_text);
    }

    return rows.join(QLatin1Char('\n'));
}

bool is_light_theme(const QString& color_theme)
{
    return color_theme.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0;
}

qreal current_device_pixel_ratio(const QQuickWindow* window)
{
    if (window == nullptr) {
        return 1.0;
    }

    const qreal device_pixel_ratio = window->effectiveDevicePixelRatio();
    if (!std::isfinite(device_pixel_ratio) || device_pixel_ratio <= 0.0) {
        return 1.0;
    }

    return device_pixel_ratio;
}

std::unique_ptr<term::Terminal_backend> make_native_backend()
{
#if defined(_WIN32)
    return term::make_windows_conpty_backend();
#elif defined(__linux__)
    return term::make_linux_pty_backend();
#else
    return nullptr;
#endif
}

VNM_TerminalSurface::Process_state surface_process_state(
    term::Terminal_process_state state)
{
    switch (state) {
        case term::Terminal_process_state::NOT_STARTED: return VNM_TerminalSurface::Process_state::NOT_STARTED;
        case term::Terminal_process_state::STARTING:    return VNM_TerminalSurface::Process_state::STARTING;
        case term::Terminal_process_state::RUNNING:     return VNM_TerminalSurface::Process_state::RUNNING;
        case term::Terminal_process_state::EXITED:      return VNM_TerminalSurface::Process_state::EXITED;
        case term::Terminal_process_state::FAILED:      return VNM_TerminalSurface::Process_state::FAILED;
    }

    return VNM_TerminalSurface::Process_state::FAILED;
}

VNM_TerminalSurface::Exit_reason surface_exit_reason(term::Terminal_exit_reason reason)
{
    switch (reason) {
        case term::Terminal_exit_reason::EXITED:          return VNM_TerminalSurface::Exit_reason::EXITED;
        case term::Terminal_exit_reason::INTERRUPTED:     return VNM_TerminalSurface::Exit_reason::INTERRUPTED;
        case term::Terminal_exit_reason::TERMINATED:      return VNM_TerminalSurface::Exit_reason::TERMINATED;
        case term::Terminal_exit_reason::FAILED_TO_START: return VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
    }

    return VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
}

VNM_TerminalSurface::Backend_error_code surface_backend_error_code(
    term::Terminal_backend_error_code code)
{
    switch (code) {
        case term::Terminal_backend_error_code::INVALID_LAUNCH_CONFIG:
            return VNM_TerminalSurface::Backend_error_code::INVALID_LAUNCH_CONFIG;
        case term::Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE:
            return VNM_TerminalSurface::Backend_error_code::INVALID_INITIAL_GRID_SIZE;
        case term::Terminal_backend_error_code::WORKING_DIRECTORY_UNAVAILABLE:
            return VNM_TerminalSurface::Backend_error_code::WORKING_DIRECTORY_UNAVAILABLE;
        case term::Terminal_backend_error_code::START_FAILED:
            return VNM_TerminalSurface::Backend_error_code::START_FAILED;
        case term::Terminal_backend_error_code::WRITE_FAILED:
            return VNM_TerminalSurface::Backend_error_code::WRITE_FAILED;
        case term::Terminal_backend_error_code::RESIZE_FAILED:
            return VNM_TerminalSurface::Backend_error_code::RESIZE_FAILED;
        case term::Terminal_backend_error_code::INTERRUPT_FAILED:
            return VNM_TerminalSurface::Backend_error_code::INTERRUPT_FAILED;
        case term::Terminal_backend_error_code::TERMINATE_FAILED:
            return VNM_TerminalSurface::Backend_error_code::TERMINATE_FAILED;
        case term::Terminal_backend_error_code::OUTPUT_OVERFLOW:
            return VNM_TerminalSurface::Backend_error_code::OUTPUT_OVERFLOW;
        case term::Terminal_backend_error_code::CALLBACK_MISSING:
            return VNM_TerminalSurface::Backend_error_code::CALLBACK_MISSING;
        case term::Terminal_backend_error_code::READ_FAILED:
            return VNM_TerminalSurface::Backend_error_code::READ_FAILED;
    }

    return VNM_TerminalSurface::Backend_error_code::START_FAILED;
}

bool is_live_process_state(term::Terminal_process_state state)
{
    return
        state == term::Terminal_process_state::STARTING ||
        state == term::Terminal_process_state::RUNNING;
}

bool is_accepted(term::Terminal_session_result_code code)
{
    return code == term::Terminal_session_result_code::ACCEPTED;
}

term::Terminal_paste_framing_policy paste_framing_policy(
    VNM_TerminalSurface::Bracketed_paste_policy policy)
{
    switch (policy) {
        case VNM_TerminalSurface::Bracketed_paste_policy::DISABLED:
            return term::Terminal_paste_framing_policy::DISABLED;
        case VNM_TerminalSurface::Bracketed_paste_policy::APPLICATION_CONTROLLED:
            return term::Terminal_paste_framing_policy::APPLICATION_CONTROLLED;
        case VNM_TerminalSurface::Bracketed_paste_policy::ENABLED:
            return term::Terminal_paste_framing_policy::ENABLED;
    }

    return term::Terminal_paste_framing_policy::APPLICATION_CONTROLLED;
}

term::Terminal_mouse_button terminal_mouse_button(Qt::MouseButton button)
{
    switch (button) {
        case Qt::LeftButton:   return term::Terminal_mouse_button::LEFT;
        case Qt::MiddleButton: return term::Terminal_mouse_button::MIDDLE;
        case Qt::RightButton:  return term::Terminal_mouse_button::RIGHT;
        default:               return term::Terminal_mouse_button::NONE;
    }
}

term::Terminal_mouse_button terminal_mouse_button(Qt::MouseButtons buttons)
{
    if ((buttons & Qt::LeftButton)   != Qt::NoButton) { return term::Terminal_mouse_button::LEFT;   }
    if ((buttons & Qt::MiddleButton) != Qt::NoButton) { return term::Terminal_mouse_button::MIDDLE; }
    if ((buttons & Qt::RightButton)  != Qt::NoButton) { return term::Terminal_mouse_button::RIGHT;  }
    return term::Terminal_mouse_button::NONE;
}

Qt::MouseButton qt_mouse_button(term::Terminal_mouse_button button)
{
    switch (button) {
        case term::Terminal_mouse_button::LEFT:   return Qt::LeftButton;
        case term::Terminal_mouse_button::MIDDLE: return Qt::MiddleButton;
        case term::Terminal_mouse_button::RIGHT:  return Qt::RightButton;
        default:                                  return Qt::NoButton;
    }
}

term::Terminal_mouse_button terminal_mouse_button(
    Qt::MouseButtons               buttons,
    term::Terminal_mouse_button    preferred_button)
{
    const Qt::MouseButton preferred_qt_button = qt_mouse_button(preferred_button);
    if (preferred_qt_button             != Qt::NoButton &&
        (buttons & preferred_qt_button) != Qt::NoButton)
    {
        return preferred_button;
    }

    return terminal_mouse_button(buttons);
}

std::optional<term::terminal_grid_position_t> grid_position_for_local_point(
    const std::shared_ptr<const term::Terminal_render_snapshot>&   snapshot,
    term::terminal_cell_metrics_t                                  metrics,
    QPointF                                                        point)
{
    if (snapshot  == nullptr                           ||
        !term::is_valid_grid_size(snapshot->grid_size) ||
        !term::is_valid_cell_metrics(metrics)          ||
        !std::isfinite(point.x())                      ||
        !std::isfinite(point.y())                      ||
        point.x() <  0.0                               ||
        point.y() <  0.0)
    {
        return std::nullopt;
    }

    const int column = static_cast<int>(std::floor(point.x() / metrics.width));
    const int row    = static_cast<int>(std::floor(point.y() / metrics.height));
    if (row <  0 || column < 0 || row >= snapshot->grid_size.rows ||
        column >= snapshot->grid_size.columns)
    {
        return std::nullopt;
    }

    return term::terminal_grid_position_t{row, column};
}

std::optional<term::terminal_grid_position_t> clamped_grid_position_for_local_point(
    const std::shared_ptr<const term::Terminal_render_snapshot>&   snapshot,
    term::terminal_cell_metrics_t                                  metrics,
    QPointF                                                        point)
{
    if (snapshot == nullptr ||
        !term::is_valid_grid_size(snapshot->grid_size) ||
        !term::is_valid_cell_metrics(metrics) ||
        !std::isfinite(point.x()) ||
        !std::isfinite(point.y()))
    {
        return std::nullopt;
    }

    const int column = std::clamp(
        static_cast<int>(std::floor(point.x() / metrics.width)),
        0,
        snapshot->grid_size.columns - 1);
    const int row = std::clamp(
        static_cast<int>(std::floor(point.y() / metrics.height)),
        0,
        snapshot->grid_size.rows - 1);
    return term::terminal_grid_position_t{row, column};
}

std::optional<term::terminal_grid_position_t> logical_grid_position_for_viewport_cell(
    const std::shared_ptr<const term::Terminal_render_snapshot>&   snapshot,
    term::terminal_grid_position_t                                 viewport_position)
{
    if (snapshot                 == nullptr                  ||
        !term::is_valid_grid_size(snapshot->grid_size)       ||
        viewport_position.row    <  0                        ||
        viewport_position.row    >= snapshot->grid_size.rows ||
        viewport_position.column <  0                        ||
        viewport_position.column >= snapshot->grid_size.columns)
    {
        return std::nullopt;
    }

    const int first_visible_logical_row =
        snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE
            ? 0
            : snapshot->viewport.scrollback_rows - snapshot->viewport.offset_from_tail;
    return term::terminal_grid_position_t{
        first_visible_logical_row + viewport_position.row,
        viewport_position.column,
    };
}

bool position_is_before(
    term::terminal_grid_position_t lhs,
    term::terminal_grid_position_t rhs)
{
    return lhs.row < rhs.row || (lhs.row == rhs.row && lhs.column < rhs.column);
}

term::Terminal_selection_range selection_range_for_drag(
    term::terminal_grid_position_t anchor,
    term::terminal_grid_position_t current)
{
    if (position_is_before(current, anchor)) {
        ++anchor.column;
        return {anchor, current, term::Terminal_selection_mode::NORMAL};
    }

    ++current.column;
    return {anchor, current, term::Terminal_selection_mode::NORMAL};
}

void set_selection_range_for_drag(
    term::Terminal_session&        session,
    term::terminal_grid_position_t anchor,
    term::terminal_grid_position_t current,
    bool                           drag_moved)
{
    if (!drag_moved && current == anchor) {
        session.clear_selection();
        return;
    }

    session.set_selection_range(selection_range_for_drag(anchor, current));
}

bool local_selection_override(Qt::KeyboardModifiers modifiers)
{
    return (modifiers & Qt::ShiftModifier) != Qt::NoModifier;
}

term::Terminal_cursor_shape terminal_cursor_shape(VNM_TerminalSurface::Cursor_style style)
{
    switch (style) {
        case VNM_TerminalSurface::Cursor_style::BLOCK:
            return term::Terminal_cursor_shape::BLOCK;
        case VNM_TerminalSurface::Cursor_style::BAR:
            return term::Terminal_cursor_shape::BAR;
        case VNM_TerminalSurface::Cursor_style::UNDERLINE:
            return term::Terminal_cursor_shape::UNDERLINE;
    }

    return term::Terminal_cursor_shape::BLOCK;
}

term::Terminal_render_options render_options_for_surface(const VNM_TerminalSurface& surface)
{
    term::Terminal_render_options options;
    const bool light_theme = is_light_theme(surface.color_theme());
    if (light_theme) {
        options.default_background   = QColor(246, 247, 242);
        options.default_foreground   = QColor(31,  44,  38);
        options.selection_background = QColor(95, 145, 190, 150);
        options.cursor_color         = QColor(30, 46, 38);
        options.preedit_background   = QColor(220, 220, 205, 170);
        options.visual_bell_color    = QColor(255, 255, 255, 96);
    }

    options.cursor_shape_override         = terminal_cursor_shape(surface.cursor_style());
    options.cursor_blink_enabled_override = surface.cursor_blink_enabled();
    options.visual_bell_enabled =
        surface.visual_bell_policy() == VNM_TerminalSurface::Bell_policy::ENABLED;
    return options;
}

int preedit_cursor_position_from_event(const QInputMethodEvent& event)
{
    const int text_size = static_cast<int>(
        std::min<qsizetype>(event.preeditString().size(), std::numeric_limits<int>::max()));
    int cursor_position = text_size;
    for (const QInputMethodEvent::Attribute& attribute : event.attributes()) {
        if (attribute.type == QInputMethodEvent::Cursor) {
            cursor_position = attribute.start;
            break;
        }
    }

    return std::clamp(cursor_position, 0, text_size);
}

QString terminal_commit_text_from_event(const QInputMethodEvent& event)
{
    // Replacement ranges target widgets with an editable local document. The
    // terminal surface only owns an input byte stream, so non-default ranges
    // are deliberately folded into the commit text and sent as ordinary input.
    return event.commitString();
}

void warm_text_layout(
    const QFont&   font,
    const QString& text)
{
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.setCacheEnabled(false);

    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) {
        line.setLineWidth(1024.0 * 1024.0);
        line.setPosition(QPointF(0.0, 0.0));
    }
    layout.endLayout();
}

void warm_prompt_glyph_text_layouts(const QFont& font)
{
    static const QStringList k_prompt_glyphs = {
        QStringLiteral("\u203A"), // U+203A single right angle quote
        QStringLiteral("\u276F"), // U+276F heavy right angle quote ornament
        QStringLiteral("\u276E"), // U+276E heavy left angle quote ornament
        QStringLiteral("\u2192"), // U+2192 right arrow
        QStringLiteral("\u2190"), // U+2190 left arrow
        QStringLiteral("\u2713"), // U+2713 check mark
        QStringLiteral("\u2717"), // U+2717 ballot x
        QStringLiteral("\u2026"), // U+2026 horizontal ellipsis
        QStringLiteral("\u26A1"), // U+26A1 high voltage sign
        QStringLiteral("\u25B8"), // U+25B8 black right-pointing small triangle
        QStringLiteral("\u25BE"), // U+25BE black down-pointing small triangle
        QStringLiteral("\uE0B0"), // powerline separator
        QStringLiteral("\uE0B1"),
        QStringLiteral("\uE0B2"),
        QStringLiteral("\uE0B3"),
    };

    for (const QString& glyph : k_prompt_glyphs) {
        warm_text_layout(font, glyph);
    }
}

int display_width_for_preedit_cursor(const QString& text, int cursor_position)
{
    const int text_size = static_cast<int>(
        std::min<qsizetype>(text.size(), std::numeric_limits<int>::max()));
    const int clamped_cursor_position = std::clamp(cursor_position, 0, text_size);
    return term::measure_utf8_width(text.left(clamped_cursor_position).toUtf8()).cells;
}

}

struct VNM_TerminalSurface::Private
{
    using Renderer_lifecycle_recorder = term::Terminal_renderer_lifecycle_recorder;

    std::shared_ptr<Renderer_lifecycle_recorder> lifecycle_recorder() const
    {
        if (!renderer_lifecycle_recorder_enabled.load(std::memory_order_acquire)) {
            return {};
        }

        const std::lock_guard<std::mutex> lock(renderer_lifecycle_recorder_mutex);
        return renderer_lifecycle_recorder;
    }

    std::shared_ptr<Renderer_lifecycle_recorder> ensure_lifecycle_recorder()
    {
        const std::lock_guard<std::mutex> lock(renderer_lifecycle_recorder_mutex);
        if (renderer_lifecycle_recorder == nullptr) {
            renderer_lifecycle_recorder = std::make_shared<Renderer_lifecycle_recorder>();
            renderer_lifecycle_recorder_enabled.store(true, std::memory_order_release);
        }

        return renderer_lifecycle_recorder;
    }

#if VNM_TERMINAL_PROFILING_ENABLED
    std::shared_ptr<term::Hierarchical_profiler> render_profiler_handle() const
    {
        if (!render_profiler_enabled.load(std::memory_order_acquire)) {
            return {};
        }

        const std::lock_guard<std::mutex> lock(render_profiler_mutex);
        return render_profiler;
    }

    void set_render_profiler(std::shared_ptr<term::Hierarchical_profiler> profiler)
    {
        const std::lock_guard<std::mutex> lock(render_profiler_mutex);
        render_profiler = std::move(profiler);
        render_profiler_snapshot = {};
        if (render_profiler != nullptr) {
            if (slow_text_layout_recorder == nullptr) {
                slow_text_layout_recorder =
                    std::make_shared<term::Terminal_text_layout_slow_diagnostics_recorder>();
            }
            else {
                slow_text_layout_recorder->reset();
            }
        }
        else {
            slow_text_layout_recorder.reset();
        }
        render_profiler_enabled.store(render_profiler != nullptr, std::memory_order_release);
    }

    std::shared_ptr<term::Terminal_text_layout_slow_diagnostics_recorder>
    slow_text_layout_recorder_handle() const
    {
        if (!render_profiler_enabled.load(std::memory_order_acquire)) {
            return {};
        }

        const std::lock_guard<std::mutex> lock(render_profiler_mutex);
        return slow_text_layout_recorder;
    }

    void publish_render_profiler_snapshot(
        const std::shared_ptr<term::Hierarchical_profiler>&
                               profiler,
        std::uint64_t          sequence)
    {
        if (profiler == nullptr) {
            return;
        }

        term::Profile_node_snapshot root_snapshot = profiler->root_snapshot();
        term::Profile_timeline_snapshot timeline_snapshot = profiler->timeline_snapshot();
        term::terminal_text_layout_slow_diagnostics_t slow_text_layouts;
        if (const std::shared_ptr<term::Terminal_text_layout_slow_diagnostics_recorder>
            recorder = slow_text_layout_recorder_handle();
            recorder != nullptr)
        {
            slow_text_layouts = recorder->snapshot();
        }

        const std::lock_guard<std::mutex> lock(render_profiler_mutex);
        if (render_profiler != profiler) {
            return;
        }

        render_profiler_snapshot.sequence          = sequence;
        render_profiler_snapshot.root              = std::move(root_snapshot);
        render_profiler_snapshot.timeline          = std::move(timeline_snapshot);
        render_profiler_snapshot.slow_text_layouts = std::move(slow_text_layouts);
    }

    term::Render_profile_snapshot_t current_render_profiler_snapshot() const
    {
        const std::lock_guard<std::mutex> lock(render_profiler_mutex);
        return render_profiler_snapshot;
    }
#else
    void set_render_profiler(std::shared_ptr<term::Hierarchical_profiler> profiler)
    {
        Q_UNUSED(profiler);
    }

    term::Render_profile_snapshot_t current_render_profiler_snapshot() const
    {
        return {};
    }
#endif

    void request_render_update(VNM_TerminalSurface& surface)
    {
        ++render_invalidation_stats.update_requests;
        QQuickWindow* render_window = surface.window();
        if (render_window == nullptr) {
            render_update_pending = false;
            render_update_window = nullptr;
            return;
        }

        if (render_node_release_pending && render_snapshot != nullptr) {
            render_node_release_requeue_update = true;
        }

        if (render_update_pending) {
            if (render_update_window == render_window) {
                ++render_invalidation_stats.coalesced_requests;
                return;
            }

            render_update_pending = false;
            render_update_window = nullptr;
        }

        render_update_pending = true;
        render_update_window = render_window;
        ++render_invalidation_stats.scheduled_updates;
        surface.update();
    }

    void reset_render_update_schedule()
    {
        render_update_pending = false;
        render_update_window = nullptr;
    }

    void request_render_node_release(VNM_TerminalSurface& surface)
    {
        render_node_release_pending = true;
        // Preserve requeue intent across repeated releaseResources() calls
        // before the release paint consumes the pending render update.
        render_node_release_requeue_update = render_node_release_requeue_update ||
            (render_update_pending                       &&
                render_update_window == surface.window() &&
                render_snapshot != nullptr);
        reset_render_update_schedule();
        if (!shutting_down.load() && surface.window() != nullptr) {
            surface.update();
        }
    }

    void consume_render_update(QQuickWindow* window)
    {
        if (!render_update_pending || render_update_window != window) {
            return;
        }

        render_update_pending = false;
        render_update_window = nullptr;
        ++render_invalidation_stats.consumed_updates;
    }

    term::Terminal_surface_render_invalidation_stats_t current_invalidation_stats() const
    {
        term::Terminal_surface_render_invalidation_stats_t stats = render_invalidation_stats;
        stats.pending_update = render_update_pending;
        return stats;
    }

    QRectF input_method_cursor_rectangle(const VNM_TerminalSurface& surface) const
    {
        const qreal cell_width = term::is_valid_cell_metrics(cell_metrics)
            ? cell_metrics.width
            : 1.0;
        const qreal cell_height = term::is_valid_cell_metrics(cell_metrics)
            ? cell_metrics.height
            : 1.0;

        int row = 0;
        int column = 0;
        if (render_snapshot != nullptr && term::is_valid_grid_size(render_snapshot->grid_size)) {
            row    = std::clamp(
                render_snapshot->cursor.position.row,
                0,
                render_snapshot->grid_size.rows - 1);
            column = std::clamp(
                render_snapshot->cursor.position.column,
                0,
                render_snapshot->grid_size.columns - 1);
            if (ime_preedit.active) {
                column = std::clamp(
                    column + display_width_for_preedit_cursor(ime_preedit.text, ime_preedit.cursor_position),
                    0,
                    render_snapshot->grid_size.columns - 1);
            }
        }

        QRectF rectangle(
            static_cast<qreal>(column) *   cell_width,
            static_cast<qreal>(row) *      cell_height,
            std::max<qreal>(1.0, cell_width),
            std::max<qreal>(1.0, cell_height));
        const QRectF bounds = surface.boundingRect();
        if (bounds.width() > 0.0) {
            rectangle.moveLeft(std::clamp(
                rectangle.left(),
                bounds.left(),
                std::max(bounds.left(), bounds.right() - rectangle.width())));
        }
        if (bounds.height() > 0.0) {
            rectangle.moveTop(std::clamp(
                rectangle.top(),
                bounds.top(),
                std::max(bounds.top(), bounds.bottom() - rectangle.height())));
        }
        return rectangle;
    }

    void set_ime_preedit_state(
        VNM_TerminalSurface&       surface,
        term::Ime_preedit_state    state)
    {
        if (term::same_ime_preedit_state(ime_preedit, state)) {
            return;
        }

        ime_preedit = std::move(state);
        surface.updateInputMethod(Qt::ImCursorRectangle);
        request_render_update(surface);
    }

    void clear_mouse_reporting_state()
    {
        mouse_reporting_pressed_buttons = Qt::NoButton;
        mouse_reporting_drag_button = term::Terminal_mouse_button::NONE;
        mouse_reporting_last_position.reset();
    }

    void clear_selection_drag_state()
    {
        selection_anchor.reset();
        selection_anchor_buffer_id.reset();
        selection_drag_active = false;
        selection_drag_moved = false;
    }

    void clear_mouse_wheel_remainders()
    {
        wheel_mouse_angle_remainder = 0.0;
        wheel_mouse_pixel_remainder = 0.0;
    }

    void clear_zoom_wheel_remainders()
    {
        wheel_zoom_angle_remainder = 0.0;
        wheel_zoom_pixel_remainder = 0.0;
    }

    void clear_wheel_remainders()
    {
        wheel_scroll_angle_remainder = 0.0;
        wheel_scroll_pixel_remainder = 0.0;
        clear_zoom_wheel_remainders();
        clear_mouse_wheel_remainders();
    }

    void warm_prompt_text_layouts_for_render_font()
    {
        const QString font_key = render_font.toString();
        if (font_key == warmed_prompt_text_layout_font_key) {
            return;
        }

        warm_prompt_glyph_text_layouts(render_font);
        warmed_prompt_text_layout_font_key = font_key;
    }

    term::Qt_grid_metrics_provider                         grid_metrics_provider;
    term::terminal_cell_metrics_t                          cell_metrics;
    QFont                                                  render_font;
    qreal                                                  render_device_pixel_ratio             = 1.0;
    bool                                                   render_light_theme                    = false;
    std::shared_ptr<const term::Terminal_render_snapshot>  render_snapshot;
    term::Ime_preedit_state                                ime_preedit;
    bool                                                   cursor_blink_visible                  = true;
    term::Qsg_terminal_renderer                            renderer;
    term::Terminal_renderer_stats_publisher                renderer_stats_publisher;
#if VNM_TERMINAL_PROFILING_ENABLED
    mutable std::mutex                                     render_profiler_mutex;
    std::shared_ptr<term::Hierarchical_profiler>           render_profiler;
    term::Render_profile_snapshot_t                        render_profiler_snapshot;
    std::shared_ptr<term::Terminal_text_layout_slow_diagnostics_recorder>
                                                           slow_text_layout_recorder;
    std::atomic_bool                                       render_profiler_enabled               = false;
#endif
    mutable std::mutex                                     renderer_lifecycle_recorder_mutex;
    std::shared_ptr<Renderer_lifecycle_recorder>           renderer_lifecycle_recorder;
    std::atomic_bool                                  renderer_lifecycle_recorder_enabled =
                                                           false;

    term::Terminal_surface_render_invalidation_stats_t     render_invalidation_stats;
    bool                                                   render_update_pending              = false;
    bool                                                   render_node_release_pending        = false;
    bool                                                   render_node_release_requeue_update = false;
    qreal                                                  wheel_scroll_angle_remainder       = 0.0;
    qreal                                                  wheel_scroll_pixel_remainder       = 0.0;
    qreal                                                  wheel_zoom_angle_remainder         = 0.0;
    qreal                                                  wheel_zoom_pixel_remainder         = 0.0;
    qreal                                                  wheel_mouse_angle_remainder        = 0.0;
    qreal                                                  wheel_mouse_pixel_remainder        = 0.0;
    Qt::MouseButtons                                  mouse_reporting_pressed_buttons =
        Qt::NoButton;
    term::Terminal_mouse_button                       mouse_reporting_drag_button =
        term::Terminal_mouse_button::NONE;
    std::optional<term::terminal_grid_position_t>          mouse_reporting_last_position;
    std::optional<term::terminal_grid_position_t>          selection_anchor;
    std::optional<term::Terminal_buffer_id>                selection_anchor_buffer_id;
    std::optional<term::Terminal_osc52_write_request>      pending_clipboard_write;
    QString                                                warmed_prompt_text_layout_font_key;
    QTimer                                                 synchronized_output_recovery_timer;
    bool                                                   selection_drag_active                 = false;
    bool                                                   selection_drag_moved                  = false;
    bool                                                   last_sgr_mouse_reporting_active       = false;
    bool                                                   last_alternate_scroll_active          = false;
    std::uint64_t                                          last_alternate_scroll_mode_generation = 0U;
    QPointer<QQuickWindow>                                 render_update_window;
    std::uint64_t                                          window_binding_generation             = 0U;
    QMetaObject::Connection                                window_screen_changed_connection;
    QMetaObject::Connection                                window_scene_graph_invalidated_connection;
    QMetaObject::Connection                                screen_dpi_changed_connection;
    QMetaObject::Connection                                screen_physical_dpi_changed_connection;
    QPointer<QQuickWindow>                                 bound_window;
    std::unique_ptr<term::Terminal_session>                session;
    std::unique_ptr<term::Terminal_resize_controller>      resize_controller;
    std::uint64_t                                          last_render_snapshot_generation       = 0U;
    std::uint64_t                                          last_ime_preedit_generation           = 0U;
    std::uint64_t                                          last_backend_error_signal_sequence    = 0U;
    std::uint64_t                                          next_clipboard_write_request_id       = 1U;
    bool                                                   dirty_row_stats_enabled               = false;
    // These atomics are read by backend callback threads through the notifier.
    // reset_session() must close the session before Private storage is destroyed.
    std::atomic_bool                                       session_drain_queued                  = false;
    std::atomic_bool                                       shutting_down                         = false;
};

VNM_TerminalSurface::VNM_TerminalSurface(QQuickItem* parent)
:
    QQuickItem(parent),
    m_private(std::make_unique<Private>())
{
    m_font_family = term::vnm_terminal_default_monospace_font_family();
    m_font_size = term::k_vnm_terminal_default_font_pixel_size;
    setFlag(ItemHasContents,        true);
    setFlag(ItemAcceptsInputMethod, true);
    setClip(true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setFocus(true);
    m_private->synchronized_output_recovery_timer.setSingleShot(true);
    QObject::connect(
        &m_private->synchronized_output_recovery_timer,
        &QTimer::timeout,
        this,
        [this] {
            handle_synchronized_output_recovery_timeout();
        });
    refresh_grid_metrics();
}

VNM_TerminalSurface::~VNM_TerminalSurface()
{
    Q_ASSERT(thread() == QThread::currentThread());

    QObject::disconnect(m_private->window_scene_graph_invalidated_connection);
    QObject::disconnect(m_private->window_screen_changed_connection);
    QObject::disconnect(m_private->screen_dpi_changed_connection);
    QObject::disconnect(m_private->screen_physical_dpi_changed_connection);
    m_private->shutting_down.store(true);
    if (auto lifecycle_recorder = m_private->lifecycle_recorder();
        lifecycle_recorder != nullptr)
    {
        lifecycle_recorder->record_item_destruction();
    }
    releaseResources();
    if (m_private->session != nullptr &&
        is_live_process_state(m_private->session->process_state()))
    {
        (void)m_private->session->terminate();
    }
    reset_session();
}

void VNM_TerminalSurface::releaseResources()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (auto lifecycle_recorder = m_private->lifecycle_recorder();
        lifecycle_recorder != nullptr)
    {
        lifecycle_recorder->record_release_resources();
    }
    m_private->request_render_node_release(*this);
    QQuickItem::releaseResources();
}

QString VNM_TerminalSurface::font_family() const
{
    return m_font_family;
}

void VNM_TerminalSurface::set_font_family(const QString& font_family)
{
    if (m_font_family == font_family) {
        return;
    }

    m_font_family = font_family;
    emit font_family_changed();
    refresh_grid_metrics();
}

qreal VNM_TerminalSurface::font_size() const
{
    return m_font_size;
}

void VNM_TerminalSurface::set_font_size(qreal font_size)
{
    const qreal normalized_font_size = normalized_font_pixel_size(font_size);
    if (same_property_value(m_font_size, normalized_font_size)) {
        return;
    }

    m_font_size = normalized_font_size;
    m_private->clear_zoom_wheel_remainders();
    emit font_size_changed();
    refresh_grid_metrics();
}

QString VNM_TerminalSurface::color_theme() const
{
    return m_color_theme;
}

void VNM_TerminalSurface::set_color_theme(const QString& color_theme)
{
    if (m_color_theme == color_theme) {
        return;
    }

    m_color_theme = color_theme;
    m_private->render_light_theme = is_light_theme(m_color_theme);
    emit color_theme_changed();
    m_private->request_render_update(*this);
}

VNM_TerminalSurface::Cursor_style VNM_TerminalSurface::cursor_style() const
{
    return m_cursor_style;
}

void VNM_TerminalSurface::set_cursor_style(Cursor_style cursor_style)
{
    if (m_cursor_style == cursor_style) {
        return;
    }

    m_cursor_style = cursor_style;
    emit cursor_style_changed();
    m_private->request_render_update(*this);
}

bool VNM_TerminalSurface::cursor_blink_enabled() const
{
    return m_cursor_blink_enabled;
}

void VNM_TerminalSurface::set_cursor_blink_enabled(bool enabled)
{
    if (m_cursor_blink_enabled == enabled) {
        return;
    }

    m_cursor_blink_enabled = enabled;
    emit cursor_blink_enabled_changed();
    m_private->request_render_update(*this);
}

int VNM_TerminalSurface::scrollback_limit() const
{
    return m_scrollback_limit;
}

void VNM_TerminalSurface::set_scrollback_limit(int limit)
{
    const int bounded_limit = std::max(0, limit);

    if (m_scrollback_limit == bounded_limit) {
        return;
    }

    m_scrollback_limit = bounded_limit;
    emit scrollback_limit_changed();
    if (m_private->session != nullptr) {
        m_private->session->set_scrollback_limit(m_scrollback_limit);
        sync_from_session();
    }
}

QString VNM_TerminalSurface::backend_output_capture_path() const
{
    return m_backend_output_capture_path;
}

void VNM_TerminalSurface::set_backend_output_capture_path(const QString& path)
{
    m_backend_output_capture_path = path;
}

int VNM_TerminalSurface::synchronized_output_stale_timeout_ms() const
{
    return m_synchronized_output_stale_timeout_ms;
}

void VNM_TerminalSurface::set_synchronized_output_stale_timeout_ms(int timeout_ms)
{
    const int bounded_timeout_ms =
        std::max(k_min_synchronized_output_stale_timeout_ms, timeout_ms);

    if (m_synchronized_output_stale_timeout_ms == bounded_timeout_ms) {
        return;
    }

    m_synchronized_output_stale_timeout_ms = bounded_timeout_ms;
    emit synchronized_output_stale_timeout_ms_changed();
    sync_synchronized_output_recovery_timer();
}

VNM_TerminalSurface::Mouse_reporting_policy VNM_TerminalSurface::mouse_reporting_policy() const
{
    return m_mouse_reporting_policy;
}

void VNM_TerminalSurface::set_mouse_reporting_policy(Mouse_reporting_policy policy)
{
    if (m_mouse_reporting_policy == policy) {
        return;
    }

    m_mouse_reporting_policy = policy;
    m_private->clear_mouse_reporting_state();
    m_private->clear_mouse_wheel_remainders();
    emit mouse_reporting_policy_changed();
}

VNM_TerminalSurface::Copy_shortcut_policy VNM_TerminalSurface::copy_shortcut_policy() const
{
    return m_copy_shortcut_policy;
}

void VNM_TerminalSurface::set_copy_shortcut_policy(Copy_shortcut_policy policy)
{
    if (m_copy_shortcut_policy == policy) {
        return;
    }

    m_copy_shortcut_policy = policy;
    emit copy_shortcut_policy_changed();
}

VNM_TerminalSurface::Wheel_event_policy VNM_TerminalSurface::wheel_event_policy() const
{
    return m_wheel_event_policy;
}

void VNM_TerminalSurface::set_wheel_event_policy(Wheel_event_policy policy)
{
    if (m_wheel_event_policy == policy) {
        return;
    }

    m_wheel_event_policy = policy;
    m_private->clear_wheel_remainders();
    emit wheel_event_policy_changed();
}

VNM_TerminalSurface::Alternate_screen_wheel_policy
VNM_TerminalSurface::alternate_screen_wheel_policy() const
{
    return m_alternate_screen_wheel_policy;
}

void VNM_TerminalSurface::set_alternate_screen_wheel_policy(
    Alternate_screen_wheel_policy policy)
{
    if (m_alternate_screen_wheel_policy == policy) {
        return;
    }

    m_alternate_screen_wheel_policy = policy;
    m_private->clear_mouse_wheel_remainders();
    emit alternate_screen_wheel_policy_changed();
}

VNM_TerminalSurface::Bracketed_paste_policy VNM_TerminalSurface::bracketed_paste_policy() const
{
    return m_bracketed_paste_policy;
}

void VNM_TerminalSurface::set_bracketed_paste_policy(Bracketed_paste_policy policy)
{
    if (m_bracketed_paste_policy == policy) {
        return;
    }

    m_bracketed_paste_policy = policy;
    emit bracketed_paste_policy_changed();
}

VNM_TerminalSurface::Bell_policy VNM_TerminalSurface::audible_bell_policy() const
{
    return m_audible_bell_policy;
}

void VNM_TerminalSurface::set_audible_bell_policy(Bell_policy policy)
{
    if (m_audible_bell_policy == policy) {
        return;
    }

    m_audible_bell_policy = policy;
    emit audible_bell_policy_changed();
}

VNM_TerminalSurface::Bell_policy VNM_TerminalSurface::visual_bell_policy() const
{
    return m_visual_bell_policy;
}

void VNM_TerminalSurface::set_visual_bell_policy(Bell_policy policy)
{
    if (m_visual_bell_policy == policy) {
        return;
    }

    m_visual_bell_policy = policy;
    emit visual_bell_policy_changed();
    m_private->request_render_update(*this);
}

QString VNM_TerminalSurface::terminal_title() const
{
    return m_terminal_title;
}

QString VNM_TerminalSurface::terminal_icon_name() const
{
    return m_terminal_icon_name;
}

VNM_TerminalSurface::Process_state VNM_TerminalSurface::process_state() const
{
    return m_process_state;
}

bool VNM_TerminalSurface::backend_ready() const
{
    return m_backend_ready;
}

bool VNM_TerminalSurface::backend_geometry_in_sync() const
{
    return m_backend_geometry_in_sync;
}

int VNM_TerminalSurface::rows() const
{
    return m_rows;
}

int VNM_TerminalSurface::columns() const
{
    return m_columns;
}

int VNM_TerminalSurface::scrollback_rows() const
{
    return m_scrollback_rows;
}

int VNM_TerminalSurface::viewport_visible_rows() const
{
    return m_viewport_visible_rows;
}

int VNM_TerminalSurface::viewport_offset_from_tail() const
{
    return m_viewport_offset_from_tail;
}

bool VNM_TerminalSurface::viewport_at_tail() const
{
    return m_viewport_at_tail;
}

VNM_TerminalSurface::Selection_state VNM_TerminalSurface::selection_state() const
{
    return m_selection_state;
}

bool VNM_TerminalSurface::respond_clipboard_write(
    quint64                        request_id,
    Clipboard_response_decision    decision)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (!m_private->pending_clipboard_write.has_value() ||
        m_private->pending_clipboard_write->request_id != request_id)
    {
        emit backend_error(
            Backend_error_code::CALLBACK_MISSING,
            QStringLiteral("no active clipboard write request"));
        return false;
    }

    term::Terminal_osc52_write_request request =
        std::move(*m_private->pending_clipboard_write);
    m_private->pending_clipboard_write.reset();

    if (decision == Clipboard_response_decision::DENY) {
        return true;
    }

    if (request.target_selection != QStringLiteral("c") &&
        request.target_selection != QStringLiteral("clipboard"))
    {
        emit backend_error(
            Backend_error_code::CALLBACK_MISSING,
            QStringLiteral("unsupported clipboard write target"));
        return false;
    }

    QGuiApplication::clipboard()->setText(
        QString::fromUtf8(request.decoded_payload),
        QClipboard::Clipboard);
    return true;
}

QString VNM_TerminalSurface::selected_text()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        return {};
    }

    drain_backend_callback_events();
    if (m_private->session->render_publication_blocked()) {
        return selected_text_from_visible_snapshot(m_private->render_snapshot);
    }

    const term::Terminal_selection_result result = m_private->session->selected_text();
    return result.code == term::Terminal_selection_result_code::OK
        ? result.text
        : QString();
}

bool VNM_TerminalSurface::copy_selected_text_to_clipboard()
{
    const auto copy_text = [](const QString& text) {
        QClipboard* clipboard = QGuiApplication::clipboard();
        if (clipboard == nullptr) {
            return;
        }

        clipboard->setText(text, QClipboard::Clipboard);
    };

    const auto copy_visible_selection = [&copy_text, this]() {
        const std::shared_ptr<const term::Terminal_render_snapshot> visible_snapshot =
            m_private->render_snapshot;
        if (visible_snapshot == nullptr || visible_snapshot->selection_spans.empty()) {
            return false;
        }

        copy_text(selected_text_from_visible_snapshot(visible_snapshot));
        return true;
    };

    if (m_private->session != nullptr) {
        if (m_private->session->render_publication_blocked()) {
            return copy_visible_selection();
        }

        if (m_private->session->has_selection()) {
            const term::Terminal_selection_result result = m_private->session->selected_text();
            if (result.code == term::Terminal_selection_result_code::OK) {
                copy_text(result.text);
            }

            return true;
        }
    }

    return copy_visible_selection();
}

void VNM_TerminalSurface::clear_selection()
{
    Q_ASSERT(thread() == QThread::currentThread());

    m_private->clear_selection_drag_state();

    if (m_private->session == nullptr) {
        return;
    }

    m_private->session->clear_selection();
    sync_from_session();
}

bool VNM_TerminalSurface::paste_text(QString text)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        return false;
    }

    const term::Terminal_paste_text_result paste_result =
        m_private->session->write_paste_text(
            std::move(text),
            paste_framing_policy(m_bracketed_paste_policy));
    sync_from_session();
    if (!paste_result.handled) {
        return false;
    }

    if (!is_accepted(paste_result.result.code)) {
        report_result_failure(paste_result.result);
        return false;
    }

    return true;
}

bool VNM_TerminalSurface::scroll_viewport_lines(int line_delta)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (line_delta == 0 || m_private->session == nullptr) {
        return false;
    }

    const term::Terminal_viewport_scroll_result scroll_result =
        m_private->session->scroll_published_viewport_lines(line_delta);
    sync_from_session();
    return scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED;
}

bool VNM_TerminalSurface::scroll_to_offset_from_tail(int offset_from_tail)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        return false;
    }

    const term::Terminal_viewport_scroll_result scroll_result =
        m_private->session->scroll_published_viewport_to_offset_from_tail(offset_from_tail);
    sync_from_session();
    return scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED;
}

bool VNM_TerminalSurface::start_process(QStringList argv, QString working_directory)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session != nullptr) {
        drain_backend_callback_events();
        if (is_live_process_state(m_private->session->process_state())) {
            report_backend_error({
                term::Terminal_backend_error_code::START_FAILED,
                QStringLiteral("start requires no active terminal process"),
            });
            return false;
        }
    }

    if (argv.isEmpty() || argv.front().trimmed().isEmpty()) {
        report_backend_error({
            term::Terminal_backend_error_code::INVALID_LAUNCH_CONFIG,
            QStringLiteral("argv must name an executable"),
        });
        set_process_state(Process_state::FAILED);
        return false;
    }

    std::unique_ptr<term::Terminal_backend> backend = make_native_backend();
    if (backend == nullptr) {
        report_backend_error({
            term::Terminal_backend_error_code::START_FAILED,
            QStringLiteral("no native terminal backend is available on this platform"),
        });
        set_process_state(Process_state::FAILED);
        return false;
    }

    return start_process_with_backend(
        std::move(backend),
        std::move(argv),
        std::move(working_directory));
}

bool VNM_TerminalSurface::interrupt_process()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        report_backend_error({
            term::Terminal_backend_error_code::INTERRUPT_FAILED,
            QStringLiteral("interrupt requires an active terminal session"),
        });
        return false;
    }

    const term::Terminal_session_result result = m_private->session->interrupt();
    sync_from_session();
    if (!is_accepted(result.code)) {
        report_result_failure(result);
        return false;
    }

    return true;
}

bool VNM_TerminalSurface::terminate_process()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        report_backend_error({
            term::Terminal_backend_error_code::TERMINATE_FAILED,
            QStringLiteral("terminate requires an active terminal session"),
        });
        return false;
    }

    const term::Terminal_session_result result = m_private->session->terminate();
    sync_from_session();
    if (!is_accepted(result.code)) {
        report_result_failure(result);
        return false;
    }

    return true;
}

QVariant VNM_TerminalSurface::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled) {
        return true;
    }

    if (query == Qt::ImCursorRectangle) {
        return m_private->input_method_cursor_rectangle(*this);
    }

    return QQuickItem::inputMethodQuery(query);
}

void VNM_TerminalSurface::geometryChange(
    const QRectF&  new_geometry,
    const QRectF&  old_geometry)
{
    QQuickItem::geometryChange(new_geometry, old_geometry);
    refresh_grid_metrics();
}

void VNM_TerminalSurface::itemChange(ItemChange change, const ItemChangeData& value)
{
    QQuickItem::itemChange(change, value);

    if (change == ItemSceneChange) {
        const bool window_identity_changed = value.window != m_private->bound_window;
        if (auto lifecycle_recorder = m_private->lifecycle_recorder();
            lifecycle_recorder != nullptr)
        {
            lifecycle_recorder->record_item_scene_change();
            if (value.window == nullptr) {
                lifecycle_recorder->record_item_scene_detach();
            }
        }
        if (window_identity_changed) {
            m_private->render_node_release_pending        = false;
            m_private->render_node_release_requeue_update = false;
            m_private->reset_render_update_schedule();
        }
        bind_window_signals(value.window);
        refresh_grid_metrics();
    }
    else
    if (change == ItemDevicePixelRatioHasChanged) {
        refresh_grid_metrics();
    }
    else
    if (change == ItemActiveFocusHasChanged && !hasActiveFocus()) {
        if (m_private->session != nullptr) {
            const term::Terminal_focus_event_result focus_result =
                m_private->session->write_focus_event(false);
            m_private->session->cancel_ime_preedit();
            sync_from_session();
            if (focus_result.handled && !is_accepted(focus_result.result.code)) {
                report_result_failure(focus_result.result);
            }
            return;
        }

        term::Ime_preedit_state state;
        m_private->set_ime_preedit_state(*this, std::move(state));
    }
    else
    if (change == ItemActiveFocusHasChanged) {
        if (m_private->session != nullptr) {
            const term::Terminal_focus_event_result focus_result =
                m_private->session->write_focus_event(true);
            sync_from_session();
            if (focus_result.handled && !is_accepted(focus_result.result.code)) {
                report_result_failure(focus_result.result);
            }
        }
    }
}

void VNM_TerminalSurface::keyPressEvent(QKeyEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (is_plain_copy_shortcut(*event)) {
        if (m_copy_shortcut_policy ==
            Copy_shortcut_policy::COPY_SELECTION_OR_TERMINAL_INPUT)
        {
            if (copy_selected_text_to_clipboard()) {
                event->accept();
                return;
            }
        }
        else
        if (m_copy_shortcut_policy == Copy_shortcut_policy::COPY_SELECTION_OR_IGNORE) {
            (void)copy_selected_text_to_clipboard();
            event->accept();
            return;
        }
    }

    if (m_private->session != nullptr) {
        if (is_unmodified_page_scroll_key(*event)) {
            drain_backend_callback_events();
            if (m_private->session == nullptr) {
                event->ignore();
                return;
            }

            const int direction = event->key() == Qt::Key_PageUp ? 1 : -1;
            const term::Terminal_viewport_state viewport =
                m_private->session->viewport_state();
            if (viewport.active_buffer   == term::Terminal_buffer_id::PRIMARY &&
                viewport.scrollback_rows >  0)
            {
                const int visible_rows =
                    std::max(1, viewport.visible_rows);
                const term::Terminal_viewport_scroll_result scroll_result =
                    m_private->session->scroll_viewport_lines(direction * visible_rows);
                if (scroll_result.action ==
                    term::Terminal_viewport_scroll_action::VIEWPORT_MOVED)
                {
                    sync_from_session();
                    event->accept();
                    return;
                }

                event->accept();
                return;
            }
        }

        const term::Terminal_key_event_result key_result =
            m_private->session->write_key_event(*event);
        if (!key_result.handled) {
            event->ignore();
            return;
        }

        event->accept();
        sync_from_session();
        if (!is_accepted(key_result.result.code)) {
            report_result_failure(key_result.result);
        }
        return;
    }

    const QByteArray bytes =
        term::encode_terminal_key_event(*event, term::Terminal_input_mode_state{});
    if (bytes.isEmpty()) {
        event->ignore();
        return;
    }

    event->accept();
}

void VNM_TerminalSurface::mousePressEvent(QMouseEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());
    event->ignore();

    forceActiveFocus(Qt::MouseFocusReason);
    drain_backend_callback_events();

    const term::Terminal_mouse_button button = terminal_mouse_button(event->button());
    const std::optional<term::terminal_grid_position_t> position = grid_position_for_local_point(
        m_private->render_snapshot,
        m_private->cell_metrics,
        event->position());
    const bool force_local_selection = local_selection_override(event->modifiers());

    if (!force_local_selection &&
        m_mouse_reporting_policy != Mouse_reporting_policy::DISABLED &&
        m_private->session       != nullptr &&
        position.has_value()                &&
        button                   != term::Terminal_mouse_button::NONE)
    {
        const term::Terminal_mouse_event_result mouse_result =
            m_private->session->write_mouse_event({
                term::Terminal_mouse_event_kind::PRESS,
                button,
                position->row,
                position->column,
                event->modifiers(),
        });
        if (mouse_result.handled) {
            m_private->mouse_reporting_pressed_buttons |= event->button();
            m_private->mouse_reporting_drag_button      = button;
            m_private->mouse_reporting_last_position    = *position;
            m_private->clear_selection_drag_state();
            m_private->session->clear_selection();
            event->accept();
            sync_from_session();
            if (!is_accepted(mouse_result.result.code)) {
                report_result_failure(mouse_result.result);
            }
            return;
        }
        if (snapshot_has_terminal_mouse_tracking(m_private->render_snapshot) &&
            !snapshot_has_sgr_mouse_reporting(m_private->render_snapshot))
        {
            m_private->mouse_reporting_pressed_buttons |= event->button();
            m_private->mouse_reporting_drag_button      = button;
            m_private->mouse_reporting_last_position    = *position;
            m_private->clear_selection_drag_state();
            m_private->session->clear_selection();
            event->accept();
            sync_from_session();
            return;
        }
    }

    if (event->button() == Qt::RightButton) {
        QClipboard* clipboard = QGuiApplication::clipboard();
        if (clipboard != nullptr && paste_text(clipboard->text())) {
            event->accept();
            return;
        }
    }

    if (event->button() != Qt::LeftButton || m_private->session == nullptr) {
        return;
    }

    if (m_private->session->render_publication_blocked()) {
        return;
    }

    if (!position.has_value()) {
        return;
    }

    const std::optional<term::terminal_grid_position_t> logical_position = logical_grid_position_for_viewport_cell(
        m_private->render_snapshot,
        *position);
    if (!logical_position.has_value()) {
        return;
    }

    m_private->selection_anchor           = *logical_position;
    m_private->selection_anchor_buffer_id = m_private->render_snapshot->viewport.active_buffer;
    m_private->selection_drag_active      = true;
    m_private->selection_drag_moved       = false;
    m_private->session->clear_selection();
    event->accept();
    sync_from_session();
}

void VNM_TerminalSurface::mouseMoveEvent(QMouseEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());
    event->ignore();
    drain_backend_callback_events();

    const std::optional<term::terminal_grid_position_t> position = grid_position_for_local_point(
        m_private->render_snapshot,
        m_private->cell_metrics,
        event->position());
    const bool force_local_selection = local_selection_override(event->modifiers());
    const bool terminal_mouse_grab_active =
        m_private->mouse_reporting_pressed_buttons != Qt::NoButton;

    if (!m_private->selection_drag_active &&
        (!force_local_selection || terminal_mouse_grab_active)       &&
        m_mouse_reporting_policy != Mouse_reporting_policy::DISABLED &&
        m_private->session       != nullptr)
    {
        std::optional<term::terminal_grid_position_t> report_position = position;
        if (!report_position.has_value() && terminal_mouse_grab_active) {
            report_position = m_private->mouse_reporting_last_position.has_value()
                ? m_private->mouse_reporting_last_position
                : clamped_grid_position_for_local_point(
                    m_private->render_snapshot,
                    m_private->cell_metrics,
                    event->position());
        }

        if (report_position.has_value()) {
            const Qt::MouseButtons active_buttons =
                event->buttons() & m_private->mouse_reporting_pressed_buttons;
            term::Terminal_mouse_button button = terminal_mouse_button(
                active_buttons,
                m_private->mouse_reporting_drag_button);
            if (button == term::Terminal_mouse_button::NONE) {
                button = terminal_mouse_button(m_private->mouse_reporting_pressed_buttons);
            }
            m_private->mouse_reporting_drag_button = button;

            const term::Terminal_mouse_event_kind kind =
                button == term::Terminal_mouse_button::NONE
                    ? term::Terminal_mouse_event_kind::MOVE
                    : term::Terminal_mouse_event_kind::DRAG;
            const term::Terminal_mouse_event_result mouse_result =
                m_private->session->write_mouse_event({
                    kind,
                    button,
                    report_position->row,
                    report_position->column,
                    event->modifiers(),
                });
            if (mouse_result.handled) {
                m_private->mouse_reporting_last_position = *report_position;
                m_private->clear_selection_drag_state();
                m_private->session->clear_selection();
                event->accept();
                sync_from_session();
                if (!is_accepted(mouse_result.result.code)) {
                    report_result_failure(mouse_result.result);
                }
                return;
            }
            if (snapshot_has_terminal_mouse_tracking(m_private->render_snapshot) &&
                !snapshot_has_sgr_mouse_reporting(m_private->render_snapshot))
            {
                m_private->mouse_reporting_last_position = *report_position;
                event->accept();
                return;
            }
        }
    }

    if (!m_private->selection_drag_active                  ||
        !m_private->selection_anchor.has_value()           ||
        !m_private->selection_anchor_buffer_id.has_value() ||
        m_private->session                  == nullptr     ||
        (event->buttons() & Qt::LeftButton) == Qt::NoButton)
    {
        return;
    }

    if (m_private->render_snapshot == nullptr ||
        m_private->render_snapshot->viewport.active_buffer !=
            *m_private->selection_anchor_buffer_id)
    {
        m_private->clear_selection_drag_state();
        m_private->session->clear_selection();
        event->accept();
        sync_from_session();
        return;
    }

    if (m_private->session->render_publication_blocked()) {
        event->accept();
        return;
    }

    const std::optional<term::terminal_grid_position_t> viewport_position =
        position.has_value()
            ? position
            : clamped_grid_position_for_local_point(
                m_private->render_snapshot,
                m_private->cell_metrics,
                event->position());
    if (!viewport_position.has_value()) {
        return;
    }

    const std::optional<term::terminal_grid_position_t> logical_position = logical_grid_position_for_viewport_cell(
        m_private->render_snapshot,
        *viewport_position);
    if (!logical_position.has_value()) {
        return;
    }

    m_private->selection_drag_moved = true;
    set_selection_range_for_drag(
        *m_private->session,
        *m_private->selection_anchor,
        *logical_position,
        true);
    event->accept();
    sync_from_session();
}

void VNM_TerminalSurface::mouseReleaseEvent(QMouseEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());
    event->ignore();
    drain_backend_callback_events();

    if (m_private->selection_drag_active &&
        event->button()    == Qt::LeftButton              &&
        m_private->selection_anchor.has_value()           &&
        m_private->selection_anchor_buffer_id.has_value() &&
        m_private->session != nullptr)
    {
        if (m_private->session->render_publication_blocked()) {
            m_private->clear_selection_drag_state();
            event->accept();
            sync_from_session();
            return;
        }

        if (m_private->render_snapshot == nullptr ||
            m_private->render_snapshot->viewport.active_buffer !=
                *m_private->selection_anchor_buffer_id)
        {
            m_private->clear_selection_drag_state();
            m_private->session->clear_selection();
            event->accept();
            sync_from_session();
            return;
        }

        const std::optional<term::terminal_grid_position_t> position = grid_position_for_local_point(
            m_private->render_snapshot,
            m_private->cell_metrics,
            event->position());
        const std::optional<term::terminal_grid_position_t> viewport_position =
            position.has_value()
                ? position
                : clamped_grid_position_for_local_point(
                    m_private->render_snapshot,
                    m_private->cell_metrics,
                    event->position());
        std::optional<term::terminal_grid_position_t> logical_position;
        if (viewport_position.has_value()) {
            logical_position = logical_grid_position_for_viewport_cell(
                m_private->render_snapshot,
                *viewport_position);
        }
        if (logical_position.has_value()) {
            set_selection_range_for_drag(
                *m_private->session,
                *m_private->selection_anchor,
                *logical_position,
                m_private->selection_drag_moved);
        }
        else
        if (!m_private->selection_drag_moved) {
            m_private->session->clear_selection();
        }

        m_private->clear_selection_drag_state();
        event->accept();
        sync_from_session();
        return;
    }

    term::Terminal_mouse_button button = terminal_mouse_button(event->button());
    if (button == term::Terminal_mouse_button::NONE) {
        button = m_private->mouse_reporting_drag_button;
    }
    const bool final_release =
        (event->buttons() &
            (Qt::LeftButton | Qt::MiddleButton | Qt::RightButton)) == Qt::NoButton;
    const bool force_local_selection = local_selection_override(event->modifiers());
    const bool terminal_mouse_grab_active =
        m_private->mouse_reporting_pressed_buttons != Qt::NoButton;

    if (m_mouse_reporting_policy == Mouse_reporting_policy::DISABLED ||
        m_private->session       == nullptr                          ||
        (force_local_selection && !terminal_mouse_grab_active))
    {
        if (final_release) {
            m_private->clear_mouse_reporting_state();
        }
        return;
    }

    if (!terminal_mouse_grab_active) {
        if (final_release) {
            m_private->mouse_reporting_last_position.reset();
        }
        return;
    }

    const Qt::MouseButton released_qt_button = event->button();
    if (released_qt_button                                                == Qt::NoButton ||
        (m_private->mouse_reporting_pressed_buttons & released_qt_button) == Qt::NoButton ||
        terminal_mouse_button(released_qt_button)                         == term::Terminal_mouse_button::NONE)
    {
        if (final_release) {
            m_private->clear_mouse_reporting_state();
        }
        return;
    }

    button = terminal_mouse_button(released_qt_button);

    const std::optional<term::terminal_grid_position_t> position = grid_position_for_local_point(
        m_private->render_snapshot,
        m_private->cell_metrics,
        event->position());
    std::optional<term::terminal_grid_position_t> report_position = position;
    if (!report_position.has_value() && terminal_mouse_grab_active) {
        report_position = m_private->mouse_reporting_last_position.has_value()
            ? m_private->mouse_reporting_last_position
            : clamped_grid_position_for_local_point(
                m_private->render_snapshot,
                m_private->cell_metrics,
                event->position());
    }
    if (!report_position.has_value() || button == term::Terminal_mouse_button::NONE) {
        if (final_release) {
            m_private->clear_mouse_reporting_state();
        }
        return;
    }

    const term::Terminal_mouse_event_result mouse_result =
        m_private->session->write_mouse_event({
            term::Terminal_mouse_event_kind::RELEASE,
            button,
            report_position->row,
            report_position->column,
            event->modifiers(),
        });
    if (!mouse_result.handled) {
        if (final_release) {
            m_private->clear_mouse_reporting_state();
        }
        else {
            m_private->mouse_reporting_pressed_buttons &= ~Qt::MouseButtons(released_qt_button);
            m_private->mouse_reporting_drag_button =
                terminal_mouse_button(m_private->mouse_reporting_pressed_buttons);
        }
        if (snapshot_has_terminal_mouse_tracking(m_private->render_snapshot) &&
            !snapshot_has_sgr_mouse_reporting(m_private->render_snapshot))
        {
            event->accept();
        }
        return;
    }

    m_private->mouse_reporting_last_position = *report_position;
    if (final_release) {
        m_private->clear_mouse_reporting_state();
    }
    else {
        m_private->mouse_reporting_pressed_buttons &= ~Qt::MouseButtons(released_qt_button);
        m_private->mouse_reporting_drag_button =
            terminal_mouse_button(m_private->mouse_reporting_pressed_buttons);
    }
    m_private->session->clear_selection();
    event->accept();
    sync_from_session();
    if (!is_accepted(mouse_result.result.code)) {
        report_result_failure(mouse_result.result);
    }
}

void VNM_TerminalSurface::hoverMoveEvent(QHoverEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());
    event->ignore();
    drain_backend_callback_events();

    if (m_mouse_reporting_policy == Mouse_reporting_policy::DISABLED ||
        m_private->session       == nullptr                          ||
        local_selection_override(event->modifiers()))
    {
        return;
    }

    const std::optional<term::terminal_grid_position_t> position = grid_position_for_local_point(
        m_private->render_snapshot,
        m_private->cell_metrics,
        event->position());
    if (!position.has_value()) {
        return;
    }

    const term::Terminal_mouse_event_result mouse_result =
        m_private->session->write_mouse_event({
            term::Terminal_mouse_event_kind::MOVE,
            term::Terminal_mouse_button::NONE,
            position->row,
            position->column,
            event->modifiers(),
        });
    if (!mouse_result.handled) {
        if (snapshot_has_terminal_mouse_tracking(m_private->render_snapshot) &&
            !snapshot_has_sgr_mouse_reporting(m_private->render_snapshot))
        {
            event->accept();
        }
        return;
    }

    event->accept();
    sync_from_session();
    if (!is_accepted(mouse_result.result.code)) {
        report_result_failure(mouse_result.result);
    }
}

void VNM_TerminalSurface::wheelEvent(QWheelEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if ((event->modifiers() & Qt::ControlModifier) != 0) {
        if (m_private->session != nullptr) {
            drain_backend_callback_events();
        }

        const int steps = vertical_wheel_steps(
            *event,
            k_angle_delta_per_wheel_step,
            m_private->wheel_zoom_angle_remainder,
            m_private->wheel_zoom_pixel_remainder);
        if (steps == 0) {
            if (has_vertical_wheel_delta(*event)) {
                event->accept();
                return;
            }

            event->ignore();
            return;
        }

        const qreal base_font_size = finite_positive_font_size_or_default(m_font_size);
        const qreal requested_font_size = std::clamp(
            base_font_size + static_cast<qreal>(steps) * k_font_zoom_wheel_step,
            k_font_zoom_min_pixel_size,
            k_font_zoom_max_pixel_size);
        const qreal previous_font_size = m_font_size;
        set_font_size(requested_font_size);
        if (has_vertical_wheel_delta(*event) ||
            !same_property_value(previous_font_size, m_font_size))
        {
            event->accept();
        }
        else {
            event->ignore();
        }
        return;
    }

    if (m_private->session == nullptr) {
        QQuickItem::wheelEvent(event);
        return;
    }

    const auto route_wheel_to_application = [&]() -> bool {
        if (!has_vertical_wheel_delta(*event)) {
            return false;
        }

        drain_backend_callback_events();
        if (m_private->session == nullptr) {
            event->ignore();
            return true;
        }

        const std::optional<term::terminal_grid_position_t> position = grid_position_for_local_point(
            m_private->render_snapshot,
            m_private->cell_metrics,
            event->position());
        const term::Terminal_viewport_state live_viewport =
            m_private->session->viewport_state();
        const bool alternate_screen =
            live_viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE ||
            (m_private->render_snapshot != nullptr &&
                m_private->render_snapshot->viewport.active_buffer ==
                    term::Terminal_buffer_id::ALTERNATE);
        const auto route_alternate_wheel_to_keys = [&]() -> bool {
            if (!alternate_screen) {
                return false;
            }

            const int steps = vertical_wheel_steps(
                *event,
                m_private->cell_metrics.height,
                m_private->wheel_mouse_angle_remainder,
                m_private->wheel_mouse_pixel_remainder);
            if (steps == 0) {
                event->accept();
                return true;
            }

            const bool page_keys =
                m_alternate_screen_wheel_policy ==
                Alternate_screen_wheel_policy::PAGE_KEYS;
            const int key_count = page_keys
                ? std::abs(steps)
                : std::abs(event->angleDelta().y() != 0
                    ? steps * k_plain_scroll_lines_per_angle_step
                    : steps);
            const int key = steps > 0
                ? (page_keys ? Qt::Key_PageUp : Qt::Key_Up)
                : (page_keys ? Qt::Key_PageDown : Qt::Key_Down);

            bool handled = false;
            term::Terminal_session_result last_result;
            for (int i = 0; i < key_count; ++i) {
                QKeyEvent key_event(QEvent::KeyPress, key, Qt::NoModifier, {});
                const term::Terminal_key_event_result key_result =
                    m_private->session->write_key_event(key_event);
                if (!key_result.handled) {
                    break;
                }

                handled = true;
                last_result = key_result.result;
                if (!is_accepted(last_result.code)) {
                    break;
                }
            }

            if (!handled) {
                return false;
            }

            event->accept();
            sync_from_session();
            if (!is_accepted(last_result.code)) {
                report_result_failure(last_result);
            }
            return true;
        };

        if (m_alternate_screen_wheel_policy !=
            Alternate_screen_wheel_policy::MOUSE_REPORTING_FIRST &&
            route_alternate_wheel_to_keys())
        {
            return true;
        }

        const bool live_sgr_mouse_reporting = m_private->session->mouse_reporting_active();
        const bool published_sgr_mouse_reporting =
            snapshot_has_sgr_mouse_reporting(m_private->render_snapshot);
        const bool published_mouse_tracking =
            snapshot_has_terminal_mouse_tracking(m_private->render_snapshot);
        if (m_mouse_reporting_policy != Mouse_reporting_policy::DISABLED &&
            position.has_value() &&
            (live_sgr_mouse_reporting || published_sgr_mouse_reporting))
        {
            const int steps = vertical_wheel_steps(
                *event,
                m_private->cell_metrics.height,
                m_private->wheel_mouse_angle_remainder,
                m_private->wheel_mouse_pixel_remainder);
            if (steps == 0) {
                event->accept();
                return true;
            }

            const term::Terminal_mouse_button button = steps > 0
                ? term::Terminal_mouse_button::WHEEL_UP
                : term::Terminal_mouse_button::WHEEL_DOWN;
            bool handled = false;
            term::Terminal_session_result last_result;
            for (int i = 0; i < std::abs(steps); ++i) {
                const term::Terminal_mouse_event_result mouse_result =
                    m_private->session->write_mouse_event({
                        term::Terminal_mouse_event_kind::WHEEL,
                        button,
                        position->row,
                        position->column,
                        event->modifiers(),
                    });
                if (!mouse_result.handled) {
                    break;
                }

                handled = true;
                last_result = mouse_result.result;
                if (!is_accepted(last_result.code)) {
                    break;
                }
            }
            if (handled) {
                event->accept();
                sync_from_session();
                if (!is_accepted(last_result.code)) {
                    report_result_failure(last_result);
                }
                return true;
            }

            return false;
        }

        if (m_mouse_reporting_policy != Mouse_reporting_policy::DISABLED &&
            position.has_value() &&
            published_mouse_tracking)
        {
            event->accept();
            return true;
        }

        return route_alternate_wheel_to_keys();
    };

    const auto scroll_viewport_locally = [&]() -> bool {
        if (!has_vertical_wheel_delta(*event)) {
            event->ignore();
            return true;
        }

        drain_backend_callback_events();
        if (m_private->session == nullptr) {
            event->ignore();
            return false;
        }

        const term::Terminal_viewport_state viewport =
            m_private->session->viewport_state();
        if (!viewport_state_can_scroll_locally(viewport, vertical_wheel_direction(*event))) {
            event->ignore();
            return false;
        }

        const int line_delta = vertical_wheel_steps(
            *event,
            m_private->cell_metrics.height,
            m_private->wheel_scroll_angle_remainder,
            m_private->wheel_scroll_pixel_remainder);
        const int effective_line_delta =
            event->angleDelta().y() != 0
                ? line_delta * k_plain_scroll_lines_per_angle_step
                : line_delta;
        if (effective_line_delta == 0) {
            event->accept();
            return true;
        }

        const term::Terminal_viewport_scroll_result scroll_result =
            m_private->session->scroll_viewport_lines(effective_line_delta);
        if (scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED) {
            sync_from_session();
            event->accept();
            return true;
        }

        event->ignore();
        return false;
    };

    if (m_wheel_event_policy == Wheel_event_policy::LOCAL_SCROLLBACK_FIRST) {
        if (m_private->session != nullptr) {
            drain_backend_callback_events();
        }
        if (m_private->session == nullptr) {
            event->ignore();
            return;
        }

        const bool try_local_scroll_first = viewport_state_can_scroll_locally(
            m_private->session->viewport_state(),
            vertical_wheel_direction(*event));
        if (try_local_scroll_first && scroll_viewport_locally()) {
            return;
        }
        if (route_wheel_to_application()) {
            return;
        }
        if (!try_local_scroll_first && scroll_viewport_locally()) {
            return;
        }
        event->ignore();
        return;
    }

    if (m_wheel_event_policy == Wheel_event_policy::LOCAL_SCROLLBACK_ONLY) {
        (void)scroll_viewport_locally();
        return;
    }

    if (route_wheel_to_application()) {
        return;
    }

    (void)scroll_viewport_locally();
}

void VNM_TerminalSurface::inputMethodEvent(QInputMethodEvent* event)
{
    Q_ASSERT(thread() == QThread::currentThread());

    const QString commit_text             = terminal_commit_text_from_event(*event);
    const QString preedit_text            = event->preeditString();
    const int     preedit_cursor_position = preedit_cursor_position_from_event(*event);

    std::optional<term::Terminal_ime_commit_result> commit_result;
    if (m_private->session != nullptr) {
        if (!commit_text.isEmpty()) {
            commit_result = m_private->session->write_ime_commit(commit_text);
        }

        const bool commit_blocks_empty_preedit_cancel =
            commit_result.has_value()               &&
            commit_result->handled                  &&
            !is_accepted(commit_result->result.code);
        if (preedit_text.isEmpty()) {
            if (!commit_blocks_empty_preedit_cancel) {
                m_private->session->cancel_ime_preedit();
            }
        }
        else {
            m_private->session->set_ime_preedit(
                preedit_text,
                preedit_cursor_position);
        }

        sync_from_session();
        if (commit_result.has_value() &&
            commit_result->handled &&
            !is_accepted(commit_result->result.code))
        {
            report_result_failure(commit_result->result);
        }
    }
    else {
        term::Ime_preedit_state state;
        if (!preedit_text.isEmpty()) {
            state.text            = preedit_text;
            state.cursor_position = preedit_cursor_position;
            state.active          = true;
        }
        m_private->set_ime_preedit_state(*this, std::move(state));
    }

    event->accept();
}

void VNM_TerminalSurface::refresh_grid_metrics()
{
    m_private->render_font               = term::vnm_terminal_font(m_font_family, m_font_size);
    m_private->render_device_pixel_ratio = current_device_pixel_ratio(window());
    m_private->render_light_theme        = is_light_theme(m_color_theme);
    m_private->grid_metrics_provider.set_font(m_private->render_font);
    m_private->grid_metrics_provider.set_device_pixel_ratio(
        m_private->render_device_pixel_ratio);

    if (!std::isfinite(m_font_size) || m_font_size <= 0.0) {
        m_private->cell_metrics = {};
        if (m_private->session != nullptr) {
            refresh_active_session_geometry();
        }
        else {
            set_grid_size(0, 0);
        }
        m_private->request_render_update(*this);
        return;
    }

    m_private->warm_prompt_text_layouts_for_render_font();
    m_private->cell_metrics = m_private->grid_metrics_provider.cell_metrics();
    const term::Terminal_metrics_result grid_result =
        m_private->grid_metrics_provider.grid_size_for_item_geometry(boundingRect().size());
    if (m_private->session != nullptr) {
        refresh_active_session_geometry();
    }
    else {
        if (grid_result.status == term::Terminal_metrics_status::OK) {
            set_grid_size(grid_result.grid_size.rows, grid_result.grid_size.columns);
        }
        else {
            set_grid_size(0, 0);
        }
    }
    m_private->request_render_update(*this);
    updateInputMethod(Qt::ImCursorRectangle);
}

void VNM_TerminalSurface::set_grid_size(int rows, int columns)
{
    if (m_rows == rows && m_columns == columns) {
        return;
    }

    m_rows    = rows;
    m_columns = columns;
    emit grid_geometry_changed();
}

void VNM_TerminalSurface::set_viewport_state(
    const term::Terminal_viewport_state& state)
{
    const int scrollback_rows = std::max(0, state.scrollback_rows);
    const int visible_rows    = std::max(0, state.visible_rows);
    const int offset_from_tail =
        std::clamp(state.offset_from_tail, 0, scrollback_rows);
    const bool at_tail = offset_from_tail == 0;

    if (m_scrollback_rows           == scrollback_rows  &&
        m_viewport_visible_rows     == visible_rows     &&
        m_viewport_offset_from_tail == offset_from_tail &&
        m_viewport_at_tail          == at_tail)
    {
        return;
    }

    m_scrollback_rows           = scrollback_rows;
    m_viewport_visible_rows     = visible_rows;
    m_viewport_offset_from_tail = offset_from_tail;
    m_viewport_at_tail          = at_tail;
    emit viewport_changed();
}

void VNM_TerminalSurface::set_process_state(Process_state state)
{
    if (m_process_state == state) {
        return;
    }

    m_process_state = state;
    emit process_state_changed();
}

void VNM_TerminalSurface::set_backend_ready(bool ready)
{
    if (m_backend_ready == ready) {
        return;
    }

    m_backend_ready = ready;
    emit backend_ready_changed();
}

void VNM_TerminalSurface::set_backend_geometry_in_sync(bool in_sync)
{
    if (m_backend_geometry_in_sync == in_sync) {
        return;
    }

    m_backend_geometry_in_sync = in_sync;
    emit geometry_sync_changed();
}

void VNM_TerminalSurface::set_selection_state(Selection_state state)
{
    if (m_selection_state == state) {
        return;
    }

    m_selection_state = state;
    emit selection_changed();
}

void VNM_TerminalSurface::bind_window_signals(QQuickWindow* window)
{
    QObject::disconnect(m_private->window_screen_changed_connection);
    QObject::disconnect(m_private->window_scene_graph_invalidated_connection);
    m_private->bound_window = window;
    ++m_private->window_binding_generation;
    const std::uint64_t window_binding_generation =
        m_private->window_binding_generation;
    bind_screen_signals(window != nullptr ? window->screen() : nullptr);

    if (window == nullptr) {
        return;
    }

    m_private->window_screen_changed_connection = QObject::connect(
        window,
        &QWindow::screenChanged,
        this,
        [this](QScreen* screen) {
            bind_screen_signals(screen);
            refresh_grid_metrics();
        });
    m_private->window_scene_graph_invalidated_connection = QObject::connect(
        window,
        &QQuickWindow::sceneGraphInvalidated,
        this,
        [this, window_binding_generation]() {
            handle_scene_graph_invalidated(window_binding_generation);
        },
        Qt::QueuedConnection);
}

void VNM_TerminalSurface::handle_scene_graph_invalidated(
    std::uint64_t window_binding_generation)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (auto lifecycle_recorder = m_private->lifecycle_recorder();
        lifecycle_recorder != nullptr)
    {
        lifecycle_recorder->record_scene_graph_invalidated();
    }
    if (window_binding_generation == m_private->window_binding_generation) {
        m_private->reset_render_update_schedule();
    }
}

void VNM_TerminalSurface::bind_screen_signals(QScreen* screen)
{
    QObject::disconnect(m_private->screen_dpi_changed_connection);
    QObject::disconnect(m_private->screen_physical_dpi_changed_connection);

    if (screen == nullptr) {
        return;
    }

    m_private->screen_dpi_changed_connection = QObject::connect(
        screen,
        &QScreen::logicalDotsPerInchChanged,
        this,
        [this](qreal) {
            refresh_grid_metrics();
        });
    m_private->screen_physical_dpi_changed_connection = QObject::connect(
        screen,
        &QScreen::physicalDotsPerInchChanged,
        this,
        [this](qreal) {
            refresh_grid_metrics();
        });
}

bool VNM_TerminalSurface::start_process_with_backend(
    std::unique_ptr<term::Terminal_backend>    backend,
    QStringList                                argv,
    QString                                    working_directory)
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session != nullptr) {
        drain_backend_callback_events();
        if (is_live_process_state(m_private->session->process_state())) {
            report_backend_error({
                term::Terminal_backend_error_code::START_FAILED,
                QStringLiteral("start requires no active terminal process"),
            });
            return false;
        }

        reset_session();
    }

    if (backend == nullptr) {
        report_backend_error({
            term::Terminal_backend_error_code::START_FAILED,
            QStringLiteral("terminal backend is not available"),
        });
        set_process_state(Process_state::FAILED);
        return false;
    }

    if (!m_terminal_title.isEmpty()) {
        m_terminal_title.clear();
        emit terminal_title_changed();
    }
    if (!m_terminal_icon_name.isEmpty()) {
        m_terminal_icon_name.clear();
        emit terminal_icon_name_changed();
    }
    set_process_state(Process_state::NOT_STARTED);
    set_backend_ready(false);
    set_backend_geometry_in_sync(false);
    m_private->render_snapshot.reset();
    m_private->set_ime_preedit_state(*this, {});
    m_private->last_render_snapshot_generation    = 0U;
    m_private->last_ime_preedit_generation        = 0U;
    m_private->last_backend_error_signal_sequence = 0U;
    m_private->request_render_update(*this);

    term::Terminal_session_config session_config;
    session_config.trace_notification_limit    = k_surface_notification_trace_limit;
    session_config.scrollback_limit            = m_scrollback_limit;
    session_config.backend_output_capture_path = m_backend_output_capture_path;
    session_config.capture_dirty_row_stats     = m_private->dirty_row_stats_enabled;
#if defined(Q_OS_WIN)
    session_config.recover_scrollback_from_primary_repaints = true;
#endif
    session_config.bell_policy.audible_enabled =
        m_audible_bell_policy == Bell_policy::ENABLED;
    session_config.bell_policy.visual_enabled =
        m_visual_bell_policy == Bell_policy::ENABLED;
    session_config.backend_event_notifier = [this] {
        if (m_private->shutting_down.load()) {
            return;
        }

        // One queued GUI wakeup is enough: Terminal_session owns the pending
        // callback commands until the lambda reaches drain_backend_callback_events().
        bool expected = false;
        if (!m_private->session_drain_queued.compare_exchange_strong(expected, true)) {
            return;
        }

        const bool queued = QMetaObject::invokeMethod(
            this,
            [this] {
                m_private->session_drain_queued.store(false);
                if (!m_private->shutting_down.load()) {
                    drain_backend_callback_events();
                }
            },
            Qt::QueuedConnection);
        if (!queued) {
            // If the wakeup could not be posted, clear the latch so a later
            // backend callback can retry instead of losing the drain request.
            m_private->session_drain_queued.store(false);
        }
    };

    m_private->session =
        std::make_unique<term::Terminal_session>(std::move(backend), session_config);
    m_private->resize_controller = std::make_unique<term::Terminal_resize_controller>(
        *m_private->session,
        m_private->grid_metrics_provider);

    term::Terminal_launch_config launch_config;
    launch_config.argv              = std::move(argv);
    launch_config.working_directory = std::move(working_directory);

    const term::Terminal_session_result result =
        m_private->resize_controller->start_from_geometry(
            std::move(launch_config),
            boundingRect().size());
    sync_from_session();
    if (!is_accepted(result.code)) {
        if (m_process_state == Process_state::NOT_STARTED) {
            set_process_state(Process_state::FAILED);
        }
        report_result_failure(result);
        reset_session();
        return false;
    }

    return true;
}

void VNM_TerminalSurface::drain_backend_callback_events()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        return;
    }

    m_private->session->process_backend_callback_events();
    sync_from_session();
}

void VNM_TerminalSurface::refresh_active_session_geometry()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr || m_private->resize_controller == nullptr) {
        return;
    }

    if (!is_live_process_state(m_private->session->process_state())) {
        sync_from_session();
        return;
    }

    term::Terminal_session* const session = m_private->session.get();
    session->process_backend_callback_events();
    sync_from_session();
    if (m_private->session.get() != session ||
        !is_live_process_state(session->process_state()))
    {
        return;
    }

    const term::Terminal_session_result result =
        m_private->resize_controller->refresh_from_geometry(boundingRect().size());
    sync_from_session();
    if (!is_accepted(result.code)) {
        report_result_failure(result);
    }
}

void VNM_TerminalSurface::sync_from_session()
{
    Q_ASSERT(thread() == QThread::currentThread());
    VNM_TERMINAL_PROFILE_SCOPE("VNM_TerminalSurface::sync_from_session");

    if (m_private->session == nullptr) {
        return;
    }

    set_process_state(surface_process_state(m_private->session->process_state()));
    set_backend_ready(m_private->session->backend_ready());
    set_backend_geometry_in_sync(m_private->session->backend_geometry_in_sync());
    set_selection_state(
        m_private->session->has_selection()
            ? Selection_state::ACTIVE
            : Selection_state::NONE);

    const term::terminal_grid_size_t grid_size = m_private->session->grid_size();
    set_grid_size(grid_size.rows, grid_size.columns);

    const bool live_mouse_reporting_active = m_private->session->mouse_reporting_active();
    if (live_mouse_reporting_active != m_private->last_sgr_mouse_reporting_active) {
        m_private->clear_mouse_wheel_remainders();
    }
    m_private->last_sgr_mouse_reporting_active = live_mouse_reporting_active;

    const bool live_alternate_scroll_active = m_private->session->alternate_scroll_active();
    const std::uint64_t alternate_scroll_mode_generation =
        m_private->session->alternate_scroll_mode_generation();
    if (alternate_scroll_mode_generation !=
        m_private->last_alternate_scroll_mode_generation)
    {
        m_private->clear_mouse_wheel_remainders();
    }
    m_private->last_alternate_scroll_mode_generation = alternate_scroll_mode_generation;

    if (live_alternate_scroll_active != m_private->last_alternate_scroll_active) {
        m_private->clear_mouse_wheel_remainders();
    }
    m_private->last_alternate_scroll_active = live_alternate_scroll_active;

    const std::uint64_t snapshot_generation =
        m_private->session->render_snapshot_generation();
    if (snapshot_generation != m_private->last_render_snapshot_generation) {
        term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
            *this,
            m_private->session->latest_render_snapshot_handle());
        m_private->session->mark_render_snapshot_synced(snapshot_generation);
        m_private->last_render_snapshot_generation = snapshot_generation;
        if (m_private->render_snapshot != nullptr) {
            set_viewport_state(m_private->render_snapshot->viewport);
        }

        const bool sgr_mouse_reporting_active =
            snapshot_has_sgr_mouse_reporting(m_private->render_snapshot);
        const bool mouse_reporting_mode_changed =
            m_private->render_snapshot != nullptr &&
            m_private->render_snapshot->metadata.mouse_reporting_mode_changed;
        if (mouse_reporting_mode_changed ||
            sgr_mouse_reporting_active != live_mouse_reporting_active)
        {
            m_private->clear_mouse_wheel_remainders();
        }
    }

    const std::uint64_t ime_preedit_generation =
        m_private->session->ime_preedit_generation();
    if (ime_preedit_generation != m_private->last_ime_preedit_generation) {
        m_private->set_ime_preedit_state(*this, m_private->session->ime_preedit_state());
        m_private->last_ime_preedit_generation = ime_preedit_generation;
    }

    const std::vector<term::Terminal_session_notification> notifications =
        m_private->session->take_pending_notifications();
    for (const term::Terminal_session_notification& notification : notifications) {
        replay_session_notification(notification);
    }

    sync_synchronized_output_recovery_timer();
}

void VNM_TerminalSurface::sync_synchronized_output_recovery_timer()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr ||
        !m_private->session->render_publication_blocked())
    {
        m_private->synchronized_output_recovery_timer.stop();
        return;
    }

    if (!m_private->synchronized_output_recovery_timer.isActive() ||
        m_private->synchronized_output_recovery_timer.interval() !=
            m_synchronized_output_stale_timeout_ms)
    {
        m_private->synchronized_output_recovery_timer.start(
            m_synchronized_output_stale_timeout_ms);
    }
}

void VNM_TerminalSurface::handle_synchronized_output_recovery_timeout()
{
    Q_ASSERT(thread() == QThread::currentThread());

    if (m_private->session == nullptr) {
        return;
    }

    term::Terminal_session* const session = m_private->session.get();
    session->process_backend_callback_events();
    sync_from_session();
    if (m_private->session.get() != session ||
        !session->render_publication_blocked())
    {
        return;
    }

    const term::Terminal_session_result result =
        session->force_release_synchronized_output();
    sync_from_session();
    if (m_private->session.get() != session) {
        return;
    }

    if (!is_accepted(result.code)) {
        report_result_failure(result);
    }
}

void VNM_TerminalSurface::replay_session_notification(
    const term::Terminal_session_notification& notification)
{
    switch (notification.kind) {
        case term::Terminal_session_notification_kind::PROCESS_STARTED:
            emit process_started();
            break;
        case term::Terminal_session_notification_kind::PROCESS_EXITED:
            m_private->pending_clipboard_write.reset();
            if (notification.exit.has_value()) {
                emit process_exited(
                    surface_exit_reason(notification.exit->reason),
                    notification.exit->exit_code);
            }
            break;
        case term::Terminal_session_notification_kind::SNAPSHOT_READY:
            break;
        case term::Terminal_session_notification_kind::OUTPUT_ACTIVITY:
            emit output_activity();
            break;
        case term::Terminal_session_notification_kind::OUTPUT_BACKPRESSURE_CHANGED:
            emit output_backpressure_changed(notification.backpressure_active);
            break;
        case term::Terminal_session_notification_kind::BACKEND_ERROR:
            if (notification.backend_error.has_value()) {
                report_backend_error(*notification.backend_error, notification.sequence);
            }
            break;
        case term::Terminal_session_notification_kind::BELL_REQUESTED:
            emit bell_requested();
            break;
        case term::Terminal_session_notification_kind::TITLE_CHANGED:
            if (m_terminal_title != notification.message) {
                m_terminal_title = notification.message;
                emit terminal_title_changed();
            }
            break;
        case term::Terminal_session_notification_kind::ICON_NAME_CHANGED:
            if (m_terminal_icon_name != notification.message) {
                m_terminal_icon_name = notification.message;
                emit terminal_icon_name_changed();
            }
            break;
        case term::Terminal_session_notification_kind::RESIZE_TRANSACTION:
            break;
        case term::Terminal_session_notification_kind::TEXT_AREA_RESIZE_REQUESTED:
            if (notification.text_area_resize_request.has_value()) {
                emit text_area_resize_requested(
                    notification.text_area_resize_request->rows,
                    notification.text_area_resize_request->columns);
            }
            break;
        case term::Terminal_session_notification_kind::HOST_REQUEST:
            if (notification.clipboard_write_request.has_value()) {
                m_private->pending_clipboard_write = *notification.clipboard_write_request;
                m_private->pending_clipboard_write->request_id =
                    m_private->next_clipboard_write_request_id++;
                if (m_private->next_clipboard_write_request_id == 0U) {
                    m_private->next_clipboard_write_request_id = 1U;
                }
                emit clipboard_write_requested(
                    m_private->pending_clipboard_write->request_id,
                    m_private->pending_clipboard_write->target_selection,
                    m_private->pending_clipboard_write->decoded_payload);
            }
            break;
    }
}

void VNM_TerminalSurface::report_backend_error(
    term::Terminal_backend_error   error,
    quint64                        sequence)
{
    if (sequence != 0U) {
        m_private->last_backend_error_signal_sequence = sequence;
    }

    emit backend_error(surface_backend_error_code(error.code), std::move(error.message));
}

void VNM_TerminalSurface::report_result_failure(
    const term::Terminal_session_result& result)
{
    if (result.error.has_value()) {
        if (result.sequence != 0U &&
            result.sequence == m_private->last_backend_error_signal_sequence)
        {
            return;
        }

        report_backend_error(*result.error, result.sequence);
        return;
    }

    report_backend_error({
        term::Terminal_backend_error_code::START_FAILED,
        QStringLiteral("terminal session command failed"),
    });
}

void VNM_TerminalSurface::reset_session()
{
    m_private->synchronized_output_recovery_timer.stop();
    m_private->resize_controller.reset();
    m_private->session.reset();
    m_private->session_drain_queued.store(false);
    m_private->clear_mouse_reporting_state();
    m_private->clear_selection_drag_state();
    m_private->pending_clipboard_write.reset();
    m_private->clear_wheel_remainders();
    m_private->last_sgr_mouse_reporting_active       = false;
    m_private->last_alternate_scroll_active          = false;
    m_private->last_alternate_scroll_mode_generation = 0U;
    set_selection_state(Selection_state::NONE);
    term::Terminal_viewport_state empty_viewport;
    empty_viewport.visible_rows = std::max(0, m_rows);
    set_viewport_state(empty_viewport);
    if (m_private->shutting_down.load()) {
        m_private->ime_preedit = {};
    }
    else {
        m_private->set_ime_preedit_state(*this, {});
    }
    m_private->last_ime_preedit_generation        = 0U;
    m_private->last_backend_error_signal_sequence = 0U;
}

QSGNode* VNM_TerminalSurface::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*)
{
    const auto update_node = [&]() -> QSGNode* {
        const QSizeF logical_size       = boundingRect().size();
        const qreal  device_pixel_ratio = m_private->render_device_pixel_ratio;

        if (window()              == nullptr   ||
            !std::isfinite(device_pixel_ratio) ||
            device_pixel_ratio    <= 0.0       ||
            !logical_size.isValid()            ||
            logical_size.width()  <= 0.0       ||
            logical_size.height() <= 0.0)
        {
            m_private->render_node_release_pending        = false;
            m_private->render_node_release_requeue_update = false;
            m_private->reset_render_update_schedule();
            m_private->renderer_stats_publisher.publish({});
            if (old_node != nullptr) {
                if (auto lifecycle_recorder = m_private->lifecycle_recorder();
                lifecycle_recorder != nullptr)
                {
                    lifecycle_recorder->record_render_node_deleted();
                }
            }
            delete old_node;
            return nullptr;
        }

        if (m_private->render_node_release_pending) {
            const bool requeue_render_update = m_private->render_node_release_requeue_update;
            m_private->render_node_release_pending        = false;
            m_private->render_node_release_requeue_update = false;
            m_private->reset_render_update_schedule();
            m_private->renderer_stats_publisher.publish({});
            if (old_node != nullptr) {
                if (auto lifecycle_recorder = m_private->lifecycle_recorder();
                lifecycle_recorder != nullptr)
                {
                    lifecycle_recorder->record_render_node_deleted();
                }
                delete old_node;
            }
            if (requeue_render_update) {
                m_private->request_render_update(*this);
            }
            return nullptr;
        }

        const term::Terminal_render_options options = render_options_for_surface(*this);
        const term::Terminal_render_frame frame = term::build_terminal_render_frame(
            m_private->render_snapshot.get(),
            logical_size,
            m_private->cell_metrics,
            options,
            m_private->cursor_blink_visible,
            &m_private->ime_preedit);
        term::terminal_renderer_stats_t renderer_stats;
        QSGNode* updated_node = m_private->renderer.update_node(
            old_node,
            window(),
            frame,
            m_private->render_font,
            device_pixel_ratio,
            m_private->lifecycle_recorder(),
            renderer_stats
#if VNM_TERMINAL_PROFILING_ENABLED
            ,
            m_private->slow_text_layout_recorder_handle()
#endif
        );
        m_private->renderer_stats_publisher.publish(renderer_stats);
        if (updated_node != nullptr && renderer_stats.paint_completed) {
            m_private->consume_render_update(window());
            m_private->render_invalidation_stats.last_rendered_snapshot_sequence =
                m_private->render_snapshot != nullptr
                    ? m_private->render_snapshot->metadata.sequence
                    : 0U;
        }
        else {
            m_private->reset_render_update_schedule();
        }
        return updated_node;
    };

#if VNM_TERMINAL_PROFILING_ENABLED
    const std::shared_ptr<term::Hierarchical_profiler> render_profiler =
        m_private->render_profiler_handle();
    QSGNode* result_node = nullptr;
    {
        term::Active_profiler_binding render_profiler_binding(render_profiler.get());
        VNM_TERMINAL_PROFILE_SCOPE("VNM_TerminalSurface::updatePaintNode");
        result_node = update_node();
    }

    m_private->publish_render_profiler_snapshot(
        render_profiler,
        m_private->render_invalidation_stats.last_rendered_snapshot_sequence);
    return result_node;
#else
    return update_node();
#endif
}

void term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
    VNM_TerminalSurface&                               surface,
    std::shared_ptr<const Terminal_render_snapshot>    snapshot)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    if (surface.m_private->render_update_pending &&
        surface.m_private->render_snapshot != nullptr &&
        snapshot                           != nullptr)
    {
        snapshot = snapshot_with_coalesced_dirty_rows(
            *surface.m_private->render_snapshot,
            *snapshot);
    }
    surface.m_private->render_snapshot = std::move(snapshot);
    surface.updateInputMethod(Qt::ImCursorRectangle);
    surface.m_private->request_render_update(surface);
}

void term::VNM_TerminalSurface_render_bridge::set_render_profiler(
    VNM_TerminalSurface&                               surface,
    std::shared_ptr<Hierarchical_profiler>             profiler)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_private->set_render_profiler(std::move(profiler));
}

term::Render_profile_snapshot_t
term::VNM_TerminalSurface_render_bridge::render_profiler_snapshot(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->current_render_profiler_snapshot();
}

void term::VNM_TerminalSurface_render_bridge::set_dirty_row_stats_enabled(
    VNM_TerminalSurface&   surface,
    bool                   enabled)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_private->dirty_row_stats_enabled = enabled;
    if (surface.m_private->session != nullptr) {
        surface.m_private->session->set_dirty_row_stats_enabled(enabled);
    }
}

term::Terminal_screen_model_dirty_row_stats
term::VNM_TerminalSurface_render_bridge::dirty_row_stats(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->session != nullptr
        ? surface.m_private->session->dirty_row_stats()
        : term::Terminal_screen_model_dirty_row_stats{};
}

term::Terminal_screen_model_dirty_row_timeline
term::VNM_TerminalSurface_render_bridge::dirty_row_timeline(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->session != nullptr
        ? surface.m_private->session->dirty_row_timeline()
        : term::Terminal_screen_model_dirty_row_timeline{};
}

void term::VNM_TerminalSurface_render_bridge::set_cursor_blink_visible(
    VNM_TerminalSurface&               surface,
    bool                               visible)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.m_private->cursor_blink_visible = visible;
    surface.m_private->request_render_update(surface);
}

bool term::VNM_TerminalSurface_render_bridge::start_process_with_backend(
    VNM_TerminalSurface&               surface,
    std::unique_ptr<Terminal_backend>  backend,
    QStringList                        argv,
    QString                            working_directory)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.start_process_with_backend(
        std::move(backend),
        std::move(argv),
        std::move(working_directory));
}

bool term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->session_drain_queued.load();
}

void term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.drain_backend_callback_events();
}

std::shared_ptr<const term::Terminal_render_snapshot>
term::VNM_TerminalSurface_render_bridge::render_snapshot(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->render_snapshot;
}

term::Ime_preedit_state term::VNM_TerminalSurface_render_bridge::ime_preedit_state(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->ime_preedit;
}

term::Terminal_surface_render_invalidation_stats_t
term::VNM_TerminalSurface_render_bridge::invalidation_stats(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->current_invalidation_stats();
}

term::terminal_renderer_stats_t
term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->renderer_stats_publisher.snapshot();
}

term::terminal_renderer_cumulative_stats_t
term::VNM_TerminalSurface_render_bridge::cumulative_renderer_stats(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->renderer_stats_publisher.cumulative_snapshot();
}

term::terminal_renderer_lifecycle_stats_t
term::VNM_TerminalSurface_render_bridge::lifecycle_stats(
    const VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    const auto lifecycle_recorder = surface.m_private->lifecycle_recorder();
    return lifecycle_recorder != nullptr
        ? lifecycle_recorder->snapshot()
        : term::terminal_renderer_lifecycle_stats_t{};
}

std::shared_ptr<term::Terminal_renderer_lifecycle_recorder>
term::VNM_TerminalSurface_render_bridge::lifecycle_recorder(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    return surface.m_private->ensure_lifecycle_recorder();
}

void term::VNM_TerminalSurface_render_bridge::release_resources(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.releaseResources();
}

void term::VNM_TerminalSurface_render_bridge::simulate_scene_graph_invalidated(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    surface.handle_scene_graph_invalidated(surface.m_private->window_binding_generation);
}

void term::VNM_TerminalSurface_render_bridge::simulate_stale_scene_graph_invalidated(
    VNM_TerminalSurface& surface)
{
    Q_ASSERT(surface.thread() == QThread::currentThread());
    const std::uint64_t generation = surface.m_private->window_binding_generation;
    surface.handle_scene_graph_invalidated(
        generation == 0U
            ? ~std::uint64_t{0U}
            : generation - 1U);
}
