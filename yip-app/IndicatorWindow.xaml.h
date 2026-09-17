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
#include <winrt/Windows.UI.ViewManagement.h>

namespace winrt::yip::implementation {
struct IndicatorWindow : IndicatorWindowT<IndicatorWindow> {
    IndicatorWindow();
    ~IndicatorWindow();

    // Event handlers (declared in XAML)
    void OnPillPointerPressed(winrt::Windows::Foundation::IInspectable const& sender,
                              winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnPillTapped(winrt::Windows::Foundation::IInspectable const& sender,
                      winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const& args);

    void OnStopClicked(winrt::Windows::Foundation::IInspectable const& sender,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnPauseClicked(winrt::Windows::Foundation::IInspectable const& sender,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

private:
    // What a layout pass should do with the morph clip. `Keep` exists because
    // a morph installs the *old* outline and then asks for the *new* layout;
    // the layout pass must not undo the clip it is about to animate.
    enum class ClipPolicy { Clear, Keep };

    // ----- HWND helpers -----
    HWND Hwnd() const noexcept { return m_hwnd; }
    void ApplyToolWindowStyle();
    void ApplyAlwaysOnTop();
    void ApplyClickThrough(bool enable);
    // No DWM corner or frame: the Border and the blur behind it are the only
    // things drawn.
    void ApplyFrameless();
    // Blur behind the capsule while transparency effects are on, a transparent
    // backdrop while they are off. Re-run whenever that setting flips.
    void ApplyBackdrop();
    // The shell's host backdrop (already blurred) through an alpha mask drawn
    // as the capsule. Leaves m_blurBrush null if the effect cannot be built.
    void BuildBackdropBrush();
    // The Border's tint: lighter over the blur, denser over the bare desktop.
    void ApplySurfaceTint();

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
    // Glyph, tooltip and dot for whichever of pause/resume the button offers.
    void ApplyPausedVisuals();
    void StopMeterAnimations();

    // ----- State machine -----
    // Driven by RecordingStateBus, not a timer: an idle pill costs nothing.
    void OnRecordingStateChanged(bool recording);
    void TransitionTo(::yip::IndicatorState s, bool animate = true);
    // Land every layout property on `s` at once: actions, readout detail,
    // Border, readout translation and clip. Every motion path ends by calling
    // this.
    void ApplyLayoutFor(::yip::IndicatorState s, ClipPolicy clip = ClipPolicy::Clear);
    // Size the Border, the blur mask and the mouse region to the state. Never
    // the HWND: that is sized once per show, by PlacePillAtHome.
    void SyncFrameToState(::yip::IndicatorState s, ClipPolicy clip = ClipPolicy::Clear);
    // Settings saved from the main window: dot or pill, top or bottom. Lands
    // on the spot, mid-take included.
    void ApplyPillSettings(bool dot, bool bottom);
    // Whether the meter and clock are laid out in `s`. Always in the pill
    // style; only while expanded in the dot style.
    bool DetailShownFor(::yip::IndicatorState s) const noexcept;

    // ----- Motion -----
    void ShowPill(bool animate);
    void HidePill(bool animate);
    // Size change between two visible states: a clip draws the capsule from
    // the old size to the new while the readout glides to its new spot.
    void MorphPill(::yip::IndicatorState from, ::yip::IndicatorState to);
    void SetClip(float w, float h, float x, float y);
    void AnimateClip(float w, float h, float x, float y);
    void ClearClip();
    // The pill visual and the backdrop mask take every opacity, scale and
    // outline change together, or the blur arrives before the tint and
    // outlives it.
    winrt::Microsoft::UI::Composition::Visual PillVisual();
    void SetPillCentre(float x, float y);
    void SetPillFade(float opacity, float scale);
    void AnimatePillOpacity(float to, int ms);
    void AnimatePillScale(float to, int ms);
    // Cut the blur to a capsule of w x h at (x, y), in PillFrame DIPs.
    void SetBackdropShape(float w, float h, float x, float y);
    // Re-rasterise the mask at the window's current size and DPI, leaving the
    // capsule drawn into it alone — mid-morph that shape is animating.
    void SyncBackdropSurface();
    // Surface *and* shape back to the whole PillFrame. The resting state.
    void ResetBackdropShape();
    // The point a morph keeps still, in PillFrame coordinates (layout only;
    // the composition translation is not included): the readout's centre in
    // the pill style, the dot's in the dot style.
    winrt::Windows::Foundation::Point TrackedCentre();

    // ----- Placement -----
    // The pill is not movable: it always sits centred at the top or bottom of
    // the primary display's work area. Run before every show, so a taskbar,
    // resolution or DPI change between takes is picked up.
    void PlacePillAtHome();
    double DpiScale() const noexcept;
    // The pill hangs from its top centre, or its bottom centre when it sits at
    // the bottom of the screen, as fractions of its width and height. That
    // point stays put when it changes size and is where it grows from.
    winrt::Windows::Foundation::Numerics::float2 Anchor() const noexcept;
    // Where layout puts a w x h PillFrame inside the window, in DIPs.
    winrt::Windows::Foundation::Numerics::float2 FrameOrigin(float w, float h) const;
    // Mouse input only over the capsule's bounding box, so the transparent
    // rest of the window does not swallow clicks meant for what is beneath it.
    void ApplyHitRegion(float w, float h);

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
    // The blur lives on the system compositor, not the XAML one: only its
    // brushes can be a window's system backdrop.
    winrt::Windows::UI::Composition::CompositionEffectBrush m_blurBrush{nullptr};
    winrt::Windows::UI::Composition::CompositionVisualSurface m_maskSurface{nullptr};
    winrt::Windows::UI::Composition::ContainerVisual m_maskRoot{nullptr};
    winrt::Windows::UI::Composition::ContainerVisual m_maskDpi{nullptr};
    winrt::Windows::UI::Composition::ShapeVisual m_maskVisual{nullptr};
    winrt::Windows::UI::Composition::CompositionRoundedRectangleGeometry m_maskShape{nullptr};
    winrt::Windows::UI::Composition::CompositionEasingFunction m_backdropEaseOut{nullptr};
    winrt::Windows::UI::Composition::CompositionEasingFunction m_backdropEaseMorph{nullptr};
    winrt::Windows::UI::ViewManagement::UISettings m_uiSettings;
    winrt::event_token m_effectsToken{};
    bool m_blurActive{false};
    ::yip::IndicatorState m_state{::yip::IndicatorState::Idle};
    ::yip::IndicatorState m_baseState{::yip::IndicatorState::Idle}; // state before expansion
    ::yip::IndicatorPersistence m_persisted{};
    ::yip::RecordingStateBus::Token m_stateToken{0};
    winrt::event_token m_themeToken{};

    // From settings.json. m_dotStyle: collapsed, the pill is the recording
    // light alone. m_bottom: it sits at the bottom of the screen.
    bool m_dotStyle{false};
    bool m_bottom{false};
    // YipIndicatorPadding as the XAML applied it. The dot style drops it while
    // collapsed and needs it back to expand.
    winrt::Microsoft::UI::Xaml::Thickness m_contentPadding{};

    // Bumped on every transition. A motion's completion handler only acts if
    // nothing has happened since it started.
    uint32_t m_motionGen{0};
    // True between the start of a size change and the layout pass that lands
    // it. A second morph starting inside that window has to land the first one
    // before it measures, or it reads a size the pill never actually had.
    bool m_morphing{false};
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

    // Last state delivered by the bus. Cheaper than asking audio-core again
    // from inside a transition.
    bool m_recording{false};

    // Mirrors rec_is_paused(). A paused take is still a take: the bus never
    // fires for it, so this arrives with the meter snapshot instead.
    bool m_paused{false};

    // Cached so a tick only touches the TextBlock when the second rolls over.
    winrt::hstring m_elapsedText{L"00:00"};
};
} // namespace winrt::yip::implementation

namespace winrt::yip::factory_implementation {
struct IndicatorWindow : IndicatorWindowT<IndicatorWindow, implementation::IndicatorWindow> {};
} // namespace winrt::yip::factory_implementation
