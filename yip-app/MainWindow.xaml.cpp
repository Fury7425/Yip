#include "pch.h"
#include "MainWindow.xaml.h"

#if __has_include("MainWindow.xaml.g.cpp")
#include "MainWindow.xaml.g.cpp"
#endif

#include "AudioCoreInterop.h"
#include "SettingsDialog.xaml.h"

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

    StartPolling();
}

MainWindow::~MainWindow()
{
    StopPolling();
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
    // Throttle meter poll: 60 Hz focused → 10 Hz blurred (spec).
    if (m_pollTimer) {
        m_pollTimer.Interval(m_focused ? std::chrono::milliseconds(16) : std::chrono::milliseconds(100));
    }
}

void MainWindow::StartPolling()
{
    auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    m_pollTimer = queue.CreateTimer();
    m_pollTimer.Interval(std::chrono::milliseconds(16));
    m_pollTimer.IsRepeating(true);
    m_pollTimer.Tick([weak = get_weak()](auto&&, auto&&) {
        if (auto self = weak.get()) {
            if (self->m_viewModel) {
                self->m_viewModel.PollPeak();
            }
        }
    });
    m_pollTimer.Start();
}

void MainWindow::StopPolling()
{
    if (m_pollTimer) {
        m_pollTimer.Stop();
        m_pollTimer = nullptr;
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

    // ContentDialog needs an XamlRoot in WinAppSDK.
    dialog.XamlRoot(strong->Content().XamlRoot());

    const auto result = co_await dialog.ShowAsync();
    if (result == winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary) {
        strong->m_viewModel.ApplySettings(dialog.OutputFolder(), dialog.SampleRate(), dialog.Channels());
    }
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
