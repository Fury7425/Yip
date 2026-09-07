#include "pch.h"
#include "RecordingStateBus.h"

#include <mutex>
#include <vector>

namespace {
struct Subscriber {
    yip::RecordingStateBus::Token token{0};
    winrt::Microsoft::UI::Dispatching::DispatcherQueue queue{nullptr};
    std::function<void(bool)> handler;
};

std::mutex g_mutex;
std::vector<Subscriber> g_subscribers;
yip::RecordingStateBus::Token g_nextToken = 1;
bool g_registered = false;
} // namespace

namespace yip::detail {
// audio-core invokes this on whichever thread called rec_start / rec_stop,
// after releasing its own lock. Copy the subscriber list under g_mutex, then
// dispatch outside it — a handler must never run with g_mutex held, and
// TryEnqueue re-entering Subscribe would otherwise deadlock.
extern "C" void YipOnRecStateChanged(uint8_t recording, void* /*user*/)
{
    const bool isRecording = recording != 0;

    std::vector<Subscriber> snapshot;
    {
        std::lock_guard lock(g_mutex);
        snapshot = g_subscribers;
    }
    for (auto& s : snapshot) {
        if (!s.queue) continue;
        s.queue.TryEnqueue([handler = s.handler, isRecording]() {
            if (handler) handler(isRecording);
        });
    }
}
} // namespace yip::detail

namespace yip {
RecordingStateBus::Token RecordingStateBus::Subscribe(
    winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue, std::function<void(bool)> handler)
{
    std::lock_guard lock(g_mutex);
    const Token token = g_nextToken++;
    g_subscribers.push_back(Subscriber{token, queue, std::move(handler)});
    if (!g_registered) {
        ::rec_set_state_callback(&yip::detail::YipOnRecStateChanged, nullptr);
        g_registered = true;
    }
    return token;
}

void RecordingStateBus::Unsubscribe(Token token)
{
    std::lock_guard lock(g_mutex);
    std::erase_if(g_subscribers, [token](Subscriber const& s) { return s.token == token; });
    if (g_subscribers.empty() && g_registered) {
        ::rec_set_state_callback(nullptr, nullptr);
        g_registered = false;
    }
}

bool RecordingStateBus::IsRecording() noexcept
{
    return ::rec_is_recording() != 0;
}
} // namespace yip
