#include "pch.h"
#include "IndicatorWindow.xaml.h"

#if __has_include("IndicatorWindow.g.cpp")
#include "IndicatorWindow.g.cpp"
#endif

#include "Markers.h"
#include "Settings.h"
#include "ThemeColors.h"

#include <DispatcherQueue.h>
#include <dwmapi.h>
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace std::chrono_literals;

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxh = winrt::Microsoft::UI::Xaml::Hosting;
namespace muxi = winrt::Microsoft::UI::Xaml::Input;
namespace mucomp = winrt::Microsoft::UI::Composition;
namespace muw = winrt::Microsoft::UI::Windowing;
namespace muxd = winrt::Microsoft::UI::Dispatching;

namespace {
// Geometry and motion for the pill are owned here, not in App.xaml: only the
// two sizes the pill's XAML actually binds to live there. Colours are the
// other way round — every one of them is resolved from the theme dictionaries
// by ResolveThemeBrushes().
// The widest state. Only used to place the pill on restore, before any state
// has sized the window; every live size comes from GeometryFor().
constexpr int kWindowW = 320;
constexpr int kWindowH = 56;
constexpr int kFadeMs = 180;
constexpr int kResizeMs = 220;
constexpr int kMeterMs = 33;
constexpr int kSnapPx = 20;
constexpr int kAutoCollapseMs = 3000;
constexpr int kBarCount = 4;
constexpr float kBarWidth = 3.0f;
constexpr float kBarGap = 4.0f;
constexpr float kBarMaxHeight = 18.0f;
constexpr int kSavingHoldMs = 350; // how long the Saving frame stays up

// Bottom of the bar meter, in dBFS. Same curve as the main window: on a linear
// amplitude scale these bars barely leave the floor.
constexpr double kMeterFloorDb = -60.0;
constexpr float kSilenceFloor = 1e-7f;

// Resting scale of a bar. Small enough to read as a dash, not a zero-height
// glitch.
constexpr float kBarRestScale = 0.06f;

// Per-bar weighting. The pair in the middle run tallest, which reads as a
// level meter rather than four identical sticks.
constexpr float kBarWeights[kBarCount] = {0.62f, 1.00f, 0.86f, 0.50f};

// Above this much of the travel the bars take the hot colour — roughly the
// last 6 dB, which is the headroom worth worrying about.
constexpr float kBarHotThreshold = 0.86f;

// Steps sampled out of the shared meter ramp for the bars.
constexpr uint32_t kBarPaletteSteps = 12;

/// Map an amplitude onto the meter's 0..1 travel, logarithmically.
float MeterNorm(float amplitude) noexcept
{
    if (!(amplitude > kSilenceFloor)) return 0.0f;
    const double db = 20.0 * std::log10(static_cast<double>(amplitude));
    const double n = (db - kMeterFloorDb) / (0.0 - kMeterFloorDb);
    return static_cast<float>(std::clamp(n, 0.0, 1.0));
}

/// "MM:SS", or "H:MM:SS" once a take passes the hour. No tenths: a digit
/// flickering ten times a second in a floating pill is a distraction.
std::wstring FormatPillElapsed(uint64_t ms) noexcept
{
    const uint64_t total = ms / 1000;
    const uint64_t hours = total / 3600;
    wchar_t buf[24];
    if (hours > 0) {
        swprintf_s(buf, L"%llu:%02llu:%02llu", hours, (total / 60) % 60, total % 60);
    } else {
        swprintf_s(buf, L"%02llu:%02llu", total / 60, total % 60);
    }
    return buf;
}

struct StateGeom {
    float w;
    float h;
    float opacity;
};

StateGeom GeometryFor(::yip::IndicatorState s) noexcept
{
    using S = ::yip::IndicatorState;
    switch (s) {
        case S::Idle:
            return {92.0f, 28.0f, 0.60f};
        case S::Armed:
            return {132.0f, 36.0f, 1.00f};
        case S::Recording:
            return {210.0f, 44.0f, 1.00f};
        case S::Saving:
            return {210.0f, 44.0f, 0.85f};
        case S::Expanded:
            return {320.0f, 56.0f, 1.00f};
    }
    return {92.0f, 28.0f, 0.60f};
}

// CubicBezier(0.4, 0.0, 0.2, 1.0) — Fluent standard easing.
mucomp::CompositionEasingFunction StandardEase(mucomp::Compositor const& c)
{
    winrt::Windows::Foundation::Numerics::float2 cp1{0.4f, 0.0f};
    winrt::Windows::Foundation::Numerics::float2 cp2{0.2f, 1.0f};
    return c.CreateCubicBezierEasingFunction(cp1, cp2);
}

// Shown only if a token key is wrong. A deliberate flat grey rather than a
// second copy of the palette, so a miss is visible instead of plausible.
constexpr winrt::Windows::UI::Color kMissingToken{0xFF, 0x80, 0x80, 0x80};
} // namespace

namespace winrt::yip::implementation {
IndicatorWindow::IndicatorWindow()
{
    InitializeComponent();

    m_persisted = ::yip::IndicatorPersistence::Load();

    // Grab the HWND. Required for tool-window style + click-through flip.
    if (auto native = try_as<::IWindowNative>()) {
        native->get_WindowHandle(&m_hwnd);
    }

    ApplyToolWindowStyle();
    ApplyAlwaysOnTop();
    // After the presenter change: SetBorderAndTitleBar re-applies the frame,
    // and the default corner preference with it.
    ApplyFrameless();

    // No acrylic. DWM draws a system backdrop across the whole window
    // rectangle, rounded only by its own 8px corner and ignoring the window
    // region, so behind a capsule it showed as light corners and a light rim.
    // A transparent backdrop leaves the Border as the only thing drawn.
    ApplyTransparentBackdrop();

    BuildCompositionLayer();
    ApplyClickThrough(m_persisted.click_through);
    RestoreFromPersistence();

    // Idle + Armed are *invisible* by design (per user). Pill only
    // materialises on Recording / Saving / Expanded.
    //   - Idle    → AppWindow.Hide()
    //   - Armed   → AppWindow.Hide() (M7 hotkey arming may revisit)
    //   - Recording / Saving / Expanded → AppWindow.Show()
    TransitionTo(::yip::IndicatorState::Idle, /*animate*/ false);
    HideWindow();

    auto dq = muxd::DispatcherQueue::GetForCurrentThread();

    // One-shot: holds the Saving frame for a beat after capture ends, then
    // drops to Idle. Replaces the old 5 Hz sync poll, which ran forever.
    m_savingTimer = dq.CreateTimer();
    m_savingTimer.Interval(std::chrono::milliseconds(kSavingHoldMs));
    m_savingTimer.IsRepeating(false);
    m_savingTimer.Tick([weak = get_weak()](auto&&, auto&&) {
        if (auto self = weak.get()) {
            if (!self->m_recording && self->m_state == ::yip::IndicatorState::Saving) {
                self->TransitionTo(::yip::IndicatorState::Idle, true);
            }
        }
    });

    // Meter polling timer — created stopped, started only while Recording.
    m_meterTimer = dq.CreateTimer();
    m_meterTimer.Interval(std::chrono::milliseconds(kMeterMs));
    m_meterTimer.IsRepeating(true);
    m_meterTimer.Tick([weak = get_weak()](auto&&, auto&&) {
        if (auto self = weak.get()) self->UpdateFromMeter();
    });

    m_collapseTimer = dq.CreateTimer();
    m_collapseTimer.Interval(std::chrono::milliseconds(kAutoCollapseMs));
    m_collapseTimer.IsRepeating(false);
    m_collapseTimer.Tick([weak = get_weak()](auto&&, auto&&) {
        if (auto self = weak.get()) {
            if (self->m_state == ::yip::IndicatorState::Expanded) {
                self->TransitionTo(self->m_baseState, true);
            }
        }
    });

    // A theme flip has to reach the composition brushes too — they are not
    // {ThemeResource} bindings, they are colours copied at build time.
    m_themeToken = Root().ActualThemeChanged({this, &IndicatorWindow::OnActualThemeChanged});

    // Subscribe last: the first callback can transition straight into
    // Recording, which touches every timer created above.
    m_stateToken = ::yip::RecordingStateBus::Subscribe(dq, [weak = get_weak()](bool recording) {
        if (auto self = weak.get()) self->OnRecordingStateChanged(recording);
    });
    if (::yip::RecordingStateBus::IsRecording()) {
        OnRecordingStateChanged(true);
    }
}

IndicatorWindow::~IndicatorWindow()
{
    ::yip::RecordingStateBus::Unsubscribe(m_stateToken);
    m_stateToken = 0;
    if (m_themeToken) {
        Root().ActualThemeChanged(m_themeToken);
        m_themeToken = {};
    }
    if (m_savingTimer) m_savingTimer.Stop();
    if (m_meterTimer) m_meterTimer.Stop();
    if (m_collapseTimer) m_collapseTimer.Stop();
    m_persisted.last_expanded = (m_state == ::yip::IndicatorState::Expanded);
    (void)m_persisted.Save();
}

// ============================================================ HWND style

void IndicatorWindow::ApplyToolWindowStyle()
{
    if (!m_hwnd) return;

    // Deliberately NOT WS_EX_LAYERED: a layered window with LWA_ALPHA is
    // opaque to the compositor, so the acrylic backdrop set in the constructor
    // never reached the screen and the pill's 62%-alpha tint composited onto
    // black. WS_EX_TRANSPARENT still routes WM_NCHITTEST through on its own,
    // which is all click-through needs.
    LONG_PTR ex = ::GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    ex |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    ex &= ~(WS_EX_APPWINDOW | WS_EX_LAYERED);
    ::SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex);
}

void IndicatorWindow::ApplyFrameless()
{
    if (!m_hwnd) return;

    // SetBorderAndTitleBar(false, false) still leaves WS_DLGFRAME and
    // WS_SYSMENU on the window, and with per-pixel alpha on that frame shows
    // as a 1px white rectangle around the capsule. Strip every frame bit.
    LONG_PTR style = ::GetWindowLongPtrW(m_hwnd, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU);
    ::SetWindowLongPtrW(m_hwnd, GWL_STYLE, style);
    ::SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // Windows 11 rounds top-level windows to 8px and strokes a 1px frame along
    // the rectangle; around a capsule both read as stray corners. Neither
    // attribute exists before Windows 11, where the calls simply fail.
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
    (void)::DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const COLORREF noBorder = DWMWA_COLOR_NONE;
    (void)::DwmSetWindowAttribute(m_hwnd, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
}

void IndicatorWindow::ApplyTransparentBackdrop()
{
    auto target = try_as<mucomp::ICompositionSupportsSystemBackdrop>();
    if (!target) return;

    // The backdrop brush comes from the system compositor
    // (Windows.UI.Composition), which needs a Windows.System dispatcher queue
    // on this thread; WinUI only guarantees the Microsoft.UI one.
    if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
        DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
        if (FAILED(::CreateDispatcherQueueController(
                options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(
                             winrt::put_abi(m_backdropQueue))))) {
            return;
        }
    }
    if (!m_backdropCompositor) m_backdropCompositor = winrt::Windows::UI::Composition::Compositor{};
    target.SystemBackdrop(m_backdropCompositor.CreateColorBrush(winrt::Microsoft::UI::Colors::Transparent()));

    // A transparent brush alone still composites onto black. Blur-behind with
    // a region entirely off the window is what turns on per-pixel alpha for
    // the window's content; nothing is actually blurred.
    if (m_hwnd) {
        DWM_BLURBEHIND blur{};
        blur.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
        blur.fEnable = TRUE;
        blur.hRgnBlur = ::CreateRectRgn(-2, -2, -1, -1);
        (void)::DwmEnableBlurBehindWindow(m_hwnd, &blur);
        if (blur.hRgnBlur) ::DeleteObject(blur.hRgnBlur);
    }
}

void IndicatorWindow::ApplyAlwaysOnTop()
{
    if (!m_hwnd) return;
    auto wid = AppWindow().Id();
    auto appWindow = muw::AppWindow::GetFromWindowId(wid);
    if (auto presenter = appWindow.Presenter().try_as<muw::OverlappedPresenter>()) {
        presenter.SetBorderAndTitleBar(false, false);
        presenter.IsResizable(false);
        presenter.IsMaximizable(false);
        presenter.IsMinimizable(false);
        presenter.IsAlwaysOnTop(true);
    }
}

void IndicatorWindow::ApplyClickThrough(bool enable)
{
    if (!m_hwnd) return;
    LONG_PTR ex = ::GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if (enable)
        ex |= WS_EX_TRANSPARENT;
    else
        ex &= ~WS_EX_TRANSPARENT;
    ::SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex);
    m_persisted.click_through = enable;
}

// ============================================================ Composition

void IndicatorWindow::BuildCompositionLayer()
{
    auto pillVisual = muxh::ElementCompositionPreview::GetElementVisual(PillFrame());
    m_compositor = pillVisual.Compositor();
    m_ease = StandardEase(m_compositor);

    // Brushes first: the bars and the dot below are handed one as they are
    // created.
    ResolveThemeBrushes();

    // Child visual tree for dot + meter bars. Parented to the meter host
    // (anchored within the pill grid by XAML layout so it stays inside
    // the visible clip).
    auto meterContainer = m_compositor.CreateContainerVisual();
    meterContainer.Size({kBarCount * (kBarWidth + kBarGap) - kBarGap, kBarMaxHeight + 4});
    muxh::ElementCompositionPreview::SetElementChildVisual(MeterHost(), meterContainer);

    // 4 vertical bars, anchored center-Y, with idle scale ~ 0.06 (a thin
    // resting glyph). Live updates drive Scale.Y via composition anims.
    for (int i = 0; i < kBarCount; ++i) {
        auto bar = m_compositor.CreateSpriteVisual();
        bar.Size({kBarWidth, kBarMaxHeight});
        bar.AnchorPoint({0.5f, 0.5f});
        bar.Offset({
            static_cast<float>(i) * (kBarWidth + kBarGap) + kBarWidth * 0.5f,
            (kBarMaxHeight + 4) * 0.5f,
            0.0f,
        });
        bar.Scale({1.0f, kBarRestScale, 1.0f});
        bar.Brush(m_barIdleBrush);
        meterContainer.Children().InsertAtTop(bar);
        m_barVisuals[static_cast<size_t>(i)] = bar;
    }

    // Recording dot — drawn as a 10×10 sprite visual on the DotHost element.
    auto dotContainer = m_compositor.CreateContainerVisual();
    dotContainer.Size({12.0f, 12.0f});
    muxh::ElementCompositionPreview::SetElementChildVisual(DotHost(), dotContainer);

    m_dotVisual = m_compositor.CreateSpriteVisual();
    m_dotVisual.Size({8.0f, 8.0f});
    m_dotVisual.AnchorPoint({0.5f, 0.5f});
    m_dotVisual.Offset({6.0f, 6.0f, 0.0f});
    m_dotVisual.Brush(m_dotNeutralBrush);
    // Composition has no "Border" geometry on SpriteVisual; we fake the
    // dot via a sized SpriteVisual with the matching corner clip below.
    auto dotClipGeo = m_compositor.CreateRoundedRectangleGeometry();
    dotClipGeo.Size({8.0f, 8.0f});
    dotClipGeo.CornerRadius({4.0f, 4.0f});
    dotClipGeo.Offset({0.0f, 0.0f});
    m_dotVisual.Clip(m_compositor.CreateGeometricClip(dotClipGeo));
    dotContainer.Children().InsertAtTop(m_dotVisual);
}

void IndicatorWindow::UpdateFromMeter()
{
    // One lock-free snapshot per tick: the pill used to call rec_peak_level()
    // while the main window called it too, and each read drained the other's.
    RecMeter snapshot{};
    if (::rec_meter(&snapshot) != REC_STATUS_OK) return;

    UpdateMeterBars(MeterNorm(snapshot.peak), snapshot.clip_count > 0);

    auto text = winrt::hstring{FormatPillElapsed(snapshot.elapsed_ms)};
    if (text != m_elapsedText) {
        m_elapsedText = text;
        ElapsedText().Text(text);
    }
}

void IndicatorWindow::UpdateMeterBars(float level, bool hot)
{
    const float clamped = std::clamp(level, 0.0f, 1.0f);
    const auto dur = std::chrono::milliseconds(kMeterMs);

    const bool wantHot = hot || clamped >= kBarHotThreshold;

    // Clipping pins the bars to the top of the ramp; otherwise they follow the
    // level through it.
    auto brush = m_barIdleBrush;
    if (!m_barPalette.empty()) {
        const auto last = static_cast<float>(m_barPalette.size() - 1);
        const auto index =
            wantHot ? m_barPalette.size() - 1 : static_cast<size_t>(std::lround(clamped * last));
        brush = m_barPalette[std::min(index, m_barPalette.size() - 1)];
    } else if (wantHot) {
        brush = m_barLiveBrush;
    }

    for (int i = 0; i < kBarCount; ++i) {
        auto& bar = m_barVisuals[static_cast<size_t>(i)];
        if (!bar) continue;
        if (brush) bar.Brush(brush);

        const float target = std::max(kBarRestScale, clamped * kBarWeights[i]);
        auto anim = m_compositor.CreateScalarKeyFrameAnimation();
        anim.InsertKeyFrame(1.0f, target, m_ease);
        anim.Duration(dur);
        bar.StartAnimation(L"Scale.Y", anim);
    }
}

void IndicatorWindow::UpdateDotForState(::yip::IndicatorState s)
{
    if (!m_dotVisual) return;
    // Expanding the pill mid-take is still a live take: the lamp stays red.
    const bool live = (s == ::yip::IndicatorState::Recording) ||
                      (s == ::yip::IndicatorState::Expanded && m_recording);
    m_dotVisual.Brush(live ? m_dotRecordBrush : m_dotNeutralBrush);

    // Pulse opacity gently during recording for "alive" feel.
    if (live) {
        auto pulse = m_compositor.CreateScalarKeyFrameAnimation();
        pulse.InsertKeyFrame(0.0f, 1.0f, m_ease);
        pulse.InsertKeyFrame(0.5f, 0.55f, m_ease);
        pulse.InsertKeyFrame(1.0f, 1.0f, m_ease);
        pulse.Duration(std::chrono::milliseconds(1200));
        pulse.IterationBehavior(mucomp::AnimationIterationBehavior::Forever);
        m_dotVisual.StartAnimation(L"Opacity", pulse);
    } else {
        m_dotVisual.StopAnimation(L"Opacity");
        m_dotVisual.Opacity(1.0f);
    }
}

void IndicatorWindow::StopMeterAnimations()
{
    // Leave the last elapsed time on screen through the Saving frame; only the
    // bars fall back, so the pill does not blank out mid-fade.
    for (auto& bar : m_barVisuals) {
        if (!bar) continue;
        bar.StopAnimation(L"Scale.Y");
        // Snap back to idle resting scale.
        auto anim = m_compositor.CreateScalarKeyFrameAnimation();
        anim.InsertKeyFrame(1.0f, kBarRestScale, m_ease);
        anim.Duration(std::chrono::milliseconds(kFadeMs));
        bar.StartAnimation(L"Scale.Y", anim);
    }
}

void IndicatorWindow::OnActualThemeChanged(winrt::Microsoft::UI::Xaml::FrameworkElement const& /*sender*/,
                                           winrt::Windows::Foundation::IInspectable const& /*args*/)
{
    ResolveThemeBrushes();
}

void IndicatorWindow::ResolveThemeBrushes()
{
    if (!m_compositor) return;

    // Reusing the brush objects rather than recreating them means every visual
    // already holding one repaints on a theme flip without being touched.
    const auto apply = [this](mucomp::CompositionColorBrush& brush, wchar_t const* key) {
        const auto color = ::yip::theme::Color(key, kMissingToken);
        if (brush) {
            brush.Color(color);
        } else {
            brush = m_compositor.CreateColorBrush(color);
        }
    };

    apply(m_barIdleBrush, L"YipIndicatorMeterBarIdleBrush");
    apply(m_barLiveBrush, L"YipIndicatorMeterBarLiveBrush");
    apply(m_dotNeutralBrush, L"YipIndicatorDotIdleBrush");
    apply(m_dotRecordBrush, L"YipIndicatorDotLiveBrush");

    // Four flat grey sticks beside a red dot read as a smudge at pill size.
    // Colouring them off the shared ramp makes the pill say the same thing
    // about a level that the main window's waveform does.
    const auto ramp = ::yip::theme::SampleMeterRamp(kBarPaletteSteps);
    if (ramp.size() == m_barPalette.size()) {
        for (size_t i = 0; i < ramp.size(); ++i)
            m_barPalette[i].Color(ramp[i]);
    } else {
        m_barPalette.clear();
        m_barPalette.reserve(ramp.size());
        for (auto const& color : ramp)
            m_barPalette.push_back(m_compositor.CreateColorBrush(color));
    }
}

// =========================================================== State machine

void IndicatorWindow::OnRecordingStateChanged(bool recording)
{
    if (m_recording == recording) return;
    m_recording = recording;

    if (recording) {
        if (m_savingTimer) m_savingTimer.Stop();
        // Expanded is a user-driven overlay; capture starting under it should
        // not yank the controls away. Just make sure the meter is live.
        if (m_state == ::yip::IndicatorState::Expanded) {
            if (m_meterTimer && !m_meterTimer.IsRunning()) m_meterTimer.Start();
            UpdateDotForState(m_state);
            return;
        }
        TransitionTo(::yip::IndicatorState::Recording, true);
        return;
    }

    // Capture ended: hold a Saving frame so the pill fades out instead of
    // vanishing, then the one-shot timer drops it to Idle.
    if (m_state == ::yip::IndicatorState::Recording || m_state == ::yip::IndicatorState::Expanded) {
        TransitionTo(::yip::IndicatorState::Saving, true);
    }
    if (m_savingTimer) {
        m_savingTimer.Stop();
        m_savingTimer.Start();
    }
}

void IndicatorWindow::TransitionTo(::yip::IndicatorState s, bool animate)
{
    // Size before show, or the first frame after Show() lands at the previous
    // state's size.
    SyncWindowToState(s);

    // Visibility gate — runs even when s == m_state on the very first call
    // from the constructor (m_state initialised to Idle).
    SyncVisibilityForState(s);

    if (s == m_state) return;

    // Remember "base" so Expanded can return after auto-collapse.
    if (s == ::yip::IndicatorState::Expanded) {
        m_baseState = m_state;
    }

    m_state = s;

    // XAML opacity transitions (composition-backed in WinUI 3).
    const bool wantActions = (s == ::yip::IndicatorState::Expanded);
    const bool wantMeter = (s == ::yip::IndicatorState::Recording || s == ::yip::IndicatorState::Expanded ||
                            s == ::yip::IndicatorState::Saving);

    // Collapsed, not transparent: at Opacity 0 the three buttons still took
    // their ~114px of the row, which left the timer a sliver of the 210px
    // recording pill and clipped it.
    ExpandedActions().Visibility(wantActions ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    ExpandedActions().IsHitTestVisible(wantActions);

    MeterHost().Opacity(wantMeter ? 1.0 : 0.0);
    ElapsedText().Opacity(wantMeter ? 1.0 : 0.0);

    AnimatePillToState(s, animate);
    UpdateDotForState(s);

    // Meter timer runs only while a meter is visible AND audio is live.
    const bool meterShouldRun = wantMeter && m_recording;
    if (m_meterTimer) {
        if (meterShouldRun && !m_meterTimer.IsRunning()) m_meterTimer.Start();
        if (!meterShouldRun && m_meterTimer.IsRunning()) {
            m_meterTimer.Stop();
            StopMeterAnimations();
        }
    }

    // Auto-collapse after 3s when expanded.
    if (s == ::yip::IndicatorState::Expanded)
        ResetAutoCollapseTimer();
    else
        StopAutoCollapseTimer();
}

void IndicatorWindow::SyncWindowToState(::yip::IndicatorState s)
{
    if (!m_hwnd) return;
    const auto g = GeometryFor(s);
    const int w = static_cast<int>(std::lround(g.w));
    const int h = static_cast<int>(std::lround(g.h));
    if (w <= 0 || h <= 0) return;

    // The window is the pill. It used to be a fixed 320x56 with a Composition
    // clip picking out the visible part — but the clip was centred while the
    // content is left-anchored, so the dot and the meter were cut off the left
    // edge and the timer lost its first digit. Sizing the window to the state
    // cannot drift out of step with the content.
    //
    // The HWND is sized in physical pixels while the geometry is DIPs:
    // unscaled, a 210x44 pill at 200% got a 105x22 window and lost half its
    // content off the right and bottom edges.
    const int pw = static_cast<int>(std::lround(g.w * DpiScale()));
    const int ph = static_cast<int>(std::lround(g.h * DpiScale()));
    auto appWindow = muw::AppWindow::GetFromWindowId(AppWindow().Id());
    if (appWindow) appWindow.Resize({pw, ph});

    // No window region: it is aliased, so it chewed the Border's antialiased
    // edge into a stepped rim, and with a transparent backdrop there is
    // nothing outside the capsule for it to hide.

    const double radius = g.h * 0.5;
    PillFrame().Width(g.w);
    PillFrame().Height(g.h);
    PillFrame().CornerRadius({radius, radius, radius, radius});
}

void IndicatorWindow::AnimatePillToState(::yip::IndicatorState s, bool animate)
{
    if (!m_compositor) return;
    const auto g = GeometryFor(s);

    // `animate == false` means "land on the resting values now" — used for the
    // initial state, which must not visibly fade in on launch. The size change
    // is instant: an HWND resize is not something to tween.
    const auto fadeMs = std::chrono::milliseconds(animate ? kFadeMs : 0);

    auto pillVisual = muxh::ElementCompositionPreview::GetElementVisual(PillFrame());
    auto fade = m_compositor.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1.0f, g.opacity, m_ease);
    fade.Duration(fadeMs);
    pillVisual.StartAnimation(L"Opacity", fade);
}

// =========================================================== Pointer + drag

void IndicatorWindow::OnPillPointerPressed(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                           muxi::PointerRoutedEventArgs const& args)
{
    // Ctrl+click → flip click-through. Do not capture; let the press fall
    // through as a "tap" for state expansion.
    const auto mods = args.KeyModifiers();
    if ((mods & winrt::Windows::System::VirtualKeyModifiers::Control) ==
        winrt::Windows::System::VirtualKeyModifiers::Control) {
        ApplyClickThrough(!m_persisted.click_through);
        (void)m_persisted.Save();
        return;
    }

    m_movedDuringPress = false;
    m_dragging = false;

    const auto pt = args.GetCurrentPoint(nullptr).Position();
    m_dragOrigin = pt;

    RECT rc{};
    ::GetWindowRect(m_hwnd, &rc);
    m_windowOriginAtDragStart = {static_cast<float>(rc.left), static_cast<float>(rc.top)};

    PillFrame().CapturePointer(args.Pointer());
    m_dragging = true;
}

void IndicatorWindow::OnPillPointerMoved(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                         muxi::PointerRoutedEventArgs const& args)
{
    if (!m_dragging || !m_hwnd) return;

    const auto pt = args.GetCurrentPoint(nullptr).Position();
    const float dx = pt.X - m_dragOrigin.X;
    const float dy = pt.Y - m_dragOrigin.Y;
    if (std::abs(dx) < 2.0f && std::abs(dy) < 2.0f) return;

    m_movedDuringPress = true;

    const int newX = static_cast<int>(std::lround(m_windowOriginAtDragStart.X + dx));
    const int newY = static_cast<int>(std::lround(m_windowOriginAtDragStart.Y + dy));

    auto wid = AppWindow().Id();
    auto appWindow = muw::AppWindow::GetFromWindowId(wid);
    appWindow.Move({newX, newY});
}

void IndicatorWindow::OnPillPointerReleased(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                            muxi::PointerRoutedEventArgs const& args)
{
    if (!m_dragging) return;
    m_dragging = false;
    PillFrame().ReleasePointerCapture(args.Pointer());

    if (m_movedDuringPress) {
        SnapToNearestEdgeIfClose();
        RememberPosition();
    }
}

void IndicatorWindow::OnPillPointerCaptureLost(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                               muxi::PointerRoutedEventArgs const& /*args*/)
{
    m_dragging = false;
}

void IndicatorWindow::OnPillTapped(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                   muxi::TappedRoutedEventArgs const& /*args*/)
{
    if (m_movedDuringPress) return; // drag, not tap

    if (m_state == ::yip::IndicatorState::Expanded) {
        TransitionTo(m_baseState, true);
    } else {
        TransitionTo(::yip::IndicatorState::Expanded, true);
    }
}

// =========================================================== Expanded actions

void IndicatorWindow::OnStopClicked(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                    mux::RoutedEventArgs const& /*args*/)
{
    (void)::rec_stop();
    // No transition here: rec_stop fires the audio-core state callback, and
    // OnRecordingStateChanged pulls the pill through Saving → Idle.
    ResetAutoCollapseTimer();
}

void IndicatorWindow::OnMarkClicked(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                    mux::RoutedEventArgs const& /*args*/)
{
    if (!::rec_is_recording()) return;
    const auto t_ms = ::rec_elapsed_ms();
    const char* path_utf8 = ::rec_current_path();
    if (!path_utf8) return;

    // utf8 → wide → path
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, nullptr, 0);
    std::wstring wide;
    if (n > 0) {
        wide.resize(static_cast<size_t>(n) - 1);
        ::MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, wide.data(), n);
    }
    std::filesystem::path p(wide);
    ::yip::markers::Marker m{t_ms, std::nullopt};
    (void)::yip::markers::Append(p, m);

    ResetAutoCollapseTimer();
}

void IndicatorWindow::OnOpenLastClicked(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                        mux::RoutedEventArgs const& /*args*/)
{
    std::wstring target;
    if (::rec_is_recording()) {
        const char* p = ::rec_current_path();
        if (p) {
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, p, -1, nullptr, 0);
            if (n > 0) {
                target.resize(static_cast<size_t>(n) - 1);
                ::MultiByteToWideChar(CP_UTF8, 0, p, -1, target.data(), n);
            }
        }
    }
    if (target.empty()) {
        // Last-modified .wav in the output folder.
        const auto root = ::yip::Settings::Load().output_folder;
        std::error_code ec;
        std::filesystem::file_time_type best{};
        std::filesystem::path bestPath;
        for (auto const& e : std::filesystem::directory_iterator(root, ec)) {
            if (ec) break;
            if (!e.is_regular_file()) continue;
            if (e.path().extension() != L".wav") continue;
            const auto t = std::filesystem::last_write_time(e.path(), ec);
            if (bestPath.empty() || t > best) {
                best = t;
                bestPath = e.path();
            }
        }
        if (!bestPath.empty()) target = bestPath.wstring();
    }
    if (target.empty()) return;

    std::wstring args = L"/select,\"" + target + L"\"";
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);

    ResetAutoCollapseTimer();
}

// =========================================================== Edge snap + persistence

double IndicatorWindow::DpiScale() const noexcept
{
    const UINT dpi = m_hwnd ? ::GetDpiForWindow(m_hwnd) : 0;
    return dpi > 0 ? static_cast<double>(dpi) / 96.0 : 1.0;
}

void IndicatorWindow::RestoreFromPersistence()
{
    if (!m_hwnd) return;

    // WorkArea is physical pixels, so the pill's DIP size has to be scaled
    // before it is used to centre or dock against it.
    const int winW = static_cast<int>(std::lround(kWindowW * DpiScale()));
    const int winH = static_cast<int>(std::lround(kWindowH * DpiScale()));

    auto wid = AppWindow().Id();
    auto appWindow = muw::AppWindow::GetFromWindowId(wid);

    // Find target monitor.
    auto displays = muw::DisplayArea::FindAll();
    muw::DisplayArea chosen{nullptr};
    for (auto const& d : displays) {
        if (d.DisplayId().Value == m_persisted.monitor_id) {
            chosen = d;
            break;
        }
    }
    if (!chosen) {
        chosen = muw::DisplayArea::Primary();
    }
    if (!chosen) return;

    const auto work = chosen.WorkArea();

    int x = work.X + (work.Width - winW) / 2;
    int y = work.Y + 20;

    const double t = std::clamp(m_persisted.edge_offset, 0.0, 1.0);
    switch (m_persisted.dock_edge) {
        case ::yip::DockEdge::Top:
            x = work.X + static_cast<int>(std::lround((work.Width - winW) * t));
            y = work.Y + 12;
            break;
        case ::yip::DockEdge::Bottom:
            x = work.X + static_cast<int>(std::lround((work.Width - winW) * t));
            y = work.Y + work.Height - winH - 12;
            break;
        case ::yip::DockEdge::Left:
            x = work.X + 12;
            y = work.Y + static_cast<int>(std::lround((work.Height - winH) * t));
            break;
        case ::yip::DockEdge::Right:
            x = work.X + work.Width - winW - 12;
            y = work.Y + static_cast<int>(std::lround((work.Height - winH) * t));
            break;
        case ::yip::DockEdge::None:
        default:
            break;
    }
    appWindow.Move({x, y});
}

void IndicatorWindow::SnapToNearestEdgeIfClose()
{
    if (!m_hwnd) return;
    RECT rc{};
    ::GetWindowRect(m_hwnd, &rc);
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;

    auto wid = AppWindow().Id();
    auto appWindow = muw::AppWindow::GetFromWindowId(wid);

    // Pick the DisplayArea containing the window center.
    muw::DisplayArea host{muw::DisplayArea::GetFromWindowId(wid, muw::DisplayAreaFallback::Primary)};
    if (!host) return;
    const auto work = host.WorkArea();

    const int winW = rc.right - rc.left;
    const int winH = rc.bottom - rc.top;

    const int distTop = std::abs(rc.top - work.Y);
    const int distBottom = std::abs((work.Y + work.Height) - rc.bottom);
    const int distLeft = std::abs(rc.left - work.X);
    const int distRight = std::abs((work.X + work.Width) - rc.right);
    const int minDist = std::min({distTop, distBottom, distLeft, distRight});
    if (minDist > kSnapPx) return;

    int x = rc.left;
    int y = rc.top;
    if (minDist == distTop)
        y = work.Y + 12;
    else if (minDist == distBottom)
        y = work.Y + work.Height - winH - 12;
    else if (minDist == distLeft)
        x = work.X + 12;
    else if (minDist == distRight)
        x = work.X + work.Width - winW - 12;
    appWindow.Move({x, y});
}

void IndicatorWindow::RememberPosition()
{
    if (!m_hwnd) return;

    auto wid = AppWindow().Id();
    muw::DisplayArea host{muw::DisplayArea::GetFromWindowId(wid, muw::DisplayAreaFallback::Primary)};
    if (!host) return;

    m_persisted.monitor_id = host.DisplayId().Value;

    RECT rc{};
    ::GetWindowRect(m_hwnd, &rc);
    const auto work = host.WorkArea();
    const int winW = rc.right - rc.left;
    const int winH = rc.bottom - rc.top;

    const int distTop = std::abs(rc.top - work.Y);
    const int distBottom = std::abs((work.Y + work.Height) - rc.bottom);
    const int distLeft = std::abs(rc.left - work.X);
    const int distRight = std::abs((work.X + work.Width) - rc.right);
    const int minDist = std::min({distTop, distBottom, distLeft, distRight});

    if (minDist > kSnapPx) {
        m_persisted.dock_edge = ::yip::DockEdge::None;
    } else if (minDist == distTop) {
        m_persisted.dock_edge = ::yip::DockEdge::Top;
        const int span = std::max(1, work.Width - winW);
        m_persisted.edge_offset = std::clamp(double(rc.left - work.X) / span, 0.0, 1.0);
    } else if (minDist == distBottom) {
        m_persisted.dock_edge = ::yip::DockEdge::Bottom;
        const int span = std::max(1, work.Width - winW);
        m_persisted.edge_offset = std::clamp(double(rc.left - work.X) / span, 0.0, 1.0);
    } else if (minDist == distLeft) {
        m_persisted.dock_edge = ::yip::DockEdge::Left;
        const int span = std::max(1, work.Height - winH);
        m_persisted.edge_offset = std::clamp(double(rc.top - work.Y) / span, 0.0, 1.0);
    } else {
        m_persisted.dock_edge = ::yip::DockEdge::Right;
        const int span = std::max(1, work.Height - winH);
        m_persisted.edge_offset = std::clamp(double(rc.top - work.Y) / span, 0.0, 1.0);
    }
    (void)m_persisted.Save();
}

// =========================================================== Visibility

void IndicatorWindow::ShowWindow()
{
    if (!m_hwnd) return;
    auto wid = AppWindow().Id();
    auto appWindow = muw::AppWindow::GetFromWindowId(wid);
    if (appWindow) appWindow.Show();
    // The presenter can put frame bits back while showing; strip them again
    // or the white 1px rectangle returns around the capsule.
    ApplyFrameless();
    // Re-assert TOPMOST + NoActivate after show, in case the Win32
    // show path clobbered them.
    ::SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void IndicatorWindow::HideWindow()
{
    if (!m_hwnd) return;
    auto wid = AppWindow().Id();
    auto appWindow = muw::AppWindow::GetFromWindowId(wid);
    if (appWindow) appWindow.Hide();
}

void IndicatorWindow::SyncVisibilityForState(::yip::IndicatorState s)
{
    using S = ::yip::IndicatorState;
    const bool visible = (s == S::Recording || s == S::Saving || s == S::Expanded);
    if (visible)
        ShowWindow();
    else
        HideWindow();
}

// =========================================================== Auto-collapse

void IndicatorWindow::ResetAutoCollapseTimer()
{
    if (!m_collapseTimer) return;
    m_collapseTimer.Stop();
    m_collapseTimer.Start();
}

void IndicatorWindow::StopAutoCollapseTimer()
{
    if (m_collapseTimer) m_collapseTimer.Stop();
}
} // namespace winrt::yip::implementation
