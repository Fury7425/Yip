#pragma once

#include "SettingsDialog.g.h"

namespace winrt::yip::implementation {
struct SettingsDialog : SettingsDialogT<SettingsDialog> {
    SettingsDialog();

    winrt::hstring OutputFolder() const noexcept;
    void OutputFolder(winrt::hstring const& v);

    uint32_t SampleRate() const noexcept;
    void SampleRate(uint32_t v);

    uint16_t Channels() const noexcept;
    void Channels(uint16_t v);

    uint16_t Format() const noexcept;
    void Format(uint16_t v);

    uint16_t BitDepth() const noexcept;
    void BitDepth(uint16_t v);

    uint16_t BitrateKbps() const noexcept;
    void BitrateKbps(uint16_t v);

    uint32_t HotkeyMods() const noexcept { return m_hotkeyMods; }
    void HotkeyMods(uint32_t v);

    uint32_t HotkeyVk() const noexcept { return m_hotkeyVk; }
    void HotkeyVk(uint32_t v);

    bool PillDot() const noexcept;
    void PillDot(bool v);

    bool PillBottom() const noexcept;
    void PillBottom(bool v);

    winrt::fire_and_forget OnPickFolder(winrt::Windows::Foundation::IInspectable const& sender,
                                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

    void OnHotkeyKeyDown(winrt::Windows::Foundation::IInspectable const& sender,
                         winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
    void OnResetHotkey(winrt::Windows::Foundation::IInspectable const& sender,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

    void OnFormatChanged(winrt::Windows::Foundation::IInspectable const& sender,
                         winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);

private:
    void ApplyToControls();
    void ReadFromControls() const;

    void ApplyHotkeyToControls();

    // Show the picker that matches the chosen format, and pull every
    // selection back inside what that codec accepts.
    void SyncFormatControls();

    winrt::hstring m_outputFolder;
    uint32_t m_sampleRate{48000};
    uint16_t m_channels{2};
    uint16_t m_format{0};
    uint16_t m_bitDepth{32};
    uint16_t m_bitrateKbps{192};
    uint32_t m_hotkeyMods{0x2 | 0x1}; // Ctrl+Alt
    uint32_t m_hotkeyVk{0x52};        // 'R'
    bool m_pillDot{false};
    bool m_pillBottom{false};
};
} // namespace winrt::yip::implementation

namespace winrt::yip::factory_implementation {
struct SettingsDialog : SettingsDialogT<SettingsDialog, implementation::SettingsDialog> {};
} // namespace winrt::yip::factory_implementation
