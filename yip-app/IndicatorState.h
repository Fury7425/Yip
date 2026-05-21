#pragma once

// State machine for the floating recording indicator. Owned by IndicatorWindow.
// Transitions are driven by:
//   * audio-core polling (idle ↔ armed ↔ recording ↔ saving)
//   * user interaction (any → expanded, expanded → previous after 3s)

#include <cstdint>

namespace yip
{
    enum class IndicatorState : uint8_t
    {
        Idle      = 0,  // collapsed, dim 60%
        Armed     = 1,  // hotkey ready, not yet recording (full opacity)
        Recording = 2,  // active capture: timer + GPU level meter
        Saving    = 3,  // brief processing animation while writer flushes
        Expanded  = 4,  // user-revealed controls (overlays any base state)
    };

    enum class DockEdge : uint8_t
    {
        None   = 0,
        Top    = 1,
        Bottom = 2,
        Left   = 3,
        Right  = 4,
    };

    inline const wchar_t* StateName(IndicatorState s) noexcept
    {
        switch (s) {
            case IndicatorState::Idle:      return L"idle";
            case IndicatorState::Armed:     return L"armed";
            case IndicatorState::Recording: return L"recording";
            case IndicatorState::Saving:    return L"saving";
            case IndicatorState::Expanded:  return L"expanded";
        }
        return L"?";
    }
}
