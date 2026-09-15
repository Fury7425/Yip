#pragma once

// App is not a runtimeclass (no App.idl): the XAML compiler owns AppT, and
// main.cpp instantiates it with make<>. Adding an idl makes cppwinrt emit a
// projected constructor that cannot be built from the XAML base.
#include "App.xaml.g.h"

namespace winrt::yip::implementation {
struct App : AppT<App> {
    App();

    void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

private:
    winrt::Microsoft::UI::Xaml::Window m_window{nullptr};
    winrt::Microsoft::UI::Xaml::Window m_indicator{nullptr};
};
} // namespace winrt::yip::implementation
