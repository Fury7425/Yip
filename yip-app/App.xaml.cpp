#include "pch.h"
#include "App.xaml.h"

#include "AudioCoreInterop.h"
#include "HotkeyManager.h"
#include "MainWindow.xaml.h"
#include "IndicatorWindow.xaml.h"
#include "TrayIcon.h"

#include <DispatcherQueue.h>
#include <microsoft.ui.xaml.window.h>

#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Windowing.h>

#include <utility>

namespace winrt {
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Composition::SystemBackdrops;
} // namespace winrt

namespace muw = winrt::Microsoft::UI::Windowing;
namespace muxd = winrt::Microsoft::UI::Dispatching;

namespace {
// Long enough for the pill to finish its Saving hold and fade-out after a take
// ends, and for XAML to let go of a closed window before the working set is
// trimmed.
constexpr auto kIdleDelay = std::chrono::milliseconds(2000);
} // namespace

namespace winrt::yip::implementation {
App::App()
{
    InitializeComponent();

    UnhandledException(
        [](IInspectable const&, winrt::Microsoft::UI::Xaml::UnhandledExceptionEventArgs const& e) {
            const auto msg = e.Message();
            ::OutputDebugStringW(L"Yip unhandled: ");
            ::OutputDebugStringW(msg.c_str());
            ::OutputDebugStringW(L"\n");
        });
}

void App::OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const& /*args*/)
{
    // By default the XAML dispatcher shuts down, and the process with it, when
    // the last window closes. Yip closes its windows while it keeps running in
    // the tray, so only Quit() ends it.
    winrt::Microsoft::UI::Xaml::Application::Current().DispatcherShutdownMode(
        winrt::Microsoft::UI::Xaml::DispatcherShutdownMode::OnExplicitShutdown);

    EnsureSystemDispatcherQueue();

    auto dispatcher = muxd::DispatcherQueue::GetForCurrentThread();
    m_idleTimer = dispatcher.CreateTimer();
    m_idleTimer.Interval(kIdleDelay);
    m_idleTimer.IsRepeating(false);
    m_idleTimer.Tick([this](auto&&, auto&&) { OnIdle(); });

    m_viewModel = winrt::make<winrt::yip::viewmodels::implementation::MainViewModel>();
    m_vmToken = m_viewModel.PropertyChanged({this, &App::OnViewModelPropertyChanged});

    // Live device updates via IMMNotificationClient. The callback fires on a
    // WASAPI worker thread, so it is marshalled here before touching the view
    // model. Kept windowless, so a hotkey take never aims at a device that
    // has gone.
    m_deviceWatcher = std::make_unique<::yip::interop::DeviceWatcher>([this, dispatcher]() {
        dispatcher.TryEnqueue([this]() {
            if (m_viewModel && !m_quitting) m_viewModel.RefreshDevices();
        });
    });

    // Subscribed before any window, so the view model is reconciled before a
    // window reacts to the same event.
    m_stateToken = ::yip::RecordingStateBus::Subscribe(dispatcher, [this](bool recording) {
        OnRecordingStateChanged(recording);
    });

    SetupTray();
    SetupHotkey();

    CreateMainWindow();
    m_window.Activate();
}

App::~App()
{
    m_hotkey.reset();
    TeardownTray();
}

void App::EnsureSystemDispatcherQueue()
{
    if (winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) return;
    DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
    // A failure leaves each window to try on its own, as it always did.
    (void)::CreateDispatcherQueueController(
        options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(
                     winrt::put_abi(m_systemQueue)));
}

// ============================================================ Notification area

void App::SetupTray()
{
    ::yip::TrayIcon::Callbacks callbacks;
    callbacks.onShow = [this]() { ShowMainWindow(); };
    callbacks.onToggleRecording = [this]() { ToggleRecording(); };
    callbacks.onExit = [this]() { Quit(false); };
    m_tray = std::make_unique<::yip::TrayIcon>(std::move(callbacks));
    m_tray->SetRecording(::yip::RecordingStateBus::IsRecording());
}

void App::TeardownTray()
{
    m_tray.reset();
}

// ============================================================ Hotkey

void App::SetupHotkey()
{
    // WM_HOTKEY needs a window that is always there. The tray's hidden host is
    // exactly that; the main window no longer is.
    if (!m_tray || !m_tray->HostWindow()) return;
    m_hotkey = std::make_unique<::yip::HotkeyManager>(m_tray->HostWindow(), [this]() { ToggleRecording(); });
    ApplyHotkey();
}

void App::ApplyHotkey()
{
    if (!m_hotkey || !m_viewModel) return;
    if (!m_hotkey->Register(m_viewModel.HotkeyMods(), m_viewModel.HotkeyVk())) {
        m_viewModel.ReportHotkeyConflict();
    }
}

void App::OnViewModelPropertyChanged(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                     winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const& args)
{
    // ApplySettings raises HotkeyMods then HotkeyVk; re-registering once, on
    // the second, sees both. Register() drops the old combo first.
    if (args.PropertyName() == L"HotkeyVk") ApplyHotkey();
}

// ============================================================ Capture state

void App::OnRecordingStateChanged(bool recording)
{
    if (m_quitting) return;
    if (m_tray) m_tray->SetRecording(recording);
    // Also where a take that failed on its own is finalised and reported.
    if (m_viewModel) m_viewModel.SyncRecordingState(recording);

    if (recording) {
        if (m_idleTimer) m_idleTimer.Stop();
        EnsureIndicator();
        return;
    }
    RestartIdleTimer();
}

void App::EnsureIndicator()
{
    if (m_indicator || m_quitting) return;
    // Its constructor reads the live state and transitions straight into
    // Recording; it never needs Activate(), since visibility is driven by
    // AppWindow.Show()/Hide() inside it.
    m_indicator = winrt::make<winrt::yip::implementation::IndicatorWindow>();
}

void App::RestartIdleTimer()
{
    if (!m_idleTimer || m_quitting) return;
    m_idleTimer.Stop();
    m_idleTimer.Start();
}

void App::OnIdle()
{
    if (m_quitting || ::rec_is_recording()) return;
    if (m_indicator) {
        // Faded out by now. Closing it gives back its XAML tree, composition
        // device and backdrop; the next take builds a fresh one.
        m_indicator.Close();
        m_indicator = nullptr;
        // Trim on the next pass, once XAML has actually released it.
        RestartIdleTimer();
        return;
    }
    if (!m_window) TrimWorkingSet();
}

void App::TrimWorkingSet()
{
    // Only with nothing running: pages trimmed here come back as faults, and
    // neither the capture nor the render thread should be the one taking them.
    if (::rec_is_recording() || ::play_is_playing()) return;
    // Hands the pages XAML no longer touches back to the system. They are
    // faulted back in on demand if a window opens again.
    (void)::SetProcessWorkingSetSize(::GetCurrentProcess(), static_cast<SIZE_T>(-1),
                                     static_cast<SIZE_T>(-1));
}

// ============================================================ Window lifetime

void App::CreateMainWindow()
{
    auto window = winrt::make<winrt::yip::implementation::MainWindow>(m_viewModel);
    window.Title(L"Yip");
    m_window = window;
    if (m_windowRect) {
        if (auto appWindow = m_window.AppWindow()) appWindow.MoveAndResize(*m_windowRect);
    }
    WireMainWindow();
}

void App::WireMainWindow()
{
    if (!m_window) return;
    auto appWindow = m_window.AppWindow();
    if (!appWindow) return;

    appWindow.Closing([this](muw::AppWindow const& sender, muw::AppWindowClosingEventArgs const& /*args*/) {
        if (m_quitting) return;

        // Without a registered icon there would be nothing left to click:
        // closing then would strand the process exactly as it used to, alive
        // with no window and no way back. So that close is a real quit.
        if (!m_tray || !m_tray->IsLive()) {
            Quit(true);
            return;
        }

        // Remember a restored window's place for the next one.
        auto presenter = sender.Presenter().try_as<muw::OverlappedPresenter>();
        if (!presenter || presenter.State() == muw::OverlappedPresenterState::Restored) {
            const auto pos = sender.Position();
            const auto size = sender.Size();
            m_windowRect = winrt::Windows::Graphics::RectInt32{pos.X, pos.Y, size.Width, size.Height};
        }

        // A take being recorded carries on without the window — that is the
        // point of closing to the tray. Playback does not: it is something you
        // are listening to in the window, and it stops with it. The view model
        // keeps the take and position, so the next window comes back as this
        // one left.
        if (m_viewModel) m_viewModel.SuspendPlayback();

        // Windows 11 files a new notification icon into the overflow flyout
        // rather than onto the taskbar, and no API promotes it out of there.
        // The balloon is what points at the chevron the first time the window
        // disappears.
        if (m_tray && !m_hintShown) {
            m_tray->ShowHint(L"Yip is still running",
                             L"The window is closed, not the app - recording and the hotkey carry on. "
                             L"Click this icon to bring the window back, or right-click it for Exit.");
            m_hintShown = true;
        }
    });

    m_window.Closed([this](auto&&, auto&&) {
        // Not released inside the window's own event: the last reference goes
        // on the next turn of the queue, once the event has unwound.
        auto closing = std::exchange(m_window, nullptr);
        muxd::DispatcherQueue::GetForCurrentThread().TryEnqueue([closing]() {});
        RestartIdleTimer();
    });
}

void App::ShowMainWindow()
{
    if (m_quitting) return;
    if (!m_window) CreateMainWindow();
    if (!m_window) return;

    // AppWindow::Show is what undoes Hide; ShowWindow alone leaves the window
    // out of the taskbar and Alt-Tab.
    if (auto appWindow = m_window.AppWindow()) {
        appWindow.Show();
    }

    HWND hwnd = nullptr;
    if (auto native = m_window.try_as<::IWindowNative>()) {
        native->get_WindowHandle(&hwnd);
    }
    if (!hwnd) {
        m_window.Activate();
        return;
    }

    if (::IsIconic(hwnd)) {
        ::ShowWindow(hwnd, SW_RESTORE);
    }
    m_window.Activate();
    ::SetForegroundWindow(hwnd);
}

void App::Quit(bool windowAlreadyClosing)
{
    if (m_quitting) return;
    m_quitting = true;

    // A take in flight is finalised rather than truncated.
    if (::rec_is_recording()) {
        (void)::rec_stop();
    }
    // And playback lets go of its file, for the same reason.
    (void)::play_stop();

    if (m_idleTimer) m_idleTimer.Stop();
    ::yip::RecordingStateBus::Unsubscribe(m_stateToken);
    m_stateToken = 0;
    if (m_viewModel && m_vmToken) {
        m_viewModel.PropertyChanged(m_vmToken);
        m_vmToken = {};
    }
    m_deviceWatcher.reset();
    // The hotkey is subclassed onto the tray's host window: it goes first.
    m_hotkey.reset();
    TeardownTray();

    if (m_indicator) {
        m_indicator.Close();
        m_indicator = nullptr;
    }
    // With the dispatcher on explicit shutdown, closing windows ends nothing
    // by itself; Exit() below is the exit.
    if (!windowAlreadyClosing && m_window) {
        m_window.Close();
    }
    Exit();
}

void App::ToggleRecording()
{
    if (m_viewModel && !m_quitting) m_viewModel.ToggleRecording();
}
} // namespace winrt::yip::implementation
