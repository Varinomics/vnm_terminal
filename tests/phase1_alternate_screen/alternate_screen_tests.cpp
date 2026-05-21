#include "vnm_terminal/internal/terminal_screen_model.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QString>
#include <cstdlib>
#include <iostream>
#include <variant>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

constexpr quint32 k_red_rgba = 0xffcd0000U;

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

term::Terminal_screen_model make_model(
    int    rows = 3,
    int    columns = 8,
    int    scrollback_limit = 8)
{
    return term::Terminal_screen_model({
        term::terminal_grid_size_t{rows, columns},
        scrollback_limit,
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

bool dirty_rows_equal(
    const term::Terminal_screen_model_result&  result,
    std::initializer_list<int>                 rows,
    const char*                                label)
{
    return check(result.dirty_rows == std::vector<int>(rows), label);
}

bool snapshot_valid(const term::Terminal_screen_model& model, std::uint64_t sequence)
{
    return term::validate_render_snapshot(model.render_snapshot(sequence)).status ==
        term::Terminal_render_snapshot_status::OK;
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

const term::Terminal_text_style& style_for_cell(
    const term::Terminal_render_snapshot&  snapshot,
    const term::Terminal_render_cell&      cell)
{
    const std::size_t style_index = static_cast<std::size_t>(cell.style_id);
    if (style_index >= snapshot.styles.size()) {
        fail_required_value("expected render snapshot style");
    }

    return snapshot.styles[style_index];
}

quint32 foreground_rgba_for_cell(
    const term::Terminal_render_snapshot&  snapshot,
    const term::Terminal_render_cell&      cell)
{
    const term::Terminal_text_style& style = style_for_cell(snapshot, cell);
    return term::resolve_terminal_color_ref(style.foreground, snapshot.color_state, true);
}

bool test_dec_47_switching_and_scrollback_isolation()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(2, 4);
    model.ingest(QByteArrayLiteral("ABCD\r\nEFGH\r\nIJKL"));

    const int     primary_scrollback_size = model.scrollback_size();
    const QString primary_top             = model.row_text(0);
    const QString primary_bottom          = model.row_text(1);
    ok &= check(primary_scrollback_size > 0, "primary setup creates scrollback");

    term::Terminal_screen_model_result result = model.ingest(QByteArrayLiteral("\x1b[?47h"));
    ok &= check(diagnostic_count(result) == 0, "47 enter has no diagnostics");
    ok &= check(model.active_buffer_id() == term::Terminal_buffer_id::ALTERNATE,
        "47 enters alternate buffer");
    ok &= check(result.viewport_changed, "47 enter reports viewport change");
    ok &= dirty_rows_equal(result, {0, 1}, "47 enter dirties all rows");

    term::Terminal_render_snapshot snapshot = model.render_snapshot(1U);
    ok &= check(snapshot.viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "snapshot reports alternate buffer");
    ok &= check(snapshot.viewport.scrollback_rows == 0,
        "alternate snapshot exposes no scrollback");
    ok &= check(model.row_text(0).isEmpty() && model.row_text(1).isEmpty(),
        "alternate starts empty");

    result  = model.ingest(QByteArrayLiteral("ALT\r\nMORE\r\nDROP"));
    ok     &= check(diagnostic_count(result) == 0, "alternate scroll has no diagnostics");
    ok     &= check(model.scrollback_size() == primary_scrollback_size,
        "alternate scroll does not mutate primary scrollback");
    ok     &= check(model.row_text(0) == QStringLiteral("MORE") &&
        model.row_text(1) == QStringLiteral("DROP"),
        "alternate content after local scroll");
    ok     &= check(snapshot_valid(model, 2U), "alternate scrolled snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[?47l"));
    ok     &= check(model.active_buffer_id() == term::Terminal_buffer_id::PRIMARY,
        "47 reset returns to primary buffer");
    ok     &= check(result.viewport_changed, "47 leave reports viewport change");
    ok     &= check(model.row_text(0) == primary_top && model.row_text(1) == primary_bottom,
        "primary visible rows are restored");
    ok     &= check(model.scrollback_size() == primary_scrollback_size,
        "primary scrollback size is retained");

    snapshot  = model.render_snapshot(3U);
    ok       &= check(snapshot.viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
        "snapshot reports primary buffer");
    ok       &= check(snapshot.viewport.scrollback_rows == primary_scrollback_size,
        "primary snapshot exposes primary scrollback rows");

    result = model.ingest(QByteArrayLiteral("\x1b[?47h"));
    ok &= check(model.row_text(0) == QStringLiteral("MORE") &&
        model.row_text(1) == QStringLiteral("DROP"),
        "47 re-enter preserves alternate contents");

    return ok;
}

bool test_1047_clears_alternate_on_entry()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(2, 5);
    model.ingest(QByteArrayLiteral("\x1b[?47hOLD\x1b[?47l"));

    term::Terminal_screen_model_result result = model.ingest(QByteArrayLiteral("\x1b[?1047h"));
    ok &= check(diagnostic_count(result) == 0, "1047 enter has no diagnostics");
    ok &= check(model.active_buffer_id() == term::Terminal_buffer_id::ALTERNATE,
        "1047 enters alternate buffer");
    ok &= check(model.row_text(0).isEmpty() && model.row_text(1).isEmpty(),
        "1047 enter clears prior alternate content");

    model.ingest(QByteArrayLiteral("NEW\x1b[?1047l\x1b[?47h"));
    ok &= check(model.row_text(0).isEmpty() && model.row_text(1).isEmpty(),
        "1047 reset clears alternate content before returning to primary");

    model.ingest(QByteArrayLiteral("\x1b[?1047h"));
    ok &= check(model.row_text(0).isEmpty(),
        "repeated 1047 enter clears alternate content again");

    return ok;
}

bool test_1049_restores_primary_cursor_and_style()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(3, 8);
    model.ingest(QByteArrayLiteral("\x1b[31mP\x1b[2;4H"));

    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("\x1b[?1049h\x1b[0mA\x1b[3;8HZ\x1b[?1049lB"));
    ok &= check(diagnostic_count(result) == 0, "1049 round trip has no diagnostics");
    ok &= check(model.active_buffer_id() == term::Terminal_buffer_id::PRIMARY,
        "1049 reset returns to primary buffer");
    ok &= check(model.row_text(0) == QStringLiteral("P"), "primary row survives 1049");
    ok &= check(model.row_text(1) == QStringLiteral("   B"),
        "1049 restores primary cursor before following output");
    ok &= check(model.cursor_position().row == 1 && model.cursor_position().column == 4,
        "1049 cursor advances after restored write");

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(10U);
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "1049 style snapshot validates");
    ok &= check(foreground_rgba_for_cell(snapshot, cell_at(snapshot, 1, 3)) == k_red_rgba,
        "1049 restores saved primary style");

    model.ingest(QByteArrayLiteral("\x1b[?47h"));
    ok &= check(model.row_text(0).isEmpty() && model.row_text(1).isEmpty() &&
        model.row_text(2).isEmpty(),
        "1049 reset clears alternate contents before returning to primary");

    return ok;
}

bool test_1049_reset_without_owned_save_is_noop()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(2, 6);
    model.ingest(QByteArrayLiteral("\x1b[2;4H\x1b[?1048h\x1b[1;1HA"));

    term::Terminal_screen_model_result result = model.ingest(QByteArrayLiteral("\x1b[?1049lB"));
    ok &= check(diagnostic_count(result) == 0, "primary 1049 reset has no diagnostics");
    ok &= check(model.active_buffer_id() == term::Terminal_buffer_id::PRIMARY,
        "primary 1049 reset keeps primary active");
    ok &= check(model.row_text(0) == QStringLiteral("AB"),
        "primary 1049 reset does not restore unrelated 1048 cursor");
    ok &= check(model.row_text(1).isEmpty(),
        "primary 1049 reset does not write at stale saved cursor");

    model   = make_model(2, 6);
    result  = model.ingest(QByteArrayLiteral(
        "\x1b[2;3H\x1b[?1049h\x1b[?1049lA\x1b[1;1H\x1b[?1049lB"));
    ok     &= check(diagnostic_count(result) == 0, "repeated 1049 reset has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("B"),
        "repeated 1049 reset does not restore after ownership is consumed");
    ok     &= check(model.row_text(1) == QStringLiteral("  A"),
        "first 1049 reset restores saved primary cursor once");

    return ok;
}

bool test_1048_saved_cursor_is_per_buffer()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(2, 6);
    model.ingest(QByteArrayLiteral("P\x1b[2;3H\x1b[?1048h\x1b[1;1H"));
    model.ingest(QByteArrayLiteral("\x1b[?47h\x1b[1;4H\x1b[?1048h"
        "\x1b[2;1H\x1b[?1048lA"));
    ok &= check(model.row_text(0) == QStringLiteral("   A"),
        "alternate 1048 restore uses alternate saved cursor");

    model.ingest(QByteArrayLiteral("\x1b[?47l\x1b[?1048lB"));
    ok &= check(model.active_buffer_id() == term::Terminal_buffer_id::PRIMARY,
        "47 leave returns to primary after alternate 1048");
    ok &= check(model.row_text(0) == QStringLiteral("P"), "primary content remains isolated");
    ok &= check(model.row_text(1) == QStringLiteral("  B"),
        "primary 1048 restore keeps primary saved cursor");
    ok &= check(snapshot_valid(model, 20U), "1048 per-buffer snapshot validates");

    return ok;
}

bool test_decrqm_alternate_modes()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(2, 4);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("\x1b[?47$p\x1b[?1047$p"
            "\x1b[?1048$p\x1b[?1049$p"));
    std::vector<term::Terminal_reply> replies = replies_in(result);
    ok &= check(replies.size() == 4U, "primary DECRQM emits four replies");
    ok &= check(reply_at(replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?47;2$y"),
        "primary 47 DECRQM reset");
    ok &= check(reply_at(replies, 1U).wire_bytes == QByteArrayLiteral("\x1b[?1047;2$y"),
        "primary 1047 DECRQM reset");
    ok &= check(reply_at(replies, 2U).wire_bytes == QByteArrayLiteral("\x1b[?1048;2$y"),
        "1048 DECRQM reports reset");
    ok &= check(reply_at(replies, 3U).wire_bytes == QByteArrayLiteral("\x1b[?1049;2$y"),
        "primary 1049 DECRQM reset");

    result   = model.ingest(QByteArrayLiteral("\x1b[?1049h\x1b[?47$p\x1b[?1047$p"
        "\x1b[?1048$p\x1b[?1049$p"));
    replies  = replies_in(result);
    ok      &= check(replies.size() == 4U, "alternate DECRQM emits four replies");
    ok      &= check(reply_at(replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?47;1$y"),
        "alternate 47 DECRQM set");
    ok      &= check(reply_at(replies, 1U).wire_bytes == QByteArrayLiteral("\x1b[?1047;1$y"),
        "alternate 1047 DECRQM set");
    ok      &= check(reply_at(replies, 2U).wire_bytes == QByteArrayLiteral("\x1b[?1048;2$y"),
        "alternate 1048 DECRQM still reset");
    ok      &= check(reply_at(replies, 3U).wire_bytes == QByteArrayLiteral("\x1b[?1049;1$y"),
        "alternate 1049 DECRQM set");

    result   = model.ingest(QByteArrayLiteral("\x1b[?1049l\x1b[?1049$p"));
    replies  = replies_in(result);
    ok      &= check(replies.size() == 1U, "1049 reset query emits one reply");
    ok      &= check(reply_at(replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?1049;2$y"),
        "1049 reset DECRQM reset");

    return ok;
}

bool test_resize_while_alternate_active()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(3, 6);
    model.ingest(QByteArrayLiteral("aaaaaa\r\nbbbbbb\r\ncccccc\r\ndddddd"));
    const int     primary_scrollback_size           = model.scrollback_size();
    const QString expected_primary_top_after_resize = model.row_text(0).left(4);

    model.ingest(QByteArrayLiteral("\x1b[?47hALT"));
    term::Terminal_screen_model_result result = model.resize({2, 4});
    ok &= check(result.viewport_changed, "resize reports viewport change");
    ok &= dirty_rows_equal(result, {0, 1}, "resize dirties visible alternate rows");
    ok &= check(model.grid_size().rows == 2 && model.grid_size().columns == 4,
        "resize updates model grid");
    ok &= check(model.active_buffer_id() == term::Terminal_buffer_id::ALTERNATE,
        "resize preserves active alternate buffer");
    ok &= check(model.row_text(0) == QStringLiteral("ALT"),
        "resize preserves alternate top-left content");

    term::Terminal_render_snapshot snapshot = model.render_snapshot(30U);
    ok &= check(snapshot.viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "resized snapshot reports alternate buffer");
    ok &= check(snapshot.viewport.scrollback_rows == 0,
        "resized alternate snapshot exposes no scrollback");
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "resized alternate snapshot validates");

    result  = model.resize({0, 4});
    ok     &= check(!result.viewport_changed, "invalid resize does not report viewport change");
    ok     &= check(result.dirty_rows.empty(), "invalid resize has no dirty rows");
    ok     &= check(model.grid_size().rows == 2 && model.grid_size().columns == 4,
        "invalid resize leaves grid unchanged");
    ok     &= check(model.row_text(0) == QStringLiteral("ALT"),
        "invalid resize leaves alternate content unchanged");

    model.ingest(QByteArrayLiteral("\x1b[?47l"));
    ok &= check(model.active_buffer_id() == term::Terminal_buffer_id::PRIMARY,
        "leave after resize returns to primary buffer");
    ok &= check(model.scrollback_size() == primary_scrollback_size,
        "resize preserves primary scrollback storage");
    ok &= check(model.row_text(0) == expected_primary_top_after_resize,
        "resize normalizes inactive primary buffer dimensions");

    return ok;
}

bool test_resize_normalizes_saved_origin_mode_and_noop()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(4, 5);
    model.ingest(QByteArrayLiteral("\x1b[2;4r\x1b[?6h\x1b[1;1H\x1b[?1048h"));

    term::Terminal_screen_model_result result = model.resize(model.grid_size());
    ok &= check(!result.viewport_changed, "same-size resize does not change viewport");
    ok &= check(result.dirty_rows.empty(), "same-size resize does not dirty rows");

    result  = model.resize({3, 5});
    ok     &= check(result.viewport_changed, "actual resize reports viewport change");
    ok     &= dirty_rows_equal(result, {0, 1, 2}, "actual resize dirties new visible rows");

    result = model.ingest(QByteArrayLiteral("\x1b[?1048l\x1b[?6$p"));
    const std::vector<term::Terminal_reply> replies = replies_in(result);
    ok &= check(replies.size() == 1U, "origin query emits one reply after resize");
    ok &= check(reply_at(replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?6;2$y"),
        "restoring saved cursor after resize does not restore origin mode");
    ok &= check(snapshot_valid(model, 41U), "saved-origin resize snapshot validates");

    return ok;
}

bool test_resize_clears_wide_cells_crossing_right_edge()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(1, 4);
    model.ingest(QByteArrayLiteral("AB") + bytes_from_hex("e4b880"));
    ok &= check(model.row_text(0) == QString::fromUtf8("AB\xe4\xb8\x80"),
        "wide resize setup");

    term::Terminal_screen_model_result result = model.resize({1, 3});
    ok &= check(result.viewport_changed, "wide shrink reports viewport change");
    ok &= check(model.row_text(0) == QStringLiteral("AB"),
        "wide cell crossing new right edge is cleared");
    ok &= check(snapshot_valid(model, 40U), "wide shrink snapshot validates");

    return ok;
}

bool test_viewport_alternate_screen_policy_hooks()
{
    bool ok = true;

    term::Terminal_viewport_controller viewport;
    viewport.set_visible_rows(2);
    viewport.set_scrollback_rows(5);
    viewport.scroll_lines(3);
    ok &= check(viewport.state().active_buffer == term::Terminal_buffer_id::PRIMARY,
        "viewport starts on primary buffer");
    ok &= check(viewport.state().offset_from_tail == 3 && !viewport.state().follow_tail,
        "viewport can detach from primary tail");

    viewport.enter_alternate_screen();
    ok &= check(viewport.state().active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "viewport enters alternate buffer");
    ok &= check(viewport.state().offset_from_tail == 0 && viewport.state().follow_tail,
        "alternate viewport returns to tail");

    viewport.set_alternate_screen_scroll_policy(
        term::Terminal_alternate_screen_scroll_policy::WHEEL_TO_TERMINAL_INPUT);
    viewport.leave_alternate_screen();
    ok &= check(viewport.state().active_buffer == term::Terminal_buffer_id::PRIMARY,
        "viewport returns to primary buffer");
    ok &= check(viewport.state().alternate_screen_scroll_policy ==
        term::Terminal_alternate_screen_scroll_policy::WHEEL_TO_TERMINAL_INPUT,
        "alternate wheel policy remains recorded");

    viewport.scroll_lines(2);
    viewport.set_scrollback_rows(1);
    ok &= check(viewport.state().offset_from_tail == 1 && !viewport.state().follow_tail,
        "viewport clamps primary offset after scrollback shrink");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_dec_47_switching_and_scrollback_isolation();
    ok &= test_1047_clears_alternate_on_entry();
    ok &= test_1049_restores_primary_cursor_and_style();
    ok &= test_1049_reset_without_owned_save_is_noop();
    ok &= test_1048_saved_cursor_is_per_buffer();
    ok &= test_decrqm_alternate_modes();
    ok &= test_resize_while_alternate_active();
    ok &= test_resize_normalizes_saved_origin_mode_and_noop();
    ok &= test_resize_clears_wide_cells_crossing_right_edge();
    ok &= test_viewport_alternate_screen_policy_hooks();
    return ok ? 0 : 1;
}
