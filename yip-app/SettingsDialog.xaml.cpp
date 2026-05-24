#include "pch.h"
#include "SettingsDialog.xaml.h"

#if __has_include("SettingsDialog.g.cpp")
#include "SettingsDialog.g.cpp"
#endif

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>

#include <shobjidl.h>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace {
HWND TryFindForegroundHwnd()
{
    // ContentDialog runs against the active XamlRoot, but FolderPicker
    // needs the *Win32 owner* HWND for COM init. The dialog's XamlRoot
    // doesn't directly expose it — use the foreground top-level window
    // belonging to this process as a pragmatic anchor.
    HWND fg = ::GetForegroundWindow();
    DWORD pid = 0;
    ::GetWindowThreadProcessId(fg, &pid);
    if (pid == ::GetCurrentProcessId()) return fg;

    // Fall back to enumerating top-level windows of this process.
    HWND result = nullptr;
    struct Ctx {
        DWORD pid;
        HWND* out;
    };
    Ctx ctx{::GetCurrentProcessId(), &result};
    ::EnumWindows(
        [](HWND hwnd, LPARAM lparam) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lparam);
            DWORD wpid = 0;
            ::GetWindowThreadProcessId(hwnd, &wpid);
            if (wpid == c->pid && ::IsWindowVisible(hwnd) && ::GetWindow(hwnd, GW_OWNER) == nullptr) {
                *c->out = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return result;
}
} // namespace

namespace winrt::yip::implementation {
SettingsDialog::SettingsDialog()
{
    InitializeComponent();
    ApplyToControls();
}

winrt::hstring SettingsDialog::OutputFolder() const noexcept
{
    ReadFromControls();
    return m_outputFolder;
}
void SettingsDialog::OutputFolder(winrt::hstring const& v)
{
    m_outputFolder = v;
    ApplyToControls();
}

uint32_t SettingsDialog::SampleRate() const noexcept
{
    ReadFromControls();
    return m_sampleRate;
}
void SettingsDialog::SampleRate(uint32_t v)
{
    m_sampleRate = v;
    ApplyToControls();
}

uint16_t SettingsDialog::Channels() const noexcept
{
    ReadFromControls();
    return m_channels;
}
void SettingsDialog::Channels(uint16_t v)
{
    m_channels = v;
    ApplyToControls();
}

void SettingsDialog::ApplyToControls()
{
    if (FolderBox()) {
        FolderBox().Text(m_outputFolder);
    }
    if (SampleRateCombo()) {
        int idx = 1; // default 48000
        switch (m_sampleRate) {
            case 44100:
                idx = 0;
                break;
            case 48000:
                idx = 1;
                break;
            case 88200:
                idx = 2;
                break;
            case 96000:
                idx = 3;
                break;
            default:
                break;
        }
        SampleRateCombo().SelectedIndex(idx);
    }
    if (ChannelsCombo()) {
        ChannelsCombo().SelectedIndex(m_channels == 1 ? 0 : 1);
    }
}

void SettingsDialog::ReadFromControls() const
{
    // SettingsDialog is the implementation type; cast away const for the
    // mutable cached fields below.
    auto* self = const_cast<SettingsDialog*>(this);
    if (FolderBox()) {
        self->m_outputFolder = FolderBox().Text();
    }
    if (SampleRateCombo()) {
        switch (SampleRateCombo().SelectedIndex()) {
            case 0:
                self->m_sampleRate = 44100;
                break;
            case 1:
                self->m_sampleRate = 48000;
                break;
            case 2:
                self->m_sampleRate = 88200;
                break;
            case 3:
                self->m_sampleRate = 96000;
                break;
            default:
                break;
        }
    }
    if (ChannelsCombo()) {
        self->m_channels = (ChannelsCombo().SelectedIndex() == 0) ? 1 : 2;
    }
}

winrt::fire_and_forget SettingsDialog::OnPickFolder(
    winrt::Windows::Foundation::IInspectable const& /*sender*/,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
{
    auto strong = get_strong();

    winrt::Windows::Storage::Pickers::FolderPicker picker;
    picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::MusicLibrary);
    picker.FileTypeFilter().Append(L"*");

    // Unpackaged WinUI 3: pickers need an HWND init via IInitializeWithWindow.
    HWND owner = TryFindForegroundHwnd();
    if (owner) {
        auto init = picker.as<::IInitializeWithWindow>();
        init->Initialize(owner);
    }

    auto folder = co_await picker.PickSingleFolderAsync();
    if (folder) {
        strong->FolderBox().Text(folder.Path());
    }
    co_return;
}
} // namespace winrt::yip::implementation
