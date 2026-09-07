#include "pch.h"
#include "IndicatorWindow.xaml.h"

#if __has_include("IndicatorWindow.g.cpp")
#include "IndicatorWindow.g.cpp"
#endif

#include "Markers.h"
#include "Settings.h"

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
// Token defaults — duplicated as fallback for code that runs before
// Application resources are queryable. App.xaml is the source of truth.
constexpr int kWindowW = 320;
constexpr int kWindowH = 56;
constexpr float kCorner = 22.0f;
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
            return {188.0f, 44.0f, 1.00f};
        case S::Saving:
            return {188.0f, 44.0f, 0.85f};
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

winrt::Windows::UI::Color FromHex(uint32_t argb) noexcept
{
    return {
        static_cast<uint8_t>((argb >> 24) & 0xff),
        static_cast<uint8_t>((argb >> 16) & 0xff),
        static_cast<uint8_t>((argb >> 8) & 0xff),
        static_cast<uint8_t>(argb & 0xff),
    };
}
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

    // Round HWND silhouette to the maximal pill outline. The Composition
    // clip below animates the *visible* shape within this silhouette
    // without further region updates.
    if (m_hwnd) {
        HRGN rgn = ::CreateRoundRectRgn(0, 0, kWindowW + 1, kWindowH + 1, static_cast<int>(kCorner * 2),
                                        static_cast<int>(kCorner * 2));
        ::SetWindowRgn(m_hwnd, rgn, TRUE); // Windows takes ownership of rgn.
    }

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
        if (auto self = weak.get()) self->UpdateMeterBars(::rec_peak_level());
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

    LONG_PTR ex = ::GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    ex |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED;
    ex &= ~WS_EX_APPWINDOW;
    ::SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex);

    // WS_EX_LAYERED with no LWA_COLORKEY/LWA_ALPHA = fully opaque inside
    // window region, fully clipped outside it.
    ::SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
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
    appWindow.Resize({kWindowW, kWindowH});
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

    // Composition geometric clip on the Border's visual. Animating this
    // geometry's Size + Offset is what conveys state changes — no XAML
    // layout passes, no UI-thread wakes during the tween.
    m_clipGeo = m_compositor.CreateRoundedRectangleGeometry();
    m_clipGeo.CornerRadius({kCorner, kCorner});
    m_clipGeo.Size({kWindowW, kWindowH});
    m_clipGeo.Offset({0.0f, 0.0f});
    auto clip = m_compositor.CreateGeometricClip(m_clipGeo);
    pillVisual.Clip(clip);

    // Child visual tree for dot + meter bars. Parented to the meter host
    // (anchored within the pill grid by XAML layout so it stays inside
    // the visible clip).
    auto meterContainer = m_compositor.CreateContainerVisual();
    meterContainer.Size({kBarCount * (kBarWidth + kBarGap) - kBarGap, kBarMaxHeight + 4});
    muxh::ElementCompositionPreview::SetElementChildVisual(MeterHost(), meterContainer);

    // 4 vertical bars, anchored center-Y, with idle scale ~ 0.06 (a thin
    // resting glyph). Live updates drive Scale.Y via composition anims.
    m_barIdleBrush = m_compositor.CreateColorBrush(FromHex(0xFF525866));
    m_barLiveBrush = m_compositor.CreateColorBrush(FromHex(0xFFE5484D));
    for (int i = 0; i < kBarCount; ++i) {
        auto bar = m_compositor.CreateSpriteVisual();
        bar.Size({kBarWidth, kBarMaxHeight});
        bar.AnchorPoint({0.5f, 0.5f});
        bar.Offset({
            static_cast<float>(i) * (kBarWidth + kBarGap) + kBarWidth * 0.5f,
            (kBarMaxHeight + 4) * 0.5f,
            0.0f,
        });
        bar.Scale({1.0f, 0.06f, 1.0f});
        bar.Brush(m_barIdleBrush);
        meterContainer.Children().InsertAtTop(bar);
        m_barVisuals[static_cast<size_t>(i)] = bar;
    }

    // Recording dot — drawn as a 10×10 sprite visual on the DotHost element.
    auto dotContainer = m_compositor.CreateContainerVisual();
    dotContainer.Size({12.0f, 12.0f});
    muxh::ElementCompositionPreview::SetElementChildVisual(DotHost(), dotContainer);

    m_dotNeutralBrush = m_compositor.CreateColorBrush(FromHex(0xFF525866));
    m_dotRecordBrush = m_compositor.CreateColorBrush(FromHex(0xFFE5484D));
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

void IndicatorWindow::UpdateMeterBars(float peak)
{
    const float clamped = std::clamp(peak, 0.0f, 1.0f);
    const auto dur = std::chrono::milliseconds(kMeterMs);

    for (int i = 0; i < kBarCount; ++i) {
        // Mild attenuation per bar position for VU-meter character.
        const float falloff = 1.0f - 0.12f * static_cast<float>(i);
        const float target = std::max(0.06f, clamped * falloff);

        auto anim = m_compositor.CreateScalarKeyFrameAnimation();
        anim.InsertKeyFrame(1.0f, target, m_ease);
        anim.Duration(dur);
        m_barVisuals[static_cast<size_t>(i)].StartAnimation(L"Scale.Y", anim);
    }
}

void IndicatorWindow::UpdateDotForState(::yip::IndicatorState s)
{
    if (!m_dotVisual) return;
    const bool live = (s == ::yip::IndicatorState::Recording);
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
    for (auto& bar : m_barVisuals) {
        if (!bar) continue;
        bar.StopAnimation(L"Scale.Y");
        // Snap back to idle resting scale.
        auto anim = m_compositor.CreateScalarKeyFrameAnimation();
        anim.InsertKeyFrame(1.0f, 0.06f, m_ease);
        anim.Duration(std::chrono::milliseconds(kFadeMs));
        bar.StartAnimation(L"Scale.Y", anim);
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
    // Visibility gate first — runs even when s == m_state on the very
    // first call from the constructor (m_state initialised to Idle).
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

    ExpandedActions().Opacity(wantActions ? 1.0 : 0.0);
    ExpandedActions().IsHitTestVisible(wantActions);

    MeterHost().Opacity(wantMeter ? 1.0 : 0.0);

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

void IndicatorWindow::AnimatePillToState(::yip::IndicatorState s, bool animate)
{
    if (!m_clipGeo) return;
    const auto g = GeometryFor(s);

    // `animate == false` means "land on the resting values now" — used for the
    // initial state, which must not visibly slide in on launch.
    const auto resizeMs = std::chrono::milliseconds(animate ? kResizeMs : 0);
    const auto fadeMs = std::chrono::milliseconds(animate ? kFadeMs : 0);

    // Center the clip rect inside the window.
    const float offX = (static_cast<float>(kWindowW) - g.w) * 0.5f;
    const float offY = (static_cast<float>(kWindowH) - g.h) * 0.5f;

    auto sizeAnim = m_compositor.CreateVector2KeyFrameAnimation();
    sizeAnim.InsertKeyFrame(1.0f, {g.w, g.h}, m_ease);
    sizeAnim.Duration(resizeMs);
    m_clipGeo.StartAnimation(L"Size", sizeAnim);

    auto offAnim = m_compositor.CreateVector2KeyFrameAnimation();
    offAnim.InsertKeyFrame(1.0f, {offX, offY}, m_ease);
    offAnim.Duration(resizeMs);
    m_clipGeo.StartAnimation(L"Offset", offAnim);

    // Pill opacity via the Border's Visual.
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

void IndicatorWindow::RestoreFromPersistence()
{
    if (!m_hwnd) return;

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

    int x = work.X + (work.Width - kWindowW) / 2;
    int y = work.Y + 20;

    const double t = std::clamp(m_persisted.edge_offset, 0.0, 1.0);
    switch (m_persisted.dock_edge) {
        case ::yip::DockEdge::Top:
            x = work.X + static_cast<int>(std::lround((work.Width - kWindowW) * t));
            y = work.Y + 12;
            break;
        case ::yip::DockEdge::Bottom:
            x = work.X + static_cast<int>(std::lround((work.Width - kWindowW) * t));
            y = work.Y + work.Height - kWindowH - 12;
            break;
        case ::yip::DockEdge::Left:
            x = work.X + 12;
            y = work.Y + static_cast<int>(std::lround((work.Height - kWindowH) * t));
            break;
        case ::yip::DockEdge::Right:
            x = work.X + work.Width - kWindowW - 12;
            y = work.Y + static_cast<int>(std::lround((work.Height - kWindowH) * t));
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
        y = work.Y + work.Height - kWindowH - 12;
    else if (minDist == distLeft)
        x = work.X + 12;
    else if (minDist == distRight)
        x = work.X + work.Width - kWindowW - 12;
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

    const int distTop = std::abs(rc.top - work.Y);
    const int distBottom = std::abs((work.Y + work.Height) - rc.bottom);
    const int distLeft = std::abs(rc.left - work.X);
    const int distRight = std::abs((work.X + work.Width) - rc.right);
    const int minDist = std::min({distTop, distBottom, distLeft, distRight});

    if (minDist > kSnapPx) {
        m_persisted.dock_edge = ::yip::DockEdge::None;
    } else if (minDist == distTop) {
        m_persisted.dock_edge = ::yip::DockEdge::Top;
        const int span = std::max(1, work.Width - kWindowW);
        m_persisted.edge_offset = std::clamp(double(rc.left - work.X) / span, 0.0, 1.0);
    } else if (minDist == distBottom) {
        m_persisted.dock_edge = ::yip::DockEdge::Bottom;
        const int span = std::max(1, work.Width - kWindowW);
        m_persisted.edge_offset = std::clamp(double(rc.left - work.X) / span, 0.0, 1.0);
    } else if (minDist == distLeft) {
        m_persisted.dock_edge = ::yip::DockEdge::Left;
        const int span = std::max(1, work.Height - kWindowH);
        m_persisted.edge_offset = std::clamp(double(rc.top - work.Y) / span, 0.0, 1.0);
    } else {
        m_persisted.dock_edge = ::yip::DockEdge::Right;
        const int span = std::max(1, work.Height - kWindowH);
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
