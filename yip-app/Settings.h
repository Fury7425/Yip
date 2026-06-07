#pragma once

// Plain C++ settings model. Persisted as JSON at
// %LOCALAPPDATA%\Yip\settings.json. Not a WinRT runtimeclass — the dialog
// reads/writes individual fields directly.

#include <filesystem>
#include <string>

namespace yip {
struct Settings {
    std::filesystem::path output_folder;
    uint32_t sample_rate{48000};
    uint16_t channels{2};

    // 0 = PCM float32 (only format in v1; reserved for future expansion).
    uint16_t format{0};

    // Global start/stop hotkey. Modifiers are the Win32 MOD_* bitmask
    // (MOD_ALT=0x1, MOD_CONTROL=0x2, MOD_SHIFT=0x4, MOD_WIN=0x8); vk is a
    // virtual-key code. Default: Ctrl+Alt+R.
    uint32_t hotkey_mods{0x2 | 0x1};
    uint32_t hotkey_vk{0x52};  // 'R'

    // Returns the path to the settings file, creating parent dirs.
    static std::filesystem::path SettingsPath();

    // Default-construct a settings object pointing at the user's
    // Music\Yip folder.
    static Settings Defaults();

    // Best-effort load. Falls back to Defaults() on any error.
    static Settings Load();

    // Atomic save (write tmp + rename). Returns false on failure.
    bool Save() const;
};
} // namespace yip
