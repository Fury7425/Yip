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

#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

using namespace std::chrono_literals;

namespace winrt {
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Dispatching;
using namespace winrt::Windows::Foundation;
} // namespace winrt

namespace winrt::yip::implementation {
MainWindow::MainWindow()
{
    InitializeComponent();
    m_viewModel = winrt::make<winrt::yip::viewmodels::implementation::MainViewModel>();
    m_viewModel.RefreshDevices();
    m_viewModel.RefreshRecordings();

    Activated({this, &MainWindow::OnActivated});

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

void MainWindow::OnActivated(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                             winrt::Microsoft::UI::Xaml::WindowActivatedEventArgs const& args)
{
    const bool now_focused =
        args.WindowActivationState() != winrt::Microsoft::UI::Xaml::WindowActivationState::Deactivated;
    if (now_focused == m_focused) return;
    m_focused = now_focused;
    // Throttle meter poll: 60 Hz focused → 10 Hz blurred (spec). The timer only
    // exists while recording, so this is a no-op when idle.
    if (m_meterTimer) {
        m_meterTimer.Interval(m_focused ? std::chrono::milliseconds(16) : std::chrono::milliseconds(100));
    }
}

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
        m_viewModel.PollPeak();
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
                self->m_viewModel.PollPeak();
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
    strong->m_viewModel.RefreshRecordings();  // pick up *-processed.wav
    co_return;
}

void MainWindow::OnRefreshList(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                               winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
{
    m_viewModel.RefreshDevices();
    m_viewModel.RefreshRecordings();
}

void MainWindow::OnRecordingClicked(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                    winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& args)
{
    if (auto entry = args.ClickedItem().try_as<winrt::yip::viewmodels::RecordingEntry>()) {
        m_viewModel.RevealRecording(entry);
    }
}
} // namespace winrt::yip::implementation
