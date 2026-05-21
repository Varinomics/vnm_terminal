#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/qsg_terminal_render_frame.h"
#include "vnm_terminal/internal/qsg_terminal_renderer.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/vnm_terminal_surface.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QIODevice>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGRendererInterface>
#include <QSGSimpleRectNode>
#include <QSGTransformNode>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

#ifndef VNM_TERMINAL_QSG_RENDERER_SOURCE_PATHS
#define VNM_TERMINAL_QSG_RENDERER_SOURCE_PATHS ""
#endif

using vnm_terminal::test_helpers::check;
using Renderer_lifecycle_recorder     = term::Terminal_renderer_lifecycle_recorder;
using Renderer_lifecycle_recorder_ptr = std::shared_ptr<Renderer_lifecycle_recorder>;

bool profile_scopes_available()
{
#if VNM_TERMINAL_PROFILING_ENABLED
    return true;
#else
    return false;
#endif
}

bool has_argument(int argc, char** argv, const char* expected)
{
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], expected) == 0) {
            return true;
        }
    }

    return false;
}

bool check_no_text_content_failures(
    const term::terminal_renderer_stats_t& stats,
    const char*                            message)
{
    return check(stats.text_content_failures == 0, message);
}

bool check_surface_no_text_content_failures(
    const VNM_TerminalSurface&             surface,
    const char*                            message)
{
    return
        check_no_text_content_failures(
            term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface),
            message);
}

bool source_rect_layer_key_guards_device_pixel_ratio(const QByteArray& source)
{
    const QByteArray function_marker      = QByteArrayLiteral("QByteArray rect_layer_key(");
    const QByteArray next_function_marker = QByteArrayLiteral("\nbool sync_rect_layer(");
    const QByteArray key_decision         =
        QByteArrayLiteral("layer_uses_software_rasterization(");
    const QByteArray guard = QByteArrayLiteral("if (include_software_rasterization_key)");
    const QByteArray dpr_append =
        QByteArrayLiteral("append_cache_key_value(key, device_pixel_ratio);");
    const QByteArray function_return = QByteArrayLiteral("return key;");

    const qsizetype function_start = source.indexOf(function_marker);
    if (function_start < 0) {
        return false;
    }

    const qsizetype next_function_start = source.indexOf(
        next_function_marker,
        function_start);
    if (next_function_start < 0) {
        return false;
    }

    const QByteArray body         = source.mid(function_start, next_function_start - function_start);
    const qsizetype  decision_pos = body.indexOf(key_decision);
    const qsizetype  guard_pos    = body.indexOf(guard);
    const qsizetype  dpr_pos      = body.indexOf(dpr_append);
    const qsizetype  return_pos   = body.indexOf(function_return);
    return
        decision_pos >= 0                                                    &&
        guard_pos                                             > decision_pos &&
        dpr_pos                                               > guard_pos    &&
        return_pos                                            > dpr_pos      &&
        body.indexOf(dpr_append, dpr_pos + dpr_append.size()) < 0;
}

bool test_qsg_renderer_source_posture()
{
    const QStringList source_paths =
        QString::fromUtf8(VNM_TERMINAL_QSG_RENDERER_SOURCE_PATHS)
            .split(QLatin1Char('|'), Qt::SkipEmptyParts);
    if (source_paths.isEmpty()) {
        std::cerr << "FAIL: renderer source posture test has no source paths\n";
        return false;
    }

    bool ok = true;

    bool found_primary_renderer           = false;
    bool found_qsg_text_node              = false;
    bool found_qtext_layout               = false;
    bool found_guarded_rect_layer_dpr_key = false;
    const std::vector<QByteArray> forbidden_tokens = {
        QByteArrayLiteral("QImage"),
        QByteArrayLiteral("QPainter"),
        QByteArrayLiteral("QSGSimpleTextureNode"),
        QByteArrayLiteral("QSGTexture"),
        QByteArrayLiteral("createTextureFromImage"),
    };

    for (const QString& source_path : source_paths) {
        QFile source_file(source_path);
        if (!source_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::cerr << "FAIL: could not open renderer source posture file: "
                << source_path.toLocal8Bit().constData() << '\n';
            ok = false;
            continue;
        }

        const QByteArray source = source_file.readAll();
        for (const QByteArray& token : forbidden_tokens) {
            if (!source.contains(token)) {
                continue;
            }

            std::cerr << "FAIL: " << source_path.toLocal8Bit().constData()
                << " contains forbidden production text token: "
                << token.constData() << '\n';
            ok = false;
        }

        if (!source_path.endsWith(QStringLiteral("src/qsg_terminal_renderer.cpp")) &&
            !source_path.endsWith(QStringLiteral("src\\qsg_terminal_renderer.cpp")))
        {
            continue;
        }

        found_primary_renderer = true;
        found_qsg_text_node    = source.contains(QByteArrayLiteral("QSGTextNode"));
        found_qtext_layout     = source.contains(QByteArrayLiteral("QTextLayout"));
        found_guarded_rect_layer_dpr_key =
            source_rect_layer_key_guards_device_pixel_ratio(source);
    }

    if (!found_primary_renderer) {
        std::cerr << "FAIL: renderer source posture did not scan src/qsg_terminal_renderer.cpp\n";
        ok = false;
    }
    if (!found_qsg_text_node) {
        std::cerr << "FAIL: qsg_terminal_renderer.cpp does not contain QSGTextNode\n";
        ok = false;
    }
    if (!found_qtext_layout) {
        std::cerr << "FAIL: qsg_terminal_renderer.cpp does not contain QTextLayout\n";
        ok = false;
    }
    if (!found_guarded_rect_layer_dpr_key) {
        std::cerr << "FAIL: rect_layer_key does not guard device-pixel-ratio key fields "
                     "behind the software rasterization key\n";
        ok = false;
    }

    return ok;
}

void configure_test_font(VNM_TerminalSurface& surface)
{
    surface.set_font_family(QString());
    surface.set_font_size(16.0);
}

term::terminal_cell_metrics_t test_metrics_for_font_size(qreal font_size)
{
    term::Qt_grid_metrics_provider provider(
        term::vnm_terminal_font(QString(), font_size),
        1.0);
    return provider.cell_metrics();
}

term::terminal_cell_metrics_t test_metrics()
{
    return test_metrics_for_font_size(16.0);
}

term::Terminal_render_text_run make_direct_text_run_for_row(
    int                            row,
    int                            logical_row,
    int                            column,
    int                            cell_count,
    const QString&                 text,
    term::terminal_cell_metrics_t  metrics,
    QColor                         foreground = QColor(196, 230, 201));

quint32 viewport_scroll_foreground_rgba(
    int                            logical_row);

term::Terminal_color_state color_state()
{
    term::Terminal_color_state state;
    state.default_foreground_rgba = 0xffc4e6c9U;
    state.default_background_rgba = 0xff090c10U;
    state.cursor_rgba             = 0xffffffffU;
    for (std::size_t i = 0; i < state.palette_rgba.size(); ++i) {
        state.palette_rgba[i] = 0xff202020U + static_cast<quint32>(i);
    }
    return state;
}

std::shared_ptr<const term::Terminal_render_snapshot> make_snapshot(
    bool                   visual_bell,
    std::vector<term::Terminal_render_dirty_row_range>
                           dirty_row_ranges = {},
    QString                row_one_suffix = QStringLiteral("1"),
    std::uint64_t          sequence = 0U)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 5;

    const std::uint64_t snapshot_sequence =
        sequence != 0U ? sequence : (visual_bell ? 201U : 200U);
    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({5, 20}, viewport, snapshot_sequence);
    snapshot.color_state = color_state();
    snapshot.cursor      = {{2, 2}, term::Terminal_cursor_shape::BLOCK, true, true};
    snapshot.selection_spans.push_back({
        { {1, 1}, {1, 6}, term::Terminal_selection_mode::NORMAL },
        1,
        1,
        5,
    });
    snapshot.cells.push_back({{0, 0}, QStringLiteral("H"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral("i"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 0}, QStringLiteral("R"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 1}, std::move(row_one_suffix), 0U, 1, false, 0U});
    snapshot.cells.push_back({{2, 2}, QStringLiteral("X"), 0U, 1, false, 0U});
    snapshot.dirty_row_ranges = std::move(dirty_row_ranges);
    snapshot.metadata.visual_bell_active = visual_bell;
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_dense_snapshot(
    std::uint64_t  sequence,
    const QString& marker)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 5;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({5, 20}, viewport, sequence);
    snapshot.color_state      = color_state();
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = {{0, 5}};
    for (int row = 0; row < 5; ++row) {
        snapshot.cells.push_back({
            { row, 0 },
            marker,
            0U,
            1,
            false,
            0U,
        });
    }
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_coalesced_dirty_text_snapshot(
    std::uint64_t          sequence,
    int                    green_row_count,
    std::vector<term::Terminal_render_dirty_row_range>
                           dirty_row_ranges)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 5;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({5, 20}, viewport, sequence);
    snapshot.color_state = color_state();
    snapshot.cursor.visible = false;

    term::Terminal_text_style red_style = term::make_default_terminal_text_style();
    red_style.foreground = term::make_rgb_terminal_color_ref(0xffff3030U);
    snapshot.styles.push_back(red_style);

    term::Terminal_text_style green_style = term::make_default_terminal_text_style();
    green_style.foreground = term::make_rgb_terminal_color_ref(0xff30ff60U);
    snapshot.styles.push_back(green_style);

    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        const term::Terminal_style_id style_id = row < green_row_count ? 2U : 1U;
        for (int column = 0; column < snapshot.grid_size.columns; ++column) {
            snapshot.cells.push_back({
                { row, column },
                QStringLiteral("M"),
                0U,
                1,
                false,
                style_id,
            });
        }
    }

    snapshot.dirty_row_ranges = std::move(dirty_row_ranges);
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_dense_same_color_row_snapshot(
    int            column_count,
    std::uint64_t  sequence)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 3;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({3, column_count}, viewport, sequence);
    snapshot.color_state      = color_state();
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = {{0, 1}};
    for (int column = 0; column < column_count; ++column) {
        snapshot.cells.push_back({
            { 0, column },
            QString(QChar(static_cast<char16_t>(u'A' + column % 26))),
            0U,
            1,
            false,
            0U,
        });
    }
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_dense_same_color_row_snapshot()
{
    return make_dense_same_color_row_snapshot(20, 307U);
}

std::shared_ptr<const term::Terminal_render_snapshot> make_multirow_dense_same_color_snapshot(
    int            row_count,
    int            column_count,
    std::uint64_t  sequence)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = row_count;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({row_count, column_count}, viewport, sequence);
    snapshot.color_state      = color_state();
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = {{0, row_count}};
    for (int row = 0; row < row_count; ++row) {
        for (int column = 0; column < column_count; ++column) {
            snapshot.cells.push_back({
                { row, column },
                QString(QChar(static_cast<char16_t>(u'A' + column % 26))),
                0U,
                1,
                false,
                0U,
            });
        }
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_dense_style_boundary_row_snapshot()
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 3;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({3, 20}, viewport, 312U);
    snapshot.color_state      = color_state();
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = {{0, 1}};

    term::Terminal_text_style accent_style = term::make_default_terminal_text_style();
    accent_style.foreground = term::make_rgb_terminal_color_ref(0xffff6060U);
    snapshot.styles.push_back(accent_style);

    for (int column = 0; column < 20; ++column) {
        snapshot.cells.push_back({
            { 0, column },
            QString(QChar(static_cast<char16_t>(u'A' + column % 26))),
            0U,
            1,
            false,
            column == 10 ? 1U : 0U,
        });
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_dense_same_foreground_style_boundary_row_snapshot()
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 3;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({3, 20}, viewport, 317U);
    snapshot.color_state      = color_state();
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = {{0, 1}};

    term::Terminal_text_style highlighted_style = term::make_default_terminal_text_style();
    highlighted_style.background = term::make_rgb_terminal_color_ref(0xff204060U);
    term::set_terminal_style_attribute(
        highlighted_style,
        term::Terminal_style_attribute::UNDERLINE);
    snapshot.styles.push_back(highlighted_style);

    for (int column = 0; column < 20; ++column) {
        snapshot.cells.push_back({
            { 0, column },
            QString(QChar(static_cast<char16_t>(u'A' + column % 26))),
            0U,
            1,
            false,
            column == 10 ? 1U : 0U,
        });
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_dense_same_foreground_style_transition_snapshot(
    std::uint64_t  sequence,
    bool           highlighted)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 3;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({3, 20}, viewport, sequence);
    snapshot.color_state      = color_state();
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = {{0, 1}};

    term::Terminal_text_style highlighted_style = term::make_default_terminal_text_style();
    highlighted_style.background = term::make_rgb_terminal_color_ref(0xff204060U);
    term::set_terminal_style_attribute(
        highlighted_style,
        term::Terminal_style_attribute::UNDERLINE);
    snapshot.styles.push_back(highlighted_style);

    for (int column = 0; column < 20; ++column) {
        snapshot.cells.push_back({
            { 0, column },
            QString(QChar(static_cast<char16_t>(u'K' + column % 10))),
            0U,
            1,
            false,
            highlighted ? 1U : 0U,
        });
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_dense_wide_boundary_row_snapshot()
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 3;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({3, 20}, viewport, 313U);
    snapshot.color_state      = color_state();
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = {{0, 1}};

    for (int column = 0; column < 10; ++column) {
        snapshot.cells.push_back({
            { 0, column },
            QString(QChar(static_cast<char16_t>(u'A' + column % 26))),
            0U,
            1,
            false,
            0U,
        });
    }
    snapshot.cells.push_back({{0, 10}, QStringLiteral("\u754c"), 0U, 2, false, 0U});
    snapshot.cells.push_back({{0, 11}, {}, 0U, 0, true, 0U});
    for (int column = 12; column < 20; ++column) {
        snapshot.cells.push_back({
            { 0, column },
            QString(QChar(static_cast<char16_t>(u'A' + column % 26))),
            0U,
            1,
            false,
            0U,
        });
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_row_clip_stress_snapshot(
    std::uint64_t sequence)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 4;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({4, 40}, viewport, sequence);
    snapshot.color_state      = color_state();
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = {{0, 4}};

    constexpr int text_row = 1;
    const QString text     = QStringLiteral("can you describe its implementation?");
    const QString below_marks =
        QString(QChar(static_cast<char16_t>(0x0323))) +
        QString(QChar(static_cast<char16_t>(0x0324))) +
        QString(QChar(static_cast<char16_t>(0x0331))) +
        QString(QChar(static_cast<char16_t>(0x0332))) +
        QString(QChar(static_cast<char16_t>(0x0333))) +
        QString(QChar(static_cast<char16_t>(0x0347)));
    for (int column = 0; column < text.size(); ++column) {
        snapshot.cells.push_back({
            { text_row, column },
            QString(text.at(column)) + below_marks,
            0U,
            1,
            false,
            0U,
        });
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_terminal_graphics_snapshot(
    std::uint64_t          sequence = 308U,
    QString                row_two_text = QStringLiteral("A"),
    std::vector<term::Terminal_render_dirty_row_range>
                           dirty_row_ranges = {{0, 3}})
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 3;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({3, 20}, viewport, sequence);
    snapshot.color_state      = color_state();
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = std::move(dirty_row_ranges);
    snapshot.cells.push_back({{0, 0}, QStringLiteral("\u250c"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral("\u2500"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 2}, QStringLiteral("\u2510"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 3}, QStringLiteral("\u256d"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 0}, QStringLiteral("\u2502"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 1}, QStringLiteral("\u2588"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 2}, QStringLiteral("\u2598"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{2, 0}, std::move(row_two_text), 0U, 1, false, 0U});
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_cursor_over_terminal_graphic_snapshot()
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 3;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({3, 20}, viewport, 309U);
    snapshot.color_state      = color_state();
    snapshot.cursor           = {{0, 1}, term::Terminal_cursor_shape::BLOCK, true, false};
    snapshot.dirty_row_ranges = {{0, 1}};
    snapshot.cells.push_back({{0, 0}, QStringLiteral("A"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral("\u2500"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 2}, QStringLiteral("B"), 0U, 1, false, 0U});
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

quint32 viewport_scroll_foreground_rgba(int logical_row)
{
    switch (logical_row % 4) {
        case 0:  return 0xffff5050U;
        case 1:  return 0xff50ff50U;
        case 2:  return 0xff5050ffU;
        default: return 0xffffff50U;
    }
}

bool is_viewport_scroll_row_color(QColor color, int logical_row)
{
    switch (logical_row % 4) {
        case 0:
            return
                color.red() > 110                &&
                color.red() > color.green() + 30 &&
                color.red() > color.blue() + 30;
        case 1:
            return
                color.green() > 110              &&
                color.green() > color.red() + 30 &&
                color.green() > color.blue() + 30;
        case 2:
            return
                color.blue() > 110              &&
                color.blue() > color.red() + 30 &&
                color.blue() > color.green() + 30;
        default:
            return
                color.red()   > 110 &&
                color.green() > 110 &&
                color.blue()  < 120;
    }
}

std::shared_ptr<const term::Terminal_render_snapshot> make_viewport_scroll_dense_snapshot(
    std::uint64_t  sequence,
    int            first_visible_logical_row)
{
    constexpr int visible_rows    = 5;
    constexpr int column_count    = 20;
    constexpr int scrollback_rows = 12;

    term::Terminal_viewport_state viewport;
    viewport.visible_rows     = visible_rows;
    viewport.scrollback_rows  = scrollback_rows;
    viewport.offset_from_tail = scrollback_rows - first_visible_logical_row;
    viewport.follow_tail      = false;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({visible_rows, column_count}, viewport, sequence);
    snapshot.color_state      = color_state();
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = {{0, visible_rows}};
    for (int logical_row = 0; logical_row <= scrollback_rows; ++logical_row) {
        term::Terminal_text_style style = term::make_default_terminal_text_style();
        style.foreground =
            term::make_rgb_terminal_color_ref(viewport_scroll_foreground_rgba(logical_row));
        snapshot.styles.push_back(style);
    }
    for (int row = 0; row < visible_rows; ++row) {
        const int  logical_row = first_visible_logical_row + row;
        const auto row_char    = static_cast<char16_t>(u'A' + logical_row % 26);
        for (int column = 0; column < column_count; ++column) {
            snapshot.cells.push_back({
                { row, column },
                QString(QChar(row_char)),
                0U,
                1,
                false,
                static_cast<term::Terminal_style_id>(logical_row + 1),
            });
        }
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

quint32 row_cache_background_rgba(int logical_row)
{
    switch (logical_row % 3) {
        case 0:  return 0xff7a2038U;
        case 1:  return 0xff207a38U;
        default: return 0xff7a5c20U;
    }
}

bool is_row_cache_background_color(QColor color, int logical_row)
{
    switch (logical_row % 3) {
        case 0:
            return
                color.red()   > 90 &&
                color.green() < 80 &&
                color.blue()  < 90;
        case 1:
            return
                color.green() > 90 &&
                color.red()   < 80 &&
                color.blue()  < 90;
        default:
            return
                color.red()   > 90 &&
                color.green() > 70 &&
                color.blue()  < 80;
    }
}

quint32 grid_resize_background_rgba(int logical_row, int phase)
{
    switch ((logical_row + phase * 2) % 5) {
        case 0:  return 0xff245a76U;
        case 1:  return 0xff705030U;
        case 2:  return 0xff245f38U;
        case 3:  return 0xff672c55U;
        default: return 0xff6a6430U;
    }
}

quint32 grid_resize_graphic_rgba(int logical_row, int phase)
{
    switch ((logical_row + phase) % 4) {
        case 0:  return 0xffe86a5cU;
        case 1:  return 0xff68d07cU;
        case 2:  return 0xff70a4f0U;
        default: return 0xffe0d45cU;
    }
}

bool color_is_close(QColor color, QColor expected)
{
    constexpr int tolerance = 4;
    return
        std::abs(color.red() - expected.red())     <= tolerance &&
        std::abs(color.green() - expected.green()) <= tolerance &&
        std::abs(color.blue() - expected.blue())   <= tolerance;
}

bool is_grid_resize_background_color(QColor color, int logical_row, int phase)
{
    return color_is_close(
        color,
        QColor::fromRgba(grid_resize_background_rgba(logical_row, phase)));
}

bool is_grid_resize_graphic_color(QColor color, int logical_row, int phase)
{
    return color_is_close(
        color,
        QColor::fromRgba(grid_resize_graphic_rgba(logical_row, phase)));
}

std::shared_ptr<const term::Terminal_render_snapshot>
make_row_cache_scroll_snapshot(
    std::uint64_t  sequence,
    int            first_visible_logical_row)
{
    constexpr int visible_rows         = 3;
    constexpr int column_count         = 8;
    constexpr int scrollback_rows      = 6;
    constexpr int selected_logical_row = 2;

    term::Terminal_viewport_state viewport;
    viewport.visible_rows     = visible_rows;
    viewport.scrollback_rows  = scrollback_rows;
    viewport.offset_from_tail = scrollback_rows - first_visible_logical_row;
    viewport.follow_tail      = false;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({visible_rows, column_count}, viewport, sequence);
    snapshot.color_state      = color_state();
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = {{0, visible_rows}};
    snapshot.styles.resize(static_cast<std::size_t>(scrollback_rows + visible_rows + 2));
    const int style_logical_rows = static_cast<int>(snapshot.styles.size()) - 1;
    for (int logical_row = 0; logical_row < style_logical_rows; ++logical_row) {
        term::Terminal_text_style style           = term::make_default_terminal_text_style();
        const quint32             background_rgba = row_cache_background_rgba(logical_row);
        style.foreground                          = term::make_rgb_terminal_color_ref(background_rgba);
        style.background                          = term::make_rgb_terminal_color_ref(background_rgba);
        snapshot.styles[static_cast<std::size_t>(logical_row + 1)] = style;
    }

    for (int row = 0; row < visible_rows; ++row) {
        const int logical_row = first_visible_logical_row + row;
        for (int column = 0; column < column_count; ++column) {
            snapshot.cells.push_back({
                { row, column },
                QString(),
                0U,
                1,
                false,
                static_cast<term::Terminal_style_id>(logical_row + 1),
            });
        }
    }

    const int selected_viewport_row = selected_logical_row - first_visible_logical_row;
    if (selected_viewport_row >= 0 && selected_viewport_row < visible_rows) {
        snapshot.selection_spans.push_back({
            { {selected_logical_row, 2}, {selected_logical_row, 5}, term::Terminal_selection_mode::NORMAL },
            selected_viewport_row,
            2,
            3,
        });
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_grid_resize_cached_rows_snapshot(
    std::uint64_t  sequence,
    int            rows,
    int            columns,
    int            first_visible_logical_row,
    int            phase)
{
    constexpr int background_sample_column = 10;
    constexpr int graphic_sample_column    = 1;
    constexpr int scrollback_rows          = 260;

    term::Terminal_viewport_state viewport;
    viewport.visible_rows     = rows;
    viewport.scrollback_rows  = scrollback_rows;
    viewport.offset_from_tail = scrollback_rows - first_visible_logical_row;
    viewport.follow_tail      = false;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({rows, columns}, viewport, sequence);
    snapshot.color_state      = color_state();
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = {{0, rows}};
    snapshot.styles.reserve(static_cast<std::size_t>(rows * 3 + 1));
    snapshot.cells.reserve(static_cast<std::size_t>(rows * 3));

    const int text_sample_column = columns - 3;
    for (int row = 0; row < rows; ++row) {
        const int logical_row = first_visible_logical_row + row;

        term::Terminal_text_style background_style = term::make_default_terminal_text_style();
        background_style.foreground = term::make_rgb_terminal_color_ref(0xfff4f4e8U);
        background_style.background = term::make_rgb_terminal_color_ref(
            grid_resize_background_rgba(logical_row, phase));
        const term::Terminal_style_id background_style_id = static_cast<term::Terminal_style_id>(
            snapshot.styles.size());
        snapshot.styles.push_back(background_style);

        term::Terminal_text_style graphic_style = background_style;
        graphic_style.foreground =
            term::make_rgb_terminal_color_ref(grid_resize_graphic_rgba(logical_row, phase));
        const term::Terminal_style_id graphic_style_id =
            static_cast<term::Terminal_style_id>(snapshot.styles.size());
        snapshot.styles.push_back(graphic_style);

        term::Terminal_text_style text_style = term::make_default_terminal_text_style();
        text_style.foreground = term::make_rgb_terminal_color_ref(0xfff4f4e8U);
        const term::Terminal_style_id text_style_id =
            static_cast<term::Terminal_style_id>(snapshot.styles.size());
        snapshot.styles.push_back(text_style);

        snapshot.cells.push_back({
            { row, background_sample_column },
            QString(),
            0U,
            1,
            false,
            background_style_id,
        });
        snapshot.cells.push_back({
            { row, graphic_sample_column },
            QStringLiteral("\u2588"),
            0U,
            1,
            false,
            graphic_style_id,
        });
        snapshot.cells.push_back({
            { row, text_sample_column },
            QStringLiteral("Z"),
            0U,
            1,
            false,
            text_style_id,
        });
    }

    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_shrunk_snapshot()
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 2;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({2, 20}, viewport, 202U);
    snapshot.color_state = color_state();
    snapshot.cells.push_back({{0, 0}, QStringLiteral("H"), 0U, 1, false, 0U});
    snapshot.dirty_row_ranges = {{0, 2}};
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_sparse_snapshot()
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 5;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({5, 20}, viewport, 203U);
    snapshot.color_state = color_state();
    snapshot.cells.push_back({{0, 0}, QStringLiteral("H"), 0U, 1, false, 0U});
    snapshot.dirty_row_ranges = {{0, 5}};
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_foreground_only_blank_snapshot()
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 5;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({5, 20}, viewport, 204U);
    snapshot.color_state = color_state();
    snapshot.cursor.visible = false;
    term::Terminal_text_style foreground_style = term::make_default_terminal_text_style();
    foreground_style.foreground = term::make_palette_terminal_color_ref(1U);
    snapshot.styles.push_back(foreground_style);
    for (int column = 0; column < snapshot.grid_size.columns; ++column) {
        snapshot.cells.push_back({
            { 0, column },
            QString(),
            0U,
            1,
            false,
            1U,
        });
    }
    snapshot.dirty_row_ranges = {{0, 1}};
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_blank_text_lifecycle_snapshot(
    std::uint64_t sequence)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 3;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({3, 20}, viewport, sequence);
    snapshot.color_state      = color_state();
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = {{0, 3}};
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot>
make_terminal_graphics_foreground_blank_snapshot(std::uint64_t sequence)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 3;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({3, 20}, viewport, sequence);
    snapshot.color_state = color_state();
    snapshot.cursor.visible = false;

    term::Terminal_text_style foreground_style = term::make_default_terminal_text_style();
    foreground_style.foreground = term::make_rgb_terminal_color_ref(0xffff4040U);
    snapshot.styles.push_back(foreground_style);
    for (int column = 0; column < snapshot.grid_size.columns; ++column) {
        snapshot.cells.push_back({
            { 0, column },
            QString(),
            0U,
            1,
            false,
            1U,
        });
    }
    snapshot.cells.push_back({{2, 0}, QStringLiteral("A"), 0U, 1, false, 0U});
    snapshot.dirty_row_ranges = {{0, 1}};
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_unicode_snapshot(
    std::uint64_t                                      sequence,
    std::vector<term::Terminal_render_cell>            cells,
    std::vector<term::Terminal_render_dirty_row_range> dirty_row_ranges = {{0, 1}})
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 3;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({3, 20}, viewport, sequence);
    snapshot.color_state      = color_state();
    snapshot.cursor.visible   = false;
    snapshot.cells            = std::move(cells);
    snapshot.dirty_row_ranges = std::move(dirty_row_ranges);
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_wide_cursor_snapshot(
    int            cursor_column = 1,
    std::uint64_t  sequence = 306U)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 3;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({3, 12}, viewport, sequence);
    snapshot.color_state = color_state();
    snapshot.cursor      = {{0, cursor_column}, term::Terminal_cursor_shape::BLOCK, true, false};
    snapshot.cells.push_back({{0, 0}, QStringLiteral("A"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral("\u754c"), 0U, 2, false, 0U});
    snapshot.cells.push_back({{0, 2}, {}, 0U, 0, true, 0U});
    snapshot.cells.push_back({{0, 4}, QStringLiteral("Z"), 0U, 1, false, 0U});
    snapshot.dirty_row_ranges = {{0, 1}};
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_block_cursor_text_snapshot(
    std::uint64_t          sequence,
    int                    cursor_column,
    std::vector<term::Terminal_render_dirty_row_range>
                           dirty_row_ranges)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 2;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({2, 8}, viewport, sequence);
    snapshot.color_state = color_state();
    snapshot.cursor      = {{0, cursor_column}, term::Terminal_cursor_shape::BLOCK, true, false};
    snapshot.cells.push_back({{0, 0}, QStringLiteral("A"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral("B"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 2}, QStringLiteral("C"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 3}, QStringLiteral("D"), 0U, 1, false, 0U});
    snapshot.dirty_row_ranges = std::move(dirty_row_ranges);
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_complex_cursor_text_snapshot(
    std::uint64_t sequence)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 2;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({2, 8}, viewport, sequence);
    snapshot.color_state = color_state();
    snapshot.cursor      = {{0, 1}, term::Terminal_cursor_shape::BLOCK, true, false};
    snapshot.cells.push_back({{0, 0}, QStringLiteral("A"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral("e\u0301"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 2}, QStringLiteral("Z"), 0U, 1, false, 0U});
    snapshot.dirty_row_ranges = {{0, 1}};
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_preedit_snapshot(
    std::uint64_t                                      sequence,
    QString                                            preedit_text,
    int                                                preedit_cursor_position,
    std::vector<term::Terminal_render_dirty_row_range> dirty_row_ranges,
    bool                                               overlap_selection)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 3;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({3, 8}, viewport, sequence);
    snapshot.color_state                 = color_state();
    snapshot.cursor.visible              = false;
    snapshot.cursor.position             = {1, 1};
    snapshot.ime_preedit.active          = true;
    snapshot.ime_preedit.text            = std::move(preedit_text);
    snapshot.ime_preedit.cursor_position = preedit_cursor_position;
    snapshot.cells.push_back({{1, 0}, QStringLiteral("A"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 1}, QStringLiteral("B"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 2}, QStringLiteral("C"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 3}, QStringLiteral("D"), 0U, 1, false, 0U});
    if (overlap_selection) {
        snapshot.selection_spans.push_back({
            { {1, 0}, {1, 4}, term::Terminal_selection_mode::NORMAL },
            1,
            0,
            4,
        });
    }
    snapshot.dirty_row_ranges = std::move(dirty_row_ranges);
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

std::shared_ptr<const term::Terminal_render_snapshot> make_preedit_cleared_snapshot(
    std::uint64_t                                      sequence,
    std::vector<term::Terminal_render_dirty_row_range> dirty_row_ranges)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 3;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({3, 8}, viewport, sequence);
    snapshot.color_state     = color_state();
    snapshot.cursor.visible  = false;
    snapshot.cursor.position = {1, 1};
    snapshot.cells.push_back({{1, 0}, QStringLiteral("A"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 1}, QStringLiteral("B"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 2}, QStringLiteral("C"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 3}, QStringLiteral("D"), 0U, 1, false, 0U});
    snapshot.dirty_row_ranges = std::move(dirty_row_ranges);
    return std::make_shared<const term::Terminal_render_snapshot>(std::move(snapshot));
}

void pump_events(QGuiApplication& app, int rounds = 8)
{
    for (int i = 0; i < rounds; ++i) {
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }
}

QImage render_window_until(
    QGuiApplication&                           app,
    QQuickWindow&                              window,
    const std::function<bool(const QImage&)>&  predicate)
{
    QImage image;
    for (int i = 0; i < 30; ++i) {
        pump_events(app, 1);
        image = window.grabWindow();
        if (!image.isNull() && predicate(image)) {
            return image;
        }
    }

    return image;
}

bool window_render_matches(
    QGuiApplication&                           app,
    QQuickWindow&                              window,
    const std::function<bool(const QImage&)>&  predicate)
{
    for (int i = 0; i < 30; ++i) {
        pump_events(app, 1);
        const QImage image = window.grabWindow();
        if (!image.isNull() && predicate(image)) {
            return true;
        }
    }

    return false;
}

bool wait_rendered_sequence_with_text_rebuilds(
    QGuiApplication&                   app,
    QQuickWindow&                      window,
    const VNM_TerminalSurface&         surface,
    std::uint64_t                      sequence,
    int                                text_content_rebuilds)
{
    return window_render_matches(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t render_stats =
            term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
        return
            invalidation_stats.last_rendered_snapshot_sequence == sequence &&
            !invalidation_stats.pending_update                             &&
            render_stats.paint_completed                                   &&
            render_stats.text_content_failures == 0                        &&
            render_stats.text_content_rebuilds == text_content_rebuilds;
    });
}

std::uint64_t profile_call_count(
    const term::Profile_node_snapshot& node,
    const std::string&                 name)
{
    std::uint64_t count = node.name == name ? node.call_count : 0U;
    for (const term::Profile_node_snapshot& child : node.children) {
        count += profile_call_count(child, name);
    }
    return count;
}

std::uint64_t prepare_text_layout_call_count(const VNM_TerminalSurface& surface)
{
    const term::Render_profile_snapshot_t profile = term::VNM_TerminalSurface_render_bridge::render_profiler_snapshot(
        surface);
    return profile_call_count(profile.root, "prepare_text_layout");
}

std::uint64_t ascii_text_coalescing_context_call_count(const VNM_TerminalSurface& surface)
{
    const term::Render_profile_snapshot_t profile = term::VNM_TerminalSurface_render_bridge::render_profiler_snapshot(
        surface);
    return profile_call_count(profile.root, "ascii_text_coalescing_context");
}

term::terminal_cell_metrics_t cell_metrics_for_font(const QFont& font)
{
    const QFontMetricsF metrics(font);
    return {
        metrics.horizontalAdvance(QLatin1Char('M')),
        metrics.lineSpacing(),
        metrics.ascent(),
        metrics.descent(),
    };
}

term::Terminal_render_text_run make_direct_text_run(
    int                            column,
    int                            cell_count,
    const QString&                 text,
    term::terminal_cell_metrics_t  metrics)
{
    return make_direct_text_run_for_row(
        0,
        0,
        column,
        cell_count,
        text,
        metrics,
        QColor(196, 230, 201));
}

term::Terminal_render_text_run make_direct_text_run_for_row(
    int                            row,
    int                            logical_row,
    int                            column,
    int                            cell_count,
    const QString&                 text,
    term::terminal_cell_metrics_t  metrics,
    QColor                         foreground)
{
    term::Terminal_render_text_run run;
    run.row             = row;
    run.logical_row     = logical_row;
    run.column          = column;
    run.rect            = QRectF(
        static_cast<qreal>(column) * metrics.width,
        static_cast<qreal>(row) * metrics.height,
        static_cast<qreal>(cell_count) * metrics.width,
        metrics.height);
    run.baseline_origin = QPointF(run.rect.left(), run.rect.top() + metrics.ascent);
    run.text            = text;
    run.foreground      = foreground;
    run.background      = QColor(9, 12, 16);
    return run;
}

term::Terminal_render_frame make_direct_text_frame(
    std::vector<term::Terminal_render_text_run>        text_runs,
    term::terminal_cell_metrics_t                      metrics)
{
    term::Terminal_render_frame frame;
    frame.logical_size          = QSizeF(340.0, 90.0);
    frame.grid_size             = {3, 20};
    frame.viewport.visible_rows = 3;
    frame.cell_metrics          = metrics;
    frame.text_runs             = std::move(text_runs);
    return frame;
}

term::Terminal_render_frame make_direct_text_slot_frame(
    term::Terminal_buffer_id                           active_buffer,
    int                                                first_visible_logical_row,
    int                                                visible_rows,
    std::vector<term::Terminal_render_text_run>        text_runs,
    term::terminal_cell_metrics_t                      metrics,
    std::vector<term::Terminal_render_dirty_row_range> dirty_row_ranges)
{
    constexpr int column_count = 20;
    const int scrollback_rows =
        std::max(first_visible_logical_row + visible_rows + 4, visible_rows);

    term::Terminal_render_frame frame;
    frame.logical_size              = QSizeF(
        metrics.width * static_cast<qreal>(column_count),
        metrics.height * static_cast<qreal>(visible_rows));
    frame.grid_size                 = {visible_rows, column_count};
    frame.viewport.active_buffer    = active_buffer;
    frame.viewport.visible_rows     = visible_rows;
    frame.viewport.scrollback_rows  = scrollback_rows;
    frame.viewport.offset_from_tail = scrollback_rows - first_visible_logical_row;
    frame.viewport.follow_tail      = false;
    frame.cell_metrics              = metrics;
    frame.text_runs                 = std::move(text_runs);
    frame.dirty_row_ranges          = std::move(dirty_row_ranges);
    return frame;
}

term::Terminal_render_frame make_direct_text_slot_rows_frame(
    term::Terminal_buffer_id                           active_buffer,
    int                                                first_visible_logical_row,
    int                                                visible_rows,
    term::terminal_cell_metrics_t                      metrics,
    std::vector<term::Terminal_render_dirty_row_range> dirty_row_ranges)
{
    std::vector<term::Terminal_render_text_run> runs;
    runs.reserve(static_cast<std::size_t>(visible_rows));
    for (int row = 0; row < visible_rows; ++row) {
        const int logical_row = first_visible_logical_row + row;
        runs.push_back(make_direct_text_run_for_row(
            row,
            logical_row,
            0,
            1,
            QString(QChar(static_cast<char16_t>(u'A' + logical_row % 26))),
            metrics,
            QColor::fromRgba(viewport_scroll_foreground_rgba(logical_row))));
    }

    return
        make_direct_text_slot_frame(
            active_buffer,
            first_visible_logical_row,
            visible_rows,
            std::move(runs),
            metrics,
            std::move(dirty_row_ranges));
}

term::Terminal_render_rect make_direct_row_rect(
    int                            row,
    int                            first_column,
    int                            column_count,
    QColor                         color,
    term::terminal_cell_metrics_t  metrics,
    bool                           antialias = false)
{
    return {
        QRectF(
            static_cast<qreal>(first_column) * metrics.width,
            static_cast<qreal>(row) * metrics.height,
            static_cast<qreal>(column_count) * metrics.width,
            metrics.height),
        color,
        antialias,
    };
}

term::Terminal_render_frame make_direct_rect_cache_frame(
    std::vector<term::Terminal_render_rect>    background_row_rects,
    std::vector<term::Terminal_render_rect>    selection_rects,
    term::terminal_cell_metrics_t              metrics,
    bool                                       include_default_background = true)
{
    term::Terminal_render_frame frame;
    frame.logical_size          = QSizeF(metrics.width * 20.0, metrics.height * 3.0);
    frame.grid_size             = {3, 20};
    frame.viewport.visible_rows = 3;
    frame.cell_metrics          = metrics;
    if (include_default_background) {
        frame.background_rects.push_back({
            QRectF(QPointF(0.0, 0.0), frame.logical_size),
            QColor(9, 12, 16),
        });
    }
    frame.background_rects.insert(
        frame.background_rects.end(),
        background_row_rects.begin(),
        background_row_rects.end());
    frame.selection_rects = std::move(selection_rects);
    return frame;
}

term::Terminal_render_frame make_direct_rect_cache_viewport_frame(
    std::vector<term::Terminal_render_rect>    background_rects,
    std::vector<term::Terminal_render_rect>    selection_rects,
    term::terminal_cell_metrics_t              metrics,
    term::Terminal_viewport_state              viewport)
{
    term::Terminal_render_frame frame =
        make_direct_rect_cache_frame({}, {}, metrics, false);
    frame.viewport         = viewport;
    frame.background_rects = std::move(background_rects);
    frame.selection_rects  = std::move(selection_rects);
    return frame;
}

term::Terminal_render_decoration make_direct_decoration_rect(
    int                                    row,
    int                                    first_column,
    int                                    column_count,
    qreal                                  row_offset,
    qreal                                  height,
    QColor                                 color,
    term::terminal_cell_metrics_t          metrics,
    term::Terminal_render_decoration_kind  kind = term::Terminal_render_decoration_kind::UNDERLINE)
{
    return {
        kind,
        QRectF(
            static_cast<qreal>(first_column) * metrics.width,
            static_cast<qreal>(row) * metrics.height + row_offset,
            static_cast<qreal>(column_count) * metrics.width,
            height),
        color,
    };
}

term::Terminal_render_arc make_direct_row_arc(
    int                            row,
    int                            first_column,
    term::Terminal_render_arc_kind kind,
    QColor                         color,
    qreal                          stroke,
    term::terminal_cell_metrics_t  metrics)
{
    return {
        kind,
        QRectF(
            static_cast<qreal>(first_column) * metrics.width,
            static_cast<qreal>(row) * metrics.height,
            metrics.width,
            metrics.height),
        color,
        stroke,
    };
}

term::Terminal_render_frame make_direct_decoration_cache_frame(
    std::vector<term::Terminal_render_decoration>  decorations,
    term::terminal_cell_metrics_t                  metrics)
{
    term::Terminal_render_frame frame;
    frame.logical_size          = QSizeF(metrics.width * 20.0, metrics.height * 3.0);
    frame.grid_size             = {3, 20};
    frame.viewport.visible_rows = 3;
    frame.cell_metrics          = metrics;
    frame.decorations           = std::move(decorations);
    return frame;
}

term::Terminal_render_frame make_direct_hyperlink_text_resource_frame(
    bool                           hyperlink,
    term::terminal_cell_metrics_t  metrics)
{
    term::Terminal_render_frame frame;
    frame.logical_size          = QSizeF(metrics.width * 20.0, metrics.height * 3.0);
    frame.grid_size             = {3, 20};
    frame.viewport.visible_rows = 3;
    frame.cell_metrics          = metrics;
    frame.dirty_row_ranges      = {{0, 1}};
    frame.background_rects.push_back({
        QRectF(QPointF(0.0, 0.0), frame.logical_size),
        QColor(9, 12, 16),
    });

    const QColor foreground(196, 230, 201);
    const QColor background(9, 12, 16);
    for (int column = 0; column < 20; ++column) {
        const QRectF rect(
            static_cast<qreal>(column) * metrics.width,
            0.0,
            metrics.width,
            metrics.height);
        frame.text_runs.push_back({
            0,
            0,
            column,
            rect,
            QRectF(),
            QPointF(rect.left(), rect.top() + metrics.ascent),
            QString(QChar(static_cast<char16_t>(u'K' + column % 10))),
            foreground,
            background,
            term::k_default_terminal_style_id,
            hyperlink ? 77U : 0U,
            false,
            false,
        });
    }

    if (hyperlink) {
        const qreal thickness = std::max<qreal>(1.0, std::floor(metrics.height * 0.08));
        frame.decorations.push_back({
            term::Terminal_render_decoration_kind::HYPERLINK_UNDERLINE,
            QRectF(
                0.0,
                metrics.ascent + thickness,
                metrics.width * 20.0,
                thickness),
            foreground,
        });
    }

    return frame;
}

term::Terminal_render_frame make_direct_text_metadata_decoration_frame(
    bool                           underline,
    bool                           strike,
    term::terminal_cell_metrics_t  metrics)
{
    term::Terminal_render_frame frame;
    frame.logical_size          = QSizeF(metrics.width * 20.0, metrics.height * 3.0);
    frame.grid_size             = {3, 20};
    frame.viewport.visible_rows = 3;
    frame.cell_metrics          = metrics;
    frame.dirty_row_ranges      = {{0, 1}};
    frame.background_rects.push_back({
        QRectF(QPointF(0.0, 0.0), frame.logical_size),
        QColor(9, 12, 16),
    });

    const QColor foreground(196, 230, 201);
    const QColor background(9, 12, 16);
    for (int column = 0; column < 20; ++column) {
        const QRectF rect(
            static_cast<qreal>(column) * metrics.width,
            0.0,
            metrics.width,
            metrics.height);
        frame.text_runs.push_back({
            0,
            0,
            column,
            rect,
            QRectF(),
            QPointF(rect.left(), rect.top() + metrics.ascent),
            QString(QChar(static_cast<char16_t>(u'P' + column % 8))),
            foreground,
            background,
            underline || strike ? 1U : term::k_default_terminal_style_id,
            0U,
            underline,
            strike,
        });
    }

    const qreal thickness = std::max<qreal>(1.0, std::floor(metrics.height * 0.08));
    if (underline) {
        frame.decorations.push_back({
            term::Terminal_render_decoration_kind::UNDERLINE,
            QRectF(
                0.0,
                metrics.ascent + thickness,
                metrics.width * 20.0,
                thickness),
            foreground,
        });
    }
    if (strike) {
        frame.decorations.push_back({
            term::Terminal_render_decoration_kind::STRIKE,
            QRectF(
                0.0,
                metrics.ascent * 0.55,
                metrics.width * 20.0,
                thickness),
            foreground,
        });
    }

    return frame;
}

term::Terminal_render_frame make_direct_graphic_row_cache_frame(
    std::vector<term::Terminal_render_rect>    rects,
    std::vector<term::Terminal_render_arc>     arcs,
    term::terminal_cell_metrics_t              metrics)
{
    term::Terminal_render_frame frame;
    frame.logical_size          = QSizeF(metrics.width * 20.0, metrics.height * 3.0);
    frame.grid_size             = {3, 20};
    frame.viewport.visible_rows = 3;
    frame.cell_metrics          = metrics;
    frame.graphic_rects         = std::move(rects);
    frame.graphic_arcs          = std::move(arcs);
    return frame;
}

term::Terminal_render_frame with_all_rows_dirty(term::Terminal_render_frame frame)
{
    frame.dirty_row_ranges = {{0, frame.grid_size.rows}};
    return frame;
}

term::Terminal_render_frame make_direct_graphic_cache_key_frame(
    bool                           rect_antialias,
    term::Terminal_render_arc_kind arc_kind,
    qreal                          arc_stroke)
{
    term::Terminal_render_frame frame;
    frame.logical_size          = QSizeF(160.0, 90.0);
    frame.grid_size             = {3, 20};
    frame.viewport.visible_rows = 3;
    frame.cell_metrics          = test_metrics();
    frame.graphic_rects.push_back({
        QRectF(6.0, 8.0, 22.0, 14.0),
        QColor(80, 180, 230),
        rect_antialias,
    });
    frame.graphic_arcs.push_back({
        arc_kind,
        QRectF(36.0, 8.0, 20.0, 20.0),
        QColor(230, 210, 80),
        arc_stroke,
    });
    return frame;
}

term::Terminal_render_frame make_direct_geometry_slot_frame(
    term::Terminal_buffer_id       active_buffer,
    int                            phase,
    term::terminal_cell_metrics_t  metrics)
{
    const QColor background_color =
        phase == 0 ? QColor(118, 46, 76) : QColor(36, 118, 78);
    const QColor selection_color =
        phase == 0 ? QColor(42, 104, 176) : QColor(186, 86, 46);
    const QColor graphic_rect_color =
        phase == 0 ? QColor(220, 174, 54) : QColor(80, 168, 222);
    const QColor arc_color =
        phase == 0 ? QColor(224, 206, 74) : QColor(82, 122, 226);
    const QColor decoration_color =
        phase == 0 ? QColor(214, 96, 156) : QColor(100, 220, 142);
    const qreal decoration_thickness =
        std::max<qreal>(1.0, std::floor(metrics.height * 0.08));

    term::Terminal_render_frame frame = make_direct_rect_cache_frame(
        { make_direct_row_rect(0, 0, 2, background_color, metrics) },
        { make_direct_row_rect(0, 3, 2, selection_color, metrics) },
        metrics,
        false);
    frame.viewport.active_buffer = active_buffer;
    frame.graphic_rects          = {
        make_direct_row_rect(1, 1, 2, graphic_rect_color, metrics),
    };
    frame.graphic_arcs           = {
        make_direct_row_arc(
            1,
            6,
            term::Terminal_render_arc_kind::DOWN_RIGHT,
            arc_color,
            2.0,
            metrics),
    };
    frame.decorations            = {
        make_direct_decoration_rect(
            2,
            2,
            4,
            metrics.ascent + decoration_thickness,
            decoration_thickness,
            decoration_color,
            metrics),
    };
    return frame;
}

bool window_uses_software_scene_graph(const QQuickWindow& window)
{
    const QSGRendererInterface* renderer_interface = window.rendererInterface();
    return
        renderer_interface                != nullptr &&
        renderer_interface->graphicsApi() == QSGRendererInterface::Software;
}

class Direct_render_item final : public QQuickItem
{
public:
    Direct_render_item()
    {
        setFlag(ItemHasContents, true);
    }

    void set_frame(const term::Terminal_render_frame& frame)
    {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_frame = frame;
        }
        setSize(frame.logical_size);
        update();
    }

    void set_device_pixel_ratio(qreal device_pixel_ratio)
    {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_device_pixel_ratio = device_pixel_ratio;
        }
        update();
    }

    void set_lifecycle_recorder(Renderer_lifecycle_recorder_ptr recorder)
    {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_lifecycle_recorder = std::move(recorder);
        }
        update();
    }

    term::terminal_renderer_stats_t last_stats() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_stats;
    }

    int render_count() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_render_count;
    }

    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) override
    {
        term::Terminal_render_frame frame;
        qreal device_pixel_ratio = 1.0;
        Renderer_lifecycle_recorder_ptr lifecycle_recorder;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            frame              = m_frame;
            device_pixel_ratio = m_device_pixel_ratio;
            lifecycle_recorder = m_lifecycle_recorder;
        }

        term::terminal_renderer_stats_t stats;
        QSGNode* node = m_renderer.update_node(
            old_node,
            window(),
            frame,
            m_font,
            device_pixel_ratio,
            std::move(lifecycle_recorder),
            stats);
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_stats = stats;
            ++m_render_count;
        }
        return node;
    }

private:
    mutable std::mutex                 m_mutex;
    term::Qsg_terminal_renderer        m_renderer;
    term::Terminal_render_frame        m_frame;
    QFont                              m_font               = term::vnm_terminal_font(QString(), 16.0);
    qreal                              m_device_pixel_ratio = 1.0;
    Renderer_lifecycle_recorder_ptr    m_lifecycle_recorder;
    term::terminal_renderer_stats_t    m_stats;
    int                                m_render_count       = 0;
};

void clear_packed_sidecars(term::Terminal_render_frame& frame)
{
    frame.packed_rows.clear();
    frame.packed_text_spans.clear();
    frame.packed_text_bytes.clear();
    frame.packed_graphic_spans.clear();
    frame.packed_graphic_codepoints.clear();
    frame.stats.packed_rows          = 0;
    frame.stats.packed_text_spans    = 0;
    frame.stats.packed_text_cells    = 0;
    frame.stats.packed_graphic_spans = 0;
    frame.stats.packed_graphic_cells = 0;
    frame.stats.packed_payload_bytes = 0U;
}

bool images_have_same_pixels(const QImage& left, const QImage& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (int y = 0; y < left.height(); ++y) {
        for (int x = 0; x < left.width(); ++x) {
            if (left.pixelColor(x, y) != right.pixelColor(x, y)) {
                return false;
            }
        }
    }

    return true;
}

struct direct_frame_render_result_t
{
    QImage                          image;
    term::terminal_renderer_stats_t stats;
};

direct_frame_render_result_t render_frame_in_fresh_window(
    QGuiApplication&                   app,
    const term::Terminal_render_frame& frame,
    int                                expected_packed_rows)
{
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(
        static_cast<int>(std::ceil(frame.logical_size.width())) + 20,
        static_cast<int>(std::ceil(frame.logical_size.height())) + 20);

    Direct_render_item item;
    item.setParentItem(window.contentItem());
    item.set_frame(frame);
    window.show();

    direct_frame_render_result_t result;
    result.image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.paint_completed            &&
            stats.text_content_failures == 0 &&
            stats.frame_packed_rows == expected_packed_rows;
    });
    result.stats = item.last_stats();
    return result;
}

std::uint64_t live_root_node_count(const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return stats.render_root_nodes_created - stats.render_root_nodes_destroyed;
}

std::uint64_t live_text_resource_count(const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return stats.render_text_resources_created - stats.render_text_resources_destroyed;
}

std::uint64_t live_rect_resource_count(const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return stats.render_rect_resources_created - stats.render_rect_resources_destroyed;
}

bool has_valid_lifecycle_resource_counts(
    const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return
        stats.render_root_nodes_created     >= stats.render_root_nodes_destroyed     &&
        stats.render_text_resources_created >= stats.render_text_resources_destroyed &&
        stats.render_rect_resources_created >= stats.render_rect_resources_destroyed;
}

bool has_live_render_tree(const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return
        has_valid_lifecycle_resource_counts(stats) &&
        live_root_node_count(stats)     == 1U      &&
        live_text_resource_count(stats) >  0U      &&
        live_rect_resource_count(stats) >  0U;
}

bool has_no_live_render_resources(const term::terminal_renderer_lifecycle_stats_t& stats)
{
    return
        has_valid_lifecycle_resource_counts(stats) &&
        live_root_node_count(stats)     == 0U      &&
        live_text_resource_count(stats) == 0U      &&
        live_rect_resource_count(stats) == 0U;
}

bool wait_lifecycle_until(
    QGuiApplication&                                                               app,
    const std::shared_ptr<term::Terminal_renderer_lifecycle_recorder>&             recorder,
    const std::function<bool(const term::terminal_renderer_lifecycle_stats_t&)>&   predicate)
{
    for (int i = 0; i < 30; ++i) {
        pump_events(app, 1);
        if (predicate(recorder->snapshot())) {
            return true;
        }
    }

    return false;
}

int count_matching_pixels(
    const QImage&                      image,
    QRect                              area,
    const std::function<bool(QColor)>& matches)
{
    int         count   = 0;
    const QRect bounded = area.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            if (matches(image.pixelColor(x, y))) {
                ++count;
            }
        }
    }
    return count;
}

bool is_terminal_background(QColor color)
{
    return color.red() < 20 && color.green() < 24 && color.blue() < 28;
}

bool green_text_pixel(QColor color)
{
    return
        color.green() > 80               &&
        color.green() > color.red() + 30 &&
        color.green() > color.blue() + 30;
}

bool highlighted_style_background_pixel(QColor color)
{
    return
        color.alpha() >  220 &&
        color.red()   >= 24  &&
        color.red()   <= 44  &&
        color.green() >= 56  &&
        color.green() <= 76  &&
        color.blue()  >= 88  &&
        color.blue()  <= 108;
}

bool default_foreground_pixel(QColor color)
{
    return
        color.alpha() >  220         &&
        color.red()   >= 150         &&
        color.green() >= 180         &&
        color.blue()  >= 150         &&
        color.green() >= color.red() &&
        color.green() >= color.blue();
}

int count_non_background_pixels(const QImage& image, QRect area)
{
    return count_matching_pixels(
        image,
        area,
        [](QColor color) {
            return !is_terminal_background(color);
        });
}

int count_background_pixels(const QImage& image, QRect area)
{
    return count_matching_pixels(image, area, is_terminal_background);
}

QRect cell_area(int row, int column, int columns, term::terminal_cell_metrics_t metrics)
{
    return
        QRect(
            static_cast<int>(std::floor(static_cast<qreal>(column) * metrics.width)) + 1,
            static_cast<int>(std::floor(static_cast<qreal>(row) * metrics.height)) + 1,
            static_cast<int>(std::ceil(static_cast<qreal>(columns) * metrics.width)) - 2,
            static_cast<int>(std::ceil(metrics.height)) - 2);
}

QRect underline_area(int row, int column, int columns, term::terminal_cell_metrics_t metrics)
{
    const qreal thickness = std::max<qreal>(1.0, std::floor(metrics.height * 0.08));
    return
        QRect(
            static_cast<int>(std::floor(static_cast<qreal>(column) * metrics.width)) + 1,
            static_cast<int>(std::floor(static_cast<qreal>(row) * metrics.height + metrics.ascent + thickness)),
            static_cast<int>(std::ceil(static_cast<qreal>(columns) * metrics.width)) - 2,
            std::max(1, static_cast<int>(std::ceil(thickness))) + 1);
}

QRect strike_area(int row, int column, int columns, term::terminal_cell_metrics_t metrics)
{
    const qreal thickness = std::max<qreal>(1.0, std::floor(metrics.height * 0.08));
    return
        QRect(
            static_cast<int>(std::floor(static_cast<qreal>(column) * metrics.width)) + 1,
            static_cast<int>(std::floor(static_cast<qreal>(row) * metrics.height + metrics.ascent * 0.55)),
            static_cast<int>(std::ceil(static_cast<qreal>(columns) * metrics.width)) - 2,
            std::max(1, static_cast<int>(std::ceil(thickness))) + 1);
}

int count_highlighted_background_pixels(
    const QImage&                  image,
    int                            row,
    int                            column,
    int                            columns,
    term::terminal_cell_metrics_t  metrics)
{
    return count_matching_pixels(
        image,
        cell_area(row, column, columns, metrics),
        highlighted_style_background_pixel);
}

int count_underline_pixels(
    const QImage&                  image,
    int                            row,
    int                            column,
    int                            columns,
    term::terminal_cell_metrics_t  metrics)
{
    return count_matching_pixels(
        image,
        underline_area(row, column, columns, metrics),
        default_foreground_pixel);
}

int count_strike_pixels(
    const QImage&                  image,
    int                            row,
    int                            column,
    int                            columns,
    term::terminal_cell_metrics_t  metrics)
{
    return count_matching_pixels(
        image,
        strike_area(row, column, columns, metrics),
        default_foreground_pixel);
}

int average_red(const QImage& image, QRect area)
{
    const QRect bounded = area.intersected(image.rect());
    int         total   = 0;
    int         count   = 0;
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            total += image.pixelColor(x, y).red();
            ++count;
        }
    }
    return count == 0 ? 0 : total / count;
}

bool mostly_background(
    const QImage&                  image,
    int                            row,
    int                            column,
    int                            columns,
    term::terminal_cell_metrics_t  metrics)
{
    const QRect area = cell_area(row, column, columns, metrics);
    return count_background_pixels(image, area) >
        static_cast<int>(static_cast<qreal>(area.width() * area.height()) * 0.75);
}

bool nearly_all_background(
    const QImage&                  image,
    int                            row,
    int                            column,
    int                            columns,
    term::terminal_cell_metrics_t  metrics)
{
    const QRect area = cell_area(row, column, columns, metrics);
    const int allowed_spill =
        std::max(1, static_cast<int>(static_cast<qreal>(area.width() * area.height()) * 0.005));
    return count_non_background_pixels(image, area) <= allowed_spill;
}

bool cell_is_nonblank(
    const QImage&                  image,
    int                            row,
    int                            column,
    term::terminal_cell_metrics_t  metrics)
{
    return count_non_background_pixels(image, cell_area(row, column, 1, metrics)) > 2;
}

QString visual_failure_directory()
{
    QDir dir(QDir::tempPath());
    const QString child = QStringLiteral("vnm_terminal_qsg_render_failures");
    if (!dir.exists(child)) {
        dir.mkpath(child);
    }
    return dir.filePath(child);
}

QString visual_failure_label(const char* message)
{
    QString label = QString::fromUtf8(message);
    for (qsizetype index = 0; index < label.size(); ++index) {
        const QChar character = label.at(index);
        const bool keep =
            character.isLetterOrNumber()  ||
            character == QLatin1Char('_') ||
            character == QLatin1Char('-');
        if (!keep) {
            label[index] = QLatin1Char('_');
        }
    }
    return label.left(80);
}

QString save_visual_failure_image(
    const QImage&  image,
    const QString& label,
    const QString& suffix)
{
    if (image.isNull()) {
        return QString();
    }

    static int index = 0;
    const QString path = QDir(visual_failure_directory()).filePath(
        QStringLiteral("%1_%2_%3.png").arg(label).arg(++index).arg(suffix));
    image.save(path);
    return path;
}

QString dirty_ranges_text(
    const std::vector<term::Terminal_render_dirty_row_range>& dirty_row_ranges)
{
    QStringList parts;
    for (const term::Terminal_render_dirty_row_range& range : dirty_row_ranges) {
        parts.push_back(QStringLiteral("[%1,%2)")
            .arg(range.first_row)
            .arg(range.first_row + range.row_count));
    }
    return parts.isEmpty() ? QStringLiteral("<empty>") : parts.join(QLatin1Char(','));
}

void report_visual_sample_pixels(const QImage& image, QRect area)
{
    const QRect bounded = area.intersected(image.rect());
    if (bounded.isEmpty()) {
        std::cerr << "INFO: visual sample area is empty after image bounds clipping\n";
        return;
    }

    const QPoint points[] = {
        bounded.topLeft(),
        bounded.center(),
        bounded.bottomRight(),
    };
    for (const QPoint& point : points) {
        const QColor color = image.pixelColor(point);
        std::cerr << "INFO: sampled pixel x=" << point.x()
            << " y=" << point.y()
            << " rgba=0x"
            << qPrintable(
                QString::number(static_cast<qulonglong>(color.rgba()), 16)
                    .rightJustified(8, QLatin1Char('0')))
            << '\n';
    }
}

bool check_visual_pixels(
    bool                   condition,
    const char*            message,
    const QImage&          image,
    QRect                  sample_area,
    int                    matching_pixels,
    std::uint64_t          sequence,
    const std::vector<term::Terminal_render_dirty_row_range>&
                           dirty_row_ranges,
    const term::terminal_renderer_stats_t&
                           stats)
{
    if (condition) {
        return true;
    }

    const QRect   bounded      = sample_area.intersected(image.rect());
    const int     sample_count = bounded.isEmpty() ? 0 : bounded.width() * bounded.height();
    const QString label        = visual_failure_label(message);
    const QString framebuffer_path =
        save_visual_failure_image(image, label, QStringLiteral("framebuffer"));
    const QString crop_path =
        save_visual_failure_image(image.copy(bounded), label, QStringLiteral("crop"));
    std::cerr << "INFO: visual failure sequence=" << sequence
        << " dirty_ranges="    << qPrintable(dirty_ranges_text(dirty_row_ranges))
        << " sample_area=(" << sample_area.x() << ',' << sample_area.y()
        << ',' << sample_area.width() << ',' << sample_area.height() << ')'
        << " sampled_pixels="  << sample_count
        << " matching_pixels=" << matching_pixels << '\n';
    std::cerr << "INFO: renderer stats paint_completed=" << stats.paint_completed
        << " route_fast_text_cells="        << stats.route_fast_text_cells
        << " route_qt_text_layout_runs="    << stats.route_qt_text_layout_runs
        << " route_graphic_geometry_cells=" << stats.route_graphic_geometry_cells
        << " route_fallback_cells="         << stats.route_fallback_cells
        << " qsg_nodes_created="            << stats.qsg_nodes_created
        << " qsg_nodes_replaced="           << stats.qsg_nodes_replaced
        << " qsg_nodes_destroyed="          << stats.qsg_nodes_destroyed
        << " cache_key_builds="             << stats.cache_key_builds
        << " cache_key_bytes="              << stats.cache_key_bytes << '\n';
    report_visual_sample_pixels(image, sample_area);
    if (!framebuffer_path.isEmpty()) {
        std::cerr << "INFO: failing framebuffer: "
            << qPrintable(QDir::toNativeSeparators(framebuffer_path)) << '\n';
    }
    if (!crop_path.isEmpty()) {
        std::cerr << "INFO: failing crop: "
            << qPrintable(QDir::toNativeSeparators(crop_path)) << '\n';
    }
    return check(false, message);
}

QColor qsg_lifecycle_row_color(int logical_row)
{
    switch (logical_row % 5) {
        case 0:  return QColor(176, 34, 54);
        case 1:  return QColor(34, 146, 74);
        case 2:  return QColor(44, 88, 188);
        case 3:  return QColor(194, 148, 36);
        default: return QColor(132, 58, 176);
    }
}

bool is_qsg_lifecycle_row_color(QColor color, int logical_row)
{
    constexpr int k_color_tolerance = 12;
    const QColor  expected          = qsg_lifecycle_row_color(logical_row);
    return
        color.alpha() >  220                                  &&
        color.red()   >= expected.red() - k_color_tolerance   &&
        color.red()   <= expected.red() + k_color_tolerance   &&
        color.green() >= expected.green() - k_color_tolerance &&
        color.green() <= expected.green() + k_color_tolerance &&
        color.blue()  >= expected.blue() - k_color_tolerance  &&
        color.blue()  <= expected.blue() + k_color_tolerance;
}

bool row_area_has_lifecycle_color(
    const QImage&                  image,
    int                            viewport_row,
    int                            logical_row,
    term::terminal_cell_metrics_t  metrics)
{
    const QRect area = cell_area(viewport_row, 0, 20, metrics);
    return count_matching_pixels(
        image,
        area,
        [logical_row](QColor color) {
            return is_qsg_lifecycle_row_color(color, logical_row);
        }) >
        static_cast<int>(static_cast<qreal>(area.width() * area.height()) * 0.65);
}

QRect lifecycle_frame_area(term::terminal_cell_metrics_t metrics)
{
    return
        QRect(
            0,
            0,
            static_cast<int>(std::ceil(metrics.width * 20.0)),
            static_cast<int>(std::ceil(metrics.height * 3.0)));
}

int frame_lifecycle_color_pixel_count(
    const QImage&                          image,
    int                                    logical_row,
    term::terminal_cell_metrics_t          metrics)
{
    const QRect frame_area = lifecycle_frame_area(metrics);
    return count_matching_pixels(
        image,
        frame_area,
        [logical_row](QColor color) {
            return is_qsg_lifecycle_row_color(color, logical_row);
        });
}

bool frame_lacks_lifecycle_color(
    const QImage&                          image,
    int                                    logical_row,
    term::terminal_cell_metrics_t          metrics)
{
    return frame_lifecycle_color_pixel_count(image, logical_row, metrics) <= 4;
}

bool check_frame_lacks_lifecycle_color(
    const QImage&                          image,
    int                                    logical_row,
    term::terminal_cell_metrics_t          metrics,
    const char*                            message,
    std::uint64_t                          sequence,
    const term::Terminal_render_frame&     frame,
    const term::terminal_renderer_stats_t& stats)
{
    const QRect frame_area = lifecycle_frame_area(metrics);
    const int matching_pixels =
        frame_lifecycle_color_pixel_count(image, logical_row, metrics);
    return
        check_visual_pixels(
            matching_pixels <= 4,
            message,
            image,
            frame_area,
            matching_pixels,
            sequence,
            frame.dirty_row_ranges,
            stats);
}

term::Terminal_render_rect make_direct_frame_background_rect(
    term::terminal_cell_metrics_t metrics)
{
    return {
        QRectF(
            0.0,
            0.0,
            metrics.width * 20.0,
            metrics.height * 3.0),
        QColor(9, 12, 16),
    };
}

term::Terminal_viewport_state make_direct_scroll_lifecycle_viewport(
    int first_visible_logical_row)
{
    constexpr int k_visible_rows    = 3;
    constexpr int k_scrollback_rows = 12;

    term::Terminal_viewport_state viewport;
    viewport.visible_rows     = k_visible_rows;
    viewport.scrollback_rows  = k_scrollback_rows;
    viewport.offset_from_tail = k_scrollback_rows - first_visible_logical_row;
    viewport.follow_tail      = false;
    return viewport;
}

term::Terminal_render_frame make_direct_scroll_lifecycle_background_frame(
    int                            first_visible_logical_row,
    term::terminal_cell_metrics_t  metrics)
{
    std::vector<term::Terminal_render_rect> background_rects = {
        make_direct_frame_background_rect(metrics),
    };
    for (int row = 0; row < 3; ++row) {
        background_rects.push_back(make_direct_row_rect(
            row,
            0,
            20,
            qsg_lifecycle_row_color(first_visible_logical_row + row),
            metrics));
    }

    return make_direct_rect_cache_viewport_frame(
        std::move(background_rects),
        {},
        metrics,
        make_direct_scroll_lifecycle_viewport(first_visible_logical_row));
}

QSGNode* child_node_at(QSGNode* parent, int index)
{
    QSGNode* child = parent != nullptr ? parent->firstChild() : nullptr;
    for (int i = 0; i < index && child != nullptr; ++i) {
        child = child->nextSibling();
    }
    return child;
}

int child_node_count(QSGNode* parent)
{
    int count = 0;
    for (QSGNode* child = parent != nullptr ? parent->firstChild() : nullptr;
        child != nullptr;
        child = child->nextSibling())
    {
        ++count;
    }
    return count;
}

std::vector<QSGTransformNode*> transform_child_nodes(QSGNode* parent)
{
    std::vector<QSGTransformNode*> nodes;
    for (QSGNode* child = parent != nullptr ? parent->firstChild() : nullptr;
        child != nullptr;
        child = child->nextSibling())
    {
        auto* transform_node = dynamic_cast<QSGTransformNode*>(child);
        if (transform_node != nullptr) {
            nodes.push_back(transform_node);
        }
    }
    return nodes;
}

std::vector<QColor> simple_rect_child_colors(QSGNode* parent)
{
    std::vector<QColor> colors;
    for (QSGNode* child = parent != nullptr ? parent->firstChild() : nullptr;
        child != nullptr;
        child = child->nextSibling())
    {
        auto* rect_node = dynamic_cast<QSGSimpleRectNode*>(child);
        if (rect_node != nullptr) {
            colors.push_back(rect_node->color());
        }
    }
    return colors;
}

std::vector<QRectF> simple_rect_child_rects(QSGNode* parent)
{
    std::vector<QRectF> rects;
    for (QSGNode* child = parent != nullptr ? parent->firstChild() : nullptr;
        child != nullptr;
        child = child->nextSibling())
    {
        auto* rect_node = dynamic_cast<QSGSimpleRectNode*>(child);
        if (rect_node != nullptr) {
            rects.push_back(rect_node->rect());
        }
    }
    return rects;
}

std::vector<QSGGeometryNode*> geometry_child_nodes(QSGNode* parent)
{
    std::vector<QSGGeometryNode*> nodes;
    for (QSGNode* child = parent != nullptr ? parent->firstChild() : nullptr;
        child != nullptr;
        child = child->nextSibling())
    {
        auto* geometry_node = dynamic_cast<QSGGeometryNode*>(child);
        if (geometry_node != nullptr) {
            nodes.push_back(geometry_node);
        }
    }
    return nodes;
}

std::vector<QRectF> batched_geometry_rect_bounds(QSGGeometryNode* node)
{
    std::vector<QRectF> bounds;
    if (node == nullptr || node->geometry() == nullptr) {
        return bounds;
    }

    QSGGeometry* geometry = node->geometry();
    const QSGGeometry::ColoredPoint2D* vertices =
        geometry->vertexDataAsColoredPoint2D();
    constexpr int vertices_per_rect = 6;
    if (vertices                == nullptr ||
        geometry->vertexCount() <  vertices_per_rect)
    {
        return bounds;
    }

    for (int offset = 0;
        offset + vertices_per_rect <= geometry->vertexCount();
        offset += vertices_per_rect)
    {
        qreal left   = vertices[offset].x;
        qreal right  = vertices[offset].x;
        qreal top    = vertices[offset].y;
        qreal bottom = vertices[offset].y;
        for (int index = offset + 1; index < offset + vertices_per_rect; ++index) {
            left   = std::min<qreal>(left, vertices[index].x);
            right  = std::max<qreal>(right, vertices[index].x);
            top    = std::min<qreal>(top, vertices[index].y);
            bottom = std::max<qreal>(bottom, vertices[index].y);
        }
        bounds.push_back(QRectF(QPointF(left, top), QPointF(right, bottom)));
    }

    return bounds;
}

std::vector<QRectF> batched_geometry_child_rect_bounds(QSGNode* parent)
{
    std::vector<QRectF> bounds;
    for (QSGGeometryNode* node : geometry_child_nodes(parent)) {
        const std::vector<QRectF> node_bounds =
            batched_geometry_rect_bounds(node);
        bounds.insert(bounds.end(), node_bounds.begin(), node_bounds.end());
    }
    return bounds;
}

bool nearly_equal(qreal actual, qreal expected, qreal tolerance = 0.25)
{
    return std::abs(actual - expected) <= tolerance;
}

bool rect_nearly_matches(
    QRectF actual,
    QRectF expected,
    qreal  tolerance = 0.25)
{
    return
        nearly_equal(actual.left(), expected.left(), tolerance)   &&
        nearly_equal(actual.top(), expected.top(), tolerance)     &&
        nearly_equal(actual.width(), expected.width(), tolerance) &&
        nearly_equal(actual.height(), expected.height(), tolerance);
}

bool test_qsg_invalidation_coalescing(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(300, 180);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(260.0, 140.0));
    configure_test_font(surface);
    window.show();

    const QImage setup_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        return stats.consumed_updates > 0U && !stats.pending_update;
    });
    const term::Terminal_surface_render_invalidation_stats_t setup_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    ok &= check(!setup_image.isNull(), "initial surface render consumes setup invalidation");
    ok &= check(setup_stats.consumed_updates > 0U && !setup_stats.pending_update,
        "setup invalidation is consumed and not left pending");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false, {{0, 5}}, QStringLiteral("A"), 601U));
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false, {{0, 5}}, QStringLiteral("B"), 602U));
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false, {{0, 5}}, QStringLiteral("C"), 603U));

    const term::Terminal_surface_render_invalidation_stats_t burst_pending_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    ok &= check(burst_pending_stats.update_requests == setup_stats.update_requests + 3U,
        "snapshot burst records every invalidation request");
    ok &= check(burst_pending_stats.scheduled_updates == setup_stats.scheduled_updates + 1U,
        "snapshot burst schedules one scene graph update");
    ok &= check(burst_pending_stats.coalesced_requests == setup_stats.coalesced_requests + 2U,
        "snapshot burst coalesces later requests");
    ok &= check(burst_pending_stats.pending_update, "snapshot burst leaves one update pending");

    const QImage burst_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        return
            stats.consumed_updates >= setup_stats.consumed_updates + 1U &&
            !stats.pending_update                                       &&
            stats.last_rendered_snapshot_sequence == 603U;
    });
    const term::Terminal_surface_render_invalidation_stats_t burst_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    const term::terminal_renderer_stats_t burst_render_stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    ok &= check(!burst_image.isNull(), "snapshot burst renders an image");
    ok &= check(burst_stats.consumed_updates >= setup_stats.consumed_updates + 1U,
        "snapshot burst consumes a scheduled update");
    ok &= check(!burst_stats.pending_update, "snapshot burst clears pending state");
    ok &= check(burst_stats.last_rendered_snapshot_sequence == 603U,
        "snapshot burst renders the latest sequence");
    ok &= check(burst_render_stats.text_content_rebuilds == 3,
        "snapshot burst rebuilds the latest snapshot's text rows");
    ok &= check_no_text_content_failures(burst_render_stats,
        "snapshot burst has no text content failures");

    term::VNM_TerminalSurface_render_bridge::set_cursor_blink_visible(surface, false);
    const term::Terminal_surface_render_invalidation_stats_t cursor_pending_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    ok &= check(cursor_pending_stats.update_requests == burst_stats.update_requests + 1U &&
        cursor_pending_stats.scheduled_updates == burst_stats.scheduled_updates + 1U &&
        cursor_pending_stats.pending_update,
        "cursor-only change schedules after a consumed paint");
    ok &= check(window_render_matches(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t render_stats =
            term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
        return
            stats.consumed_updates >= burst_stats.consumed_updates + 1U &&
            !stats.pending_update                                       &&
            render_stats.root_reused                                    &&
            render_stats.text_content_failures == 0                     &&
            render_stats.text_content_rebuilds == 0;
    }), "cursor-only update reaches a fresh consumed render");
    const term::Terminal_surface_render_invalidation_stats_t cursor_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    const term::terminal_renderer_stats_t cursor_render_stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    ok &= check(cursor_stats.consumed_updates >= burst_stats.consumed_updates + 1U &&
        !cursor_stats.pending_update,
        "cursor-only update is consumed and clears pending state");
    ok &= check(cursor_render_stats.text_content_rebuilds == 0,
        "cursor-only update after consumed paint does not rebuild text content");
    ok &= check_no_text_content_failures(cursor_render_stats,
        "cursor-only update has no text content failures");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_dense_snapshot(700U, QStringLiteral("A")));
    const QImage dense_setup_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        return stats.last_rendered_snapshot_sequence == 700U && !stats.pending_update;
    });
    const term::Terminal_surface_render_invalidation_stats_t dense_base_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    ok &= check(!dense_setup_image.isNull(),
        "dense baseline render produces an image");
    ok &= check(dense_base_stats.last_rendered_snapshot_sequence == 700U &&
        !dense_base_stats.pending_update,
        "dense baseline render completes before dirty-row burst");

    for (std::uint64_t sequence = 701U; sequence <= 708U; ++sequence) {
        const int marker_offset = static_cast<int>(sequence - 701U);
        term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
            surface,
            make_dense_snapshot(
                sequence,
                QString(QChar(static_cast<ushort>('B' + marker_offset)))));
    }
    const term::Terminal_surface_render_invalidation_stats_t dirty_pending_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    ok &= check(dirty_pending_stats.update_requests == dense_base_stats.update_requests + 8U,
        "dirty-row burst records every invalidation request");
    ok &= check(dirty_pending_stats.scheduled_updates == dense_base_stats.scheduled_updates + 1U,
        "dirty-row burst schedules one scene graph update");
    ok &= check(dirty_pending_stats.coalesced_requests == dense_base_stats.coalesced_requests + 7U,
        "dirty-row burst coalesces repeated updates");

    const QImage dirty_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        return stats.last_rendered_snapshot_sequence == 708U && !stats.pending_update;
    });
    const term::Terminal_surface_render_invalidation_stats_t dirty_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    const term::terminal_renderer_stats_t dirty_render_stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    ok &= check(!dirty_image.isNull(), "dirty-row burst renders an image");
    ok &= check(dirty_stats.last_rendered_snapshot_sequence == 708U &&
        !dirty_stats.pending_update,
        "dirty-row burst renders the final sequence and clears pending state");
    ok &= check(dirty_render_stats.text_content_rebuilds == 5,
        "repeated dirty-row updates rebuild visible current text rows");
    ok &= check_no_text_content_failures(dirty_render_stats,
        "dirty-row burst has no text content failures");
    return ok;
}

bool test_qsg_coalesced_dirty_snapshots_rebuild_skipped_rows(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(300, 180);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(260.0, 140.0));
    configure_test_font(surface);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_coalesced_dirty_text_snapshot(720U, 0, {{0, 5}}));
    window.show();

    const QImage baseline_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        return stats.last_rendered_snapshot_sequence == 720U && !stats.pending_update;
    });
    const term::Terminal_surface_render_invalidation_stats_t baseline_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    ok &= check(!baseline_image.isNull(), "coalesced dirty-row baseline renders an image");
    ok &= check(baseline_stats.last_rendered_snapshot_sequence == 720U &&
        !baseline_stats.pending_update,
        "coalesced dirty-row baseline render completes before burst");

    for (int row = 0; row < 5; ++row) {
        term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
            surface,
            make_coalesced_dirty_text_snapshot(
                721U + static_cast<std::uint64_t>(row),
                row + 1,
                {{row, 1}}));
    }

    const term::Terminal_surface_render_invalidation_stats_t pending_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    ok &= check(pending_stats.update_requests == baseline_stats.update_requests + 5U,
        "coalesced dirty-row burst records every snapshot request");
    ok &= check(pending_stats.scheduled_updates == baseline_stats.scheduled_updates + 1U,
        "coalesced dirty-row burst schedules one render update");
    ok &= check(pending_stats.coalesced_requests == baseline_stats.coalesced_requests + 4U,
        "coalesced dirty-row burst coalesces pending updates");
    ok &= check(pending_stats.pending_update,
        "coalesced dirty-row burst leaves one render update pending");

    const QImage final_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t render_stats =
            term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
        return
            stats.last_rendered_snapshot_sequence == 725U &&
            !stats.pending_update                         &&
            render_stats.paint_completed;
    });
    const term::Terminal_surface_render_invalidation_stats_t final_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    const term::terminal_renderer_stats_t final_render_stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    ok &= check(!final_image.isNull(), "coalesced dirty-row burst renders an image");
    ok &= check(final_stats.last_rendered_snapshot_sequence == 725U &&
        !final_stats.pending_update,
        "coalesced dirty-row burst renders the latest snapshot sequence");
    ok &= check(final_render_stats.text_content_rebuilds >= 5,
        "coalesced dirty-row burst rebuilds rows changed by skipped snapshots");
    ok &= check_no_text_content_failures(final_render_stats,
        "coalesced dirty-row burst has no text content failures");

    const term::terminal_cell_metrics_t metrics = test_metrics();
    for (int row = 0; row < 5; ++row) {
        const QRect row_area = cell_area(row, 0, 20, metrics);
        const int green_pixels =
            count_matching_pixels(final_image, row_area, green_text_pixel);
        if (green_pixels <= 20) {
            std::cerr << "INFO: row " << row
                << " green text pixels after coalesced dirty-row burst: "
                << green_pixels << '\n';
        }
        ok &= check(green_pixels > 20,
            "coalesced dirty-row burst leaves every final row visibly current");
    }

    return ok;
}

bool test_qsg_coalesced_style_only_transition_repaints_skipped_row(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(300, 180);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(260.0, 140.0));
    configure_test_font(surface);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_coalesced_dirty_text_snapshot(730U, 0, {{0, 5}}));
    window.show();

    const QImage baseline_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        return stats.last_rendered_snapshot_sequence == 730U && !stats.pending_update;
    });
    const term::Terminal_surface_render_invalidation_stats_t baseline_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    ok &= check(!baseline_image.isNull(),
        "coalesced style-only baseline renders an image");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_coalesced_dirty_text_snapshot(731U, 1, {{0, 1}}));
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_coalesced_dirty_text_snapshot(732U, 2, {{1, 1}}));

    const term::Terminal_surface_render_invalidation_stats_t pending_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    ok &= check(pending_stats.update_requests == baseline_stats.update_requests + 2U &&
        pending_stats.scheduled_updates == baseline_stats.scheduled_updates + 1U &&
        pending_stats.coalesced_requests == baseline_stats.coalesced_requests + 1U,
        "coalesced style-only transition coalesces one skipped render update");

    const QImage final_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t render_stats =
            term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
        return
            stats.last_rendered_snapshot_sequence == 732U &&
            !stats.pending_update                         &&
            render_stats.paint_completed;
    });
    const term::terminal_renderer_stats_t final_render_stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    ok &= check(!final_image.isNull(),
        "coalesced style-only transition renders an image");
    ok &= check(final_render_stats.text_content_rebuilds >= 2,
        "coalesced style-only transition rebuilds both skipped changed rows");
    ok &= check_no_text_content_failures(final_render_stats,
        "coalesced style-only transition has no text content failures");

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const int row_zero_green_pixels = count_matching_pixels(
        final_image,
        cell_area(0, 0, 20, metrics),
        green_text_pixel);
    const int row_one_green_pixels = count_matching_pixels(
        final_image,
        cell_area(1, 0, 20, metrics),
        green_text_pixel);
    if (row_zero_green_pixels <= 20 || row_one_green_pixels <= 20) {
        std::cerr << "INFO: coalesced style-only green pixels: row0="
            << row_zero_green_pixels << " row1=" << row_one_green_pixels << '\n';
    }
    ok &= check(row_zero_green_pixels > 20 && row_one_green_pixels > 20,
        "coalesced style-only transition leaves skipped and final dirty rows current");
    return ok;
}

bool test_qsg_detached_snapshot_updates(QGuiApplication& app)
{
    bool ok = true;
    VNM_TerminalSurface surface;
    surface.setSize(QSizeF(260.0, 140.0));
    configure_test_font(surface);

    const term::Terminal_surface_render_invalidation_stats_t detached_base_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    ok &= check(detached_base_stats.scheduled_updates == 0U &&
        detached_base_stats.consumed_updates == 0U &&
        !detached_base_stats.pending_update,
        "detached surface records no scheduled or consumed updates");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false, {{0, 5}}, QStringLiteral("D"), 801U));
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false, {{0, 5}}, QStringLiteral("E"), 802U));

    const term::Terminal_surface_render_invalidation_stats_t detached_snapshot_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    ok &= check(detached_snapshot_stats.update_requests ==
        detached_base_stats.update_requests + 2U,
        "detached snapshots record invalidation requests");
    ok &= check(detached_snapshot_stats.scheduled_updates ==
        detached_base_stats.scheduled_updates &&
        detached_snapshot_stats.consumed_updates == detached_base_stats.consumed_updates &&
        !detached_snapshot_stats.pending_update,
        "detached snapshots do not schedule or consume scene graph updates");
    ok &= check(detached_snapshot_stats.last_rendered_snapshot_sequence == 0U,
        "detached snapshots are not reported as rendered");

    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(300, 180);
    surface.setParentItem(window.contentItem());

    const term::Terminal_surface_render_invalidation_stats_t attached_pending_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    ok &= check(attached_pending_stats.scheduled_updates >=
        detached_snapshot_stats.scheduled_updates + 1U &&
        attached_pending_stats.pending_update,
        "attaching a detached surface schedules a fresh scene graph update");

    window.show();
    const QImage image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        return
            stats.consumed_updates >= detached_snapshot_stats.consumed_updates + 1U &&
            stats.last_rendered_snapshot_sequence == 802U                           &&
            !stats.pending_update;
    });
    const term::Terminal_surface_render_invalidation_stats_t attached_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    ok &= check(!image.isNull(), "attached detached-snapshot surface renders an image");
    ok &= check(attached_stats.last_rendered_snapshot_sequence == 802U &&
        !attached_stats.pending_update,
        "attached detached-snapshot surface renders the latest snapshot");
    ok &= check_surface_no_text_content_failures(surface,
        "attached detached-snapshot render has no text content failures");
    return ok;
}

bool test_qsg_snapshot_rendering(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(300, 180);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(260.0, 140.0));
    configure_test_font(surface);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false));

    ok &= check(surface.childItems().empty(), "surface has no child items before render");
    window.show();

    const auto has_terminal_background = [&](const QImage& image) {
        return count_matching_pixels(
            image,
            QRect(220, 110, 24, 18),
            is_terminal_background) > 250;
    };

    const QImage image = render_window_until(app, window, has_terminal_background);
    const term::terminal_renderer_stats_t first_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    const term::terminal_cell_metrics_t metrics = test_metrics();
    ok &= check(!image.isNull(), "QSG render produced an image");
    ok &= check(first_stats.paint_completed &&
        first_stats.text_content_rebuilds == 3,
        "first render rebuilds visible text rows");
    ok &= check(first_stats.text_wrapper_order_rebuilt,
        "first render attaches text wrappers in visual order");
    ok &= check_no_text_content_failures(first_stats,
        "first snapshot render has no text content failures");
    ok &= check(first_stats.route_fast_text_cells == 0 &&
        first_stats.frame.simple_content.route_fast_text_cells == 5 &&
        first_stats.route_graphic_geometry_cells ==
            first_stats.frame.text_cells_rendered_as_graphic &&
        first_stats.route_qt_text_layout_runs > 0 &&
        first_stats.qt_text_layout_calls == first_stats.route_qt_text_layout_runs,
        "first render reports candidate eligibility separately from actual routes");
    ok &= check(first_stats.qsg_nodes_created > 0 &&
        first_stats.qsg_nodes_replaced == 0 &&
        first_stats.cache_key_builds ==
            first_stats.text_key_builds + first_stats.rect_key_builds &&
        first_stats.cache_key_bytes ==
            first_stats.text_key_bytes + first_stats.rect_key_bytes,
        "first render reports node and cache-key diagnostics");
    ok &= check(first_stats.frame_text_runs == first_stats.frame.text_runs_emitted &&
        first_stats.frame_selection_rects == first_stats.frame.selection_rects_emitted &&
        first_stats.frame_dirty_row_ranges == 0 &&
        first_stats.frame_packed_rows == first_stats.frame.packed_rows &&
        first_stats.frame_packed_text_cells ==
            first_stats.frame.simple_content.eligible_after_all_gates_cells &&
        first_stats.frame_packed_payload_bytes > 0U,
        "first render reports frame vector sizes with packed sidecar payloads");
    ok &= check(first_stats.background_layer_rebuilt &&
        first_stats.selection_layer_rebuilt &&
        first_stats.graphic_layer_rebuilt &&
        first_stats.decoration_layer_rebuilt &&
        first_stats.cursor_layer_rebuilt &&
        first_stats.cursor_graphic_layer_rebuilt &&
        first_stats.cursor_text_layer_rebuilt &&
        first_stats.overlay_layer_rebuilt,
        "first render builds fixed geometry layers");
    ok &= check(image.width() >= 150 && image.height() >= 80,
        "QSG render target has stable dimensions");
    ok &= check(has_terminal_background(image),
        "empty region is terminal background");
    ok &= check(count_matching_pixels(
        image,
        cell_area(1, 1, 4, metrics),
        [](QColor color) {
            return color.blue() > color.red() && color.blue() > color.green();
        }) > 20,
        "selection region is rendered");
    ok &= check(count_matching_pixels(
        image,
        cell_area(2, 2, 1, metrics),
        [](QColor color) {
            return color.red() > 180 && color.green() > 180 && color.blue() > 180;
        }) > 20,
        "cursor region is rendered");
    const int cursor_glyph_pixels = count_matching_pixels(
        image,
        cell_area(2, 2, 1, metrics),
        [](QColor color) {
            return color.red() + color.green() + color.blue() < 650;
        });
    if (cursor_glyph_pixels <= 4) {
        std::cerr << "INFO: observed cursor contrast pixels: " << cursor_glyph_pixels << '\n';
    }
    ok &= check(cursor_glyph_pixels > 4,
        "block cursor preserves contrasting glyph pixels");
    ok &= check(count_matching_pixels(
        image,
        cell_area(0, 0, 3, metrics),
        [](QColor color) {
            return color.green() > 80 && color.green() > color.red();
        }) > 5,
        "text row is nonblank");
    ok &= check(surface.childItems().empty(), "surface creates no child QQuickItems");

    const term::Terminal_surface_render_invalidation_stats_t pre_blink_hidden_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    term::VNM_TerminalSurface_render_bridge::set_cursor_blink_visible(surface, false);
    ok &= check(window_render_matches(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            invalidation_stats.consumed_updates > pre_blink_hidden_stats.consumed_updates &&
            !invalidation_stats.pending_update                                            &&
            stats.root_reused                                                             &&
            stats.text_content_failures == 0                                              &&
            stats.text_content_rebuilds == 0;
    }), "blink-hidden update reaches a fresh consumed render");
    const term::terminal_renderer_stats_t blink_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    ok &= check(blink_stats.text_content_rebuilds == 0,
        "blink-only update reuses text content");
    ok &= check(!blink_stats.text_wrapper_order_rebuilt,
        "blink-only update leaves text wrapper order untouched");
    ok &= check_no_text_content_failures(blink_stats,
        "blink-hidden update has no text content failures");
    ok &= check(!blink_stats.background_layer_rebuilt &&
        !blink_stats.selection_layer_rebuilt &&
        !blink_stats.graphic_layer_rebuilt &&
        !blink_stats.decoration_layer_rebuilt,
        "blink-only update preserves static geometry layers");
    ok &= check(blink_stats.cursor_layer_rebuilt &&
        !blink_stats.cursor_graphic_layer_rebuilt &&
        blink_stats.cursor_text_layer_rebuilt &&
        !blink_stats.overlay_layer_rebuilt,
        "blink-hidden update rebuilds only cursor geometry and text overlay");
    const QImage blink_hidden_image = window.grabWindow();

    const term::Terminal_surface_render_invalidation_stats_t pre_blink_visible_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    term::VNM_TerminalSurface_render_bridge::set_cursor_blink_visible(surface, true);
    ok &= check(window_render_matches(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            invalidation_stats.consumed_updates > pre_blink_visible_stats.consumed_updates &&
            !invalidation_stats.pending_update                                             &&
            stats.root_reused                                                              &&
            stats.text_content_failures == 0                                               &&
            stats.text_content_rebuilds == 0                                               &&
            stats.cursor_text_layer_rebuilt;
    }), "blink-visible update reaches a fresh consumed render");
    const term::terminal_renderer_stats_t blink_visible_stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    ok &= check(blink_visible_stats.text_content_rebuilds == 0 &&
        blink_visible_stats.cursor_layer_rebuilt &&
        !blink_visible_stats.cursor_graphic_layer_rebuilt &&
        blink_visible_stats.cursor_text_layer_rebuilt,
        "blink-visible update reports cursor text separately from main text content");
    ok &= check_no_text_content_failures(blink_visible_stats,
        "blink-visible update has no text content failures");
    const QImage blink_visible_image = window.grabWindow();
    ok &= check(!images_have_same_pixels(blink_hidden_image, blink_visible_image),
        "cursor blink toggles rendered pixels");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false, {{1, 1}}, QStringLiteral("2")));
    render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            stats.root_reused                &&
            stats.text_content_failures == 0 &&
            stats.text_content_rebuilds == 1 &&
            stats.text_content_reused   == 2 &&
            stats.text_content_removed  == 0;
    });
    const term::terminal_renderer_stats_t dirty_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    const bool dirty_key_stats_ok =
        dirty_stats.text_content_rebuilds == 1 &&
            dirty_stats.text_content_reused == 2 &&
            dirty_stats.text_content_removed == 0 &&
            dirty_stats.text_groups_considered == 3 &&
            dirty_stats.text_groups_dirty == 1 &&
            dirty_stats.text_groups_clean == 2 &&
            dirty_stats.text_clean_reuse_skips == 1 &&
            dirty_stats.text_key_builds == 4 &&
            dirty_stats.text_dirty_row_ranges == 1 &&
            dirty_stats.text_dirty_rows == 1 &&
            !dirty_stats.text_wrapper_order_rebuilt;
    if (!dirty_key_stats_ok) {
        std::cerr << "INFO: dirty text stats rebuilds="
            << dirty_stats.text_content_rebuilds
            << " reused="          << dirty_stats.text_content_reused
            << " removed="         << dirty_stats.text_content_removed
            << " groups="          << dirty_stats.text_groups_considered
            << " dirty_groups="    << dirty_stats.text_groups_dirty
            << " clean_groups="    << dirty_stats.text_groups_clean
            << " clean_skips="     << dirty_stats.text_clean_reuse_skips
            << " text_key_builds=" << dirty_stats.text_key_builds
            << " dirty_ranges="    << dirty_stats.text_dirty_row_ranges
            << " dirty_rows="      << dirty_stats.text_dirty_rows
            << " wrapper_order="   << dirty_stats.text_wrapper_order_rebuilt
            << '\n';
    }
    ok &= check(dirty_key_stats_ok,
        "single dirty content change skips clean text row key rebuild work");
    ok &= check_no_text_content_failures(dirty_stats,
        "single dirty content change has no text content failures");
    ok &= check(dirty_stats.route_fast_text_cells == 0 &&
        dirty_stats.frame.simple_content.route_fast_text_cells > 0 &&
        dirty_stats.frame.simple_content.dirty_eligible_cells == 2 &&
        dirty_stats.frame.simple_content.clean_eligible_cells == 3 &&
        dirty_stats.route_graphic_geometry_cells ==
            dirty_stats.frame.text_cells_rendered_as_graphic &&
        dirty_stats.qsg_nodes_replaced > 0 &&
        dirty_stats.qsg_nodes_destroyed > 0 &&
        dirty_stats.cache_key_builds ==
            dirty_stats.text_key_builds + dirty_stats.rect_key_builds &&
        dirty_stats.cache_key_bytes ==
            dirty_stats.text_key_bytes + dirty_stats.rect_key_bytes,
        "dirty render reports candidates, actual routes, node churn, and cache-key diagnostics");
    ok &= check(dirty_stats.row_cache_hits ==
        dirty_stats.text_content_reused -
        dirty_stats.text_clean_reuse_skips +
        dirty_stats.background_rows_reused +
        dirty_stats.selection_rows_reused +
        dirty_stats.decoration_rows_reused +
        dirty_stats.graphic_rect_rows_reused +
        dirty_stats.graphic_arc_rows_reused &&
            dirty_stats.row_cache_clean_skips ==
        dirty_stats.text_clean_reuse_skips +
        dirty_stats.background_row_clean_reuse_skips +
        dirty_stats.selection_row_clean_reuse_skips +
        dirty_stats.decoration_row_clean_reuse_skips +
        dirty_stats.graphic_rect_row_clean_reuse_skips +
        dirty_stats.graphic_arc_row_clean_reuse_skips,
        "dirty render reports aggregate row-cache hits and clean skips");
    ok &= check(!dirty_stats.background_layer_rebuilt &&
        !dirty_stats.selection_layer_rebuilt &&
        !dirty_stats.graphic_layer_rebuilt &&
        !dirty_stats.decoration_layer_rebuilt &&
        !dirty_stats.cursor_layer_rebuilt &&
        !dirty_stats.cursor_graphic_layer_rebuilt &&
        !dirty_stats.cursor_text_layer_rebuilt &&
        !dirty_stats.overlay_layer_rebuilt,
        "single dirty row preserves unchanged geometry layers");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false, {{0, 5}}, QStringLiteral("2")));
    render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            stats.root_reused                &&
            stats.text_content_failures == 0 &&
            stats.text_content_rebuilds == 0 &&
            stats.text_content_reused == 3   &&
            stats.text_content_removed == 0  &&
            !stats.text_wrapper_order_rebuilt;
    });
    const term::terminal_renderer_stats_t full_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    const bool full_key_stats_ok =
        full_stats.text_content_rebuilds == 0 &&
            full_stats.text_content_reused == 3 &&
            full_stats.text_content_removed == 0 &&
            full_stats.text_groups_considered == 3 &&
            full_stats.text_groups_dirty == 3 &&
            full_stats.text_groups_clean == 0 &&
            full_stats.text_clean_reuse_skips == 0 &&
            full_stats.text_resource_descriptor_reuses == 2 &&
            full_stats.text_key_builds == 3 &&
            full_stats.text_dirty_row_ranges == 1 &&
            full_stats.text_dirty_rows == 5 &&
            !full_stats.text_wrapper_order_rebuilt;
    if (!full_key_stats_ok) {
        std::cerr << "INFO: full-dirty text stats rebuilds="
            << full_stats.text_content_rebuilds
            << " reused="          << full_stats.text_content_reused
            << " removed="         << full_stats.text_content_removed
            << " groups="          << full_stats.text_groups_considered
            << " dirty_groups="    << full_stats.text_groups_dirty
            << " clean_groups="    << full_stats.text_groups_clean
            << " clean_skips="     << full_stats.text_clean_reuse_skips
            << " descriptor_reuses="
            << full_stats.text_resource_descriptor_reuses
            << " text_key_builds=" << full_stats.text_key_builds
            << " dirty_ranges="    << full_stats.text_dirty_row_ranges
            << " dirty_rows="      << full_stats.text_dirty_rows
            << " wrapper_order="   << full_stats.text_wrapper_order_rebuilt
            << '\n';
    }
    ok &= check(full_key_stats_ok,
        "full dirty hint with unchanged content reuses text content before row key rebuilds");
    ok &= check_no_text_content_failures(full_stats,
        "full dirty hint with unchanged content has no text content failures");
    ok &= check(!full_stats.background_layer_rebuilt &&
        !full_stats.selection_layer_rebuilt &&
        !full_stats.graphic_layer_rebuilt &&
        !full_stats.cursor_graphic_layer_rebuilt &&
        !full_stats.decoration_layer_rebuilt,
        "full text repaint preserves unchanged static geometry layers");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(true, {}, QStringLiteral("2")));
    const int base_red = average_red(image, QRect(220, 110, 24, 18));
    const QImage bell_image = render_window_until(app, window, [&](const QImage& candidate) {
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            stats.overlay_layer_rebuilt      &&
            stats.text_content_failures == 0 &&
            average_red(candidate, QRect(220, 110, 24, 18)) > base_red;
    });
    const term::terminal_renderer_stats_t bell_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    ok &= check(average_red(bell_image, QRect(220, 110, 24, 18)) >
        base_red,
        "visual bell overlay brightens background");
    ok &= check(bell_stats.text_content_rebuilds == 0 &&
        !bell_stats.background_layer_rebuilt &&
        !bell_stats.selection_layer_rebuilt &&
        !bell_stats.graphic_layer_rebuilt &&
        !bell_stats.decoration_layer_rebuilt &&
        !bell_stats.cursor_layer_rebuilt &&
        !bell_stats.cursor_graphic_layer_rebuilt &&
        !bell_stats.cursor_text_layer_rebuilt &&
        bell_stats.overlay_layer_rebuilt,
        "visual-bell-only update rebuilds only overlay geometry");
    ok &= check_no_text_content_failures(bell_stats,
        "visual-bell-only update has no text content failures");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(surface, make_sparse_snapshot());
    render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t render_stats =
            term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
        return
            stats.last_rendered_snapshot_sequence == 203U &&
            !stats.pending_update                         &&
            render_stats.text_content_failures == 0       &&
            render_stats.text_content_rebuilds == 1       &&
            render_stats.text_content_reused == 0         &&
            render_stats.text_content_removed == 2;
    });
    const term::terminal_renderer_stats_t sparse_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    ok &= check(sparse_stats.root_reused &&
        sparse_stats.text_content_rebuilds == 1 &&
        sparse_stats.text_content_reused == 0 &&
        sparse_stats.text_content_removed == 2,
        "rows becoming empty remove stale text content and reuse the scene graph root");
    ok &= check_no_text_content_failures(sparse_stats,
        "rows becoming empty has no text content failures");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_foreground_only_blank_snapshot());
    render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t render_stats =
            term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
        return
            stats.last_rendered_snapshot_sequence == 204U &&
            !stats.pending_update                         &&
            render_stats.text_content_failures == 0       &&
            render_stats.text_content_rebuilds == 0       &&
            render_stats.text_content_removed == 1;
    });
    const term::terminal_renderer_stats_t foreground_blank_stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    ok &= check(foreground_blank_stats.root_reused &&
        foreground_blank_stats.text_content_rebuilds == 0 &&
        foreground_blank_stats.text_content_removed == 1,
        "foreground-only blank cells remove stale text resources");
    ok &= check_no_text_content_failures(foreground_blank_stats,
        "foreground-only blank cells have no text content failures");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(surface, make_snapshot(false));
    render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        return stats.last_rendered_snapshot_sequence == 200U && !stats.pending_update;
    });

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(surface, make_shrunk_snapshot());
    render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t render_stats =
            term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
        return
            stats.last_rendered_snapshot_sequence == 202U &&
            !stats.pending_update                         &&
            render_stats.text_content_failures == 0       &&
            render_stats.text_content_rebuilds == 1       &&
            render_stats.text_content_reused == 0         &&
            render_stats.text_content_removed == 2;
    });
    const term::terminal_renderer_stats_t shrink_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    ok &= check(shrink_stats.root_reused &&
        shrink_stats.text_content_rebuilds == 1 &&
        shrink_stats.text_content_reused == 0 &&
        shrink_stats.text_content_removed == 2,
        "grid shrink removes stale text content and reuses the scene graph root");
    ok &= check_no_text_content_failures(shrink_stats,
        "grid shrink has no text content failures");
    return ok;
}

bool test_qsg_text_leaf_batching(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(340.0, 90.0));
    configure_test_font(surface);
    auto render_profiler = std::make_shared<term::Hierarchical_profiler>();
    term::VNM_TerminalSurface_render_bridge::set_render_profiler(surface, render_profiler);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_dense_same_color_row_snapshot());
    window.show();

    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        307U,
        1),
        "dense same-color row renders as one rebuilt text resource row");

    const term::terminal_renderer_stats_t stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    if (profile_scopes_available()) {
        ok &= check(prepare_text_layout_call_count(surface) == 1U,
            "dense same-color row coalesces to one prepare_text_layout call");
        ok &= check(ascii_text_coalescing_context_call_count(surface) == 1U,
            "dense same-color row computes the ASCII coalescing font gate once");
    }
    ok &= check(stats.text_leaf_nodes_created == 1,
        "dense same-color row batches populated cells into one QSGTextNode leaf");
    ok &= check(stats.text_leaf_nodes_created < 20 / 4,
        "dense same-color row creates far fewer QSGTextNode leaves than populated cells");
    ok &= check_no_text_content_failures(stats,
        "dense same-color row has no text content failures");

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const QImage image = window.grabWindow();
    ok &= check(!image.isNull(), "dense same-color row produced an image");
    ok &= check(cell_is_nonblank(image, 0, 0, metrics) &&
        cell_is_nonblank(image, 0, 9, metrics) &&
        cell_is_nonblank(image, 0, 19, metrics),
        "dense same-color row renders text at cell-stable positions");
    ok &= check(mostly_background(image, 1, 0, 5, metrics),
        "dense same-color row leaves the following row untouched");

    render_profiler->reset();
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_dense_style_boundary_row_snapshot());
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        312U,
        1),
        "style boundary row renders as one rebuilt text resource row");
    if (profile_scopes_available()) {
        ok &= check(prepare_text_layout_call_count(surface) == 3U,
            "style boundary row does not coalesce across foreground changes");
        ok &= check(ascii_text_coalescing_context_call_count(surface) <= 1U,
            "style boundary row computes or reuses the ASCII coalescing font gate");
    }
    const QImage style_image = window.grabWindow();
    ok &= check(!style_image.isNull() &&
        cell_is_nonblank(style_image, 0, 0, metrics) &&
        cell_is_nonblank(style_image, 0, 10, metrics) &&
        cell_is_nonblank(style_image, 0, 19, metrics),
        "style boundary row keeps sampled cells visible");

    render_profiler->reset();
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_dense_same_foreground_style_boundary_row_snapshot());
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        317U,
        1),
        "same-foreground style boundary row renders as one rebuilt text resource row");
    if (profile_scopes_available()) {
        ok &= check(prepare_text_layout_call_count(surface) == 1U,
            "same-foreground style boundary row coalesces across non-text style changes");
        ok &= check(ascii_text_coalescing_context_call_count(surface) <= 1U,
            "same-foreground style boundary row computes or reuses the ASCII coalescing font gate");
    }
    const term::terminal_renderer_stats_t same_foreground_stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    ok &= check(same_foreground_stats.background_layer_rebuilt &&
        same_foreground_stats.decoration_layer_rebuilt,
        "same-foreground style boundary still updates background and decoration layers");
    const QImage same_foreground_image = window.grabWindow();
    ok &= check(!same_foreground_image.isNull() &&
        cell_is_nonblank(same_foreground_image, 0, 0, metrics) &&
        cell_is_nonblank(same_foreground_image, 0, 10, metrics) &&
        cell_is_nonblank(same_foreground_image, 0, 19, metrics),
        "same-foreground style boundary row keeps sampled cells visible");

    render_profiler->reset();
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_dense_same_foreground_style_transition_snapshot(318U, false));
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        318U,
        1),
        "same-foreground style transition baseline renders one text resource row");
    const QImage default_style_image = window.grabWindow();
    ok &= check(!default_style_image.isNull(),
        "same-foreground style transition baseline produces an image");
    const QRect transition_row_area = cell_area(0, 0, 20, metrics);
    ok &= check(count_highlighted_background_pixels(
        default_style_image,
        0,
        0,
        20,
        metrics) <=
            static_cast<int>(static_cast<qreal>(transition_row_area.width() *
            transition_row_area.height()) *
        0.02),
        "same-foreground style transition baseline has no highlighted background pixels");

    render_profiler->reset();
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_dense_same_foreground_style_transition_snapshot(319U, true));
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        319U,
        0),
        "same-foreground style transition reuses text resource content");
    const term::terminal_renderer_stats_t style_transition_stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    const QImage highlighted_style_image = window.grabWindow();
    ok &= check(style_transition_stats.text_content_reused == 1,
        "same-foreground style transition reuses the cached text resource row");
    ok &= check(style_transition_stats.background_layer_rebuilt &&
        style_transition_stats.background_rows_rebuilt == 1 &&
        style_transition_stats.decoration_layer_rebuilt &&
        style_transition_stats.decoration_rows_rebuilt == 1,
        "same-foreground style transition rebuilds non-text style layers");
    const int highlighted_background_pixels = count_highlighted_background_pixels(
        highlighted_style_image,
        0,
        0,
        20,
        metrics);
    const int highlighted_underline_pixels =
        count_underline_pixels(highlighted_style_image, 0, 0, 20, metrics);
    if (highlighted_background_pixels <=
        static_cast<int>(static_cast<qreal>(transition_row_area.width() *
                transition_row_area.height()) *
            0.50) ||
        highlighted_underline_pixels <= 20)
    {
        std::cerr << "INFO: same-foreground highlighted pixels: background="
            << highlighted_background_pixels
            << " underline=" << highlighted_underline_pixels << '\n';
    }
    ok &= check(!highlighted_style_image.isNull() &&
        highlighted_background_pixels >
            static_cast<int>(static_cast<qreal>(transition_row_area.width() *
                transition_row_area.height()) *
                0.50),
        "same-foreground style transition paints the highlighted background");
    ok &= check(highlighted_underline_pixels > 20,
        "same-foreground style transition paints the underline after text reuse");

    render_profiler->reset();
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_dense_same_foreground_style_transition_snapshot(320U, false));
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        320U,
        0),
        "same-foreground style revert reuses text resource content");
    const term::terminal_renderer_stats_t style_revert_stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    const QImage reverted_style_image = window.grabWindow();
    const int reverted_background_pixels =
        count_highlighted_background_pixels(reverted_style_image, 0, 0, 20, metrics);
    const int reverted_underline_pixels =
        count_underline_pixels(reverted_style_image, 0, 0, 20, metrics);
    if (reverted_background_pixels >  4 ||
        reverted_underline_pixels  >= highlighted_underline_pixels / 2)
    {
        std::cerr << "INFO: same-foreground reverted pixels: background="
            << reverted_background_pixels
            << " underline=" << reverted_underline_pixels << '\n';
    }
    ok &= check(style_revert_stats.text_content_reused == 1,
        "same-foreground style revert reuses the cached text resource row");
    ok &= check(style_revert_stats.background_layer_rebuilt &&
        style_revert_stats.decoration_layer_rebuilt,
        "same-foreground style revert rebuilds non-text style layers");
    ok &= check(!reverted_style_image.isNull() && reverted_background_pixels <= 4,
        "same-foreground style revert clears the highlighted background");
    ok &= check(reverted_underline_pixels < highlighted_underline_pixels / 2,
        "same-foreground style revert clears the underline after text reuse");

    render_profiler->reset();
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_dense_wide_boundary_row_snapshot());
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        313U,
        1),
        "wide boundary row renders as one rebuilt text resource row");
    if (profile_scopes_available()) {
        ok &= check(prepare_text_layout_call_count(surface) == 3U,
            "wide boundary row leaves non-ASCII wide text off the coalesced ASCII path");
        ok &= check(ascii_text_coalescing_context_call_count(surface) <= 1U,
            "wide boundary row computes or reuses the ASCII coalescing font gate");
    }
    const QImage wide_image = window.grabWindow();
    ok &= check(!wide_image.isNull() &&
        cell_is_nonblank(wide_image, 0, 0, metrics) &&
        cell_is_nonblank(wide_image, 0, 10, metrics) &&
        cell_is_nonblank(wide_image, 0, 19, metrics),
        "wide boundary row keeps ASCII and wide cells visible");

    constexpr int k_long_dense_column_count = 600;
    render_profiler->reset();
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_dense_same_color_row_snapshot(k_long_dense_column_count, 314U));
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        314U,
        1),
        "long dense row renders as one rebuilt text resource row");
    if (profile_scopes_available()) {
        const std::uint64_t long_prepare_calls = prepare_text_layout_call_count(surface);
        ok &= check(long_prepare_calls == 2U,
            "long dense row coalesces into two bounded text layout chunks");
        ok &= check(ascii_text_coalescing_context_call_count(surface) <= 1U,
            "long dense row computes or reuses the ASCII coalescing font gate");
    }
    const QImage long_image = window.grabWindow();
    ok &= check(!long_image.isNull() &&
        cell_is_nonblank(long_image, 0, 0, metrics) &&
        cell_is_nonblank(long_image, 0, 19, metrics),
        "long dense row keeps visible sampled cells aligned");

    render_profiler->reset();
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_multirow_dense_same_color_snapshot(3, 20, 315U));
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        315U,
        3),
        "multi-row dense render rebuilds every dense text resource row");
    if (profile_scopes_available()) {
        ok &= check(prepare_text_layout_call_count(surface) == 3U,
            "multi-row dense render coalesces each row to one text layout");
        ok &= check(ascii_text_coalescing_context_call_count(surface) <= 1U,
            "multi-row dense render computes or reuses the ASCII coalescing font gate");
    }

    render_profiler->reset();
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_dense_same_color_row_snapshot(1, 316U));
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        316U,
        1),
        "single-cell row renders as one rebuilt text resource row");
    if (profile_scopes_available()) {
        ok &= check(prepare_text_layout_call_count(surface) == 1U,
            "single-cell row keeps one prepare_text_layout call");
        ok &= check(ascii_text_coalescing_context_call_count(surface) == 0U,
            "single-cell row skips the ASCII coalescing font gate");
    }
    return ok;
}

bool test_qsg_text_resource_key_ignores_hyperlink_metadata(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    Direct_render_item item;
    item.setParentItem(window.contentItem());
    item.set_frame(make_direct_hyperlink_text_resource_frame(false, metrics));
    window.show();

    const QImage baseline_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.paint_completed              &&
            stats.text_content_failures   == 0 &&
            stats.text_content_rebuilds   == 1 &&
            stats.decoration_rows_rebuilt == 0;
    });
    ok &= check(!baseline_image.isNull(),
        "hyperlink text-resource baseline renders an image");
    ok &= check(count_underline_pixels(baseline_image, 0, 0, 20, metrics) <= 4,
        "hyperlink text-resource baseline has no hyperlink underline pixels");

    item.set_frame(make_direct_hyperlink_text_resource_frame(true, metrics));
    const QImage hyperlink_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.root_reused                          &&
            stats.paint_completed                      &&
            stats.text_content_failures == 0           &&
            stats.text_content_rebuilds == 0           &&
            stats.text_content_reused == 1             &&
            stats.text_resource_descriptor_reuses == 1 &&
            stats.text_key_builds == 1                 &&
            stats.decoration_layer_rebuilt             &&
            stats.decoration_rows_rebuilt == 1;
    });
    const term::terminal_renderer_stats_t hyperlink_stats = item.last_stats();
    const int hyperlink_underline_pixels =
        count_underline_pixels(hyperlink_image, 0, 0, 20, metrics);
    if (hyperlink_underline_pixels <= 20) {
        std::cerr << "INFO: hyperlink underline pixels after text reuse: "
            << hyperlink_underline_pixels << '\n';
    }
    ok &= check(hyperlink_stats.text_content_reused == 1,
        "hyperlink metadata transition reuses the cached text resource row");
    ok &= check(hyperlink_stats.text_resource_descriptor_reuses == 1 &&
        hyperlink_stats.text_key_builds == 1 &&
        hyperlink_stats.route_fast_text_cells == 0,
        "hyperlink metadata transition reuses before full text-resource key work");
    ok &= check(!hyperlink_image.isNull() && hyperlink_underline_pixels > 20,
        "hyperlink metadata transition paints underline pixels after text reuse");

    item.set_frame(make_direct_hyperlink_text_resource_frame(false, metrics));
    const QImage reverted_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.root_reused                          &&
            stats.paint_completed                      &&
            stats.text_content_failures == 0           &&
            stats.text_content_rebuilds == 0           &&
            stats.text_content_reused == 1             &&
            stats.text_resource_descriptor_reuses == 1 &&
            stats.text_key_builds == 1                 &&
            stats.decoration_layer_rebuilt             &&
            stats.decoration_rows_removed == 1;
    });
    const term::terminal_renderer_stats_t reverted_stats = item.last_stats();
    const int reverted_underline_pixels =
        count_underline_pixels(reverted_image, 0, 0, 20, metrics);
    if (reverted_underline_pixels >= hyperlink_underline_pixels / 2) {
        std::cerr << "INFO: hyperlink underline pixels after revert: "
            << reverted_underline_pixels << '\n';
    }
    ok &= check(reverted_stats.text_content_reused == 1,
        "hyperlink metadata revert reuses the cached text resource row");
    ok &= check(reverted_stats.text_resource_descriptor_reuses == 1 &&
        reverted_stats.text_key_builds == 1 &&
        reverted_stats.route_fast_text_cells == 0,
        "hyperlink metadata revert reuses before full text-resource key work");
    ok &= check(!reverted_image.isNull() &&
        reverted_underline_pixels < hyperlink_underline_pixels / 2,
        "hyperlink metadata revert clears underline pixels after text reuse");
    return ok;
}

bool test_qsg_text_resource_descriptor_ignores_decoration_metadata(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    Direct_render_item item;
    item.setParentItem(window.contentItem());
    item.set_frame(make_direct_text_metadata_decoration_frame(false, false, metrics));
    window.show();

    const QImage baseline_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.paint_completed              &&
            stats.text_content_failures   == 0 &&
            stats.text_content_rebuilds   == 1 &&
            stats.decoration_rows_rebuilt == 0;
    });
    ok &= check(!baseline_image.isNull(),
        "decoration metadata baseline renders an image");
    const int baseline_strike_pixels =
        count_strike_pixels(baseline_image, 0, 0, 20, metrics);
    ok &= check(count_underline_pixels(baseline_image, 0, 0, 20, metrics) <= 4,
        "decoration metadata baseline has no underline pixels");

    item.set_frame(make_direct_text_metadata_decoration_frame(true, true, metrics));
    const QImage decorated_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.root_reused                          &&
            stats.paint_completed                      &&
            stats.text_content_failures == 0           &&
            stats.text_content_rebuilds == 0           &&
            stats.text_content_reused == 1             &&
            stats.text_resource_descriptor_reuses == 1 &&
            stats.text_key_builds == 1                 &&
            stats.decoration_layer_rebuilt             &&
            stats.decoration_rows_rebuilt == 1;
    });
    const term::terminal_renderer_stats_t decorated_stats = item.last_stats();
    const int decorated_underline_pixels =
        count_underline_pixels(decorated_image, 0, 0, 20, metrics);
    const int decorated_strike_pixels =
        count_strike_pixels(decorated_image, 0, 0, 20, metrics);
    if (decorated_underline_pixels <= 20 ||
        decorated_strike_pixels    <= baseline_strike_pixels + 20)
    {
        std::cerr << "INFO: decoration metadata pixels after text reuse: underline="
            << decorated_underline_pixels
            << " baseline_strike=" << baseline_strike_pixels
            << " strike="          << decorated_strike_pixels << '\n';
    }
    ok &= check(decorated_stats.text_resource_descriptor_reuses == 1 &&
        decorated_stats.route_fast_text_cells == 0,
        "underline and strike metadata transition reuses before full text-resource key work");
    ok &= check(!decorated_image.isNull() &&
        decorated_underline_pixels > 20 &&
        decorated_strike_pixels > baseline_strike_pixels + 20,
        "underline and strike metadata transition paints decoration pixels after text reuse");

    item.set_frame(make_direct_text_metadata_decoration_frame(false, false, metrics));
    const QImage reverted_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.root_reused                          &&
            stats.paint_completed                      &&
            stats.text_content_failures == 0           &&
            stats.text_content_rebuilds == 0           &&
            stats.text_content_reused == 1             &&
            stats.text_resource_descriptor_reuses == 1 &&
            stats.text_key_builds == 1                 &&
            stats.decoration_layer_rebuilt             &&
            stats.decoration_rows_removed == 1;
    });
    const int reverted_underline_pixels =
        count_underline_pixels(reverted_image, 0, 0, 20, metrics);
    const int reverted_strike_pixels =
        count_strike_pixels(reverted_image, 0, 0, 20, metrics);
    if (reverted_underline_pixels >= decorated_underline_pixels / 2 ||
        reverted_strike_pixels    >  baseline_strike_pixels + 8)
    {
        std::cerr << "INFO: decoration metadata pixels after revert: underline="
            << reverted_underline_pixels
            << " baseline_strike=" << baseline_strike_pixels
            << " strike="          << reverted_strike_pixels << '\n';
    }
    ok &= check(!reverted_image.isNull() &&
        reverted_underline_pixels < decorated_underline_pixels / 2 &&
        reverted_strike_pixels <= baseline_strike_pixels + 8,
        "underline and strike metadata revert clears decoration pixels after text reuse");
    return ok;
}

term::Terminal_render_frame build_qsg_sidecar_test_frame(
    const term::Terminal_render_snapshot&  snapshot,
    term::terminal_cell_metrics_t          metrics)
{
    return term::build_terminal_render_frame(
        &snapshot,
        QSizeF(
            static_cast<qreal>(snapshot.grid_size.columns) * metrics.width,
            static_cast<qreal>(snapshot.grid_size.rows) * metrics.height),
        metrics,
        {},
        true);
}

bool render_frame_matches_without_packed_sidecars(
    QGuiApplication&                       app,
    const term::Terminal_render_frame&     packed_frame,
    const char*                            label)
{
    bool ok = true;

    const direct_frame_render_result_t packed_render = render_frame_in_fresh_window(
        app,
        packed_frame,
        static_cast<int>(packed_frame.packed_rows.size()));
    const QImage& packed_image = packed_render.image;
    const term::terminal_renderer_stats_t& packed_stats = packed_render.stats;
    ok &= check(!packed_image.isNull(), label);
    ok &= check(!packed_stats.root_reused && packed_stats.qsg_nodes_created > 0,
        "packed-sidecar render uses a fresh scene graph root");
    ok &= check(packed_stats.frame_packed_rows > 0 &&
        packed_stats.frame_packed_payload_bytes > 0U &&
        packed_stats.route_fast_text_cells == 0,
        "packed-sidecar render publishes sidecar stats without using the fast text route");

    term::Terminal_render_frame unpacked_frame = packed_frame;
    clear_packed_sidecars(unpacked_frame);
    const direct_frame_render_result_t unpacked_render =
        render_frame_in_fresh_window(app, unpacked_frame, 0);
    const QImage& unpacked_image = unpacked_render.image;
    const term::terminal_renderer_stats_t& unpacked_stats = unpacked_render.stats;
    ok &= check(!unpacked_image.isNull(),
        "packed-sidecar cleared render produces an image");
    ok &= check(!unpacked_stats.root_reused && unpacked_stats.qsg_nodes_created > 0,
        "packed-sidecar cleared render uses a fresh scene graph root");
    if (!images_have_same_pixels(packed_image, unpacked_image)) {
        save_visual_failure_image(packed_image, QString::fromUtf8(label), QStringLiteral("packed"));
        save_visual_failure_image(
            unpacked_image,
            QString::fromUtf8(label),
            QStringLiteral("packed_cleared"));
    }
    ok &= check(images_have_same_pixels(packed_image, unpacked_image),
        "clearing only packed sidecars does not change rendered pixels");
    ok &= check(unpacked_stats.route_fast_text_cells == 0,
        "packed-sidecar cleared render still leaves the fast text route unused");
    return ok;
}

bool test_qsg_packed_sidecars_do_not_affect_visual_output(QGuiApplication& app)
{
    bool ok = true;
    const term::terminal_cell_metrics_t metrics = test_metrics();

    term::Terminal_viewport_state cursor_viewport;
    cursor_viewport.visible_rows = 3;
    term::Terminal_render_snapshot cursor_snapshot =
        term::make_empty_render_snapshot({3, 12}, cursor_viewport, 1201U);
    cursor_snapshot.color_state      = color_state();
    cursor_snapshot.cursor           = {{0, 1}, term::Terminal_cursor_shape::BLOCK, true, false};
    cursor_snapshot.dirty_row_ranges = {{0, 3}};
    cursor_snapshot.cells.push_back({{0, 0}, QStringLiteral("A"), 0U, 1, false, 0U});
    cursor_snapshot.cells.push_back({{0, 1}, QStringLiteral("B"), 0U, 1, false, 0U});
    cursor_snapshot.cells.push_back({{0, 2}, QStringLiteral("C"), 0U, 1, false, 0U});

    const term::Terminal_render_frame cursor_frame =
        build_qsg_sidecar_test_frame(cursor_snapshot, metrics);
    ok &= check(cursor_frame.stats.packed_text_cells == 2 &&
        cursor_frame.stats.packed_graphic_cells == 0 &&
        cursor_frame.cursor_text_runs.size() == 1U,
        "packed simple ASCII block-cursor frame has sidecars and cursor text");
    ok &= render_frame_matches_without_packed_sidecars(
        app,
        cursor_frame,
        "packed simple ASCII block cursor render produced an image");

    term::Terminal_viewport_state graphics_viewport;
    graphics_viewport.active_buffer = term::Terminal_buffer_id::ALTERNATE;
    graphics_viewport.visible_rows  = 3;
    term::Terminal_render_snapshot graphics_snapshot =
        term::make_empty_render_snapshot({3, 12}, graphics_viewport, 1202U);
    graphics_snapshot.color_state      = color_state();
    graphics_snapshot.cursor.visible   = false;
    graphics_snapshot.dirty_row_ranges = {{0, 3}};
    graphics_snapshot.cells.push_back({{0, 0}, QStringLiteral("\u250c"), 0U, 1, false, 0U});
    graphics_snapshot.cells.push_back({{0, 1}, QStringLiteral("\u2500"), 0U, 1, false, 0U});
    graphics_snapshot.cells.push_back({{1, 0}, QStringLiteral("\u2510"), 0U, 1, false, 0U});
    graphics_snapshot.cells.push_back({{2, 0}, QStringLiteral("A"), 0U, 1, false, 0U});

    const term::Terminal_render_frame graphics_frame =
        build_qsg_sidecar_test_frame(graphics_snapshot, metrics);
    ok &= check(graphics_frame.stats.packed_text_cells == 1 &&
        graphics_frame.stats.packed_graphic_cells == 3 &&
        graphics_frame.packed_rows.front().active_buffer ==
            term::Terminal_buffer_id::ALTERNATE,
        "packed mixed box-graphic/ASCII frame has sidecars and alternate-buffer row identity");
    ok &= render_frame_matches_without_packed_sidecars(
        app,
        graphics_frame,
        "packed mixed terminal box graphics render produced an image");
    return ok;
}

bool test_qsg_text_coalescing_rejects_cell_width_drift(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        make_dense_same_color_row_snapshot();
    const QFont font = term::vnm_terminal_font(QString(), 16.0);
    term::terminal_cell_metrics_t mismatched_metrics = cell_metrics_for_font(font);
    mismatched_metrics.width += 0.005;
    const term::Terminal_render_frame frame = term::build_terminal_render_frame(
        snapshot.get(),
        QSizeF(340.0, 90.0),
        mismatched_metrics,
        {},
        false);

    term::Qsg_terminal_renderer renderer;
    term::terminal_renderer_stats_t stats;
    term::Hierarchical_profiler profiler;
    QSGNode* node = nullptr;
    {
        term::Active_profiler_binding binding(&profiler);
        node = renderer.update_node(nullptr, &window, frame, font, 1.0, {}, stats);
    }

    ok &= check(node != nullptr, "cell-width-drift direct QSG render creates a node");
    ok &= check(stats.paint_completed &&
        stats.text_content_failures == 0 &&
        stats.text_content_rebuilds == 1,
        "cell-width-drift direct QSG render builds one text resource row");
    if (profile_scopes_available()) {
        ok &= check(profile_call_count(profiler.root_snapshot(), "prepare_text_layout") == 20U,
            "cell-width-drift direct QSG render keeps one prepare_text_layout call per cell");
        ok &= check(profile_call_count(
            profiler.root_snapshot(),
            "ascii_text_coalescing_context") == 1U,
            "cell-width-drift direct QSG render evaluates the ASCII coalescing font gate once");
    }

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_text_resource_key_separates_coalesced_ascii_from_original_runs(
    QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);
    const term::terminal_cell_metrics_t metrics = cell_metrics_for_font(font);
    const term::Terminal_render_frame coalescible_frame = make_direct_text_frame(
        {
            make_direct_text_run(0, 1, QStringLiteral("A"), metrics),
            make_direct_text_run(1, 1, QStringLiteral("B"), metrics),
        },
        metrics);
    const term::Terminal_render_frame original_multichar_frame = make_direct_text_frame(
        {
            make_direct_text_run(0, 2, QStringLiteral("AB"), metrics),
        },
        metrics);

    term::Qsg_terminal_renderer renderer;
    term::terminal_renderer_stats_t first_stats;
    term::Hierarchical_profiler first_profiler;
    QSGNode* node = nullptr;
    {
        term::Active_profiler_binding binding(&first_profiler);
        node = renderer.update_node(
            nullptr,
            &window,
            coalescible_frame,
            font,
            1.0,
            {},
            first_stats);
    }

    ok &= check(node != nullptr, "coalesced/original key test creates a node");
    ok &= check(first_stats.paint_completed &&
        first_stats.text_content_failures == 0 &&
        first_stats.text_content_rebuilds == 1,
        "coalesced/original key test builds the initial coalesced text resource");
    if (profile_scopes_available()) {
        ok &= check(
            profile_call_count(first_profiler.root_snapshot(), "prepare_text_layout") == 1U,
            "coalesced/original key test builds the initial row as one coalesced layout");
    }

    term::terminal_renderer_stats_t second_stats;
    node  = renderer.update_node(
        node,
        &window,
        original_multichar_frame,
        font,
        1.0,
        {},
        second_stats);
    ok   &= check(node != nullptr, "coalesced/original key test keeps the node alive");
    ok   &= check(second_stats.paint_completed &&
        second_stats.text_content_failures == 0 &&
        second_stats.text_content_rebuilds == 1 &&
        second_stats.text_content_reused == 0,
        "original multi-character ASCII run does not alias a coalesced ASCII resource key");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_background_selection_row_cache(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const term::Terminal_render_rect background_row_zero =
        make_direct_row_rect(0, 0, 3, QColor(18, 46, 36), metrics);
    const term::Terminal_render_rect background_row_one =
        make_direct_row_rect(1, 1, 4, QColor(40, 28, 86), metrics);
    const term::Terminal_render_rect background_row_one_changed = make_direct_row_rect(
        1,
        1,
        4,
        QColor(92, 44, 34),
        metrics);
    const term::Terminal_render_rect selection_row_one = make_direct_row_rect(
        1,
        2,
        3,
        QColor(54, 96, 154, 190),
        metrics);
    const term::Terminal_render_rect selection_row_two = make_direct_row_rect(
        2,
        4,
        2,
        QColor(74, 116, 174, 190),
        metrics);
    const term::Terminal_render_rect selection_row_two_changed = make_direct_row_rect(
        2,
        4,
        3,
        QColor(104, 136, 194, 190),
        metrics);

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats,
        qreal device_pixel_ratio = 1.0) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            device_pixel_ratio,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };

    const term::Terminal_render_frame empty_frame =
        make_direct_rect_cache_frame({}, {}, metrics, false);
    term::terminal_renderer_stats_t first_empty_stats;
    ok &= check(update(empty_frame, first_empty_stats),
        "background/selection row cache creates a node for an empty direct frame");
    ok &= check(first_empty_stats.background_layer_rebuilt &&
        first_empty_stats.selection_layer_rebuilt &&
        first_empty_stats.background_rows_rebuilt == 0 &&
        first_empty_stats.selection_rows_rebuilt == 0,
        "first empty direct frame initializes background and selection layer caches");

    term::terminal_renderer_stats_t empty_reuse_stats;
    ok &= check(update(empty_frame, empty_reuse_stats),
        "background/selection row cache rerenders the unchanged empty direct frame");
    ok &= check(empty_reuse_stats.root_reused &&
        !empty_reuse_stats.background_layer_rebuilt &&
        !empty_reuse_stats.selection_layer_rebuilt &&
        empty_reuse_stats.background_row_clean_reuse_skips == 1 &&
        empty_reuse_stats.selection_row_clean_reuse_skips == 1 &&
        empty_reuse_stats.background_rows_reused == 0 &&
        empty_reuse_stats.selection_rows_reused == 0,
        "unchanged empty direct frame keeps initialized layer caches clean");

    const term::Terminal_render_frame base_frame = make_direct_rect_cache_frame(
        { background_row_zero, background_row_one },
        { selection_row_one, selection_row_two },
        metrics);
    term::terminal_renderer_stats_t base_stats;
    ok &= check(update(base_frame, base_stats),
        "background/selection row cache renders the base row-rect frame");
    ok &= check(base_stats.background_layer_rebuilt &&
        base_stats.selection_layer_rebuilt &&
        base_stats.background_rows_rebuilt == 2 &&
        base_stats.selection_rows_rebuilt == 2,
        "base row-rect frame builds background and selection row resources");

    term::terminal_renderer_stats_t row_reuse_stats;
    ok &= check(update(base_frame, row_reuse_stats),
        "background/selection row cache rerenders the unchanged row-rect frame");
    ok &= check(!row_reuse_stats.background_layer_rebuilt &&
        !row_reuse_stats.selection_layer_rebuilt &&
        row_reuse_stats.background_row_clean_reuse_skips == 1 &&
        row_reuse_stats.selection_row_clean_reuse_skips == 1 &&
        row_reuse_stats.background_rows_reused == 0 &&
        row_reuse_stats.selection_rows_reused == 0,
        "unchanged empty-dirty row-rect frame skips background and selection row work");

    const term::Terminal_render_frame dirty_base_frame =
        with_all_rows_dirty(base_frame);
    term::terminal_renderer_stats_t dirty_reuse_stats;
    ok &= check(update(dirty_base_frame, dirty_reuse_stats),
        "background/selection row cache rerenders the dirty unchanged row-rect frame");
    ok &= check(!dirty_reuse_stats.background_layer_rebuilt &&
        !dirty_reuse_stats.selection_layer_rebuilt &&
        dirty_reuse_stats.background_row_clean_reuse_skips == 1 &&
        dirty_reuse_stats.selection_row_clean_reuse_skips == 1 &&
        dirty_reuse_stats.background_rows_reused == 0 &&
        dirty_reuse_stats.selection_rows_reused == 0,
        "dirty unchanged row-rect frame skips unchanged geometry layers");

    term::terminal_renderer_stats_t dpr_reuse_stats;
    ok &= check(update(base_frame, dpr_reuse_stats, 2.0),
        "background/selection row cache rerenders non-antialiased rows after DPR changes");
    ok &= check(!dpr_reuse_stats.background_layer_rebuilt &&
        !dpr_reuse_stats.selection_layer_rebuilt &&
        dpr_reuse_stats.background_row_clean_reuse_skips == 1 &&
        dpr_reuse_stats.selection_row_clean_reuse_skips == 1 &&
        dpr_reuse_stats.background_rows_reused == 0 &&
        dpr_reuse_stats.selection_rows_reused == 0,
        "DPR changes do not reopen non-antialiased background or selection row caches");

    const term::Terminal_render_frame background_changed_frame = make_direct_rect_cache_frame(
        { background_row_zero, background_row_one_changed },
        { selection_row_one, selection_row_two },
        metrics);
    term::terminal_renderer_stats_t background_changed_stats;
    ok &= check(update(background_changed_frame, background_changed_stats),
        "background/selection row cache renders a background-only row mutation");
    ok &= check(background_changed_stats.background_layer_rebuilt &&
        !background_changed_stats.selection_layer_rebuilt &&
        background_changed_stats.background_rows_rebuilt == 1 &&
        background_changed_stats.background_rows_reused == 1 &&
        background_changed_stats.selection_row_clean_reuse_skips == 1 &&
        background_changed_stats.selection_rows_reused == 0,
        "one changed background row bypasses the empty-dirty gate only for background");

    const term::Terminal_render_frame selection_changed_frame = make_direct_rect_cache_frame(
        { background_row_zero, background_row_one_changed },
        { selection_row_one, selection_row_two_changed },
        metrics);
    term::terminal_renderer_stats_t selection_changed_stats;
    ok &= check(update(selection_changed_frame, selection_changed_stats),
        "background/selection row cache renders a selection-only row mutation");
    ok &= check(!selection_changed_stats.background_layer_rebuilt &&
        selection_changed_stats.selection_layer_rebuilt &&
        selection_changed_stats.background_row_clean_reuse_skips == 1 &&
        selection_changed_stats.background_rows_reused == 0 &&
        selection_changed_stats.selection_rows_rebuilt == 1 &&
        selection_changed_stats.selection_rows_reused == 1,
        "one changed selection row bypasses the empty-dirty gate only for selection");

    const term::Terminal_render_frame selection_removed_frame = make_direct_rect_cache_frame(
        { background_row_zero, background_row_one_changed },
        { selection_row_one },
        metrics);
    term::terminal_renderer_stats_t selection_removed_stats;
    ok &= check(update(selection_removed_frame, selection_removed_stats),
        "background/selection row cache renders a selection-row removal");
    ok &= check(!selection_removed_stats.background_layer_rebuilt &&
        selection_removed_stats.selection_layer_rebuilt &&
        selection_removed_stats.background_row_clean_reuse_skips == 1 &&
        selection_removed_stats.background_rows_reused == 0 &&
        selection_removed_stats.selection_rows_reused == 1 &&
        selection_removed_stats.selection_rows_removed == 1,
        "removing one selection row removes only that cached selection row");

    const term::Terminal_render_frame selection_empty_frame = make_direct_rect_cache_frame(
        { background_row_zero, background_row_one_changed },
        {},
        metrics);
    term::terminal_renderer_stats_t selection_empty_stats;
    ok &= check(update(selection_empty_frame, selection_empty_stats),
        "background/selection row cache renders cached selection-to-empty removal");
    ok &= check(!selection_empty_stats.background_layer_rebuilt &&
        selection_empty_stats.selection_layer_rebuilt &&
        selection_empty_stats.background_row_clean_reuse_skips == 1 &&
        selection_empty_stats.background_rows_reused == 0 &&
        selection_empty_stats.selection_rows_removed == 1,
        "removing the final cached selection row clears the selection row cache");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_background_row_cache_uses_batched_geometry(
    QGuiApplication&   app,
    bool               require_batched_geometry)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const bool software_scene_graph = window_uses_software_scene_graph(window);
    ok &= check(!require_batched_geometry || !software_scene_graph,
        "background batched-geometry mode requires a non-software scene graph");
    const term::Terminal_render_rect row_zero_left =
        make_direct_row_rect(0, 1, 2, QColor(130, 36, 48), metrics);
    const term::Terminal_render_rect row_zero_right =
        make_direct_row_rect(0, 5, 2, QColor(34, 126, 68), metrics);

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    term::terminal_renderer_stats_t stats;
    node = renderer.update_node(
        node,
        &window,
        make_direct_rect_cache_frame({row_zero_left, row_zero_right}, {}, metrics, false),
        font,
        1.0,
        {},
        stats);

    ok &= check(node != nullptr &&
        stats.paint_completed &&
        stats.text_content_failures == 0,
        "batched background geometry test renders a direct background frame");
    ok &= check(stats.background_rows_rebuilt == 1 &&
        stats.background_row_rects_before_coalescing == 2 &&
        stats.background_row_rects_after_coalescing == 2,
        "background row geometry reports row-local rect counters");
    if (software_scene_graph) {
        ok &= check(stats.background_batched_rects == 0 &&
            stats.background_batched_vertices == 0,
            "software background row fallback leaves batched-geometry counters at zero");
    }
    else {
        ok &= check(stats.background_batched_rects == 2 &&
            stats.background_batched_vertices == 12,
            "batched background geometry reports six vertices per row-local rect");
    }

    QSGNode* background_layer = child_node_at(node,             0);
    QSGNode* frame_layer      = child_node_at(background_layer, 0);
    QSGNode* row_layer        = child_node_at(background_layer, 1);
    const std::vector<QSGTransformNode*> row_wrappers =
        transform_child_nodes(row_layer);
    ok &= check(frame_layer != nullptr &&
        frame_layer->firstChild() == nullptr,
        "batched background geometry keeps the frame layer empty for row-only input");
    ok &= check(row_wrappers.size() == 1U,
        "batched background geometry creates one cached row wrapper");
    if (row_wrappers.size() == 1U) {
        const std::vector<QSGGeometryNode*> geometry_nodes =
            geometry_child_nodes(row_wrappers[0]);
        const std::vector<QRectF> simple_rects =
            simple_rect_child_rects(row_wrappers[0]);
        if (software_scene_graph) {
            ok &= check(geometry_nodes.size() == simple_rects.size() &&
                simple_rects.size() == 2U,
                "software background row fallback keeps simple rect nodes private to software");
        }
        else {
            ok &= check(geometry_nodes.size() == 1U &&
                simple_rects.empty(),
                "cached background row uses one geometry node instead of simple rect nodes");
            if (geometry_nodes.size() == 1U) {
                ok &= check(geometry_nodes[0]->geometry() != nullptr &&
                    geometry_nodes[0]->geometry()->vertexCount() == 12,
                    "cached background geometry has six vertices per row-local rect");
            }
        }
    }

    const term::Terminal_render_frame direct_hard_frame = make_direct_graphic_row_cache_frame(
        { make_direct_row_rect(0, 0, 1, QColor(92, 146, 214), metrics) },
        {},
        metrics);
    term::terminal_renderer_stats_t direct_hard_stats;
    node  = renderer.update_node(
        node,
        &window,
        direct_hard_frame,
        font,
        1.0,
        {},
        direct_hard_stats);
    ok   &= check(node != nullptr &&
        direct_hard_stats.paint_completed &&
        direct_hard_stats.graphic_rect_rows_rebuilt == 1,
        "direct hard graphic rect stays on the graphic simple row path");
    ok   &= check(direct_hard_stats.graphic_batched_rects == 0 &&
        direct_hard_stats.graphic_batched_vertices == 0,
        "direct hard graphic rect leaves batched counters at zero");

    term::terminal_renderer_stats_t direct_hard_dpr_stats;
    node = renderer.update_node(
        node,
        &window,
        direct_hard_frame,
        font,
        2.0,
        {},
        direct_hard_dpr_stats);
    ok &= check(node != nullptr &&
        direct_hard_dpr_stats.root_reused &&
        direct_hard_dpr_stats.paint_completed &&
        !direct_hard_dpr_stats.graphic_layer_rebuilt &&
        direct_hard_dpr_stats.graphic_rect_row_clean_reuse_skips == 1 &&
        direct_hard_dpr_stats.graphic_batched_rects == 0 &&
        direct_hard_dpr_stats.graphic_batched_vertices == 0,
        "direct hard graphic simple row cache is stable across DPR changes");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_selection_row_cache_uses_batched_geometry(
    QGuiApplication&   app,
    bool               require_batched_geometry)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const bool software_scene_graph = window_uses_software_scene_graph(window);
    ok &= check(!require_batched_geometry || !software_scene_graph,
        "selection batched-geometry mode requires a non-software scene graph");
    const term::Terminal_render_rect row_zero_left =
        make_direct_row_rect(0, 1, 2, QColor(54, 96, 154, 190), metrics);
    const term::Terminal_render_rect row_zero_right =
        make_direct_row_rect(0, 5, 2, QColor(74, 116, 174, 190), metrics);

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    term::terminal_renderer_stats_t stats;
    node = renderer.update_node(
        node,
        &window,
        make_direct_rect_cache_frame({}, {row_zero_left, row_zero_right}, metrics, false),
        font,
        1.0,
        {},
        stats);

    ok &= check(node != nullptr &&
        stats.paint_completed &&
        stats.text_content_failures == 0,
        "batched selection geometry test renders a direct selection frame");
    ok &= check(stats.selection_rows_rebuilt == 1,
        "selection row geometry reports one rebuilt cached row");
    if (software_scene_graph) {
        ok &= check(stats.selection_batched_rects == 0 &&
            stats.selection_batched_vertices == 0,
            "software selection row fallback leaves batched-geometry counters at zero");
    }
    else {
        ok &= check(stats.selection_batched_rects == 2 &&
            stats.selection_batched_vertices == 12,
            "batched selection geometry reports six vertices per row-local rect");
    }

    QSGNode* selection_layer = child_node_at(node,            1);
    QSGNode* frame_layer     = child_node_at(selection_layer, 0);
    QSGNode* row_layer       = child_node_at(selection_layer, 1);
    const std::vector<QSGTransformNode*> row_wrappers =
        transform_child_nodes(row_layer);
    ok &= check(frame_layer != nullptr &&
        frame_layer->firstChild() == nullptr,
        "batched selection geometry keeps the frame layer empty for row-only input");
    ok &= check(row_wrappers.size() == 1U,
        "batched selection geometry creates one cached row wrapper");
    if (row_wrappers.size() == 1U) {
        const std::vector<QSGGeometryNode*> geometry_nodes =
            geometry_child_nodes(row_wrappers[0]);
        const std::vector<QRectF> simple_rects =
            simple_rect_child_rects(row_wrappers[0]);
        if (software_scene_graph) {
            ok &= check(geometry_nodes.size() == simple_rects.size() &&
                simple_rects.size() == 2U,
                "software selection row fallback keeps simple rect nodes");
        }
        else {
            ok &= check(geometry_nodes.size() == 1U &&
                simple_rects.empty(),
                "cached selection row uses one geometry node instead of simple rect nodes");
            if (geometry_nodes.size() == 1U) {
                ok &= check(geometry_nodes[0]->geometry() != nullptr &&
                    geometry_nodes[0]->geometry()->vertexCount() == 12,
                    "cached selection geometry has six vertices per row-local rect");
            }
        }
    }

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_decoration_row_cache_uses_batched_geometry(
    QGuiApplication&   app,
    bool               require_batched_geometry)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const bool software_scene_graph = window_uses_software_scene_graph(window);
    ok &= check(!require_batched_geometry || !software_scene_graph,
        "decoration batched-geometry mode requires a non-software scene graph");

    const qreal thickness = std::max<qreal>(1.0, std::floor(metrics.height * 0.08));
    std::vector<term::Terminal_render_decoration> decorations = {
        make_direct_decoration_rect(
            0,
            1,
            2,
            metrics.ascent + thickness,
            thickness,
            QColor(212, 184, 72),
            metrics,
            term::Terminal_render_decoration_kind::UNDERLINE),
        make_direct_decoration_rect(
            0,
            5,
            2,
            metrics.ascent * 0.55,
            thickness,
            QColor(232, 96, 92),
            metrics,
            term::Terminal_render_decoration_kind::STRIKE),
        make_direct_decoration_rect(
            0,
            9,
            2,
            metrics.ascent + thickness,
            thickness,
            QColor(82, 186, 132),
            metrics,
            term::Terminal_render_decoration_kind::HYPERLINK_UNDERLINE),
        make_direct_decoration_rect(
            0,
            14,
            1,
            0.0,
            metrics.height,
            QColor(92, 146, 214),
            metrics,
            term::Terminal_render_decoration_kind::PREEDIT_CARET),
    };

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    term::terminal_renderer_stats_t stats;
    node = renderer.update_node(
        node,
        &window,
        make_direct_decoration_cache_frame(std::move(decorations), metrics),
        font,
        1.0,
        {},
        stats);

    ok &= check(node != nullptr &&
        stats.paint_completed &&
        stats.text_content_failures == 0,
        "batched decoration geometry test renders a direct decoration frame");
    ok &= check(stats.decoration_rows_rebuilt == 1,
        "decoration row geometry reports one rebuilt cached row");
    if (software_scene_graph) {
        ok &= check(stats.decoration_batched_rects == 0 &&
            stats.decoration_batched_vertices == 0,
            "software decoration row fallback leaves batched-geometry counters at zero");
    }
    else {
        ok &= check(stats.decoration_batched_rects == 4 &&
            stats.decoration_batched_vertices == 24,
            "batched decoration geometry reports six vertices per row-local rect");
    }

    QSGNode* decoration_layer = child_node_at(node,             4);
    QSGNode* frame_layer      = child_node_at(decoration_layer, 0);
    QSGNode* row_layer        = child_node_at(decoration_layer, 1);
    const std::vector<QSGTransformNode*> row_wrappers =
        transform_child_nodes(row_layer);
    ok &= check(frame_layer != nullptr &&
        frame_layer->firstChild() == nullptr,
        "batched decoration geometry keeps the frame layer empty for row-only input");
    ok &= check(row_wrappers.size() == 1U,
        "batched decoration geometry creates one cached row wrapper");
    if (row_wrappers.size() == 1U) {
        const std::vector<QSGGeometryNode*> geometry_nodes =
            geometry_child_nodes(row_wrappers[0]);
        const std::vector<QRectF> simple_rects =
            simple_rect_child_rects(row_wrappers[0]);
        if (software_scene_graph) {
            ok &= check(geometry_nodes.size() == simple_rects.size() &&
                simple_rects.size() == 4U,
                "software decoration row fallback keeps simple rect nodes");
        }
        else {
            ok &= check(geometry_nodes.size() == 1U &&
                simple_rects.empty(),
                "cached decoration row uses one geometry node instead of simple rect nodes");
            if (geometry_nodes.size() == 1U) {
                ok &= check(geometry_nodes[0]->geometry() != nullptr &&
                    geometry_nodes[0]->geometry()->vertexCount() == 24,
                    "cached decoration geometry has six vertices per row-local rect");
                const std::vector<QRectF> batched_rects =
                    batched_geometry_rect_bounds(geometry_nodes[0]);
                ok &= check(batched_rects.size() == 4U,
                    "cached decoration geometry exposes one rect bound per decoration");
                if (batched_rects.size() == 4U) {
                    ok &= check(rect_nearly_matches(
                        batched_rects[3],
                        QRectF(
                            14.0 * metrics.width,
                            0.0,
                            metrics.width,
                            metrics.height)),
                        "cached preedit caret decoration keeps row-local bounds");
                }
            }
        }
    }

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_graphic_row_cache_uses_batched_geometry(
    QGuiApplication&   app,
    bool               require_batched_geometry)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const bool software_scene_graph = window_uses_software_scene_graph(window);
    ok &= check(!require_batched_geometry || !software_scene_graph,
        "graphic batched-geometry mode requires a non-software scene graph");

    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 3;
    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({3, 12}, viewport, 1301U);
    snapshot.color_state      = color_state();
    snapshot.cursor.visible   = false;
    snapshot.dirty_row_ranges = {{0, 3}};
    snapshot.cells.push_back({{1, 1}, QStringLiteral("\u2588"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 2}, QStringLiteral("\u2598"), 0U, 1, false, 0U});

    const term::Terminal_render_frame frame =
        build_qsg_sidecar_test_frame(snapshot, metrics);
    ok &= check(frame.stats.packed_graphic_cells == 2 &&
        frame.graphic_rects.empty(),
        "packed hard graphic sidecar frame has no duplicate direct rect primitives");

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    term::terminal_renderer_stats_t stats;
    node = renderer.update_node(
        node,
        &window,
        frame,
        font,
        1.0,
        {},
        stats);

    ok &= check(node != nullptr &&
        stats.paint_completed &&
        stats.text_content_failures == 0 &&
        stats.frame_packed_graphic_cells == 2 &&
        stats.frame_graphic_rects == 0,
        "batched graphic geometry test renders a packed hard-block frame");
    ok &= check(stats.graphic_rect_rows_rebuilt == 1 &&
        stats.graphic_rect_row_cache_fallbacks == 0 &&
        stats.rect_resource_rects_before_coalescing == 2 &&
        stats.rect_resource_rects_after_coalescing == 2,
        "packed hard graphics build one cached graphic rect row without duplicate rects");
    if (software_scene_graph) {
        ok &= check(stats.graphic_batched_rects == 0 &&
            stats.graphic_batched_vertices == 0,
            "software graphic row fallback leaves batched-geometry counters at zero");
    }
    else {
        ok &= check(stats.graphic_batched_rects == 2 &&
            stats.graphic_batched_vertices == 12,
            "batched graphic geometry reports six vertices per packed hard rect");
    }

    QSGNode* graphic_layer   = child_node_at(node,            2);
    QSGNode* rect_pass_layer = child_node_at(graphic_layer,   0);
    QSGNode* frame_layer     = child_node_at(rect_pass_layer, 0);
    QSGNode* row_layer       = child_node_at(rect_pass_layer, 1);
    const std::vector<QSGTransformNode*> row_wrappers =
        transform_child_nodes(row_layer);
    ok &= check(frame_layer != nullptr &&
        frame_layer->firstChild() == nullptr,
        "packed hard graphics leave the graphic rect frame layer empty");
    ok &= check(row_wrappers.size() == 1U,
        "packed hard graphics create one cached graphic row wrapper");
    if (row_wrappers.size() == 1U) {
        const std::vector<QSGGeometryNode*> geometry_nodes =
            geometry_child_nodes(row_wrappers[0]);
        const std::vector<QRectF> simple_rects =
            simple_rect_child_rects(row_wrappers[0]);
        if (software_scene_graph) {
            ok &= check(geometry_nodes.size() == simple_rects.size() &&
                simple_rects.size() == 2U,
                "software packed hard graphics keep simple rect nodes");
        }
        else {
            ok &= check(geometry_nodes.size() == 1U &&
                simple_rects.empty(),
                "packed hard graphics use one batched geometry node");
            if (geometry_nodes.size() == 1U) {
                ok &= check(geometry_nodes[0]->geometry() != nullptr &&
                    geometry_nodes[0]->geometry()->vertexCount() == 12,
                    "packed hard graphic geometry has six vertices per rect");
            }
        }
    }

    term::terminal_renderer_stats_t dpr_stats;
    node = renderer.update_node(
        node,
        &window,
        frame,
        font,
        2.0,
        {},
        dpr_stats);
    ok &= check(node != nullptr &&
        dpr_stats.root_reused &&
        dpr_stats.paint_completed &&
        !dpr_stats.graphic_layer_rebuilt &&
        dpr_stats.graphic_rect_row_clean_reuse_skips == 1,
        "packed hard graphic batched row cache is stable across DPR changes");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_background_row_cache_mixed_rect_fallback(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const term::Terminal_render_rect row_zero =
        make_direct_row_rect(0, 0, 3, QColor(130, 36, 48), metrics);
    const term::Terminal_render_rect row_one =
        make_direct_row_rect(1, 1, 3, QColor(34, 126, 68), metrics);
    const term::Terminal_render_rect non_row_rect{
        QRectF(
            metrics.width * 0.5,
            metrics.height * 0.5,
            metrics.width * 2.0,
            metrics.height * 1.5),
        QColor(38, 54, 144),
    };

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };

    const term::Terminal_render_frame cached_frame =
        make_direct_rect_cache_frame({row_zero, row_one}, {}, metrics, false);
    term::terminal_renderer_stats_t cached_stats;
    ok &= check(update(cached_frame, cached_stats),
        "mixed background rect fallback test builds the initial row cache");
    ok &= check(cached_stats.background_rows_rebuilt == 2,
        "mixed background rect fallback setup creates two cached rows");

    const term::Terminal_render_frame mixed_frame =
        make_direct_rect_cache_frame({row_zero, non_row_rect, row_one}, {}, metrics, false);
    term::terminal_renderer_stats_t mixed_stats;
    ok &= check(update(mixed_frame, mixed_stats),
        "mixed background rect fallback test renders an interleaved direct frame");
    ok &= check(mixed_stats.background_layer_rebuilt &&
        mixed_stats.background_row_cache_fallbacks == 1 &&
        mixed_stats.background_rows_rebuilt == 0 &&
        mixed_stats.background_rows_reused == 0 &&
        mixed_stats.background_rows_removed == 2,
        "interleaved row/non-row background rects fall back to flat rendering and clear row cache");

    QSGNode*                  background_layer = child_node_at(node,             0);
    QSGNode*                  frame_layer      = child_node_at(background_layer, 0);
    QSGNode*                  row_layer        = child_node_at(background_layer, 1);
    const std::vector<QColor> frame_colors     = simple_rect_child_colors(frame_layer);
    ok                                        &= check(frame_colors.size() == 3 &&
        frame_colors[0].rgba() == row_zero.color.rgba() &&
        frame_colors[1].rgba() == non_row_rect.color.rgba() &&
        frame_colors[2].rgba() == row_one.color.rgba(),
        "flat fallback keeps mixed background rect child order");
    ok                                        &= check(row_layer != nullptr && row_layer->firstChild() == nullptr,
        "flat fallback leaves no cached background row wrappers attached");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_background_selection_row_cache_viewport_move_reuses_logical_row(
    QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const term::Terminal_render_rect background_at_row_zero = make_direct_row_rect(
        0,
        1,
        3,
        QColor(122, 46, 80),
        metrics);
    const term::Terminal_render_rect background_at_row_one = make_direct_row_rect(
        1,
        1,
        3,
        QColor(122, 46, 80),
        metrics);
    const term::Terminal_render_rect selection_at_row_zero = make_direct_row_rect(
        0,
        2,
        2,
        QColor(42, 96, 160, 190),
        metrics);
    const term::Terminal_render_rect selection_at_row_one = make_direct_row_rect(
        1,
        2,
        2,
        QColor(42, 96, 160, 190),
        metrics);

    term::Terminal_viewport_state first_viewport;
    first_viewport.visible_rows     = 3;
    first_viewport.scrollback_rows  = 10;
    first_viewport.offset_from_tail = 3;
    first_viewport.follow_tail      = false;

    term::Terminal_viewport_state moved_viewport = first_viewport;
    moved_viewport.offset_from_tail = 4;

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };

    const term::Terminal_render_frame first_frame = make_direct_rect_cache_viewport_frame(
        { background_at_row_zero },
        { selection_at_row_zero },
        metrics,
        first_viewport);
    term::terminal_renderer_stats_t first_stats;
    ok &= check(update(first_frame, first_stats),
        "viewport-move row cache test builds the initial logical row resources");
    ok &= check(first_stats.background_rows_rebuilt == 1 &&
        first_stats.selection_rows_rebuilt == 1,
        "viewport-move row cache setup builds one background and selection row");

    const term::Terminal_render_frame moved_frame = make_direct_rect_cache_viewport_frame(
        { background_at_row_one },
        { selection_at_row_one },
        metrics,
        moved_viewport);
    term::terminal_renderer_stats_t moved_stats;
    ok &= check(update(moved_frame, moved_stats),
        "viewport-move row cache test renders the same logical row at a new viewport row");
    ok &= check(moved_stats.background_layer_rebuilt &&
        moved_stats.selection_layer_rebuilt &&
        moved_stats.background_rows_rebuilt == 0 &&
        moved_stats.selection_rows_rebuilt == 0 &&
        moved_stats.background_rows_reused == 1 &&
        moved_stats.selection_rows_reused == 1,
        "moving a cached logical row reuses row resources while reporting absolute geometry change");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_background_selection_row_cache_scroll_pixels(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(300, 140);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(260.0, 90.0));
    configure_test_font(surface);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_row_cache_scroll_snapshot(940U, 2));
    window.show();

    const QImage first_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            invalidation_stats.last_rendered_snapshot_sequence == 940U &&
            !invalidation_stats.pending_update                         &&
            stats.paint_completed                                      &&
            stats.text_content_failures == 0                           &&
            stats.background_rows_rebuilt == 3                         &&
            stats.selection_rows_rebuilt == 1;
    });
    ok &= check(!first_image.isNull(),
        "row-cache scroll placement test renders the initial image");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_row_cache_scroll_snapshot(941U, 1));
    const QImage scroll_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            invalidation_stats.last_rendered_snapshot_sequence == 941U &&
            !invalidation_stats.pending_update                         &&
            stats.paint_completed                                      &&
            stats.text_content_failures == 0                           &&
            stats.background_layer_rebuilt                             &&
            stats.selection_layer_rebuilt                              &&
            stats.background_rows_reused >= 2                          &&
            stats.selection_rows_reused == 1;
    });
    ok &= check(!scroll_image.isNull(),
        "row-cache scroll placement test renders the moved image");

    const term::terminal_cell_metrics_t metrics = test_metrics();
    ok &= check(count_matching_pixels(
        scroll_image,
        cell_area(0, 0, 1, metrics),
        [](QColor color) {
            return is_row_cache_background_color(color, 1);
        }) > 10,
        "row-cache scroll placement renders the exposed logical row at viewport row zero");
    ok &= check(count_matching_pixels(
        scroll_image,
        cell_area(1, 0, 1, metrics),
        [](QColor color) {
            return is_row_cache_background_color(color, 2);
        }) > 10,
        "row-cache scroll placement moves the reused background row to viewport row one");
    ok &= check(count_matching_pixels(
        scroll_image,
        cell_area(1, 2, 3, metrics),
        [](QColor color) {
            return color.blue() > color.red() && color.blue() > color.green();
        }) > 20,
        "row-cache scroll placement moves the reused selection row to viewport row one");

    return ok;
}

bool test_qsg_background_stale_pixel_change_blank_scroll_identity(
    QGuiApplication&   app,
    bool               require_batched_geometry)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(9, 12, 16));
    window.resize(380, 120);

    Direct_render_item item;
    item.setParentItem(window.contentItem());
    window.show();
    pump_events(app, 2);

    const term::terminal_cell_metrics_t metrics                 = test_metrics();
    const bool                          software_scene_graph    = window_uses_software_scene_graph(window);
    const bool                          expect_batched_geometry = !software_scene_graph;
    ok &= check(!require_batched_geometry || !software_scene_graph,
        "background batched-geometry stale-pixel mode requires a non-software scene graph");
    const auto make_frame = [&](
        int first_visible_logical_row,
        std::vector<std::pair<int, int>> row_color_ids) {
        std::vector<term::Terminal_render_rect> rects = {
            make_direct_frame_background_rect(metrics),
        };
        for (const auto& [viewport_row, color_id] : row_color_ids) {
            rects.push_back(make_direct_row_rect(
                viewport_row,
                0,
                20,
                qsg_lifecycle_row_color(color_id),
                metrics));
        }
        return make_direct_rect_cache_viewport_frame(
            std::move(rects),
            {},
            metrics,
            make_direct_scroll_lifecycle_viewport(first_visible_logical_row));
    };

    const auto row_color_pixels = [&](const QImage& image, int viewport_row, int color_id) {
        return count_matching_pixels(
            image,
            cell_area(viewport_row, 0, 20, metrics),
            [color_id](QColor color) {
                return is_qsg_lifecycle_row_color(color, color_id);
            });
    };
    const auto check_row_color = [&](
        const QImage& image,
        int viewport_row,
        int color_id,
        const char* message,
        std::uint64_t sequence,
        const term::Terminal_render_frame& frame,
        const term::terminal_renderer_stats_t& stats) {
        const QRect area            = cell_area(viewport_row, 0, 20, metrics);
        const int   matching_pixels = row_color_pixels(image, viewport_row, color_id);
        return
            check_visual_pixels(
                matching_pixels > static_cast<int>(static_cast<qreal>(area.width() * area.height()) * 0.65),
                message,
                image,
                area,
                matching_pixels,
                sequence,
                frame.dirty_row_ranges,
                stats);
    };
    const auto check_row_blank = [&](
        const QImage& image,
        int viewport_row,
        const char* message,
        std::uint64_t sequence,
        const term::Terminal_render_frame& frame,
        const term::terminal_renderer_stats_t& stats) {
        const QRect area            = cell_area(viewport_row, 0, 20, metrics);
        const int   matching_pixels = count_background_pixels(image, area);
        return
            check_visual_pixels(
                matching_pixels > static_cast<int>(static_cast<qreal>(area.width() * area.height()) * 0.75),
                message,
                image,
                area,
                matching_pixels,
                sequence,
                frame.dirty_row_ranges,
                stats);
    };

    const term::Terminal_render_frame setup_frame =
        make_frame(4, {{0, 4}, {1, 5}, {2, 6}});
    item.set_frame(setup_frame);
    const QImage setup_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.paint_completed            &&
            stats.text_content_failures == 0 &&
            stats.background_rows_rebuilt == 3;
    });
    const term::terminal_renderer_stats_t setup_stats = item.last_stats();
    ok &= check(!setup_image.isNull(),
        "background stale-pixel setup produced an image");
    ok &= check_row_color(
        setup_image,
        0,
        4,
        "background stale-pixel setup paints logical row 4",
        970U,
        setup_frame,
        setup_stats);
    ok &= check_row_color(
        setup_image,
        1,
        5,
        "background stale-pixel setup paints logical row 5",
        970U,
        setup_frame,
        setup_stats);
    ok &= check_row_color(
        setup_image,
        2,
        6,
        "background stale-pixel setup paints logical row 6",
        970U,
        setup_frame,
        setup_stats);
    if (expect_batched_geometry) {
        ok &= check(setup_stats.background_batched_rects > 0 &&
            setup_stats.background_batched_vertices ==
                setup_stats.background_batched_rects * 6,
            "background stale-pixel setup uses batched row geometry");
    }

    const term::Terminal_render_frame changed_blank_frame =
        make_frame(4, {{0, 7}, {2, 6}});
    item.set_frame(changed_blank_frame);
    const QImage changed_blank_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.root_reused                  &&
            stats.paint_completed              &&
            stats.text_content_failures == 0   &&
            stats.background_rows_rebuilt == 1 &&
            stats.background_rows_removed == 1;
    });
    const term::terminal_renderer_stats_t changed_blank_stats = item.last_stats();
    ok &= check(!changed_blank_image.isNull(),
        "background stale-pixel changed/blank frame produced an image");
    ok &= check_row_color(
        changed_blank_image,
        0,
        7,
        "background stale-pixel change replaces the first row color",
        971U,
        changed_blank_frame,
        changed_blank_stats);
    ok &= check_row_blank(
        changed_blank_image,
        1,
        "background stale-pixel blanked row returns to default background",
        971U,
        changed_blank_frame,
        changed_blank_stats);
    ok &= check_frame_lacks_lifecycle_color(
        changed_blank_image,
        4,
        metrics,
        "background stale-pixel changed/blank frame removes old row 4 color",
        971U,
        changed_blank_frame,
        changed_blank_stats);
    ok &= check_frame_lacks_lifecycle_color(
        changed_blank_image,
        5,
        metrics,
        "background stale-pixel changed/blank frame removes old row 5 color",
        971U,
        changed_blank_frame,
        changed_blank_stats);
    if (expect_batched_geometry) {
        ok &= check(changed_blank_stats.background_batched_rects > 0 &&
            changed_blank_stats.background_batched_vertices ==
                changed_blank_stats.background_batched_rects * 6,
            "background stale-pixel changed/blank frame uses batched row geometry");
    }

    const term::Terminal_render_frame scrolled_frame =
        make_frame(3, {{0, 3}, {1, 7}});
    item.set_frame(scrolled_frame);
    const QImage scrolled_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.root_reused                 &&
            stats.paint_completed             &&
            stats.text_content_failures == 0  &&
            stats.background_rows_reused >= 1 &&
            stats.background_rows_removed >= 1;
    });
    const term::terminal_renderer_stats_t scrolled_stats = item.last_stats();
    ok &= check(!scrolled_image.isNull(),
        "background stale-pixel scrolled frame produced an image");
    ok &= check(scrolled_stats.background_rows_reused >= 1,
        "background stale-pixel scroll reuses at least one cached row identity");
    ok &= check_row_color(
        scrolled_image,
        1,
        7,
        "background stale-pixel scroll moves the reused changed row downward",
        972U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_row_blank(
        scrolled_image,
        2,
        "background stale-pixel scroll keeps the omitted logical row blank",
        972U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_frame_lacks_lifecycle_color(
        scrolled_image,
        4,
        metrics,
        "background stale-pixel scroll leaves no stale color from logical row 4",
        972U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_frame_lacks_lifecycle_color(
        scrolled_image,
        5,
        metrics,
        "background stale-pixel scroll leaves no stale color from logical row 5",
        972U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_frame_lacks_lifecycle_color(
        scrolled_image,
        6,
        metrics,
        "background stale-pixel scroll leaves no stale color from logical row 6",
        972U,
        scrolled_frame,
        scrolled_stats);
    if (expect_batched_geometry) {
        ok &= check(scrolled_stats.background_batched_rects > 0 &&
            scrolled_stats.background_batched_vertices ==
                scrolled_stats.background_batched_rects * 6,
            "background stale-pixel scroll frame uses batched row geometry");
    }

    return ok;
}

bool test_qsg_graphic_stale_pixel_change_blank_scroll_identity(
    QGuiApplication&   app,
    bool               require_batched_geometry)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(9, 12, 16));
    window.resize(380, 120);

    Direct_render_item item;
    item.setParentItem(window.contentItem());
    window.show();
    pump_events(app, 2);

    const term::terminal_cell_metrics_t metrics                 = test_metrics();
    const bool                          software_scene_graph    = window_uses_software_scene_graph(window);
    const bool                          expect_batched_geometry = !software_scene_graph;
    ok &= check(!require_batched_geometry || !software_scene_graph,
        "graphic batched-geometry stale-pixel mode requires a non-software scene graph");
    const auto make_frame = [&](
        int first_visible_logical_row,
        std::vector<std::pair<int, int>> row_color_ids) {
        term::Terminal_render_frame frame =
            make_direct_graphic_row_cache_frame({}, {}, metrics);
        frame.viewport         = make_direct_scroll_lifecycle_viewport(first_visible_logical_row);
        frame.background_rects = {make_direct_frame_background_rect(metrics)};
        frame.packed_rows.reserve(3U);
        frame.packed_graphic_spans.reserve(row_color_ids.size());
        frame.packed_graphic_codepoints.reserve(row_color_ids.size() * 20U);
        for (int viewport_row = 0; viewport_row < 3; ++viewport_row) {
            term::terminal_packed_render_row_t row;
            row.active_buffer = frame.viewport.active_buffer;
            row.viewport_row  = viewport_row;
            row.logical_row   = first_visible_logical_row + viewport_row;
            row.first_graphic_span =
                static_cast<std::uint32_t>(frame.packed_graphic_spans.size());
            const auto color_match = std::find_if(
                row_color_ids.begin(),
                row_color_ids.end(),
                [viewport_row](const auto& entry) {
                    return entry.first == viewport_row;
                });
            if (color_match != row_color_ids.end()) {
                term::terminal_packed_graphic_span_t span;
                span.first_column    = 0;
                span.column_count    = 20;
                span.foreground_rgba = static_cast<std::uint32_t>(
                    qsg_lifecycle_row_color(color_match->second).rgba());
                span.background_rgba =
                    static_cast<std::uint32_t>(QColor(9, 12, 16).rgba());
                span.codepoint_offset =
                    static_cast<std::uint32_t>(frame.packed_graphic_codepoints.size());
                span.codepoint_count = 20U;
                frame.packed_graphic_spans.push_back(span);
                for (int column = 0; column < 20; ++column) {
                    frame.packed_graphic_codepoints.push_back(0x2588U);
                }
                row.graphic_span_count = 1U;
                frame.stats.packed_graphic_cells += 20;
            }
            frame.packed_rows.push_back(row);
        }
        frame.stats.packed_rows =
            static_cast<int>(frame.packed_rows.size());
        frame.stats.packed_graphic_spans =
            static_cast<int>(frame.packed_graphic_spans.size());
        frame.stats.packed_payload_bytes =
            frame.packed_graphic_codepoints.size() * sizeof(std::uint32_t);
        return frame;
    };

    const auto check_row_color = [&](
        const QImage& image,
        int viewport_row,
        int color_id,
        const char* message,
        std::uint64_t sequence,
        const term::Terminal_render_frame& frame,
        const term::terminal_renderer_stats_t& stats) {
        const QRect area = cell_area(viewport_row, 0, 20, metrics);
        const int matching_pixels = count_matching_pixels(
            image,
            area,
            [color_id](QColor color) {
                return is_qsg_lifecycle_row_color(color, color_id);
            });
        return
            check_visual_pixels(
                matching_pixels > static_cast<int>(static_cast<qreal>(area.width() * area.height()) * 0.65),
                message,
                image,
                area,
                matching_pixels,
                sequence,
                frame.dirty_row_ranges,
                stats);
    };
    const auto check_row_blank = [&](
        const QImage& image,
        int viewport_row,
        const char* message,
        std::uint64_t sequence,
        const term::Terminal_render_frame& frame,
        const term::terminal_renderer_stats_t& stats) {
        const QRect area            = cell_area(viewport_row, 0, 20, metrics);
        const int   matching_pixels = count_background_pixels(image, area);
        return
            check_visual_pixels(
                matching_pixels > static_cast<int>(static_cast<qreal>(area.width() * area.height()) * 0.75),
                message,
                image,
                area,
                matching_pixels,
                sequence,
                frame.dirty_row_ranges,
                stats);
    };

    const term::Terminal_render_frame setup_frame =
        make_frame(4, {{0, 4}, {1, 5}, {2, 6}});
    item.set_frame(setup_frame);
    const QImage setup_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.paint_completed            &&
            stats.text_content_failures == 0 &&
            stats.graphic_rect_rows_rebuilt == 3;
    });
    const term::terminal_renderer_stats_t setup_stats = item.last_stats();
    ok &= check(!setup_image.isNull(),
        "graphic stale-pixel setup produced an image");
    ok &= check_row_color(
        setup_image,
        0,
        4,
        "graphic stale-pixel setup paints logical row 4",
        976U,
        setup_frame,
        setup_stats);
    ok &= check_row_color(
        setup_image,
        1,
        5,
        "graphic stale-pixel setup paints logical row 5",
        976U,
        setup_frame,
        setup_stats);
    ok &= check_row_color(
        setup_image,
        2,
        6,
        "graphic stale-pixel setup paints logical row 6",
        976U,
        setup_frame,
        setup_stats);
    if (expect_batched_geometry) {
        ok &= check(setup_stats.graphic_batched_rects > 0 &&
            setup_stats.graphic_batched_vertices ==
                setup_stats.graphic_batched_rects * 6,
            "graphic stale-pixel setup uses batched row geometry");
    }

    const term::Terminal_render_frame changed_blank_frame =
        make_frame(4, {{0, 7}, {2, 6}});
    item.set_frame(changed_blank_frame);
    const QImage changed_blank_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.root_reused                    &&
            stats.paint_completed                &&
            stats.text_content_failures == 0     &&
            stats.graphic_rect_rows_rebuilt == 1 &&
            stats.graphic_rect_rows_removed == 1;
    });
    const term::terminal_renderer_stats_t changed_blank_stats = item.last_stats();
    ok &= check(!changed_blank_image.isNull(),
        "graphic stale-pixel changed/blank frame produced an image");
    ok &= check_row_color(
        changed_blank_image,
        0,
        7,
        "graphic stale-pixel change replaces the first row color",
        977U,
        changed_blank_frame,
        changed_blank_stats);
    ok &= check_row_blank(
        changed_blank_image,
        1,
        "graphic stale-pixel blanked row returns to default background",
        977U,
        changed_blank_frame,
        changed_blank_stats);
    ok &= check_frame_lacks_lifecycle_color(
        changed_blank_image,
        4,
        metrics,
        "graphic stale-pixel changed/blank frame removes previous row 4 color",
        977U,
        changed_blank_frame,
        changed_blank_stats);
    ok &= check_frame_lacks_lifecycle_color(
        changed_blank_image,
        5,
        metrics,
        "graphic stale-pixel changed/blank frame removes previous row 5 color",
        977U,
        changed_blank_frame,
        changed_blank_stats);
    if (expect_batched_geometry) {
        ok &= check(changed_blank_stats.graphic_batched_rects > 0 &&
            changed_blank_stats.graphic_batched_vertices ==
                changed_blank_stats.graphic_batched_rects * 6,
            "graphic stale-pixel changed/blank frame uses batched row geometry");
    }

    const term::Terminal_render_frame scrolled_frame =
        make_frame(3, {{0, 3}, {1, 7}});
    item.set_frame(scrolled_frame);
    const QImage scrolled_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.root_reused                   &&
            stats.paint_completed               &&
            stats.text_content_failures == 0    &&
            stats.graphic_rect_rows_reused >= 1 &&
            stats.graphic_rect_rows_removed >= 1;
    });
    const term::terminal_renderer_stats_t scrolled_stats = item.last_stats();
    ok &= check(!scrolled_image.isNull(),
        "graphic stale-pixel scrolled frame produced an image");
    ok &= check(scrolled_stats.graphic_rect_rows_reused >= 1,
        "graphic stale-pixel scroll reuses at least one cached row identity");
    ok &= check_row_color(
        scrolled_image,
        1,
        7,
        "graphic stale-pixel scroll moves the reused changed row downward",
        978U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_row_blank(
        scrolled_image,
        2,
        "graphic stale-pixel scroll keeps the omitted logical row blank",
        978U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_frame_lacks_lifecycle_color(
        scrolled_image,
        4,
        metrics,
        "graphic stale-pixel scroll leaves no stale color from logical row 4",
        978U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_frame_lacks_lifecycle_color(
        scrolled_image,
        5,
        metrics,
        "graphic stale-pixel scroll leaves no stale color from logical row 5",
        978U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_frame_lacks_lifecycle_color(
        scrolled_image,
        6,
        metrics,
        "graphic stale-pixel scroll leaves no stale color from logical row 6",
        978U,
        scrolled_frame,
        scrolled_stats);
    if (expect_batched_geometry) {
        ok &= check(scrolled_stats.graphic_batched_rects > 0 &&
            scrolled_stats.graphic_batched_vertices ==
                scrolled_stats.graphic_batched_rects * 6,
            "graphic stale-pixel scroll frame uses batched row geometry");
    }

    return ok;
}

bool test_qsg_selection_stale_pixel_change_blank_scroll_identity(
    QGuiApplication&   app,
    bool               require_batched_geometry)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(9, 12, 16));
    window.resize(380, 120);

    Direct_render_item item;
    item.setParentItem(window.contentItem());
    window.show();
    pump_events(app, 2);

    const term::terminal_cell_metrics_t metrics                 = test_metrics();
    const bool                          software_scene_graph    = window_uses_software_scene_graph(window);
    const bool                          expect_batched_geometry = !software_scene_graph;
    ok &= check(!require_batched_geometry || !software_scene_graph,
        "selection batched-geometry stale-pixel mode requires a non-software scene graph");
    const auto make_frame = [&](
        int first_visible_logical_row,
        std::vector<std::pair<int, int>> row_color_ids) {
        std::vector<term::Terminal_render_rect> selection_rects;
        selection_rects.reserve(row_color_ids.size());
        for (const auto& [viewport_row, color_id] : row_color_ids) {
            selection_rects.push_back(make_direct_row_rect(
                viewport_row,
                0,
                20,
                qsg_lifecycle_row_color(color_id),
                metrics));
        }

        return make_direct_rect_cache_viewport_frame(
            { make_direct_frame_background_rect(metrics) },
            std::move(selection_rects),
            metrics,
            make_direct_scroll_lifecycle_viewport(first_visible_logical_row));
    };

    const auto row_color_pixels = [&](const QImage& image, int viewport_row, int color_id) {
        return count_matching_pixels(
            image,
            cell_area(viewport_row, 0, 20, metrics),
            [color_id](QColor color) {
                return is_qsg_lifecycle_row_color(color, color_id);
            });
    };
    const auto check_row_color = [&](
        const QImage& image,
        int viewport_row,
        int color_id,
        const char* message,
        std::uint64_t sequence,
        const term::Terminal_render_frame& frame,
        const term::terminal_renderer_stats_t& stats) {
        const QRect area            = cell_area(viewport_row, 0, 20, metrics);
        const int   matching_pixels = row_color_pixels(image, viewport_row, color_id);
        return
            check_visual_pixels(
                matching_pixels > static_cast<int>(static_cast<qreal>(area.width() * area.height()) * 0.65),
                message,
                image,
                area,
                matching_pixels,
                sequence,
                frame.dirty_row_ranges,
                stats);
    };
    const auto check_row_blank = [&](
        const QImage& image,
        int viewport_row,
        const char* message,
        std::uint64_t sequence,
        const term::Terminal_render_frame& frame,
        const term::terminal_renderer_stats_t& stats) {
        const QRect area            = cell_area(viewport_row, 0, 20, metrics);
        const int   matching_pixels = count_background_pixels(image, area);
        return
            check_visual_pixels(
                matching_pixels > static_cast<int>(static_cast<qreal>(area.width() * area.height()) * 0.75),
                message,
                image,
                area,
                matching_pixels,
                sequence,
                frame.dirty_row_ranges,
                stats);
    };

    const term::Terminal_render_frame setup_frame =
        make_frame(4, {{0, 4}, {1, 5}, {2, 6}});
    item.set_frame(setup_frame);
    const QImage setup_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.paint_completed            &&
            stats.text_content_failures == 0 &&
            stats.selection_rows_rebuilt == 3;
    });
    const term::terminal_renderer_stats_t setup_stats = item.last_stats();
    ok &= check(!setup_image.isNull(),
        "selection stale-pixel setup produced an image");
    ok &= check_row_color(
        setup_image,
        0,
        4,
        "selection stale-pixel setup paints logical row 4",
        973U,
        setup_frame,
        setup_stats);
    ok &= check_row_color(
        setup_image,
        1,
        5,
        "selection stale-pixel setup paints logical row 5",
        973U,
        setup_frame,
        setup_stats);
    ok &= check_row_color(
        setup_image,
        2,
        6,
        "selection stale-pixel setup paints logical row 6",
        973U,
        setup_frame,
        setup_stats);
    if (expect_batched_geometry) {
        ok &= check(setup_stats.selection_batched_rects > 0 &&
            setup_stats.selection_batched_vertices ==
                setup_stats.selection_batched_rects * 6,
            "selection stale-pixel setup uses batched row geometry");
    }

    const term::Terminal_render_frame changed_blank_frame =
        make_frame(4, {{0, 7}, {2, 6}});
    item.set_frame(changed_blank_frame);
    const QImage changed_blank_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.root_reused                 &&
            stats.paint_completed             &&
            stats.text_content_failures == 0  &&
            stats.selection_rows_rebuilt == 1 &&
            stats.selection_rows_removed == 1;
    });
    const term::terminal_renderer_stats_t changed_blank_stats = item.last_stats();
    ok &= check(!changed_blank_image.isNull(),
        "selection stale-pixel changed/blank frame produced an image");
    ok &= check_row_color(
        changed_blank_image,
        0,
        7,
        "selection stale-pixel change replaces the first row color",
        974U,
        changed_blank_frame,
        changed_blank_stats);
    ok &= check_row_blank(
        changed_blank_image,
        1,
        "selection stale-pixel blanked row returns to default background",
        974U,
        changed_blank_frame,
        changed_blank_stats);
    ok &= check_frame_lacks_lifecycle_color(
        changed_blank_image,
        4,
        metrics,
        "selection stale-pixel changed/blank frame removes old row 4 color",
        974U,
        changed_blank_frame,
        changed_blank_stats);
    ok &= check_frame_lacks_lifecycle_color(
        changed_blank_image,
        5,
        metrics,
        "selection stale-pixel changed/blank frame removes old row 5 color",
        974U,
        changed_blank_frame,
        changed_blank_stats);
    if (expect_batched_geometry) {
        ok &= check(changed_blank_stats.selection_batched_rects > 0 &&
            changed_blank_stats.selection_batched_vertices ==
                changed_blank_stats.selection_batched_rects * 6,
            "selection stale-pixel changed/blank frame uses batched row geometry");
    }

    const term::Terminal_render_frame scrolled_frame =
        make_frame(3, {{0, 3}, {1, 7}});
    item.set_frame(scrolled_frame);
    const QImage scrolled_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.root_reused                &&
            stats.paint_completed            &&
            stats.text_content_failures == 0 &&
            stats.selection_rows_reused >= 1 &&
            stats.selection_rows_removed >= 1;
    });
    const term::terminal_renderer_stats_t scrolled_stats = item.last_stats();
    ok &= check(!scrolled_image.isNull(),
        "selection stale-pixel scrolled frame produced an image");
    ok &= check(scrolled_stats.selection_rows_reused >= 1,
        "selection stale-pixel scroll reuses at least one cached row identity");
    ok &= check_row_color(
        scrolled_image,
        1,
        7,
        "selection stale-pixel scroll moves the reused changed row downward",
        975U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_row_blank(
        scrolled_image,
        2,
        "selection stale-pixel scroll keeps the omitted logical row blank",
        975U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_frame_lacks_lifecycle_color(
        scrolled_image,
        4,
        metrics,
        "selection stale-pixel scroll leaves no stale color from logical row 4",
        975U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_frame_lacks_lifecycle_color(
        scrolled_image,
        5,
        metrics,
        "selection stale-pixel scroll leaves no stale color from logical row 5",
        975U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_frame_lacks_lifecycle_color(
        scrolled_image,
        6,
        metrics,
        "selection stale-pixel scroll leaves no stale color from logical row 6",
        975U,
        scrolled_frame,
        scrolled_stats);
    if (expect_batched_geometry) {
        ok &= check(scrolled_stats.selection_batched_rects > 0 &&
            scrolled_stats.selection_batched_vertices ==
                scrolled_stats.selection_batched_rects * 6,
            "selection stale-pixel scroll frame uses batched row geometry");
    }

    return ok;
}

bool test_qsg_decoration_stale_pixel_change_blank_scroll_identity(
    QGuiApplication&   app,
    bool               require_batched_geometry)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(9, 12, 16));
    window.resize(380, 120);

    Direct_render_item item;
    item.setParentItem(window.contentItem());
    window.show();
    pump_events(app, 2);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const bool software_scene_graph             = window_uses_software_scene_graph(window);
    const bool expect_batched_geometry          = !software_scene_graph;
    const qreal decoration_top                  = std::floor(metrics.height * 0.24);
    const qreal decoration_height               = std::max<qreal>(3.0, std::floor(metrics.height * 0.42));
    ok &= check(!require_batched_geometry || !software_scene_graph,
        "decoration batched-geometry stale-pixel mode requires a non-software scene graph");
    const auto make_frame = [&](
        int first_visible_logical_row,
        std::vector<std::pair<int, int>> row_color_ids) {
        term::Terminal_render_frame frame = make_direct_decoration_cache_frame({}, metrics);
        frame.viewport         = make_direct_scroll_lifecycle_viewport(first_visible_logical_row);
        frame.background_rects = {make_direct_frame_background_rect(metrics)};
        frame.decorations.reserve(row_color_ids.size());
        for (const auto& [viewport_row, color_id] : row_color_ids) {
            frame.decorations.push_back(make_direct_decoration_rect(
                viewport_row,
                0,
                20,
                decoration_top,
                decoration_height,
                qsg_lifecycle_row_color(color_id),
                metrics,
                term::Terminal_render_decoration_kind::STRIKE));
        }
        return frame;
    };

    const auto decoration_area = [&](int viewport_row) {
        return
            QRect(
                0,
                static_cast<int>(std::floor(static_cast<qreal>(viewport_row) * metrics.height + decoration_top)),
                static_cast<int>(std::ceil(metrics.width * 20.0)),
                static_cast<int>(std::ceil(decoration_height)));
    };
    const auto check_row_color = [&](
        const QImage& image,
        int viewport_row,
        int color_id,
        const char* message,
        std::uint64_t sequence,
        const term::Terminal_render_frame& frame,
        const term::terminal_renderer_stats_t& stats) {
        const QRect area = decoration_area(viewport_row);
        const int matching_pixels = count_matching_pixels(
            image,
            area,
            [color_id](QColor color) {
                return is_qsg_lifecycle_row_color(color, color_id);
            });
        return
            check_visual_pixels(
                matching_pixels > static_cast<int>(static_cast<qreal>(area.width() * area.height()) * 0.65),
                message,
                image,
                area,
                matching_pixels,
                sequence,
                frame.dirty_row_ranges,
                stats);
    };
    const auto check_row_blank = [&](
        const QImage& image,
        int viewport_row,
        const char* message,
        std::uint64_t sequence,
        const term::Terminal_render_frame& frame,
        const term::terminal_renderer_stats_t& stats) {
        const QRect area            = decoration_area(viewport_row);
        const int   matching_pixels = count_background_pixels(image, area);
        return
            check_visual_pixels(
                matching_pixels > static_cast<int>(static_cast<qreal>(area.width() * area.height()) * 0.75),
                message,
                image,
                area,
                matching_pixels,
                sequence,
                frame.dirty_row_ranges,
                stats);
    };

    const term::Terminal_render_frame setup_frame =
        make_frame(4, {{0, 4}, {1, 5}, {2, 6}});
    item.set_frame(setup_frame);
    const QImage setup_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.paint_completed            &&
            stats.text_content_failures == 0 &&
            stats.decoration_rows_rebuilt == 3;
    });
    const term::terminal_renderer_stats_t setup_stats = item.last_stats();
    ok &= check(!setup_image.isNull(),
        "decoration stale-pixel setup produced an image");
    ok &= check_row_color(
        setup_image,
        0,
        4,
        "decoration stale-pixel setup paints logical row 4",
        976U,
        setup_frame,
        setup_stats);
    ok &= check_row_color(
        setup_image,
        1,
        5,
        "decoration stale-pixel setup paints logical row 5",
        976U,
        setup_frame,
        setup_stats);
    ok &= check_row_color(
        setup_image,
        2,
        6,
        "decoration stale-pixel setup paints logical row 6",
        976U,
        setup_frame,
        setup_stats);
    if (expect_batched_geometry) {
        ok &= check(setup_stats.decoration_batched_rects > 0 &&
            setup_stats.decoration_batched_vertices ==
                setup_stats.decoration_batched_rects * 6,
            "decoration stale-pixel setup uses batched row geometry");
    }

    const term::Terminal_render_frame changed_blank_frame =
        make_frame(4, {{0, 7}, {2, 6}});
    item.set_frame(changed_blank_frame);
    const QImage changed_blank_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.root_reused                  &&
            stats.paint_completed              &&
            stats.text_content_failures == 0   &&
            stats.decoration_rows_rebuilt == 1 &&
            stats.decoration_rows_removed == 1;
    });
    const term::terminal_renderer_stats_t changed_blank_stats = item.last_stats();
    ok &= check(!changed_blank_image.isNull(),
        "decoration stale-pixel changed/blank frame produced an image");
    ok &= check_row_color(
        changed_blank_image,
        0,
        7,
        "decoration stale-pixel change replaces the first row color",
        977U,
        changed_blank_frame,
        changed_blank_stats);
    ok &= check_row_blank(
        changed_blank_image,
        1,
        "decoration stale-pixel blanked row returns to default background",
        977U,
        changed_blank_frame,
        changed_blank_stats);
    ok &= check_frame_lacks_lifecycle_color(
        changed_blank_image,
        4,
        metrics,
        "decoration stale-pixel changed/blank frame removes old row 4 color",
        977U,
        changed_blank_frame,
        changed_blank_stats);
    ok &= check_frame_lacks_lifecycle_color(
        changed_blank_image,
        5,
        metrics,
        "decoration stale-pixel changed/blank frame removes old row 5 color",
        977U,
        changed_blank_frame,
        changed_blank_stats);
    if (expect_batched_geometry) {
        ok &= check(changed_blank_stats.decoration_batched_rects > 0 &&
            changed_blank_stats.decoration_batched_vertices ==
                changed_blank_stats.decoration_batched_rects * 6,
            "decoration stale-pixel changed/blank frame uses batched row geometry");
    }

    const term::Terminal_render_frame scrolled_frame =
        make_frame(3, {{0, 3}, {1, 7}});
    item.set_frame(scrolled_frame);
    const QImage scrolled_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.root_reused                 &&
            stats.paint_completed             &&
            stats.text_content_failures == 0  &&
            stats.decoration_rows_reused >= 1 &&
            stats.decoration_rows_removed >= 1;
    });
    const term::terminal_renderer_stats_t scrolled_stats = item.last_stats();
    ok &= check(!scrolled_image.isNull(),
        "decoration stale-pixel scrolled frame produced an image");
    ok &= check(scrolled_stats.decoration_rows_reused >= 1,
        "decoration stale-pixel scroll reuses at least one cached row identity");
    ok &= check_row_color(
        scrolled_image,
        1,
        7,
        "decoration stale-pixel scroll moves the reused changed row downward",
        978U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_row_blank(
        scrolled_image,
        2,
        "decoration stale-pixel scroll keeps the omitted logical row blank",
        978U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_frame_lacks_lifecycle_color(
        scrolled_image,
        4,
        metrics,
        "decoration stale-pixel scroll leaves no stale color from logical row 4",
        978U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_frame_lacks_lifecycle_color(
        scrolled_image,
        5,
        metrics,
        "decoration stale-pixel scroll leaves no stale color from logical row 5",
        978U,
        scrolled_frame,
        scrolled_stats);
    ok &= check_frame_lacks_lifecycle_color(
        scrolled_image,
        6,
        metrics,
        "decoration stale-pixel scroll leaves no stale color from logical row 6",
        978U,
        scrolled_frame,
        scrolled_stats);
    if (expect_batched_geometry) {
        ok &= check(scrolled_stats.decoration_batched_rects > 0 &&
            scrolled_stats.decoration_batched_vertices ==
                scrolled_stats.decoration_batched_rects * 6,
            "decoration stale-pixel scroll frame uses batched row geometry");
    }

    return ok;
}

bool test_qsg_selection_decoration_layer_order_and_strike_pixels(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(9, 12, 16));
    window.resize(380, 120);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const qreal strike_thickness =
        std::max<qreal>(2.0, std::floor(metrics.height * 0.12));
    const qreal strike_top = std::floor(metrics.ascent * 0.55);

    term::Terminal_render_frame frame = make_direct_rect_cache_frame(
        { make_direct_frame_background_rect(metrics) },
        { make_direct_row_rect(0, 1, 4, QColor(44, 82, 174), metrics) },
        metrics,
        false);
    frame.graphic_rects = {
        make_direct_row_rect(0, 1, 1, QColor(42, 178, 82), metrics),
    };
    frame.text_runs     = {
        make_direct_text_run(2, 1, QString(QChar(0x2588)), metrics),
    };
    frame.decorations   = {
        make_direct_decoration_rect(
            0,
            1,
            4,
            strike_top,
            strike_thickness,
            QColor(236, 210, 62),
            metrics,
            term::Terminal_render_decoration_kind::STRIKE),
    };

    Direct_render_item item;
    item.setParentItem(window.contentItem());
    item.set_frame(frame);
    window.show();

    const QImage image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.paint_completed                &&
            stats.text_content_failures     == 0 &&
            stats.selection_rows_rebuilt    == 1 &&
            stats.graphic_rect_rows_rebuilt == 1 &&
            stats.decoration_rows_rebuilt   == 1;
    });
    const term::terminal_renderer_stats_t stats = item.last_stats();
    ok &= check(!image.isNull(),
        "selection/decoration layer-order test renders the direct frame");

    const auto selection_pixel = [](QColor color) {
        return
            color.blue() > color.red() + 50 &&
            color.blue() > color.green() + 30;
    };
    const auto graphic_pixel = [](QColor color) {
        return
            color.green() > color.red() + 50 &&
            color.green() > color.blue() + 20;
    };
    const auto strike_pixel = [](QColor color) {
        return
            color.red()   > 180 &&
            color.green() > 150 &&
            color.blue()  < 120;
    };
    const auto bright_text_pixel = [](QColor color) {
        return
            color.red()   > 150 &&
            color.green() > 170 &&
            color.blue()  > 150;
    };
    const auto strike_band_area = [&](int column) {
        const int horizontal_inset =
            static_cast<int>(std::floor(metrics.width * 0.2));
        const QRect cell = cell_area(0, column, 1, metrics);
        return
            QRect(
                cell.left() + horizontal_inset,
                static_cast<int>(std::floor(strike_top)),
                std::max(1, cell.width() - horizontal_inset * 2),
                static_cast<int>(std::ceil(strike_thickness)));
    };

    ok &= check(mostly_background(image, 0, 0, 1, metrics),
        "layer-order test leaves untouched background cells at the background color");
    ok &= check(count_matching_pixels(
        image,
        cell_area(0, 3, 1, metrics),
        selection_pixel) > 20,
        "selection layer paints above the background layer");
    ok &= check(count_matching_pixels(
        image,
        cell_area(0, 1, 1, metrics),
        graphic_pixel) > 20,
        "graphic layer paints above the selection layer");
    ok &= check(count_matching_pixels(
        image,
        cell_area(0, 2, 1, metrics),
        bright_text_pixel) > 2,
        "text remains visible above the selection layer");

    const QRect graphic_strike_area = strike_band_area(1);
    const int graphic_strike_pixels =
        count_matching_pixels(image, graphic_strike_area, strike_pixel);
    if (graphic_strike_pixels < 6) {
        std::cerr << "INFO: observed graphic-overlap strike decoration pixels: "
            << graphic_strike_pixels << '\n';
    }
    ok &= check_visual_pixels(
        graphic_strike_pixels >= 6,
        "strike decoration pixels paint above graphics",
        image,
        graphic_strike_area,
        graphic_strike_pixels,
        979U,
        frame.dirty_row_ranges,
        stats);

    const QRect text_strike_area = strike_band_area(2);
    const int text_strike_pixels =
        count_matching_pixels(image, text_strike_area, strike_pixel);
    if (text_strike_pixels < 6) {
        std::cerr << "INFO: observed text-overlap strike decoration pixels: "
            << text_strike_pixels << '\n';
    }
    ok &= check_visual_pixels(
        text_strike_pixels >= 6,
        "strike decoration pixels paint above text",
        image,
        text_strike_area,
        text_strike_pixels,
        980U,
        frame.dirty_row_ranges,
        stats);

    return ok;
}

bool test_qsg_grid_resize_full_dirty_cached_rows(QGuiApplication& app)
{
    constexpr int initial_rows              = 24;
    constexpr int initial_columns           = 80;
    constexpr int large_rows                = 48;
    constexpr int large_columns             = 160;
    constexpr int initial_first_logical_row = 200;
    constexpr int large_first_logical_row   =
        initial_first_logical_row - (large_rows - initial_rows);
    constexpr int background_sample_column = 10;
    constexpr int graphic_sample_column    = 1;
    constexpr int initial_text_column      = initial_columns - 3;
    constexpr int large_text_column        = large_columns - 3;

    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(9, 12, 16));

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    configure_test_font(surface);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const bool software_scene_graph = window_uses_software_scene_graph(window);
    const auto logical_size = [&](int rows, int columns) {
        return QSizeF(
            metrics.width * static_cast<qreal>(columns),
            metrics.height * static_cast<qreal>(rows));
    };
    const auto resize_surface = [&](int rows, int columns) {
        const QSizeF size = logical_size(rows, columns);
        surface.setSize(size);
        window.resize(
            static_cast<int>(std::ceil(size.width())),
            static_cast<int>(std::ceil(size.height())));
    };
    const auto cell_matches_background =
        [&](const QImage& image, int row, int column, int logical_row, int phase) {
            const QRect area = cell_area(row, column, 1, metrics);
            return count_matching_pixels(
                image,
                area,
                [&](QColor color) {
                    return is_grid_resize_background_color(color, logical_row, phase);
                }) >
            static_cast<int>(
                static_cast<qreal>(area.width() * area.height()) * 0.75);
        };
    const auto cell_matches_graphic =
        [&](const QImage& image, int row, int column, int logical_row, int phase) {
            const QRect area = cell_area(row, column, 1, metrics);
            return count_matching_pixels(
                image,
                area,
                [&](QColor color) {
                    return is_grid_resize_graphic_color(color, logical_row, phase);
                }) >
            static_cast<int>(
                static_cast<qreal>(area.width() * area.height()) * 0.40);
        };
    const auto bright_text_pixels = [&](const QImage& image, int row, int column) {
        return count_matching_pixels(
            image,
            cell_area(row, column, 1, metrics),
            [](QColor color) {
                return
                    color.red()   > 180 &&
                    color.green() > 180 &&
                    color.blue()  > 170;
            });
    };

    resize_surface(initial_rows, initial_columns);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_grid_resize_cached_rows_snapshot(
            960U,
            initial_rows,
            initial_columns,
            initial_first_logical_row,
            0));
    window.show();

    const QImage initial_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            invalidation_stats.last_rendered_snapshot_sequence == 960U &&
            !invalidation_stats.pending_update                         &&
            stats.paint_completed                                      &&
            stats.text_content_failures == 0                           &&
            stats.text_content_rebuilds == initial_rows                &&
            stats.background_rows_rebuilt == initial_rows              &&
            stats.graphic_rect_rows_rebuilt == initial_rows;
    });
    ok &= check(!initial_image.isNull(),
        "grid-resize cached-row baseline renders the 80x24 image");

    resize_surface(large_rows, large_columns);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_grid_resize_cached_rows_snapshot(
            961U,
            large_rows,
            large_columns,
            large_first_logical_row,
            0));
    const QImage resized_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            invalidation_stats.last_rendered_snapshot_sequence == 961U   &&
            !invalidation_stats.pending_update                           &&
            stats.root_reused                                            &&
            stats.paint_completed                                        &&
            stats.text_content_failures == 0                             &&
            stats.text_content_rebuilds == large_rows                    &&
            stats.background_rows_rebuilt == large_rows - initial_rows   &&
            stats.background_rows_reused == initial_rows                 &&
            stats.graphic_rect_rows_rebuilt == large_rows - initial_rows &&
            stats.graphic_rect_rows_reused == initial_rows;
    });
    const term::terminal_renderer_stats_t resize_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    ok &= check(!resized_image.isNull(),
        "grid-resize cached-row transition renders the 160x48 image");
    ok &= check(resize_stats.text_content_rebuilds == large_rows,
        "grid-resize full dirty transition rebuilds every visible text row");
    ok &= check(resize_stats.background_rows_reused == initial_rows &&
        resize_stats.graphic_rect_rows_reused == initial_rows,
        "grid-resize transition reuses cached background and graphic rows by logical identity");
    if (software_scene_graph) {
        ok &= check(resize_stats.graphic_batched_rects == 0 &&
            resize_stats.graphic_batched_vertices == 0,
            "software grid-resize transition leaves graphic batched counters at zero");
    }
    else {
        ok &= check(resize_stats.graphic_batched_rects == large_rows &&
            resize_stats.graphic_batched_vertices ==
                resize_stats.graphic_batched_rects * 6,
            "grid-resize transition batches every visible graphic row rect");
    }

    const int moved_initial_viewport_row = large_rows - initial_rows;
    ok &= check(cell_matches_background(
        resized_image,
        0,
        background_sample_column,
        large_first_logical_row,
        0),
        "grid-resize transition paints the new top logical row background");
    ok &= check(!cell_matches_background(
        resized_image,
        0,
        background_sample_column,
        initial_first_logical_row,
        0),
        "grid-resize transition leaves no stale old top-row background at viewport row zero");
    ok &= check(cell_matches_background(
        resized_image,
        moved_initial_viewport_row,
        background_sample_column,
        initial_first_logical_row,
        0),
        "grid-resize transition moves the old top logical row background downward");
    ok &= check(cell_matches_graphic(
        resized_image,
        moved_initial_viewport_row,
        graphic_sample_column,
        initial_first_logical_row,
        0),
        "grid-resize transition moves the cached graphic row downward");
    ok &= check(bright_text_pixels(
        resized_image,
        moved_initial_viewport_row,
        large_text_column) > 2,
        "grid-resize transition repaints moved text at the widened right edge");
    ok &= check(bright_text_pixels(
        resized_image,
        moved_initial_viewport_row,
        initial_text_column) == 0,
        "grid-resize transition leaves no stale text at the old right edge");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_grid_resize_cached_rows_snapshot(
            962U,
            large_rows,
            large_columns,
            large_first_logical_row,
            1));
    const QImage repaint_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            invalidation_stats.last_rendered_snapshot_sequence == 962U &&
            !invalidation_stats.pending_update                         &&
            stats.root_reused                                          &&
            stats.paint_completed                                      &&
            stats.text_content_failures == 0                           &&
            stats.text_content_rebuilds == large_rows                  &&
            stats.background_rows_rebuilt == large_rows                &&
            stats.graphic_rect_rows_rebuilt == large_rows;
    });
    const term::terminal_renderer_stats_t repaint_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    ok &= check(!repaint_image.isNull(),
        "grid-resize full-dirty repaint renders the second large image");
    ok &= check(repaint_stats.background_rows_rebuilt == large_rows &&
        repaint_stats.graphic_rect_rows_rebuilt == large_rows,
        "grid-resize full-dirty repaint rebuilds cached background and graphic rows");
    if (software_scene_graph) {
        ok &= check(repaint_stats.graphic_batched_rects == 0 &&
            repaint_stats.graphic_batched_vertices == 0,
            "software grid-resize full-dirty repaint leaves graphic batched counters at zero");
    }
    else {
        ok &= check(repaint_stats.graphic_batched_rects == large_rows &&
            repaint_stats.graphic_batched_vertices ==
                repaint_stats.graphic_batched_rects * 6,
            "grid-resize full-dirty repaint batches every visible graphic row rect");
    }
    ok &= check(cell_matches_background(
        repaint_image,
        0,
        background_sample_column,
        large_first_logical_row,
        1),
        "grid-resize full-dirty repaint paints the replacement top-row background");
    ok &= check(!cell_matches_background(
        repaint_image,
        0,
        background_sample_column,
        large_first_logical_row,
        0),
        "grid-resize full-dirty repaint leaves no stale first-phase top-row background");
    ok &= check(cell_matches_graphic(
        repaint_image,
        0,
        graphic_sample_column,
        large_first_logical_row,
        1),
        "grid-resize full-dirty repaint paints the replacement top-row graphic");

    resize_surface(initial_rows, initial_columns);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_grid_resize_cached_rows_snapshot(
            963U,
            initial_rows,
            initial_columns,
            initial_first_logical_row,
            1));
    const QImage shrink_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            invalidation_stats.last_rendered_snapshot_sequence == 963U &&
            !invalidation_stats.pending_update                         &&
            stats.root_reused                                          &&
            stats.paint_completed                                      &&
            stats.text_content_failures == 0                           &&
            stats.background_rows_reused == initial_rows               &&
            stats.background_rows_removed == large_rows - initial_rows &&
            stats.graphic_rect_rows_reused == initial_rows             &&
            stats.graphic_rect_rows_removed == large_rows - initial_rows;
    });
    const term::terminal_renderer_stats_t shrink_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    ok &= check(!shrink_image.isNull(),
        "grid-resize shrink transition renders the 80x24 image");
    ok &= check(shrink_stats.background_rows_reused == initial_rows &&
        shrink_stats.background_rows_removed == large_rows - initial_rows &&
        shrink_stats.graphic_rect_rows_reused == initial_rows &&
        shrink_stats.graphic_rect_rows_removed == large_rows - initial_rows,
        "grid-resize shrink reuses surviving visible rows and removes departed slots");
    ok &= check(cell_matches_background(
        shrink_image,
        0,
        background_sample_column,
        initial_first_logical_row,
        1),
        "grid-resize shrink moves the first surviving logical row to viewport row zero");
    ok &= check(!cell_matches_background(
        shrink_image,
        0,
        background_sample_column,
        large_first_logical_row,
        1),
        "grid-resize shrink does not revive stale pixels from removed top rows");

    return ok;
}

bool test_qsg_row_cache_scroll_removal_reorder_pixels_and_lifecycle(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(9, 12, 16));
    window.resize(380, 120);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    auto lifecycle_recorder = std::make_shared<Renderer_lifecycle_recorder>();
    Direct_render_item item;
    item.setParentItem(window.contentItem());
    item.set_lifecycle_recorder(lifecycle_recorder);
    item.set_frame(make_direct_scroll_lifecycle_background_frame(4, metrics));
    window.show();

    const QImage setup_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.paint_completed            &&
            stats.text_content_failures == 0 &&
            stats.background_layer_rebuilt   &&
            stats.background_rows_rebuilt == 3;
    });
    const term::terminal_renderer_lifecycle_stats_t setup_lifecycle_stats =
        lifecycle_recorder->snapshot();
    ok &= check(!setup_image.isNull(),
        "row-cache scroll lifecycle test renders the initial cached rows");
    ok &= check(row_area_has_lifecycle_color(setup_image, 0, 4, metrics) &&
        row_area_has_lifecycle_color(setup_image, 1, 5, metrics) &&
        row_area_has_lifecycle_color(setup_image, 2, 6, metrics),
        "row-cache scroll lifecycle setup paints each logical row in viewport order");
    ok &= check(live_rect_resource_count(setup_lifecycle_stats) == 3U,
        "row-cache scroll lifecycle setup records three live row wrapper resources");

    item.set_frame(make_direct_scroll_lifecycle_background_frame(5, metrics));
    const QImage scroll_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.root_reused                  &&
            stats.paint_completed              &&
            stats.text_content_failures == 0   &&
            stats.background_layer_rebuilt     &&
            stats.background_rows_rebuilt == 1 &&
            stats.background_rows_reused == 2  &&
            stats.background_rows_removed == 1;
    });
    const term::terminal_renderer_lifecycle_stats_t scroll_lifecycle_stats =
        lifecycle_recorder->snapshot();
    ok &= check(!scroll_image.isNull(),
        "row-cache scroll lifecycle test renders the first scrolled frame");
    ok &= check(row_area_has_lifecycle_color(scroll_image, 0, 5, metrics) &&
        row_area_has_lifecycle_color(scroll_image, 1, 6, metrics) &&
        row_area_has_lifecycle_color(scroll_image, 2, 7, metrics),
        "row-cache scroll lifecycle reorders reused wrappers to their new viewport rows");
    ok &= check(frame_lacks_lifecycle_color(scroll_image, 4, metrics),
        "row-cache scroll lifecycle removes pixels for the row that left the viewport");
    ok &= check(live_rect_resource_count(scroll_lifecycle_stats) == 3U &&
        scroll_lifecycle_stats.render_rect_resources_destroyed >
            setup_lifecycle_stats.render_rect_resources_destroyed,
        "row-cache scroll lifecycle destroys the removed row wrapper and keeps three live rows");

    item.set_frame(make_direct_scroll_lifecycle_background_frame(6, metrics));
    const QImage consecutive_scroll_image = render_window_until(
        app,
        window,
        [&](const QImage&) {
            const term::terminal_renderer_stats_t stats = item.last_stats();
            return
                stats.root_reused                  &&
                stats.paint_completed              &&
                stats.text_content_failures == 0   &&
                stats.background_layer_rebuilt     &&
                stats.background_rows_rebuilt == 1 &&
                stats.background_rows_reused == 2  &&
                stats.background_rows_removed == 1;
        });
    const term::terminal_renderer_lifecycle_stats_t consecutive_lifecycle_stats =
        lifecycle_recorder->snapshot();
    ok &= check(!consecutive_scroll_image.isNull(),
        "row-cache scroll lifecycle test renders the consecutive scrolled frame");
    ok &= check(row_area_has_lifecycle_color(consecutive_scroll_image, 0, 6, metrics) &&
        row_area_has_lifecycle_color(consecutive_scroll_image, 1, 7, metrics) &&
        row_area_has_lifecycle_color(consecutive_scroll_image, 2, 8, metrics),
        "consecutive row-cache scroll keeps logical row identity reuse aligned");
    ok &= check(frame_lacks_lifecycle_color(consecutive_scroll_image, 5, metrics),
        "consecutive row-cache scroll removes the previous top-row pixels");
    ok &= check(live_rect_resource_count(consecutive_lifecycle_stats) == 3U,
        "consecutive row-cache scroll keeps the live wrapper count bounded");
    return ok;
}

bool test_qsg_row_cache_flat_fallback_transition_clears_pixels_and_lifecycle(
    QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(9, 12, 16));
    window.resize(380, 120);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const auto row_rect = [&](int row, int color_id) {
        return make_direct_row_rect(
            row,
            0,
            20,
            qsg_lifecycle_row_color(color_id),
            metrics);
    };
    const term::Terminal_render_rect non_row_rect{
        QRectF(
            metrics.width * 4.0,
            metrics.height * 0.70,
            metrics.width * 5.0,
            metrics.height * 1.20),
        qsg_lifecycle_row_color(4),
    };

    auto lifecycle_recorder = std::make_shared<Renderer_lifecycle_recorder>();
    Direct_render_item item;
    item.setParentItem(window.contentItem());
    item.set_lifecycle_recorder(lifecycle_recorder);
    item.set_frame(make_direct_rect_cache_frame(
        { row_rect(0, 0), row_rect(1, 1), row_rect(2, 2) },
        {},
        metrics));
    window.show();

    const QImage cached_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.paint_completed            &&
            stats.text_content_failures == 0 &&
            stats.background_layer_rebuilt   &&
            stats.background_rows_rebuilt == 3;
    });
    ok &= check(!cached_image.isNull(),
        "flat fallback transition test renders the initial cached rows");
    ok &= check(row_area_has_lifecycle_color(cached_image, 0, 0, metrics) &&
        row_area_has_lifecycle_color(cached_image, 1, 1, metrics) &&
        row_area_has_lifecycle_color(cached_image, 2, 2, metrics),
        "flat fallback transition setup paints row-cached colors");
    ok &= check(live_rect_resource_count(lifecycle_recorder->snapshot()) == 3U,
        "flat fallback transition setup records live row wrappers");

    item.set_frame(make_direct_rect_cache_frame(
        { row_rect(0, 3), non_row_rect, row_rect(2, 0) },
        {},
        metrics));
    const QImage fallback_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.root_reused                         &&
            stats.paint_completed                     &&
            stats.text_content_failures == 0          &&
            stats.background_layer_rebuilt            &&
            stats.background_row_cache_fallbacks == 1 &&
            stats.background_rows_removed == 3;
    });
    const term::terminal_renderer_lifecycle_stats_t fallback_lifecycle_stats =
        lifecycle_recorder->snapshot();
    ok &= check(!fallback_image.isNull(),
        "flat fallback transition test renders the mixed fallback frame");
    ok &= check(row_area_has_lifecycle_color(fallback_image, 0, 3, metrics) &&
        count_matching_pixels(
            fallback_image,
            cell_area(1, 4, 5, metrics),
            [](QColor color) {
                return is_qsg_lifecycle_row_color(color, 4);
            }) > 20 &&
        mostly_background(fallback_image, 1, 0, 2, metrics),
        "flat fallback transition paints the flat mixed frame without stale row-cache fill");
    ok &= check(frame_lacks_lifecycle_color(fallback_image, 1, metrics),
        "flat fallback transition removes pixels from a discarded cached row");
    ok &= check(live_rect_resource_count(fallback_lifecycle_stats) == 0U,
        "flat fallback transition destroys all row wrapper resources");

    item.set_frame(make_direct_rect_cache_frame(
        { row_rect(0, 2), row_rect(1, 3), row_rect(2, 0) },
        {},
        metrics));
    const QImage recovered_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.root_reused                         &&
            stats.paint_completed                     &&
            stats.text_content_failures == 0          &&
            stats.background_layer_rebuilt            &&
            stats.background_row_cache_fallbacks == 0 &&
            stats.background_rows_rebuilt == 3;
    });
    const term::terminal_renderer_lifecycle_stats_t recovered_lifecycle_stats =
        lifecycle_recorder->snapshot();
    ok &= check(!recovered_image.isNull(),
        "flat fallback transition test renders the recovered row-cache frame");
    ok &= check(row_area_has_lifecycle_color(recovered_image, 0, 2, metrics) &&
        row_area_has_lifecycle_color(recovered_image, 1, 3, metrics) &&
        row_area_has_lifecycle_color(recovered_image, 2, 0, metrics),
        "flat fallback transition rebuilds row-cache pixels after the flat pass");
    ok &= check(frame_lacks_lifecycle_color(recovered_image, 4, metrics),
        "flat fallback transition removes stale flat fallback pixels after row-cache recovery");
    ok &= check(live_rect_resource_count(recovered_lifecycle_stats) == 3U,
        "flat fallback transition recreates three live row wrapper resources");
    return ok;
}

bool test_qsg_decoration_row_cache(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);

    const term::terminal_cell_metrics_t metrics              = test_metrics();
    const bool                          software_scene_graph = window_uses_software_scene_graph(window);
    const qreal                         decoration_thickness =
        std::max<qreal>(1.0, std::floor(metrics.height * 0.08));
    const term::Terminal_render_decoration underline_row_zero = make_direct_decoration_rect(
        0,
        1,
        4,
        metrics.ascent + decoration_thickness,
        decoration_thickness,
        QColor(212, 184, 72),
        metrics);
    const term::Terminal_render_decoration underline_row_one = make_direct_decoration_rect(
        1,
        2,
        3,
        metrics.ascent + decoration_thickness,
        decoration_thickness,
        QColor(82, 186, 132),
        metrics);
    const term::Terminal_render_decoration underline_row_one_changed = make_direct_decoration_rect(
        1,
        2,
        4,
        metrics.ascent + decoration_thickness,
        decoration_thickness,
        QColor(92, 146, 214),
        metrics);

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };

    const term::Terminal_render_frame base_frame = make_direct_decoration_cache_frame(
        { underline_row_zero, underline_row_one },
        metrics);
    term::terminal_renderer_stats_t base_stats;
    ok &= check(update(base_frame, base_stats),
        "decoration row cache renders the base thin decoration frame");
    ok &= check(base_stats.decoration_layer_rebuilt &&
        base_stats.decoration_rows_rebuilt == 2 &&
        base_stats.decoration_row_cache_fallbacks == 0,
        "thin in-row decorations build row cached decoration resources");

    QSGNode* decoration_layer       = child_node_at(node,             4);
    QSGNode* decoration_frame_layer = child_node_at(decoration_layer, 0);
    QSGNode* decoration_row_layer   = child_node_at(decoration_layer, 1);
    const std::vector<QSGTransformNode*> base_decoration_wrappers =
        transform_child_nodes(decoration_row_layer);
    ok &= check(decoration_frame_layer != nullptr &&
        decoration_frame_layer->firstChild() == nullptr,
        "row-cached decorations keep the flat decoration frame layer empty");
    ok &= check(child_node_count(decoration_row_layer) == 2 &&
        base_decoration_wrappers.size() == 2U,
        "row-cached decorations attach one wrapper per decorated row");
    if (base_decoration_wrappers.size() == 2U) {
        const QPointF row_zero_origin =
            base_decoration_wrappers[0]->matrix().map(QPointF(0.0, 0.0));
        const QPointF row_one_origin =
            base_decoration_wrappers[1]->matrix().map(QPointF(0.0, 0.0));
        const std::vector<QRectF> row_zero_simple_rects =
            simple_rect_child_rects(base_decoration_wrappers[0]);
        const std::vector<QRectF> row_one_simple_rects =
            simple_rect_child_rects(base_decoration_wrappers[1]);
        const std::vector<QSGGeometryNode*> row_zero_geometry_nodes =
            geometry_child_nodes(base_decoration_wrappers[0]);
        const std::vector<QSGGeometryNode*> row_one_geometry_nodes =
            geometry_child_nodes(base_decoration_wrappers[1]);
        const std::vector<QRectF> row_zero_rects =
            software_scene_graph ? row_zero_simple_rects :
                                   batched_geometry_child_rect_bounds(
                                       base_decoration_wrappers[0]);
        const std::vector<QRectF> row_one_rects =
            software_scene_graph ? row_one_simple_rects :
                                   batched_geometry_child_rect_bounds(
                                       base_decoration_wrappers[1]);
        const qreal expected_decoration_top = metrics.ascent + decoration_thickness;
        ok &= check(std::abs(row_zero_origin.y()) <= 0.25 &&
            std::abs(row_one_origin.y() - metrics.height) <= 0.25,
            "decoration row wrappers carry absolute row placement");
        if (software_scene_graph) {
            ok &= check(row_zero_simple_rects.size() == 1U &&
                row_one_simple_rects.size() == 1U,
                "software decoration row cache keeps one simple rect per wrapper");
        }
        else {
            ok &= check(row_zero_simple_rects.empty() &&
                row_one_simple_rects.empty() &&
                row_zero_geometry_nodes.size() == 1U &&
                row_one_geometry_nodes.size() == 1U &&
                row_zero_geometry_nodes[0]->geometry() != nullptr &&
                row_zero_geometry_nodes[0]->geometry()->vertexCount() == 6 &&
                row_one_geometry_nodes[0]->geometry() != nullptr &&
                row_one_geometry_nodes[0]->geometry()->vertexCount() == 6,
                "non-software decoration row cache keeps one batched geometry node per wrapper");
        }
        ok &= check(row_zero_rects.size() == 1U &&
            row_one_rects.size() == 1U &&
            std::abs(row_zero_rects[0].top() - expected_decoration_top) <= 0.25 &&
            std::abs(row_one_rects[0].top()  - expected_decoration_top) <= 0.25,
            "decoration rect geometry stays row-local inside cached wrappers");
    }

    term::terminal_renderer_stats_t reuse_stats;
    ok &= check(update(base_frame, reuse_stats),
        "decoration row cache rerenders the unchanged thin decoration frame");
    ok &= check(!reuse_stats.decoration_layer_rebuilt &&
        reuse_stats.decoration_row_clean_reuse_skips == 1 &&
        reuse_stats.decoration_rows_reused == 0,
        "unchanged empty-dirty thin decorations skip row cached decoration work");

    const term::Terminal_render_frame dirty_base_frame =
        with_all_rows_dirty(base_frame);
    term::terminal_renderer_stats_t dirty_reuse_stats;
    ok &= check(update(dirty_base_frame, dirty_reuse_stats),
        "decoration row cache rerenders a dirty unchanged thin decoration frame");
    ok &= check(!dirty_reuse_stats.decoration_layer_rebuilt &&
        dirty_reuse_stats.decoration_row_clean_reuse_skips == 1 &&
        dirty_reuse_stats.decoration_rows_reused == 0,
        "dirty unchanged thin decorations skip unchanged geometry layers");

    const term::Terminal_render_frame changed_frame = make_direct_decoration_cache_frame(
        { underline_row_zero, underline_row_one_changed },
        metrics);
    term::terminal_renderer_stats_t changed_stats;
    ok &= check(update(changed_frame, changed_stats),
        "decoration row cache renders a one-row decoration mutation");
    ok &= check(changed_stats.decoration_layer_rebuilt &&
        changed_stats.decoration_rows_rebuilt == 1 &&
        changed_stats.decoration_rows_reused == 1,
        "changing one thin decoration rebuilds only that decoration row");

    const term::Terminal_render_frame removed_frame =
        make_direct_decoration_cache_frame({underline_row_zero}, metrics);
    term::terminal_renderer_stats_t removed_stats;
    ok &= check(update(removed_frame, removed_stats),
        "decoration row cache renders a decoration row removal");
    ok &= check(removed_stats.decoration_layer_rebuilt &&
        removed_stats.decoration_rows_reused == 1 &&
        removed_stats.decoration_rows_removed == 1,
        "removing one decoration row removes only that cached decoration row");

    decoration_layer     = child_node_at(node,             4);
    decoration_row_layer = child_node_at(decoration_layer, 1);
    const std::vector<QSGTransformNode*> removed_decoration_wrappers =
        transform_child_nodes(decoration_row_layer);
    ok &= check(child_node_count(decoration_row_layer) == 1 &&
        removed_decoration_wrappers.size() == 1U,
        "removing one decoration row detaches the stale row wrapper");
    if (removed_decoration_wrappers.size() == 1U) {
        const QPointF row_zero_origin =
            removed_decoration_wrappers[0]->matrix().map(QPointF(0.0, 0.0));
        ok &= check(std::abs(row_zero_origin.y()) <= 0.25,
            "remaining decoration wrapper keeps the surviving row placement");
    }

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_graphic_rect_arc_row_cache(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const bool software_scene_graph = window_uses_software_scene_graph(window);
    const term::Terminal_render_rect    rect_row_zero        = make_direct_row_rect(
        0,
        1,
        3,
        QColor(54, 176, 112),
        metrics);
    const term::Terminal_render_rect rect_row_zero_changed = make_direct_row_rect(
        0,
        1,
        3,
        QColor(74, 126, 212),
        metrics);
    const term::Terminal_render_arc arc_row_one = make_direct_row_arc(
        1,
        5,
        term::Terminal_render_arc_kind::DOWN_RIGHT,
        QColor(222, 190, 74),
        2.0,
        metrics);
    const term::Terminal_render_arc arc_row_one_changed = make_direct_row_arc(
        1,
        5,
        term::Terminal_render_arc_kind::UP_LEFT,
        QColor(222, 190, 74),
        3.0,
        metrics);

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };

    const term::Terminal_render_frame base_frame =
        make_direct_graphic_row_cache_frame({rect_row_zero}, {arc_row_one}, metrics);
    term::terminal_renderer_stats_t base_stats;
    ok &= check(update(base_frame, base_stats),
        "graphic row cache renders the base rect and arc frame");
    if (software_scene_graph) {
        ok &= check(base_stats.graphic_layer_rebuilt &&
            base_stats.graphic_rect_rows_rebuilt == 1 &&
            base_stats.graphic_arc_row_cache_fallbacks == 1 &&
            base_stats.graphic_arc_rows_rebuilt == 0,
            "software graphic frame row-caches rects and flat-fallbacks arcs");
    }
    else {
        ok &= check(base_stats.graphic_layer_rebuilt &&
            base_stats.graphic_rect_rows_rebuilt == 1 &&
            base_stats.graphic_arc_rows_rebuilt == 1,
            "base graphic frame builds separate row resources for rects and arcs");
    }

    QSGNode* graphic_layer   = child_node_at(node,          2);
    QSGNode* rect_pass_layer = child_node_at(graphic_layer, 0);
    QSGNode* arc_pass_layer  = child_node_at(graphic_layer, 1);
    ok &= check(rect_pass_layer != nullptr &&
        arc_pass_layer != nullptr &&
        rect_pass_layer->nextSibling() == arc_pass_layer,
        "graphic row cache keeps the rect pass before the arc pass");
    if (software_scene_graph) {
        QSGNode* arc_frame_layer = child_node_at(arc_pass_layer, 0);
        QSGNode* arc_row_layer   = child_node_at(arc_pass_layer, 1);
        ok &= check(arc_frame_layer != nullptr &&
            arc_frame_layer->firstChild() != nullptr &&
            arc_row_layer != nullptr &&
            arc_row_layer->firstChild() == nullptr,
            "software arc fallback uses flat arc nodes and leaves no arc row wrappers");
    }

    term::terminal_renderer_stats_t reuse_stats;
    ok &= check(update(base_frame, reuse_stats),
        "graphic row cache rerenders the unchanged rect and arc frame");
    if (software_scene_graph) {
        ok &= check(!reuse_stats.graphic_layer_rebuilt &&
            reuse_stats.graphic_rect_row_clean_reuse_skips == 1 &&
            reuse_stats.graphic_arc_row_clean_reuse_skips == 1 &&
            reuse_stats.graphic_rect_rows_reused == 0 &&
            reuse_stats.graphic_arc_row_cache_fallbacks == 0,
            "unchanged empty-dirty software graphic frame skips rect and arc passes");
    }
    else {
        ok &= check(!reuse_stats.graphic_layer_rebuilt &&
            reuse_stats.graphic_rect_row_clean_reuse_skips == 1 &&
            reuse_stats.graphic_arc_row_clean_reuse_skips == 1 &&
            reuse_stats.graphic_rect_rows_reused == 0 &&
            reuse_stats.graphic_arc_rows_reused == 0,
            "unchanged empty-dirty graphic rect and arc passes are skipped");
    }

    const term::Terminal_render_frame dirty_base_frame =
        with_all_rows_dirty(base_frame);
    term::terminal_renderer_stats_t dirty_reuse_stats;
    ok &= check(update(dirty_base_frame, dirty_reuse_stats),
        "graphic row cache rerenders the dirty unchanged rect and arc frame");
    if (software_scene_graph) {
        ok &= check(!dirty_reuse_stats.graphic_layer_rebuilt &&
            dirty_reuse_stats.graphic_rect_row_clean_reuse_skips == 1 &&
            dirty_reuse_stats.graphic_arc_row_clean_reuse_skips == 1 &&
            dirty_reuse_stats.graphic_rect_rows_reused == 0 &&
            dirty_reuse_stats.graphic_arc_row_cache_fallbacks == 0,
            "dirty unchanged software graphic frame skips unchanged geometry layers");
    }
    else {
        ok &= check(!dirty_reuse_stats.graphic_layer_rebuilt &&
            dirty_reuse_stats.graphic_rect_row_clean_reuse_skips == 1 &&
            dirty_reuse_stats.graphic_arc_row_clean_reuse_skips == 1 &&
            dirty_reuse_stats.graphic_rect_rows_reused == 0 &&
            dirty_reuse_stats.graphic_arc_rows_reused == 0,
            "dirty unchanged graphic frame skips unchanged geometry layers");
    }

    const term::Terminal_render_frame rect_changed_frame = make_direct_graphic_row_cache_frame(
        { rect_row_zero_changed },
        { arc_row_one },
        metrics);
    term::terminal_renderer_stats_t rect_changed_stats;
    ok &= check(update(rect_changed_frame, rect_changed_stats),
        "graphic row cache renders a graphic rect mutation");
    if (software_scene_graph) {
        ok &= check(rect_changed_stats.graphic_layer_rebuilt &&
            rect_changed_stats.graphic_rect_rows_rebuilt == 1 &&
            rect_changed_stats.graphic_arc_row_clean_reuse_skips == 1 &&
            rect_changed_stats.graphic_arc_row_cache_fallbacks == 0,
            "changing a graphic rect rebuilds the rect row while skipping unchanged arcs");
    }
    else {
        ok &= check(rect_changed_stats.graphic_layer_rebuilt &&
            rect_changed_stats.graphic_rect_rows_rebuilt == 1 &&
            rect_changed_stats.graphic_arc_row_clean_reuse_skips == 1 &&
            rect_changed_stats.graphic_arc_rows_reused == 0,
            "changing a graphic rect rebuilds the rect row while skipping the arc row");
    }

    const term::Terminal_render_frame arc_changed_frame = make_direct_graphic_row_cache_frame(
        { rect_row_zero_changed },
        { arc_row_one_changed },
        metrics);
    term::terminal_renderer_stats_t arc_changed_stats;
    ok &= check(update(arc_changed_frame, arc_changed_stats),
        "graphic row cache renders a graphic arc mutation");
    if (software_scene_graph) {
        ok &= check(arc_changed_stats.graphic_layer_rebuilt &&
            arc_changed_stats.graphic_rect_row_clean_reuse_skips == 1 &&
            arc_changed_stats.graphic_rect_rows_reused == 0 &&
            arc_changed_stats.graphic_arc_row_cache_fallbacks == 1 &&
            arc_changed_stats.graphic_arc_rows_rebuilt == 0,
            "changing a software graphic arc rebuilds the flat arc pass while skipping rect rows");
    }
    else {
        ok &= check(arc_changed_stats.graphic_layer_rebuilt &&
            arc_changed_stats.graphic_rect_row_clean_reuse_skips == 1 &&
            arc_changed_stats.graphic_rect_rows_reused == 0 &&
            arc_changed_stats.graphic_arc_rows_rebuilt == 1,
            "changing a graphic arc rebuilds the arc row while skipping the rect row");
    }

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_geometry_row_slot_active_buffer_identity(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(9, 12, 16));
    window.resize(380, 120);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    Direct_render_item item;
    item.setParentItem(window.contentItem());
    item.set_frame(make_direct_geometry_slot_frame(
        term::Terminal_buffer_id::PRIMARY,
        0,
        metrics));
    window.show();
    pump_events(app, 2);

    const bool software_scene_graph = window_uses_software_scene_graph(window);
    const auto geometry_rows_rebuilt =
        [&](const term::terminal_renderer_stats_t& stats) {
            const bool arc_rebuilt = software_scene_graph
                ? stats.graphic_arc_row_cache_fallbacks == 1
                : stats.graphic_arc_rows_rebuilt == 1;
            return
                stats.paint_completed                &&
                stats.text_content_failures == 0     &&
                stats.background_rows_rebuilt == 1   &&
                stats.selection_rows_rebuilt == 1    &&
                stats.graphic_rect_rows_rebuilt == 1 &&
                stats.decoration_rows_rebuilt == 1   &&
                arc_rebuilt;
        };
    const auto active_buffer_identity_rebuilt =
        [&](const term::terminal_renderer_stats_t& stats) {
            const bool arc_identity_rebuilt = software_scene_graph
                ? stats.graphic_arc_row_cache_fallbacks == 1 &&
                    stats.graphic_arc_row_clean_reuse_skips == 0 &&
                    stats.graphic_arc_rows_rebuilt == 0 &&
                    stats.graphic_arc_rows_reused == 0 &&
                    stats.graphic_arc_rows_removed == 0
                : stats.graphic_arc_rows_rebuilt == 1 &&
                    stats.graphic_arc_row_clean_reuse_skips == 0 &&
                    stats.graphic_arc_rows_reused == 0 &&
                    stats.graphic_arc_rows_removed == 1;
            return
                stats.paint_completed                         &&
                stats.root_reused                             &&
                stats.text_content_failures == 0              &&
                stats.background_layer_rebuilt                &&
                stats.selection_layer_rebuilt                 &&
                stats.graphic_layer_rebuilt                   &&
                stats.decoration_layer_rebuilt                &&
                stats.background_rows_rebuilt == 1            &&
                stats.background_row_clean_reuse_skips == 0   &&
                stats.background_rows_reused == 0             &&
                stats.background_rows_removed == 1            &&
                stats.selection_rows_rebuilt == 1             &&
                stats.selection_row_clean_reuse_skips == 0    &&
                stats.selection_rows_reused == 0              &&
                stats.selection_rows_removed == 1             &&
                stats.graphic_rect_rows_rebuilt == 1          &&
                stats.graphic_rect_row_clean_reuse_skips == 0 &&
                stats.graphic_rect_rows_reused == 0           &&
                stats.graphic_rect_rows_removed == 1          &&
                stats.decoration_rows_rebuilt == 1            &&
                stats.decoration_row_clean_reuse_skips == 0   &&
                stats.decoration_rows_reused == 0             &&
                stats.decoration_rows_removed == 1            &&
                arc_identity_rebuilt;
        };
    const auto has_color_pixels =
        [&](const QImage& image, QRect area, QColor expected, int minimum_pixels) {
            return count_matching_pixels(
                image,
                area,
                [&](QColor color) { return color_is_close(color, expected); }) >
            minimum_pixels;
        };
    const auto primary_arc_pixel = [](QColor color) {
        return
            color.red()   > 150 &&
            color.green() > 130 &&
            color.blue()  < 130;
    };
    const auto alternate_arc_pixel = [](QColor color) {
        return
            color.blue() > 140 &&
            color.red()  < 130;
    };

    const QImage primary_image = render_window_until(app, window, [&](const QImage&) {
        return geometry_rows_rebuilt(item.last_stats());
    });
    ok &= check(!primary_image.isNull(),
        "geometry row slot active-buffer test renders the primary-buffer frame");
    ok &= check(has_color_pixels(
        primary_image,
        cell_area(0, 0, 1, metrics),
        QColor(118, 46, 76),
        20) &&
        has_color_pixels(
            primary_image,
            cell_area(0, 3, 1, metrics),
            QColor(42, 104, 176),
            20) &&
        has_color_pixels(
            primary_image,
            cell_area(1, 1, 1, metrics),
            QColor(220, 174, 54),
            20) &&
        has_color_pixels(
            primary_image,
            underline_area(2, 2, 4, metrics),
            QColor(214, 96, 156),
            2) &&
        count_matching_pixels(
            primary_image,
            cell_area(1, 6, 1, metrics),
            primary_arc_pixel) > 2,
        "primary-buffer geometry row slots paint every geometry family");

    item.set_frame(make_direct_geometry_slot_frame(
        term::Terminal_buffer_id::ALTERNATE,
        0,
        metrics));
    const QImage alternate_same_content_image = render_window_until(
        app,
        window,
        [&](const QImage&) {
            return active_buffer_identity_rebuilt(item.last_stats());
        });
    ok &= check(!alternate_same_content_image.isNull(),
        "same-content alternate-buffer geometry frame rebuilds by active-buffer identity");
    const term::terminal_renderer_stats_t alternate_same_content_stats =
        item.last_stats();
    ok &= check(active_buffer_identity_rebuilt(alternate_same_content_stats),
        "same-content active-buffer transition rebuilds geometry slots without clean skip or identity reuse");
    ok &= check(has_color_pixels(
        alternate_same_content_image,
        cell_area(0, 0, 1, metrics),
        QColor(118, 46, 76),
        20) &&
        has_color_pixels(
            alternate_same_content_image,
            cell_area(0, 3, 1, metrics),
            QColor(42, 104, 176),
            20) &&
        has_color_pixels(
            alternate_same_content_image,
            cell_area(1, 1, 1, metrics),
            QColor(220, 174, 54),
            20) &&
        has_color_pixels(
            alternate_same_content_image,
            underline_area(2, 2, 4, metrics),
            QColor(214, 96, 156),
            2) &&
        count_matching_pixels(
            alternate_same_content_image,
            cell_area(1, 6, 1, metrics),
            primary_arc_pixel) > 2,
        "same-content alternate-buffer geometry frame preserves primary-phase pixels");

    item.set_frame(make_direct_geometry_slot_frame(
        term::Terminal_buffer_id::ALTERNATE,
        1,
        metrics));
    const QImage alternate_image = render_window_until(app, window, [&](const QImage&) {
        return
            item.last_stats().root_reused &&
            geometry_rows_rebuilt(item.last_stats());
    });
    ok &= check(!alternate_image.isNull(),
        "geometry row slot active-buffer test renders the alternate-buffer frame");
    ok &= check(has_color_pixels(
        alternate_image,
        cell_area(0, 0, 1, metrics),
        QColor(36, 118, 78),
        20) &&
        !has_color_pixels(
            alternate_image,
            cell_area(0, 0, 1, metrics),
            QColor(118, 46, 76),
            20) &&
        has_color_pixels(
            alternate_image,
            cell_area(0, 3, 1, metrics),
            QColor(186, 86, 46),
            20) &&
        has_color_pixels(
            alternate_image,
            cell_area(1, 1, 1, metrics),
            QColor(80, 168, 222),
            20) &&
        has_color_pixels(
            alternate_image,
            underline_area(2, 2, 4, metrics),
            QColor(100, 220, 142),
            2) &&
        count_matching_pixels(
            alternate_image,
            cell_area(1, 6, 1, metrics),
            alternate_arc_pixel) > 2 &&
        count_matching_pixels(
            alternate_image,
            cell_area(1, 6, 1, metrics),
            primary_arc_pixel) == 0,
        "alternate-buffer geometry row slots replace same logical-row stale pixels");

    return ok;
}

bool test_qsg_geometry_row_slot_exact_content_descriptors(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const bool software_scene_graph = window_uses_software_scene_graph(window);
    const term::Terminal_render_frame   base_frame           = make_direct_geometry_slot_frame(
        term::Terminal_buffer_id::PRIMARY,
        0,
        metrics);

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };
    const auto reset_to_base = [&]() {
        term::terminal_renderer_stats_t stats;
        ok &= check(update(base_frame, stats),
            "geometry exact descriptor test resets the base frame");
    };
    const auto arc_changed = [&](const term::terminal_renderer_stats_t& stats) {
        return software_scene_graph
            ? stats.graphic_arc_row_cache_fallbacks == 1
            : stats.graphic_arc_rows_rebuilt == 1;
    };

    term::terminal_renderer_stats_t first_stats;
    ok &= check(update(base_frame, first_stats),
        "geometry exact descriptor test creates the base frame");
    ok &= check(first_stats.background_rows_rebuilt == 1 &&
        first_stats.selection_rows_rebuilt == 1 &&
        first_stats.graphic_rect_rows_rebuilt == 1 &&
        first_stats.decoration_rows_rebuilt == 1,
        "base exact descriptor frame builds migrated geometry row slots");

    term::terminal_renderer_stats_t reuse_stats;
    ok &= check(update(base_frame, reuse_stats),
        "geometry exact descriptor test rerenders the unchanged base frame");
    ok &= check(!reuse_stats.background_layer_rebuilt &&
        !reuse_stats.selection_layer_rebuilt &&
        !reuse_stats.graphic_layer_rebuilt &&
        !reuse_stats.decoration_layer_rebuilt &&
        reuse_stats.background_row_clean_reuse_skips == 1 &&
        reuse_stats.selection_row_clean_reuse_skips == 1 &&
        reuse_stats.graphic_rect_row_clean_reuse_skips == 1 &&
        reuse_stats.graphic_arc_row_clean_reuse_skips == 1 &&
        reuse_stats.decoration_row_clean_reuse_skips == 1,
        "unchanged exact descriptor frame clean-skips every geometry row layer");

    term::Terminal_render_frame background_color_frame = base_frame;
    background_color_frame.background_rects = {
        make_direct_row_rect(0, 0, 2, QColor(140, 68, 46), metrics),
    };
    reset_to_base();
    term::terminal_renderer_stats_t background_color_stats;
    ok &= check(update(background_color_frame, background_color_stats),
        "geometry exact descriptor test changes background color");
    ok &= check(background_color_stats.background_rows_rebuilt == 1,
        "background color participates in exact row slot comparison");

    term::Terminal_render_frame selection_alpha_frame = base_frame;
    selection_alpha_frame.selection_rects = {
        make_direct_row_rect(0, 3, 2, QColor(42, 104, 176, 128), metrics),
    };
    reset_to_base();
    term::terminal_renderer_stats_t selection_alpha_stats;
    ok &= check(update(selection_alpha_frame, selection_alpha_stats),
        "geometry exact descriptor test changes selection alpha");
    ok &= check(selection_alpha_stats.selection_rows_rebuilt == 1,
        "selection alpha participates in exact row slot comparison");

    term::Terminal_render_frame graphic_rect_frame = base_frame;
    graphic_rect_frame.graphic_rects = {
        make_direct_row_rect(1, 1, 3, QColor(220, 174, 54), metrics),
    };
    reset_to_base();
    term::terminal_renderer_stats_t graphic_rect_stats;
    ok &= check(update(graphic_rect_frame, graphic_rect_stats),
        "geometry exact descriptor test changes graphic rect geometry");
    ok &= check(graphic_rect_stats.graphic_rect_rows_rebuilt == 1,
        "graphic rect geometry participates in exact row slot comparison");

    term::Terminal_render_frame graphic_antialias_frame = base_frame;
    graphic_antialias_frame.graphic_rects = {
        make_direct_row_rect(1, 1, 2, QColor(220, 174, 54), metrics, true),
    };
    reset_to_base();
    term::terminal_renderer_stats_t graphic_antialias_stats;
    ok &= check(update(graphic_antialias_frame, graphic_antialias_stats),
        "geometry exact descriptor test changes graphic rect antialias");
    ok &= check(graphic_antialias_stats.graphic_layer_rebuilt &&
        graphic_antialias_stats.graphic_rect_rows_removed == 1,
        "graphic rect antialias leaves the row slot route instead of reusing stale geometry");

    const qreal decoration_thickness =
        std::max<qreal>(1.0, std::floor(metrics.height * 0.08));
    term::Terminal_render_frame decoration_frame = base_frame;
    decoration_frame.decorations = {
        make_direct_decoration_rect(
            2,
            2,
            4,
            metrics.ascent * 0.5,
            decoration_thickness + 1.0,
            QColor(214, 96, 156),
            metrics,
            term::Terminal_render_decoration_kind::STRIKE),
    };
    reset_to_base();
    term::terminal_renderer_stats_t decoration_stats;
    ok &= check(update(decoration_frame, decoration_stats),
        "geometry exact descriptor test changes decoration kind and thickness");
    ok &= check(decoration_stats.decoration_rows_rebuilt == 1,
        "decoration geometry participates in exact row slot comparison");

    term::Terminal_render_frame arc_color_frame = base_frame;
    arc_color_frame.graphic_arcs = {
        make_direct_row_arc(
            1,
            6,
            term::Terminal_render_arc_kind::DOWN_RIGHT,
            QColor(226, 82, 92),
            2.0,
            metrics),
    };
    reset_to_base();
    term::terminal_renderer_stats_t arc_color_stats;
    ok &= check(update(arc_color_frame, arc_color_stats),
        "geometry exact descriptor test changes arc color");
    ok &= check(arc_changed(arc_color_stats),
        "arc color participates in exact row slot comparison");

    term::Terminal_render_frame arc_shape_frame = base_frame;
    arc_shape_frame.graphic_arcs = {
        make_direct_row_arc(
            1,
            6,
            term::Terminal_render_arc_kind::UP_LEFT,
            QColor(224, 206, 74),
            3.0,
            metrics),
    };
    reset_to_base();
    term::terminal_renderer_stats_t arc_shape_stats;
    ok &= check(update(arc_shape_frame, arc_shape_stats),
        "geometry exact descriptor test changes arc kind and stroke");
    ok &= check(arc_changed(arc_shape_stats),
        "arc kind and stroke participate in exact row slot comparison");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_rect_row_cache_coalesces_adjacent_simple_rects(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);
    const term::terminal_cell_metrics_t metrics = test_metrics();
    const QColor merged_color(54, 176, 112);
    const QColor split_color(74, 126, 212);
    const term::Terminal_render_rect row_zero_left =
        make_direct_row_rect(0, 1, 2, merged_color, metrics);
    const term::Terminal_render_rect row_zero_right =
        make_direct_row_rect(0, 3, 3, merged_color, metrics);
    const term::Terminal_render_rect row_one_left =
        make_direct_row_rect(1, 1, 2, merged_color, metrics);
    const term::Terminal_render_rect row_one_right =
        make_direct_row_rect(1, 3, 3, split_color, metrics);

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    term::terminal_renderer_stats_t stats;
    node = renderer.update_node(
        node,
        &window,
        make_direct_graphic_row_cache_frame(
            { row_zero_left, row_zero_right, row_one_left, row_one_right },
            {},
            metrics),
        font,
        1.0,
        {},
        stats);

    ok &= check(node != nullptr &&
        stats.paint_completed &&
        stats.text_content_failures == 0,
        "adjacent simple rect coalescing renders a direct graphic frame");
    ok &= check(stats.graphic_rect_rows_rebuilt == 2 &&
        stats.rect_resource_rects_before_coalescing == 4 &&
        stats.rect_resource_rects_after_coalescing == 3,
        "adjacent same-color simple rects coalesce only within matching row runs");
    ok &= check(stats.graphic_batched_rects == 0 &&
        stats.graphic_batched_vertices == 0,
        "direct adjacent graphic rect coalescing leaves batched counters at zero");

    QSGNode* graphic_layer   = child_node_at(node,            2);
    QSGNode* rect_pass_layer = child_node_at(graphic_layer,   0);
    QSGNode* row_layer       = child_node_at(rect_pass_layer, 1);
    const std::vector<QSGTransformNode*> row_wrappers =
        transform_child_nodes(row_layer);
    ok &= check(row_wrappers.size() == 2U,
        "coalesced graphic rect frame keeps one wrapper per row");
    if (row_wrappers.size() == 2U) {
        const std::vector<QRectF> row_zero_rects =
            simple_rect_child_rects(row_wrappers[0]);
        const std::vector<QRectF> row_one_rects =
            simple_rect_child_rects(row_wrappers[1]);
        ok &= check(row_zero_rects.size() == 1U &&
            row_zero_rects[0] == QRectF(
                metrics.width,
                0.0,
                metrics.width * 5.0,
                metrics.height),
            "same-color adjacent row rects become one wider simple rect");
        ok &= check(row_one_rects.size() == 2U,
            "different-color adjacent row rects remain separate simple rects");
    }

    term::terminal_renderer_stats_t split_stats;
    node  = renderer.update_node(
        node,
        &window,
        make_direct_graphic_row_cache_frame(
            {
                make_direct_row_rect(0, 1, 2, merged_color, metrics),
                make_direct_row_rect(0, 4, 2, merged_color, metrics),
            },
            {},
            metrics),
        font,
        1.0,
        {},
        split_stats);
    ok   &= check(split_stats.graphic_rect_rows_rebuilt == 1 &&
        split_stats.graphic_rect_rows_removed == 1 &&
        split_stats.rect_resource_rects_before_coalescing == 2 &&
        split_stats.rect_resource_rects_after_coalescing == 2,
        "gapped same-color rect transition rebuilds the row without coalescing across the gap");
    ok   &= check(split_stats.graphic_batched_rects == 0 &&
        split_stats.graphic_batched_vertices == 0,
        "direct gapped graphic rect transition leaves batched counters at zero");

    graphic_layer   = child_node_at(node,            2);
    rect_pass_layer = child_node_at(graphic_layer,   0);
    row_layer       = child_node_at(rect_pass_layer, 1);
    const std::vector<QSGTransformNode*> split_row_wrappers =
        transform_child_nodes(row_layer);
    ok &= check(split_row_wrappers.size() == 1U,
        "gapped transition removes the stale second row wrapper");
    if (split_row_wrappers.size() == 1U) {
        const std::vector<QRectF> split_row_rects =
            simple_rect_child_rects(split_row_wrappers[0]);
        ok &= check(split_row_rects.size() == 2U &&
            split_row_rects[0] == QRectF(
                metrics.width,
                0.0,
                metrics.width * 2.0,
                metrics.height) &&
            split_row_rects[1] == QRectF(
                metrics.width * 4.0,
                0.0,
                metrics.width * 2.0,
                metrics.height),
            "gapped transition leaves two row-local simple rect nodes");
    }

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_row_cache_empty_dirty_input_key_gate_safety(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const bool software_scene_graph = window_uses_software_scene_graph(window);
    const term::Terminal_render_rect    background_base      = make_direct_row_rect(
        0,
        0,
        3,
        QColor(28, 64, 44),
        metrics);
    const term::Terminal_render_rect background_changed =
        make_direct_row_rect(0, 0, 3, QColor(92, 48, 38), metrics);
    const term::Terminal_render_rect selection_base =
        make_direct_row_rect(1, 2, 3, QColor(54, 96, 154, 190), metrics);
    const term::Terminal_render_rect selection_changed = make_direct_row_rect(
        1,
        2,
        4,
        QColor(74, 116, 174, 190),
        metrics);
    const term::Terminal_render_rect graphic_rect_base =
        make_direct_row_rect(2, 1, 3, QColor(54, 176, 112), metrics);
    const term::Terminal_render_rect graphic_rect_changed = make_direct_row_rect(
        2,
        1,
        3,
        QColor(74, 126, 212),
        metrics);
    const term::Terminal_render_arc graphic_arc_base = make_direct_row_arc(
        1,
        6,
        term::Terminal_render_arc_kind::DOWN_RIGHT,
        QColor(222, 190, 74),
        2.0,
        metrics);
    const term::Terminal_render_arc graphic_arc_changed = make_direct_row_arc(
        1,
        6,
        term::Terminal_render_arc_kind::UP_LEFT,
        QColor(222, 190, 74),
        3.0,
        metrics);

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };

    term::Terminal_render_frame base_frame =
        make_direct_rect_cache_frame({background_base}, {selection_base}, metrics, false);
    base_frame.graphic_rects  = {graphic_rect_base};
    base_frame.graphic_arcs   = {graphic_arc_base};
    ok                       &= check(base_frame.dirty_row_ranges.empty(),
        "empty-dirty input gate safety fixture starts with empty dirty ranges");

    term::terminal_renderer_stats_t base_stats;
    ok &= check(update(base_frame, base_stats),
        "empty-dirty input gate safety test renders the base direct frame");
    ok &= check(base_stats.background_rows_rebuilt == 1 &&
        base_stats.selection_rows_rebuilt == 1 &&
        base_stats.graphic_rect_rows_rebuilt == 1,
        "empty-dirty input gate safety setup builds row cached rect passes");

    term::terminal_renderer_stats_t idle_stats;
    ok &= check(update(base_frame, idle_stats),
        "empty-dirty input gate safety test rerenders an unchanged direct frame");
    ok &= check(!idle_stats.background_layer_rebuilt &&
        !idle_stats.selection_layer_rebuilt &&
        !idle_stats.graphic_layer_rebuilt &&
        idle_stats.background_row_clean_reuse_skips == 1 &&
        idle_stats.selection_row_clean_reuse_skips == 1 &&
        idle_stats.graphic_rect_row_clean_reuse_skips == 1 &&
        idle_stats.graphic_arc_row_clean_reuse_skips == 1,
        "unchanged empty-dirty direct frame skips all converted row-cache passes");

    term::Terminal_render_frame background_frame = base_frame;
    background_frame.background_rects = {background_changed};
    term::terminal_renderer_stats_t background_stats;
    ok &= check(update(background_frame, background_stats),
        "empty-dirty input gate safety test renders a background mutation");
    ok &= check(background_stats.background_layer_rebuilt &&
        background_stats.background_rows_rebuilt == 1 &&
        background_stats.background_row_clean_reuse_skips == 0 &&
        background_stats.selection_row_clean_reuse_skips == 1 &&
        background_stats.graphic_rect_row_clean_reuse_skips == 1 &&
        background_stats.graphic_arc_row_clean_reuse_skips == 1,
        "background input-key change prevents only the background pass from clean-skipping");

    term::Terminal_render_frame selection_frame = background_frame;
    selection_frame.selection_rects = {selection_changed};
    term::terminal_renderer_stats_t selection_stats;
    ok &= check(update(selection_frame, selection_stats),
        "empty-dirty input gate safety test renders a selection mutation");
    ok &= check(selection_stats.selection_layer_rebuilt &&
        selection_stats.selection_rows_rebuilt == 1 &&
        selection_stats.background_row_clean_reuse_skips == 1 &&
        selection_stats.selection_row_clean_reuse_skips == 0 &&
        selection_stats.graphic_rect_row_clean_reuse_skips == 1 &&
        selection_stats.graphic_arc_row_clean_reuse_skips == 1,
        "selection input-key change prevents only the selection pass from clean-skipping");

    term::Terminal_render_frame graphic_rect_frame = selection_frame;
    graphic_rect_frame.graphic_rects = {graphic_rect_changed};
    term::terminal_renderer_stats_t graphic_rect_stats;
    ok &= check(update(graphic_rect_frame, graphic_rect_stats),
        "empty-dirty input gate safety test renders a graphic rect mutation");
    ok &= check(graphic_rect_stats.graphic_layer_rebuilt &&
        graphic_rect_stats.graphic_rect_rows_rebuilt == 1 &&
        graphic_rect_stats.background_row_clean_reuse_skips == 1 &&
        graphic_rect_stats.selection_row_clean_reuse_skips == 1 &&
        graphic_rect_stats.graphic_rect_row_clean_reuse_skips == 0 &&
        graphic_rect_stats.graphic_arc_row_clean_reuse_skips == 1,
        "graphic rect input-key change prevents only the graphic rect pass from clean-skipping");

    term::Terminal_render_frame graphic_arc_frame = graphic_rect_frame;
    graphic_arc_frame.graphic_arcs = {graphic_arc_changed};
    term::terminal_renderer_stats_t graphic_arc_stats;
    ok &= check(update(graphic_arc_frame, graphic_arc_stats),
        "empty-dirty input gate safety test renders a graphic arc mutation");
    if (software_scene_graph) {
        ok &= check(graphic_arc_stats.graphic_layer_rebuilt &&
            graphic_arc_stats.graphic_arc_row_cache_fallbacks == 1 &&
            graphic_arc_stats.background_row_clean_reuse_skips == 1 &&
            graphic_arc_stats.selection_row_clean_reuse_skips == 1 &&
            graphic_arc_stats.graphic_rect_row_clean_reuse_skips == 1 &&
            graphic_arc_stats.graphic_arc_row_clean_reuse_skips == 0,
            "software graphic arc input-key change prevents only the arc pass from clean-skipping");
    }
    else {
        ok &= check(graphic_arc_stats.graphic_layer_rebuilt &&
            graphic_arc_stats.graphic_arc_rows_rebuilt == 1 &&
            graphic_arc_stats.background_row_clean_reuse_skips == 1 &&
            graphic_arc_stats.selection_row_clean_reuse_skips == 1 &&
            graphic_arc_stats.graphic_rect_row_clean_reuse_skips == 1 &&
            graphic_arc_stats.graphic_arc_row_clean_reuse_skips == 0,
            "graphic arc input-key change prevents only the arc pass from clean-skipping");
    }

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_graphic_arc_fallback_pixels_and_order(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(9, 12, 16));
    window.resize(420, 140);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const term::Terminal_render_rect background_rect{
        QRectF(
            0.0,
            0.0,
            metrics.width * 20.0,
            metrics.height * 3.0),
        QColor(9, 12, 16),
    };
    const term::Terminal_render_rect rect_row_one =
        make_direct_row_rect(1, 1, 3, QColor(34, 168, 74), metrics);
    const term::Terminal_render_arc arc_row_one = make_direct_row_arc(
        1,
        1,
        term::Terminal_render_arc_kind::DOWN_RIGHT,
        QColor(230, 42, 48),
        std::max<qreal>(3.0, std::floor(metrics.height * 0.24)),
        metrics);
    const term::Terminal_render_arc non_row_arc{
        term::Terminal_render_arc_kind::DOWN_LEFT,
        QRectF(
            metrics.width * 6.0,
            metrics.height * 0.35,
            metrics.width,
            metrics.height * 1.2),
        QColor(218, 190, 46),
        std::max<qreal>(3.0, std::floor(metrics.height * 0.20)),
    };
    const term::Terminal_render_arc arc_row_two = make_direct_row_arc(
        2,
        8,
        term::Terminal_render_arc_kind::UP_LEFT,
        QColor(80, 150, 226),
        std::max<qreal>(3.0, std::floor(metrics.height * 0.20)),
        metrics);

    term::Terminal_render_frame frame = make_direct_graphic_row_cache_frame(
        { rect_row_one },
        { arc_row_one, non_row_arc, arc_row_two },
        metrics);
    frame.background_rects.push_back(background_rect);

    Direct_render_item item;
    item.setParentItem(window.contentItem());
    item.set_frame(frame);
    window.show();

    const QImage image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            stats.paint_completed                &&
            stats.text_content_failures == 0     &&
            stats.graphic_layer_rebuilt          &&
            stats.graphic_rect_rows_rebuilt == 1 &&
            stats.graphic_arc_row_cache_fallbacks == 1;
    });
    const term::terminal_renderer_stats_t stats = item.last_stats();
    ok &= check(!image.isNull(),
        "graphic arc fallback pixel test renders the first direct frame");
    ok &= check(stats.graphic_arc_rows_rebuilt == 0 &&
        stats.graphic_arc_rows_reused == 0,
        "interleaved row/non-row/row arcs use the flat arc fallback");

    const auto red_arc_pixel = [](QColor color) {
        return color.red() > 120 && color.red() > color.green() + 30;
    };
    const auto green_rect_pixel = [](QColor color) {
        return color.green() > 100 && color.green() > color.red() + 30;
    };

    const QRect overlap_area      = cell_area(1, 1, 1, metrics);
    const QRect rect_visible_area = cell_area(1, 2, 2, metrics);
    const int   red_pixels        = count_matching_pixels(image, overlap_area,      red_arc_pixel);
    const int   green_pixels      = count_matching_pixels(image, rect_visible_area, green_rect_pixel);
    if (green_pixels <= 20) {
        std::cerr << "INFO: observed graphic overlap pixels: red="
            << red_pixels << " green=" << green_pixels << '\n';
    }
    ok &= check(red_pixels > 4,
        "graphic arc fallback paints the arc over an overlapping rect");
    ok &= check(green_pixels > 20,
        "graphic rect remains visible outside the overlapping arc");
    ok &= check(count_matching_pixels(image, cell_area(0, 1, 1, metrics), red_arc_pixel) == 0,
        "graphic arc fallback keeps the row-one arc out of row zero");

    term::Terminal_render_frame rect_only_frame =
        make_direct_graphic_row_cache_frame({rect_row_one}, {}, metrics);
    rect_only_frame.background_rects.push_back(background_rect);
    item.set_frame(rect_only_frame);
    const QImage recovered_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t recovered_stats = item.last_stats();
        return
            recovered_stats.root_reused                             &&
            recovered_stats.paint_completed                         &&
            recovered_stats.graphic_layer_rebuilt                   &&
            recovered_stats.graphic_rect_row_clean_reuse_skips == 1 &&
            recovered_stats.graphic_rect_rows_reused == 0           &&
            recovered_stats.graphic_arc_row_cache_fallbacks == 0;
    });
    ok &= check(!recovered_image.isNull(),
        "graphic arc fallback pixel test renders after removing arcs");
    ok &= check(count_matching_pixels(recovered_image, overlap_area, red_arc_pixel) == 0 &&
        count_matching_pixels(recovered_image, rect_visible_area, green_rect_pixel) > 20,
        "removing fallback arcs leaves no stale flat arc pixels over row-cached rects");

    return ok;
}

bool test_qsg_graphic_rect_row_cache_mixed_fallback(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const term::Terminal_render_rect row_zero =
        make_direct_row_rect(0, 0, 3, QColor(130, 36, 48), metrics);
    const term::Terminal_render_rect row_one =
        make_direct_row_rect(1, 1, 3, QColor(34, 126, 68), metrics);
    const term::Terminal_render_rect non_row_rect{
        QRectF(
            metrics.width * 0.5,
            metrics.height * 0.5,
            metrics.width * 2.0,
            metrics.height * 1.25),
        QColor(38, 54, 144),
    };

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };

    const term::Terminal_render_frame cached_frame =
        make_direct_graphic_row_cache_frame({row_zero, row_one}, {}, metrics);
    term::terminal_renderer_stats_t cached_stats;
    ok &= check(update(cached_frame, cached_stats),
        "mixed graphic rect fallback test builds the initial row cache");
    ok &= check(cached_stats.graphic_rect_rows_rebuilt == 2,
        "mixed graphic rect fallback setup creates two cached graphic rect rows");

    const term::Terminal_render_frame mixed_frame = make_direct_graphic_row_cache_frame(
        { row_zero, non_row_rect, row_one },
        {},
        metrics);
    term::terminal_renderer_stats_t mixed_stats;
    ok &= check(update(mixed_frame, mixed_stats),
        "mixed graphic rect fallback test renders an interleaved direct frame");
    ok &= check(mixed_stats.graphic_layer_rebuilt &&
        mixed_stats.graphic_rect_row_cache_fallbacks == 1 &&
        mixed_stats.graphic_rect_rows_rebuilt == 0 &&
        mixed_stats.graphic_rect_rows_reused == 0 &&
        mixed_stats.graphic_rect_rows_removed == 2,
        "interleaved row/non-row graphic rects fall back to flat rendering and clear row cache");

    QSGNode*                  graphic_layer   = child_node_at(node,            2);
    QSGNode*                  rect_pass_layer = child_node_at(graphic_layer,   0);
    QSGNode*                  frame_layer     = child_node_at(rect_pass_layer, 0);
    QSGNode*                  row_layer       = child_node_at(rect_pass_layer, 1);
    const std::vector<QColor> frame_colors    = simple_rect_child_colors(frame_layer);
    ok                                       &= check(frame_colors.size() == 3 &&
        frame_colors[0].rgba() == row_zero.color.rgba() &&
        frame_colors[1].rgba() == non_row_rect.color.rgba() &&
        frame_colors[2].rgba() == row_one.color.rgba(),
        "flat fallback keeps mixed graphic rect child order");
    ok                                       &= check(row_layer != nullptr && row_layer->firstChild() == nullptr,
        "flat fallback leaves no cached graphic rect row wrappers attached");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_graphic_rect_antialias_frame_route_recovers(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const bool software_scene_graph = window_uses_software_scene_graph(window);
    const term::Terminal_render_rect    row_zero             = make_direct_row_rect(
        0,
        0,
        3,
        QColor(130, 36, 48),
        metrics);
    const term::Terminal_render_rect row_one =
        make_direct_row_rect(1, 1, 3, QColor(34, 126, 68), metrics);
    const term::Terminal_render_rect antialiased_row_zero = make_direct_row_rect(
        0,
        0,
        3,
        QColor(180, 82, 42),
        metrics,
        true);

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };

    const term::Terminal_render_frame cached_frame =
        make_direct_graphic_row_cache_frame({row_zero, row_one}, {}, metrics);
    term::terminal_renderer_stats_t cached_stats;
    ok &= check(update(cached_frame, cached_stats),
        "antialias frame-route recovery test builds the initial rect row cache");
    ok &= check(cached_stats.graphic_rect_rows_rebuilt == 2,
        "antialias frame-route recovery setup creates two cached graphic rect rows");
    if (software_scene_graph) {
        ok &= check(cached_stats.graphic_batched_rects == 0 &&
            cached_stats.graphic_batched_vertices == 0,
            "software antialias recovery setup leaves batched counters at zero");
    }

    const term::Terminal_render_frame antialias_frame =
        make_direct_graphic_row_cache_frame({antialiased_row_zero, row_one}, {}, metrics);
    term::terminal_renderer_stats_t antialias_stats;
    ok &= check(update(antialias_frame, antialias_stats),
        "antialias frame-route recovery test renders an antialiased rect pass");
    ok &= check(antialias_stats.graphic_layer_rebuilt &&
        antialias_stats.graphic_rect_row_cache_fallbacks == 0 &&
        antialias_stats.graphic_rect_rows_rebuilt == 0 &&
        antialias_stats.graphic_rect_rows_reused == 1 &&
        antialias_stats.graphic_rect_rows_removed == 1,
        "antialiased graphic rects stay on the frame route while hard rect rows stay cached");
    if (software_scene_graph) {
        ok &= check(antialias_stats.graphic_batched_rects == 0 &&
            antialias_stats.graphic_batched_vertices == 0,
            "software antialiased graphic rect pass leaves batched counters at zero");
    }

    QSGNode* graphic_layer   = child_node_at(node,            2);
    QSGNode* rect_pass_layer = child_node_at(graphic_layer,   0);
    QSGNode* frame_layer     = child_node_at(rect_pass_layer, 0);
    QSGNode* row_layer       = child_node_at(rect_pass_layer, 1);
    ok &= check(frame_layer != nullptr &&
        frame_layer->firstChild() != nullptr &&
        row_layer != nullptr &&
        child_node_count(row_layer) == 1,
        "antialiased graphic rect pass leaves one hard-rect row wrapper attached");

    term::terminal_renderer_stats_t recovered_stats;
    ok &= check(update(cached_frame, recovered_stats),
        "antialias frame-route recovery test renders cacheable rect rows again");
    ok &= check(recovered_stats.graphic_layer_rebuilt &&
        recovered_stats.graphic_rect_row_cache_fallbacks == 0 &&
        recovered_stats.graphic_rect_rows_rebuilt == 1 &&
        recovered_stats.graphic_rect_rows_reused == 1,
        "hard rect row cache rebuilds only the row recovered from the antialias frame route");
    if (software_scene_graph) {
        ok &= check(recovered_stats.graphic_batched_rects == 0 &&
            recovered_stats.graphic_batched_vertices == 0,
            "software antialias recovery leaves batched counters at zero");
    }

    graphic_layer    = child_node_at(node,            2);
    rect_pass_layer  = child_node_at(graphic_layer,   0);
    frame_layer      = child_node_at(rect_pass_layer, 0);
    row_layer        = child_node_at(rect_pass_layer, 1);
    ok              &= check(frame_layer != nullptr &&
        frame_layer->firstChild() == nullptr &&
        row_layer != nullptr &&
        child_node_count(row_layer) == 2,
        "recovering to hard row-cache rendering removes stale antialiased frame rect nodes");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_rect_layer_cache_key_invariants(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);
    const term::Terminal_render_frame base_frame = make_direct_graphic_cache_key_frame(
        false,
        term::Terminal_render_arc_kind::DOWN_RIGHT,
        2.0);
    const term::Terminal_render_frame antialias_frame = make_direct_graphic_cache_key_frame(
        true,
        term::Terminal_render_arc_kind::DOWN_RIGHT,
        2.0);
    const term::Terminal_render_frame arc_kind_frame = make_direct_graphic_cache_key_frame(
        true,
        term::Terminal_render_arc_kind::UP_LEFT,
        2.0);
    const term::Terminal_render_frame arc_stroke_frame = make_direct_graphic_cache_key_frame(
        true,
        term::Terminal_render_arc_kind::UP_LEFT,
        3.0);

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        qreal device_pixel_ratio,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            device_pixel_ratio,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };

    term::terminal_renderer_stats_t first_stats;
    ok &= check(update(base_frame, 1.0, first_stats),
        "rect-layer cache key test creates the initial direct render node");
    ok &= check(first_stats.graphic_layer_rebuilt,
        "first direct graphic rect/arc frame rebuilds the graphic layer");

    term::terminal_renderer_stats_t reuse_stats;
    ok &= check(update(base_frame, 1.0, reuse_stats),
        "rect-layer cache key test rerenders the same direct frame");
    ok &= check(reuse_stats.root_reused && !reuse_stats.graphic_layer_rebuilt,
        "unchanged direct graphic rect/arc frame reuses the graphic layer");

    term::terminal_renderer_stats_t antialias_stats;
    ok &= check(update(antialias_frame, 1.0, antialias_stats),
        "rect-layer cache key test rerenders after rect antialias changes");
    ok &= check(antialias_stats.graphic_layer_rebuilt,
        "changing direct graphic rect antialias rebuilds the graphic layer");

    term::terminal_renderer_stats_t arc_kind_stats;
    ok &= check(update(arc_kind_frame, 1.0, arc_kind_stats),
        "rect-layer cache key test rerenders after arc kind changes");
    ok &= check(arc_kind_stats.graphic_layer_rebuilt,
        "changing direct graphic arc kind rebuilds the graphic layer");

    term::terminal_renderer_stats_t arc_stroke_stats;
    ok &= check(update(arc_stroke_frame, 1.0, arc_stroke_stats),
        "rect-layer cache key test rerenders after arc stroke changes");
    ok &= check(arc_stroke_stats.graphic_layer_rebuilt,
        "changing direct graphic arc stroke rebuilds the graphic layer");

    term::terminal_renderer_stats_t dpr_stats;
    ok &= check(update(arc_stroke_frame, 2.0, dpr_stats),
        "rect-layer cache key test rerenders after device pixel ratio changes");
    ok &= check(dpr_stats.graphic_layer_rebuilt,
        "device pixel ratio rebuilds direct antialiased graphic rect frame nodes");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_text_rows_clip_to_terminal_row(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(340.0, 70.0));
    surface.set_font_family(QString());
    surface.set_font_size(term::k_vnm_terminal_default_font_pixel_size);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_row_clip_stress_snapshot(310U));
    window.show();

    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        310U,
        1),
        "row clip stress render rebuilds one text resource row");

    const term::terminal_renderer_stats_t stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    ok &= check_no_text_content_failures(stats,
        "row clip stress render has no text content failures");

    const term::terminal_cell_metrics_t metrics = test_metrics_for_font_size(
        term::k_vnm_terminal_default_font_pixel_size);
    const QImage image = window.grabWindow();
    ok &= check(!image.isNull(), "row clip stress render produced an image");
    ok &= check(cell_is_nonblank(image, 1, 0, metrics) &&
        cell_is_nonblank(image, 1, 10, metrics) &&
        cell_is_nonblank(image, 1, 21, metrics),
        "row clip stress render keeps prompt text visible");
    ok &= check(nearly_all_background(image, 2, 0, 30, metrics),
        "row clip stress render does not spill prompt text into the blank row below");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_row_clip_stress_snapshot(311U));
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        311U,
        0),
        "row clip stress rerender reuses the cached text resource row");

    const term::terminal_renderer_stats_t reuse_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    ok &= check(reuse_stats.text_content_reused == 1,
        "row clip stress rerender reports one reused text resource row");
    const QImage reuse_image = window.grabWindow();
    ok &= check(!reuse_image.isNull() &&
        cell_is_nonblank(reuse_image, 1, 10, metrics) &&
        nearly_all_background(reuse_image, 2, 0, 30, metrics),
        "row clip stress rerender keeps reused text clipped to its row");
    return ok;
}

bool test_qsg_clipped_text_run_transition_clears_pixels(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(260, 100);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const auto make_frame = [&](
        QRectF clip_rect,
        std::vector<term::Terminal_render_dirty_row_range> dirty_row_ranges) {
        term::Terminal_render_text_run run = make_direct_text_run_for_row(
            0,
            0,
            0,
            4,
            QStringLiteral("MMMM"),
            metrics);
        run.clip_rect = clip_rect;
        term::Terminal_render_frame frame = make_direct_text_slot_frame(
            term::Terminal_buffer_id::PRIMARY,
            0,
            1,
            { std::move(run) },
            metrics,
            std::move(dirty_row_ranges));
        frame.background_rects.push_back({
            QRectF(QPointF(0.0, 0.0), frame.logical_size),
            QColor(9, 12, 16),
        });
        return frame;
    };

    Direct_render_item item;
    item.setParentItem(window.contentItem());
    item.set_frame(make_frame(QRectF(), {{0, 1}}));
    window.show();

    const QImage baseline_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            item.render_count() > 0          &&
            stats.paint_completed            &&
            stats.text_content_failures == 0 &&
            stats.text_content_rebuilds == 1;
    });
    const term::terminal_renderer_stats_t baseline_stats = item.last_stats();
    ok &= check(!baseline_image.isNull() &&
        baseline_stats.route_qt_text_layout_runs > 0 &&
        cell_is_nonblank(baseline_image, 0, 2, metrics),
        "per-run clip transition baseline renders unclipped text into the later cells");

    const int before_clipped_render_count = item.render_count();
    item.set_frame(make_frame(
        QRectF(0.0, 0.0, metrics.width, metrics.height),
        {{1, 1}}));
    const QImage clipped_image = render_window_until(app, window, [&](const QImage& image) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            item.render_count() > before_clipped_render_count &&
            stats.paint_completed                             &&
            stats.text_content_failures == 0                  &&
            stats.text_content_rebuilds == 1                  &&
            stats.text_resource_descriptor_reuses == 0        &&
            stats.text_cache_entries_replaced == 1            &&
            cell_is_nonblank(image, 0, 0, metrics)            &&
            nearly_all_background(image, 0, 2, 1, metrics);
    });
    const term::terminal_renderer_stats_t clipped_stats = item.last_stats();
    ok &= check(clipped_stats.text_cache_entries_replaced == 1 &&
        clipped_stats.text_clean_reuse_skips == 0 &&
        clipped_stats.route_qt_text_layout_runs > 0 &&
        clipped_stats.qt_text_layout_calls > 0 &&
        clipped_stats.route_fast_text_cells == 0,
        "clean per-run clipped text rebuilds through the Qt text layout route");
    ok &= check(!clipped_image.isNull() &&
        cell_is_nonblank(clipped_image, 0, 0, metrics) &&
        nearly_all_background(clipped_image, 0, 2, 1, metrics),
        "clean per-run clipped text stays visible inside the clip and clears outside pixels");
    return ok;
}

bool test_qsg_text_resource_removal_clears_pixels_across_frames(QGuiApplication& app)
{
    constexpr int k_visible_rows = 3;

    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(340.0, 90.0));
    configure_test_font(surface);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_multirow_dense_same_color_snapshot(k_visible_rows, 20, 950U));
    Renderer_lifecycle_recorder_ptr lifecycle_recorder = term::VNM_TerminalSurface_render_bridge::lifecycle_recorder(
        surface);
    window.show();

    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        950U,
        k_visible_rows),
        "text removal pixel test builds every visible text resource row");
    ok &= check_surface_no_text_content_failures(surface,
        "text removal pixel setup has no text content failures");

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const QImage setup_image = window.grabWindow();
    ok &= check(!setup_image.isNull() &&
        cell_is_nonblank(setup_image, 0, 0, metrics) &&
        cell_is_nonblank(setup_image, 1, 0, metrics) &&
        cell_is_nonblank(setup_image, 2, 0, metrics),
        "text removal pixel setup renders text in every visible row");
    const term::terminal_renderer_lifecycle_stats_t setup_lifecycle_stats =
        lifecycle_recorder->snapshot();
    ok &= check(live_text_resource_count(setup_lifecycle_stats) >= k_visible_rows,
        "text removal pixel setup records live text resources");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_blank_text_lifecycle_snapshot(951U));
    const QImage blank_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            invalidation_stats.last_rendered_snapshot_sequence == 951U &&
            !invalidation_stats.pending_update                         &&
            stats.paint_completed                                      &&
            stats.text_content_failures == 0                           &&
            stats.text_content_rebuilds == 0                           &&
            stats.text_content_removed == k_visible_rows;
    });
    const term::terminal_renderer_lifecycle_stats_t blank_lifecycle_stats =
        lifecycle_recorder->snapshot();
    ok &= check(!blank_image.isNull(), "text removal pixel test renders the blank frame");
    ok &= check(nearly_all_background(blank_image, 0, 0, 20, metrics) &&
        nearly_all_background(blank_image, 1, 0, 20, metrics) &&
        nearly_all_background(blank_image, 2, 0, 20, metrics),
        "removing all text rows clears stale QSG text pixels");
    ok &= check(live_text_resource_count(blank_lifecycle_stats) == 0U,
        "removing all text rows destroys live text resources");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_blank_text_lifecycle_snapshot(952U));
    const QImage consecutive_blank_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            invalidation_stats.last_rendered_snapshot_sequence == 952U &&
            !invalidation_stats.pending_update                         &&
            stats.paint_completed                                      &&
            stats.text_content_failures == 0                           &&
            stats.text_content_rebuilds == 0                           &&
            stats.text_content_removed == 0;
    });
    const term::terminal_renderer_lifecycle_stats_t consecutive_lifecycle_stats =
        lifecycle_recorder->snapshot();
    ok &= check(!consecutive_blank_image.isNull(),
        "text removal pixel test renders a consecutive blank frame");
    ok &= check(nearly_all_background(consecutive_blank_image, 0, 0, 20, metrics) &&
        nearly_all_background(consecutive_blank_image, 1, 0, 20, metrics) &&
        nearly_all_background(consecutive_blank_image, 2, 0, 20, metrics),
        "consecutive blank frame keeps removed QSG text pixels absent");
    ok &= check(live_text_resource_count(consecutive_lifecycle_stats) == 0U,
        "consecutive blank frame leaves no live text resources");
    return ok;
}

bool test_qsg_viewport_scroll_reuses_text_resources(QGuiApplication& app)
{
    constexpr int visible_rows = 5;

    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 160);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(340.0, 120.0));
    configure_test_font(surface);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_viewport_scroll_dense_snapshot(920U, 7));
    window.show();

    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        920U,
        visible_rows),
        "dense viewport-scroll baseline builds every visible text row");
    ok &= check_surface_no_text_content_failures(surface,
        "dense viewport-scroll baseline has no text content failures");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_viewport_scroll_dense_snapshot(921U, 6));
    const QImage scroll_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t render_stats =
            term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
        return
            invalidation_stats.last_rendered_snapshot_sequence == 921U &&
            !invalidation_stats.pending_update                         &&
            render_stats.root_reused                                   &&
            render_stats.paint_completed                               &&
            render_stats.text_content_failures == 0                    &&
            render_stats.text_content_rebuilds == 1                    &&
            render_stats.text_content_reused >= visible_rows - 1;
    });
    const term::terminal_renderer_stats_t scroll_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    ok &= check(!scroll_image.isNull(), "dense one-line viewport scroll renders an image");
    ok &= check(scroll_stats.text_content_rebuilds == 1,
        "dense one-line viewport scroll rebuilds only the exposed text row");
    ok &= check(scroll_stats.text_content_reused >= visible_rows - 1,
        "dense one-line viewport scroll reuses overlapping text resources");
    ok &= check(scroll_stats.text_content_removed == 1,
        "dense one-line viewport scroll removes the row that left the viewport");
    ok &= check(scroll_stats.text_wrapper_order_rebuilt,
        "dense one-line viewport scroll resyncs wrapper order for the exposed row");
    ok &= check_no_text_content_failures(scroll_stats,
        "dense one-line viewport scroll has no text content failures");
    const term::terminal_cell_metrics_t metrics = test_metrics();
    ok &= check(count_matching_pixels(
        scroll_image,
        cell_area(0, 0, 20, metrics),
        [](QColor color) {
            return is_viewport_scroll_row_color(color, 6);
        }) > 10,
        "dense one-line viewport scroll renders exposed logical row at viewport row zero");
    ok &= check(count_matching_pixels(
        scroll_image,
        cell_area(1, 0, 20, metrics),
        [](QColor color) {
            return is_viewport_scroll_row_color(color, 7);
        }) > 10,
        "dense one-line viewport scroll moves reused logical row to viewport row one");
    return ok;
}

bool test_qsg_text_resource_descriptor_dirty_reuse(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(260, 100);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);
    const term::terminal_cell_metrics_t metrics = test_metrics();
    const auto make_frame = [&](QString first, QString second) {
        return make_direct_text_slot_frame(
            term::Terminal_buffer_id::PRIMARY,
            0,
            1,
            {
                make_direct_text_run_for_row(0, 0, 0, 1, first, metrics),
                make_direct_text_run_for_row(0, 0, 1, 1, second, metrics),
            },
            metrics,
            {{0, 1}});
    };

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };

    const term::Terminal_render_frame base_frame =
        make_frame(QStringLiteral("A"), QStringLiteral("B"));
    term::terminal_renderer_stats_t first_stats;
    ok &= check(update(base_frame, first_stats),
        "text resource descriptor baseline renders");
    ok &= check(first_stats.text_content_rebuilds == 1 &&
        first_stats.text_cache_entries_created == 1,
        "text resource descriptor baseline builds one text row");

    term::terminal_renderer_stats_t reuse_stats;
    ok &= check(update(base_frame, reuse_stats),
        "text resource descriptor dirty unchanged frame renders");
    ok &= check(reuse_stats.text_content_rebuilds == 0 &&
        reuse_stats.text_content_reused == 1 &&
        reuse_stats.text_resource_descriptor_reuses == 1 &&
        reuse_stats.qt_text_layout_calls == 0 &&
        reuse_stats.text_leaf_nodes_created == 0 &&
        reuse_stats.text_cache_entries_replaced == 0 &&
        reuse_stats.qsg_nodes_replaced == 0 &&
        reuse_stats.text_key_builds == 1 &&
        reuse_stats.text_key_bytes < first_stats.text_key_bytes &&
        reuse_stats.route_fast_text_cells == 0,
        "dirty unchanged text resource row reuses before key/layout/node rebuild work");

    term::terminal_renderer_stats_t changed_stats;
    ok &= check(update(make_frame(QStringLiteral("C"), QStringLiteral("B")), changed_stats),
        "text resource descriptor changed frame renders");
    ok &= check(changed_stats.text_content_rebuilds == 1 &&
        changed_stats.text_content_reused == 0 &&
        changed_stats.text_resource_descriptor_reuses == 0 &&
        changed_stats.text_cache_entries_replaced == 1 &&
        changed_stats.qt_text_layout_calls > 0 &&
        changed_stats.text_leaf_nodes_created > 0 &&
        changed_stats.route_fast_text_cells == 0,
        "changed text resource descriptor row rebuilds instead of stale-reusing");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_text_resource_descriptor_styled_ascii_reuse(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(260, 100);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);
    const term::terminal_cell_metrics_t metrics = test_metrics();
    const auto make_frame = [&](QColor foreground) {
        term::Terminal_render_text_run first =
            make_direct_text_run_for_row(0, 0, 0, 2, QStringLiteral("AB"), metrics);
        first.style_id   = 1U;
        first.background = QColor(30, 42, 54);

        term::Terminal_render_text_run second = make_direct_text_run_for_row(
            0,
            0,
            2,
            2,
            QStringLiteral("CD"),
            metrics,
            foreground);
        second.style_id   = 2U;
        second.background = QColor(46, 36, 28);

        return make_direct_text_slot_frame(
            term::Terminal_buffer_id::PRIMARY,
            0,
            1,
            { first, second },
            metrics,
            {{0, 1}});
    };

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };

    const term::Terminal_render_frame base_frame = make_frame(QColor(196, 230, 201));
    term::terminal_renderer_stats_t first_stats;
    ok &= check(update(base_frame, first_stats),
        "styled ASCII descriptor baseline renders");
    ok &= check(first_stats.text_content_rebuilds == 1,
        "styled ASCII descriptor baseline builds one text resource row");

    term::terminal_renderer_stats_t reuse_stats;
    ok &= check(update(base_frame, reuse_stats),
        "styled ASCII descriptor dirty unchanged frame renders");
    ok &= check(reuse_stats.text_content_rebuilds == 0 &&
        reuse_stats.text_content_reused == 1 &&
        reuse_stats.text_resource_descriptor_reuses == 1 &&
        reuse_stats.text_key_builds == 1 &&
        reuse_stats.qt_text_layout_calls == 0 &&
        reuse_stats.text_leaf_nodes_created == 0 &&
        reuse_stats.text_cache_entries_replaced == 0 &&
        reuse_stats.qsg_nodes_created == 0 &&
        reuse_stats.qsg_nodes_replaced == 0 &&
        reuse_stats.qsg_nodes_destroyed == 0 &&
        reuse_stats.route_fast_text_cells == 0,
        "dirty unchanged styled ASCII row reuses before key/layout/node rebuild work");

    term::terminal_renderer_stats_t foreground_stats;
    ok &= check(update(make_frame(QColor(230, 110, 90)), foreground_stats),
        "styled ASCII descriptor foreground change renders");
    ok &= check(foreground_stats.text_content_rebuilds == 1 &&
        foreground_stats.text_content_reused == 0 &&
        foreground_stats.text_resource_descriptor_reuses == 0 &&
        foreground_stats.text_cache_entries_replaced == 1 &&
        foreground_stats.qt_text_layout_calls > 0 &&
        foreground_stats.text_leaf_nodes_created > 0 &&
        foreground_stats.route_fast_text_cells == 0,
        "foreground color change rebuilds the styled text resource row");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_text_resource_descriptor_complex_transitions(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(260, 100);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);
    const term::terminal_cell_metrics_t metrics = test_metrics();
    const auto make_frame = [&](QString text, int cell_count) {
        return make_direct_text_slot_frame(
            term::Terminal_buffer_id::PRIMARY,
            0,
            1,
            {
                make_direct_text_run_for_row(0, 0, 0, cell_count, text, metrics),
            },
            metrics,
            {{0, 1}});
    };

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };

    const term::Terminal_render_frame simple_frame =
        make_frame(QStringLiteral("A"), 1);
    const term::Terminal_render_frame complex_frame =
        make_frame(QStringLiteral("\u00e9"), 1);

    term::terminal_renderer_stats_t simple_stats;
    ok &= check(update(simple_frame, simple_stats),
        "simple-to-complex descriptor baseline renders");
    ok &= check(simple_stats.text_content_rebuilds == 1,
        "simple-to-complex descriptor baseline builds the simple row");

    term::terminal_renderer_stats_t complex_stats;
    ok &= check(update(complex_frame, complex_stats),
        "simple-to-complex descriptor complex frame renders");
    ok &= check(complex_stats.text_content_rebuilds == 1 &&
        complex_stats.text_content_reused == 0 &&
        complex_stats.text_resource_descriptor_reuses == 0 &&
        complex_stats.text_cache_entries_replaced == 1 &&
        complex_stats.route_fast_text_cells == 0,
        "simple-to-complex transition rebuilds on text descriptor mismatch");

    term::terminal_renderer_stats_t simple_again_stats;
    ok &= check(update(simple_frame, simple_again_stats),
        "complex-to-simple descriptor simple frame renders");
    ok &= check(simple_again_stats.text_content_rebuilds == 1 &&
        simple_again_stats.text_content_reused == 0 &&
        simple_again_stats.text_resource_descriptor_reuses == 0 &&
        simple_again_stats.text_cache_entries_replaced == 1 &&
        simple_again_stats.route_fast_text_cells == 0,
        "complex-to-simple transition rebuilds on text descriptor mismatch");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_text_resource_descriptor_keeps_cursor_text_separate(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(260, 100);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);
    const term::terminal_cell_metrics_t metrics = test_metrics();
    auto make_frame = [&]() {
        term::Terminal_render_frame frame = make_direct_text_slot_frame(
            term::Terminal_buffer_id::PRIMARY,
            0,
            1,
            {
                make_direct_text_run_for_row(
                    0,
                    0,
                    0,
                    1,
                    QStringLiteral("A"),
                    metrics),
            },
            metrics,
            {{0, 1}});
        frame.cursors.push_back({
            term::Terminal_cursor_shape::BLOCK,
            frame.text_runs.front().rect,
            QColor(255, 255, 255),
        });
        frame.cursor_text_runs.push_back(frame.text_runs.front());
        frame.cursor_text_runs.back().foreground = frame.cursor_text_runs.back().background;
        frame.cursor_text_runs.back().clip_rect = frame.cursors.front().rect;
        return frame;
    };

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };

    const term::Terminal_render_frame frame = make_frame();
    term::terminal_renderer_stats_t first_stats;
    ok &= check(update(frame, first_stats),
        "cursor text descriptor boundary baseline renders");
    ok &= check(first_stats.text_content_rebuilds == 1 &&
        first_stats.cursor_text_layer_rebuilt,
        "cursor text descriptor boundary baseline builds main and cursor text");

    term::terminal_renderer_stats_t reuse_stats;
    ok &= check(update(frame, reuse_stats),
        "cursor text descriptor boundary dirty unchanged frame renders");
    ok &= check(reuse_stats.text_content_rebuilds == 0 &&
        reuse_stats.text_content_reused == 1 &&
        reuse_stats.text_resource_descriptor_reuses == 0 &&
        reuse_stats.qt_text_layout_calls == 0 &&
        reuse_stats.text_key_builds == 3 &&
        !reuse_stats.cursor_text_layer_rebuilt &&
        reuse_stats.route_fast_text_cells == 0,
        "cursor-covered text stays on the existing text-key and cursor-text paths");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_text_resource_descriptor_rejects_preedit_and_clipped_rows(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(260, 100);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);
    const term::terminal_cell_metrics_t metrics = test_metrics();
    const term::Terminal_render_frame base_frame = make_direct_text_slot_frame(
        term::Terminal_buffer_id::PRIMARY,
        0,
        1,
        {
            make_direct_text_run_for_row(0, 0, 0, 1, QStringLiteral("A"), metrics),
        },
        metrics,
        {{0, 1}});

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };
    const auto reset_to_base = [&]() {
        term::terminal_renderer_stats_t stats;
        ok &= check(update(base_frame, stats),
            "descriptor ineligibility test resets the base frame");
    };

    term::terminal_renderer_stats_t first_stats;
    ok &= check(update(base_frame, first_stats),
        "descriptor ineligibility baseline renders");
    ok &= check(first_stats.text_content_rebuilds == 1,
        "descriptor ineligibility baseline builds one text resource");

    term::Terminal_render_frame preedit_frame = base_frame;
    preedit_frame.decorations.push_back({
        term::Terminal_render_decoration_kind::PREEDIT_CARET,
        QRectF(
            metrics.width,
            0.0,
            std::max<qreal>(1.0, std::floor(metrics.width * 0.12)),
            metrics.height),
        QColor(196, 230, 201),
    });
    term::terminal_renderer_stats_t preedit_stats;
    ok &= check(update(preedit_frame, preedit_stats),
        "descriptor ineligibility preedit frame renders");
    ok &= check(preedit_stats.text_content_rebuilds == 0 &&
        preedit_stats.text_content_reused == 1 &&
        preedit_stats.text_resource_descriptor_reuses == 0 &&
        preedit_stats.text_key_builds > 1 &&
        preedit_stats.route_fast_text_cells == 0,
        "preedit rows stay descriptor-ineligible and fall back to key-match reuse");

    reset_to_base();

    term::Terminal_render_frame clipped_frame = base_frame;
    clipped_frame.text_runs.front().clip_rect = QRectF(
        0.0,
        0.0,
        metrics.width * 0.5,
        metrics.height);
    term::terminal_renderer_stats_t clipped_stats;
    ok &= check(update(clipped_frame, clipped_stats),
        "descriptor ineligibility clipped frame renders");
    ok &= check(clipped_stats.text_content_rebuilds == 1 &&
        clipped_stats.text_content_reused == 0 &&
        clipped_stats.text_resource_descriptor_reuses == 0 &&
        clipped_stats.text_cache_entries_replaced == 1 &&
        clipped_stats.text_key_builds > 1 &&
        clipped_stats.route_qt_text_layout_runs > 0 &&
        clipped_stats.qt_text_layout_calls > 0 &&
        clipped_stats.route_fast_text_cells == 0,
        "clipped rows stay descriptor-ineligible and rebuild through the full key path");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_text_row_slot_active_buffer_identity(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(260, 100);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);
    const term::terminal_cell_metrics_t metrics = test_metrics();
    const auto make_frame = [&](term::Terminal_buffer_id active_buffer) {
        return make_direct_text_slot_frame(
            active_buffer,
            0,
            1,
            {
                make_direct_text_run_for_row(
                    0,
                    0,
                    0,
                    4,
                    QStringLiteral("SAME"),
                    metrics),
            },
            metrics,
            {{0, 1}});
    };

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    term::terminal_renderer_stats_t primary_stats;
    node = renderer.update_node(
        node,
        &window,
        make_frame(term::Terminal_buffer_id::PRIMARY),
        font,
        1.0,
        {},
        primary_stats);
    ok &= check(node != nullptr &&
        primary_stats.paint_completed &&
        primary_stats.text_content_failures == 0 &&
        primary_stats.text_content_rebuilds == 1 &&
        primary_stats.text_wrapper_order_rebuilt,
        "text row slot active-buffer baseline builds the primary text resource");

    term::terminal_renderer_stats_t alternate_stats;
    node = renderer.update_node(
        node,
        &window,
        make_frame(term::Terminal_buffer_id::ALTERNATE),
        font,
        1.0,
        {},
        alternate_stats);
    ok &= check(node != nullptr &&
        alternate_stats.paint_completed &&
        alternate_stats.root_reused &&
        alternate_stats.text_content_failures == 0 &&
        alternate_stats.text_content_rebuilds == 1 &&
        alternate_stats.text_content_reused == 0 &&
        alternate_stats.text_content_removed == 1 &&
        alternate_stats.text_cache_entries_created == 1 &&
        alternate_stats.text_cache_entries_removed == 1 &&
        alternate_stats.text_wrapper_order_rebuilt,
        "same-content alternate-buffer text row rebuilds by active-buffer identity");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_text_row_slot_active_buffer_roundtrip_pixels(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(260, 100);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const auto make_frame = [&](
        term::Terminal_buffer_id active_buffer,
        QString text,
        QColor foreground) {
        term::Terminal_render_frame frame = make_direct_text_slot_frame(
            active_buffer,
            0,
            1,
            {
                make_direct_text_run_for_row(
                    0,
                    0,
                    0,
                    4,
                    text,
                    metrics,
                    foreground),
            },
            metrics,
            {{0, 1}});
        frame.background_rects.push_back({
            QRectF(QPointF(0.0, 0.0), frame.logical_size),
            QColor(9, 12, 16),
        });
        return frame;
    };
    const auto count_colored_pixels = [&](
        const QImage& image,
        const std::function<bool(QColor)>& matches) {
        return count_matching_pixels(image, cell_area(0, 0, 4, metrics), matches);
    };
    const auto red_text_pixel = [](QColor color) {
        return
            color.red() > 120                &&
            color.red() > color.green() + 40 &&
            color.red() > color.blue() + 40;
    };
    const auto blue_text_pixel = [](QColor color) {
        return
            color.blue() > 120              &&
            color.blue() > color.red() + 40 &&
            color.blue() > color.green() + 40;
    };
    const auto green_text_pixel = [](QColor color) {
        return
            color.green() > 120              &&
            color.green() > color.red() + 40 &&
            color.green() > color.blue() + 40;
    };

    Direct_render_item item;
    item.setParentItem(window.contentItem());
    item.set_frame(make_frame(
        term::Terminal_buffer_id::PRIMARY,
        QStringLiteral("PPPP"),
        QColor(230, 70, 70)));
    window.show();

    const QImage primary_image = render_window_until(app, window, [&](const QImage& image) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            item.render_count() > 0          &&
            stats.paint_completed            &&
            stats.text_content_failures == 0 &&
            stats.text_content_rebuilds == 1 &&
            count_colored_pixels(image, red_text_pixel) > 5;
    });
    ok &= check(!primary_image.isNull(),
        "active-buffer roundtrip baseline renders primary text pixels");

    const int before_alternate_render_count = item.render_count();
    item.set_frame(make_frame(
        term::Terminal_buffer_id::ALTERNATE,
        QStringLiteral("AAAA"),
        QColor(70, 110, 240)));
    const QImage alternate_image = render_window_until(app, window, [&](const QImage& image) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            item.render_count() > before_alternate_render_count &&
            stats.paint_completed                               &&
            stats.text_content_failures == 0                    &&
            stats.text_content_rebuilds == 1                    &&
            stats.text_content_removed == 1                     &&
            count_colored_pixels(image, blue_text_pixel) > 5;
    });
    ok &= check(!alternate_image.isNull(),
        "active-buffer roundtrip renders alternate text pixels");

    const int before_primary_return_render_count = item.render_count();
    item.set_frame(make_frame(
        term::Terminal_buffer_id::PRIMARY,
        QStringLiteral("PPPP"),
        QColor(80, 230, 110)));
    const QImage primary_return_image = render_window_until(
        app,
        window,
        [&](const QImage& image) {
            const term::terminal_renderer_stats_t stats = item.last_stats();

            const int green_pixels = count_colored_pixels(image, green_text_pixel);
            const int blue_pixels  = count_colored_pixels(image, blue_text_pixel);
            return
                item.render_count() > before_primary_return_render_count &&
                stats.paint_completed                                    &&
                stats.text_content_failures == 0                         &&
                stats.text_content_rebuilds == 1                         &&
                stats.text_content_removed == 1                          &&
                green_pixels > 5                                         &&
                green_pixels > blue_pixels + 5;
        });
    const int return_green_pixels =
        count_colored_pixels(primary_return_image, green_text_pixel);
    const int return_blue_pixels =
        count_colored_pixels(primary_return_image, blue_text_pixel);
    ok &= check(!primary_return_image.isNull() &&
        return_green_pixels > 5 &&
        return_green_pixels > return_blue_pixels + 5,
        "active-buffer roundtrip clears alternate pixels after returning to primary");
    return ok;
}

bool test_qsg_text_row_slot_viewport_order_and_resize_cleanup(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(260, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);
    const term::terminal_cell_metrics_t metrics = test_metrics();
    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };
    const auto text_wrapper_tops = [&]() {
        std::vector<qreal> tops;
        QSGNode* text_layer = child_node_at(node, 3);
        for (QSGTransformNode* wrapper : transform_child_nodes(text_layer)) {
            tops.push_back(wrapper->matrix().map(QPointF(0.0, 0.0)).y());
        }
        return tops;
    };

    const term::Terminal_render_frame baseline_frame = make_direct_text_slot_rows_frame(
        term::Terminal_buffer_id::PRIMARY,
        4,
        3,
        metrics,
        {{0, 3}});
    term::terminal_renderer_stats_t baseline_stats;
    ok &= check(update(baseline_frame, baseline_stats),
        "text row slot viewport-order baseline renders");
    ok &= check(baseline_stats.text_content_rebuilds == 3 &&
        baseline_stats.text_wrapper_order_rebuilt,
        "text row slot viewport-order baseline builds three rows");

    const term::Terminal_render_frame scrolled_frame = make_direct_text_slot_rows_frame(
        term::Terminal_buffer_id::PRIMARY,
        3,
        3,
        metrics,
        {{0, 3}});
    term::terminal_renderer_stats_t scrolled_stats;
    ok &= check(update(scrolled_frame, scrolled_stats),
        "text row slot viewport-order scroll renders");
    const std::vector<qreal> scrolled_tops = text_wrapper_tops();
    ok &= check(scrolled_stats.text_content_rebuilds == 1 &&
        scrolled_stats.text_content_reused == 2 &&
        scrolled_stats.text_content_removed == 1 &&
        scrolled_stats.text_resource_descriptor_reuses == 2 &&
        scrolled_stats.qt_text_layout_calls == 1 &&
        scrolled_stats.text_leaf_nodes_created == 1 &&
        scrolled_stats.text_key_builds == 2 &&
        scrolled_stats.text_wrapper_order_rebuilt,
        "one-line text viewport movement descriptor-reuses overlapping logical rows");
    ok &= check(scrolled_tops.size() == 3U &&
        nearly_equal(scrolled_tops[0], 0.0) &&
        nearly_equal(scrolled_tops[1], metrics.height) &&
        nearly_equal(scrolled_tops[2], metrics.height * 2.0),
        "text row slot wrappers are reparented in viewport-row order after scroll");

    const term::Terminal_render_frame shrunk_frame = make_direct_text_slot_rows_frame(
        term::Terminal_buffer_id::PRIMARY,
        3,
        2,
        metrics,
        {{0, 2}});
    term::terminal_renderer_stats_t shrunk_stats;
    ok &= check(update(shrunk_frame, shrunk_stats),
        "text row slot viewport-order shrink renders");
    const std::vector<qreal> shrunk_tops = text_wrapper_tops();
    ok &= check(shrunk_stats.text_content_rebuilds == 2 &&
        shrunk_stats.text_content_reused == 0 &&
        shrunk_stats.text_content_removed == 1 &&
        shrunk_stats.text_wrapper_order_rebuilt &&
        shrunk_tops.size() == 2U,
        "shrinking visible text rows rebuilds resized live slots and destroys the stale row slot");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_text_row_slot_dirty_clean_skip_safety(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(260, 100);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);
    const term::terminal_cell_metrics_t metrics = test_metrics();
    const auto make_runs = [&](QString row_zero_text, QString row_one_text) {
        return std::vector<term::Terminal_render_text_run>{
            make_direct_text_run_for_row(0, 0, 0, 1, row_zero_text, metrics),
            make_direct_text_run_for_row(1, 1, 0, 1, row_one_text, metrics),
        };
    };

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };

    term::terminal_renderer_stats_t baseline_stats;
    ok &= check(update(
        make_direct_text_slot_frame(
            term::Terminal_buffer_id::PRIMARY,
            0,
            2,
            make_runs(QStringLiteral("A"), QStringLiteral("B")),
            metrics,
            {{0, 2}}),
        baseline_stats),
        "text dirty clean-skip baseline renders");

    term::terminal_renderer_stats_t clean_skip_stats;
    ok &= check(update(
        make_direct_text_slot_frame(
            term::Terminal_buffer_id::PRIMARY,
            0,
            2,
            make_runs(QStringLiteral("A"), QStringLiteral("B")),
            metrics,
            {{1, 1}}),
        clean_skip_stats),
        "text dirty clean-skip unchanged clean row renders");
    ok &= check(clean_skip_stats.text_content_rebuilds == 0 &&
        clean_skip_stats.text_content_reused == 2 &&
        clean_skip_stats.text_clean_reuse_skips == 1 &&
        clean_skip_stats.text_resource_descriptor_reuses == 1 &&
        clean_skip_stats.text_key_builds == 1 &&
        !clean_skip_stats.text_wrapper_order_rebuilt,
        "dirty text update skips the clean row and descriptor-reuses unchanged dirty text");

    term::Terminal_render_frame cursor_clean_frame = make_direct_text_slot_frame(
        term::Terminal_buffer_id::PRIMARY,
        0,
        2,
        make_runs(QStringLiteral("A"), QStringLiteral("B")),
        metrics,
        {{1, 1}});
    cursor_clean_frame.cursors.push_back({
        term::Terminal_cursor_shape::BLOCK,
        cursor_clean_frame.text_runs.front().rect,
        QColor(255, 255, 255),
    });
    cursor_clean_frame.cursor_text_runs.push_back(cursor_clean_frame.text_runs.front());
    cursor_clean_frame.cursor_text_runs.back().foreground =
        cursor_clean_frame.cursor_text_runs.back().background;
    cursor_clean_frame.cursor_text_runs.back().clip_rect =
        cursor_clean_frame.cursors.front().rect;
    term::terminal_renderer_stats_t cursor_clean_stats;
    ok &= check(update(cursor_clean_frame, cursor_clean_stats),
        "text dirty clean-skip cursor row renders");
    ok &= check(cursor_clean_stats.text_content_rebuilds == 0 &&
        cursor_clean_stats.text_content_reused == 2 &&
        cursor_clean_stats.text_clean_reuse_skips == 0 &&
        cursor_clean_stats.text_resource_descriptor_reuses == 1 &&
        cursor_clean_stats.text_key_builds > 1 &&
        !cursor_clean_stats.text_wrapper_order_rebuilt,
        "clean cursor-text row avoids clean-skip and reuses through the key path");

    term::terminal_renderer_stats_t empty_dirty_change_stats;
    ok &= check(update(
        make_direct_text_slot_frame(
            term::Terminal_buffer_id::PRIMARY,
            0,
            2,
            make_runs(QStringLiteral("C"), QStringLiteral("B")),
            metrics,
            {}),
        empty_dirty_change_stats),
        "text empty-dirty content change renders");
    ok &= check(empty_dirty_change_stats.text_content_rebuilds == 1 &&
        empty_dirty_change_stats.text_content_reused == 1 &&
        empty_dirty_change_stats.text_clean_reuse_skips == 0 &&
        empty_dirty_change_stats.text_resource_descriptor_reuses == 1 &&
        empty_dirty_change_stats.text_key_builds == 2,
        "empty dirty ranges rebuild changed text and descriptor-reuse unchanged text");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_text_row_slot_exact_content_descriptors(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(260, 100);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);
    const term::terminal_cell_metrics_t metrics = test_metrics();
    const auto make_frame = [&](term::Terminal_render_text_run run) {
        return make_direct_text_slot_frame(
            term::Terminal_buffer_id::PRIMARY,
            0,
            1,
            { std::move(run) },
            metrics,
            {{0, 1}});
    };
    const term::Terminal_render_text_run base_run = make_direct_text_run_for_row(
        0,
        0,
        0,
        1,
        QStringLiteral("A"),
        metrics);
    const term::Terminal_render_frame base_frame = make_frame(base_run);

    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            {},
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };
    const auto reset_to_base = [&]() {
        term::terminal_renderer_stats_t stats;
        ok &= check(update(base_frame, stats),
            "text exact descriptor test resets the base frame");
    };
    const auto descriptor_rebuilt =
        [](const term::terminal_renderer_stats_t& stats) {
            return
                stats.text_content_rebuilds           == 1 &&
                stats.text_cache_entries_replaced     == 1 &&
                stats.text_content_reused             == 0 &&
                stats.text_resource_descriptor_reuses == 0;
        };

    term::terminal_renderer_stats_t first_stats;
    ok &= check(update(base_frame, first_stats),
        "text exact descriptor test creates the base row slot");
    ok &= check(first_stats.text_content_rebuilds == 1,
        "text exact descriptor base frame builds one text resource");

    term::terminal_renderer_stats_t reuse_stats;
    ok &= check(update(base_frame, reuse_stats),
        "text exact descriptor test rerenders unchanged base frame");
    ok &= check(reuse_stats.text_content_rebuilds == 0 &&
        reuse_stats.text_content_reused == 1 &&
        reuse_stats.text_resource_descriptor_reuses == 1,
        "unchanged text exact descriptor reuses the row resource");

    term::Terminal_render_frame mixed_frame = make_direct_text_slot_frame(
        term::Terminal_buffer_id::PRIMARY,
        0,
        1,
        {
            make_direct_text_run_for_row(0, 0, 0, 1, QStringLiteral("A"), metrics),
            make_direct_text_run_for_row(0, 0, 1, 1, QStringLiteral("\u00e9"), metrics),
            make_direct_text_run_for_row(0, 0, 2, 1, QStringLiteral("B"), metrics),
        },
        metrics,
        {{0, 1}});
    term::terminal_renderer_stats_t mixed_baseline_stats;
    ok &= check(update(mixed_frame, mixed_baseline_stats),
        "text exact descriptor test builds mixed ASCII and non-ASCII row");

    term::terminal_renderer_stats_t mixed_reuse_stats;
    ok &= check(update(mixed_frame, mixed_reuse_stats),
        "text exact descriptor test rerenders unchanged mixed row");
    ok &= check(mixed_reuse_stats.text_content_rebuilds == 0 &&
        mixed_reuse_stats.text_content_reused == 1 &&
        mixed_reuse_stats.text_resource_descriptor_reuses == 1 &&
        mixed_reuse_stats.text_key_builds == 1 &&
        mixed_reuse_stats.qt_text_layout_calls == 0 &&
        mixed_reuse_stats.text_leaf_nodes_created == 0 &&
        mixed_reuse_stats.route_fast_text_cells == 0,
        "unchanged mixed ASCII and non-ASCII row reuses before key/layout/node work");

    term::Terminal_render_frame mixed_mutation_frame = mixed_frame;
    mixed_mutation_frame.text_runs[1].text = QStringLiteral("\u00f1");
    term::terminal_renderer_stats_t mixed_mutation_stats;
    ok &= check(update(mixed_mutation_frame, mixed_mutation_stats),
        "text exact descriptor test mutates mixed row text");
    ok &= check(mixed_mutation_stats.text_content_rebuilds == 1 &&
        mixed_mutation_stats.text_cache_entries_replaced == 1 &&
        mixed_mutation_stats.text_content_reused == 0 &&
        mixed_mutation_stats.text_resource_descriptor_reuses == 0 &&
        mixed_mutation_stats.route_fast_text_cells == 0,
        "mixed text mutation rebuilds instead of stale-reusing");

    reset_to_base();

    term::Terminal_render_text_run foreground_run = base_run;
    foreground_run.foreground = QColor(230, 110, 90);
    reset_to_base();
    term::terminal_renderer_stats_t foreground_stats;
    ok &= check(update(make_frame(foreground_run), foreground_stats),
        "text exact descriptor test changes foreground color");
    ok &= check(descriptor_rebuilt(foreground_stats),
        "foreground color participates in exact text row slot comparison");

    term::Terminal_render_text_run text_run = base_run;
    text_run.text = QStringLiteral("B");
    reset_to_base();
    term::terminal_renderer_stats_t text_stats;
    ok &= check(update(make_frame(text_run), text_stats),
        "text exact descriptor test changes text content");
    ok &= check(descriptor_rebuilt(text_stats),
        "text content participates in exact text row slot comparison");

    term::Terminal_render_text_run column_run =
        make_direct_text_run_for_row(0, 0, 1, 1, QStringLiteral("A"), metrics);
    reset_to_base();
    term::terminal_renderer_stats_t column_stats;
    ok &= check(update(make_frame(column_run), column_stats),
        "text exact descriptor test changes column");
    ok &= check(descriptor_rebuilt(column_stats),
        "column participates in exact text row slot comparison");

    term::Terminal_render_text_run rect_height_run = base_run;
    rect_height_run.rect.setHeight(rect_height_run.rect.height() + 1.0);
    reset_to_base();
    term::terminal_renderer_stats_t rect_height_stats;
    ok &= check(update(make_frame(rect_height_run), rect_height_stats),
        "text exact descriptor test changes rect height");
    ok &= check(descriptor_rebuilt(rect_height_stats),
        "row-local rect height participates in exact text row slot comparison");

    term::Terminal_render_frame run_count_frame = make_direct_text_slot_frame(
        term::Terminal_buffer_id::PRIMARY,
        0,
        1,
        {
            base_run,
            make_direct_text_run_for_row(0, 0, 1, 1, QStringLiteral("B"), metrics),
        },
        metrics,
        {{0, 1}});
    reset_to_base();
    term::terminal_renderer_stats_t run_count_stats;
    ok &= check(update(run_count_frame, run_count_stats),
        "text exact descriptor test changes run count");
    ok &= check(descriptor_rebuilt(run_count_stats),
        "run count participates in exact text row slot comparison");

    term::Terminal_render_frame logical_size_frame = base_frame;
    logical_size_frame.logical_size.rwidth() += metrics.width;
    reset_to_base();
    term::terminal_renderer_stats_t logical_size_stats;
    ok &= check(update(logical_size_frame, logical_size_stats),
        "text exact descriptor test changes logical size");
    ok &= check(descriptor_rebuilt(logical_size_stats),
        "logical size remains a text frame/key dependency");

    term::Terminal_render_text_run baseline_run = base_run;
    baseline_run.baseline_origin.ry() += 1.0;
    reset_to_base();
    term::terminal_renderer_stats_t baseline_stats;
    ok &= check(update(make_frame(baseline_run), baseline_stats),
        "text exact descriptor test changes baseline");
    ok &= check(descriptor_rebuilt(baseline_stats),
        "baseline participates in exact text row slot comparison");

    term::Terminal_render_text_run tiny_baseline_run = base_run;
    tiny_baseline_run.baseline_origin.ry() = std::nextafter(
        tiny_baseline_run.baseline_origin.y(),
        tiny_baseline_run.baseline_origin.y() + 1.0);
    reset_to_base();
    term::terminal_renderer_stats_t tiny_baseline_stats;
    ok &= check(update(make_frame(tiny_baseline_run), tiny_baseline_stats),
        "text exact descriptor test changes baseline by one representable step");
    ok &= check(descriptor_rebuilt(tiny_baseline_stats),
        "sub-fuzzy baseline changes participate in exact text row slot comparison");

    term::Terminal_render_text_run clip_run = base_run;
    clip_run.clip_rect = QRectF(0.0, 0.0, metrics.width * 0.5, metrics.height);
    reset_to_base();
    term::terminal_renderer_stats_t clip_stats;
    ok &= check(update(make_frame(clip_run), clip_stats),
        "text exact descriptor test changes clip rect");
    ok &= check(descriptor_rebuilt(clip_stats),
        "clip rect keeps the row descriptor-ineligible and rebuilds through the full key path");

    term::terminal_renderer_stats_t cleanup_stats;
    renderer.update_node(node, nullptr, {}, font, 1.0, {}, cleanup_stats);
    return ok;
}

bool test_qsg_block_cursor_text_overlay_paths(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(300, 120);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(240.0, 80.0));
    configure_test_font(surface);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_block_cursor_text_snapshot(1320U, 1, {{0, 1}}));
    window.show();

    const auto white_cursor_pixels = [&](const QImage& image, int column) {
        return count_matching_pixels(
            image,
            cell_area(0, column, 1, test_metrics()),
            [](QColor color) {
                return color.red() > 180 && color.green() > 180 && color.blue() > 180;
            });
    };
    const auto dark_glyph_pixels = [&](const QImage& image, int column) {
        return count_matching_pixels(
            image,
            cell_area(0, column, 1, test_metrics()),
            [](QColor color) {
                return color.red() + color.green() + color.blue() < 650;
            });
    };

    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        1320U,
        1),
        "ASCII block cursor render reports one rebuilt text row");
    const QImage ascii_image = window.grabWindow();
    const term::terminal_renderer_stats_t ascii_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    ok &= check(ascii_stats.frame_cursor_text_runs == 1 &&
        ascii_stats.frame_packed_text_cells == 3 &&
        ascii_stats.cursor_text_layer_rebuilt &&
        ascii_stats.route_fast_text_cells == 0,
        "ASCII block cursor text stays on the cursor overlay and canonical text routes");
    ok &= check(!ascii_image.isNull() &&
        white_cursor_pixels(ascii_image, 1) > 20 &&
        dark_glyph_pixels(ascii_image, 1) > 1,
        "ASCII block cursor paints a block and clipped inverse glyph pixels");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_complex_cursor_text_snapshot(1321U));
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        1321U,
        1),
        "complex block cursor render reports one rebuilt text row");
    const QImage complex_image = window.grabWindow();
    const term::terminal_renderer_stats_t complex_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    ok &= check(complex_stats.frame_cursor_text_runs == 1 &&
        complex_stats.frame_packed_text_cells == 2 &&
        complex_stats.route_qt_text_layout_runs > 0 &&
        complex_stats.route_fast_text_cells == 0,
        "complex block cursor text uses the existing Qt text layout route");
    ok &= check(!complex_image.isNull() &&
        white_cursor_pixels(complex_image, 1) > 20 &&
        dark_glyph_pixels(complex_image, 1) > 1,
        "complex block cursor paints a block and clipped inverse glyph pixels");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_block_cursor_text_snapshot(1322U, 1, {{0, 1}}));
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        1322U,
        1),
        "cursor movement baseline rebuilds the text row");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_block_cursor_text_snapshot(1323U, 2, {{1, 1}}));
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        1323U,
        0),
        "cursor movement reuses main text without rebuilding the text row");
    const QImage moved_image = window.grabWindow();
    const term::terminal_renderer_stats_t moved_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    const int old_cursor_white = white_cursor_pixels(moved_image, 1);
    const int new_cursor_white = white_cursor_pixels(moved_image, 2);
    ok                        &= check(moved_stats.text_content_rebuilds == 0 &&
        moved_stats.text_clean_reuse_skips == 0 &&
        moved_stats.cursor_layer_rebuilt &&
        moved_stats.cursor_text_layer_rebuilt,
        "cursor movement on a clean text row avoids clean-skip and rebuilds only cursor layers");
    ok                        &= check(!moved_image.isNull() &&
        new_cursor_white > old_cursor_white + 20,
        "cursor movement clears old block pixels and paints the new cursor cell");
    return ok;
}

bool test_qsg_wide_block_cursor_overlay(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(300, 140);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(260.0, 100.0));
    configure_test_font(surface);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_wide_cursor_snapshot());
    window.show();

    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        306U,
        1),
        "wide-glyph cursor render reports one rebuilt text row");
    ok &= check_surface_no_text_content_failures(surface,
        "wide-glyph cursor render has no text content failures");

    const term::terminal_renderer_stats_t stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    ok &= check(stats.frame_cursor_text_runs == 1 &&
        stats.cursor_text_layer_rebuilt &&
        stats.route_fast_text_cells == 0,
        "wide-glyph base cursor uses a clipped cursor text overlay");

    const QImage                        image       = window.grabWindow();
    const term::terminal_cell_metrics_t metrics     = test_metrics();
    const QRect                         cursor_area = cell_area(0, 1, 1, metrics);
    ok &= check(!image.isNull(), "wide-glyph cursor render produced an image");
    ok &= check(count_matching_pixels(
        image,
        cursor_area,
        [](QColor color) {
            return color.red() > 180 && color.green() > 180 && color.blue() > 180;
        }) > 20,
        "wide-glyph block cursor paints the cursor cell");
    ok &= check(count_matching_pixels(
        image,
        cursor_area,
        [](QColor color) {
            return color.red() + color.green() + color.blue() < 650;
        }) > 4,
        "wide-glyph block cursor preserves clipped contrasting glyph pixels");
    ok &= check(cell_is_nonblank(image, 0, 4, metrics),
        "wide-glyph cursor render keeps following ASCII visible");
    ok &= check(surface.childItems().empty(),
        "wide-glyph cursor render creates no child QQuickItems");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_wide_cursor_snapshot(2, 1330U));
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        1330U,
        0),
        "wide-glyph continuation cursor reuses main text content");
    const term::terminal_renderer_stats_t continuation_stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    ok &= check(continuation_stats.frame_cursor_text_runs == 1 &&
        continuation_stats.text_content_reused == 1 &&
        continuation_stats.cursor_text_layer_rebuilt &&
        continuation_stats.route_fast_text_cells == 0,
        "wide-glyph continuation cursor keeps inverse text in the cursor overlay");
    const QImage continuation_image = window.grabWindow();
    ok &= check(!continuation_image.isNull() &&
        cell_is_nonblank(continuation_image, 0, 2, metrics),
        "wide-glyph continuation cursor paints the cursor cell");
    return ok;
}

bool test_qsg_active_preedit_snapshot_rendering(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(300, 140);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const auto make_frame = [&](
        std::uint64_t sequence,
        QString preedit_text,
        int preedit_cursor_position,
        std::vector<term::Terminal_render_dirty_row_range> dirty_row_ranges,
        bool overlap_selection) {
        return build_qsg_sidecar_test_frame(
            *make_preedit_snapshot(
                sequence,
                std::move(preedit_text),
                preedit_cursor_position,
                std::move(dirty_row_ranges),
                overlap_selection),
            metrics);
    };
    const auto make_cleared_frame = [&](
        std::uint64_t sequence,
        std::vector<term::Terminal_render_dirty_row_range> dirty_row_ranges) {
        return
            build_qsg_sidecar_test_frame(
                *make_preedit_cleared_snapshot(sequence, std::move(dirty_row_ranges)),
                metrics);
    };
    const auto preedit_background_pixels = [&](const QImage& image) {
        return count_matching_pixels(
            image,
            cell_area(1, 1, 2, metrics),
            [](QColor color) {
                const int max_channel =
                    std::max(color.red(), std::max(color.green(), color.blue()));
                const int min_channel =
                    std::min(color.red(), std::min(color.green(), color.blue()));
                return
                    color.red()               > 28 &&
                    color.green()             > 28 &&
                    color.blue()              > 28 &&
                    max_channel - min_channel < 18;
            });
    };

    Direct_render_item item;
    item.setParentItem(window.contentItem());
    item.set_frame(make_frame(1340U, QStringLiteral("xy"), 1, {{0, 3}}, true));
    window.show();

    const QImage ascii_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            item.render_count() > 0          &&
            stats.paint_completed            &&
            stats.text_content_failures == 0 &&
            stats.text_content_rebuilds == 1;
    });
    ok &= check(!ascii_image.isNull(),
        "ASCII preedit render reports one rebuilt text row");
    const term::terminal_renderer_stats_t ascii_stats = item.last_stats();
    const bool ascii_preedit_stats_ok =
        ascii_stats.frame_text_runs == 5 &&
            ascii_stats.frame_selection_rects == 2 &&
            ascii_stats.frame_decorations == 1 &&
            ascii_stats.frame_packed_text_cells == 2 &&
            ascii_stats.route_fast_text_cells == 0;
    if (!ascii_preedit_stats_ok) {
        std::cerr << "INFO: ASCII preedit stats text_runs="
            << ascii_stats.frame_text_runs
            << " selections="  << ascii_stats.frame_selection_rects
            << " decorations=" << ascii_stats.frame_decorations
            << " packed_text=" << ascii_stats.frame_packed_text_cells
            << " route_fast="  << ascii_stats.route_fast_text_cells
            << " rebuilds="    << ascii_stats.text_content_rebuilds
            << " reused="      << ascii_stats.text_content_reused
            << '\n';
    }
    ok &= check(ascii_preedit_stats_ok,
        "ASCII preedit uses canonical text, selection, decoration, and packed-exclusion routes");
    ok &= check(!ascii_image.isNull() &&
        cell_is_nonblank(ascii_image, 1, 1, metrics),
        "ASCII preedit text is visible over the overlapping selected row");

    const int before_cjk_render_count = item.render_count();
    item.set_frame(make_frame(1341U, QStringLiteral("\u754c"), 1, {{0, 3}}, true));
    const QImage cjk_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            item.render_count() > before_cjk_render_count &&
            stats.paint_completed                         &&
            stats.text_content_failures == 0              &&
            stats.text_content_rebuilds == 1;
    });
    ok &= check(!cjk_image.isNull(),
        "CJK preedit render reports one rebuilt text row");
    const term::terminal_renderer_stats_t cjk_stats = item.last_stats();
    const bool cjk_preedit_stats_ok =
        cjk_stats.frame_text_runs == 5 &&
            cjk_stats.frame_selection_rects == 2 &&
            cjk_stats.frame_decorations == 1 &&
            cjk_stats.frame_packed_text_cells == 2 &&
            cjk_stats.route_qt_text_layout_runs > 0 &&
            cjk_stats.route_fast_text_cells == 0;
    if (!cjk_preedit_stats_ok) {
        std::cerr << "INFO: CJK preedit stats text_runs="
            << cjk_stats.frame_text_runs
            << " selections="  << cjk_stats.frame_selection_rects
            << " decorations=" << cjk_stats.frame_decorations
            << " packed_text=" << cjk_stats.frame_packed_text_cells
            << " qt_runs="     << cjk_stats.route_qt_text_layout_runs
            << " route_fast="  << cjk_stats.route_fast_text_cells
            << " rebuilds="    << cjk_stats.text_content_rebuilds
            << " reused="      << cjk_stats.text_content_reused
            << '\n';
    }
    ok &= check(cjk_preedit_stats_ok,
        "CJK preedit stays on the Qt text layout route and excludes covered packed text");
    ok &= check(!cjk_image.isNull() &&
        cell_is_nonblank(cjk_image, 1, 1, metrics) &&
        cell_is_nonblank(cjk_image, 1, 2, metrics),
        "CJK preedit spans the expected display cells");

    const int before_text_change_render_count = item.render_count();
    item.set_frame(make_frame(1342U, QStringLiteral("uv"), 2, {{0, 1}}, true));
    const QImage text_change_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            item.render_count() > before_text_change_render_count &&
            stats.paint_completed                                 &&
            stats.text_content_failures == 0                      &&
            stats.text_content_rebuilds == 1;
    });
    ok &= check(!text_change_image.isNull(),
        "preedit text change rebuilds when the preedit row is clean");
    const term::terminal_renderer_stats_t text_change_stats = item.last_stats();
    const bool preedit_clean_rebuild_stats_ok =
        text_change_stats.text_groups_clean == 1 &&
            text_change_stats.text_clean_reuse_skips == 0 &&
            text_change_stats.text_resource_descriptor_reuses == 0 &&
            text_change_stats.frame_packed_text_cells == 2;
    if (!preedit_clean_rebuild_stats_ok) {
        std::cerr << "INFO: preedit clean-row text-change stats rebuilds="
            << text_change_stats.text_content_rebuilds
            << " reused="       << text_change_stats.text_content_reused
            << " clean_groups=" << text_change_stats.text_groups_clean
            << " clean_skips="  << text_change_stats.text_clean_reuse_skips
            << " descriptor_reuses="
            << text_change_stats.text_resource_descriptor_reuses
            << " packed_text="  << text_change_stats.frame_packed_text_cells
            << '\n';
    }
    ok &= check(preedit_clean_rebuild_stats_ok,
        "active preedit clean row avoids clean-skip and rebuilds through the text key");

    const int before_preedit_clear_setup_render_count = item.render_count();
    item.set_frame(make_frame(1346U, QStringLiteral("  "), 2, {{0, 3}}, false));
    const QImage preedit_clear_setup_image = render_window_until(
        app,
        window,
        [&](const QImage& image) {
            const term::terminal_renderer_stats_t stats = item.last_stats();
            return
                item.render_count() > before_preedit_clear_setup_render_count &&
                stats.paint_completed                                         &&
                stats.text_content_failures == 0                              &&
                stats.text_content_rebuilds == 1                              &&
                preedit_background_pixels(image) > 10;
        });
    const int active_preedit_pixels       = preedit_background_pixels(preedit_clear_setup_image);
    const int before_cleared_render_count = item.render_count();
    item.set_frame(make_cleared_frame(1345U, {{0, 1}}));
    const QImage cleared_image = render_window_until(app, window, [&](const QImage& image) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            item.render_count() > before_cleared_render_count &&
            stats.paint_completed                             &&
            stats.text_content_failures == 0                  &&
            stats.text_content_rebuilds == 1                  &&
            stats.text_clean_reuse_skips == 0                 &&
            preedit_background_pixels(image) < std::max(1, active_preedit_pixels / 3);
    });
    const term::terminal_renderer_stats_t cleared_stats = item.last_stats();
    ok &= check(cleared_stats.text_groups_clean == 1 &&
        cleared_stats.text_clean_reuse_skips == 0 &&
        cleared_stats.text_cache_entries_replaced == 1 &&
        cleared_stats.text_resource_descriptor_reuses == 0,
        "clearing active preedit on a clean row rebuilds instead of clean-skipping");
    ok &= check(!cleared_image.isNull() &&
        active_preedit_pixels > 10 &&
        preedit_background_pixels(cleared_image) < active_preedit_pixels / 3,
        "clearing active preedit removes stale preedit pixels from the clean row");

    const int before_caret_start_render_count = item.render_count();
    item.set_frame(make_frame(1343U, QStringLiteral("  "), 0, {{0, 3}}, false));
    const QImage caret_start_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            item.render_count() > before_caret_start_render_count &&
            stats.paint_completed                                 &&
            stats.text_content_failures == 0                      &&
            stats.text_content_rebuilds == 1;
    });
    ok &= check(!caret_start_image.isNull(),
        "preedit caret movement baseline renders spaces and caret");

    const int before_caret_moved_render_count = item.render_count();
    item.set_frame(make_frame(1344U, QStringLiteral("  "), 2, {{0, 1}}, false));
    const QImage caret_moved_image = render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_stats_t stats = item.last_stats();
        return
            item.render_count() > before_caret_moved_render_count &&
            stats.paint_completed                                 &&
            stats.text_content_failures == 0                      &&
            stats.text_content_rebuilds == 0;
    });
    ok &= check(!caret_moved_image.isNull(),
        "preedit caret movement reuses text when the preedit row is clean");
    const term::terminal_renderer_stats_t caret_moved_stats = item.last_stats();
    const QRect old_caret_area = QRect(
        static_cast<int>(std::floor(metrics.width)),
        static_cast<int>(std::floor(metrics.height)),
        std::max(2, static_cast<int>(std::ceil(metrics.width * 0.2))),
        static_cast<int>(std::ceil(metrics.height)) - 1);
    const QRect new_caret_area = QRect(
        static_cast<int>(std::floor(metrics.width * 3.0)),
        static_cast<int>(std::floor(metrics.height)),
        std::max(2, static_cast<int>(std::ceil(metrics.width * 0.2))),
        static_cast<int>(std::ceil(metrics.height)) - 1);
    const auto caret_pixels = [](const QImage& image, QRect area) {
        return count_matching_pixels(
            image,
            area,
            [](QColor color) {
                return default_foreground_pixel(color);
            });
    };
    ok &= check(caret_moved_stats.text_content_rebuilds == 0 &&
        caret_moved_stats.text_content_reused == 1 &&
        caret_moved_stats.text_groups_clean == 1 &&
        caret_moved_stats.text_clean_reuse_skips == 0 &&
        caret_moved_stats.decoration_layer_rebuilt,
        "preedit caret movement on a clean row avoids clean-skip and rebuilds decorations");
    const int start_old_caret_pixels = caret_pixels(caret_start_image, old_caret_area);
    const int moved_old_caret_pixels = caret_pixels(caret_moved_image, old_caret_area);
    const int moved_new_caret_pixels = caret_pixels(caret_moved_image, new_caret_area);
    ok &= check(!caret_start_image.isNull() &&
        !caret_moved_image.isNull() &&
        start_old_caret_pixels > 0 &&
        moved_new_caret_pixels > moved_old_caret_pixels,
        "preedit caret movement clears the old caret pixels and paints the new caret");
    return ok;
}

bool test_qsg_block_cursor_over_terminal_graphic(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(300, 140);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(260.0, 100.0));
    configure_test_font(surface);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_cursor_over_terminal_graphic_snapshot());
    window.show();

    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        309U,
        1),
        "graphic cursor render reports one rebuilt text row");
    ok &= check_surface_no_text_content_failures(surface,
        "graphic cursor render has no text content failures");

    const term::terminal_renderer_stats_t stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    ok &= check(stats.cursor_layer_rebuilt &&
        stats.cursor_graphic_layer_rebuilt &&
        !stats.cursor_text_layer_rebuilt,
        "graphic cursor render uses graphic overlay instead of font cursor text");
    ok &= check(stats.frame_packed_graphic_cells == 0 &&
        stats.graphic_batched_rects == 0 &&
        stats.graphic_batched_vertices == 0,
        "cursor-covered terminal graphics stay out of the packed hard-rect batch");

    const QImage                        image       = window.grabWindow();
    const term::terminal_cell_metrics_t metrics     = test_metrics();
    const QRect                         cursor_area = cell_area(0, 1, 1, metrics);
    const int bright_cursor_pixels = count_matching_pixels(
        image,
        cursor_area,
        [](QColor color) {
            return color.red() > 180 && color.green() > 180 && color.blue() > 180;
        });
    const int graphic_overlay_pixels = count_matching_pixels(
        image,
        cursor_area,
        [](QColor color) {
            return color.red() + color.green() + color.blue() < 340;
        });
    if (graphic_overlay_pixels <= 3) {
        std::cerr << "INFO: observed graphic cursor overlay pixels: "
            << graphic_overlay_pixels << '\n';
    }
    ok &= check(!image.isNull(), "graphic cursor render produced an image");
    ok &= check(bright_cursor_pixels >
        static_cast<int>(
            static_cast<qreal>(cursor_area.width() * cursor_area.height()) * 0.35),
        "graphic cursor render paints the block cursor background");
    ok &= check(graphic_overlay_pixels > 3,
        "graphic cursor render preserves contrasting primitive pixels over the block cursor");
    ok &= check(cell_is_nonblank(image, 0, 0, metrics) &&
        cell_is_nonblank(image, 0, 2, metrics),
        "graphic cursor render keeps neighboring text visible");
    ok &= check(surface.childItems().empty(),
        "graphic cursor render creates no child QQuickItems");
    return ok;
}

bool render_sequence(
    QGuiApplication&           app,
    QQuickWindow&              window,
    const VNM_TerminalSurface& surface,
    std::uint64_t              sequence)
{
    return window_render_matches(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t render_stats =
            term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
        return
            stats.last_rendered_snapshot_sequence == sequence &&
            !stats.pending_update                             &&
            render_stats.text_content_failures == 0;
    });
}

bool test_qsg_rect_resource_lifecycle_direct_cleanup_paths(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(380, 120);
    window.show();
    pump_events(app, 2);

    const QFont font = term::vnm_terminal_font(QString(), 16.0);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const term::Terminal_render_rect background_row_zero =
        make_direct_row_rect(0, 0, 3, QColor(130, 36, 48), metrics);
    const term::Terminal_render_rect background_row_one =
        make_direct_row_rect(1, 1, 4, QColor(34, 126, 68), metrics);
    const term::Terminal_render_rect selection_row_zero = make_direct_row_rect(
        0,
        4,
        3,
        QColor(48, 96, 160, 190),
        metrics);
    const term::Terminal_render_rect graphic_rect_row_zero = make_direct_row_rect(
        0,
        8,
        1,
        QColor(210, 156, 48),
        metrics);
    const term::Terminal_render_arc graphic_arc_row_one = make_direct_row_arc(
        1,
        8,
        term::Terminal_render_arc_kind::DOWN_RIGHT,
        QColor(80, 150, 226),
        std::max<qreal>(3.0, std::floor(metrics.height * 0.20)),
        metrics);
    const term::Terminal_render_decoration decoration_row_two = make_direct_decoration_rect(
        2,
        2,
        5,
        std::floor(metrics.ascent + 1.0),
        1.0,
        QColor(196, 230, 201),
        metrics);

    term::Terminal_render_frame resource_frame = make_direct_rect_cache_frame(
        { background_row_zero, background_row_one },
        { selection_row_zero },
        metrics);
    resource_frame.graphic_rects    = {graphic_rect_row_zero};
    resource_frame.graphic_arcs     = {graphic_arc_row_one};
    resource_frame.decorations      = {decoration_row_two};
    resource_frame.text_runs        = {
        make_direct_text_run(10, 4, QStringLiteral("RECT"), metrics),
    };
    resource_frame.dirty_row_ranges = {{0, 3}};

    auto lifecycle_recorder =
        std::make_shared<term::Terminal_renderer_lifecycle_recorder>();
    term::Qsg_terminal_renderer renderer;
    QSGNode* node = nullptr;
    auto update = [&](
        const term::Terminal_render_frame& frame,
        term::terminal_renderer_stats_t& stats) {
        node = renderer.update_node(
            node,
            &window,
            frame,
            font,
            1.0,
            lifecycle_recorder,
            stats);
        return
            node != nullptr       &&
            stats.paint_completed &&
            stats.text_content_failures == 0;
    };

    term::terminal_renderer_stats_t setup_render_stats;
    ok &= check(update(resource_frame, setup_render_stats),
        "rect resource lifecycle setup renders cached row resources");
    const term::terminal_renderer_lifecycle_stats_t setup_lifecycle_stats =
        lifecycle_recorder->snapshot();
    ok &= check(setup_render_stats.background_rows_rebuilt == 2 &&
        setup_render_stats.selection_rows_rebuilt == 1 &&
        setup_render_stats.graphic_rect_rows_rebuilt == 1 &&
        setup_render_stats.decoration_rows_rebuilt == 1,
        "rect resource lifecycle setup builds row wrappers for converted rect passes");
    ok &= check(setup_lifecycle_stats.render_rect_resources_created >= 4U &&
        live_rect_resource_count(setup_lifecycle_stats) >= 4U,
        "rect resource lifecycle setup records live row-cache wrapper resources");

    const term::Terminal_render_rect non_row_background_rect{
        QRectF(
            metrics.width * 0.5,
            metrics.height * 0.5,
            metrics.width * 2.0,
            metrics.height * 1.25),
        QColor(38, 54, 144),
    };
    term::Terminal_render_frame fallback_frame = make_direct_rect_cache_frame(
        { background_row_zero, non_row_background_rect, background_row_one },
        {},
        metrics);
    fallback_frame.dirty_row_ranges = {{0, 3}};

    term::terminal_renderer_stats_t fallback_render_stats;
    ok &= check(update(fallback_frame, fallback_render_stats),
        "rect resource lifecycle renders a background flat fallback cleanup frame");
    const term::terminal_renderer_lifecycle_stats_t fallback_lifecycle_stats =
        lifecycle_recorder->snapshot();
    ok &= check(fallback_render_stats.background_row_cache_fallbacks == 1 &&
        fallback_render_stats.background_rows_removed == 2,
        "flat fallback removes cached background row wrapper resources");
    ok &= check(live_rect_resource_count(fallback_lifecycle_stats) == 0U,
        "flat fallback and empty sibling passes leave no live rect row resources");

    term::terminal_renderer_stats_t rebuilt_render_stats;
    ok &= check(update(resource_frame, rebuilt_render_stats),
        "rect resource lifecycle rebuilds row resources after fallback cleanup");
    const term::terminal_renderer_lifecycle_stats_t rebuilt_lifecycle_stats =
        lifecycle_recorder->snapshot();
    ok &= check(live_rect_resource_count(rebuilt_lifecycle_stats) >= 4U,
        "rect resource lifecycle has live row resources after rebuild");

    term::Terminal_render_frame empty_row_frame =
        make_direct_rect_cache_frame({}, {}, metrics);
    empty_row_frame.dirty_row_ranges = {{0, 3}};
    term::terminal_renderer_stats_t removal_render_stats;
    ok &= check(update(empty_row_frame, removal_render_stats),
        "rect resource lifecycle renders an empty row-cache cleanup frame");
    const term::terminal_renderer_lifecycle_stats_t removal_lifecycle_stats =
        lifecycle_recorder->snapshot();
    ok &= check(removal_render_stats.background_rows_removed == 2 &&
        removal_render_stats.selection_rows_removed == 1 &&
        removal_render_stats.graphic_rect_rows_removed == 1 &&
        removal_render_stats.decoration_rows_removed == 1,
        "empty frame removes cached rect row wrappers from every converted pass");
    ok &= check(live_rect_resource_count(removal_lifecycle_stats) == 0U,
        "row removal leaves no live rect row resources");

    term::terminal_renderer_stats_t final_setup_render_stats;
    ok &= check(update(resource_frame, final_setup_render_stats),
        "rect resource lifecycle rebuilds row resources before direct teardown");
    term::terminal_renderer_stats_t cleanup_render_stats;
    node = renderer.update_node(
        node,
        nullptr,
        {},
        font,
        1.0,
        lifecycle_recorder,
        cleanup_render_stats);
    const term::terminal_renderer_lifecycle_stats_t cleanup_lifecycle_stats =
        lifecycle_recorder->snapshot();
    ok &= check(node == nullptr,
        "direct null-window renderer cleanup returns no render node");
    ok &= check(has_no_live_render_resources(cleanup_lifecycle_stats),
        "direct null-window renderer cleanup leaves no live render resources");
    return ok;
}

bool test_qsg_resource_lifecycle(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(300, 180);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(260.0, 140.0));
    configure_test_font(surface);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false, {{0, 5}}, QStringLiteral("L"), 901U));
    std::shared_ptr<term::Terminal_renderer_lifecycle_recorder> lifecycle_recorder =
        term::VNM_TerminalSurface_render_bridge::lifecycle_recorder(surface);
    window.show();

    ok &= check(render_sequence(app, window, surface, 901U),
        "lifecycle setup renders a resource-owning node tree");
    ok &= check_surface_no_text_content_failures(surface,
        "lifecycle setup render has no text content failures");
    const term::terminal_renderer_lifecycle_stats_t setup_stats =
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(surface);
    ok &= check(setup_stats.render_root_nodes_created >= 1U,
        "initial render creates a QSG root resource object");
    ok &= check(setup_stats.render_text_resources_created > 0U,
        "initial render creates QSG-owned text resources");
    ok &= check(setup_stats.render_rect_resources_created > 0U,
        "initial render creates QSG-owned rect row resources");
    ok &= check(has_live_render_tree(setup_stats),
        "initial render leaves exactly one live QSG render tree");

    surface.setSize(QSizeF(0.0, 0.0));
    render_window_until(app, window, [&](const QImage&) {
        const term::terminal_renderer_lifecycle_stats_t stats =
            term::VNM_TerminalSurface_render_bridge::lifecycle_stats(surface);
        return stats.render_node_deletions_in_paint >
            setup_stats.render_node_deletions_in_paint;
    });
    const term::terminal_renderer_lifecycle_stats_t zero_size_stats =
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(surface);
    ok &= check(zero_size_stats.render_node_deletions_in_paint >
        setup_stats.render_node_deletions_in_paint,
        "invalid paint geometry deletes the old render node in updatePaintNode");
    ok &= check(zero_size_stats.render_root_nodes_destroyed >
        setup_stats.render_root_nodes_destroyed,
        "invalid paint geometry destroys the QSG root resource object");
    ok &= check(zero_size_stats.render_text_resources_destroyed >
        setup_stats.render_text_resources_destroyed,
        "invalid paint geometry destroys QSG-owned text resources");
    ok &= check(zero_size_stats.render_rect_resources_destroyed >
        setup_stats.render_rect_resources_destroyed,
        "invalid paint geometry destroys QSG-owned rect row resources");
    ok &= check(has_no_live_render_resources(zero_size_stats),
        "invalid paint geometry leaves no stale live QSG resources");

    surface.setSize(QSizeF(260.0, 140.0));
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false, {{0, 5}}, QStringLiteral("M"), 902U));
    ok &= check(render_sequence(app, window, surface, 902U),
        "surface rerenders after updatePaintNode resource cleanup");
    ok &= check_surface_no_text_content_failures(surface,
        "rerender after updatePaintNode resource cleanup has no text content failures");
    ok &= check(has_live_render_tree(
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(surface)),
        "rerender after invalid geometry owns exactly one live QSG render tree");

    const term::terminal_renderer_lifecycle_stats_t pre_release_stats =
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(surface);
    term::VNM_TerminalSurface_render_bridge::release_resources(surface);
    ok &= check(
        wait_lifecycle_until(app, lifecycle_recorder, [&](const auto& stats) {
            return
                stats.release_resources_calls == pre_release_stats.release_resources_calls + 1U &&
                has_no_live_render_resources(stats);
        }),
        "explicit releaseResources lifecycle converges");
    const term::terminal_renderer_lifecycle_stats_t release_stats = lifecycle_recorder->snapshot();
    ok &= check(release_stats.release_resources_calls ==
        pre_release_stats.release_resources_calls + 1U,
        "explicit releaseResources path is recorded");
    ok &= check(has_no_live_render_resources(release_stats),
        "explicit releaseResources leaves no stale live QSG resources");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false, {{0, 5}}, QStringLiteral("N"), 903U));
    ok &= check(render_sequence(app, window, surface, 903U),
        "surface rerenders after explicit releaseResources");
    ok &= check_surface_no_text_content_failures(surface,
        "rerender after explicit releaseResources has no text content failures");
    ok &= check(has_live_render_tree(
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(surface)),
        "rerender after explicit releaseResources owns exactly one live QSG render tree");

    const term::terminal_renderer_lifecycle_stats_t pre_pending_release_stats =
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(surface);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false, {{0, 5}}, QStringLiteral("P"), 906U));
    term::VNM_TerminalSurface_render_bridge::release_resources(surface);
    term::VNM_TerminalSurface_render_bridge::release_resources(surface);
    ok &= check(
        window_render_matches(app, window, [&](const QImage&) {
            const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
                term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
            const term::terminal_renderer_lifecycle_stats_t lifecycle_stats =
                term::VNM_TerminalSurface_render_bridge::lifecycle_stats(surface);
            const term::terminal_renderer_stats_t render_stats =
                term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
            const bool release_count_advanced =
                lifecycle_stats.release_resources_calls ==
                pre_pending_release_stats.release_resources_calls + 2U;
            const bool render_deletions_advanced =
                lifecycle_stats.render_node_deletions_in_paint >
                pre_pending_release_stats.render_node_deletions_in_paint;
            return
                invalidation_stats.last_rendered_snapshot_sequence == 906U &&
                !invalidation_stats.pending_update                         &&
                render_stats.text_content_failures == 0                    &&
                release_count_advanced                                     &&
                render_deletions_advanced                                  &&
                has_live_render_tree(lifecycle_stats);
        }),
        "repeated releaseResources with pending snapshot update requeues the render update");

    const term::terminal_renderer_lifecycle_stats_t pre_release_then_snapshot_stats =
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(surface);
    term::VNM_TerminalSurface_render_bridge::release_resources(surface);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false, {{0, 5}}, QStringLiteral("Q"), 907U));
    ok &= check(
        window_render_matches(app, window, [&](const QImage&) {
            const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
                term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
            const term::terminal_renderer_lifecycle_stats_t lifecycle_stats =
                term::VNM_TerminalSurface_render_bridge::lifecycle_stats(surface);
            const term::terminal_renderer_stats_t render_stats =
                term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
            const bool release_count_advanced =
                lifecycle_stats.release_resources_calls ==
                pre_release_then_snapshot_stats.release_resources_calls + 1U;
            const bool render_deletions_advanced =
                lifecycle_stats.render_node_deletions_in_paint >
                pre_release_then_snapshot_stats.render_node_deletions_in_paint;
            return
                invalidation_stats.last_rendered_snapshot_sequence == 907U &&
                !invalidation_stats.pending_update                         &&
                render_stats.text_content_failures == 0                    &&
                release_count_advanced                                     &&
                render_deletions_advanced                                  &&
                has_live_render_tree(lifecycle_stats);
        }),
        "snapshot queued after releaseResources is rendered after release cleanup");

    const term::terminal_renderer_lifecycle_stats_t pre_detach_stats =
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(surface);
    surface.setParentItem(nullptr);
    ok &= check(
        wait_lifecycle_until(app, lifecycle_recorder, [&](const auto& stats) {
            return
                stats.item_scene_detaches == pre_detach_stats.item_scene_detaches + 1U         &&
                stats.release_resources_calls >= pre_detach_stats.release_resources_calls + 1U &&
                has_no_live_render_resources(stats);
        }),
        "item scene detach lifecycle converges");
    const term::terminal_renderer_lifecycle_stats_t detach_stats = lifecycle_recorder->snapshot();
    ok &= check(detach_stats.item_scene_changes == pre_detach_stats.item_scene_changes + 1U,
        "item scene detach records a scene change");
    ok &= check(detach_stats.item_scene_detaches == pre_detach_stats.item_scene_detaches + 1U,
        "item scene detach records the window loss");
    ok &= check(detach_stats.release_resources_calls >=
        pre_detach_stats.release_resources_calls + 1U,
        "item scene detach releases render resources");
    ok &= check(has_no_live_render_resources(detach_stats),
        "item scene detach leaves no stale live QSG resources");

    surface.setParentItem(window.contentItem());
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false, {{0, 5}}, QStringLiteral("O"), 904U));
    ok &= check(render_sequence(app, window, surface, 904U),
        "surface rerenders after item scene reattach");
    ok &= check_surface_no_text_content_failures(surface,
        "reattach rerender has no text content failures");
    ok &= check(has_live_render_tree(
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(surface)),
        "reattach rerender owns exactly one live QSG render tree");

    const term::terminal_renderer_lifecycle_stats_t pre_invalidation_stats =
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(surface);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false, {{0, 5}}, QStringLiteral("I"), 908U));
    ok &= check(term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface)
        .pending_update,
        "scene graph invalidation setup leaves a render update pending");
    term::VNM_TerminalSurface_render_bridge::simulate_stale_scene_graph_invalidated(surface);
    const term::terminal_renderer_lifecycle_stats_t stale_invalidation_stats =
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(surface);
    ok &= check(term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface)
        .pending_update,
        "stale queued scene graph invalidation does not clear a newer pending update");
    ok &= check(stale_invalidation_stats.scene_graph_invalidated_calls ==
        pre_invalidation_stats.scene_graph_invalidated_calls + 1U,
        "stale queued scene graph invalidation is recorded");
    term::VNM_TerminalSurface_render_bridge::simulate_scene_graph_invalidated(surface);
    const term::terminal_renderer_lifecycle_stats_t invalidation_stats =
        term::VNM_TerminalSurface_render_bridge::lifecycle_stats(surface);
    const term::Terminal_surface_render_invalidation_stats_t invalidated_render_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    ok &= check(invalidation_stats.scene_graph_invalidated_calls ==
        stale_invalidation_stats.scene_graph_invalidated_calls + 1U,
        "scene graph invalidation handler is recorded");
    ok &= check(!invalidated_render_stats.pending_update,
        "scene graph invalidation handler clears pending render updates");
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_snapshot(false, {{0, 5}}, QStringLiteral("R"), 909U));
    ok &= check(render_sequence(app, window, surface, 909U),
        "surface rerenders after scene graph invalidation handling");
    ok &= check_surface_no_text_content_failures(surface,
        "scene graph invalidation rerender has no text content failures");

    return ok;
}

bool test_qsg_item_teardown_with_resources(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(300, 180);

    auto surface = std::make_unique<VNM_TerminalSurface>();
    surface->setParentItem(window.contentItem());
    surface->setSize(QSizeF(260.0, 140.0));
    configure_test_font(*surface);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        *surface,
        make_snapshot(false, {{0, 5}}, QStringLiteral("T"), 905U));

    std::shared_ptr<term::Terminal_renderer_lifecycle_recorder> lifecycle_recorder =
        term::VNM_TerminalSurface_render_bridge::lifecycle_recorder(*surface);
    window.show();
    ok &= check(render_sequence(app, window, *surface, 905U),
        "teardown setup renders resources before item destruction");
    ok &= check_surface_no_text_content_failures(*surface,
        "teardown setup render has no text content failures");

    const term::terminal_renderer_lifecycle_stats_t pre_teardown_stats =
        lifecycle_recorder->snapshot();
    surface.reset();
    ok &= check(
        wait_lifecycle_until(app, lifecycle_recorder, [&](const auto& stats) {
            return
                stats.item_destructions == pre_teardown_stats.item_destructions + 1U             &&
                stats.release_resources_calls == pre_teardown_stats.release_resources_calls + 1U &&
                has_no_live_render_resources(stats);
        }),
        "item teardown lifecycle converges");
    const term::terminal_renderer_lifecycle_stats_t teardown_stats = lifecycle_recorder->snapshot();
    ok &= check(teardown_stats.item_destructions ==
        pre_teardown_stats.item_destructions + 1U,
        "item teardown records surface destruction");
    ok &= check(teardown_stats.release_resources_calls ==
        pre_teardown_stats.release_resources_calls + 1U,
        "item teardown routes through releaseResources");
    ok &= check(teardown_stats.render_root_nodes_destroyed >=
        pre_teardown_stats.render_root_nodes_destroyed + 1U,
        "item teardown destroys the QSG root resource object");
    ok &= check(teardown_stats.render_text_resources_destroyed >
        pre_teardown_stats.render_text_resources_destroyed,
        "item teardown destroys QSG-owned text resources");
    ok &= check(teardown_stats.render_rect_resources_destroyed >
        pre_teardown_stats.render_rect_resources_destroyed,
        "item teardown destroys QSG-owned rect row resources");
    ok &= check(has_no_live_render_resources(teardown_stats),
        "item teardown leaves no stale live QSG resources");
    return ok;
}

bool test_qsg_terminal_graphics_rendering(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(360, 130);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(320.0, 90.0));
    configure_test_font(surface);
    const std::shared_ptr<const term::Terminal_render_snapshot> graphics_snapshot =
        make_terminal_graphics_snapshot();
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        graphics_snapshot);
    window.show();

    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        308U,
        1),
        "terminal graphics render reports one rebuilt text row");
    const term::terminal_renderer_stats_t stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    ok &= check(stats.graphic_layer_rebuilt,
        "terminal graphics render rebuilds the graphic geometry layer");
    ok &= check(stats.frame_packed_graphic_cells >= 2 &&
        stats.frame_graphic_rects > 0 &&
        stats.frame_graphic_arcs > 0,
        "terminal graphics render publishes packed graphics, rects, and arcs");
    if (window_uses_software_scene_graph(window)) {
        ok &= check(stats.graphic_batched_rects == 0 &&
            stats.graphic_batched_vertices == 0,
            "software terminal graphics render leaves graphic batched counters at zero");
    }
    else {
        ok &= check(stats.graphic_batched_rects == 2 &&
            stats.graphic_batched_vertices == stats.graphic_batched_rects * 6 &&
            stats.frame_graphic_rects > stats.graphic_batched_rects,
            "terminal graphics render batches only hard block rects and leaves boxes on existing route");
    }
    ok &= check_surface_no_text_content_failures(surface,
        "terminal graphics render has no text content failures");

    const term::terminal_cell_metrics_t metrics           = test_metrics();
    const QImage                        image             = window.grabWindow();
    const QRect                         full_block_area   = cell_area(1, 1, 1, metrics);
    const int                           full_block_pixels = count_non_background_pixels(image, full_block_area);
    ok &= check(!image.isNull(), "terminal graphics QSG render produced an image");
    ok &= check_visual_pixels(
        !image.isNull() &&
            full_block_pixels >
            static_cast<int>(
                static_cast<qreal>(full_block_area.width() * full_block_area.height()) * 0.75),
        "full block graphic fills its terminal cell",
        image,
        full_block_area,
        full_block_pixels,
        graphics_snapshot->metadata.sequence,
        graphics_snapshot->dirty_row_ranges,
        stats);
    const QRect box_line_area   = cell_area(0, 1, 1, metrics);
    const int   box_line_pixels = count_non_background_pixels(image, box_line_area);
    ok &= check_visual_pixels(
        !image.isNull() && box_line_pixels > 2,
        "box drawing graphic renders without font text",
        image,
        box_line_area,
        box_line_pixels,
        graphics_snapshot->metadata.sequence,
        graphics_snapshot->dirty_row_ranges,
        stats);
    const int softened_box_line_pixels = count_matching_pixels(
        image,
        box_line_area,
        [](QColor color) {
            return
                !is_terminal_background(color) &&
                color.red() < 190              &&
                color.green() < 220;
        });
    ok &= check(softened_box_line_pixels > 0,
        "box drawing graphic uses antialiased edge coverage");
    const QRect rounded_corner_area = cell_area(0, 3, 1, metrics);
    const int rounded_corner_pixels =
        count_non_background_pixels(image, rounded_corner_area);
    if (rounded_corner_pixels <= 2) {
        std::cerr << "INFO: observed rounded corner pixels: "
            << rounded_corner_pixels << '\n';
    }
    ok &= check(rounded_corner_pixels > 2,
        "rounded box corner graphic renders without font text");
    const int softened_rounded_corner_pixels = count_matching_pixels(
        image,
        rounded_corner_area,
        [](QColor color) {
            return
                !is_terminal_background(color) &&
                color.red() < 190              &&
                color.green() < 220;
        });
    ok &= check(softened_rounded_corner_pixels > 0,
        "rounded box corner graphic uses antialiased edge coverage");
    ok &= check(cell_is_nonblank(image, 2, 0, metrics),
        "ordinary text still renders alongside terminal graphics");
    ok &= check(surface.childItems().empty(),
        "terminal graphics render creates no child QQuickItems");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_terminal_graphics_snapshot(310U, QStringLiteral("B"), {{2, 1}}));
    ok &= check(window_render_matches(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t render_stats =
            term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
        return
            invalidation_stats.last_rendered_snapshot_sequence == 310U &&
            !invalidation_stats.pending_update                         &&
            render_stats.root_reused                                   &&
            render_stats.paint_completed                               &&
            render_stats.text_content_failures == 0                    &&
            render_stats.text_content_rebuilds == 1                    &&
            !render_stats.graphic_layer_rebuilt;
    }), "terminal graphics text dirty update reaches a fresh render");
    const term::terminal_renderer_stats_t dirty_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    ok &= check(dirty_stats.text_content_rebuilds == 1 &&
        !dirty_stats.graphic_layer_rebuilt &&
        !dirty_stats.cursor_graphic_layer_rebuilt,
        "terminal graphics text dirty update reuses unchanged graphic layers");
    ok &= check_surface_no_text_content_failures(surface,
        "terminal graphics text dirty update has no text content failures");
    return ok;
}

bool test_qsg_terminal_graphics_pixel_transitions(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(360, 130);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(320.0, 90.0));
    configure_test_font(surface);

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_unicode_snapshot(
            980U,
            {
                { {0, 1}, QStringLiteral("\u2500"), 0U, 1, false, 0U },
                { {0, 3}, QStringLiteral("\u256d"), 0U, 1, false, 0U },
                { {1, 1}, QStringLiteral("\u2588"), 0U, 1, false, 0U },
                { {1, 2}, QStringLiteral("\u2598"), 0U, 1, false, 0U },
                { {2, 0}, QStringLiteral("A"),      0U, 1, false, 0U },
            },
            {{0, 2}}));
    window.show();

    ok &= check(wait_rendered_sequence_with_text_rebuilds(app, window, surface, 980U, 1),
        "terminal graphics transition setup renders one text row");
    const term::terminal_renderer_stats_t setup_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    if (window_uses_software_scene_graph(window)) {
        ok &= check(setup_stats.graphic_batched_rects == 0 &&
            setup_stats.graphic_batched_vertices == 0,
            "software terminal graphics transition setup leaves batched counters at zero");
    }
    else {
        ok &= check(setup_stats.graphic_batched_rects == 2 &&
            setup_stats.graphic_batched_vertices == setup_stats.graphic_batched_rects * 6,
            "terminal graphics transition setup batches hard block rects");
    }
    const term::terminal_cell_metrics_t metrics         = test_metrics();
    const QImage                        graphic_image   = window.grabWindow();
    const QRect                         full_block_area = cell_area(1, 1, 1, metrics);
    ok &= check(!graphic_image.isNull() &&
        count_non_background_pixels(graphic_image, full_block_area) >
            static_cast<int>(
                static_cast<qreal>(
                    full_block_area.width() * full_block_area.height()) * 0.75) &&
        count_non_background_pixels(graphic_image, cell_area(0, 3, 1, metrics)) > 2,
        "terminal graphics transition setup renders block and arc pixels");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_unicode_snapshot(
            981U,
            {
                { {0, 0}, QStringLiteral("T"), 0U, 1, false, 0U },
                { {2, 0}, QStringLiteral("A"), 0U, 1, false, 0U },
            },
            {{0, 2}}));
    const QImage text_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            invalidation_stats.last_rendered_snapshot_sequence == 981U &&
            !invalidation_stats.pending_update                         &&
            stats.paint_completed                                      &&
            stats.text_content_failures == 0                           &&
            stats.graphic_layer_rebuilt;
    });
    ok &= check(!text_image.isNull() &&
        cell_is_nonblank(text_image, 0, 0, metrics) &&
        mostly_background(text_image, 0, 3, 1, metrics) &&
        mostly_background(text_image, 1, 1, 1, metrics),
        "terminal graphics-to-text transition clears stale arc and block pixels");
    const term::terminal_renderer_stats_t text_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    ok &= check(text_stats.graphic_batched_rects == 0 &&
        text_stats.graphic_batched_vertices == 0,
        "terminal graphics-to-text transition clears graphic batched counters");

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_unicode_snapshot(
            982U,
            {
                { {0, 1}, QStringLiteral("\u2588"), 0U, 1, false, 0U },
                { {0, 3}, QStringLiteral("\u2570"), 0U, 1, false, 0U },
                { {2, 0}, QStringLiteral("A"),      0U, 1, false, 0U },
            },
            {{0, 1}}));
    const QImage block_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            invalidation_stats.last_rendered_snapshot_sequence == 982U &&
            !invalidation_stats.pending_update                         &&
            stats.paint_completed                                      &&
            stats.text_content_failures == 0                           &&
            stats.graphic_layer_rebuilt;
    });
    const QRect row_zero_block_area = cell_area(0, 1, 1, metrics);
    ok &= check(!block_image.isNull() &&
        count_non_background_pixels(block_image, row_zero_block_area) >
            static_cast<int>(
                static_cast<qreal>(
                    row_zero_block_area.width() * row_zero_block_area.height()) * 0.75) &&
        mostly_background(block_image, 0, 0, 1, metrics),
        "terminal text-to-graphics transition clears stale text and renders block pixels");
    const term::terminal_renderer_stats_t block_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    if (!window_uses_software_scene_graph(window)) {
        ok &= check(block_stats.graphic_batched_rects == 1 &&
            block_stats.graphic_batched_vertices == 6,
            "terminal text-to-graphics transition batches the new hard block rect");
    }

    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_terminal_graphics_foreground_blank_snapshot(983U));
    const QImage blank_image = render_window_until(app, window, [&](const QImage&) {
        const term::Terminal_surface_render_invalidation_stats_t invalidation_stats =
            term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
        const term::terminal_renderer_stats_t stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
            surface);
        return
            invalidation_stats.last_rendered_snapshot_sequence == 983U &&
            !invalidation_stats.pending_update                         &&
            stats.paint_completed                                      &&
            stats.text_content_failures == 0                           &&
            stats.graphic_layer_rebuilt;
    });
    ok &= check(!blank_image.isNull() &&
        mostly_background(blank_image, 0, 1, 1, metrics) &&
        mostly_background(blank_image, 0, 3, 1, metrics),
        "terminal graphics-to-foreground-blank transition clears stale graphic pixels");
    const term::terminal_renderer_stats_t blank_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    ok &= check(blank_stats.graphic_batched_rects == 0 &&
        blank_stats.graphic_batched_vertices == 0,
        "terminal graphics-to-foreground-blank transition clears batched graphic counters");
    return ok;
}

bool test_qsg_unicode_rendering(QGuiApplication& app)
{
    bool ok = true;
    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(360, 130);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(320.0, 90.0));
    configure_test_font(surface);

    const term::terminal_cell_metrics_t metrics = test_metrics();
    const auto unicode_cells = std::vector<term::Terminal_render_cell>{
        { {0, 0}, QStringLiteral("A"), 0U, 1, false, 0U },
        { {0, 1}, QStringLiteral("\u754c"), 0U, 2, false, 0U },
        { {0, 2}, {}, 0U, 0, true, 0U },
        { {0, 3}, QStringLiteral("B"), 0U, 1, false, 0U },
        { {0, 4}, QStringLiteral("e\u0301"), 0U, 1, false, 0U },
        { {0, 5}, QStringLiteral("C"), 0U, 1, false, 0U },
        { {0, 6}, QString::fromUcs4(U"\u2764\ufe0e"), 0U, 1, false, 0U },
        { {0, 7}, QStringLiteral("D"), 0U, 1, false, 0U },
        { {0, 8}, QString::fromUcs4(U"\u2764\ufe0f"), 0U, 2, false, 0U },
        { {0, 9}, {}, 0U, 0, true, 0U },
        { {0, 10}, QStringLiteral("E"), 0U, 1, false, 0U },
        { {0, 11}, QString::fromUcs4(U"\U0001f600"), 0U, 2, false, 0U },
        { {0, 12}, {}, 0U, 0, true, 0U },
        { {0, 13}, QStringLiteral("F"),      0U, 1, false, 0U },
        { {0, 14}, QStringLiteral("\u03a9"), 0U, 1, false, 0U },
        { {0, 15}, QStringLiteral("G"),      0U, 1, false, 0U },
    };
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_unicode_snapshot(301U, unicode_cells, {{0, 1}}));
    window.show();

    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        301U,
        1),
        "unicode first render reports one rebuilt text row");
    ok &= check_surface_no_text_content_failures(surface,
        "unicode first render has no text content failures");
    const QImage unicode_image = window.grabWindow();
    ok &= check(!unicode_image.isNull(), "unicode QSG render produced an image");
    ok &= check(cell_is_nonblank(unicode_image, 0, 0, metrics) &&
        cell_is_nonblank(unicode_image, 0, 3, metrics) &&
        cell_is_nonblank(unicode_image, 0, 5, metrics),
        "mixed Unicode row keeps ASCII sentinels visible");
    ok &= check(cell_is_nonblank(unicode_image, 0, 4, metrics) &&
        cell_is_nonblank(unicode_image, 0, 5, metrics),
        "combining cluster stays one cell and following ASCII is aligned");
    ok &= check(cell_is_nonblank(unicode_image, 0, 7, metrics) &&
        cell_is_nonblank(unicode_image, 0, 10, metrics) &&
        cell_is_nonblank(unicode_image, 0, 13, metrics),
        "variation-selector and emoji sentinels stay aligned");
    ok &= check(mostly_background(unicode_image, 1, 0, 4, metrics),
        "mixed Unicode row does not materially bleed into blank neighbors");
    ok &= check(surface.childItems().empty(), "unicode render creates no child QQuickItems");

    const auto narrow_cells = std::vector<term::Terminal_render_cell>{
        { {0, 0}, QStringLiteral("A"), 0U, 1, false, 0U },
        { {0, 1}, QStringLiteral("B"), 0U, 1, false, 0U },
        { {0, 2}, QStringLiteral("C"), 0U, 1, false, 0U },
        { {0, 3}, QStringLiteral("D"), 0U, 1, false, 0U },
    };
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_unicode_snapshot(302U, narrow_cells));
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        302U,
        1),
        "narrow setup render reports one rebuilt text row");
    ok &= check_surface_no_text_content_failures(surface,
        "narrow setup render has no text content failures");
    const QImage narrow_setup_image = window.grabWindow();
    ok &= check(!narrow_setup_image.isNull() &&
        cell_is_nonblank(narrow_setup_image, 0, 2, metrics) &&
        cell_is_nonblank(narrow_setup_image, 0, 3, metrics) &&
        mostly_background(narrow_setup_image, 0, 4, 1, metrics),
        "narrow setup renders columns 2-3 before narrow-to-wide overwrite");

    const auto wide_cells = std::vector<term::Terminal_render_cell>{
        { {0, 0}, QStringLiteral("A"), 0U, 1, false, 0U },
        { {0, 1}, QStringLiteral("\u754c"), 0U, 2, false, 0U },
        { {0, 2}, {}, 0U, 0, true, 0U },
        { {0, 4}, QStringLiteral("D"), 0U, 1, false, 0U },
    };
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_unicode_snapshot(303U, wide_cells));
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        303U,
        1),
        "narrow-to-wide overwrite reports one rebuilt text row");
    ok &= check_surface_no_text_content_failures(surface,
        "narrow-to-wide overwrite has no text content failures");
    const QImage wide_image = window.grabWindow();
    ok &= check(cell_is_nonblank(wide_image, 0, 1, metrics) &&
        mostly_background(wide_image, 0, 3, 1, metrics) &&
        cell_is_nonblank(wide_image, 0, 4, metrics),
        "narrow-to-wide overwrite renders wide CJK and moves following ASCII");

    const auto narrow_with_blank_cells = std::vector<term::Terminal_render_cell>{
        { {0, 0}, QStringLiteral("A"), 0U, 1, false, 0U },
        { {0, 1}, QStringLiteral("B"), 0U, 1, false, 0U },
        { {0, 3}, QStringLiteral("D"), 0U, 1, false, 0U },
    };
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_unicode_snapshot(304U, narrow_with_blank_cells));
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        304U,
        1),
        "wide-to-narrow overwrite reports one rebuilt text row");
    ok &= check_surface_no_text_content_failures(surface,
        "wide-to-narrow overwrite has no text content failures");
    const QImage narrow_image = window.grabWindow();
    ok &= check(mostly_background(narrow_image, 0, 2, 1, metrics) &&
        cell_is_nonblank(narrow_image, 0, 3, metrics),
        "wide-to-narrow overwrite rebuilds text content without stale continuation pixels");

    const QString invalid = QString(QChar(0xd800));
    const auto invalid_cells = std::vector<term::Terminal_render_cell>{
        { {0, 0}, invalid, 0U, 1, false, 0U },
        { {0, 2}, QStringLiteral("Z"), 0U, 1, false, 0U },
    };
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(
        surface,
        make_unicode_snapshot(305U, invalid_cells));
    ok &= check(wait_rendered_sequence_with_text_rebuilds(
        app,
        window,
        surface,
        305U,
        1),
        "invalid glyph update reports one rebuilt text row");
    ok &= check_surface_no_text_content_failures(surface,
        "invalid glyph update has no text content failures");
    const QImage invalid_image = window.grabWindow();
    ok &= check(!invalid_image.isNull() &&
        mostly_background(invalid_image, 0, 1, 1, metrics) &&
        cell_is_nonblank(invalid_image, 0, 2, metrics) &&
        mostly_background(invalid_image, 0, 3, 1, metrics),
        "invalid glyph content does not crash and new following ASCII is aligned");
    ok &= check(surface.childItems().empty(), "unicode render keeps child item list empty");
    return ok;
}

bool test_qsg_slow_text_layout_diagnostics_recorder()
{
#if VNM_TERMINAL_PROFILING_ENABLED
    term::Terminal_text_layout_slow_diagnostics_recorder recorder;
    term::Terminal_render_text_run run;
    run.row          = 2;
    run.logical_row  = 14;
    run.column       = 7;
    run.rect         = QRectF(7.0, 2.0, 21.0, 14.0);
    run.text         = QString::fromUtf8("abc\n\xCE\xBB");
    run.style_id     = 3U;
    run.hyperlink_id = 42U;

    QFont font(QStringLiteral("Cascadia Mono"));
    font.setPointSizeF(11.0);
    font.setItalic(true);

    const std::uint64_t threshold = recorder.snapshot().threshold_ns;
    recorder.record_layout(
        threshold - 1U,
        font,
        run,
        true,
        true,
        false,
        true);
    term::terminal_text_layout_slow_diagnostics_t snapshot = recorder.snapshot();

    bool ok = check(snapshot.slow_call_count == 0U,
        "slow text layout recorder ignores below-threshold samples");
    ok &= check(snapshot.samples.empty(),
        "slow text layout recorder stores no below-threshold sample");

    recorder.record_layout(
        threshold,
        font,
        run,
        true,
        true,
        false,
        true);
    snapshot  = recorder.snapshot();
    ok       &= check(snapshot.slow_call_count == 1U,
        "slow text layout recorder counts threshold samples");
    ok       &= check(snapshot.samples.size() == 1U,
        "slow text layout recorder stores threshold sample");
    if (!snapshot.samples.empty()) {
        const term::terminal_text_layout_slow_diagnostic_t& sample =
            snapshot.samples.front();
        ok &= check(sample.duration_ns == threshold,
            "slow text layout diagnostic records duration");
        ok &= check(sample.row == run.row && sample.logical_row == run.logical_row &&
            sample.column == run.column,
            "slow text layout diagnostic records run position");
        ok &= check(sample.clipped && sample.force_blended_order &&
            !sample.ascii_layout_font,
            "slow text layout diagnostic records layout mode");
        ok &= check(!sample.ascii_only && !sample.printable_ascii_only &&
            sample.has_control_codepoint,
            "slow text layout diagnostic classifies text content");
        ok &= check(sample.text_preview == run.text &&
            !sample.codepoint_sample.isEmpty(),
            "slow text layout diagnostic records text samples");
        ok &= check(sample.font_family == font.family() && sample.font_italic,
            "slow text layout diagnostic records font metadata");
    }

    return ok;
#else
    return true;
#endif
}

}

int main(int argc, char** argv)
{
    const bool software_renderer = has_argument(argc, argv, "--software-renderer");
    const bool flat_rect_batched_geometry_only =
        has_argument(argc, argv, "--flat-rect-batched-geometry");
    const bool geometry_row_slot_hardware_only =
        has_argument(argc, argv, "--geometry-row-slot-hardware");
    if (software_renderer) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }

    QGuiApplication app(argc, argv);

    bool ok = test_qsg_renderer_source_posture();
    if (flat_rect_batched_geometry_only) {
        ok &= test_qsg_background_row_cache_uses_batched_geometry(app, true);
        ok &= test_qsg_selection_row_cache_uses_batched_geometry(app, true);
        ok &= test_qsg_graphic_row_cache_uses_batched_geometry(app, true);
        ok &= test_qsg_decoration_row_cache_uses_batched_geometry(app, true);
        ok &= test_qsg_background_stale_pixel_change_blank_scroll_identity(app, true);
        ok &= test_qsg_selection_stale_pixel_change_blank_scroll_identity(app, true);
        ok &= test_qsg_graphic_stale_pixel_change_blank_scroll_identity(app, true);
        ok &= test_qsg_decoration_stale_pixel_change_blank_scroll_identity(app, true);
        return ok ? 0 : 1;
    }
    if (geometry_row_slot_hardware_only) {
        QQuickWindow probe_window;
        probe_window.resize(64, 64);
        probe_window.show();
        pump_events(app, 2);
        const bool software_scene_graph =
            window_uses_software_scene_graph(probe_window);
        ok &= check(!software_scene_graph,
            "geometry row-slot hardware gate requires a non-software scene graph");
        probe_window.hide();
        pump_events(app, 1);
        if (!software_scene_graph) {
            ok &= test_qsg_geometry_row_slot_active_buffer_identity(app);
            ok &= test_qsg_geometry_row_slot_exact_content_descriptors(app);
        }
        return ok ? 0 : 1;
    }

    ok &= test_qsg_slow_text_layout_diagnostics_recorder();
    ok &= test_qsg_invalidation_coalescing(app);
    ok &= test_qsg_coalesced_dirty_snapshots_rebuild_skipped_rows(app);
    ok &= test_qsg_coalesced_style_only_transition_repaints_skipped_row(app);
    ok &= test_qsg_detached_snapshot_updates(app);
    ok &= test_qsg_snapshot_rendering(app);
    ok &= test_qsg_text_leaf_batching(app);
    ok &= test_qsg_text_resource_key_ignores_hyperlink_metadata(app);
    ok &= test_qsg_text_resource_descriptor_ignores_decoration_metadata(app);
    ok &= test_qsg_packed_sidecars_do_not_affect_visual_output(app);
    ok &= test_qsg_text_coalescing_rejects_cell_width_drift(app);
    ok &= test_qsg_text_resource_key_separates_coalesced_ascii_from_original_runs(app);
    ok &= test_qsg_background_selection_row_cache(app);
    ok &= test_qsg_background_row_cache_uses_batched_geometry(app, false);
    ok &= test_qsg_selection_row_cache_uses_batched_geometry(app, false);
    ok &= test_qsg_graphic_row_cache_uses_batched_geometry(app, false);
    ok &= test_qsg_decoration_row_cache_uses_batched_geometry(app, false);
    ok &= test_qsg_background_row_cache_mixed_rect_fallback(app);
    ok &= test_qsg_background_selection_row_cache_viewport_move_reuses_logical_row(app);
    ok &= test_qsg_background_selection_row_cache_scroll_pixels(app);
    ok &= test_qsg_background_stale_pixel_change_blank_scroll_identity(app, false);
    ok &= test_qsg_selection_stale_pixel_change_blank_scroll_identity(app, false);
    ok &= test_qsg_graphic_stale_pixel_change_blank_scroll_identity(app, false);
    ok &= test_qsg_decoration_stale_pixel_change_blank_scroll_identity(app, false);
    ok &= test_qsg_row_cache_scroll_removal_reorder_pixels_and_lifecycle(app);
    ok &= test_qsg_row_cache_flat_fallback_transition_clears_pixels_and_lifecycle(app);
    ok &= test_qsg_grid_resize_full_dirty_cached_rows(app);
    ok &= test_qsg_decoration_row_cache(app);
    ok &= test_qsg_selection_decoration_layer_order_and_strike_pixels(app);
    ok &= test_qsg_graphic_rect_arc_row_cache(app);
    ok &= test_qsg_geometry_row_slot_active_buffer_identity(app);
    ok &= test_qsg_geometry_row_slot_exact_content_descriptors(app);
    ok &= test_qsg_rect_row_cache_coalesces_adjacent_simple_rects(app);
    ok &= test_qsg_row_cache_empty_dirty_input_key_gate_safety(app);
    ok &= test_qsg_graphic_arc_fallback_pixels_and_order(app);
    ok &= test_qsg_graphic_rect_row_cache_mixed_fallback(app);
    ok &= test_qsg_graphic_rect_antialias_frame_route_recovers(app);
    ok &= test_qsg_rect_layer_cache_key_invariants(app);
    ok &= test_qsg_text_rows_clip_to_terminal_row(app);
    ok &= test_qsg_clipped_text_run_transition_clears_pixels(app);
    ok &= test_qsg_text_resource_removal_clears_pixels_across_frames(app);
    ok &= test_qsg_viewport_scroll_reuses_text_resources(app);
    ok &= test_qsg_text_resource_descriptor_dirty_reuse(app);
    ok &= test_qsg_text_resource_descriptor_styled_ascii_reuse(app);
    ok &= test_qsg_text_resource_descriptor_complex_transitions(app);
    ok &= test_qsg_text_resource_descriptor_keeps_cursor_text_separate(app);
    ok &= test_qsg_text_resource_descriptor_rejects_preedit_and_clipped_rows(app);
    ok &= test_qsg_text_row_slot_active_buffer_identity(app);
    ok &= test_qsg_text_row_slot_active_buffer_roundtrip_pixels(app);
    ok &= test_qsg_text_row_slot_viewport_order_and_resize_cleanup(app);
    ok &= test_qsg_text_row_slot_dirty_clean_skip_safety(app);
    ok &= test_qsg_text_row_slot_exact_content_descriptors(app);
    ok &= test_qsg_block_cursor_text_overlay_paths(app);
    ok &= test_qsg_wide_block_cursor_overlay(app);
    ok &= test_qsg_active_preedit_snapshot_rendering(app);
    ok &= test_qsg_block_cursor_over_terminal_graphic(app);
    ok &= test_qsg_rect_resource_lifecycle_direct_cleanup_paths(app);
    ok &= test_qsg_resource_lifecycle(app);
    ok &= test_qsg_item_teardown_with_resources(app);
    ok &= test_qsg_terminal_graphics_rendering(app);
    ok &= test_qsg_terminal_graphics_pixel_transitions(app);
    ok &= test_qsg_unicode_rendering(app);
    return ok ? 0 : 1;
}
