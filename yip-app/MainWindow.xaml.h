#pragma once

#include "MainWindow.g.h"
#include "RecordingStateBus.h"
#include "viewmodels/MainViewModel.h"

#include <memory>
#include <vector>

#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Windows.System.h>

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
    void OnWaveSizeChanged(winrt::Windows::Foundation::IInspectable const& sender,
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
    winrt::event_token m_themeToken{};
    HWND m_hwnd{nullptr};
    bool m_focused{true};

    // ----- backdrop -----
    // Driven through the controller rather than Window::SystemBackdrop so the
    // configuration can report the window as always active: the stock
    // DesktopAcrylicBackdrop drops to a flat fill whenever focus leaves.
    winrt::Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController m_backdrop{nullptr};
    winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration m_backdropConfig{nullptr};
    winrt::Windows::System::DispatcherQueueController m_backdropQueue{nullptr};

    // ----- waveform -----
    // A scrolling history of the peak envelope, drawn entirely in the
    // compositor. Every bar exists twice, half a strip apart, so advancing the
    // history is two scale writes plus one container offset — O(1) per tick
    // instead of rewriting the whole strip, and no layout pass either way.
    winrt::Microsoft::UI::Composition::ContainerVisual m_waveRoot{nullptr};
    winrt::Microsoft::UI::Composition::ContainerVisual m_waveScroller{nullptr};
    winrt::Microsoft::UI::Composition::SpriteVisual m_waveHold{nullptr};
    std::vector<winrt::Microsoft::UI::Composition::SpriteVisual> m_waveBars;
    // Sampled from YipMeterGradientBrush, so the waveform and the XAML meter
    // cannot drift apart. Mutated in place on a theme change.
    std::vector<winrt::Microsoft::UI::Composition::CompositionColorBrush> m_wavePalette;
    winrt::Microsoft::UI::Composition::CompositionColorBrush m_waveRestBrush{nullptr};
    winrt::Microsoft::UI::Composition::CompositionColorBrush m_waveHoldBrush{nullptr};
    int m_waveCount{0};
    int m_waveHead{0};
    double m_waveWidth{0.0};
    double m_waveHeight{0.0};

    // Resolved from App.xaml, re-resolved when the system theme flips.
    winrt::Microsoft::UI::Xaml::Media::Brush m_lampIdleBrush{nullptr};
    winrt::Microsoft::UI::Xaml::Media::Brush m_lampLiveBrush{nullptr};

    // Meter polling only runs while capture is live — see OnRecordingStateChanged.
    void StartMeterPolling();
    void StopMeterPolling();
    void OnRecordingStateChanged(bool recording);
    void ApplyHotkeyFromSettings();
    void OnActivated(winrt::Windows::Foundation::IInspectable const& sender,
                     winrt::Microsoft::UI::Xaml::WindowActivatedEventArgs const& args);
    void OnViewModelPropertyChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                    winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const& args);
    void OnActualThemeChanged(winrt::Microsoft::UI::Xaml::FrameworkElement const& sender,
                              winrt::Windows::Foundation::IInspectable const& args);

    void SetupTitleBar();
    void SetupBackdrop();
    void TeardownBackdrop();
    void UpdateTitleBarInset();
    double DpiScale() const noexcept;
    void UpdateRecordButtonShape();
    void UpdateEmptyState();
    // Press feedback on the record button, driven by ButtonBase::IsPressed.
    void WireRecordButtonPress();
    void PressRecordButton(bool down);

    void ResolveThemeBrushes();
    void BuildWaveVisuals(double width, double height);
    void PushWaveSample(float level, float hold);
    void ClearWave();
    void FadeWave(float opacity);
};
} // namespace winrt::yip::implementation

namespace winrt::yip::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
} // namespace winrt::yip::factory_implementation
