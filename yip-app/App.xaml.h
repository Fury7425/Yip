#pragma once

// App is not a runtimeclass (no App.idl): the XAML compiler owns AppT, and
// main.cpp instantiates it with make<>. Adding an idl makes cppwinrt emit a
// projected constructor that cannot be built from the XAML base.
#include "App.xaml.g.h"
#include "RecordingStateBus.h"

#include <memory>

namespace yip {
class TrayIcon;
}

namespace winrt::yip::implementation {
struct App : AppT<App> {
    App();
    ~App();

    void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

private:
    // Notification-area icon. Owned here rather than by MainWindow: it has to be
    // there whenever the process is, including while the main window is hidden
    // or minimised. Only Quit() takes it down.
    void SetupTray();
    void TeardownTray();

    // Closing the window hides it instead of ending the session: a take in
    // progress is not the window's to cancel, and the global hotkey lives on
    // MainWindow's HWND. The tray icon is the way back, so the interception
    // only happens while that icon is actually registered.
    void WireCloseToTray();
    void HideMainWindow();
    void ShowMainWindow();

    // The one real exit. `windowAlreadyClosing` is set when the call comes from
    // inside the window's own Closing handler, which is already closing it.
    void Quit(bool windowAlreadyClosing);

    void ToggleRecording();

    winrt::Microsoft::UI::Xaml::Window m_window{nullptr};
    winrt::Microsoft::UI::Xaml::Window m_indicator{nullptr};
    std::unique_ptr<::yip::TrayIcon> m_tray;
    ::yip::RecordingStateBus::Token m_trayStateToken{0};
    winrt::event_token m_closingToken{};

    // Set once Quit() is under way, so the Closing handler stops intercepting.
    bool m_quitting{false};
    // "Still running down here" is shown once per session, on the first hide.
    // After that the user knows where the window went.
    bool m_hintShown{false};
};
} // namespace winrt::yip::implementation
