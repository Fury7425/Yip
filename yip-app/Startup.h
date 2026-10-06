#pragma once

// "Start Yip when I sign in": a per-user value under HKCU\...\Run. The registry
// is the source of truth, not settings.json, so the toggle can never drift
// from what Windows will really do. `--background` keeps the main window shut
// and leaves Yip in the notification area.

#include <string>

namespace yip::startup {
inline constexpr wchar_t kBackgroundArg[] = L"--background";

namespace detail {
inline constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kValue[] = L"Yip";
} // namespace detail

inline bool IsEnabled()
{
    return ::RegGetValueW(HKEY_CURRENT_USER, detail::kRunKey, detail::kValue, RRF_RT_REG_SZ, nullptr, nullptr,
                          nullptr) == ERROR_SUCCESS;
}

inline bool SetEnabled(bool enabled)
{
    if (!enabled) {
        const auto rc = ::RegDeleteKeyValueW(HKEY_CURRENT_USER, detail::kRunKey, detail::kValue);
        return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
    }
    wchar_t exe[MAX_PATH];
    const auto n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const std::wstring cmd = std::wstring{L"\""} + exe + L"\" " + kBackgroundArg;
    return ::RegSetKeyValueW(HKEY_CURRENT_USER, detail::kRunKey, detail::kValue, REG_SZ, cmd.c_str(),
                             static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
} // namespace yip::startup
