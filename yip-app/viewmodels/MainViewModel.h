#pragma once

#include "viewmodels/MainViewModel.g.h"
#include "viewmodels/DeviceEntry.g.h"
#include "viewmodels/RecordingEntry.g.h"

#include "Settings.h"

#include <filesystem>
#include <optional>

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
    winrt::hstring m_glyph{L""};
    bool m_isCapture{true};
    bool m_isDefault{false};
};

// ----- RecordingEntry -----

struct RecordingEntry : RecordingEntryT<RecordingEntry> {
    RecordingEntry() = default;
    RecordingEntry(winrt::hstring fullPath, winrt::hstring fileName, winrt::hstring duration,
                   winrt::hstring modifiedAt)
        : m_fullPath(std::move(fullPath)), m_fileName(std::move(fileName)), m_duration(std::move(duration)),
          m_modifiedAt(std::move(modifiedAt))
    {}

    winrt::hstring FullPath() const noexcept { return m_fullPath; }
    void FullPath(winrt::hstring const& v) { m_fullPath = v; }
    winrt::hstring FileName() const noexcept { return m_fileName; }
    void FileName(winrt::hstring const& v) { m_fileName = v; }
    winrt::hstring Duration() const noexcept { return m_duration; }
    void Duration(winrt::hstring const& v) { m_duration = v; }
    winrt::hstring ModifiedAt() const noexcept { return m_modifiedAt; }
    void ModifiedAt(winrt::hstring const& v) { m_modifiedAt = v; }

private:
    winrt::hstring m_fullPath;
    winrt::hstring m_fileName;
    winrt::hstring m_duration;
    winrt::hstring m_modifiedAt;
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

    float PeakLevel() const noexcept { return m_peakLevel; }
    winrt::hstring PeakLabel() const noexcept { return m_peakLabel; }

    winrt::hstring StatusText() const noexcept { return m_statusText; }
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

    void RefreshDevices();
    void RefreshRecordings();
    void PollPeak();
    void ToggleRecording();
    void ApplySettings(winrt::hstring const& folder, uint32_t sampleRate, uint16_t channels);
    void RevealRecording(winrt::yip::viewmodels::RecordingEntry const& entry);

    winrt::event_token PropertyChanged(
        winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler);
    void PropertyChanged(winrt::event_token const& token) noexcept;

private:
    void Raise(winrt::hstring const& name);
    std::filesystem::path NextRecordingPath() const;
    void SetStatus(winrt::hstring const& s);

    winrt::Windows::Foundation::Collections::IObservableVector<winrt::yip::viewmodels::DeviceEntry> m_devices{
        winrt::single_threaded_observable_vector<winrt::yip::viewmodels::DeviceEntry>()};
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::yip::viewmodels::RecordingEntry>
        m_recordings{winrt::single_threaded_observable_vector<winrt::yip::viewmodels::RecordingEntry>()};

    winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_propertyChanged;

    int32_t m_selectedDeviceIndex{-1};
    float m_peakLevel{0.0f};
    winrt::hstring m_peakLabel{L"-inf dB"};
    winrt::hstring m_statusText{L"Ready"};
    bool m_isRecording{false};
    std::optional<std::filesystem::path> m_activeRecordingPath;
    ::yip::Settings m_settings{::yip::Settings::Load()};
};
} // namespace winrt::yip::viewmodels::implementation

namespace winrt::yip::viewmodels::factory_implementation {
struct DeviceEntry : DeviceEntryT<DeviceEntry, implementation::DeviceEntry> {};
struct RecordingEntry : RecordingEntryT<RecordingEntry, implementation::RecordingEntry> {};
struct MainViewModel : MainViewModelT<MainViewModel, implementation::MainViewModel> {};
} // namespace winrt::yip::viewmodels::factory_implementation
