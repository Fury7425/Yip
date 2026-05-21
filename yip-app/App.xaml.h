#pragma once

#include "App.xaml.g.h"

namespace winrt::yip::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

    private:
        winrt::Microsoft::UI::Xaml::Window m_window{ nullptr };
        winrt::Microsoft::UI::Xaml::Window m_indicator{ nullptr };
    };
}
