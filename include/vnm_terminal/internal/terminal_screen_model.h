#pragma once

#include "vnm_terminal/internal/parser_action.h"
#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/utf8_scan.h"
#include "vnm_terminal/internal/terminal_input_mode.h"
#include "vnm_terminal/internal/terminal_style.h"
#include <QByteArray>
#include <QByteArrayView>
#include <QChar>
#include <QString>
#include <QStringView>
#include <QtGlobal>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <vector>

namespace vnm_terminal::internal {

constexpr int         k_terminal_screen_model_max_rows    = 4096;
constexpr int         k_terminal_screen_model_max_columns = 4096;
constexpr std::size_t k_terminal_screen_model_max_cells   = 1024U * 1024U;

inline bool is_terminal_screen_model_grid_size_supported(terminal_grid_size_t grid_size)
{
    if (grid_size.rows <= 0 || grid_size.columns <= 0) {
        return false;
    }

    if (grid_size.rows    > k_terminal_screen_model_max_rows ||
        grid_size.columns > k_terminal_screen_model_max_columns)
    {
        return false;
    }

    const std::size_t rows    = static_cast<std::size_t>(grid_size.rows);
    const std::size_t columns = static_cast<std::size_t>(grid_size.columns);
    return rows <= k_terminal_screen_model_max_cells / columns;
}

struct Terminal_screen_model_config
{
    terminal_grid_size_t   grid_size;
    int                    scrollback_limit                         = 1000;
    int                    tab_width                                = 8;
    bool                   recover_scrollback_from_primary_repaints = false;
    bool                   retain_structural_actions                = true;
};

enum class Terminal_screen_model_config_status
{
    OK,
    INVALID_GRID_SIZE,
    INVALID_SCROLLBACK_LIMIT,
    INVALID_TAB_WIDTH,
};

Terminal_screen_model_config_status validate_terminal_screen_model_config(
    const Terminal_screen_model_config&    config);

struct Terminal_screen_model_result
{
    std::vector<Parser_action> actions;
    std::vector<int>           dirty_rows;
    bool                       viewport_changed              = false;
    bool                       mode_state_changed            = false;
    bool                       mouse_reporting_mode_changed  = false;
    bool                       alternate_scroll_mode_changed = false;
    int                        scrollback_rows               = 0;
    int                        evicted_scrollback_rows       = 0;
};

struct Terminal_screen_model_dirty_row_stats
{
    bool                       enabled                     = false;
    std::uint64_t              mark_requests               = 0U;
    std::uint64_t              duplicate_mark_requests     = 0U;
    std::uint64_t              out_of_bounds_mark_requests = 0U;
    std::uint64_t              unique_pending_row_marks    = 0U;
    std::uint64_t              mark_all_dirty_calls        = 0U;
    std::uint64_t              dirty_rows_snapshot_calls   = 0U;
    std::uint64_t              dirty_rows_snapshot_rows    = 0U;
    std::uint64_t              collect_synchronized_calls  = 0U;
    std::uint64_t              collect_synchronized_rows   = 0U;
    std::uint64_t              publish_pending_calls       = 0U;
    std::uint64_t              published_unique_rows       = 0U;
    std::uint64_t              release_synchronized_calls  = 0U;
    std::uint64_t              released_synchronized_rows  = 0U;
    std::uint64_t              max_pending_dirty_rows      = 0U;
    std::uint64_t              max_synchronized_dirty_rows = 0U;
};

struct Terminal_screen_model_dirty_row_bucket_stats
{
    std::uint64_t              start_ms                    = 0U;
    std::uint64_t              end_ms                      = 0U;
    std::uint64_t              mark_requests               = 0U;
    std::uint64_t              duplicate_mark_requests     = 0U;
    std::uint64_t              out_of_bounds_mark_requests = 0U;
    std::uint64_t              unique_pending_row_marks    = 0U;
    std::uint64_t              mark_all_dirty_calls        = 0U;
    std::uint64_t              dirty_rows_snapshot_calls   = 0U;
    std::uint64_t              dirty_rows_snapshot_rows    = 0U;
    std::uint64_t              collect_synchronized_calls  = 0U;
    std::uint64_t              collect_synchronized_rows   = 0U;
    std::uint64_t              publish_pending_calls       = 0U;
    std::uint64_t              published_unique_rows       = 0U;
    std::uint64_t              release_synchronized_calls  = 0U;
    std::uint64_t              released_synchronized_rows  = 0U;
    std::uint64_t              max_pending_dirty_rows      = 0U;
    std::uint64_t              max_synchronized_dirty_rows = 0U;
};

struct Terminal_screen_model_dirty_row_timeline
{
    std::uint64_t              bucket_width_ms             = 100U;
    std::vector<Terminal_screen_model_dirty_row_bucket_stats>
                               buckets;
};

class Terminal_byte_stream_parser
{
public:
    std::vector<Parser_action> ingest(QByteArrayView bytes);

private:
    enum class String_state_result
    {
        NOT_STRING,
        CONSUMED,
    };

    std::vector<Parser_action> ingest_buffer(
        QByteArrayView                 bytes);

    String_state_result try_start_string(
        QByteArrayView                 bytes,
        qsizetype&                     offset,
        std::vector<Parser_action>&    actions);

    String_state_result try_consume_escape_or_csi(
        QByteArrayView                 bytes,
        qsizetype&                     offset,
        std::vector<Parser_action>&    actions);

    void continue_string(
        QByteArrayView                 bytes,
        qsizetype&                     offset,
        std::vector<Parser_action>&    actions);

    void start_string(
        Parser_sequence_family         family,
        QByteArrayView                 bytes,
        qsizetype                      payload_begin,
        qsizetype&                     offset,
        std::vector<Parser_action>&    actions);

    qsizetype find_string_terminator(
        QByteArrayView                 bytes,
        Parser_sequence_family         family,
        qsizetype                      payload_begin,
        Parser_string_terminator&      terminator);

    bool append_string_payload(
        Parser_sequence_family         family,
        QByteArrayView                 payload,
        std::vector<Parser_action>&    actions);

    void finish_string(
        Parser_sequence_family         family,
        Parser_string_terminator       terminator,
        std::vector<Parser_action>&    actions);

    void finish_csi_sequence(
        QByteArrayView                 bytes,
        qsizetype                      csi_begin,
        qsizetype                      final_offset,
        std::vector<Parser_action>&    actions);

    void handle_osc_payload(
        QByteArray                     payload,
        std::vector<Parser_action>&    actions);

    bool should_buffer_incomplete_utf8(
        QByteArrayView                 bytes,
        qsizetype                      offset) const;

    void emit_unsupported_control(
        QString                        source_sequence,
        Parser_sequence_family         family,
        std::vector<Parser_action>&    actions);

    void continue_discarded_csi(
        QByteArrayView                 bytes,
        qsizetype&                     offset);

    QByteArray                 m_pending_prefix;
    QByteArray                 m_string_payload;
    Parser_sequence_family     m_string_family                 = Parser_sequence_family::NONE;
    bool                       m_string_over_limit             = false;
    Terminal_utf8_scan_state   m_string_utf8_scan_state;
    bool                       m_discarding_csi                = false;
    std::uint64_t              m_next_host_request_id          = 1U;
};

class Terminal_screen_model
{
public:
    explicit Terminal_screen_model(Terminal_screen_model_config config);

    Terminal_screen_model_result ingest(QByteArrayView bytes);
    Terminal_screen_model_result resize(terminal_grid_size_t grid_size);
    Terminal_screen_model_result set_scrollback_limit(int limit);
    Terminal_screen_model_result force_release_synchronized_output();

    Terminal_render_snapshot render_snapshot(
        std::uint64_t                  sequence) const;

    Terminal_render_snapshot render_snapshot(
        const Terminal_render_snapshot_request&        request) const;

    Terminal_selection_result selected_text(
        const Terminal_selection_range&                selection) const;

    QString visible_text() const;
    QString row_text(int row) const;
    void apply_action(const Parser_action& action);

    terminal_grid_position_t cursor_position() const;
    terminal_grid_size_t grid_size() const;
    Terminal_buffer_id active_buffer_id() const;
    const Terminal_mode_state& mode_state() const;
    Terminal_input_mode_state input_mode_state() const;
    const QString& title() const;
    const QString& icon_name() const;
    int scrollback_size() const;
    void set_dirty_row_stats_enabled(bool enabled);
    Terminal_screen_model_dirty_row_stats dirty_row_stats() const;
    Terminal_screen_model_dirty_row_timeline dirty_row_timeline() const;

private:
    struct Cell
    {
        QString                        text              = QStringLiteral(" ");
        int                            display_width     = 1;
        bool                           wide_continuation = false;
        bool                           occupied          = false;
        Terminal_style_id              style_id          = k_default_terminal_style_id;
        std::uint64_t                  hyperlink_id      = 0U;
    };

    struct scrollback_row_t
    {
        std::vector<Cell>                       cells;
        std::map<std::uint64_t, QByteArray>     hyperlink_identity_keys;
    };

    struct saved_cursor_state_t
    {
        terminal_grid_position_t       position;
        Terminal_text_style            style;
        Terminal_style_id              style_id     = k_default_terminal_style_id;
        bool                           pending_wrap = false;
        bool                           origin_mode  = false;
        bool                           valid        = false;
    };

    struct screen_buffer_state_t
    {
        std::vector<std::vector<Cell>> cells;
        saved_cursor_state_t           saved_cursor;
        terminal_grid_position_t       cursor;
        int                            scroll_top     = 0;
        int                            scroll_bottom  = 0;
        bool                           origin_mode    = false;
        bool                           pending_wrap   = false;
    };

    struct ingest_publication_t
    {
        std::set<int>                  dirty_rows;
        bool                           viewport_changed              = false;
        bool                           mode_state_changed            = false;
        bool                           mouse_reporting_mode_changed  = false;
        bool                           alternate_scroll_mode_changed = false;
    };

    struct primary_repaint_recovery_candidate_t
    {
        std::vector<std::vector<Cell>> rows;
        int                            scrollback_rows         = 0;
        int                            unmatched_finish_budget = 0;
        bool                           active                  = false;
    };

    screen_buffer_state_t make_empty_buffer_state() const;
    screen_buffer_state_t capture_current_buffer_state() const;
    void restore_buffer_state(const screen_buffer_state_t& state);
    void save_active_buffer_state();
    screen_buffer_state_t& active_stored_buffer();
    const screen_buffer_state_t& active_stored_buffer() const;
    void resize_buffer_state(screen_buffer_state_t& state, terminal_grid_size_t grid_size) const;
    void resize_cells(std::vector<std::vector<Cell>>& cells, terminal_grid_size_t grid_size) const;
    std::vector<bool> default_tab_stops(int column_count) const;
    void reset_grid();

    void apply_action(
        const Parser_action&           action,
        std::vector<Parser_action>&    generated_actions,
        ingest_publication_t*          publication);

    void apply_control_sequence(
        const Parser_control_sequence& sequence,
        std::vector<Parser_action>&    generated_actions,
        ingest_publication_t*          publication);

    void apply_sgr_sequence(
        const Terminal_sgr_sequence&   sequence);

    void apply_sgr_operation(
        const Terminal_sgr_operation&  operation);

    Terminal_style_id intern_style(
        const Terminal_text_style&     style);

    Parser_action make_color_query_reply(
        const Terminal_color_query&    query) const;

    Parser_action make_unsupported_control_diagnostic(
        const Parser_control_sequence& sequence) const;

    Parser_action make_private_mode_diagnostic(
        int                            mode,
        const Parser_control_sequence& sequence) const;

    bool apply_grid_resize(
        terminal_grid_size_t           grid_size);

    void reset_scroll_region();
    void reset_tab_stops();

    void put_scalar(
        QString                        text);

    void put_text(
        QString                        text);

    void put_printable_ascii_text(
        QStringView                    text);

    void write_printable_ascii_span(
        int                            row,
        int                            first_column,
        QStringView                    text);

    void write_printable_ascii_cell(
        terminal_grid_position_t       position,
        QChar                          text);

    void put_spacing_scalar(
        QString                        text,
        int                            display_width);

    void append_zero_width_scalar(
        QString                        text);

    void install_cell_span(
        terminal_grid_position_t       position,
        QString                        text,
        int                            display_width,
        Terminal_style_id              style_id,
        std::uint64_t                  hyperlink_id);

    void place_cell_text(
        terminal_grid_position_t       position,
        QString                        text,
        int                            display_width);

    void clear_cell_span(
        terminal_grid_position_t       position);

    void clear_cell_at(
        terminal_grid_position_t       position);

    Cell erased_cell() const;
    void fill_row_with_erased_cells(std::vector<Cell>& row) const;
    void erase_cell_at(terminal_grid_position_t position);
    void erase_row_range(int row, int first_column, int last_column);
    void clear_screen_before_cursor();
    void clear_screen_after_cursor();
    void erase_visible_screen();

    void erase_in_display(
        int                            mode);

    void erase_in_line(
        int                            mode);

    void erase_characters(
        int                            count);

    void insert_cells(
        int                            count);

    void delete_cells(
        int                            count);

    void insert_lines(
        int                            count);

    void delete_lines(
        int                            count);

    terminal_grid_position_t cell_base_position(
        terminal_grid_position_t       position) const;

    void set_cursor_after_cell(
        terminal_grid_position_t       position,
        int                            display_width);

    void set_cursor_position(
        int                            row,
        int                            column);

    void set_cursor_address(
        int                            row_parameter,
        int                            column_parameter);

    void move_cursor_relative(
        int                            row_delta,
        int                            column_delta);

    void set_scroll_region(
        int                            top_parameter,
        int                            bottom_parameter);

    void set_origin_mode(
        bool                           enabled);

    void set_autowrap_mode(
        bool                           enabled);

    void set_application_keypad_mode(
        bool                           enabled);

    void set_hyperlink(
        QByteArray                     identity_key);

    void set_synchronized_output_mode(
        bool                           enabled,
        ingest_publication_t*          publication);

    void apply_dec_private_mode(
        int                            mode,
        bool                           enabled,
        std::vector<Parser_action>&    generated_actions,
        const Parser_control_sequence& sequence,
        ingest_publication_t*          publication);

    void apply_mouse_tracking_mode(
        Terminal_mouse_tracking_mode   target,
        bool                           enabled);

    int dec_private_mode_status(
        int                            mode) const;

    void enter_alternate_screen(
        bool                           clear_alternate,
        int                            active_mode);

    bool leave_alternate_screen(
        bool                           clear_alternate);

    void save_cursor();
    void restore_cursor();
    void clear_current_tab_stop();
    void clear_all_tab_stops();
    void append_scrollback_row(const std::vector<Cell>& cells);
    void scroll_up_region(int top, int bottom, bool append_scrollback, int count = 1);
    void scroll_down_region(int top, int bottom, int count = 1);
    void reverse_index();
    void begin_primary_repaint_recovery_candidate();
    void finish_primary_repaint_recovery_candidate(bool discard_if_no_match);
    void cancel_primary_repaint_recovery_candidate();

    int inferred_primary_repaint_scroll_rows(
        const primary_repaint_recovery_candidate_t&    candidate) const;

    bool rows_have_matching_text(
        const std::vector<Cell>&       left,
        const std::vector<Cell>&       right) const;

    bool row_has_visible_text(
        const std::vector<Cell>&       row) const;

    void carriage_return();
    void line_feed();
    void backspace();
    void horizontal_tab();
    void mark_cursor_dirty();
    void mark_dirty(int row);
    void mark_viewport_changed();
    void mark_mode_state_changed();
    void mark_mouse_reporting_mode_changed();
    void mark_alternate_scroll_mode_changed();
    void mark_all_dirty();
    void repair_wide_spans_in_row(std::vector<Cell>& row, int column_count) const;
    void clear_dirty();
    Terminal_screen_model_dirty_row_bucket_stats& dirty_row_stats_bucket() const;
    void update_pending_dirty_row_stats_watermark();
    void update_synchronized_dirty_row_stats_watermark();
    std::vector<int> dirty_rows() const;
    void collect_synchronized_changes();
    void publish_pending_changes(ingest_publication_t& publication);
    void release_synchronized_changes(ingest_publication_t& publication);
    std::uint64_t next_hyperlink_id();
    std::uint64_t hyperlink_id_for_identity(const QByteArray& identity_key);
    void retain_referenced_hyperlink_ids();

    scrollback_row_t make_scrollback_row(
        const std::vector<Cell>&       cells) const;

    void add_scrollback_hyperlink_refs(
        const scrollback_row_t&        row);

    void remove_scrollback_hyperlink_refs(
        const scrollback_row_t&        row);

    void rebuild_scrollback_row_hyperlinks(
        scrollback_row_t&              row) const;

    void resize_scrollback_rows(
        int                            column_count);

    std::vector<int> viewport_dirty_rows(
        const Terminal_viewport_state& viewport,
        const std::vector<int>&        dirty_rows) const;

    void append_snapshot_cells_from_row(
        Terminal_render_snapshot&      snapshot,
        const std::vector<Cell>&       row,
        int                            snapshot_row) const;

    QString row_text_from_cells(
        const std::vector<Cell>&       row,
        int                            first_column,
        int                            end_column) const;

    const std::vector<Cell>* logical_row_cells(
        Terminal_buffer_id             buffer_id,
        int                            logical_row) const;

    const std::vector<Cell>* viewport_row_cells(
        const Terminal_viewport_state& viewport,
        int                            viewport_row) const;

    std::vector<Terminal_render_hyperlink_metadata> hyperlink_metadata_for_cells(
        const std::vector<Terminal_render_cell>&       cells) const;

    Terminal_screen_model_config    m_config;
    Terminal_byte_stream_parser     m_parser;
    Terminal_color_state            m_color_state;
    Terminal_text_style             m_current_style = make_default_terminal_text_style();
    Terminal_style_id               m_current_style_id = k_default_terminal_style_id;
    std::vector<Terminal_text_style>
                                    m_styles;
    screen_buffer_state_t           m_primary_buffer;
    screen_buffer_state_t           m_alternate_buffer;
    std::vector<std::vector<Cell>>  m_cells;
    std::deque<scrollback_row_t>    m_scrollback;
    std::set<int>                   m_dirty_rows;
    int                             m_last_dirty_row = -1;
    std::vector<bool>               m_tab_stops;
    saved_cursor_state_t            m_saved_cursor;
    terminal_grid_position_t        m_cursor;
    QString                         m_title;
    QString                         m_icon_name;
    Terminal_buffer_id              m_active_buffer_id = Terminal_buffer_id::PRIMARY;
    Terminal_mode_state             m_modes;
    // DECPAM/DECPNM/DECNKM affect input encoding only, so they stay outside
    // the render snapshot mode state and do not invalidate render output.
    bool                            m_application_keypad = false;
    int                             m_active_alternate_mode = 0;
    std::uint64_t                   m_current_hyperlink_id = 0U;
    std::uint64_t                   m_next_hyperlink_id = 1U;
    std::map<QByteArray, std::uint64_t>
                                    m_hyperlink_ids;
    std::map<std::uint64_t, QByteArray>
                                    m_scrollback_hyperlink_identity_keys;
    std::map<std::uint64_t, int>    m_scrollback_hyperlink_ref_counts;
    int                             m_scroll_top = 0;
    int                             m_scroll_bottom = 0;
    bool                            m_origin_mode = false;
    bool                            m_dec_1049_saved_primary_cursor = false;
    bool                            m_pending_wrap = false;
    bool                            m_viewport_changed = false;
    bool                            m_mode_state_changed = false;
    bool                            m_mouse_reporting_mode_changed = false;
    bool                            m_alternate_scroll_mode_changed = false;
    std::set<int>                   m_synchronized_dirty_rows;
    mutable Terminal_screen_model_dirty_row_stats
                                    m_dirty_row_stats;
    mutable Terminal_screen_model_dirty_row_timeline
                                    m_dirty_row_timeline;
    mutable std::chrono::steady_clock::time_point
                                    m_dirty_row_stats_start_time;
    bool                            m_synchronized_viewport_changed = false;
    bool                            m_synchronized_mode_state_changed = false;
    bool                            m_synchronized_mouse_reporting_mode_changed = false;
    bool                            m_synchronized_alternate_scroll_mode_changed = false;
    int                             m_scrollback_evicted_rows = 0;
    primary_repaint_recovery_candidate_t
                                    m_primary_repaint_recovery_candidate;
};

}
