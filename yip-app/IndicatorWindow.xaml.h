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
    // Land every layout property on `s` at once: actions, window, Border,
    // readout translation and clip. Every motion path ends by calling this.
    void ApplyLayoutFor(::yip::IndicatorState s);
    // Resize the HWND and the Border to match the state, around m_anchor.
    // The window *is* the pill.
    void SyncWindowToState(::yip::IndicatorState s);

    // ----- Motion -----
    void ShowPill(bool animate);
    void HidePill(bool animate);
    // Size change between two visible states: a clip draws the capsule from
    // the old size to the new while the readout glides to its new spot.
    void MorphPill(::yip::IndicatorState from, ::yip::IndicatorState to);
    void SetClip(float w, float h, float x, float y);
    void AnimateClip(float w, float h, float x, float y);
    void ClearClip();
    // Centre of the readout group in PillFrame coordinates (layout only; the
    // composition translation is not included).
    winrt::Windows::Foundation::Point ReadoutCentre();

    // ----- Edge dock / monitor restore -----
    void RestoreFromPersistence();
    double DpiScale() const noexcept;
    void SnapToNearestEdgeIfClose();
    void RememberPosition();
    void UpdateAnchorFromWindow();

    // ----- Visibility -----
    void ShowWindow();
    void HideWindow();

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

    // The physical-pixel point the pill is sized around: its top centre while
    // floating or docked top, the docked edge's midpoint otherwise. Resizing
    // around it is what keeps a centred pill centred when it expands.
    POINT m_anchor{};

    // Bumped on every transition. A motion's completion handler only acts if
    // nothing has happened since it started.
    uint32_t m_motionGen{0};
    // m_shown is the state machine's intent; m_windowVisible is the HWND. They
    // differ while the pill is fading out.
    bool m_shown{false};
    bool m_windowVisible{false};

    // Composition
    winrt::Microsoft::UI::Composition::Compositor m_compositor{nullptr};
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
    winrt::Microsoft::UI::Composition::CompositionEasingFunction m_easeOut{nullptr};
    winrt::Microsoft::UI::Composition::CompositionEasingFunction m_easeMorph{nullptr};
    // Attached to PillFrame only while a morph runs; at rest the Border's own
    // antialiased edge is the outline.
    winrt::Microsoft::UI::Composition::CompositionRoundedRectangleGeometry m_clipGeometry{nullptr};
    winrt::Microsoft::UI::Composition::CompositionGeometricClip m_clip{nullptr};

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
