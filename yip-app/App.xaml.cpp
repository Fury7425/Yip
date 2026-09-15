#include "pch.h"
#include "App.xaml.h"

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
}
} // namespace winrt::yip::implementation
