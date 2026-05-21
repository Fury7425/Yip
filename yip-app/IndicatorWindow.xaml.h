#pragma once

#include "IndicatorWindow.g.h"
#include "IndicatorState.h"
#include "IndicatorPersistence.h"

#include <array>
#include <chrono>

namespace winrt::yip::implementation
{
    struct IndicatorWindow : IndicatorWindowT<IndicatorWindow>
    {
        IndicatorWindow();
        ~IndicatorWindow();

        // Event handlers (declared in XAML)
        void OnPillPointerPressed(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnPillPointerMoved(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnPillPointerReleased(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnPillPointerCaptureLost(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnPillTapped(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const& args);

        void OnStopClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnMarkClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnOpenLastClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
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
        void SyncFromAudioCore();   // polled by m_pollTimer
        void TransitionTo(::yip::IndicatorState s, bool animate = true);
        void AnimatePillToState(::yip::IndicatorState s);

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
        HWND m_hwnd{ nullptr };
        ::yip::IndicatorState m_state{ ::yip::IndicatorState::Idle };
        ::yip::IndicatorState m_baseState{ ::yip::IndicatorState::Idle }; // state before expansion
        ::yip::IndicatorPersistence m_persisted{};

        // Composition
        winrt::Microsoft::UI::Composition::Compositor m_compositor{ nullptr };
        winrt::Microsoft::UI::Composition::ContainerVisual m_pillRoot{ nullptr };
        winrt::Microsoft::UI::Composition::CompositionRoundedRectangleGeometry m_clipGeo{ nullptr };
        winrt::Microsoft::UI::Composition::SpriteVisual m_dotVisual{ nullptr };
        std::array<winrt::Microsoft::UI::Composition::SpriteVisual, 4> m_barVisuals{};
        winrt::Microsoft::UI::Composition::CompositionColorBrush m_dotNeutralBrush{ nullptr };
        winrt::Microsoft::UI::Composition::CompositionColorBrush m_dotRecordBrush{ nullptr };
        winrt::Microsoft::UI::Composition::CompositionColorBrush m_barIdleBrush{ nullptr };
        winrt::Microsoft::UI::Composition::CompositionColorBrush m_barLiveBrush{ nullptr };
        winrt::Microsoft::UI::Composition::CompositionEasingFunction m_ease{ nullptr };

        // Timers
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_pollTimer{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_meterTimer{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_collapseTimer{ nullptr };

        // Drag state
        bool m_dragging{ false };
        winrt::Windows::Foundation::Point m_dragOrigin{};
        winrt::Windows::Foundation::Point m_windowOriginAtDragStart{};
        bool m_movedDuringPress{ false };

        // Saving-state debouncer
        std::chrono::steady_clock::time_point m_lastRecordingTrueTs{};
    };
}

namespace winrt::yip::factory_implementation
{
    struct IndicatorWindow : IndicatorWindowT<IndicatorWindow, implementation::IndicatorWindow> {};
}
