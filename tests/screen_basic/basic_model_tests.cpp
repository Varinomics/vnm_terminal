#include "vnm_terminal/internal/terminal_screen_model.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QString>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <variant>

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
    return QByteArray::fromHex(hex);
}

const term::Terminal_render_cell& snapshot_cell_at_index(
    const term::Terminal_render_snapshot&  snapshot,
    std::size_t                            index)
{
    if (index >= snapshot.cells.size()) {
        fail_required_value("expected render snapshot cell");
    }

    return snapshot.cells[index];
}

const term::Terminal_render_cell* snapshot_cell_at_position(
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
    const term::Terminal_render_cell* cell =
        snapshot_cell_at_position(snapshot, row, column);
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

bool check_cell_attribute(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    column,
    term::Terminal_style_attribute         attribute,
    const char*                            label)
{
    bool ok = true;
    const term::Terminal_render_cell* cell =
        snapshot_cell_at_position(snapshot, row, column);
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
    ok &= check(term::terminal_style_has_attribute(style, attribute), label);
    return ok;
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

int title_notification_count(const term::Terminal_screen_model_result& result)
{
    int count = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) != term::Parser_action_kind::NOTIFICATION) {
            continue;
        }

        const term::Parser_notification& notification =
            std::get<term::Parser_notification>(action.payload);
        if (notification.kind == term::Parser_notification_kind::TITLE_CHANGED) {
            ++count;
        }
    }
    return count;
}

int icon_name_notification_count(const term::Terminal_screen_model_result& result)
{
    int count = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) != term::Parser_action_kind::NOTIFICATION) {
            continue;
        }

        const term::Parser_notification& notification =
            std::get<term::Parser_notification>(action.payload);
        if (notification.kind == term::Parser_notification_kind::ICON_NAME_CHANGED) {
            ++count;
        }
    }
    return count;
}

int screen_mutation_count(const term::Terminal_screen_model_result& result)
{
    int count = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::SCREEN_MUTATION) {
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

int terminal_reply_count(const term::Terminal_screen_model_result& result)
{
    int count = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::TERMINAL_REPLY) {
            ++count;
        }
    }
    return count;
}

int host_request_count(const term::Terminal_screen_model_result& result)
{
    int count = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::HOST_REQUEST) {
            ++count;
        }
    }
    return count;
}

const term::Terminal_osc52_write_request& first_host_request(
    const term::Terminal_screen_model_result& result)
{
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::HOST_REQUEST) {
            return std::get<term::Terminal_osc52_write_request>(action.payload);
        }
    }

    fail_required_value("expected OSC 52 host request");
}

const term::Parser_payload_diagnostic& diagnostic_at(
    const term::Terminal_screen_model_result&  result,
    int                                        index)
{
    int current = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) != term::Parser_action_kind::DIAGNOSTIC) {
            continue;
        }

        if (current == index) {
            return std::get<term::Parser_payload_diagnostic>(action.payload);
        }

        ++current;
    }

    fail_required_value("expected diagnostic action at index");
}

bool check_diagnostic_metadata(
    const term::Parser_payload_diagnostic&     diagnostic,
    std::size_t                                raw_payload_size,
    std::size_t                                limit_bytes,
    term::Parser_recovery_strategy             recovery,
    const char*                                label)
{
    bool ok = true;
    ok &= check(diagnostic.raw_payload_size == raw_payload_size, label);
    ok &= check(diagnostic.limit_bytes == limit_bytes, label);
    ok &= check(diagnostic.recovery == recovery, label);
    return ok;
}

term::Terminal_screen_model make_model(int rows = 3, int columns = 6)
{
    term::Terminal_screen_model_config config;
    config.grid_size        = term::terminal_grid_size_t{rows, columns};
    config.scrollback_limit = 4;
    config.tab_width        = 4;
    return term::Terminal_screen_model(config);
}

term::Terminal_screen_model make_model_without_structural_actions(
    int    rows = 3,
    int    columns = 6)
{
    term::Terminal_screen_model_config config;
    config.grid_size                 = term::terminal_grid_size_t{rows, columns};
    config.scrollback_limit          = 4;
    config.tab_width                 = 4;
    config.retain_structural_actions = false;
    return term::Terminal_screen_model(config);
}

term::Terminal_viewport_state make_viewport(int visible_rows)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = visible_rows;
    return viewport;
}

bool check_no_screen_mutation(
    const term::Terminal_screen_model_result&  result,
    const term::Terminal_screen_model&         model,
    const char*                                label)
{
    bool ok = true;
    ok &= check(screen_mutation_count(result) == 0, label);
    ok &= check(result.dirty_rows.empty(), label);
    ok &= check(model.cursor_position().row == 0 && model.cursor_position().column == 0,
        label);
    ok &= check(model.render_snapshot(17U).cells.empty(), label);
    return ok;
}

bool test_config_validation()
{
    bool ok = true;

    ok &= check(term::validate_terminal_screen_model_config({
        term::terminal_grid_size_t{3, 6},
        4,
        4,
        }) == term::Terminal_screen_model_config_status::OK,
        "valid screen model config");
    ok &= check(term::validate_terminal_screen_model_config({
        term::terminal_grid_size_t{0, 6},
        4,
        4,
        }) == term::Terminal_screen_model_config_status::INVALID_GRID_SIZE,
        "invalid model rows rejected");
    ok &= check(term::validate_terminal_screen_model_config({
        term::terminal_grid_size_t{3, 6},
        -1,
        4,
        }) == term::Terminal_screen_model_config_status::INVALID_SCROLLBACK_LIMIT,
        "invalid scrollback rejected");
    ok &= check(term::validate_terminal_screen_model_config({
        term::terminal_grid_size_t{3, 6},
        4,
        0,
        }) == term::Terminal_screen_model_config_status::INVALID_TAB_WIDTH,
        "invalid tab width rejected");

    bool threw = false;
    try {
        term::Terminal_screen_model invalid(
            {
                term::terminal_grid_size_t{3, 0},
                4,
                4,
                });
    }
    catch (const std::invalid_argument&) {
        threw = true;
    }
    ok &= check(threw, "invalid model config fails construction");

    return ok;
}

bool test_structural_action_retention_can_be_disabled()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model_without_structural_actions();
    const term::Terminal_screen_model_result text_result =
        model.ingest(QByteArrayLiteral("abc"));
    ok &= check(text_result.actions.empty(),
        "filtered action mode drops structural printable actions");
    ok &= check(model.row_text(0) == QStringLiteral("abc"),
        "filtered action mode still applies printable text");

    const term::Terminal_screen_model_result bell_result =
        model.ingest(QByteArrayLiteral("\a"));
    ok &= check(screen_mutation_count(bell_result) == 0,
        "filtered action mode drops bell screen mutation");
    ok &= check(bell_result.actions.size() == 1U,
        "filtered action mode retains bell notification");
    ok &= check(!bell_result.actions.empty() &&
        term::parser_action_kind(bell_result.actions.front()) ==
        term::Parser_action_kind::NOTIFICATION,
        "filtered action mode exposes notification action");

    const term::Terminal_screen_model_result title_result =
        model.ingest(QByteArrayLiteral("\x1b]0;filtered\a"));
    ok &= check(screen_mutation_kind_count(
        title_result,
        term::Screen_mutation_kind::SET_TITLE) == 0,
        "filtered action mode drops title screen mutation");
    ok &= check(title_notification_count(title_result) == 1,
        "filtered action mode retains title notification");
    ok &= check(model.title() == QStringLiteral("filtered"),
        "filtered action mode still applies title mutation");

    return ok;
}

bool test_printable_controls_wrap_and_scrollback()
{
    bool                               ok     = true;
    term::Terminal_screen_model        model  = make_model();
    term::Terminal_screen_model_result result = model.ingest(QByteArrayLiteral("abc"));
    ok &= check(diagnostic_count(result) == 0, "ASCII ingest has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("abc"), "ASCII inserted on first row");
    ok &= check(model.cursor_position().row == 0 && model.cursor_position().column == 3,
        "ASCII cursor position");
    ok &= check(result.dirty_rows.size() == 1U && result.dirty_rows[0] == 0,
        "ASCII marks first row dirty");

    result = model.ingest(QByteArrayLiteral("\bZ"));
    ok &= check(model.row_text(0) == QStringLiteral("abZ"), "backspace overwrites previous cell");

    result = model.ingest(QByteArrayLiteral("\rX"));
    ok &= check(model.row_text(0) == QStringLiteral("XbZ"), "carriage return moves to column zero");

    result = model.ingest(QByteArrayLiteral("\r\nY"));
    ok &= check(model.row_text(1) == QStringLiteral("Y"), "CR LF moves to next row");

    result  = model.ingest(QByteArrayLiteral("\tT"));
    ok     &= check(model.cursor_position().row == 1 && model.cursor_position().column == 5,
        "tab advances to configured tab stop");
    ok     &= check(model.row_text(1) == QStringLiteral("Y   T"), "tab advances over blank cells");

    term::Terminal_screen_model wrap_model = make_model(2, 4);
    result  = wrap_model.ingest(QByteArrayLiteral("abcdE"));
    ok     &= check(wrap_model.row_text(0) == QStringLiteral("abcd"), "wrap keeps full first row");
    ok     &= check(wrap_model.row_text(1) == QStringLiteral("E"), "wrap writes next printable on next row");
    ok     &= check(wrap_model.cursor_position().row == 1 && wrap_model.cursor_position().column == 1,
        "wrap cursor after next printable");

    term::Terminal_screen_model erase_wrap_model = make_model(2, 5);
    result  = erase_wrap_model.ingest(QByteArrayLiteral("\x1b[2;1Habcde"));
    ok     &= check(diagnostic_count(result) == 0, "pending-wrap erase setup has no diagnostics");
    result  = erase_wrap_model.ingest(QByteArrayLiteral("\x1b[1;1H12345\x1b[K  \x1b[1C   "));
    ok     &= check(diagnostic_count(result) == 0, "pending-wrap erase line has no diagnostics");
    ok     &= check(erase_wrap_model.row_text(1).isEmpty(),
        "printable after pending-wrap erase line wraps and clears the sparse row");

    term::Terminal_screen_model erase_cr_model = make_model(3, 5);
    result  = erase_cr_model.ingest(QByteArrayLiteral("\x1b[1;1H12345\x1b[K\r\nZ"));
    ok     &= check(diagnostic_count(result) == 0, "pending-wrap erase line CR LF has no diagnostics");
    ok     &= check(erase_cr_model.row_text(1) == QStringLiteral("Z"),
        "CR after pending-wrap erase line cancels wrap before LF");
    ok     &= check(erase_cr_model.row_text(2).isEmpty(),
        "pending-wrap erase line followed by CR LF does not scroll early");

    result  = wrap_model.ingest(QByteArrayLiteral("\r\n1\r\n2\r\n3\r\n4"));
    ok     &= check(wrap_model.scrollback_size() == 4, "scrollback is bounded by configured limit");
    ok     &= check(wrap_model.row_text(1) == QStringLiteral("4"), "scrolling leaves latest row visible");
    ok     &= check(result.dirty_rows.size() == 2U &&
        result.dirty_rows[0] == 0 &&
        result.dirty_rows[1] == 1,
        "scroll marks exact visible rows dirty");
    ok     &= check(result.viewport_changed, "scrollback growth reports viewport change");

    return ok;
}

bool test_printable_ascii_span_semantics()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(3, 4);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("ABCDEFGHI"));
    ok &= check(diagnostic_count(result) == 0, "ASCII span wrap has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("ABCD"),
        "ASCII span fills first row");
    ok &= check(model.row_text(1) == QStringLiteral("EFGH"),
        "ASCII span fills second row");
    ok &= check(model.row_text(2) == QStringLiteral("I"),
        "ASCII span writes remainder after wrap");
    ok &= check(model.cursor_position().row == 2 && model.cursor_position().column == 1,
        "ASCII span cursor after wrap");

    model   = make_model(1, 5);
    result  = model.ingest(bytes_from_hex("e4b880") + QByteArrayLiteral("\x1b[1;2HAB"));
    ok     &= check(diagnostic_count(result) == 0,
        "ASCII span over wide continuation has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral(" AB"),
        "ASCII span over wide continuation clears the full wide cell");
    ok     &= check(term::validate_render_snapshot(model.render_snapshot(40U)).status ==
        term::Terminal_render_snapshot_status::OK,
        "ASCII span over wide continuation snapshot validates");

    model   = make_model(1, 4);
    result  = model.ingest(QByteArrayLiteral("AB") + bytes_from_hex("cc81"));
    ok     &= check(diagnostic_count(result) == 0,
        "combining mark after ASCII span has no diagnostics");
    ok     &= check(model.row_text(0) == QString::fromUtf8("AB\xcc\x81"),
        "combining mark after ASCII span attaches to previous cell");

    model   = make_model(1, 4);
    result  = model.ingest(QByteArrayLiteral("\x1b[41m "));
    ok     &= check(diagnostic_count(result) == 0,
        "styled ASCII space span has no diagnostics");
    const term::Terminal_render_snapshot styled_space_snapshot = model.render_snapshot(41U);
    ok &= check(styled_space_snapshot.cells.size() == 1U,
        "styled ASCII space span materializes the printed space");
    ok &= check_cell_background_palette(
        styled_space_snapshot,
        0,
        0,
        1U,
        "styled ASCII space span keeps current background");

    return ok;
}

bool test_erase_uses_current_style_for_blank_cells()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(1, 8);
    const term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("\x1b[41m\x1b[KHi"));
    ok &= check(diagnostic_count(result) == 0, "styled erase has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("Hi"), "styled erase row text trims blanks");

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(21U);
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled erase snapshot validates");
    ok &= check(snapshot.cells.size() == 8U,
        "styled erase preserves trailing blank cells in the render snapshot");

    for (int column = 0; column < 8; ++column) {
        const term::Terminal_render_cell* cell =
            snapshot_cell_at_position(snapshot, 0, column);
        ok &= check(cell != nullptr, "styled erase snapshot contains every column");
        if (cell == nullptr) {
            continue;
        }

        QString expected_text = QStringLiteral(" ");
        if (column == 0) {
            expected_text = QStringLiteral("H");
        }
        else
        if (column == 1) {
            expected_text = QStringLiteral("i");
        }
        ok &= check(cell->text == expected_text, "styled erase cell text");
        ok &= check_cell_background_palette(
            snapshot,
            0,
            column,
            1U,
            "styled erase cell keeps current background");
    }

    term::Terminal_screen_model whole_line_model = make_model(1, 8);
    const term::Terminal_screen_model_result whole_line_result =
        whole_line_model.ingest(QByteArrayLiteral("\x1b[41mAB\x1b[2KHi"));
    ok &= check(diagnostic_count(whole_line_result) == 0,
        "styled whole-line erase has no diagnostics");
    ok &= check(whole_line_model.row_text(0) == QStringLiteral("  Hi"),
        "styled whole-line erase preserves cursor and trims trailing blanks");

    const term::Terminal_render_snapshot whole_line_snapshot =
        whole_line_model.render_snapshot(22U);
    ok &= check(whole_line_snapshot.cells.size() == 8U,
        "styled whole-line erase materializes the full row");
    for (int column = 0; column < 8; ++column) {
        ok &= check_cell_background_palette(
            whole_line_snapshot,
            0,
            column,
            1U,
            "styled whole-line erase cell keeps current background");
    }

    term::Terminal_screen_model full_screen_model = make_model(2, 4);
    const term::Terminal_screen_model_result full_screen_result =
        full_screen_model.ingest(QByteArrayLiteral("\x1b[41mAB\r\nCD\x1b[2JZ"));
    ok &= check(diagnostic_count(full_screen_result) == 0,
        "styled full-screen erase has no diagnostics");
    ok &= check(full_screen_model.row_text(0).isEmpty(),
        "styled full-screen erase blanks the first row");
    ok &= check(full_screen_model.row_text(1) == QStringLiteral("  Z"),
        "styled full-screen erase preserves cursor and trims trailing blanks");

    const term::Terminal_render_snapshot full_screen_snapshot =
        full_screen_model.render_snapshot(23U);
    ok &= check(full_screen_snapshot.cells.size() == 8U,
        "styled full-screen erase materializes the visible grid");
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 4; ++column) {
            ok &= check_cell_background_palette(
                full_screen_snapshot,
                row,
                column,
                1U,
                "styled full-screen erase cell keeps current background");
        }
    }

    term::Terminal_screen_model erase_character_model = make_model(1, 8);
    const term::Terminal_screen_model_result erase_character_result =
        erase_character_model.ingest(QByteArrayLiteral("\x1b[41mABCD\x1b[1;2H\x1b[2X"));
    ok &= check(diagnostic_count(erase_character_result) == 0,
        "styled character erase has no diagnostics");
    ok &= check(erase_character_model.row_text(0) == QStringLiteral("A  D"),
        "styled character erase blanks the requested span");

    const term::Terminal_render_snapshot erase_character_snapshot =
        erase_character_model.render_snapshot(24U);
    const term::Terminal_render_cell* erased_first =
        snapshot_cell_at_position(erase_character_snapshot, 0, 1);
    const term::Terminal_render_cell* erased_second =
        snapshot_cell_at_position(erase_character_snapshot, 0, 2);
    ok &= check(erased_first != nullptr && erased_first->text == QStringLiteral(" "),
        "styled character erase materializes first blank");
    ok &= check(erased_second != nullptr && erased_second->text == QStringLiteral(" "),
        "styled character erase materializes second blank");
    ok &= check_cell_background_palette(
        erase_character_snapshot,
        0,
        1,
        1U,
        "styled character erase first blank keeps current background");
    ok &= check_cell_background_palette(
        erase_character_snapshot,
        0,
        2,
        1U,
        "styled character erase second blank keeps current background");

    term::Terminal_screen_model wide_erase_character_model = make_model(1, 5);
    const term::Terminal_screen_model_result wide_erase_character_result =
        wide_erase_character_model.ingest(
            QByteArrayLiteral("\x1b[41m") +
            bytes_from_hex("e4b880") +
            QByteArrayLiteral("AB\x1b[1;2H\x1b[X"));
    ok &= check(diagnostic_count(wide_erase_character_result) == 0,
        "styled wide character erase has no diagnostics");
    ok &= check(wide_erase_character_model.row_text(0) == QStringLiteral("  AB"),
        "styled wide character erase blanks the full wide cell");

    const term::Terminal_render_snapshot wide_erase_character_snapshot =
        wide_erase_character_model.render_snapshot(25U);
    ok &= check_cell_background_palette(
        wide_erase_character_snapshot,
        0,
        0,
        1U,
        "styled wide character erase first blank keeps current background");
    ok &= check_cell_background_palette(
        wide_erase_character_snapshot,
        0,
        1,
        1U,
        "styled wide character erase continuation blank keeps current background");

    term::Terminal_screen_model wide_erase_line_model = make_model(1, 5);
    const term::Terminal_screen_model_result wide_erase_line_result =
        wide_erase_line_model.ingest(
            QByteArrayLiteral("\x1b[41m") +
            bytes_from_hex("e4b880") +
            QByteArrayLiteral("AB\x1b[1;2H\x1b[K"));
    ok &= check(diagnostic_count(wide_erase_line_result) == 0,
        "styled wide line erase has no diagnostics");
    ok &= check(wide_erase_line_model.row_text(0).isEmpty(),
        "styled wide line erase blanks through the row end");

    const term::Terminal_render_snapshot wide_erase_line_snapshot =
        wide_erase_line_model.render_snapshot(26U);
    ok &= check(wide_erase_line_snapshot.cells.size() == 5U,
        "styled wide line erase materializes the full row");
    for (int column = 0; column < 5; ++column) {
        ok &= check_cell_background_palette(
            wide_erase_line_snapshot,
            0,
            column,
            1U,
            "styled wide line erase cell keeps current background");
    }

    term::Terminal_screen_model inverse_model = make_model(1, 6);
    const term::Terminal_screen_model_result inverse_result =
        inverse_model.ingest(QByteArrayLiteral("\x1b[7m\x1b[KX"));
    ok &= check(diagnostic_count(inverse_result) == 0,
        "inverse erase has no diagnostics");
    const term::Terminal_render_snapshot inverse_snapshot = inverse_model.render_snapshot(27U);
    ok &= check(inverse_snapshot.cells.size() == 6U,
        "inverse erase preserves trailing blank cells in the render snapshot");
    ok &= check_cell_attribute(
        inverse_snapshot,
        0,
        5,
        term::Terminal_style_attribute::INVERSE,
        "inverse erase blank keeps current inverse attribute");

    return ok;
}

bool test_utf8_replacement_and_snapshot()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model();
    const term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("A") + bytes_from_hex("ff") + QByteArrayLiteral("B"));
    ok &= check(diagnostic_count(result) == 1, "invalid UTF-8 emits diagnostic");
    ok &= check(first_diagnostic(result).code == term::Parser_diagnostic_code::MALFORMED_INPUT,
        "invalid UTF-8 diagnostic code");
    ok &= check(first_diagnostic(result).recovery == term::Parser_recovery_strategy::IGNORE_BYTE,
        "invalid UTF-8 recovery");
    ok &= check(model.row_text(0) == QString::fromUtf8("A\xef\xbf\xbd" "B"),
        "invalid UTF-8 inserts replacement scalar");

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(7U);
    ok &= check(snapshot.metadata.sequence == 7U, "snapshot sequence");
    ok &= check(snapshot.cells.size() == 3U, "snapshot contains non-blank cells");
    const term::Terminal_render_cell& cell_0 = snapshot_cell_at_index(snapshot, 0U);
    const term::Terminal_render_cell& cell_1 = snapshot_cell_at_index(snapshot, 1U);
    const term::Terminal_render_cell& cell_2 = snapshot_cell_at_index(snapshot, 2U);
    ok &= check(cell_0.position.row == 0 && cell_0.position.column == 0,
        "snapshot first cell position");
    ok &= check(cell_0.text == QStringLiteral("A"), "snapshot first cell text");
    ok &= check(cell_1.position.row == 0 && cell_1.position.column == 1,
        "snapshot replacement cell position");
    ok &= check(cell_1.text == QString::fromUtf8("\xef\xbf\xbd"),
        "snapshot replacement cell text");
    ok &= check(cell_2.position.row == 0 && cell_2.position.column == 2,
        "snapshot third cell position");
    ok &= check(cell_2.text == QStringLiteral("B"), "snapshot third cell text");
    ok &= check(cell_0.style_id == term::k_default_terminal_style_id,
        "snapshot default style id");
    ok &= check(cell_0.display_width == 1 && !cell_0.wide_continuation,
        "snapshot default width fields");
    ok &= check(snapshot.cursor.position.row == 0 && snapshot.cursor.position.column == 3,
        "snapshot cursor");
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "snapshot validates");

    term::Terminal_render_snapshot invalid_width = snapshot;
    snapshot_cell_at_index(invalid_width, 0U);
    invalid_width.cells[0].display_width = 0;
    ok &= check(term::validate_render_snapshot(invalid_width).status ==
        term::Terminal_render_snapshot_status::INVALID_CELL_WIDTH,
        "snapshot rejects invalid display width");

    term::Terminal_render_snapshot edge_wide = term::make_empty_render_snapshot(
        term::terminal_grid_size_t{1, 2},
        make_viewport(1),
        8U);
    edge_wide.cells.push_back({
        { 0, 1 },
        QStringLiteral("W"),
        0U,
        2,
        false,
    });
    ok &= check(term::validate_render_snapshot(edge_wide).status ==
        term::Terminal_render_snapshot_status::INVALID_CELL_WIDTH,
        "snapshot rejects wide cell past edge");

    term::Terminal_render_snapshot valid_wide = term::make_empty_render_snapshot(
        term::terminal_grid_size_t{1, 3},
        make_viewport(1),
        9U);
    valid_wide.cells.push_back({
        { 0, 0 },
        QStringLiteral("W"),
        0U,
        2,
        false,
    });
    valid_wide.cells.push_back({
        { 0, 1 },
        {},
        0U,
        0,
        true,
    });
    ok &= check(term::validate_render_snapshot(valid_wide).status ==
        term::Terminal_render_snapshot_status::OK,
        "snapshot accepts explicit wide continuation");

    term::Terminal_render_snapshot mismatched_wide_style = valid_wide;
    term::Terminal_text_style      bold_style            = term::make_default_terminal_text_style();
    term::set_terminal_style_attribute(bold_style, term::Terminal_style_attribute::BOLD);
    mismatched_wide_style.styles.push_back(bold_style);
    snapshot_cell_at_index(mismatched_wide_style, 1U);
    mismatched_wide_style.cells[1].style_id = 1U;
    ok &= check(term::validate_render_snapshot(mismatched_wide_style).status ==
        term::Terminal_render_snapshot_status::INVALID_WIDE_CELL_STYLE,
        "snapshot rejects mismatched wide continuation style");

    term::Terminal_render_snapshot mismatched_wide_hyperlink = valid_wide;
    snapshot_cell_at_index(mismatched_wide_hyperlink, 1U);
    mismatched_wide_hyperlink.cells[1].hyperlink_id = 7U;
    ok &= check(term::validate_render_snapshot(mismatched_wide_hyperlink).status ==
        term::Terminal_render_snapshot_status::INVALID_WIDE_CELL_STYLE,
        "snapshot rejects mismatched wide continuation hyperlink");

    term::Terminal_render_snapshot invalid_style_id = term::make_empty_render_snapshot(
        term::terminal_grid_size_t{1, 2},
        make_viewport(1),
        16U);
    invalid_style_id.cells.push_back({
        { 0, 0 },
        QStringLiteral("X"),
        0U,
        1,
        false,
        1U,
    });
    ok &= check(term::validate_render_snapshot(invalid_style_id).status ==
        term::Terminal_render_snapshot_status::INVALID_STYLE_ID,
        "snapshot rejects out-of-range style id");

    term::Terminal_render_snapshot invalid_default_style = term::make_empty_render_snapshot(
        term::terminal_grid_size_t{1, 2},
        make_viewport(1),
        17U);
    if (invalid_default_style.styles.empty()) {
        fail_required_value("expected default style slot");
    }
    term::set_terminal_style_attribute(
        invalid_default_style.styles[0], term::Terminal_style_attribute::BOLD);
    ok &= check(term::validate_render_snapshot(invalid_default_style).status ==
        term::Terminal_render_snapshot_status::INVALID_STYLE_ID,
        "snapshot rejects non-default style slot zero");

    term::Terminal_render_snapshot missing_continuation = valid_wide;
    missing_continuation.cells.pop_back();
    ok &= check(term::validate_render_snapshot(missing_continuation).status ==
        term::Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION,
        "snapshot rejects missing wide continuation");

    term::Terminal_render_snapshot orphan_continuation = term::make_empty_render_snapshot(
        term::terminal_grid_size_t{1, 3},
        make_viewport(1),
        10U);
    orphan_continuation.cells.push_back({
        { 0, 1 },
        {},
        0U,
        0,
        true,
    });
    ok &= check(term::validate_render_snapshot(orphan_continuation).status ==
        term::Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION,
        "snapshot rejects orphan wide continuation");

    term::Terminal_render_snapshot duplicate_position = term::make_empty_render_snapshot(
        term::terminal_grid_size_t{1, 2},
        make_viewport(1),
        11U);
    duplicate_position.cells.push_back({
        { 0, 0 },
        QStringLiteral("A"),
    });
    duplicate_position.cells.push_back({
        { 0, 0 },
        QStringLiteral("B"),
    });
    ok &= check(term::validate_render_snapshot(duplicate_position).status ==
        term::Terminal_render_snapshot_status::INVALID_CELL_OVERLAP,
        "snapshot rejects duplicate explicit positions");

    term::Terminal_render_snapshot overlapping_base = term::make_empty_render_snapshot(
        term::terminal_grid_size_t{1, 3},
        make_viewport(1),
        12U);
    overlapping_base.cells.push_back({
        { 0, 0 },
        QStringLiteral("W"),
        0U,
        2,
        false,
    });
    overlapping_base.cells.push_back({
        { 0, 1 },
        QStringLiteral("X"),
        0U,
        1,
        false,
    });
    ok &= check(term::validate_render_snapshot(overlapping_base).status ==
        term::Terminal_render_snapshot_status::INVALID_CELL_OVERLAP,
        "snapshot rejects base cell overlap");

    term::Terminal_render_snapshot huge_width = term::make_empty_render_snapshot(
        term::terminal_grid_size_t{1, 2},
        make_viewport(1),
        13U);
    huge_width.cells.push_back({
        { 0, 0 },
        QStringLiteral("X"),
        0U,
        std::numeric_limits<int>::max(),
        false,
    });
    ok &= check(term::validate_render_snapshot(huge_width).status ==
        term::Terminal_render_snapshot_status::INVALID_CELL_WIDTH,
        "snapshot rejects huge display width without overflow");

    term::Terminal_render_snapshot huge_sparse = term::make_empty_render_snapshot(
        term::terminal_grid_size_t{
            std::numeric_limits<int>::max(),
            std::numeric_limits<int>::max(),
        },
        make_viewport(1),
        14U);
    ok &= check(term::validate_render_snapshot(huge_sparse).status ==
        term::Terminal_render_snapshot_status::OK,
        "snapshot validates huge sparse grid without allocation");

    term::Terminal_render_snapshot huge_missing_continuations = term::make_empty_render_snapshot(
        term::terminal_grid_size_t{1, std::numeric_limits<int>::max()},
        make_viewport(1),
        15U);
    huge_missing_continuations.cells.push_back({
        { 0, 0 },
        QStringLiteral("X"),
        0U,
        std::numeric_limits<int>::max(),
        false,
    });
    ok &= check(term::validate_render_snapshot(huge_missing_continuations).status ==
        term::Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION,
        "snapshot rejects huge missing continuations without width loop");

    return ok;
}

bool test_unicode_width_model()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(2, 4);
    term::Terminal_screen_model_result result =
        model.ingest(bytes_from_hex("e4b880") + QByteArrayLiteral("X"));
    ok &= check(diagnostic_count(result) == 0, "CJK wide ingest has no diagnostics");
    ok &= check(model.row_text(0) == QString::fromUtf8("\xe4\xb8\x80" "X"),
        "CJK wide row text");
    ok &= check(model.cursor_position().row == 0 && model.cursor_position().column == 3,
        "CJK wide cursor position");

    term::Terminal_render_snapshot snapshot = model.render_snapshot(20U);
    ok &= check(snapshot.cells.size() == 3U, "CJK wide snapshot cell count");
    const term::Terminal_render_cell& wide_cell_0 = snapshot_cell_at_index(snapshot, 0U);
    const term::Terminal_render_cell& wide_cell_1 = snapshot_cell_at_index(snapshot, 1U);
    const term::Terminal_render_cell& wide_cell_2 = snapshot_cell_at_index(snapshot, 2U);
    ok &= check(wide_cell_0.position.column == 0 &&
        wide_cell_0.display_width == 2 &&
        !wide_cell_0.wide_continuation,
        "CJK wide base cell");
    ok &= check(wide_cell_1.position.column == 1 &&
        wide_cell_1.display_width == 0 &&
        wide_cell_1.wide_continuation,
        "CJK wide continuation cell");
    ok &= check(wide_cell_2.position.column == 2 &&
        wide_cell_2.text == QStringLiteral("X"),
        "CJK wide following cell");
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "CJK wide snapshot validates");

    model   = make_model(2, 4);
    result  = model.ingest(QByteArrayLiteral("abc") + bytes_from_hex("e4b880"));
    ok     &= check(diagnostic_count(result) == 0, "edge CJK wide ingest has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("abc"), "edge CJK keeps first row text");
    ok     &= check(model.row_text(1) == QString::fromUtf8("\xe4\xb8\x80"),
        "edge CJK wraps to next row");
    ok     &= check(model.cursor_position().row == 1 && model.cursor_position().column == 2,
        "edge CJK cursor position");

    model     = make_model(2, 4);
    result    = model.ingest(QByteArrayLiteral("A") + bytes_from_hex("cc81"));
    ok       &= check(diagnostic_count(result) == 0, "combining mark ingest has no diagnostics");
    ok       &= check(model.row_text(0) == QString::fromUtf8("A\xcc\x81"),
        "combining mark attaches to previous cell");
    ok       &= check(model.cursor_position().row == 0 && model.cursor_position().column == 1,
        "combining mark does not advance cursor");
    snapshot  = model.render_snapshot(21U);
    ok       &= check(snapshot.cells.size() == 1U, "combining mark snapshot cell count");
    const term::Terminal_render_cell& combining_cell = snapshot_cell_at_index(snapshot, 0U);
    ok &= check(combining_cell.display_width == 1 && !combining_cell.wide_continuation,
        "combining mark remains single cell");

    model     = make_model(2, 4);
    result    = model.ingest(QByteArrayLiteral(" ") + bytes_from_hex("cc81"));
    ok       &= check(diagnostic_count(result) == 0,
        "combining mark after printed space has no diagnostics");
    ok       &= check(model.row_text(0) == QString::fromUtf8(" \xcc\x81"),
        "combining mark attaches to printed space");
    snapshot  = model.render_snapshot(24U);
    ok       &= check(snapshot.cells.size() == 1U,
        "printed space with combining mark snapshot cell count");
    const term::Terminal_render_cell& space_combining_cell =
        snapshot_cell_at_index(snapshot, 0U);
    ok &= check(space_combining_cell.text == QString::fromUtf8(" \xcc\x81") &&
        space_combining_cell.display_width == 1,
        "printed space with combining mark remains occupied cell");

    model     = make_model(2, 4);
    result    = model.ingest(bytes_from_hex("f09f9880"));
    ok       &= check(diagnostic_count(result) == 0, "emoji ingest has no diagnostics");
    ok       &= check(model.cursor_position().row == 0 && model.cursor_position().column == 2,
        "emoji advances by two cells");
    snapshot  = model.render_snapshot(22U);
    ok       &= check(snapshot.cells.size() == 2U, "emoji snapshot cell count");
    const term::Terminal_render_cell& emoji_cell = snapshot_cell_at_index(snapshot, 0U);
    const term::Terminal_render_cell& emoji_continuation =
        snapshot_cell_at_index(snapshot, 1U);
    ok &= check(emoji_cell.display_width == 2 && emoji_continuation.wide_continuation,
        "emoji snapshot uses wide continuation");

    model     = make_model(2, 4);
    result    = model.ingest(bytes_from_hex("e29da4efb88f"));
    ok       &= check(diagnostic_count(result) == 0, "VS16 heart ingest has no diagnostics");
    ok       &= check(model.cursor_position().row == 0 && model.cursor_position().column == 2,
        "VS16 heart widens cursor position");
    snapshot  = model.render_snapshot(23U);
    ok       &= check(snapshot.cells.size() == 2U, "VS16 heart snapshot cell count");
    const term::Terminal_render_cell& heart_cell = snapshot_cell_at_index(snapshot, 0U);
    const term::Terminal_render_cell& heart_continuation =
        snapshot_cell_at_index(snapshot, 1U);
    ok &= check(heart_cell.text == QString::fromUtf8("\xe2\x9d\xa4\xef\xb8\x8f") &&
        heart_cell.display_width == 2 &&
        heart_continuation.wide_continuation,
        "VS16 heart widens previous cell");

    model     = make_model(2, 4);
    result    = model.ingest(
        QByteArrayLiteral("AB\r") + bytes_from_hex("e29da4efb88f"));
    ok       &= check(diagnostic_count(result) == 0,
        "VS16 heart over occupied neighbor has no diagnostics");
    ok       &= check(model.row_text(0) == QString::fromUtf8("\xe2\x9d\xa4\xef\xb8\x8f"),
        "VS16 heart clears occupied neighbor");
    snapshot  = model.render_snapshot(25U);
    ok       &= check(snapshot.cells.size() == 2U,
        "VS16 heart over occupied neighbor snapshot cell count");
    const term::Terminal_render_cell& overwritten_heart = snapshot_cell_at_index(snapshot, 0U);
    const term::Terminal_render_cell& overwritten_heart_continuation =
        snapshot_cell_at_index(snapshot, 1U);
    ok &= check(overwritten_heart.display_width == 2 &&
        overwritten_heart_continuation.wide_continuation &&
        term::validate_render_snapshot(snapshot).status ==
            term::Terminal_render_snapshot_status::OK,
        "VS16 heart over occupied neighbor snapshot validates");

    model     = make_model(1, 4);
    result    = model.ingest(bytes_from_hex("e28c9aefb88e") + QByteArrayLiteral("A"));
    ok       &= check(diagnostic_count(result) == 0, "VS15 emoji shrink ingest has no diagnostics");
    ok       &= check(model.row_text(0) == QString::fromUtf8("\xe2\x8c\x9a\xef\xb8\x8e" "A"),
        "VS15 emoji shrink clears old continuation");
    ok       &= check(model.cursor_position().row == 0 && model.cursor_position().column == 2,
        "VS15 emoji shrink cursor position");
    snapshot  = model.render_snapshot(26U);
    ok       &= check(snapshot.cells.size() == 2U, "VS15 emoji shrink snapshot cell count");
    ok       &= check(snapshot_cell_at_index(snapshot, 0U).display_width == 1 &&
        snapshot_cell_at_index(snapshot, 1U).position.column == 1,
        "VS15 emoji shrink leaves next column reusable");
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "VS15 emoji shrink snapshot validates");

    model     = make_model(1, 1);
    result    = model.ingest(bytes_from_hex("e29da4efb88f"));
    ok       &= check(diagnostic_count(result) == 0, "one-column VS16 heart has no diagnostics");
    ok       &= check(model.row_text(0) == QString::fromUtf8("\xe2\x9d\xa4\xef\xb8\x8f"),
        "one-column VS16 heart remains representable");
    snapshot  = model.render_snapshot(27U);
    ok       &= check(snapshot.cells.size() == 1U, "one-column VS16 heart snapshot cell count");
    ok       &= check(snapshot_cell_at_index(snapshot, 0U).display_width == 1,
        "one-column VS16 heart is clamped to one cell");
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "one-column VS16 heart snapshot validates");

    model     = make_model(2, 4);
    result    = model.ingest(
        QByteArrayLiteral("abc") + bytes_from_hex("e29da4efb88f"));
    ok       &= check(diagnostic_count(result) == 0, "edge VS16 heart ingest has no diagnostics");
    ok       &= check(model.row_text(0) == QStringLiteral("abc"),
        "edge VS16 heart clears narrow base before wrap");
    ok       &= check(model.row_text(1) == QString::fromUtf8("\xe2\x9d\xa4\xef\xb8\x8f"),
        "edge VS16 heart wraps widened cluster");
    ok       &= check(model.cursor_position().row == 1 && model.cursor_position().column == 2,
        "edge VS16 heart cursor position");
    snapshot  = model.render_snapshot(28U);
    ok       &= check(snapshot.cells.size() == 5U, "edge VS16 heart snapshot cell count");
    const term::Terminal_render_cell& edge_heart = snapshot_cell_at_index(snapshot, 3U);
    const term::Terminal_render_cell& edge_heart_continuation =
        snapshot_cell_at_index(snapshot, 4U);
    ok &= check(edge_heart.position.row == 1 &&
        edge_heart.position.column == 0 &&
        edge_heart.display_width == 2 &&
        edge_heart_continuation.wide_continuation,
        "edge VS16 heart snapshot wraps as wide cell");

    model   = make_model(2, 4);
    result  = model.ingest(QByteArrayLiteral("abcd") + bytes_from_hex("cc81"));
    ok     &= check(diagnostic_count(result) == 0,
        "pending-wrap combining mark has no diagnostics");
    ok     &= check(model.row_text(0) == QString::fromUtf8("abcd\xcc\x81"),
        "pending-wrap combining mark attaches at right edge");
    ok     &= check(model.cursor_position().row == 0 && model.cursor_position().column == 3,
        "pending-wrap combining mark keeps cursor at right edge");
    ok     &= check(term::validate_render_snapshot(model.render_snapshot(29U)).status ==
        term::Terminal_render_snapshot_status::OK,
        "pending-wrap combining snapshot validates");

    model   = make_model(1, 4);
    result  = model.ingest(QByteArrayLiteral("\x1b[?7labcd") + bytes_from_hex("cc81"));
    ok     &= check(diagnostic_count(result) == 0,
        "autowrap-off margin combining mark has no diagnostics");
    ok     &= check(model.row_text(0) == QString::fromUtf8("abcd\xcc\x81"),
        "autowrap-off margin combining mark attaches to right edge");
    ok     &= check(model.cursor_position().row == 0 && model.cursor_position().column == 3,
        "autowrap-off margin combining mark keeps cursor at right edge");
    ok     &= check(term::validate_render_snapshot(model.render_snapshot(30U)).status ==
        term::Terminal_render_snapshot_status::OK,
        "autowrap-off margin combining snapshot validates");

    model   = make_model(2, 4);
    result  = model.ingest(
        bytes_from_hex("e4b880") + QByteArrayLiteral("A\x1b[2;1HB") +
        bytes_from_hex("cc81"));
    ok     &= check(diagnostic_count(result) == 0, "wide visible-text setup has no diagnostics");
    ok     &= check(model.visible_text() == QString::fromUtf8("\xe4\xb8\x80" "A\nB\xcc\x81"),
        "visible text preserves wide and combining clusters");

    return ok;
}

bool test_incremental_byte_stream_boundaries()
{
    bool                               ok     = true;
    term::Terminal_screen_model        model  = make_model();
    term::Terminal_screen_model_result result = model.ingest(bytes_from_hex("e282"));
    ok &= check(diagnostic_count(result) == 0, "split UTF-8 prefix waits for continuation");
    ok &= check_no_screen_mutation(result, model, "split UTF-8 prefix does not mutate screen");

    result  = model.ingest(bytes_from_hex("ac"));
    ok     &= check(diagnostic_count(result) == 0, "split UTF-8 completes without diagnostic");
    ok     &= check(model.row_text(0) == QString::fromUtf8("\xe2\x82\xac"),
        "split UTF-8 inserts completed scalar");

    model   = make_model();
    result  = model.ingest(QByteArrayLiteral("\x1b]0;split"));
    ok     &= check(diagnostic_count(result) == 0, "split OSC title waits for terminator");
    ok     &= check_no_screen_mutation(result, model, "split OSC title does not mutate screen");
    ok     &= check(model.title().isEmpty(), "split OSC title does not publish early");

    result  = model.ingest(QByteArrayLiteral("-title\aX"));
    ok     &= check(diagnostic_count(result) == 0, "split OSC title completes without diagnostic");
    ok     &= check(title_notification_count(result) == 1,
        "split OSC title emits title notification");
    ok     &= check(model.title() == QStringLiteral("split-title"), "split OSC title is published");
    ok     &= check(model.row_text(0) == QStringLiteral("X"), "parser resumes after split OSC");

    model   = make_model();
    result  = model.ingest(
        QByteArrayLiteral("\x1b]0;") +
        bytes_from_hex("e29cbb") +
        QByteArrayLiteral(" Terminal App\a"));
    ok     &= check(diagnostic_count(result) == 0,
        "OSC UTF-8 title continuation byte is not treated as ST");
    ok     &= check(title_notification_count(result) == 1,
        "OSC UTF-8 title emits title notification");
    ok     &= check(screen_mutation_kind_count(result, term::Screen_mutation_kind::PRINT_TEXT) == 0,
        "OSC UTF-8 title with 0x9c continuation emits no leaked text");
    ok     &= check(model.title() ==
        QString::fromUtf8(bytes_from_hex("e29cbb") + QByteArrayLiteral(" Terminal App")),
        "OSC UTF-8 title with 0x9c continuation is published");

    model   = make_model();
    result  = model.ingest(QByteArrayLiteral("\x1b]0;") + bytes_from_hex("e2"));
    ok     &= check(diagnostic_count(result) == 0,
        "split OSC UTF-8 title waits after lead byte");
    ok     &= check(model.title().isEmpty(), "split OSC UTF-8 title does not publish early");

    result  = model.ingest(bytes_from_hex("9cbb") + QByteArrayLiteral(" Terminal App\a"));
    ok     &= check(diagnostic_count(result) == 0,
        "split OSC UTF-8 title continuation byte is not treated as ST");
    ok     &= check(screen_mutation_kind_count(result, term::Screen_mutation_kind::PRINT_TEXT) == 0,
        "split OSC UTF-8 title with 0x9c continuation emits no leaked text");
    ok     &= check(model.title() ==
        QString::fromUtf8(bytes_from_hex("e29cbb") + QByteArrayLiteral(" Terminal App")),
        "split OSC UTF-8 title with 0x9c continuation is published");

    model   = make_model();
    result  = model.ingest(
        QByteArrayLiteral("\x1b]2;") +
        bytes_from_hex("e29cb6") +
        QByteArrayLiteral(" Terminal App\x1b\\"));
    ok     &= check(diagnostic_count(result) == 0,
        "OSC 2 UTF-8 title with ST terminator has no diagnostics");
    ok     &= check(title_notification_count(result) == 1,
        "OSC 2 UTF-8 title with ST terminator emits title notification");
    ok     &= check(screen_mutation_kind_count(result, term::Screen_mutation_kind::PRINT_TEXT) == 0,
        "OSC 2 UTF-8 title with ST terminator emits no leaked text");
    ok     &= check(model.title() ==
        QString::fromUtf8(bytes_from_hex("e29cb6") + QByteArrayLiteral(" Terminal App")),
        "OSC 2 UTF-8 title with ST terminator is published");

    model = make_model();
    QByteArray raw_st_title = QByteArrayLiteral("\x1b]0;raw-st");
    raw_st_title.append(static_cast<char>(0x9c));
    result  = model.ingest(raw_st_title);
    ok     &= check(diagnostic_count(result) == 0, "OSC raw 8-bit ST title has no diagnostic");
    ok     &= check(model.title() == QStringLiteral("raw-st"),
        "OSC raw 8-bit ST still terminates title");

    model = make_model();
    QByteArray malformed_utf8_st_title = QByteArrayLiteral("\x1b]0;");
    malformed_utf8_st_title.append(bytes_from_hex("e0"));
    malformed_utf8_st_title.append(static_cast<char>(0x9c));
    malformed_utf8_st_title.append('X');
    result = model.ingest(malformed_utf8_st_title);
    ok &= check(model.row_text(0) == QStringLiteral("X"),
        "malformed OSC UTF-8 does not hide raw 8-bit ST terminator");

    model   = make_model();
    result  = model.ingest(QByteArrayLiteral("\x1b[?9999"));
    ok     &= check(diagnostic_count(result) == 0, "split CSI waits for final byte");
    ok     &= check_no_screen_mutation(result, model, "split CSI does not leak parameters");

    result  = model.ingest(QByteArrayLiteral("hX"));
    ok     &= check(diagnostic_count(result) == 1, "unsupported split CSI emits diagnostic");
    ok     &= check(first_diagnostic(result).family == term::Parser_sequence_family::CSI,
        "unsupported split CSI diagnostic family");
    ok     &= check(model.row_text(0) == QStringLiteral("X"), "parser resumes after split CSI");

    model   = make_model();
    result  = model.ingest(QByteArrayLiteral("\x1bPsplit\x1b"));
    ok     &= check(diagnostic_count(result) == 0, "split ST prefix waits inside string");
    ok     &= check_no_screen_mutation(result, model, "split ST prefix does not mutate screen");

    result  = model.ingest(QByteArrayLiteral("\\Z"));
    ok     &= check(diagnostic_count(result) == 1, "split ST terminates string");
    ok     &= check(first_diagnostic(result).family == term::Parser_sequence_family::DCS,
        "split ST diagnostic family");
    ok     &= check(model.row_text(0) == QStringLiteral("Z"), "parser resumes after split ST");

    return ok;
}

bool test_osc_title_limit_and_clipboard_deny()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model();
    term::Terminal_screen_model_result result =
        model.ingest(QByteArray("\x1b]0;first\a", 10));
    ok &= check(diagnostic_count(result) == 0, "OSC title has no diagnostics");
    ok &= check(title_notification_count(result) == 1, "OSC title emits title notification");
    ok &= check(icon_name_notification_count(result) == 1,
        "OSC 0 emits icon name notification");
    ok &= check(model.title() == QStringLiteral("first"), "OSC 0 sets title");
    ok &= check(model.icon_name() == QStringLiteral("first"), "OSC 0 sets icon name");

    result  = model.ingest(QByteArrayLiteral("\x1b]1;spinner-frame\a"));
    ok     &= check(diagnostic_count(result) == 0, "OSC 1 icon name has no diagnostics");
    ok     &= check(title_notification_count(result) == 0,
        "OSC 1 does not emit title notification");
    ok     &= check(icon_name_notification_count(result) == 1,
        "OSC 1 emits icon name notification");
    ok     &= check(model.title() == QStringLiteral("first"), "OSC 1 preserves title");
    ok     &= check(model.icon_name() == QStringLiteral("spinner-frame"),
        "OSC 1 sets icon name");

    result  = model.ingest(QByteArrayLiteral("\x1b]2;window-title\a"));
    ok     &= check(diagnostic_count(result) == 0, "OSC 2 title has no diagnostics");
    ok     &= check(title_notification_count(result) == 1, "OSC 2 emits title notification");
    ok     &= check(icon_name_notification_count(result) == 0,
        "OSC 2 does not emit icon name notification");
    ok     &= check(model.title() == QStringLiteral("window-title"), "OSC 2 sets title");
    ok     &= check(model.icon_name() == QStringLiteral("spinner-frame"),
        "OSC 2 preserves icon name");

    QByteArray title_overflow("\x1b]2;", 4);
    title_overflow.append(QByteArray(static_cast<int>(term::k_title_scalar_limit + 1U), 'a'));
    title_overflow.append('\a');
    result  = model.ingest(title_overflow);
    ok     &= check(model.title() == QStringLiteral("window-title"),
        "title overflow preserves previous title");
    ok     &= check(diagnostic_count(result) == 1, "title overflow emits diagnostic");
    ok     &= check(title_notification_count(result) == 0, "title overflow emits no notification");
    ok     &= check(screen_mutation_kind_count(result, term::Screen_mutation_kind::SET_TITLE) == 0,
        "title overflow emits no title mutation");
    ok     &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::TITLE_LIMIT_EXCEEDED,
        "title overflow diagnostic code");
    ok     &= check(first_diagnostic(result).family == term::Parser_sequence_family::OSC,
        "title overflow diagnostic family");
    ok     &= check(first_diagnostic(result).recovery ==
        term::Parser_recovery_strategy::DISCARD_STRING,
        "title overflow recovery");
    ok     &= check(first_diagnostic(result).limit_bytes == term::k_title_scalar_limit,
        "title overflow limit");
    ok     &= check(first_diagnostic(result).raw_payload_size == term::k_title_scalar_limit + 1U,
        "title overflow raw size");

    QByteArray icon_name_overflow("\x1b]1;", 4);
    icon_name_overflow.append(
        QByteArray(static_cast<int>(term::k_title_scalar_limit + 1U), 'b'));
    icon_name_overflow.append('\a');
    result  = model.ingest(icon_name_overflow);
    ok     &= check(model.title() == QStringLiteral("window-title"),
        "OSC 1 overflow preserves previous title");
    ok     &= check(model.icon_name() == QStringLiteral("spinner-frame"),
        "OSC 1 overflow preserves previous icon name");
    ok     &= check(diagnostic_count(result) == 1, "OSC 1 overflow emits diagnostic");
    ok     &= check(title_notification_count(result) == 0,
        "OSC 1 overflow emits no title notification");
    ok     &= check(icon_name_notification_count(result) == 0,
        "OSC 1 overflow emits no icon name notification");
    ok     &= check(screen_mutation_kind_count(result, term::Screen_mutation_kind::SET_TITLE) == 0,
        "OSC 1 overflow emits no title mutation");
    ok     &= check(
        screen_mutation_kind_count(result, term::Screen_mutation_kind::SET_ICON_NAME) == 0,
        "OSC 1 overflow emits no icon name mutation");
    ok     &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::TITLE_LIMIT_EXCEEDED,
        "OSC 1 overflow diagnostic code");
    ok     &= check(first_diagnostic(result).limit_bytes == term::k_title_scalar_limit,
        "OSC 1 overflow limit");
    ok     &= check(first_diagnostic(result).raw_payload_size == term::k_title_scalar_limit + 1U,
        "OSC 1 overflow raw size");

    QByteArray combined_title_overflow("\x1b]0;", 4);
    combined_title_overflow.append(
        QByteArray(static_cast<int>(term::k_title_scalar_limit + 1U), 'c'));
    combined_title_overflow.append('\a');
    result  = model.ingest(combined_title_overflow);
    ok     &= check(model.title() == QStringLiteral("window-title"),
        "OSC 0 overflow preserves previous title");
    ok     &= check(model.icon_name() == QStringLiteral("spinner-frame"),
        "OSC 0 overflow preserves previous icon name");
    ok     &= check(diagnostic_count(result) == 1, "OSC 0 overflow emits diagnostic");
    ok     &= check(title_notification_count(result) == 0,
        "OSC 0 overflow emits no title notification");
    ok     &= check(icon_name_notification_count(result) == 0,
        "OSC 0 overflow emits no icon name notification");
    ok     &= check(screen_mutation_kind_count(result, term::Screen_mutation_kind::SET_TITLE) == 0,
        "OSC 0 overflow emits no title mutation");
    ok     &= check(
        screen_mutation_kind_count(result, term::Screen_mutation_kind::SET_ICON_NAME) == 0,
        "OSC 0 overflow emits no icon name mutation");
    ok     &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::TITLE_LIMIT_EXCEEDED,
        "OSC 0 overflow diagnostic code");
    ok     &= check(first_diagnostic(result).limit_bytes == term::k_title_scalar_limit,
        "OSC 0 overflow limit");
    ok     &= check(first_diagnostic(result).raw_payload_size == term::k_title_scalar_limit + 1U,
        "OSC 0 overflow raw size");

    model   = make_model();
    result  = model.ingest(QByteArrayLiteral("\x1b]52;c;Y29weQ==\x1b\\"));
    ok     &= check(host_request_count(result) == 1, "OSC 52 write emits host request");
    ok     &= check(first_host_request(result).request_id == 1U, "OSC 52 write request id");
    ok     &= check(first_host_request(result).target_selection == QStringLiteral("c"),
        "OSC 52 write target selection");
    ok     &= check(first_host_request(result).decoded_payload == QByteArrayLiteral("copy"),
        "OSC 52 write decoded payload");
    ok     &= check(first_host_request(result).raw_payload_size ==
        static_cast<std::size_t>(QByteArrayLiteral("52;c;Y29weQ==").size()),
        "OSC 52 write raw payload size");
    ok     &= check(first_host_request(result).source_sequence == QStringLiteral("OSC 52 write"),
        "OSC 52 write source sequence");
    ok     &= check(diagnostic_count(result) == 1, "OSC 52 write emits deny diagnostic");
    ok     &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::CLIPBOARD_WRITE_DENIED,
        "OSC 52 write deny diagnostic code");
    ok     &= check_diagnostic_metadata(
        first_diagnostic(result),
        static_cast<std::size_t>(QByteArrayLiteral("52;c;Y29weQ==").size()),
        term::k_osc_payload_limit_bytes,
        term::Parser_recovery_strategy::DISCARD_STRING,
        "OSC 52 write deny metadata");
    ok     &= check(terminal_reply_count(result) == 0, "OSC 52 write emits no terminal reply");
    ok     &= check_no_screen_mutation(result, model, "OSC 52 write does not mutate screen");

    model   = make_model();
    result  = model.ingest(QByteArrayLiteral("\x1b]52;c;not*base64\x1b\\"));
    ok     &= check(host_request_count(result) == 0, "malformed OSC 52 write emits no host request");
    ok     &= check(diagnostic_count(result) == 1, "malformed OSC 52 write emits diagnostic");
    ok     &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::CLIPBOARD_WRITE_DENIED,
        "malformed OSC 52 write deny diagnostic code");
    ok     &= check_diagnostic_metadata(
        first_diagnostic(result),
        static_cast<std::size_t>(QByteArrayLiteral("52;c;not*base64").size()),
        term::k_osc_payload_limit_bytes,
        term::Parser_recovery_strategy::DISCARD_STRING,
        "malformed OSC 52 write deny metadata");
    ok     &= check_no_screen_mutation(result, model, "malformed OSC 52 write does not mutate screen");

    model   = make_model();
    result  = model.ingest(QByteArrayLiteral("\x1b]52;c\x1b\\"));
    ok     &= check(host_request_count(result) == 0,
        "malformed OSC 52 write shape emits no host request");
    ok     &= check(diagnostic_count(result) == 1, "malformed OSC 52 write shape emits diagnostic");
    ok     &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::CLIPBOARD_WRITE_DENIED,
        "malformed OSC 52 write shape deny diagnostic code");
    ok     &= check_diagnostic_metadata(
        first_diagnostic(result),
        static_cast<std::size_t>(QByteArrayLiteral("52;c").size()),
        term::k_osc_payload_limit_bytes,
        term::Parser_recovery_strategy::DISCARD_STRING,
        "malformed OSC 52 write shape deny metadata");
    ok     &= check_no_screen_mutation(result, model,
        "malformed OSC 52 write shape does not mutate screen");

    model   = make_model();
    result  = model.ingest(QByteArrayLiteral("\x1b]52;c;?\x1b\\"));
    ok     &= check(host_request_count(result) == 0, "OSC 52 read emits no host request");
    ok     &= check(diagnostic_count(result) == 1, "OSC 52 read emits deny diagnostic");
    ok     &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::CLIPBOARD_READ_DENIED,
        "OSC 52 read deny diagnostic code");
    ok     &= check_diagnostic_metadata(
        first_diagnostic(result),
        static_cast<std::size_t>(QByteArrayLiteral("52;c;?").size()),
        term::k_osc_payload_limit_bytes,
        term::Parser_recovery_strategy::DISCARD_STRING,
        "OSC 52 read deny metadata");
    ok     &= check(terminal_reply_count(result) == 0, "OSC 52 read emits no terminal reply");
    ok     &= check_no_screen_mutation(result, model, "OSC 52 read does not mutate screen");

    return ok;
}

QByteArray over_limit_string(char introducer, std::size_t limit)
{
    QByteArray bytes;
    bytes.append('\x1b');
    bytes.append(introducer);
    bytes.append(QByteArray(static_cast<int>(limit + 1U), 'x'));
    bytes.append("\x1b\\", 2);
    bytes.append('R');
    return bytes;
}

bool check_over_limit_family(
    char                           introducer,
    std::size_t                    limit,
    term::Parser_sequence_family   family,
    const char*                    label)
{
    term::Terminal_screen_model model = make_model();
    const term::Terminal_screen_model_result result =
        model.ingest(over_limit_string(introducer, limit));

    bool ok = true;
    ok &= check(diagnostic_count(result) == 1, label);
    ok &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::PAYLOAD_LIMIT_EXCEEDED,
        label);
    ok &= check(first_diagnostic(result).family == family, label);
    ok &= check_diagnostic_metadata(
        first_diagnostic(result),
        limit + 1U,
        limit,
        term::Parser_recovery_strategy::DISCARD_STRING,
        label);
    ok &= check(model.row_text(0) == QStringLiteral("R"), label);
    return ok;
}

bool test_string_payload_limits_and_recovery()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model();
    QByteArray over_limit_osc("\x1b]", 2);
    over_limit_osc.append(QByteArray(static_cast<int>(term::k_osc_payload_limit_bytes + 1U), 'x'));
    over_limit_osc.append('\a');
    over_limit_osc.append("OK");

    term::Terminal_screen_model_result result = model.ingest(over_limit_osc);
    ok &= check(diagnostic_count(result) == 1, "over-limit OSC emits diagnostic");
    ok &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::PAYLOAD_LIMIT_EXCEEDED,
        "over-limit OSC diagnostic code");
    ok &= check(first_diagnostic(result).family == term::Parser_sequence_family::OSC,
        "over-limit OSC diagnostic family");
    ok &= check_diagnostic_metadata(
        first_diagnostic(result),
        term::k_osc_payload_limit_bytes + 1U,
        term::k_osc_payload_limit_bytes,
        term::Parser_recovery_strategy::DISCARD_STRING,
        "over-limit OSC diagnostic metadata");
    ok &= check(model.row_text(0) == QStringLiteral("OK"), "parser recovers after OSC terminator");

    ok &= check_over_limit_family(
        'P',
        term::k_dcs_payload_limit_bytes,
        term::Parser_sequence_family::DCS,
        "DCS over-limit discard");
    ok &= check_over_limit_family(
        '_',
        term::k_apc_payload_limit_bytes,
        term::Parser_sequence_family::APC,
        "APC over-limit discard");
    ok &= check_over_limit_family(
        '^',
        term::k_pm_payload_limit_bytes,
        term::Parser_sequence_family::PM,
        "PM over-limit discard");
    ok &= check_over_limit_family(
        'X',
        term::k_sos_payload_limit_bytes,
        term::Parser_sequence_family::SOS,
        "SOS over-limit discard");

    model = make_model();
    QByteArray exact_limit_dcs("\x1bP", 2);
    exact_limit_dcs.append(QByteArray(static_cast<int>(term::k_dcs_payload_limit_bytes), 'x'));
    exact_limit_dcs.append("\x1b\\R", 3);
    result  = model.ingest(exact_limit_dcs);
    ok     &= check(diagnostic_count(result) == 1, "DCS exact payload limit accepted");
    ok     &= check(first_diagnostic(result).code == term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "DCS exact payload limit remains unsupported sequence");
    ok     &= check_diagnostic_metadata(
        first_diagnostic(result),
        term::k_dcs_payload_limit_bytes,
        term::k_dcs_payload_limit_bytes,
        term::Parser_recovery_strategy::DISCARD_STRING,
        "DCS exact payload limit diagnostic metadata");
    ok     &= check(model.row_text(0) == QStringLiteral("R"), "DCS exact limit resumes after ST");

    model = make_model();
    QByteArray incremental_dcs("\x1bP", 2);
    incremental_dcs.append(
        QByteArray(static_cast<int>(term::k_dcs_payload_limit_bytes - 1U), 'x'));
    result  = model.ingest(incremental_dcs);
    ok     &= check(diagnostic_count(result) == 0, "incremental DCS waits below limit");
    ok     &= check_no_screen_mutation(result, model, "incremental DCS below limit does not mutate");

    result  = model.ingest(QByteArrayLiteral("xx"));
    ok     &= check(diagnostic_count(result) == 1, "incremental DCS diagnoses when crossing limit");
    ok     &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::PAYLOAD_LIMIT_EXCEEDED,
        "incremental DCS over-limit code");
    ok     &= check_diagnostic_metadata(
        first_diagnostic(result),
        term::k_dcs_payload_limit_bytes + 1U,
        term::k_dcs_payload_limit_bytes,
        term::Parser_recovery_strategy::DISCARD_STRING,
        "incremental DCS over-limit metadata");
    ok     &= check_no_screen_mutation(result, model, "incremental DCS over-limit does not mutate");

    result  = model.ingest(QByteArrayLiteral("\x1b\\R"));
    ok     &= check(diagnostic_count(result) == 0, "incremental DCS over-limit emits no second diagnostic");
    ok     &= check(model.row_text(0) == QStringLiteral("R"),
        "incremental DCS resumes after over-limit ST");

    model   = make_model();
    result  = model.ingest(QByteArrayLiteral("\x1bPignored\x1b\\Z"));
    ok     &= check(diagnostic_count(result) == 1, "unsupported DCS emits diagnostic");
    ok     &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "unsupported DCS diagnostic code");
    ok     &= check(first_diagnostic(result).recovery ==
        term::Parser_recovery_strategy::DISCARD_STRING,
        "unsupported DCS recovery strategy");
    ok     &= check(model.row_text(0) == QStringLiteral("Z"), "parser resumes after unsupported DCS");

    model   = make_model();
    result  = model.ingest(QByteArrayLiteral("\x1bPignored\aVISIBLE\x1b\\Z"));
    ok     &= check(diagnostic_count(result) == 1, "DCS ignores BEL terminator");
    ok     &= check(model.row_text(0) == QStringLiteral("Z"), "DCS BEL does not leak payload text");

    model   = make_model();
    result  = model.ingest(QByteArrayLiteral("\x1bPignored\x1b[?9999hX"));
    ok     &= check(diagnostic_count(result) == 2, "DCS recovers at CSI boundary");
    ok     &= check(diagnostic_at(result, 0).family == term::Parser_sequence_family::DCS,
        "DCS recovery diagnostic family");
    ok     &= check(diagnostic_at(result, 0).recovery ==
        term::Parser_recovery_strategy::RESET_TO_GROUND,
        "DCS recovery diagnostic strategy");
    ok     &= check(diagnostic_at(result, 1).family == term::Parser_sequence_family::CSI,
        "CSI after DCS recovery diagnostic family");
    ok     &= check(model.row_text(0) == QStringLiteral("X"), "parser resumes after DCS recovery");

    model = make_model();
    QByteArray recovery_over_limit("\x1bP", 2);
    recovery_over_limit.append(
        QByteArray(static_cast<int>(term::k_dcs_payload_limit_bytes + 1U), 'x'));
    result  = model.ingest(recovery_over_limit);
    ok     &= check(diagnostic_count(result) == 1, "over-limit DCS before recovery boundary");
    ok     &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::PAYLOAD_LIMIT_EXCEEDED,
        "over-limit DCS recovery setup code");

    result  = model.ingest(QByteArrayLiteral("\x1b[?9999hY"));
    ok     &= check(diagnostic_count(result) == 1, "over-limit DCS recovers at CSI boundary");
    ok     &= check(first_diagnostic(result).family == term::Parser_sequence_family::CSI,
        "over-limit DCS recovery reprocesses CSI");
    ok     &= check(model.row_text(0) == QStringLiteral("Y"),
        "parser resumes after over-limit DCS recovery");

    model   = make_model();
    result  = model.ingest(QByteArrayLiteral("\x1b_ignored\x1b\\A\x1b^ignored\x1b\\B\x1bXignored\x1b\\C"));
    ok     &= check(diagnostic_count(result) == 3, "APC PM SOS unsupported diagnostics");
    ok     &= check(diagnostic_at(result, 0).family == term::Parser_sequence_family::APC,
        "APC diagnostic family");
    ok     &= check(diagnostic_at(result, 1).family == term::Parser_sequence_family::PM,
        "PM diagnostic family");
    ok     &= check(diagnostic_at(result, 2).family == term::Parser_sequence_family::SOS,
        "SOS diagnostic family");
    ok     &= check(diagnostic_at(result, 0).recovery ==
        term::Parser_recovery_strategy::DISCARD_STRING &&
        diagnostic_at(result, 1).recovery ==
            term::Parser_recovery_strategy::DISCARD_STRING &&
        diagnostic_at(result, 2).recovery ==
            term::Parser_recovery_strategy::DISCARD_STRING,
        "APC PM SOS recovery strategy");
    ok     &= check(model.row_text(0) == QStringLiteral("ABC"), "parser recovers after APC PM SOS");

    model   = make_model();
    result  = model.ingest(QByteArrayLiteral("\x1b]0;unterminated"));
    ok     &= check(diagnostic_count(result) == 0, "split unterminated string waits for more bytes");
    ok     &= check_no_screen_mutation(result, model, "unterminated title does not mutate screen");
    ok     &= check(model.title().isEmpty(), "unterminated title does not mutate title");

    return ok;
}

bool test_bounded_csi_recovery()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model();
    QByteArray overlong_csi("\x1b[", 2);
    overlong_csi.append(
        QByteArray(static_cast<int>(term::k_control_sequence_pending_limit_bytes), ';'));

    term::Terminal_screen_model_result result = model.ingest(overlong_csi);
    ok &= check(diagnostic_count(result) == 1, "overlong CSI emits diagnostic");
    ok &= check(first_diagnostic(result).family == term::Parser_sequence_family::CSI,
        "overlong CSI diagnostic family");
    ok &= check(first_diagnostic(result).recovery ==
        term::Parser_recovery_strategy::DISCARD_SEQUENCE,
        "overlong CSI recovery strategy");
    ok &= check_no_screen_mutation(result, model, "overlong CSI does not mutate screen");

    result  = model.ingest(QByteArray(128, ';'));
    ok     &= check(diagnostic_count(result) == 0, "discarding CSI ignores continuation bytes");
    ok     &= check_no_screen_mutation(result, model, "discarding CSI continuation does not mutate");

    result  = model.ingest(QByteArrayLiteral("mZ"));
    ok     &= check(diagnostic_count(result) == 0, "discarding CSI stops at final byte");
    ok     &= check(model.row_text(0) == QStringLiteral("Z"), "parser resumes after overlong CSI");

    return ok;
}

bool test_unsupported_escape_does_not_leak()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model();
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("\x1b[?9999hX"));
    ok &= check(diagnostic_count(result) == 1, "unsupported CSI emits diagnostic");
    ok &= check(first_diagnostic(result).family == term::Parser_sequence_family::CSI,
        "unsupported CSI diagnostic family");
    ok &= check(model.row_text(0) == QStringLiteral("X"), "unsupported CSI does not leak text");

    result  = model.ingest(QByteArray("\x1b", 1) + QByteArrayLiteral("cY"));
    ok     &= check(diagnostic_count(result) == 1, "unsupported ESC emits diagnostic");
    ok     &= check(first_diagnostic(result).family == term::Parser_sequence_family::ESC,
        "unsupported ESC diagnostic family");
    ok     &= check(model.row_text(0) == QStringLiteral("XY"), "unsupported ESC does not leak text");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_config_validation();
    ok &= test_structural_action_retention_can_be_disabled();
    ok &= test_printable_controls_wrap_and_scrollback();
    ok &= test_printable_ascii_span_semantics();
    ok &= test_erase_uses_current_style_for_blank_cells();
    ok &= test_utf8_replacement_and_snapshot();
    ok &= test_unicode_width_model();
    ok &= test_incremental_byte_stream_boundaries();
    ok &= test_osc_title_limit_and_clipboard_deny();
    ok &= test_string_payload_limits_and_recovery();
    ok &= test_bounded_csi_recovery();
    ok &= test_unsupported_escape_does_not_leak();
    return ok ? 0 : 1;
}
