#pragma once

#include <QtGlobal>

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>

class QQuickWindow;

#if QT_VERSION >= QT_VERSION_CHECK(5, 2, 0)
#define VNM_TERMINAL_PRESENTATION_HAS_AFTER_SYNCHRONIZING 1
#else
#define VNM_TERMINAL_PRESENTATION_HAS_AFTER_SYNCHRONIZING 0
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
#define VNM_TERMINAL_PRESENTATION_HAS_RENDER_PASS_RECORDING 1
#else
#define VNM_TERMINAL_PRESENTATION_HAS_RENDER_PASS_RECORDING 0
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#define VNM_TERMINAL_PRESENTATION_HAS_FRAME_BOUNDARIES 1
#else
#define VNM_TERMINAL_PRESENTATION_HAS_FRAME_BOUNDARIES 0
#endif

namespace vnm_terminal::terminal_app {

enum class Presentation_signal
{
    FRAME_SWAPPED,
    BEFORE_FRAME_BEGIN,
    BEFORE_SYNCHRONIZING,
    AFTER_SYNCHRONIZING,
    BEFORE_RENDERING,
    AFTER_RENDERING,
    BEFORE_RENDER_PASS_RECORDING,
    AFTER_RENDER_PASS_RECORDING,
    AFTER_FRAME_END,
    COUNT,
};

struct presentation_signal_metrics_t
{
    std::uint64_t count            = 0U;
    std::uint64_t last_interval_ns = 0U;
    std::uint64_t max_interval_ns  = 0U;
};

struct presentation_metrics_snapshot_t
{
    std::array<
        presentation_signal_metrics_t,
        static_cast<std::size_t>(Presentation_signal::COUNT)
    > stages;
};

class Presentation_metrics_recorder
{
public:
    void record(Presentation_signal signal);
    void record_for_testing(
        Presentation_signal                         signal,
        std::chrono::steady_clock::time_point       timestamp);

    presentation_metrics_snapshot_t snapshot() const;

private:
    struct Signal_state
    {
        presentation_signal_metrics_t          metrics;
        std::chrono::steady_clock::time_point  last_timestamp;
        bool                                   has_timestamp = false;
    };

    void record_at(
        Presentation_signal                         signal,
        std::chrono::steady_clock::time_point       timestamp);

    mutable std::mutex m_mutex;
    std::array<
        Signal_state,
        static_cast<std::size_t>(Presentation_signal::COUNT)
    > m_signals;
};

bool presentation_signal_available(Presentation_signal signal);
const char* presentation_signal_json_key(Presentation_signal signal);

void connect_presentation_metrics_recorder(
    QQuickWindow&                   window,
    Presentation_metrics_recorder&  recorder);

} // namespace vnm_terminal::terminal_app
