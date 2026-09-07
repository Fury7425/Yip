#pragma once

// Fan-out for the audio-core recording-state callback.
//
// `rec_set_state_callback` has exactly one slot, but both MainWindow and
// IndicatorWindow need the signal. This bus owns the single registration and
// dispatches to each subscriber on the DispatcherQueue it subscribed with, so
// handlers always run on their own UI thread.
//
// Replacing the old 5 Hz / 60 Hz poll timers is the point: with no timers
// ticking, an idle Yip issues no FFI calls at all.

#include <cstdint>
#include <functional>

#include <winrt/Microsoft.UI.Dispatching.h>

namespace yip {
class RecordingStateBus {
public:
    using Token = uint64_t;

    // Register `handler`, invoked with `true` when capture starts and `false`
    // when it stops. Never fires for the current state — read IsRecording()
    // once at subscribe time if the initial value matters.
    static Token Subscribe(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue,
                           std::function<void(bool)> handler);

    // Drop a subscription. Safe with a stale or zero token.
    static void Unsubscribe(Token token);

    // Live state straight from audio-core.
    static bool IsRecording() noexcept;
};
} // namespace yip
