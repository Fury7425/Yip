#pragma once

// Global start/stop hotkey. Wraps RegisterHotKey on a target HWND and routes
// WM_HOTKEY to a callback via a window subclass. The callback runs on the
// window's UI thread, so it can touch the view model directly.

#include <functional>

#include <windows.h>

namespace yip
{
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
