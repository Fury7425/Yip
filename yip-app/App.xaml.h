#pragma once

// App is not a runtimeclass (no App.idl): the XAML compiler owns AppT, and
// main.cpp instantiates it with make<>. Adding an idl makes cppwinrt emit a
// projected constructor that cannot be built from the XAML base.
#include "App.xaml.g.h"
#include "RecordingStateBus.h"
#include "viewmodels/MainViewModel.h"

#include <memory>
#include <optional>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.System.h>

namespace yip {
class TrayIcon;
class HotkeyManager;
} // namespace yip
namespace yip::interop {
class DeviceWatcher;
}

namespace winrt::yip::implementation {
struct App : AppT<App> {
    App();
    ~App();

    void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

private:
    // Notification-area icon. Owned here rather than by MainWindow: it has to be
    // there whenever the process is, including while no window exists. Only
    // Quit() takes it down.
    void SetupTray();
    void TeardownTray();

    // Everything that has to keep working with no window open: the view model
    // (the take, the transport, the settings), the global hotkey, device
    // changes. The main window is only a view onto them.
    void SetupHotkey();
    void ApplyHotkey();
    void OnViewModelPropertyChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                    winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const& args);
    void OnRecordingStateChanged(bool recording);

    // The system compositor (acrylic, the pill's blur) needs a
    // Windows.System dispatcher queue on this thread. Owned here, not by a
    // window: a window that created it and then closed would take it away from
    // the other one.
    void EnsureSystemDispatcherQueue();

    // Closing the main window closes it for real — its XAML tree is most of
    // the memory Yip holds — but not the session: a take carries on, and the
    // tray icon or a relaunch opens a new window onto the same view model.
    // Only while the icon is registered; without one a close is a quit.
    void CreateMainWindow();
    void WireMainWindow();
    void ShowMainWindow();

    // The pill exists only around a take: built when capture starts, closed a
    // little after it ends. Between takes it would be a whole hidden XAML
    // window doing nothing.
    void EnsureIndicator();
    // One-shot, restarted whenever something may have gone idle: closes the
    // pill once it has faded out, then trims the working set if no window is
    // open.
    void RestartIdleTimer();
    void OnIdle();
    void TrimWorkingSet();

    // The one real exit. `windowAlreadyClosing` is set when the call comes from
    // inside the window's own Closing handler, which is already closing it.
    void Quit(bool windowAlreadyClosing);

    void ToggleRecording();

    winrt::yip::viewmodels::MainViewModel m_viewModel{nullptr};
    winrt::event_token m_vmToken{};
    winrt::Microsoft::UI::Xaml::Window m_window{nullptr};
    winrt::Microsoft::UI::Xaml::Window m_indicator{nullptr};
    std::unique_ptr<::yip::TrayIcon> m_tray;
    std::unique_ptr<::yip::HotkeyManager> m_hotkey;
    std::unique_ptr<::yip::interop::DeviceWatcher> m_deviceWatcher;
    ::yip::RecordingStateBus::Token m_stateToken{0};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_idleTimer{nullptr};
    winrt::Windows::System::DispatcherQueueController m_systemQueue{nullptr};

    // Where the main window was when it last closed, so the next one opens in
    // the same place. Empty until a restored (not minimised or maximised)
    // window has closed once.
    std::optional<winrt::Windows::Graphics::RectInt32> m_windowRect;

    // Set once Quit() is under way, so the Closing handler stops intercepting.
    bool m_quitting{false};
    // "Still running down here" is shown once per session, on the first close.
    // After that the user knows where the window went.
    bool m_hintShown{false};
};
} // namespace winrt::yip::implementation
