#include "vnm_terminal/internal/terminal_session.h"

#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/terminal_input_encoder.h"
#include <QFile>
#include <QKeyEvent>
#include <QString>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>
#include <variant>

namespace vnm_terminal::internal {

namespace {

constexpr std::size_t k_pending_notification_limit = 4096U;
constexpr QByteArrayView k_focus_in_report("\x1b[I", 3);
constexpr QByteArrayView k_focus_out_report("\x1b[O", 3);

Terminal_backend_error make_backend_error(
    Terminal_backend_error_code    code,
    QString                        message)
{
    return {code, std::move(message)};
}

bool model_result_warrants_render_snapshot(const Terminal_screen_model_result& result)
{
    return
        !result.dirty_rows.empty() ||
        result.viewport_changed    ||
        result.mode_state_changed  ||
        result.mouse_reporting_mode_changed;
}

bool model_allows_render_snapshot(const Terminal_screen_model& model)
{
    return !model.mode_state().synchronized_output;
}

bool grid_sizes_match(terminal_grid_size_t left, terminal_grid_size_t right)
{
    return left.rows == right.rows && left.columns == right.columns;
}

bool snapshots_share_row_identity_space(
    const Terminal_render_snapshot&    left,
    const Terminal_render_snapshot&    right)
{
    return
        grid_sizes_match(left.grid_size, right.grid_size)                &&
        left.viewport.active_buffer    == right.viewport.active_buffer   &&
        left.viewport.visible_rows     == right.viewport.visible_rows    &&
        left.viewport.scrollback_rows  == right.viewport.scrollback_rows &&
        left.viewport.offset_from_tail == right.viewport.offset_from_tail;
}

void append_dirty_range_rows(
    std::vector<int>&      rows,
    const std::vector<Terminal_render_dirty_row_range>&
                           ranges)
{
    for (const Terminal_render_dirty_row_range& range : ranges) {
        for (int row = range.first_row; row < range.first_row + range.row_count; ++row) {
            rows.push_back(row);
        }
    }
}

Terminal_render_snapshot snapshot_with_coalesced_dirty_rows(
    const Terminal_render_snapshot&        previous_snapshot,
    Terminal_render_snapshot               snapshot)
{
    if (!snapshots_share_row_identity_space(previous_snapshot, snapshot)) {
        snapshot.dirty_row_ranges =
            compact_dirty_row_ranges({}, snapshot.grid_size.rows, true);
        return snapshot;
    }

    std::vector<int> dirty_rows;
    append_dirty_range_rows(dirty_rows, previous_snapshot.dirty_row_ranges);
    append_dirty_range_rows(dirty_rows, snapshot.dirty_row_ranges);
    snapshot.dirty_row_ranges =
        compact_dirty_row_ranges(std::move(dirty_rows), snapshot.grid_size.rows, false);
    return snapshot;
}

bool model_should_publish_render_snapshot(
    const Terminal_screen_model&           model,
    const Terminal_screen_model_result&    result)
{
    return
        model_result_warrants_render_snapshot(result) &&
        model_allows_render_snapshot(model);
}

int bounded_string_size(const QString& text)
{
    return static_cast<int>(
        std::min<qsizetype>(text.size(), std::numeric_limits<int>::max()));
}

bool cell_position_fits_grid(
    const Terminal_render_cell&    cell,
    terminal_grid_size_t           grid_size)
{
    return
        cell.position.row    >= 0              &&
        cell.position.row    <  grid_size.rows &&
        cell.position.column >= 0              &&
        cell.position.column <  grid_size.columns;
}

std::size_t cell_position_index(
    terminal_grid_position_t       position,
    terminal_grid_size_t           grid_size)
{
    return
        static_cast<std::size_t>(position.row) * static_cast<std::size_t>(grid_size.columns) +
        static_cast<std::size_t>(position.column);
}

bool continuation_matches_base(
    const Terminal_render_cell&    continuation,
    const Terminal_render_cell&    base)
{
    return
        continuation.wide_continuation         &&
        continuation.style_id == base.style_id &&
        continuation.hyperlink_id == base.hyperlink_id;
}

std::vector<Terminal_render_cell> cells_adapted_to_grid(
    const std::vector<Terminal_render_cell>&   cells,
    terminal_grid_size_t                       grid_size)
{
    const std::size_t cell_count =
        static_cast<std::size_t>(grid_size.rows) *
        static_cast<std::size_t>(grid_size.columns);
    std::vector<const Terminal_render_cell*> cells_by_position(cell_count, nullptr);
    for (const Terminal_render_cell& cell : cells) {
        if (cell_position_fits_grid(cell, grid_size)) {
            cells_by_position[cell_position_index(cell.position, grid_size)] = &cell;
        }
    }

    std::vector<Terminal_render_cell> adapted_cells;
    adapted_cells.reserve(cells.size());
    for (const Terminal_render_cell& cell : cells) {
        if (!cell_position_fits_grid(cell, grid_size) || cell.wide_continuation) {
            continue;
        }

        if (cell.display_width <= 0 ||
            cell.display_width >  grid_size.columns - cell.position.column)
        {
            continue;
        }

        bool complete_cell_span = true;
        for (int column_delta = 1; column_delta < cell.display_width; ++column_delta) {
            const terminal_grid_position_t continuation_position = {
                cell.position.row,
                cell.position.column + column_delta,
            };
            const Terminal_render_cell* continuation =
                cells_by_position[cell_position_index(continuation_position, grid_size)];
            if (continuation == nullptr ||
                !continuation_matches_base(*continuation, cell))
            {
                complete_cell_span = false;
                break;
            }
        }

        if (!complete_cell_span) {
            continue;
        }

        adapted_cells.push_back(cell);
        for (int column_delta = 1; column_delta < cell.display_width; ++column_delta) {
            const terminal_grid_position_t continuation_position = {
                cell.position.row,
                cell.position.column + column_delta,
            };
            adapted_cells.push_back(
                *cells_by_position[cell_position_index(continuation_position, grid_size)]);
        }
    }

    return adapted_cells;
}

bool snapshot_cells_reference_hyperlink(
    const std::vector<Terminal_render_cell>&           cells,
    std::uint64_t                                      hyperlink_id)
{
    return std::any_of(
        cells.begin(),
        cells.end(),
        [hyperlink_id](const Terminal_render_cell& cell) {
            return cell.hyperlink_id == hyperlink_id;
        });
}

std::vector<Terminal_render_hyperlink_metadata> hyperlinks_referenced_by_cells(
    const std::vector<Terminal_render_hyperlink_metadata>& hyperlinks,
    const std::vector<Terminal_render_cell>&               cells)
{
    std::vector<Terminal_render_hyperlink_metadata> referenced_hyperlinks;
    referenced_hyperlinks.reserve(hyperlinks.size());
    for (const Terminal_render_hyperlink_metadata& hyperlink : hyperlinks) {
        if (snapshot_cells_reference_hyperlink(cells, hyperlink.hyperlink_id)) {
            referenced_hyperlinks.push_back(hyperlink);
        }
    }

    return referenced_hyperlinks;
}

std::vector<Terminal_render_selection_span> selection_spans_adapted_to_grid(
    const std::vector<Terminal_render_selection_span>& spans,
    terminal_grid_size_t                               grid_size)
{
    std::vector<Terminal_render_selection_span> adapted_spans;
    adapted_spans.reserve(spans.size());
    for (Terminal_render_selection_span span : spans) {
        if (span.row          < 0  || span.row          >= grid_size.rows ||
            span.first_column < 0  || span.first_column >= grid_size.columns)
        {
            continue;
        }

        span.column_count =
            std::min(span.column_count, grid_size.columns - span.first_column);
        if (span.column_count > 0) {
            adapted_spans.push_back(span);
        }
    }

    return adapted_spans;
}

Terminal_viewport_state viewport_adapted_to_grid(
    Terminal_viewport_state            viewport,
    terminal_grid_size_t               grid_size)
{
    viewport.visible_rows = grid_size.rows;
    if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        viewport.scrollback_rows  = 0;
        viewport.offset_from_tail = 0;
        viewport.follow_tail      = true;
        return viewport;
    }

    viewport.scrollback_rows = std::max(0, viewport.scrollback_rows);
    viewport.offset_from_tail =
        std::clamp(viewport.offset_from_tail, 0, viewport.scrollback_rows);
    viewport.follow_tail = viewport.offset_from_tail == 0;
    return viewport;
}

Terminal_render_snapshot geometry_snapshot_from_public_snapshot(
    const Terminal_render_snapshot&    public_snapshot,
    terminal_grid_size_t               grid_size,
    std::uint64_t                      sequence,
    bool                               backend_geometry_in_sync)
{
    Terminal_render_snapshot snapshot = public_snapshot;
    snapshot.grid_size  = grid_size;
    snapshot.viewport   = viewport_adapted_to_grid(snapshot.viewport, grid_size);
    snapshot.cells      = cells_adapted_to_grid(snapshot.cells, grid_size);
    snapshot.hyperlinks = hyperlinks_referenced_by_cells(snapshot.hyperlinks, snapshot.cells);
    snapshot.selection_spans =
        selection_spans_adapted_to_grid(snapshot.selection_spans, grid_size);
    snapshot.dirty_row_ranges                      = compact_dirty_row_ranges({}, grid_size.rows, true);
    snapshot.metadata.sequence                     = sequence;
    snapshot.metadata.backend_geometry_in_sync     = backend_geometry_in_sync;
    snapshot.metadata.visual_bell_active           = false;
    snapshot.metadata.mouse_reporting_mode_changed = false;
    if (snapshot.cursor.visible) {
        snapshot.cursor.position.row =
            std::clamp(snapshot.cursor.position.row, 0, grid_size.rows - 1);
        snapshot.cursor.position.column =
            std::clamp(snapshot.cursor.position.column, 0, grid_size.columns - 1);
    }
    return snapshot;
}

struct Sync_parameter_location
{
    bool       found       = false;
    qsizetype  group_begin = -1;
    qsizetype  prefix_end  = -1;
};

struct Sync_set_sequence
{
    qsizetype  start           = -1;
    qsizetype  end             = -1;
    qsizetype  parameter_begin = -1;
    qsizetype  parameter_end   = -1;
    qsizetype  sync_begin      = -1;
    qsizetype  prefix_end      = -1;
};

bool is_csi_parameter_byte(unsigned char byte)
{
    return byte >= 0x30U && byte <= 0x3fU;
}

bool is_csi_intermediate_byte(unsigned char byte)
{
    return byte >= 0x20U && byte <= 0x2fU;
}

bool is_csi_final_byte(unsigned char byte)
{
    return byte >= 0x40U && byte <= 0x7eU;
}

Sync_parameter_location find_sync_parameter_location(QByteArrayView parameter_bytes)
{
    if (parameter_bytes.empty() || parameter_bytes[0] != '?') {
        return {};
    }

    bool        saw_sync_parameter = false;
    qsizetype   sync_group_begin   = -1;
    qsizetype   sync_prefix_end    = -1;
    std::size_t group_count        = 0U;
    qsizetype   offset             = 1;
    for (;;) {
        if (group_count >= k_csi_parameter_group_limit) {
            return {};
        }

        const qsizetype group_begin = offset;
        while (offset               <  parameter_bytes.size() &&
            parameter_bytes[offset] >= '0'                    &&
            parameter_bytes[offset] <= '9')
        {
            ++offset;
        }

        const qsizetype length = offset - group_begin;
        if (length                           <= 0 ||
            static_cast<std::size_t>(length) >  k_csi_parameter_digit_limit)
        {
            return {};
        }
        ++group_count;

        if (length                           == 4   &&
            parameter_bytes[group_begin]     == '2' &&
            parameter_bytes[group_begin + 1] == '0' &&
            parameter_bytes[group_begin + 2] == '2' &&
            parameter_bytes[group_begin + 3] == '6')
        {
            saw_sync_parameter = true;
            sync_group_begin   = group_begin;
            sync_prefix_end    = group_begin;
            if (sync_prefix_end > 1 && parameter_bytes[sync_prefix_end - 1] == ';') {
                --sync_prefix_end;
            }
        }

        if (offset >= parameter_bytes.size()) {
            break;
        }

        if (parameter_bytes[offset] != ';') {
            return {};
        }
        ++offset;
    }

    return {
        saw_sync_parameter,
        sync_group_begin,
        sync_prefix_end,
    };
}

Sync_set_sequence next_synchronized_output_set_sequence(
    QByteArrayView             bytes,
    Terminal_utf8_scan_state   initial_utf8_scan_state)
{
    Terminal_utf8_scan_state utf8_scan_state = initial_utf8_scan_state;
    for (qsizetype offset = 0; offset < bytes.size(); ++offset) {
        if (utf8_scan_consumes_byte(static_cast<unsigned char>(bytes[offset]), utf8_scan_state)) {
            continue;
        }

        qsizetype parameter_begin = -1;
        if (static_cast<unsigned char>(bytes[offset]) == 0x9bU) {
            parameter_begin = offset + 1;
        }
        else
        if (static_cast<unsigned char>(bytes[offset]) == 0x1bU        &&
            offset + 1                                <  bytes.size() &&
            bytes[offset + 1]                         == '[')
        {
            parameter_begin = offset + 2;
        }
        else {
            continue;
        }

        qsizetype cursor = parameter_begin;
        while (cursor < bytes.size() &&
            is_csi_parameter_byte(static_cast<unsigned char>(bytes[cursor])))
        {
            ++cursor;
        }

        const qsizetype parameter_end = cursor;
        while (cursor < bytes.size() &&
            is_csi_intermediate_byte(static_cast<unsigned char>(bytes[cursor])))
        {
            ++cursor;
        }
        const bool has_intermediates = cursor > parameter_end;

        if (cursor >= bytes.size()) {
            return {};
        }

        const unsigned char final_byte = static_cast<unsigned char>(bytes[cursor]);
        if (!is_csi_final_byte(final_byte)) {
            continue;
        }

        const QByteArrayView parameters(
            bytes.data() + parameter_begin,
            parameter_end - parameter_begin);
        const Sync_parameter_location sync_location =
            (final_byte == 'h' && !has_intermediates)
                ? find_sync_parameter_location(parameters)
                : Sync_parameter_location{};
        if (sync_location.found)
        {
            return {
                offset,
                cursor + 1,
                parameter_begin,
                parameter_end,
                parameter_begin + sync_location.group_begin,
                parameter_begin + sync_location.prefix_end,
            };
        }

        offset = cursor;
    }

    return {};
}

QByteArray csi_set_private_modes(QByteArrayView parameter_bytes)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[");
    bytes.append(parameter_bytes.data(), parameter_bytes.size());
    bytes.append('h');
    return bytes;
}

QByteArray sync_sequence_prefix(QByteArrayView bytes, const Sync_set_sequence& sequence)
{
    if (sequence.prefix_end <= sequence.parameter_begin + 1) {
        return {};
    }

    return
        csi_set_private_modes(
            QByteArrayView(bytes.data() + sequence.parameter_begin, sequence.prefix_end - sequence.parameter_begin));
}

QByteArray sync_sequence_suffix_and_tail(QByteArrayView bytes, const Sync_set_sequence& sequence)
{
    QByteArray suffix = QByteArrayLiteral("\x1b[?");
    suffix.append(
        bytes.data() + sequence.sync_begin,
        sequence.parameter_end - sequence.sync_begin);
    suffix.append('h');
    suffix.append(bytes.data() + sequence.end, bytes.size() - sequence.end);
    return suffix;
}

qsizetype trailing_incomplete_csi_start(
    QByteArrayView             bytes,
    Terminal_utf8_scan_state   initial_utf8_scan_state)
{
    Terminal_utf8_scan_state utf8_scan_state = initial_utf8_scan_state;
    for (qsizetype offset = 0; offset < bytes.size(); ++offset) {
        if (utf8_scan_consumes_byte(static_cast<unsigned char>(bytes[offset]), utf8_scan_state)) {
            continue;
        }

        qsizetype parameter_begin = -1;
        if (static_cast<unsigned char>(bytes[offset]) == 0x9bU) {
            parameter_begin = offset + 1;
        }
        else
        if (static_cast<unsigned char>(bytes[offset]) == 0x1bU) {
            if (offset + 1 >= bytes.size()) {
                return offset;
            }

            if (bytes[offset + 1] == '[') {
                parameter_begin = offset + 2;
            }
            else {
                continue;
            }
        }
        else {
            continue;
        }

        qsizetype cursor = parameter_begin;
        while (cursor < bytes.size() &&
            is_csi_parameter_byte(static_cast<unsigned char>(bytes[cursor])))
        {
            ++cursor;
        }

        while (cursor < bytes.size() &&
            is_csi_intermediate_byte(static_cast<unsigned char>(bytes[cursor])))
        {
            ++cursor;
        }

        if (cursor >= bytes.size()) {
            return offset;
        }

        if (is_csi_final_byte(static_cast<unsigned char>(bytes[cursor]))) {
            offset = cursor;
        }
    }

    return -1;
}

bool uses_deferred_backend_callbacks(const Terminal_session_config& config)
{
    return static_cast<bool>(config.backend_event_notifier);
}

class Backend_callback_invocation
{
public:
    explicit Backend_callback_invocation(
        const std::shared_ptr<Terminal_session_callback_lifetime>& lifetime);

    ~Backend_callback_invocation();

    Backend_callback_invocation(const Backend_callback_invocation&)            = delete;
    Backend_callback_invocation& operator=(const Backend_callback_invocation&) = delete;

    Terminal_session* session() const { return m_session; }
    Terminal_queue_result enqueue(Terminal_session_command command);

private:
    std::shared_ptr<Terminal_session_callback_lifetime> m_lifetime;
    Terminal_session* m_session = nullptr;
};

}

class Terminal_session_callback_lifetime
{
public:
    // Backend threads enqueue commands through this shared lifetime object.
    // close() stops new callbacks and waits for in-flight callbacks before
    // Terminal_session members can be destroyed. Callback command count and
    // output bytes are bounded here because notifier-driven owners may not
    // drain while the GUI thread is busy.
    explicit Terminal_session_callback_lifetime(
        Terminal_session*      session,
        Terminal_queue_limits  output_queue_limits,
        bool                   coalesce_output_callbacks)
    :
        m_session(session),
        m_callback_queue_limits(output_queue_limits),
        m_pending_callback_queue(output_queue_limits),
        m_coalesce_output_callbacks(coalesce_output_callbacks)
    {}

    Terminal_session* begin_callback()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_accepting_callbacks || m_session == nullptr) {
            return nullptr;
        }

        ++m_active_callbacks;
        return m_session;
    }

    void end_callback()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        --m_active_callbacks;
        if (m_active_callbacks == 0U) {
            m_idle.notify_all();
        }
    }

    Terminal_queue_result enqueue(Terminal_session_command command)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_backend_callbacks_stopped) {
            if (command.kind == Terminal_session_command_kind::BACKEND_EXIT &&
                !m_backend_exit_after_stop_pending)
            {
                m_backend_exit_after_stop_pending = true;
                m_pending_commands.push_back(std::move(command));
            }
            return {Terminal_queue_result_code::ACCEPTED, false};
        }

        const bool output_command =
            command.kind == Terminal_session_command_kind::BACKEND_OUTPUT;
        if (output_command) {
            if (m_backend_output_stopped) {
                return {Terminal_queue_result_code::ACCEPTED, false};
            }
        }

        const std::size_t byte_count =
            output_command
                ? static_cast<std::size_t>(command.bytes.size())
                : 0U;
        const bool append_to_previous_output =
            m_coalesce_output_callbacks                                                     &&
            output_command                                                                  &&
            !m_pending_commands.empty()                                                     &&
            m_pending_commands.back().kind == Terminal_session_command_kind::BACKEND_OUTPUT;
        const std::size_t command_count = append_to_previous_output ? 0U : 1U;
        const Terminal_queue_result result =
            m_pending_callback_queue.reserve(byte_count, command_count);
        if (result.code == Terminal_queue_result_code::HARD_LIMIT_REACHED) {
            drop_pending_output_locked();
            if (!m_backend_callback_overflow_report_pending) {
                m_pending_commands.push_back(make_backend_error_command(
                    0U,
                    make_backend_error(
                        Terminal_backend_error_code::OUTPUT_OVERFLOW,
                        QStringLiteral("pending backend callback hard limit reached"))));
                m_backend_callback_overflow_report_pending = true;
            }
            if (!output_command) {
                m_backend_callbacks_stopped = true;
            }
            m_backend_output_stopped = true;
            return result;
        }

        if (append_to_previous_output) {
            m_pending_commands.back().bytes += command.bytes;
            return result;
        }

        m_pending_commands.push_back(std::move(command));
        return result;
    }

    std::deque<Terminal_session_command> take_pending_commands()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::deque<Terminal_session_command> commands;
        commands.swap(m_pending_commands);
        m_pending_callback_queue = Bounded_terminal_command_queue(m_callback_queue_limits);
        m_backend_callback_overflow_report_pending = false;
        return commands;
    }

    bool high_water_reached()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        return m_pending_callback_queue.high_water_reached();
    }

    void stop_backend_output()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_backend_output_stopped = true;

        drop_pending_output_locked();
    }

    void close()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_accepting_callbacks = false;
        m_session = nullptr;
        m_pending_commands.clear();
        m_idle.wait(lock, [this] { return m_active_callbacks == 0U; });
        m_pending_commands.clear();
    }

private:
    void drop_pending_output_locked()
    {
        std::deque<Terminal_session_command> retained_commands;
        while (!m_pending_commands.empty()) {
            Terminal_session_command command = std::move(m_pending_commands.front());
            m_pending_commands.pop_front();
            if (command.kind != Terminal_session_command_kind::BACKEND_OUTPUT) {
                retained_commands.push_back(std::move(command));
            }
        }
        m_pending_commands.swap(retained_commands);
        m_pending_callback_queue = Bounded_terminal_command_queue(m_callback_queue_limits);
        for (const Terminal_session_command& command : m_pending_commands) {
            const std::size_t byte_count =
                command.kind == Terminal_session_command_kind::BACKEND_OUTPUT
                    ? static_cast<std::size_t>(command.bytes.size())
                    : 0U;
            (void)m_pending_callback_queue.reserve(byte_count);
        }
    }

    std::mutex                           m_mutex;
    std::condition_variable              m_idle;
    Terminal_session*                    m_session = nullptr;
    std::deque<Terminal_session_command> m_pending_commands;
    Terminal_queue_limits                m_callback_queue_limits;
    Bounded_terminal_command_queue       m_pending_callback_queue;
    std::size_t                          m_active_callbacks = 0U;
    bool                                 m_accepting_callbacks = true;
    bool                                 m_backend_callbacks_stopped = false;
    bool                                 m_backend_exit_after_stop_pending = false;
    bool                                 m_backend_output_stopped = false;
    bool                                 m_backend_callback_overflow_report_pending = false;
    bool                                 m_coalesce_output_callbacks = false;
};

Backend_callback_invocation::Backend_callback_invocation(
    const std::shared_ptr<Terminal_session_callback_lifetime>& lifetime)
:
    m_lifetime(lifetime)
{
    if (m_lifetime != nullptr) {
        m_session = m_lifetime->begin_callback();
    }
}

Backend_callback_invocation::~Backend_callback_invocation()
{
    if (m_session != nullptr) {
        m_lifetime->end_callback();
    }
}

Terminal_queue_result Backend_callback_invocation::enqueue(Terminal_session_command command)
{
    if (m_session != nullptr) {
        return m_lifetime->enqueue(std::move(command));
    }

    return {Terminal_queue_result_code::ACCEPTED, false};
}

Terminal_session::Terminal_session(
    std::unique_ptr<Terminal_backend>  backend,
    Terminal_session_config            config)
:
    m_callback_lifetime(
        std::make_shared<Terminal_session_callback_lifetime>(
            this,
            config.output_queue_limits,
            uses_deferred_backend_callbacks(config))),
    m_backend(std::move(backend)),
    m_config(config),
    m_output_queue(m_config.output_queue_limits),
    m_write_queue(m_config.write_queue_limits)
{
    m_config.scrollback_limit = std::max(0, m_config.scrollback_limit);
    m_bell_state.policy = m_config.bell_policy;
}

Terminal_session::~Terminal_session()
{
    m_callback_lifetime->close();
    m_backend.reset();
}

Terminal_session_result Terminal_session::start(Terminal_launch_config launch_config)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();

    const std::uint64_t sequence = next_sequence();
    return enqueue_and_process_synchronous_command(
        make_start_command(sequence, std::move(launch_config)));
}

Terminal_session_result Terminal_session::write_user_bytes(QByteArray bytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();

    return write_user_bytes_locked(
        std::move(bytes),
        User_write_viewport_policy::RETURN_TO_TAIL);
}

Terminal_session_result Terminal_session::write_user_bytes_locked(
    QByteArray                 bytes,
    User_write_viewport_policy viewport_policy)
{
    const std::uint64_t sequence = next_sequence();
    if (!is_session_writable()) {
        return make_rejected_result(
            sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("session write requires a running backend")));
    }

    const Terminal_session_result result = enqueue_and_process_synchronous_command(
        make_user_write_command(sequence, std::move(bytes)));
    if (result.code     == Terminal_session_result_code::ACCEPTED &&
        viewport_policy == User_write_viewport_policy::RETURN_TO_TAIL)
    {
        return_viewport_to_tail_after_user_input(sequence);
    }
    return result;
}

Terminal_key_event_result Terminal_session::write_key_event(const QKeyEvent& event)
{
    if (!terminal_key_event_may_encode(event)) {
        return {};
    }

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    const Terminal_input_mode_state modes = m_screen_model.has_value()
        ? m_screen_model->input_mode_state()
        : Terminal_input_mode_state{};
    QByteArray bytes = encode_terminal_key_event(event, modes);
    if (bytes.isEmpty()) {
        return {};
    }

    if (m_process_state == Terminal_process_state::NOT_STARTED ||
        m_process_state == Terminal_process_state::STARTING)
    {
        return {};
    }

    const std::uint64_t sequence = next_sequence();
    if (!is_session_writable()) {
        return {
            true,
            make_rejected_result(
                sequence,
                Terminal_session_result_code::INVALID_STATE,
                make_backend_error(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("session write requires a running backend"))),
        };
    }

    const Terminal_session_result result = enqueue_and_process_synchronous_command(
        make_user_write_command(sequence, std::move(bytes)));
    if (result.code == Terminal_session_result_code::ACCEPTED) {
        return_viewport_to_tail_after_user_input(sequence);
    }
    return {true, result};
}

Terminal_mouse_event_result Terminal_session::write_mouse_event(Terminal_mouse_event event)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    const Terminal_input_mode_state modes = m_screen_model.has_value()
        ? m_screen_model->input_mode_state()
        : Terminal_input_mode_state{};
    QByteArray bytes = encode_terminal_mouse_event(event, modes);
    if (bytes.isEmpty()) {
        return {};
    }

    if (!is_session_writable()) {
        return {};
    }

    const User_write_viewport_policy viewport_policy =
        event.kind == Terminal_mouse_event_kind::MOVE
            ? User_write_viewport_policy::PRESERVE_VIEWPORT
            : User_write_viewport_policy::RETURN_TO_TAIL;
    return {
        true,
        write_user_bytes_locked(
            std::move(bytes),
            viewport_policy),
    };
}

Terminal_ime_commit_result Terminal_session::write_ime_commit(QString text)
{
    if (text.isEmpty()) {
        return {};
    }

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    QByteArray bytes = text.toUtf8();
    if (m_process_state == Terminal_process_state::NOT_STARTED ||
        m_process_state == Terminal_process_state::STARTING)
    {
        return {};
    }

    const std::uint64_t sequence = next_sequence();
    if (!is_session_writable()) {
        return {
            true,
            make_rejected_result(
                sequence,
                Terminal_session_result_code::INVALID_STATE,
                make_backend_error(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("session write requires a running backend"))),
        };
    }

    const Terminal_session_result result = enqueue_and_process_synchronous_command(
        make_user_write_command(sequence, std::move(bytes)));

    if (result.code == Terminal_session_result_code::ACCEPTED) {
        return_viewport_to_tail_after_user_input(sequence);
    }

    if (result.code == Terminal_session_result_code::ACCEPTED && ime_preedit_has_content(m_ime_preedit)) {
        m_ime_preedit = {};
        advance_ime_preedit_generation();
    }

    return {true, result};
}

Terminal_paste_text_result Terminal_session::write_paste_text(
    QString                        text,
    Terminal_paste_framing_policy  policy)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    const Terminal_input_mode_state modes = m_screen_model.has_value()
        ? m_screen_model->input_mode_state()
        : Terminal_input_mode_state{};
    QByteArray bytes = encode_terminal_paste_text(std::move(text), modes, policy);
    if (bytes.isEmpty()) {
        return {};
    }

    if (m_process_state == Terminal_process_state::NOT_STARTED ||
        m_process_state == Terminal_process_state::STARTING)
    {
        return {};
    }

    const std::uint64_t sequence = next_sequence();
    if (!is_session_writable()) {
        return {
            true,
            make_rejected_result(
                sequence,
                Terminal_session_result_code::INVALID_STATE,
                make_backend_error(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("session write requires a running backend"))),
        };
    }

    const Terminal_session_result result = enqueue_and_process_synchronous_command(
        make_user_paste_command(sequence, std::move(bytes)));
    if (result.code == Terminal_session_result_code::ACCEPTED) {
        return_viewport_to_tail_after_user_input(sequence);
    }
    return {true, result};
}

Terminal_focus_event_result Terminal_session::write_focus_event(bool focused)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    if (!m_screen_model.has_value() || !m_screen_model->mode_state().focus_reporting) {
        return {};
    }

    // Focus reports are terminal-mode side effects. After exit, drop them silently
    // instead of surfacing a public write error on ordinary focus transitions.
    if (!is_session_writable()) {
        return {};
    }

    const QByteArrayView report = focused ? k_focus_in_report : k_focus_out_report;
    return {
        true,
        write_user_bytes_locked(
            QByteArray(report.data(), report.size()),
            User_write_viewport_policy::PRESERVE_VIEWPORT),
    };
}

void Terminal_session::set_ime_preedit(QString text, int cursor_position)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    Ime_preedit_state state;
    state.text            = std::move(text);
    state.cursor_position = std::clamp(cursor_position, 0, bounded_string_size(state.text));
    state.active          = !state.text.isEmpty();

    if (same_ime_preedit_state(m_ime_preedit, state)) {
        return;
    }

    m_ime_preedit = std::move(state);
    advance_ime_preedit_generation();
}

void Terminal_session::cancel_ime_preedit()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!ime_preedit_has_content(m_ime_preedit)) {
        return;
    }

    m_ime_preedit = {};
    advance_ime_preedit_generation();
}

Terminal_session_result Terminal_session::write_terminal_reply(const Terminal_reply& reply)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();

    const Terminal_session_command command =
        make_terminal_reply_command(next_sequence(), reply);
    if (!is_session_writable()) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("session write requires a running backend")));
    }

    return enqueue_and_process_synchronous_command(command);
}

Terminal_session_result Terminal_session::resize(
    QSizeF                 source_geometry,
    terminal_grid_size_t   grid_size)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();

    const std::uint64_t sequence = next_sequence();

    Terminal_resize_transaction resize;
    resize.id                       = next_resize_id();
    resize.source_geometry          = source_geometry;
    resize.target_grid_size         = grid_size;
    resize.snapshot_geometry        = source_geometry;
    resize.snapshot_grid_size       = grid_size;
    resize.backend_geometry_in_sync = m_backend_geometry_in_sync;

    return enqueue_and_process_synchronous_command(make_resize_command(sequence, resize));
}

Terminal_viewport_scroll_result Terminal_session::scroll_viewport_lines(int line_delta)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    if (!m_screen_model.has_value() || line_delta == 0) {
        return {};
    }

    const Terminal_viewport_scroll_result scroll_result =
        m_viewport_controller.scroll_lines(line_delta);
    if (scroll_result.action != Terminal_viewport_scroll_action::VIEWPORT_MOVED) {
        return scroll_result;
    }

    publish_viewport_snapshot_if_allowed(
        next_sequence(),
        QStringLiteral("viewport scrolled"));
    return scroll_result;
}

// Internal wheel/page scrolling may intentionally defer publication while
// synchronized output is active; public chrome needs the stricter variant below.
Terminal_viewport_scroll_result Terminal_session::scroll_published_viewport_lines(
    int line_delta)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    if (!m_screen_model.has_value() ||
        line_delta == 0 ||
        !model_allows_render_snapshot(*m_screen_model))
    {
        return {};
    }
    if (m_viewport_controller.state().active_buffer != Terminal_buffer_id::PRIMARY) {
        return {};
    }

    const Terminal_viewport_scroll_result scroll_result =
        m_viewport_controller.scroll_lines(line_delta);
    if (scroll_result.action != Terminal_viewport_scroll_action::VIEWPORT_MOVED) {
        return scroll_result;
    }

    publish_viewport_snapshot_if_allowed(
        next_sequence(),
        QStringLiteral("viewport scrolled"));
    return scroll_result;
}

Terminal_viewport_scroll_result Terminal_session::scroll_published_viewport_to_offset_from_tail(
    int offset_from_tail)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    if (!m_screen_model.has_value() || !model_allows_render_snapshot(*m_screen_model)) {
        return {};
    }
    if (m_viewport_controller.state().active_buffer != Terminal_buffer_id::PRIMARY) {
        return {};
    }

    const Terminal_viewport_state viewport = m_viewport_controller.state();
    const int target_offset =
        std::clamp(offset_from_tail, 0, std::max(0, viewport.scrollback_rows));
    const int line_delta = target_offset - viewport.offset_from_tail;
    if (line_delta == 0) {
        return {};
    }

    const Terminal_viewport_scroll_result scroll_result =
        m_viewport_controller.scroll_lines(line_delta);
    if (scroll_result.action != Terminal_viewport_scroll_action::VIEWPORT_MOVED) {
        return scroll_result;
    }

    publish_viewport_snapshot_if_allowed(
        next_sequence(),
        QStringLiteral("viewport scrolled"));
    return scroll_result;
}

void Terminal_session::set_selection_range(Terminal_selection_range range)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    if (!m_screen_model.has_value()) {
        return;
    }

    if (!selection_range_is_valid_for_active_model(range)) {
        if (m_selection.has_selection()) {
            m_selection.clear();
            m_selection_buffer_id = m_screen_model->active_buffer_id();
            publish_selection_snapshot(next_sequence(), QStringLiteral("selection cleared"));
        }
        return;
    }

    const Terminal_buffer_id active_buffer = m_screen_model->active_buffer_id();
    if (m_selection.has_selection() &&
        m_selection.range()   == range &&
        m_selection_buffer_id == active_buffer)
    {
        return;
    }

    m_selection.set_range(range);
    m_selection_buffer_id = active_buffer;
    publish_selection_snapshot(next_sequence(), QStringLiteral("selection changed"));
}

void Terminal_session::clear_selection()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    if (!m_selection.has_selection()) {
        return;
    }

    m_selection.clear();
    if (m_screen_model.has_value()) {
        m_selection_buffer_id = m_screen_model->active_buffer_id();
        publish_selection_snapshot(next_sequence(), QStringLiteral("selection cleared"));
    }
}

void Terminal_session::set_scrollback_limit(int limit)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();
    process_pending_commands();

    m_config.scrollback_limit = std::max(0, limit);
    if (!m_screen_model.has_value()) {
        return;
    }

    const Terminal_screen_model_result model_result =
        m_screen_model->set_scrollback_limit(m_config.scrollback_limit);
    m_render_snapshot_model_result = model_result;
    sync_viewport_from_model_result(model_result);

    if ((model_result_warrants_render_snapshot(model_result) || m_visual_bell_active) &&
        model_allows_render_snapshot(*m_screen_model))
    {
        publish_render_snapshot(next_sequence(), QStringLiteral("scrollback limit changed"));
    }
}

Terminal_session_result Terminal_session::interrupt()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();

    const std::uint64_t sequence = next_sequence();
    return enqueue_and_process_synchronous_command(make_interrupt_command(sequence));
}

Terminal_session_result Terminal_session::terminate()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();

    const std::uint64_t sequence = next_sequence();
    return enqueue_and_process_synchronous_command(make_terminate_command(sequence));
}

Terminal_session_result Terminal_session::force_release_synchronized_output()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    drain_backend_callback_commands();

    const std::uint64_t sequence = next_sequence();
    return enqueue_and_process_synchronous_command(
        make_force_release_synchronized_output_command(sequence));
}

Terminal_process_state Terminal_session::process_state() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_process_state;
}

bool Terminal_session::backend_ready() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_backend_ready;
}

bool Terminal_session::backend_geometry_in_sync() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_backend_geometry_in_sync;
}

bool Terminal_session::output_backpressure_active() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_output_backpressure_active;
}

bool Terminal_session::render_publication_blocked() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_screen_model.has_value() && !model_allows_render_snapshot(*m_screen_model);
}

bool Terminal_session::mouse_reporting_active() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_screen_model.has_value()) {
        return false;
    }

    const Terminal_input_mode_state modes = m_screen_model->input_mode_state();
    return
        modes.sgr_mouse_encoding                                         &&
        modes.mouse_tracking != Terminal_input_mouse_tracking_mode::NONE &&
        modes.mouse_tracking != Terminal_input_mouse_tracking_mode::X10;
}

bool Terminal_session::alternate_scroll_active() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return
        m_screen_model.has_value() &&
        m_screen_model->mode_state().alternate_scroll;
}

std::uint64_t Terminal_session::alternate_scroll_mode_generation() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_alternate_scroll_mode_generation;
}

Terminal_viewport_state Terminal_session::viewport_state() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_viewport_controller.state();
}

terminal_grid_size_t Terminal_session::grid_size() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_grid_size;
}

std::uint64_t Terminal_session::last_processed_sequence() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_last_processed_sequence;
}

bool Terminal_session::has_selection() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_selection.has_selection();
}

Terminal_selection_result Terminal_session::selected_text() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_selection.has_selection()) {
        return {Terminal_selection_result_code::NO_SELECTION, {}};
    }

    if (!m_screen_model.has_value()) {
        return {Terminal_selection_result_code::INVALID_RANGE, {}};
    }

    if (m_selection_buffer_id != m_screen_model->active_buffer_id()) {
        return {Terminal_selection_result_code::NO_SELECTION, {}};
    }

    return m_screen_model->selected_text(m_selection.range());
}

std::vector<Terminal_session_command> Terminal_session::processed_commands() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_processed_commands;
}

std::vector<Terminal_session_notification> Terminal_session::notifications() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_notifications;
}

std::vector<Terminal_session_notification> Terminal_session::take_pending_notifications()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    std::vector<Terminal_session_notification> notifications;
    notifications.swap(m_pending_notifications);
    return notifications;
}

std::vector<Terminal_resize_transaction> Terminal_session::resize_transactions() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_resize_transactions;
}

std::vector<QByteArray> Terminal_session::output_chunks() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_output_chunks;
}

std::optional<Terminal_render_snapshot> Terminal_session::latest_render_snapshot() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_latest_render_snapshot == nullptr) {
        return std::nullopt;
    }

    return *m_latest_render_snapshot;
}

std::shared_ptr<const Terminal_render_snapshot>
Terminal_session::latest_render_snapshot_handle() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_latest_render_snapshot;
}

std::uint64_t Terminal_session::render_snapshot_generation() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_render_snapshot_generation;
}

void Terminal_session::mark_render_snapshot_synced(std::uint64_t generation)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (generation <= m_render_snapshot_generation) {
        m_render_snapshot_synced_generation =
            std::max(m_render_snapshot_synced_generation, generation);
    }
}

Ime_preedit_state Terminal_session::ime_preedit_state() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_ime_preedit;
}

std::uint64_t Terminal_session::ime_preedit_generation() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_ime_preedit_generation;
}

std::optional<Terminal_screen_model_result> Terminal_session::last_model_ingest_result() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_last_model_ingest_result;
}

void Terminal_session::set_dirty_row_stats_enabled(bool enabled)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    m_config.capture_dirty_row_stats = enabled;
    if (m_screen_model.has_value()) {
        m_screen_model->set_dirty_row_stats_enabled(enabled);
    }
}

Terminal_screen_model_dirty_row_stats Terminal_session::dirty_row_stats() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_screen_model.has_value()
        ? m_screen_model->dirty_row_stats()
        : Terminal_screen_model_dirty_row_stats{};
}

Terminal_screen_model_dirty_row_timeline Terminal_session::dirty_row_timeline() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_screen_model.has_value()
        ? m_screen_model->dirty_row_timeline()
        : Terminal_screen_model_dirty_row_timeline{};
}

std::optional<Terminal_backend_exit> Terminal_session::exit_status() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    return m_exit_status;
}

Terminal_session_result Terminal_session::enqueue_and_process_synchronous_command(
    Terminal_session_command command)
{
    const std::uint64_t sequence = command.sequence;
    const Terminal_session_result enqueue_result = enqueue_command(std::move(command));
    if (enqueue_result.code != Terminal_session_result_code::ACCEPTED) {
        return enqueue_result;
    }

    begin_result_capture(sequence);
    process_pending_commands();
    const Terminal_session_result result = result_after_processing(sequence, enqueue_result);
    end_result_capture();
    return result;
}

Terminal_session_result Terminal_session::enqueue_command(Terminal_session_command command)
{
    const Queue_category category   = queue_category_for(command.kind);
    const std::size_t    byte_count = static_cast<std::size_t>(command.bytes.size());
    if (category == Queue_category::OUTPUT &&
        should_ignore_backend_output_after_stop(command.sequence))
    {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::OUTPUT_OVERFLOW,
                QStringLiteral("backend output ignored after terminal stop request")));
    }

    const Terminal_queue_result queue_result =
        would_accept_command(category, byte_count, 1U);

    if (queue_result.code == Terminal_queue_result_code::HARD_LIMIT_REACHED) {
        Terminal_backend_error error;
        Terminal_session_result_code code = Terminal_session_result_code::QUEUE_HARD_LIMIT_REACHED;
        if (category == Queue_category::OUTPUT) {
            return handle_output_overflow(
                command.sequence,
                QStringLiteral("backend output queue hard limit reached"));
        }
        else
        if (command.kind == Terminal_session_command_kind::USER_PASTE) {
            error = make_backend_error(
                Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("paste text exceeds session write queue hard limit"));
        }
        else {
            error = make_backend_error(
                Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("session command queue hard limit reached"));
        }

        return make_rejected_result(command.sequence, code, std::move(error));
    }

    add_to_queue_state(category, byte_count);
    const std::uint64_t sequence = command.sequence;
    m_pending_commands.push_back(std::move(command));

    if (category == Queue_category::OUTPUT) {
        set_output_backpressure_active(queue_result.high_water_reached, sequence);
    }

    return {
        Terminal_session_result_code::ACCEPTED,
        sequence,
        queue_result.high_water_reached,
        std::nullopt,
    };
}

void Terminal_session::process_pending_commands()
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::process_pending_commands");

    if (m_processing_commands) {
        return;
    }

    m_processing_commands = true;
    for (;;) {
        drain_backend_callback_commands();
        if (m_pending_commands.empty()) {
            break;
        }

        Terminal_session_command command = std::move(m_pending_commands.front());
        m_pending_commands.pop_front();

        const Queue_category category   = queue_category_for(command.kind);
        const std::size_t    byte_count = static_cast<std::size_t>(command.bytes.size());
        record_processed_command(command);
        m_last_processed_sequence = command.sequence;

        m_backend_error_queued_during_command = false;
        record_result(process_command(std::move(command)));
        remove_from_queue_state(category, byte_count);
        if (category == Queue_category::OUTPUT) {
            set_output_backpressure_active(
                queue_high_water_reached(category),
                m_last_processed_sequence);
        }
    }
    m_processing_commands = false;
}

Terminal_session_result Terminal_session::process_command(Terminal_session_command command)
{
    switch (command.kind) {
        case Terminal_session_command_kind::START:
            return process_start_command(command);
        case Terminal_session_command_kind::USER_WRITE:
        case Terminal_session_command_kind::USER_PASTE:
        case Terminal_session_command_kind::TERMINAL_REPLY:
            return process_write_command(command);
        case Terminal_session_command_kind::RESIZE:
            return process_resize_command(std::move(command));
        case Terminal_session_command_kind::INTERRUPT:
            return process_interrupt_command(command);
        case Terminal_session_command_kind::TERMINATE:
            return process_terminate_command(command);
        case Terminal_session_command_kind::FORCE_RELEASE_SYNCHRONIZED_OUTPUT:
            return process_force_release_synchronized_output_command(command);
        case Terminal_session_command_kind::BACKEND_OUTPUT:
            return process_backend_output_command(command);
        case Terminal_session_command_kind::BACKEND_EXIT:
            return process_backend_exit_command(command);
        case Terminal_session_command_kind::BACKEND_ERROR:
            return process_backend_error_command(command);
    }

    return {
        Terminal_session_result_code::ACCEPTED,
        command.sequence,
        false,
        std::nullopt,
    };
}

Terminal_session_result Terminal_session::process_start_command(
    const Terminal_session_command& command)
{
    if (m_backend == nullptr || !command.launch_config.has_value()) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_ARGUMENT,
            make_backend_error(
                Terminal_backend_error_code::START_FAILED,
                QStringLiteral("session start requires a backend and launch config")));
    }

    if (m_process_state != Terminal_process_state::NOT_STARTED) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::START_FAILED,
                QStringLiteral("session is already running")));
    }

    const Terminal_backend_result config_result =
        validate_launch_config(*command.launch_config);
    if (is_backend_rejection(config_result)) {
        m_process_state = Terminal_process_state::FAILED;
        m_backend_ready = false;
        record_backend_error(command.sequence, *config_result.error);
        return {
            Terminal_session_result_code::INVALID_ARGUMENT,
            command.sequence,
            false,
            config_result.error,
        };
    }

    if (!is_terminal_screen_model_grid_size_supported(*command.launch_config->initial_grid_size)) {
        Terminal_backend_error error = make_backend_error(
            Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
            QStringLiteral("initial terminal size exceeds screen model limits"));
        m_process_state = Terminal_process_state::FAILED;
        m_backend_ready = false;
        record_backend_error(command.sequence, error);
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_ARGUMENT,
            std::move(error));
    }

    m_grid_size = *command.launch_config->initial_grid_size;
    initialize_screen_model(m_grid_size);

    m_process_state = Terminal_process_state::STARTING;
    m_backend_geometry_in_sync = true;
    const Terminal_backend_result backend_result =
        m_backend->start(*command.launch_config, make_backend_callbacks());
    drain_backend_callback_commands();

    if (is_backend_rejection(backend_result)) {
        m_process_state            = Terminal_process_state::FAILED;
        m_backend_ready            = false;
        m_backend_geometry_in_sync = false;
        if (!m_backend_error_queued_during_command) {
            record_backend_error(command.sequence, *backend_result.error);
        }
        return {
            Terminal_session_result_code::BACKEND_REJECTED,
            command.sequence,
            false,
            backend_result.error,
        };
    }

    if (m_stop_requested) {
        m_process_state            = Terminal_process_state::RUNNING;
        m_backend_ready            = false;
        m_backend_geometry_in_sync = false;
        return {
            Terminal_session_result_code::BACKEND_REJECTED,
            command.sequence,
            false,
            make_backend_error(
                Terminal_backend_error_code::OUTPUT_OVERFLOW,
                QStringLiteral("backend output overflowed during start")),
        };
    }

    m_process_state            = Terminal_process_state::RUNNING;
    m_backend_ready            = true;
    m_backend_geometry_in_sync = true;
    record_notification({
        Terminal_session_notification_kind::PROCESS_STARTED,
        command.sequence,
        QStringLiteral("process started"),
    });

    return {
        Terminal_session_result_code::ACCEPTED,
        command.sequence,
        false,
        std::nullopt,
    };
}

Terminal_session_result Terminal_session::process_write_command(
    const Terminal_session_command& command)
{
    if (m_backend       == nullptr                         ||
        m_process_state != Terminal_process_state::RUNNING ||
        m_stop_requested)
    {
        Terminal_backend_error error = make_backend_error(
            Terminal_backend_error_code::WRITE_FAILED,
            QStringLiteral("session write requires a running backend"));
        if (command.kind == Terminal_session_command_kind::TERMINAL_REPLY) {
            record_backend_error(command.sequence, error);
        }
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            std::move(error));
    }

    if (command.bytes.isEmpty()) {
        Terminal_backend_error error = make_backend_error(
            Terminal_backend_error_code::WRITE_FAILED,
            QStringLiteral("session write requires bytes"));
        if (command.kind == Terminal_session_command_kind::TERMINAL_REPLY) {
            record_backend_error(command.sequence, error);
        }
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_ARGUMENT,
            std::move(error));
    }

    const Terminal_backend_result backend_result = m_backend->write(command.bytes);
    if (is_backend_rejection(backend_result)) {
        record_backend_error(command.sequence, *backend_result.error);
        return {
            Terminal_session_result_code::BACKEND_REJECTED,
            command.sequence,
            false,
            backend_result.error,
        };
    }

    return {
        Terminal_session_result_code::ACCEPTED,
        command.sequence,
        false,
        std::nullopt,
    };
}

Terminal_session_result Terminal_session::process_resize_command(
    Terminal_session_command command)
{
    if (!command.resize.has_value()) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_ARGUMENT,
            make_backend_error(
                Terminal_backend_error_code::RESIZE_FAILED,
                QStringLiteral("resize command requires a transaction")));
    }

    Terminal_resize_transaction resize = *command.resize;
    if (m_screen_model.has_value()) {
        resize.active_buffer = m_screen_model->active_buffer_id();
    }

    if (!is_terminal_screen_model_grid_size_supported(resize.target_grid_size)) {
        resize.model_result             = Terminal_model_resize_result::INVALID_GRID_SIZE;
        resize.backend_result           = Terminal_backend_resize_result::FAILED;
        resize.snapshot_grid_size       = m_grid_size;
        resize.backend_geometry_in_sync = m_backend_geometry_in_sync;
        record_resize_transaction(resize);
        record_notification({
            Terminal_session_notification_kind::RESIZE_TRANSACTION,
            command.sequence,
            QStringLiteral("resize rejected"),
            std::nullopt,
            std::nullopt,
            resize,
            false,
        });

        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_ARGUMENT,
            make_backend_error(
                Terminal_backend_error_code::RESIZE_FAILED,
                is_valid_grid_size(resize.target_grid_size)
                    ? QStringLiteral("resize exceeds screen model limits")
                    : QStringLiteral("resize requires a positive grid")));
    }

    if (m_backend       == nullptr                         ||
        m_process_state != Terminal_process_state::RUNNING ||
        m_stop_requested)
    {
        resize.model_result             = Terminal_model_resize_result::NOT_APPLIED;
        resize.backend_result           = Terminal_backend_resize_result::FAILED;
        resize.snapshot_grid_size       = m_grid_size;
        resize.backend_geometry_in_sync = m_backend_geometry_in_sync;
        record_resize_transaction(resize);
        record_notification({
            Terminal_session_notification_kind::RESIZE_TRANSACTION,
            command.sequence,
            QStringLiteral("resize requires a running backend"),
            std::nullopt,
            std::nullopt,
            resize,
            false,
        });

        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::RESIZE_FAILED,
                QStringLiteral("resize requires a running backend")));
    }

    if (!m_screen_model.has_value()) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::RESIZE_FAILED,
                QStringLiteral("resize requires an initialized screen model")));
    }

    const terminal_grid_size_t previous_grid_size = m_grid_size;
    m_grid_size               = resize.target_grid_size;
    resize.snapshot_grid_size = m_grid_size;
    resize.model_result       = Terminal_model_resize_result::APPLIED;
    const Terminal_screen_model_result model_result = m_screen_model->resize(m_grid_size);
    sync_viewport_from_model_result(model_result);
    m_render_snapshot_model_result = model_result;
    const bool backend_geometry_was_in_sync = m_backend_geometry_in_sync;
    const bool render_snapshot_available =
        model_allows_render_snapshot(*m_screen_model);
    const bool grid_size_changed = !grid_sizes_match(previous_grid_size, m_grid_size);

    const Terminal_backend_result backend_result =
        m_backend->resize({resize.id, resize.target_grid_size});
    if (is_backend_rejection(backend_result)) {
        resize.backend_result           = Terminal_backend_resize_result::FAILED;
        resize.backend_geometry_in_sync = false;
        m_backend_geometry_in_sync      = false;
        record_resize_transaction(resize);
        record_backend_error(command.sequence, *backend_result.error);
        record_notification({
            Terminal_session_notification_kind::RESIZE_TRANSACTION,
            command.sequence,
            QStringLiteral("resize failed"),
            std::nullopt,
            std::nullopt,
            resize,
            false,
        });

        if (!render_snapshot_available &&
            (grid_size_changed || backend_geometry_was_in_sync ||
                m_latest_render_snapshot == nullptr))
        {
            publish_synchronized_resize_snapshot(
                command.sequence,
                QStringLiteral("resize geometry snapshot ready"));
        }
        else
        if (render_snapshot_available &&
            (model_result_warrants_render_snapshot(model_result) ||
                backend_geometry_was_in_sync))
        {
            publish_render_snapshot(command.sequence, QStringLiteral("resize snapshot ready"));
        }

        return {
            Terminal_session_result_code::BACKEND_REJECTED,
            command.sequence,
            false,
            backend_result.error,
        };
    }

    resize.backend_result           = Terminal_backend_resize_result::APPLIED;
    resize.backend_geometry_in_sync = true;
    m_backend_geometry_in_sync      = true;
    record_resize_transaction(resize);
    record_notification({
        Terminal_session_notification_kind::RESIZE_TRANSACTION,
        command.sequence,
        QStringLiteral("resize applied"),
        std::nullopt,
        std::nullopt,
        resize,
        false,
    });

    if (!render_snapshot_available &&
        (grid_size_changed ||
            !backend_geometry_was_in_sync ||
            m_latest_render_snapshot == nullptr))
    {
        publish_synchronized_resize_snapshot(
            command.sequence,
            QStringLiteral("resize geometry snapshot ready"));
    }
    else
    if (render_snapshot_available &&
        (model_result_warrants_render_snapshot(model_result) ||
            !backend_geometry_was_in_sync))
    {
        publish_render_snapshot(command.sequence, QStringLiteral("resize snapshot ready"));
    }

    return {
        Terminal_session_result_code::ACCEPTED,
        command.sequence,
        false,
        std::nullopt,
    };
}

Terminal_session_result Terminal_session::process_interrupt_command(
    const Terminal_session_command& command)
{
    if (m_backend       == nullptr                         ||
        m_process_state != Terminal_process_state::RUNNING ||
        m_stop_requested)
    {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::INTERRUPT_FAILED,
                QStringLiteral("interrupt requires a running backend")));
    }

    const Terminal_backend_result backend_result = m_backend->interrupt();
    if (is_backend_rejection(backend_result)) {
        if (!m_backend_error_queued_during_command) {
            record_backend_error(command.sequence, *backend_result.error);
        }
        return {
            Terminal_session_result_code::BACKEND_REJECTED,
            command.sequence,
            false,
            backend_result.error,
        };
    }

    return {
        Terminal_session_result_code::ACCEPTED,
        command.sequence,
        false,
        std::nullopt,
    };
}

Terminal_session_result Terminal_session::process_terminate_command(
    const Terminal_session_command& command)
{
    if (m_backend           == nullptr                           ||
        (m_process_state != Terminal_process_state::RUNNING &&
         m_process_state != Terminal_process_state::STARTING) ||
        m_stop_requested)
    {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::TERMINATE_FAILED,
                QStringLiteral("terminate requires a live backend")));
    }

    m_stop_requested          = true;
    m_stop_requested_sequence = command.sequence;
    m_backend_ready           = false;
    const Terminal_backend_result backend_result = m_backend->terminate();
    if (is_backend_rejection(backend_result)) {
        m_stop_requested          = false;
        m_stop_requested_sequence = 0U;
        m_backend_ready           = true;
        if (!m_backend_error_queued_during_command) {
            record_backend_error(command.sequence, *backend_result.error);
        }
        return {
            Terminal_session_result_code::BACKEND_REJECTED,
            command.sequence,
            false,
            backend_result.error,
        };
    }

    return {
        Terminal_session_result_code::ACCEPTED,
        command.sequence,
        false,
        std::nullopt,
    };
}

Terminal_session_result Terminal_session::process_force_release_synchronized_output_command(
    const Terminal_session_command& command)
{
    if (!m_screen_model.has_value()) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::READ_FAILED,
                QStringLiteral("synchronized output release requires an initialized screen model")));
    }

    const Terminal_screen_model_result model_result =
        m_screen_model->force_release_synchronized_output();
    if (m_config.capture_last_model_ingest_result) {
        m_last_model_ingest_result = model_result;
    }
    m_render_snapshot_model_result = model_result;
    sync_viewport_from_model_result(model_result);

    if (model_result_warrants_render_snapshot(model_result) || m_visual_bell_active) {
        publish_render_snapshot(
            command.sequence,
            QStringLiteral("synchronized output force released"));
    }

    return {
        Terminal_session_result_code::ACCEPTED,
        command.sequence,
        false,
        std::nullopt,
    };
}

Terminal_session_result Terminal_session::process_backend_output_command(
    const Terminal_session_command& command)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::process_backend_output_command");

    if (should_ignore_backend_output_after_stop(command.sequence)) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::OUTPUT_OVERFLOW,
                QStringLiteral("backend output ignored after terminal stop request")));
    }

    record_output_chunk(command.bytes);
    if (!m_screen_model.has_value()) {
        Terminal_backend_error error = make_backend_error(
            Terminal_backend_error_code::READ_FAILED,
            QStringLiteral("backend output requires an initialized screen model"));
        record_backend_error(command.sequence, error);
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            std::move(error));
    }

    if (!command.bytes.isEmpty()) {
        record_output_activity(command.sequence);
    }

    const Terminal_utf8_scan_state backend_output_prescan_utf8_state =
        m_backend_output_prescan_utf8_state;
    QByteArray combined_output;
    QByteArrayView remaining(command.bytes);
    if (!m_backend_output_prescan_pending.isEmpty()) {
        combined_output = m_backend_output_prescan_pending + command.bytes;
        m_backend_output_prescan_pending.clear();
        remaining = QByteArrayView(combined_output);
    }

    const qsizetype incomplete_csi_start = trailing_incomplete_csi_start(
        remaining,
        backend_output_prescan_utf8_state);
    if (incomplete_csi_start >= 0) {
        const qsizetype pending_size = remaining.size() - incomplete_csi_start;
        if (static_cast<std::size_t>(pending_size) <= k_control_sequence_pending_limit_bytes) {
            m_backend_output_prescan_pending = QByteArray(
                remaining.data() + incomplete_csi_start,
                pending_size);
            remaining = remaining.sliced(0, incomplete_csi_start);
        }
    }

    Terminal_utf8_scan_state remaining_utf8_scan_state = backend_output_prescan_utf8_state;
    while (!remaining.empty()) {
        const Sync_set_sequence sync_set = next_synchronized_output_set_sequence(
            remaining,
            remaining_utf8_scan_state);
        if (sync_set.start < 0) {
            ingest_backend_output_segment(command.sequence, remaining);
            break;
        }

        if (sync_set.start > 0) {
            ingest_backend_output_segment(
                command.sequence,
                remaining.sliced(0, sync_set.start));
            remaining = remaining.sliced(sync_set.start);
            reset_utf8_scan_state(remaining_utf8_scan_state);
            continue;
        }

        const QByteArray prefix = sync_sequence_prefix(remaining, sync_set);
        if (!prefix.isEmpty()) {
            ingest_backend_output_segment(command.sequence, QByteArrayView(prefix));
            combined_output = sync_sequence_suffix_and_tail(remaining, sync_set);
            remaining = QByteArrayView(combined_output);
            reset_utf8_scan_state(remaining_utf8_scan_state);
            continue;
        }

        if (sync_set.end < remaining.size()) {
            ingest_backend_output_segment(
                command.sequence,
                remaining.sliced(0, sync_set.end));
            remaining = remaining.sliced(sync_set.end);
            reset_utf8_scan_state(remaining_utf8_scan_state);
            continue;
        }

        ingest_backend_output_segment(command.sequence, remaining);
        break;
    }

    m_backend_output_prescan_utf8_state = utf8_scan_state_after(
        command.bytes,
        m_backend_output_prescan_utf8_state);

    return {
        Terminal_session_result_code::ACCEPTED,
        command.sequence,
        false,
        std::nullopt,
    };
}

Terminal_session_result Terminal_session::process_backend_exit_command(
    const Terminal_session_command& command)
{
    if (!command.exit.has_value()) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_ARGUMENT,
            make_backend_error(
                Terminal_backend_error_code::START_FAILED,
                QStringLiteral("backend exit command requires exit status")));
    }

    if (m_exit_status.has_value()) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_STATE,
            make_backend_error(
                Terminal_backend_error_code::START_FAILED,
                QStringLiteral("backend exit was already reported")));
    }

    m_exit_status    = *command.exit;
    m_process_state  = command.exit->reason == Terminal_exit_reason::FAILED_TO_START
        ? Terminal_process_state::FAILED
        : Terminal_process_state::EXITED;
    m_backend_ready  = false;
    m_stop_requested = false;
    record_notification({
        Terminal_session_notification_kind::PROCESS_EXITED,
        command.sequence,
        QStringLiteral("process exited"),
        std::nullopt,
        *command.exit,
        std::nullopt,
        false,
    });

    return {
        Terminal_session_result_code::ACCEPTED,
        command.sequence,
        false,
        std::nullopt,
    };
}

Terminal_session_result Terminal_session::process_backend_error_command(
    const Terminal_session_command& command)
{
    if (!command.error.has_value()) {
        return make_rejected_result(
            command.sequence,
            Terminal_session_result_code::INVALID_ARGUMENT,
            make_backend_error(
                Terminal_backend_error_code::START_FAILED,
                QStringLiteral("backend error command requires an error")));
    }

    record_backend_error(command.sequence, *command.error);
    return {
        Terminal_session_result_code::ACCEPTED,
        command.sequence,
        false,
        std::nullopt,
    };
}

Terminal_backend_callbacks Terminal_session::make_backend_callbacks()
{
    const std::shared_ptr<Terminal_session_callback_lifetime> lifetime =
        m_callback_lifetime;
    const std::function<void()> backend_event_notifier = m_config.backend_event_notifier;
    const bool deferred_callback_delivery = uses_deferred_backend_callbacks(m_config);

    const auto notify_backend_event = [backend_event_notifier](Terminal_session* session) {
        if (backend_event_notifier) {
            // Callback commands are durable in the lifetime queue; the notifier
            // only needs to wake the owner once for all commands queued so far.
            backend_event_notifier();
            return;
        }

        session->process_backend_callback_events();
    };

    Terminal_backend_callbacks callbacks;
    callbacks.output_received =
        [lifetime, notify_backend_event, deferred_callback_delivery](QByteArray bytes) {
            Backend_callback_invocation callback(lifetime);
            if (Terminal_session* session = callback.session()) {
                session->record_backend_output_capture_chunk(bytes);
            }
            const Terminal_queue_result queue_result =
                callback.enqueue(make_backend_output_command(0U, std::move(bytes)));
            if (Terminal_session* session = callback.session()) {
                if (deferred_callback_delivery &&
                    queue_result.code == Terminal_queue_result_code::ACCEPTED &&
                    queue_result.high_water_reached)
                {
                    session->pause_backend_output_from_callback_ingress();
                }
                notify_backend_event(session);
            }
        };
    callbacks.process_exited = [lifetime, notify_backend_event](Terminal_backend_exit exit) {
        Backend_callback_invocation callback(lifetime);
        callback.enqueue(make_backend_exit_command(0U, exit));
        if (Terminal_session* session = callback.session()) {
            notify_backend_event(session);
        }
    };
    callbacks.error_reported = [lifetime, notify_backend_event](Terminal_backend_error error) {
        Backend_callback_invocation callback(lifetime);
        callback.enqueue(make_backend_error_command(0U, std::move(error)));
        if (Terminal_session* session = callback.session()) {
            notify_backend_event(session);
        }
    };
    return callbacks;
}

void Terminal_session::process_backend_callback_events()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    process_pending_commands();
}

void Terminal_session::pause_backend_output_from_callback_ingress()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_callback_lifetime->high_water_reached() &&
        !queue_high_water_reached(Queue_category::OUTPUT))
    {
        return;
    }

    set_output_backpressure_active(true, next_sequence());
}

void Terminal_session::drain_backend_callback_commands()
{
    for (;;) {
        std::deque<Terminal_session_command> commands =
            m_callback_lifetime->take_pending_commands();
        if (commands.empty()) {
            return;
        }

        while (!commands.empty()) {
            Terminal_session_command command = std::move(commands.front());
            commands.pop_front();

            command.sequence = next_sequence();
            if (command.kind == Terminal_session_command_kind::BACKEND_ERROR &&
                m_processing_commands)
            {
                m_backend_error_queued_during_command = true;
            }

            if (command.kind        == Terminal_session_command_kind::BACKEND_ERROR &&
                command.error.has_value()                                           &&
                command.error->code == Terminal_backend_error_code::OUTPUT_OVERFLOW)
            {
                Terminal_session_result result = handle_output_overflow(
                    command.sequence,
                    command.error->message);
                record_result(std::move(result));
                continue;
            }

            if (command.kind == Terminal_session_command_kind::BACKEND_OUTPUT &&
                should_ignore_backend_output_after_stop(command.sequence))
            {
                Terminal_session_result result = make_rejected_result(
                    command.sequence,
                    Terminal_session_result_code::INVALID_STATE,
                    make_backend_error(
                        Terminal_backend_error_code::OUTPUT_OVERFLOW,
                        QStringLiteral("backend output ignored after terminal stop request")));
                record_result(std::move(result));
                continue;
            }

            Terminal_session_result result = enqueue_command(std::move(command));
            record_result(std::move(result));
        }
    }
}

void Terminal_session::record_processed_command(Terminal_session_command command)
{
    if (m_config.trace_command_limit == 0U) {
        return;
    }

    m_processed_commands.push_back(std::move(command));
    if (m_processed_commands.size() > m_config.trace_command_limit) {
        m_processed_commands.erase(m_processed_commands.begin());
    }
}

void Terminal_session::record_notification(Terminal_session_notification notification)
{
    record_pending_notification(notification);

    if (m_config.trace_notification_limit == 0U) {
        return;
    }

    m_notifications.push_back(std::move(notification));
    if (m_notifications.size() > m_config.trace_notification_limit) {
        m_notifications.erase(m_notifications.begin());
    }
}

void Terminal_session::record_pending_notification(
    Terminal_session_notification notification)
{
    const auto same_kind = [&notification](const Terminal_session_notification& pending) {
        return pending.kind == notification.kind;
    };

    switch (notification.kind) {
        case Terminal_session_notification_kind::SNAPSHOT_READY:
        case Terminal_session_notification_kind::RESIZE_TRANSACTION:
            return;
        case Terminal_session_notification_kind::OUTPUT_ACTIVITY:
        case Terminal_session_notification_kind::OUTPUT_BACKPRESSURE_CHANGED:
        case Terminal_session_notification_kind::BELL_REQUESTED:
        case Terminal_session_notification_kind::TITLE_CHANGED:
        case Terminal_session_notification_kind::ICON_NAME_CHANGED:
        case Terminal_session_notification_kind::TEXT_AREA_RESIZE_REQUESTED:
            if (auto it = std::find_if(
                    m_pending_notifications.begin(),
                    m_pending_notifications.end(),
                    same_kind);
                it != m_pending_notifications.end())
            {
                *it = std::move(notification);
                return;
            }
            break;
        case Terminal_session_notification_kind::PROCESS_STARTED:
        case Terminal_session_notification_kind::PROCESS_EXITED:
        case Terminal_session_notification_kind::BACKEND_ERROR:
        case Terminal_session_notification_kind::HOST_REQUEST:
            break;
    }

    if (m_pending_notifications.size() < k_pending_notification_limit) {
        m_pending_notifications.push_back(std::move(notification));
        return;
    }

    const auto coalescible = [](const Terminal_session_notification& pending) {
        return
            pending.kind == Terminal_session_notification_kind::OUTPUT_ACTIVITY             ||
            pending.kind == Terminal_session_notification_kind::OUTPUT_BACKPRESSURE_CHANGED ||
            pending.kind == Terminal_session_notification_kind::BELL_REQUESTED              ||
            pending.kind == Terminal_session_notification_kind::TITLE_CHANGED               ||
            pending.kind == Terminal_session_notification_kind::ICON_NAME_CHANGED           ||
            pending.kind == Terminal_session_notification_kind::TEXT_AREA_RESIZE_REQUESTED;
    };
    if (auto it = std::find_if(
            m_pending_notifications.begin(),
            m_pending_notifications.end(),
            coalescible);
        it != m_pending_notifications.end())
    {
        *it = std::move(notification);
        return;
    }

    m_pending_notifications.erase(m_pending_notifications.begin());
    m_pending_notifications.push_back(std::move(notification));
}

void Terminal_session::record_resize_transaction(Terminal_resize_transaction resize)
{
    if (m_config.trace_resize_limit == 0U) {
        return;
    }

    m_resize_transactions.push_back(resize);
    if (m_resize_transactions.size() > m_config.trace_resize_limit) {
        m_resize_transactions.erase(m_resize_transactions.begin());
    }
}

void Terminal_session::record_backend_output_capture_chunk(QByteArrayView bytes)
{
    if (m_config.backend_output_capture_path.isEmpty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_backend_output_capture_mutex);
    QFile capture_file(m_config.backend_output_capture_path);
    if (capture_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        capture_file.write(bytes.data(), bytes.size());
    }
}

void Terminal_session::record_output_chunk(QByteArray bytes)
{
    if (m_config.trace_output_chunk_limit == 0U) {
        return;
    }

    m_output_chunks.push_back(std::move(bytes));
    if (m_output_chunks.size() > m_config.trace_output_chunk_limit) {
        m_output_chunks.erase(m_output_chunks.begin());
    }
}

void Terminal_session::record_output_activity(std::uint64_t sequence)
{
    record_notification({
        Terminal_session_notification_kind::OUTPUT_ACTIVITY,
        sequence,
        QStringLiteral("output activity"),
    });
}

void Terminal_session::record_backend_error(
    std::uint64_t          sequence,
    Terminal_backend_error error)
{
    const QString message = error.message;
    record_notification({
        Terminal_session_notification_kind::BACKEND_ERROR,
        sequence,
        message,
        std::move(error),
        std::nullopt,
        std::nullopt,
        false,
    });
}

void Terminal_session::initialize_screen_model(terminal_grid_size_t grid_size)
{
    Terminal_screen_model_config screen_config;
    screen_config.grid_size                 = grid_size;
    screen_config.scrollback_limit          = m_config.scrollback_limit;
    screen_config.retain_structural_actions = m_config.capture_last_model_ingest_result;
    screen_config.recover_scrollback_from_primary_repaints =
        m_config.recover_scrollback_from_primary_repaints;
    m_screen_model.emplace(screen_config);
    m_screen_model->set_dirty_row_stats_enabled(m_config.capture_dirty_row_stats);
    m_viewport_controller = Terminal_viewport_controller{};
    m_viewport_controller.set_visible_rows(grid_size.rows);
    m_backend_output_prescan_pending.clear();
    reset_utf8_scan_state(m_backend_output_prescan_utf8_state);
    m_latest_render_snapshot.reset();
    m_latest_content_render_snapshot.reset();
    m_last_model_ingest_result.reset();
    m_render_snapshot_model_result.reset();
    m_ime_preedit                       = {};
    m_render_snapshot_generation        = 0U;
    m_render_snapshot_synced_generation = 0U;
    m_ime_preedit_generation            = 0U;
    m_alternate_scroll_mode_generation  = 0U;
    m_visual_bell_active                = false;
}

void Terminal_session::ingest_backend_output_segment(
    std::uint64_t  sequence,
    QByteArrayView bytes)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::ingest_backend_output_segment");

    if (bytes.empty()) {
        return;
    }

    Terminal_screen_model_result ingest_result;
    {
        VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::model_ingest");
        ingest_result = m_screen_model->ingest(bytes);
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::store_ingest_result");
        if (m_config.capture_last_model_ingest_result) {
            m_last_model_ingest_result = ingest_result;
        }
        m_render_snapshot_model_result = ingest_result;
    }

    bool render_snapshot_metadata_changed = false;
    {
        VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::handle_parser_actions");
        render_snapshot_metadata_changed = handle_parser_actions(sequence, ingest_result);
    }
    {
        VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::sync_viewport_from_model_result");
        sync_viewport_from_model_result(ingest_result);
    }

    if ((model_result_warrants_render_snapshot(ingest_result) ||
        render_snapshot_metadata_changed                      ||
        m_visual_bell_active)
        &&
        model_allows_render_snapshot(*m_screen_model))
    {
        VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::publish_backend_render_snapshot");
        publish_render_snapshot(sequence, QStringLiteral("backend output received"));
    }
}

bool Terminal_session::apply_text_area_resize_request(
    std::uint64_t          sequence,
    terminal_grid_size_t   grid_size)
{
    if (!m_screen_model.has_value() ||
        !is_terminal_screen_model_grid_size_supported(grid_size) ||
        !grid_sizes_match(m_screen_model->grid_size(), grid_size))
    {
        return false;
    }

    const bool grid_size_changed = !grid_sizes_match(m_grid_size, grid_size);
    if (!grid_size_changed && m_backend_geometry_in_sync) {
        return false;
    }
    const bool backend_geometry_was_in_sync = m_backend_geometry_in_sync;

    Terminal_resize_transaction resize;
    resize.id                 = next_resize_id();
    resize.target_grid_size   = grid_size;
    resize.active_buffer      = m_screen_model->active_buffer_id();
    resize.model_result       = Terminal_model_resize_result::APPLIED;
    resize.snapshot_grid_size = grid_size;

    m_grid_size = grid_size;

    if (m_backend       == nullptr                         ||
        m_process_state != Terminal_process_state::RUNNING ||
        m_stop_requested)
    {
        resize.backend_result           = Terminal_backend_resize_result::FAILED;
        resize.backend_geometry_in_sync = false;
        m_backend_geometry_in_sync      = false;
        record_resize_transaction(resize);
        record_notification({
            Terminal_session_notification_kind::RESIZE_TRANSACTION,
            sequence,
            QStringLiteral("text-area resize requires a running backend"),
            std::nullopt,
            std::nullopt,
            resize,
            false,
        });
        return backend_geometry_was_in_sync != m_backend_geometry_in_sync;
    }

    const Terminal_backend_result backend_result =
        m_backend->resize({resize.id, resize.target_grid_size});
    if (is_backend_rejection(backend_result)) {
        resize.backend_result           = Terminal_backend_resize_result::FAILED;
        resize.backend_geometry_in_sync = false;
        m_backend_geometry_in_sync      = false;
        record_resize_transaction(resize);
        record_backend_error(sequence, *backend_result.error);
        record_notification({
            Terminal_session_notification_kind::RESIZE_TRANSACTION,
            sequence,
            QStringLiteral("text-area resize failed"),
            std::nullopt,
            std::nullopt,
            resize,
            false,
        });
        return backend_geometry_was_in_sync != m_backend_geometry_in_sync;
    }

    resize.backend_result           = Terminal_backend_resize_result::APPLIED;
    resize.backend_geometry_in_sync = true;
    m_backend_geometry_in_sync      = true;
    record_resize_transaction(resize);
    record_notification({
        Terminal_session_notification_kind::RESIZE_TRANSACTION,
        sequence,
        QStringLiteral("text-area resize applied"),
        std::nullopt,
        std::nullopt,
        resize,
        false,
    });
    return backend_geometry_was_in_sync != m_backend_geometry_in_sync;
}

bool Terminal_session::handle_parser_actions(
    std::uint64_t                          sequence,
    const Terminal_screen_model_result&    result)
{
    bool render_snapshot_metadata_changed = false;

    for (const Parser_action& action : result.actions) {
        switch (parser_action_kind(action)) {
            case Parser_action_kind::NOTIFICATION:
                {
                    const Parser_notification& notification =
                        std::get<Parser_notification>(action.payload);
                    switch (notification.kind) {
                        case Parser_notification_kind::BELL_REQUESTED:
                            handle_bell_request(sequence);
                            break;
                        case Parser_notification_kind::TITLE_CHANGED:
                            record_notification({
                                Terminal_session_notification_kind::TITLE_CHANGED,
                                sequence,
                                notification.text,
                            });
                            break;
                        case Parser_notification_kind::ICON_NAME_CHANGED:
                            record_notification({
                                Terminal_session_notification_kind::ICON_NAME_CHANGED,
                                sequence,
                                notification.text,
                            });
                            break;
                        case Parser_notification_kind::TEXT_AREA_RESIZE_REQUESTED:
                            render_snapshot_metadata_changed =
                                apply_text_area_resize_request(
                                    sequence,
                                    terminal_grid_size_t{notification.rows, notification.columns}) ||
                                render_snapshot_metadata_changed;
                            record_notification({
                                Terminal_session_notification_kind::TEXT_AREA_RESIZE_REQUESTED,
                                sequence,
                                QStringLiteral("text-area resize requested"),
                                std::nullopt,
                                std::nullopt,
                                std::nullopt,
                                false,
                                std::nullopt,
                                terminal_grid_size_t{notification.rows, notification.columns},
                            });
                            break;
                        case Parser_notification_kind::OUTPUT_ACTIVITY:
                            break;
                    }
                    break;
            }
            case Parser_action_kind::TERMINAL_REPLY:
                {
                    const Terminal_reply& reply          = std::get<Terminal_reply>(action.payload);
                    const std::uint64_t   reply_sequence = next_sequence();
                    const Terminal_session_result enqueue_result = enqueue_command(
                        make_terminal_reply_command(reply_sequence, reply));
                    record_result(enqueue_result);
                    if (enqueue_result.code != Terminal_session_result_code::ACCEPTED &&
                        enqueue_result.error.has_value())
                    {
                        record_backend_error(reply_sequence, *enqueue_result.error);
                    }
                    break;
            }
            case Parser_action_kind::HOST_REQUEST:
                {
                    const Terminal_osc52_write_request& request = std::get<Terminal_osc52_write_request>(
                        action.payload);
                    record_notification({
                        Terminal_session_notification_kind::HOST_REQUEST,
                        sequence,
                        request.source_sequence,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        false,
                        request,
                    });
                    break;
            }
            case Parser_action_kind::SCREEN_MUTATION:
            case Parser_action_kind::STYLE_MUTATION:
            case Parser_action_kind::CONTROL_SEQUENCE:
            case Parser_action_kind::TERMINAL_QUERY:
            case Parser_action_kind::DIAGNOSTIC:
                break;
        }
    }

    return render_snapshot_metadata_changed;
}

void Terminal_session::handle_bell_request(std::uint64_t sequence)
{
    const Terminal_bell_request request =
        record_bell_event(m_bell_state, bell_clock_milliseconds());
    if (!request.audible && !request.visual) {
        return;
    }

    record_notification({
        Terminal_session_notification_kind::BELL_REQUESTED,
        sequence,
        QStringLiteral("bell requested"),
    });
    if (request.visual) {
        m_visual_bell_active = true;
    }
}

void Terminal_session::sync_viewport_from_model_result(
    const Terminal_screen_model_result& result)
{
    if (!m_screen_model.has_value()) {
        return;
    }

    if (result.alternate_scroll_mode_changed) {
        ++m_alternate_scroll_mode_generation;
    }

    m_viewport_controller.set_visible_rows(m_screen_model->grid_size().rows);
    m_viewport_controller.sync_scrollback_rows(
        result.scrollback_rows,
        result.evicted_scrollback_rows);
    if (m_selection.has_selection() &&
        m_selection_buffer_id == Terminal_buffer_id::PRIMARY)
    {
        m_selection.apply_scrollback_eviction(result.evicted_scrollback_rows);
    }
    const Terminal_buffer_id active_buffer = m_screen_model->active_buffer_id();
    const bool selection_crossed_buffers =
        m_selection.has_selection() && m_selection_buffer_id != active_buffer;
    if (selection_crossed_buffers) {
        m_selection.clear();
    }
    if (!m_selection.has_selection())                   { m_selection_buffer_id = active_buffer;          }
    if (active_buffer == Terminal_buffer_id::ALTERNATE) { m_viewport_controller.enter_alternate_screen(); }
    else {
        m_viewport_controller.leave_alternate_screen();
    }
}

bool Terminal_session::publish_viewport_snapshot_if_allowed(
    std::uint64_t  sequence,
    QString        message)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::publish_viewport_snapshot_if_allowed");

    if (!m_screen_model.has_value()) {
        return false;
    }

    if (!model_allows_render_snapshot(*m_screen_model)) {
        m_deferred_viewport_changed = true;
        return false;
    }

    Terminal_screen_model_result viewport_result;
    viewport_result.viewport_changed = true;
    m_render_snapshot_model_result = viewport_result;
    publish_render_snapshot(sequence, std::move(message));
    return true;
}

bool Terminal_session::return_viewport_to_tail_after_user_input(std::uint64_t sequence)
{
    if (!m_screen_model.has_value()) {
        return false;
    }

    const int previous_offset = m_viewport_controller.state().offset_from_tail;
    m_viewport_controller.notify_user_input();
    if (previous_offset == 0) {
        return false;
    }

    return publish_viewport_snapshot_if_allowed(
        sequence,
        QStringLiteral("viewport returned to tail"));
}

void Terminal_session::publish_selection_snapshot(
    std::uint64_t  sequence,
    QString        message)
{
    if (!m_screen_model.has_value()) {
        return;
    }

    if (!model_allows_render_snapshot(*m_screen_model)) {
        return;
    }

    Terminal_screen_model_result selection_result;
    m_render_snapshot_model_result = selection_result;
    publish_render_snapshot(sequence, std::move(message));
}

Terminal_render_snapshot_request Terminal_session::make_render_snapshot_request(
    std::uint64_t sequence) const
{
    Terminal_render_snapshot_request request;
    request.sequence                 = sequence;
    request.viewport                 = m_viewport_controller.state();
    request.backend_geometry_in_sync = m_backend_geometry_in_sync;
    request.visual_bell_active       = m_visual_bell_active;
    request.ime_preedit              = m_ime_preedit;
    if (m_render_snapshot_model_result.has_value()) {
        request.dirty_rows       = m_render_snapshot_model_result->dirty_rows;
        request.viewport_changed = m_render_snapshot_model_result->viewport_changed;
        request.mouse_reporting_mode_changed =
            m_render_snapshot_model_result->mouse_reporting_mode_changed;
    }
    request.viewport_changed = request.viewport_changed || m_deferred_viewport_changed;
    if (m_selection.has_selection() &&
        m_selection_buffer_id == m_screen_model->active_buffer_id())
    {
        request.selections.push_back(m_selection.range());
    }
    return request;
}

bool Terminal_session::selection_range_is_valid_for_active_model(
    const Terminal_selection_range& range) const
{
    Q_ASSERT(m_screen_model.has_value());

    const terminal_grid_size_t grid          = m_screen_model->grid_size();
    const Terminal_buffer_id   active_buffer = m_screen_model->active_buffer_id();

    const int max_row =
        active_buffer == Terminal_buffer_id::ALTERNATE
            ? grid.rows - 1
            : m_screen_model->scrollback_size() + grid.rows - 1;
    const auto position_is_valid = [grid, max_row](terminal_grid_position_t position) {
        return
            position.row    >= 0       &&
            position.row    <= max_row &&
            position.column >= 0       &&
            position.column <= grid.columns;
    };

    return
        range.mode != Terminal_selection_mode::NONE &&
        position_is_valid(range.start)              &&
        position_is_valid(range.end);
}

void Terminal_session::publish_render_snapshot(
    std::uint64_t  sequence,
    QString        message)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::publish_render_snapshot");

    Terminal_render_snapshot snapshot =
        m_screen_model->render_snapshot(make_render_snapshot_request(sequence));
    if (m_latest_render_snapshot            != nullptr &&
        m_render_snapshot_synced_generation <  m_render_snapshot_generation)
    {
        snapshot = snapshot_with_coalesced_dirty_rows(
            *m_latest_render_snapshot,
            std::move(snapshot));
    }
    std::shared_ptr<const Terminal_render_snapshot> snapshot_handle = std::make_shared<const Terminal_render_snapshot>(
        std::move(snapshot));
    m_latest_render_snapshot         = snapshot_handle;
    m_latest_content_render_snapshot = std::move(snapshot_handle);
    m_deferred_viewport_changed      = false;
    m_visual_bell_active             = false;
    ++m_render_snapshot_generation;
    record_notification({
        Terminal_session_notification_kind::SNAPSHOT_READY,
        sequence,
        std::move(message),
    });
}

void Terminal_session::publish_synchronized_resize_snapshot(
    std::uint64_t  sequence,
    QString        message)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_session::publish_synchronized_resize_snapshot");

    Terminal_render_snapshot snapshot =
        m_latest_content_render_snapshot != nullptr
            ? geometry_snapshot_from_public_snapshot(
                *m_latest_content_render_snapshot,
                m_grid_size,
                sequence,
                m_backend_geometry_in_sync)
            : make_empty_render_snapshot(
                m_grid_size,
                viewport_adapted_to_grid({}, m_grid_size),
                sequence);
    snapshot.metadata.backend_geometry_in_sync = m_backend_geometry_in_sync;
    snapshot.dirty_row_ranges = compact_dirty_row_ranges({}, m_grid_size.rows, true);
    m_latest_render_snapshot =
        std::make_shared<const Terminal_render_snapshot>(std::move(snapshot));
    ++m_render_snapshot_generation;
    record_notification({
        Terminal_session_notification_kind::SNAPSHOT_READY,
        sequence,
        std::move(message),
    });
}

void Terminal_session::advance_ime_preedit_generation()
{
    ++m_ime_preedit_generation;
}

void Terminal_session::record_result(Terminal_session_result result)
{
    if (m_result_capture_sequence != 0U && result.sequence == m_result_capture_sequence) {
        m_captured_result = result;
    }

    if (result.sequence == 0U || m_config.trace_result_limit == 0U) {
        return;
    }

    for (Terminal_session_result& existing : m_results) {
        if (existing.sequence == result.sequence) {
            existing = std::move(result);
            return;
        }
    }

    m_results.push_back(std::move(result));
    if (m_results.size() > m_config.trace_result_limit) {
        m_results.erase(m_results.begin());
    }
}

Terminal_session_result Terminal_session::result_for_sequence(
    std::uint64_t              sequence,
    Terminal_session_result    fallback) const
{
    for (const Terminal_session_result& result : m_results) {
        if (result.sequence == sequence) {
            return result;
        }
    }

    return fallback;
}

Terminal_session_result Terminal_session::result_after_processing(
    std::uint64_t              sequence,
    Terminal_session_result    enqueue_result) const
{
    Terminal_session_result result =
        (m_captured_result.has_value() && m_captured_result->sequence == sequence)
            ? *m_captured_result
            : result_for_sequence(sequence, enqueue_result);
    result.high_water_reached =
        result.high_water_reached || enqueue_result.high_water_reached;
    return result;
}

std::uint64_t Terminal_session::bell_clock_milliseconds() const
{
    if (m_config.bell_clock_ms) {
        return m_config.bell_clock_ms();
    }

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

void Terminal_session::begin_result_capture(std::uint64_t sequence)
{
    m_result_capture_sequence = sequence;
    m_captured_result.reset();
}

void Terminal_session::end_result_capture()
{
    m_result_capture_sequence = 0U;
    m_captured_result.reset();
}

Terminal_session_result Terminal_session::make_rejected_result(
    std::uint64_t                  sequence,
    Terminal_session_result_code   code,
    Terminal_backend_error         error) const
{
    return {
        code,
        sequence,
        false,
        std::move(error),
    };
}

std::uint64_t Terminal_session::next_sequence()
{
    const std::uint64_t sequence = m_next_sequence++;
    if (m_next_sequence == 0U) {
        m_next_sequence = 1U;
    }
    return sequence;
}

std::uint64_t Terminal_session::next_resize_id()
{
    const std::uint64_t id = m_next_resize_id++;
    if (m_next_resize_id == 0U) {
        m_next_resize_id = 1U;
    }
    return id;
}

Terminal_session::Queue_category Terminal_session::queue_category_for(
    Terminal_session_command_kind kind) const
{
    switch (kind) {
        case Terminal_session_command_kind::BACKEND_OUTPUT:
            return Queue_category::OUTPUT;
        case Terminal_session_command_kind::USER_WRITE:
        case Terminal_session_command_kind::USER_PASTE:
        case Terminal_session_command_kind::TERMINAL_REPLY:
            return Queue_category::WRITE;
        case Terminal_session_command_kind::START:
        case Terminal_session_command_kind::INTERRUPT:
        case Terminal_session_command_kind::TERMINATE:
        case Terminal_session_command_kind::FORCE_RELEASE_SYNCHRONIZED_OUTPUT:
        case Terminal_session_command_kind::BACKEND_EXIT:
        case Terminal_session_command_kind::BACKEND_ERROR:
        case Terminal_session_command_kind::RESIZE:
            return Queue_category::NONE;
    }

    return Queue_category::NONE;
}

Bounded_terminal_command_queue& Terminal_session::queue_for(Queue_category category)
{
    if (category == Queue_category::OUTPUT) {
        return m_output_queue;
    }

    return m_write_queue;
}

const Bounded_terminal_command_queue& Terminal_session::queue_for(
    Queue_category category) const
{
    if (category == Queue_category::OUTPUT) {
        return m_output_queue;
    }

    return m_write_queue;
}

bool Terminal_session::is_session_writable() const
{
    return
        m_backend != nullptr                               &&
        m_process_state == Terminal_process_state::RUNNING &&
        !m_stop_requested;
}

Terminal_queue_result Terminal_session::would_accept_command(
    Queue_category category,
    std::size_t    byte_count,
    std::size_t    command_count) const
{
    if (category == Queue_category::NONE) {
        return {Terminal_queue_result_code::ACCEPTED, false};
    }

    return queue_for(category).would_accept(byte_count, command_count);
}

void Terminal_session::add_to_queue_state(
    Queue_category category,
    std::size_t    byte_count)
{
    if (category == Queue_category::NONE) {
        return;
    }

    (void)queue_for(category).reserve(byte_count);
}

void Terminal_session::remove_from_queue_state(
    Queue_category category,
    std::size_t    byte_count)
{
    if (category == Queue_category::NONE) {
        return;
    }

    queue_for(category).release(byte_count);
}

bool Terminal_session::queue_high_water_reached(Queue_category category) const
{
    if (category == Queue_category::NONE) {
        return false;
    }

    return queue_for(category).high_water_reached();
}

void Terminal_session::set_output_backpressure_active(
    bool           active,
    std::uint64_t  sequence)
{
    if (m_output_backpressure_active == active) {
        return;
    }

    if (m_backend           != nullptr                           &&
        (m_process_state == Terminal_process_state::RUNNING ||
         m_process_state == Terminal_process_state::STARTING) &&
        !m_stop_requested)
    {
        const Terminal_backend_result pause_result = m_backend->set_output_paused(active);
        if (is_backend_rejection(pause_result)) {
            record_backend_error(sequence, *pause_result.error);
            return;
        }
    }

    m_output_backpressure_active = active;
    record_notification({
        Terminal_session_notification_kind::OUTPUT_BACKPRESSURE_CHANGED,
        sequence,
        active
            ? QStringLiteral("output backpressure active")
            : QStringLiteral("output backpressure released"),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        active,
    });
}

Terminal_session_result Terminal_session::handle_output_overflow(
    std::uint64_t  sequence,
    QString        message)
{
    Terminal_backend_error error = make_backend_error(
        Terminal_backend_error_code::OUTPUT_OVERFLOW,
        std::move(message));
    record_backend_error(sequence, error);
    terminate_after_output_overflow(sequence);
    return make_rejected_result(
        sequence,
        Terminal_session_result_code::QUEUE_HARD_LIMIT_REACHED,
        std::move(error));
}

void Terminal_session::terminate_after_output_overflow(std::uint64_t sequence)
{
    m_stop_requested = true;
    if (m_stop_requested_sequence == 0U || sequence < m_stop_requested_sequence) {
        m_stop_requested_sequence = sequence;
    }
    m_backend_ready = false;
    m_callback_lifetime->stop_backend_output();

    if (m_backend           == nullptr ||
        (m_process_state != Terminal_process_state::RUNNING &&
         m_process_state != Terminal_process_state::STARTING))
    {
        return;
    }

    const Terminal_backend_result terminate_result = m_backend->terminate();
    if (is_backend_rejection(terminate_result)) {
        record_backend_error(sequence, *terminate_result.error);
    }
}

bool Terminal_session::should_ignore_backend_output_after_stop(
    std::uint64_t sequence) const
{
    return m_stop_requested_sequence != 0U && sequence > m_stop_requested_sequence;
}

}
