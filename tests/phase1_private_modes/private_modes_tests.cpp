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

using vnm_terminal::test_helpers::check;

[[noreturn]] void fail_required_value(const char* message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

term::Terminal_screen_model make_model(int rows = 3, int columns = 8)
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

int screen_mutation_kind_count(
    const term::Terminal_screen_model_result&  result,
    term::Screen_mutation_kind                 kind)
{
    int count = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) != term::Parser_action_kind::SCREEN_MUTATION) {
            continue;
        }

        const term::Screen_mutation& mutation =
            std::get<term::Screen_mutation>(action.payload);
        if (term::screen_mutation_kind(mutation) == kind) {
            ++count;
        }
    }
    return count;
}

int notification_kind_count(
    const term::Terminal_screen_model_result&  result,
    term::Parser_notification_kind             kind)
{
    int count = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) != term::Parser_action_kind::NOTIFICATION) {
            continue;
        }

        const term::Parser_notification& notification =
            std::get<term::Parser_notification>(action.payload);
        if (notification.kind == kind) {
            ++count;
        }
    }
    return count;
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
    const term::Terminal_render_snapshot&      snapshot,
    int                                        row,
    int                                        column)
{
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row == row && cell.position.column == column) {
            return cell;
        }
    }

    fail_required_value("expected render cell");
}

bool snapshot_valid(const term::Terminal_screen_model& model, std::uint64_t sequence)
{
    return term::validate_render_snapshot(model.render_snapshot(sequence)).status ==
        term::Terminal_render_snapshot_status::OK;
}

bool test_mode_state_and_decrqm()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model();
    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[?1;5;25;1000;1004;1006;1007;2004;2026h"));
    ok &= check(diagnostic_count(result) == 0, "mode set has no diagnostics");
    ok &= check(result.mode_state_changed, "mode set reports mode-state change");

    term::Terminal_mode_state modes = model.mode_state();
    ok &= check(modes.application_cursor_keys, "application cursor mode set");
    ok &= check(modes.reverse_video, "reverse video mode set");
    ok &= check(modes.cursor_visible, "cursor visible mode remains set");
    ok &= check(modes.mouse_tracking == term::Terminal_mouse_tracking_mode::BUTTON,
        "button mouse tracking set");
    ok &= check(modes.focus_reporting, "focus reporting set");
    ok &= check(modes.sgr_mouse_encoding, "SGR mouse encoding set");
    ok &= check(modes.alternate_scroll, "alternate scroll set");
    ok &= check(modes.bracketed_paste, "bracketed paste set");
    ok &= check(modes.synchronized_output, "synchronized output set");

    term::Terminal_render_snapshot snapshot = model.render_snapshot(1U);
    ok &= check(snapshot.modes.application_cursor_keys, "snapshot carries app cursor mode");
    ok &= check(snapshot.modes.reverse_video, "snapshot carries reverse video mode");
    ok &= check(snapshot.modes.synchronized_output, "snapshot carries synchronized output mode");

    result = model.ingest(QByteArrayLiteral(
        "\x1b[?1$p\x1b[?5$p\x1b[?7$p\x1b[?25$p\x1b[?1000$p"
        "\x1b[?1004$p\x1b[?1006$p\x1b[?1007$p\x1b[?2004$p\x1b[?2026$p"));
    const std::vector<term::Terminal_reply> replies = replies_in(result);
    ok &= check(replies.size() == 10U, "mode DECRQM emits replies");
    ok &= check(reply_at(replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?1;1$y"),
        "application cursor DECRQM set");
    ok &= check(reply_at(replies, 1U).wire_bytes == QByteArrayLiteral("\x1b[?5;1$y"),
        "reverse video DECRQM set");
    ok &= check(reply_at(replies, 2U).wire_bytes == QByteArrayLiteral("\x1b[?7;1$y"),
        "autowrap DECRQM set by default");
    ok &= check(reply_at(replies, 3U).wire_bytes == QByteArrayLiteral("\x1b[?25;1$y"),
        "cursor visible DECRQM set");
    ok &= check(reply_at(replies, 4U).wire_bytes == QByteArrayLiteral("\x1b[?1000;1$y"),
        "button mouse DECRQM set");
    ok &= check(reply_at(replies, 5U).wire_bytes == QByteArrayLiteral("\x1b[?1004;1$y"),
        "focus reporting DECRQM set");
    ok &= check(reply_at(replies, 6U).wire_bytes == QByteArrayLiteral("\x1b[?1006;1$y"),
        "SGR mouse DECRQM set");
    ok &= check(reply_at(replies, 7U).wire_bytes == QByteArrayLiteral("\x1b[?1007;1$y"),
        "alternate scroll DECRQM set");
    ok &= check(reply_at(replies, 8U).wire_bytes == QByteArrayLiteral("\x1b[?2004;1$y"),
        "bracketed paste DECRQM set");
    ok &= check(reply_at(replies, 9U).wire_bytes == QByteArrayLiteral("\x1b[?2026;1$y"),
        "synchronized output DECRQM set");

    result    = model.ingest(
        QByteArrayLiteral("\x1b[?25l\x1b[?1;5;1000;1004;1006;1007;2004;2026l"));
    ok       &= check(diagnostic_count(result) == 0, "mode reset has no diagnostics");
    snapshot  = model.render_snapshot(2U);
    ok       &= check(!snapshot.cursor.visible, "cursor visibility reset reaches snapshot");
    ok       &= check(!snapshot.modes.application_cursor_keys, "application cursor reset");
    ok       &= check(!snapshot.modes.reverse_video, "reverse video reset");
    ok       &= check(snapshot.modes.mouse_tracking == term::Terminal_mouse_tracking_mode::NONE,
        "button mouse reset");
    ok       &= check(!snapshot.modes.focus_reporting, "focus reporting reset");
    ok       &= check(!snapshot.modes.sgr_mouse_encoding, "SGR mouse reset");
    ok       &= check(!snapshot.modes.alternate_scroll, "alternate scroll reset");
    ok       &= check(!snapshot.modes.bracketed_paste, "bracketed paste reset");
    ok       &= check(!snapshot.modes.synchronized_output, "synchronized output reset");

    result = model.ingest(QByteArrayLiteral(
        "\x1b[?1$p\x1b[?5$p\x1b[?25$p\x1b[?1000$p"
        "\x1b[?1004$p\x1b[?1006$p\x1b[?1007$p\x1b[?2004$p\x1b[?2026$p"));
    const std::vector<term::Terminal_reply> reset_replies = replies_in(result);
    ok &= check(reset_replies.size() == 9U, "mode reset DECRQM emits replies");
    ok &= check(reply_at(reset_replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?1;2$y"),
        "application cursor DECRQM reset");
    ok &= check(reply_at(reset_replies, 1U).wire_bytes == QByteArrayLiteral("\x1b[?5;2$y"),
        "reverse video DECRQM reset");
    ok &= check(reply_at(reset_replies, 2U).wire_bytes == QByteArrayLiteral("\x1b[?25;2$y"),
        "cursor visible DECRQM reset");
    ok &= check(reply_at(reset_replies, 3U).wire_bytes == QByteArrayLiteral("\x1b[?1000;2$y"),
        "button mouse DECRQM reset");
    ok &= check(reply_at(reset_replies, 4U).wire_bytes == QByteArrayLiteral("\x1b[?1004;2$y"),
        "focus reporting DECRQM reset");
    ok &= check(reply_at(reset_replies, 5U).wire_bytes == QByteArrayLiteral("\x1b[?1006;2$y"),
        "SGR mouse DECRQM reset");
    ok &= check(reply_at(reset_replies, 6U).wire_bytes == QByteArrayLiteral("\x1b[?1007;2$y"),
        "alternate scroll DECRQM reset");
    ok &= check(reply_at(reset_replies, 7U).wire_bytes == QByteArrayLiteral("\x1b[?2004;2$y"),
        "bracketed paste DECRQM reset");
    ok &= check(reply_at(reset_replies, 8U).wire_bytes == QByteArrayLiteral("\x1b[?2026;2$y"),
        "synchronized output DECRQM reset");

    return ok;
}

bool test_mouse_tracking_levels()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model();
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("\x1b[?1002h\x1b[?1000$p\x1b[?1002$p"));
    std::vector<term::Terminal_reply> replies = replies_in(result);
    ok &= check(model.mode_state().mouse_tracking == term::Terminal_mouse_tracking_mode::DRAG,
        "drag mouse tracking set");
    ok &= check(replies.size() == 2U, "drag mouse DECRQM emits replies");
    ok &= check(reply_at(replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?1000;2$y"),
        "button mouse reset while drag is active");
    ok &= check(reply_at(replies, 1U).wire_bytes == QByteArrayLiteral("\x1b[?1002;1$y"),
        "drag mouse DECRQM set");

    result   = model.ingest(QByteArrayLiteral("\x1b[?1003h\x1b[?1002$p\x1b[?1003$p"));
    replies  = replies_in(result);
    ok      &= check(model.mode_state().mouse_tracking == term::Terminal_mouse_tracking_mode::ANY,
        "any-motion mouse tracking set");
    ok      &= check(replies.size() == 2U, "any-motion mouse DECRQM emits replies");
    ok      &= check(reply_at(replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?1002;2$y"),
        "drag mouse reset while any-motion is active");
    ok      &= check(reply_at(replies, 1U).wire_bytes == QByteArrayLiteral("\x1b[?1003;1$y"),
        "any-motion mouse DECRQM set");

    result   = model.ingest(QByteArrayLiteral("\x1b[?1003l\x1b[?1003$p"));
    replies  = replies_in(result);
    ok      &= check(model.mode_state().mouse_tracking == term::Terminal_mouse_tracking_mode::NONE,
        "any-motion mouse reset");
    ok      &= check(replies.size() == 1U, "any-motion mouse reset DECRQM emits one reply");
    ok      &= check(reply_at(replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?1003;2$y"),
        "any-motion mouse DECRQM reset");

    result   = model.ingest(QByteArrayLiteral("\x1b[?1002h\x1b[?1002l\x1b[?1002$p"));
    replies  = replies_in(result);
    ok      &= check(model.mode_state().mouse_tracking == term::Terminal_mouse_tracking_mode::NONE,
        "drag mouse reset");
    ok      &= check(replies.size() == 1U, "drag mouse reset DECRQM emits one reply");
    ok      &= check(reply_at(replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?1002;2$y"),
        "drag mouse DECRQM reset");

    return ok;
}

bool test_cursor_visibility_and_autowrap()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(2, 3);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("\x1b[?25l"));
    ok &= check(result.mode_state_changed, "cursor visibility reset reports mode change");
    ok &= check(!model.render_snapshot(10U).cursor.visible, "cursor hidden in snapshot");

    model   = make_model(2, 3);
    result  = model.ingest(QByteArrayLiteral("\x1b[?7lABCDEF"));
    ok     &= check(diagnostic_count(result) == 0, "autowrap reset has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("ABF"),
        "disabled autowrap overwrites right margin");
    ok     &= check(model.row_text(1).isEmpty(), "disabled autowrap does not move down");
    ok     &= check(model.cursor_position().row == 0 && model.cursor_position().column == 2,
        "disabled autowrap keeps cursor at right margin");

    result  = model.ingest(QByteArrayLiteral("\x1b[?7hGH"));
    ok     &= check(model.row_text(0) == QStringLiteral("ABG"),
        "re-enabled autowrap writes at margin first");
    ok     &= check(model.row_text(1) == QStringLiteral("H"),
        "re-enabled autowrap wraps following cell");

    model   = make_model(2, 3);
    result  = model.ingest(QByteArrayLiteral("ABC\x1b[?7lD"));
    ok     &= check(model.row_text(0) == QStringLiteral("ABD"),
        "turning autowrap off clears pending wrap");
    ok     &= check(model.row_text(1).isEmpty(), "cleared pending wrap prevents line feed");

    result = model.ingest(QByteArrayLiteral("\x1b[?7$p"));
    const std::vector<term::Terminal_reply> replies = replies_in(result);
    ok &= check(replies.size() == 1U, "autowrap query emits one reply");
    ok &= check(reply_at(replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?7;2$y"),
        "autowrap DECRQM reset");

    return ok;
}

bool test_application_keypad_mode()
{
    bool                               ok     = true;
    term::Terminal_screen_model        model  = make_model();
    term::Terminal_screen_model_result result = model.ingest(QByteArrayLiteral("\x1b="));
    ok &= check(diagnostic_count(result) == 0, "application keypad set has no diagnostics");
    ok &= check(!result.mode_state_changed,
        "application keypad set is input-only");
    ok &= check(model.input_mode_state().application_keypad,
        "application keypad mode reaches input mode state");

    result = model.ingest(QByteArrayLiteral("\x1b[?66$p"));
    std::vector<term::Terminal_reply> replies = replies_in(result);
    ok &= check(replies.size() == 1U,
        "ESC application keypad set DECRQM emits one reply");
    ok &= check(reply_at(replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?66;1$y"),
        "ESC application keypad set is visible through DECRQM");

    result  = model.ingest(QByteArrayLiteral("\x1b="));
    ok     &= check(diagnostic_count(result) == 0,
        "application keypad repeated ESC set has no diagnostics");
    ok     &= check(!result.mode_state_changed,
        "application keypad repeated ESC set is idempotent");

    result  = model.ingest(QByteArrayLiteral("\x1b>"));
    ok     &= check(diagnostic_count(result) == 0, "application keypad reset has no diagnostics");
    ok     &= check(!result.mode_state_changed,
        "application keypad reset is input-only");
    ok     &= check(!model.input_mode_state().application_keypad,
        "application keypad reset reaches input mode state");

    result  = model.ingest(QByteArrayLiteral("\x1b>"));
    ok     &= check(diagnostic_count(result) == 0,
        "application keypad repeated ESC reset has no diagnostics");
    ok     &= check(!result.mode_state_changed,
        "application keypad repeated ESC reset is idempotent");

    result  = model.ingest(QByteArrayLiteral("\x1b[?66h"));
    ok     &= check(diagnostic_count(result) == 0, "DECNKM set has no diagnostics");
    ok     &= check(!result.mode_state_changed, "DECNKM set is input-only");
    ok     &= check(model.input_mode_state().application_keypad,
        "DECNKM set reaches input mode state");

    result  = model.ingest(QByteArrayLiteral("\x1b[?66h"));
    ok     &= check(diagnostic_count(result) == 0,
        "DECNKM repeated set has no diagnostics");
    ok     &= check(!result.mode_state_changed, "DECNKM repeated set is idempotent");

    result   = model.ingest(QByteArrayLiteral("\x1b[?66$p"));
    replies  = replies_in(result);
    ok      &= check(replies.size() == 1U, "DECNKM set DECRQM emits one reply");
    ok      &= check(reply_at(replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?66;1$y"),
        "DECNKM set DECRQM reports set");

    result  = model.ingest(QByteArrayLiteral("\x1b>"));
    ok     &= check(diagnostic_count(result) == 0,
        "ESC application keypad reset after DECNKM set has no diagnostics");
    ok     &= check(!result.mode_state_changed,
        "ESC application keypad reset after DECNKM set is input-only");
    ok     &= check(!model.input_mode_state().application_keypad,
        "ESC application keypad reset after DECNKM set reaches input mode state");

    result  = model.ingest(QByteArrayLiteral("\x1b[?66h"));
    ok     &= check(diagnostic_count(result) == 0,
        "DECNKM set after ESC reset has no diagnostics");
    ok     &= check(!result.mode_state_changed,
        "DECNKM set after ESC reset is input-only");
    ok     &= check(model.input_mode_state().application_keypad,
        "DECNKM set after ESC reset reaches input mode state");

    result  = model.ingest(QByteArrayLiteral("\x1b[?66l"));
    ok     &= check(diagnostic_count(result) == 0, "DECNKM reset has no diagnostics");
    ok     &= check(!result.mode_state_changed, "DECNKM reset is input-only");
    ok     &= check(!model.input_mode_state().application_keypad,
        "DECNKM reset reaches input mode state");

    result  = model.ingest(QByteArrayLiteral("\x1b[?66l"));
    ok     &= check(diagnostic_count(result) == 0,
        "DECNKM repeated reset has no diagnostics");
    ok     &= check(!result.mode_state_changed, "DECNKM repeated reset is idempotent");

    result   = model.ingest(QByteArrayLiteral("\x1b[?66$p"));
    replies  = replies_in(result);
    ok      &= check(replies.size() == 1U, "DECNKM reset DECRQM emits one reply");
    ok      &= check(reply_at(replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?66;2$y"),
        "DECNKM reset DECRQM reports reset");

    result  = model.ingest(QByteArrayLiteral("\x1b[?1h\x1b="));
    ok     &= check(diagnostic_count(result) == 0,
        "application keypad does not interfere with cursor mode set");
    ok     &= check(model.input_mode_state().application_cursor_keys,
        "application cursor mode remains in input state after keypad set");
    ok     &= check(model.render_snapshot(35U).modes.application_cursor_keys,
        "application cursor mode remains in render state after keypad set");

    return ok;
}

bool test_origin_mode_snapshot_and_restore()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(4, 5);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("\x1b[2;3r\x1b[?6h"));
    ok &= check(result.mode_state_changed, "origin mode set reports mode change");
    ok &= check(model.mode_state().origin_mode, "origin mode reaches model state");
    ok &= check(model.render_snapshot(18U).modes.origin_mode,
        "origin mode reaches snapshot state");

    result  = model.ingest(QByteArrayLiteral("\x1b[?1048h\x1b[?6l"));
    ok     &= check(!model.mode_state().origin_mode, "origin mode reset reaches model state");
    ok     &= check(!model.render_snapshot(19U).modes.origin_mode,
        "origin mode reset reaches snapshot state");

    result = model.ingest(QByteArrayLiteral("\x1b[?1048l\x1b[?6$p"));
    const std::vector<term::Terminal_reply> replies = replies_in(result);
    ok &= check(model.mode_state().origin_mode,
        "cursor restore restores origin mode in model state");
    ok &= check(model.render_snapshot(20U).modes.origin_mode,
        "cursor restore restores origin mode in snapshot state");
    ok &= check(replies.size() == 1U, "origin restore query emits one reply");
    ok &= check(reply_at(replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?6;1$y"),
        "origin restore DECRQM reports set");

    return ok;
}

bool test_ignored_and_rejected_modes()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model();
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("\x1b[?3;1005;1015;2027hX"));
    ok &= check(diagnostic_count(result) == 4, "ignored/rejected modes diagnose");
    ok &= check(model.row_text(0) == QStringLiteral("X"),
        "parser resumes after ignored/rejected modes");
    ok &= check(!result.mode_state_changed, "ignored/rejected modes do not report mode change");
    ok &= check(model.grid_size().columns == 8, "ignored DECCOLM does not resize model");
    ok &= check(model.mode_state().mouse_tracking == term::Terminal_mouse_tracking_mode::NONE,
        "ignored mouse protocols do not enable tracking");
    ok &= check(!model.mode_state().sgr_mouse_encoding,
        "ignored mouse protocols do not enable SGR mouse encoding");

    result = model.ingest(QByteArrayLiteral("\x1b[?3$p\x1b[?1005$p\x1b[?1015$p"));
    std::vector<term::Terminal_reply> replies = replies_in(result);
    ok &= check(replies.size() == 3U, "ignored modes emit DECRQM replies");
    ok &= check(reply_at(replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[?3;4$y"),
        "DECCOLM ignored mode reports permanently reset");
    ok &= check(reply_at(replies, 1U).wire_bytes == QByteArrayLiteral("\x1b[?1005;4$y"),
        "1005 ignored mode reports permanently reset");
    ok &= check(reply_at(replies, 2U).wire_bytes == QByteArrayLiteral("\x1b[?1015;4$y"),
        "1015 ignored mode reports permanently reset");

    result  = model.ingest(QByteArrayLiteral("\x1b[?2027$pY"));
    ok     &= check(replies_in(result).empty(), "rejected 2027 emits no DECRQM reply");
    ok     &= check(diagnostic_count(result) == 1, "rejected 2027 query diagnoses");
    ok     &= check(!result.mode_state_changed, "rejected 2027 query reports no mode change");
    ok     &= check(model.row_text(0) == QStringLiteral("XY"),
        "parser resumes after rejected 2027 query");

    return ok;
}

bool test_osc8_hyperlinks()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(1, 8);
    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b]8;id=7;https://example.test\x1b\\AB"
            "\x1b]8;;\x1b\\C"));
    ok &= check(diagnostic_count(result) == 0, "OSC 8 open/close has no diagnostics");
    term::Terminal_render_snapshot snapshot = model.render_snapshot(20U);
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "OSC 8 snapshot validates");
    const std::uint64_t explicit_hyperlink_id = cell_at(snapshot, 0, 0).hyperlink_id;
    ok &= check(explicit_hyperlink_id != 0U, "A carries hyperlink id");
    ok &= check(cell_at(snapshot, 0, 1).hyperlink_id == explicit_hyperlink_id,
        "B carries same hyperlink id");
    ok &= check(cell_at(snapshot, 0, 2).hyperlink_id == 0U, "C is unlinked after close");

    model     = make_model(1, 8);
    result    = model.ingest(
        QByteArrayLiteral("\x1b]8;;https://example.test\x1b\\XY\x1b]8;id=close;\x1b\\Z"));
    ok       &= check(diagnostic_count(result) == 0, "OSC 8 implicit id has no diagnostics");
    snapshot  = model.render_snapshot(22U);
    const std::uint64_t implicit_hyperlink_id = cell_at(snapshot, 0, 0).hyperlink_id;
    ok       &= check(implicit_hyperlink_id != 0U, "implicit OSC 8 assigns nonzero id");
    ok       &= check(cell_at(snapshot, 0, 1).hyperlink_id == implicit_hyperlink_id,
        "implicit OSC 8 linked cells share id");
    ok       &= check(cell_at(snapshot, 0, 2).hyperlink_id == 0U,
        "parameterized OSC 8 close clears active id");

    model     = make_model(1, 8);
    result    = model.ingest(
        QByteArrayLiteral("\x1b]8;id=string-key;https://example.test\x1b\\S"
            "\x1b]8;;\x1b\\"
            "\x1b]8;id=string-key;https://example.test\x1b\\T"
            "\x1b]8;;\x1b\\"
            "\x1b]8;id=string-key;https://different.test\x1b\\U"));
    ok       &= check(diagnostic_count(result) == 0, "OSC 8 string id has no diagnostics");
    snapshot  = model.render_snapshot(23U);
    ok       &= check(cell_at(snapshot, 0, 0).hyperlink_id != 0U,
        "string OSC 8 id assigns nonzero id");
    ok       &= check(cell_at(snapshot, 0, 1).hyperlink_id == cell_at(snapshot, 0, 0).hyperlink_id,
        "same string OSC 8 id and URI reuses stable id");
    ok       &= check(cell_at(snapshot, 0, 2).hyperlink_id != cell_at(snapshot, 0, 0).hyperlink_id,
        "same string OSC 8 id with different URI gets distinct id");

    model    = make_model(1, 8);
    result   = model.ingest(
        QByteArrayLiteral("\x1b]8;;https://example.test\x1b\\A\x1b]8;;\x1b\\"));
    snapshot = model.render_snapshot(24U);
    const std::uint64_t first_hyperlink_id = cell_at(snapshot, 0, 0).hyperlink_id;
    ok &= check(first_hyperlink_id != 0U, "referenced OSC 8 id is assigned");

    result    = model.ingest(QByteArrayLiteral("\x1b[1;1H "));
    snapshot  = model.render_snapshot(25U);
    ok       &= check(cell_at(snapshot, 0, 0).hyperlink_id == 0U,
        "overwriting linked cell clears hyperlink id");

    result    = model.ingest(
        QByteArrayLiteral("\x1b[1;2H\x1b]8;;https://example.test\x1b\\B"));
    snapshot  = model.render_snapshot(26U);
    ok       &= check(cell_at(snapshot, 0, 1).hyperlink_id != 0U,
        "same OSC 8 URI can be reopened after pruning");
    ok       &= check(cell_at(snapshot, 0, 1).hyperlink_id != first_hyperlink_id,
        "unreferenced OSC 8 identity is pruned");

    model    = make_model(1, 8);
    result   = model.ingest(
        QByteArrayLiteral("\x1b]8;;https://example.test\x1b\\A\x1b]8;;\x1b\\"));
    snapshot = model.render_snapshot(27U);
    const std::uint64_t same_chunk_first_hyperlink_id =
        cell_at(snapshot, 0, 0).hyperlink_id;
    ok &= check(same_chunk_first_hyperlink_id != 0U,
        "same-chunk OSC 8 prune setup assigns id");

    result    = model.ingest(
        QByteArrayLiteral("\x1b[1;1H \x1b]8;;https://example.test\x1b\\B"));
    snapshot  = model.render_snapshot(28U);
    ok       &= check(cell_at(snapshot, 0, 0).hyperlink_id == 0U,
        "same-chunk overwrite clears linked cell");
    ok       &= check(cell_at(snapshot, 0, 1).hyperlink_id != 0U,
        "same-chunk reopen assigns id");
    ok       &= check(cell_at(snapshot, 0, 1).hyperlink_id != same_chunk_first_hyperlink_id,
        "same-chunk unreferenced OSC 8 identity is pruned before reopen");

    model     = make_model(1, 8);
    result    = model.ingest(QByteArrayLiteral("\x1b]8;bad\x1b\\M"));
    ok       &= check(diagnostic_count(result) == 1, "malformed OSC 8 diagnoses");
    snapshot  = model.render_snapshot(21U);
    ok       &= check(cell_at(snapshot, 0, 0).hyperlink_id == 0U,
        "malformed OSC 8 does not link following text");

    return ok;
}

bool test_bell_and_synchronized_output_mutation()
{
    bool ok = true;

    term::Terminal_screen_model        model  = make_model(2, 4);
    term::Terminal_screen_model_result result = model.ingest(QByteArray("\a", 1));
    ok &= check(
        notification_kind_count(result, term::Parser_notification_kind::BELL_REQUESTED) == 1,
        "BEL emits bell notification");
    ok &= check(screen_mutation_kind_count(result, term::Screen_mutation_kind::BELL) == 1,
        "BEL emits typed screen action");
    ok &= check(model.visible_text() == QStringLiteral("\n"), "BEL does not write cells");

    result  = model.ingest(QByteArrayLiteral("\x1b[?2026hX\x1b[6n"));
    ok     &= check(model.mode_state().synchronized_output, "synchronized output mode set");
    ok     &= check(model.row_text(0) == QStringLiteral("X"),
        "synchronized output still mutates screen");
    ok     &= check(result.dirty_rows.empty(), "synchronized output suppresses dirty rows");
    ok     &= check(!result.viewport_changed, "synchronized output suppresses viewport change");
    ok     &= check(result.mode_state_changed, "synchronized output enter remains observable");
    ok     &= check(replies_in(result).size() == 1U,
        "synchronized output does not suppress terminal replies");

    result  = model.ingest(QByteArrayLiteral("Y"));
    ok     &= check(model.row_text(0) == QStringLiteral("XY"),
        "synchronized output keeps mutating after enter");
    ok     &= check(result.dirty_rows.empty(), "active synchronized output coalesces dirty rows");
    ok     &= check(!result.mode_state_changed, "active synchronized output has no mode change");

    result  = model.ingest(QByteArrayLiteral("\x1b[2;1H\n"));
    ok     &= check(model.scrollback_size() == 1, "synchronized output still mutates viewport");
    ok     &= check(!result.viewport_changed,
        "active synchronized output coalesces viewport change");

    result  = model.ingest(QByteArrayLiteral("\x1b[?2026l"));
    ok     &= check(!model.mode_state().synchronized_output, "synchronized output mode reset");
    ok     &= check(result.dirty_rows == std::vector<int>({0, 1}),
        "synchronized output reset flushes coalesced dirty rows");
    ok     &= check(result.viewport_changed,
        "synchronized output reset flushes coalesced viewport change");
    ok     &= check(result.mode_state_changed,
        "synchronized output reset flushes coalesced mode change");
    ok     &= check(snapshot_valid(model, 30U), "synchronized output snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[?2026hA\x1b[?2026l\x1b[?2026hB"));
    ok     &= check(model.mode_state().synchronized_output,
        "same-chunk synchronized output can re-enter");
    ok     &= check(!result.dirty_rows.empty(),
        "same-chunk synchronized output reset releases first session");
    ok     &= check(result.mode_state_changed,
        "same-chunk synchronized output reset releases mode change");

    model   = make_model(2, 4);
    result  = model.ingest(QByteArrayLiteral("A\x1b[?2026hB"));
    ok     &= check(model.mode_state().synchronized_output,
        "pre-entry synchronized output leaves mode active");
    ok     &= check(result.dirty_rows == std::vector<int>({0}),
        "pre-entry dirty row remains immediate");

    result  = model.ingest(QByteArrayLiteral("\x1b[?2026l"));
    ok     &= check(!model.mode_state().synchronized_output,
        "pre-entry synchronized output reset leaves mode inactive");
    ok     &= check(result.dirty_rows == std::vector<int>({0}),
        "pre-entry synchronized output reset flushes only synchronized write");

    model   = make_model(2, 4);
    result  = model.ingest(QByteArrayLiteral("\x1b[2;1H\n\x1b[?2026hX"));
    ok     &= check(model.mode_state().synchronized_output,
        "pre-entry viewport synchronized output leaves mode active");
    ok     &= check(result.viewport_changed,
        "pre-entry viewport change remains immediate");

    model   = make_model(2, 4);
    result  = model.ingest(
        QByteArrayLiteral("\x1b[?2026hA\x1b[?2026l\x1b[2;1HB\x1b[?2026hC"));
    ok     &= check(model.mode_state().synchronized_output,
        "post-reset synchronized output can re-enter");
    ok     &= check(result.dirty_rows == std::vector<int>({0, 1}),
        "post-reset pre-reentry dirty row remains immediate");

    model   = make_model(2, 4);
    result  = model.ingest(QByteArrayLiteral("\x1b[?1049;2026hX"));
    ok     &= check(model.active_buffer_id() == term::Terminal_buffer_id::ALTERNATE,
        "same-CSI pre-entry alternate screen applies");
    ok     &= check(model.mode_state().synchronized_output,
        "same-CSI synchronized output leaves mode active");
    ok     &= check(result.viewport_changed,
        "same-CSI pre-entry viewport change remains immediate");
    ok     &= check(result.dirty_rows == std::vector<int>({0, 1}),
        "same-CSI pre-entry dirty rows remain immediate");

    result  = model.ingest(QByteArrayLiteral("\x1b[?1049;2026l"));
    ok     &= check(model.active_buffer_id() == term::Terminal_buffer_id::PRIMARY,
        "same-CSI synchronized reset leaves alternate screen");
    ok     &= check(!model.mode_state().synchronized_output,
        "same-CSI synchronized reset leaves mode inactive");
    ok     &= check(result.viewport_changed,
        "same-CSI synchronized reset releases coalesced viewport change");
    ok     &= check(!result.dirty_rows.empty(),
        "same-CSI synchronized reset releases coalesced dirty rows");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_mode_state_and_decrqm();
    ok &= test_mouse_tracking_levels();
    ok &= test_cursor_visibility_and_autowrap();
    ok &= test_application_keypad_mode();
    ok &= test_origin_mode_snapshot_and_restore();
    ok &= test_ignored_and_rejected_modes();
    ok &= test_osc8_hyperlinks();
    ok &= test_bell_and_synchronized_output_mutation();
    return ok ? 0 : 1;
}
