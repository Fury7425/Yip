#include "pch.h"
#include "SettingsDialog.xaml.h"

#if __has_include("SettingsDialog.g.cpp")
#include "SettingsDialog.g.cpp"
#endif

#include "HotkeyManager.h"
#include "RecordingFormat.h"

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Storage.Pickers.h>

#include <shobjidl.h>

#include <algorithm>
#include <cwchar>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace audiofmt = ::yip::audiofmt;

namespace {
// Numeric Tag of a ComboBoxItem ("24", "192"), or 0 when it has none.
uint32_t TagValue(winrt::Windows::Foundation::IInspectable const& item)
{
    const auto cbi = item ? item.try_as<ComboBoxItem>() : nullptr;
    if (!cbi) return 0;
    const auto text = winrt::unbox_value_or<winrt::hstring>(cbi.Tag(), L"");
    return static_cast<uint32_t>(std::wcstoul(text.c_str(), nullptr, 10));
}

int32_t IndexOfTag(ComboBox const& combo, uint32_t tag)
{
    const auto items = combo.Items();
    for (uint32_t i = 0; i < items.Size(); ++i) {
        if (TagValue(items.GetAt(i)) == tag) return static_cast<int32_t>(i);
    }
    return -1;
}

// Enable each item `allowed` accepts. If that strands the selection, move it
// to `fallback`.
template <class Allowed>
void RestrictItems(ComboBox const& combo, Allowed allowed, uint32_t fallback)
{
    const auto items = combo.Items();
    for (uint32_t i = 0; i < items.Size(); ++i) {
        if (const auto cbi = items.GetAt(i).try_as<ComboBoxItem>()) {
            cbi.IsEnabled(allowed(TagValue(cbi)));
        }
    }
    const auto selected = combo.SelectedItem();
    if (!selected || !allowed(TagValue(selected))) {
        combo.SelectedIndex(std::max(0, IndexOfTag(combo, fallback)));
    }
}

const wchar_t* FormatHintText(uint16_t format)
{
    switch (format) {
        case audiofmt::kFlac:
            return L"Lossless, and about half the size of WAV.";
        case audiofmt::kMp3:
            return L"Lossy. Plays anywhere. Records at 44.1 or 48 kHz.";
        case audiofmt::kM4a:
            return L"Lossy, smaller than MP3 at the same quality. Records at 44.1 or 48 kHz.";
        default:
            return L"Uncompressed. The largest files, and no quality loss.";
    }
}

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

uint16_t SettingsDialog::Format() const noexcept
{
    ReadFromControls();
    return m_format;
}
void SettingsDialog::Format(uint16_t v)
{
    m_format = v;
    ApplyToControls();
}

uint16_t SettingsDialog::BitDepth() const noexcept
{
    ReadFromControls();
    return m_bitDepth;
}
void SettingsDialog::BitDepth(uint16_t v)
{
    m_bitDepth = v;
    ApplyToControls();
}

uint16_t SettingsDialog::BitrateKbps() const noexcept
{
    ReadFromControls();
    return m_bitrateKbps;
}
void SettingsDialog::BitrateKbps(uint16_t v)
{
    m_bitrateKbps = v;
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

bool SettingsDialog::PillDot() const noexcept
{
    ReadFromControls();
    return m_pillDot;
}
void SettingsDialog::PillDot(bool v)
{
    m_pillDot = v;
    ApplyToControls();
}

bool SettingsDialog::PillBottom() const noexcept
{
    ReadFromControls();
    return m_pillBottom;
}
void SettingsDialog::PillBottom(bool v)
{
    m_pillBottom = v;
    ApplyToControls();
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

void SettingsDialog::OnFormatChanged(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                     SelectionChangedEventArgs const& /*args*/)
{
    SyncFormatControls();
}

void SettingsDialog::SyncFormatControls()
{
    // SelectionChanged can fire from InitializeComponent before every named
    // control is connected.
    if (!FormatCombo() || !BitDepthCombo() || !BitrateCombo() || !SampleRateCombo() || !FormatHint() ||
        !QualityLabel()) {
        return;
    }

    const auto format = static_cast<uint16_t>(std::max(0, FormatCombo().SelectedIndex()));
    const bool lossy = audiofmt::IsLossy(format);
    const auto shown = winrt::Microsoft::UI::Xaml::Visibility::Visible;
    const auto hidden = winrt::Microsoft::UI::Xaml::Visibility::Collapsed;

    FormatHint().Text(FormatHintText(format));
    QualityLabel().Text(lossy ? L"Bitrate" : L"Bit depth");
    BitDepthCombo().Visibility(lossy ? hidden : shown);
    BitrateCombo().Visibility(lossy ? shown : hidden);

    // FLAC has no float mode.
    RestrictItems(
        BitDepthCombo(), [format](uint32_t bits) { return bits != 32 || format != audiofmt::kFlac; }, 24);

    // Each lossy codec has its own bitrate ladder. Leave everything enabled
    // while hidden, so switching back to a lossy format never finds a
    // stranded selection.
    RestrictItems(
        BitrateCombo(),
        [format](uint32_t kbps) {
            const auto k = static_cast<uint16_t>(kbps);
            if (format == audiofmt::kMp3) return audiofmt::IsMp3Bitrate(k);
            if (format == audiofmt::kM4a) return audiofmt::IsAacBitrate(k);
            return true;
        },
        audiofmt::kDefaultKbps);

    // The MP3 and AAC encoders only take 44.1 and 48 kHz.
    RestrictItems(
        SampleRateCombo(), [lossy](uint32_t rate) { return !lossy || rate == 44100 || rate == 48000; },
        48000);
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
    if (FormatCombo() && BitDepthCombo() && BitrateCombo()) {
        auto format = m_format;
        auto bits = m_bitDepth;
        auto kbps = m_bitrateKbps;
        audiofmt::Normalize(format, bits, kbps);
        // Format first, so the item restrictions match before the quality
        // selections land; then sync once more for the final state.
        FormatCombo().SelectedIndex(format);
        BitDepthCombo().SelectedIndex(std::max(0, IndexOfTag(BitDepthCombo(), bits)));
        BitrateCombo().SelectedIndex(std::max(0, IndexOfTag(BitrateCombo(), kbps)));
        SyncFormatControls();
    }
    if (PillStyleCombo()) {
        PillStyleCombo().SelectedIndex(m_pillDot ? 1 : 0);
    }
    if (PillEdgeCombo()) {
        PillEdgeCombo().SelectedIndex(m_pillBottom ? 1 : 0);
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
    if (self->FormatCombo() && self->FormatCombo().SelectedIndex() >= 0) {
        self->m_format = static_cast<uint16_t>(self->FormatCombo().SelectedIndex());
    }
    if (self->BitDepthCombo()) {
        if (const auto bits = TagValue(self->BitDepthCombo().SelectedItem())) {
            self->m_bitDepth = static_cast<uint16_t>(bits);
        }
    }
    if (self->BitrateCombo()) {
        if (const auto kbps = TagValue(self->BitrateCombo().SelectedItem())) {
            self->m_bitrateKbps = static_cast<uint16_t>(kbps);
        }
    }
    if (self->PillStyleCombo()) {
        self->m_pillDot = self->PillStyleCombo().SelectedIndex() == 1;
    }
    if (self->PillEdgeCombo()) {
        self->m_pillBottom = self->PillEdgeCombo().SelectedIndex() == 1;
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
