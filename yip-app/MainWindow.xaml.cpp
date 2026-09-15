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

#include <microsoft.ui.xaml.window.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>

#include <algorithm>

using namespace std::chrono_literals;

namespace winrt {
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Dispatching;
using namespace winrt::Windows::Foundation;
} // namespace winrt

namespace {

// Default window size on first show. Tall enough for the transport card plus a
// handful of takes without scrolling.
constexpr int kDefaultWindowW = 470;
constexpr int kDefaultWindowH = 640;

// Right margin for the title-bar actions when the caption-button inset is not
// readable yet. Wide enough to clear minimise/maximise/close at 100% scale.
constexpr double kFallbackCaptionInset = 140.0;

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

namespace winrt::yip::implementation {
MainWindow::MainWindow()
{
    InitializeComponent();

    m_viewModel = winrt::make<winrt::yip::viewmodels::implementation::MainViewModel>();
    m_viewModel.RefreshDevices();
    m_viewModel.RefreshRecordings();

    m_vmToken = m_viewModel.PropertyChanged({this, &MainWindow::OnViewModelPropertyChanged});

    m_lampIdleBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{
        winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x76, 0x7C, 0x8C)};
    m_lampLiveBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{
        winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0xE5, 0x48, 0x4D)};

    Activated({this, &MainWindow::OnActivated});

    SetupTitleBar();
    if (auto appWindow = AppWindow()) {
        appWindow.Resize({kDefaultWindowW, kDefaultWindowH});
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
    if (auto native = try_as<::IWindowNative>()) {
        native->get_WindowHandle(&m_hwnd);
    }
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
    StopMeterPolling();
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
    // Without this the app gets the stock grey title bar and Mica stops at it.
    ExtendsContentIntoTitleBar(true);
    SetTitleBar(AppTitleBar());
    UpdateTitleBarInset();
}

void MainWindow::UpdateTitleBarInset()
{
    double inset = kFallbackCaptionInset;

    auto appWindow = AppWindow();
    if (appWindow) {
        if (auto titleBar = appWindow.TitleBar()) {
            double scale = 1.0;
            if (auto content = Content()) {
                if (auto root = content.XamlRoot()) {
                    scale = root.RasterizationScale();
                }
            }
            if (scale <= 0.0) scale = 1.0;
            // RightInset is in physical pixels; XAML margins are in DIPs.
            const double captionWidth = static_cast<double>(titleBar.RightInset()) / scale;
            if (captionWidth > 0.0) inset = captionWidth + 4.0;
        }
    }
    TitleBarActions().Margin({0.0, 0.0, inset, 0.0});
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
    RecordDot().Visibility(recording ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed
                                     : winrt::Microsoft::UI::Xaml::Visibility::Visible);
    StopSquare().Visibility(recording ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                                      : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    TitleLamp().Fill(recording ? m_lampLiveBrush : m_lampIdleBrush);
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

// ============================================================ Meter

void MainWindow::OnMeterSizeChanged(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                    winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args)
{
    m_meterWidth = args.NewSize().Width;
    m_meterHeight = args.NewSize().Height;
    UpdateMeterVisuals();
}

void MainWindow::UpdateMeterVisuals()
{
    if (!m_viewModel || m_meterWidth <= 0.0) return;

    const double peak = std::clamp(static_cast<double>(m_viewModel.MeterPeak()), 0.0, 1.0);
    const double rms = std::clamp(static_cast<double>(m_viewModel.MeterRms()), 0.0, 1.0);
    const double hold = std::clamp(static_cast<double>(m_viewModel.MeterHold()), 0.0, 1.0);

    const auto h = static_cast<float>(m_meterHeight);
    PeakClip().Rect({0.0f, 0.0f, static_cast<float>(m_meterWidth * peak), h});
    RmsClip().Rect({0.0f, 0.0f, static_cast<float>(m_meterWidth * rms), h});

    const double markWidth = HoldMark().Width();
    const double x =
        std::clamp(m_meterWidth * hold - markWidth * 0.5, 0.0, std::max(0.0, m_meterWidth - markWidth));
    HoldTranslate().X(x);
    HoldMark().Opacity(hold > 0.002 ? 1.0 : 0.0);
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
        StartMeterPolling();
        return;
    }
    StopMeterPolling();
    if (m_viewModel) {
        // One last pull so the meter lands on the post-stop zero instead of
        // freezing at whatever the final tick read.
        m_viewModel.Tick();
        UpdateMeterVisuals();
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
                self->UpdateMeterVisuals();
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
