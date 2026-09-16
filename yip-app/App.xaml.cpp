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
    callbacks.onExit = [this]() {
        // Same door as the title bar's close button: MainWindow's destructor
        // stops any take in flight.
        if (m_window) m_window.Close();
    };
    m_tray = std::make_unique<::yip::TrayIcon>(std::move(callbacks));

    // The glyph follows capture state. Pushed from audio-core rather than
    // polled, so an idle Yip still ticks nothing.
    auto dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    m_trayStateToken = ::yip::RecordingStateBus::Subscribe(dispatcher, [this](bool recording) {
        if (m_tray) m_tray->SetRecording(recording);
    });
    m_tray->SetRecording(::yip::RecordingStateBus::IsRecording());

    // The icon outlives nothing: once the main window is gone the process is on
    // its way out, and an icon left registered is a dead slot in the tray until
    // the shell next sweeps it.
    if (m_window) {
        m_window.Closed([this](auto&&, auto&&) { TeardownTray(); });
    }
}

void App::TeardownTray()
{
    ::yip::RecordingStateBus::Unsubscribe(m_trayStateToken);
    m_trayStateToken = 0;
    m_tray.reset();
}

void App::ShowMainWindow()
{
    if (!m_window) return;

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
    } else if (!::IsWindowVisible(hwnd)) {
        ::ShowWindow(hwnd, SW_SHOW);
    }
    ::SetForegroundWindow(hwnd);
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
