#pragma once

#include "vnm_terminal/internal/session_contract.h"
#include "vnm_terminal/internal/selection_contract.h"
#include "vnm_terminal/internal/terminal_input_encoder.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "vnm_terminal/internal/utf8_scan.h"
#include <QByteArray>
#include <QSizeF>
#include <QString>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <vector>

class QKeyEvent;

namespace vnm_terminal::internal {

class Terminal_session_callback_lifetime;

struct Terminal_input_event_result
{
    bool                       handled = false;
    Terminal_session_result    result;
};

using Terminal_key_event_result   = Terminal_input_event_result;
using Terminal_mouse_event_result = Terminal_input_event_result;
using Terminal_ime_commit_result  = Terminal_input_event_result;
using Terminal_paste_text_result  = Terminal_input_event_result;
using Terminal_focus_event_result = Terminal_input_event_result;

class Terminal_session
{
public:
    Terminal_session(
        std::unique_ptr<Terminal_backend>      backend,
        Terminal_session_config                config = {});

    ~Terminal_session();

    Terminal_session(const Terminal_session&)            = delete;
    Terminal_session& operator=(const Terminal_session&) = delete;

    Terminal_session_result start(
        Terminal_launch_config     launch_config);

    Terminal_session_result write_user_bytes(
        QByteArray                 bytes);

    Terminal_key_event_result write_key_event(
        const QKeyEvent&           event);

    Terminal_mouse_event_result write_mouse_event(
        Terminal_mouse_event       event);

    Terminal_ime_commit_result write_ime_commit(
        QString                    text);

    Terminal_paste_text_result write_paste_text(
        QString                                text,
        Terminal_paste_framing_policy          policy);

    Terminal_focus_event_result write_focus_event(
        bool                       focused);

    void set_ime_preedit(
        QString                    text,
        int                        cursor_position);

    void cancel_ime_preedit();

    Terminal_session_result write_terminal_reply(
        const Terminal_reply&      reply);

    Terminal_session_result resize(
        QSizeF                     source_geometry,
        terminal_grid_size_t       grid_size);

    Terminal_viewport_scroll_result scroll_viewport_lines(
        int                        line_delta);

    Terminal_viewport_scroll_result scroll_published_viewport_lines(
        int                        line_delta);

    Terminal_viewport_scroll_result scroll_published_viewport_to_offset_from_tail(
        int                        offset_from_tail);

    void set_selection_range(
        Terminal_selection_range   range);

    void clear_selection();
    void set_scrollback_limit(int limit);
    Terminal_session_result interrupt();
    Terminal_session_result terminate();
    Terminal_session_result force_release_synchronized_output();

    Terminal_process_state process_state() const;
    bool backend_ready() const;
    bool backend_geometry_in_sync() const;
    bool output_backpressure_active() const;
    bool render_publication_blocked() const;
    bool mouse_reporting_active() const;
    bool alternate_scroll_active() const;
    std::uint64_t alternate_scroll_mode_generation() const;
    Terminal_viewport_state viewport_state() const;
    terminal_grid_size_t grid_size() const;
    std::uint64_t last_processed_sequence() const;
    bool has_selection() const;
    Terminal_selection_result selected_text() const;

    std::vector<Terminal_session_command> processed_commands() const;
    std::vector<Terminal_session_notification> notifications() const;
    /**
     * @brief Drain public session notifications since the previous call.
     *
     * notifications() remains a bounded diagnostic trace; callers that deliver
     * public events must use this drainable channel. The channel coalesces
     * high-frequency state notifications and keeps a bounded critical-event
     * queue so trace truncation does not drive public signal delivery.
     */
    std::vector<Terminal_session_notification> take_pending_notifications();
    std::vector<Terminal_resize_transaction> resize_transactions() const;
    std::vector<QByteArray> output_chunks() const;
    std::optional<Terminal_render_snapshot> latest_render_snapshot() const;
    std::shared_ptr<const Terminal_render_snapshot> latest_render_snapshot_handle() const;
    std::uint64_t render_snapshot_generation() const;
    void mark_render_snapshot_synced(std::uint64_t generation);
    Ime_preedit_state ime_preedit_state() const;
    std::uint64_t ime_preedit_generation() const;
    std::optional<Terminal_screen_model_result> last_model_ingest_result() const;
    void set_dirty_row_stats_enabled(bool enabled);
    Terminal_screen_model_dirty_row_stats dirty_row_stats() const;
    Terminal_screen_model_dirty_row_timeline dirty_row_timeline() const;
    std::optional<Terminal_backend_exit> exit_status() const;

    /**
     * @brief Drain backend callbacks that were posted through Terminal_backend_callbacks.
     *
     * When Terminal_session_config::backend_event_notifier is set, backend callback
     * threads only enqueue into the bounded callback ingress and invoke the notifier.
     * The owner must then call this method on the session owner thread before reading
     * session state or taking pending notifications. Without a notifier the callback
     * path calls this method inline, preserving the synchronous test/backend contract.
     */
    void process_backend_callback_events();

private:
    enum class Queue_category
    {
        NONE,
        OUTPUT,
        WRITE,
    };

    enum class User_write_viewport_policy
    {
        RETURN_TO_TAIL,
        PRESERVE_VIEWPORT,
    };

    Terminal_session_result enqueue_command(
        Terminal_session_command   command);

    Terminal_session_result enqueue_and_process_synchronous_command(
        Terminal_session_command   command);

    void process_pending_commands();

    Terminal_session_result process_command(
        Terminal_session_command   command);

    Terminal_session_result process_start_command(
        const Terminal_session_command&        command);

    Terminal_session_result process_write_command(
        const Terminal_session_command&        command);

    Terminal_session_result process_resize_command(
        Terminal_session_command   command);

    Terminal_session_result process_interrupt_command(
        const Terminal_session_command&        command);

    Terminal_session_result process_terminate_command(
        const Terminal_session_command&        command);

    Terminal_session_result process_force_release_synchronized_output_command(
        const Terminal_session_command&        command);

    Terminal_session_result process_backend_output_command(
        const Terminal_session_command&        command);

    Terminal_session_result process_backend_exit_command(
        const Terminal_session_command&        command);

    Terminal_session_result process_backend_error_command(
        const Terminal_session_command&        command);

    Terminal_session_result write_user_bytes_locked(
        QByteArray                 bytes,
        User_write_viewport_policy viewport_policy);

    Terminal_backend_callbacks make_backend_callbacks();
    void drain_backend_callback_commands();
    void pause_backend_output_from_callback_ingress();

    void record_processed_command(
        Terminal_session_command   command);

    void record_notification(
        Terminal_session_notification          notification);

    void record_pending_notification(
        Terminal_session_notification          notification);

    void record_resize_transaction(
        Terminal_resize_transaction            resize);

    void record_backend_output_capture_chunk(
        QByteArrayView             bytes);

    void record_output_chunk(
        QByteArray                 bytes);

    void record_output_activity(
        std::uint64_t              sequence);

    void record_backend_error(
        std::uint64_t              sequence,
        Terminal_backend_error     error);

    void initialize_screen_model(
        terminal_grid_size_t       grid_size);

    void ingest_backend_output_segment(
        std::uint64_t              sequence,
        QByteArrayView             bytes);

    bool handle_parser_actions(
        std::uint64_t                          sequence,
        const Terminal_screen_model_result&    result);

    bool apply_text_area_resize_request(
        std::uint64_t              sequence,
        terminal_grid_size_t       grid_size);

    void handle_bell_request(
        std::uint64_t              sequence);

    void sync_viewport_from_model_result(
        const Terminal_screen_model_result&    result);

    bool publish_viewport_snapshot_if_allowed(
        std::uint64_t              sequence,
        QString                    message);

    bool return_viewport_to_tail_after_user_input(
        std::uint64_t              sequence);

    void publish_selection_snapshot(
        std::uint64_t              sequence,
        QString                    message);

    Terminal_render_snapshot_request make_render_snapshot_request(
        std::uint64_t              sequence) const;

    void publish_render_snapshot(
        std::uint64_t              sequence,
        QString                    message);

    void publish_synchronized_resize_snapshot(
        std::uint64_t              sequence,
        QString                    message);

    bool selection_range_is_valid_for_active_model(
        const Terminal_selection_range&        range) const;

    void advance_ime_preedit_generation();

    void record_result(
        Terminal_session_result    result);

    Terminal_session_result result_for_sequence(
        std::uint64_t              sequence,
        Terminal_session_result    fallback) const;

    Terminal_session_result result_after_processing(
        std::uint64_t              sequence,
        Terminal_session_result    enqueue_result) const;

    std::uint64_t bell_clock_milliseconds() const;
    void begin_result_capture(std::uint64_t sequence);
    void end_result_capture();

    Terminal_session_result make_rejected_result(
        std::uint64_t                          sequence,
        Terminal_session_result_code           code,
        Terminal_backend_error                 error) const;

    std::uint64_t next_sequence();
    std::uint64_t next_resize_id();
    Queue_category queue_category_for(Terminal_session_command_kind kind) const;
    Bounded_terminal_command_queue& queue_for(Queue_category category);
    const Bounded_terminal_command_queue& queue_for(Queue_category category) const;
    bool is_session_writable() const;

    Terminal_queue_result would_accept_command(
        Queue_category             category,
        std::size_t                byte_count,
        std::size_t                command_count) const;

    void add_to_queue_state(
        Queue_category             category,
        std::size_t                byte_count);

    void remove_from_queue_state(
        Queue_category             category,
        std::size_t                byte_count);

    bool queue_high_water_reached(
        Queue_category             category) const;

    void set_output_backpressure_active(
        bool                       active,
        std::uint64_t              sequence);

    Terminal_session_result handle_output_overflow(
        std::uint64_t              sequence,
        QString                    message);

    void terminate_after_output_overflow(
        std::uint64_t              sequence);

    bool should_ignore_backend_output_after_stop(
        std::uint64_t              sequence) const;

    mutable std::recursive_mutex                           m_mutex;
    std::shared_ptr<Terminal_session_callback_lifetime>    m_callback_lifetime;

    std::unique_ptr<Terminal_backend>                      m_backend;
    Terminal_session_config                                m_config;
    std::deque<Terminal_session_command>                   m_pending_commands;
    Bounded_terminal_command_queue                         m_output_queue;
    Bounded_terminal_command_queue                         m_write_queue;
    std::vector<Terminal_session_command>                  m_processed_commands;
    std::vector<Terminal_session_notification>             m_notifications;
    std::vector<Terminal_session_notification>             m_pending_notifications;
    std::vector<Terminal_session_result>                   m_results;
    std::vector<Terminal_resize_transaction>               m_resize_transactions;
    std::vector<QByteArray>                                m_output_chunks;
    QByteArray                                             m_backend_output_prescan_pending;
    Terminal_utf8_scan_state                               m_backend_output_prescan_utf8_state;
    std::mutex                                             m_backend_output_capture_mutex;
    std::optional<Terminal_screen_model>                   m_screen_model;
    std::shared_ptr<const Terminal_render_snapshot>        m_latest_render_snapshot;
    std::shared_ptr<const Terminal_render_snapshot>        m_latest_content_render_snapshot;
    std::optional<Terminal_screen_model_result>            m_last_model_ingest_result;
    std::optional<Terminal_screen_model_result>            m_render_snapshot_model_result;
    std::optional<Terminal_backend_exit>                   m_exit_status;
    Ime_preedit_state                                      m_ime_preedit;
    Terminal_process_state                     m_process_state =
        Terminal_process_state::NOT_STARTED;
    terminal_grid_size_t                                   m_grid_size;
    std::uint64_t                                          m_next_sequence = 1U;
    std::uint64_t                                          m_next_resize_id = 1U;
    std::uint64_t                                          m_last_processed_sequence = 0U;
    std::uint64_t                                          m_render_snapshot_generation = 0U;
    std::uint64_t                                          m_render_snapshot_synced_generation = 0U;
    std::uint64_t                                          m_ime_preedit_generation = 0U;
    std::uint64_t                                          m_alternate_scroll_mode_generation = 0U;
    bool                                                   m_backend_ready = false;
    bool                                                   m_backend_geometry_in_sync = false;
    bool                                                   m_output_backpressure_active = false;
    bool                                                   m_processing_commands = false;
    bool                                                   m_backend_error_queued_during_command = false;
    bool                                                   m_stop_requested = false;
    std::uint64_t                                          m_stop_requested_sequence = 0U;
    std::uint64_t                                          m_result_capture_sequence = 0U;
    std::optional<Terminal_session_result>                 m_captured_result;
    Terminal_viewport_controller                           m_viewport_controller;
    Terminal_bell_state                                    m_bell_state;
    Selection_contract_controller                          m_selection;
    Terminal_buffer_id                         m_selection_buffer_id =
        Terminal_buffer_id::PRIMARY;
    bool                                                   m_deferred_viewport_changed = false;
    bool                                                   m_visual_bell_active        = false;
};

}
