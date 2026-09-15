#pragma once

#include "IndicatorWindow.g.h"
#include "IndicatorState.h"
#include "IndicatorPersistence.h"
#include "RecordingStateBus.h"

#include <array>
#include <chrono>
#include <vector>

#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>

namespace winrt::yip::implementation {
struct IndicatorWindow : IndicatorWindowT<IndicatorWindow> {
    IndicatorWindow();
    ~IndicatorWindow();

    // Event handlers (declared in XAML)
    void OnPillPointerPressed(winrt::Windows::Foundation::IInspectable const& sender,
                              winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnPillPointerMoved(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnPillPointerReleased(winrt::Windows::Foundation::IInspectable const& sender,
                               winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnPillPointerCaptureLost(winrt::Windows::Foundation::IInspectable const& sender,
                                  winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnPillTapped(winrt::Windows::Foundation::IInspectable const& sender,
                      winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const& args);

    void OnStopClicked(winrt::Windows::Foundation::IInspectable const& sender,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnMarkClicked(winrt::Windows::Foundation::IInspectable const& sender,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnOpenLastClicked(winrt::Windows::Foundation::IInspectable const& sender,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

private:
    // ----- HWND helpers -----
    HWND Hwnd() const noexcept { return m_hwnd; }
    void ApplyToolWindowStyle();
    void ApplyAlwaysOnTop();
    void ApplyClickThrough(bool enable);
    // No DWM corner or frame, and a transparent system backdrop: the Border is
    // the only thing drawn.
    void ApplyFrameless();
    void ApplyTransparentBackdrop();

    // ----- Composition layer -----
    void BuildCompositionLayer();
    // Composition holds Colors, not {ThemeResource} bindings, so the tokens
    // have to be re-read whenever the system theme flips.
    void ResolveThemeBrushes();
    void OnActualThemeChanged(winrt::Microsoft::UI::Xaml::FrameworkElement const& sender,
                              winrt::Windows::Foundation::IInspectable const& args);
    // One rec_meter() snapshot per tick, fanned out to the bars and the timer.
    void UpdateFromMeter();
    void UpdateMeterBars(float level, bool hot);
    void UpdateDotForState(::yip::IndicatorState s);
    void StopMeterAnimations();

    // ----- State machine -----
    // Driven by RecordingStateBus, not a timer: an idle pill costs nothing.
    void OnRecordingStateChanged(bool recording);
    void TransitionTo(::yip::IndicatorState s, bool animate = true);
    // Resize the HWND and the Border to match the state.
    // The window *is* the pill; nothing clips it.
    void SyncWindowToState(::yip::IndicatorState s);
    void AnimatePillToState(::yip::IndicatorState s, bool animate);

    // ----- Edge dock / monitor restore -----
    void RestoreFromPersistence();
    double DpiScale() const noexcept;
    void SnapToNearestEdgeIfClose();
    void RememberPosition();

    // ----- Visibility -----
    void ShowWindow();
    void HideWindow();
    void SyncVisibilityForState(::yip::IndicatorState s);

    // ----- Auto-collapse -----
    void ResetAutoCollapseTimer();
    void StopAutoCollapseTimer();

    // Field state
    HWND m_hwnd{nullptr};
    winrt::Windows::System::DispatcherQueueController m_backdropQueue{nullptr};
    winrt::Windows::UI::Composition::Compositor m_backdropCompositor{nullptr};
    ::yip::IndicatorState m_state{::yip::IndicatorState::Idle};
    ::yip::IndicatorState m_baseState{::yip::IndicatorState::Idle}; // state before expansion
    ::yip::IndicatorPersistence m_persisted{};
    ::yip::RecordingStateBus::Token m_stateToken{0};
    winrt::event_token m_themeToken{};

    // Composition
    winrt::Microsoft::UI::Composition::Compositor m_compositor{nullptr};
    winrt::Microsoft::UI::Composition::ContainerVisual m_pillRoot{nullptr};
    winrt::Microsoft::UI::Composition::SpriteVisual m_dotVisual{nullptr};
    std::array<winrt::Microsoft::UI::Composition::SpriteVisual, 4> m_barVisuals{nullptr, nullptr, nullptr,
                                                                                nullptr};
    winrt::Microsoft::UI::Composition::CompositionColorBrush m_dotNeutralBrush{nullptr};
    winrt::Microsoft::UI::Composition::CompositionColorBrush m_dotRecordBrush{nullptr};
    winrt::Microsoft::UI::Composition::CompositionColorBrush m_barIdleBrush{nullptr};
    winrt::Microsoft::UI::Composition::CompositionColorBrush m_barLiveBrush{nullptr};
    // Sampled from the same YipMeterGradientBrush the main window's waveform
    // uses, so a level looks the same in both places.
    std::vector<winrt::Microsoft::UI::Composition::CompositionColorBrush> m_barPalette;
    winrt::Microsoft::UI::Composition::CompositionEasingFunction m_ease{nullptr};

    // Timers. m_savingTimer is one-shot: it only exists to hold the Saving
    // frame on screen briefly after capture ends.
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_savingTimer{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_meterTimer{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_collapseTimer{nullptr};

    // Drag state
    bool m_dragging{false};
    winrt::Windows::Foundation::Point m_dragOrigin{};
    winrt::Windows::Foundation::Point m_windowOriginAtDragStart{};
    bool m_movedDuringPress{false};

    // Last state delivered by the bus. Cheaper than asking audio-core again
    // from inside a transition.
    bool m_recording{false};

    // Cached so a tick only touches the TextBlock when the second rolls over.
    winrt::hstring m_elapsedText{L"00:00"};
};
} // namespace winrt::yip::implementation

namespace winrt::yip::factory_implementation {
struct IndicatorWindow : IndicatorWindowT<IndicatorWindow, implementation::IndicatorWindow> {};
} // namespace winrt::yip::factory_implementation
