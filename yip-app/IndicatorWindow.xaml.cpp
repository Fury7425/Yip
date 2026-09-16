#include "pch.h"
#include "IndicatorWindow.xaml.h"

#if __has_include("IndicatorWindow.g.cpp")
#include "IndicatorWindow.g.cpp"
#endif

#include "ThemeColors.h"

#include <DispatcherQueue.h>
#include <dwmapi.h>
#include <microsoft.ui.xaml.window.h>
#include <windows.graphics.effects.interop.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

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
namespace wuc = winrt::Windows::UI::Composition;
namespace wge = winrt::Windows::Graphics::Effects;
namespace abi_ge = ABI::Windows::Graphics::Effects;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {
// Geometry and motion for the pill are owned here, not in App.xaml: only the
// padding the pill's XAML actually binds to lives there. Colours are the other
// way round — every one of them is resolved from the theme dictionaries by
// ResolveThemeBrushes().
constexpr int kMeterMs = 33;
constexpr int kAutoCollapseMs = 3000;
constexpr int kBarCount = 4;
constexpr float kBarWidth = 3.0f;
constexpr float kBarGap = 4.0f;
constexpr float kBarMaxHeight = 18.0f;
constexpr int kSavingHoldMs = 350; // how long the Saving frame stays up

// Motion. Entrances and the morph use a strong ease-out so the pill moves on
// the frame it is asked to; exits are shorter than entrances, because nobody
// is waiting to watch something leave.
constexpr int kFadeMs = 180;          // opacity settle between two visible states
constexpr int kShowMs = 220;          // hidden -> visible
constexpr int kHideMs = 160;          // visible -> hidden
constexpr int kMorphMs = 260;         // collapsed <-> expanded
constexpr int kActionsInMs = 180;     // buttons arriving, after the capsule has started to open
constexpr int kActionsInDelayMs = 70;
constexpr int kActionsOutMs = 90;     // buttons leaving, before the capsule closes over them
constexpr float kShowScale = 0.9f;    // never from zero: nothing appears out of nowhere
constexpr float kHideScale = 0.96f;
constexpr float kActionsSlidePx = 8.0f;

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
    // Collapsed, the readout (dot, meter, clock: ~93 DIP) sits centred with room
    // for the capsule's round ends either side. Expanded, it centres in the
    // space left of the two 34 DIP buttons, whose outer circle is concentric
    // with the capsule's end. The hidden states share the collapsed size so a
    // take starting never resizes a window that is about to appear.
    using S = ::yip::IndicatorState;
    switch (s) {
        case S::Idle:
        case S::Armed:
        case S::Recording:
            return {156.0f, 44.0f, 1.00f};
        case S::Saving:
            return {156.0f, 44.0f, 0.85f};
        case S::Expanded:
            return {232.0f, 56.0f, 1.00f};
    }
    return {156.0f, 44.0f, 1.00f};
}

bool IsShownState(::yip::IndicatorState s) noexcept
{
    // Idle + Armed are invisible by product decision; the pill only exists on
    // screen while there is a take to talk about.
    using S = ::yip::IndicatorState;
    return s == S::Recording || s == S::Saving || s == S::Expanded;
}

/// Where in the pill its anchor sits, as fractions of width and height. The
/// pill hangs from its top centre: that is the point that stays put when it
/// changes size, and the point it grows out of when it appears.
struct AnchorFraction {
    float x;
    float y;
};

constexpr AnchorFraction kAnchor{0.5f, 0.0f};

/// Gap between the pill and the top of the work area, in physical pixels.
constexpr int kHomeMarginPx = 12;

// The curves and animation helpers are templated over the compositor: the XAML
// compositor draws the pill, the system compositor draws the blur behind it,
// and the two have to move on the same curve.

// CubicBezier(0.4, 0.0, 0.2, 1.0) — Fluent standard easing, for the meter.
template <typename Compositor>
auto StandardEase(Compositor const& c)
{
    return c.CreateCubicBezierEasingFunction(float2{0.4f, 0.0f}, float2{0.2f, 1.0f});
}

// Strong ease-out for anything entering, leaving or answering a click.
template <typename Compositor>
auto StrongEaseOut(Compositor const& c)
{
    return c.CreateCubicBezierEasingFunction(float2{0.23f, 1.0f}, float2{0.32f, 1.0f});
}

// Drawer-style curve for the capsule changing size on screen: quick off the
// mark, long soft landing.
template <typename Compositor>
auto MorphEase(Compositor const& c)
{
    return c.CreateCubicBezierEasingFunction(float2{0.32f, 0.72f}, float2{0.0f, 1.0f});
}

template <typename Compositor, typename Target, typename Ease>
void AnimateScalar(Compositor const& c, Target const& target, wchar_t const* property, float to, int ms,
                   Ease const& ease, int delayMs = 0)
{
    auto anim = c.CreateScalarKeyFrameAnimation();
    anim.InsertKeyFrame(1.0f, to, ease);
    anim.Duration(std::chrono::milliseconds(std::max(ms, 1)));
    if (delayMs > 0) anim.DelayTime(std::chrono::milliseconds(delayMs));
    target.StartAnimation(property, anim);
}

template <typename Compositor, typename Target, typename Ease>
void AnimateVector2(Compositor const& c, Target const& target, wchar_t const* property, float2 to, int ms,
                    Ease const& ease)
{
    auto anim = c.CreateVector2KeyFrameAnimation();
    anim.InsertKeyFrame(1.0f, to, ease);
    anim.Duration(std::chrono::milliseconds(std::max(ms, 1)));
    target.StartAnimation(property, anim);
}

template <typename Compositor, typename Target, typename Ease>
void AnimateVector3(Compositor const& c, Target const& target, wchar_t const* property, float3 to, int ms,
                    Ease const& ease, int delayMs = 0)
{
    auto anim = c.CreateVector3KeyFrameAnimation();
    anim.InsertKeyFrame(1.0f, to, ease);
    anim.Duration(std::chrono::milliseconds(std::max(ms, 1)));
    if (delayMs > 0) anim.DelayTime(std::chrono::milliseconds(delayMs));
    target.StartAnimation(property, anim);
}

/// Jump an element's composition Translation, cancelling any animation on it.
/// The element must have had translation enabled (BuildCompositionLayer).
void SetTranslation(mux::UIElement const& element, float dx, float dy)
{
    auto visual = muxh::ElementCompositionPreview::GetElementVisual(element);
    visual.StopAnimation(L"Translation");
    visual.Properties().InsertVector3(L"Translation", float3{dx, dy, 0.0f});
}

// Shown only if a token key is wrong. A deliberate flat grey rather than a
// second copy of the palette, so a miss is visible instead of plausible.
constexpr winrt::Windows::UI::Color kMissingToken{0xFF, 0x80, 0x80, 0x80};

// CLSID_D2D1AlphaMask, spelled out so one GUID does not pull in d2d1effects_2.h
// and a dxguid.lib link.
constexpr GUID kAlphaMaskEffectId{0xc80ecff0, 0x3fd5, 0x4f05, {0x83, 0x28, 0xc5, 0xd1, 0x72, 0x4b, 0x4f, 0x0a}};

/// Direct2D's alpha-mask effect as a composition effect graph: source 0 is
/// multiplied by the alpha of source 1. Composition reads the effect through
/// the D2D1 interop metadata, which Win2D would normally provide; the project
/// has no Win2D, so this is that metadata by hand.
struct AlphaMaskEffect : winrt::implements<AlphaMaskEffect, wge::IGraphicsEffect, wge::IGraphicsEffectSource,
                                           abi_ge::IGraphicsEffectD2D1Interop> {
    AlphaMaskEffect(wge::IGraphicsEffectSource source, wge::IGraphicsEffectSource mask)
        : m_sources{std::move(source), std::move(mask)}
    {
    }

    winrt::hstring Name() const { return m_name; }
    void Name(winrt::hstring const& name) { m_name = name; }

    HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) noexcept override
    {
        if (!id) return E_POINTER;
        *id = kAlphaMaskEffectId;
        return S_OK;
    }

    // The effect has no properties, so there is nothing to name or animate.
    HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR, UINT*,
                                                      abi_ge::GRAPHICS_EFFECT_PROPERTY_MAPPING*) noexcept override
    {
        return E_INVALIDARG;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) noexcept override
    {
        if (!count) return E_POINTER;
        *count = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(UINT, ABI::Windows::Foundation::IPropertyValue**) noexcept override
    {
        return E_BOUNDS;
    }

    HRESULT STDMETHODCALLTYPE GetSource(UINT index, abi_ge::IGraphicsEffectSource** source) noexcept override
    {
        if (!source) return E_POINTER;
        if (index >= m_sources.size()) return E_BOUNDS;
        wge::IGraphicsEffectSource copy = m_sources[index];
        *source = static_cast<abi_ge::IGraphicsEffectSource*>(winrt::detach_abi(copy));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) noexcept override
    {
        if (!count) return E_POINTER;
        *count = static_cast<UINT>(m_sources.size());
        return S_OK;
    }

private:
    winrt::hstring m_name;
    std::array<wge::IGraphicsEffectSource, 2> m_sources;
};
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

    // Not acrylic. DWM draws a system backdrop across the whole window
    // rectangle, rounded only by its own 8px corner and ignoring the window
    // region, so behind a capsule acrylic showed as light corners and a light
    // rim. The blur is cut to the capsule by an alpha mask instead, and outside
    // the capsule the window stays fully transparent.
    ApplyBackdrop();

    BuildCompositionLayer();
    ApplyClickThrough(m_persisted.click_through);
    PlacePillAtHome();

    // Hidden until there is a take: m_state starts Idle and m_shown false, and
    // TransitionTo only ever shows the window for a visible state.
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

    // Transparency effects can be switched off in Settings or by power policy,
    // and the blur has to follow. The event arrives on a worker thread.
    m_effectsToken = m_uiSettings.AdvancedEffectsEnabledChanged([weak = get_weak(), dq](auto&&, auto&&) {
        dq.TryEnqueue([weak] {
            if (auto self = weak.get()) self->ApplyBackdrop();
        });
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
    if (m_effectsToken) {
        m_uiSettings.AdvancedEffectsEnabledChanged(m_effectsToken);
        m_effectsToken = {};
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

void IndicatorWindow::ApplyBackdrop()
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
    if (!m_backdropCompositor) m_backdropCompositor = wuc::Compositor{};

    // With transparency effects off the host backdrop stops being translucent,
    // so there is nothing worth masking: a transparent backdrop and the denser
    // tint, as before the blur existed.
    const bool wantBlur = m_uiSettings.AdvancedEffectsEnabled();
    if (wantBlur && !m_blurBrush) BuildBackdropBrush();
    m_blurActive = wantBlur && m_blurBrush;
    if (m_blurActive) {
        target.SystemBackdrop(m_blurBrush);
    } else {
        target.SystemBackdrop(m_backdropCompositor.CreateColorBrush(winrt::Microsoft::UI::Colors::Transparent()));
    }
    ApplySurfaceTint();

    // Either brush alone still composites onto black. DWM's own blur-behind,
    // with a region entirely off the window, is what turns on per-pixel alpha
    // for the window's content; DWM itself blurs nothing.
    if (m_hwnd) {
        DWM_BLURBEHIND blur{};
        blur.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
        blur.fEnable = TRUE;
        blur.hRgnBlur = ::CreateRectRgn(-2, -2, -1, -1);
        (void)::DwmEnableBlurBehindWindow(m_hwnd, &blur);
        if (blur.hRgnBlur) ::DeleteObject(blur.hRgnBlur);
    }
}

void IndicatorWindow::BuildBackdropBrush()
{
    auto const& c = m_backdropCompositor;
    try {
        // The mask is a capsule drawn into a visual surface. The DPI container
        // rasterises it at physical pixels, so its antialiased edge lines up
        // with the Border's; the shape visual under it mirrors the pill's
        // opacity and scale.
        auto shape = c.CreateRoundedRectangleGeometry();
        auto fill = c.CreateSpriteShape(shape);
        // Not a colour on screen: to the mask, opaque white just means alpha 1.
        fill.FillBrush(c.CreateColorBrush(winrt::Microsoft::UI::Colors::White()));
        auto visual = c.CreateShapeVisual();
        visual.Shapes().Append(fill);
        auto dpi = c.CreateContainerVisual();
        dpi.Children().InsertAtTop(visual);
        auto root = c.CreateContainerVisual();
        root.Children().InsertAtTop(dpi);
        auto surface = c.CreateVisualSurface();
        surface.SourceVisual(root);
        auto mask = c.CreateSurfaceBrush(surface);
        mask.Stretch(wuc::CompositionStretch::Fill);

        // The host backdrop arrives already blurred by the shell; the effect
        // only cuts it to the capsule.
        auto effect = winrt::make<AlphaMaskEffect>(wuc::CompositionEffectSourceParameter{L"Backdrop"},
                                                   wuc::CompositionEffectSourceParameter{L"Mask"});
        auto brush = c.CreateEffectFactory(effect).CreateBrush();
        brush.SetSourceParameter(L"Backdrop", c.CreateHostBackdropBrush());
        brush.SetSourceParameter(L"Mask", mask);

        // Rebuilt mid-take when effects come back on: start where the pill is.
        auto pill = PillVisual();
        visual.Opacity(pill.Opacity());
        visual.Scale(pill.Scale());
        visual.CenterPoint(pill.CenterPoint());

        m_maskShape = shape;
        m_maskVisual = visual;
        m_maskDpi = dpi;
        m_maskRoot = root;
        m_maskSurface = surface;
        m_backdropEaseOut = StrongEaseOut(c);
        m_backdropEaseMorph = MorphEase(c);
        m_blurBrush = brush;
        ResetBackdropShape();
    } catch (winrt::hresult_error const&) {
        // No effect support on this compositor: stay on the transparent backdrop.
        m_blurBrush = nullptr;
    }
}

void IndicatorWindow::ApplySurfaceTint()
{
    PillFrame().Background(
        ::yip::theme::Brush(m_blurActive ? L"YipIndicatorSurfaceBlurredBrush" : L"YipIndicatorSurfaceBrush"));
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
    m_easeOut = StrongEaseOut(m_compositor);
    m_easeMorph = MorphEase(m_compositor);

    // The morph clip. Built once, attached only while a size change runs.
    m_clipGeometry = m_compositor.CreateRoundedRectangleGeometry();
    m_clip = m_compositor.CreateGeometricClip(m_clipGeometry);

    // The readout glides and the buttons slide during a morph; both move via
    // composition Translation so no layout pass runs per frame.
    muxh::ElementCompositionPreview::SetIsTranslationEnabled(ReadoutGroup(), true);
    muxh::ElementCompositionPreview::SetIsTranslationEnabled(ExpandedActions(), true);

    // Brushes first: the bars and the dot below are handed one as they are
    // created.
    ResolveThemeBrushes();

    // Child visual tree for the meter bars, parented to the meter host (which
    // XAML layout places inside the readout group).
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

    // Recording dot — drawn as an 8×8 sprite visual on the DotHost element.
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

    // Pause is not on the state bus — a held take has not ended — so the
    // snapshot is where the UI finds out, including when something other than
    // this button did it.
    if (const bool paused = snapshot.paused != 0; paused != m_paused) {
        m_paused = paused;
        ApplyPausedVisuals();
    }

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
    // A paused one is not — nothing is reaching the file, so the lamp goes
    // neutral and stops pulsing.
    const bool live = !m_paused && ((s == ::yip::IndicatorState::Recording) ||
                                    (s == ::yip::IndicatorState::Expanded && m_recording));
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

void IndicatorWindow::ApplyPausedVisuals()
{
    // Segoe Fluent Icons: E768 Play, E769 Pause. The button shows what the
    // next click does, which is the opposite of what the session is doing.
    PauseGlyph().Glyph(m_paused ? L"\uE768" : L"\uE769");

    // Braces, not `=`: copy-init from a literal would need two user-defined
    // conversions to reach hstring.
    winrt::hstring const label{m_paused ? L"Resume" : L"Pause"};
    mux::Automation::AutomationProperties::SetName(PauseButton(), label);
    muxc::ToolTipService::SetToolTip(PauseButton(), winrt::box_value(label));

    UpdateDotForState(m_state);
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
    // Set from code, so it is a local value rather than a {ThemeResource}.
    ApplySurfaceTint();
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

    // Neither end of a session is ever paused. Land that before anything below
    // repaints the dot.
    if (m_paused) {
        m_paused = false;
        ApplyPausedVisuals();
    }

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
    using S = ::yip::IndicatorState;
    const bool show = IsShownState(s);
    if (s == m_state && show == m_shown) return;

    const auto from = m_state;
    // Remember "base" so Expanded can return after auto-collapse.
    if (s == S::Expanded && from != S::Expanded) m_baseState = from;
    m_state = s;
    ++m_motionGen;

    const bool wantMeter = (s == S::Recording || s == S::Expanded || s == S::Saving);
    // A pill on its way out keeps its last frame; blanking the readout
    // mid-fade reads as a glitch.
    if (show) {
        MeterHost().Opacity(wantMeter ? 1.0 : 0.0);
        ElapsedText().Opacity(wantMeter ? 1.0 : 0.0);
    }
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
    if (s == S::Expanded)
        ResetAutoCollapseTimer();
    else
        StopAutoCollapseTimer();

    if (!show) {
        HidePill(animate);
        return;
    }
    if (!m_shown) {
        // Size before show, or the first frame lands at the previous size.
        ApplyLayoutFor(s);
        ShowPill(animate);
        return;
    }
    if (animate) {
        MorphPill(from, s);
        return;
    }
    ApplyLayoutFor(s);
    SetPillFade(GeometryFor(s).opacity, 1.0f);
}

void IndicatorWindow::ApplyLayoutFor(::yip::IndicatorState s, ClipPolicy clip)
{
    // Collapsed, not transparent: at Opacity 0 the buttons still took their
    // width, which pushed the readout off centre.
    const bool wantActions = (s == ::yip::IndicatorState::Expanded);
    ExpandedActions().Visibility(wantActions ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    ExpandedActions().IsHitTestVisible(wantActions);

    auto actions = muxh::ElementCompositionPreview::GetElementVisual(ExpandedActions());
    actions.StopAnimation(L"Opacity");
    actions.Opacity(1.0f);
    SetTranslation(ExpandedActions(), 0.0f, 0.0f);
    SetTranslation(ReadoutGroup(), 0.0f, 0.0f);
    if (clip == ClipPolicy::Clear) {
        ClearClip();
        m_morphing = false;
    }

    SyncWindowToState(s, clip);
}

void IndicatorWindow::SyncWindowToState(::yip::IndicatorState s, ClipPolicy clip)
{
    const auto g = GeometryFor(s);

    // Border first, HWND last, always. MoveAndResize dispatches WM_SIZE
    // synchronously and the XAML island can commit a frame off the back of it;
    // everything that describes the new size has to be in place before that
    // happens, or that one frame shows the old capsule in the new window.
    //
    // No window region either: it is aliased, so it chewed the Border's
    // antialiased edge into a stepped rim, and with a transparent backdrop
    // there is nothing outside the capsule for it to hide.
    const double radius = g.h * 0.5;
    PillFrame().Width(g.w);
    PillFrame().Height(g.h);
    PillFrame().CornerRadius({radius, radius, radius, radius});

    // Keep means a morph owns the mask's capsule right now: the surface still
    // has to grow with the window, but the shape drawn into it is animating
    // and must not be snapped to the new size under it.
    if (clip == ClipPolicy::Clear)
        ResetBackdropShape();
    else
        SyncBackdropSurface();

    // The window is the pill. The HWND is sized in physical pixels while the
    // geometry is DIPs: unscaled, a pill at 200% got half a window and lost
    // its content off the right and bottom edges.
    //
    // It is sized around m_anchor rather than from its top-left corner, so a
    // pill centred on the screen stays centred as it expands instead of
    // growing off to the right.
    if (m_hwnd) {
        const double scale = DpiScale();
        const int pw = static_cast<int>(std::lround(g.w * scale));
        const int ph = static_cast<int>(std::lround(g.h * scale));
        if (pw > 0 && ph > 0) {
            const int x = static_cast<int>(m_anchor.x) - static_cast<int>(std::lround(pw * kAnchor.x));
            const int y = static_cast<int>(m_anchor.y) - static_cast<int>(std::lround(ph * kAnchor.y));
            auto appWindow = muw::AppWindow::GetFromWindowId(AppWindow().Id());
            if (appWindow) appWindow.MoveAndResize({x, y, pw, ph});
        }
    }
}

// =========================================================== Motion

void IndicatorWindow::ShowPill(bool animate)
{
    m_shown = true;
    const auto g = GeometryFor(m_state);
    // Grows out of its anchor — down from the top edge it is pinned to, not
    // outward from its middle.
    SetPillCentre(g.w * kAnchor.x, g.h * kAnchor.y);

    if (!animate) {
        SetPillFade(g.opacity, 1.0f);
        if (!m_windowVisible) ShowWindow();
        return;
    }

    // A pill still fading out is picked up from wherever it has got to.
    if (!m_windowVisible) {
        SetPillFade(0.0f, kShowScale);
        ShowWindow();
    }
    AnimatePillOpacity(g.opacity, kShowMs);
    AnimatePillScale(1.0f, kShowMs);
}

void IndicatorWindow::HidePill(bool animate)
{
    m_shown = false;
    if (!m_windowVisible) return;
    if (!animate) {
        HideWindow();
        return;
    }

    const auto w = static_cast<float>(PillFrame().Width());
    const auto h = static_cast<float>(PillFrame().Height());
    SetPillCentre(w * kAnchor.x, h * kAnchor.y);

    const auto gen = m_motionGen;
    auto batch = m_compositor.CreateScopedBatch(mucomp::CompositionBatchTypes::Animation);
    AnimatePillOpacity(0.0f, kHideMs);
    AnimatePillScale(kHideScale, kHideMs);
    batch.End();
    batch.Completed([weak = get_weak(), gen](auto&&, auto&&) {
        if (auto self = weak.get(); self && self->m_motionGen == gen && !self->m_shown) {
            self->HideWindow();
        }
    });
}

void IndicatorWindow::MorphPill(::yip::IndicatorState from, ::yip::IndicatorState to)
{
    // A morph already in flight left the HWND at one size and the clip
    // somewhere between two others. Land it before anything below measures:
    // `a` has to be a size the pill actually has, or the capsule jumps to a
    // width it never had and morphs out of that.
    if (m_morphing) {
        ApplyLayoutFor(from);
        PillFrame().UpdateLayout();
    }

    const auto a = GeometryFor(from);
    const auto b = GeometryFor(to);

    if (a.w == b.w && a.h == b.h) {
        ApplyLayoutFor(to);
        AnimatePillOpacity(b.opacity, kFadeMs);
        return;
    }

    // The HWND cannot be tweened, so the window jumps to whichever of the two
    // sizes is larger and a rounded clip draws the capsule between them. Where
    // the readout sits in each layout is measured, and the gap between the two
    // is played out as a composition translation, so it glides rather than
    // teleports. `kAnchor` says which part of the pill stays put on screen.
    const float dw = b.w - a.w;
    const float dh = b.h - a.h;
    const auto before = ReadoutCentre();
    auto readout = muxh::ElementCompositionPreview::GetElementVisual(ReadoutGroup());
    auto actions = muxh::ElementCompositionPreview::GetElementVisual(ExpandedActions());
    const auto gen = m_motionGen;

    auto batch = m_compositor.CreateScopedBatch(mucomp::CompositionBatchTypes::Animation);
    AnimatePillOpacity(b.opacity, kFadeMs);

    if (dw >= 0.0f) {
        // Opening: hold the old outline first, *then* take the new layout, then
        // let the outline go. Clipping before the resize is the whole point —
        // the resize can render a frame on its own, and with no clip installed
        // that frame is the full expanded capsule appearing out of nowhere.
        SetClip(a.w, a.h, dw * kAnchor.x, dh * kAnchor.y);
        ApplyLayoutFor(to, ClipPolicy::Keep);
        PillFrame().UpdateLayout();
        const auto after = ReadoutCentre();

        SetTranslation(ReadoutGroup(), before.X - after.X + dw * kAnchor.x,
                       before.Y - after.Y + dh * kAnchor.y);
        AnimateVector3(m_compositor, readout, L"Translation", {0.0f, 0.0f, 0.0f}, kMorphMs, m_easeMorph);

        AnimateClip(b.w, b.h, 0.0f, 0.0f);

        // The buttons arrive once there is room for them, sliding out from
        // behind the readout.
        actions.Opacity(0.0f);
        SetTranslation(ExpandedActions(), -kActionsSlidePx, 0.0f);
        AnimateScalar(m_compositor, actions, L"Opacity", 1.0f, kActionsInMs, m_easeOut, kActionsInDelayMs);
        AnimateVector3(m_compositor, actions, L"Translation", {0.0f, 0.0f, 0.0f}, kActionsInMs, m_easeOut,
                       kActionsInDelayMs);

        batch.End();
        batch.Completed([weak = get_weak(), gen](auto&&, auto&&) {
            if (auto self = weak.get(); self && self->m_motionGen == gen) {
                self->ClearClip();
                self->m_morphing = false;
            }
        });
        m_morphing = true;
        return;
    }

    // Closing: the buttons leave first, the outline closes over them, and only
    // then does the window shrink and the layout change underneath. Hit
    // testing goes with the fade, not with the layout pass 170 ms later — a
    // button nobody can see must not still be a button.
    ExpandedActions().IsHitTestVisible(false);
    AnimateScalar(m_compositor, actions, L"Opacity", 0.0f, kActionsOutMs, m_easeOut);

    // In the collapsed layout the readout is centred in the new outline.
    const float tx = -dw * kAnchor.x + b.w * 0.5f - before.X;
    const float ty = -dh * kAnchor.y + b.h * 0.5f - before.Y;
    AnimateVector3(m_compositor, readout, L"Translation", {tx, ty, 0.0f}, kMorphMs, m_easeMorph);

    SetClip(a.w, a.h, 0.0f, 0.0f);
    AnimateClip(b.w, b.h, -dw * kAnchor.x, -dh * kAnchor.y);

    batch.End();
    batch.Completed([weak = get_weak(), gen](auto&&, auto&&) {
        if (auto self = weak.get(); self && self->m_motionGen == gen) self->ApplyLayoutFor(self->m_state);
    });
    m_morphing = true;
}

void IndicatorWindow::SetClip(float w, float h, float x, float y)
{
    if (!m_clipGeometry) return;
    m_clipGeometry.StopAnimation(L"Size");
    m_clipGeometry.StopAnimation(L"Offset");
    m_clipGeometry.StopAnimation(L"CornerRadius");
    m_clipGeometry.Size({w, h});
    m_clipGeometry.Offset({x, y});
    m_clipGeometry.CornerRadius({h * 0.5f, h * 0.5f});
    PillVisual().Clip(m_clip);
    SetBackdropShape(w, h, x, y);
}

void IndicatorWindow::AnimateClip(float w, float h, float x, float y)
{
    if (!m_clipGeometry) return;
    AnimateVector2(m_compositor, m_clipGeometry, L"Size", {w, h}, kMorphMs, m_easeMorph);
    AnimateVector2(m_compositor, m_clipGeometry, L"Offset", {x, y}, kMorphMs, m_easeMorph);
    AnimateVector2(m_compositor, m_clipGeometry, L"CornerRadius", {h * 0.5f, h * 0.5f}, kMorphMs, m_easeMorph);

    if (!m_maskShape) return;
    AnimateVector2(m_backdropCompositor, m_maskShape, L"Size", {w, h}, kMorphMs, m_backdropEaseMorph);
    AnimateVector2(m_backdropCompositor, m_maskShape, L"Offset", {x, y}, kMorphMs, m_backdropEaseMorph);
    AnimateVector2(m_backdropCompositor, m_maskShape, L"CornerRadius", {h * 0.5f, h * 0.5f}, kMorphMs,
                   m_backdropEaseMorph);
}

void IndicatorWindow::ClearClip()
{
    if (!m_clipGeometry) return;
    m_clipGeometry.StopAnimation(L"Size");
    m_clipGeometry.StopAnimation(L"Offset");
    m_clipGeometry.StopAnimation(L"CornerRadius");
    PillVisual().Clip(nullptr);
    // Set, not merely stopped: the mask runs on the other compositor and may
    // still be a frame short of where the clip landed.
    ResetBackdropShape();
}

mucomp::Visual IndicatorWindow::PillVisual()
{
    return muxh::ElementCompositionPreview::GetElementVisual(PillFrame());
}

void IndicatorWindow::SetPillCentre(float x, float y)
{
    PillVisual().CenterPoint({x, y, 0.0f});
    if (m_maskVisual) m_maskVisual.CenterPoint({x, y, 0.0f});
}

void IndicatorWindow::SetPillFade(float opacity, float scale)
{
    const auto land = [opacity, scale](auto const& visual) {
        visual.StopAnimation(L"Opacity");
        visual.StopAnimation(L"Scale");
        visual.Opacity(opacity);
        visual.Scale({scale, scale, 1.0f});
    };
    land(PillVisual());
    if (m_maskVisual) land(m_maskVisual);
}

void IndicatorWindow::AnimatePillOpacity(float to, int ms)
{
    AnimateScalar(m_compositor, PillVisual(), L"Opacity", to, ms, m_easeOut);
    if (m_maskVisual) AnimateScalar(m_backdropCompositor, m_maskVisual, L"Opacity", to, ms, m_backdropEaseOut);
}

void IndicatorWindow::AnimatePillScale(float to, int ms)
{
    AnimateVector3(m_compositor, PillVisual(), L"Scale", {to, to, 1.0f}, ms, m_easeOut);
    if (m_maskVisual) {
        AnimateVector3(m_backdropCompositor, m_maskVisual, L"Scale", {to, to, 1.0f}, ms, m_backdropEaseOut);
    }
}

void IndicatorWindow::SetBackdropShape(float w, float h, float x, float y)
{
    if (!m_maskShape) return;
    m_maskShape.StopAnimation(L"Size");
    m_maskShape.StopAnimation(L"Offset");
    m_maskShape.StopAnimation(L"CornerRadius");
    m_maskShape.Size({w, h});
    m_maskShape.Offset({x, y});
    m_maskShape.CornerRadius({h * 0.5f, h * 0.5f});
}

void IndicatorWindow::SyncBackdropSurface()
{
    if (!m_maskSurface) return;
    const auto w = static_cast<float>(PillFrame().Width());
    const auto h = static_cast<float>(PillFrame().Height());
    if (!(w > 0.0f && h > 0.0f)) return; // NaN until the first SyncWindowToState

    // The surface is the window's size in physical pixels and the shape is in
    // DIPs, scaled up by the DPI container. The brush's Fill stretch then maps
    // the surface onto the window exactly, whatever units the backdrop is
    // painted in.
    const auto scale = static_cast<float>(DpiScale());
    const float2 px{w * scale, h * scale};
    m_maskSurface.SourceSize(px);
    m_maskRoot.Size(px);
    m_maskDpi.Size({w, h});
    m_maskDpi.Scale({scale, scale, 1.0f});
    m_maskVisual.Size({w, h});
}

void IndicatorWindow::ResetBackdropShape()
{
    SyncBackdropSurface();
    if (!m_maskShape) return;
    const auto w = static_cast<float>(PillFrame().Width());
    const auto h = static_cast<float>(PillFrame().Height());
    if (!(w > 0.0f && h > 0.0f)) return;
    SetBackdropShape(w, h, 0.0f, 0.0f);
}

winrt::Windows::Foundation::Point IndicatorWindow::ReadoutCentre()
{
    auto group = ReadoutGroup();
    const auto origin =
        group.TransformToVisual(PillFrame()).TransformPoint(winrt::Windows::Foundation::Point{0.0f, 0.0f});
    return {origin.X + static_cast<float>(group.ActualWidth()) * 0.5f,
            origin.Y + static_cast<float>(group.ActualHeight()) * 0.5f};
}

// ================================================================= Pointer

void IndicatorWindow::OnPillPointerPressed(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                           muxi::PointerRoutedEventArgs const& args)
{
    // Ctrl+click → flip click-through. Nothing else happens on the press: the
    // pill cannot be moved, so there is no drag to start and no pointer to
    // capture, and a plain press falls through as a "tap" for state expansion.
    const auto mods = args.KeyModifiers();
    if ((mods & winrt::Windows::System::VirtualKeyModifiers::Control) ==
        winrt::Windows::System::VirtualKeyModifiers::Control) {
        ApplyClickThrough(!m_persisted.click_through);
        (void)m_persisted.Save();
    }
}

void IndicatorWindow::OnPillTapped(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                   muxi::TappedRoutedEventArgs const& /*args*/)
{
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

void IndicatorWindow::OnPauseClicked(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                    mux::RoutedEventArgs const& /*args*/)
{
    // Both calls are idempotent and refuse a dead session, so the return value
    // says nothing the next read does not. Ask audio-core what actually
    // happened rather than assuming the flip took.
    if (m_paused)
        (void)::rec_resume();
    else
        (void)::rec_pause();

    // Repaint now instead of waiting up to a meter tick: the button has to
    // answer the click on the frame it was clicked.
    m_paused = ::rec_is_paused() != 0;
    ApplyPausedVisuals();

    ResetAutoCollapseTimer();
}

// =============================================================== Placement

double IndicatorWindow::DpiScale() const noexcept
{
    const UINT dpi = m_hwnd ? ::GetDpiForWindow(m_hwnd) : 0;
    return dpi > 0 ? static_cast<double>(dpi) / 96.0 : 1.0;
}

void IndicatorWindow::PlacePillAtHome()
{
    if (!m_hwnd) return;

    // WorkArea is physical pixels, so the pill's DIP size has to be scaled
    // before it is used to centre against it. Placed at the size it will first
    // appear at, so the anchor derived below is exact.
    const auto g = GeometryFor(::yip::IndicatorState::Recording);
    const int winW = static_cast<int>(std::lround(g.w * DpiScale()));
    const int winH = static_cast<int>(std::lround(g.h * DpiScale()));

    auto appWindow = muw::AppWindow::GetFromWindowId(AppWindow().Id());
    auto primary = muw::DisplayArea::Primary();
    if (!appWindow || !primary) return;

    const auto work = primary.WorkArea();
    const int x = work.X + (work.Width - winW) / 2;
    const int y = work.Y + kHomeMarginPx;
    appWindow.MoveAndResize({x, y, winW, winH});

    m_anchor.x = x + static_cast<LONG>(std::lround(winW * kAnchor.x));
    m_anchor.y = y + static_cast<LONG>(std::lround(winH * kAnchor.y));
}

// =========================================================== Visibility

void IndicatorWindow::ShowWindow()
{
    if (!m_hwnd) return;
    auto wid = AppWindow().Id();
    auto appWindow = muw::AppWindow::GetFromWindowId(wid);
    if (appWindow) appWindow.Show();
    m_windowVisible = true;
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
    m_windowVisible = false;
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
