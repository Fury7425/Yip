#pragma once

// Persisted indicator state: click-through toggle, last expanded-or-collapsed
// preference. JSON at %LOCALAPPDATA%\Yip\indicator.json. The pill's position
// is not among them — it is pinned to the top centre of the primary display.

#include <filesystem>
#include <string>

namespace yip {
struct IndicatorPersistence {
    bool click_through{false};
    bool last_expanded{false};

    static std::filesystem::path FilePath();
    static IndicatorPersistence Load();
    bool Save() const;
};
} // namespace yip
