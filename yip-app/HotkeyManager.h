#pragma once

// Global start/stop hotkey. Wraps RegisterHotKey on a target HWND and routes
// WM_HOTKEY to a callback via a window subclass. The callback runs on the
// window's UI thread, so it can touch the view model directly.

#include <functional>
#include <string>

#include <windows.h>

namespace yip
{
    // Win32 MOD_* values, spelled out so callers do not have to include
    // winuser.h just to build a combo.
    inline constexpr uint32_t kModAlt     = 0x0001;
    inline constexpr uint32_t kModControl = 0x0002;
    inline constexpr uint32_t kModShift   = 0x0004;
    inline constexpr uint32_t kModWin     = 0x0008;

    // Human-readable combo, e.g. "Ctrl + Alt + R". Returns "None" when `vk` is
    // zero. Modifier order is fixed so the label never reshuffles.
    std::wstring FormatHotkey(uint32_t mods, uint32_t vk);

    // True when the combo is registrable: at least one modifier and a key that
    // is not itself a modifier. RegisterHotKey accepts more, but a bare letter
    // would swallow typing system-wide.
    bool IsValidHotkey(uint32_t mods, uint32_t vk) noexcept;

    class HotkeyManager
    {
    public:
        HotkeyManager(HWND hwnd, std::function<void()> onTrigger);
        ~HotkeyManager();
        HotkeyManager(const HotkeyManager&)            = delete;
        HotkeyManager& operator=(const HotkeyManager&) = delete;

        // (Re)register the combo. mods = Win32 MOD_* bitmask, vk = virtual key.
        // Returns false if the combo is already owned by another app.
        bool Register(uint32_t mods, uint32_t vk);
        void Unregister();

    private:
        static LRESULT CALLBACK SubclassProc(HWND, UINT, WPARAM, LPARAM,
                                             UINT_PTR, DWORD_PTR);

        HWND m_hwnd{ nullptr };
        std::function<void()> m_onTrigger;
        bool m_registered{ false };
        bool m_subclassed{ false };
    };
}
