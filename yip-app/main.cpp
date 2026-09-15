// Unpackaged WinUI 3 entry point.
// Self-contained build: the Windows App SDK runtime ships next to the exe, so
// there is no MddBootstrap call (it would prompt to install the framework
// package). Hands straight off to Application::Start.

#include "pch.h"
#include "App.xaml.h"

namespace winrt {
using namespace winrt::Microsoft::UI::Xaml;
}

int APIENTRY wWinMain(_In_ HINSTANCE /*hInstance*/, _In_opt_ HINSTANCE /*hPrevInstance*/,
                      _In_ LPWSTR /*lpCmdLine*/, _In_ int /*nCmdShow*/)
{
    // Smoke-tests: confirm both native libs are linked and reachable.
    const int32_t audioProbe = rec_dummy();
    const int32_t vstProbe = yip_vst_dummy();
    wchar_t buf[128];
    swprintf_s(buf, L"Yip: audio_core dummy=%d, vst_host dummy=%d\n", audioProbe, vstProbe);
    OutputDebugStringW(buf);

    {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        winrt::Microsoft::UI::Xaml::Application::Start(
            [](auto&&) { winrt::make<::winrt::yip::implementation::App>(); });
    }
    return 0;
}
