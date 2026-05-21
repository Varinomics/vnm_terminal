#pragma once

#include "vnm_terminal/internal/metrics_contract.h"
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace vnm_terminal::internal {

enum class Terminal_environment_operation
{
    SET,
    UNSET,
};

enum class Terminal_process_state
{
    NOT_STARTED,
    STARTING,
    RUNNING,
    EXITED,
    FAILED,
};

enum class Terminal_exit_reason
{
    EXITED,
    INTERRUPTED,
    TERMINATED,
    FAILED_TO_START,
};

enum class Terminal_backend_error_code
{
    INVALID_LAUNCH_CONFIG,
    INVALID_INITIAL_GRID_SIZE,
    WORKING_DIRECTORY_UNAVAILABLE,
    START_FAILED,
    WRITE_FAILED,
    RESIZE_FAILED,
    INTERRUPT_FAILED,
    TERMINATE_FAILED,
    OUTPUT_OVERFLOW,
    CALLBACK_MISSING,
    READ_FAILED,
};

enum class Terminal_backend_result_code
{
    ACCEPTED,
    REJECTED,
};

enum class Terminal_process_group_policy
{
    BACKEND_DEFAULT,
    CREATE_NEW_SESSION,
};

struct Terminal_environment_edit
{
    Terminal_environment_operation         operation = Terminal_environment_operation::SET;
    QString                                name;
    QString                                value;
};

struct Terminal_identity
{
    QString                                term      = QStringLiteral("xterm-256color");
    QString                                colorterm = QStringLiteral("truecolor");
};

struct Terminal_termination_policy
{
    std::chrono::milliseconds              graceful_interval = std::chrono::milliseconds(1500);
    std::chrono::milliseconds              kill_interval     = std::chrono::milliseconds(500);
};

struct Terminal_launch_config
{
    QStringList                            argv;
    QString                                working_directory;
    bool                                   inherit_environment = true;
    std::vector<Terminal_environment_edit> environment_edits;
    Terminal_identity                      identity;
    std::optional<terminal_grid_size_t>    initial_grid_size;
    Terminal_process_group_policy          process_group_policy =
        Terminal_process_group_policy::BACKEND_DEFAULT;
    Terminal_termination_policy            termination_policy;
};

struct Terminal_effective_launch_config
{
    QStringList                            argv;
    QString                                working_directory;
    QProcessEnvironment                    environment;
    terminal_grid_size_t                   initial_grid_size;
    Terminal_process_group_policy process_group_policy =
        Terminal_process_group_policy::BACKEND_DEFAULT;
    Terminal_termination_policy            termination_policy;
};

struct Terminal_backend_error
{
    Terminal_backend_error_code            code = Terminal_backend_error_code::START_FAILED;
    QString                                message;
};

struct Terminal_backend_result
{
    Terminal_backend_result_code           code = Terminal_backend_result_code::ACCEPTED;
    std::optional<Terminal_backend_error>  error;
};

struct Terminal_backend_resize_request
{
    std::uint64_t                          transaction_id = 0U;
    terminal_grid_size_t                   grid_size;
};

struct Terminal_backend_exit
{
    Terminal_exit_reason                   reason    = Terminal_exit_reason::EXITED;
    int                                    exit_code = 0;
};

struct Terminal_backend_callbacks
{
    std::function<void(QByteArray)>                output_received;
    std::function<void(Terminal_backend_exit)>     process_exited;
    std::function<void(Terminal_backend_error)>    error_reported;
};

inline Terminal_backend_result backend_accept()
{
    return {};
}

inline Terminal_backend_result backend_reject(
    Terminal_backend_error_code    code,
    QString                        message)
{
    return {
        Terminal_backend_result_code::REJECTED,
        Terminal_backend_error{code, std::move(message)},
    };
}

inline bool is_valid_backend_result(const Terminal_backend_result& result)
{
    if (result.code == Terminal_backend_result_code::ACCEPTED) {
        return !result.error.has_value();
    }

    return result.error.has_value();
}

inline bool is_backend_rejection(const Terminal_backend_result& result)
{
    return result.code == Terminal_backend_result_code::REJECTED && result.error.has_value();
}

inline bool is_valid_environment_name(const QString& name)
{
    return !name.isEmpty() && !name.contains(QChar(u'=')) && !name.contains(QChar(u'\0'));
}

inline bool is_valid_grid_size(terminal_grid_size_t grid_size)
{
    return grid_size.rows > 0 && grid_size.columns > 0;
}

inline Terminal_backend_result validate_launch_config(
    const Terminal_launch_config& config)
{
    if (config.argv.isEmpty() || config.argv.front().trimmed().isEmpty()) {
        return
            backend_reject(
                Terminal_backend_error_code::INVALID_LAUNCH_CONFIG,
                QStringLiteral("argv must name an executable"));
    }

    if (!config.initial_grid_size.has_value() ||
        !is_valid_grid_size(*config.initial_grid_size))
    {
        return
            backend_reject(
                Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
                QStringLiteral("initial terminal size must be positive before start"));
    }

    if (config.identity.term.trimmed().isEmpty()) {
        return
            backend_reject(
                Terminal_backend_error_code::INVALID_LAUNCH_CONFIG,
                QStringLiteral("TERM identity must be non-empty"));
    }

    for (const Terminal_environment_edit& edit : config.environment_edits) {
        if (!is_valid_environment_name(edit.name)) {
            return
                backend_reject(
                    Terminal_backend_error_code::INVALID_LAUNCH_CONFIG,
                    QStringLiteral("environment edit names must be non-empty variables"));
        }

        if (edit.name == QStringLiteral("TERM") &&
            (edit.operation == Terminal_environment_operation::UNSET ||
             edit.value.trimmed().isEmpty()))
        {
            return
                backend_reject(
                    Terminal_backend_error_code::INVALID_LAUNCH_CONFIG,
                    QStringLiteral("TERM must remain present in the launch environment"));
        }
    }

    return backend_accept();
}

inline QProcessEnvironment build_launch_environment(
    const Terminal_launch_config&  config,
    const QProcessEnvironment&     inherited)
{
    QProcessEnvironment environment =
        config.inherit_environment ? inherited : QProcessEnvironment();

    environment.remove(QStringLiteral("NO_COLOR"));
    environment.insert(QStringLiteral("TERM"), config.identity.term);
    if (config.identity.colorterm.isEmpty()) {
        environment.remove(QStringLiteral("COLORTERM"));
    }
    else {
        environment.insert(QStringLiteral("COLORTERM"), config.identity.colorterm);
    }

    for (const Terminal_environment_edit& edit : config.environment_edits) {
        if (edit.operation == Terminal_environment_operation::SET) {
            environment.insert(edit.name, edit.value);
        }
        else {
            environment.remove(edit.name);
        }
    }

    if (environment.value(QStringLiteral("TERM")).trimmed().isEmpty()) {
        environment.insert(QStringLiteral("TERM"), config.identity.term);
    }

    return environment;
}

inline std::optional<Terminal_effective_launch_config> make_effective_launch_config(
    const Terminal_launch_config&  config,
    const QProcessEnvironment&     inherited)
{
    if (is_backend_rejection(validate_launch_config(config))) {
        return std::nullopt;
    }

    return Terminal_effective_launch_config{
        config.argv,
        config.working_directory,
        build_launch_environment(config, inherited),
        *config.initial_grid_size,
        config.process_group_policy,
        config.termination_policy,
    };
}

inline Terminal_backend_result validate_backend_callbacks(
    const Terminal_backend_callbacks& callbacks)
{
    if (!callbacks.output_received || !callbacks.process_exited || !callbacks.error_reported) {
        return
            backend_reject(
                Terminal_backend_error_code::CALLBACK_MISSING,
                QStringLiteral("backend callbacks must all be provided"));
    }

    return backend_accept();
}

class Terminal_backend
{
public:
    virtual ~Terminal_backend() = default;

    virtual Terminal_backend_result start(
        const Terminal_launch_config&   config,
        Terminal_backend_callbacks      callbacks) = 0;

    virtual Terminal_backend_result write(
        QByteArray                      bytes) = 0;

    virtual Terminal_backend_result resize(
        Terminal_backend_resize_request request) = 0;

    virtual Terminal_backend_result set_output_paused(
        bool                            paused) = 0;

    virtual Terminal_backend_result interrupt() = 0;
    virtual Terminal_backend_result terminate() = 0;
};

}
