#include "pch.h"
#include "SingleInstance.h"

#include "TrayIcon.h"

namespace {

// Session-local on purpose: fast user switching and a second RDP session each
// get their own Yip, which is what a per-user recorder wants. A Global\ name
// would lock the second user out of the app entirely.
constexpr wchar_t kMutexName[] = L"Local\\Yip.SingleInstance.{4F2B9C1E-7A55-4D3E-9E6B-2C8F1A0D5B37}";

// The winner may still be starting when the loser goes looking for it, so the
// handover is retried for ~2s before the loser quietly gives up and exits.
constexpr int kFindAttempts = 20;
constexpr DWORD kFindIntervalMs = 100;

} // namespace

namespace yip {

SingleInstance::SingleInstance()
{
    // Ownership is not taken: only the *name* matters, and a name lives as long
    // as a handle to it is open. Not owning also keeps a crash from leaving an
    // abandoned mutex behind to reason about.
    m_mutex = ::CreateMutexW(nullptr, FALSE, kMutexName);
    const DWORD error = ::GetLastError();
    m_primary = m_mutex != nullptr && error != ERROR_ALREADY_EXISTS;
}

SingleInstance::~SingleInstance()
{
    if (m_mutex) {
        ::CloseHandle(m_mutex);
        m_mutex = nullptr;
    }
}

void ActivateRunningInstance()
{
    for (int attempt = 0; attempt < kFindAttempts; ++attempt) {
        if (const HWND host = ::FindWindowW(TrayIcon::HostClassName(), nullptr)) {
            // Hand our foreground rights to the winner. Without this its
            // SetForegroundWindow is demoted to a flashing taskbar button,
            // because the shell gave the foreground to *this* process.
            DWORD pid = 0;
            ::GetWindowThreadProcessId(host, &pid);
            ::AllowSetForegroundWindow(pid != 0 ? pid : ASFW_ANY);
            ::PostMessageW(host, TrayIcon::ActivateMessage(), 0, 0);
            return;
        }
        ::Sleep(kFindIntervalMs);
    }
}

} // namespace yip
