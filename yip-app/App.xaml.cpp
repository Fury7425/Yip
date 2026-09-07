#include "pch.h"
#include "App.xaml.h"

#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif

#include "MainWindow.xaml.h"
#include "IndicatorWindow.xaml.h"

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

    // Mica backdrop — Window owns the controller lifetime since 1.4.
    if (winrt::Microsoft::UI::Composition::SystemBackdrops::MicaController::IsSupported()) {
        m_window.SystemBackdrop(winrt::Microsoft::UI::Xaml::Media::MicaBackdrop{});
    } else {
        // Older hardware → acrylic fallback.
        m_window.SystemBackdrop(winrt::Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop{});
    }

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
}
} // namespace winrt::yip::implementation
