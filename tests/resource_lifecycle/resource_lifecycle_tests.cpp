#include "vnm_terminal/internal/terminal_session.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QSizeF>
#include <QString>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

constexpr std::chrono::milliseconds k_lifecycle_wait_timeout{5000};

using vnm_terminal::test_helpers::check;

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return predicate();
}

term::Terminal_launch_config valid_launch_config()
{
    term::Terminal_launch_config config;
    config.argv              = {QStringLiteral("resource-lifecycle-fixture"), QStringLiteral("--session")};
    config.working_directory = QStringLiteral("C:/workspace");
    config.initial_grid_size = term::terminal_grid_size_t{24, 80};
    return config;
}

term::Terminal_session_config trace_session_config(term::Terminal_session_config config = {})
{
    config.trace_command_limit              = 512U;
    config.trace_notification_limit         = 512U;
    config.trace_result_limit               = 512U;
    config.trace_resize_limit               = 128U;
    config.trace_output_chunk_limit         = 128U;
    config.capture_last_model_ingest_result = true;
    return config;
}

struct Shared_backend_state
{
    std::atomic<int>           destructed_count{0};
    std::atomic<int>           destructor_callback_attempts{0};
    std::atomic<int>           destructor_callback_completions{0};
    std::atomic<int>           worker_callback_attempts{0};
    std::atomic<int>           worker_callback_completions{0};
    std::atomic<int>           worker_callback_wait_timeouts{0};
    std::atomic<int>           worker_join_timeouts{0};
    std::mutex                 lifecycle_mutex;
    std::condition_variable    backend_destructor_started;
    std::condition_variable    worker_callback_completed;
    int                        backend_destructor_begin_count = 0;
};

[[noreturn]] void terminate_after_worker_join_timeout(
    const Shared_backend_state& state)
{
    std::cerr << "FAIL: Tracking_backend worker did not finish within "
        << k_lifecycle_wait_timeout.count()
        << "ms during backend destruction; refusing to detach a worker "
                 "that may still mutate shared test state\n"
        << "  worker_callback_attempts="
        << state.worker_callback_attempts.load()
        << " worker_callback_completions="
        << state.worker_callback_completions.load()
        << " worker_callback_wait_timeouts="
        << state.worker_callback_wait_timeouts.load()
        << " worker_join_timeouts="
        << state.worker_join_timeouts.load()
        << " backend_destructor_begin_count="
        << state.backend_destructor_begin_count
        << " destructor_callback_attempts="
        << state.destructor_callback_attempts.load()
        << " destructor_callback_completions="
        << state.destructor_callback_completions.load()
        << '\n';
    std::terminate();
}

class Tracking_backend final : public term::Terminal_backend
{
public:
    explicit Tracking_backend(std::shared_ptr<Shared_backend_state> state)
    :
        m_state(std::move(state))
    {}

    ~Tracking_backend() override
    {
        {
            std::lock_guard<std::mutex> lock(m_state->lifecycle_mutex);
            ++m_state->backend_destructor_begin_count;
        }
        m_state->backend_destructor_started.notify_all();

        if (emit_output_in_destructor) {
            ++m_state->destructor_callback_attempts;
            m_callbacks.output_received(QByteArrayLiteral("destructor-output\r\n"));
            ++m_state->destructor_callback_completions;
        }

        if (m_worker.joinable()) {
            std::unique_lock<std::mutex> lock(m_state->lifecycle_mutex);
            const bool worker_finished = m_state->worker_callback_completed.wait_for(
                lock,
                k_lifecycle_wait_timeout,
                [&] {
                    return m_state->worker_callback_completions.load() >
                        m_worker_callback_completions_before_start;
                });
            lock.unlock();

            if (worker_finished) {
                m_worker.join();
            }
            else {
                ++m_state->worker_join_timeouts;
                terminate_after_worker_join_timeout(*m_state);
            }
        }

        ++m_state->destructed_count;
    }

    term::Terminal_backend_result start(
        const term::Terminal_launch_config&    config,
        term::Terminal_backend_callbacks       callbacks) override
    {
        const term::Terminal_backend_result callback_result =
            term::validate_backend_callbacks(callbacks);
        if (term::is_backend_rejection(callback_result)) {
            return callback_result;
        }

        m_callbacks = std::move(callbacks);
        start_configs.push_back(config);

        if (reject_start) {
            if (report_error_on_start_failure) {
                m_callbacks.error_reported({
                    term::Terminal_backend_error_code::START_FAILED,
                    QStringLiteral("tracking backend start failure"),
                });
            }

            return
                term::backend_reject(
                    term::Terminal_backend_error_code::START_FAILED,
                    QStringLiteral("tracking backend start failure"));
        }

        const term::Terminal_backend_result config_result =
            term::validate_launch_config(config);
        if (term::is_backend_rejection(config_result)) {
            m_callbacks.error_reported(*config_result.error);
            return config_result;
        }

        running = true;
        return term::backend_accept();
    }

    term::Terminal_backend_result write(QByteArray bytes) override
    {
        if (!running || reject_write) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("tracking backend write failure"));
        }

        writes.push_back(std::move(bytes));
        return term::backend_accept();
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request request) override
    {
        resize_requests.push_back(request);
        if (!running || reject_resize || !term::is_valid_grid_size(request.grid_size)) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::RESIZE_FAILED,
                    QStringLiteral("tracking backend resize failure"));
        }

        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool paused) override
    {
        output_pause_requests.push_back(paused);
        output_paused = paused;
        return term::backend_accept();
    }

    term::Terminal_backend_result interrupt() override
    {
        if (!running) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::INTERRUPT_FAILED,
                    QStringLiteral("tracking backend interrupt failure"));
        }

        ++interrupt_count;
        running = false;
        if (exit_on_interrupt) {
            m_callbacks.process_exited({term::Terminal_exit_reason::INTERRUPTED, 130});
        }
        return term::backend_accept();
    }

    term::Terminal_backend_result terminate() override
    {
        if (!running) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::TERMINATE_FAILED,
                    QStringLiteral("tracking backend terminate failure"));
        }

        ++terminate_count;
        running = false;
        if (exit_on_terminate) {
            m_callbacks.process_exited({term::Terminal_exit_reason::TERMINATED, 0});
        }
        return term::backend_accept();
    }

    void emit_output(QByteArray bytes)
    {
        m_callbacks.output_received(std::move(bytes));
    }

    void emit_exit(term::Terminal_backend_exit exit)
    {
        running = false;
        m_callbacks.process_exited(exit);
    }

    void emit_output_from_worker_after_delay(QByteArray bytes, std::chrono::milliseconds delay)
    {
        if (m_worker.joinable()) {
            m_worker.join();
        }

        term::Terminal_backend_callbacks callbacks = m_callbacks;
        std::shared_ptr<Shared_backend_state> state = m_state;
        m_worker_callback_completions_before_start =
            state->worker_callback_completions.load();
        m_worker = std::thread([callbacks, state, bytes = std::move(bytes), delay]() mutable {
            std::this_thread::sleep_for(delay);
            ++state->worker_callback_attempts;
            callbacks.output_received(std::move(bytes));
            ++state->worker_callback_completions;
            state->worker_callback_completed.notify_all();
        });
    }

    void emit_output_from_worker_after_backend_destructor_begins(QByteArray bytes)
    {
        if (m_worker.joinable()) {
            m_worker.join();
        }

        term::Terminal_backend_callbacks callbacks  = m_callbacks;
        std::shared_ptr<Shared_backend_state> state = m_state;
        int destructor_begin_count_before_start     = 0;
        {
            std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
            destructor_begin_count_before_start = state->backend_destructor_begin_count;
        }
        m_worker_callback_completions_before_start =
            state->worker_callback_completions.load();

        m_worker = std::thread(
            [callbacks, state, bytes = std::move(bytes),
                destructor_begin_count_before_start]() mutable {
                    std::unique_lock<std::mutex> lock(state->lifecycle_mutex);
                    const bool destructor_started = state->backend_destructor_started.wait_for(
                        lock,
                        k_lifecycle_wait_timeout,
                        [&] {
                            return state->backend_destructor_begin_count >
                                destructor_begin_count_before_start;
                        });
                    lock.unlock();

                    if (!destructor_started) {
                        ++state->worker_callback_wait_timeouts;
                        ++state->worker_callback_completions;
                        state->worker_callback_completed.notify_all();
                        return;
                    }

                    ++state->worker_callback_attempts;
                    callbacks.output_received(std::move(bytes));
                    ++state->worker_callback_completions;
                    state->worker_callback_completed.notify_all();
                });
    }

    bool                       reject_start                               = false;
    bool                       report_error_on_start_failure              = false;
    bool                       reject_write                               = false;
    bool                       reject_resize                              = false;
    bool                       running                                    = false;
    bool                       exit_on_interrupt                          = true;
    bool                       exit_on_terminate                          = true;
    bool                       emit_output_in_destructor                  = false;
    bool                       output_paused                              = false;
    int                        interrupt_count                            = 0;
    int                        terminate_count                            = 0;
    std::vector<term::Terminal_launch_config>
                               start_configs;
    std::vector<QByteArray>    writes;
    std::vector<term::Terminal_backend_resize_request>
                               resize_requests;
    std::vector<bool>          output_pause_requests;

private:
    std::shared_ptr<Shared_backend_state>
                               m_state;
    term::Terminal_backend_callbacks
                               m_callbacks;
    std::thread                m_worker;
    int                        m_worker_callback_completions_before_start = 0;
};

Tracking_backend* make_session(
    std::unique_ptr<term::Terminal_session>&   session,
    std::shared_ptr<Shared_backend_state>      state,
    term::Terminal_session_config              config = {})
{
    auto              backend     = std::make_unique<Tracking_backend>(std::move(state));
    Tracking_backend* backend_ptr = backend.get();
    session = std::make_unique<term::Terminal_session>(
        std::move(backend),
        trace_session_config(config));
    return backend_ptr;
}

std::size_t notification_count(
    const std::vector<term::Terminal_session_notification>&    notifications,
    term::Terminal_session_notification_kind                   kind)
{
    std::size_t count = 0U;
    for (const term::Terminal_session_notification& notification : notifications) {
        if (notification.kind == kind) {
            ++count;
        }
    }

    return count;
}

std::size_t notification_count(
    const term::Terminal_session&              session,
    term::Terminal_session_notification_kind   kind)
{
    return notification_count(session.notifications(), kind);
}

bool test_repeated_create_start_resize_terminate_destroy()
{
    bool ok = true;

    auto state = std::make_shared<Shared_backend_state>();
    constexpr int k_iteration_count = 64;
    for (int i = 0; i < k_iteration_count; ++i) {
        std::unique_ptr<term::Terminal_session> session;
        Tracking_backend* backend = make_session(session, state);

        const term::Terminal_session_result start_result =
            session->start(valid_launch_config());
        ok &= check(start_result.code == term::Terminal_session_result_code::ACCEPTED,
            "repeated lifecycle start is accepted");
        ok &= check(session->process_state() == term::Terminal_process_state::RUNNING &&
            session->backend_ready(),
            "repeated lifecycle session becomes running and backend-ready");
        ok &= check(backend->start_configs.size() == 1U,
            "repeated lifecycle start reaches backend exactly once");

        const term::terminal_grid_size_t resized_grid{24 + (i % 2), 80 + i};
        const term::Terminal_session_result resize_result =
            session->resize(QSizeF(720.0 + i, 360.0), resized_grid);
        ok &= check(resize_result.code == term::Terminal_session_result_code::ACCEPTED,
            "repeated lifecycle resize is accepted while active");
        ok &= check(backend->resize_requests.size() == 1U &&
            backend->resize_requests.back().grid_size.rows == resized_grid.rows &&
            backend->resize_requests.back().grid_size.columns == resized_grid.columns,
            "repeated lifecycle resize reaches backend with requested grid");

        backend->emit_output(QByteArrayLiteral("resource-lifecycle-live-output\r\n"));
        ok &= check(!session->output_chunks().empty(),
            "repeated lifecycle observes backend output before shutdown");

        const term::Terminal_session_result terminate_result = session->terminate();
        ok &= check(terminate_result.code == term::Terminal_session_result_code::ACCEPTED,
            "repeated lifecycle terminate is accepted");
        ok &= check(backend->terminate_count == 1,
            "repeated lifecycle terminate reaches backend exactly once");
        ok &= check(session->process_state() == term::Terminal_process_state::EXITED,
            "repeated lifecycle backend exit is observed");
        ok &= check(session->exit_status().has_value() &&
            session->exit_status()->reason == term::Terminal_exit_reason::TERMINATED,
            "repeated lifecycle terminate records terminated exit status");

        session.reset();
        ok &= check(state->destructed_count.load() == i + 1,
            "repeated lifecycle releases owned backend on session destruction");
    }

    return ok;
}

bool test_failed_start_cleanup_blocks_later_lifecycle_commands()
{
    bool ok = true;

    auto state = std::make_shared<Shared_backend_state>();
    std::unique_ptr<term::Terminal_session> session;
    Tracking_backend* backend = make_session(session, state);
    backend->reject_start                  = true;
    backend->report_error_on_start_failure = true;

    const term::Terminal_session_result start_result = session->start(valid_launch_config());
    ok &= check(start_result.code == term::Terminal_session_result_code::BACKEND_REJECTED,
        "failed-start lifecycle reports backend rejection");
    ok &= check(session->process_state() == term::Terminal_process_state::FAILED &&
        !session->backend_ready(),
        "failed-start lifecycle leaves no running backend");
    ok &= check(notification_count(
        *session,
        term::Terminal_session_notification_kind::BACKEND_ERROR) == 1U,
        "failed-start lifecycle records one backend error");

    const term::Terminal_session_result write_result =
        session->write_user_bytes(QByteArrayLiteral("after-failure"));
    ok &= check(write_result.code == term::Terminal_session_result_code::INVALID_STATE,
        "failed-start lifecycle rejects later user writes");
    ok &= check(backend->writes.empty(),
        "failed-start lifecycle does not forward later writes");

    const term::Terminal_session_result resize_result =
        session->resize(QSizeF(640.0, 320.0), {20, 80});
    ok &= check(resize_result.code == term::Terminal_session_result_code::INVALID_STATE,
        "failed-start lifecycle rejects later resizes");
    ok &= check(backend->resize_requests.empty(),
        "failed-start lifecycle does not forward later resizes");

    const term::Terminal_session_result terminate_result = session->terminate();
    ok &= check(terminate_result.code == term::Terminal_session_result_code::INVALID_STATE,
        "failed-start lifecycle rejects later terminate");
    ok &= check(backend->terminate_count == 0,
        "failed-start lifecycle does not forward later terminate");

    session.reset();
    ok &= check(state->destructed_count.load() == 1,
        "failed-start lifecycle releases owned backend on destruction");

    return ok;
}

bool test_resize_and_output_during_shutdown_are_blocked_until_exit()
{
    bool ok = true;

    auto state = std::make_shared<Shared_backend_state>();
    std::unique_ptr<term::Terminal_session> session;
    Tracking_backend* backend  = make_session(session, state);
    backend->exit_on_terminate = false;

    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "shutdown lifecycle session starts");
    backend->emit_output(QByteArrayLiteral("before-shutdown\r\n"));
    const std::size_t output_count_before_stop = session->output_chunks().size();
    ok &= check(output_count_before_stop == 1U,
        "shutdown lifecycle records live output before terminate");

    const term::Terminal_session_result terminate_result = session->terminate();
    ok &= check(terminate_result.code == term::Terminal_session_result_code::ACCEPTED,
        "shutdown lifecycle terminate request is accepted");
    ok &= check(!session->backend_ready(),
        "shutdown lifecycle clears backend readiness before backend exit");

    const term::Terminal_session_result resize_result =
        session->resize(QSizeF(900.0, 460.0), {30, 100});
    ok &= check(resize_result.code == term::Terminal_session_result_code::INVALID_STATE,
        "shutdown lifecycle rejects resize during pending termination");
    ok &= check(backend->resize_requests.empty(),
        "shutdown lifecycle does not forward resize during pending termination");

    backend->emit_output(QByteArrayLiteral("ignored-after-stop\r\n"));
    ok &= check(session->output_chunks().size() == output_count_before_stop,
        "shutdown lifecycle ignores backend output after stop request");

    const term::Terminal_session_result second_terminate_result = session->terminate();
    ok &= check(second_terminate_result.code == term::Terminal_session_result_code::INVALID_STATE,
        "shutdown lifecycle rejects duplicate terminate before backend exit");
    ok &= check(backend->terminate_count == 1,
        "shutdown lifecycle forwards only one terminate request");

    backend->emit_exit({term::Terminal_exit_reason::TERMINATED, 0});
    ok &= check(session->process_state() == term::Terminal_process_state::EXITED,
        "shutdown lifecycle observes delayed backend exit");
    ok &= check(session->exit_status().has_value() &&
        session->exit_status()->reason == term::Terminal_exit_reason::TERMINATED,
        "shutdown lifecycle records delayed terminated status");
    ok &= check(notification_count(
        *session,
        term::Terminal_session_notification_kind::PROCESS_EXITED) == 1U,
        "shutdown lifecycle publishes one process-exited notification");
    const std::vector<term::Terminal_session_notification> pending_notifications =
        session->take_pending_notifications();
    ok &= check(notification_count(
        pending_notifications,
        term::Terminal_session_notification_kind::PROCESS_EXITED) == 1U,
        "shutdown lifecycle drains one process-exited notification");

    return ok;
}

bool test_deferred_worker_callbacks_are_bounded_and_explicitly_drained()
{
    bool ok = true;

    std::atomic<int> notifier_count{0};
    term::Terminal_session_config config;
    config.backend_event_notifier = [&] {
        ++notifier_count;
    };

    auto state = std::make_shared<Shared_backend_state>();
    std::unique_ptr<term::Terminal_session> session;
    Tracking_backend* backend = make_session(session, state, config);

    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "deferred worker lifecycle session starts");

    backend->emit_output_from_worker_after_delay(
        QByteArrayLiteral("deferred-worker-output\r\n"),
        std::chrono::milliseconds(10));
    ok &= check(wait_until(
        [&] {
            return
                notifier_count.load()                  > 0 &&
                state->worker_callback_attempts.load() > 0;
        },
        k_lifecycle_wait_timeout),
        "deferred worker lifecycle callback arrives before timeout");
    ok &= check(session->output_chunks().empty(),
        "deferred worker lifecycle does not drain callback inline");

    session->process_backend_callback_events();
    ok &= check(session->output_chunks().size() == 1U &&
        session->output_chunks().front() ==
            QByteArrayLiteral("deferred-worker-output\r\n"),
        "deferred worker lifecycle drains output explicitly");

    backend->emit_exit({term::Terminal_exit_reason::EXITED, 5});
    ok &= check(session->process_state() == term::Terminal_process_state::RUNNING,
        "deferred worker lifecycle keeps process running until exit drain");

    session->process_backend_callback_events();
    ok &= check(session->process_state() == term::Terminal_process_state::EXITED,
        "deferred worker lifecycle drains backend exit explicitly");
    ok &= check(session->exit_status().has_value() &&
        session->exit_status()->reason == term::Terminal_exit_reason::EXITED &&
        session->exit_status()->exit_code == 5,
        "deferred worker lifecycle records drained backend exit");

    return ok;
}

bool test_destructor_closes_late_worker_callbacks()
{
    bool ok = true;

    std::atomic<int> notifier_count{0};
    term::Terminal_session_config config;
    config.backend_event_notifier = [&] {
        ++notifier_count;
    };

    auto state = std::make_shared<Shared_backend_state>();
    {
        std::unique_ptr<term::Terminal_session> session;
        Tracking_backend* backend = make_session(session, state, config);
        ok &= check(session->start(valid_launch_config()).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "late-worker lifecycle session starts");

        backend->emit_output_from_worker_after_backend_destructor_begins(
            QByteArrayLiteral("late-worker-output\r\n"));
        session.reset();
    }

    ok &= check(state->destructed_count.load() == 1,
        "late-worker lifecycle destroys backend after closing callbacks");
    ok &= check(state->worker_callback_attempts.load() == 1,
        "late-worker lifecycle worker attempts callback without hanging teardown");
    ok &= check(state->worker_callback_completions.load() == 1,
        "late-worker lifecycle worker callback returns during bounded teardown");
    ok &= check(state->worker_callback_wait_timeouts.load() == 0,
        "late-worker lifecycle worker waits for backend destructor deterministically");
    ok &= check(state->worker_join_timeouts.load() == 0,
        "late-worker lifecycle worker teardown completes before bounded timeout");
    ok &= check(notifier_count.load() == 0,
        "late-worker lifecycle drops callback after destruction without notifying owner");

    return ok;
}

bool test_destructor_closes_synchronous_backend_destructor_callbacks()
{
    bool ok = true;

    std::atomic<int> notifier_count{0};
    term::Terminal_session_config config;
    config.backend_event_notifier = [&] {
        ++notifier_count;
    };

    auto state = std::make_shared<Shared_backend_state>();
    {
        std::unique_ptr<term::Terminal_session> session;
        Tracking_backend* backend = make_session(session, state, config);
        ok &= check(session->start(valid_launch_config()).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "destructor-callback lifecycle session starts");

        backend->emit_output(QByteArrayLiteral("destructor-callback-control\r\n"));
        ok &= check(notifier_count.load() == 1,
            "destructor-callback lifecycle notifies for live callback before close");
        session->process_backend_callback_events();
        ok &= check(session->output_chunks().size() == 1U &&
            session->output_chunks().front() ==
                QByteArrayLiteral("destructor-callback-control\r\n"),
            "destructor-callback lifecycle drains live callback before close");
        notifier_count.store(0);

        backend->emit_output_in_destructor = true;
        session.reset();
    }

    ok &= check(state->destructed_count.load() == 1,
        "destructor-callback lifecycle destroys backend after closing callbacks");
    ok &= check(state->destructor_callback_attempts.load() == 1,
        "destructor-callback lifecycle attempts direct destructor callback");
    ok &= check(state->destructor_callback_completions.load() == 1,
        "destructor-callback lifecycle direct destructor callback returns");
    ok &= check(notifier_count.load() == 0,
        "destructor-callback lifecycle drops direct destructor callback after close");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_repeated_create_start_resize_terminate_destroy();
    ok &= test_failed_start_cleanup_blocks_later_lifecycle_commands();
    ok &= test_resize_and_output_during_shutdown_are_blocked_until_exit();
    ok &= test_deferred_worker_callbacks_are_bounded_and_explicitly_drained();
    ok &= test_destructor_closes_late_worker_callbacks();
    ok &= test_destructor_closes_synchronous_backend_destructor_callbacks();
    return ok ? 0 : 1;
}
