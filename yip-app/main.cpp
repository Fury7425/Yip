// Unpackaged WinUI 3 entry point.
// Self-contained build: the Windows App SDK runtime ships next to the exe, so
// there is no MddBootstrap call (it would prompt to install the framework
// package). Hands straight off to Application::Start.

#include "pch.h"
#include "App.xaml.h"
#include "SingleInstance.h"

namespace winrt {
using namespace winrt::Microsoft::UI::Xaml;
}

int APIENTRY wWinMain(_In_ HINSTANCE /*hInstance*/, _In_opt_ HINSTANCE /*hPrevInstance*/,
                      _In_ LPWSTR /*lpCmdLine*/, _In_ int /*nCmdShow*/)
{
    // One Yip per session. Claimed before anything else runs, so a second
    // launch never reaches WASAPI, RegisterHotKey or the notification area.
    const ::yip::SingleInstance instance;
    if (!instance.IsPrimary()) {
        ::yip::ActivateRunningInstance();
        return 0;
    }

    // Smoke-test: confirm the native audio lib is linked and reachable.
    const int32_t audioProbe = rec_dummy();
    wchar_t buf[64];
    swprintf_s(buf, L"Yip: audio_core dummy=%d\n", audioProbe);
    OutputDebugStringW(buf);

    {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        winrt::Microsoft::UI::Xaml::Application::Start(
            [](auto&&) { winrt::make<::winrt::yip::implementation::App>(); });
    }
    return 0;
}
