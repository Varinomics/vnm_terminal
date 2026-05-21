#include "vnm_terminal/internal/qsg_terminal_renderer.h"

#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/qsg_terminal_render_frame.h"
#include "vnm_terminal/internal/unicode_width.h"
#include <QByteArray>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QLatin1Char>
#include <QMatrix4x4>
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGGeometryNode>
#include <QSGOpacityNode>
#include <QSGRendererInterface>
#include <QSGSimpleRectNode>
#include <QSGTextNode>
#include <QSGVertexColorMaterial>
#include <QTextLayout>
#include <QTextOption>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

namespace vnm_terminal::internal {

namespace {

constexpr qreal       k_no_wrap_text_line_width             = 1024.0 * 1024.0;
constexpr qreal       k_pi                                  = 3.14159265358979323846;
constexpr qreal       k_graphic_antialias_feather           = 0.5;
constexpr qreal       k_ascii_advance_tolerance_floor       = 0.000001;
constexpr qreal       k_ascii_advance_tolerance_ratio       = 0.0000001;
constexpr qreal       k_text_geometry_tolerance             = k_ascii_advance_tolerance_floor;
constexpr std::size_t k_max_coalesced_ascii_text_run_length = 384U;
constexpr std::size_t k_eager_render_style_attribute_limit  = 256U;
constexpr int         k_arc_geometry_segment_count          = 32;
constexpr int         k_flat_rect_vertices_per_rect         = 6;
#if VNM_TERMINAL_PROFILING_ENABLED
constexpr std::uint64_t k_slow_text_layout_threshold_ns           = 10U * 1000U * 1000U;
constexpr std::size_t   k_max_slow_text_layout_samples            = 16U;
constexpr int           k_slow_text_layout_preview_utf16_units    = 96;
constexpr int           k_slow_text_layout_codepoint_sample_count = 24;

struct text_layout_diagnostic_context_t
{
    const QFont*                    font = nullptr;
    const Terminal_render_text_run* run = nullptr;
    bool                            clipped = false;
    bool                            force_blended_order = false;
    bool                            ascii_layout_font = false;
};

thread_local Terminal_text_layout_slow_diagnostics_recorder*
    active_slow_text_layout_recorder = nullptr;
thread_local const text_layout_diagnostic_context_t*
    active_text_layout_diagnostic_context = nullptr;

class Active_slow_text_layout_recorder_binding
{
public:
    explicit Active_slow_text_layout_recorder_binding(
        Terminal_text_layout_slow_diagnostics_recorder* recorder)
    :
        m_previous(active_slow_text_layout_recorder)
    {
        active_slow_text_layout_recorder = recorder;
    }

    ~Active_slow_text_layout_recorder_binding()
    {
        active_slow_text_layout_recorder = m_previous;
    }

    Active_slow_text_layout_recorder_binding(
        const Active_slow_text_layout_recorder_binding&) = delete;
    Active_slow_text_layout_recorder_binding& operator=(
        const Active_slow_text_layout_recorder_binding&) = delete;

private:
    Terminal_text_layout_slow_diagnostics_recorder* m_previous = nullptr;
};

class Active_text_layout_diagnostic_context_binding
{
public:
    explicit Active_text_layout_diagnostic_context_binding(
        const text_layout_diagnostic_context_t* context)
    :
        m_previous(active_text_layout_diagnostic_context)
    {
        active_text_layout_diagnostic_context = context;
    }

    ~Active_text_layout_diagnostic_context_binding()
    {
        active_text_layout_diagnostic_context = m_previous;
    }

    Active_text_layout_diagnostic_context_binding(
        const Active_text_layout_diagnostic_context_binding&) = delete;
    Active_text_layout_diagnostic_context_binding& operator=(
        const Active_text_layout_diagnostic_context_binding&) = delete;

private:
    const text_layout_diagnostic_context_t* m_previous = nullptr;
};

std::uint32_t codepoint_at(const QString& text, int& index)
{
    const QChar current = text.at(index);
    if (current.isHighSurrogate() && index + 1 < text.size()) {
        const QChar next = text.at(index + 1);
        if (next.isLowSurrogate()) {
            index += 2;
            return 0x10000U +
                (((static_cast<std::uint32_t>(current.unicode()) - 0xD800U) << 10U) |
                    (static_cast<std::uint32_t>(next.unicode()) - 0xDC00U));
        }
    }

    ++index;
    return static_cast<std::uint32_t>(current.unicode());
}

int text_codepoint_count(const QString& text)
{
    int count = 0;
    for (int index = 0; index < text.size();) {
        (void)codepoint_at(text, index);
        ++count;
    }
    return count;
}

std::uint64_t text_hash(const QString& text)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const QChar character : text) {
        hash ^= static_cast<std::uint64_t>(character.unicode());
        hash *= 1099511628211ULL;
    }
    return hash;
}

QString text_codepoint_sample(const QString& text)
{
    QString sample;
    int emitted = 0;
    int index   = 0;
    for (;
        index < text.size() && emitted < k_slow_text_layout_codepoint_sample_count;)
    {
        const std::uint32_t codepoint = codepoint_at(text, index);
        if (!sample.isEmpty()) {
            sample += QLatin1Char(' ');
        }
        sample += QStringLiteral("U+%1")
            .arg(
                static_cast<qulonglong>(codepoint),
                codepoint > 0xFFFFU ? 6 : 4,
                16,
                QLatin1Char('0'))
            .toUpper();
        ++emitted;
    }
    if (index < text.size()) {
        sample += QStringLiteral(" ...");
    }
    return sample;
}

void classify_text(
    const QString& text,
    bool&          out_ascii_only,
    bool&          out_printable_ascii_only,
    bool&          out_has_control_codepoint)
{
    out_ascii_only            = true;
    out_printable_ascii_only  = true;
    out_has_control_codepoint = false;

    for (const QChar character : text) {
        const ushort value = character.unicode();
        if (value > 0x7FU) {
            out_ascii_only = false;
            out_printable_ascii_only = false;
        }
        if (value < 0x20U || value == 0x7FU) {
            out_has_control_codepoint = true;
            out_printable_ascii_only = false;
        }
    }
}

terminal_text_layout_slow_diagnostic_t make_slow_text_layout_diagnostic(
    std::uint64_t                      duration_ns,
    const QFont&                       font,
    const Terminal_render_text_run&    run,
    bool                               clipped,
    bool                               force_blended_order,
    bool                               ascii_layout_font,
    bool                               line_has_text)
{
    terminal_text_layout_slow_diagnostic_t diagnostic;
    diagnostic.duration_ns              = duration_ns;
    diagnostic.text_hash                = text_hash(run.text);
    diagnostic.text_utf16_units         = run.text.size();
    diagnostic.text_codepoints          = text_codepoint_count(run.text);
    diagnostic.row                      = run.row;
    diagnostic.logical_row              = run.logical_row;
    diagnostic.column                   = run.column;
    diagnostic.style_id                 = static_cast<int>(run.style_id);
    diagnostic.hyperlink_id             = static_cast<std::uint64_t>(run.hyperlink_id);
    diagnostic.rect_width               = run.rect.width();
    diagnostic.rect_height              = run.rect.height();
    diagnostic.font_point_size          = font.pointSizeF();
    diagnostic.font_pixel_size          = font.pixelSize();
    diagnostic.font_weight              = font.weight();
    diagnostic.clipped                  = clipped;
    diagnostic.force_blended_order      = force_blended_order;
    diagnostic.ascii_layout_font        = ascii_layout_font;
    diagnostic.line_has_text            = line_has_text;
    diagnostic.font_italic              = font.italic();
    diagnostic.font_family              = font.family();
    diagnostic.font_style_name          = font.styleName();
    const QFontInfo font_info(font);
    diagnostic.resolved_font_family     = font_info.family();
    diagnostic.resolved_font_style_name = font_info.styleName();
    diagnostic.text_preview_truncated =
        run.text.size() > k_slow_text_layout_preview_utf16_units;
    diagnostic.text_preview     = run.text.left(k_slow_text_layout_preview_utf16_units);
    diagnostic.codepoint_sample = text_codepoint_sample(run.text);
    classify_text(
        run.text,
        diagnostic.ascii_only,
        diagnostic.printable_ascii_only,
        diagnostic.has_control_codepoint);
    return diagnostic;
}
#endif

QColor qcolor_from_rgba(quint32 rgba)
{
    return QColor::fromRgba(rgba);
}

QColor resolved_color(
    const Terminal_color_ref&          color,
    const Terminal_color_state&        color_state,
    bool                               foreground)
{
    return qcolor_from_rgba(resolve_terminal_color_ref(color, color_state, foreground));
}

QRectF cell_rect(
    int                                row,
    int                                column,
    int                                column_count,
    terminal_cell_metrics_t            metrics)
{
    return
        QRectF(
            static_cast<qreal>(column) * metrics.width,
            static_cast<qreal>(row) * metrics.height,
            static_cast<qreal>(column_count) * metrics.width,
            metrics.height);
}

QColor effective_foreground(
    const Terminal_text_style&         style,
    const Terminal_render_snapshot&    snapshot)
{
    return resolved_color(style.foreground, snapshot.color_state, true);
}

QColor effective_background(
    const Terminal_text_style&         style,
    const Terminal_render_snapshot&    snapshot)
{
    return resolved_color(style.background, snapshot.color_state, false);
}

QColor default_terminal_background(
    const Terminal_render_snapshot& snapshot)
{
    const Terminal_text_style& default_style = snapshot.styles.empty()
        ? make_default_terminal_text_style()
        : snapshot.styles.front();
    return snapshot.modes.reverse_video
        ? effective_foreground(default_style, snapshot)
        : effective_background(default_style, snapshot);
}

bool style_is_inverse(
    const Terminal_text_style&         style,
    const Terminal_render_snapshot&    snapshot)
{
    return terminal_style_has_attribute(style, Terminal_style_attribute::INVERSE) !=
        snapshot.modes.reverse_video;
}

struct render_style_attributes_t
{
    QColor foreground;
    QColor background;
    bool   underline = false;
    bool   strike    = false;
};

render_style_attributes_t render_style_attributes(
    const Terminal_text_style&         style,
    const Terminal_render_snapshot&    snapshot)
{
    QColor foreground = effective_foreground(style, snapshot);
    QColor background = effective_background(style, snapshot);
    if (style_is_inverse(style, snapshot)) {
        std::swap(foreground, background);
    }
    if (terminal_style_has_attribute(style, Terminal_style_attribute::INVISIBLE)) {
        foreground = background;
    }

    return {
        foreground,
        background,
        terminal_style_has_attribute(style, Terminal_style_attribute::UNDERLINE),
        terminal_style_has_attribute(style, Terminal_style_attribute::STRIKE),
    };
}

class Render_style_attribute_cache
{
public:
    explicit Render_style_attribute_cache(const Terminal_render_snapshot& snapshot)
    :
        m_snapshot(snapshot)
    {
        if (snapshot.styles.size() > k_eager_render_style_attribute_limit) {
            return;
        }

        m_attributes_by_index.reserve(snapshot.styles.size());
        for (const Terminal_text_style& style : snapshot.styles) {
            m_attributes_by_index.push_back(render_style_attributes(style, snapshot));
        }
    }

    const render_style_attributes_t& attributes(Terminal_style_id style_id)
    {
        if (!m_attributes_by_index.empty()) {
            return m_attributes_by_index[static_cast<std::size_t>(style_id)];
        }

        const auto found = m_attributes.find(style_id);
        if (found != m_attributes.end()) {
            return found->second;
        }

        const auto inserted = m_attributes.emplace(
            style_id,
            render_style_attributes(
                m_snapshot.styles[static_cast<std::size_t>(style_id)],
                m_snapshot));
        return inserted.first->second;
    }

private:
    const Terminal_render_snapshot&                        m_snapshot;
    std::vector<render_style_attributes_t>                 m_attributes_by_index;
    std::map<Terminal_style_id, render_style_attributes_t> m_attributes;
};

QRectF cursor_rect(
    const Terminal_render_cursor&  cursor,
    Terminal_cursor_shape          shape,
    terminal_cell_metrics_t        metrics)
{
    QRectF rect = cell_rect(cursor.position.row, cursor.position.column, 1, metrics);
    switch (shape) {
        case Terminal_cursor_shape::BLOCK:
            return rect;
        case Terminal_cursor_shape::BAR:
            rect.setWidth(std::max<qreal>(1.0, metrics.width * 0.18));
            return rect;
        case Terminal_cursor_shape::UNDERLINE:
            rect.setTop(rect.bottom() - std::max<qreal>(1.0, metrics.height * 0.16));
            return rect;
    }

    return rect;
}

void append_positive_rect(std::vector<QRectF>& rects, QRectF rect)
{
    if (rect.width() > 0.0 && rect.height() > 0.0) {
        rects.push_back(rect);
    }
}

void append_rect_difference(
    std::vector<QRectF>&   rects,
    const QRectF&          source,
    const QRectF&          cut)
{
    const QRectF clipped = source.intersected(cut);
    if (clipped.width() <= 0.0 || clipped.height() <= 0.0) {
        rects.push_back(source);
        return;
    }

    append_positive_rect(
        rects,
        QRectF(source.left(), source.top(), source.width(), clipped.top() - source.top()));
    append_positive_rect(
        rects,
        QRectF(source.left(), clipped.bottom(), source.width(), source.bottom() - clipped.bottom()));
    append_positive_rect(
        rects,
        QRectF(source.left(), clipped.top(), clipped.left() - source.left(), clipped.height()));
    append_positive_rect(
        rects,
        QRectF(clipped.right(), clipped.top(), source.right() - clipped.right(), clipped.height()));
}

std::vector<QRectF> cursor_rects_excluding_graphics(
    const QRectF&                              cursor,
    const std::vector<Terminal_render_rect>&   graphic_rects)
{
    std::vector<QRectF> remaining{cursor};
    for (const Terminal_render_rect& graphic_rect : graphic_rects) {
        std::vector<QRectF> next;
        for (const QRectF& rect : remaining) {
            append_rect_difference(next, rect, graphic_rect.rect);
        }
        remaining = std::move(next);
    }
    return remaining;
}

QRectF decoration_rect(
    const Terminal_render_text_run&            run,
    Terminal_render_decoration_kind            kind,
    terminal_cell_metrics_t                    metrics)
{
    const qreal thickness = std::max<qreal>(1.0, std::floor(metrics.height * 0.08));
    QRectF      rect      = run.rect;
    switch (kind) {
        case Terminal_render_decoration_kind::UNDERLINE:
        case Terminal_render_decoration_kind::HYPERLINK_UNDERLINE:
            rect.setTop(run.rect.top() + metrics.ascent + thickness);
            rect.setHeight(thickness);
            return rect;
        case Terminal_render_decoration_kind::STRIKE:
            rect.setTop(run.rect.top() + metrics.ascent * 0.55);
            rect.setHeight(thickness);
            return rect;
        case Terminal_render_decoration_kind::PREEDIT_CARET:
            rect.setWidth(thickness);
            return rect;
    }

    return rect;
}

qreal terminal_graphic_light_stroke(terminal_cell_metrics_t metrics)
{
    return std::max<qreal>(
        1.0,
        std::floor(std::min(metrics.width, metrics.height) * 0.12));
}

qreal terminal_graphic_heavy_stroke(terminal_cell_metrics_t metrics)
{
    return
        std::max<qreal>(
            terminal_graphic_light_stroke(metrics) + 1.0,
            std::floor(std::min(metrics.width, metrics.height) * 0.20));
}

void append_terminal_graphic_rect(
    std::vector<Terminal_render_rect>& rects,
    QRectF                             rect,
    const QColor&                      color,
    bool                               antialias = false)
{
    if (rect.width() <= 0.0 || rect.height() <= 0.0) {
        return;
    }

    rects.push_back({rect, color, antialias});
}

void append_terminal_graphic_arc(
    std::vector<Terminal_render_arc>&  arcs,
    Terminal_render_arc_kind           kind,
    const QRectF&                      cell,
    const QColor&                      color,
    qreal                              stroke)
{
    if (cell.width() <= 0.0 || cell.height() <= 0.0 || stroke <= 0.0) {
        return;
    }

    arcs.push_back({kind, cell, color, stroke});
}

QRectF centered_horizontal_rect(
    const QRectF&  cell,
    qreal          left,
    qreal          right,
    qreal          stroke)
{
    return QRectF(
        left,
        cell.center().y() - stroke * 0.5,
        right - left,
        stroke);
}

QRectF centered_vertical_rect(
    const QRectF&  cell,
    qreal          top,
    qreal          bottom,
    qreal          stroke)
{
    return QRectF(
        cell.center().x() - stroke * 0.5,
        top,
        stroke,
        bottom - top);
}

void append_terminal_box_drawing_rects(
    std::vector<Terminal_render_rect>& rects,
    const QRectF&                      cell,
    const QColor&                      color,
    bool                               connects_left,
    bool                               connects_right,
    bool                               connects_up,
    bool                               connects_down,
    qreal                              stroke)
{
    const qreal center_x = cell.center().x();
    const qreal center_y = cell.center().y();

    if (connects_left || connects_right) {
        const qreal left  = connects_left ? cell.left() : center_x;
        const qreal right = connects_right ? cell.right() : center_x;
        append_terminal_graphic_rect(
            rects,
            centered_horizontal_rect(cell, left, right, stroke),
            color,
            true);
    }
    if (connects_up || connects_down) {
        const qreal top    = connects_up ? cell.top() : center_y;
        const qreal bottom = connects_down ? cell.bottom() : center_y;
        append_terminal_graphic_rect(
            rects,
            centered_vertical_rect(cell, top, bottom, stroke),
            color,
            true);
    }
}

constexpr unsigned int k_box_left  = 0x01U;
constexpr unsigned int k_box_right = 0x02U;
constexpr unsigned int k_box_up    = 0x04U;
constexpr unsigned int k_box_down  = 0x08U;

struct terminal_box_stroke_spec_t
{
    ushort                     codepoint;
    unsigned int               connections;
    bool                       heavy;
};

struct terminal_box_arc_spec_t
{
    ushort                     codepoint;
    Terminal_render_arc_kind   kind;
};

constexpr terminal_box_stroke_spec_t k_terminal_box_stroke_specs[] = {
    {0x2500, k_box_left | k_box_right, false}, // light horizontal
    {0x2501, k_box_left | k_box_right, true},  // heavy horizontal
    {0x2502, k_box_up | k_box_down, false},    // light vertical
    {0x2503, k_box_up | k_box_down, true},     // heavy vertical
    {0x250c, k_box_right | k_box_down, false}, // light down and right
    {0x250f, k_box_right | k_box_down, true},  // heavy down and right
    {0x2510, k_box_left | k_box_down, false},  // light down and left
    {0x2513, k_box_left | k_box_down, true},   // heavy down and left
    {0x2514, k_box_right | k_box_up, false},   // light up and right
    {0x2517, k_box_right | k_box_up, true},    // heavy up and right
    {0x2518, k_box_left | k_box_up, false},    // light up and left
    {0x251b, k_box_left | k_box_up, true},     // heavy up and left
    {0x251c, k_box_right | k_box_up | k_box_down, false}, // light vertical and right
    {0x2523, k_box_right | k_box_up | k_box_down, true},  // heavy vertical and right
    {0x2524, k_box_left | k_box_up | k_box_down, false},  // light vertical and left
    {0x252b, k_box_left | k_box_up | k_box_down, true},   // heavy vertical and left
    {0x252c, k_box_left | k_box_right | k_box_down, false}, // light down and horizontal
    {0x2533, k_box_left | k_box_right | k_box_down, true},  // heavy down and horizontal
    {0x2534, k_box_left | k_box_right | k_box_up, false},   // light up and horizontal
    {0x253b, k_box_left | k_box_right | k_box_up, true},    // heavy up and horizontal
    {0x253c, k_box_left | k_box_right | k_box_up | k_box_down, false}, // light vertical and horizontal
    {0x254b, k_box_left | k_box_right | k_box_up | k_box_down, true},  // heavy vertical and horizontal
    {0x2574, k_box_left, false},  // light left
    {0x2575, k_box_up, false},    // light up
    {0x2576, k_box_right, false}, // light right
    {0x2577, k_box_down, false},  // light down
};

constexpr terminal_box_arc_spec_t k_terminal_box_arc_specs[] = {
    {0x256d, Terminal_render_arc_kind::DOWN_RIGHT}, // light arc down and right
    {0x256e, Terminal_render_arc_kind::DOWN_LEFT},  // light arc down and left
    {0x256f, Terminal_render_arc_kind::UP_LEFT},    // light arc up and left
    {0x2570, Terminal_render_arc_kind::UP_RIGHT},   // light arc up and right
};

constexpr bool terminal_box_connects(unsigned int connections, unsigned int connection)
{
    return (connections & connection) != 0U;
}

const terminal_box_stroke_spec_t* find_terminal_box_stroke_spec(ushort codepoint)
{
    for (const terminal_box_stroke_spec_t& spec : k_terminal_box_stroke_specs) {
        if (spec.codepoint == codepoint) {
            return &spec;
        }
    }

    return nullptr;
}

const terminal_box_arc_spec_t* find_terminal_box_arc_spec(ushort codepoint)
{
    for (const terminal_box_arc_spec_t& spec : k_terminal_box_arc_specs) {
        if (spec.codepoint == codepoint) {
            return &spec;
        }
    }

    return nullptr;
}

bool append_terminal_box_graphic_rects(
    std::vector<Terminal_render_rect>& rects,
    std::vector<Terminal_render_arc>&  arcs,
    const QRectF&                      cell,
    ushort                             codepoint,
    const QColor&                      color,
    terminal_cell_metrics_t            metrics)
{
    if (const terminal_box_stroke_spec_t* spec = find_terminal_box_stroke_spec(codepoint); spec != nullptr) {
        const qreal stroke = spec->heavy
            ? terminal_graphic_heavy_stroke(metrics)
            : terminal_graphic_light_stroke(metrics);

        append_terminal_box_drawing_rects(
            rects,
            cell,
            color,
            terminal_box_connects(spec->connections, k_box_left),
            terminal_box_connects(spec->connections, k_box_right),
            terminal_box_connects(spec->connections, k_box_up),
            terminal_box_connects(spec->connections, k_box_down),
            stroke);
        return true;
    }

    if (const terminal_box_arc_spec_t* spec = find_terminal_box_arc_spec(codepoint); spec != nullptr) {
        append_terminal_graphic_arc(
            arcs,
            spec->kind,
            cell,
            color,
            terminal_graphic_light_stroke(metrics));
        return true;
    }

    return false;
}

void append_terminal_quadrant_rect(
    std::vector<Terminal_render_rect>& rects,
    const QRectF&                      cell,
    const QColor&                      color,
    bool                               upper,
    bool                               left)
{
    const qreal half_width  = cell.width() * 0.5;
    const qreal half_height = cell.height() * 0.5;
    append_terminal_graphic_rect(
        rects,
        QRectF(
            left ? cell.left() : cell.left() + half_width,
            upper ? cell.top() : cell.top() + half_height,
            half_width,
            half_height),
        color);
}

bool append_terminal_block_graphic_rects(
    std::vector<Terminal_render_rect>& rects,
    const QRectF&                      cell,
    ushort                             codepoint,
    const QColor&                      color)
{
    switch (codepoint) {
        case 0x2580: // upper half block
            append_terminal_graphic_rect(
                rects,
                QRectF(cell.left(), cell.top(), cell.width(), cell.height() * 0.5),
                color);
            return true;
        case 0x2581: // lower one eighth block
        case 0x2582: // lower one quarter block
        case 0x2583: // lower three eighths block
        case 0x2584: // lower half block
        case 0x2585: // lower five eighths block
        case 0x2586: // lower three quarters block
        case 0x2587: { // lower seven eighths block
            const qreal eighth_count = static_cast<qreal>(codepoint - 0x2580);
            const qreal height       = cell.height() * eighth_count / 8.0;
            append_terminal_graphic_rect(
                rects,
                QRectF(cell.left(), cell.bottom() - height, cell.width(), height),
                color);
            return true;
        }
        case 0x2588: // full block
            append_terminal_graphic_rect(rects, cell, color);
            return true;
        case 0x2589: // left seven eighths block
        case 0x258a: // left three quarters block
        case 0x258b: // left five eighths block
        case 0x258c: // left half block
        case 0x258d: // left three eighths block
        case 0x258e: // left one quarter block
        case 0x258f: { // left one eighth block
            const qreal eighth_count = static_cast<qreal>(0x2590 - codepoint);
            append_terminal_graphic_rect(
                rects,
                QRectF(cell.left(), cell.top(), cell.width() * eighth_count / 8.0, cell.height()),
                color);
            return true;
        }
        case 0x2590: // right half block
            append_terminal_graphic_rect(
                rects,
                QRectF(cell.center().x(), cell.top(), cell.width() * 0.5, cell.height()),
                color);
            return true;
        case 0x2596: // quadrant lower left
            append_terminal_quadrant_rect(rects, cell, color, false, true);
            return true;
        case 0x2597: // quadrant lower right
            append_terminal_quadrant_rect(rects, cell, color, false, false);
            return true;
        case 0x2598: // quadrant upper left
            append_terminal_quadrant_rect(rects, cell, color, true, true);
            return true;
        case 0x2599: // quadrant upper left, lower left, and lower right
            append_terminal_quadrant_rect(rects, cell, color, true,  true);
            append_terminal_quadrant_rect(rects, cell, color, false, true);
            append_terminal_quadrant_rect(rects, cell, color, false, false);
            return true;
        case 0x259a: // quadrant upper left and lower right
            append_terminal_quadrant_rect(rects, cell, color, true,  true);
            append_terminal_quadrant_rect(rects, cell, color, false, false);
            return true;
        case 0x259b: // quadrant upper left, upper right, and lower left
            append_terminal_quadrant_rect(rects, cell, color, true,  true);
            append_terminal_quadrant_rect(rects, cell, color, true,  false);
            append_terminal_quadrant_rect(rects, cell, color, false, true);
            return true;
        case 0x259c: // quadrant upper left, upper right, and lower right
            append_terminal_quadrant_rect(rects, cell, color, true,  true);
            append_terminal_quadrant_rect(rects, cell, color, true,  false);
            append_terminal_quadrant_rect(rects, cell, color, false, false);
            return true;
        case 0x259d: // quadrant upper right
            append_terminal_quadrant_rect(rects, cell, color, true, false);
            return true;
        case 0x259e: // quadrant upper right and lower left
            append_terminal_quadrant_rect(rects, cell, color, true,  false);
            append_terminal_quadrant_rect(rects, cell, color, false, true);
            return true;
        case 0x259f: // quadrant upper right, lower left, and lower right
            append_terminal_quadrant_rect(rects, cell, color, true,  false);
            append_terminal_quadrant_rect(rects, cell, color, false, true);
            append_terminal_quadrant_rect(rects, cell, color, false, false);
            return true;
        default:
            return false;
    }
}

bool append_terminal_graphic_codepoint_rects(
    std::vector<Terminal_render_rect>& rects,
    std::vector<Terminal_render_arc>&  arcs,
    const QRectF&                      cell,
    ushort                             codepoint,
    const QColor&                      color,
    terminal_cell_metrics_t            metrics)
{
    if (codepoint < 0x2500U || codepoint > 0x259fU) {
        return false;
    }

    if (codepoint >= 0x2580U) {
        return append_terminal_block_graphic_rects(rects, cell, codepoint, color);
    }

    return append_terminal_box_graphic_rects(rects, arcs, cell, codepoint, color, metrics);
}

bool append_terminal_graphic_rects(
    std::vector<Terminal_render_rect>& rects,
    std::vector<Terminal_render_arc>&  arcs,
    const QRectF&                      cell,
    const QString&                     text,
    const QColor&                      color,
    terminal_cell_metrics_t            metrics)
{
    if (text.size() != 1) {
        return false;
    }

    return append_terminal_graphic_codepoint_rects(
        rects,
        arcs,
        cell,
        text.at(0).unicode(),
        color,
        metrics);
}

bool terminal_block_graphic_is_supported(ushort codepoint)
{
    std::vector<Terminal_render_rect> rects;
    return append_terminal_block_graphic_rects(
        rects,
        QRectF(0.0, 0.0, 0.0, 0.0),
        codepoint,
        QColor());
}

bool terminal_hard_block_graphic_is_supported(ushort codepoint)
{
    return
        codepoint >= 0x2580U &&
        codepoint <= 0x259fU &&
        terminal_block_graphic_is_supported(codepoint);
}

bool terminal_hard_block_graphic_text_is_supported(const QString& text)
{
    return
        text.size() == 1 &&
        terminal_hard_block_graphic_is_supported(text.at(0).unicode());
}

bool is_terminal_graphic_text(const QString& text)
{
    if (text.size() != 1) {
        return false;
    }

    const ushort codepoint = text.at(0).unicode();
    if (terminal_hard_block_graphic_is_supported(codepoint)) {
        return true;
    }

    return
        find_terminal_box_stroke_spec(codepoint) != nullptr ||
        find_terminal_box_arc_spec(codepoint)    != nullptr;
}

QColor cursor_graphic_overlay_color(QColor color)
{
    // Match cursor text ordering: a nearly-opaque color keeps the overlay in
    // the blended pass so opaque cursor rectangles cannot batch over it.
    if (color.alpha() == 255) {
        color.setAlpha(254);
    }
    return color;
}

void clip_terminal_graphic_rects(
    std::vector<Terminal_render_rect>& rects,
    std::size_t                        first_index,
    const QRectF&                      clip_rect)
{
    auto write = rects.begin() + static_cast<std::ptrdiff_t>(first_index);
    for (auto read = write; read != rects.end(); ++read) {
        read->rect = read->rect.intersected(clip_rect);
        if (read->rect.width() <= 0.0 || read->rect.height() <= 0.0) {
            continue;
        }

        *write = *read;
        ++write;
    }
    rects.erase(write, rects.end());
}

void delete_node_tree(QSGNode* node)
{
    while (QSGNode* child = node->firstChild()) {
        node->removeChildNode(child);
        delete_node_tree(child);
    }

    delete node;
}

void clear_layer(QSGNode& layer)
{
    VNM_TERMINAL_PROFILE_SCOPE("clear_layer");

    while (QSGNode* child = layer.firstChild()) {
        layer.removeChildNode(child);
        delete_node_tree(child);
    }
}

class Terminal_text_resource_node final : public QSGNode
{
public:
    explicit Terminal_text_resource_node(
        std::shared_ptr<Terminal_renderer_lifecycle_recorder> lifecycle_recorder)
    :
        m_lifecycle_recorder(std::move(lifecycle_recorder))
    {
        if (m_lifecycle_recorder != nullptr) {
            m_lifecycle_recorder->record_text_resource_created();
        }
    }

    ~Terminal_text_resource_node() override
    {
        clear_layer(*this);

        if (m_lifecycle_recorder != nullptr) {
            m_lifecycle_recorder->record_text_resource_destroyed();
        }
    }

private:
    std::shared_ptr<Terminal_renderer_lifecycle_recorder> m_lifecycle_recorder;
};

class Terminal_rect_resource_node final : public QSGTransformNode
{
public:
    explicit Terminal_rect_resource_node(
        std::shared_ptr<Terminal_renderer_lifecycle_recorder> lifecycle_recorder)
    :
        m_lifecycle_recorder(std::move(lifecycle_recorder))
    {
        if (m_lifecycle_recorder != nullptr) {
            m_lifecycle_recorder->record_rect_resource_created();
        }
    }

    ~Terminal_rect_resource_node() override
    {
        clear_layer(*this);

        if (m_lifecycle_recorder != nullptr) {
            m_lifecycle_recorder->record_rect_resource_destroyed();
        }
    }

private:
    std::shared_ptr<Terminal_renderer_lifecycle_recorder> m_lifecycle_recorder;
};

struct row_cache_identity_t
{
    Terminal_buffer_id active_buffer = Terminal_buffer_id::PRIMARY;
    int                logical_row   = 0;
};

bool operator==(
    const row_cache_identity_t& left,
    const row_cache_identity_t& right)
{
    return
        left.active_buffer == right.active_buffer &&
        left.logical_row   == right.logical_row;
}

bool operator<(
    const row_cache_identity_t& left,
    const row_cache_identity_t& right)
{
    if (left.active_buffer != right.active_buffer) {
        return left.active_buffer < right.active_buffer;
    }

    return left.logical_row < right.logical_row;
}

struct row_text_group_t
{
    row_cache_identity_t                         identity;
    int                                          viewport_row = 0;
    qreal                                        row_top      = 0.0;
    std::vector<const Terminal_render_text_run*> runs;
};

struct Ascii_text_coalescing_context
{
    bool                   enabled                            = false;
    QFont                  layout_font;
};

class Ascii_text_coalescing_context_cache
{
public:
    const Ascii_text_coalescing_context& context(const QFont& font, qreal cell_width);

private:
    std::map<QString, Ascii_text_coalescing_context> m_contexts;
};

struct row_rect_group_t
{
    row_cache_identity_t              identity;
    int                               viewport_row = 0;
    qreal                             row_top      = 0.0;
    std::vector<Terminal_render_rect> rects;
};

struct row_arc_group_t
{
    row_cache_identity_t             identity;
    int                              viewport_row = 0;
    qreal                            row_top      = 0.0;
    std::vector<Terminal_render_arc> arcs;
};

struct text_resource_run_t
{
    Terminal_render_text_run   run;
    bool                       use_ascii_layout_font = false;
};

struct Text_resource_rect_descriptor
{
    bool   valid  = false;
    qreal  left   = 0.0;
    qreal  top    = 0.0;
    qreal  width  = 0.0;
    qreal  height = 0.0;

    bool operator==(const Text_resource_rect_descriptor&) const = default;
};

struct Text_resource_row_run_descriptor
{
    int                           column          = 0;
    Text_resource_rect_descriptor rect;
    qreal                         baseline_x      = 0.0;
    qreal                         baseline_y      = 0.0;
    QRgb                          foreground_rgba = 0U;
    QString                       text;

    bool operator==(const Text_resource_row_run_descriptor&) const = default;
};

struct Text_resource_row_descriptor
{
    std::vector<Text_resource_row_run_descriptor> runs;

    bool operator==(const Text_resource_row_descriptor&) const = default;
};

class Terminal_scene_node final : public QSGNode
{
public:
    explicit Terminal_scene_node(
        std::shared_ptr<Terminal_renderer_lifecycle_recorder> lifecycle_recorder)
    :
        m_lifecycle_recorder(std::move(lifecycle_recorder))
    {
        if (m_lifecycle_recorder != nullptr) {
            m_lifecycle_recorder->record_root_node_created();
        }

        background_layer         = make_layer();
        background_frame_layer   = make_layer();
        background_row_layer     = make_layer();
        selection_layer          = make_layer();
        selection_frame_layer    = make_layer();
        selection_row_layer      = make_layer();
        graphic_layer            = make_layer();
        graphic_rect_layer       = make_layer();
        graphic_rect_frame_layer = make_layer();
        graphic_rect_row_layer   = make_layer();
        graphic_arc_layer        = make_layer();
        graphic_arc_frame_layer  = make_layer();
        graphic_arc_row_layer    = make_layer();
        text_layer               = make_layer();
        decoration_layer         = make_layer();
        decoration_frame_layer   = make_layer();
        decoration_row_layer     = make_layer();
        cursor_layer             = make_layer();
        cursor_graphic_layer     = make_blended_layer();
        cursor_text_layer        = make_layer();
        overlay_layer            = make_layer();

        background_layer->appendChildNode(background_frame_layer);
        background_layer->appendChildNode(background_row_layer);
        selection_layer->appendChildNode(selection_frame_layer);
        selection_layer->appendChildNode(selection_row_layer);
        graphic_rect_layer->appendChildNode(graphic_rect_frame_layer);
        graphic_rect_layer->appendChildNode(graphic_rect_row_layer);
        graphic_arc_layer->appendChildNode(graphic_arc_frame_layer);
        graphic_arc_layer->appendChildNode(graphic_arc_row_layer);
        graphic_layer->appendChildNode(graphic_rect_layer);
        graphic_layer->appendChildNode(graphic_arc_layer);
        decoration_layer->appendChildNode(decoration_frame_layer);
        decoration_layer->appendChildNode(decoration_row_layer);

        appendChildNode(background_layer);
        appendChildNode(selection_layer);
        appendChildNode(graphic_layer);
        appendChildNode(text_layer);
        appendChildNode(decoration_layer);
        appendChildNode(cursor_layer);
        appendChildNode(cursor_graphic_layer);
        appendChildNode(cursor_text_layer);
        appendChildNode(overlay_layer);
    }

    ~Terminal_scene_node() override
    {
        clear_layer(*background_layer);
        clear_layer(*selection_layer);
        clear_layer(*graphic_layer);
        clear_layer(*text_layer);
        clear_layer(*decoration_layer);
        clear_layer(*cursor_layer);
        clear_layer(*cursor_graphic_layer);
        clear_layer(*cursor_text_layer);
        clear_layer(*overlay_layer);

        if (m_lifecycle_recorder != nullptr) {
            m_lifecycle_recorder->record_root_node_destroyed();
        }
    }

    QSGNode*                               background_layer         = nullptr;
    QSGNode*                               background_frame_layer   = nullptr;
    QSGNode*                               background_row_layer     = nullptr;
    QSGNode*                               selection_layer          = nullptr;
    QSGNode*                               selection_frame_layer    = nullptr;
    QSGNode*                               selection_row_layer      = nullptr;
    QSGNode*                               graphic_layer            = nullptr;
    QSGNode*                               graphic_rect_layer       = nullptr;
    QSGNode*                               graphic_rect_frame_layer = nullptr;
    QSGNode*                               graphic_rect_row_layer   = nullptr;
    QSGNode*                               graphic_arc_layer        = nullptr;
    QSGNode*                               graphic_arc_frame_layer  = nullptr;
    QSGNode*                               graphic_arc_row_layer    = nullptr;
    QSGNode*                               text_layer               = nullptr;
    QSGNode*                               decoration_layer         = nullptr;
    QSGNode*                               decoration_frame_layer   = nullptr;
    QSGNode*                               decoration_row_layer     = nullptr;
    QSGNode*                               cursor_layer             = nullptr;
    QSGNode*                               cursor_graphic_layer     = nullptr;
    QSGNode*                               cursor_text_layer        = nullptr;
    QSGNode*                               overlay_layer            = nullptr;

    QByteArray                             background_layer_input_key;
    QByteArray                             background_frame_layer_key;
    QByteArray                             selection_layer_input_key;
    QByteArray                             selection_frame_layer_key;
    QByteArray                             graphic_rect_layer_input_key;
    QByteArray                             graphic_rect_frame_layer_key;
    QByteArray                             graphic_arc_layer_input_key;
    QByteArray                             graphic_arc_frame_layer_key;
    QByteArray                             decoration_layer_input_key;
    QByteArray                             decoration_frame_layer_key;
    QByteArray                             cursor_layer_key;
    QByteArray                             cursor_graphic_layer_key;
    QByteArray                             cursor_text_layer_key;
    QByteArray                             overlay_layer_key;

    struct Geometry_row_cache_slot
    {
        row_cache_identity_t          identity;
        Terminal_rect_resource_node*  wrapper = nullptr;
        QByteArray                    key;
        qreal                         row_top = 0.0;
    };

    struct Text_resource_cache_slot
    {
        row_cache_identity_t         identity;
        QSGTransformNode*            wrapper = nullptr;
        QSGClipNode*                 clip    = nullptr;
        Terminal_text_resource_node* node    = nullptr;
        QRectF                       clip_rect;
        QByteArray                   key;
        std::optional<Text_resource_row_descriptor>
                                     text_resource_descriptor;
        qreal                        row_top = 0.0;
    };

    std::vector<Geometry_row_cache_slot>   background_row_slots;
    std::vector<Geometry_row_cache_slot>   selection_row_slots;
    std::vector<Geometry_row_cache_slot>   graphic_rect_row_slots;
    std::vector<Geometry_row_cache_slot>   graphic_arc_row_slots;
    std::vector<Geometry_row_cache_slot>   decoration_row_slots;
    std::vector<Text_resource_cache_slot>  text_row_slots;
    QByteArray                             text_frame_key;
    Ascii_text_coalescing_context_cache    text_coalescing_context_cache;

private:
    static QSGNode* make_layer()
    {
        QSGNode* layer = new QSGNode();
        layer->setFlag(QSGNode::OwnedByParent, true);
        return layer;
    }

    static QSGNode* make_blended_layer()
    {
        auto* layer = new QSGOpacityNode();
        layer->setFlag(QSGNode::OwnedByParent, true);
        layer->setOpacity(254.0 / 255.0);
        return layer;
    }

    std::shared_ptr<Terminal_renderer_lifecycle_recorder>
                                           m_lifecycle_recorder;
};

void destroy_text_cache_entry(Terminal_scene_node::Text_resource_cache_slot& cache)
{
    VNM_TERMINAL_PROFILE_SCOPE("destroy_text_cache_entry");

    if (cache.wrapper->parent() != nullptr) {
        cache.wrapper->parent()->removeChildNode(cache.wrapper);
    }
    delete_node_tree(cache.wrapper);
}

int node_subtree_child_count(const QSGNode& node)
{
    int count = 0;
    for (const QSGNode* child = node.firstChild(); child != nullptr; child = child->nextSibling()) {
        ++count;
        count += node_subtree_child_count(*child);
    }
    return count;
}

#if VNM_TERMINAL_PROFILING_ENABLED
void record_text_cache_entry_nodes_cleared(
    terminal_renderer_stats_t& stats,
    const Terminal_scene_node::Text_resource_cache_slot&
                               cache,
    bool                       replacement)
{
    VNM_TERMINAL_PROFILE_SCOPE("record_text_cache_entry_nodes_cleared");

    const int nodes_cleared = node_subtree_child_count(*cache.node);
    if (replacement) {
        stats.text_cache_entry_child_nodes_cleared_for_replacement += nodes_cleared;
    }
    else {
        stats.text_cache_entry_child_nodes_cleared_for_removal += nodes_cleared;
    }
    stats.text_cache_entry_max_child_nodes_cleared = std::max(
        stats.text_cache_entry_max_child_nodes_cleared,
        nodes_cleared);
}
#else
void record_text_cache_entry_nodes_cleared(
    terminal_renderer_stats_t&,
    const Terminal_scene_node::Text_resource_cache_slot&,
    bool)
{}
#endif

struct arc_geometry_t
{
    QPointF    center;
    qreal      start_angle = 0.0;
    qreal      end_angle   = 0.0;
};

arc_geometry_t arc_geometry(const Terminal_render_arc& arc)
{
    const QRectF& cell = arc.rect;
    switch (arc.kind) {
        case Terminal_render_arc_kind::DOWN_RIGHT: return {{cell.right(), cell.bottom()}, k_pi, k_pi * 1.5};
        case Terminal_render_arc_kind::DOWN_LEFT:  return {{cell.left(), cell.bottom()}, k_pi * 1.5, k_pi * 2.0};
        case Terminal_render_arc_kind::UP_LEFT:    return {{cell.left(), cell.top()}, 0.0, k_pi * 0.5};
        case Terminal_render_arc_kind::UP_RIGHT:   return {{cell.right(), cell.top()}, k_pi * 0.5, k_pi};
    }

    return {{cell.right(), cell.bottom()}, k_pi, k_pi * 1.5};
}

QPointF arc_point_at(
    const Terminal_render_arc& arc,
    const arc_geometry_t&      arc_spec,
    qreal                      angle)
{
    const qreal radius_x = arc.rect.width() * 0.5;
    const qreal radius_y = arc.rect.height() * 0.5;
    return QPointF(
        arc_spec.center.x() + std::cos(angle) * radius_x,
        arc_spec.center.y() + std::sin(angle) * radius_y);
}

QPointF arc_vector_point_at(
    const Terminal_render_arc& arc,
    const arc_geometry_t&      arc_spec,
    qreal                      angle,
    qreal                      radius_offset)
{
    const qreal radius_x = std::max<qreal>(0.0, arc.rect.width() * 0.5 + radius_offset);
    const qreal radius_y = std::max<qreal>(0.0, arc.rect.height() * 0.5 + radius_offset);
    return QPointF(
        arc_spec.center.x() + std::cos(angle) * radius_x,
        arc_spec.center.y() + std::sin(angle) * radius_y);
}

bool angle_is_inside_arc(
    qreal                      angle,
    const arc_geometry_t&      arc_spec)
{
    return angle >= arc_spec.start_angle && angle <= arc_spec.end_angle;
}

qreal arc_pixel_coverage(
    const Terminal_render_arc& arc,
    const arc_geometry_t&      arc_spec,
    QPointF                    point)
{
    const qreal radius_x = arc.rect.width() * 0.5;
    const qreal radius_y = arc.rect.height() * 0.5;
    if (radius_x <= 0.0 || radius_y <= 0.0) {
        return 0.0;
    }

    qreal angle = std::atan2(
        (point.y() - arc_spec.center.y()) / radius_y,
        (point.x() - arc_spec.center.x()) / radius_x);
    if (angle < 0.0)                           { angle += k_pi * 2.0; }
    if (!angle_is_inside_arc(angle, arc_spec)) { return 0.0;          }

    const QPointF curve_point = arc_point_at(arc, arc_spec, angle);
    const qreal distance = std::hypot(
        point.x() - curve_point.x(),
        point.y() - curve_point.y());
    return std::clamp(
        arc.stroke * 0.5 + k_graphic_antialias_feather - distance,
        0.0,
        1.0);
}

QColor coverage_color(
    QColor color,
    qreal  coverage)
{
    color.setAlpha(std::clamp(
        static_cast<int>(std::round(static_cast<qreal>(color.alpha()) * coverage)),
        0,
        255));
    return color;
}

void set_premultiplied_vertex(
    QSGGeometry::ColoredPoint2D&   vertex,
    QPointF                        point,
    QColor                         color,
    qreal                          alpha_factor)
{
    const qreal alpha_ratio = std::clamp(
        (static_cast<qreal>(color.alpha()) / 255.0) * alpha_factor,
        0.0,
        1.0);
    vertex.set(
        static_cast<float>(point.x()),
        static_cast<float>(point.y()),
        static_cast<uchar>(std::round(static_cast<qreal>(color.red()) * alpha_ratio)),
        static_cast<uchar>(std::round(static_cast<qreal>(color.green()) * alpha_ratio)),
        static_cast<uchar>(std::round(static_cast<qreal>(color.blue()) * alpha_ratio)),
        static_cast<uchar>(std::round(alpha_ratio * 255.0)));
}

int flat_rect_vertex_count(std::size_t rect_count)
{
    return static_cast<int>(rect_count) * k_flat_rect_vertices_per_rect;
}

bool flat_rect_batch_uses_blending(const std::vector<Terminal_render_rect>& rects)
{
    return std::any_of(
        rects.begin(),
        rects.end(),
        [](const Terminal_render_rect& rect) {
            return rect.color.alpha() != 255;
        });
}

void append_flat_rect_triangles(
    QSGGeometry::ColoredPoint2D*               vertices,
    int&                                       vertex_index,
    const Terminal_render_rect&                rect)
{
    const QPointF top_left(rect.rect.left(), rect.rect.top());
    const QPointF top_right(rect.rect.right(), rect.rect.top());
    const QPointF bottom_left(rect.rect.left(), rect.rect.bottom());
    const QPointF bottom_right(rect.rect.right(), rect.rect.bottom());

    set_premultiplied_vertex(vertices[vertex_index++], top_left,     rect.color, 1.0);
    set_premultiplied_vertex(vertices[vertex_index++], top_right,    rect.color, 1.0);
    set_premultiplied_vertex(vertices[vertex_index++], bottom_left,  rect.color, 1.0);
    set_premultiplied_vertex(vertices[vertex_index++], bottom_left,  rect.color, 1.0);
    set_premultiplied_vertex(vertices[vertex_index++], top_right,    rect.color, 1.0);
    set_premultiplied_vertex(vertices[vertex_index++], bottom_right, rect.color, 1.0);
}

void write_flat_rect_batch_vertices(
    QSGGeometry&                               geometry,
    const std::vector<Terminal_render_rect>&   rects)
{
    QSGGeometry::ColoredPoint2D* vertices = geometry.vertexDataAsColoredPoint2D();
    int vertex_index = 0;
    for (const Terminal_render_rect& rect : rects) {
        append_flat_rect_triangles(vertices, vertex_index, rect);
    }
}

bool set_vertex_color_material_blending(
    QSGVertexColorMaterial&    material,
    bool                       blending)
{
    const bool old_blending =
        (material.flags() & QSGMaterial::Blending) == QSGMaterial::Blending;
    if (old_blending == blending) {
        return false;
    }

    material.setFlag(QSGMaterial::Blending, blending);
    return true;
}

QSGGeometryNode* make_flat_rect_batch_node(
    const std::vector<Terminal_render_rect>& rects)
{
    auto* node = new QSGGeometryNode();
    auto* geometry = new QSGGeometry(
        QSGGeometry::defaultAttributes_ColoredPoint2D(),
        flat_rect_vertex_count(rects.size()));
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
    write_flat_rect_batch_vertices(*geometry, rects);

    auto* material = new QSGVertexColorMaterial();
    material->setFlag(QSGMaterial::Blending, flat_rect_batch_uses_blending(rects));
    node->setGeometry(geometry);
    node->setFlag(QSGGeometryNode::OwnsGeometry);
    node->setMaterial(material);
    node->setFlag(QSGGeometryNode::OwnsMaterial);
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    return node;
}

void update_flat_rect_batch_node(
    QSGGeometryNode&                           node,
    const std::vector<Terminal_render_rect>&   rects)
{
    QSGGeometry* geometry = node.geometry();
    if (geometry->vertexCount() != flat_rect_vertex_count(rects.size())) {
        geometry->allocate(flat_rect_vertex_count(rects.size()));
    }
    write_flat_rect_batch_vertices(*geometry, rects);
    geometry->markVertexDataDirty();
    node.markDirty(QSGNode::DirtyGeometry);

    auto* material = static_cast<QSGVertexColorMaterial*>(node.material());
    if (set_vertex_color_material_blending(
            *material, flat_rect_batch_uses_blending(rects)))
    {
        node.markDirty(QSGNode::DirtyMaterial);
    }
}

void append_arc_band_triangle(
    QSGGeometry::ColoredPoint2D*               vertices,
    int&                                       vertex_index,
    QPointF                                    left_0,
    QPointF                                    right_0,
    QPointF                                    left_1,
    QPointF                                    right_1,
    QColor                                     color,
    qreal                                      alpha_0,
    qreal                                      alpha_1)
{
    set_premultiplied_vertex(vertices[vertex_index++], left_0,  color, alpha_0);
    set_premultiplied_vertex(vertices[vertex_index++], right_0, color, alpha_1);
    set_premultiplied_vertex(vertices[vertex_index++], left_1,  color, alpha_0);
    set_premultiplied_vertex(vertices[vertex_index++], left_1,  color, alpha_0);
    set_premultiplied_vertex(vertices[vertex_index++], right_0, color, alpha_1);
    set_premultiplied_vertex(vertices[vertex_index++], right_1, color, alpha_1);
}

QSGGeometryNode* make_vector_arc_node(const Terminal_render_arc& arc)
{
    const int ring_count   = 4;
    const int band_count   = ring_count - 1;
    const int vertex_count = k_arc_geometry_segment_count * band_count * 6;

    auto* node = new QSGGeometryNode();
    auto* geometry = new QSGGeometry(
        QSGGeometry::defaultAttributes_ColoredPoint2D(),
        vertex_count);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::StaticPattern);

    const arc_geometry_t arc_spec = arc_geometry(arc);
    const qreal radius_offsets[ring_count] = {
        -arc.stroke * 0.5 - k_graphic_antialias_feather,
        -arc.stroke * 0.5,
        arc.stroke * 0.5,
        arc.stroke * 0.5 + k_graphic_antialias_feather,
    };
    const qreal alpha_factors[ring_count] = {0.0, 1.0, 1.0, 0.0};

    QSGGeometry::ColoredPoint2D* vertices = geometry->vertexDataAsColoredPoint2D();
    int vertex_index = 0;
    for (int segment = 0; segment < k_arc_geometry_segment_count; ++segment) {
        const qreal t0 = static_cast<qreal>(segment) /
            static_cast<qreal>(k_arc_geometry_segment_count);
        const qreal t1 = static_cast<qreal>(segment + 1) /
            static_cast<qreal>(k_arc_geometry_segment_count);
        const qreal angle_0 =
            arc_spec.start_angle + (arc_spec.end_angle - arc_spec.start_angle) * t0;
        const qreal angle_1 =
            arc_spec.start_angle + (arc_spec.end_angle - arc_spec.start_angle) * t1;

        QPointF band_points_0[ring_count];
        QPointF band_points_1[ring_count];
        for (int ring = 0; ring < ring_count; ++ring) {
            band_points_0[ring] = arc_vector_point_at(
                arc,
                arc_spec,
                angle_0,
                radius_offsets[ring]);
            band_points_1[ring] = arc_vector_point_at(
                arc,
                arc_spec,
                angle_1,
                radius_offsets[ring]);
        }

        for (int band = 0; band < band_count; ++band) {
            append_arc_band_triangle(
                vertices,
                vertex_index,
                band_points_0[band],
                band_points_0[band + 1],
                band_points_1[band],
                band_points_1[band + 1],
                arc.color,
                alpha_factors[band],
                alpha_factors[band + 1]);
        }
    }

    auto* material = new QSGVertexColorMaterial();
    material->setFlag(QSGMaterial::Blending);
    node->setGeometry(geometry);
    node->setFlag(QSGGeometryNode::OwnsGeometry);
    node->setMaterial(material);
    node->setFlag(QSGGeometryNode::OwnsMaterial);
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    return node;
}

void append_rect_band_triangle(
    QSGGeometry::ColoredPoint2D*   vertices,
    int&                           vertex_index,
    QRectF                         rect,
    QColor                         color,
    qreal                          alpha_0,
    qreal                          alpha_1,
    bool                           alpha_changes_vertically)
{
    const QPointF top_left(rect.left(), rect.top());
    const QPointF top_right(rect.right(), rect.top());
    const QPointF bottom_left(rect.left(), rect.bottom());
    const QPointF bottom_right(rect.right(), rect.bottom());
    const qreal top_left_alpha     = alpha_0;
    const qreal top_right_alpha    = alpha_changes_vertically ? alpha_0 : alpha_1;
    const qreal bottom_left_alpha  = alpha_changes_vertically ? alpha_1 : alpha_0;
    const qreal bottom_right_alpha = alpha_1;

    set_premultiplied_vertex(vertices[vertex_index++], top_left,     color, top_left_alpha);
    set_premultiplied_vertex(vertices[vertex_index++], top_right,    color, top_right_alpha);
    set_premultiplied_vertex(vertices[vertex_index++], bottom_left,  color, bottom_left_alpha);
    set_premultiplied_vertex(vertices[vertex_index++], bottom_left,  color, bottom_left_alpha);
    set_premultiplied_vertex(vertices[vertex_index++], top_right,    color, top_right_alpha);
    set_premultiplied_vertex(vertices[vertex_index++], bottom_right, color, bottom_right_alpha);
}

QSGGeometryNode* make_vector_rect_node(const Terminal_render_rect& rect)
{
    constexpr int band_count   = 3;
    constexpr int vertex_count = band_count * 6;

    auto* node = new QSGGeometryNode();
    auto* geometry = new QSGGeometry(
        QSGGeometry::defaultAttributes_ColoredPoint2D(),
        vertex_count);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::StaticPattern);

    const bool horizontal = rect.rect.width() >= rect.rect.height();
    QSGGeometry::ColoredPoint2D* vertices = geometry->vertexDataAsColoredPoint2D();

    int vertex_index = 0;

    if (horizontal) {
        append_rect_band_triangle(
            vertices,
            vertex_index,
            QRectF(
                rect.rect.left(),
                rect.rect.top() - k_graphic_antialias_feather,
                rect.rect.width(),
                k_graphic_antialias_feather),
            rect.color,
            0.0,
            1.0,
            true);
        append_rect_band_triangle(
            vertices,
            vertex_index,
            rect.rect,
            rect.color,
            1.0,
            1.0,
            true);
        append_rect_band_triangle(
            vertices,
            vertex_index,
            QRectF(
                rect.rect.left(),
                rect.rect.bottom(),
                rect.rect.width(),
                k_graphic_antialias_feather),
            rect.color,
            1.0,
            0.0,
            true);
    }
    else {
        append_rect_band_triangle(
            vertices,
            vertex_index,
            QRectF(
                rect.rect.left() - k_graphic_antialias_feather,
                rect.rect.top(),
                k_graphic_antialias_feather,
                rect.rect.height()),
            rect.color,
            0.0,
            1.0,
            false);
        append_rect_band_triangle(
            vertices,
            vertex_index,
            rect.rect,
            rect.color,
            1.0,
            1.0,
            false);
        append_rect_band_triangle(
            vertices,
            vertex_index,
            QRectF(
                rect.rect.right(),
                rect.rect.top(),
                k_graphic_antialias_feather,
                rect.rect.height()),
            rect.color,
            1.0,
            0.0,
            false);
    }

    auto* material = new QSGVertexColorMaterial();
    node->setGeometry(geometry);
    node->setFlag(QSGGeometryNode::OwnsGeometry);
    node->setMaterial(material);
    node->setFlag(QSGGeometryNode::OwnsMaterial);
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    return node;
}

qreal logical_pixel_size(qreal device_pixel_ratio)
{
    return 1.0 / std::max<qreal>(1.0, device_pixel_ratio);
}

void append_software_span_node(
    QSGNode*   parent,
    int        y_pixel,
    int        first_x_pixel,
    int        past_last_x_pixel,
    qreal      pixel,
    QColor     color)
{
    if (past_last_x_pixel <= first_x_pixel || color.alpha() == 0) {
        return;
    }

    parent->appendChildNode(new QSGSimpleRectNode(
        QRectF(
            static_cast<qreal>(first_x_pixel) * pixel,
            static_cast<qreal>(y_pixel) * pixel,
            static_cast<qreal>(past_last_x_pixel - first_x_pixel) * pixel,
            pixel),
        color));
}

qreal antialiased_rect_pixel_coverage(
    const Terminal_render_rect&    rect,
    QPointF                        point)
{
    const QRectF& shape = rect.rect;
    if (shape.width() >= shape.height()) {
        if (point.x() < shape.left() || point.x() > shape.right()) {
            return 0.0;
        }

        const qreal distance = std::abs(point.y() - shape.center().y());
        return std::clamp(
            shape.height() * 0.5 + k_graphic_antialias_feather - distance,
            0.0,
            1.0);
    }

    if (point.y() < shape.top() || point.y() > shape.bottom()) {
        return 0.0;
    }

    const qreal distance = std::abs(point.x() - shape.center().x());
    return std::clamp(
        shape.width() * 0.5 + k_graphic_antialias_feather - distance,
        0.0,
        1.0);
}

template <typename CoverageFn>
void append_coverage_rasterized_shape(
    QSGNode*       parent,
    const QRectF&  bounds,
    const QColor&  shape_color,
    qreal          device_pixel_ratio,
    CoverageFn     coverage_at)
{
    const qreal pixel  = logical_pixel_size(device_pixel_ratio);
    const int   left   = static_cast<int>(std::floor(bounds.left()   / pixel));
    const int   top    = static_cast<int>(std::floor(bounds.top()    / pixel));
    const int   right  = static_cast<int>(std::ceil (bounds.right()  / pixel));
    const int   bottom = static_cast<int>(std::ceil (bounds.bottom() / pixel));

    for (int y = top; y < bottom; ++y) {
        bool has_span   = false;
        int  span_start = left;
        QColor span_color;
        for (int x = left; x < right; ++x) {
            const qreal coverage = coverage_at(
                QPointF((static_cast<qreal>(x) + 0.5) * pixel, (static_cast<qreal>(y) + 0.5) * pixel));
            const QColor color = coverage > 0.0
                ? coverage_color(shape_color, coverage)
                : QColor();
            if (has_span && color.rgba() == span_color.rgba()) {
                continue;
            }

            if (has_span) {
                append_software_span_node(parent, y, span_start, x, pixel, span_color);
            }
            has_span   = color.alpha() != 0;
            span_start = x;
            span_color = color;
        }

        if (has_span) {
            append_software_span_node(parent, y, span_start, right, pixel, span_color);
        }
    }
}

void append_antialiased_rect_nodes(
    QSGNode*                       parent,
    const Terminal_render_rect&    rect,
    qreal                          device_pixel_ratio)
{
    const QRectF bounds = rect.rect.adjusted(
        -k_graphic_antialias_feather,
        -k_graphic_antialias_feather,
        k_graphic_antialias_feather,
        k_graphic_antialias_feather);
    append_coverage_rasterized_shape(
        parent,
        bounds,
        rect.color,
        device_pixel_ratio,
        [&](QPointF center) { return antialiased_rect_pixel_coverage(rect, center); });
}

void append_arc_rect_nodes(
    QSGNode*                       parent,
    const Terminal_render_arc&     arc,
    qreal                          device_pixel_ratio)
{
    const arc_geometry_t arc_spec = arc_geometry(arc);
    append_coverage_rasterized_shape(
        parent,
        arc.rect,
        arc.color,
        device_pixel_ratio,
        [&](QPointF center) { return arc_pixel_coverage(arc, arc_spec, center); });
}

void append_rect_nodes(
    QSGNode*                                   parent,
    const std::vector<Terminal_render_rect>&   rects,
    bool                                       use_software_graphic_fallback,
    qreal                                      device_pixel_ratio)
{
    for (const Terminal_render_rect& rect : rects) {
        if (rect.antialias && !use_software_graphic_fallback) {
            parent->appendChildNode(make_vector_rect_node(rect));
        }
        else
        if (rect.antialias) {
            append_antialiased_rect_nodes(parent, rect, device_pixel_ratio);
        }
        else {
            parent->appendChildNode(new QSGSimpleRectNode(rect.rect, rect.color));
        }
    }
}

void append_arc_nodes(
    QSGNode*                                   parent,
    const std::vector<Terminal_render_arc>&    arcs,
    bool                                       use_software_graphic_fallback,
    qreal                                      device_pixel_ratio)
{
    for (const Terminal_render_arc& arc : arcs) {
        if (use_software_graphic_fallback) {
            append_arc_rect_nodes(parent, arc, device_pixel_ratio);
        }
        else {
            parent->appendChildNode(make_vector_arc_node(arc));
        }
    }
}

void append_graphic_nodes(
    QSGNode*                                   parent,
    const std::vector<Terminal_render_rect>&   rects,
    const std::vector<Terminal_render_arc>&    arcs,
    bool                                       use_software_graphic_fallback,
    qreal                                      device_pixel_ratio)
{
    append_rect_nodes(parent, rects, use_software_graphic_fallback, device_pixel_ratio);
    append_arc_nodes(parent, arcs, use_software_graphic_fallback, device_pixel_ratio);
}

template <typename T>
void append_cache_key_value(QByteArray& key, const T& value)
{
    static_assert(
        std::is_arithmetic_v<T> || std::is_enum_v<T>,
        "append_cache_key_value accepts only scalar cache key fields");
    key.append(
        reinterpret_cast<const char*>(&value),
        static_cast<qsizetype>(sizeof(value)));
}

void append_cache_key_bool(QByteArray& key, bool value)
{
    const char encoded = value ? 1 : 0;
    key.append(&encoded, 1);
}

void append_cache_key_string(QByteArray& key, const QString& field)
{
    const std::uint64_t length = static_cast<std::uint64_t>(field.size());
    append_cache_key_value(key, length);
    if (!field.isEmpty()) {
        key.append(
            reinterpret_cast<const char*>(field.constData()),
            field.size() * static_cast<qsizetype>(sizeof(QChar)));
    }
}

void append_cache_key_size(QByteArray& key, std::size_t value)
{
    append_cache_key_value(key, static_cast<std::uint64_t>(value));
}

void append_rect_layer_geometry_key_fields(QByteArray& key, const QRectF& rect)
{
    append_cache_key_value(key, rect.x());
    append_cache_key_value(key, rect.y());
    append_cache_key_value(key, rect.width());
    append_cache_key_value(key, rect.height());
}

bool has_antialiased_rects(const std::vector<Terminal_render_rect>& rects)
{
    return std::any_of(
        rects.begin(),
        rects.end(),
        [](const Terminal_render_rect& rect) { return rect.antialias; });
}

bool layer_uses_software_rasterization(
    const std::vector<Terminal_render_rect>&   rects,
    const std::vector<Terminal_render_arc>&    arcs,
    bool                                       use_software_graphic_fallback)
{
    return use_software_graphic_fallback &&
        (!arcs.empty() || has_antialiased_rects(rects));
}

enum class Flat_rect_render_route
{
    SOFTWARE_FALLBACK,
    BATCHED_GEOMETRY,
};

Flat_rect_render_route flat_rect_render_route(
    bool use_software_graphic_fallback)
{
    return use_software_graphic_fallback
        ? Flat_rect_render_route::SOFTWARE_FALLBACK
        : Flat_rect_render_route::BATCHED_GEOMETRY;
}

QByteArray rect_layer_key(
    const std::vector<Terminal_render_rect>&   rects,
    const std::vector<Terminal_render_arc>&    arcs,
    bool                                       use_software_graphic_fallback,
    qreal                                      device_pixel_ratio)
{
    QByteArray key;
    append_cache_key_size(key, rects.size());
    for (const Terminal_render_rect& rect : rects) {
        append_rect_layer_geometry_key_fields(key, rect.rect);
        append_cache_key_value(key, rect.color.rgba());
        append_cache_key_bool(key, rect.antialias);
    }
    append_cache_key_size(key, arcs.size());
    for (const Terminal_render_arc& arc : arcs) {
        append_cache_key_value(key, static_cast<int>(arc.kind));
        append_rect_layer_geometry_key_fields(key, arc.rect);
        append_cache_key_value(key, arc.stroke);
        append_cache_key_value(key, arc.color.rgba());
    }
    const bool include_software_rasterization_key = layer_uses_software_rasterization(
        rects,
        arcs,
        use_software_graphic_fallback);
    append_cache_key_bool(key, include_software_rasterization_key);
    if (include_software_rasterization_key) {
        append_cache_key_value(key, device_pixel_ratio);
    }
    return key;
}

QByteArray flat_rect_layer_key(
    const std::vector<Terminal_render_rect>&   rects,
    bool                                       use_software_graphic_fallback,
    qreal                                      device_pixel_ratio)
{
    QByteArray key = rect_layer_key(
        rects,
        {},
        use_software_graphic_fallback,
        device_pixel_ratio);
    append_cache_key_value(
        key,
        flat_rect_render_route(use_software_graphic_fallback));
    return key;
}

QByteArray batched_rect_layer_key(
    const std::vector<Terminal_render_rect>&   batched_rects,
    const std::vector<Terminal_render_rect>&   frame_rects,
    bool                                       use_software_graphic_fallback,
    qreal                                      device_pixel_ratio)
{
    QByteArray key = flat_rect_layer_key(
        batched_rects,
        use_software_graphic_fallback,
        device_pixel_ratio);
    const QByteArray frame_rect_key = rect_layer_key(
        frame_rects,
        {},
        use_software_graphic_fallback,
        device_pixel_ratio);
    append_cache_key_size(key, static_cast<std::size_t>(frame_rect_key.size()));
    key.append(frame_rect_key);
    return key;
}

std::vector<Terminal_render_rect> rects_for_frame_group(
    const std::vector<Terminal_render_rect>&   first,
    const std::vector<Terminal_render_rect>&   second)
{
    if (first.empty())  { return second; }
    if (second.empty()) { return first;  }

    std::vector<Terminal_render_rect> rects;
    rects.reserve(first.size() + second.size());
    rects.insert(rects.end(), first.begin(),  first.end());
    rects.insert(rects.end(), second.begin(), second.end());
    return rects;
}

const std::vector<Terminal_render_rect>& empty_terminal_render_rects()
{
    static const std::vector<Terminal_render_rect> rects;
    return rects;
}

std::vector<Terminal_render_rect> packed_hard_graphic_rects(
    const Terminal_render_frame& frame)
{
    VNM_TERMINAL_PROFILE_SCOPE("packed_hard_graphic_rects");

    std::vector<Terminal_render_rect> rects;
    rects.reserve(frame.packed_graphic_codepoints.size());

    for (const terminal_packed_render_row_t& row : frame.packed_rows) {
        const std::size_t first_span =
            static_cast<std::size_t>(row.first_graphic_span);
        if (first_span >= frame.packed_graphic_spans.size()) {
            continue;
        }

        const std::size_t span_count = std::min<std::size_t>(
            row.graphic_span_count,
            frame.packed_graphic_spans.size() - first_span);
        for (std::size_t span_offset = 0U; span_offset < span_count; ++span_offset) {
            const terminal_packed_graphic_span_t& span =
                frame.packed_graphic_spans[first_span + span_offset];
            const std::size_t first_codepoint =
                static_cast<std::size_t>(span.codepoint_offset);
            if (first_codepoint >= frame.packed_graphic_codepoints.size()) {
                continue;
            }

            const std::size_t codepoint_count = std::min<std::size_t>(
                std::min<std::size_t>(span.codepoint_count, span.column_count),
                frame.packed_graphic_codepoints.size() - first_codepoint);
            const QColor foreground = qcolor_from_rgba(span.foreground_rgba);
            for (std::size_t codepoint_offset = 0U;
                codepoint_offset < codepoint_count;
                ++codepoint_offset)
            {
                const std::uint32_t codepoint =
                    frame.packed_graphic_codepoints[first_codepoint + codepoint_offset];
                if (codepoint > 0xffffU ||
                    !terminal_hard_block_graphic_is_supported(
                        static_cast<ushort>(codepoint)))
                {
                    continue;
                }

                append_terminal_block_graphic_rects(
                    rects,
                    cell_rect(
                        row.viewport_row,
                        span.first_column + static_cast<int>(codepoint_offset),
                        1,
                        frame.cell_metrics),
                    static_cast<ushort>(codepoint),
                    foreground);
            }
        }
    }

    return rects;
}

struct graphic_rect_layer_inputs_t
{
    std::vector<Terminal_render_rect>  batched_rects;
    std::vector<Terminal_render_rect>  frame_rects;
    bool                               use_simple_rect_nodes_for_row_rects = false;
};

graphic_rect_layer_inputs_t graphic_rect_layer_inputs(
    const Terminal_render_frame& frame)
{
    graphic_rect_layer_inputs_t inputs;
    std::vector<Terminal_render_rect> packed_rects =
        packed_hard_graphic_rects(frame);
    const bool packed_frame_input = !frame.packed_rows.empty();

    inputs.batched_rects.reserve(frame.graphic_rects.size() + packed_rects.size());
    inputs.frame_rects.reserve(frame.graphic_rects.size());
    if (packed_frame_input) {
        inputs.batched_rects = std::move(packed_rects);
        inputs.frame_rects   = frame.graphic_rects;
        return inputs;
    }

    inputs.use_simple_rect_nodes_for_row_rects = true;
    for (const Terminal_render_rect& direct_rect : frame.graphic_rects) {
        if (direct_rect.antialias) {
            inputs.frame_rects.push_back(direct_rect);
            continue;
        }

        inputs.batched_rects.push_back(direct_rect);
    }

    return inputs;
}

void record_rect_cache_key_build(
    terminal_renderer_stats_t& stats,
    const QByteArray&          key)
{
    ++stats.rect_key_builds;
    stats.rect_key_bytes += static_cast<std::uint64_t>(key.size());
    ++stats.cache_key_builds;
    stats.cache_key_bytes += static_cast<std::uint64_t>(key.size());
}

void record_text_cache_key_build(
    terminal_renderer_stats_t& stats,
    const QByteArray&          key)
{
    ++stats.text_key_builds;
    stats.text_key_bytes += static_cast<std::uint64_t>(key.size());
    ++stats.cache_key_builds;
    stats.cache_key_bytes += static_cast<std::uint64_t>(key.size());
}

bool sync_rect_layer(
    QSGNode&                                   layer,
    QByteArray&                                cached_key,
    const std::vector<Terminal_render_rect>&   rects,
    const std::vector<Terminal_render_arc>&    arcs = {},
    bool                                       use_software_graphic_fallback = false,
    qreal                                      device_pixel_ratio = 1.0,
    terminal_renderer_stats_t*                 stats = nullptr)
{
    const QByteArray key = rect_layer_key(
        rects,
        arcs,
        use_software_graphic_fallback,
        device_pixel_ratio);
    if (stats != nullptr)  { record_rect_cache_key_build(*stats, key); }
    if (cached_key == key) { return false;                             }

    if (stats != nullptr) {
        stats->qsg_nodes_destroyed += node_subtree_child_count(layer);
    }
    clear_layer(layer);
    append_graphic_nodes(
        &layer,
        rects,
        arcs,
        use_software_graphic_fallback,
        device_pixel_ratio);
    if (stats != nullptr) {
        stats->qsg_nodes_created += node_subtree_child_count(layer);
    }
    cached_key = key;
    return true;
}

bool uses_software_scene_graph(const QQuickWindow& window)
{
    const QSGRendererInterface* renderer_interface = window.rendererInterface();
    return
        renderer_interface                != nullptr &&
        renderer_interface->graphicsApi() == QSGRendererInterface::Software;
}

bool within_tolerance(qreal left, qreal right, qreal tolerance)
{
    return std::abs(left - right) <= tolerance;
}

bool nearly_same_text_geometry(qreal left, qreal right)
{
    return within_tolerance(left, right, k_text_geometry_tolerance);
}

qreal ascii_advance_tolerance(qreal cell_width)
{
    return std::max(
        k_ascii_advance_tolerance_floor,
        std::abs(cell_width) * k_ascii_advance_tolerance_ratio);
}

bool ascii_widths_match(qreal left, qreal right, qreal cell_width)
{
    return within_tolerance(left, right, ascii_advance_tolerance(cell_width));
}

bool ascii_advance_matches_cell_width(qreal advance, qreal cell_width)
{
    return ascii_widths_match(advance, cell_width, cell_width);
}

bool is_printable_ascii_text(const QString& text)
{
    if (text.isEmpty()) {
        return false;
    }

    for (const QChar character : text) {
        const ushort codepoint = character.unicode();
        if (codepoint < 0x20U || codepoint > 0x7eU) {
            return false;
        }
    }

    return true;
}

bool contains_non_ascii_text(const QString& text)
{
    for (const QChar character : text) {
        if (character.unicode() > 0x7fU) {
            return true;
        }
    }

    return false;
}

bool is_printable_ascii_cell_text(const QString& text)
{
    if (text.size() != 1) {
        return false;
    }

    const ushort codepoint = text.at(0).unicode();
    return codepoint >= 0x20U && codepoint <= 0x7eU;
}

QFont cell_stable_ascii_layout_font(const QFont& font)
{
    QFont layout_font = font;
    layout_font.setKerning(false);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    layout_font.setFeature(QFont::Tag("calt"), 0U);
    layout_font.setFeature(QFont::Tag("clig"), 0U);
    layout_font.setFeature(QFont::Tag("dlig"), 0U);
    layout_font.setFeature(QFont::Tag("hlig"), 0U);
    layout_font.setFeature(QFont::Tag("liga"), 0U);
    layout_font.setFeature(QFont::Tag("rlig"), 0U);
#endif
    layout_font.setStyleStrategy(static_cast<QFont::StyleStrategy>(
        static_cast<int>(layout_font.styleStrategy()) |
        static_cast<int>(QFont::PreferNoShaping)));
    return layout_font;
}

bool ascii_text_probe_layout_is_cell_stable(const QFont& font, qreal cell_width)
{
    const QString probe_text = QStringLiteral("==!=->=><=>=::///www");
    QTextLayout layout(probe_text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.setCacheEnabled(false);

    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (!line.isValid()) {
        layout.endLayout();
        return false;
    }

    line.setLineWidth(k_no_wrap_text_line_width);
    line.setPosition(QPointF(0.0, 0.0));
    const bool line_is_complete = line.textStart() == 0 &&
        line.textLength() == probe_text.size();
    const bool line_advance_is_stable = ascii_widths_match(
        line.horizontalAdvance(),
        static_cast<qreal>(probe_text.size()) * cell_width,
        cell_width);
    const QList<QGlyphRun> glyph_runs = line.glyphRuns(
        0,
        probe_text.size(),
        QTextLayout::RetrieveGlyphIndexes       |
            QTextLayout::RetrieveGlyphPositions |
            QTextLayout::RetrieveStringIndexes);
    layout.endLayout();

    if (!line_is_complete || !line_advance_is_stable) {
        return false;
    }

    qsizetype glyph_count = 0;
    std::vector<bool> seen_string_indexes(static_cast<std::size_t>(probe_text.size()));
    for (const QGlyphRun& glyph_run : glyph_runs) {
        const QList<quint32>   glyph_indexes  = glyph_run.glyphIndexes();
        const QList<QPointF>   positions      = glyph_run.positions();
        const QList<qsizetype> string_indexes = glyph_run.stringIndexes();
        if (glyph_indexes.size() != positions.size() ||
            glyph_indexes.size() != string_indexes.size())
        {
            return false;
        }

        for (qsizetype index = 0; index < glyph_indexes.size(); ++index) {
            const qsizetype string_index = string_indexes.at(index);
            if (string_index < 0 || string_index >= probe_text.size()) {
                return false;
            }

            const std::size_t seen_index = static_cast<std::size_t>(string_index);
            if (seen_string_indexes[seen_index]) {
                return false;
            }
            seen_string_indexes[seen_index] = true;

            if (!within_tolerance(
                    positions.at(index).x(), static_cast<qreal>(string_index) * cell_width,
                    ascii_advance_tolerance(cell_width)))
            {
                return false;
            }
            ++glyph_count;
        }
    }

    return glyph_count == probe_text.size();
}

Ascii_text_coalescing_context ascii_text_coalescing_context(
    const QFont&   font,
    qreal          cell_width)
{
    VNM_TERMINAL_PROFILE_SCOPE("ascii_text_coalescing_context");

    if (!std::isfinite(cell_width) || cell_width <= 0.0) {
        return {};
    }

    Ascii_text_coalescing_context context;
    context.layout_font = cell_stable_ascii_layout_font(font);
    const QFontInfo resolved_font(context.layout_font);
    if (!resolved_font.fixedPitch()) {
        return context;
    }

    const QFontMetricsF metrics(context.layout_font);
    for (ushort codepoint = 0x20U; codepoint <= 0x7eU; ++codepoint) {
        if (!ascii_advance_matches_cell_width(
                metrics.horizontalAdvance(QLatin1Char(static_cast<char>(codepoint))), cell_width))
        {
            return context;
        }
    }

    if (!ascii_text_probe_layout_is_cell_stable(context.layout_font, cell_width)) {
        return context;
    }

    context.enabled = true;
    return context;
}

QString ascii_text_coalescing_context_cache_key(const QFont& font, qreal cell_width)
{
    QString key = font.key();
    key += QLatin1Char('\n');
    key += QString::number(cell_width, 'g', 17);
    return key;
}

const Ascii_text_coalescing_context& Ascii_text_coalescing_context_cache::context(
    const QFont&   font,
    qreal          cell_width)
{
    const QString key   = ascii_text_coalescing_context_cache_key(font, cell_width);
    const auto    found = m_contexts.find(key);
    if (found != m_contexts.end()) {
        return found->second;
    }

    const auto inserted = m_contexts.emplace(
        key,
        ascii_text_coalescing_context(font, cell_width));
    return inserted.first->second;
}

bool is_one_cell_unclipped_printable_ascii_run(
    const Terminal_render_text_run&    run,
    qreal                              cell_width)
{
    return
        !run.clip_rect.isValid()                                &&
        is_printable_ascii_cell_text(run.text)                  &&
        nearly_same_text_geometry(run.rect.width(), cell_width) &&
        nearly_same_text_geometry(run.baseline_origin.x(), run.rect.left());
}

bool text_runs_have_matching_paint(
    const Terminal_render_text_run&    left,
    const Terminal_render_text_run&    right)
{
    return left.foreground.rgba() == right.foreground.rgba();
}

bool has_adjacent_coalescing_geometry(
    const Terminal_render_text_run&    left,
    const Terminal_render_text_run&    right,
    qreal                              cell_width)
{
    return
        left.row == right.row                                                          &&
        left.logical_row == right.logical_row                                          &&
        right.column == left.column + 1                                                &&
        nearly_same_text_geometry(left.rect.top(), right.rect.top())                   &&
        nearly_same_text_geometry(left.rect.height(), right.rect.height())             &&
        nearly_same_text_geometry(left.rect.left() + cell_width, right.rect.left())    &&
        nearly_same_text_geometry(left.baseline_origin.y(), right.baseline_origin.y()) &&
        nearly_same_text_geometry(left.baseline_origin.x() + cell_width, right.baseline_origin.x());
}

bool can_coalesce_adjacent_text_runs(
    const Terminal_render_text_run&    left,
    const Terminal_render_text_run&    right,
    qreal                              cell_width)
{
    return
        is_one_cell_unclipped_printable_ascii_run(left, cell_width)  &&
        is_one_cell_unclipped_printable_ascii_run(right, cell_width) &&
        text_runs_have_matching_paint(left, right)                   &&
        has_adjacent_coalescing_geometry(left, right, cell_width);
}

bool prepare_text_layout(QTextLayout& layout, qreal& out_line_ascent)
{
    VNM_TERMINAL_PROFILE_SCOPE("prepare_text_layout");

#if VNM_TERMINAL_PROFILING_ENABLED
    const auto started_at = std::chrono::steady_clock::now();
#endif

    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.setCacheEnabled(false);

    layout.beginLayout();
    QTextLine line          = layout.createLine();
    bool      line_has_text = false;
    if (line.isValid()) {
        line.setLineWidth(k_no_wrap_text_line_width);
        line.setPosition(QPointF(0.0, 0.0));
        out_line_ascent = line.ascent();
        line_has_text = line.textLength() > 0;
    }
    layout.endLayout();

#if VNM_TERMINAL_PROFILING_ENABLED
    if (active_slow_text_layout_recorder            != nullptr &&
        active_text_layout_diagnostic_context       != nullptr &&
        active_text_layout_diagnostic_context->font != nullptr &&
        active_text_layout_diagnostic_context->run  != nullptr)
    {
        const auto finished_at = std::chrono::steady_clock::now();
        const std::uint64_t duration_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished_at - started_at).count());
        active_slow_text_layout_recorder->record_layout(
            duration_ns,
            *active_text_layout_diagnostic_context->font,
            *active_text_layout_diagnostic_context->run,
            active_text_layout_diagnostic_context->clipped,
            active_text_layout_diagnostic_context->force_blended_order,
            active_text_layout_diagnostic_context->ascii_layout_font,
            line_has_text);
    }
#endif

    return line_has_text;
}

bool prepare_text_run_layout(
    QTextLayout&                       layout,
    qreal&                             out_line_ascent,
    const QFont&                       font,
    const Terminal_render_text_run&    run,
    bool                               clipped,
    bool                               force_blended_order,
    bool                               ascii_layout_font,
    terminal_renderer_stats_t&         stats)
{
#if VNM_TERMINAL_PROFILING_ENABLED
    const text_layout_diagnostic_context_t diagnostic_context{
        &font,
        &run,
        clipped,
        force_blended_order,
        ascii_layout_font,
    };
    const Active_text_layout_diagnostic_context_binding diagnostic_context_binding(
        &diagnostic_context);
#endif

    layout.setText(run.text);
    layout.setFont(font);
    ++stats.qt_text_layout_calls;
    ++stats.route_qt_text_layout_runs;
    return prepare_text_layout(layout, out_line_ascent);
}

QColor text_node_foreground(const Terminal_render_text_run& run, bool force_blended_order)
{
    QColor foreground = run.foreground;
    // Cursor overlay text has to stay in the blended pass so opaque cursor
    // rectangles cannot batch over it.
    if (force_blended_order && foreground.alpha() == 255) {
        foreground.setAlpha(254);
    }
    return foreground;
}

QSGTextNode* make_text_leaf_node(
    QQuickWindow&  window,
    const QColor&  foreground,
    const QRectF&  frame_viewport)
{
    QSGTextNode* text_node = window.createTextNode();
    if (text_node == nullptr) {
        return nullptr;
    }

    text_node->setFlag(QSGNode::OwnedByParent, true);
    text_node->setColor(foreground);
    text_node->setRenderType(QSGTextNode::QtRendering);
    text_node->setViewport(frame_viewport);
    return text_node;
}

void add_text_run_layout(
    QSGTextNode&                       text_node,
    QTextLayout&                       layout,
    const Terminal_render_text_run&    run,
    qreal                              line_ascent)
{
    VNM_TERMINAL_PROFILE_SCOPE("add_text_run_layout");

    text_node.addTextLayout(
        QPointF(run.baseline_origin.x(), run.baseline_origin.y() - line_ascent),
        &layout);
}

bool append_clipped_text_run_node(
    QSGNode&                           parent,
    QQuickWindow&                      window,
    const QFont&                       font,
    const Terminal_render_text_run&    run,
    const QRectF&                      frame_viewport,
    bool                               force_blended_order,
    bool                               ascii_layout_font,
    terminal_renderer_stats_t&         stats,
    int&                               out_text_leaf_nodes_created)
{
    if (run.text.isEmpty()) {
        return true;
    }

    QTextLayout layout;
    qreal line_ascent = 0.0;
    if (!prepare_text_run_layout(
            layout, line_ascent, font, run, true, force_blended_order, ascii_layout_font, stats))
    {
        return true;
    }

    QSGTextNode* text_node = make_text_leaf_node(
        window,
        text_node_foreground(run, force_blended_order),
        frame_viewport);
    if (text_node == nullptr) {
        return false;
    }
    ++out_text_leaf_nodes_created;
    ++stats.qsg_nodes_created;

    add_text_run_layout(*text_node, layout, run, line_ascent);

    auto* clip_node = new QSGClipNode();
    clip_node->setFlag(QSGNode::OwnedByParent, true);
    clip_node->setIsRectangular(true);
    clip_node->setClipRect(run.clip_rect);
    clip_node->appendChildNode(text_node);
    parent.appendChildNode(clip_node);
    ++stats.qsg_nodes_created;
    return true;
}

bool append_unclipped_text_run_layout(
    QSGNode&                           parent,
    QQuickWindow&                      window,
    const QFont&                       font,
    const Terminal_render_text_run&    run,
    const QRectF&                      frame_viewport,
    bool                               force_blended_order,
    bool                               ascii_layout_font,
    QSGTextNode*&                      active_text_node,
    QRgb&                              active_foreground_rgba,
    terminal_renderer_stats_t&         stats,
    int&                               out_text_leaf_nodes_created)
{
    if (run.text.isEmpty()) {
        return true;
    }

    QTextLayout layout;
    qreal line_ascent = 0.0;
    if (!prepare_text_run_layout(
            layout, line_ascent, font, run, false, force_blended_order, ascii_layout_font, stats))
    {
        return true;
    }

    const QColor foreground = text_node_foreground(run, force_blended_order);
    if (active_text_node       == nullptr ||
        active_foreground_rgba != foreground.rgba())
    {
        active_text_node = make_text_leaf_node(window, foreground, frame_viewport);
        if (active_text_node == nullptr) {
            return false;
        }

        parent.appendChildNode(active_text_node);
        active_foreground_rgba = foreground.rgba();
        ++out_text_leaf_nodes_created;
        ++stats.qsg_nodes_created;
    }

    add_text_run_layout(*active_text_node, layout, run, line_ascent);
    return true;
}

Terminal_render_text_run coalesced_ascii_text_run(
    const std::vector<Terminal_render_text_run>&   runs,
    std::size_t                                    first_index,
    std::size_t                                    past_last_index)
{
    Terminal_render_text_run coalesced_run = runs[first_index];
    coalesced_run.text.reserve(static_cast<qsizetype>(past_last_index - first_index));
    for (std::size_t index = first_index + 1U; index < past_last_index; ++index) {
        const Terminal_render_text_run& run = runs[index];
        coalesced_run.text += run.text;
    }
    const Terminal_render_text_run& last_run = runs[past_last_index - 1U];
    coalesced_run.rect.setWidth(
        last_run.rect.left() + last_run.rect.width() - coalesced_run.rect.left());
    return coalesced_run;
}

std::size_t past_last_coalescible_ascii_text_run(
    const std::vector<Terminal_render_text_run>&   runs,
    std::size_t                                    first_index,
    qreal                                          cell_width)
{
    std::size_t past_last_index = first_index + 1U;
    while (past_last_index            < runs.size()                           &&
        past_last_index - first_index < k_max_coalesced_ascii_text_run_length &&
        can_coalesce_adjacent_text_runs(
            runs[past_last_index - 1U], runs[past_last_index], cell_width))
    {
        ++past_last_index;
    }

    return past_last_index;
}

bool try_coalesce_ascii_text_resource_runs(
    const std::vector<Terminal_render_text_run>&   runs,
    qreal                                          cell_width,
    std::vector<text_resource_run_t>&              out_runs)
{
    out_runs.clear();
    if (runs.size() <  2U          ||
        !std::isfinite(cell_width) ||
        cell_width  <= 0.0)
    {
        return false;
    }

    bool coalesced = false;
    for (std::size_t index = 0U; index < runs.size();) {
        const std::size_t past_last_index =
            past_last_coalescible_ascii_text_run(runs, index, cell_width);
        if (past_last_index > index + 1U) {
            if (!coalesced) {
                out_runs.reserve(runs.size());
                for (std::size_t prefix_index = 0U; prefix_index < index; ++prefix_index) {
                    out_runs.push_back({runs[prefix_index], false});
                }
                coalesced = true;
            }

            out_runs.push_back({
                coalesced_ascii_text_run(runs, index, past_last_index),
                true,
            });
            index = past_last_index;
            continue;
        }

        if (coalesced) {
            out_runs.push_back({runs[index], false});
        }
        ++index;
    }

    if (!coalesced) {
        out_runs.clear();
    }
    return coalesced;
}

bool append_batched_text_run_nodes(
    QSGNode&                                       parent,
    QQuickWindow&                                  window,
    const QFont&                                   font,
    const std::vector<Terminal_render_text_run>&   runs,
    const QRectF&                                  frame_viewport,
    bool                                           force_blended_order,
    terminal_renderer_stats_t&                     stats,
    int&                                           out_text_leaf_nodes_created)
{
    VNM_TERMINAL_PROFILE_SCOPE("append_batched_text_run_nodes");

    QSGTextNode* active_text_node       = nullptr;
    QRgb         active_foreground_rgba = 0U;
    for (std::size_t index = 0U; index < runs.size();) {
        const Terminal_render_text_run& run = runs[index];
        if (run.clip_rect.isValid()) {
            active_text_node = nullptr;
            if (append_clipped_text_run_node(
                    parent, window, font, run, frame_viewport, force_blended_order,
                    false, stats, out_text_leaf_nodes_created))
            {
                ++index;
                continue;
            }
            return false;
        }

        if (run.text.isEmpty()) {
            ++index;
            continue;
        }

        if (!append_unclipped_text_run_layout(
                parent, window, font, run, frame_viewport, force_blended_order, false,
                active_text_node, active_foreground_rgba, stats, out_text_leaf_nodes_created))
        {
            return false;
        }

        ++index;
    }

    return true;
}

bool append_batched_text_run_nodes(
    QSGNode&                                   parent,
    QQuickWindow&                              window,
    const QFont&                               font,
    const std::vector<text_resource_run_t>&    runs,
    const QRectF&                              frame_viewport,
    qreal                                      cell_width,
    const Ascii_text_coalescing_context*       coalescing_context,
    bool                                       force_blended_order,
    terminal_renderer_stats_t&                 stats,
    int&                                       out_text_leaf_nodes_created)
{
    VNM_TERMINAL_PROFILE_SCOPE("append_batched_text_run_nodes");
    (void)cell_width;

    QSGTextNode* active_text_node       = nullptr;
    QRgb         active_foreground_rgba = 0U;
    for (const text_resource_run_t& resource_run : runs) {
        const Terminal_render_text_run& run = resource_run.run;
        Q_ASSERT(!resource_run.use_ascii_layout_font ||
            (coalescing_context != nullptr && coalescing_context->enabled));
        const QFont& layout_font = resource_run.use_ascii_layout_font
            ? coalescing_context->layout_font
            : font;
        if (run.clip_rect.isValid()) {
            active_text_node = nullptr;
            if (append_clipped_text_run_node(
                    parent, window, layout_font, run, frame_viewport, force_blended_order,
                    resource_run.use_ascii_layout_font, stats, out_text_leaf_nodes_created))
            {
                continue;
            }
            return false;
        }

        if (run.text.isEmpty()) {
            continue;
        }

        if (!append_unclipped_text_run_layout(
                parent, window, layout_font, run, frame_viewport, force_blended_order,
                resource_run.use_ascii_layout_font, active_text_node,
                active_foreground_rgba, stats, out_text_leaf_nodes_created))
        {
            return false;
        }
    }

    return true;
}

template <typename Append_text_runs>
Terminal_text_resource_node* make_text_resource_node_with_children(
    const std::shared_ptr<Terminal_renderer_lifecycle_recorder>&
                               lifecycle_recorder,
    terminal_renderer_stats_t& stats,
    bool&                      out_failed,
    int&                       out_text_leaf_nodes_created,
    const Append_text_runs&    append_text_runs)
{
    VNM_TERMINAL_PROFILE_SCOPE("make_text_resource_node");

    auto* node = new Terminal_text_resource_node(lifecycle_recorder);
    ++stats.qsg_nodes_created;
    int text_leaf_nodes_created = 0;
    if (!append_text_runs(*node, text_leaf_nodes_created)) {
        out_failed = true;
        stats.qsg_nodes_destroyed += 1 + node_subtree_child_count(*node);
        delete node;
        return nullptr;
    }

    out_text_leaf_nodes_created = text_leaf_nodes_created;
    return node;
}

Terminal_text_resource_node* make_text_resource_node(
    QQuickWindow&              window,
    const std::vector<Terminal_render_text_run>&
                               runs,
    const QFont&               font,
    const QRectF&              frame_viewport,
    const std::shared_ptr<Terminal_renderer_lifecycle_recorder>&
                               lifecycle_recorder,
    bool                       force_blended_order,
    terminal_renderer_stats_t& stats,
    bool&                      out_failed,
    int&                       out_text_leaf_nodes_created)
{
    return make_text_resource_node_with_children(
        lifecycle_recorder,
        stats,
        out_failed,
        out_text_leaf_nodes_created,
        [&](Terminal_text_resource_node& node, int& text_leaf_nodes_created) {
            return
                append_batched_text_run_nodes(
                    node,
                    window,
                    font,
                    runs,
                    frame_viewport,
                    force_blended_order,
                    stats,
                    text_leaf_nodes_created);
        });
}

Terminal_text_resource_node* make_text_resource_node(
    QQuickWindow&              window,
    const std::vector<text_resource_run_t>&
                               runs,
    const QFont&               font,
    const QRectF&              frame_viewport,
    qreal                      cell_width,
    const Ascii_text_coalescing_context*
                               coalescing_context,
    const std::shared_ptr<Terminal_renderer_lifecycle_recorder>&
                               lifecycle_recorder,
    bool                       force_blended_order,
    terminal_renderer_stats_t& stats,
    bool&                      out_failed,
    int&                       out_text_leaf_nodes_created)
{
    return make_text_resource_node_with_children(
        lifecycle_recorder,
        stats,
        out_failed,
        out_text_leaf_nodes_created,
        [&](Terminal_text_resource_node& node, int& text_leaf_nodes_created) {
            return
                append_batched_text_run_nodes(
                    node,
                    window,
                    font,
                    runs,
                    frame_viewport,
                    cell_width,
                    coalescing_context,
                    force_blended_order,
                    stats,
                    text_leaf_nodes_created);
        });
}

qreal row_top_for_viewport_row(
    int                        row,
    terminal_cell_metrics_t    metrics)
{
    return static_cast<qreal>(row) * metrics.height;
}

QRectF row_local_rect(QRectF rect, qreal row_top)
{
    if (rect.isValid()) {
        rect.translate(0.0, -row_top);
    }

    return rect;
}

Terminal_render_text_run row_local_text_run(
    const Terminal_render_text_run&    run,
    qreal                              row_top)
{
    Terminal_render_text_run local_run = run;
    local_run.row             = 0;
    local_run.logical_row     = 0;
    local_run.rect            = row_local_rect(run.rect,      row_top);
    local_run.clip_rect       = row_local_rect(run.clip_rect, row_top);
    local_run.baseline_origin = QPointF(
        run.baseline_origin.x(),
        run.baseline_origin.y() - row_top);
    return local_run;
}

void assign_row_local_text_runs(
    const std::vector<const Terminal_render_text_run*>&    runs,
    qreal                                                  row_top,
    std::vector<Terminal_render_text_run>&                 out_runs)
{
    VNM_TERMINAL_PROFILE_SCOPE("row_local_text_runs");

    out_runs.clear();
    out_runs.reserve(runs.size());
    for (const Terminal_render_text_run* run : runs) {
        out_runs.push_back(row_local_text_run(*run, row_top));
    }
}

void set_text_wrapper_row_top(QSGTransformNode& wrapper, qreal row_top)
{
    VNM_TERMINAL_PROFILE_SCOPE("set_text_wrapper_row_top");

    QMatrix4x4 matrix;
    matrix.translate(0.0F, static_cast<float>(row_top));
    wrapper.setMatrix(matrix);
}

QSGTransformNode* make_text_wrapper_node(qreal row_top)
{
    auto* wrapper = new QSGTransformNode();
    wrapper->setFlag(QSGNode::OwnedByParent, true);
    set_text_wrapper_row_top(*wrapper, row_top);
    return wrapper;
}

QRectF row_text_clip_rect(const Terminal_render_frame& frame)
{
    return QRectF(
        0.0,
        0.0,
        frame.logical_size.width(),
        frame.cell_metrics.height);
}

QSGClipNode* make_row_text_clip_node(QRectF clip_rect)
{
    auto* clip = new QSGClipNode();
    clip->setFlag(QSGNode::OwnedByParent, true);
    clip->setIsRectangular(true);
    clip->setClipRect(clip_rect);
    return clip;
}

void set_row_text_clip_rect(
    Terminal_scene_node::Text_resource_cache_slot& cache,
    QRectF                                         clip_rect)
{
    VNM_TERMINAL_PROFILE_SCOPE("set_row_text_clip_rect");

    if (cache.clip_rect == clip_rect) {
        return;
    }

    cache.clip->setClipRect(clip_rect);
    cache.clip_rect = clip_rect;
}

bool sync_text_wrapper_row_top(
    Terminal_scene_node::Text_resource_cache_slot& slot,
    qreal                                          row_top)
{
    if (slot.row_top == row_top) {
        return false;
    }

    set_text_wrapper_row_top(*slot.wrapper, row_top);
    slot.row_top = row_top;
    return true;
}

QString font_cache_key(const QFont& font)
{
    return font.toString();
}

QByteArray text_frame_cache_key(
    const Terminal_render_frame&   frame,
    const QString&                 font_key)
{
    VNM_TERMINAL_PROFILE_SCOPE("text_frame_cache_key");

    QByteArray key;
    constexpr std::size_t k_string_length_byte_count = sizeof(std::uint64_t);
    key.reserve(
        static_cast<qsizetype>(
            k_string_length_byte_count +
            static_cast<std::size_t>(font_key.size()) * sizeof(QChar) +
            6U * sizeof(qreal) +
            3U * sizeof(int)));
    append_cache_key_string(key, font_key);
    append_cache_key_value(key, frame.logical_size.width());
    append_cache_key_value(key, frame.logical_size.height());
    append_cache_key_value(key, frame.cell_metrics.width);
    append_cache_key_value(key, frame.cell_metrics.height);
    append_cache_key_value(key, frame.cell_metrics.ascent);
    append_cache_key_value(key, frame.cell_metrics.descent);
    append_cache_key_value(key, frame.grid_size.rows);
    append_cache_key_value(key, frame.grid_size.columns);
    append_cache_key_value(key, static_cast<int>(frame.viewport.active_buffer));
    return key;
}

void append_text_rect_key_fields(QByteArray& key, const QRectF& rect)
{
    append_cache_key_bool(key, rect.isValid());
    append_cache_key_value(key, rect.x());
    append_cache_key_value(key, rect.y());
    append_cache_key_value(key, rect.width());
    append_cache_key_value(key, rect.height());
}

const Terminal_render_text_run& text_resource_key_run(const Terminal_render_text_run& run)
{
    return run;
}

const Terminal_render_text_run& text_resource_key_run(const text_resource_run_t& run)
{
    return run.run;
}

bool text_resource_key_uses_ascii_layout_font(const Terminal_render_text_run& run)
{
    (void)run;
    return false;
}

bool text_resource_key_uses_ascii_layout_font(const text_resource_run_t& run)
{
    return run.use_ascii_layout_font;
}

template <typename Text_runs>
qsizetype text_resource_key_reserve_size(
    const Text_runs&   runs,
    const QString&     font_key)
{
    std::size_t utf16_code_units = static_cast<std::size_t>(font_key.size());
    for (const auto& resource_run : runs) {
        const Terminal_render_text_run& run = text_resource_key_run(resource_run);
        utf16_code_units += static_cast<std::size_t>(run.text.size());
    }

    constexpr std::size_t k_string_length_byte_count = sizeof(std::uint64_t);
    constexpr std::size_t k_rect_byte_count =
        sizeof(char) + 4U * sizeof(qreal);
    constexpr std::size_t k_header_byte_count =
        k_string_length_byte_count +
        2U * sizeof(qreal) +
        sizeof(std::uint64_t);
    constexpr std::size_t k_run_fixed_byte_count =
        3U * sizeof(int) +
        2U * k_rect_byte_count +
        2U * sizeof(qreal) +
        sizeof(QRgb) +
        sizeof(char) +
        k_string_length_byte_count;

    return static_cast<qsizetype>(
        k_header_byte_count                  +
        runs.size() * k_run_fixed_byte_count +
        utf16_code_units * sizeof(QChar));
}

template <typename Text_runs>
QByteArray text_resource_key(
    const Text_runs&   runs,
    const QString&     font_key,
    QSizeF             logical_size)
{
    VNM_TERMINAL_PROFILE_SCOPE("text_resource_key");

    QByteArray key;
    key.reserve(text_resource_key_reserve_size(runs, font_key));
    append_cache_key_string(key, font_key);
    append_cache_key_value(key, logical_size.width());
    append_cache_key_value(key, logical_size.height());
    append_cache_key_size(key, runs.size());
    for (const auto& resource_run : runs) {
        const Terminal_render_text_run& run = text_resource_key_run(resource_run);
        append_cache_key_bool(key, text_resource_key_uses_ascii_layout_font(resource_run));
        append_cache_key_value(key, run.row);
        append_cache_key_value(key, run.logical_row);
        append_cache_key_value(key, run.column);
        append_text_rect_key_fields(key, run.rect);
        append_text_rect_key_fields(key, run.clip_rect);
        append_cache_key_value(key, run.baseline_origin.x());
        append_cache_key_value(key, run.baseline_origin.y());
        append_cache_key_value(key, run.foreground.rgba());
        append_cache_key_string(key, run.text);
    }
    return key;
}

std::map<int, std::vector<Terminal_render_text_run>> text_runs_by_row(
    const std::vector<Terminal_render_text_run>&   runs,
    int                                            row_count)
{
    std::map<int, std::vector<Terminal_render_text_run>> rows;
    for (const Terminal_render_text_run& run : runs) {
        if (run.row >= 0 && run.row < row_count) {
            rows[run.row].push_back(run);
        }
    }
    return rows;
}

std::vector<row_text_group_t> text_run_groups_by_viewport_row(
    const Terminal_render_frame& frame)
{
    VNM_TERMINAL_PROFILE_SCOPE("text_run_groups_by_viewport_row");

    if (frame.grid_size.rows <= 0) {
        return {};
    }

    std::vector<int> run_counts_by_row(static_cast<std::size_t>(frame.grid_size.rows), 0);
    for (const Terminal_render_text_run& run : frame.text_runs) {
        if (run.row >= 0 && run.row < frame.grid_size.rows) {
            ++run_counts_by_row[static_cast<std::size_t>(run.row)];
        }
    }

    std::vector<std::optional<row_text_group_t>> groups_by_row(
        static_cast<std::size_t>(frame.grid_size.rows));
    for (const Terminal_render_text_run& run : frame.text_runs) {
        if (run.row < 0 || run.row >= frame.grid_size.rows) {
            continue;
        }

        const std::size_t row_index = static_cast<std::size_t>(run.row);
        const row_cache_identity_t identity{
            frame.viewport.active_buffer,
            run.logical_row,
        };
        std::optional<row_text_group_t>& slot = groups_by_row[row_index];
        if (!slot.has_value()) {
            slot.emplace();
        }

        row_text_group_t& group = *slot;
        if (group.runs.empty()) {
            group.identity     = identity;
            group.viewport_row = run.row;
            group.row_top      = row_top_for_viewport_row(run.row, frame.cell_metrics);
            group.runs.reserve(static_cast<std::size_t>(run_counts_by_row[row_index]));
        }
        group.runs.push_back(&run);
    }

    std::vector<row_text_group_t> groups;
    groups.reserve(groups_by_row.size());
    for (std::optional<row_text_group_t>& group : groups_by_row) {
        if (group.has_value()) {
            groups.push_back(std::move(*group));
        }
    }
    return groups;
}

int dirty_row_count(const Terminal_render_frame& frame)
{
    int rows = 0;
    for (const Terminal_render_dirty_row_range& range : frame.dirty_row_ranges) {
        rows += range.row_count;
    }
    return rows;
}

bool row_is_dirty(
    const Terminal_render_frame&   frame,
    int                            row)
{
    for (const Terminal_render_dirty_row_range& range : frame.dirty_row_ranges) {
        if (row >= range.first_row && row < range.first_row + range.row_count) { return true;  }
        if (row <  range.first_row)                                            { return false; }
    }
    return false;
}

bool row_has_cursor_text_run(
    const Terminal_render_frame&   frame,
    int                            row)
{
    for (const Terminal_render_text_run& run : frame.cursor_text_runs) {
        if (run.row == row) {
            return true;
        }
    }

    return false;
}

bool row_has_preedit_caret(
    const Terminal_render_frame&   frame,
    int                            row)
{
    const qreal row_top = row_top_for_viewport_row(row, frame.cell_metrics);
    for (const Terminal_render_decoration& decoration : frame.decorations) {
        if (decoration.kind == Terminal_render_decoration_kind::PREEDIT_CARET &&
            nearly_same_text_geometry(decoration.rect.top(), row_top))
        {
            return true;
        }
    }

    return false;
}

Text_resource_rect_descriptor make_text_resource_rect_descriptor(QRectF rect)
{
    return {
        rect.isValid(),
        rect.left(),
        rect.top(),
        rect.width(),
        rect.height(),
    };
}

bool text_resource_descriptor_run_is_eligible(
    const Terminal_render_text_run&    run,
    const row_text_group_t&            group)
{
    return
        run.row == group.viewport_row                 &&
        run.logical_row == group.identity.logical_row &&
        !run.clip_rect.isValid();
}

std::optional<Text_resource_row_descriptor> text_resource_row_descriptor(
    const Terminal_render_frame&       frame,
    const row_text_group_t&            group)
{
    VNM_TERMINAL_PROFILE_SCOPE("text_resource_row_descriptor");

    if (row_has_cursor_text_run(frame, group.viewport_row) ||
        row_has_preedit_caret(frame, group.viewport_row))
    {
        return std::nullopt;
    }

    Text_resource_row_descriptor descriptor;
    descriptor.runs.reserve(group.runs.size());
    for (const Terminal_render_text_run* run_ptr : group.runs) {
        const Terminal_render_text_run& run = *run_ptr;
        if (!text_resource_descriptor_run_is_eligible(run, group)) {
            return std::nullopt;
        }

        const QRectF local_rect = row_local_rect(run.rect, group.row_top);
        descriptor.runs.push_back({
            run.column,
            make_text_resource_rect_descriptor(local_rect),
            run.baseline_origin.x(),
            run.baseline_origin.y() - group.row_top,
            run.foreground.rgba(),
            run.text,
        });
    }

    return descriptor;
}

bool text_row_slot_order_changed(
    const std::vector<Terminal_scene_node::Text_resource_cache_slot>&  previous_slots,
    const std::vector<Terminal_scene_node::Text_resource_cache_slot>&  current_slots)
{
    if (previous_slots.size() != current_slots.size()) {
        return true;
    }

    for (std::size_t index = 0U; index < previous_slots.size(); ++index) {
        if (previous_slots[index].identity != current_slots[index].identity) {
            return true;
        }
    }

    return false;
}

struct Text_row_slot_transfer
{
    std::vector<Terminal_scene_node::Text_resource_cache_slot> cache_slots;
    std::map<row_cache_identity_t, std::size_t>                index_by_identity;
    std::vector<bool>                                          consumed_slots;
};

Text_row_slot_transfer take_text_row_slots(
    std::vector<Terminal_scene_node::Text_resource_cache_slot>& row_slots)
{
    Text_row_slot_transfer transfer;
    transfer.cache_slots = std::move(row_slots);
    transfer.index_by_identity.clear();
    transfer.consumed_slots.assign(transfer.cache_slots.size(), false);
    for (std::size_t index = 0; index < transfer.cache_slots.size(); ++index) {
        const auto inserted =
            transfer.index_by_identity.emplace(transfer.cache_slots[index].identity, index);
        Q_ASSERT(inserted.second);
    }
    return transfer;
}

std::optional<Terminal_scene_node::Text_resource_cache_slot> take_text_row_slot(
    Text_row_slot_transfer&        transfer,
    const row_cache_identity_t&    identity)
{
    const auto index = transfer.index_by_identity.find(identity);
    if (index == transfer.index_by_identity.end()) {
        return std::nullopt;
    }

    Q_ASSERT(!transfer.consumed_slots[index->second]);
    transfer.consumed_slots[index->second] = true;
    return std::move(transfer.cache_slots[index->second]);
}

void append_text_row_slot_in_order(
    QSGNode&               text_layer,
    Terminal_scene_node::Text_resource_cache_slot&
                           slot)
{
    if (slot.wrapper->parent() != nullptr) {
        slot.wrapper->parent()->removeChildNode(slot.wrapper);
    }
    text_layer.appendChildNode(slot.wrapper);
}

int destroy_unconsumed_text_row_slots(
    Text_row_slot_transfer&    transfer,
    terminal_renderer_stats_t& stats)
{
    int rows_removed = 0;
    for (std::size_t index = 0; index < transfer.cache_slots.size(); ++index) {
        if (transfer.consumed_slots[index]) {
            continue;
        }

        Terminal_scene_node::Text_resource_cache_slot& slot = transfer.cache_slots[index];
        record_text_cache_entry_nodes_cleared(stats, slot, false);
        ++stats.text_cache_entries_removed;
        stats.qsg_nodes_destroyed += 1 + node_subtree_child_count(*slot.wrapper);
        destroy_text_cache_entry(slot);
        ++stats.text_content_removed;
        ++rows_removed;
    }
    return rows_removed;
}

void sync_text_resource_nodes(
    Terminal_scene_node&       root,
    QQuickWindow&              window,
    const Terminal_render_frame&
                               frame,
    const QFont&               font,
    Ascii_text_coalescing_context_cache&
                               coalescing_context_cache,
    terminal_renderer_stats_t& stats,
    const std::shared_ptr<Terminal_renderer_lifecycle_recorder>&
                               lifecycle_recorder)
{
    VNM_TERMINAL_PROFILE_SCOPE("sync_text_resource_nodes");

    const QRectF frame_viewport(QPointF(0.0, 0.0), frame.logical_size);
    const QRectF     row_clip_rect          = row_text_clip_rect(frame);
    const QString    text_font_key          = font_cache_key(font);
    const QByteArray current_text_frame_key = text_frame_cache_key(frame, text_font_key);
    record_text_cache_key_build(stats, current_text_frame_key);
    const bool same_text_frame_key = root.text_frame_key == current_text_frame_key;
    const bool clean_row_cache_skip_available =
        !frame.dirty_row_ranges.empty() &&
        same_text_frame_key;
    stats.text_dirty_row_ranges = static_cast<int>(frame.dirty_row_ranges.size());
    stats.text_dirty_rows       = dirty_row_count(frame);
    stats.text_runs_considered  = static_cast<int>(frame.text_runs.size());
    const std::vector<row_text_group_t> groups    = text_run_groups_by_viewport_row(frame);
    Text_row_slot_transfer              old_slots = take_text_row_slots(root.text_row_slots);
    std::vector<Terminal_scene_node::Text_resource_cache_slot> new_slots;
    new_slots.reserve(groups.size());
    // Scratch storage is live only while use_coalesced_resource_runs selects it.
    std::vector<Terminal_render_text_run> local_runs;
    std::vector<text_resource_run_t> coalesced_resource_runs;
    for (const row_text_group_t& group : groups) {
        std::optional<Terminal_scene_node::Text_resource_cache_slot> old_slot;
        bool dirty_group = false;
        {
            VNM_TERMINAL_PROFILE_SCOPE("sync_text_resource_nodes::group_bookkeeping");

            ++stats.text_groups_considered;

            old_slot = take_text_row_slot(old_slots, group.identity);
            dirty_group = row_is_dirty(frame, group.viewport_row);
            if (dirty_group) {
                ++stats.text_groups_dirty;
            }
            else {
                ++stats.text_groups_clean;
            }
        }

        std::optional<Text_resource_row_descriptor> text_resource_descriptor = text_resource_row_descriptor(
            frame,
            group);
        if (!dirty_group &&
            clean_row_cache_skip_available &&
            old_slot.has_value() &&
            text_resource_descriptor.has_value() &&
            old_slot->text_resource_descriptor.has_value())
        {
            VNM_TERMINAL_PROFILE_SCOPE("sync_text_resource_nodes::clean_cache_skip");

            sync_text_wrapper_row_top(*old_slot, group.row_top);
            set_row_text_clip_rect(*old_slot, row_clip_rect);
            ++stats.text_content_reused;
            ++stats.text_clean_reuse_skips;
            new_slots.push_back(std::move(*old_slot));
            continue;
        }

        if (same_text_frame_key &&
            old_slot.has_value() &&
            text_resource_descriptor.has_value() &&
            old_slot->text_resource_descriptor == text_resource_descriptor)
        {
            VNM_TERMINAL_PROFILE_SCOPE("sync_text_resource_nodes::text_resource_descriptor_reuse");

            sync_text_wrapper_row_top(*old_slot, group.row_top);
            set_row_text_clip_rect(*old_slot, row_clip_rect);
            ++stats.text_content_reused;
            ++stats.text_resource_descriptor_reuses;
            new_slots.push_back(std::move(*old_slot));
            continue;
        }

        assign_row_local_text_runs(group.runs, group.row_top, local_runs);
        const Ascii_text_coalescing_context* coalescing_context = nullptr;
        bool use_coalesced_resource_runs = false;
        {
            VNM_TERMINAL_PROFILE_SCOPE("sync_text_resource_nodes::coalescing");

            if (try_coalesce_ascii_text_resource_runs(
                    local_runs, frame.cell_metrics.width, coalesced_resource_runs))
            {
                ++stats.text_coalescing_candidate_groups;
                coalescing_context =
                    &coalescing_context_cache.context(font, frame.cell_metrics.width);
                if (coalescing_context->enabled) {
                    ++stats.text_coalescing_enabled_groups;
                    use_coalesced_resource_runs = true;
                }
            }
        }

        stats.text_resource_runs_before_coalescing +=
            static_cast<int>(local_runs.size());
        stats.text_resource_runs_after_coalescing += use_coalesced_resource_runs
            ? static_cast<int>(coalesced_resource_runs.size())
            : static_cast<int>(local_runs.size());
        QByteArray key = use_coalesced_resource_runs
            ? text_resource_key(coalesced_resource_runs, text_font_key, frame.logical_size)
            : text_resource_key(local_runs, text_font_key, frame.logical_size);
        record_text_cache_key_build(stats, key);
        if (old_slot.has_value() &&
            old_slot->key == key)
        {
            VNM_TERMINAL_PROFILE_SCOPE("sync_text_resource_nodes::key_match_reuse");

            sync_text_wrapper_row_top(*old_slot, group.row_top);
            set_row_text_clip_rect(*old_slot, row_clip_rect);
            old_slot->text_resource_descriptor = std::move(text_resource_descriptor);
            ++stats.text_content_reused;
            new_slots.push_back(std::move(*old_slot));
            continue;
        }

        bool failed                  = false;
        int  text_leaf_nodes_created = 0;
        Terminal_text_resource_node* node = use_coalesced_resource_runs
            ? make_text_resource_node(
                window,
                coalesced_resource_runs,
                font,
                frame_viewport,
                frame.cell_metrics.width,
                coalescing_context,
                lifecycle_recorder,
                false,
                stats,
                failed,
                text_leaf_nodes_created)
            : make_text_resource_node(
                window,
                local_runs,
                font,
                frame_viewport,
                lifecycle_recorder,
                false,
                stats,
                failed,
                text_leaf_nodes_created);
        if (node != nullptr) {
            if (old_slot.has_value()) {
                VNM_TERMINAL_PROFILE_SCOPE("sync_text_resource_nodes::replace_cache_entry");

                record_text_cache_entry_nodes_cleared(stats, *old_slot, true);
                ++stats.qsg_nodes_replaced;
                stats.qsg_nodes_destroyed += 1 + node_subtree_child_count(*old_slot->node);
                old_slot->clip->removeChildNode(old_slot->node);
                delete old_slot->node;
                old_slot->node                     = node;
                old_slot->key                      = std::move(key);
                old_slot->text_resource_descriptor = std::move(text_resource_descriptor);
                sync_text_wrapper_row_top(*old_slot, group.row_top);
                set_row_text_clip_rect(*old_slot, row_clip_rect);
                old_slot->clip->appendChildNode(node);
                new_slots.push_back(std::move(*old_slot));
                ++stats.text_cache_entries_replaced;
            }
            else {
                VNM_TERMINAL_PROFILE_SCOPE("sync_text_resource_nodes::create_cache_entry");

                QSGTransformNode* wrapper = make_text_wrapper_node(group.row_top);
                QSGClipNode*      clip    = make_row_text_clip_node(row_clip_rect);
                clip->appendChildNode(node);
                wrapper->appendChildNode(clip);
                stats.qsg_nodes_created += 2;
                new_slots.push_back({
                    .identity  = group.identity,
                    .wrapper   = wrapper,
                    .clip      = clip,
                    .node      = node,
                    .clip_rect = row_clip_rect,
                    .key       = std::move(key),
                    .text_resource_descriptor = std::move(text_resource_descriptor),
                    .row_top   = group.row_top,
                });
                ++stats.text_cache_entries_created;
            }
            ++stats.text_content_rebuilds;
            stats.text_leaf_nodes_created += text_leaf_nodes_created;
        }
        else {
            ++stats.text_content_failures;
            if (old_slot.has_value()) {
                VNM_TERMINAL_PROFILE_SCOPE("sync_text_resource_nodes::failed_cache_cleanup");

                record_text_cache_entry_nodes_cleared(stats, *old_slot, false);
                ++stats.text_cache_entries_removed;
                stats.qsg_nodes_destroyed += 1 + node_subtree_child_count(*old_slot->wrapper);
                destroy_text_cache_entry(*old_slot);
                ++stats.text_content_removed;
            }
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("sync_text_resource_nodes::remove_stale_entries");

        (void)destroy_unconsumed_text_row_slots(old_slots, stats);
    }

    stats.text_wrapper_order_rebuilt =
        text_row_slot_order_changed(old_slots.cache_slots, new_slots);
    if (stats.text_wrapper_order_rebuilt) {
        VNM_TERMINAL_PROFILE_SCOPE("sync_text_resource_nodes::reparent_slots");

        for (Terminal_scene_node::Text_resource_cache_slot& slot : new_slots) {
            append_text_row_slot_in_order(*root.text_layer, slot);
        }
    }
    root.text_row_slots = std::move(new_slots);
    root.text_frame_key = current_text_frame_key;
}

int logical_row_for_viewport_row(
    const Terminal_viewport_state&                 viewport,
    int                                            row);

bool rect_matches_frame_viewport(
    const QRectF&  rect,
    QSizeF         logical_size)
{
    return
        logical_size.isValid()                                        &&
        nearly_same_text_geometry(rect.left(), 0.0)                   &&
        nearly_same_text_geometry(rect.top(), 0.0)                    &&
        nearly_same_text_geometry(rect.width(), logical_size.width()) &&
        nearly_same_text_geometry(rect.height(), logical_size.height());
}

std::optional<int> row_for_rect_geometry(
    const Terminal_render_frame&   frame,
    const QRectF&                  rect,
    bool                           full_viewport_rect_is_frame)
{
    if (full_viewport_rect_is_frame &&
        rect_matches_frame_viewport(rect, frame.logical_size))
    {
        return std::nullopt;
    }

    if (frame.grid_size.rows      <= 0            ||
        !std::isfinite(frame.cell_metrics.height) ||
        frame.cell_metrics.height <= 0.0          ||
        !std::isfinite(rect.left())               ||
        !std::isfinite(rect.top())                ||
        !std::isfinite(rect.width())              ||
        !std::isfinite(rect.height())             ||
        rect.width()              <= 0.0          ||
        rect.height()             <= 0.0)
    {
        return std::nullopt;
    }

    const qreal rect_top     = rect.top();
    const qreal rect_bottom  = rect.top() + rect.height();
    const qreal row_position = rect_top / frame.cell_metrics.height;
    const qreal row_tolerance =
        k_text_geometry_tolerance / frame.cell_metrics.height;
    if (!std::isfinite(row_position) ||
        !std::isfinite(rect_bottom) ||
        rect_top < -k_text_geometry_tolerance)
    {
        return std::nullopt;
    }

    const int row = static_cast<int>(std::floor(row_position + row_tolerance));
    if (row < 0 || row >= frame.grid_size.rows) {
        return std::nullopt;
    }

    const qreal row_top    = row_top_for_viewport_row(row, frame.cell_metrics);
    const qreal row_bottom = row_top + frame.cell_metrics.height;
    if (rect_top    < row_top - k_text_geometry_tolerance ||
        rect_bottom > row_bottom + k_text_geometry_tolerance)
    {
        return std::nullopt;
    }

    return row;
}

std::optional<int> row_for_rect(
    const Terminal_render_frame&   frame,
    const Terminal_render_rect&    rect,
    bool                           full_viewport_rect_is_frame)
{
    return row_for_rect_geometry(frame, rect.rect, full_viewport_rect_is_frame);
}

std::optional<int> row_for_arc(
    const Terminal_render_frame&   frame,
    const Terminal_render_arc&     arc)
{
    return row_for_rect_geometry(frame, arc.rect, false);
}

Terminal_render_rect row_local_rect_primitive(
    const Terminal_render_rect&    rect,
    qreal                          row_top)
{
    Terminal_render_rect local_rect = rect;
    local_rect.rect = row_local_rect(rect.rect, row_top);
    return local_rect;
}

std::vector<Terminal_render_rect> row_local_rects(
    const std::vector<Terminal_render_rect>&   rects,
    qreal                                      row_top)
{
    VNM_TERMINAL_PROFILE_SCOPE("row_local_rects");

    std::vector<Terminal_render_rect> local_rects;
    local_rects.reserve(rects.size());
    for (const Terminal_render_rect& rect : rects) {
        Terminal_render_rect local_rect = row_local_rect_primitive(rect, row_top);
        if (!local_rects.empty()) {
            Terminal_render_rect& previous      = local_rects.back();
            const QRectF&         previous_rect = previous.rect;
            const QRectF&         current_rect  = local_rect.rect;
            const bool            can_coalesce  =
                !previous.antialias                                                &&
                !local_rect.antialias                                              &&
                previous.color.rgba() == local_rect.color.rgba()                   &&
                previous_rect.isValid()                                            &&
                current_rect.isValid()                                             &&
                previous_rect.top() == current_rect.top()                          &&
                previous_rect.height() == current_rect.height()                    &&
                previous_rect.left() + previous_rect.width() == current_rect.left();
            if (can_coalesce) {
                previous.rect.setWidth(previous_rect.width() + current_rect.width());
                continue;
            }
        }

        local_rects.push_back(std::move(local_rect));
    }
    return local_rects;
}

void set_rect_wrapper_row_top(QSGTransformNode& wrapper, qreal row_top)
{
    VNM_TERMINAL_PROFILE_SCOPE("set_rect_wrapper_row_top");

    QMatrix4x4 matrix;
    matrix.translate(0.0F, static_cast<float>(row_top));
    wrapper.setMatrix(matrix);
}

Terminal_rect_resource_node* make_rect_wrapper_node(
    qreal                  row_top,
    const std::shared_ptr<Terminal_renderer_lifecycle_recorder>&
                           lifecycle_recorder)
{
    auto* wrapper = new Terminal_rect_resource_node(lifecycle_recorder);
    wrapper->setFlag(QSGNode::OwnedByParent, true);
    set_rect_wrapper_row_top(*wrapper, row_top);
    return wrapper;
}

struct rect_layer_groups_t
{
    std::vector<Terminal_render_rect>  frame_rects;
    std::vector<row_rect_group_t>      row_groups;
    bool                               needs_flat_fallback = false;
};

rect_layer_groups_t rect_layer_groups(
    const Terminal_render_frame&               frame,
    const std::vector<Terminal_render_rect>&   rects,
    bool                                       full_viewport_rect_is_frame)
{
    VNM_TERMINAL_PROFILE_SCOPE("rect_layer_groups");

    constexpr std::size_t k_missing_row_group_index =
        std::numeric_limits<std::size_t>::max();

    rect_layer_groups_t groups;
    groups.row_groups.reserve(std::min<std::size_t>(
        rects.size(),
        static_cast<std::size_t>(std::max(frame.grid_size.rows, 0))));

    std::vector<std::size_t> row_index_by_viewport_row(
        static_cast<std::size_t>(std::max(frame.grid_size.rows, 0)),
        k_missing_row_group_index);
    std::vector<bool> closed_viewport_rows(row_index_by_viewport_row.size(), false);
    std::optional<int> active_viewport_row;
    const auto close_active_row = [&]() {
        if (!active_viewport_row.has_value()) {
            return;
        }

        closed_viewport_rows[static_cast<std::size_t>(*active_viewport_row)] = true;
        active_viewport_row.reset();
    };

    bool saw_row_rect = false;
    for (const Terminal_render_rect& rect : rects) {
        const std::optional<int> viewport_row =
            row_for_rect(frame, rect, full_viewport_rect_is_frame);
        if (!viewport_row.has_value()) {
            if (saw_row_rect) {
                groups.needs_flat_fallback = true;
            }
            groups.frame_rects.push_back(rect);
            close_active_row();
            continue;
        }

        saw_row_rect = true;
        if (!active_viewport_row.has_value() ||
            *active_viewport_row != *viewport_row)
        {
            close_active_row();
            if (closed_viewport_rows[static_cast<std::size_t>(*viewport_row)]) {
                groups.needs_flat_fallback = true;
            }
            active_viewport_row = *viewport_row;
        }

        std::size_t& group_index =
            row_index_by_viewport_row[static_cast<std::size_t>(*viewport_row)];
        if (group_index == k_missing_row_group_index) {
            group_index = groups.row_groups.size();
            groups.row_groups.push_back({
                .identity     = {
                    frame.viewport.active_buffer,
                    logical_row_for_viewport_row(frame.viewport, *viewport_row),
                },
                .viewport_row = *viewport_row,
                .row_top      = row_top_for_viewport_row(*viewport_row, frame.cell_metrics),
                .rects        = {},
            });
        }

        row_rect_group_t& group = groups.row_groups[group_index];
        group.rects.push_back(rect);
    }

    return groups;
}

Terminal_render_arc row_local_arc_primitive(
    const Terminal_render_arc& arc,
    qreal                      row_top)
{
    Terminal_render_arc local_arc = arc;
    local_arc.rect = row_local_rect(arc.rect, row_top);
    return local_arc;
}

std::vector<Terminal_render_arc> row_local_arcs(
    const std::vector<Terminal_render_arc>&    arcs,
    qreal                                      row_top)
{
    VNM_TERMINAL_PROFILE_SCOPE("row_local_arcs");

    std::vector<Terminal_render_arc> local_arcs;
    local_arcs.reserve(arcs.size());
    for (const Terminal_render_arc& arc : arcs) {
        local_arcs.push_back(row_local_arc_primitive(arc, row_top));
    }
    return local_arcs;
}

struct arc_layer_groups_t
{
    std::vector<Terminal_render_arc>   frame_arcs;
    std::vector<row_arc_group_t>       row_groups;
    bool                               needs_flat_fallback = false;
};

arc_layer_groups_t arc_layer_groups(
    const Terminal_render_frame&               frame,
    const std::vector<Terminal_render_arc>&    arcs)
{
    VNM_TERMINAL_PROFILE_SCOPE("arc_layer_groups");

    constexpr std::size_t k_missing_row_group_index =
        std::numeric_limits<std::size_t>::max();

    arc_layer_groups_t groups;
    groups.row_groups.reserve(std::min<std::size_t>(
        arcs.size(),
        static_cast<std::size_t>(std::max(frame.grid_size.rows, 0))));

    std::vector<std::size_t> row_index_by_viewport_row(
        static_cast<std::size_t>(std::max(frame.grid_size.rows, 0)),
        k_missing_row_group_index);
    std::vector<bool> closed_viewport_rows(row_index_by_viewport_row.size(), false);
    std::optional<int> active_viewport_row;
    const auto close_active_row = [&]() {
        if (!active_viewport_row.has_value()) {
            return;
        }

        closed_viewport_rows[static_cast<std::size_t>(*active_viewport_row)] = true;
        active_viewport_row.reset();
    };

    bool saw_row_arc = false;
    for (const Terminal_render_arc& arc : arcs) {
        const std::optional<int> viewport_row = row_for_arc(frame, arc);
        if (!viewport_row.has_value()) {
            if (saw_row_arc) {
                groups.needs_flat_fallback = true;
            }
            groups.frame_arcs.push_back(arc);
            close_active_row();
            continue;
        }

        saw_row_arc = true;
        if (!active_viewport_row.has_value() ||
            *active_viewport_row != *viewport_row)
        {
            close_active_row();
            if (closed_viewport_rows[static_cast<std::size_t>(*viewport_row)]) {
                groups.needs_flat_fallback = true;
            }
            active_viewport_row = *viewport_row;
        }

        std::size_t& group_index =
            row_index_by_viewport_row[static_cast<std::size_t>(*viewport_row)];
        if (group_index == k_missing_row_group_index) {
            group_index = groups.row_groups.size();
            groups.row_groups.push_back({
                .identity     = {
                    frame.viewport.active_buffer,
                    logical_row_for_viewport_row(frame.viewport, *viewport_row),
                },
                .viewport_row = *viewport_row,
                .row_top      = row_top_for_viewport_row(*viewport_row, frame.cell_metrics),
                .arcs         = {},
            });
        }

        row_arc_group_t& group = groups.row_groups[group_index];
        group.arcs.push_back(arc);
    }

    return groups;
}

bool sync_frame_rect_group(
    QSGNode&                                   frame_layer,
    QByteArray&                                cached_key,
    const QByteArray&                          key,
    const std::vector<Terminal_render_rect>&   rects,
    bool                                       use_software_graphic_fallback,
    qreal                                      device_pixel_ratio,
    terminal_renderer_stats_t*                 stats = nullptr)
{
    if (cached_key == key) {
        return false;
    }

    if (stats != nullptr) {
        stats->qsg_nodes_destroyed += node_subtree_child_count(frame_layer);
    }
    clear_layer(frame_layer);
    append_graphic_nodes(
        &frame_layer,
        rects,
        {},
        use_software_graphic_fallback,
        device_pixel_ratio);
    if (stats != nullptr) {
        stats->qsg_nodes_created += node_subtree_child_count(frame_layer);
    }
    cached_key = key;
    return true;
}

bool sync_frame_rect_group(
    QSGNode&                                   frame_layer,
    QByteArray&                                cached_key,
    const std::vector<Terminal_render_rect>&   rects,
    bool                                       use_software_graphic_fallback,
    qreal                                      device_pixel_ratio,
    terminal_renderer_stats_t*                 stats = nullptr)
{
    return sync_frame_rect_group(
        frame_layer,
        cached_key,
        rect_layer_key(rects, {}, use_software_graphic_fallback, device_pixel_ratio),
        rects,
        use_software_graphic_fallback,
        device_pixel_ratio,
        stats);
}

bool sync_frame_arc_group(
    QSGNode&                                   frame_layer,
    QByteArray&                                cached_key,
    const QByteArray&                          key,
    const std::vector<Terminal_render_arc>&    arcs,
    bool                                       use_software_graphic_fallback,
    qreal                                      device_pixel_ratio,
    terminal_renderer_stats_t*                 stats = nullptr)
{
    if (cached_key == key) {
        return false;
    }

    if (stats != nullptr) {
        stats->qsg_nodes_destroyed += node_subtree_child_count(frame_layer);
    }
    clear_layer(frame_layer);
    append_arc_nodes(
        &frame_layer,
        arcs,
        use_software_graphic_fallback,
        device_pixel_ratio);
    if (stats != nullptr) {
        stats->qsg_nodes_created += node_subtree_child_count(frame_layer);
    }
    cached_key = key;
    return true;
}

bool sync_frame_arc_group(
    QSGNode&                                   frame_layer,
    QByteArray&                                cached_key,
    const std::vector<Terminal_render_arc>&    arcs,
    bool                                       use_software_graphic_fallback,
    qreal                                      device_pixel_ratio,
    terminal_renderer_stats_t*                 stats = nullptr)
{
    return sync_frame_arc_group(
        frame_layer,
        cached_key,
        rect_layer_key({}, arcs, use_software_graphic_fallback, device_pixel_ratio),
        arcs,
        use_software_graphic_fallback,
        device_pixel_ratio,
        stats);
}

void rebuild_rect_row_wrapper(
    QSGTransformNode&                          wrapper,
    const std::vector<Terminal_render_rect>&   local_rects,
    bool                                       use_software_graphic_fallback,
    qreal                                      device_pixel_ratio)
{
    clear_layer(wrapper);
    append_rect_nodes(
        &wrapper,
        local_rects,
        use_software_graphic_fallback,
        device_pixel_ratio);
}

void rebuild_arc_row_wrapper(
    QSGTransformNode&                          wrapper,
    const std::vector<Terminal_render_arc>&    local_arcs,
    bool                                       use_software_graphic_fallback,
    qreal                                      device_pixel_ratio)
{
    clear_layer(wrapper);
    append_arc_nodes(
        &wrapper,
        local_arcs,
        use_software_graphic_fallback,
        device_pixel_ratio);
}

void append_geometry_row_identity_key(
    QByteArray&                                key,
    const row_cache_identity_t&                identity)
{
    append_cache_key_value(key, identity.active_buffer);
    append_cache_key_value(key, identity.logical_row);
}

template <typename Row_group>
QByteArray geometry_row_layer_descriptor(
    const QByteArray&              content_key,
    const std::vector<Row_group>&  row_groups)
{
    QByteArray key = content_key;
    append_cache_key_size(key, row_groups.size());
    for (const Row_group& group : row_groups) {
        append_geometry_row_identity_key(key, group.identity);
        append_cache_key_value(key, group.viewport_row);
    }
    return key;
}

bool sync_geometry_wrapper_row_top(
    Terminal_scene_node::Geometry_row_cache_slot&
                           slot,
    qreal                  row_top)
{
    if (slot.row_top == row_top) {
        return false;
    }

    set_rect_wrapper_row_top(*slot.wrapper, row_top);
    slot.row_top = row_top;
    return true;
}

void destroy_geometry_row_cache_slot(
    Terminal_scene_node::Geometry_row_cache_slot& slot)
{
    VNM_TERMINAL_PROFILE_SCOPE("destroy_geometry_row_cache_slot");

    if (slot.wrapper->parent() != nullptr) {
        slot.wrapper->parent()->removeChildNode(slot.wrapper);
    }
    delete_node_tree(slot.wrapper);
}

struct Geometry_row_slot_transfer
{
    std::vector<Terminal_scene_node::Geometry_row_cache_slot>  cache_slots;
    std::map<row_cache_identity_t, std::size_t>                index_by_identity;
    std::vector<bool>                                          consumed_slots;
};

Geometry_row_slot_transfer take_geometry_row_slots(
    std::vector<Terminal_scene_node::Geometry_row_cache_slot>& row_slots)
{
    Geometry_row_slot_transfer transfer;
    transfer.cache_slots = std::move(row_slots);
    transfer.index_by_identity.clear();
    transfer.consumed_slots.assign(transfer.cache_slots.size(), false);
    for (std::size_t index = 0; index < transfer.cache_slots.size(); ++index) {
        const auto inserted =
            transfer.index_by_identity.emplace(transfer.cache_slots[index].identity, index);
        Q_ASSERT(inserted.second);
    }
    return transfer;
}

std::optional<Terminal_scene_node::Geometry_row_cache_slot> take_geometry_row_slot(
    Geometry_row_slot_transfer&    transfer,
    const row_cache_identity_t&    identity)
{
    const auto index = transfer.index_by_identity.find(identity);
    if (index == transfer.index_by_identity.end()) {
        return std::nullopt;
    }

    Q_ASSERT(!transfer.consumed_slots[index->second]);
    transfer.consumed_slots[index->second] = true;
    return std::move(transfer.cache_slots[index->second]);
}

void append_geometry_row_slot_in_order(
    QSGNode&               row_layer,
    Terminal_scene_node::Geometry_row_cache_slot&
                           slot)
{
    if (slot.wrapper->parent() != nullptr) {
        slot.wrapper->parent()->removeChildNode(slot.wrapper);
    }
    row_layer.appendChildNode(slot.wrapper);
}

int destroy_unconsumed_geometry_row_slots(
    Geometry_row_slot_transfer&    transfer,
    terminal_renderer_stats_t*     stats = nullptr,
    int*                           out_qsg_nodes_destroyed = nullptr)
{
    int rows_removed = 0;
    for (std::size_t index = 0; index < transfer.cache_slots.size(); ++index) {
        if (transfer.consumed_slots[index]) {
            continue;
        }

        Terminal_scene_node::Geometry_row_cache_slot& slot = transfer.cache_slots[index];
        const int destroyed_nodes = 1 + node_subtree_child_count(*slot.wrapper);
        if (stats != nullptr)                   { stats->qsg_nodes_destroyed += destroyed_nodes; }
        if (out_qsg_nodes_destroyed != nullptr) { *out_qsg_nodes_destroyed   += destroyed_nodes; }
        destroy_geometry_row_cache_slot(slot);
        ++rows_removed;
    }
    return rows_removed;
}

bool clear_geometry_row_slots(
    QSGNode&                   row_layer,
    std::vector<Terminal_scene_node::Geometry_row_cache_slot>&
                               row_slots,
    int&                       out_rows_removed,
    terminal_renderer_stats_t* stats = nullptr,
    int*                       out_qsg_nodes_destroyed = nullptr)
{
    const bool had_row_nodes = !row_slots.empty() ||
        row_layer.firstChild() != nullptr;

    for (Terminal_scene_node::Geometry_row_cache_slot& slot : row_slots) {
        const int destroyed_nodes = 1 + node_subtree_child_count(*slot.wrapper);
        if (stats != nullptr)                   { stats->qsg_nodes_destroyed += destroyed_nodes; }
        if (out_qsg_nodes_destroyed != nullptr) { *out_qsg_nodes_destroyed   += destroyed_nodes; }
        destroy_geometry_row_cache_slot(slot);
        ++out_rows_removed;
    }

    row_slots.clear();
    clear_layer(row_layer);
    return had_row_nodes;
}

enum class Batched_rect_layer_kind
{
    BACKGROUND,
    SELECTION,
    GRAPHIC,
    DECORATION,
};

struct batched_rect_row_layer_config_t
{
    Batched_rect_layer_kind            layer_kind                  = Batched_rect_layer_kind::SELECTION;
    const char*                        sync_scope_name             = "";
    const char*                        update_scope_name           = "";
    bool                               full_viewport_rect_is_frame = false;
    int terminal_renderer_stats_t::*   rows_rebuilt                = nullptr;
    int terminal_renderer_stats_t::*   rows_reused                 = nullptr;
    int terminal_renderer_stats_t::*   clean_reuse_skips           = nullptr;
    int terminal_renderer_stats_t::*   rows_removed                = nullptr;
    int terminal_renderer_stats_t::*   cache_fallbacks             = nullptr;
    int terminal_renderer_stats_t::*   batched_rects               = nullptr;
    int terminal_renderer_stats_t::*   batched_vertices            = nullptr;
    int terminal_renderer_stats_t::*   row_rects_before_coalescing = nullptr;
    int terminal_renderer_stats_t::*   row_rects_after_coalescing  = nullptr;
};

int& renderer_stat(
    terminal_renderer_stats_t&         stats,
    int terminal_renderer_stats_t::*   member)
{
    Q_ASSERT(member != nullptr);
    return stats.*member;
}

void add_optional_renderer_stat(
    terminal_renderer_stats_t&         stats,
    int terminal_renderer_stats_t::*   member,
    int                                value)
{
    if (member != nullptr) {
        stats.*member += value;
    }
}

void record_batched_rect_qsg_nodes_created(
    terminal_renderer_stats_t&         stats,
    Batched_rect_layer_kind            layer_kind,
    int                                count)
{
    stats.qsg_nodes_created += count;
    if (layer_kind == Batched_rect_layer_kind::BACKGROUND) {
        stats.background_qsg_nodes_created += count;
    }
}

void record_batched_rect_qsg_nodes_replaced(
    terminal_renderer_stats_t&         stats,
    Batched_rect_layer_kind            layer_kind,
    int                                count)
{
    stats.qsg_nodes_replaced += count;
    if (layer_kind == Batched_rect_layer_kind::BACKGROUND) {
        stats.background_qsg_nodes_replaced += count;
    }
}

void record_batched_rect_qsg_nodes_destroyed(
    terminal_renderer_stats_t&         stats,
    Batched_rect_layer_kind            layer_kind,
    int                                count)
{
    stats.qsg_nodes_destroyed += count;
    if (layer_kind == Batched_rect_layer_kind::BACKGROUND) {
        stats.background_qsg_nodes_destroyed += count;
    }
}

QSGGeometryNode* batched_rect_geometry_node(QSGTransformNode& wrapper)
{
    QSGNode* child = wrapper.firstChild();
    if (child == nullptr || child->nextSibling() != nullptr) {
        return nullptr;
    }

    auto* geometry_node = dynamic_cast<QSGGeometryNode*>(child);
    if (geometry_node                                                    == nullptr ||
        geometry_node->geometry()                                        == nullptr ||
        dynamic_cast<QSGVertexColorMaterial*>(geometry_node->material()) == nullptr)
    {
        return nullptr;
    }

    return geometry_node;
}

void append_batched_rect_geometry(
    QSGTransformNode&                          wrapper,
    const std::vector<Terminal_render_rect>&   local_rects,
    const batched_rect_row_layer_config_t&     config,
    terminal_renderer_stats_t&                 stats)
{
    VNM_TERMINAL_PROFILE_SCOPE(config.update_scope_name);

    wrapper.appendChildNode(make_flat_rect_batch_node(local_rects));
    record_batched_rect_qsg_nodes_created(stats, config.layer_kind, 1);
}

void rebuild_batched_rect_geometry(
    QSGTransformNode&                          wrapper,
    const std::vector<Terminal_render_rect>&   local_rects,
    const batched_rect_row_layer_config_t&     config,
    terminal_renderer_stats_t&                 stats)
{
    VNM_TERMINAL_PROFILE_SCOPE(config.update_scope_name);

    if (QSGGeometryNode* geometry_node = batched_rect_geometry_node(wrapper)) {
        update_flat_rect_batch_node(*geometry_node, local_rects);
        return;
    }

    const int destroyed_nodes = node_subtree_child_count(wrapper);
    if (destroyed_nodes > 0) {
        record_batched_rect_qsg_nodes_replaced(stats, config.layer_kind, 1);
        record_batched_rect_qsg_nodes_destroyed(stats, config.layer_kind, destroyed_nodes);
        clear_layer(wrapper);
    }
    wrapper.appendChildNode(make_flat_rect_batch_node(local_rects));
    record_batched_rect_qsg_nodes_created(stats, config.layer_kind, 1);
}

bool sync_batched_rect_row_layer(
    QSGNode&                   frame_layer,
    QSGNode&                   row_layer,
    QByteArray&                cached_layer_descriptor,
    const QByteArray&          current_layer_content_key,
    QByteArray&                frame_key,
    std::vector<Terminal_scene_node::Geometry_row_cache_slot>&
                               row_slots,
    const Terminal_render_frame&
                               frame,
    const std::vector<Terminal_render_rect>&
                               rects,
    bool                       use_software_graphic_fallback,
    qreal                      device_pixel_ratio,
    const std::shared_ptr<Terminal_renderer_lifecycle_recorder>&
                               lifecycle_recorder,
    const batched_rect_row_layer_config_t&
                               config,
    terminal_renderer_stats_t& stats,
    const std::vector<Terminal_render_rect>*
                               extra_frame_rects = nullptr)
{
    VNM_TERMINAL_PROFILE_SCOPE(config.sync_scope_name);

    renderer_stat(stats, config.rows_rebuilt)      = 0;
    renderer_stat(stats, config.rows_reused)       = 0;
    renderer_stat(stats, config.clean_reuse_skips) = 0;
    renderer_stat(stats, config.rows_removed)      = 0;
    renderer_stat(stats, config.cache_fallbacks)   = 0;

    const rect_layer_groups_t groups =
        rect_layer_groups(frame, rects, config.full_viewport_rect_is_frame);
    const QByteArray current_layer_descriptor = geometry_row_layer_descriptor(
        current_layer_content_key,
        groups.row_groups);
    record_rect_cache_key_build(stats, current_layer_descriptor);
    if (cached_layer_descriptor == current_layer_descriptor) {
        ++renderer_stat(stats, config.clean_reuse_skips);
        return false;
    }

    const std::vector<Terminal_render_rect>& frame_rects =
        extra_frame_rects != nullptr ? *extra_frame_rects :
                                       empty_terminal_render_rects();
    const bool needs_flat_fallback =
        groups.needs_flat_fallback ||
        has_antialiased_rects(rects);
    if (needs_flat_fallback) {
        ++renderer_stat(stats, config.cache_fallbacks);
        const std::vector<Terminal_render_rect> fallback_rects =
            rects_for_frame_group(frame_rects, rects);
        bool changed = sync_frame_rect_group(
            frame_layer,
            frame_key,
            current_layer_content_key,
            fallback_rects,
            use_software_graphic_fallback,
            device_pixel_ratio,
            &stats);
        int* layer_destroyed_counter =
            config.layer_kind == Batched_rect_layer_kind::BACKGROUND
                ? &stats.background_qsg_nodes_destroyed
                : nullptr;
        if (clear_geometry_row_slots(
                row_layer, row_slots, renderer_stat(stats, config.rows_removed), &stats, layer_destroyed_counter))
        {
            changed = true;
        }
        cached_layer_descriptor = current_layer_descriptor;
        return changed;
    }

    const std::vector<Terminal_render_rect> frame_group_rects =
        rects_for_frame_group(frame_rects, groups.frame_rects);
    const QByteArray frame_group_key = rect_layer_key(
        frame_group_rects,
        {},
        use_software_graphic_fallback,
        device_pixel_ratio);
    record_rect_cache_key_build(stats, frame_group_key);
    bool changed = sync_frame_rect_group(
        frame_layer,
        frame_key,
        frame_group_key,
        frame_group_rects,
        use_software_graphic_fallback,
        device_pixel_ratio,
        &stats);

    Geometry_row_slot_transfer old_slots = take_geometry_row_slots(row_slots);
    std::vector<Terminal_scene_node::Geometry_row_cache_slot> new_slots;
    new_slots.reserve(groups.row_groups.size());
    for (const row_rect_group_t& group : groups.row_groups) {
        const std::vector<Terminal_render_rect> local_rects =
            row_local_rects(group.rects, group.row_top);
        stats.rect_resource_rects_before_coalescing +=
            static_cast<int>(group.rects.size());
        stats.rect_resource_rects_after_coalescing +=
            static_cast<int>(local_rects.size());
        add_optional_renderer_stat(
            stats,
            config.row_rects_before_coalescing,
            static_cast<int>(group.rects.size()));
        add_optional_renderer_stat(
            stats,
            config.row_rects_after_coalescing,
            static_cast<int>(local_rects.size()));
        if (!use_software_graphic_fallback) {
            renderer_stat(stats, config.batched_rects) +=
                static_cast<int>(local_rects.size());
            renderer_stat(stats, config.batched_vertices) +=
                flat_rect_vertex_count(local_rects.size());
        }

        const QByteArray key = flat_rect_layer_key(
            local_rects,
            use_software_graphic_fallback,
            device_pixel_ratio);
        record_rect_cache_key_build(stats, key);
        std::optional<Terminal_scene_node::Geometry_row_cache_slot> old_slot = take_geometry_row_slot(
            old_slots,
            group.identity);
        if (old_slot.has_value() &&
            old_slot->key == key)
        {
            if (sync_geometry_wrapper_row_top(*old_slot, group.row_top)) {
                changed = true;
            }
            ++renderer_stat(stats, config.rows_reused);
            append_geometry_row_slot_in_order(row_layer, *old_slot);
            new_slots.push_back(std::move(*old_slot));
            continue;
        }

        Terminal_scene_node::Geometry_row_cache_slot new_slot;
        if (old_slot.has_value()) {
            new_slot = std::move(*old_slot);
            if (use_software_graphic_fallback) {
                record_batched_rect_qsg_nodes_replaced(stats, config.layer_kind, 1);
                record_batched_rect_qsg_nodes_destroyed(
                    stats,
                    config.layer_kind,
                    node_subtree_child_count(*new_slot.wrapper));
                rebuild_rect_row_wrapper(
                    *new_slot.wrapper,
                    local_rects,
                    use_software_graphic_fallback,
                    device_pixel_ratio);
                record_batched_rect_qsg_nodes_created(
                    stats,
                    config.layer_kind,
                    node_subtree_child_count(*new_slot.wrapper));
            }
            else {
                rebuild_batched_rect_geometry(
                    *new_slot.wrapper,
                    local_rects,
                    config,
                    stats);
            }
            sync_geometry_wrapper_row_top(new_slot, group.row_top);
            new_slot.key = key;
        }
        else {
            Terminal_rect_resource_node* wrapper =
                make_rect_wrapper_node(group.row_top, lifecycle_recorder);
            if (use_software_graphic_fallback) {
                append_rect_nodes(
                    wrapper,
                    local_rects,
                    use_software_graphic_fallback,
                    device_pixel_ratio);
                record_batched_rect_qsg_nodes_created(
                    stats,
                    config.layer_kind,
                    1 + node_subtree_child_count(*wrapper));
            }
            else {
                record_batched_rect_qsg_nodes_created(stats, config.layer_kind, 1);
                append_batched_rect_geometry(*wrapper, local_rects, config, stats);
            }
            new_slot = {
                .identity = group.identity,
                .wrapper  = wrapper,
                .key      = key,
                .row_top  = group.row_top,
            };
        }
        append_geometry_row_slot_in_order(row_layer, new_slot);
        new_slots.push_back(std::move(new_slot));
        ++renderer_stat(stats, config.rows_rebuilt);
        changed = true;
    }

    int* layer_destroyed_counter =
        config.layer_kind == Batched_rect_layer_kind::BACKGROUND
            ? &stats.background_qsg_nodes_destroyed
            : nullptr;
    const int removed_slots =
        destroy_unconsumed_geometry_row_slots(old_slots, &stats, layer_destroyed_counter);
    if (removed_slots > 0) {
        renderer_stat(stats, config.rows_removed) += removed_slots;
        changed = true;
    }

    row_slots = std::move(new_slots);
    cached_layer_descriptor = current_layer_descriptor;
    return changed;
}

const batched_rect_row_layer_config_t k_background_batched_rect_row_layer = {
    .layer_kind                    = Batched_rect_layer_kind::BACKGROUND,
    .sync_scope_name               = "sync_background_rect_row_layer",
    .update_scope_name             = "update_background_batched_rect_geometry",
    .full_viewport_rect_is_frame   = true,
    .rows_rebuilt                  = &terminal_renderer_stats_t::background_rows_rebuilt,
    .rows_reused                   = &terminal_renderer_stats_t::background_rows_reused,
    .clean_reuse_skips             = &terminal_renderer_stats_t::background_row_clean_reuse_skips,
    .rows_removed                  = &terminal_renderer_stats_t::background_rows_removed,
    .cache_fallbacks               = &terminal_renderer_stats_t::background_row_cache_fallbacks,
    .batched_rects                 = &terminal_renderer_stats_t::background_batched_rects,
    .batched_vertices              = &terminal_renderer_stats_t::background_batched_vertices,
    .row_rects_before_coalescing   =
        &terminal_renderer_stats_t::background_row_rects_before_coalescing,
    .row_rects_after_coalescing    =
        &terminal_renderer_stats_t::background_row_rects_after_coalescing,
};

const batched_rect_row_layer_config_t k_selection_batched_rect_row_layer = {
    .layer_kind                  = Batched_rect_layer_kind::SELECTION,
    .sync_scope_name             = "sync_selection_rect_row_layer",
    .update_scope_name           = "update_selection_batched_rect_geometry",
    .full_viewport_rect_is_frame = false,
    .rows_rebuilt                = &terminal_renderer_stats_t::selection_rows_rebuilt,
    .rows_reused                 = &terminal_renderer_stats_t::selection_rows_reused,
    .clean_reuse_skips           = &terminal_renderer_stats_t::selection_row_clean_reuse_skips,
    .rows_removed                = &terminal_renderer_stats_t::selection_rows_removed,
    .cache_fallbacks             = &terminal_renderer_stats_t::selection_row_cache_fallbacks,
    .batched_rects               = &terminal_renderer_stats_t::selection_batched_rects,
    .batched_vertices            = &terminal_renderer_stats_t::selection_batched_vertices,
};

const batched_rect_row_layer_config_t k_graphic_batched_rect_row_layer = {
    .layer_kind                  = Batched_rect_layer_kind::GRAPHIC,
    .sync_scope_name             = "sync_graphic_rect_row_layer",
    .update_scope_name           = "update_graphic_batched_rect_geometry",
    .full_viewport_rect_is_frame = false,
    .rows_rebuilt                = &terminal_renderer_stats_t::graphic_rect_rows_rebuilt,
    .rows_reused                 = &terminal_renderer_stats_t::graphic_rect_rows_reused,
    .clean_reuse_skips           = &terminal_renderer_stats_t::graphic_rect_row_clean_reuse_skips,
    .rows_removed                = &terminal_renderer_stats_t::graphic_rect_rows_removed,
    .cache_fallbacks             = &terminal_renderer_stats_t::graphic_rect_row_cache_fallbacks,
    .batched_rects               = &terminal_renderer_stats_t::graphic_batched_rects,
    .batched_vertices            = &terminal_renderer_stats_t::graphic_batched_vertices,
};

const batched_rect_row_layer_config_t k_decoration_batched_rect_row_layer = {
    .layer_kind                  = Batched_rect_layer_kind::DECORATION,
    .sync_scope_name             = "sync_decoration_rect_row_layer",
    .update_scope_name           = "update_decoration_batched_rect_geometry",
    .full_viewport_rect_is_frame = false,
    .rows_rebuilt                = &terminal_renderer_stats_t::decoration_rows_rebuilt,
    .rows_reused                 = &terminal_renderer_stats_t::decoration_rows_reused,
    .clean_reuse_skips           = &terminal_renderer_stats_t::decoration_row_clean_reuse_skips,
    .rows_removed                = &terminal_renderer_stats_t::decoration_rows_removed,
    .cache_fallbacks             = &terminal_renderer_stats_t::decoration_row_cache_fallbacks,
    .batched_rects               = &terminal_renderer_stats_t::decoration_batched_rects,
    .batched_vertices            = &terminal_renderer_stats_t::decoration_batched_vertices,
};

bool sync_arc_row_layer(
    QSGNode&                   frame_layer,
    QSGNode&                   row_layer,
    QByteArray&                cached_layer_descriptor,
    const QByteArray&          current_layer_content_key,
    QByteArray&                frame_key,
    std::vector<Terminal_scene_node::Geometry_row_cache_slot>&
                               row_slots,
    const Terminal_render_frame&
                               frame,
    const std::vector<Terminal_render_arc>&
                               arcs,
    bool                       use_software_graphic_fallback,
    qreal                      device_pixel_ratio,
    const std::shared_ptr<Terminal_renderer_lifecycle_recorder>&
                               lifecycle_recorder,
    terminal_renderer_stats_t& stats,
    int&                       out_rows_rebuilt,
    int&                       out_rows_reused,
    int&                       out_clean_reuse_skips,
    int&                       out_rows_removed,
    int&                       out_cache_fallbacks)
{
    VNM_TERMINAL_PROFILE_SCOPE("sync_arc_row_layer");

    out_rows_rebuilt      = 0;
    out_rows_reused       = 0;
    out_clean_reuse_skips = 0;
    out_rows_removed      = 0;
    out_cache_fallbacks   = 0;

    const arc_layer_groups_t groups = arc_layer_groups(frame, arcs);
    const QByteArray current_layer_descriptor = geometry_row_layer_descriptor(
        current_layer_content_key,
        groups.row_groups);
    record_rect_cache_key_build(stats, current_layer_descriptor);
    if (cached_layer_descriptor == current_layer_descriptor) {
        ++out_clean_reuse_skips;
        return false;
    }

    const bool needs_flat_fallback =
        groups.needs_flat_fallback ||
        (use_software_graphic_fallback && !arcs.empty());
    if (needs_flat_fallback) {
        ++out_cache_fallbacks;
        bool changed = sync_frame_arc_group(
            frame_layer,
            frame_key,
            current_layer_content_key,
            arcs,
            use_software_graphic_fallback,
            device_pixel_ratio,
            &stats);
        if (clear_geometry_row_slots(row_layer, row_slots, out_rows_removed, &stats)) {
            changed = true;
        }
        cached_layer_descriptor = current_layer_descriptor;
        return changed;
    }

    const QByteArray frame_group_key = rect_layer_key(
        {},
        groups.frame_arcs,
        use_software_graphic_fallback,
        device_pixel_ratio);
    record_rect_cache_key_build(stats, frame_group_key);
    bool changed = sync_frame_arc_group(
        frame_layer,
        frame_key,
        frame_group_key,
        groups.frame_arcs,
        use_software_graphic_fallback,
        device_pixel_ratio,
        &stats);

    Geometry_row_slot_transfer old_slots = take_geometry_row_slots(row_slots);
    std::vector<Terminal_scene_node::Geometry_row_cache_slot> new_slots;
    new_slots.reserve(groups.row_groups.size());
    for (const row_arc_group_t& group : groups.row_groups) {
        const std::vector<Terminal_render_arc> local_arcs =
            row_local_arcs(group.arcs, group.row_top);
        const QByteArray key = rect_layer_key(
            {},
            local_arcs,
            use_software_graphic_fallback,
            device_pixel_ratio);
        record_rect_cache_key_build(stats, key);
        std::optional<Terminal_scene_node::Geometry_row_cache_slot> old_slot = take_geometry_row_slot(
            old_slots,
            group.identity);
        if (old_slot.has_value() &&
            old_slot->key == key)
        {
            if (sync_geometry_wrapper_row_top(*old_slot, group.row_top)) {
                changed = true;
            }
            ++out_rows_reused;
            append_geometry_row_slot_in_order(row_layer, *old_slot);
            new_slots.push_back(std::move(*old_slot));
            continue;
        }

        Terminal_scene_node::Geometry_row_cache_slot new_slot;
        if (old_slot.has_value()) {
            new_slot = std::move(*old_slot);
            ++stats.qsg_nodes_replaced;
            stats.qsg_nodes_destroyed += node_subtree_child_count(*new_slot.wrapper);
            rebuild_arc_row_wrapper(
                *new_slot.wrapper,
                local_arcs,
                use_software_graphic_fallback,
                device_pixel_ratio);
            stats.qsg_nodes_created += node_subtree_child_count(*new_slot.wrapper);
            sync_geometry_wrapper_row_top(new_slot, group.row_top);
            new_slot.key = key;
        }
        else {
            Terminal_rect_resource_node* wrapper =
                make_rect_wrapper_node(group.row_top, lifecycle_recorder);
            append_arc_nodes(
                wrapper,
                local_arcs,
                use_software_graphic_fallback,
                device_pixel_ratio);
            stats.qsg_nodes_created += 1 + node_subtree_child_count(*wrapper);
            new_slot = {
                .identity = group.identity,
                .wrapper  = wrapper,
                .key      = key,
                .row_top  = group.row_top,
            };
        }
        append_geometry_row_slot_in_order(row_layer, new_slot);
        new_slots.push_back(std::move(new_slot));
        ++out_rows_rebuilt;
        changed = true;
    }

    const int removed_slots = destroy_unconsumed_geometry_row_slots(old_slots, &stats);
    if (removed_slots > 0) {
        out_rows_removed += removed_slots;
        changed = true;
    }

    row_slots = std::move(new_slots);
    cached_layer_descriptor = current_layer_descriptor;
    return changed;
}

bool sync_cursor_text_layer(
    Terminal_scene_node&       root,
    QQuickWindow&              window,
    const Terminal_render_frame&
                               frame,
    const QFont&               font,
    terminal_renderer_stats_t& stats,
    const std::shared_ptr<Terminal_renderer_lifecycle_recorder>&
                               lifecycle_recorder)
{
    if (frame.cursor_text_runs.empty()) {
        if (root.cursor_text_layer_key.isEmpty()) {
            return false;
        }

        stats.qsg_nodes_destroyed += node_subtree_child_count(*root.cursor_text_layer);
        clear_layer(*root.cursor_text_layer);
        root.cursor_text_layer_key.clear();
        return true;
    }

    const QString text_font_key = font_cache_key(font);
    const QByteArray key = text_resource_key(
        frame.cursor_text_runs,
        text_font_key,
        frame.logical_size);
    record_text_cache_key_build(stats, key);
    if (root.cursor_text_layer_key == key) {
        return false;
    }

    const QRectF frame_viewport(QPointF(0.0, 0.0), frame.logical_size);
    const std::map<int, std::vector<Terminal_render_text_run>> runs_by_row = text_runs_by_row(
        frame.cursor_text_runs,
        frame.grid_size.rows);
    bool failed = false;
    int replacement_text_leaf_nodes_created = 0;
    std::vector<Terminal_text_resource_node*> replacement_nodes;
    for (const auto& [row, row_runs] : runs_by_row) {
        (void)row;
        int text_leaf_nodes_created = 0;
        Terminal_text_resource_node* node = make_text_resource_node(
            window,
            row_runs,
            font,
            frame_viewport,
            lifecycle_recorder,
            true,
            stats,
            failed,
            text_leaf_nodes_created);
        if (node == nullptr) {
            break;
        }

        replacement_nodes.push_back(node);
        replacement_text_leaf_nodes_created += text_leaf_nodes_created;
    }

    if (failed) {
        for (Terminal_text_resource_node* node : replacement_nodes) {
            stats.qsg_nodes_destroyed += 1 + node_subtree_child_count(*node);
            delete_node_tree(node);
        }
        ++stats.text_content_failures;
        return false;
    }

    stats.qsg_nodes_destroyed += node_subtree_child_count(*root.cursor_text_layer);
    clear_layer(*root.cursor_text_layer);
    for (Terminal_text_resource_node* node : replacement_nodes) {
        root.cursor_text_layer->appendChildNode(node);
    }
    root.cursor_text_layer_key = key;
    stats.text_leaf_nodes_created += replacement_text_leaf_nodes_created;
    return true;
}

template<typename Primitive>
std::vector<Terminal_render_rect> primitive_rects(const std::vector<Primitive>& primitives)
{
    std::vector<Terminal_render_rect> rects;
    rects.reserve(primitives.size());
    for (const Primitive& primitive : primitives) {
        rects.push_back({primitive.rect, primitive.color});
    }
    return rects;
}

int display_width_for_text(const QString& text)
{
    return std::max(0, measure_utf8_width(text.toUtf8()).cells);
}

int display_width_for_prefix(const QString& text, int code_unit_count)
{
    const int clamped_count = std::clamp(code_unit_count, 0, static_cast<int>(text.size()));
    return display_width_for_text(text.left(clamped_count));
}

bool position_inside_grid(
    terminal_grid_position_t       position,
    terminal_grid_size_t           grid_size)
{
    return
        position.row    >= 0              &&
        position.row    <  grid_size.rows &&
        position.column >= 0              &&
        position.column <  grid_size.columns;
}

int logical_row_for_viewport_row(
    const Terminal_viewport_state& viewport,
    int                            row)
{
    if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        return row;
    }

    return viewport.scrollback_rows - viewport.offset_from_tail + row;
}

}

#if VNM_TERMINAL_PROFILING_ENABLED
void Terminal_text_layout_slow_diagnostics_recorder::reset()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_slow_call_count = 0U;
    m_samples.clear();
}

void Terminal_text_layout_slow_diagnostics_recorder::record_layout(
    std::uint64_t                      duration_ns,
    const QFont&                       font,
    const Terminal_render_text_run&    run,
    bool                               clipped,
    bool                               force_blended_order,
    bool                               ascii_layout_font,
    bool                               line_has_text)
{
    if (duration_ns < k_slow_text_layout_threshold_ns) {
        return;
    }

    terminal_text_layout_slow_diagnostic_t diagnostic = make_slow_text_layout_diagnostic(
        duration_ns,
        font,
        run,
        clipped,
        force_blended_order,
        ascii_layout_font,
        line_has_text);

    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_slow_call_count;
    if (m_samples.size() < k_max_slow_text_layout_samples) {
        m_samples.push_back(std::move(diagnostic));
    }
    else {
        const auto smallest = std::min_element(
            m_samples.begin(),
            m_samples.end(),
            [](const auto& left, const auto& right) {
                return left.duration_ns < right.duration_ns;
            });
        if (smallest               != m_samples.end() &&
            diagnostic.duration_ns >  smallest->duration_ns)
        {
            *smallest = std::move(diagnostic);
        }
    }

    std::sort(
        m_samples.begin(),
        m_samples.end(),
        [](const auto& left, const auto& right) {
            return left.duration_ns > right.duration_ns;
        });
}

terminal_text_layout_slow_diagnostics_t
Terminal_text_layout_slow_diagnostics_recorder::snapshot() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return {
        k_slow_text_layout_threshold_ns,
        m_slow_call_count,
        m_samples,
    };
}
#endif

namespace {

std::uint64_t count_from_bool(bool value)
{
    return value ? 1U : 0U;
}

void accumulate_simple_content_stats(
    terminal_simple_content_cumulative_stats_t&    total,
    const terminal_simple_content_stats_t&         stats);

void accumulate_frame_stats(
    terminal_render_frame_cumulative_stats_t&      total,
    const terminal_render_frame_stats_t&           stats)
{
    accumulate_simple_content_stats(total.simple_content, stats.simple_content);
    total.cells_considered      += static_cast<std::uint64_t>(stats.cells_considered);
    total.cells_skipped_invalid += static_cast<std::uint64_t>(stats.cells_skipped_invalid);
    total.cells_skipped_wide_continuation +=
        static_cast<std::uint64_t>(stats.cells_skipped_wide_continuation);
    total.cells_rendered   += static_cast<std::uint64_t>(stats.cells_rendered);
    total.text_cells_empty += static_cast<std::uint64_t>(stats.text_cells_empty);
    total.text_cells_rendered_as_text     +=
        static_cast<std::uint64_t>(stats.text_cells_rendered_as_text);
    total.text_cells_rendered_as_graphic  +=
        static_cast<std::uint64_t>(stats.text_cells_rendered_as_graphic);
    total.text_cells_printable_ascii      +=
        static_cast<std::uint64_t>(stats.text_cells_printable_ascii);
    total.text_cells_other_ascii          +=
        static_cast<std::uint64_t>(stats.text_cells_other_ascii);
    total.text_cells_non_ascii            +=
        static_cast<std::uint64_t>(stats.text_cells_non_ascii);
    total.text_cells_simple_ascii         +=
        static_cast<std::uint64_t>(stats.text_cells_simple_ascii);
    total.text_cells_single_width         +=
        static_cast<std::uint64_t>(stats.text_cells_single_width);
    total.text_cells_multi_width          +=
        static_cast<std::uint64_t>(stats.text_cells_multi_width);
    total.text_cells_with_decorations     +=
        static_cast<std::uint64_t>(stats.text_cells_with_decorations);
    total.text_cells_with_hyperlink       +=
        static_cast<std::uint64_t>(stats.text_cells_with_hyperlink);
    total.text_style_changes   += static_cast<std::uint64_t>(stats.text_style_changes);
    total.text_distinct_styles += static_cast<std::uint64_t>(stats.text_distinct_styles);
    total.background_rects_emitted        +=
        static_cast<std::uint64_t>(stats.background_rects_emitted);
    total.selection_rects_emitted         +=
        static_cast<std::uint64_t>(stats.selection_rects_emitted);
    total.graphic_rects_emitted           +=
        static_cast<std::uint64_t>(stats.graphic_rects_emitted);
    total.graphic_arcs_emitted            +=
        static_cast<std::uint64_t>(stats.graphic_arcs_emitted);
    total.text_runs_emitted               += static_cast<std::uint64_t>(stats.text_runs_emitted);
    total.cursor_text_runs_emitted        +=
        static_cast<std::uint64_t>(stats.cursor_text_runs_emitted);
    total.decoration_rects_emitted        +=
        static_cast<std::uint64_t>(stats.decoration_rects_emitted);
    total.cursor_rects_emitted            += static_cast<std::uint64_t>(stats.cursor_rects_emitted);
    total.cursor_graphic_rects_emitted    +=
        static_cast<std::uint64_t>(stats.cursor_graphic_rects_emitted);
    total.cursor_graphic_arcs_emitted     +=
        static_cast<std::uint64_t>(stats.cursor_graphic_arcs_emitted);
    total.overlay_rects_emitted += static_cast<std::uint64_t>(stats.overlay_rects_emitted);
    total.packed_rows           += static_cast<std::uint64_t>(stats.packed_rows);
    total.packed_text_spans     += static_cast<std::uint64_t>(stats.packed_text_spans);
    total.packed_text_cells     += static_cast<std::uint64_t>(stats.packed_text_cells);
    total.packed_graphic_spans            +=
        static_cast<std::uint64_t>(stats.packed_graphic_spans);
    total.packed_graphic_cells            +=
        static_cast<std::uint64_t>(stats.packed_graphic_cells);
    total.packed_payload_bytes            += stats.packed_payload_bytes;
}

void accumulate_renderer_stats(
    terminal_renderer_cumulative_stats_t&  total,
    const terminal_renderer_stats_t&       stats)
{
    ++total.frames_published;
    total.paint_completed_frames += count_from_bool(stats.paint_completed);
    total.root_reused_frames     += count_from_bool(stats.root_reused);
    accumulate_frame_stats(total.frame, stats.frame);
    total.text_content_rebuilds   += static_cast<std::uint64_t>(stats.text_content_rebuilds);
    total.text_content_reused     += static_cast<std::uint64_t>(stats.text_content_reused);
    total.text_content_removed    += static_cast<std::uint64_t>(stats.text_content_removed);
    total.text_content_failures   += static_cast<std::uint64_t>(stats.text_content_failures);
    total.text_leaf_nodes_created += static_cast<std::uint64_t>(stats.text_leaf_nodes_created);
    total.text_cache_entry_child_nodes_cleared_for_replacement += static_cast<std::uint64_t>(
        stats.text_cache_entry_child_nodes_cleared_for_replacement);
    total.text_cache_entry_child_nodes_cleared_for_removal += static_cast<std::uint64_t>(
        stats.text_cache_entry_child_nodes_cleared_for_removal);
    total.text_cache_entry_max_child_nodes_cleared  = std::max(
        total.text_cache_entry_max_child_nodes_cleared,
        static_cast<std::uint64_t>(stats.text_cache_entry_max_child_nodes_cleared));
    total.route_fast_text_cells                    += static_cast<std::uint64_t>(stats.route_fast_text_cells);
    total.route_qt_text_layout_runs      +=
        static_cast<std::uint64_t>(stats.route_qt_text_layout_runs);
    total.route_graphic_geometry_cells   +=
        static_cast<std::uint64_t>(stats.route_graphic_geometry_cells);
    total.route_fallback_cells += static_cast<std::uint64_t>(stats.route_fallback_cells);
    total.qt_text_layout_calls += static_cast<std::uint64_t>(stats.qt_text_layout_calls);
    total.qsg_nodes_created    += static_cast<std::uint64_t>(stats.qsg_nodes_created);
    total.qsg_nodes_replaced   += static_cast<std::uint64_t>(stats.qsg_nodes_replaced);
    total.qsg_nodes_destroyed  += static_cast<std::uint64_t>(stats.qsg_nodes_destroyed);
    total.background_qsg_nodes_created   +=
        static_cast<std::uint64_t>(stats.background_qsg_nodes_created);
    total.background_qsg_nodes_replaced  +=
        static_cast<std::uint64_t>(stats.background_qsg_nodes_replaced);
    total.background_qsg_nodes_destroyed +=
        static_cast<std::uint64_t>(stats.background_qsg_nodes_destroyed);
    total.text_groups_considered += static_cast<std::uint64_t>(stats.text_groups_considered);
    total.text_groups_dirty      += static_cast<std::uint64_t>(stats.text_groups_dirty);
    total.text_groups_clean      += static_cast<std::uint64_t>(stats.text_groups_clean);
    total.text_clean_reuse_skips += static_cast<std::uint64_t>(stats.text_clean_reuse_skips);
    total.text_resource_descriptor_reuses +=
        static_cast<std::uint64_t>(stats.text_resource_descriptor_reuses);
    total.text_key_builds       += static_cast<std::uint64_t>(stats.text_key_builds);
    total.text_key_bytes        += stats.text_key_bytes;
    total.rect_key_builds       += static_cast<std::uint64_t>(stats.rect_key_builds);
    total.rect_key_bytes        += stats.rect_key_bytes;
    total.cache_key_builds      += static_cast<std::uint64_t>(stats.cache_key_builds);
    total.cache_key_bytes       += stats.cache_key_bytes;
    total.text_dirty_row_ranges += static_cast<std::uint64_t>(stats.text_dirty_row_ranges);
    total.text_dirty_rows       += static_cast<std::uint64_t>(stats.text_dirty_rows);
    total.text_runs_considered  += static_cast<std::uint64_t>(stats.text_runs_considered);
    total.text_coalescing_candidate_groups +=
        static_cast<std::uint64_t>(stats.text_coalescing_candidate_groups);
    total.text_coalescing_enabled_groups   +=
        static_cast<std::uint64_t>(stats.text_coalescing_enabled_groups);
    total.text_resource_runs_before_coalescing += static_cast<std::uint64_t>(
        stats.text_resource_runs_before_coalescing);
    total.text_resource_runs_after_coalescing +=
        static_cast<std::uint64_t>(stats.text_resource_runs_after_coalescing);
    total.rect_resource_rects_before_coalescing  += static_cast<std::uint64_t>(
        stats.rect_resource_rects_before_coalescing);
    total.rect_resource_rects_after_coalescing   += static_cast<std::uint64_t>(
        stats.rect_resource_rects_after_coalescing);
    total.background_row_rects_before_coalescing += static_cast<std::uint64_t>(
        stats.background_row_rects_before_coalescing);
    total.background_row_rects_after_coalescing  += static_cast<std::uint64_t>(
        stats.background_row_rects_after_coalescing);
    total.background_batched_rects +=
        static_cast<std::uint64_t>(stats.background_batched_rects);
    total.background_batched_vertices +=
        static_cast<std::uint64_t>(stats.background_batched_vertices);
    total.selection_batched_rects +=
        static_cast<std::uint64_t>(stats.selection_batched_rects);
    total.selection_batched_vertices +=
        static_cast<std::uint64_t>(stats.selection_batched_vertices);
    total.graphic_batched_rects +=
        static_cast<std::uint64_t>(stats.graphic_batched_rects);
    total.graphic_batched_vertices +=
        static_cast<std::uint64_t>(stats.graphic_batched_vertices);
    total.decoration_batched_rects +=
        static_cast<std::uint64_t>(stats.decoration_batched_rects);
    total.decoration_batched_vertices +=
        static_cast<std::uint64_t>(stats.decoration_batched_vertices);
    total.text_cache_entries_created       +=
        static_cast<std::uint64_t>(stats.text_cache_entries_created);
    total.text_cache_entries_replaced      +=
        static_cast<std::uint64_t>(stats.text_cache_entries_replaced);
    total.text_cache_entries_removed       +=
        static_cast<std::uint64_t>(stats.text_cache_entries_removed);
    total.frame_background_rects += static_cast<std::uint64_t>(stats.frame_background_rects);
    total.frame_selection_rects  += static_cast<std::uint64_t>(stats.frame_selection_rects);
    total.frame_graphic_rects    += static_cast<std::uint64_t>(stats.frame_graphic_rects);
    total.frame_graphic_arcs     += static_cast<std::uint64_t>(stats.frame_graphic_arcs);
    total.frame_text_runs        += static_cast<std::uint64_t>(stats.frame_text_runs);
    total.frame_cursor_text_runs += static_cast<std::uint64_t>(stats.frame_cursor_text_runs);
    total.frame_decorations      += static_cast<std::uint64_t>(stats.frame_decorations);
    total.frame_cursors          += static_cast<std::uint64_t>(stats.frame_cursors);
    total.frame_cursor_graphic_rects       +=
        static_cast<std::uint64_t>(stats.frame_cursor_graphic_rects);
    total.frame_cursor_graphic_arcs        +=
        static_cast<std::uint64_t>(stats.frame_cursor_graphic_arcs);
    total.frame_overlay_rects              += static_cast<std::uint64_t>(stats.frame_overlay_rects);
    total.frame_dirty_row_ranges           +=
        static_cast<std::uint64_t>(stats.frame_dirty_row_ranges);
    total.frame_packed_rows                += static_cast<std::uint64_t>(stats.frame_packed_rows);
    total.frame_packed_text_spans          +=
        static_cast<std::uint64_t>(stats.frame_packed_text_spans);
    total.frame_packed_text_cells          +=
        static_cast<std::uint64_t>(stats.frame_packed_text_cells);
    total.frame_packed_graphic_spans       +=
        static_cast<std::uint64_t>(stats.frame_packed_graphic_spans);
    total.frame_packed_graphic_cells       +=
        static_cast<std::uint64_t>(stats.frame_packed_graphic_cells);
    total.frame_packed_payload_bytes += stats.frame_packed_payload_bytes;
    total.row_cache_hits             += static_cast<std::uint64_t>(stats.row_cache_hits);
    total.row_cache_clean_skips            +=
        static_cast<std::uint64_t>(stats.row_cache_clean_skips);
    total.text_wrapper_order_rebuilds   += count_from_bool(stats.text_wrapper_order_rebuilt);
    total.background_layer_rebuilds     += count_from_bool(stats.background_layer_rebuilt);
    total.selection_layer_rebuilds      += count_from_bool(stats.selection_layer_rebuilt);
    total.graphic_layer_rebuilds        += count_from_bool(stats.graphic_layer_rebuilt);
    total.decoration_layer_rebuilds     += count_from_bool(stats.decoration_layer_rebuilt);
    total.cursor_layer_rebuilds         += count_from_bool(stats.cursor_layer_rebuilt);
    total.cursor_graphic_layer_rebuilds += count_from_bool(stats.cursor_graphic_layer_rebuilt);
    total.cursor_text_layer_rebuilds    += count_from_bool(stats.cursor_text_layer_rebuilt);
    total.overlay_layer_rebuilds        += count_from_bool(stats.overlay_layer_rebuilt);
    total.background_rows_rebuilt       += static_cast<std::uint64_t>(stats.background_rows_rebuilt);
    total.background_rows_reused        += static_cast<std::uint64_t>(stats.background_rows_reused);
    total.background_row_clean_reuse_skips +=
        static_cast<std::uint64_t>(stats.background_row_clean_reuse_skips);
    total.background_rows_removed          += static_cast<std::uint64_t>(stats.background_rows_removed);
    total.background_row_cache_fallbacks   +=
        static_cast<std::uint64_t>(stats.background_row_cache_fallbacks);
    total.selection_rows_rebuilt += static_cast<std::uint64_t>(stats.selection_rows_rebuilt);
    total.selection_rows_reused  += static_cast<std::uint64_t>(stats.selection_rows_reused);
    total.selection_row_clean_reuse_skips  +=
        static_cast<std::uint64_t>(stats.selection_row_clean_reuse_skips);
    total.selection_rows_removed           += static_cast<std::uint64_t>(stats.selection_rows_removed);
    total.selection_row_cache_fallbacks    +=
        static_cast<std::uint64_t>(stats.selection_row_cache_fallbacks);
    total.decoration_rows_rebuilt += static_cast<std::uint64_t>(stats.decoration_rows_rebuilt);
    total.decoration_rows_reused  += static_cast<std::uint64_t>(stats.decoration_rows_reused);
    total.decoration_row_clean_reuse_skips +=
        static_cast<std::uint64_t>(stats.decoration_row_clean_reuse_skips);
    total.decoration_rows_removed          += static_cast<std::uint64_t>(stats.decoration_rows_removed);
    total.decoration_row_cache_fallbacks   +=
        static_cast<std::uint64_t>(stats.decoration_row_cache_fallbacks);
    total.graphic_rect_rows_rebuilt        +=
        static_cast<std::uint64_t>(stats.graphic_rect_rows_rebuilt);
    total.graphic_rect_rows_reused         +=
        static_cast<std::uint64_t>(stats.graphic_rect_rows_reused);
    total.graphic_rect_row_clean_reuse_skips +=
        static_cast<std::uint64_t>(stats.graphic_rect_row_clean_reuse_skips);
    total.graphic_rect_rows_removed        +=
        static_cast<std::uint64_t>(stats.graphic_rect_rows_removed);
    total.graphic_rect_row_cache_fallbacks +=
        static_cast<std::uint64_t>(stats.graphic_rect_row_cache_fallbacks);
    total.graphic_arc_rows_rebuilt         +=
        static_cast<std::uint64_t>(stats.graphic_arc_rows_rebuilt);
    total.graphic_arc_rows_reused          +=
        static_cast<std::uint64_t>(stats.graphic_arc_rows_reused);
    total.graphic_arc_row_clean_reuse_skips +=
        static_cast<std::uint64_t>(stats.graphic_arc_row_clean_reuse_skips);
    total.graphic_arc_rows_removed         +=
        static_cast<std::uint64_t>(stats.graphic_arc_rows_removed);
    total.graphic_arc_row_cache_fallbacks  +=
        static_cast<std::uint64_t>(stats.graphic_arc_row_cache_fallbacks);
}

}

terminal_renderer_lifecycle_stats_t Terminal_renderer_lifecycle_recorder::snapshot() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

void Terminal_renderer_stats_publisher::publish(const terminal_renderer_stats_t& stats)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_stats = stats;
    accumulate_renderer_stats(m_cumulative_stats, stats);
}

terminal_renderer_stats_t Terminal_renderer_stats_publisher::snapshot() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

terminal_renderer_cumulative_stats_t
Terminal_renderer_stats_publisher::cumulative_snapshot() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_cumulative_stats;
}

namespace {

Terminal_simple_content_text_category simple_content_text_category(const QString& text)
{
    if (text.isEmpty()) {
        return Terminal_simple_content_text_category::EMPTY;
    }

    if (is_printable_ascii_text(text)) {
        return Terminal_simple_content_text_category::PRINTABLE_ASCII;
    }

    return contains_non_ascii_text(text)
        ? Terminal_simple_content_text_category::NON_ASCII
        : Terminal_simple_content_text_category::OTHER_ASCII;
}

Terminal_simple_content_rejection_reason unrepresented_simple_cell_semantics_rejection(
    const Terminal_render_cell&    cell,
    bool                           has_decoration)
{
    if (cell.hyperlink_id != 0U) {
        return Terminal_simple_content_rejection_reason::HYPERLINK;
    }

    if (has_decoration) {
        return Terminal_simple_content_rejection_reason::DECORATION;
    }

    return Terminal_simple_content_rejection_reason::NONE;
}

} // namespace

terminal_simple_content_classification_t classify_terminal_simple_content_cell(
    const Terminal_render_cell&        cell,
    terminal_grid_size_t               grid_size,
    std::size_t                        style_count,
    bool                               has_decoration,
    bool                               dirty_row,
    Terminal_shaped_presentation_mode  presentation_mode)
{
    terminal_simple_content_classification_t classification;
    classification.text_category = simple_content_text_category(cell.text);
    classification.row           = cell.position.row;
    classification.style_id      = cell.style_id;
    classification.dirty_row     = dirty_row;

    const auto reject = [&](
        Terminal_simple_content_route route,
        Terminal_simple_content_rejection_reason reason) {
        classification.route            = route;
        classification.rejection_reason = reason;
        return classification;
    };

    if (grid_size.rows <= 0 || grid_size.columns <= 0) {
        return reject(
            Terminal_simple_content_route::FALLBACK,
            Terminal_simple_content_rejection_reason::INVALID_GRID);
    }

    if (!position_inside_grid(cell.position, grid_size)) {
        return
            reject(
                Terminal_simple_content_route::FALLBACK,
                Terminal_simple_content_rejection_reason::INVALID_POSITION);
    }

    if (static_cast<std::size_t>(cell.style_id) >= style_count) {
        return
            reject(
                Terminal_simple_content_route::FALLBACK,
                Terminal_simple_content_rejection_reason::INVALID_STYLE_ID);
    }

    if (cell.wide_continuation) {
        return reject(
            Terminal_simple_content_route::NONE,
            Terminal_simple_content_rejection_reason::WIDE_CONTINUATION);
    }

    if (cell.display_width <= 0 ||
        cell.display_width >  grid_size.columns - cell.position.column)
    {
        return
            reject(
                Terminal_simple_content_route::FALLBACK,
                Terminal_simple_content_rejection_reason::INVALID_DISPLAY_WIDTH);
    }

    classification.valid_terminal_cell = true;
    if (cell.text.isEmpty()) {
        return reject(
            Terminal_simple_content_route::NONE,
            Terminal_simple_content_rejection_reason::EMPTY_TEXT);
    }

    Terminal_cell_shaping_input shaping_input;
    shaping_input.column            = cell.position.column;
    shaping_input.text              = cell.text;
    shaping_input.display_width     = cell.display_width;
    shaping_input.wide_continuation = cell.wide_continuation;
    shaping_input.style_id          = cell.style_id;
    shaping_input.hyperlink_id      = cell.hyperlink_id;
    shaping_input.presentation_mode = presentation_mode;

    const Terminal_shaped_run_status scalar_status =
        validate_cell_text_scalars(shaping_input.text);
    if (scalar_status != Terminal_shaped_run_status::OK) {
        return
            reject(
                Terminal_simple_content_route::QT_TEXT_LAYOUT,
                Terminal_simple_content_rejection_reason::INVALID_TEXT_ENCODING);
    }

    const Terminal_shaped_run_status width_status =
        validate_cell_text_width(shaping_input);
    if (width_status != Terminal_shaped_run_status::OK) {
        return
            reject(
                Terminal_simple_content_route::QT_TEXT_LAYOUT,
                Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH);
    }

    if (cell.display_width != 1) {
        return
            reject(
                Terminal_simple_content_route::QT_TEXT_LAYOUT,
                Terminal_simple_content_rejection_reason::MULTI_CELL_TEXT);
    }

    const Terminal_simple_content_rejection_reason semantics_rejection = unrepresented_simple_cell_semantics_rejection(
        cell,
        has_decoration);
    if (is_terminal_graphic_text(cell.text)) {
        if (semantics_rejection != Terminal_simple_content_rejection_reason::NONE) {
            return reject(
                Terminal_simple_content_route::QT_TEXT_LAYOUT,
                semantics_rejection);
        }

        return
            reject(
                Terminal_simple_content_route::GRAPHIC_GEOMETRY,
                Terminal_simple_content_rejection_reason::TERMINAL_GRAPHIC);
    }

    if (classification.text_category ==
        Terminal_simple_content_text_category::NON_ASCII)
    {
        return
            reject(
                Terminal_simple_content_route::QT_TEXT_LAYOUT,
                Terminal_simple_content_rejection_reason::NON_ASCII_TEXT);
    }

    if (classification.text_category !=
        Terminal_simple_content_text_category::PRINTABLE_ASCII)
    {
        return
            reject(
                Terminal_simple_content_route::QT_TEXT_LAYOUT,
                Terminal_simple_content_rejection_reason::NON_PRINTABLE_ASCII);
    }

    if (semantics_rejection != Terminal_simple_content_rejection_reason::NONE) {
        return reject(
            Terminal_simple_content_route::QT_TEXT_LAYOUT,
            semantics_rejection);
    }

    classification.route = Terminal_simple_content_route::FAST_TEXT;
    classification.rejection_reason =
        Terminal_simple_content_rejection_reason::NONE;
    classification.fast_text_eligible = true;
    return classification;
}

namespace {

void accumulate_simple_content_stats(
    terminal_simple_content_cumulative_stats_t&    total,
    const terminal_simple_content_stats_t&         stats)
{
    total.cells_considered += static_cast<std::uint64_t>(stats.cells_considered);
    total.eligible_cells   += static_cast<std::uint64_t>(stats.eligible_cells);
    total.eligible_after_all_gates_cells      +=
        static_cast<std::uint64_t>(stats.eligible_after_all_gates_cells);
    total.rows_with_eligible_cells            +=
        static_cast<std::uint64_t>(stats.rows_with_eligible_cells);
    total.styles_with_eligible_cells          +=
        static_cast<std::uint64_t>(stats.styles_with_eligible_cells);
    total.dirty_eligible_cells                +=
        static_cast<std::uint64_t>(stats.dirty_eligible_cells);
    total.clean_eligible_cells                +=
        static_cast<std::uint64_t>(stats.clean_eligible_cells);
    total.text_category_empty_cells           +=
        static_cast<std::uint64_t>(stats.text_category_empty_cells);
    total.text_category_printable_ascii_cells +=
        static_cast<std::uint64_t>(stats.text_category_printable_ascii_cells);
    total.text_category_other_ascii_cells     +=
        static_cast<std::uint64_t>(stats.text_category_other_ascii_cells);
    total.text_category_non_ascii_cells       +=
        static_cast<std::uint64_t>(stats.text_category_non_ascii_cells);
    total.route_none_cells                    +=
        static_cast<std::uint64_t>(stats.route_none_cells);
    total.route_fast_text_cells               +=
        static_cast<std::uint64_t>(stats.route_fast_text_cells);
    total.route_qt_text_layout_cells          +=
        static_cast<std::uint64_t>(stats.route_qt_text_layout_cells);
    total.route_graphic_geometry_cells        +=
        static_cast<std::uint64_t>(stats.route_graphic_geometry_cells);
    total.route_fallback_cells                +=
        static_cast<std::uint64_t>(stats.route_fallback_cells);
    total.rejection_none_cells                +=
        static_cast<std::uint64_t>(stats.rejection_none_cells);
    total.rejection_empty_text_cells          +=
        static_cast<std::uint64_t>(stats.rejection_empty_text_cells);
    total.rejection_invalid_grid_cells        +=
        static_cast<std::uint64_t>(stats.rejection_invalid_grid_cells);
    total.rejection_invalid_position_cells    +=
        static_cast<std::uint64_t>(stats.rejection_invalid_position_cells);
    total.rejection_invalid_style_id_cells    +=
        static_cast<std::uint64_t>(stats.rejection_invalid_style_id_cells);
    total.rejection_wide_continuation_cells   +=
        static_cast<std::uint64_t>(stats.rejection_wide_continuation_cells);
    total.rejection_invalid_display_width_cells += static_cast<std::uint64_t>(
        stats.rejection_invalid_display_width_cells);
    total.rejection_invalid_text_encoding_cells += static_cast<std::uint64_t>(
        stats.rejection_invalid_text_encoding_cells);
    total.rejection_invalid_text_width_cells  +=
        static_cast<std::uint64_t>(stats.rejection_invalid_text_width_cells);
    total.rejection_multi_cell_text_cells     +=
        static_cast<std::uint64_t>(stats.rejection_multi_cell_text_cells);
    total.rejection_non_printable_ascii_cells +=
        static_cast<std::uint64_t>(stats.rejection_non_printable_ascii_cells);
    total.rejection_non_ascii_text_cells      +=
        static_cast<std::uint64_t>(stats.rejection_non_ascii_text_cells);
    total.rejection_decoration_cells          +=
        static_cast<std::uint64_t>(stats.rejection_decoration_cells);
    total.rejection_hyperlink_cells           +=
        static_cast<std::uint64_t>(stats.rejection_hyperlink_cells);
    total.rejection_terminal_graphic_cells    +=
        static_cast<std::uint64_t>(stats.rejection_terminal_graphic_cells);
}

void record_simple_content_text_category(
    terminal_simple_content_stats_t&       stats,
    Terminal_simple_content_text_category  category)
{
    switch (category) {
        case Terminal_simple_content_text_category::EMPTY:
            ++stats.text_category_empty_cells;
            return;
        case Terminal_simple_content_text_category::PRINTABLE_ASCII:
            ++stats.text_category_printable_ascii_cells;
            return;
        case Terminal_simple_content_text_category::OTHER_ASCII:
            ++stats.text_category_other_ascii_cells;
            return;
        case Terminal_simple_content_text_category::NON_ASCII:
            ++stats.text_category_non_ascii_cells;
            return;
    }
}

void record_simple_content_route(
    terminal_simple_content_stats_t&       stats,
    Terminal_simple_content_route          route)
{
    switch (route) {
        case Terminal_simple_content_route::NONE:
            ++stats.route_none_cells;
            return;
        case Terminal_simple_content_route::FAST_TEXT:
            ++stats.route_fast_text_cells;
            return;
        case Terminal_simple_content_route::QT_TEXT_LAYOUT:
            ++stats.route_qt_text_layout_cells;
            return;
        case Terminal_simple_content_route::GRAPHIC_GEOMETRY:
            ++stats.route_graphic_geometry_cells;
            return;
        case Terminal_simple_content_route::FALLBACK:
            ++stats.route_fallback_cells;
            return;
    }
}

void record_simple_content_rejection(
    terminal_simple_content_stats_t&           stats,
    Terminal_simple_content_rejection_reason   reason)
{
    switch (reason) {
        case Terminal_simple_content_rejection_reason::NONE:
            ++stats.rejection_none_cells;
            return;
        case Terminal_simple_content_rejection_reason::EMPTY_TEXT:
            ++stats.rejection_empty_text_cells;
            return;
        case Terminal_simple_content_rejection_reason::INVALID_GRID:
            ++stats.rejection_invalid_grid_cells;
            return;
        case Terminal_simple_content_rejection_reason::INVALID_POSITION:
            ++stats.rejection_invalid_position_cells;
            return;
        case Terminal_simple_content_rejection_reason::INVALID_STYLE_ID:
            ++stats.rejection_invalid_style_id_cells;
            return;
        case Terminal_simple_content_rejection_reason::WIDE_CONTINUATION:
            ++stats.rejection_wide_continuation_cells;
            return;
        case Terminal_simple_content_rejection_reason::INVALID_DISPLAY_WIDTH:
            ++stats.rejection_invalid_display_width_cells;
            return;
        case Terminal_simple_content_rejection_reason::INVALID_TEXT_ENCODING:
            ++stats.rejection_invalid_text_encoding_cells;
            return;
        case Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH:
            ++stats.rejection_invalid_text_width_cells;
            return;
        case Terminal_simple_content_rejection_reason::MULTI_CELL_TEXT:
            ++stats.rejection_multi_cell_text_cells;
            return;
        case Terminal_simple_content_rejection_reason::NON_PRINTABLE_ASCII:
            ++stats.rejection_non_printable_ascii_cells;
            return;
        case Terminal_simple_content_rejection_reason::NON_ASCII_TEXT:
            ++stats.rejection_non_ascii_text_cells;
            return;
        case Terminal_simple_content_rejection_reason::DECORATION:
            ++stats.rejection_decoration_cells;
            return;
        case Terminal_simple_content_rejection_reason::HYPERLINK:
            ++stats.rejection_hyperlink_cells;
            return;
        case Terminal_simple_content_rejection_reason::TERMINAL_GRAPHIC:
            ++stats.rejection_terminal_graphic_cells;
            return;
    }
}

void record_simple_content_classification(
    terminal_simple_content_stats_t&                   stats,
    const terminal_simple_content_classification_t&    classification,
    bool                                               eligible_after_all_gates,
    std::set<int>&                                     eligible_rows,
    std::set<Terminal_style_id>&                       eligible_styles)
{
    ++stats.cells_considered;
    record_simple_content_text_category(stats, classification.text_category);
    record_simple_content_route(stats, classification.route);
    record_simple_content_rejection(stats, classification.rejection_reason);

    if (!classification.fast_text_eligible) {
        return;
    }

    ++stats.eligible_cells;
    if (eligible_after_all_gates) {
        ++stats.eligible_after_all_gates_cells;
    }
    if (classification.dirty_row) {
        ++stats.dirty_eligible_cells;
    }
    else {
        ++stats.clean_eligible_cells;
    }

    eligible_rows.insert(classification.row);
    eligible_styles.insert(classification.style_id);
}

bool cell_text_decoration_enabled(
    const Terminal_render_cell&    cell,
    std::size_t                    style_count,
    Render_style_attribute_cache&  style_attributes)
{
    if (cell.wide_continuation ||
        static_cast<std::size_t>(cell.style_id) >= style_count)
    {
        return false;
    }

    const render_style_attributes_t& style = style_attributes.attributes(cell.style_id);
    return style.underline || style.strike;
}

void finalize_simple_content_stats(
    terminal_simple_content_stats_t&   stats,
    const std::set<int>&               eligible_rows,
    const std::set<Terminal_style_id>& eligible_styles)
{
    stats.rows_with_eligible_cells   = static_cast<int>(eligible_rows.size());
    stats.styles_with_eligible_cells = static_cast<int>(eligible_styles.size());
}

bool cell_intersects_grid_columns(
    const Terminal_render_cell&    cell,
    int                            first_column,
    int                            column_count)
{
    const int cell_end = cell.position.column + cell.display_width;
    const int span_end = first_column + column_count;
    return
        column_count         > 0        &&
        cell.position.column < span_end &&
        first_column         < cell_end;
}

bool block_cursor_covers_cell(
    const Terminal_render_cell&    cell,
    const Terminal_render_cursor&  cursor,
    bool                           block_cursor_visible)
{
    return
        block_cursor_visible                     &&
        cell.position.row == cursor.position.row &&
        cell_intersects_grid_columns(cell, cursor.position.column, 1);
}

bool ime_preedit_covers_cell(
    const Terminal_render_cell&    cell,
    bool                           ime_preedit_visible,
    int                            ime_preedit_row,
    int                            ime_preedit_column,
    int                            ime_preedit_columns)
{
    return
        ime_preedit_visible                  &&
        cell.position.row == ime_preedit_row &&
        cell_intersects_grid_columns(cell, ime_preedit_column, ime_preedit_columns);
}

bool snapshot_row_is_dirty(
    const Terminal_render_snapshot&    snapshot,
    int                                row)
{
    for (const Terminal_render_dirty_row_range& range : snapshot.dirty_row_ranges) {
        if (row >= range.first_row && row < range.first_row + range.row_count) { return true;  }
        if (row <  range.first_row)                                            { return false; }
    }
    return false;
}

std::vector<std::vector<const Terminal_render_cell*>> build_explicit_snapshot_row_table(
    const Terminal_render_snapshot& snapshot)
{
    if (snapshot.grid_size.rows <= 0) {
        return {};
    }

    std::vector<std::vector<const Terminal_render_cell*>> row_table(
        static_cast<std::size_t>(snapshot.grid_size.rows));
    for (const Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row < 0 || cell.position.row >= snapshot.grid_size.rows) {
            continue;
        }
        row_table[static_cast<std::size_t>(cell.position.row)].push_back(&cell);
    }

    for (std::vector<const Terminal_render_cell*>& row_cells : row_table) {
        std::stable_sort(
            row_cells.begin(),
            row_cells.end(),
            [](const Terminal_render_cell* left, const Terminal_render_cell* right) {
                return left->position.column < right->position.column;
            });
    }

    return row_table;
}

std::uint32_t packed_color_rgba(const QColor& color)
{
    return static_cast<std::uint32_t>(color.rgba());
}

void append_packed_text_bytes(
    Terminal_render_frame&             frame,
    terminal_packed_text_span_t&       span,
    const QString&                     text)
{
    const QByteArray bytes = text.toUtf8();
    frame.packed_text_bytes.insert(
        frame.packed_text_bytes.end(),
        bytes.constData(),
        bytes.constData() + bytes.size());
    span.text_length += static_cast<std::uint32_t>(bytes.size());
}

void append_packed_text_cell(
    Terminal_render_frame&             frame,
    terminal_packed_render_row_t&      row,
    const Terminal_render_cell&        cell,
    const render_style_attributes_t&   style)
{
    const std::uint32_t foreground_rgba = packed_color_rgba(style.foreground);
    const std::uint32_t background_rgba = packed_color_rgba(style.background);
    if (row.text_span_count > 0U) {
        terminal_packed_text_span_t& span =
            frame.packed_text_spans[static_cast<std::size_t>(
                row.first_text_span + row.text_span_count - 1U)];
        if (span.first_column + span.column_count == cell.position.column &&
            span.style_id                         == cell.style_id        &&
            span.foreground_rgba                  == foreground_rgba      &&
            span.background_rgba                  == background_rgba)
        {
            span.column_count += cell.display_width;
            append_packed_text_bytes(frame, span, cell.text);
            ++frame.stats.packed_text_cells;
            return;
        }
    }

    if (row.text_span_count == 0U) {
        row.first_text_span =
            static_cast<std::uint32_t>(frame.packed_text_spans.size());
    }

    terminal_packed_text_span_t span;
    span.first_column    = cell.position.column;
    span.column_count    = cell.display_width;
    span.style_id        = cell.style_id;
    span.foreground_rgba = foreground_rgba;
    span.background_rgba = background_rgba;
    span.text_offset     = static_cast<std::uint32_t>(frame.packed_text_bytes.size());
    append_packed_text_bytes(frame, span, cell.text);
    frame.packed_text_spans.push_back(span);
    ++row.text_span_count;
    ++frame.stats.packed_text_cells;
}

void append_packed_graphic_cell(
    Terminal_render_frame&             frame,
    terminal_packed_render_row_t&      row,
    const Terminal_render_cell&        cell,
    const render_style_attributes_t&   style)
{
    const std::uint32_t foreground_rgba = packed_color_rgba(style.foreground);
    const std::uint32_t background_rgba = packed_color_rgba(style.background);
    if (row.graphic_span_count > 0U) {
        terminal_packed_graphic_span_t& span =
            frame.packed_graphic_spans[static_cast<std::size_t>(
                row.first_graphic_span + row.graphic_span_count - 1U)];
        if (span.first_column + span.column_count == cell.position.column &&
            span.style_id                         == cell.style_id        &&
            span.foreground_rgba                  == foreground_rgba      &&
            span.background_rgba                  == background_rgba)
        {
            span.column_count += cell.display_width;
            ++span.codepoint_count;
            frame.packed_graphic_codepoints.push_back(
                static_cast<std::uint32_t>(cell.text.at(0).unicode()));
            ++frame.stats.packed_graphic_cells;
            return;
        }
    }

    if (row.graphic_span_count == 0U) {
        row.first_graphic_span =
            static_cast<std::uint32_t>(frame.packed_graphic_spans.size());
    }

    terminal_packed_graphic_span_t span;
    span.first_column    = cell.position.column;
    span.column_count    = cell.display_width;
    span.style_id        = cell.style_id;
    span.foreground_rgba = foreground_rgba;
    span.background_rgba = background_rgba;
    span.codepoint_offset =
        static_cast<std::uint32_t>(frame.packed_graphic_codepoints.size());
    span.codepoint_count  = 1U;
    frame.packed_graphic_codepoints.push_back(
        static_cast<std::uint32_t>(cell.text.at(0).unicode()));
    frame.packed_graphic_spans.push_back(span);
    ++row.graphic_span_count;
    ++frame.stats.packed_graphic_cells;
}

std::uint64_t packed_payload_byte_count(const Terminal_render_frame& frame)
{
    return
        static_cast<std::uint64_t>(frame.packed_rows.size()) * sizeof(terminal_packed_render_row_t)            +
        static_cast<std::uint64_t>(frame.packed_text_spans.size()) * sizeof(terminal_packed_text_span_t)       +
        static_cast<std::uint64_t>(frame.packed_text_bytes.size()) * sizeof(char)                              +
        static_cast<std::uint64_t>(frame.packed_graphic_spans.size()) * sizeof(terminal_packed_graphic_span_t) +
        static_cast<std::uint64_t>(frame.packed_graphic_codepoints.size()) * sizeof(std::uint32_t);
}

void finalize_packed_render_frame_stats(Terminal_render_frame& frame)
{
    frame.stats.packed_rows          = static_cast<int>(frame.packed_rows.size());
    frame.stats.packed_text_spans    = static_cast<int>(frame.packed_text_spans.size());
    frame.stats.packed_graphic_spans = static_cast<int>(frame.packed_graphic_spans.size());
    frame.stats.packed_payload_bytes = packed_payload_byte_count(frame);
}

void build_terminal_render_frame_packed_data(
    Terminal_render_frame&             frame,
    const Terminal_render_snapshot&    snapshot,
    Render_style_attribute_cache&      style_attributes,
    bool                               block_cursor_visible,
    bool                               ime_preedit_visible,
    int                                ime_preedit_row,
    int                                ime_preedit_column,
    int                                ime_preedit_columns)
{
    VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::packed_data");

    if (snapshot.grid_size.rows <= 0 || snapshot.grid_size.columns <= 0) {
        return;
    }

    const std::vector<std::vector<const Terminal_render_cell*>> row_table =
        build_explicit_snapshot_row_table(snapshot);
    frame.packed_rows.reserve(static_cast<std::size_t>(snapshot.grid_size.rows));
    frame.packed_text_spans.reserve(snapshot.cells.size() / 2U);
    frame.packed_text_bytes.reserve(snapshot.cells.size());
    frame.packed_graphic_spans.reserve(snapshot.cells.size() / 8U);
    frame.packed_graphic_codepoints.reserve(snapshot.cells.size() / 8U);

    for (int row_index = 0; row_index < snapshot.grid_size.rows; ++row_index) {
        terminal_packed_render_row_t row;
        row.active_buffer = snapshot.viewport.active_buffer;
        row.viewport_row  = row_index;
        row.logical_row   = logical_row_for_viewport_row(snapshot.viewport, row_index);
        row.first_text_span =
            static_cast<std::uint32_t>(frame.packed_text_spans.size());
        row.first_graphic_span =
            static_cast<std::uint32_t>(frame.packed_graphic_spans.size());
        frame.packed_rows.push_back(row);

        terminal_packed_render_row_t& packed_row = frame.packed_rows.back();
        for (const Terminal_render_cell* cell : row_table[static_cast<std::size_t>(row_index)]) {
            const bool has_decoration = cell_text_decoration_enabled(
                *cell,
                snapshot.styles.size(),
                style_attributes);
            const terminal_simple_content_classification_t classification = classify_terminal_simple_content_cell(
                *cell,
                snapshot.grid_size,
                snapshot.styles.size(),
                has_decoration,
                snapshot_row_is_dirty(snapshot, cell->position.row));
            if (block_cursor_covers_cell(*cell, snapshot.cursor, block_cursor_visible) ||
                ime_preedit_covers_cell(
                    *cell, ime_preedit_visible, ime_preedit_row,
                    ime_preedit_column, ime_preedit_columns))
            {
                continue;
            }

            if (classification.route == Terminal_simple_content_route::GRAPHIC_GEOMETRY) {
                append_packed_graphic_cell(
                    frame,
                    packed_row,
                    *cell,
                    style_attributes.attributes(cell->style_id));
                continue;
            }

            if (!classification.fast_text_eligible) {
                continue;
            }

            append_packed_text_cell(
                frame,
                packed_row,
                *cell,
                style_attributes.attributes(cell->style_id));
        }
    }

    finalize_packed_render_frame_stats(frame);
}

} // namespace

Terminal_render_frame build_terminal_render_frame(
    const Terminal_render_snapshot*    snapshot,
    QSizeF                             logical_size,
    terminal_cell_metrics_t            cell_metrics,
    const Terminal_render_options&     options,
    bool                               cursor_blink_visible,
    const Ime_preedit_state*           ime_preedit_override)
{
    VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame");

    const bool metrics_valid = is_valid_cell_metrics(cell_metrics);
    Terminal_render_frame frame;
    frame.logical_size = logical_size;
    frame.cell_metrics = cell_metrics;
    frame.background_rects.reserve(
        snapshot != nullptr && metrics_valid ? snapshot->cells.size() + 1U : 1U);
    frame.background_rects.push_back({
        QRectF(QPointF(0.0, 0.0), logical_size),
        options.default_background,
    });
    frame.stats.background_rects_emitted =
        static_cast<int>(frame.background_rects.size());

    if (snapshot == nullptr || !metrics_valid) {
        return frame;
    }

    const QColor grid_background = default_terminal_background(*snapshot);
    frame.background_rects.front().color = grid_background;
    frame.grid_size        = snapshot->grid_size;
    frame.viewport         = snapshot->viewport;
    frame.dirty_row_ranges = snapshot->dirty_row_ranges;
    const bool valid_grid = snapshot->grid_size.rows > 0 && snapshot->grid_size.columns > 0;
    const Ime_preedit_state& ime_preedit =
        ime_preedit_override != nullptr ? *ime_preedit_override : snapshot->ime_preedit;
    const bool cursor_in_grid = valid_grid &&
        position_inside_grid(snapshot->cursor.position, snapshot->grid_size);
    const bool cursor_blink_enabled =
        options.cursor_blink_enabled_override.value_or(snapshot->cursor.blink_enabled);
    const bool cursor_visible =
        cursor_in_grid && snapshot->cursor.visible &&
        (!cursor_blink_enabled || cursor_blink_visible);
    const Terminal_cursor_shape cursor_shape =
        options.cursor_shape_override.value_or(snapshot->cursor.shape);
    const bool block_cursor_visible =
        cursor_visible && cursor_shape == Terminal_cursor_shape::BLOCK;
    const QRectF block_cursor_rect = block_cursor_visible
        ? cursor_rect(snapshot->cursor, cursor_shape, cell_metrics)
        : QRectF();
    const bool ime_preedit_visible =
        valid_grid && ime_preedit.active && !ime_preedit.text.isEmpty();
    const int ime_preedit_row = ime_preedit_visible
        ? std::clamp(snapshot->cursor.position.row, 0, snapshot->grid_size.rows - 1)
        : -1;
    const int ime_preedit_column = ime_preedit_visible
        ? std::clamp(snapshot->cursor.position.column, 0, snapshot->grid_size.columns - 1)
        : -1;
    const int ime_preedit_columns = ime_preedit_visible
        ? std::clamp(
            display_width_for_text(ime_preedit.text),
            1,
            snapshot->grid_size.columns - ime_preedit_column)
        : 0;
    Render_style_attribute_cache style_attributes(*snapshot);

    const std::size_t cell_count = snapshot->cells.size();
    frame.selection_rects.reserve(snapshot->selection_spans.size() + 1U);
    frame.graphic_rects.reserve(cell_count / 8U);
    frame.graphic_arcs.reserve(cell_count / 16U);
    frame.text_runs.reserve(cell_count);
    frame.decorations.reserve(cell_count / 8U);
    frame.cursors.reserve(2U);
    frame.cursor_graphic_rects.reserve(4U);
    frame.cursor_graphic_arcs.reserve(2U);
    frame.overlay_rects.reserve(1U);

    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::cells");

        std::vector<bool> seen_style_ids(snapshot->styles.size(), false);
        Terminal_style_id previous_style_id     = k_default_terminal_style_id;
        bool              has_previous_style_id = false;
        std::set<int> simple_eligible_rows;
        std::set<Terminal_style_id> simple_eligible_styles;
        for (const Terminal_render_cell& cell : snapshot->cells) {
            ++frame.stats.cells_considered;
            const bool classification_has_decoration = cell_text_decoration_enabled(
                cell,
                snapshot->styles.size(),
                style_attributes);
            const terminal_simple_content_classification_t classification = classify_terminal_simple_content_cell(
                cell,
                snapshot->grid_size,
                snapshot->styles.size(),
                classification_has_decoration,
                snapshot_row_is_dirty(*snapshot, cell.position.row));
            const bool block_cursor_cell =
                block_cursor_covers_cell(cell, snapshot->cursor, block_cursor_visible);
            const bool ime_preedit_cell = ime_preedit_covers_cell(
                cell,
                ime_preedit_visible,
                ime_preedit_row,
                ime_preedit_column,
                ime_preedit_columns);
            const bool eligible_after_all_gates =
                classification.fast_text_eligible &&
                !block_cursor_cell                &&
                !ime_preedit_cell;
            record_simple_content_classification(
                frame.stats.simple_content,
                classification,
                eligible_after_all_gates,
                simple_eligible_rows,
                simple_eligible_styles);

            if (cell.wide_continuation) {
                ++frame.stats.cells_skipped_wide_continuation;
                continue;
            }

            if (!valid_grid ||
                cell.position.row                       <  0                                                  ||
                cell.position.row                       >= snapshot->grid_size.rows                           ||
                cell.position.column                    <  0                                                  ||
                cell.position.column                    >= snapshot->grid_size.columns                        ||
                cell.display_width                      <= 0                                                  ||
                cell.display_width                      >  snapshot->grid_size.columns - cell.position.column ||
                static_cast<std::size_t>(cell.style_id) >= snapshot->styles.size())
            {
                ++frame.stats.cells_skipped_invalid;
                continue;
            }

            ++frame.stats.cells_rendered;
            if (has_previous_style_id && previous_style_id != cell.style_id) {
                ++frame.stats.text_style_changes;
            }
            previous_style_id = cell.style_id;
            has_previous_style_id = true;

            const std::size_t style_index = static_cast<std::size_t>(cell.style_id);
            if (!seen_style_ids[style_index]) {
                seen_style_ids[style_index] = true;
                ++frame.stats.text_distinct_styles;
            }

            const render_style_attributes_t& style =
                style_attributes.attributes(cell.style_id);
            const QColor foreground = style.foreground;
            const QColor background = style.background;

            const QRectF rect = cell_rect(
                cell.position.row,
                cell.position.column,
                cell.display_width,
                cell_metrics);
            if (background != grid_background) {
                frame.background_rects.push_back({rect, background});
            }

            const bool text_is_empty = cell.text.isEmpty();
            if (text_is_empty) {
                ++frame.stats.text_cells_empty;
            }
            else {
                if (cell.display_width == 1) {
                    ++frame.stats.text_cells_single_width;
                }
                else {
                    ++frame.stats.text_cells_multi_width;
                }

                const bool printable_ascii = is_printable_ascii_text(cell.text);
                if (printable_ascii) {
                    ++frame.stats.text_cells_printable_ascii;
                    if (cell.display_width == 1) {
                        ++frame.stats.text_cells_simple_ascii;
                    }
                }
                else
                if (contains_non_ascii_text(cell.text)) {
                    ++frame.stats.text_cells_non_ascii;
                }
                else {
                    ++frame.stats.text_cells_other_ascii;
                }

                if (cell.hyperlink_id != 0U) {
                    ++frame.stats.text_cells_with_hyperlink;
                }
            }

            Terminal_render_text_run run;
            run.row             = cell.position.row;
            run.logical_row     = logical_row_for_viewport_row(snapshot->viewport, cell.position.row);
            run.column          = cell.position.column;
            run.rect            = rect;
            run.baseline_origin = QPointF(rect.left(), rect.top() + cell_metrics.ascent);
            run.text            = cell.text;
            run.foreground      = foreground;
            run.background      = background;
            run.style_id        = cell.style_id;
            run.hyperlink_id    = cell.hyperlink_id;
            run.underline       = style.underline;
            run.strike          = style.strike;
            const bool packed_hard_graphic_covered =
                !text_is_empty                                                          &&
                classification.route == Terminal_simple_content_route::GRAPHIC_GEOMETRY &&
                terminal_hard_block_graphic_text_is_supported(cell.text)                &&
                !block_cursor_cell                                                      &&
                !ime_preedit_cell;
            const bool rendered_as_graphic = packed_hard_graphic_covered ||
                (!text_is_empty &&
                    append_terminal_graphic_rects(
                        frame.graphic_rects,
                        frame.graphic_arcs,
                        rect,
                        cell.text,
                        foreground,
                        cell_metrics));
            if (rendered_as_graphic) {
                if (block_cursor_visible &&
                    cell.position.row    == snapshot->cursor.position.row &&
                    cell.position.column == snapshot->cursor.position.column)
                {
                    const std::size_t first_cursor_graphic_rect =
                        frame.cursor_graphic_rects.size();
                    append_terminal_graphic_rects(
                        frame.cursor_graphic_rects,
                        frame.cursor_graphic_arcs,
                        rect,
                        cell.text,
                        cursor_graphic_overlay_color(background),
                        cell_metrics);
                    clip_terminal_graphic_rects(
                        frame.cursor_graphic_rects,
                        first_cursor_graphic_rect,
                        block_cursor_rect);
                }
                ++frame.stats.text_cells_rendered_as_graphic;
            }
            else
            if (!text_is_empty) {
                frame.text_runs.push_back(run);
                ++frame.stats.text_cells_rendered_as_text;
            }

            const bool has_decoration =
                run.underline                                           ||
                run.strike                                              ||
                (options.underline_hyperlinks && run.hyperlink_id != 0U);
            if (has_decoration) {
                ++frame.stats.text_cells_with_decorations;
            }
            if (run.underline) {
                frame.decorations.push_back({
                    Terminal_render_decoration_kind::UNDERLINE,
                    decoration_rect(run, Terminal_render_decoration_kind::UNDERLINE, cell_metrics),
                    foreground,
                });
            }
            if (run.strike) {
                frame.decorations.push_back({
                    Terminal_render_decoration_kind::STRIKE,
                    decoration_rect(run, Terminal_render_decoration_kind::STRIKE, cell_metrics),
                    foreground,
                });
            }
            if (options.underline_hyperlinks && run.hyperlink_id != 0U) {
                frame.decorations.push_back({
                    Terminal_render_decoration_kind::HYPERLINK_UNDERLINE,
                    decoration_rect(
                        run,
                        Terminal_render_decoration_kind::HYPERLINK_UNDERLINE,
                        cell_metrics),
                    foreground,
                });
            }
        }
        finalize_simple_content_stats(
            frame.stats.simple_content,
            simple_eligible_rows,
            simple_eligible_styles);
    }

    build_terminal_render_frame_packed_data(
        frame,
        *snapshot,
        style_attributes,
        block_cursor_visible,
        ime_preedit_visible,
        ime_preedit_row,
        ime_preedit_column,
        ime_preedit_columns);

    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::selection");

        for (const Terminal_render_selection_span& span : snapshot->selection_spans) {
            frame.selection_rects.push_back({
                cell_rect(span.row, span.first_column, span.column_count, cell_metrics),
                options.selection_background,
            });
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::cursor");

        if (cursor_visible) {
            const QRectF rendered_cursor_rect =
                cursor_rect(snapshot->cursor, cursor_shape, cell_metrics);
            if (cursor_shape == Terminal_cursor_shape::BLOCK &&
                !frame.cursor_graphic_rects.empty())
            {
                for (const QRectF& rect : cursor_rects_excluding_graphics(
                    rendered_cursor_rect,
                    frame.cursor_graphic_rects))
                {
                    frame.cursors.push_back({
                        cursor_shape,
                        rect,
                        options.cursor_color,
                    });
                }
            }
            else {
                frame.cursors.push_back({
                    cursor_shape,
                    rendered_cursor_rect,
                    options.cursor_color,
                });
            }
            if (cursor_shape == Terminal_cursor_shape::BLOCK) {
                const auto text_run = std::find_if(
                    frame.text_runs.begin(),
                    frame.text_runs.end(),
                    [&](const Terminal_render_text_run& run) {
                        return
                            run.row == snapshot->cursor.position.row  &&
                            run.rect.intersects(rendered_cursor_rect) &&
                            !run.text.isEmpty();
                    });
                if (text_run != frame.text_runs.end()) {
                    Terminal_render_text_run cursor_run = *text_run;
                    cursor_run.foreground = cursor_run.background;
                    cursor_run.clip_rect  = rendered_cursor_rect;
                    frame.cursor_text_runs.push_back(std::move(cursor_run));
                }
            }
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("build_terminal_render_frame::ime");

        if (valid_grid && ime_preedit.active && !ime_preedit.text.isEmpty()) {
            const int row =
                std::clamp(snapshot->cursor.position.row, 0, snapshot->grid_size.rows - 1);
            const int column =
                std::clamp(snapshot->cursor.position.column, 0, snapshot->grid_size.columns - 1);
            const int column_count = std::clamp(
                display_width_for_text(ime_preedit.text),
                1,
                snapshot->grid_size.columns - column);
            const QRectF rect = cell_rect(row, column, column_count, cell_metrics);
            frame.selection_rects.push_back({rect, options.preedit_background});

            Terminal_render_text_run run;
            run.row             = row;
            run.logical_row     = logical_row_for_viewport_row(snapshot->viewport, row);
            run.column          = column;
            run.rect            = rect;
            run.baseline_origin = QPointF(rect.left(), rect.top() + cell_metrics.ascent);
            run.text            = ime_preedit.text;
            run.foreground      = options.default_foreground;
            run.background      = options.preedit_background;
            frame.text_runs.push_back(run);

            const int caret_column = std::clamp(
                display_width_for_prefix(ime_preedit.text, ime_preedit.cursor_position),
                0,
                column_count);
            const int caret_grid_column =
                std::clamp(column + caret_column, 0, snapshot->grid_size.columns - 1);
            QRectF caret_rect = cell_rect(row, caret_grid_column, 1, cell_metrics);
            caret_rect.setWidth(std::max<qreal>(1.0, cell_metrics.width * 0.12));
            frame.decorations.push_back({
                Terminal_render_decoration_kind::PREEDIT_CARET,
                caret_rect,
                options.default_foreground,
            });
        }
    }

    if (snapshot->metadata.visual_bell_active && options.visual_bell_enabled) {
        frame.overlay_rects.push_back({
            QRectF(QPointF(0.0, 0.0), logical_size),
            options.visual_bell_color,
        });
    }

    frame.stats.background_rects_emitted = static_cast<int>(frame.background_rects.size());
    frame.stats.selection_rects_emitted  = static_cast<int>(frame.selection_rects.size());
    frame.stats.graphic_rects_emitted    = static_cast<int>(frame.graphic_rects.size());
    frame.stats.graphic_arcs_emitted     = static_cast<int>(frame.graphic_arcs.size());
    frame.stats.text_runs_emitted        = static_cast<int>(frame.text_runs.size());
    frame.stats.cursor_text_runs_emitted = static_cast<int>(frame.cursor_text_runs.size());
    frame.stats.decoration_rects_emitted = static_cast<int>(frame.decorations.size());
    frame.stats.cursor_rects_emitted     = static_cast<int>(frame.cursors.size());
    frame.stats.cursor_graphic_rects_emitted =
        static_cast<int>(frame.cursor_graphic_rects.size());
    frame.stats.cursor_graphic_arcs_emitted  =
        static_cast<int>(frame.cursor_graphic_arcs.size());
    frame.stats.overlay_rects_emitted        = static_cast<int>(frame.overlay_rects.size());
    return frame;
}

#if VNM_TERMINAL_PROFILING_ENABLED
QSGNode* Qsg_terminal_renderer::update_node(
    QSGNode*               old_node,
    QQuickWindow*          window,
    const Terminal_render_frame&
                           frame,
    const QFont&           font,
    qreal                  device_pixel_ratio,
    std::shared_ptr<Terminal_renderer_lifecycle_recorder>
                           lifecycle_recorder,
    terminal_renderer_stats_t& out_stats,
    std::shared_ptr<Terminal_text_layout_slow_diagnostics_recorder> slow_text_layout_recorder)
#else
QSGNode* Qsg_terminal_renderer::update_node(
    QSGNode*               old_node,
    QQuickWindow*          window,
    const Terminal_render_frame&
                           frame,
    const QFont&           font,
    qreal                  device_pixel_ratio,
    std::shared_ptr<Terminal_renderer_lifecycle_recorder>
                           lifecycle_recorder,
    terminal_renderer_stats_t& out_stats)
#endif
{
    VNM_TERMINAL_PROFILE_SCOPE("Qsg_terminal_renderer::update_node");

#if VNM_TERMINAL_PROFILING_ENABLED
    const Active_slow_text_layout_recorder_binding slow_text_layout_recorder_binding(
        slow_text_layout_recorder.get());
#endif

    terminal_renderer_stats_t stats;
    out_stats                          = {};
    stats.frame                        = frame.stats;
    stats.route_graphic_geometry_cells = frame.stats.text_cells_rendered_as_graphic;
    stats.frame_background_rects       = static_cast<int>(frame.background_rects.size());
    stats.frame_selection_rects        = static_cast<int>(frame.selection_rects.size());
    stats.frame_graphic_rects          = static_cast<int>(frame.graphic_rects.size());
    stats.frame_graphic_arcs           = static_cast<int>(frame.graphic_arcs.size());
    stats.frame_text_runs              = static_cast<int>(frame.text_runs.size());
    stats.frame_cursor_text_runs       = static_cast<int>(frame.cursor_text_runs.size());
    stats.frame_decorations            = static_cast<int>(frame.decorations.size());
    stats.frame_cursors                = static_cast<int>(frame.cursors.size());
    stats.frame_cursor_graphic_rects   = static_cast<int>(frame.cursor_graphic_rects.size());
    stats.frame_cursor_graphic_arcs    = static_cast<int>(frame.cursor_graphic_arcs.size());
    stats.frame_overlay_rects          = static_cast<int>(frame.overlay_rects.size());
    stats.frame_dirty_row_ranges       = static_cast<int>(frame.dirty_row_ranges.size());
    stats.frame_packed_rows            = static_cast<int>(frame.packed_rows.size());
    stats.frame_packed_text_spans      = static_cast<int>(frame.packed_text_spans.size());
    stats.frame_packed_text_cells      = frame.stats.packed_text_cells;
    stats.frame_packed_graphic_spans   = static_cast<int>(frame.packed_graphic_spans.size());
    stats.frame_packed_graphic_cells   = frame.stats.packed_graphic_cells;
    stats.frame_packed_payload_bytes   = frame.stats.packed_payload_bytes;

    if (window                      == nullptr ||
        !std::isfinite(device_pixel_ratio)     ||
        device_pixel_ratio          <= 0.0     ||
        !frame.logical_size.isValid()          ||
        frame.logical_size.width()  <= 0.0     ||
        frame.logical_size.height() <= 0.0)
    {
        if (old_node != nullptr) {
            stats.qsg_nodes_destroyed += 1 + node_subtree_child_count(*old_node);
        }
        delete old_node;
        out_stats = stats;
        return nullptr;
    }

    Terminal_scene_node* root = dynamic_cast<Terminal_scene_node*>(old_node);
    if (root == nullptr) {
        if (old_node != nullptr) {
            stats.qsg_nodes_destroyed += 1 + node_subtree_child_count(*old_node);
        }
        delete old_node;
        root = new Terminal_scene_node(lifecycle_recorder);
        stats.qsg_nodes_created += 1 + node_subtree_child_count(*root);
    }
    else {
        stats.root_reused = true;
    }

    const bool use_software_graphic_fallback = uses_software_scene_graph(*window);
    const QByteArray background_layer_input_key = flat_rect_layer_key(
        frame.background_rects,
        use_software_graphic_fallback,
        device_pixel_ratio);
    record_rect_cache_key_build(stats, background_layer_input_key);
    const QByteArray selection_layer_input_key = flat_rect_layer_key(
        frame.selection_rects,
        use_software_graphic_fallback,
        device_pixel_ratio);
    record_rect_cache_key_build(stats, selection_layer_input_key);
    const graphic_rect_layer_inputs_t graphic_rect_inputs =
        graphic_rect_layer_inputs(frame);
    const bool graphic_rect_uses_simple_row_nodes =
        use_software_graphic_fallback ||
        graphic_rect_inputs.use_simple_rect_nodes_for_row_rects;
    const QByteArray graphic_rect_layer_input_key = batched_rect_layer_key(
        graphic_rect_inputs.batched_rects,
        graphic_rect_inputs.frame_rects,
        graphic_rect_uses_simple_row_nodes,
        device_pixel_ratio);
    record_rect_cache_key_build(stats, graphic_rect_layer_input_key);
    const QByteArray graphic_arc_layer_input_key = rect_layer_key(
        {},
        frame.graphic_arcs,
        use_software_graphic_fallback,
        device_pixel_ratio);
    record_rect_cache_key_build(stats, graphic_arc_layer_input_key);
    const std::vector<Terminal_render_rect> decoration_rects =
        primitive_rects(frame.decorations);
    const QByteArray decoration_layer_input_key = flat_rect_layer_key(
        decoration_rects,
        use_software_graphic_fallback,
        device_pixel_ratio);
    record_rect_cache_key_build(stats, decoration_layer_input_key);
    stats.background_layer_rebuilt = sync_batched_rect_row_layer(
        *root->background_frame_layer,
        *root->background_row_layer,
        root->background_layer_input_key,
        background_layer_input_key,
        root->background_frame_layer_key,
        root->background_row_slots,
        frame,
        frame.background_rects,
        use_software_graphic_fallback,
        device_pixel_ratio,
        lifecycle_recorder,
        k_background_batched_rect_row_layer,
        stats);
    stats.selection_layer_rebuilt  = sync_batched_rect_row_layer(
        *root->selection_frame_layer,
        *root->selection_row_layer,
        root->selection_layer_input_key,
        selection_layer_input_key,
        root->selection_frame_layer_key,
        root->selection_row_slots,
        frame,
        frame.selection_rects,
        use_software_graphic_fallback,
        device_pixel_ratio,
        lifecycle_recorder,
        k_selection_batched_rect_row_layer,
        stats);
    const bool graphic_rect_layer_rebuilt = sync_batched_rect_row_layer(
        *root->graphic_rect_frame_layer,
        *root->graphic_rect_row_layer,
        root->graphic_rect_layer_input_key,
        graphic_rect_layer_input_key,
        root->graphic_rect_frame_layer_key,
        root->graphic_rect_row_slots,
        frame,
        graphic_rect_inputs.batched_rects,
        graphic_rect_uses_simple_row_nodes,
        device_pixel_ratio,
        lifecycle_recorder,
        k_graphic_batched_rect_row_layer,
        stats,
        &graphic_rect_inputs.frame_rects);
    const bool graphic_arc_layer_rebuilt = sync_arc_row_layer(
        *root->graphic_arc_frame_layer,
        *root->graphic_arc_row_layer,
        root->graphic_arc_layer_input_key,
        graphic_arc_layer_input_key,
        root->graphic_arc_frame_layer_key,
        root->graphic_arc_row_slots,
        frame,
        frame.graphic_arcs,
        use_software_graphic_fallback,
        device_pixel_ratio,
        lifecycle_recorder,
        stats,
        stats.graphic_arc_rows_rebuilt,
        stats.graphic_arc_rows_reused,
        stats.graphic_arc_row_clean_reuse_skips,
        stats.graphic_arc_rows_removed,
        stats.graphic_arc_row_cache_fallbacks);
    stats.graphic_layer_rebuilt =
        graphic_rect_layer_rebuilt || graphic_arc_layer_rebuilt;
    sync_text_resource_nodes(
        *root,
        *window,
        frame,
        font,
        root->text_coalescing_context_cache,
        stats,
        lifecycle_recorder);
    stats.decoration_layer_rebuilt     = sync_batched_rect_row_layer(
        *root->decoration_frame_layer,
        *root->decoration_row_layer,
        root->decoration_layer_input_key,
        decoration_layer_input_key,
        root->decoration_frame_layer_key,
        root->decoration_row_slots,
        frame,
        decoration_rects,
        use_software_graphic_fallback,
        device_pixel_ratio,
        lifecycle_recorder,
        k_decoration_batched_rect_row_layer,
        stats);
    stats.cursor_layer_rebuilt         = sync_rect_layer(
        *root->cursor_layer,
        root->cursor_layer_key,
        primitive_rects(frame.cursors),
        {},
        false,
        1.0,
        &stats);
    stats.cursor_graphic_layer_rebuilt = sync_rect_layer(
        *root->cursor_graphic_layer,
        root->cursor_graphic_layer_key,
        frame.cursor_graphic_rects,
        frame.cursor_graphic_arcs,
        use_software_graphic_fallback,
        device_pixel_ratio,
        &stats);
    stats.cursor_text_layer_rebuilt    = sync_cursor_text_layer(
        *root,
        *window,
        frame,
        font,
        stats,
        lifecycle_recorder);
    stats.overlay_layer_rebuilt        = sync_rect_layer(
        *root->overlay_layer,
        root->overlay_layer_key,
        frame.overlay_rects,
        {},
        false,
        1.0,
        &stats);
    const int text_row_cache_hits =
        stats.text_content_reused - stats.text_clean_reuse_skips;
    stats.row_cache_hits =
        text_row_cache_hits +
        stats.background_rows_reused +
        stats.selection_rows_reused +
        stats.decoration_rows_reused +
        stats.graphic_rect_rows_reused +
        stats.graphic_arc_rows_reused;
    stats.row_cache_clean_skips =
        stats.text_clean_reuse_skips +
        stats.background_row_clean_reuse_skips +
        stats.selection_row_clean_reuse_skips +
        stats.decoration_row_clean_reuse_skips +
        stats.graphic_rect_row_clean_reuse_skips +
        stats.graphic_arc_row_clean_reuse_skips;
    stats.paint_completed = stats.text_content_failures == 0;
    out_stats = stats;
    return root;
}

}
