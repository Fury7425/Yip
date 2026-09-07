#include "pch.h"
#include "ProcessDialog.xaml.h"

#if __has_include("ProcessDialog.g.cpp")
#include "ProcessDialog.g.cpp"
#endif

#include "Settings.h"

#include <microsoft.ui.xaml.window.h>
#include <shobjidl.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Storage.Pickers.h>

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace muxc = winrt::Microsoft::UI::Xaml::Controls;

namespace
{
    HWND ForegroundHwndForProcess()
    {
        HWND fg = ::GetForegroundWindow();
        DWORD pid = 0;
        ::GetWindowThreadProcessId(fg, &pid);
        if (pid == ::GetCurrentProcessId()) return fg;
        HWND result = nullptr;
        struct Ctx { DWORD pid; HWND* out; } ctx{ ::GetCurrentProcessId(), &result };
        ::EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD wpid = 0;
            ::GetWindowThreadProcessId(hwnd, &wpid);
            if (wpid == c->pid && ::IsWindowVisible(hwnd) && ::GetWindow(hwnd, GW_OWNER) == nullptr) {
                *c->out = hwnd;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return result;
    }
}

namespace winrt::yip::implementation
{
    ProcessDialog::ProcessDialog()
    {
        InitializeComponent();
        PrimaryButtonClick({ this, &ProcessDialog::OnRender });
        PopulateRecordings();
    }

    ProcessDialog::~ProcessDialog()
    {
        for (auto& p : m_chain) {
            if (p) ::yip::vst::Unload(*p);
        }
    }

    void ProcessDialog::PopulateRecordings()
    {
        RecordingCombo().Items().Clear();
        const auto folder = ::yip::Settings::Load().output_folder;
        std::error_code ec;
        if (!fs::exists(folder, ec)) return;

        std::vector<fs::path> wavs;
        for (auto const& e : fs::directory_iterator(folder, ec)) {
            if (ec) break;
            if (e.is_regular_file() && e.path().extension() == L".wav") {
                wavs.push_back(e.path());
            }
        }
        std::sort(wavs.begin(), wavs.end(), [](auto& a, auto& b) {
            std::error_code e2;
            return fs::last_write_time(a, e2) > fs::last_write_time(b, e2);
        });
        for (auto const& w : wavs) {
            muxc::ComboBoxItem item;
            item.Content(winrt::box_value(winrt::hstring{ w.filename().wstring() }));
            item.Tag(winrt::box_value(winrt::hstring{ w.wstring() }));
            RecordingCombo().Items().Append(item);
        }
        if (!wavs.empty()) RecordingCombo().SelectedIndex(0);
    }

    winrt::fire_and_forget ProcessDialog::OnAddPlugin(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileOpenPicker picker;
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::ComputerFolder);
        picker.FileTypeFilter().Append(L".vst3");
        // Without the SDK the host accepts any path; allow all so the flow works.
        picker.FileTypeFilter().Append(L"*");

        if (HWND owner = ForegroundHwndForProcess()) {
            picker.as<::IInitializeWithWindow>()->Initialize(owner);
        }

        auto file = co_await picker.PickSingleFileAsync();
        if (!file) co_return;

        const std::wstring path{ file.Path() };
        auto loaded = ::yip::vst::Load(path);
        if (!loaded) {
            strong->SetStatus(L"Failed to load plugin");
            co_return;
        }
        auto shared = std::make_shared<::yip::vst::LoadedPlugin>(std::move(*loaded));
        strong->m_chain.push_back(shared);

        muxc::ListViewItem item;
        item.Content(winrt::box_value(winrt::hstring{ shared->display_name }));
        strong->PluginList().Items().Append(item);
        strong->PluginList().SelectedIndex(static_cast<int32_t>(strong->m_chain.size()) - 1);
        strong->SetStatus(L"");
    }

    void ProcessDialog::OnRemovePlugin(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        const int32_t idx = PluginList().SelectedIndex();
        if (idx < 0 || static_cast<size_t>(idx) >= m_chain.size()) return;
        ::yip::vst::Unload(*m_chain[static_cast<size_t>(idx)]);
        m_chain.erase(m_chain.begin() + idx);
        PluginList().Items().RemoveAt(static_cast<uint32_t>(idx));
        RebuildParamEditors();
    }

    void ProcessDialog::OnPluginSelectionChanged(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        RebuildParamEditors();
    }

    void ProcessDialog::RebuildParamEditors()
    {
        ParamHost().Children().Clear();
        const int32_t idx = PluginList().SelectedIndex();
        if (idx < 0 || static_cast<size_t>(idx) >= m_chain.size()) {
            ParamEmptyHint().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            return;
        }
        ParamEmptyHint().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

        auto plugin = m_chain[static_cast<size_t>(idx)];
        for (size_t i = 0; i < plugin->params.size(); ++i) {
            const auto& info = plugin->params[i];

            muxc::StackPanel row;
            row.Spacing(2);

            muxc::TextBlock label;
            std::wstring caption = info.name;
            if (!info.unit.empty()) caption += L" (" + info.unit + L")";
            label.Text(winrt::hstring{ caption });
            label.FontSize(12);
            label.Opacity(0.8);
            row.Children().Append(label);

            muxc::Slider slider;
            slider.Minimum(0.0);
            slider.Maximum(1.0);
            slider.StepFrequency(0.01);
            slider.Value(plugin->values[i]);
            slider.ValueChanged(
                [plugin, i](winrt::Windows::Foundation::IInspectable const&,
                            winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e) {
                    if (i < plugin->values.size()) plugin->values[i] = e.NewValue();
                });
            row.Children().Append(slider);

            ParamHost().Children().Append(row);
        }
    }

    winrt::fire_and_forget ProcessDialog::OnRender(
        winrt::Microsoft::UI::Xaml::Controls::ContentDialog const&,
        winrt::Microsoft::UI::Xaml::Controls::ContentDialogButtonClickEventArgs const& args)
    {
        auto strong = get_strong();

        // Keep the dialog open while rendering.
        auto deferral = args.GetDeferral();
        args.Cancel(true);

        auto selected = RecordingCombo().SelectedItem().try_as<muxc::ComboBoxItem>();
        if (!selected) {
            SetStatus(L"Pick a source recording first");
            deferral.Complete();
            co_return;
        }
        const std::wstring input{ winrt::unbox_value<winrt::hstring>(selected.Tag()) };
        fs::path out = fs::path(input);
        out.replace_filename(out.stem().wstring() + L"-processed.wav");
        const std::wstring output = out.wstring();

        // Snapshot chain pointers for the worker.
        std::vector<::yip::vst::LoadedPlugin*> chain;
        for (auto& p : strong->m_chain) chain.push_back(p.get());

        auto dq = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        SetStatus(L"Rendering…");
        RenderProgress().Value(0.0);

        co_await winrt::resume_background();

        std::wstring err;
        const bool ok = ::yip::vst::Render(
            input, output, chain,
            [dq, weak = strong->get_weak()](float pr) {
                dq.TryEnqueue([weak, pr]() {
                    if (auto self = weak.get()) self->RenderProgress().Value(pr);
                });
            },
            &err);

        co_await winrt::Microsoft::UI::Dispatching::resume_foreground(dq);

        if (ok) {
            SetStatus(winrt::hstring{ L"Saved: " + out.filename().wstring() });
        } else {
            SetStatus(winrt::hstring{ L"Render failed: " + err });
        }
        deferral.Complete();
    }

    void ProcessDialog::SetStatus(winrt::hstring const& s)
    {
        StatusText().Text(s);
    }
}
