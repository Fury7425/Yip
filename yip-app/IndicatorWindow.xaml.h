#pragma once

#include "IndicatorWindow.g.h"
#include "IndicatorState.h"
#include "IndicatorPersistence.h"
#include "RecordingStateBus.h"

#include <array>
#include <chrono>

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

    // ----- Composition layer -----
    void BuildCompositionLayer();
    void UpdateMeterBars(float peak);
    void UpdateDotForState(::yip::IndicatorState s);
    void StopMeterAnimations();

    // ----- State machine -----
    // Driven by RecordingStateBus, not a timer: an idle pill costs nothing.
    void OnRecordingStateChanged(bool recording);
    void TransitionTo(::yip::IndicatorState s, bool animate = true);
    void AnimatePillToState(::yip::IndicatorState s, bool animate);

    // ----- Edge dock / monitor restore -----
    void RestoreFromPersistence();
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
    ::yip::IndicatorState m_state{::yip::IndicatorState::Idle};
    ::yip::IndicatorState m_baseState{::yip::IndicatorState::Idle}; // state before expansion
    ::yip::IndicatorPersistence m_persisted{};
    ::yip::RecordingStateBus::Token m_stateToken{0};

    // Composition
    winrt::Microsoft::UI::Composition::Compositor m_compositor{nullptr};
    winrt::Microsoft::UI::Composition::ContainerVisual m_pillRoot{nullptr};
    winrt::Microsoft::UI::Composition::CompositionRoundedRectangleGeometry m_clipGeo{nullptr};
    winrt::Microsoft::UI::Composition::SpriteVisual m_dotVisual{nullptr};
    std::array<winrt::Microsoft::UI::Composition::SpriteVisual, 4> m_barVisuals{
        nullptr, nullptr, nullptr, nullptr};
    winrt::Microsoft::UI::Composition::CompositionColorBrush m_dotNeutralBrush{nullptr};
    winrt::Microsoft::UI::Composition::CompositionColorBrush m_dotRecordBrush{nullptr};
    winrt::Microsoft::UI::Composition::CompositionColorBrush m_barIdleBrush{nullptr};
    winrt::Microsoft::UI::Composition::CompositionColorBrush m_barLiveBrush{nullptr};
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
};
} // namespace winrt::yip::implementation

namespace winrt::yip::factory_implementation {
struct IndicatorWindow : IndicatorWindowT<IndicatorWindow, implementation::IndicatorWindow> {};
} // namespace winrt::yip::factory_implementation
