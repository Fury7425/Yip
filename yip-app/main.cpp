// Unpackaged WinUI 3 entry point.
// Bootstraps the Windows App SDK runtime, then hands off to Application::Start.

#include "pch.h"
#include "App.xaml.h"

namespace winrt {
using namespace winrt::Microsoft::UI::Xaml;
}

int APIENTRY wWinMain(_In_ HINSTANCE /*hInstance*/, _In_opt_ HINSTANCE /*hPrevInstance*/,
                      _In_ LPWSTR /*lpCmdLine*/, _In_ int /*nCmdShow*/)
{

    // Locate the matching Windows App SDK runtime (1.6.x). Required for
    // unpackaged apps so framework DLLs resolve without an MSIX identity.
    constexpr PCWSTR kVersionTag = L"";
    constexpr PACKAGE_VERSION kMinVersion{};
    const HRESULT bootstrapHr =
        MddBootstrapInitialize2(WINDOWSAPPSDK_RELEASE_MAJORMINOR, kVersionTag, kMinVersion,
                                MddBootstrapInitializeOptions_OnNoMatch_ShowUI |
                                    MddBootstrapInitializeOptions_OnPackageIdentity_NOOP);
    if (FAILED(bootstrapHr)) {
        OutputDebugStringW(L"Yip: MddBootstrapInitialize2 failed.\n");
        return bootstrapHr;
    }

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

    MddBootstrapShutdown();
    return 0;
}
