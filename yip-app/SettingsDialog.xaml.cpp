#include "pch.h"
#include "SettingsDialog.xaml.h"

#if __has_include("SettingsDialog.g.cpp")
#include "SettingsDialog.g.cpp"
#endif

#include "HotkeyManager.h"

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
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

void SettingsDialog::HotkeyMods(uint32_t v)
{
    m_hotkeyMods = v;
    ApplyHotkeyToControls();
}

void SettingsDialog::HotkeyVk(uint32_t v)
{
    m_hotkeyVk = v;
    ApplyHotkeyToControls();
}

void SettingsDialog::ApplyHotkeyToControls()
{
    if (HotkeyBox()) {
        HotkeyBox().Text(winrt::hstring{::yip::FormatHotkey(m_hotkeyMods, m_hotkeyVk)});
    }
}

void SettingsDialog::OnHotkeyKeyDown(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                     winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
{
    const auto vk = static_cast<uint32_t>(args.Key());

    // Let the dialog keep its own keys when they arrive unmodified, so the box
    // never traps the user: Esc still cancels and Tab still moves focus.
    uint32_t mods = 0;
    if (::GetKeyState(VK_CONTROL) < 0) mods |= ::yip::kModControl;
    if (::GetKeyState(VK_MENU) < 0) mods |= ::yip::kModAlt;
    if (::GetKeyState(VK_SHIFT) < 0) mods |= ::yip::kModShift;
    if (::GetKeyState(VK_LWIN) < 0 || ::GetKeyState(VK_RWIN) < 0) mods |= ::yip::kModWin;

    if (mods == 0) return;

    // A modifier arriving on its own is the first half of a combo, not a combo.
    if (!::yip::IsValidHotkey(mods, vk)) {
        args.Handled(true);
        return;
    }

    m_hotkeyMods = mods;
    m_hotkeyVk = vk;
    ApplyHotkeyToControls();
    args.Handled(true);
}

void SettingsDialog::OnResetHotkey(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                   winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
{
    m_hotkeyMods = ::yip::kModControl | ::yip::kModAlt;
    m_hotkeyVk = 0x52; // 'R'
    ApplyHotkeyToControls();
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
    ApplyHotkeyToControls();
}

void SettingsDialog::ReadFromControls() const
{
    // SettingsDialog is the implementation type; cast away const for the
    // mutable cached fields below.
    auto* self = const_cast<SettingsDialog*>(this);
    if (self->FolderBox()) {
        self->m_outputFolder = self->FolderBox().Text();
    }
    if (self->SampleRateCombo()) {
        switch (self->SampleRateCombo().SelectedIndex()) {
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
    if (self->ChannelsCombo()) {
        self->m_channels = (self->ChannelsCombo().SelectedIndex() == 0) ? 1 : 2;
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
