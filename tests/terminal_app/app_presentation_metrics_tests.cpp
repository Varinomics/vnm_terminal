#include "app_presentation_metrics.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace chrome = vnm_terminal::terminal_app;

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

const chrome::presentation_signal_metrics_t& signal_metrics(
    const chrome::presentation_metrics_snapshot_t&  snapshot,
    chrome::Presentation_signal                     signal)
{
    return snapshot.stages[static_cast<std::size_t>(signal)];
}

bool test_recorder_counts_and_intervals()
{
    using namespace std::chrono;

    bool ok = true;
    chrome::Presentation_metrics_recorder recorder;
    const steady_clock::time_point base(steady_clock::duration(1'000'000'000));

    recorder.record_for_testing(chrome::Presentation_signal::FRAME_SWAPPED, base);
    chrome::presentation_metrics_snapshot_t snapshot = recorder.snapshot();
    const chrome::presentation_signal_metrics_t& first_frame =
        signal_metrics(snapshot, chrome::Presentation_signal::FRAME_SWAPPED);
    ok &= check(first_frame.count == 1U, "first frameSwapped sample increments count");
    ok &= check(
        first_frame.last_interval_ns == 0U,
        "first frameSwapped sample has no interval");
    ok &= check(
        first_frame.max_interval_ns == 0U,
        "first frameSwapped sample has no max interval");

    recorder.record_for_testing(
        chrome::Presentation_signal::FRAME_SWAPPED,
        base + milliseconds(16));
    recorder.record_for_testing(
        chrome::Presentation_signal::FRAME_SWAPPED,
        base + milliseconds(50));
    recorder.record_for_testing(
        chrome::Presentation_signal::BEFORE_RENDERING,
        base + milliseconds(51));
    snapshot = recorder.snapshot();

    const chrome::presentation_signal_metrics_t& frame =
        signal_metrics(snapshot, chrome::Presentation_signal::FRAME_SWAPPED);
    const chrome::presentation_signal_metrics_t& before_rendering =
        signal_metrics(snapshot, chrome::Presentation_signal::BEFORE_RENDERING);
    ok &= check(frame.count == 3U, "frameSwapped records all samples");
    ok &= check(
        frame.last_interval_ns == 34'000'000U,
        "frameSwapped last interval records the most recent delta");
    ok &= check(
        frame.max_interval_ns == 34'000'000U,
        "frameSwapped max interval records the largest delta");
    ok &= check(
        before_rendering.count == 1U,
        "beforeRendering has its own independent counter");
    ok &= check(
        before_rendering.last_interval_ns == 0U,
        "first beforeRendering sample has no interval");
    return ok;
}

bool test_signal_metadata_matches_qt_headers()
{
    bool ok = true;
    ok &= check(
        chrome::presentation_signal_available(chrome::Presentation_signal::FRAME_SWAPPED),
        "frameSwapped is always available");
    ok &= check(
        chrome::presentation_signal_available(chrome::Presentation_signal::BEFORE_RENDERING),
        "beforeRendering is always available");
    ok &= check(
        std::string(
            chrome::presentation_signal_json_key(
                chrome::Presentation_signal::AFTER_FRAME_END)) == "afterFrameEnd",
        "afterFrameEnd JSON key is stable");

#if VNM_TERMINAL_PRESENTATION_HAS_FRAME_BOUNDARIES
    ok &= check(
        chrome::presentation_signal_available(chrome::Presentation_signal::AFTER_FRAME_END),
        "afterFrameEnd availability follows Qt frame-boundary signals");
#else
    ok &= check(
        !chrome::presentation_signal_available(chrome::Presentation_signal::AFTER_FRAME_END),
        "afterFrameEnd is omitted when Qt lacks frame-boundary signals");
#endif

#if VNM_TERMINAL_PRESENTATION_HAS_RENDER_PASS_RECORDING
    ok &= check(
        chrome::presentation_signal_available(
            chrome::Presentation_signal::BEFORE_RENDER_PASS_RECORDING),
        "render-pass recording signals follow the local Qt headers");
#else
    ok &= check(
        !chrome::presentation_signal_available(
            chrome::Presentation_signal::BEFORE_RENDER_PASS_RECORDING),
        "render-pass recording signals are omitted when Qt lacks them");
#endif

    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= test_recorder_counts_and_intervals();
    ok &= test_signal_metadata_matches_qt_headers();
    return ok ? 0 : 1;
}
