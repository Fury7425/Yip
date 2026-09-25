#include "pch.h"
#include "IndicatorWindow.xaml.h"

#if __has_include("IndicatorWindow.g.cpp")
#include "IndicatorWindow.g.cpp"
#endif

#include "Settings.h"
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
// 25 Hz. Every tick that moves a bar costs one compositor frame — the blurred
// backdrop and its mask included — so the rate is the GPU cost of a take.
// The bars land on each tick's level rather than gliding to it: a glide a
// little longer than a tick never finished before the next one started, so
// through any live signal the pill redrew at the display's refresh rate
// (60–165 frames a second) instead of the tick rate.
constexpr int kMeterMs = 40;
constexpr int kAutoCollapseMs = 3000;
constexpr int kBarCount = 4;
constexpr float kBarWidth = 3.0f;
constexpr float kBarGap = 4.0f;
constexpr float kBarMaxHeight = 18.0f;
constexpr int kSavingHoldMs = 350; // how long the Saving frame stays up

// Motion. The pill's shapes are one goo: the entrance drips from the screen
// edge, and expanding splits the Pause and Stop discs off the capsule. Every
// shape motion ends in a small overshoot and spring-back, so the goo lands
// like a liquid rather than stopping dead. Every frame of a motion redraws the
// goo and the blur behind it at the display's refresh rate, so a motion's
// length is its GPU cost: measured, the effect itself is a small part of it.
constexpr int kFadeMs = 180;           // opacity settle between two visible states (Recording <-> Saving)
constexpr int kDripInMs = 560;         // hidden -> visible: the drop hangs, pinches off and lands
constexpr int kDripOutMs = 400;        // visible -> hidden: the same drip backwards; nobody waits to watch it go
constexpr int kSplitMs = 380;          // collapsed <-> expanded, bounce included
constexpr float kSplitStagger = 0.12f; // Stop trails Pause out and leads it back in, as a fraction of kSplitMs

// The bounce. A shape reaching its spot runs kBounceTravel past it, stretched
// along its path, springs back a little the other way, then settles. A shape
// that only grows swells past its size instead.
constexpr float kBounceTravel = 5.0f;
constexpr float kBounceStretch = 5.0f; // added to the width while it overshoots
constexpr float kBounceSquash = 3.0f;  // taken off the height while it overshoots

// Bottom of the bar meter, in dBFS. Same curve as the main window: on a linear
// amplitude scale these bars barely leave the floor.
constexpr double kMeterFloorDb = -60.0;
constexpr float kSilenceFloor = 1e-7f;

// Resting scale of a bar. Small enough to read as a dash, not a zero-height
// glitch.
constexpr float kBarRestScale = 0.06f;

/// A bar's height, in DIPs, for a fraction of the full travel.
constexpr float BarHeight(float fraction) noexcept
{
    return kBarMaxHeight * fraction;
}

// Per-bar weighting. The pair in the middle run tallest, which reads as a
// level meter rather than four identical sticks.
constexpr float kBarWeights[kBarCount] = {0.62f, 1.00f, 0.86f, 0.50f};

// Above this much of the travel the bars take the hot colour — roughly the
// last 6 dB, which is the headroom worth worrying about.
constexpr float kBarHotThreshold = 0.86f;

// A bar height change smaller than this (DIPs) is left alone: under a pixel at
// 100% scale and a small fraction of the 18 DIP travel, so nobody sees it, but
// animating it keeps the compositor busy through steady tone and room noise.
constexpr float kBarTargetEpsilon = 0.75f;

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

// Geometry, in DIPs. Collapsed, the readout (dot, meter, clock: ~93 DIP) sits
// centred in the capsule with room for its round ends. Expanded, the Pause and
// Stop discs sit beside it, a gap apart; in the dot style the lamp's disc
// grows to their size and the three sit in a row.
constexpr float kPillW = 156.0f;
constexpr float kPillH = 44.0f;
// Expanded, the capsule gives up some of its length to the discs it sheds, so
// the goo reads as one amount of liquid divided rather than two discs made
// from nothing. Still room for the readout at "H:MM:SS" (~108 DIP).
constexpr float kPillSplitW = 124.0f;
// The dot style's disc: the 8 DIP lamp with enough glass round it to read on
// any wallpaper and still take a click.
constexpr float kDotSize = 24.0f;
constexpr float kActionDot = 40.0f;
// Resting gap between shapes. The goo fuses anything closer than about one
// and a half blur radii, so this has to stay well clear of that or the discs
// never part.
constexpr float kSplitGap = 12.0f;
/// Gap between the resting pill and the edge of the work area it hangs from.
constexpr float kHomeMarginDip = 12.0f;
// Room around the resting shapes for the bounce to overshoot into. The window
// reaches the screen edge itself, so the entrance has an edge to drip from.
constexpr float kBounceRoom = 12.0f;
constexpr float kWindowW = std::max(kPillW, kPillSplitW + 2.0f * (kSplitGap + kActionDot)) + 2.0f * kBounceRoom;
constexpr float kWindowH = kHomeMarginDip + kPillH + kBounceRoom;

// The goo: the shapes are blurred by kGooBlurDip, then cut where the blurred
// alpha crosses a threshold (alpha' = kGooGain * alpha + offset, clamped).
// Apart, each shape keeps its outline; within a couple of blur radii of each
// other they melt together. The fill cuts at ~0.48, which leaves a straight
// edge where it was. The rim cuts lower, so its outer edge lands about a
// quarter of a blur radius (~1 DIP) outside the fill: that ring is the quiet
// stroke the capsule's Border used to draw.
constexpr float kGooBlurDip = 4.0f;
constexpr float kGooGain = 24.0f;
constexpr float kGooFillOffset = -11.0f;
constexpr float kGooRimOffset = -9.0f;

/// One shape: centre across the window, the centre's distance from the
/// screen edge the pill hangs from, and size. Measured from the edge, one set
/// of numbers serves the pill at the top of the screen and at the bottom.
struct Blob {
    float cx;
    float d;
    float w;
    float h;
};

struct PillLayout {
    Blob cap;
    Blob pause;
    Blob stop;
};

/// Where the shapes rest. Collapsed, Pause and Stop are tucked away at zero
/// size inside the capsule (or the dot's middle), which is where they split
/// from and melt back into. In the pill style that is the round end the
/// shorter, expanded capsule comes to rest with, so the discs travel out of
/// the capsule while it draws back from them.
PillLayout LayoutFor(bool dot, bool expanded) noexcept
{
    constexpr float c = kWindowW * 0.5f;
    if (!dot) {
        constexpr float d = kHomeMarginDip + kPillH * 0.5f;
        constexpr float left = c - (kPillSplitW + 2.0f * (kSplitGap + kActionDot)) * 0.5f;
        if (!expanded) {
            constexpr Blob tucked{left + kPillSplitW - kPillH * 0.5f, d, 0.0f, 0.0f};
            return {{c, d, kPillW, kPillH}, tucked, tucked};
        }
        return {{left + kPillSplitW * 0.5f, d, kPillSplitW, kPillH},
                {left + kPillSplitW + kSplitGap + kActionDot * 0.5f, d, kActionDot, kActionDot},
                {left + kPillSplitW + 2.0f * kSplitGap + kActionDot * 1.5f, d, kActionDot, kActionDot}};
    }
    if (!expanded) {
        constexpr float d = kHomeMarginDip + kDotSize * 0.5f;
        constexpr Blob tucked{c, d, 0.0f, 0.0f};
        return {{c, d, kDotSize, kDotSize}, tucked, tucked};
    }
    constexpr float d = kHomeMarginDip + kActionDot * 0.5f;
    constexpr float step = kActionDot + kSplitGap;
    return {{c - step, d, kActionDot, kActionDot},
            {c, d, kActionDot, kActionDot},
            {c + step, d, kActionDot, kActionDot}};
}

/// A shape shrunk to nothing where `b` is.
constexpr Blob Gone(Blob const& b) noexcept
{
    return {b.cx, b.d, 0.0f, 0.0f};
}

// Where the edge and the neck go when they are done: back into the screen edge.
constexpr Blob kIntoEdge{kWindowW * 0.5f, -10.0f, 0.0f, 0.0f};

/// A distance from the screen edge as a window y, in DIPs.
float BlobY(float d, bool bottom) noexcept
{
    return bottom ? kWindowH - d : d;
}

float OpacityFor(::yip::IndicatorState s) noexcept
{
    return s == ::yip::IndicatorState::Saving ? 0.85f : 1.0f;
}

bool IsShownState(::yip::IndicatorState s) noexcept
{
    // Idle + Armed are invisible by product decision; the pill only exists on
    // screen while there is a take to talk about.
    using S = ::yip::IndicatorState;
    return s == S::Recording || s == S::Saving || s == S::Expanded;
}

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

// Drawer-style curve for a shape travelling on screen: quick off the mark,
// long soft landing.
template <typename Compositor>
auto MorphEase(Compositor const& c)
{
    return c.CreateCubicBezierEasingFunction(float2{0.32f, 0.72f}, float2{0.0f, 1.0f});
}

template <typename EasingFunction, typename Compositor>
winrt::yip::implementation::PillEases<EasingFunction> MakeEases(Compositor const& c)
{
    winrt::yip::implementation::PillEases<EasingFunction> e;
    e.standard = StandardEase(c);
    e.out = StrongEaseOut(c);
    e.morph = MorphEase(c);
    // The drop stretching under its own weight: slow to start, slow to stop.
    e.inOut = c.CreateCubicBezierEasingFunction(float2{0.65f, 0.0f}, float2{0.35f, 1.0f});
    e.linear = c.CreateLinearEasingFunction();
    return e;
}

enum class Ease : uint8_t { Standard, Out, Morph, InOut, Linear };

template <typename EasingFunction>
EasingFunction const& Pick(winrt::yip::implementation::PillEases<EasingFunction> const& e, Ease ease) noexcept
{
    switch (ease) {
        case Ease::Out:
            return e.out;
        case Ease::Morph:
            return e.morph;
        case Ease::InOut:
            return e.inOut;
        case Ease::Linear:
            return e.linear;
        case Ease::Standard:
            break;
    }
    return e.standard;
}

/// A keyframe for a shape. `fromCurrent` holds the shape wherever it is when
/// the motion starts (mid-way through another one included) until time `t`.
struct BlobKey {
    float t;
    Blob b;
    Ease ease;
    bool fromCurrent;
};

struct ScalarKey {
    float t;
    float v;
    Ease ease;
    bool fromCurrent;
};

constexpr BlobKey Hold(float t) noexcept
{
    return {t, {}, Ease::Linear, true};
}

constexpr BlobKey At(float t, Blob const& b, Ease ease) noexcept
{
    return {t, b, ease, false};
}

constexpr ScalarKey HoldValue(float t) noexcept
{
    return {t, 0.0f, Ease::Linear, true};
}

constexpr ScalarKey Fade(float t, float v, Ease ease) noexcept
{
    return {t, v, ease, false};
}

float2 BlobOffset(Blob const& b, bool bottom) noexcept
{
    return {b.cx - b.w * 0.5f, BlobY(b.d, bottom) - b.h * 0.5f};
}

/// Put a shape at `b` at once, cancelling any motion on it.
template <typename Geometry>
void SetBlob(Geometry const& g, Blob const& b, bool bottom)
{
    if (!g) return;
    g.StopAnimation(L"Size");
    g.StopAnimation(L"Offset");
    g.StopAnimation(L"CornerRadius");
    const float r = std::min(b.w, b.h) * 0.5f;
    g.Size({b.w, b.h});
    g.Offset(BlobOffset(b, bottom));
    g.CornerRadius({r, r});
}

/// Last key that names a place, for landing a motion at once.
template <typename Key>
Key const* LastPlace(std::vector<Key> const& keys) noexcept
{
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        if (!it->fromCurrent) return &*it;
    }
    return nullptr;
}

/// Play `keys` on a shape: size, position and corner radius move together,
/// the radius always half the shorter side, so every in-between is a capsule.
template <typename Compositor, typename Geometry, typename EasingFunction>
void AnimateBlob(Compositor const& c, Geometry const& g, std::vector<BlobKey> const& keys, int ms,
                 winrt::yip::implementation::PillEases<EasingFunction> const& eases, bool bottom)
{
    if (!g || keys.empty()) return;
    if (ms <= 0 || keys.size() == 1) {
        if (auto const* last = LastPlace(keys)) SetBlob(g, last->b, bottom);
        return;
    }
    auto size = c.CreateVector2KeyFrameAnimation();
    auto offset = c.CreateVector2KeyFrameAnimation();
    auto radius = c.CreateVector2KeyFrameAnimation();
    for (auto const& k : keys) {
        auto const& ease = Pick(eases, k.ease);
        if (k.fromCurrent) {
            size.InsertExpressionKeyFrame(k.t, L"this.StartingValue", ease);
            offset.InsertExpressionKeyFrame(k.t, L"this.StartingValue", ease);
            radius.InsertExpressionKeyFrame(k.t, L"this.StartingValue", ease);
            continue;
        }
        const float r = std::min(k.b.w, k.b.h) * 0.5f;
        size.InsertKeyFrame(k.t, {k.b.w, k.b.h}, ease);
        offset.InsertKeyFrame(k.t, BlobOffset(k.b, bottom), ease);
        radius.InsertKeyFrame(k.t, {r, r}, ease);
    }
    const auto duration = std::chrono::milliseconds(ms);
    size.Duration(duration);
    offset.Duration(duration);
    radius.Duration(duration);
    g.StartAnimation(L"Size", size);
    g.StartAnimation(L"Offset", offset);
    g.StartAnimation(L"CornerRadius", radius);
}

template <typename Compositor, typename Visual, typename EasingFunction>
void AnimateOpacity(Compositor const& c, Visual const& visual, std::vector<ScalarKey> const& keys, int ms,
                    winrt::yip::implementation::PillEases<EasingFunction> const& eases)
{
    if (!visual || keys.empty()) return;
    if (ms <= 0 || keys.size() == 1) {
        if (auto const* last = LastPlace(keys)) {
            visual.StopAnimation(L"Opacity");
            visual.Opacity(last->v);
        }
        return;
    }
    auto anim = c.CreateScalarKeyFrameAnimation();
    for (auto const& k : keys) {
        if (k.fromCurrent)
            anim.InsertExpressionKeyFrame(k.t, L"this.StartingValue", Pick(eases, k.ease));
        else
            anim.InsertKeyFrame(k.t, k.v, Pick(eases, k.ease));
    }
    anim.Duration(std::chrono::milliseconds(ms));
    visual.StartAnimation(L"Opacity", anim);
}

/// Travel from wherever the shape is to `to`, run past it the way it was
/// heading (`dir`: -1 left, +1 right, 0 for a shape that only grows), spring
/// back a little, settle. `start` holds it in place that long first.
std::vector<BlobKey> BounceTo(Blob const& to, float dir, float start)
{
    const auto at = [start](float t) { return start + (1.0f - start) * t; };
    Blob over = to;
    Blob back = to;
    if (dir == 0.0f) {
        over.w = to.w + kBounceStretch;
        over.h = to.h + kBounceStretch;
        back.w = to.w - kBounceStretch * 0.4f;
        back.h = to.h - kBounceStretch * 0.4f;
    } else {
        over.cx = to.cx + dir * kBounceTravel;
        over.w = to.w + kBounceStretch;
        over.h = to.h - kBounceSquash;
        back.cx = to.cx - dir * kBounceTravel * 0.3f;
        back.w = to.w - kBounceStretch * 0.4f;
        back.h = to.h + kBounceSquash * 0.5f;
    }
    std::vector<BlobKey> keys{Hold(0.0f)};
    if (start > 0.0f) keys.push_back(Hold(start));
    keys.push_back(At(at(0.55f), over, Ease::Morph));
    keys.push_back(At(at(0.8f), back, Ease::Standard));
    keys.push_back(At(1.0f, to, Ease::Standard));
    return keys;
}

/// Which way a shape travels between two layouts.
float Heading(Blob const& from, Blob const& to) noexcept
{
    if (to.cx > from.cx + 0.5f) return 1.0f;
    if (to.cx < from.cx - 0.5f) return -1.0f;
    return 0.0f;
}

/// Edge and neck, wherever a cut-short entrance left them, back into the edge.
std::vector<BlobKey> Retract()
{
    return {Hold(0.0f), At(0.3f, kIntoEdge, Ease::Standard)};
}

template <typename Compositor, typename Target, typename Easing>
void AnimateScalar(Compositor const& c, Target const& target, wchar_t const* property, float to, int ms,
                   Easing const& ease, int delayMs = 0)
{
    auto anim = c.CreateScalarKeyFrameAnimation();
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

/// Keep an element's composition Translation on `shape`'s centre, given the
/// centre layout rests the element on. While the shape travels the element
/// rides along with no layout pass; where they meet the translation is zero.
void Follow(mucomp::Compositor const& c, mux::UIElement const& element,
            mucomp::CompositionRoundedRectangleGeometry const& shape, float2 rest)
{
    if (!shape) return;
    auto expr = c.CreateExpressionAnimation(
        L"Vector3(g.Offset.X + g.Size.X * 0.5 - rest.X, g.Offset.Y + g.Size.Y * 0.5 - rest.Y, 0)");
    expr.SetReferenceParameter(L"g", shape);
    expr.SetVector2Parameter(L"rest", rest);
    muxh::ElementCompositionPreview::GetElementVisual(element).StartAnimation(L"Translation", expr);
}

// Shown only if a token key is wrong. A deliberate flat grey rather than a
// second copy of the palette, so a miss is visible instead of plausible.
constexpr winrt::Windows::UI::Color kMissingToken{0xFF, 0x80, 0x80, 0x80};

// Direct2D effect CLSIDs, spelled out so three GUIDs do not pull in
// d2d1effects_2.h and a dxguid.lib link.
constexpr GUID kAlphaMaskEffectId{0xc80ecff0, 0x3fd5, 0x4f05, {0x83, 0x28, 0xc5, 0xd1, 0x72, 0x4b, 0x4f, 0x0a}};
constexpr GUID kGaussianBlurEffectId{0x1feb6d69, 0x2fe6, 0x4ac9, {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};
constexpr GUID kColorMatrixEffectId{0x921f03d6, 0x641c, 0x47df, {0x85, 0x2d, 0xb4, 0xbb, 0x61, 0x53, 0xae, 0x11}};

// The D2D enum values the effects below are set with.
constexpr uint32_t kBlurOptimizationBalanced = 1; // D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED
constexpr uint32_t kBorderModeSoft = 0;           // D2D1_BORDER_MODE_SOFT
constexpr uint32_t kColorMatrixPremultiplied = 1; // D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED
constexpr uint32_t kColorMatrixStraight = 2;      // D2D1_COLORMATRIX_ALPHA_MODE_STRAIGHT: the matrix as is

/// A Direct2D effect as a composition effect graph node. Composition reads an
/// effect through the D2D1 interop metadata, which Win2D would normally
/// provide; the project has no Win2D, so this is that metadata by hand. The
/// properties are in D2D's own index order, as the property values D2D takes.
struct D2DEffect : winrt::implements<D2DEffect, wge::IGraphicsEffect, wge::IGraphicsEffectSource,
                                     abi_ge::IGraphicsEffectD2D1Interop> {
    D2DEffect(GUID const& id, std::vector<wge::IGraphicsEffectSource> sources,
              std::vector<winrt::Windows::Foundation::IInspectable> properties)
        : m_id{id}, m_sources{std::move(sources)}, m_properties{std::move(properties)}
    {
    }

    winrt::hstring Name() const { return m_name; }
    void Name(winrt::hstring const& name) { m_name = name; }

    HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) noexcept override
    {
        if (!id) return E_POINTER;
        *id = m_id;
        return S_OK;
    }

    // Nothing is animated, so no property needs a name.
    HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR, UINT*,
                                                      abi_ge::GRAPHICS_EFFECT_PROPERTY_MAPPING*) noexcept override
    {
        return E_INVALIDARG;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) noexcept override
    {
        if (!count) return E_POINTER;
        *count = static_cast<UINT>(m_properties.size());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override
    {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (index >= m_properties.size()) return E_BOUNDS;
        try {
            auto property = m_properties[index].as<winrt::Windows::Foundation::IPropertyValue>();
            *value = static_cast<ABI::Windows::Foundation::IPropertyValue*>(winrt::detach_abi(property));
            return S_OK;
        } catch (...) {
            return winrt::to_hresult();
        }
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
    GUID m_id;
    winrt::hstring m_name;
    std::vector<wge::IGraphicsEffectSource> m_sources;
    std::vector<winrt::Windows::Foundation::IInspectable> m_properties;
};

using winrt::Windows::Foundation::PropertyValue;

wge::IGraphicsEffect Effect(GUID const& id, std::vector<wge::IGraphicsEffectSource> sources,
                            std::vector<winrt::Windows::Foundation::IInspectable> properties = {})
{
    return winrt::make<D2DEffect>(id, std::move(sources), std::move(properties));
}

/// `source` multiplied by the alpha of `mask`.
wge::IGraphicsEffect AlphaMask(wge::IGraphicsEffectSource source, wge::IGraphicsEffectSource mask)
{
    return Effect(kAlphaMaskEffectId, {std::move(source), std::move(mask)});
}

wge::IGraphicsEffect Blur(wge::IGraphicsEffectSource source, float sigma)
{
    return Effect(kGaussianBlurEffectId, {std::move(source)},
                  {PropertyValue::CreateSingle(sigma), PropertyValue::CreateUInt32(kBlurOptimizationBalanced),
                   PropertyValue::CreateUInt32(kBorderModeSoft)});
}

/// A clamped colour matrix. D2D1_MATRIX_5X4_F, row-major: rows are the input
/// R, G, B, A and a constant; columns the output R, G, B, A.
wge::IGraphicsEffect ColorMatrix(wge::IGraphicsEffectSource source, std::array<float, 20> const& m,
                                 uint32_t alphaMode)
{
    return Effect(kColorMatrixEffectId, {std::move(source)},
                  {PropertyValue::CreateSingleArray(m), PropertyValue::CreateUInt32(alphaMode),
                   PropertyValue::CreateBoolean(true)});
}

/// Opaque white wherever `source`'s alpha crosses the goo's threshold.
wge::IGraphicsEffect Threshold(wge::IGraphicsEffectSource source, float offset)
{
    std::array<float, 20> m{};
    m[15] = kGooGain; // A <- A
    m[16] = 1.0f;     // R <- 1
    m[17] = 1.0f;     // G <- 1
    m[18] = 1.0f;     // B <- 1
    m[19] = offset;   // A <- + offset
    return ColorMatrix(std::move(source), m, kColorMatrixPremultiplied);
}

/// The pill in one blur: the shapes in "Shapes", cut to goo, filled with
/// `tint` and rimmed with `rim`. One matrix makes both cuts at once, the rim's
/// into red and the fill's into green, and a second mixes those into
/// the two colours. (Layered — blur, threshold and composite per cut — a
/// compositor on an Intel Arc refused the graph as "too complex" and the rim
/// never showed.) The colours are numbers in the matrix rather than brushes,
/// so the brush is rebuilt when either changes.
template <typename Param>
wge::IGraphicsEffect FusedGooGraph(float sigma, winrt::Windows::UI::Color tint, winrt::Windows::UI::Color rim)
{
    std::array<float, 20> cut{};
    cut[12] = kGooGain;       // R <- A
    cut[13] = kGooGain;       // G <- A
    cut[16] = kGooRimOffset;  // R <- + rim offset
    cut[17] = kGooFillOffset; // G <- + fill offset
    cut[19] = 1.0f;           // A <- 1, so premultiplied and straight agree for the mix
    // The rim cut R contains the fill cut G, and where the fill has begun the
    // rim is already whole, so rim over the band and tint inside is
    // R * rim + G * (tint - rim), in premultiplied colour. (Over the fill's
    // own antialiased pixel the old graph let the rim show a touch less
    // through the translucent tint; nothing a person can see.)
    const auto premul = [](winrt::Windows::UI::Color c) {
        const float a = c.A / 255.0f;
        return std::array<float, 4>{c.R / 255.0f * a, c.G / 255.0f * a, c.B / 255.0f * a, a};
    };
    const auto t = premul(tint);
    const auto r = premul(rim);
    std::array<float, 20> mix{};
    for (size_t i = 0; i < 4; ++i) {
        mix[i] = r[i];            // from R
        mix[4 + i] = t[i] - r[i]; // from G
    }
    return ColorMatrix(ColorMatrix(Blur(Param{L"Shapes"}, sigma), cut, kColorMatrixPremultiplied), mix,
                       kColorMatrixStraight);
}

/// The goo without its rim, for a compositor that refuses the one-blur
/// graph's straight-alpha matrix: the shapes in "Shapes", cut to goo and
/// filled with the "Tint" brush.
template <typename Param>
wge::IGraphicsEffect RimlessGooGraph(float sigma)
{
    return AlphaMask(Param{L"Tint"}, Threshold(Blur(Param{L"Shapes"}, sigma), kGooFillOffset));
}

// M4: temporary. Under a debugger only, asks the compositor which of the goo's
// effects and property values it accepts, one at a time, and says so on the
// debug output. Remove once the goo builds on every compositor it meets.
void ProbeGooEffects(mucomp::Compositor const& c)
{
    if (!::IsDebuggerPresent()) return;
    using P = mucomp::CompositionEffectSourceParameter;
    std::array<float, 20> m{};
    m[15] = kGooGain;
    m[19] = kGooFillOffset;
    const auto probe = [&c](wchar_t const* name, wge::IGraphicsEffect const& effect) {
        std::wstring line = L"Yip goo probe: ";
        line += name;
        try {
            (void)c.CreateEffectFactory(effect);
            line += L" ok\n";
        } catch (winrt::hresult_error const& e) {
            wchar_t hr[24];
            swprintf_s(hr, L" FAIL 0x%08X\n", static_cast<uint32_t>(e.code()));
            line += hr;
        }
        ::OutputDebugStringW(line.c_str());
    };
    probe(L"alphamask", AlphaMask(P{L"A"}, P{L"B"}));
    probe(L"blur", Blur(P{L"S"}, 4.0f));
    probe(L"blur-hard", Effect(kGaussianBlurEffectId, {P{L"S"}},
                               {PropertyValue::CreateSingle(4.0f), PropertyValue::CreateUInt32(1),
                                PropertyValue::CreateUInt32(1)}));
    probe(L"blur-speed", Effect(kGaussianBlurEffectId, {P{L"S"}},
                                {PropertyValue::CreateSingle(4.0f), PropertyValue::CreateUInt32(0),
                                 PropertyValue::CreateUInt32(0)}));
    probe(L"matrix", Threshold(P{L"S"}, kGooFillOffset));
    probe(L"matrix-noclamp", Effect(kColorMatrixEffectId, {P{L"S"}},
                                    {PropertyValue::CreateSingleArray(m), PropertyValue::CreateUInt32(1),
                                     PropertyValue::CreateBoolean(false)}));
    probe(L"matrix-straight", Effect(kColorMatrixEffectId, {P{L"S"}},
                                     {PropertyValue::CreateSingleArray(m), PropertyValue::CreateUInt32(2),
                                      PropertyValue::CreateBoolean(true)}));
    probe(L"matrix-alphamode0", Effect(kColorMatrixEffectId, {P{L"S"}},
                                       {PropertyValue::CreateSingleArray(m), PropertyValue::CreateUInt32(0),
                                        PropertyValue::CreateBoolean(true)}));
    probe(L"matrix-only", Effect(kColorMatrixEffectId, {P{L"S"}}, {PropertyValue::CreateSingleArray(m)}));
    probe(L"cut", Threshold(Blur(P{L"S"}, 4.0f), kGooFillOffset));
    probe(L"rimless", RimlessGooGraph<P>(4.0f));
    probe(L"goo", FusedGooGraph<P>(4.0f, kMissingToken, kMissingToken));
}
} // namespace

namespace winrt::yip::implementation {
/// One motion of the pill: a track per shape (empty leaves that shape alone),
/// plus the readout's and the buttons' opacity.
struct PillChoreo {
    int ms{0};
    std::array<std::vector<BlobKey>, kPillBlobCount> blobs;
    std::vector<ScalarKey> readout;
    std::vector<ScalarKey> actions;

    std::vector<BlobKey>& operator[](PillBlob b) { return blobs[static_cast<size_t>(b)]; }
};

namespace {
/// The entrance: a drop swells out of the screen edge, hangs from it on a
/// neck that thins and snaps, lands with a squash, and bounces to rest. Without
/// the goo the edge and neck would be lumps of their own, so they stay out.
PillChoreo DripIn(bool dot, bool goo)
{
    const auto T = LayoutFor(dot, false).cap;
    const float c = T.cx;
    PillChoreo ch;
    ch.ms = kDripInMs;
    if (goo) {
        ch[PillBlob::Edge] = {At(0.0f, {c, -8.0f, 0.0f, 16.0f}, Ease::Linear),
                              At(0.16f, {c, -6.0f, 56.0f, 22.0f}, Ease::Standard),
                              At(0.40f, {c, -6.0f, 40.0f, 20.0f}, Ease::Standard),
                              At(0.58f, {c, -8.0f, 16.0f, 12.0f}, Ease::Standard),
                              At(0.74f, kIntoEdge, Ease::Standard)};
        ch[PillBlob::Neck] = {At(0.0f, {c, 0.0f, 0.0f, 0.0f}, Ease::Linear),
                              At(0.16f, {c, 4.0f, 12.0f, 14.0f}, Ease::Standard),
                              At(0.40f, {c, T.d * 0.4f, 10.0f, T.d * 0.8f}, Ease::Standard),
                              At(0.58f, {c, 3.0f, 6.0f, 10.0f}, Ease::Out),
                              At(0.74f, kIntoEdge, Ease::Standard)};
    } else {
        ch[PillBlob::Edge] = {At(0.0f, kIntoEdge, Ease::Linear)};
        ch[PillBlob::Neck] = {At(0.0f, kIntoEdge, Ease::Linear)};
    }
    ch[PillBlob::Cap] = {At(0.0f, {c, 0.0f, 10.0f, 10.0f}, Ease::Linear),
                         At(0.16f, {c, 9.0f, 22.0f, 22.0f}, Ease::Standard),
                         At(0.40f, {c, T.d * 0.8f, std::min(T.w, 28.0f), T.h * 0.95f}, Ease::InOut),
                         At(0.56f, {c, T.d + 7.0f, T.w * 1.08f, T.h * 0.8f}, Ease::Out),
                         At(0.72f, {c, T.d - 3.0f, T.w * 0.96f, T.h * 1.08f}, Ease::Standard),
                         At(0.86f, {c, T.d + 1.0f, T.w * 1.015f, T.h * 0.97f}, Ease::Standard),
                         At(1.0f, T, Ease::Standard)};
    ch[PillBlob::Pause] = {At(0.0f, Gone(T), Ease::Linear)};
    ch[PillBlob::Stop] = {At(0.0f, Gone(T), Ease::Linear)};
    ch.readout = {Fade(0.0f, 0.0f, Ease::Linear), Fade(0.56f, 0.0f, Ease::Linear), Fade(0.86f, 1.0f, Ease::Out)};
    ch.actions = {Fade(0.0f, 0.0f, Ease::Linear)};
    return ch;
}

/// The exit: the entrance backwards, starting from wherever the pill is.
PillChoreo DripOut(bool dot, bool goo)
{
    const auto in = DripIn(dot, goo);
    const auto rest = LayoutFor(dot, false).cap;
    PillChoreo ch;
    ch.ms = kDripOutMs;
    for (auto blob : {PillBlob::Edge, PillBlob::Neck, PillBlob::Cap}) {
        auto const& keys = in.blobs[static_cast<size_t>(blob)];
        auto& out = ch[blob];
        for (auto it = keys.rbegin(); it != keys.rend(); ++it)
            out.push_back(At(1.0f - it->t, it->b, Ease::Standard));
        if (!out.empty() && out.front().t > 0.0f) out.insert(out.begin(), At(0.0f, out.front().b, Ease::Linear));
    }
    // The drop leaves from where it is, not from where it would rest.
    ch[PillBlob::Cap].front() = Hold(0.0f);
    ch[PillBlob::Pause] = {Hold(0.0f), At(0.3f, Gone(rest), Ease::Standard)};
    ch[PillBlob::Stop] = {Hold(0.0f), At(0.3f, Gone(rest), Ease::Standard)};
    ch.readout = {HoldValue(0.0f), Fade(0.2f, 0.0f, Ease::Standard)};
    ch.actions = {HoldValue(0.0f), Fade(0.15f, 0.0f, Ease::Standard)};
    return ch;
}

/// Expand: Pause and Stop bud out of the capsule's far end (or the dot's
/// middle), stretch, pinch off and bounce into place; Stop a beat behind.
PillChoreo Split(bool dot)
{
    const auto from = LayoutFor(dot, false);
    const auto to = LayoutFor(dot, true);
    PillChoreo ch;
    ch.ms = kSplitMs;
    ch[PillBlob::Edge] = Retract();
    ch[PillBlob::Neck] = Retract();
    ch[PillBlob::Cap] = BounceTo(to.cap, Heading(from.cap, to.cap), 0.0f);
    ch[PillBlob::Pause] = BounceTo(to.pause, Heading(from.pause, to.pause), 0.0f);
    ch[PillBlob::Stop] = BounceTo(to.stop, Heading(from.stop, to.stop), kSplitStagger);
    ch.readout = {HoldValue(0.0f), Fade(0.3f, 1.0f, Ease::Out)};
    ch.actions = {HoldValue(0.0f), HoldValue(0.45f), Fade(0.85f, 1.0f, Ease::Out)};
    return ch;
}

/// Collapse: Stop melts into Pause, Pause into the capsule, and the capsule
/// wobbles as it takes them in.
PillChoreo Merge(bool dot)
{
    const auto from = LayoutFor(dot, true);
    const auto to = LayoutFor(dot, false);
    PillChoreo ch;
    ch.ms = kSplitMs;
    ch[PillBlob::Edge] = Retract();
    ch[PillBlob::Neck] = Retract();
    ch[PillBlob::Stop] = {Hold(0.0f), At(0.7f, to.stop, Ease::Morph), At(1.0f, to.stop, Ease::Linear)};
    ch[PillBlob::Pause] = {Hold(0.0f), Hold(kSplitStagger), At(0.82f, to.pause, Ease::Morph),
                           At(1.0f, to.pause, Ease::Linear)};
    ch[PillBlob::Cap] = BounceTo(to.cap, Heading(from.cap, to.cap), kSplitStagger);
    ch.readout = {HoldValue(0.0f), Fade(0.3f, 1.0f, Ease::Out)};
    ch.actions = {HoldValue(0.0f), Fade(0.2f, 0.0f, Ease::Out)};
    return ch;
}

/// Back to rest from wherever the shapes are: an exit called off half way.
PillChoreo Settle(bool dot, bool expanded)
{
    const auto to = LayoutFor(dot, expanded);
    PillChoreo ch;
    ch.ms = kSplitMs;
    ch[PillBlob::Edge] = Retract();
    ch[PillBlob::Neck] = Retract();
    ch[PillBlob::Cap] = BounceTo(to.cap, 0.0f, 0.0f);
    ch[PillBlob::Pause] = {Hold(0.0f), At(0.8f, to.pause, Ease::Morph), At(1.0f, to.pause, Ease::Linear)};
    ch[PillBlob::Stop] = {Hold(0.0f), At(0.8f, to.stop, Ease::Morph), At(1.0f, to.stop, Ease::Linear)};
    ch.readout = {HoldValue(0.0f), Fade(0.4f, 1.0f, Ease::Out)};
    ch.actions = {HoldValue(0.0f), Fade(0.4f, expanded ? 1.0f : 0.0f, Ease::Out)};
    return ch;
}
} // namespace

IndicatorWindow::IndicatorWindow()
{
    InitializeComponent();

    m_persisted = ::yip::IndicatorPersistence::Load();
    {
        const auto settings = ::yip::Settings::Load();
        m_dotStyle = settings.pill_dot;
        m_bottom = settings.pill_bottom;
    }
    // Grab the HWND. Required for tool-window style + click-through flip.
    if (auto native = try_as<::IWindowNative>()) {
        native->get_WindowHandle(&m_hwnd);
    }

    ApplyToolWindowStyle();
    ApplyAlwaysOnTop();
    // After the presenter change: SetBorderAndTitleBar re-applies the frame,
    // and the default corner preference with it.
    ApplyFrameless();

    // Before the backdrop: the mask behind the pill is cut the same way as
    // the pill, and whether that is goo depends on the pill's effect building.
    BuildCompositionLayer();

    // Not acrylic. DWM draws a system backdrop across the whole window
    // rectangle, rounded only by its own 8px corner and ignoring the window
    // region, so behind a capsule acrylic showed as light corners and a light
    // rim. The blur is cut to the shapes by an alpha mask instead, and outside
    // them the window stays fully transparent.
    ApplyBackdrop();
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

    // Posted rather than run inside Save(): the view model is still in the
    // middle of applying the dialog when it saves.
    ::yip::Settings::SetSavedHandler([weak = get_weak(), dq](::yip::Settings const& settings) {
        dq.TryEnqueue([weak, dot = settings.pill_dot, bottom = settings.pill_bottom] {
            if (auto self = weak.get()) self->ApplyPillSettings(dot, bottom);
        });
    });

    // App closes the pill between takes to give its memory back. Everything
    // global it hooked is released on Closed, not in the destructor, which can
    // run after the next take's pill already exists and would unhook that one.
    Closed([weak = get_weak()](auto&&, auto&&) {
        if (auto self = weak.get()) self->Teardown();
    });

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
    Teardown();
}

void IndicatorWindow::Teardown()
{
    if (m_tornDown) return;
    m_tornDown = true;

    ::yip::RecordingStateBus::Unsubscribe(m_stateToken);
    m_stateToken = 0;
    ::yip::Settings::SetSavedHandler(nullptr);
    RevokeFirstFrame();
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
    //
    // Only when a bit is actually back: this runs on every show, and a
    // SWP_FRAMECHANGED there made DWM rebuild the frame just as the fade-in
    // started.
    constexpr auto kFrameBits = static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU);
    const LONG_PTR style = ::GetWindowLongPtrW(m_hwnd, GWL_STYLE);
    if (style & kFrameBits) {
        ::SetWindowLongPtrW(m_hwnd, GWL_STYLE, style & ~kFrameBits);
        ::SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

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

void IndicatorWindow::BuildMaskShapes()
{
    if (m_maskVisual || !m_backdropCompositor) return;
    auto const& c = m_backdropCompositor;

    // The pill's shapes again, on this compositor, drawn into a visual surface.
    // The DPI container rasterises them at physical pixels, so the mask's edge
    // lines up with the pill's. Not a colour on screen: to the mask, opaque
    // white just means alpha 1.
    auto white = c.CreateColorBrush(winrt::Microsoft::UI::Colors::White());
    auto visual = c.CreateShapeVisual();
    visual.Size({kWindowW, kWindowH});
    for (size_t i = 0; i < kPillBlobCount; ++i) {
        auto geometry = c.CreateRoundedRectangleGeometry();
        // Rebuilt mid-take when effects come back on: start where the pill is.
        if (auto const& twin = m_blobs[i]) {
            geometry.Size(twin.Size());
            geometry.Offset(twin.Offset());
            geometry.CornerRadius(twin.CornerRadius());
        }
        auto shape = c.CreateSpriteShape(geometry);
        shape.FillBrush(white);
        visual.Shapes().Append(shape);
        m_maskBlobs[i] = geometry;
    }
    visual.Opacity(PillVisual().Opacity());

    auto dpi = c.CreateContainerVisual();
    dpi.Children().InsertAtTop(visual);
    auto root = c.CreateContainerVisual();
    root.Children().InsertAtTop(dpi);
    auto surface = c.CreateVisualSurface();
    surface.SourceVisual(root);

    m_maskVisual = visual;
    m_maskDpi = dpi;
    m_maskRoot = root;
    m_maskSurface = surface;
    m_backdropEases = MakeEases<wuc::CompositionEasingFunction>(c);
}

void IndicatorWindow::BuildBackdropBrush()
{
    BuildMaskShapes();
    if (!m_maskSurface) return;
    auto const& c = m_backdropCompositor;
    const auto scale = static_cast<float>(DpiScale());
    auto mask = c.CreateSurfaceBrush(m_maskSurface);
    mask.Stretch(wuc::CompositionStretch::Fill);

    // The host backdrop arrives already blurred by the shell; the effect only
    // cuts it to the shapes — as goo when the pill is goo, so the blur follows
    // the pill's outline through every merge. This brush is painted in
    // physical pixels, so the radius is too. A compositor that refuses the goo
    // still gets the plain cut.
    const wuc::CompositionEffectSourceParameter backdrop{L"Backdrop"};
    const wuc::CompositionEffectSourceParameter maskParam{L"Mask"};
    m_blurBrush = nullptr;
    for (const bool goo : {m_gooActive, false}) {
        try {
            auto effect = goo ? AlphaMask(backdrop, Threshold(Blur(maskParam, kGooBlurDip * scale), kGooFillOffset))
                              : AlphaMask(backdrop, maskParam);
            auto brush = c.CreateEffectFactory(effect).CreateBrush();
            brush.SetSourceParameter(L"Backdrop", c.CreateHostBackdropBrush());
            brush.SetSourceParameter(L"Mask", mask);
            m_blurBrush = brush;
            m_maskScale = scale;
            break;
        } catch (winrt::hresult_error const&) {
            // No effect support on this compositor: stay on the transparent
            // backdrop unless the plain cut works.
        }
    }
    SyncShapeSurfaces();
}

void IndicatorWindow::ApplySurfaceTint()
{
    if (!m_tintBrush) return;
    m_tintBrush.Color(::yip::theme::Color(
        m_blurActive ? L"YipIndicatorSurfaceBlurredBrush" : L"YipIndicatorSurfaceBrush", kMissingToken));
    // The one-blur goo carries its colours in the effect, not in brushes.
    SyncGooColors();
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
    m_compositor = PillVisual().Compositor();
    m_eases = MakeEases<mucomp::CompositionEasingFunction>(m_compositor);

    // The content rides its shapes via composition Translation (Follow), so no
    // layout pass runs per frame.
    muxh::ElementCompositionPreview::SetIsTranslationEnabled(PillFrame(), true);
    muxh::ElementCompositionPreview::SetIsTranslationEnabled(PauseButton(), true);
    muxh::ElementCompositionPreview::SetIsTranslationEnabled(StopButton(), true);

    // Brushes first: the bars, the dot and the goo below are handed one as
    // they are created.
    ResolveThemeBrushes();
    m_tintBrush = m_compositor.CreateColorBrush(kMissingToken);
    ApplySurfaceTint();

    // The pill's shapes, all at nothing until the first show poses them.
    m_shapeVisual = m_compositor.CreateShapeVisual();
    m_shapeVisual.Size({kWindowW, kWindowH});
    for (size_t i = 0; i < kPillBlobCount; ++i) {
        m_blobs[i] = m_compositor.CreateRoundedRectangleGeometry();
        m_blobShapes[i] = m_compositor.CreateSpriteShape(m_blobs[i]);
        m_shapeVisual.Shapes().Append(m_blobShapes[i]);
    }
    BuildGooBrush();

    // Child visual tree for the meter bars, parented to the meter host (which
    // XAML layout places inside the readout group).
    auto meterContainer = m_compositor.CreateContainerVisual();
    meterContainer.Size({kBarCount * (kBarWidth + kBarGap) - kBarGap, kBarMaxHeight + 4});
    muxh::ElementCompositionPreview::SetElementChildVisual(MeterHost(), meterContainer);

    // 4 vertical bars, anchored center-Y, resting at ~6% height (a thin
    // glyph). Live updates animate Size.Y, not Scale.Y: a scale would squash
    // the round ends along with the bar. The clip follows the size on the
    // compositor, its radius half the narrower side, so every height from the
    // resting dash to full travel keeps fully round ends.
    for (int i = 0; i < kBarCount; ++i) {
        auto bar = m_compositor.CreateSpriteVisual();
        bar.Size({kBarWidth, BarHeight(kBarRestScale)});
        bar.AnchorPoint({0.5f, 0.5f});
        bar.Offset({
            static_cast<float>(i) * (kBarWidth + kBarGap) + kBarWidth * 0.5f,
            (kBarMaxHeight + 4) * 0.5f,
            0.0f,
        });
        auto round = m_compositor.CreateRoundedRectangleGeometry();
        auto follow = m_compositor.CreateExpressionAnimation(L"bar.Size");
        follow.SetReferenceParameter(L"bar", bar);
        round.StartAnimation(L"Size", follow);
        auto radius = m_compositor.CreateExpressionAnimation(
            L"Vector2(Min(bar.Size.X, bar.Size.Y), Min(bar.Size.X, bar.Size.Y)) * 0.5");
        radius.SetReferenceParameter(L"bar", bar);
        round.StartAnimation(L"CornerRadius", radius);
        bar.Clip(m_compositor.CreateGeometricClip(round));
        bar.Brush(m_barIdleBrush);
        meterContainer.Children().InsertAtTop(bar);
        m_barVisuals[static_cast<size_t>(i)] = bar;
        m_barTargets[static_cast<size_t>(i)] = BarHeight(kBarRestScale);
    }
    m_barBrush = m_barIdleBrush;

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

void IndicatorWindow::BuildGooBrush()
{
    ProbeGooEffects(m_compositor);

    // To the effect a shape is only alpha, so they are drawn opaque white into
    // a surface at physical pixels: the DPI container scales the DIP shapes
    // up, and the brush's Fill stretch maps the surface back onto the window.
    auto white = m_compositor.CreateColorBrush(winrt::Microsoft::UI::Colors::White());
    for (auto const& shape : m_blobShapes)
        shape.FillBrush(white);
    auto dpi = m_compositor.CreateContainerVisual();
    dpi.Children().InsertAtTop(m_shapeVisual);
    auto root = m_compositor.CreateContainerVisual();
    root.Children().InsertAtTop(dpi);
    auto surface = m_compositor.CreateVisualSurface();
    surface.SourceVisual(root);
    auto shapes = m_compositor.CreateSurfaceBrush(surface);
    shapes.Stretch(mucomp::CompositionStretch::Fill);

    // The one-blur goo, then the goo without its rim.
    m_shapeBrush = shapes;
    mucomp::CompositionEffectBrush brush{nullptr};
    for (const auto kind : {GooKind::Fused, GooKind::Rimless}) {
        try {
            brush = MakeGooBrush(kind);
            m_gooKind = kind;
            break;
        } catch (winrt::hresult_error const& e) {
            brush = nullptr;
            std::wstring line =
                kind == GooKind::Fused ? L"Yip: goo effect refused" : L"Yip: rimless goo effect refused";
            line += L": ";
            line += std::wstring_view{e.message()};
            line += L'\n';
            ::OutputDebugStringW(line.c_str());
        }
    }

    if (brush) {
        m_gooSprite = m_compositor.CreateSpriteVisual();
        m_gooSprite.Size({kWindowW, kWindowH});
        m_gooSprite.Brush(brush);
        m_shapeDpi = dpi;
        m_shapeRoot = root;
        m_shapeSurface = surface;
        muxh::ElementCompositionPreview::SetElementChildVisual(GooHost(), m_gooSprite);
        m_gooActive = true;
        return;
    }

    // No effect support: the shapes are drawn as they are, tint and stroke on
    // each. They overlap rather than melt, which is all that is lost.
    m_gooActive = false;
    dpi.Children().RemoveAll();
    for (auto const& shape : m_blobShapes) {
        shape.FillBrush(m_tintBrush);
        shape.StrokeBrush(m_strokeBrush);
        shape.StrokeThickness(1.0f);
    }
    muxh::ElementCompositionPreview::SetElementChildVisual(GooHost(), m_shapeVisual);
}

mucomp::CompositionEffectBrush IndicatorWindow::MakeGooBrush(GooKind kind)
{
    // The pill is drawn in DIPs, and so is the blur radius.
    using P = mucomp::CompositionEffectSourceParameter;
    const bool fused = kind == GooKind::Fused;
    auto effect = fused ? FusedGooGraph<P>(kGooBlurDip, m_tintBrush.Color(), m_strokeBrush.Color())
                        : RimlessGooGraph<P>(kGooBlurDip);
    auto brush = m_compositor.CreateEffectFactory(effect).CreateBrush();
    brush.SetSourceParameter(L"Shapes", m_shapeBrush);
    if (fused) {
        m_gooTint = m_tintBrush.Color();
        m_gooRim = m_strokeBrush.Color();
    } else {
        brush.SetSourceParameter(L"Tint", m_tintBrush);
    }
    return brush;
}

void IndicatorWindow::SyncGooColors()
{
    if (!m_gooSprite || m_gooKind != GooKind::Fused) return;
    if (m_tintBrush.Color() == m_gooTint && m_strokeBrush.Color() == m_gooRim) return;
    try {
        m_gooSprite.Brush(MakeGooBrush(GooKind::Fused));
    } catch (winrt::hresult_error const&) {
        // Built once already, so this compositor takes the graph; keep the
        // old colours rather than lose the pill.
    }
}

void IndicatorWindow::SyncShapeSurfaces()
{
    if (!m_hwnd) return;
    RECT client{};
    if (!::GetClientRect(m_hwnd, &client) || client.right <= 0 || client.bottom <= 0) return;

    // Each surface is the window's size in physical pixels and the shapes are
    // in window DIPs: the DPI container scales them up. The brushes' Fill
    // stretch then maps the surface onto the window exactly.
    const auto scale = static_cast<float>(DpiScale());
    const float2 px{static_cast<float>(client.right), static_cast<float>(client.bottom)};
    const auto fit = [&](auto const& surface, auto const& root, auto const& dpi) {
        if (!surface) return;
        surface.SourceSize(px);
        root.Size(px);
        dpi.Size({kWindowW, kWindowH});
        dpi.Scale({scale, scale, 1.0f});
    };
    fit(m_shapeSurface, m_shapeRoot, m_shapeDpi);
    fit(m_maskSurface, m_maskRoot, m_maskDpi);
    if (m_gooSprite) m_gooSprite.Size({px.x / scale, px.y / scale});

    // The mask's blur radius is in pixels: a new scale needs a new brush.
    if (m_blurBrush && m_gooActive && scale != m_maskScale) {
        m_blurBrush = nullptr;
        ApplyBackdrop();
    }
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

    // The dot style collapses the meter and clock away until a tap expands
    // the pill. Nothing reads them there, so the tick keeps only its pause
    // check; ApplyLayoutFor brings them current the moment they are shown.
    if (ReadoutDetail().Visibility() == mux::Visibility::Collapsed) return;

    ApplyReadout(snapshot, false);
}

void IndicatorWindow::ApplyReadout(RecMeter const& snapshot, bool snap)
{
    UpdateMeterBars(MeterNorm(snapshot.peak), snapshot.clip_count > 0, snap);

    auto text = winrt::hstring{FormatPillElapsed(snapshot.elapsed_ms)};
    if (text != m_elapsedText) {
        m_elapsedText = text;
        ElapsedText().Text(text);
    }
}

void IndicatorWindow::SyncReadoutNow()
{
    RecMeter snapshot{};
    if (::rec_meter(&snapshot) != REC_STATUS_OK) return;
    ApplyReadout(snapshot, true);
}

void IndicatorWindow::UpdateMeterBars(float level, bool hot, bool snap)
{
    const float clamped = std::clamp(level, 0.0f, 1.0f);

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

    // Brushes are mutated in place on a theme change, so identity is enough
    // to know the bars already wear this one.
    const bool newBrush = brush && brush != m_barBrush;
    if (newBrush) m_barBrush = brush;

    for (int i = 0; i < kBarCount; ++i) {
        auto& bar = m_barVisuals[static_cast<size_t>(i)];
        if (!bar) continue;
        if (newBrush) bar.Brush(brush);

        // Silence and steady tone would otherwise rewrite four heights every
        // tick that the bars already hold, keeping the pill and its blurred
        // backdrop redrawing for nothing. `snap` (revealed mid-take) always
        // writes, so the bar lands on the live level whatever it last held.
        const float target = BarHeight(std::max(kBarRestScale, clamped * kBarWeights[i]));
        auto& last = m_barTargets[static_cast<size_t>(i)];
        if (!snap && std::abs(target - last) < kBarTargetEpsilon) continue;
        last = target;
        // A direct write is one frame. The stop only matters after
        // StopMeterAnimations, whose settle may still be running.
        bar.StopAnimation(L"Size.Y");
        bar.Size({kBarWidth, target});
    }
}

void IndicatorWindow::UpdateDotForState(::yip::IndicatorState s)
{
    if (!m_dotVisual) return;
    // Expanding the pill mid-take is still a live take: the lamp stays red.
    // A paused one is not — nothing is reaching the file, so the lamp goes
    // neutral.
    const bool live = !m_paused && ((s == ::yip::IndicatorState::Recording) ||
                                    (s == ::yip::IndicatorState::Expanded && m_recording));
    m_dotVisual.Brush(live ? m_dotRecordBrush : m_dotNeutralBrush);

    // A steady lamp. It used to pulse on a Forever animation, and a running
    // animation makes the compositor draw every display refresh — 60 to 165
    // frames a second, each re-running the backdrop blur and mask — for the
    // whole take, silence included. The red lamp and the meter already say
    // "recording"; the pulse was paid for in GPU time.
    m_dotVisual.StopAnimation(L"Opacity");
    m_dotVisual.Opacity(1.0f);
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
    // The bars go quiet with the lamp. Left to the next meter tick they kept
    // showing a live level for two frames after the lamp had gone grey.
    if (m_paused) UpdateMeterBars(0.0f, false);
}

void IndicatorWindow::StopMeterAnimations()
{
    // Leave the last elapsed time on screen through the Saving frame; only the
    // bars fall back, so the pill does not blank out mid-fade.
    m_barTargets.fill(BarHeight(kBarRestScale));
    for (auto& bar : m_barVisuals) {
        if (!bar) continue;
        bar.StopAnimation(L"Size.Y");
        // Fall back to the resting height.
        auto anim = m_compositor.CreateScalarKeyFrameAnimation();
        anim.InsertKeyFrame(1.0f, BarHeight(kBarRestScale), m_eases.standard);
        anim.Duration(std::chrono::milliseconds(kFadeMs));
        bar.StartAnimation(L"Size.Y", anim);
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
    // The goo's rim. Its fill is the surface tint, set by ApplySurfaceTint.
    apply(m_strokeBrush, L"YipIndicatorStrokeQuietBrush");

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
        // Placed while still hidden: this is the only time the HWND moves, so
        // nothing on screen can catch it half done.
        if (!m_windowVisible) PlacePillAtHome();
        ShowPill(animate);
        return;
    }
    if (animate) {
        MorphPill(from, s);
        return;
    }
    ApplyLayoutFor(s);
    SetPillOpacity(OpacityFor(s));
}

void IndicatorWindow::ApplyLayoutFor(::yip::IndicatorState s)
{
    // Whatever was moving, this is where it lands.
    ++m_shapeGen;
    const bool expanded = (s == ::yip::IndicatorState::Expanded);
    const auto layout = LayoutFor(m_dotStyle, expanded);
    const std::array<Blob, kPillBlobCount> rest{kIntoEdge, kIntoEdge, layout.cap, layout.pause, layout.stop};
    for (size_t i = 0; i < kPillBlobCount; ++i) {
        SetBlob(m_blobs[i], rest[i], m_bottom);
        SetBlob(m_maskBlobs[i], rest[i], m_bottom);
    }
    PlaceContent(s);

    const auto land = [](mucomp::Visual const& visual, float opacity) {
        visual.StopAnimation(L"Opacity");
        visual.Opacity(opacity);
    };
    land(muxh::ElementCompositionPreview::GetElementVisual(ReadoutGroup()), 1.0f);
    for (auto const& button : {PauseButton(), StopButton()}) {
        // Collapsed, not transparent: a button nobody can see must not still be
        // a tab stop.
        button.Visibility(expanded ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        button.IsHitTestVisible(expanded);
        land(muxh::ElementCompositionPreview::GetElementVisual(button), expanded ? 1.0f : 0.0f);
    }
    // At rest the content sits exactly where layout put it: a follow's
    // sub-pixel remainder would leave the clock's text soft.
    SetTranslation(PillFrame(), 0.0f, 0.0f);
    SetTranslation(PauseButton(), 0.0f, 0.0f);
    SetTranslation(StopButton(), 0.0f, 0.0f);
    ApplyHitRegion(s);
}

void IndicatorWindow::PlaceContent(::yip::IndicatorState s)
{
    const auto layout = LayoutFor(m_dotStyle, s == ::yip::IndicatorState::Expanded);

    // The meter and clock ride the capsule; the dot style is the lamp alone.
    const bool detailWasHidden = ReadoutDetail().Visibility() == mux::Visibility::Collapsed;
    ReadoutDetail().Visibility(m_dotStyle ? mux::Visibility::Collapsed : mux::Visibility::Visible);
    if (!m_dotStyle && detailWasHidden && m_recording) SyncReadoutNow();

    PillFrame().Width(layout.cap.w);
    PillFrame().Height(layout.cap.h);
    const auto put = [this](mux::UIElement const& element, Blob const& b, float w, float h) {
        muxc::Canvas::SetLeft(element, b.cx - w * 0.5f);
        muxc::Canvas::SetTop(element, BlobY(b.d, m_bottom) - h * 0.5f);
    };
    put(PillFrame(), layout.cap, layout.cap.w, layout.cap.h);
    put(PauseButton(), layout.pause, kActionDot, kActionDot);
    put(StopButton(), layout.stop, kActionDot, kActionDot);
    // Arranged now, in the same frame as the follows below: otherwise an
    // element would sit at its old spot with its new translation for a frame.
    PillCanvas().UpdateLayout();

    const auto centre = [this](Blob const& b) { return float2{b.cx, BlobY(b.d, m_bottom)}; };
    Follow(m_compositor, PillFrame(), m_blobs[static_cast<size_t>(PillBlob::Cap)], centre(layout.cap));
    Follow(m_compositor, PauseButton(), m_blobs[static_cast<size_t>(PillBlob::Pause)], centre(layout.pause));
    Follow(m_compositor, StopButton(), m_blobs[static_cast<size_t>(PillBlob::Stop)], centre(layout.stop));
}

void IndicatorWindow::ApplyPillSettings(bool dot, bool bottom)
{
    if (dot == m_dotStyle && bottom == m_bottom) return;
    m_dotStyle = dot;
    m_bottom = bottom;

    // Orphan whatever motion is running: everything below lands in one step.
    ++m_motionGen;
    ++m_shapeGen;
    if (!m_shown && m_windowVisible) {
        // An exit whose completion was just orphaned would never hide the
        // window; finish it now instead.
        HideWindow();
    }

    if (m_windowVisible) {
        PlacePillAtHome();
        ApplyLayoutFor(m_state);
        SetPillOpacity(OpacityFor(m_state));
    }
    // Hidden, the next show places and lays out the pill anyway.
}

// =========================================================== Motion

int IndicatorWindow::MotionMs(int ms) const
{
    return m_uiSettings.AnimationsEnabled() ? ms : 0;
}

void IndicatorWindow::Play(PillChoreo const& choreo, std::function<void(IndicatorWindow&)> landed)
{
    const auto gen = ++m_shapeGen;
    const int ms = MotionMs(choreo.ms);
    mucomp::CompositionScopedBatch batch{nullptr};
    if (ms > 0) batch = m_compositor.CreateScopedBatch(mucomp::CompositionBatchTypes::Animation);

    for (size_t i = 0; i < kPillBlobCount; ++i) {
        AnimateBlob(m_compositor, m_blobs[i], choreo.blobs[i], ms, m_eases, m_bottom);
        AnimateBlob(m_backdropCompositor, m_maskBlobs[i], choreo.blobs[i], ms, m_backdropEases, m_bottom);
    }
    AnimateOpacity(m_compositor, muxh::ElementCompositionPreview::GetElementVisual(ReadoutGroup()), choreo.readout,
                   ms, m_eases);
    for (auto const& button : {PauseButton(), StopButton()}) {
        AnimateOpacity(m_compositor, muxh::ElementCompositionPreview::GetElementVisual(button), choreo.actions, ms,
                       m_eases);
    }

    if (!batch) {
        if (landed) landed(*this);
        return;
    }
    batch.End();
    batch.Completed([weak = get_weak(), gen, landed = std::move(landed)](auto&&, auto&&) {
        if (auto self = weak.get(); self && self->m_shapeGen == gen && landed) landed(*self);
    });
}

void IndicatorWindow::ShowPill(bool animate)
{
    m_shown = true;
    const bool expanded = (m_state == ::yip::IndicatorState::Expanded);

    if (!animate || MotionMs(kDripInMs) == 0) {
        RevokeFirstFrame();
        ApplyLayoutFor(m_state);
        SetPillOpacity(OpacityFor(m_state));
        if (!m_windowVisible) ShowWindow();
        return;
    }

    for (auto const& button : {PauseButton(), StopButton()}) {
        button.Visibility(expanded ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        button.IsHitTestVisible(expanded);
    }
    ApplyHitRegionWhole();
    PlaceContent(m_state);

    // A pill still dripping out is called back from wherever it has got to.
    if (m_windowVisible) {
        RevokeFirstFrame();
        AnimatePillOpacity(OpacityFor(m_state), kFadeMs);
        Play(Settle(m_dotStyle, expanded), [](IndicatorWindow& self) { self.ApplyLayoutFor(self.m_state); });
        return;
    }

    // The drop's first frame is posed while the window is still hidden; the
    // drip itself waits for XAML to draw.
    SetPillOpacity(OpacityFor(m_state));
    auto pose = DripIn(m_dotStyle, m_gooActive);
    pose.ms = 0;
    for (auto& keys : pose.blobs) {
        if (keys.size() > 1) keys.resize(1);
    }
    pose.readout.resize(std::min<size_t>(pose.readout.size(), 1));
    pose.actions.resize(std::min<size_t>(pose.actions.size(), 1));
    Play(pose, nullptr);
    ShowWindow();
    StartShowOnFirstFrame();
}

void IndicatorWindow::StartShowOnFirstFrame()
{
    RevokeFirstFrame();
    m_firstFrameToken = mux::Media::CompositionTarget::Rendering([weak = get_weak()](auto&&, auto&&) {
        auto self = weak.get();
        if (!self) return;
        self->RevokeFirstFrame();
        // A hide since the show owns the pill now. Any other transition in
        // between left the drop posed at its first frame, so still bring it
        // in; it lands on whatever state the pill is in by then.
        if (!self->m_shown) return;
        self->Play(DripIn(self->m_dotStyle, self->m_gooActive),
                   [](IndicatorWindow& pill) { pill.ApplyLayoutFor(pill.m_state); });
    });
}

void IndicatorWindow::RevokeFirstFrame()
{
    if (!m_firstFrameToken) return;
    mux::Media::CompositionTarget::Rendering(m_firstFrameToken);
    m_firstFrameToken = {};
}

void IndicatorWindow::HidePill(bool animate)
{
    m_shown = false;
    RevokeFirstFrame();
    if (!m_windowVisible) return;
    if (!animate || MotionMs(kDripOutMs) == 0) {
        HideWindow();
        return;
    }

    PauseButton().IsHitTestVisible(false);
    StopButton().IsHitTestVisible(false);
    ApplyHitRegionWhole();
    const auto gen = m_motionGen;
    Play(DripOut(m_dotStyle, m_gooActive), [gen](IndicatorWindow& self) {
        if (self.m_motionGen == gen && !self.m_shown) self.HideWindow();
    });
}

void IndicatorWindow::MorphPill(::yip::IndicatorState from, ::yip::IndicatorState to)
{
    AnimatePillOpacity(OpacityFor(to), kFadeMs);

    // Recording <-> Saving: the same shapes. A merge still carrying the discs
    // home keeps going and lands on the new state.
    const bool expand = (to == ::yip::IndicatorState::Expanded);
    if ((from == ::yip::IndicatorState::Expanded) == expand) return;

    ApplyHitRegionWhole();
    for (auto const& button : {PauseButton(), StopButton()}) {
        // Showing now, so they can fade in as their discs arrive. Leaving, hit
        // testing goes with the fade, not with the landing — a button nobody
        // can see must not still be a button.
        if (expand) button.Visibility(mux::Visibility::Visible);
        button.IsHitTestVisible(expand);
    }
    PlaceContent(to);
    Play(expand ? Split(m_dotStyle) : Merge(m_dotStyle),
         [](IndicatorWindow& self) { self.ApplyLayoutFor(self.m_state); });
}

mucomp::Visual IndicatorWindow::PillVisual()
{
    return muxh::ElementCompositionPreview::GetElementVisual(Root());
}

void IndicatorWindow::SetPillOpacity(float opacity)
{
    const auto land = [opacity](auto const& visual) {
        visual.StopAnimation(L"Opacity");
        visual.Opacity(opacity);
    };
    land(PillVisual());
    if (m_maskVisual) land(m_maskVisual);
}

void IndicatorWindow::AnimatePillOpacity(float to, int ms)
{
    if (MotionMs(ms) == 0) {
        SetPillOpacity(to);
        return;
    }
    AnimateScalar(m_compositor, PillVisual(), L"Opacity", to, ms, m_eases.out);
    if (m_maskVisual) AnimateScalar(m_backdropCompositor, m_maskVisual, L"Opacity", to, ms, m_backdropEases.out);
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
    } else if (m_recording) {
        // Not while Saving: a finished take has nothing left to pause or stop,
        // and a pill opened over Saving missed the one-shot timer that hides
        // it, then collapsed back into Saving and stayed on screen for good.
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

void IndicatorWindow::ApplyHitRegion(::yip::IndicatorState s)
{
    if (!m_hwnd) return;

    // The window is sized for the expanded pill and its bounce, so around the
    // resting shapes it is mostly transparent pixels — which still take the
    // mouse, because the window is not layered. A rectangle per shape rather
    // than the shape itself: a region is aliased, and a rounded one once
    // chewed the capsule's antialiased edge into steps. The slack keeps that
    // edge, and the ~1 DIP rim outside it, well inside the region, which clips
    // drawing as well as the mouse.
    const bool expanded = (s == ::yip::IndicatorState::Expanded);
    const auto layout = LayoutFor(m_dotStyle, expanded);
    const double scale = DpiScale();
    const int slack = static_cast<int>(std::ceil(2.0 * scale));

    HRGN region = ::CreateRectRgn(0, 0, 0, 0);
    if (!region) return;
    const auto add = [&](Blob const& b) {
        if (!(b.w > 0.0f && b.h > 0.0f)) return;
        const double x = b.cx - b.w * 0.5;
        const double y = BlobY(b.d, m_bottom) - b.h * 0.5;
        HRGN part = ::CreateRectRgn(static_cast<int>(std::floor(x * scale)) - slack,
                                    static_cast<int>(std::floor(y * scale)) - slack,
                                    static_cast<int>(std::ceil((x + b.w) * scale)) + slack,
                                    static_cast<int>(std::ceil((y + b.h) * scale)) + slack);
        if (!part) return;
        (void)::CombineRgn(region, region, part, RGN_OR);
        ::DeleteObject(part);
    };
    add(layout.cap);
    if (expanded) {
        add(layout.pause);
        add(layout.stop);
    }
    // On success the window owns the region; only a refusal leaves it ours.
    if (!::SetWindowRgn(m_hwnd, region, TRUE)) ::DeleteObject(region);
}

void IndicatorWindow::ApplyHitRegionWhole()
{
    // While shapes move nothing is at rest to cut around, and a region would
    // clip whatever overshoots it.
    if (m_hwnd) (void)::SetWindowRgn(m_hwnd, nullptr, TRUE);
}

void IndicatorWindow::PlacePillAtHome()
{
    if (!m_hwnd) return;

    auto appWindow = muw::AppWindow::GetFromWindowId(AppWindow().Id());
    auto primary = muw::DisplayArea::Primary();
    if (!appWindow || !primary) return;

    // WorkArea is physical pixels, so the DIP sizes are scaled first. Rounded
    // up, so every shape always fits inside the window. Flush with the work
    // area's edge: the pill's own margin is inside the window, which is what
    // gives the entrance an edge to drip from. Twice at most: the first move
    // can carry the window onto a display with another DPI, and the size has
    // to be worked out in that display's pixels.
    const auto work = primary.WorkArea();
    for (int pass = 0; pass < 2; ++pass) {
        const double scale = DpiScale();
        const int w = static_cast<int>(std::ceil(kWindowW * scale));
        const int h = static_cast<int>(std::ceil(kWindowH * scale));
        const int x = work.X + (work.Width - w) / 2;
        const int y = m_bottom ? work.Y + work.Height - h : work.Y;
        appWindow.MoveAndResize({x, y, w, h});
        if (DpiScale() == scale) break;
    }
    SyncShapeSurfaces();
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
