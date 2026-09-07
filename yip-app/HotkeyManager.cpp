#include "pch.h"
#include "HotkeyManager.h"

#include <commctrl.h>

#include <array>

namespace
{
    constexpr int      kHotkeyId   = 0xB001;  // arbitrary unique id
    constexpr UINT_PTR kSubclassId = 0xA17;
}

namespace yip
{
    std::wstring FormatHotkey(uint32_t mods, uint32_t vk)
    {
        if (vk == 0) return L"None";

        std::wstring out;
        auto add = [&out](const wchar_t* part) {
            if (!out.empty()) out += L" + ";
            out += part;
        };
        if (mods & kModControl) add(L"Ctrl");
        if (mods & kModAlt) add(L"Alt");
        if (mods & kModShift) add(L"Shift");
        if (mods & kModWin) add(L"Win");

        // MapVirtualKey gives the layout-correct name for letters, digits and
        // most punctuation; the named keys below have no printable form.
        std::wstring key;
        switch (vk) {
            case VK_SPACE:  key = L"Space"; break;
            case VK_RETURN: key = L"Enter"; break;
            case VK_ESCAPE: key = L"Esc"; break;
            case VK_TAB:    key = L"Tab"; break;
            case VK_BACK:   key = L"Backspace"; break;
            case VK_DELETE: key = L"Delete"; break;
            case VK_INSERT: key = L"Insert"; break;
            case VK_HOME:   key = L"Home"; break;
            case VK_END:    key = L"End"; break;
            case VK_PRIOR:  key = L"Page Up"; break;
            case VK_NEXT:   key = L"Page Down"; break;
            case VK_LEFT:   key = L"Left"; break;
            case VK_RIGHT:  key = L"Right"; break;
            case VK_UP:     key = L"Up"; break;
            case VK_DOWN:   key = L"Down"; break;
            default: {
                if (vk >= VK_F1 && vk <= VK_F24) {
                    key = L"F" + std::to_wstring(vk - VK_F1 + 1);
                    break;
                }
                const UINT ch = ::MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFFFFFFu;
                if (ch >= 0x20) {
                    key.assign(1, static_cast<wchar_t>(ch));
                } else {
                    std::array<wchar_t, 16> buf{};
                    swprintf_s(buf.data(), buf.size(), L"0x%02X", vk);
                    key = buf.data();
                }
                break;
            }
        }

        if (!out.empty()) out += L" + ";
        out += key;
        return out;
    }

    bool IsValidHotkey(uint32_t mods, uint32_t vk) noexcept
    {
        if (vk == 0) return false;
        if ((mods & (kModAlt | kModControl | kModShift | kModWin)) == 0) return false;
        switch (vk) {
            case VK_SHIFT:
            case VK_CONTROL:
            case VK_MENU:
            case VK_LWIN:
            case VK_RWIN:
            case VK_LSHIFT:
            case VK_RSHIFT:
            case VK_LCONTROL:
            case VK_RCONTROL:
            case VK_LMENU:
            case VK_RMENU:
                return false;
            default:
                return true;
        }
    }

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
