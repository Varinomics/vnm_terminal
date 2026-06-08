#pragma once

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QMetaEnum>
#include <QString>
#include <QtGlobal>

namespace vnm_terminal::terminal_app {

constexpr int k_exit_usage_error            = 2;
constexpr int k_exit_start_failed           = 3;
constexpr int k_exit_process_failed         = 4;
constexpr int k_exit_timeout                = 5;
constexpr int k_exit_no_output              = 6;
constexpr int k_timeout_force_exit_grace_ms = 5000;

constexpr qreal k_text_area_resize_max_window_axis = 8192.0;

struct Runtime_state
{
    int                backend_error_count = 0;
    int                process_exit_code   = 0;
    qint64             first_output_elapsed_ms = -1;
    VNM_TerminalSurface::Exit_reason process_exit_reason =
        VNM_TerminalSurface::Exit_reason::EXITED;
    bool               output_seen     = false;
    bool               process_exited  = false;
    bool               timeout_expired = false;
};

struct metrics_timing_t
{
    qint64             app_elapsed_ms           = 0;
    qint64             profile_write_elapsed_ms = 0;
    bool               profile_text_requested   = false;
};

void print_error(const QString& message);

template <typename T>
QString enum_key(T value)
{
    const QMetaEnum meta = QMetaEnum::fromType<T>();
    const char*     key  = meta.valueToKey(static_cast<int>(value));
    if (key != nullptr) {
        return QString::fromLatin1(key);
    }

    return QString::number(static_cast<int>(value));
}

} // namespace vnm_terminal::terminal_app
