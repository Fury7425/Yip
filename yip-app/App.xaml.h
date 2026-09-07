#pragma once

// App.g.h carries the factory_implementation base; App.xaml.g.h carries the
// XAML one. MainWindow needs only the former, but App is declared in both.
#include "App.g.h"
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

namespace winrt::yip::factory_implementation {
struct App : AppT<App, implementation::App> {};
} // namespace winrt::yip::factory_implementation
