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

    // ----- transport -----
    void OnRecordToggle(winrt::Windows::Foundation::IInspectable const& sender,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::fire_and_forget OnOpenSettings(winrt::Windows::Foundation::IInspectable const& sender,
                                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::fire_and_forget OnOpenProcess(winrt::Windows::Foundation::IInspectable const& sender,
                                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnRefreshList(winrt::Windows::Foundation::IInspectable const& sender,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnMeterSizeChanged(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
    void OnAcknowledgeClip(winrt::Windows::Foundation::IInspectable const& sender,
                           winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const& args);
    void OnDismissError(winrt::Microsoft::UI::Xaml::Controls::InfoBar const& sender,
                        winrt::Windows::Foundation::IInspectable const& args);

    // ----- recordings list -----
    void OnFilterChanged(winrt::Windows::Foundation::IInspectable const& sender,
                         winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const& args);
    void OnRecordingActivated(winrt::Windows::Foundation::IInspectable const& sender,
                              winrt::Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const& args);
    void OnPlayItem(winrt::Windows::Foundation::IInspectable const& sender,
                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnRevealItem(winrt::Windows::Foundation::IInspectable const& sender,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnCopyPathItem(winrt::Windows::Foundation::IInspectable const& sender,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::fire_and_forget OnDeleteItem(winrt::Windows::Foundation::IInspectable const& sender,
                                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

    // ----- accelerators -----
    void OnRecordAccelerator(
        winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& sender,
        winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);
    void OnProcessAccelerator(
        winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& sender,
        winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);
    void OnSearchAccelerator(
        winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& sender,
        winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);
    void OnRefreshAccelerator(
        winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& sender,
        winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);

private:
    winrt::yip::viewmodels::MainViewModel m_viewModel{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_meterTimer{nullptr};
    std::unique_ptr<::yip::interop::DeviceWatcher> m_deviceWatcher;
    std::unique_ptr<::yip::HotkeyManager> m_hotkey;
    ::yip::RecordingStateBus::Token m_stateToken{0};
    winrt::event_token m_vmToken{};
    HWND m_hwnd{nullptr};
    bool m_focused{true};

    // Meter geometry, cached from the host's SizeChanged. The tick only writes
    // clip rectangles, so a level change costs no measure or arrange pass.
    double m_meterWidth{0.0};
    double m_meterHeight{0.0};

    winrt::Microsoft::UI::Xaml::Media::SolidColorBrush m_lampIdleBrush{nullptr};
    winrt::Microsoft::UI::Xaml::Media::SolidColorBrush m_lampLiveBrush{nullptr};

    // Meter polling only runs while capture is live — see OnRecordingStateChanged.
    void StartMeterPolling();
    void StopMeterPolling();
    void OnRecordingStateChanged(bool recording);
    void ApplyHotkeyFromSettings();
    void OnActivated(winrt::Windows::Foundation::IInspectable const& sender,
                     winrt::Microsoft::UI::Xaml::WindowActivatedEventArgs const& args);
    void OnViewModelPropertyChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                    winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const& args);

    void SetupTitleBar();
    void UpdateTitleBarInset();
    void UpdateMeterVisuals();
    void UpdateRecordButtonShape();
    void UpdateEmptyState();
};
} // namespace winrt::yip::implementation

namespace winrt::yip::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
} // namespace winrt::yip::factory_implementation
