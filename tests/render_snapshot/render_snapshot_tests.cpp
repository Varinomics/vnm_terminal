#include "helpers/test_check.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "vnm_terminal/internal/terminal_session.h"

#include <QByteArray>
#include <QSizeF>
#include <QString>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

term::Terminal_screen_model make_model(term::terminal_grid_size_t grid_size)
{
    return term::Terminal_screen_model({grid_size, 16, 8});
}

term::Terminal_viewport_state tail_viewport(
    const term::Terminal_screen_model& model,
    int                                offset_from_tail = 0)
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
    return viewport;
}

term::Terminal_render_snapshot_request request_for_model(
    const term::Terminal_screen_model& model,
    std::uint64_t                      sequence,
    int                                offset_from_tail = 0)
{
    term::Terminal_render_snapshot_request request;
    request.sequence = sequence;
    request.viewport = tail_viewport(model, offset_from_tail);
    return request;
}

QString row_text(const term::Terminal_render_snapshot& snapshot, int row)
{
    QString text;
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row == row && !cell.wide_continuation) {
            text += cell.text;
        }
    }
    return text;
}

const term::Terminal_render_cell* cell_with_text(
    const term::Terminal_render_snapshot&  snapshot,
    QString                                text)
{
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.text == text) {
            return &cell;
        }
    }

    return nullptr;
}

const term::Terminal_render_hyperlink_metadata* hyperlink_by_id(
    const term::Terminal_render_snapshot&  snapshot,
    std::uint64_t                          hyperlink_id)
{
    for (const term::Terminal_render_hyperlink_metadata& hyperlink : snapshot.hyperlinks) {
        if (hyperlink.hyperlink_id == hyperlink_id) {
            return &hyperlink;
        }
    }

    return nullptr;
}

term::Terminal_launch_config launch_config(term::terminal_grid_size_t grid_size)
{
    term::Terminal_launch_config config;
    config.argv              = {QStringLiteral("snapshot-test-backend")};
    config.working_directory = QStringLiteral("C:/workspace");
    config.initial_grid_size = grid_size;
    return config;
}

class Recording_backend final : public term::Terminal_backend
{
public:
    term::Terminal_backend_result start(
        const term::Terminal_launch_config&    config,
        term::Terminal_backend_callbacks       callbacks) override
    {
        const term::Terminal_backend_result callback_result =
            term::validate_backend_callbacks(callbacks);
        if (term::is_backend_rejection(callback_result)) {
            return callback_result;
        }

        const term::Terminal_backend_result config_result =
            term::validate_launch_config(config);
        if (term::is_backend_rejection(config_result)) {
            return config_result;
        }

        backend_callbacks = std::move(callbacks);
        running = true;
        return term::backend_accept();
    }

    term::Terminal_backend_result write(QByteArray bytes) override
    {
        writes.push_back(std::move(bytes));
        return term::backend_accept();
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request request) override
    {
        resize_requests.push_back(request);
        if (reject_next_resize) {
            reject_next_resize = false;
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::RESIZE_FAILED,
                    QStringLiteral("recording backend rejected resize"));
        }

        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool paused) override
    {
        pause_requests.push_back(paused);
        return term::backend_accept();
    }

    term::Terminal_backend_result interrupt() override
    {
        running = false;
        return term::backend_accept();
    }

    term::Terminal_backend_result terminate() override
    {
        running = false;
        return term::backend_accept();
    }

    bool emit_output(QByteArray bytes)
    {
        if (!backend_callbacks.output_received) {
            return false;
        }

        backend_callbacks.output_received(std::move(bytes));
        return true;
    }

    bool                       running            = false;
    bool                       reject_next_resize = false;
    term::Terminal_backend_callbacks
                               backend_callbacks;
    std::vector<term::Terminal_backend_resize_request>
                               resize_requests;
    std::vector<QByteArray>    writes;
    std::vector<bool>          pause_requests;
};

std::unique_ptr<term::Terminal_session> make_session(Recording_backend*& backend)
{
    auto owned_backend = std::make_unique<Recording_backend>();
    backend = owned_backend.get();

    term::Terminal_session_config config;
    config.trace_notification_limit = 64U;
    config.trace_resize_limit       = 64U;

    return std::make_unique<term::Terminal_session>(
        std::move(owned_backend),
        config);
}

bool test_owned_styled_wide_hyperlink_scrollback_snapshot()
{
    bool ok = true;
    term::Terminal_screen_model model = make_model({2, 16});

    QByteArray payload =
        QByteArrayLiteral("\x1b]8;id=main;https://example.test\x1b\\")
        + QByteArrayLiteral("\x1b[31mLINK")
        + QStringLiteral("\u754c").toUtf8()
        + QByteArrayLiteral("\x1b[0m\x1b]8;;\x1b\\\r\nNEXT\r\nTAIL");
    model.ingest(payload);

    term::Terminal_render_snapshot_request request = request_for_model(model, 10U, 1);
    request.viewport_changed = true;
    const term::Terminal_render_snapshot snapshot = model.render_snapshot(request);

    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled scrollback snapshot validates");
    ok &= check(row_text(snapshot, 0).contains(QStringLiteral("LINK")),
        "viewport snapshot includes scrollback row text");

    const term::Terminal_render_cell* link_cell =
        cell_with_text(snapshot, QStringLiteral("L"));
    ok &= check(link_cell != nullptr && link_cell->hyperlink_id != 0U,
        "scrollback cell retains hyperlink id");
    if (link_cell != nullptr) {
        const term::Terminal_render_hyperlink_metadata* hyperlink =
            hyperlink_by_id(snapshot, link_cell->hyperlink_id);
        ok &= check(hyperlink != nullptr &&
            hyperlink->uri == QByteArrayLiteral("https://example.test"),
            "snapshot owns hyperlink metadata");
        ok &= check(snapshot.styles[link_cell->style_id].foreground.kind ==
            term::Terminal_color_ref_kind::PALETTE_INDEX,
            "scrollback cell retains style id");
    }

    const term::Terminal_render_cell* wide_cell =
        cell_with_text(snapshot, QStringLiteral("\u754c"));
    ok &= check(wide_cell != nullptr && wide_cell->display_width == 2,
        "wide cell is copied into snapshot");

    model.ingest(QByteArrayLiteral("\x1b[2Jmutated"));
    ok &= check(row_text(snapshot, 0).contains(QStringLiteral("LINK")),
        "old snapshot remains immutable after model mutation");

    term::Terminal_render_snapshot_request retained_request =
        request_for_model(model, 11U, model.scrollback_size());
    const term::Terminal_render_snapshot retained_snapshot =
        model.render_snapshot(retained_request);
    const term::Terminal_render_cell* retained_link_cell =
        cell_with_text(retained_snapshot, QStringLiteral("L"));
    ok &= check(retained_link_cell != nullptr &&
        hyperlink_by_id(retained_snapshot, retained_link_cell->hyperlink_id) != nullptr,
        "scrollback hyperlink metadata resolves after active model mutation");
    return ok;
}

bool test_scrollback_wide_rows_are_repaired_on_resize()
{
    bool ok = true;
    term::Terminal_screen_model model = make_model({2, 6});
    model.ingest(
        QByteArrayLiteral("ABCD") +
        QStringLiteral("\u754c").toUtf8() +
        QByteArrayLiteral("\r\nNEXT\r\nTAIL"));

    ok &= check(model.scrollback_size() > 0, "wide-cell row reached scrollback");
    model.resize({2, 5});

    const term::Terminal_render_snapshot snapshot =
        model.render_snapshot(request_for_model(model, 40U, model.scrollback_size()));
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "scrollback rows are repaired after narrowing resize");
    return ok;
}

bool test_alternate_screen_hides_primary_scrollback()
{
    bool ok = true;
    term::Terminal_screen_model model = make_model({2, 10});
    model.ingest(QByteArrayLiteral("PRIMARY\r\nSCROLL\r\nTAIL"));
    ok &= check(model.scrollback_size() > 0, "primary scrollback exists before alternate");

    model.ingest(QByteArrayLiteral("\x1b[?1049hALT"));
    const term::Terminal_render_snapshot snapshot =
        model.render_snapshot(request_for_model(model, 20U));

    ok &= check(snapshot.viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "alternate snapshot reports alternate buffer");
    ok &= check(snapshot.viewport.scrollback_rows == 0, "alternate snapshot has no scrollback");
    ok &= check(!row_text(snapshot, 0).contains(QStringLiteral("PRIMARY")) &&
        !row_text(snapshot, 1).contains(QStringLiteral("PRIMARY")),
        "alternate snapshot does not expose primary scrollback text");
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "alternate snapshot validates");
    return ok;
}

bool test_request_metadata_damage_selection_and_ime()
{
    bool ok = true;
    term::Terminal_screen_model model = make_model({6, 12});
    model.ingest(QByteArrayLiteral("abcdef"));

    term::Terminal_render_snapshot_request request = request_for_model(model, 30U);
    request.dirty_rows                  = {5, 1, 2, 2, 3};
    request.cursor_shape                = term::Terminal_cursor_shape::BAR;
    request.cursor_blink_enabled        = false;
    request.backend_geometry_in_sync    = false;
    request.visual_bell_active          = true;
    request.ime_preedit.text            = QStringLiteral("ime");
    request.ime_preedit.cursor_position = 2;
    request.ime_preedit.active          = true;
    request.selections.push_back({{0, 1}, {1, 4}, term::Terminal_selection_mode::NORMAL});

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(request);
    ok &= check(snapshot.dirty_row_ranges.size() == 2U &&
        snapshot.dirty_row_ranges[0].first_row == 1 &&
        snapshot.dirty_row_ranges[0].row_count == 3 &&
        snapshot.dirty_row_ranges[1].first_row == 5 &&
        snapshot.dirty_row_ranges[1].row_count == 1,
        "dirty rows are compacted into contiguous ranges");
    ok &= check(snapshot.cursor.shape == term::Terminal_cursor_shape::BAR &&
        !snapshot.cursor.blink_enabled,
        "cursor shape and blink metadata are copied");
    ok &= check(snapshot.ime_preedit.text == QStringLiteral("ime") &&
        snapshot.ime_preedit.active,
        "IME preedit is copied");
    ok &= check(!snapshot.metadata.backend_geometry_in_sync &&
        snapshot.metadata.visual_bell_active,
        "backend sync and visual bell metadata are copied");
    ok &= check(snapshot.selection_spans.size() == 2U &&
        snapshot.selection_spans[0].row == 0 &&
        snapshot.selection_spans[1].row == 1,
        "selection range is converted to grid-relative spans");

    request.viewport_changed = true;
    const term::Terminal_render_snapshot full_repaint = model.render_snapshot(request);
    ok &= check(full_repaint.dirty_row_ranges.size() == 1U &&
        full_repaint.dirty_row_ranges.front().first_row == 0 &&
        full_repaint.dirty_row_ranges.front().row_count == 6,
        "viewport change publishes full visible-row damage");
    ok &= check(term::validate_render_snapshot(full_repaint).status ==
        term::Terminal_render_snapshot_status::OK,
        "context-rich snapshot validates");
    return ok;
}

bool test_dirty_rows_are_viewport_relative()
{
    bool ok = true;
    term::Terminal_screen_model model = make_model({3, 8});
    model.ingest(QByteArrayLiteral("one\r\ntwo\r\nthree\r\nfour"));

    ok &= check(model.scrollback_size() > 0, "dirty-row test has scrollback");
    term::Terminal_render_snapshot_request request = request_for_model(model, 50U, 1);
    request.dirty_rows = {0, 2};
    const term::Terminal_render_snapshot snapshot = model.render_snapshot(request);

    ok &= check(snapshot.dirty_row_ranges.size() == 1U &&
        snapshot.dirty_row_ranges.front().first_row == 1 &&
        snapshot.dirty_row_ranges.front().row_count == 1,
        "screen dirty row is translated to viewport-relative damage");
    return ok;
}

bool test_session_snapshot_handles_and_synchronized_release()
{
    bool                                    ok      = true;
    Recording_backend*                      backend = nullptr;
    std::unique_ptr<term::Terminal_session> session = make_session(backend);

    ok &= check(session->start(launch_config({3, 12})).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "session starts");
    ok &= check(backend != nullptr &&
        backend->emit_output(QByteArrayLiteral("one")),
        "backend emits first output");

    const std::shared_ptr<const term::Terminal_render_snapshot> first =
        session->latest_render_snapshot_handle();
    ok &= check(first != nullptr && row_text(*first, 0).contains(QStringLiteral("one")),
        "session publishes immutable snapshot handle");
    const std::uint64_t first_generation = session->render_snapshot_generation();

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026hheld")),
        "backend emits synchronized-output hold");
    ok &= check(session->render_snapshot_generation() == first_generation,
        "held synchronized output does not publish a snapshot");
    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "backend releases synchronized output");
    ok &= check(session->render_snapshot_generation() == first_generation + 1U,
        "synchronized release publishes one coalesced snapshot");
    ok &= check(row_text(*first, 0).contains(QStringLiteral("one")),
        "old session snapshot handle remains immutable");

    const std::shared_ptr<const term::Terminal_render_snapshot> released =
        session->latest_render_snapshot_handle();
    ok &= check(released != nullptr &&
        (row_text(*released, 0) + row_text(*released, 1)).contains(QStringLiteral("held")),
        "released snapshot contains held text");

    ok &= check(backend->emit_output(QByteArrayLiteral("\a")),
        "backend emits bell");
    const std::shared_ptr<const term::Terminal_render_snapshot> bell =
        session->latest_render_snapshot_handle();
    ok &= check(bell != nullptr && bell->metadata.visual_bell_active,
        "bell publishes visual bell metadata without text mutation");
    ok &= check(bell != nullptr && bell->metadata.backend_geometry_in_sync,
        "snapshot carries backend geometry sync metadata");
    return ok;
}

bool test_session_ime_overlay_does_not_clone_render_snapshot()
{
    bool                                    ok      = true;
    Recording_backend*                      backend = nullptr;
    std::unique_ptr<term::Terminal_session> session = make_session(backend);

    ok &= check(session->start(launch_config({3, 12})).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "IME overlay session starts");
    ok &= check(backend != nullptr &&
        backend->emit_output(QByteArrayLiteral("base")),
        "IME overlay session publishes initial model snapshot");

    const std::shared_ptr<const term::Terminal_render_snapshot> base_snapshot =
        session->latest_render_snapshot_handle();
    const std::uint64_t base_snapshot_generation = session->render_snapshot_generation();
    const std::uint64_t base_ime_generation      = session->ime_preedit_generation();
    (void)session->take_pending_notifications();

    session->set_ime_preedit(QStringLiteral("ime"), 2);
    term::Ime_preedit_state ime_preedit = session->ime_preedit_state();
    ok &= check(ime_preedit.active &&
        ime_preedit.text == QStringLiteral("ime") &&
        ime_preedit.cursor_position == 2,
        "IME overlay state is queryable from session");
    ok &= check(session->ime_preedit_generation() == base_ime_generation + 1U,
        "IME preedit update advances only IME generation");
    ok &= check(session->render_snapshot_generation() == base_snapshot_generation,
        "IME preedit update does not advance render snapshot generation");
    ok &= check(session->latest_render_snapshot_handle() == base_snapshot,
        "IME preedit update does not replace latest render snapshot handle");

    ok &= check(session->take_pending_notifications().empty(),
        "IME preedit update does not enqueue public session notifications");

    session->cancel_ime_preedit();
    ime_preedit  = session->ime_preedit_state();
    ok          &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "IME cancel clears session overlay state");
    ok          &= check(session->ime_preedit_generation() == base_ime_generation + 2U,
        "IME cancel advances IME generation");
    ok          &= check(session->render_snapshot_generation() == base_snapshot_generation,
        "IME cancel does not advance render snapshot generation");
    ok          &= check(session->latest_render_snapshot_handle() == base_snapshot,
        "IME cancel does not replace latest render snapshot handle");

    return ok;
}

bool test_backend_sync_metadata_publishes_after_same_grid_retry()
{
    bool                                    ok      = true;
    Recording_backend*                      backend = nullptr;
    std::unique_ptr<term::Terminal_session> session = make_session(backend);

    ok &= check(session->start(launch_config({3, 12})).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "retry session starts");
    ok &= check(backend != nullptr &&
        backend->emit_output(QByteArrayLiteral("ready")),
        "retry session publishes an initial snapshot");

    const std::uint64_t initial_generation = session->render_snapshot_generation();
    backend->reject_next_resize = true;
    ok &= check(session->resize(QSizeF(120.0, 80.0), {4, 12}).code ==
        term::Terminal_session_result_code::BACKEND_REJECTED,
        "first resize is rejected by backend");

    const std::shared_ptr<const term::Terminal_render_snapshot> failed =
        session->latest_render_snapshot_handle();
    ok &= check(failed != nullptr && !failed->metadata.backend_geometry_in_sync,
        "failed resize publishes out-of-sync metadata");
    const std::uint64_t failed_generation = session->render_snapshot_generation();
    ok &= check(failed_generation > initial_generation,
        "failed resize advances snapshot generation");

    ok &= check(session->resize(QSizeF(120.0, 80.0), {4, 12}).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "same-grid resize retry succeeds");
    const std::shared_ptr<const term::Terminal_render_snapshot> recovered =
        session->latest_render_snapshot_handle();
    ok &= check(recovered != nullptr && recovered->metadata.backend_geometry_in_sync,
        "same-grid retry publishes restored sync metadata");
    ok &= check(session->render_snapshot_generation() == failed_generation + 1U,
        "same-grid retry advances snapshot generation");
    return ok;
}

bool test_resize_metadata_publication_respects_synchronized_output()
{
    bool                                    ok      = true;
    Recording_backend*                      backend = nullptr;
    std::unique_ptr<term::Terminal_session> session = make_session(backend);

    ok &= check(session->start(launch_config({3, 12})).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "synchronized resize session starts");
    ok &= check(backend != nullptr &&
        backend->emit_output(QByteArrayLiteral("base")),
        "synchronized resize session publishes an initial snapshot");

    const std::uint64_t initial_generation = session->render_snapshot_generation();
    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026h\r\nH")),
        "synchronized resize session enters hold");
    ok &= check(session->render_snapshot_generation() == initial_generation,
        "held output does not publish before resize");

    backend->reject_next_resize = true;
    ok &= check(session->resize(QSizeF(20.0, 80.0), {4, 2}).code ==
        term::Terminal_session_result_code::BACKEND_REJECTED,
        "held resize failure is reported");
    const std::shared_ptr<const term::Terminal_render_snapshot> failed =
        session->latest_render_snapshot_handle();
    ok &= check(session->render_snapshot_generation() == initial_generation + 1U,
        "held resize failure publishes a geometry-only snapshot");
    ok &= check(failed != nullptr &&
        failed->grid_size.rows == 4 &&
        failed->grid_size.columns == 2 &&
        !failed->metadata.backend_geometry_in_sync &&
        row_text(*failed, 0) == QStringLiteral("ba") &&
        cell_with_text(*failed, QStringLiteral("H")) == nullptr,
        "held resize failure publishes clipped public geometry without hidden content");
    ok &= check(failed != nullptr &&
        term::validate_render_snapshot(*failed).status ==
            term::Terminal_render_snapshot_status::OK,
        "held resize failure geometry snapshot validates");

    ok &= check(session->resize(QSizeF(120.0, 80.0), {4, 12}).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "held grow resize recovery succeeds");
    const std::shared_ptr<const term::Terminal_render_snapshot> recovered =
        session->latest_render_snapshot_handle();
    ok &= check(session->render_snapshot_generation() == initial_generation + 2U,
        "held resize recovery publishes a geometry-only snapshot");
    ok &= check(recovered != nullptr &&
        recovered->grid_size.rows == 4 &&
        recovered->grid_size.columns == 12 &&
        recovered->metadata.backend_geometry_in_sync &&
        row_text(*recovered, 0) == QStringLiteral("base") &&
        cell_with_text(*recovered, QStringLiteral("H")) == nullptr,
        "held resize recovery restores public baseline without hidden content");
    ok &= check(recovered != nullptr &&
        term::validate_render_snapshot(*recovered).status ==
            term::Terminal_render_snapshot_status::OK,
        "held resize recovery geometry snapshot validates");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "synchronized resize session releases hold");
    const std::shared_ptr<const term::Terminal_render_snapshot> released =
        session->latest_render_snapshot_handle();
    ok &= check(session->render_snapshot_generation() == initial_generation + 3U,
        "release publishes accumulated content after held resize geometry snapshots");
    ok &= check(released != nullptr && released->metadata.backend_geometry_in_sync,
        "released coalesced snapshot carries current backend sync metadata");
    ok &= check(released != nullptr &&
        (row_text(*released, 0) + row_text(*released, 1)).contains(QStringLiteral("H")),
        "released coalesced snapshot includes held output");
    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_owned_styled_wide_hyperlink_scrollback_snapshot();
    ok &= test_scrollback_wide_rows_are_repaired_on_resize();
    ok &= test_alternate_screen_hides_primary_scrollback();
    ok &= test_request_metadata_damage_selection_and_ime();
    ok &= test_dirty_rows_are_viewport_relative();
    ok &= test_session_snapshot_handles_and_synchronized_release();
    ok &= test_session_ime_overlay_does_not_clone_render_snapshot();
    ok &= test_backend_sync_metadata_publishes_after_same_grid_retry();
    ok &= test_resize_metadata_publication_respects_synchronized_output();
    return ok ? 0 : 1;
}
