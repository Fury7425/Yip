#pragma once

#include "MainWindow.g.h"
#include "RecordingStateBus.h"
#include "viewmodels/MainViewModel.h"

#include <memory>

namespace yip::interop {
class DeviceWatcher;
}
namespace yip {
class HotkeyManager;
}

namespace winrt::yip::implementation {
struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
    ~MainWindow();

    winrt::yip::viewmodels::MainViewModel ViewModel();

    void OnRecordToggle(winrt::Windows::Foundation::IInspectable const& sender,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::fire_and_forget OnOpenSettings(winrt::Windows::Foundation::IInspectable const& sender,
                                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::fire_and_forget OnOpenProcess(winrt::Windows::Foundation::IInspectable const& sender,
                                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnRefreshList(winrt::Windows::Foundation::IInspectable const& sender,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnRecordingClicked(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& args);

private:
    winrt::yip::viewmodels::MainViewModel m_viewModel{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_meterTimer{nullptr};
    std::unique_ptr<::yip::interop::DeviceWatcher> m_deviceWatcher;
    std::unique_ptr<::yip::HotkeyManager> m_hotkey;
    ::yip::RecordingStateBus::Token m_stateToken{0};
    HWND m_hwnd{nullptr};
    bool m_focused{true};

    // Meter polling only runs while capture is live — see OnRecordingStateChanged.
    void StartMeterPolling();
    void StopMeterPolling();
    void OnRecordingStateChanged(bool recording);
    void ApplyHotkeyFromSettings();
    void OnActivated(winrt::Windows::Foundation::IInspectable const& sender,
                     winrt::Microsoft::UI::Xaml::WindowActivatedEventArgs const& args);
};
} // namespace winrt::yip::implementation

namespace winrt::yip::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
} // namespace winrt::yip::factory_implementation
