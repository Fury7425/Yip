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

    uint32_t HotkeyMods() const noexcept { return m_hotkeyMods; }
    void HotkeyMods(uint32_t v);

    uint32_t HotkeyVk() const noexcept { return m_hotkeyVk; }
    void HotkeyVk(uint32_t v);

    winrt::fire_and_forget OnPickFolder(winrt::Windows::Foundation::IInspectable const& sender,
                                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

    void OnHotkeyKeyDown(winrt::Windows::Foundation::IInspectable const& sender,
                         winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
    void OnResetHotkey(winrt::Windows::Foundation::IInspectable const& sender,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

private:
    void ApplyToControls();
    void ReadFromControls() const;

    void ApplyHotkeyToControls();

    winrt::hstring m_outputFolder;
    uint32_t m_sampleRate{48000};
    uint16_t m_channels{2};
    uint32_t m_hotkeyMods{0x2 | 0x1}; // Ctrl+Alt
    uint32_t m_hotkeyVk{0x52};        // 'R'
};
} // namespace winrt::yip::implementation

namespace winrt::yip::factory_implementation {
struct SettingsDialog : SettingsDialogT<SettingsDialog, implementation::SettingsDialog> {};
} // namespace winrt::yip::factory_implementation
