#pragma once

#include "IndicatorWindow.g.h"
#include "IndicatorState.h"
#include "IndicatorPersistence.h"
#include "RecordingStateBus.h"

#include <array>
#include <chrono>
#include <functional>
#include <vector>

#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.ViewManagement.h>

namespace winrt::yip::implementation {
// One motion of the pill's shapes, defined in IndicatorWindow.xaml.cpp.
struct PillChoreo;

// The pill is drawn as these shapes, one geometry each, on both compositors.
enum class PillBlob : uint8_t { Edge, Neck, Cap, Pause, Stop };
inline constexpr size_t kPillBlobCount = 5;

// The same curves, built once per compositor.
template <typename EasingFunction>
struct PillEases {
    EasingFunction standard{nullptr};
    EasingFunction out{nullptr};
    EasingFunction morph{nullptr};
    EasingFunction inOut{nullptr};
    EasingFunction linear{nullptr};
};

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
    // Unhook everything global and persist. Idempotent: Closed runs it, and
    // the destructor makes sure it ran.
    void Teardown();

    // ----- HWND helpers -----
    HWND Hwnd() const noexcept { return m_hwnd; }
    void ApplyToolWindowStyle();
    void ApplyAlwaysOnTop();
    void ApplyClickThrough(bool enable);
    // No DWM corner or frame: the goo and the blur behind it are the only
    // things drawn.
    void ApplyFrameless();
    // Blur behind the shapes while transparency effects are on, a transparent
    // backdrop while they are off. Re-run whenever that setting flips.
    void ApplyBackdrop();
    // The shell's host backdrop (already blurred) through an alpha mask drawn
    // as the same goo as the pill. Leaves m_blurBrush null if the effect
    // cannot be built.
    void BuildBackdropBrush();
    // The mask's shapes, built once; BuildBackdropBrush reads them.
    void BuildMaskShapes();
    // The surface tint: lighter over the blur, denser over the bare desktop.
    void ApplySurfaceTint();

    // ----- Composition layer -----
    void BuildCompositionLayer();
    // The goo: every shape, blurred and cut at an alpha threshold, filled with
    // the surface tint and rimmed with the quiet stroke. Falls back to the
    // shapes drawn plainly if this compositor cannot run the effect.
    void BuildGooBrush();
    // Composition holds Colors, not {ThemeResource} bindings, so the tokens
    // have to be re-read whenever the system theme flips.
    void ResolveThemeBrushes();
    void OnActualThemeChanged(winrt::Microsoft::UI::Xaml::FrameworkElement const& sender,
                              winrt::Windows::Foundation::IInspectable const& args);
    // One rec_meter() snapshot per tick, fanned out to the bars and the timer.
    void UpdateFromMeter();
    void ApplyReadout(RecMeter const& snapshot, bool snap);
    void SyncReadoutNow();
    void UpdateMeterBars(float level, bool hot, bool snap = false);
    void UpdateDotForState(::yip::IndicatorState s);
    // Glyph, tooltip and dot for whichever of pause/resume the button offers.
    void ApplyPausedVisuals();
    void StopMeterAnimations();

    // ----- State machine -----
    // Driven by RecordingStateBus, not a timer: an idle pill costs nothing.
    void OnRecordingStateChanged(bool recording);
    void TransitionTo(::yip::IndicatorState s, bool animate = true);
    // Land everything on `s` at once: shapes, content, button visibility and
    // the mouse region. Every motion path ends by calling this.
    void ApplyLayoutFor(::yip::IndicatorState s);
    // Move the content and hit targets to where `s` puts the shapes, now. The
    // shapes may still be travelling there; the content follows them.
    void PlaceContent(::yip::IndicatorState s);
    // Settings saved from the main window: dot or pill, top or bottom. Lands
    // on the spot, mid-take included.
    void ApplyPillSettings(bool dot, bool bottom);

    // ----- Motion -----
    // In: a drop hangs from the screen edge, pinches off and lands. Out: the
    // same, backwards.
    void ShowPill(bool animate);
    void HidePill(bool animate);
    // Between two visible states: the Pause and Stop discs split off the
    // capsule, or melt back into it.
    void MorphPill(::yip::IndicatorState from, ::yip::IndicatorState to);
    // Run `choreo` on both compositors and bump m_shapeGen. `landed` runs once
    // the shapes arrive, unless a later motion has taken them over by then;
    // with animations off everything lands, and `landed` runs, at once.
    void Play(PillChoreo const& choreo, std::function<void(IndicatorWindow&)> landed);
    // A motion's length, or 0 when Windows is set to show no animations.
    int MotionMs(int ms) const;
    // The pill's content and the backdrop mask take every opacity change
    // together, or the blur arrives before the tint and outlives it.
    winrt::Microsoft::UI::Composition::Visual PillVisual();
    void SetPillOpacity(float opacity);
    void AnimatePillOpacity(float to, int ms);
    // Starts the entrance once XAML is actually drawing the window. Started at
    // Show() it ran against the DWM's first-present stall, and the first
    // ~100 ms of it never reached the screen.
    void StartShowOnFirstFrame();
    void RevokeFirstFrame();
    // Size the goo and mask surfaces to the window at its current DPI, and
    // rebuild the effects if that DPI moved the blur radius.
    void SyncShapeSurfaces();

    // ----- Placement -----
    // The pill is not movable: it always sits centred at the top or bottom of
    // the primary display's work area. Run before every show, so a taskbar,
    // resolution or DPI change between takes is picked up.
    void PlacePillAtHome();
    double DpiScale() const noexcept;
    // Mouse input only over the resting shapes of `s`, so the transparent
    // rest of the window, the gaps between the discs included, does not
    // swallow clicks meant for what is beneath it. While shapes move, the
    // whole window takes the mouse instead.
    void ApplyHitRegion(::yip::IndicatorState s);
    void ApplyHitRegionWhole();

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
    std::array<winrt::Windows::UI::Composition::CompositionRoundedRectangleGeometry, kPillBlobCount> m_maskBlobs{
        nullptr, nullptr, nullptr, nullptr, nullptr};
    PillEases<winrt::Windows::UI::Composition::CompositionEasingFunction> m_backdropEases;
    // The display scale the mask's blur was built for.
    float m_maskScale{0.0f};
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

    // Bumped on every transition. A hide's completion only acts if nothing
    // has happened since it started.
    uint32_t m_motionGen{0};
    // Bumped whenever the shapes are sent somewhere new. A shape motion's
    // completion only lands the layout if no later one has taken the shapes
    // over. Kept apart from m_motionGen so a same-shape transition (Recording
    // -> Saving as a take stops mid-merge) fades the pill without orphaning
    // the merge that is still carrying the discs home.
    uint32_t m_shapeGen{0};
    // m_shown is the state machine's intent; m_windowVisible is the HWND. They
    // differ while the pill is fading out.
    bool m_shown{false};
    bool m_windowVisible{false};
    // One-shot CompositionTarget::Rendering hook for the show fade; empty
    // when none is pending.
    winrt::event_token m_firstFrameToken{};

    // Composition
    winrt::Microsoft::UI::Composition::Compositor m_compositor{nullptr};
    // The shapes, drawn into a surface at physical pixels (m_shapeRoot ->
    // m_shapeDpi -> m_shapeVisual) that the goo effect reads, or parented
    // straight under GooHost when the effect is unavailable.
    std::array<winrt::Microsoft::UI::Composition::CompositionRoundedRectangleGeometry, kPillBlobCount> m_blobs{
        nullptr, nullptr, nullptr, nullptr, nullptr};
    std::array<winrt::Microsoft::UI::Composition::CompositionSpriteShape, kPillBlobCount> m_blobShapes{
        nullptr, nullptr, nullptr, nullptr, nullptr};
    winrt::Microsoft::UI::Composition::ShapeVisual m_shapeVisual{nullptr};
    winrt::Microsoft::UI::Composition::ContainerVisual m_shapeDpi{nullptr};
    winrt::Microsoft::UI::Composition::ContainerVisual m_shapeRoot{nullptr};
    winrt::Microsoft::UI::Composition::CompositionVisualSurface m_shapeSurface{nullptr};
    winrt::Microsoft::UI::Composition::SpriteVisual m_gooSprite{nullptr};
    winrt::Microsoft::UI::Composition::CompositionColorBrush m_tintBrush{nullptr};
    winrt::Microsoft::UI::Composition::CompositionColorBrush m_strokeBrush{nullptr};
    // True while the goo effect paints the shapes; false while they are drawn
    // plainly, where the drip's edge and neck would show as lumps and are
    // left out.
    bool m_gooActive{false};
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
    // What the bars were last told, so a tick that changes nothing visible
    // writes nothing and leaves the compositor idle.
    std::array<float, 4> m_barTargets{};
    winrt::Microsoft::UI::Composition::CompositionColorBrush m_barBrush{nullptr};
    PillEases<winrt::Microsoft::UI::Composition::CompositionEasingFunction> m_eases;

    // Timers. m_savingTimer is one-shot: it only exists to hold the Saving
    // frame on screen briefly after capture ends.
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_savingTimer{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_meterTimer{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_collapseTimer{nullptr};

    // Last state delivered by the bus. Cheaper than asking audio-core again
    // from inside a transition.
    bool m_recording{false};

    bool m_tornDown{false};

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
