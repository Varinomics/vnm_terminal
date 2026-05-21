#pragma once

#include "vnm_terminal/internal/backend_contract.h"
#include <QByteArray>
#include <QString>
#include <QStringView>
#include <cstddef>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace vnm_terminal::internal {

constexpr std::size_t k_native_backend_output_read_chunk_bytes =   16U * 1024U;
constexpr std::size_t k_native_backend_max_queued_write_bytes  = 1024U * 1024U;

struct Native_backend_start_gate
{
    std::mutex& mutex;
    bool&       running;
    bool&       start_attempted;
    bool&       start_in_progress;
};

struct Native_backend_start_precheck
{
    Terminal_backend_result                         result;
    std::optional<Terminal_effective_launch_config> effective_config;
};

Native_backend_start_precheck validate_native_backend_start_preconditions(
    const Terminal_launch_config&      config,
    const Terminal_backend_callbacks&  callbacks,
    Native_backend_start_gate          start_gate,
    QStringView                        backend_label);

void clear_native_backend_start_in_progress(
    Native_backend_start_gate          start_gate);

Terminal_backend_result reject_native_backend_start_attempt(
    const Terminal_backend_callbacks&  callbacks,
    Native_backend_start_gate          start_gate,
    Terminal_backend_error_code        code,
    QString                            message);

Terminal_backend_result reject_native_backend_start_with_report(
    const Terminal_backend_callbacks&  callbacks,
    Terminal_backend_error_code        code,
    QString                            message);

bool native_backend_write_queue_can_accept(
    std::size_t                        queued_write_bytes,
    std::size_t                        incoming_write_bytes);

void add_native_backend_queued_write_bytes(
    std::size_t&                       queued_write_bytes,
    std::size_t                        incoming_write_bytes);

void remove_native_backend_queued_write_bytes(
    std::size_t&                       queued_write_bytes,
    std::size_t                        completed_write_bytes);

void append_native_backend_paused_output(
    QByteArray&                        paused_output,
    QByteArray                         bytes);

void report_native_backend_error(
    const Terminal_backend_callbacks&  callbacks,
    Terminal_backend_error_code        code,
    QString                            message);

void deliver_native_backend_output(
    const Terminal_backend_callbacks&  callbacks,
    QByteArray                         bytes);

template <typename Can_buffer_paused_output_fn>
void deliver_or_buffer_native_backend_output(
    std::mutex&                        mutex,
    Terminal_backend_callbacks&        callbacks,
    QByteArray&                        paused_output,
    bool&                              output_paused,
    QByteArray                         bytes,
    Can_buffer_paused_output_fn&&      can_buffer_paused_output)
{
    bool should_deliver = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (output_paused && can_buffer_paused_output()) {
            append_native_backend_paused_output(paused_output, std::move(bytes));
        }
        else {
            should_deliver = true;
        }
    }

    if (!should_deliver) {
        return;
    }

    Terminal_backend_callbacks callback_snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex);
        callback_snapshot = callbacks;
    }

    deliver_native_backend_output(callback_snapshot, std::move(bytes));
}

void report_native_backend_exit(
    const Terminal_backend_callbacks&  callbacks,
    Terminal_exit_reason               reason,
    int                                exit_code);

void join_or_detach_native_backend_thread(
    std::thread&                       thread);

}
