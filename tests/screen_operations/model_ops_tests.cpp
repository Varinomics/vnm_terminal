#include "vnm_terminal/internal/terminal_screen_model.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QString>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <variant>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

[[noreturn]] void fail_required_value(const char* message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

QByteArray bytes_from_hex(const char* hex)
{
    return QByteArray::fromHex(QByteArray(hex));
}

term::Terminal_screen_model make_model(int rows = 4, int columns = 8)
{
    return term::Terminal_screen_model({
        term::terminal_grid_size_t{rows, columns},
        8,
        4,
    });
}

int diagnostic_count(const term::Terminal_screen_model_result& result)
{
    int count = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::DIAGNOSTIC) {
            ++count;
        }
    }
    return count;
}

const term::Parser_payload_diagnostic& first_diagnostic(
    const term::Terminal_screen_model_result& result)
{
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::DIAGNOSTIC) {
            return std::get<term::Parser_payload_diagnostic>(action.payload);
        }
    }

    fail_required_value("expected diagnostic action");
}

bool dirty_rows_equal(
    const term::Terminal_screen_model_result&  result,
    std::initializer_list<int>                 rows,
    const char*                                label)
{
    return check(result.dirty_rows == std::vector<int>(rows), label);
}

bool dirty_rows_equal(
    const term::Terminal_screen_model_result&  result,
    const std::vector<int>&                    rows,
    const char*                                label)
{
    return check(result.dirty_rows == rows, label);
}

std::vector<int> row_range(int row_count)
{
    std::vector<int> rows;
    rows.reserve(static_cast<std::size_t>(row_count));
    for (int row = 0; row < row_count; ++row) {
        rows.push_back(row);
    }
    return rows;
}

bool snapshot_valid(const term::Terminal_screen_model& model, std::uint64_t sequence)
{
    return term::validate_render_snapshot(model.render_snapshot(sequence)).status ==
        term::Terminal_render_snapshot_status::OK;
}

term::Terminal_render_snapshot_request request_for_model(
    const term::Terminal_screen_model&     model,
    std::uint64_t                          sequence,
    int                                    offset_from_tail = 0)
{
    term::Terminal_viewport_state viewport;
    viewport.active_buffer = model.active_buffer_id();
    viewport.visible_rows  = model.grid_size().rows;
    viewport.scrollback_rows =
        model.active_buffer_id() == term::Terminal_buffer_id::PRIMARY
            ? model.scrollback_size()
            : 0;
    viewport.offset_from_tail = offset_from_tail;
    viewport.follow_tail      = offset_from_tail == 0;

    term::Terminal_render_snapshot_request request;
    request.sequence = sequence;
    request.viewport = viewport;
    return request;
}

QString snapshot_row_text(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row)
{
    QString text;
    for (int column = 0; column < snapshot.grid_size.columns; ++column) {
        QString cell_text = QStringLiteral(" ");
        for (const term::Terminal_render_cell& cell : snapshot.cells) {
            if (cell.position.row == row && cell.position.column == column) {
                cell_text = cell.text;
                break;
            }
        }
        text += cell_text;
    }

    while (!text.isEmpty() && text.back() == QChar(u' ')) {
        text.chop(1);
    }
    return text;
}

std::vector<term::Terminal_reply> replies_in(
    const term::Terminal_screen_model_result& result)
{
    std::vector<term::Terminal_reply> replies;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::TERMINAL_REPLY) {
            replies.push_back(std::get<term::Terminal_reply>(action.payload));
        }
    }
    return replies;
}

std::vector<term::Parser_notification> notifications_in(
    const term::Terminal_screen_model_result& result)
{
    std::vector<term::Parser_notification> notifications;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::NOTIFICATION) {
            notifications.push_back(std::get<term::Parser_notification>(action.payload));
        }
    }
    return notifications;
}

const term::Terminal_reply& reply_at(
    const std::vector<term::Terminal_reply>&   replies,
    std::size_t                                index)
{
    if (index >= replies.size()) {
        fail_required_value("expected terminal reply");
    }

    return replies[index];
}

const term::Terminal_render_cell& cell_at(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    column)
{
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row == row && cell.position.column == column) {
            return cell;
        }
    }

    fail_required_value("expected render cell");
}

const term::Terminal_render_cell* cell_at_or_null(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    column)
{
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row == row && cell.position.column == column) {
            return &cell;
        }
    }

    return nullptr;
}

bool check_cell_background_palette(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    column,
    std::uint16_t                          palette_index,
    const char*                            label)
{
    bool ok = true;
    const term::Terminal_render_cell* cell = cell_at_or_null(snapshot, row, column);
    ok &= check(cell != nullptr, label);
    if (cell == nullptr) {
        return false;
    }

    ok &= check(static_cast<std::size_t>(cell->style_id) < snapshot.styles.size(), label);
    if (static_cast<std::size_t>(cell->style_id) >= snapshot.styles.size()) {
        return false;
    }

    const term::Terminal_text_style& style =
        snapshot.styles[static_cast<std::size_t>(cell->style_id)];
    ok &= check(
        style.background.kind == term::Terminal_color_ref_kind::PALETTE_INDEX &&
            style.background.palette_index == palette_index,
        label);
    return ok;
}

bool check_row_background_palette(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    first_column,
    int                                    column_count,
    std::uint16_t                          palette_index,
    const char*                            label)
{
    bool ok = true;
    for (int column = first_column; column < first_column + column_count; ++column) {
        ok &= check_cell_background_palette(snapshot, row, column, palette_index, label);
    }
    return ok;
}

bool test_cursor_addressing_and_split_csi()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(3, 5);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("A\x1b[2;3HB"));
    ok &= check(diagnostic_count(result) == 0, "CUP has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("A"), "CUP leaves first row");
    ok &= check(model.row_text(1) == QStringLiteral("  B"), "CUP writes addressed row");
    ok &= check(model.cursor_position().row == 1 && model.cursor_position().column == 3,
        "CUP cursor advances after write");

    result  = model.ingest(QByteArray("\x1b[", 2));
    ok     &= check(diagnostic_count(result) == 0, "split CSI prefix waits");
    ok     &= check(model.cursor_position().row == 1 && model.cursor_position().column == 3,
        "split CSI prefix does not move cursor");

    result  = model.ingest(QByteArrayLiteral("3;99HC"));
    ok     &= check(diagnostic_count(result) == 0, "split CUP completes");
    ok     &= check(model.row_text(2) == QStringLiteral("    C"), "CUP clamps column");
    ok     &= check(model.cursor_position().row == 2 && model.cursor_position().column == 4,
        "CUP clamps cursor position");
    ok     &= check(snapshot_valid(model, 10U), "CUP snapshot validates");

    term::Terminal_screen_model relative_model = make_model(3, 5);
    result  = relative_model.ingest(QByteArrayLiteral("\x1b[2;2H\x1b[CX\x1b[2DY\x1b[BZ\x1b[2AC"));
    ok     &= check(diagnostic_count(result) == 0, "relative cursor movement has no diagnostics");
    ok     &= check(relative_model.row_text(0) == QStringLiteral("   C"),
        "relative CUU/CUF/CUB writes first row");
    ok     &= check(relative_model.row_text(1) == QStringLiteral(" YX"),
        "relative CUF/CUB writes middle row");
    ok     &= check(relative_model.row_text(2) == QStringLiteral("  Z"),
        "relative CUD writes lower row");
    ok     &= check(relative_model.cursor_position().row == 0 &&
        relative_model.cursor_position().column == 4,
        "relative cursor movement leaves cursor after final write");

    term::Terminal_screen_model scroll_region_relative_model = make_model(5, 5);
    result  = scroll_region_relative_model.ingest(
        QByteArrayLiteral("\x1b[2;4r\x1b[2;3H\x1b[10AX\x1b[4;1H\x1b[10BY"));
    ok     &= check(diagnostic_count(result) == 0,
        "scroll-region relative cursor movement has no diagnostics");
    ok     &= check(scroll_region_relative_model.row_text(1) == QStringLiteral("  X"),
        "CUU stops at the scroll-region top margin");
    ok     &= check(scroll_region_relative_model.row_text(3) == QStringLiteral("Y"),
        "CUD stops at the scroll-region bottom margin");

    term::Terminal_screen_model outside_region_relative_model = make_model(5, 5);
    result  = outside_region_relative_model.ingest(
        QByteArrayLiteral("\x1b[2;4r\x1b[1;1H\x1b[10BZ"));
    ok     &= check(diagnostic_count(result) == 0,
        "outside-scroll-region relative cursor movement has no diagnostics");
    ok     &= check(outside_region_relative_model.row_text(4) == QStringLiteral("Z"),
        "CUD outside the scroll region still clamps to the screen");

    term::Terminal_screen_model horizontal_model = make_model(2, 5);
    result  = horizontal_model.ingest(QByteArrayLiteral("abcde\x1b[2;3H\x1b[GZ\x1b[999GY"));
    ok     &= check(diagnostic_count(result) == 0, "CHA has no diagnostics");
    ok     &= check(horizontal_model.row_text(0) == QStringLiteral("abcde"),
        "CHA preserves previous row content");
    ok     &= check(horizontal_model.row_text(1) == QStringLiteral("Z   Y"),
        "CHA addresses absolute columns on the current row");
    ok     &= check(horizontal_model.cursor_position().row == 1 &&
        horizontal_model.cursor_position().column == 4,
        "CHA clamps the target column");

    term::Terminal_screen_model hvp_model = make_model(3, 5);
    result  = hvp_model.ingest(QByteArrayLiteral("\x1b[2;2fZ"));
    ok     &= check(diagnostic_count(result) == 0, "HVP has no diagnostics");
    ok     &= check(hvp_model.row_text(1) == QStringLiteral(" Z"), "HVP writes addressed row");
    ok     &= dirty_rows_equal(result, {0, 1}, "HVP dirties old and new cursor rows");
    ok     &= check(snapshot_valid(hvp_model, 11U), "HVP snapshot validates");

    term::Terminal_screen_model direct_model = make_model(3, 5);
    direct_model.apply_action(
        term::make_csi_dispatch_action({2, 3}, QByteArrayLiteral("H")));
    direct_model.ingest(QByteArrayLiteral("Q"));
    ok &= check(direct_model.row_text(1) == QStringLiteral("  Q"),
        "direct CSI action carries parameters");

    term::Terminal_screen_model c0_model = make_model(3, 5);
    result  = c0_model.ingest(QByteArrayLiteral("A\x1b[\n"));
    ok     &= check(diagnostic_count(result) == 0, "split CSI with C0 waits after executing C0");
    result  = c0_model.ingest(QByteArrayLiteral("qB"));
    ok     &= check(diagnostic_count(result) == 1, "CSI with C0 emits unsupported final diagnostic");
    ok     &= check(c0_model.row_text(0) == QStringLiteral("A"),
        "CSI C0 preserves text before line feed");
    ok     &= check(c0_model.row_text(1) == QStringLiteral(" B"),
        "CSI C0 executes line feed while collecting CSI");
    ok     &= check(snapshot_valid(c0_model, 12U), "CSI C0 snapshot validates");

    return ok;
}

bool test_erase_operations_and_wide_damage()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(2, 6);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("abcdef\x1b[2;1Hghijkl\x1b[1;4H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[J"));
    ok     &= check(diagnostic_count(result) == 0, "ED has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("abc"), "ED clears after cursor row");
    ok     &= check(model.row_text(1).isEmpty(), "ED clears following rows");
    ok     &= dirty_rows_equal(result, {0, 1}, "ED suffix dirty rows");
    ok     &= check(snapshot_valid(model, 12U), "ED suffix snapshot validates");

    model = make_model(1, 6);
    model.ingest(QByteArrayLiteral("abcdef\x1b[1;3H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[K"));
    ok     &= check(diagnostic_count(result) == 0, "EL has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("ab"), "EL clears row suffix");
    ok     &= dirty_rows_equal(result, {0}, "EL suffix dirty rows");
    ok     &= check(!result.viewport_changed, "EL suffix does not change viewport");
    ok     &= check(snapshot_valid(model, 14U), "EL suffix snapshot validates");

    model = make_model(2, 6);
    model.ingest(QByteArrayLiteral("abcdef\x1b[2;1Hghijkl\x1b[2;3H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[1J"));
    ok     &= check(diagnostic_count(result) == 0, "ED prefix has no diagnostics");
    ok     &= check(model.row_text(0).isEmpty(), "ED prefix clears earlier rows");
    ok     &= check(model.row_text(1) == QStringLiteral("   jkl"), "ED prefix clears through cursor");
    ok     &= dirty_rows_equal(result, {0, 1}, "ED prefix dirty rows");
    ok     &= check(!result.viewport_changed, "ED prefix does not change viewport");
    ok     &= check(snapshot_valid(model, 15U), "ED prefix snapshot validates");

    model = make_model(2, 6);
    model.ingest(QByteArrayLiteral("abcdef\x1b[2;1Hghijkl\x1b[1;4H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[2J"));
    ok     &= check(diagnostic_count(result) == 0, "ED full has no diagnostics");
    ok     &= check(model.row_text(0).isEmpty() && model.row_text(1).isEmpty(),
        "ED full clears visible screen");
    ok     &= dirty_rows_equal(result, {0, 1}, "ED full dirty rows");
    ok     &= check(!result.viewport_changed, "ED full does not change viewport");
    ok     &= check(snapshot_valid(model, 16U), "ED full snapshot validates");

    model = make_model(2, 4);
    model.ingest(QByteArrayLiteral("1\r\n2\r\n3"));
    ok     &= check(model.scrollback_size() > 0, "ED3 setup has scrollback");
    result  = model.ingest(QByteArrayLiteral("\x1b[3J"));
    ok     &= check(diagnostic_count(result) == 0, "ED3 has no diagnostics");
    ok     &= check(model.scrollback_size() == 0, "ED3 clears scrollback");
    ok     &= check(result.dirty_rows.empty(), "ED3 does not dirty visible rows");
    ok     &= check(result.viewport_changed, "ED3 reports viewport change");
    ok     &= check(snapshot_valid(model, 17U), "ED3 snapshot validates");

    model = make_model(1, 6);
    model.ingest(QByteArrayLiteral("abcdef\x1b[1;4H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[1K"));
    ok     &= check(diagnostic_count(result) == 0, "EL prefix has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("    ef"), "EL prefix clears through cursor");
    ok     &= dirty_rows_equal(result, {0}, "EL prefix dirty rows");
    ok     &= check(snapshot_valid(model, 18U), "EL prefix snapshot validates");

    model = make_model(1, 6);
    model.ingest(QByteArrayLiteral("abcdef\x1b[1;4H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[2K"));
    ok     &= check(diagnostic_count(result) == 0, "EL full has no diagnostics");
    ok     &= check(model.row_text(0).isEmpty(), "EL full clears row");
    ok     &= dirty_rows_equal(result, {0}, "EL full dirty rows");
    ok     &= check(snapshot_valid(model, 19U), "EL full snapshot validates");

    model = make_model(1, 6);
    model.ingest(QByteArrayLiteral("abcdef\x1b[1;3H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[2X"));
    ok     &= check(diagnostic_count(result) == 0, "ECH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("ab  ef"),
        "ECH clears characters without shifting");
    ok     &= check(model.cursor_position().row == 0 && model.cursor_position().column == 2,
        "ECH does not move the cursor");
    ok     &= dirty_rows_equal(result, {0}, "ECH dirty rows");
    ok     &= check(snapshot_valid(model, 24U), "ECH snapshot validates");

    model   = make_model(1, 5);
    result  = model.ingest(bytes_from_hex("e4b880") + QByteArrayLiteral("AB"));
    ok     &= check(diagnostic_count(result) == 0, "wide setup has no diagnostics");
    result  = model.ingest(QByteArrayLiteral("\x1b[1;2H\x1b[K"));
    ok     &= check(diagnostic_count(result) == 0, "wide EL has no diagnostics");
    ok     &= check(model.row_text(0).isEmpty(), "EL touching continuation clears wide cell");
    ok     &= check(snapshot_valid(model, 13U), "wide erase snapshot validates");

    model = make_model(1, 5);
    model.ingest(bytes_from_hex("e4b880") + QByteArrayLiteral("AB\x1b[1;2H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[X"));
    ok     &= check(diagnostic_count(result) == 0, "wide ECH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("  AB"),
        "ECH touching continuation clears the full wide cell");
    ok     &= check(snapshot_valid(model, 25U), "wide ECH snapshot validates");

    return ok;
}

bool test_scroll_region_and_origin_mode()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(4, 4);
    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"));
    ok &= check(diagnostic_count(result) == 0, "scroll setup has no diagnostics");

    model.ingest(QByteArrayLiteral("\x1b[2;3r\x1b[3;1H"));
    result  = model.ingest(QByteArrayLiteral("\n"));
    ok     &= check(diagnostic_count(result) == 0, "DECSTBM region scroll has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("111"), "region scroll preserves row above");
    ok     &= check(model.row_text(1) == QStringLiteral("333"), "region scroll moves region up");
    ok     &= check(model.row_text(2).isEmpty(), "region scroll clears bottom row");
    ok     &= check(model.row_text(3) == QStringLiteral("444"), "region scroll preserves row below");
    ok     &= check(model.scrollback_size() == 0, "partial region scroll does not append scrollback");
    ok     &= dirty_rows_equal(result, {1, 2}, "region scroll dirty rows");
    ok     &= check(snapshot_valid(model, 20U), "region scroll snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[?6h\x1b[1;1HX"));
    ok     &= check(diagnostic_count(result) == 0, "DECOM enable has no diagnostics");
    ok     &= check(model.row_text(1) == QStringLiteral("X33"), "origin mode addresses inside region");
    ok     &= check(snapshot_valid(model, 21U), "origin mode snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[?6;9999h\x1b[1;2HY"));
    ok     &= check(diagnostic_count(result) == 1, "mixed DECSET diagnoses unsupported mode");
    ok     &= check(model.row_text(1) == QStringLiteral("XY3"),
        "mixed DECSET still applies supported origin mode");
    ok     &= check(snapshot_valid(model, 23U), "mixed DECSET snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[?6l\x1b[H Y"));
    ok     &= check(diagnostic_count(result) == 0, "DECOM reset has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral(" Y1"),
        "origin reset addresses absolute home");
    ok     &= check(snapshot_valid(model, 22U), "origin reset snapshot validates");

    return ok;
}

bool test_top_anchored_scroll_region_appends_scrollback()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(4, 4);
    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"));
    ok &= check(diagnostic_count(result) == 0,
        "top-anchored scroll region setup has no diagnostics");

    model.ingest(QByteArrayLiteral("\x1b[1;2r\x1b[2;1H"));
    result  = model.ingest(QByteArrayLiteral("\n"));
    ok     &= check(diagnostic_count(result) == 0,
        "top-anchored scroll region has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("222"),
        "top-anchored scroll region moves row up");
    ok     &= check(model.row_text(1).isEmpty(),
        "top-anchored scroll region clears region bottom row");
    ok     &= check(model.row_text(2) == QStringLiteral("333"),
        "top-anchored scroll region preserves first row below region");
    ok     &= check(model.row_text(3) == QStringLiteral("444"),
        "top-anchored scroll region preserves second row below region");
    ok     &= check(model.scrollback_size() == 1,
        "top-anchored scroll region appends scrollback");
    ok     &= check(result.viewport_changed,
        "top-anchored scroll region reports viewport changed");
    ok     &= check(result.scrollback_rows == 1,
        "top-anchored scroll region result reports scrollback row count");
    ok     &= check(result.evicted_scrollback_rows == 0,
        "top-anchored scroll region does not evict scrollback");
    const term::Terminal_render_snapshot scrollback_snapshot =
        model.render_snapshot(request_for_model(model, 22U, 1));
    ok &= check(term::validate_render_snapshot(scrollback_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "top-anchored scrollback snapshot validates");
    ok &= check(snapshot_row_text(scrollback_snapshot, 0) == QStringLiteral("111"),
        "top-anchored scroll region appends the scrolled-out row");
    ok &= check(snapshot_row_text(scrollback_snapshot, 1) == QStringLiteral("222"),
        "top-anchored scrollback snapshot includes shifted active row");
    ok &= check(snapshot_row_text(scrollback_snapshot, 2).isEmpty(),
        "top-anchored scrollback snapshot includes cleared region bottom");
    ok &= check(snapshot_row_text(scrollback_snapshot, 3) == QStringLiteral("333"),
        "top-anchored scrollback snapshot includes row below region");
    ok &= dirty_rows_equal(result, {0, 1},
        "top-anchored scroll region dirty rows");
    ok &= check(snapshot_valid(model, 21U),
        "top-anchored scroll region snapshot validates");

    return ok;
}

bool test_alternate_top_anchored_scroll_region_does_not_append_scrollback()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(4, 4);
    model.ingest(QByteArrayLiteral("AAA\x1b[4;1H\n\x1b[1;1HPPP"));
    const int primary_scrollback_size = model.scrollback_size();
    const term::Terminal_render_snapshot primary_before =
        model.render_snapshot(request_for_model(model, 24U, 1));

    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[?1049h"
            "\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"));
    ok &= check(diagnostic_count(result) == 0,
        "alternate top-anchored setup has no diagnostics");

    model.ingest(QByteArrayLiteral("\x1b[1;2r\x1b[2;1H"));
    result  = model.ingest(QByteArrayLiteral("\n"));
    ok     &= check(diagnostic_count(result) == 0,
        "alternate top-anchored scroll region has no diagnostics");
    ok     &= check(model.active_buffer_id() == term::Terminal_buffer_id::ALTERNATE,
        "alternate top-anchored scroll stays in alternate buffer");
    ok     &= check(model.row_text(0) == QStringLiteral("222"),
        "alternate top-anchored scroll moves row up");
    ok     &= check(model.row_text(1).isEmpty(),
        "alternate top-anchored scroll clears region bottom row");
    ok     &= check(model.scrollback_size() == primary_scrollback_size,
        "alternate top-anchored scroll does not append primary scrollback");
    ok     &= check(!result.viewport_changed,
        "alternate top-anchored scroll does not report viewport changed");
    ok     &= check(result.evicted_scrollback_rows == 0,
        "alternate top-anchored scroll does not evict primary scrollback");
    ok     &= check(snapshot_valid(model, 23U),
        "alternate top-anchored scroll snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[?1049l"));
    ok     &= check(diagnostic_count(result) == 0,
        "alternate top-anchored leave has no diagnostics");
    ok     &= check(model.active_buffer_id() == term::Terminal_buffer_id::PRIMARY,
        "alternate top-anchored leave returns to primary buffer");
    const term::Terminal_render_snapshot primary_after =
        model.render_snapshot(request_for_model(model, 25U, 1));
    ok &= check(snapshot_row_text(primary_after, 0) ==
        snapshot_row_text(primary_before, 0),
        "alternate top-anchored scroll preserves primary scrollback row");
    ok &= check(snapshot_row_text(primary_after, 1) ==
        snapshot_row_text(primary_before, 1),
        "alternate top-anchored scroll preserves primary visible row");

    return ok;
}

bool test_escape_index_controls()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(3, 5);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("A\x1b" "DB\x1b" "EC"));
    ok &= check(diagnostic_count(result) == 0,
        "ESC IND and NEL have no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("A"), "IND preserves current column");
    ok &= check(model.row_text(1) == QStringLiteral(" B"), "IND moves down without CR");
    ok &= check(model.row_text(2) == QStringLiteral("C"), "NEL moves to next line home");
    ok &= check(snapshot_valid(model, 26U), "IND/NEL snapshot validates");

    model = make_model(4, 5);
    model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"
            "\x1b[2;4r\x1b[2;1H"));
    result  = model.ingest(QByteArrayLiteral("\x1b" "M"));
    ok     &= check(diagnostic_count(result) == 0, "RI has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("111"), "RI preserves row above region");
    ok     &= check(model.row_text(1).isEmpty(), "RI blanks scroll-region top row");
    ok     &= check(model.row_text(2) == QStringLiteral("222"), "RI shifts top row down");
    ok     &= check(model.row_text(3) == QStringLiteral("333"), "RI drops bottom row");
    ok     &= check(model.scrollback_size() == 0, "RI does not append scrollback");
    ok     &= check(!result.viewport_changed, "RI does not report viewport change");
    ok     &= dirty_rows_equal(result, {1, 2, 3}, "RI dirty rows");
    ok     &= check(snapshot_valid(model, 27U), "RI snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[4;1H\x1b" "MZ"));
    ok     &= check(diagnostic_count(result) == 0, "RI away from top margin has no diagnostics");
    ok     &= check(model.row_text(2) == QStringLiteral("Z22"),
        "RI away from top margin moves cursor up");
    ok     &= check(snapshot_valid(model, 28U), "RI cursor movement snapshot validates");

    return ok;
}

bool test_scroll_region_history_insert_after_reverse_index()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(5, 8);
    model.ingest(
        QByteArrayLiteral("\x1b[1;1Htop-one"
            "\x1b[2;1Htop-two"
            "\x1b[3;1Hview"
            "\x1b[4;1Hbelow"
            "\x1b[5;1Hprompt"));

    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[4;5r\x1b[4;1H\x1b" "M"
            "\x1b[r"
            "\x1b[1;4r\x1b[3;1H\r\nHIST"
            "\x1b[r\x1b[5;1H"));

    ok &= check(diagnostic_count(result) == 0,
        "scroll-region first history insert has no diagnostics");
    ok &= check(model.scrollback_size() == 0,
        "scroll-region first history insert does not append before overflow");
    ok &= check(!result.viewport_changed,
        "scroll-region first history insert does not report viewport changed");
    ok &= check(model.row_text(3) == QStringLiteral("HIST"),
        "scroll-region first history insert writes inserted row");

    result = model.ingest(
        QByteArrayLiteral("\x1b[1;4r\x1b[4;1H\r\nNEXT"
            "\x1b[r\x1b[5;1H"));

    ok &= check(diagnostic_count(result) == 0,
        "scroll-region overflowing history insert has no diagnostics");
    ok &= check(model.scrollback_size() == 1,
        "scroll-region overflowing history insert appends scrollback");
    ok &= check(result.viewport_changed,
        "scroll-region overflowing history insert reports viewport changed");
    const term::Terminal_render_snapshot scrollback_snapshot =
        model.render_snapshot(request_for_model(model, 29U, 1));
    ok &= check(term::validate_render_snapshot(scrollback_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "scroll-region overflowing history insert snapshot validates");
    ok &= check(snapshot_row_text(scrollback_snapshot, 0) == QStringLiteral("top-one"),
        "scroll-region overflowing history insert keeps scrolled row in scrollback");
    ok &= check(snapshot_row_text(scrollback_snapshot, 1) == QStringLiteral("top-two"),
        "scroll-region overflowing history insert shifts following row into viewport");
    ok &= check(snapshot_row_text(scrollback_snapshot, 4) == QStringLiteral("NEXT"),
        "scroll-region overflowing history insert writes inserted row");

    return ok;
}

bool test_conpty_primary_repaint_recovery_preserves_scrollback()
{
    bool ok = true;

    term::Terminal_screen_model model(
        {
            term::terminal_grid_size_t{5, 20},
            8,
            4,
            true,
            });
    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[?25l\x1b[2J\x1b[m\x1b[H"
            "top-one\r\n"
            "top-two\r\n"
            "view\r\n"
            "HIST\r\n"
            "below\x1b[?25h"));
    ok &= check(diagnostic_count(result) == 0,
        "ConPTY repaint recovery prefill has no diagnostics");

    result = model.ingest(
        QByteArrayLiteral("\x1b[?25l\x1b[H"
            "top-two\x1b[K\r\n"
            "view\x1b[K\r\n"
            "HIST\x1b[K\r\n"
            "NEXT\x1b[K\r\n"
            "below\x1b[K\r\x1b[?25h"));

    ok &= check(diagnostic_count(result) == 0,
        "ConPTY repaint recovery has no diagnostics");
    ok &= check(model.scrollback_size() == 1,
        "ConPTY repaint recovery appends displaced top row");
    ok &= check(result.viewport_changed,
        "ConPTY repaint recovery reports viewport change");
    const term::Terminal_render_snapshot scrollback_snapshot =
        model.render_snapshot(request_for_model(model, 33U, 1));
    ok &= check(snapshot_row_text(scrollback_snapshot, 0) == QStringLiteral("top-one"),
        "ConPTY repaint recovery keeps old top row in scrollback");
    ok &= check(snapshot_row_text(scrollback_snapshot, 1) == QStringLiteral("top-two"),
        "ConPTY repaint recovery keeps current top row at tail");

    return ok;
}

bool test_primary_repaint_recovery_ignores_plain_home_repaint()
{
    bool ok = true;

    term::Terminal_screen_model model(
        {
            term::terminal_grid_size_t{5, 20},
            8,
            4,
            true,
            });
    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[H"
            "top-one\r\n"
            "top-two\r\n"
            "view\r\n"
            "HIST\r\n"
            "below"));
    ok &= check(diagnostic_count(result) == 0,
        "plain home repaint recovery prefill has no diagnostics");

    result = model.ingest(
        QByteArrayLiteral("\x1b[H"
            "top-two\x1b[K\r\n"
            "view\x1b[K\r\n"
            "HIST\x1b[K\r\n"
            "NEXT\x1b[K\r\n"
            "below\x1b[K\r"));

    ok &= check(diagnostic_count(result) == 0,
        "plain home repaint recovery candidate has no diagnostics");
    ok &= check(model.scrollback_size() == 0,
        "plain visible-cursor home repaint does not synthesize scrollback");
    ok &= check(!result.viewport_changed,
        "plain visible-cursor home repaint does not report viewport changed");
    ok &= check(model.row_text(0) == QStringLiteral("top-two"),
        "plain visible-cursor home repaint still updates top row");

    return ok;
}

bool test_primary_repaint_recovery_ignores_clear_after_home()
{
    bool ok = true;

    term::Terminal_screen_model model(
        {
            term::terminal_grid_size_t{5, 20},
            8,
            4,
            true,
            });
    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[?25l\x1b[H"
            "top-one\r\n"
            "top-two\r\n"
            "view\r\n"
            "HIST\r\n"
            "below"));
    ok &= check(diagnostic_count(result) == 0,
        "clear-after-home repaint setup has no diagnostics");

    result = model.ingest(
        QByteArrayLiteral("\x1b[?25l\x1b[H\x1b[J"
            "top-two\r\n"
            "view\r\n"
            "HIST\r\n"
            "NEXT\r\n"
            "below\x1b[?25h"));

    ok &= check(diagnostic_count(result) == 0,
        "clear-after-home repaint has no diagnostics");
    ok &= check(model.scrollback_size() == 0,
        "clear-after-home repaint does not synthesize scrollback");
    ok &= check(!result.viewport_changed,
        "clear-after-home repaint does not report viewport changed");
    ok &= check(model.row_text(0) == QStringLiteral("top-two"),
        "clear-after-home repaint still updates top row");

    return ok;
}

bool test_primary_repaint_recovery_survives_split_repaint()
{
    bool ok = true;

    term::Terminal_screen_model model(
        {
            term::terminal_grid_size_t{5, 20},
            8,
            4,
            true,
            });
    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[?25l\x1b[H"
            "top-one\r\n"
            "top-two\r\n"
            "view\r\n"
            "HIST\r\n"
            "below"));
    ok &= check(diagnostic_count(result) == 0,
        "split repaint recovery setup has no diagnostics");

    result  = model.ingest(QByteArrayLiteral("\x1b[?25l\x1b[Htop-two\x1b[K\r\n"));
    ok     &= check(diagnostic_count(result) == 0,
        "split repaint first chunk has no diagnostics");
    ok     &= check(model.scrollback_size() == 0,
        "split repaint first chunk does not append scrollback early");

    result  = model.ingest(QByteArrayLiteral("view\x1b[K\r\nHIST\x1b[K\r\n"));
    ok     &= check(diagnostic_count(result) == 0,
        "split repaint second chunk has no diagnostics");
    ok     &= check(model.scrollback_size() == 0,
        "split repaint second chunk still waits for enough evidence");

    result  = model.ingest(QByteArrayLiteral("NEXT\x1b[K\r\nbelow\x1b[K\r\x1b[?25h"));
    ok     &= check(diagnostic_count(result) == 0,
        "split repaint final chunk has no diagnostics");
    ok     &= check(model.scrollback_size() == 1,
        "split repaint recovery appends displaced top row");
    ok     &= check(result.viewport_changed,
        "split repaint recovery reports viewport change on final chunk");
    const term::Terminal_render_snapshot scrollback_snapshot =
        model.render_snapshot(request_for_model(model, 35U, 1));
    ok &= check(snapshot_row_text(scrollback_snapshot, 0) == QStringLiteral("top-one"),
        "split repaint recovery keeps old top row in scrollback");

    return ok;
}

bool test_primary_repaint_recovery_ignores_synchronized_repaint()
{
    bool ok = true;

    term::Terminal_screen_model model(
        {
            term::terminal_grid_size_t{5, 20},
            8,
            4,
            true,
            });
    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[?25l\x1b[2J\x1b[H"
            "top-one\r\n"
            "top-two\r\n"
            "view\r\n"
            "HIST\r\n"
            "below"));
    ok &= check(diagnostic_count(result) == 0,
        "synchronized repaint recovery setup has no diagnostics");

    result = model.ingest(
        QByteArrayLiteral("\x1b[?2026h\x1b[H"
            "top-two\x1b[K\r\n"
            "view\x1b[K\r\n"
            "HIST\x1b[K\r\n"
            "NEXT\x1b[K\r\n"
            "below\x1b[K\r\x1b[?25h\x1b[?2026l"));

    ok &= check(diagnostic_count(result) == 0,
        "synchronized home repaint has no diagnostics");
    ok &= check(model.scrollback_size() == 0,
        "synchronized home repaint does not synthesize scrollback");
    ok &= check(model.row_text(0) == QStringLiteral("top-two"),
        "synchronized home repaint still updates the visible top row");

    model   = term::Terminal_screen_model({
        term::terminal_grid_size_t{5, 20},
        8,
        4,
        true,
    });
    result  = model.ingest(
        QByteArrayLiteral("\x1b[?25l\x1b[H"
            "top-one\r\n"
            "top-two\r\n"
            "view\r\n"
            "HIST\r\n"
            "below"));
    ok     &= check(diagnostic_count(result) == 0,
        "pre-armed synchronized repaint setup has no diagnostics");
    result  = model.ingest(QByteArrayLiteral("\x1b[H"));
    ok     &= check(diagnostic_count(result) == 0,
        "pre-armed synchronized repaint candidate has no diagnostics");

    result = model.ingest(
        QByteArrayLiteral("\x1b[?2026h"
            "top-two\x1b[K\r\n"
            "view\x1b[K\r\n"
            "HIST\x1b[K\r\n"
            "NEXT\x1b[K\r\n"
            "below\x1b[K\r\x1b[?2026l\x1b[?25h"));

    ok &= check(diagnostic_count(result) == 0,
        "pre-armed synchronized repaint has no diagnostics");
    ok &= check(model.scrollback_size() == 0,
        "entering synchronized output cancels pre-armed repaint recovery");

    return ok;
}

bool test_scroll_region_zero_bottom_defaults_to_screen_bottom()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(4, 5);
    model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"));

    const term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("\x1b[1;0r\x1b[4;1H\r\n555"));
    ok &= check(diagnostic_count(result) == 0,
        "zero-bottom scroll region has no diagnostics");
    ok &= check(model.scrollback_size() == 1,
        "zero-bottom scroll region uses screen bottom");
    ok &= check(model.row_text(0) == QStringLiteral("222"),
        "zero-bottom scroll region scrolls full screen");
    ok &= check(model.row_text(3) == QStringLiteral("555"),
        "zero-bottom scroll region writes after scrolling");
    const term::Terminal_render_snapshot scrollback_snapshot =
        model.render_snapshot(request_for_model(model, 34U, 1));
    ok &= check(snapshot_row_text(scrollback_snapshot, 0) == QStringLiteral("111"),
        "zero-bottom scroll region preserves scrolled row");

    return ok;
}

bool test_scroll_up_down_sequences()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(4, 5);
    model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"
            "\x1b[1;3r"));
    term::Terminal_screen_model_result result = model.ingest(QByteArrayLiteral("\x1b[2S"));

    ok &= check(diagnostic_count(result) == 0, "SU has no diagnostics");
    ok &= check(model.scrollback_size() == 2,
        "top-anchored SU appends scrolled rows to primary scrollback");
    ok &= check(result.viewport_changed, "top-anchored SU reports viewport changed");
    ok &= check(model.row_text(0) == QStringLiteral("333"), "SU shifts rows up by count");
    ok &= check(model.row_text(1).isEmpty(), "SU blanks first vacated bottom row");
    ok &= check(model.row_text(2).isEmpty(), "SU blanks second vacated bottom row");
    ok &= check(model.row_text(3) == QStringLiteral("444"), "SU preserves row below region");
    ok &= dirty_rows_equal(result, {0, 1, 2}, "SU dirty rows");
    const term::Terminal_render_snapshot scrollback_snapshot =
        model.render_snapshot(request_for_model(model, 30U, 2));
    ok &= check(snapshot_row_text(scrollback_snapshot, 0) == QStringLiteral("111"),
        "SU first scrolled row is in scrollback");
    ok &= check(snapshot_row_text(scrollback_snapshot, 1) == QStringLiteral("222"),
        "SU second scrolled row is in scrollback");
    ok &= check(snapshot_valid(model, 31U), "SU snapshot validates");

    model = make_model(4, 5);
    model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"
            "\x1b[1;3r"));
    result = model.ingest(QByteArrayLiteral("\x1b[2T"));

    ok &= check(diagnostic_count(result) == 0, "SD has no diagnostics");
    ok &= check(model.scrollback_size() == 0, "SD does not append scrollback");
    ok &= check(!result.viewport_changed, "SD does not report viewport changed");
    ok &= check(model.row_text(0).isEmpty(), "SD blanks first vacated top row");
    ok &= check(model.row_text(1).isEmpty(), "SD blanks second vacated top row");
    ok &= check(model.row_text(2) == QStringLiteral("111"), "SD shifts rows down by count");
    ok &= check(model.row_text(3) == QStringLiteral("444"), "SD preserves row below region");
    ok &= dirty_rows_equal(result, {0, 1, 2}, "SD dirty rows");
    ok &= check(snapshot_valid(model, 32U), "SD snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[1;2;3;4;5T"));
    ok     &= check(diagnostic_count(result) == 1,
        "XTHIMOUSE-shaped CSI T emits unsupported diagnostic");
    ok     &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "XTHIMOUSE-shaped CSI T diagnostic is unsupported");
    ok     &= check(model.row_text(2) == QStringLiteral("111"),
        "unsupported multi-parameter CSI T does not mutate rows");

    return ok;
}

bool test_blank_fill_operations_use_current_style()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(1, 8);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("\x1b[44mABCD\x1b[41m\x1b[1;2H\x1b[2@"));
    ok &= check(diagnostic_count(result) == 0, "styled ICH has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("A  BCD"),
        "styled ICH inserts blanks without losing text");
    term::Terminal_render_snapshot snapshot = model.render_snapshot(43U);
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled ICH snapshot validates");
    ok &= check_row_background_palette(
        snapshot,
        0,
        1,
        2,
        1U,
        "styled ICH inserted blanks keep current background");

    model     = make_model(1, 8);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[44mABCDEFGH\x1b[41m\x1b[1;3H\x1b[2P"));
    ok       &= check(diagnostic_count(result) == 0, "styled DCH has no diagnostics");
    ok       &= check(model.row_text(0) == QStringLiteral("ABEFGH"),
        "styled DCH deletes cells and fills the right edge");
    snapshot  = model.render_snapshot(44U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled DCH snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        0,
        6,
        2,
        1U,
        "styled DCH right-edge blanks keep current background");

    model     = make_model(4, 5);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333"
            "\x1b[4;1H444\x1b[2;4r\x1b[2;1H\x1b[L"));
    ok       &= check(diagnostic_count(result) == 0, "styled IL has no diagnostics");
    ok       &= check(model.row_text(1).isEmpty(), "styled IL blanks the exposed row");
    snapshot  = model.render_snapshot(45U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled IL snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        1,
        0,
        5,
        1U,
        "styled IL exposed row keeps current background");

    model     = make_model(4, 5);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333"
            "\x1b[4;1H444\x1b[2;4r\x1b[2;1H\x1b[M"));
    ok       &= check(diagnostic_count(result) == 0, "styled DL has no diagnostics");
    ok       &= check(model.row_text(3).isEmpty(), "styled DL blanks the exposed bottom row");
    snapshot  = model.render_snapshot(46U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled DL snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        3,
        0,
        5,
        1U,
        "styled DL exposed row keeps current background");

    model     = make_model(3, 4);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m\x1b[1;1H1111\x1b[2;1H2222"
            "\x1b[3;1H3333\x1b[S"));
    ok       &= check(diagnostic_count(result) == 0, "styled SU has no diagnostics");
    ok       &= check(model.row_text(2).isEmpty(), "styled SU blanks the exposed bottom row");
    snapshot  = model.render_snapshot(47U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled SU snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        2,
        0,
        4,
        1U,
        "styled SU exposed row keeps current background");

    model     = make_model(3, 4);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m\x1b[1;1H1111\x1b[2;1H2222"
            "\x1b[3;1H3333\x1b[T"));
    ok       &= check(diagnostic_count(result) == 0, "styled SD has no diagnostics");
    ok       &= check(model.row_text(0).isEmpty(), "styled SD blanks the exposed top row");
    snapshot  = model.render_snapshot(48U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled SD snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        0,
        0,
        4,
        1U,
        "styled SD exposed row keeps current background");

    model     = make_model(2, 4);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m\x1b[1;1H1111\x1b[2;1H2222"
            "\x1b[2;1H\x1b" "D"));
    ok       &= check(diagnostic_count(result) == 0, "styled IND scroll has no diagnostics");
    ok       &= check(model.row_text(1).isEmpty(), "styled IND scroll blanks the exposed row");
    snapshot  = model.render_snapshot(49U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled IND scroll snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        1,
        0,
        4,
        1U,
        "styled IND scroll exposed row keeps current background");

    model     = make_model(2, 4);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m\x1b[1;1H1111\x1b[2;1H2222"
            "\x1b[1;1H\x1b" "M"));
    ok       &= check(diagnostic_count(result) == 0, "styled RI scroll has no diagnostics");
    ok       &= check(model.row_text(0).isEmpty(), "styled RI scroll blanks the exposed row");
    snapshot  = model.render_snapshot(50U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled RI scroll snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        0,
        0,
        4,
        1U,
        "styled RI scroll exposed row keeps current background");

    model     = make_model(1, 6);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m") +
        bytes_from_hex("e4b880") +
        QByteArrayLiteral("AB\x1b[1;2H\x1b[@"));
    ok       &= check(diagnostic_count(result) == 0,
        "styled wide-boundary ICH has no diagnostics");
    ok       &= check(model.row_text(0) == QStringLiteral("   AB"),
        "styled wide-boundary ICH blanks the damaged wide cell");
    snapshot  = model.render_snapshot(51U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled wide-boundary ICH snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        0,
        0,
        3,
        1U,
        "styled wide-boundary ICH blanks keep current background");

    model     = make_model(1, 6);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m") +
        bytes_from_hex("e4b880") +
        QByteArrayLiteral("AB\x1b[1;2H\x1b[P"));
    ok       &= check(diagnostic_count(result) == 0,
        "styled wide-boundary DCH has no diagnostics");
    ok       &= check(model.row_text(0) == QStringLiteral(" AB"),
        "styled wide-boundary DCH blanks the damaged wide cell");
    snapshot  = model.render_snapshot(52U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled wide-boundary DCH snapshot validates");
    ok       &= check_cell_background_palette(
        snapshot,
        0,
        0,
        1U,
        "styled wide-boundary DCH blank keeps current background");

    return ok;
}

bool test_insert_delete_lines_cells_and_tabs()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(1, 8);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("abcdef\x1b[1;3H\x1b[2@"));
    ok &= check(diagnostic_count(result) == 0, "ICH has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("ab  cdef"), "ICH shifts cells right");
    ok &= dirty_rows_equal(result, {0}, "ICH dirty rows");
    ok &= check(snapshot_valid(model, 30U), "ICH snapshot validates");

    model   = make_model(1, 8);
    result  = model.ingest(QByteArrayLiteral("abcdef\x1b[1;3H\x1b[2P"));
    ok     &= check(diagnostic_count(result) == 0, "DCH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("abef"), "DCH shifts cells left");
    ok     &= dirty_rows_equal(result, {0}, "DCH dirty rows");
    ok     &= check(snapshot_valid(model, 31U), "DCH snapshot validates");

    model = make_model(1, 6);
    model.ingest(bytes_from_hex("e4b880") + QByteArrayLiteral("AB\x1b[1;1H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[@"));
    ok     &= check(diagnostic_count(result) == 0, "wide-preserving ICH has no diagnostics");
    ok     &= check(model.row_text(0) == QString::fromUtf8(" \xe4\xb8\x80" "AB"),
        "ICH before wide glyph preserves shifted wide cell");
    ok     &= check(snapshot_valid(model, 32U), "wide-preserving ICH snapshot validates");

    model = make_model(1, 6);
    model.ingest(bytes_from_hex("e4b880") + QByteArrayLiteral("AB\x1b[1;2H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[@"));
    ok     &= check(diagnostic_count(result) == 0, "wide ICH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("   AB"), "ICH touching continuation clears wide cell");
    ok     &= check(snapshot_valid(model, 33U), "wide ICH snapshot validates");

    model = make_model(1, 6);
    model.ingest(bytes_from_hex("e4b880") + QByteArrayLiteral("AB\x1b[1;2H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[P"));
    ok     &= check(diagnostic_count(result) == 0, "wide DCH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral(" AB"),
        "DCH touching continuation clears wide cell");
    ok     &= check(snapshot_valid(model, 34U), "wide DCH snapshot validates");

    model = make_model(1, 4);
    model.ingest(QByteArrayLiteral("AB") + bytes_from_hex("e4b880") + QByteArrayLiteral("\x1b[1;3H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[@"));
    ok     &= check(diagnostic_count(result) == 0, "right-edge wide ICH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("AB"),
        "ICH clipping right-edge wide glyph clears full span");
    ok     &= check(snapshot_valid(model, 38U), "right-edge wide ICH snapshot validates");

    model = make_model(1, 6);
    model.ingest(bytes_from_hex("e4b880") + QByteArrayLiteral("AB\x1b[1;2H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[2@"));
    ok     &= check(diagnostic_count(result) == 0, "multi-count wide ICH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("    AB"),
        "multi-count ICH touching continuation clears wide cell");
    ok     &= check(snapshot_valid(model, 39U), "multi-count wide ICH snapshot validates");

    model = make_model(1, 6);
    model.ingest(QByteArrayLiteral("AB") + bytes_from_hex("e4b880") +
        QByteArrayLiteral("C\x1b[1;2H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[P"));
    ok     &= check(diagnostic_count(result) == 0, "wide-shift DCH has no diagnostics");
    ok     &= check(model.row_text(0) == QString::fromUtf8("A\xe4\xb8\x80" "C"),
        "DCH shifts wide glyph left intact");
    {
        const term::Terminal_render_snapshot shifted = model.render_snapshot(40U);
        ok &= check(cell_at(shifted, 0, 1).display_width == 2 &&
            cell_at(shifted, 0, 2).wide_continuation,
            "DCH shifted wide glyph has continuation");
        ok &= check(term::validate_render_snapshot(shifted).status ==
            term::Terminal_render_snapshot_status::OK,
            "wide-shift DCH snapshot validates");
    }

    model = make_model(1, 6);
    model.ingest(QByteArrayLiteral("A") + bytes_from_hex("e4b880") +
        QByteArrayLiteral("BC\x1b[1;2H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[2P"));
    ok     &= check(diagnostic_count(result) == 0, "wide-span DCH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("ABC"),
        "multi-count DCH deletes full wide span");
    ok     &= check(snapshot_valid(model, 41U), "wide-span DCH snapshot validates");

    model = make_model(4, 5);
    model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"
            "\x1b[2;4r\x1b[2;1H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[L"));
    ok     &= check(diagnostic_count(result) == 0, "IL has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("111"), "IL preserves row above region");
    ok     &= check(model.row_text(1).isEmpty(), "IL blanks cursor row");
    ok     &= check(model.row_text(2) == QStringLiteral("222"), "IL shifts row down");
    ok     &= check(model.row_text(3) == QStringLiteral("333"), "IL drops bottom row");
    ok     &= dirty_rows_equal(result, {1, 2, 3}, "IL dirty rows");
    ok     &= check(snapshot_valid(model, 35U), "IL snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[2;1H\x1b[M"));
    ok     &= check(diagnostic_count(result) == 0, "DL has no diagnostics");
    ok     &= check(model.row_text(1) == QStringLiteral("222"), "DL shifts row up");
    ok     &= check(model.row_text(3).isEmpty(), "DL blanks bottom row");
    ok     &= dirty_rows_equal(result, {1, 2, 3}, "DL dirty rows");
    ok     &= check(snapshot_valid(model, 36U), "DL snapshot validates");

    model   = make_model(1, 8);
    result  = model.ingest(QByteArrayLiteral("A\tB"));
    ok     &= check(diagnostic_count(result) == 0, "HT has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("A   B"), "HT advances to tab stop");
    ok     &= check(model.render_snapshot(37U).cells.size() == 2U,
        "HT does not materialize intermediate blank cells");

    model   = make_model(1, 8);
    result  = model.ingest(QByteArrayLiteral("A\x1b[3g\tB"));
    ok     &= check(diagnostic_count(result) == 0, "TBC all has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("A      B"),
        "TBC all makes HT use right edge");
    ok     &= check(snapshot_valid(model, 38U), "TBC snapshot validates");

    return ok;
}

bool test_replies_and_cursor_save_restore()
{
    bool ok = true;

    auto check_rejected_csi_t_query = [&](const term::Terminal_screen_model_result& query_result,
        term::Parser_diagnostic_code expected_code,
        const char* label) {
        const std::vector<term::Terminal_reply> query_replies = replies_in(query_result);
        ok &= check(query_replies.empty(), label);
        ok &= check(diagnostic_count(query_result) == 1, label);
        ok &= check(first_diagnostic(query_result).code == expected_code, label);
    };

    term::Terminal_screen_model model = make_model(4, 5);
    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[2;3H\x1b[6nB\x1b[c\x1b[>c\x1b[?6$p"));
    ok &= check(diagnostic_count(result) == 0, "terminal queries have no diagnostics");

    const std::vector<term::Terminal_reply> replies = replies_in(result);
    ok &= check(replies.size() == 4U, "terminal queries emit four replies");
    const term::Terminal_reply& dsr_reply    = reply_at(replies, 0U);
    const term::Terminal_reply& da1_reply    = reply_at(replies, 1U);
    const term::Terminal_reply& da2_reply    = reply_at(replies, 2U);
    const term::Terminal_reply& decrqm_reply = reply_at(replies, 3U);
    ok &= check(dsr_reply.kind == term::Terminal_reply_kind::DSR_CURSOR_POSITION &&
        dsr_reply.wire_bytes == QByteArrayLiteral("\x1b[2;3R"),
        "DSR cursor reply");
    ok &= check(da1_reply.kind == term::Terminal_reply_kind::DA1 &&
        da1_reply.wire_bytes == QByteArrayLiteral("\x1b[?1;2c"),
        "DA1 reply");
    ok &= check(da2_reply.kind == term::Terminal_reply_kind::DA2 &&
        da2_reply.wire_bytes == QByteArrayLiteral("\x1b[>0;0;0c"),
        "DA2 reply");
    ok &= check(decrqm_reply.kind == term::Terminal_reply_kind::DECRQM &&
        decrqm_reply.wire_bytes == QByteArrayLiteral("\x1b[?6;2$y"),
        "DECRQM origin reset reply");
    ok &= check(snapshot_valid(model, 40U), "reply mixed snapshot validates");

    model = make_model(4, 5);
    result = model.ingest(QByteArrayLiteral("\x1b[2;4r\x1b[?6h\x1b[1;2H\x1b[6n"));
    const std::vector<term::Terminal_reply> origin_replies = replies_in(result);
    ok &= check(origin_replies.size() == 1U, "origin DSR emits one reply");
    ok &= check(reply_at(origin_replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[1;2R"),
        "origin DSR reports region-relative row");
    ok &= check(snapshot_valid(model, 43U), "origin DSR snapshot validates");

    model = make_model(7, 13);
    model.ingest(QByteArrayLiteral("abc"));
    result = model.ingest(QByteArrayLiteral("\x1b[18t"));
    ok &= check(diagnostic_count(result) == 0, "text-area size query has no diagnostics");
    const std::vector<term::Terminal_reply> text_area_size_replies = replies_in(result);
    ok &= check(text_area_size_replies.size() == 1U,
        "text-area size query emits one reply");
    const term::Terminal_reply& text_area_size_reply =
        reply_at(text_area_size_replies, 0U);
    ok &= check(text_area_size_reply.kind == term::Terminal_reply_kind::TEXT_AREA_SIZE &&
        text_area_size_reply.wire_bytes == QByteArrayLiteral("\x1b[8;7;13t"),
        "text-area size query reports configured grid");
    ok &= check(text_area_size_reply.source_sequence == QStringLiteral("CSI 18 t"),
        "text-area size query reply source");
    ok &= check(model.row_text(0) == QStringLiteral("abc"),
        "text-area size query does not mutate screen text");
    ok &= dirty_rows_equal(result, {}, "text-area size query has no dirty rows");
    ok &= check(!result.viewport_changed,
        "text-area size query does not move viewport");

    result  = model.ingest(QByteArrayLiteral("\x1b[8;5;9t"));
    ok     &= check(diagnostic_count(result) == 0,
        "text-area resize request has no diagnostics");
    ok     &= check(replies_in(result).empty(),
        "text-area resize request emits no terminal reply");
    const std::vector<term::Parser_notification> text_area_resize_notifications =
        notifications_in(result);
    ok &= check(text_area_resize_notifications.size() == 1U,
        "text-area resize request emits one notification");
    if (!text_area_resize_notifications.empty()) {
        const term::Parser_notification& notification =
            text_area_resize_notifications.front();
        ok &= check(notification.kind ==
            term::Parser_notification_kind::TEXT_AREA_RESIZE_REQUESTED &&
            notification.rows == 5 &&
            notification.columns == 9,
            "text-area resize request carries requested grid");
    }
    ok &= check(model.grid_size().rows == 5 && model.grid_size().columns == 9,
        "text-area resize request applies requested grid");
    ok &= dirty_rows_equal(result, row_range(5),
        "text-area resize request dirties resized visible rows");

    result  = model.ingest(QByteArrayLiteral("\x1b[8;4097;9t"));
    ok     &= check(diagnostic_count(result) == 1,
        "oversized text-area resize request emits a diagnostic");
    ok     &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "oversized text-area resize request is unsupported");
    ok     &= check(notifications_in(result).empty(),
        "oversized text-area resize request emits no notification");
    ok     &= check(model.grid_size().rows == 5 && model.grid_size().columns == 9,
        "oversized text-area resize request leaves grid unchanged");

    model   = make_model(2, 4);
    result  = model.ingest(QByteArrayLiteral("aa\x1b[8;3;5t\x1b[3;5HZ"));
    ok     &= check(diagnostic_count(result) == 0,
        "inline text-area resize sequence has no diagnostics");
    ok     &= check(model.grid_size().rows == 3 && model.grid_size().columns == 5,
        "inline text-area resize updates grid before following output");
    ok     &= check(model.row_text(2) == QStringLiteral("    Z"),
        "inline text-area resize interprets following cursor address in resized grid");
    ok     &= check(snapshot_valid(model, 44U),
        "inline text-area resize snapshot validates");

    result = model.ingest(QByteArrayLiteral("\x1b[14t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "CSI 14 t is an unsupported lowercase t query");

    result = model.ingest(QByteArrayLiteral("\x1b[19t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "CSI 19 t is an unsupported lowercase t query");

    result = model.ingest(QByteArrayLiteral("\x1b[22;0t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "CSI 22;0 t is an unsupported lowercase t subcommand");

    result = model.ingest(QByteArrayLiteral("\x1b[18:1t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::MALFORMED_INPUT,
        "CSI 18:1 t is malformed for lowercase t");

    result = model.ingest(QByteArrayLiteral("\x1b[?18t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "CSI ?18 t is an unsupported private lowercase t query");

    result = model.ingest(QByteArrayLiteral("\x1b[>18t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "CSI >18 t is an unsupported private lowercase t query");

    result = model.ingest(QByteArrayLiteral("\x1b[18$t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "CSI 18 $ t is an unsupported intermediate lowercase t query");

    result = model.ingest(QByteArrayLiteral("\x1b[t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "bare CSI t is an unsupported lowercase t query");

    model = make_model(3, 4);
    model.resize(term::terminal_grid_size_t{9, 11});
    result = model.ingest(QByteArrayLiteral("\x1b[18t"));
    ok &= check(diagnostic_count(result) == 0,
        "resized text-area size query has no diagnostics");
    const std::vector<term::Terminal_reply> resized_text_area_size_replies =
        replies_in(result);
    ok &= check(resized_text_area_size_replies.size() == 1U,
        "resized text-area size query emits one reply");
    const term::Terminal_reply& resized_text_area_size_reply =
        reply_at(resized_text_area_size_replies, 0U);
    ok &= check(resized_text_area_size_reply.kind == term::Terminal_reply_kind::TEXT_AREA_SIZE &&
        resized_text_area_size_reply.wire_bytes == QByteArrayLiteral("\x1b[8;9;11t"),
        "resized text-area size query reports post-resize grid");

    model   = make_model(2, 5);
    result  = model.ingest(QByteArrayLiteral("\x1b[2;4H\x1b" "7\x1b[1;1HA\x1b" "8B"));
    ok     &= check(diagnostic_count(result) == 0, "ESC save/restore has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("A"), "restore leaves home write");
    ok     &= check(model.row_text(1) == QStringLiteral("   B"), "restore writes saved cursor");
    ok     &= check(snapshot_valid(model, 41U), "ESC restore snapshot validates");

    model   = make_model(2, 5);
    result  = model.ingest(QByteArrayLiteral("\x1b[2;4H\x1b[s\x1b[1;1HA\x1b[uB"));
    ok     &= check(diagnostic_count(result) == 0, "CSI save/restore has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("A"), "CSI restore leaves home write");
    ok     &= check(model.row_text(1) == QStringLiteral("   B"), "CSI restore writes saved cursor");
    ok     &= check(snapshot_valid(model, 42U), "CSI restore snapshot validates");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_cursor_addressing_and_split_csi();
    ok &= test_erase_operations_and_wide_damage();
    ok &= test_scroll_region_and_origin_mode();
    ok &= test_top_anchored_scroll_region_appends_scrollback();
    ok &= test_alternate_top_anchored_scroll_region_does_not_append_scrollback();
    ok &= test_escape_index_controls();
    ok &= test_scroll_region_history_insert_after_reverse_index();
    ok &= test_conpty_primary_repaint_recovery_preserves_scrollback();
    ok &= test_primary_repaint_recovery_ignores_plain_home_repaint();
    ok &= test_primary_repaint_recovery_ignores_clear_after_home();
    ok &= test_primary_repaint_recovery_survives_split_repaint();
    ok &= test_primary_repaint_recovery_ignores_synchronized_repaint();
    ok &= test_scroll_region_zero_bottom_defaults_to_screen_bottom();
    ok &= test_scroll_up_down_sequences();
    ok &= test_blank_fill_operations_use_current_style();
    ok &= test_insert_delete_lines_cells_and_tabs();
    ok &= test_replies_and_cursor_save_restore();
    return ok ? 0 : 1;
}
