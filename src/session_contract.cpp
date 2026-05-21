#include "vnm_terminal/internal/session_contract.h"

#include <algorithm>
#include <utility>

namespace vnm_terminal::internal {

namespace {

bool would_exceed_limit(std::size_t current, std::size_t added, std::size_t limit)
{
    return added > limit || current > limit - added;
}

bool would_reach_limit(std::size_t current, std::size_t added, std::size_t limit)
{
    if (current >= limit) {
        return true;
    }

    return added >= limit - current;
}

}

Terminal_queue_result Bounded_terminal_command_queue::enqueue(Terminal_session_command command)
{
    const std::size_t           byte_count  = static_cast<std::size_t>(command.bytes.size());
    const Terminal_queue_result reservation = reserve(byte_count);
    if (reservation.code != Terminal_queue_result_code::ACCEPTED) {
        return reservation;
    }

    m_commands.push_back(std::move(command));
    return {Terminal_queue_result_code::ACCEPTED, high_water_reached()};
}

std::optional<Terminal_session_command> Bounded_terminal_command_queue::dequeue()
{
    if (m_commands.empty()) {
        return std::nullopt;
    }

    Terminal_session_command command = std::move(m_commands.front());
    m_commands.pop_front();
    release(static_cast<std::size_t>(command.bytes.size()));
    return command;
}

Terminal_queue_result Bounded_terminal_command_queue::would_accept(
    std::size_t    byte_count,
    std::size_t    command_count) const
{
    if (would_exceed_limit(m_command_count, command_count, m_limits.hard_limit_commands)) {
        return {Terminal_queue_result_code::HARD_LIMIT_REACHED, high_water_reached()};
    }

    if (would_exceed_limit(m_bytes, byte_count, m_limits.hard_limit_bytes)) {
        return {Terminal_queue_result_code::HARD_LIMIT_REACHED, high_water_reached()};
    }

    return {
        Terminal_queue_result_code::ACCEPTED,
        would_reach_limit(m_bytes, byte_count, m_limits.high_water_bytes) ||
            would_reach_limit(
                m_command_count, command_count, m_limits.high_water_commands),
    };
}

Terminal_queue_result Bounded_terminal_command_queue::reserve(
    std::size_t    byte_count,
    std::size_t    command_count)
{
    const Terminal_queue_result result = would_accept(byte_count, command_count);
    if (result.code != Terminal_queue_result_code::ACCEPTED) {
        return result;
    }

    m_bytes += byte_count;
    m_command_count += command_count;
    return {Terminal_queue_result_code::ACCEPTED, high_water_reached()};
}

void Bounded_terminal_command_queue::release(
    std::size_t    byte_count,
    std::size_t    command_count)
{
    m_bytes         -= std::min(m_bytes, byte_count);
    m_command_count -= std::min(m_command_count, command_count);
}

bool Bounded_terminal_command_queue::high_water_reached() const
{
    return
        m_bytes         >= m_limits.high_water_bytes ||
        m_command_count >= m_limits.high_water_commands;
}

Terminal_bell_request record_bell_event(
    Terminal_bell_state&   state,
    std::uint64_t          timestamp_ms)
{
    if (state.policy.max_events_per_window == 0U) {
        return {};
    }

    if (state.policy.coalescing_window_ms == 0U) {
        state.window_start_ms  = timestamp_ms;
        state.events_in_window = 1U;
        return {state.policy.audible_enabled, state.policy.visual_enabled};
    }

    if (state.events_in_window               == 0U ||
        timestamp_ms - state.window_start_ms >= state.policy.coalescing_window_ms)
    {
        state.window_start_ms  = timestamp_ms;
        state.events_in_window = 0U;
    }

    if (state.events_in_window >= state.policy.max_events_per_window) {
        return {};
    }

    ++state.events_in_window;
    return {state.policy.audible_enabled, state.policy.visual_enabled};
}

Terminal_session_command make_terminal_reply_command(
    std::uint64_t          sequence,
    const Terminal_reply&  reply)
{
    return {.sequence = sequence,
            .kind  = Terminal_session_command_kind::TERMINAL_REPLY,
            .bytes = reply.wire_bytes};
}

Terminal_session_command make_start_command(
    std::uint64_t          sequence,
    Terminal_launch_config launch_config)
{
    return {.sequence      = sequence,
            .kind          = Terminal_session_command_kind::START,
            .launch_config = std::move(launch_config)};
}

Terminal_session_command make_interrupt_command(std::uint64_t sequence)
{
    return {.sequence = sequence,
            .kind     = Terminal_session_command_kind::INTERRUPT};
}

Terminal_session_command make_terminate_command(std::uint64_t sequence)
{
    return {.sequence = sequence,
            .kind     = Terminal_session_command_kind::TERMINATE};
}

Terminal_session_command make_force_release_synchronized_output_command(
    std::uint64_t sequence)
{
    return {.sequence = sequence,
            .kind     = Terminal_session_command_kind::FORCE_RELEASE_SYNCHRONIZED_OUTPUT};
}

Terminal_session_command make_backend_exit_command(
    std::uint64_t          sequence,
    Terminal_backend_exit  exit)
{
    return {.sequence = sequence,
            .kind = Terminal_session_command_kind::BACKEND_EXIT,
            .exit = exit};
}

Terminal_session_command make_backend_error_command(
    std::uint64_t          sequence,
    Terminal_backend_error error)
{
    return {.sequence = sequence,
            .kind  = Terminal_session_command_kind::BACKEND_ERROR,
            .error = std::move(error)};
}

Terminal_session_command make_backend_output_command(
    std::uint64_t          sequence,
    QByteArray             bytes)
{
    return {.sequence = sequence,
            .kind  = Terminal_session_command_kind::BACKEND_OUTPUT,
            .bytes = std::move(bytes)};
}

Terminal_session_command make_user_write_command(
    std::uint64_t          sequence,
    QByteArray             bytes)
{
    return {.sequence = sequence,
            .kind  = Terminal_session_command_kind::USER_WRITE,
            .bytes = std::move(bytes)};
}

Terminal_session_command make_user_paste_command(
    std::uint64_t          sequence,
    QByteArray             bytes)
{
    return {.sequence = sequence,
            .kind  = Terminal_session_command_kind::USER_PASTE,
            .bytes = std::move(bytes)};
}

Terminal_session_command make_resize_command(
    std::uint64_t                  sequence,
    Terminal_resize_transaction    resize)
{
    return {.sequence = sequence,
            .kind   = Terminal_session_command_kind::RESIZE,
            .resize = std::move(resize)};
}

}
