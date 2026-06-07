#pragma once

#include "ProcessDialog.g.h"
#include "VstInterop.h"

#include <memory>
#include <vector>

namespace winrt::yip::implementation
{
    struct ProcessDialog : ProcessDialogT<ProcessDialog>
    {
        ProcessDialog();
        ~ProcessDialog();

        winrt::fire_and_forget OnAddPlugin(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnRemovePlugin(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnPluginSelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        winrt::fire_and_forget OnRender(
            winrt::Microsoft::UI::Xaml::Controls::ContentDialog const& sender,
            winrt::Microsoft::UI::Xaml::Controls::ContentDialogButtonClickEventArgs const& args);

    private:
        void PopulateRecordings();
        void RebuildParamEditors();
        void SetStatus(winrt::hstring const& s);

        std::vector<std::shared_ptr<::yip::vst::LoadedPlugin>> m_chain;
    };
}

namespace winrt::yip::factory_implementation
{
    struct ProcessDialog : ProcessDialogT<ProcessDialog, implementation::ProcessDialog> {};
}
