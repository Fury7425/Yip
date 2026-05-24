#pragma once

// Persisted indicator state: dock edge, monitor anchor, click-through toggle,
// last expanded-or-collapsed preference. JSON at
// %LOCALAPPDATA%\Yip\indicator.json.

#include "IndicatorState.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace yip {
struct IndicatorPersistence {
    DockEdge dock_edge{DockEdge::Top};
    // Stable monitor key from Microsoft.UI.Windowing.DisplayId.Value.
    uint64_t monitor_id{0};
    // Position within the docked edge (0.0 = top/left, 1.0 = bottom/right).
    double edge_offset{0.5};
    bool click_through{false};
    bool last_expanded{false};

    static std::filesystem::path FilePath();
    static IndicatorPersistence Load();
    bool Save() const;
};
} // namespace yip
