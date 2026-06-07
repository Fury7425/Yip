#include "pch.h"
#include "HotkeyManager.h"

#include <commctrl.h>

namespace
{
    constexpr int      kHotkeyId   = 0xB001;  // arbitrary unique id
    constexpr UINT_PTR kSubclassId = 0xA17;
}

namespace yip
{
    HotkeyManager::HotkeyManager(HWND hwnd, std::function<void()> onTrigger)
        : m_hwnd(hwnd), m_onTrigger(std::move(onTrigger))
    {
        if (m_hwnd) {
            // Stash `this` as subclass ref data so the static proc can route.
            if (::SetWindowSubclass(m_hwnd, &HotkeyManager::SubclassProc, kSubclassId,
                                    reinterpret_cast<DWORD_PTR>(this))) {
                m_subclassed = true;
            }
        }
    }

    HotkeyManager::~HotkeyManager()
    {
        Unregister();
        if (m_subclassed && m_hwnd) {
            ::RemoveWindowSubclass(m_hwnd, &HotkeyManager::SubclassProc, kSubclassId);
            m_subclassed = false;
        }
    }

    bool HotkeyManager::Register(uint32_t mods, uint32_t vk)
    {
        Unregister();
        if (!m_hwnd) return false;
        // MOD_NOREPEAT (0x4000) so holding the keys fires once.
        const UINT flags = static_cast<UINT>(mods) | 0x4000u;
        if (::RegisterHotKey(m_hwnd, kHotkeyId, flags, vk)) {
            m_registered = true;
            return true;
        }
        return false;
    }

    void HotkeyManager::Unregister()
    {
        if (m_registered && m_hwnd) {
            ::UnregisterHotKey(m_hwnd, kHotkeyId);
            m_registered = false;
        }
    }

    LRESULT CALLBACK HotkeyManager::SubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                                 LPARAM lParam, UINT_PTR /*id*/,
                                                 DWORD_PTR refData)
    {
        if (msg == WM_HOTKEY && wParam == kHotkeyId) {
            auto* self = reinterpret_cast<HotkeyManager*>(refData);
            if (self && self->m_onTrigger) self->m_onTrigger();
            return 0;
        }
        return ::DefSubclassProc(hwnd, msg, wParam, lParam);
    }
}
