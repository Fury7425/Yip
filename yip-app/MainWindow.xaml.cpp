#include "pch.h"
#include "MainWindow.xaml.h"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "AudioCoreInterop.h"
#include "SettingsDialog.xaml.h"
#include "Settings.h"
#include "ThemeColors.h"
#include "resource.h"

#include <DispatcherQueue.h>
#include <microsoft.ui.xaml.window.h>

#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>

#include <algorithm>
#include <cmath>

using namespace std::chrono_literals;

namespace winrt {
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Dispatching;
using namespace winrt::Windows::Foundation;
} // namespace winrt

namespace mucomp = winrt::Microsoft::UI::Composition;
namespace musb = winrt::Microsoft::UI::Composition::SystemBackdrops;
namespace muxh = winrt::Microsoft::UI::Xaml::Hosting;
using winrt::Windows::Foundation::Numerics::float3;

namespace {

// Default window size on first show. Tall enough for the transport card plus a
// handful of takes without scrolling.
constexpr int kDefaultWindowW = 470;
constexpr int kDefaultWindowH = 660;

// Right margin for the title-bar actions when the caption-button inset is not
// readable yet. Wide enough to clear minimise/maximise/close at 100% scale.
constexpr double kFallbackCaptionInset = 140.0;

// Waveform geometry. Bar pitch is fixed; the bar count follows the host width.
constexpr float kWaveBarWidth = 3.0f;
constexpr float kWaveBarGap = 2.0f;
constexpr float kWavePitch = kWaveBarWidth + kWaveBarGap;
// Silence draws nothing: a row of 3px stubs read as a dotted-line artefact.
// The static baseline behind the strip carries the "alive" signal instead.
constexpr float kWaveRestPx = 0.0f;
constexpr int kWaveMinBars = 8;
constexpr int kWaveMaxBars = 480;
constexpr uint32_t kWavePaletteSteps = 16;

// Shown only if a token key is wrong. A deliberate flat grey rather than a
// second copy of the palette, so a miss is visible instead of plausible.
constexpr winrt::Windows::UI::Color kMissingToken{0xFF, 0x80, 0x80, 0x80};

// Playback poll. Fast enough that the scrubber and the level read as
// continuous, and it only runs while a take is loaded.
constexpr int kPlaybackTickFocusedMs = 50;
constexpr int kPlaybackTickBlurredMs = 200;
// Minimized, the transport is not on screen. The poll keeps running only
// because it is what notices the file ending and releases the player.
constexpr int kPlaybackTickMinimizedMs = 1000;

// Capture meter poll: 60 Hz focused, 10 Hz blurred. Stopped while minimized.
constexpr int kMeterTickFocusedMs = 16;
constexpr int kMeterTickBlurredMs = 100;

// Ticks the scrubber is left alone for after a seek, so the thumb is not
// dragged back by a position that has not caught up yet.
constexpr int kSeekHoldTicks = 6;

// Transport glyphs. Play while held, pause while running: a transport button
// shows what it will do, not what it is doing.
constexpr wchar_t kPlayGlyph[] = L"\uE768";
constexpr wchar_t kPauseGlyph[] = L"\uE769";

// The elapsed clock is dimmed until there is something to count.
constexpr double kIdleClockOpacity = 0.55;

// The strip keeps the finished take's shape after a stop, but at full
// brightness beside a 00:00.0 clock it reads as though capture is still
// running. Dimmed, it reads as what it is: the last take.
constexpr float kWaveIdleOpacity = 0.32f;

// Motion. The strip lights up fast when a take starts (the user is watching
// for it) and settles slower when it ends (nothing is waiting on it).
constexpr int kWaveLightMs = 180;
constexpr int kWaveDimMs = 320;
// Record button press: subtle enough to feel like a key, not a bounce.
constexpr float kRecordPressScale = 0.94f;
// Resting scale of whichever record glyph is hidden. Never zero: a shape that
// grows out of nothing reads as appearing from nowhere.
constexpr float kGlyphHiddenScale = 0.5f;

// Strong ease-out: starts moving on the frame it is asked to.
mucomp::CompositionEasingFunction EaseOut(mucomp::Compositor const& c)
{
    return c.CreateCubicBezierEasingFunction({0.23f, 1.0f}, {0.32f, 1.0f});
}

musb::SystemBackdropTheme BackdropThemeFor(winrt::Microsoft::UI::Xaml::ElementTheme theme) noexcept
{
    switch (theme) {
        case winrt::Microsoft::UI::Xaml::ElementTheme::Dark:
            return musb::SystemBackdropTheme::Dark;
        case winrt::Microsoft::UI::Xaml::ElementTheme::Light:
            return musb::SystemBackdropTheme::Light;
        default:
            return musb::SystemBackdropTheme::Default;
    }
}

/// Pull the recording an item-scoped event belongs to out of its DataContext.
winrt::yip::viewmodels::RecordingEntry EntryFrom(winrt::Windows::Foundation::IInspectable const& sender)
{
    auto element = sender.try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>();
    if (!element) return nullptr;
    auto context = element.DataContext();
    if (!context) return nullptr;
    return context.try_as<winrt::yip::viewmodels::RecordingEntry>();
}

} // namespace

namespace {
// The shell reads the exe's icon resource for Explorer and the taskbar, but the
// title bar, Alt-Tab and window thumbnails read the icon set on the HWND. Two
// sizes are set because Windows picks between them by context; LoadImage
// selects the .ico frame drawn for that size rather than scaling one, and
// LR_SHARED hands back a cached handle that must not be destroyed.
void ApplyWindowIcon(HWND hwnd)
{
    const HMODULE instance = ::GetModuleHandleW(nullptr);
    const auto load = [instance](int cx, int cy) {
        return static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_YIP_APP), IMAGE_ICON, cx, cy,
                                               LR_DEFAULTCOLOR | LR_SHARED));
    };

    if (const HICON iconSmall = load(::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON))) {
        ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall));
    }
    if (const HICON iconBig = load(::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON))) {
        ::SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig));
    }
}
} // namespace

namespace winrt::yip::implementation {
MainWindow::MainWindow() : MainWindow(nullptr) {}

MainWindow::MainWindow(winrt::yip::viewmodels::MainViewModel const& viewModel)
{
    InitializeComponent();

    // The view model normally belongs to App and outlives this window: closing
    // to the tray destroys the window, and the take, the transport and the
    // settings carry on without it. Standalone, the window makes its own.
    m_viewModel =
        viewModel ? viewModel : winrt::make<winrt::yip::viewmodels::implementation::MainViewModel>();
    // A fresh window has an empty search box, so the list must not come back
    // filtered by whatever was typed into the last one. Brushes may be from a
    // theme that changed while no window was open.
    m_viewModel.FilterText(L"");
    m_viewModel.InvalidateThemeBrushes();
    m_viewModel.RefreshDevices();
    m_viewModel.RefreshRecordings();

    m_vmToken = m_viewModel.PropertyChanged({this, &MainWindow::OnViewModelPropertyChanged});
    m_themeToken = Root().ActualThemeChanged({this, &MainWindow::OnActualThemeChanged});

    ResolveThemeBrushes();
    Activated({this, &MainWindow::OnActivated});

    if (auto native = try_as<::IWindowNative>()) {
        native->get_WindowHandle(&m_hwnd);
    }
    if (m_hwnd) ApplyWindowIcon(m_hwnd);

    SetupTitleBar();
    SetupBackdrop();
    Closed([weak = get_weak()](auto&&, auto&&) {
        if (auto self = weak.get()) self->Teardown();
    });
    WireRecordButtonPress();
    if (auto appWindow = AppWindow()) {
        // AppWindow::Resize takes physical pixels. The layout is designed in
        // DIPs, so at 200% scale an unscaled 470x660 opened a window half the
        // intended size and clipped the clock, the meter and the list.
        const double scale = DpiScale();
        appWindow.Resize({static_cast<int32_t>(std::lround(kDefaultWindowW * scale)),
                          static_cast<int32_t>(std::lround(kDefaultWindowH * scale))});
        // Minimize and restore both raise Changed. OnActivated re-checks too,
        // so a missed restore can never leave the meter stopped.
        m_appWindow = appWindow;
        m_appWindowToken = appWindow.Changed([weak = get_weak()](auto&&, auto&&) {
            if (auto self = weak.get()) self->UpdateMinimized();
        });
    }

    UpdateRecordButtonShape();
    UpdateEmptyState();
    UpdatePlaybackBar();

    // Device changes, the global hotkey and the view model's own capture-state
    // sync live in App: they have to work while no window exists.
    auto dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

    // Meter polling is driven by audio-core state, not by a free-running timer:
    // idle Yip must not tick at all. Subscribe first, then reconcile in case a
    // session is somehow already live.
    m_stateToken = ::yip::RecordingStateBus::Subscribe(dispatcher, [weak = get_weak()](bool recording) {
        if (auto self = weak.get()) self->OnRecordingStateChanged(recording);
    });
    if (::yip::RecordingStateBus::IsRecording()) {
        // A take already running (started from the tray or the hotkey while no
        // window existed): pick the strip up live.
        ClearWave();
        FadeWave(1.0f);
        StartMeterPolling();
    }
}

MainWindow::~MainWindow()
{
    // Normally already done by Closed. Capture and playback are not this
    // window's to stop: App::Quit ends both.
    Teardown();
}

void MainWindow::Teardown()
{
    if (m_tornDown) return;
    m_tornDown = true;

    ::yip::RecordingStateBus::Unsubscribe(m_stateToken);
    m_stateToken = 0;
    if (m_appWindow) {
        m_appWindow.Changed(m_appWindowToken);
        m_appWindowToken = {};
        m_appWindow = nullptr;
    }
    StopMeterPolling();
    StopPlaybackPolling();
    if (m_themeToken) {
        Root().ActualThemeChanged(m_themeToken);
        m_themeToken = {};
    }
    TeardownBackdrop();

    if (!m_viewModel) return;
    if (m_vmToken) {
        m_viewModel.PropertyChanged(m_vmToken);
        m_vmToken = {};
    }
    // The view model outlives this window, so everything in the window that
    // listens to it lets go now. Otherwise the view model keeps the dead
    // window's element tree alive — the memory closing it was meant to free —
    // and raises into it. Unbinding the lists makes the ComboBox write -1
    // back through its two-way SelectedIndex; the selection is put back after.
    const auto selectedDevice = m_viewModel.SelectedDeviceIndex();
    Bindings->StopTracking();
    DeviceCombo().ItemsSource(nullptr);
    RecordingsList().ItemsSource(nullptr);
    m_viewModel.SelectedDeviceIndex(selectedDevice);
}

winrt::yip::viewmodels::MainViewModel MainWindow::ViewModel()
{
    return m_viewModel;
}

// ============================================================ Title bar

void MainWindow::SetupTitleBar()
{
    // Content under the caption area, with AppTitleBar as the drag region.
    // Without this the app gets the stock grey title bar and the backdrop stops
    // at it.
    ExtendsContentIntoTitleBar(true);
    SetTitleBar(AppTitleBar());
    UpdateTitleBarInset();
}

void MainWindow::SetupBackdrop()
{
    // Acrylic, not Mica: Mica is a faint static tint, and with the cards
    // covering nearly the whole window it read as a flat opaque box.
    //
    // And acrylic that stays acrylic. Window::SystemBackdrop's
    // DesktopAcrylicBackdrop swaps to its fallback fill the moment the window
    // loses focus — which for a recorder is most of the time, since you click
    // into whatever you are recording — and Yip turned into a grey slab.
    // Driving the controller directly lets the configuration report the
    // window as always active.
    if (!musb::DesktopAcrylicController::IsSupported()) return;
    auto target = try_as<mucomp::ICompositionSupportsSystemBackdrop>();
    if (!target) return;

    // The controller lives in the system compositor, which needs a
    // Windows.System dispatcher queue on this thread; WinUI only guarantees the
    // Microsoft.UI one.
    if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
        DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
        if (FAILED(::CreateDispatcherQueueController(
                options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(
                             winrt::put_abi(m_backdropQueue))))) {
            return;
        }
    }

    m_backdropConfig = musb::SystemBackdropConfiguration{};
    m_backdropConfig.IsInputActive(true);
    m_backdropConfig.Theme(BackdropThemeFor(Root().ActualTheme()));

    m_backdrop = musb::DesktopAcrylicController{};
    m_backdrop.SetSystemBackdropConfiguration(m_backdropConfig);
    m_backdrop.AddSystemBackdropTarget(target);
}

void MainWindow::TeardownBackdrop()
{
    if (m_backdrop) {
        m_backdrop.Close();
        m_backdrop = nullptr;
    }
    m_backdropConfig = nullptr;
}

double MainWindow::DpiScale() const noexcept
{
    // Readable from the HWND before XAML has a XamlRoot, which is when the
    // initial size and the first caption inset are computed.
    const UINT dpi = m_hwnd ? ::GetDpiForWindow(m_hwnd) : 0;
    return dpi > 0 ? static_cast<double>(dpi) / 96.0 : 1.0;
}

void MainWindow::UpdateTitleBarInset()
{
    double inset = kFallbackCaptionInset;

    auto appWindow = AppWindow();
    if (appWindow) {
        if (auto titleBar = appWindow.TitleBar()) {
            // RightInset is in physical pixels; XAML margins are in DIPs.
            const double captionWidth = static_cast<double>(titleBar.RightInset()) / DpiScale();
            if (captionWidth > 0.0) inset = captionWidth + 4.0;
        }
    }
    TitleBarActions().Margin({0.0, 0.0, inset, 0.0});
}

// ============================================================ Theme

void MainWindow::ResolveThemeBrushes()
{
    m_lampIdleBrush = ::yip::theme::Brush(L"YipLampIdleBrush");
    m_lampLiveBrush = ::yip::theme::Brush(L"YipLampLiveBrush");

    if (!m_waveRoot) return;

    auto compositor = muxh::ElementCompositionPreview::GetElementVisual(WaveHost()).Compositor();

    // Mutate the brushes in place rather than making new ones: every bar
    // already holds a reference, so they all repaint without being touched.
    const auto rest = ::yip::theme::Color(L"YipWaveRestBrush", kMissingToken);
    if (m_waveRestBrush) {
        m_waveRestBrush.Color(rest);
    } else {
        m_waveRestBrush = compositor.CreateColorBrush(rest);
    }

    const auto hold = ::yip::theme::Color(L"YipMeterHoldBrush", kMissingToken);
    if (m_waveHoldBrush) {
        m_waveHoldBrush.Color(hold);
    } else {
        m_waveHoldBrush = compositor.CreateColorBrush(hold);
    }

    const auto ramp = ::yip::theme::SampleMeterRamp(kWavePaletteSteps);
    if (ramp.size() == m_wavePalette.size()) {
        for (size_t i = 0; i < ramp.size(); ++i)
            m_wavePalette[i].Color(ramp[i]);
    } else {
        m_wavePalette.clear();
        m_wavePalette.reserve(ramp.size());
        for (auto const& color : ramp)
            m_wavePalette.push_back(compositor.CreateColorBrush(color));
    }
}

void MainWindow::OnActualThemeChanged(winrt::Microsoft::UI::Xaml::FrameworkElement const& /*sender*/,
                                      winrt::Windows::Foundation::IInspectable const& /*args*/)
{
    if (m_viewModel) m_viewModel.InvalidateThemeBrushes();
    if (m_backdropConfig) m_backdropConfig.Theme(BackdropThemeFor(Root().ActualTheme()));
    ResolveThemeBrushes();
    UpdateRecordButtonShape();
}

// ============================================================ Waveform

void MainWindow::OnWaveSizeChanged(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                   winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args)
{
    BuildWaveVisuals(args.NewSize().Width, args.NewSize().Height);
}

void MainWindow::BuildWaveVisuals(double width, double height)
{
    if (width <= 1.0 || height <= 1.0) return;

    auto host = WaveHost();
    auto compositor = muxh::ElementCompositionPreview::GetElementVisual(host).Compositor();

    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);
    const int bars =
        std::clamp(static_cast<int>(std::ceil(width / kWavePitch)) + 1, kWaveMinBars, kWaveMaxBars);

    // Resizing rebuilds and loses the history. That only happens when the user
    // drags the window, which is not a moment anyone is reading the strip.
    m_waveWidth = width;
    m_waveHeight = height;

    auto root = compositor.CreateContainerVisual();
    root.Size({w, h});
    root.Clip(compositor.CreateInsetClip());

    m_waveRoot = root;
    ResolveThemeBrushes(); // needs m_waveRoot set so it knows a compositor exists

    auto scroller = compositor.CreateContainerVisual();
    scroller.Size({kWavePitch * static_cast<float>(bars) * 2.0f, h});

    const float rest = kWaveRestPx / std::max(h, 1.0f);
    m_waveBars.clear();
    m_waveBars.reserve(static_cast<size_t>(bars) * 2);
    for (int i = 0; i < bars * 2; ++i) {
        auto bar = compositor.CreateSpriteVisual();
        bar.Size({kWaveBarWidth, h});
        bar.AnchorPoint({0.5f, 0.5f});
        bar.Offset({static_cast<float>(i) * kWavePitch + kWavePitch * 0.5f, h * 0.5f, 0.0f});
        bar.Scale({1.0f, rest, 1.0f});
        if (m_waveRestBrush) bar.Brush(m_waveRestBrush);
        scroller.Children().InsertAtTop(bar);
        m_waveBars.push_back(bar);
    }
    root.Children().InsertAtTop(scroller);

    // Peak hold: one line across the strip, above the bars.
    auto hold = compositor.CreateSpriteVisual();
    hold.Size({w, 1.0f});
    hold.Offset({0.0f, h * 0.5f, 0.0f});
    if (m_waveHoldBrush) hold.Brush(m_waveHoldBrush);
    hold.Opacity(0.0f);
    root.Children().InsertAtTop(hold);

    muxh::ElementCompositionPreview::SetElementChildVisual(host, root);

    m_waveScroller = scroller;
    m_waveHold = hold;
    m_waveCount = bars;
    m_waveHead = 0;
    scroller.Offset({0.0f, 0.0f, 0.0f});
    root.Opacity((m_viewModel && m_viewModel.IsRecording()) ? 1.0f : kWaveIdleOpacity);
}

void MainWindow::PushWaveSample(float level, float hold)
{
    if (!m_waveScroller || m_waveCount <= 0) return;
    if (m_waveBars.size() < static_cast<size_t>(m_waveCount) * 2) return;

    const auto h = static_cast<float>(m_waveHeight);
    const float rest = kWaveRestPx / std::max(h, 1.0f);
    const float clamped = std::clamp(level, 0.0f, 1.0f);
    const float scale = std::max(rest, clamped);

    auto brush = m_waveRestBrush;
    if (!m_wavePalette.empty()) {
        const auto last = static_cast<float>(m_wavePalette.size() - 1);
        const auto index = static_cast<size_t>(std::lround(clamped * last));
        brush = m_wavePalette[std::min(index, m_wavePalette.size() - 1)];
    }

    // The same sample is written to both copies of the bar; the scroller then
    // steps one pitch. Five property writes, whatever the strip's width.
    const auto a = static_cast<size_t>(m_waveHead);
    const auto b = a + static_cast<size_t>(m_waveCount);
    m_waveBars[a].Scale({1.0f, scale, 1.0f});
    m_waveBars[b].Scale({1.0f, scale, 1.0f});
    if (brush) {
        m_waveBars[a].Brush(brush);
        m_waveBars[b].Brush(brush);
    }

    m_waveHead = (m_waveHead + 1) % m_waveCount;
    m_waveScroller.Offset({-static_cast<float>(m_waveHead) * kWavePitch, 0.0f, 0.0f});

    if (m_waveHold) {
        const float held = std::clamp(hold, 0.0f, 1.0f);
        m_waveHold.Offset({0.0f, h * 0.5f - held * h * 0.5f, 0.0f});
        m_waveHold.Opacity(held > 0.002f ? 1.0f : 0.0f);
    }
}

void MainWindow::ClearWave()
{
    if (m_waveBars.empty()) return;
    const float rest = kWaveRestPx / std::max(static_cast<float>(m_waveHeight), 1.0f);
    for (auto& bar : m_waveBars) {
        bar.Scale({1.0f, rest, 1.0f});
        if (m_waveRestBrush) bar.Brush(m_waveRestBrush);
    }
    m_waveHead = 0;
    if (m_waveScroller) m_waveScroller.Offset({0.0f, 0.0f, 0.0f});
    if (m_waveHold) m_waveHold.Opacity(0.0f);
}

void MainWindow::FadeWave(float opacity)
{
    if (!m_waveRoot) return;
    auto compositor = m_waveRoot.Compositor();
    auto anim = compositor.CreateScalarKeyFrameAnimation();
    anim.InsertKeyFrame(1.0f, opacity, EaseOut(compositor));
    anim.Duration(std::chrono::milliseconds(opacity >= 1.0f ? kWaveLightMs : kWaveDimMs));
    m_waveRoot.StartAnimation(L"Opacity", anim);
}

// ============================================================ Activation

void MainWindow::OnActivated(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                             winrt::Microsoft::UI::Xaml::WindowActivatedEventArgs const& args)
{
    const bool now_focused =
        args.WindowActivationState() != winrt::Microsoft::UI::Xaml::WindowActivationState::Deactivated;

    // Caption metrics settle after the first activation, and change again on a
    // DPI move, so re-measure whichever way focus went.
    UpdateTitleBarInset();
    UpdateMinimized();

    if (now_focused == m_focused) return;
    m_focused = now_focused;
    // Throttle meter poll: 60 Hz focused → 10 Hz blurred (spec). The timer only
    // exists while recording, so this is a no-op when idle.
    if (m_meterTimer) m_meterTimer.Interval(MeterInterval());
    if (m_playbackTimer) m_playbackTimer.Interval(PlaybackInterval());
}

bool MainWindow::IsMinimized()
{
    namespace muw = winrt::Microsoft::UI::Windowing;
    if (m_appWindow) {
        if (auto presenter = m_appWindow.Presenter().try_as<muw::OverlappedPresenter>()) {
            return presenter.State() == muw::OverlappedPresenterState::Minimized;
        }
    }
    return m_hwnd && ::IsIconic(m_hwnd);
}

void MainWindow::UpdateMinimized()
{
    if (m_tornDown) return;
    const bool minimized = IsMinimized();
    if (minimized == m_minimized) return;
    m_minimized = minimized;

    // Nothing reads the meter or the strip while minimized. The view model
    // keeps no per-tick history, so the first tick after a restore lands on
    // the live values and the strip carries on from where it stopped.
    if (m_minimized) {
        StopMeterPolling();
    } else if (::yip::RecordingStateBus::IsRecording()) {
        StartMeterPolling();
    }
    if (m_playbackTimer) m_playbackTimer.Interval(PlaybackInterval());
}

std::chrono::milliseconds MainWindow::MeterInterval() const noexcept
{
    return std::chrono::milliseconds(m_focused ? kMeterTickFocusedMs : kMeterTickBlurredMs);
}

std::chrono::milliseconds MainWindow::PlaybackInterval() const noexcept
{
    if (m_minimized) return std::chrono::milliseconds(kPlaybackTickMinimizedMs);
    return std::chrono::milliseconds(m_focused ? kPlaybackTickFocusedMs : kPlaybackTickBlurredMs);
}

// ============================================================ View model

void MainWindow::OnViewModelPropertyChanged(
    winrt::Windows::Foundation::IInspectable const& /*sender*/,
    winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const& args)
{
    const auto name = args.PropertyName();

    if (name == L"IsRecording") {
        UpdateRecordButtonShape();
    } else if (name == L"HasError") {
        // InfoBar owns IsOpen once the user hits its close button, so it is
        // driven here rather than bound one-way and fought over.
        ErrorBar().IsOpen(m_viewModel.HasError());
    } else if (name == L"HasClipped") {
        ClipLamp().Opacity(m_viewModel.HasClipped() ? 1.0 : 0.18);
    } else if (name == L"IsEmpty") {
        UpdateEmptyState();
    } else if (name == L"IsPlaybackLoaded" || name == L"IsPlaybackLive") {
        UpdatePlaybackBar();
    } else if (name == L"IsPlaybackPlaying") {
        UpdatePlayPauseGlyph();
    }
}

void MainWindow::UpdateRecordButtonShape()
{
    const bool recording = m_viewModel && m_viewModel.IsRecording();
    // Both glyphs stay in the tree; their Opacity/Scale transitions in XAML
    // turn this into a crossfade where the dot shrinks out as the square grows in.
    const float3 shown{1.0f, 1.0f, 1.0f};
    const float3 hidden{kGlyphHiddenScale, kGlyphHiddenScale, 1.0f};
    RecordDot().Opacity(recording ? 0.0 : 1.0);
    RecordDot().Scale(recording ? hidden : shown);
    StopSquare().Opacity(recording ? 1.0 : 0.0);
    StopSquare().Scale(recording ? shown : hidden);
    TitleLamp().Fill(recording ? m_lampLiveBrush : m_lampIdleBrush);
    ElapsedLabel().Opacity(recording ? 1.0 : kIdleClockOpacity);
}

void MainWindow::WireRecordButtonPress()
{
    // IsPressed tracks the pointer going down and coming back up (or leaving),
    // which is exactly the span press feedback should cover. Ctrl+R and the
    // global hotkey never set it, so the shortcuts stay unanimated.
    RecordButton().RegisterPropertyChangedCallback(
        winrt::Microsoft::UI::Xaml::Controls::Primitives::ButtonBase::IsPressedProperty(),
        [weak = get_weak()](winrt::Microsoft::UI::Xaml::DependencyObject const&,
                            winrt::Microsoft::UI::Xaml::DependencyProperty const&) {
            if (auto self = weak.get()) self->PressRecordButton(self->RecordButton().IsPressed());
        });
}

void MainWindow::PressRecordButton(bool down)
{
    // The ScaleTransition in XAML animates both directions.
    const float s = (down && RecordButton().IsEnabled()) ? kRecordPressScale : 1.0f;
    RecordButton().Scale({s, s, 1.0f});
}

void MainWindow::UpdateEmptyState()
{
    if (!m_viewModel) return;
    const bool empty = m_viewModel.IsEmpty();
    EmptyState().Visibility(empty ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                                  : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    if (!empty) return;

    // "Nothing recorded yet" and "nothing matches your filter" are different
    // problems and want different sentences.
    const bool filtered = !m_viewModel.FilterText().empty();
    EmptyStateText().Text(filtered ? L"No matches" : L"No recordings yet");
    EmptyStateHint().Text(filtered ? L"Try a different filter." : L"Press Record, or use the global hotkey.");
}

void MainWindow::OnAcknowledgeClip(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                   winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const& /*args*/)
{
    m_viewModel.AcknowledgeClip();
}

void MainWindow::OnDismissError(winrt::Microsoft::UI::Xaml::Controls::InfoBar const& /*sender*/,
                                winrt::Windows::Foundation::IInspectable const& /*args*/)
{
    m_viewModel.DismissError();
}

// ============================================================ Capture state

void MainWindow::OnRecordingStateChanged(bool recording)
{
    if (recording) {
        // A new take starts from an empty strip; the previous one is history
        // nobody wants scrolling underneath it.
        ClearWave();
        FadeWave(1.0f);
        StartMeterPolling();
        return;
    }
    StopMeterPolling();
    FadeWave(kWaveIdleOpacity);
    if (m_viewModel) {
        // One last pull so the readouts land on the post-stop zero instead of
        // freezing at whatever the final tick read. The strip itself is left
        // standing: it is the shape of the take that just finished. The view
        // model's own state is synced by App, which also runs windowless.
        m_viewModel.Tick();
        if (m_waveHold) m_waveHold.Opacity(0.0f);
    }
}

void MainWindow::StartMeterPolling()
{
    // A take started while minimized picks its meter up on restore.
    if (m_meterTimer || m_minimized) return;
    auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    m_meterTimer = queue.CreateTimer();
    m_meterTimer.Interval(MeterInterval());
    m_meterTimer.IsRepeating(true);
    m_meterTimer.Tick([weak = get_weak()](auto&&, auto&&) {
        if (auto self = weak.get()) {
            if (self->m_viewModel) {
                self->m_viewModel.Tick();
                self->PushWaveSample(self->m_viewModel.MeterPeak(), self->m_viewModel.MeterHold());
            }
        }
    });
    m_meterTimer.Start();
}

void MainWindow::StopMeterPolling()
{
    if (m_meterTimer) {
        m_meterTimer.Stop();
        m_meterTimer = nullptr;
    }
}

// ============================================================ Playback

void MainWindow::StartPlaybackPolling()
{
    if (m_playbackTimer) return;
    auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    m_playbackTimer = queue.CreateTimer();
    m_playbackTimer.Interval(PlaybackInterval());
    m_playbackTimer.IsRepeating(true);
    m_playbackTimer.Tick([weak = get_weak()](auto&&, auto&&) {
        if (auto self = weak.get()) {
            if (self->m_viewModel) {
                // The tick is what notices the take ending, so the bar can go
                // away on its own the moment the file runs out.
                self->m_viewModel.PlaybackTick();
                self->UpdateSeekSlider();
            }
        }
    });
    m_playbackTimer.Start();
}

void MainWindow::StopPlaybackPolling()
{
    if (m_playbackTimer) {
        m_playbackTimer.Stop();
        m_playbackTimer = nullptr;
    }
}

void MainWindow::UpdatePlaybackBar()
{
    if (!m_viewModel) return;
    const bool loaded = m_viewModel.IsPlaybackLoaded();
    PlaybackBar().Visibility(loaded ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                                    : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    UpdatePlayPauseGlyph();

    if (loaded) {
        // Poll only a take with a player behind it. One kept by
        // SuspendPlayback is shown, held, and costs nothing until play.
        if (m_viewModel.IsPlaybackLive()) {
            StartPlaybackPolling();
        } else {
            StopPlaybackPolling();
            m_seekHoldTicks = 0;
        }
        UpdateSeekSlider();
        return;
    }
    StopPlaybackPolling();
    m_seekHoldTicks = 0;
    m_suppressSeek = true;
    SeekSlider().Value(0.0);
    m_suppressSeek = false;
}

void MainWindow::UpdatePlayPauseGlyph()
{
    if (!m_viewModel) return;
    PlayPauseGlyph().Glyph(m_viewModel.IsPlaybackPlaying() ? kPauseGlyph : kPlayGlyph);
}

void MainWindow::UpdateSeekSlider()
{
    if (!m_viewModel) return;
    const double duration = m_viewModel.PlaybackDurationMs();
    const bool seekable = duration > 0.0;

    // A container that does not declare a duration still plays; there is just
    // nothing for the thumb to span.
    SeekSlider().IsEnabled(seekable);

    m_suppressSeek = true;
    SeekSlider().Maximum(seekable ? duration : 100.0);
    if (m_seekHoldTicks > 0) {
        --m_seekHoldTicks;
    } else {
        SeekSlider().Value(std::clamp(m_viewModel.PlaybackPositionMs(), 0.0, SeekSlider().Maximum()));
    }
    m_suppressSeek = false;
}

void MainWindow::OnTogglePlayback(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
{
    m_viewModel.TogglePlayback();
}

void MainWindow::OnStopPlayback(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
{
    m_viewModel.StopPlayback();
}

void MainWindow::OnSeekChanged(
    winrt::Windows::Foundation::IInspectable const& /*sender*/,
    winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
{
    // Coercions from setting Maximum come through here too; only a value the
    // user put there is a seek.
    if (m_suppressSeek || !m_viewModel) return;
    m_viewModel.SeekPlayback(static_cast<uint64_t>(std::max(0.0, args.NewValue())));
    m_seekHoldTicks = kSeekHoldTicks;
}

// ============================================================ Commands

void MainWindow::OnRecordToggle(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
{
    m_viewModel.ToggleRecording();
}

winrt::fire_and_forget MainWindow::OnOpenSettings(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
{
    auto strong = get_strong();

    auto dialog = winrt::make<winrt::yip::implementation::SettingsDialog>();
    dialog.OutputFolder(strong->m_viewModel.OutputFolder());
    dialog.SampleRate(strong->m_viewModel.SampleRate());
    dialog.Channels(strong->m_viewModel.Channels());
    dialog.Format(strong->m_viewModel.Format());
    dialog.BitDepth(strong->m_viewModel.BitDepth());
    dialog.BitrateKbps(strong->m_viewModel.BitrateKbps());
    dialog.HotkeyMods(strong->m_viewModel.HotkeyMods());
    dialog.HotkeyVk(strong->m_viewModel.HotkeyVk());
    dialog.PillDot(strong->m_viewModel.PillDot());
    dialog.PillBottom(strong->m_viewModel.PillBottom());

    // ContentDialog needs an XamlRoot in WinAppSDK.
    dialog.XamlRoot(strong->Content().XamlRoot());

    const auto result = co_await dialog.ShowAsync();
    if (result == winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary) {
        strong->m_viewModel.ApplySettings(dialog.OutputFolder(), dialog.SampleRate(), dialog.Channels(),
                                          dialog.Format(), dialog.BitDepth(), dialog.BitrateKbps(),
                                          dialog.HotkeyMods(), dialog.HotkeyVk(), dialog.PillDot(),
                                          dialog.PillBottom());
        // The hotkey follows on its own: App re-registers when the view model
        // raises HotkeyVk.
        strong->UpdateEmptyState();
    }
    co_return;
}

void MainWindow::OnRefreshList(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                               winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
{
    m_viewModel.RefreshDevices();
    m_viewModel.RefreshRecordings();
    UpdateEmptyState();
}

void MainWindow::OnFilterChanged(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                 winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const& /*args*/)
{
    m_viewModel.FilterText(SearchBox().Text());
    UpdateEmptyState();
}

void MainWindow::OnRecordingClick(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                  winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& args)
{
    // One click plays, and a click on the row already playing holds it. Nothing
    // here launches another app: playback is Yip's own.
    if (auto entry = args.ClickedItem().try_as<winrt::yip::viewmodels::RecordingEntry>()) {
        m_viewModel.ActivateRecording(entry);
    }
}

void MainWindow::OnPlayItem(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
{
    // From the menu, Play means play this one from the top, whatever is loaded.
    if (auto entry = EntryFrom(sender)) m_viewModel.PlayRecording(entry);
}

void MainWindow::OnOpenExternallyItem(winrt::Windows::Foundation::IInspectable const& sender,
                                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
{
    if (auto entry = EntryFrom(sender)) m_viewModel.OpenRecordingExternally(entry);
}

void MainWindow::OnRevealItem(winrt::Windows::Foundation::IInspectable const& sender,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
{
    if (auto entry = EntryFrom(sender)) m_viewModel.RevealRecording(entry);
}

void MainWindow::OnCopyPathItem(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
{
    if (auto entry = EntryFrom(sender)) m_viewModel.CopyRecordingPath(entry);
}

winrt::fire_and_forget MainWindow::OnDeleteItem(winrt::Windows::Foundation::IInspectable const& sender,
                                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
{
    auto strong = get_strong();
    auto entry = EntryFrom(sender);
    if (!entry) co_return;

    winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog;
    dialog.XamlRoot(strong->Content().XamlRoot());
    dialog.Title(winrt::box_value(winrt::hstring{L"Delete recording?"}));
    dialog.Content(winrt::box_value(entry.FileName()));
    dialog.PrimaryButtonText(L"Delete");
    dialog.CloseButtonText(L"Cancel");
    dialog.DefaultButton(winrt::Microsoft::UI::Xaml::Controls::ContentDialogButton::Close);

    const auto result = co_await dialog.ShowAsync();
    if (result == winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary) {
        strong->m_viewModel.DeleteRecording(entry);
        strong->UpdateEmptyState();
    }
    co_return;
}

// ============================================================ Accelerators

void MainWindow::OnRecordAccelerator(
    winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& /*sender*/,
    winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
{
    args.Handled(true);
    m_viewModel.ToggleRecording();
}

void MainWindow::OnSearchAccelerator(
    winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& /*sender*/,
    winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
{
    args.Handled(true);
    SearchBox().Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
}

void MainWindow::OnRefreshAccelerator(
    winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& /*sender*/,
    winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
{
    args.Handled(true);
    OnRefreshList(nullptr, nullptr);
}

void MainWindow::OnPlayAccelerator(
    winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& /*sender*/,
    winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
{
    args.Handled(true);
    if (!m_viewModel) return;
    // With nothing loaded, Ctrl+P plays whatever is selected — which is what
    // the shortcut is for when the list has focus.
    if (!m_viewModel.IsPlaybackLoaded()) {
        auto selected = RecordingsList().SelectedItem();
        if (!selected) return;
        if (auto entry = selected.try_as<winrt::yip::viewmodels::RecordingEntry>()) {
            m_viewModel.PlayRecording(entry);
        }
        return;
    }
    m_viewModel.TogglePlayback();
}
} // namespace winrt::yip::implementation
