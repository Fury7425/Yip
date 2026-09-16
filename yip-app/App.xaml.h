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
    // Notification-area icon. Owned here rather than by MainWindow: it has to
    // be there whenever the process is, including while the main window sits
    // minimised behind everything else.
    void SetupTray();
    void TeardownTray();
    void ShowMainWindow();
    void ToggleRecording();

    winrt::Microsoft::UI::Xaml::Window m_window{nullptr};
    winrt::Microsoft::UI::Xaml::Window m_indicator{nullptr};
    std::unique_ptr<::yip::TrayIcon> m_tray;
    ::yip::RecordingStateBus::Token m_trayStateToken{0};
};
} // namespace winrt::yip::implementation
