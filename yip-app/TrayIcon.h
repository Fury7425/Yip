#pragma once

// Notification-area icon.
//
// The icon hangs off its own hidden top-level window, not off MainWindow's
// HWND. Two reasons: the shell addresses an icon through the window that
// registered it, so a window the user can close is the wrong owner; and
// `TaskbarCreated` is an HWND_BROADCAST message, which reaches hidden
// top-level windows but never a message-only (HWND_MESSAGE) one. Missing that
// broadcast is the usual reason a tray icon is absent after explorer restarts.
//
// The host window is also the address a second instance posts to — see
// [SingleInstance.h](SingleInstance.h).

#include <functional>

#include <windows.h>

namespace yip {
class TrayIcon {
public:
    struct Callbacks {
        std::function<void()> onShow;             // left click, or "Open Yip"
        std::function<void()> onToggleRecording;  // menu
        std::function<void()> onExit;             // menu
    };

    explicit TrayIcon(Callbacks callbacks);
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // Swap glyph and tooltip. Safe to call before the icon is on screen.
    void SetRecording(bool recording);

    // True once the shell has accepted the icon.
    bool IsLive() const noexcept { return m_added; }

    // Class name of the hidden host window. A second instance finds the window
    // by it, so the string is part of the contract between the two processes.
    static const wchar_t* HostClassName() noexcept;

    // Registered message meaning "you own this session, show yourself".
    // Registered from the same string in every process, so both sides agree on
    // the value without sharing anything else.
    static UINT ActivateMessage();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool AddIcon();
    void RemoveIcon();
    void ApplyIconState();
    void ShowMenu(POINT anchor);
    HICON CurrentIcon() const noexcept;
    const wchar_t* CurrentTip() const noexcept;

    Callbacks m_callbacks;
    HWND m_host{nullptr};
    HICON m_iconIdle{nullptr};
    HICON m_iconRecording{nullptr};
    bool m_added{false};
    bool m_recording{false};
    int m_addAttempts{0};
};
} // namespace yip
