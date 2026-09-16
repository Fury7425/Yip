#include "pch.h"
#include "MainWindow.xaml.h"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "AudioCoreInterop.h"
#include "SettingsDialog.xaml.h"
#include "ProcessDialog.xaml.h"
#include "HotkeyManager.h"
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
MainWindow::MainWindow()
{
    InitializeComponent();

    m_viewModel = winrt::make<winrt::yip::viewmodels::implementation::MainViewModel>();
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
        if (auto self = weak.get()) self->TeardownBackdrop();
    });
    WireRecordButtonPress();
    if (auto appWindow = AppWindow()) {
        // AppWindow::Resize takes physical pixels. The layout is designed in
        // DIPs, so at 200% scale an unscaled 470x660 opened a window half the
        // intended size and clipped the clock, the meter and the list.
        const double scale = DpiScale();
        appWindow.Resize({static_cast<int32_t>(std::lround(kDefaultWindowW * scale)),
                          static_cast<int32_t>(std::lround(kDefaultWindowH * scale))});
    }

    UpdateRecordButtonShape();
    UpdateEmptyState();

    // Live device updates via IMMNotificationClient. Callback fires on a
    // WASAPI worker thread → marshal to UI dispatcher before touching VM.
    auto dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    m_deviceWatcher = std::make_unique<::yip::interop::DeviceWatcher>([weak = get_weak(), dispatcher]() {
        dispatcher.TryEnqueue([weak]() {
            if (auto self = weak.get()) {
                if (self->m_viewModel) self->m_viewModel.RefreshDevices();
            }
        });
    });

    // Meter polling is driven by audio-core state, not by a free-running timer:
    // idle Yip must not tick at all. Subscribe first, then reconcile in case a
    // session is somehow already live.
    m_stateToken = ::yip::RecordingStateBus::Subscribe(dispatcher, [weak = get_weak()](bool recording) {
        if (auto self = weak.get()) self->OnRecordingStateChanged(recording);
    });
    OnRecordingStateChanged(::yip::RecordingStateBus::IsRecording());

    // Global start/stop hotkey. WM_HOTKEY is delivered to this window's UI
    // thread, so the callback can touch the view model directly.
    if (m_hwnd) {
        m_hotkey = std::make_unique<::yip::HotkeyManager>(m_hwnd, [weak = get_weak()]() {
            if (auto self = weak.get()) {
                if (self->m_viewModel) self->m_viewModel.ToggleRecording();
            }
        });
        ApplyHotkeyFromSettings();
    }
}

MainWindow::~MainWindow()
{
    ::yip::RecordingStateBus::Unsubscribe(m_stateToken);
    m_stateToken = 0;
    if (m_viewModel && m_vmToken) {
        m_viewModel.PropertyChanged(m_vmToken);
        m_vmToken = {};
    }
    if (m_themeToken) {
        Root().ActualThemeChanged(m_themeToken);
        m_themeToken = {};
    }
    StopMeterPolling();
    TeardownBackdrop();
    m_hotkey.reset();
    m_deviceWatcher.reset();
    if (rec_is_recording()) {
        (void)rec_stop();
    }
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

    if (now_focused == m_focused) return;
    m_focused = now_focused;
    // Throttle meter poll: 60 Hz focused → 10 Hz blurred (spec). The timer only
    // exists while recording, so this is a no-op when idle.
    if (m_meterTimer) {
        m_meterTimer.Interval(m_focused ? std::chrono::milliseconds(16) : std::chrono::milliseconds(100));
    }
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
        // standing: it is the shape of the take that just finished.
        m_viewModel.Tick();
        if (m_waveHold) m_waveHold.Opacity(0.0f);
        m_viewModel.SyncRecordingState(false);
    }
}

void MainWindow::ApplyHotkeyFromSettings()
{
    if (!m_hotkey || !m_viewModel) return;
    const bool ok = m_hotkey->Register(m_viewModel.HotkeyMods(), m_viewModel.HotkeyVk());
    if (!ok) {
        m_viewModel.ReportHotkeyConflict();
    }
}

void MainWindow::StartMeterPolling()
{
    if (m_meterTimer) return;
    auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    m_meterTimer = queue.CreateTimer();
    m_meterTimer.Interval(m_focused ? std::chrono::milliseconds(16) : std::chrono::milliseconds(100));
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
    dialog.HotkeyMods(strong->m_viewModel.HotkeyMods());
    dialog.HotkeyVk(strong->m_viewModel.HotkeyVk());

    // ContentDialog needs an XamlRoot in WinAppSDK.
    dialog.XamlRoot(strong->Content().XamlRoot());

    const auto result = co_await dialog.ShowAsync();
    if (result == winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary) {
        strong->m_viewModel.ApplySettings(dialog.OutputFolder(), dialog.SampleRate(), dialog.Channels(),
                                          dialog.HotkeyMods(), dialog.HotkeyVk());
        // Re-grab the combo: the old registration is dropped inside Register().
        strong->ApplyHotkeyFromSettings();
        strong->UpdateEmptyState();
    }
    co_return;
}

winrt::fire_and_forget MainWindow::OnOpenProcess(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                                 winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
{
    auto strong = get_strong();
    auto dialog = winrt::make<winrt::yip::implementation::ProcessDialog>();
    dialog.XamlRoot(strong->Content().XamlRoot());
    co_await dialog.ShowAsync();
    strong->m_viewModel.RefreshRecordings(); // pick up *-processed.wav
    strong->UpdateEmptyState();
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

void MainWindow::OnRecordingActivated(
    winrt::Windows::Foundation::IInspectable const& sender,
    winrt::Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const& /*args*/)
{
    // Double-click plays. Single click used to fire Explorer, which is a
    // surprising amount of window for picking a row.
    if (auto entry = EntryFrom(sender)) {
        m_viewModel.OpenRecording(entry);
    }
}

void MainWindow::OnPlayItem(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
{
    if (auto entry = EntryFrom(sender)) m_viewModel.OpenRecording(entry);
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

void MainWindow::OnProcessAccelerator(
    winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& /*sender*/,
    winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
{
    args.Handled(true);
    OnOpenProcess(nullptr, nullptr);
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
} // namespace winrt::yip::implementation
