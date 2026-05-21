#include "vnm_terminal/internal/selection_contract.h"
#include "vnm_terminal/internal/viewport_contract.h"
#include "helpers/test_check.h"

#include <QString>

#include <iostream>
#include <limits>
#include <span>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

bool test_follow_tail_and_detached_output()
{
    bool ok = true;

    term::Terminal_viewport_controller viewport;
    ok &= check(viewport.set_visible_rows(3).code ==
        term::Terminal_viewport_result_code::ACCEPTED,
        "viewport accepts visible row count");

    viewport.set_scrollback_rows(5);
    ok &= check(viewport.state().offset_from_tail == 0,
        "follow-tail remains at tail after output");
    ok &= check(viewport.first_visible_logical_row() == 5,
        "tail viewport starts after scrollback rows");

    const term::Terminal_viewport_scroll_result scroll_result = viewport.scroll_lines(2);
    ok &= check(scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "scrollback movement reports viewport movement");
    ok &= check(scroll_result.applied_line_delta == 2,
        "scrollback movement reports applied line delta");
    ok &= check(!viewport.state().follow_tail, "scrollback movement detaches from tail");
    ok &= check(viewport.first_visible_logical_row() == 3,
        "detached viewport maps first visible row");

    viewport.set_scrollback_rows(8);
    ok &= check(viewport.state().offset_from_tail == 5,
        "detached viewport preserves viewed logical row when output grows");
    ok &= check(viewport.first_visible_logical_row() == 3,
        "detached output growth preserves first visible row");

    viewport.sync_scrollback_rows(8, 2);
    ok &= check(viewport.state().offset_from_tail == 7,
        "detached viewport tracks bounded scrollback eviction");
    ok &= check(viewport.first_visible_logical_row() == 1,
        "scrollback eviction rebases preserved first visible row");

    viewport.notify_user_input();
    ok &= check(viewport.state().follow_tail, "user input returns viewport to tail");
    ok &= check(viewport.state().offset_from_tail == 0,
        "tail restoration clears viewport offset");

    return ok;
}

bool test_page_scroll_and_bounds()
{
    bool ok = true;

    term::Terminal_viewport_controller viewport;
    viewport.set_visible_rows(4);
    viewport.set_scrollback_rows(9);

    term::Terminal_viewport_scroll_result result = viewport.scroll_pages(1);
    ok &= check(result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "page scroll moves viewport");
    ok &= check(result.applied_line_delta == 4, "page scroll uses visible row count");
    ok &= check(viewport.state().offset_from_tail == 4, "page scroll updates offset");

    result  = viewport.scroll_pages(4);
    ok     &= check(result.applied_line_delta == 5,
        "page scroll clamps to top of scrollback");
    ok     &= check(viewport.state().offset_from_tail == viewport.max_offset_from_tail(),
        "page scroll clamps at maximum offset");

    result  = viewport.scroll_lines(1);
    ok     &= check(result.action == term::Terminal_viewport_scroll_action::AT_BOUNDARY,
        "scroll at top reports boundary");
    ok     &= check(result.applied_line_delta == 0,
        "boundary scroll has no applied delta");

    result  = viewport.scroll_pages(-3);
    ok     &= check(result.applied_line_delta == -9,
        "negative page scroll returns to tail");
    ok     &= check(viewport.state().follow_tail, "negative page scroll follows tail at zero");

    viewport.set_scrollback_rows(20);
    viewport.scroll_lines(3);
    result  = viewport.scroll_lines(std::numeric_limits<int>::max());
    ok     &= check(result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "maximum positive line delta from detached offset moves viewport");
    ok     &= check(viewport.state().offset_from_tail == viewport.max_offset_from_tail(),
        "maximum positive line delta from detached offset clamps at scrollback top");

    result  = viewport.scroll_lines(std::numeric_limits<int>::min());
    ok     &= check(result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "maximum negative line delta moves viewport");
    ok     &= check(viewport.state().offset_from_tail == 0,
        "maximum negative line delta clamps at tail");

    result  = viewport.scroll_pages(std::numeric_limits<int>::max());
    ok     &= check(result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "maximum positive page delta moves viewport");
    ok     &= check(viewport.state().offset_from_tail == viewport.max_offset_from_tail(),
        "maximum positive page delta clamps at scrollback top");

    return ok;
}

bool test_alternate_screen_policy_and_restore()
{
    bool ok = true;

    term::Terminal_viewport_controller viewport;
    viewport.set_visible_rows(5);
    viewport.set_scrollback_rows(20);
    viewport.scroll_lines(6);

    viewport.enter_alternate_screen();
    ok &= check(viewport.state().active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "alternate screen becomes active");
    ok &= check(viewport.state().scrollback_rows == 0,
        "alternate screen exposes no primary scrollback");
    ok &= check(viewport.state().offset_from_tail == 0 && viewport.state().follow_tail,
        "alternate screen starts at tail");

    term::Terminal_viewport_scroll_result result = viewport.scroll_lines(3);
    ok &= check(result.action == term::Terminal_viewport_scroll_action::AT_BOUNDARY,
        "alternate keep-tail policy does not move viewport");

    viewport.set_alternate_screen_scroll_policy(
        term::Terminal_alternate_screen_scroll_policy::WHEEL_TO_TERMINAL_INPUT);
    result = viewport.scroll_lines(3);
    ok &= check(result.action == term::Terminal_viewport_scroll_action::TERMINAL_INPUT,
        "alternate wheel policy requests terminal input");

    viewport.sync_scrollback_rows(20, 5);
    viewport.leave_alternate_screen();
    ok &= check(viewport.state().active_buffer == term::Terminal_buffer_id::PRIMARY,
        "leaving alternate returns to primary");
    ok &= check(viewport.state().scrollback_rows == 20,
        "leaving alternate restores primary scrollback rows");
    ok &= check(viewport.state().offset_from_tail == 11,
        "leaving alternate restores primary viewport after eviction");
    ok &= check(viewport.state().alternate_screen_scroll_policy ==
        term::Terminal_alternate_screen_scroll_policy::WHEEL_TO_TERMINAL_INPUT,
        "alternate scroll policy remains recorded");

    return ok;
}

bool test_coordinate_mapping_and_selection_rows()
{
    bool ok = true;

    term::Terminal_viewport_controller viewport;
    viewport.set_visible_rows(3);
    viewport.set_scrollback_rows(8);
    viewport.scroll_lines(2);

    term::Terminal_viewport_coordinate_result coordinate =
        viewport.viewport_row_to_logical_row(0);
    ok &= check(coordinate.code == term::Terminal_viewport_coordinate_result_code::OK,
        "top viewport row maps to logical row");
    ok &= check(coordinate.logical_row == 6, "top viewport row logical index");

    coordinate  = viewport.viewport_row_to_logical_row(2);
    ok         &= check(coordinate.code == term::Terminal_viewport_coordinate_result_code::OK,
        "bottom viewport row maps to logical row");
    ok         &= check(coordinate.logical_row == 8, "bottom viewport row logical index");

    coordinate = viewport.viewport_row_to_logical_row(3);
    ok &= check(coordinate.code ==
        term::Terminal_viewport_coordinate_result_code::INVALID_VIEWPORT_ROW,
        "row outside viewport is rejected");

    std::vector<QString> logical_rows = {
        QStringLiteral("row0"),
        QStringLiteral("row1"),
        QStringLiteral("row2"),
        QStringLiteral("row3"),
        QStringLiteral("scrollback-a"),
        QStringLiteral("scrollback-b"),
        QStringLiteral("scrollback-c"),
        QStringLiteral("scrollback-d"),
        QStringLiteral("visible-a"),
        QStringLiteral("visible-b"),
        QStringLiteral("visible-c"),
    };

    term::Selection_contract_controller selection;
    selection.begin({viewport.viewport_row_to_logical_row(0).logical_row, 0});
    selection.extend({viewport.viewport_row_to_logical_row(2).logical_row, 9});

    const term::Terminal_selection_result selected =
        selection.selected_text(std::span<const QString>(logical_rows));
    ok &= check(selected.code == term::Terminal_selection_result_code::OK,
        "selection extracts viewport-mapped logical rows");
    ok &= check(selected.text == QStringLiteral("scrollback-c\nscrollback-d\nvisible-a"),
        "selection text spans scrollback and visible rows");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_follow_tail_and_detached_output();
    ok &= test_page_scroll_and_bounds();
    ok &= test_alternate_screen_policy_and_restore();
    ok &= test_coordinate_mapping_and_selection_rows();
    return ok ? 0 : 1;
}
