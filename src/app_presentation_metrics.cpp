#include "app_presentation_metrics.h"

#include <QObject>
#include <QQuickWindow>

#include <algorithm>

namespace vnm_terminal::terminal_app {

namespace {

constexpr std::size_t presentation_signal_index(Presentation_signal signal)
{
    return static_cast<std::size_t>(signal);
}

template<typename Signal>
void connect_presentation_signal(
    QQuickWindow&                   window,
    Presentation_metrics_recorder&  recorder,
    Signal                          signal,
    Presentation_signal             presentation_signal)
{
    QObject::connect(
        &window,
        signal,
        &window,
        [&recorder, presentation_signal] {
            recorder.record(presentation_signal);
        },
        Qt::DirectConnection);
}

} // namespace

void Presentation_metrics_recorder::record(Presentation_signal signal)
{
    record_at(signal, std::chrono::steady_clock::now());
}

void Presentation_metrics_recorder::record_for_testing(
    Presentation_signal                    signal,
    std::chrono::steady_clock::time_point  timestamp)
{
    record_at(signal, timestamp);
}

presentation_metrics_snapshot_t Presentation_metrics_recorder::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    presentation_metrics_snapshot_t out;
    for (std::size_t index = 0U; index < m_signals.size(); ++index) {
        out.stages[index] = m_signals[index].metrics;
    }
    return out;
}

void Presentation_metrics_recorder::record_at(
    Presentation_signal                    signal,
    std::chrono::steady_clock::time_point  timestamp)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    Signal_state& state = m_signals[presentation_signal_index(signal)];
    ++state.metrics.count;
    if (state.has_timestamp) {
        const auto interval = timestamp - state.last_timestamp;
        const auto interval_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::max(interval, std::chrono::steady_clock::duration::zero()));
        state.metrics.last_interval_ns = static_cast<std::uint64_t>(interval_ns.count());
        state.metrics.max_interval_ns =
            std::max(state.metrics.max_interval_ns, state.metrics.last_interval_ns);
    }

    state.last_timestamp = timestamp;
    state.has_timestamp  = true;
}

bool presentation_signal_available(Presentation_signal signal)
{
    switch (signal) {
        case Presentation_signal::FRAME_SWAPPED:
        case Presentation_signal::BEFORE_SYNCHRONIZING:
        case Presentation_signal::BEFORE_RENDERING:
        case Presentation_signal::AFTER_RENDERING:
            return true;
        case Presentation_signal::AFTER_SYNCHRONIZING:
            return VNM_TERMINAL_PRESENTATION_HAS_AFTER_SYNCHRONIZING;
        case Presentation_signal::BEFORE_RENDER_PASS_RECORDING:
        case Presentation_signal::AFTER_RENDER_PASS_RECORDING:
            return VNM_TERMINAL_PRESENTATION_HAS_RENDER_PASS_RECORDING;
        case Presentation_signal::BEFORE_FRAME_BEGIN:
        case Presentation_signal::AFTER_FRAME_END:
            return VNM_TERMINAL_PRESENTATION_HAS_FRAME_BOUNDARIES;
        case Presentation_signal::COUNT:
            return false;
    }

    return false;
}

const char* presentation_signal_json_key(Presentation_signal signal)
{
    switch (signal) {
        case Presentation_signal::FRAME_SWAPPED:
            return "frameSwapped";
        case Presentation_signal::BEFORE_FRAME_BEGIN:
            return "beforeFrameBegin";
        case Presentation_signal::BEFORE_SYNCHRONIZING:
            return "beforeSynchronizing";
        case Presentation_signal::AFTER_SYNCHRONIZING:
            return "afterSynchronizing";
        case Presentation_signal::BEFORE_RENDERING:
            return "beforeRendering";
        case Presentation_signal::AFTER_RENDERING:
            return "afterRendering";
        case Presentation_signal::BEFORE_RENDER_PASS_RECORDING:
            return "beforeRenderPassRecording";
        case Presentation_signal::AFTER_RENDER_PASS_RECORDING:
            return "afterRenderPassRecording";
        case Presentation_signal::AFTER_FRAME_END:
            return "afterFrameEnd";
        case Presentation_signal::COUNT:
            return "unknown";
    }

    return "unknown";
}

void connect_presentation_metrics_recorder(
    QQuickWindow&                   window,
    Presentation_metrics_recorder&  recorder)
{
    connect_presentation_signal(
        window,
        recorder,
        &QQuickWindow::frameSwapped,
        Presentation_signal::FRAME_SWAPPED);

#if VNM_TERMINAL_PRESENTATION_HAS_FRAME_BOUNDARIES
    connect_presentation_signal(
        window,
        recorder,
        &QQuickWindow::beforeFrameBegin,
        Presentation_signal::BEFORE_FRAME_BEGIN);
#endif

    connect_presentation_signal(
        window,
        recorder,
        &QQuickWindow::beforeSynchronizing,
        Presentation_signal::BEFORE_SYNCHRONIZING);

#if VNM_TERMINAL_PRESENTATION_HAS_AFTER_SYNCHRONIZING
    connect_presentation_signal(
        window,
        recorder,
        &QQuickWindow::afterSynchronizing,
        Presentation_signal::AFTER_SYNCHRONIZING);
#endif

    connect_presentation_signal(
        window,
        recorder,
        &QQuickWindow::beforeRendering,
        Presentation_signal::BEFORE_RENDERING);
    connect_presentation_signal(
        window,
        recorder,
        &QQuickWindow::afterRendering,
        Presentation_signal::AFTER_RENDERING);

#if VNM_TERMINAL_PRESENTATION_HAS_RENDER_PASS_RECORDING
    connect_presentation_signal(
        window,
        recorder,
        &QQuickWindow::beforeRenderPassRecording,
        Presentation_signal::BEFORE_RENDER_PASS_RECORDING);
    connect_presentation_signal(
        window,
        recorder,
        &QQuickWindow::afterRenderPassRecording,
        Presentation_signal::AFTER_RENDER_PASS_RECORDING);
#endif

#if VNM_TERMINAL_PRESENTATION_HAS_FRAME_BOUNDARIES
    connect_presentation_signal(
        window,
        recorder,
        &QQuickWindow::afterFrameEnd,
        Presentation_signal::AFTER_FRAME_END);
#endif
}

} // namespace vnm_terminal::terminal_app
