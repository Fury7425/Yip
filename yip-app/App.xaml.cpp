#include "pch.h"
#include "App.xaml.h"

#include "MainWindow.xaml.h"
#include "IndicatorWindow.xaml.h"
#include "TrayIcon.h"

#include <microsoft.ui.xaml.window.h>

#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>

namespace winrt {
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Composition::SystemBackdrops;
} // namespace winrt

namespace muw = winrt::Microsoft::UI::Windowing;

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
    m_window = winrt::make<winrt::yip::implementation::MainWindow>();
    m_window.Title(L"Yip");

    // The acrylic backdrop is owned by MainWindow (SetupBackdrop), which keeps
    // it lit while the window is inactive.
    m_window.Activate();

    // Floating indicator pill — always-on-top tool window. Owns its own
    // poll loop against the audio-core FFI; survives without MainWindow
    // being focused.
    //
    // Per current design the pill is *invisible* in Idle + Armed states,
    // so we do NOT call Activate() here — visibility is driven entirely
    // by AppWindow.Show()/Hide() in TransitionTo. Window construction is
    // enough to wire composition + the dispatcher queue.
    m_indicator = winrt::make<winrt::yip::implementation::IndicatorWindow>();

    SetupTray();
    WireCloseToTray();
}

App::~App()
{
    TeardownTray();
}

// ============================================================ Notification area

void App::SetupTray()
{
    ::yip::TrayIcon::Callbacks callbacks;
    callbacks.onShow = [this]() { ShowMainWindow(); };
    callbacks.onToggleRecording = [this]() { ToggleRecording(); };
    callbacks.onExit = [this]() { Quit(false); };
    m_tray = std::make_unique<::yip::TrayIcon>(std::move(callbacks));

    // The glyph follows capture state. Pushed from audio-core rather than
    // polled, so an idle Yip still ticks nothing.
    auto dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    m_trayStateToken = ::yip::RecordingStateBus::Subscribe(dispatcher, [this](bool recording) {
        if (m_tray) m_tray->SetRecording(recording);
    });
    m_tray->SetRecording(::yip::RecordingStateBus::IsRecording());
}

void App::TeardownTray()
{
    ::yip::RecordingStateBus::Unsubscribe(m_trayStateToken);
    m_trayStateToken = 0;
    m_tray.reset();
}

// ============================================================ Window lifetime

void App::WireCloseToTray()
{
    if (!m_window) return;
    auto appWindow = m_window.AppWindow();
    if (!appWindow) return;

    m_closingToken = appWindow.Closing(
        [this](muw::AppWindow const&, muw::AppWindowClosingEventArgs const& args) {
            if (m_quitting) return;

            // Without a registered icon there would be nothing left to click:
            // hiding then would strand the process exactly as it used to, alive
            // with no window and no way back. So that close is a real quit.
            if (!m_tray || !m_tray->IsLive()) {
                Quit(true);
                return;
            }

            args.Cancel(true);
            HideMainWindow();
        });
}

void App::HideMainWindow()
{
    if (!m_window) return;
    if (auto appWindow = m_window.AppWindow()) {
        appWindow.Hide();
    }
    // Windows 11 files a new notification icon into the overflow flyout rather
    // than onto the taskbar, and no API promotes it out of there. The balloon is
    // what points at the chevron the first time the window disappears.
    if (m_tray && !m_hintShown) {
        m_tray->ShowHint(L"Yip is still running",
                         L"The window is closed, not the app - recording and the hotkey carry on. "
                         L"Click this icon to bring the window back, or right-click it for Exit.");
        m_hintShown = true;
    }
}

void App::ShowMainWindow()
{
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

    // A take in flight is finalised rather than truncated. MainWindow's
    // destructor does this too, but Application::Exit is not obliged to run it.
    if (::rec_is_recording()) {
        (void)::rec_stop();
    }

    TeardownTray();

    if (m_indicator) {
        m_indicator.Close();
        m_indicator = nullptr;
    }
    // Closing the main window used to leave the process alive behind it — the
    // XAML application does not end with it — so the exit is explicit.
    if (!windowAlreadyClosing && m_window) {
        m_window.Close();
    }
    Exit();
}

void App::ToggleRecording()
{
    if (!m_window) return;
    if (auto window = m_window.try_as<winrt::yip::MainWindow>()) {
        if (auto viewModel = window.ViewModel()) {
            viewModel.ToggleRecording();
        }
    }
}
} // namespace winrt::yip::implementation
