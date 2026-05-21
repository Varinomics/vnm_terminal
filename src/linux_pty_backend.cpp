#include "vnm_terminal/internal/linux_pty_backend.h"

#if defined(__linux__)

#include "native_backend_io_core.h"
#include <QFile>
#include <QProcessEnvironment>
#include <QStringList>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vnm_terminal::internal {

namespace {

constexpr std::chrono::milliseconds k_exit_output_drain_timeout(250);
constexpr int k_waitpid_failure_exit_code = -1;

QString posix_error_message(QStringView context, int code)
{
    return QStringLiteral("%1: %2")
        .arg(context.toString(), QString::fromLocal8Bit(std::strerror(code)));
}

bool size_fits_pty(terminal_grid_size_t grid_size)
{
    return
        is_valid_grid_size(grid_size)                                &&
        grid_size.rows <= std::numeric_limits<unsigned short>::max() &&
        grid_size.columns <= std::numeric_limits<unsigned short>::max();
}

winsize winsize_from_grid_size(terminal_grid_size_t grid_size)
{
    winsize size{};
    size.ws_row = static_cast<unsigned short>(grid_size.rows);
    size.ws_col = static_cast<unsigned short>(grid_size.columns);
    return size;
}

class Unique_fd
{
public:
    Unique_fd() = default;

    explicit Unique_fd(int value)
    :
        m_fd(value)
    {}

    ~Unique_fd()
    {
        reset();
    }

    Unique_fd(const Unique_fd&)            = delete;
    Unique_fd& operator=(const Unique_fd&) = delete;

    Unique_fd(Unique_fd&& other) noexcept
    :
        m_fd(std::exchange(other.m_fd, -1))
    {}

    Unique_fd& operator=(Unique_fd&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_fd = std::exchange(other.m_fd, -1);
        }

        return *this;
    }

    int get() const
    {
        return m_fd;
    }

    int release()
    {
        return std::exchange(m_fd, -1);
    }

    void reset(int value = -1)
    {
        if (m_fd >= 0) {
            ::close(m_fd);
        }

        m_fd = value;
    }

    explicit operator bool() const
    {
        return m_fd >= 0;
    }

private:
    int m_fd = -1;
};

struct Queued_write
{
    QByteArray                 bytes;
};

struct Signal_targets
{
    std::array<pid_t, 2>       process_groups{};
    std::size_t                count = 0U;
};

struct Native_launch_data
{
    std::vector<QByteArray>    argv_bytes;
    std::vector<char*>         argv_ptrs;
    std::vector<QByteArray>    env_bytes;
    std::vector<char*>         env_ptrs;
    std::vector<QByteArray>    executable_candidates;
    QByteArray                 working_directory;
};

bool string_list_has_nul(const QStringList& values)
{
    for (const QString& value : values) {
        if (value.contains(QChar(u'\0'))) {
            return true;
        }
    }

    return false;
}

std::optional<QByteArray> environment_value(
    const QProcessEnvironment& environment,
    const QString&             name)
{
    if (!environment.contains(name)) {
        return std::nullopt;
    }

    return environment.value(name).toLocal8Bit();
}

std::vector<QByteArray> executable_candidates(
    const QByteArray&          executable,
    const QProcessEnvironment& environment)
{
    if (executable.contains('/')) {
        return {executable};
    }

    QByteArray path = environment_value(environment, QStringLiteral("PATH"))
        .value_or(QByteArrayLiteral("/usr/local/bin:/usr/bin:/bin"));
    if (path.isEmpty()) {
        path = QByteArrayLiteral(".");
    }

    std::vector<QByteArray> candidates;
    int start = 0;
    for (;;) {
        const int separator = path.indexOf(':', start);
        QByteArray directory =
            separator < 0 ? path.mid(start) : path.mid(start, separator - start);
        if (directory.isEmpty()) {
            directory = QByteArrayLiteral(".");
        }

        QByteArray candidate = directory;
        if (!candidate.endsWith('/')) {
            candidate.append('/');
        }
        candidate.append(executable);
        candidates.push_back(std::move(candidate));

        if (separator < 0) {
            break;
        }

        start = separator + 1;
    }

    return candidates;
}

std::optional<Native_launch_data> make_native_launch_data(
    const Terminal_effective_launch_config& config)
{
    if (string_list_has_nul(config.argv)) {
        return std::nullopt;
    }

    QStringList env_entries = config.environment.toStringList();
    if (string_list_has_nul(env_entries)) {
        return std::nullopt;
    }

    Native_launch_data data;
    data.argv_bytes.reserve(static_cast<std::size_t>(config.argv.size()));
    for (const QString& argument : config.argv) {
        data.argv_bytes.push_back(QFile::encodeName(argument));
    }

    data.argv_ptrs.reserve(data.argv_bytes.size() + 1U);
    for (QByteArray& argument : data.argv_bytes) {
        data.argv_ptrs.push_back(argument.data());
    }
    data.argv_ptrs.push_back(nullptr);

    data.env_bytes.reserve(static_cast<std::size_t>(env_entries.size()));
    for (const QString& entry : env_entries) {
        data.env_bytes.push_back(entry.toLocal8Bit());
    }

    data.env_ptrs.reserve(data.env_bytes.size() + 1U);
    for (QByteArray& entry : data.env_bytes) {
        data.env_ptrs.push_back(entry.data());
    }
    data.env_ptrs.push_back(nullptr);

    data.executable_candidates =
        executable_candidates(data.argv_bytes.front(), config.environment);
    data.working_directory = QFile::encodeName(config.working_directory);
    return data;
}

int set_fd_nonblocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return errno;
    }

    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return errno;
    }

    return 0;
}

int set_fd_cloexec(int fd)
{
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0) {
        return errno;
    }

    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        return errno;
    }

    return 0;
}

int create_pipe(Unique_fd& read_end, Unique_fd& write_end, bool nonblocking)
{
    int fds[2] = {-1, -1};
    if (::pipe(fds) < 0) {
        return errno;
    }

    read_end.reset(fds[0]);
    write_end.reset(fds[1]);

    int result = set_fd_cloexec(read_end.get());
    if (result != 0) {
        return result;
    }

    result = set_fd_cloexec(write_end.get());
    if (result != 0) {
        return result;
    }

    if (nonblocking) {
        result = set_fd_nonblocking(read_end.get());
        if (result != 0) {
            return result;
        }

        result = set_fd_nonblocking(write_end.get());
        if (result != 0) {
            return result;
        }
    }

    return 0;
}

bool pipe_read_exact(int fd, int& value)
{
    char*       out    = reinterpret_cast<char*>(&value);
    std::size_t offset = 0U;
    while (offset < sizeof(value)) {
        const ssize_t count = ::read(fd, out + offset, sizeof(value) - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }

        if (count == 0) {
            return false;
        }

        if (errno == EINTR) {
            continue;
        }

        return false;
    }

    return true;
}

void pipe_write_errno_and_exit(int fd, int code)
{
    const char* bytes  = reinterpret_cast<const char*>(&code);
    std::size_t offset = 0U;
    while (offset < sizeof(code)) {
        const ssize_t count = ::write(fd, bytes + offset, sizeof(code) - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }

        if (count < 0 && errno == EINTR) {
            continue;
        }

        break;
    }

    _exit(127);
}

[[noreturn]] void exec_child(
    const Native_launch_data&              data,
    int                                    startup_error_write)
{
    if (!data.working_directory.isEmpty() &&
        ::chdir(data.working_directory.constData()) < 0)
    {
        pipe_write_errno_and_exit(startup_error_write, errno);
    }

    int  last_error        = ENOENT;
    bool saw_access_denied = false;
    for (const QByteArray& candidate : data.executable_candidates) {
        ::execve(candidate.constData(), data.argv_ptrs.data(), data.env_ptrs.data());

        if (errno == EACCES) {
            saw_access_denied = true;
        }

        if (errno != ENOENT && errno != ENOTDIR && errno != EACCES) {
            last_error = errno;
            break;
        }

        last_error = errno;
    }

    pipe_write_errno_and_exit(
        startup_error_write,
        saw_access_denied ? EACCES : last_error);
}

Terminal_exit_reason exit_reason_from_wait_status(
    int                                    status,
    std::optional<Terminal_exit_reason>    override_reason)
{
    if (override_reason.has_value()) {
        return *override_reason;
    }

    if (WIFSIGNALED(status)) {
        const int signal_number = WTERMSIG(status);
        if (signal_number == SIGINT) {
            return Terminal_exit_reason::INTERRUPTED;
        }

        return Terminal_exit_reason::TERMINATED;
    }

    return Terminal_exit_reason::EXITED;
}

int exit_code_from_wait_status(int status)
{
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return 1;
}

int waitpid_nointr(pid_t pid, int* status, int options)
{
    for (;;) {
        const pid_t result = ::waitpid(pid, status, options);
        if (result >= 0 || errno != EINTR) {
            return static_cast<int>(result);
        }
    }
}

std::chrono::milliseconds bounded_sleep_interval(
    std::chrono::milliseconds remaining)
{
    constexpr std::chrono::milliseconds k_poll_interval(10);
    return std::min(k_poll_interval, remaining);
}

int poll_timeout_until(std::optional<std::chrono::steady_clock::time_point> deadline)
{
    if (!deadline.has_value()) {
        return -1;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= *deadline) {
        return 0;
    }

    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
    return static_cast<int>(std::min<std::int64_t>(
        remaining.count(),
        std::numeric_limits<int>::max()));
}

void add_signal_target(Signal_targets& targets, pid_t process_group)
{
    if (process_group <= 0) {
        return;
    }

    for (std::size_t i = 0U; i < targets.count; ++i) {
        if (targets.process_groups[i] == process_group) {
            return;
        }
    }

    if (targets.count < targets.process_groups.size()) {
        targets.process_groups[targets.count++] = process_group;
    }
}

Signal_targets process_signal_targets(pid_t child_process_group, int master)
{
    Signal_targets targets;
    add_signal_target(targets, child_process_group);

    if (master >= 0) {
        const pid_t foreground_pgid = ::tcgetpgrp(master);
        if (foreground_pgid > 0) {
            add_signal_target(targets, foreground_pgid);
        }
    }

    return targets;
}

Signal_targets shutdown_signal_targets(
    pid_t  child_process_group,
    int    master,
    bool   child_reaped,
    bool   reader_finished)
{
    if (child_reaped && reader_finished) {
        return {};
    }

    return process_signal_targets(child_process_group, master);
}

std::optional<int> send_signal_to_targets(const Signal_targets& targets, int signal_number)
{
    int first_error = 0;
    for (std::size_t i = 0U; i < targets.count; ++i) {
        if (::kill(-targets.process_groups[i], signal_number) == 0) {
            continue;
        }

        if (errno != ESRCH && first_error == 0) {
            first_error = errno;
        }
    }

    if (first_error != 0) {
        return first_error;
    }

    return std::nullopt;
}

}

class Linux_pty_backend::Impl
{
public:
    ~Impl()
    {
        shutdown();
    }

    Terminal_backend_result start(
        const Terminal_launch_config&  config,
        Terminal_backend_callbacks     callbacks)
    {
        Native_backend_start_gate start_gate{
            m_mutex,
            m_running,
            m_start_attempted,
            m_start_in_progress,
        };
        Native_backend_start_precheck precheck = validate_native_backend_start_preconditions(
            config,
            callbacks,
            start_gate,
            QStringLiteral("Linux PTY"));
        if (is_backend_rejection(precheck.result)) {
            return precheck.result;
        }

        Terminal_effective_launch_config effective_config =
            std::move(*precheck.effective_config);

        const auto reject_start = [&](Terminal_backend_error_code code, QString message) {
            return reject_native_backend_start_attempt(
                callbacks,
                start_gate,
                code,
                std::move(message));
        };

        if (!size_fits_pty(effective_config.initial_grid_size)) {
            return
                reject_start(
                    Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
                    QStringLiteral("initial terminal size is outside the PTY range"));
        }

        const std::optional<Native_launch_data> native_launch =
            make_native_launch_data(effective_config);
        if (!native_launch.has_value()) {
            return
                reject_start(
                    Terminal_backend_error_code::INVALID_LAUNCH_CONFIG,
                    QStringLiteral("launch argv and environment must not contain NUL bytes"));
        }

        Unique_fd startup_error_read;
        Unique_fd startup_error_write;
        int pipe_result = create_pipe(startup_error_read, startup_error_write, false);
        if (pipe_result != 0) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("pipe startup error"), pipe_result));
        }

        Unique_fd read_wake_read;
        Unique_fd read_wake_write;
        pipe_result = create_pipe(read_wake_read, read_wake_write, true);
        if (pipe_result != 0) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("pipe backend read wake"), pipe_result));
        }

        Unique_fd write_wake_read;
        Unique_fd write_wake_write;
        pipe_result = create_pipe(write_wake_read, write_wake_write, true);
        if (pipe_result != 0) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("pipe backend write wake"), pipe_result));
        }

        winsize initial_winsize =
            winsize_from_grid_size(effective_config.initial_grid_size);
        int master_fd = -1;
        const pid_t child_pid =
            ::forkpty(&master_fd, nullptr, nullptr, &initial_winsize);
        if (child_pid < 0) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("forkpty"), errno));
        }

        if (child_pid == 0) {
            startup_error_read.reset();
            exec_child(*native_launch, startup_error_write.get());
        }

        startup_error_write.reset();
        Unique_fd master(master_fd);

        const int cloexec_result = set_fd_cloexec(master.get());
        if (cloexec_result != 0) {
            ::kill(child_pid, SIGKILL);
            waitpid_nointr(child_pid, nullptr, 0);
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("fcntl PTY close-on-exec"), cloexec_result));
        }

        int child_errno = 0;
        if (pipe_read_exact(startup_error_read.get(), child_errno)) {
            waitpid_nointr(child_pid, nullptr, 0);
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("execve"), child_errno));
        }
        startup_error_read.reset();

        const int nonblocking_result = set_fd_nonblocking(master.get());
        if (nonblocking_result != 0) {
            ::kill(-child_pid, SIGKILL);
            waitpid_nointr(child_pid, nullptr, 0);
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("fcntl PTY nonblocking"), nonblocking_result));
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_callbacks                          = std::move(callbacks);
            m_master                             = std::move(master);
            m_read_wake_read                     = std::move(read_wake_read);
            m_read_wake_write                    = std::move(read_wake_write);
            m_write_wake_read                    = std::move(write_wake_read);
            m_write_wake_write                   = std::move(write_wake_write);
            m_child_pid                          = child_pid;
            m_child_process_group                = child_pid;
            m_running                            = true;
            m_start_attempted                    = true;
            m_start_in_progress                  = false;
            m_stopping                           = false;
            m_process_stopping                   = false;
            m_output_paused                      = false;
            m_exit_reported                      = false;
            m_reader_finished                    = false;
            m_writer_failed                      = false;
            m_child_reaped                       = false;
            m_reader_timed_out_after_child_reap  = false;
            m_paused_output_delivery_in_progress = false;
            m_termination_policy                 = effective_config.termination_policy;
            m_exit_reason_override.reset();
            m_queued_write_bytes = 0U;
            m_write_queue.clear();
        }

        try {
            m_reader_thread = std::thread([this] { read_loop(); });
            m_writer_thread = std::thread([this] { write_loop(); });
            m_wait_thread   = std::thread([this] { wait_loop(); });
        }
        catch (const std::system_error& error) {
            const QString message = QStringLiteral("Linux PTY worker thread startup failed: %1")
                .arg(QString::fromLocal8Bit(error.what()));
            report_error(Terminal_backend_error_code::START_FAILED, message);
            shutdown();
            return backend_reject(Terminal_backend_error_code::START_FAILED, message);
        }

        return backend_accept();
    }

    Terminal_backend_result write(QByteArray bytes)
    {
        if (bytes.isEmpty()) {
            return
                backend_reject(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("Linux PTY write requires bytes"));
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running || m_process_stopping || m_stopping || m_writer_failed || !m_master) {
            return
                backend_reject(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("Linux PTY backend is not writable"));
        }

        const std::size_t byte_count = static_cast<std::size_t>(bytes.size());
        if (!native_backend_write_queue_can_accept(m_queued_write_bytes, byte_count)) {
            return
                backend_reject(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("Linux PTY write queue limit reached"));
        }

        add_native_backend_queued_write_bytes(m_queued_write_bytes, byte_count);
        m_write_queue.push_back({std::move(bytes)});
        m_write_cv.notify_one();
        return backend_accept();
    }

    Terminal_backend_result resize(Terminal_backend_resize_request request)
    {
        if (!size_fits_pty(request.grid_size)) {
            return
                backend_reject(
                    Terminal_backend_error_code::RESIZE_FAILED,
                    QStringLiteral("Linux PTY resize requires a positive unsigned-short grid"));
        }

        int master = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_process_stopping || m_stopping || !m_master || m_child_pid <= 0) {
                return
                    backend_reject(
                        Terminal_backend_error_code::RESIZE_FAILED,
                        QStringLiteral("Linux PTY resize requires a running process"));
            }

            master = m_master.get();
        }

        winsize size = winsize_from_grid_size(request.grid_size);
        if (::ioctl(master, TIOCSWINSZ, &size) < 0) {
            return
                backend_reject(
                    Terminal_backend_error_code::RESIZE_FAILED,
                    posix_error_message(QStringLiteral("TIOCSWINSZ"), errno));
        }

        return backend_accept();
    }

    Terminal_backend_result set_output_paused(bool paused)
    {
        QByteArray paused_output;
        bool paused_output_delivery_started = false;
        bool should_wake_reader = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_process_stopping || m_stopping) {
                m_output_paused = false;
                should_wake_reader = true;
            }
            else {
                m_output_paused = paused;
                should_wake_reader = !paused;
            }

            if (!m_output_paused && !m_paused_output.isEmpty()) {
                paused_output_delivery_started =
                    take_paused_output_for_delivery_locked(paused_output);
            }
        }

        if (!paused_output.isEmpty()) {
            deliver_output(std::move(paused_output));
        }
        finish_paused_output_delivery(paused_output_delivery_started);

        if (should_wake_reader && !paused_output_delivery_started) {
            m_output_cv.notify_all();
            wake_io_threads();
        }

        return backend_accept();
    }

    Terminal_backend_result interrupt()
    {
        int master = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_process_stopping || m_stopping || !m_master || m_child_pid <= 0) {
                return
                    backend_reject(
                        Terminal_backend_error_code::INTERRUPT_FAILED,
                        QStringLiteral("Linux PTY interrupt requires a running process"));
            }

            master = m_master.get();
        }

        const pid_t foreground_pgid = ::tcgetpgrp(master);
        if (foreground_pgid < 0) {
            return
                backend_reject(
                    Terminal_backend_error_code::INTERRUPT_FAILED,
                    posix_error_message(QStringLiteral("tcgetpgrp"), errno));
        }
        if (foreground_pgid == 0) {
            return
                backend_reject(
                    Terminal_backend_error_code::INTERRUPT_FAILED,
                    QStringLiteral("Linux PTY has no foreground process group"));
        }

        int interrupt_error = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_process_stopping || m_stopping) {
                return
                    backend_reject(
                        Terminal_backend_error_code::INTERRUPT_FAILED,
                        QStringLiteral("Linux PTY interrupt requires a running process"));
            }

            if (::kill(-foreground_pgid, SIGINT) < 0) {
                interrupt_error = errno;
            }
        }

        if (interrupt_error != 0) {
            return
                backend_reject(
                    Terminal_backend_error_code::INTERRUPT_FAILED,
                    posix_error_message(QStringLiteral("SIGINT"), interrupt_error));
        }

        return backend_accept();
    }

    Terminal_backend_result terminate()
    {
        pid_t child_pid           = -1;
        pid_t child_process_group = -1;
        int master                = -1;
        QByteArray paused_output;
        bool paused_output_delivery_started = false;
        Terminal_termination_policy policy;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_process_stopping || m_stopping || m_child_pid <= 0) {
                return
                    backend_reject(
                        Terminal_backend_error_code::TERMINATE_FAILED,
                        QStringLiteral("Linux PTY terminate requires a running process"));
            }

            child_pid           = m_child_pid;
            child_process_group = m_child_process_group;
            master              = m_master.get();
            policy              = m_termination_policy;

            m_process_stopping     = true;
            m_output_paused        = false;
            m_exit_reason_override = Terminal_exit_reason::TERMINATED;
            if (!m_paused_output.isEmpty()) {
                paused_output_delivery_started =
                    take_paused_output_for_delivery_locked(paused_output);
            }
            m_write_queue.clear();
            m_queued_write_bytes = 0U;
        }

        if (!paused_output.isEmpty()) {
            deliver_output(std::move(paused_output));
        }
        finish_paused_output_delivery(paused_output_delivery_started);
        m_output_cv.notify_all();
        m_write_cv.notify_all();
        wake_io_threads();
        return start_termination_escalation(
            child_pid,
            process_signal_targets(child_process_group, master),
            policy);
    }

    Terminal_backend_result start_termination_escalation(
        pid_t                          child_pid,
        Signal_targets                 targets,
        Terminal_termination_policy    policy)
    {
        try {
            m_termination_thread = std::thread(
                [this, child_pid, targets, policy] {
                    termination_escalation_loop(child_pid, targets, policy);
                });
        }
        catch (const std::system_error& error) {
            const QString message =
                QStringLiteral("Linux PTY termination escalation worker failed: %1")
                    .arg(QString::fromLocal8Bit(error.what()));
            const std::optional<int> kill_error = send_signal_to_targets(targets, SIGKILL);
            if (kill_error.has_value()) {
                return
                    backend_reject(
                        Terminal_backend_error_code::TERMINATE_FAILED,
                        posix_error_message(QStringLiteral("SIGKILL"), *kill_error));
            }

            return backend_reject(Terminal_backend_error_code::TERMINATE_FAILED, message);
        }

        return backend_accept();
    }

    void shutdown()
    {
        pid_t child_pid           = -1;
        pid_t child_process_group = -1;
        int master                = -1;
        bool child_reaped         = false;
        bool reader_finished      = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown_started) {
                return;
            }

            m_shutdown_started                   = true;
            m_stopping                           = true;
            m_process_stopping                   = true;
            m_output_paused                      = false;
            m_paused_output_delivery_in_progress = false;
            m_callbacks                          = {};
            child_pid                            = m_child_pid;
            child_process_group                  = m_child_process_group;
            master                               = m_master.get();
            child_reaped                         = m_child_reaped;
            reader_finished                      = m_reader_finished;
        }

        m_output_cv.notify_all();
        m_write_cv.notify_all();
        wake_io_threads();

        (void)send_signal_to_targets(
            shutdown_signal_targets(
                child_process_group,
                master,
                child_reaped,
                reader_finished),
            SIGKILL);

        join_threads();
        reap_child_if_unreaped(child_pid);

        std::lock_guard<std::mutex> lock(m_mutex);
        m_master.reset();
        m_read_wake_read.reset();
        m_read_wake_write.reset();
        m_write_wake_read.reset();
        m_write_wake_write.reset();
        m_write_queue.clear();
        m_queued_write_bytes  = 0U;
        m_child_process_group = -1;
        m_running             = false;
    }

    bool is_worker_thread() const
    {
        const std::thread::id current_id = std::this_thread::get_id();
        const auto matches_current_thread = [current_id](const std::thread& thread) {
            return thread.joinable() && thread.get_id() == current_id;
        };

        return
            matches_current_thread(m_reader_thread) ||
            matches_current_thread(m_writer_thread) ||
            matches_current_thread(m_wait_thread) || matches_current_thread(
                m_termination_thread);
    }

    void defer_shutdown_and_delete() noexcept
    {
        try {
            std::thread([this] {
                shutdown();
                delete this;
            }).detach();
        }
        catch (...) {
            request_shutdown_without_cleanup();
        }
    }

private:
    void request_shutdown_without_cleanup()
    {
        pid_t child_pid           = -1;
        pid_t child_process_group = -1;
        int master                = -1;
        bool child_reaped         = false;
        bool reader_finished      = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown_started) {
                return;
            }

            m_shutdown_started                   = true;
            m_stopping                           = true;
            m_process_stopping                   = true;
            m_output_paused                      = false;
            m_paused_output_delivery_in_progress = false;
            m_callbacks                          = {};
            child_pid                            = m_child_pid;
            child_process_group                  = m_child_process_group;
            master                               = m_master.get();
            child_reaped                         = m_child_reaped;
            reader_finished                      = m_reader_finished;
        }

        m_output_cv.notify_all();
        m_write_cv.notify_all();
        wake_io_threads();

        (void)send_signal_to_targets(
            shutdown_signal_targets(
                child_process_group,
                master,
                child_reaped,
                reader_finished),
            SIGKILL);
    }

    Terminal_backend_callbacks callbacks_for_delivery()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_callbacks;
    }

    void report_error(Terminal_backend_error_code code, QString message)
    {
        Terminal_backend_callbacks callbacks = callbacks_for_delivery();
        report_native_backend_error(callbacks, code, std::move(message));
    }

    void report_exit_once(int wait_status)
    {
        std::optional<Terminal_exit_reason> override_reason;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            override_reason = m_exit_reason_override;
        }

        report_exit_once(
            exit_reason_from_wait_status(wait_status, override_reason),
            exit_code_from_wait_status(wait_status));
    }

    void report_exit_once(Terminal_exit_reason reason, int exit_code)
    {
        Terminal_backend_callbacks callbacks;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_exit_reported) {
                return;
            }

            m_exit_reported    = true;
            m_running          = false;
            m_process_stopping = true;
            m_stopping         = true;
            m_output_paused    = false;
        }

        m_output_cv.notify_all();
        m_write_cv.notify_all();
        wake_io_threads();

        callbacks = callbacks_for_delivery();
        report_native_backend_exit(callbacks, reason, exit_code);
    }

    void deliver_output(QByteArray bytes)
    {
        Terminal_backend_callbacks callbacks = callbacks_for_delivery();
        deliver_native_backend_output(callbacks, std::move(bytes));
    }

    bool take_paused_output_for_delivery_locked(QByteArray& paused_output)
    {
        if (m_paused_output.isEmpty()) {
            return false;
        }

        paused_output = std::move(m_paused_output);
        m_paused_output.clear();
        m_paused_output_delivery_in_progress = true;
        return true;
    }

    void finish_paused_output_delivery(bool delivery_started)
    {
        if (!delivery_started) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_paused_output_delivery_in_progress = false;
        }

        m_output_cv.notify_all();
        wake_io_threads();
    }

    void deliver_or_buffer_output(QByteArray bytes)
    {
        deliver_or_buffer_native_backend_output(
            m_mutex,
            m_callbacks,
            m_paused_output,
            m_output_paused,
            std::move(bytes),
            [this] {
                return !m_process_stopping && !m_stopping;
            });
    }

    void mark_reader_finished(bool timed_out_after_child_reap)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_reader_timed_out_after_child_reap = timed_out_after_child_reap;
            m_reader_finished = true;
        }

        m_reader_cv.notify_all();
    }

    void wait_for_reader_finished()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_reader_cv.wait(lock, [&] {
            return m_reader_finished;
        });
    }

    void reap_child_if_unreaped(pid_t child_pid)
    {
        if (child_pid <= 0) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_child_reaped) {
                return;
            }
        }

        const int result = waitpid_nointr(child_pid, nullptr, 0);
        if (result == child_pid || (result < 0 && errno == ECHILD)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_child_reaped = true;
            m_child_pid = -1;
        }
    }

    bool signal_target_active(pid_t process_group)
    {
        if (process_group <= 0) {
            return false;
        }

        if (::kill(-process_group, 0) == 0) {
            return true;
        }

        return errno == EPERM;
    }

    bool signal_targets_active(const Signal_targets& targets)
    {
        for (std::size_t i = 0U; i < targets.count; ++i) {
            if (signal_target_active(targets.process_groups[i])) {
                return true;
            }
        }

        return false;
    }

    bool wait_for_signal_targets_exit_observed(
        const Signal_targets&      targets,
        std::chrono::milliseconds  interval)
    {
        const auto deadline = std::chrono::steady_clock::now() + interval;
        do {
            if (!signal_targets_active(targets)) {
                return true;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }

            std::this_thread::sleep_for(bounded_sleep_interval(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
        }
        while (true);
    }

    void wake_io_threads()
    {
        int read_wake_write = -1;
        int write_wake_write = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            read_wake_write = m_read_wake_write.get();
            write_wake_write = m_write_wake_write.get();
        }

        const unsigned char byte = 1U;
        if (read_wake_write >= 0) {
            const ssize_t result = ::write(read_wake_write, &byte, sizeof(byte));
            (void)result;
        }

        if (write_wake_write >= 0) {
            const ssize_t result = ::write(write_wake_write, &byte, sizeof(byte));
            (void)result;
        }
    }

    void drain_wake_pipe(int wake_read)
    {
        unsigned char buffer[64];
        for (;;) {
            const ssize_t count = ::read(wake_read, buffer, sizeof(buffer));
            if (count > 0) {
                continue;
            }

            if (count < 0 && errno == EINTR) {
                continue;
            }

            break;
        }
    }

    bool stopping()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stopping;
    }

    bool write_stopping()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stopping || m_process_stopping;
    }

    void read_loop()
    {
        std::vector<char> buffer(k_native_backend_output_read_chunk_bytes);
        std::optional<std::chrono::steady_clock::time_point> final_drain_deadline;
        bool timed_out_after_child_reap = false;

        for (;;) {
            int master = -1;
            int wake_read = -1;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_output_cv.wait(lock, [&] {
                    return m_stopping ||
                        ((m_process_stopping || !m_output_paused) &&
                            !m_paused_output_delivery_in_progress);
                });

                if (m_stopping) {
                    break;
                }

                if (m_child_reaped && !final_drain_deadline.has_value()) {
                    final_drain_deadline =
                        std::chrono::steady_clock::now() + k_exit_output_drain_timeout;
                }

                master = m_master.get();
                wake_read = m_read_wake_read.get();
            }

            pollfd fds[2] = {
                { master, POLLIN | POLLHUP | POLLERR, 0 },
                { wake_read, POLLIN, 0 },
            };

            if (final_drain_deadline.has_value() &&
                std::chrono::steady_clock::now() >= *final_drain_deadline)
            {
                timed_out_after_child_reap = true;
                break;
            }

            const int poll_result = ::poll(fds, 2, poll_timeout_until(final_drain_deadline));
            if (poll_result == 0) {
                timed_out_after_child_reap = final_drain_deadline.has_value();
                break;
            }

            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue;
                }

                if (!stopping()) {
                    report_error(
                        Terminal_backend_error_code::READ_FAILED,
                        posix_error_message(QStringLiteral("poll PTY output"), errno));
                }
                break;
            }

            if ((fds[1].revents & POLLIN) != 0) {
                drain_wake_pipe(wake_read);
                if (stopping()) {
                    break;
                }
            }

            if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }

            const ssize_t bytes_read = ::read(master, buffer.data(), buffer.size());
            if (bytes_read < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }

                if (errno != EIO && !stopping()) {
                    report_error(
                        Terminal_backend_error_code::READ_FAILED,
                        posix_error_message(QStringLiteral("PTY output read"), errno));
                }
                break;
            }

            if (bytes_read == 0) {
                break;
            }

            deliver_or_buffer_output(QByteArray(
                buffer.data(),
                static_cast<qsizetype>(bytes_read)));
        }

        mark_reader_finished(timed_out_after_child_reap);
    }

    void write_loop()
    {
        for (;;) {
            Queued_write write;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_write_cv.wait(lock, [&] {
                    return m_stopping || m_process_stopping || !m_write_queue.empty();
                });

                if (m_stopping || m_process_stopping) {
                    return;
                }

                write = std::move(m_write_queue.front());
                m_write_queue.pop_front();
                remove_native_backend_queued_write_bytes(
                    m_queued_write_bytes,
                    static_cast<std::size_t>(write.bytes.size()));
            }

            if (!write_all(write.bytes)) {
                mark_writer_failed();
                return;
            }
        }
    }

    void mark_writer_failed()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_stopping && !m_process_stopping) {
            m_writer_failed = true;
        }
    }

    bool wait_until_master_writable(int master, int wake_read)
    {
        for (;;) {
            pollfd fds[2] = {
                { master, POLLOUT | POLLHUP | POLLERR, 0 },
                { wake_read, POLLIN, 0 },
            };

            const int poll_result = ::poll(fds, 2, -1);
            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue;
                }

                if (!write_stopping()) {
                    report_error(
                        Terminal_backend_error_code::WRITE_FAILED,
                        posix_error_message(QStringLiteral("poll PTY input"), errno));
                }
                return false;
            }

            if ((fds[1].revents & POLLIN) != 0) {
                drain_wake_pipe(wake_read);
                if (write_stopping()) {
                    return false;
                }
            }

            if ((fds[0].revents & POLLOUT) != 0) {
                return true;
            }

            if ((fds[0].revents & (POLLHUP | POLLERR)) != 0) {
                return false;
            }
        }
    }

    bool write_all(const QByteArray& bytes)
    {
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            int master = -1;
            int wake_read = -1;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopping || m_process_stopping || !m_master) {
                    return false;
                }

                master = m_master.get();
                wake_read = m_write_wake_read.get();
            }

            if (!wait_until_master_writable(master, wake_read)) {
                return false;
            }

            if (write_stopping()) {
                return false;
            }

            const qsizetype remaining = bytes.size() - offset;
            const ssize_t count = ::write(
                master,
                bytes.constData() + offset,
                static_cast<std::size_t>(remaining));
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }

                if (errno != EIO && !write_stopping()) {
                    report_error(
                        Terminal_backend_error_code::WRITE_FAILED,
                        posix_error_message(QStringLiteral("PTY input write"), errno));
                }
                return false;
            }

            if (count == 0) {
                report_error(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("PTY input write made no progress"));
                return false;
            }

            offset += static_cast<qsizetype>(count);
        }

        return true;
    }

    void wait_loop()
    {
        pid_t child_pid = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            child_pid = m_child_pid;
        }

        if (child_pid <= 0) {
            return;
        }

        int status = 0;
        if (waitpid_nointr(child_pid, &status, 0) != child_pid) {
            const int wait_error = errno;
            QByteArray paused_output;
            bool paused_output_delivery_started = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_child_reaped = wait_error == ECHILD;
                if (m_child_reaped) {
                    m_child_pid = -1;
                }
                m_process_stopping = true;
                m_output_paused = false;
                m_write_queue.clear();
                m_queued_write_bytes = 0U;
                if (!m_paused_output.isEmpty()) {
                    paused_output_delivery_started =
                        take_paused_output_for_delivery_locked(paused_output);
                }
            }
            if (!paused_output.isEmpty()) {
                deliver_output(std::move(paused_output));
            }
            finish_paused_output_delivery(paused_output_delivery_started);
            m_output_cv.notify_all();
            m_write_cv.notify_all();
            wake_io_threads();
            wait_for_reader_finished();
            report_error(
                Terminal_backend_error_code::TERMINATE_FAILED,
                posix_error_message(QStringLiteral("waitpid"), wait_error));
            report_exit_once(
                Terminal_exit_reason::TERMINATED,
                k_waitpid_failure_exit_code);
            return;
        }

        QByteArray paused_output;
        bool paused_output_delivery_started = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_child_reaped     = true;
            m_child_pid        = -1;
            m_process_stopping = true;
            m_output_paused    = false;
            m_write_queue.clear();
            m_queued_write_bytes = 0U;
            if (!m_paused_output.isEmpty()) {
                paused_output_delivery_started =
                    take_paused_output_for_delivery_locked(paused_output);
            }
        }
        if (!paused_output.isEmpty()) {
            deliver_output(std::move(paused_output));
        }
        finish_paused_output_delivery(paused_output_delivery_started);
        m_output_cv.notify_all();
        m_write_cv.notify_all();
        wake_io_threads();
        wait_for_reader_finished();
        kill_child_process_group_after_exit_timeout();
        report_exit_once(status);
    }

    void kill_child_process_group_after_exit_timeout()
    {
        pid_t child_process_group = -1;
        int master = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_reader_timed_out_after_child_reap) {
                return;
            }

            child_process_group = m_child_process_group;
            master = m_master.get();
        }

        (void)send_signal_to_targets(
            process_signal_targets(child_process_group, master),
            SIGKILL);

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_child_process_group == child_process_group) {
            m_child_process_group = -1;
        }
    }

    void termination_escalation_loop(
        pid_t                          child_pid,
        Signal_targets                 targets,
        Terminal_termination_policy    policy)
    {
        (void)child_pid;

        if (!signal_targets_active(targets)) {
            return;
        }

        const std::optional<int> term_error = send_signal_to_targets(targets, SIGTERM);
        if (term_error.has_value()) {
            report_error(
                Terminal_backend_error_code::TERMINATE_FAILED,
                posix_error_message(QStringLiteral("SIGTERM"), *term_error));
            return;
        }

        if (wait_for_signal_targets_exit_observed(targets, policy.graceful_interval)) {
            return;
        }

        const std::optional<int> kill_error = send_signal_to_targets(targets, SIGKILL);
        if (kill_error.has_value()) {
            report_error(
                Terminal_backend_error_code::TERMINATE_FAILED,
                posix_error_message(QStringLiteral("SIGKILL"), *kill_error));
            return;
        }

        if (wait_for_signal_targets_exit_observed(targets, policy.kill_interval)) {
            return;
        }

        report_error(
            Terminal_backend_error_code::TERMINATE_FAILED,
            QStringLiteral("Linux PTY process remained active after forced termination"));
    }

    void join_threads()
    {
        join_or_detach_native_backend_thread(m_reader_thread);
        join_or_detach_native_backend_thread(m_writer_thread);
        join_or_detach_native_backend_thread(m_wait_thread);
        join_or_detach_native_backend_thread(m_termination_thread);
    }

    std::mutex                          m_mutex;
    std::condition_variable             m_output_cv;
    std::condition_variable             m_write_cv;
    std::condition_variable             m_reader_cv;
    Terminal_backend_callbacks          m_callbacks;
    Unique_fd                           m_master;
    Unique_fd                           m_read_wake_read;
    Unique_fd                           m_read_wake_write;
    Unique_fd                           m_write_wake_read;
    Unique_fd                           m_write_wake_write;
    std::thread                         m_reader_thread;
    std::thread                         m_writer_thread;
    std::thread                         m_wait_thread;
    std::thread                         m_termination_thread;
    QByteArray                          m_paused_output;
    std::deque<Queued_write>            m_write_queue;
    std::optional<Terminal_exit_reason> m_exit_reason_override;
    Terminal_termination_policy         m_termination_policy;
    std::size_t                         m_queued_write_bytes = 0U;
    pid_t                               m_child_pid = -1;
    pid_t                               m_child_process_group = -1;
    bool                                m_running = false;
    bool                                m_start_attempted = false;
    bool                                m_start_in_progress = false;
    bool                                m_process_stopping = false;
    bool                                m_stopping = false;
    bool                                m_output_paused = false;
    bool                                m_paused_output_delivery_in_progress = false;
    bool                                m_exit_reported = false;
    bool                                m_shutdown_started = false;
    bool                                m_reader_finished = false;
    bool                                m_reader_timed_out_after_child_reap = false;
    bool                                m_writer_failed = false;
    bool                                m_child_reaped = false;
};

Linux_pty_backend::Linux_pty_backend()
:
    m_impl(std::make_unique<Impl>())
{}

Linux_pty_backend::~Linux_pty_backend()
{
    if (m_impl && m_impl->is_worker_thread()) {
        Impl* impl = m_impl.release();
        impl->defer_shutdown_and_delete();
    }
}

Terminal_backend_result Linux_pty_backend::start(
    const Terminal_launch_config&  config,
    Terminal_backend_callbacks     callbacks)
{
    return m_impl->start(config, std::move(callbacks));
}

Terminal_backend_result Linux_pty_backend::write(QByteArray bytes)
{
    return m_impl->write(std::move(bytes));
}

Terminal_backend_result Linux_pty_backend::resize(
    Terminal_backend_resize_request request)
{
    return m_impl->resize(request);
}

Terminal_backend_result Linux_pty_backend::set_output_paused(bool paused)
{
    return m_impl->set_output_paused(paused);
}

Terminal_backend_result Linux_pty_backend::interrupt()
{
    return m_impl->interrupt();
}

Terminal_backend_result Linux_pty_backend::terminate()
{
    return m_impl->terminate();
}

std::unique_ptr<Terminal_backend> make_linux_pty_backend()
{
    return std::make_unique<Linux_pty_backend>();
}

}

#endif
