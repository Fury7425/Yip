#pragma once

#include "viewmodels.MainViewModel.g.h"
#include "viewmodels.DeviceEntry.g.h"
#include "viewmodels.RecordingEntry.g.h"

#include "Settings.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

namespace winrt::yip::viewmodels::implementation {
// ----- DeviceEntry -----

struct DeviceEntry : DeviceEntryT<DeviceEntry> {
    DeviceEntry() = default;
    DeviceEntry(winrt::hstring id, winrt::hstring name, winrt::hstring glyph, bool isCapture, bool isDefault)
        : m_id(std::move(id)), m_name(std::move(name)), m_glyph(std::move(glyph)), m_isCapture(isCapture),
          m_isDefault(isDefault)
    {}

    winrt::hstring Id() const noexcept { return m_id; }
    void Id(winrt::hstring const& v) { m_id = v; }
    winrt::hstring Name() const noexcept { return m_name; }
    void Name(winrt::hstring const& v) { m_name = v; }
    winrt::hstring Glyph() const noexcept { return m_glyph; }
    void Glyph(winrt::hstring const& v) { m_glyph = v; }
    bool IsCapture() const noexcept { return m_isCapture; }
    void IsCapture(bool v) { m_isCapture = v; }
    bool IsDefault() const noexcept { return m_isDefault; }
    void IsDefault(bool v) { m_isDefault = v; }

private:
    winrt::hstring m_id;
    winrt::hstring m_name;
    winrt::hstring m_glyph{L""};
    bool m_isCapture{true};
    bool m_isDefault{false};
};

// ----- RecordingEntry -----

struct RecordingEntry : RecordingEntryT<RecordingEntry> {
    RecordingEntry() = default;
    RecordingEntry(winrt::hstring fullPath, winrt::hstring fileName, winrt::hstring duration,
                   winrt::hstring modifiedAt, winrt::hstring subtitle)
        : m_fullPath(std::move(fullPath)), m_fileName(std::move(fileName)), m_duration(std::move(duration)),
          m_modifiedAt(std::move(modifiedAt)), m_subtitle(std::move(subtitle))
    {}

    winrt::hstring FullPath() const noexcept { return m_fullPath; }
    void FullPath(winrt::hstring const& v) { m_fullPath = v; }
    winrt::hstring FileName() const noexcept { return m_fileName; }
    void FileName(winrt::hstring const& v) { m_fileName = v; }
    winrt::hstring Duration() const noexcept { return m_duration; }
    void Duration(winrt::hstring const& v) { m_duration = v; }
    winrt::hstring ModifiedAt() const noexcept { return m_modifiedAt; }
    void ModifiedAt(winrt::hstring const& v) { m_modifiedAt = v; }
    winrt::hstring Subtitle() const noexcept { return m_subtitle; }
    void Subtitle(winrt::hstring const& v) { m_subtitle = v; }

private:
    winrt::hstring m_fullPath;
    winrt::hstring m_fileName;
    winrt::hstring m_duration;
    winrt::hstring m_modifiedAt;
    winrt::hstring m_subtitle;
};

// ----- MainViewModel -----

struct MainViewModel : MainViewModelT<MainViewModel> {
    MainViewModel();

    winrt::Windows::Foundation::Collections::IObservableVector<winrt::yip::viewmodels::DeviceEntry> Devices()
        const noexcept
    {
        return m_devices;
    }
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::yip::viewmodels::RecordingEntry>
    Recordings() const noexcept
    {
        return m_recordings;
    }

    int32_t SelectedDeviceIndex() const noexcept { return m_selectedDeviceIndex; }
    void SelectedDeviceIndex(int32_t v);

    float MeterRms() const noexcept { return m_meterRms; }
    float MeterPeak() const noexcept { return m_meterPeak; }
    float MeterHold() const noexcept { return m_meterHold; }
    winrt::hstring PeakLabel() const noexcept { return m_peakLabel; }
    winrt::hstring RmsLabel() const noexcept { return m_rmsLabel; }
    bool HasClipped() const noexcept { return m_clipCount > 0; }
    uint32_t DropoutCount() const noexcept { return m_dropoutCount; }
    bool HasDropouts() const noexcept { return m_dropoutCount > 0; }

    winrt::hstring ElapsedText() const noexcept { return m_elapsedText; }

    winrt::hstring StatusText() const noexcept { return m_statusText; }
    winrt::hstring ErrorText() const noexcept { return m_errorText; }
    bool HasError() const noexcept { return !m_errorText.empty(); }

    winrt::hstring RecordButtonText() const noexcept { return m_isRecording ? L"Stop" : L"Record"; }
    winrt::Microsoft::UI::Xaml::Media::Brush RecordButtonBrush() const;
    bool CanRecord() const noexcept { return m_selectedDeviceIndex >= 0 || m_isRecording; }
    bool IsRecording() const noexcept { return m_isRecording; }

    winrt::hstring OutputFolder() const noexcept
    {
        return winrt::hstring{m_settings.output_folder.wstring()};
    }
    uint32_t SampleRate() const noexcept { return m_settings.sample_rate; }
    uint16_t Channels() const noexcept { return m_settings.channels; }
    uint16_t Format() const noexcept { return m_settings.format; }
    uint16_t BitDepth() const noexcept { return m_settings.bit_depth; }
    uint16_t BitrateKbps() const noexcept { return m_settings.bitrate_kbps; }
    winrt::hstring FormatLabel() const;

    uint32_t HotkeyMods() const noexcept { return m_settings.hotkey_mods; }
    uint32_t HotkeyVk() const noexcept { return m_settings.hotkey_vk; }
    winrt::hstring HotkeyLabel() const;

    bool PillDot() const noexcept { return m_settings.pill_dot; }
    bool PillBottom() const noexcept { return m_settings.pill_bottom; }

    winrt::hstring FilterText() const noexcept { return m_filterText; }
    void FilterText(winrt::hstring const& v);
    bool IsEmpty() const noexcept { return m_recordings.Size() == 0; }
    winrt::hstring RecordingsSummary() const noexcept { return m_recordingsSummary; }

    void RefreshDevices();
    void RefreshRecordings();
    void Tick();
    void ToggleRecording();
    void ApplySettings(winrt::hstring const& folder, uint32_t sampleRate, uint16_t channels, uint16_t format,
                       uint16_t bitDepth, uint16_t bitrateKbps, uint32_t hotkeyMods, uint32_t hotkeyVk,
                       bool pillDot, bool pillBottom);
    void RevealRecording(winrt::yip::viewmodels::RecordingEntry const& entry);
    void OpenRecording(winrt::yip::viewmodels::RecordingEntry const& entry);
    bool DeleteRecording(winrt::yip::viewmodels::RecordingEntry const& entry);
    void CopyRecordingPath(winrt::yip::viewmodels::RecordingEntry const& entry);
    void SyncRecordingState(bool recording);
    void ReportHotkeyConflict();
    void InvalidateThemeBrushes();
    void DismissError();
    void AcknowledgeClip();

    winrt::event_token PropertyChanged(
        winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler);
    void PropertyChanged(winrt::event_token const& token) noexcept;

private:
    // One scanned recording. Cached so filtering never re-reads the folder.
    struct Row {
        std::filesystem::path path;
        std::wstring fileName;
        std::wstring lowerName; // pre-folded for the filter compare
        std::wstring duration;
        std::wstring modifiedAt;
        std::wstring subtitle;
        uint64_t sizeBytes{0};
        std::filesystem::file_time_type modified{};
    };

    void Raise(winrt::hstring const& name);
    std::filesystem::path NextRecordingPath() const;
    void SetStatus(winrt::hstring const& s);
    void SetError(winrt::hstring const& s);
    // Read the last audio-core error, or `fallback` when there is none.
    static winrt::hstring LastCoreError(wchar_t const* fallback);
    // Rebuild the observable list from m_rows + m_filterText.
    void ProjectRecordings();

    winrt::Windows::Foundation::Collections::IObservableVector<winrt::yip::viewmodels::DeviceEntry> m_devices{
        winrt::single_threaded_observable_vector<winrt::yip::viewmodels::DeviceEntry>()};
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::yip::viewmodels::RecordingEntry>
        m_recordings{winrt::single_threaded_observable_vector<winrt::yip::viewmodels::RecordingEntry>()};

    winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_propertyChanged;

    int32_t m_selectedDeviceIndex{-1};

    // Meter state, all in meter-scale units (0..1) except the labels.
    float m_meterRms{0.0f};
    float m_meterPeak{0.0f};
    float m_meterHold{0.0f};
    uint32_t m_clipCount{0};
    uint32_t m_dropoutCount{0};
    // Silence, spelled the same way the live readout spells it — Tick() only
    // writes these when the level moves, so a bare dash would survive startup.
    winrt::hstring m_peakLabel{L"-\u221E dB"};
    winrt::hstring m_rmsLabel{L"-\u221E dB"};
    winrt::hstring m_elapsedText{L"00:00.0"};

    winrt::hstring m_statusText{L"Ready"};
    winrt::hstring m_errorText{L""};
    bool m_isRecording{false};

    winrt::hstring m_filterText{L""};
    winrt::hstring m_recordingsSummary{L""};
    std::vector<Row> m_rows;

    // Resolved from App.xaml on first read and cached, because the button's
    // visual states ask for it often. Dropped on a theme change so the next
    // read picks up the other dictionary.
    mutable winrt::Microsoft::UI::Xaml::Media::Brush m_idleBrush{nullptr};
    mutable winrt::Microsoft::UI::Xaml::Media::Brush m_recordBrush{nullptr};

    std::optional<std::filesystem::path> m_activeRecordingPath;
    ::yip::Settings m_settings{::yip::Settings::Load()};
};
} // namespace winrt::yip::viewmodels::implementation

namespace winrt::yip::viewmodels::factory_implementation {
struct DeviceEntry : DeviceEntryT<DeviceEntry, implementation::DeviceEntry> {};
struct RecordingEntry : RecordingEntryT<RecordingEntry, implementation::RecordingEntry> {};
struct MainViewModel : MainViewModelT<MainViewModel, implementation::MainViewModel> {};
} // namespace winrt::yip::viewmodels::factory_implementation
