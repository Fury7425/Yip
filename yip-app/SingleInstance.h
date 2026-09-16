#pragma once

// One Yip per session.
//
// A second copy would fight the first over the capture device, the global
// hotkey (RegisterHotKey is first come, first served) and settings.json, and
// would put a second icon in the notification area. The loser of the race hands
// the session to the winner and exits before any of that exists.

#include <windows.h>

namespace yip {
class SingleInstance {
public:
    SingleInstance();
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    // False when another Yip already holds the name — this process must exit.
    bool IsPrimary() const noexcept { return m_primary; }

private:
    HANDLE m_mutex{nullptr};
    bool m_primary{false};
};

// Ask the Yip that owns the session to come forward, then let the caller exit.
// Best effort: if the winner is still starting and has no host window yet the
// launch is simply swallowed. What must not happen is a second recorder.
void ActivateRunningInstance();
} // namespace yip
