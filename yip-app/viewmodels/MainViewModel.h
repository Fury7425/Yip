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

    bool IsPlaying() const noexcept { return m_isPlaying; }
    void IsPlaying(bool v);
    // Play while running, pause while held. Empty when this row is not the one
    // loaded, so nothing is drawn under a zero opacity either.
    winrt::hstring PlayGlyph() const noexcept { return m_playGlyph; }
    double PlayingOpacity() const noexcept { return m_isPlaying ? 1.0 : 0.0; }
    // Paused rows show the pause glyph. Separate from IsPlaying because the
    // row stays marked while the take is held.
    void SetPlaybackGlyph(bool paused);

    winrt::event_token PropertyChanged(
        winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler)
    {
        return m_propertyChanged.add(handler);
    }
    void PropertyChanged(winrt::event_token const& token) noexcept { m_propertyChanged.remove(token); }

private:
    void Raise(winrt::hstring const& name);

    winrt::hstring m_fullPath;
    winrt::hstring m_fileName;
    winrt::hstring m_duration;
    winrt::hstring m_modifiedAt;
    winrt::hstring m_subtitle;
    winrt::hstring m_playGlyph{L""};
    bool m_isPlaying{false};
    winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_propertyChanged;
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
    void OpenRecordingExternally(winrt::yip::viewmodels::RecordingEntry const& entry);
    bool DeleteRecording(winrt::yip::viewmodels::RecordingEntry const& entry);
    void CopyRecordingPath(winrt::yip::viewmodels::RecordingEntry const& entry);

    // ----- playback -----
    void ActivateRecording(winrt::yip::viewmodels::RecordingEntry const& entry);
    void PlayRecording(winrt::yip::viewmodels::RecordingEntry const& entry);
    void TogglePlayback();
    void StopPlayback();
    void SeekPlayback(uint64_t positionMs);
    void SuspendPlayback();
    void PlaybackTick();

    bool IsPlaybackLoaded() const noexcept { return m_playbackLoaded; }
    bool IsPlaybackLive() const noexcept { return m_playbackLoaded && !m_playbackSuspended; }
    bool IsPlaybackPlaying() const noexcept { return m_playbackLoaded && !m_playbackPaused; }
    winrt::hstring PlayingFileName() const noexcept { return m_playingFileName; }
    winrt::hstring PlaybackPositionText() const noexcept { return m_positionText; }
    winrt::hstring PlaybackDurationText() const noexcept { return m_durationText; }
    double PlaybackPositionMs() const noexcept { return static_cast<double>(m_positionMs); }
    double PlaybackDurationMs() const noexcept { return static_cast<double>(m_durationMs); }
    double PlaybackLevelPercent() const noexcept { return m_playbackLevel * 100.0; }
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
        // The header probe succeeded. Only a successful probe is reused by the
        // next refresh; a failed one is retried.
        bool probed{false};
    };

    void Raise(winrt::hstring const& name);
    std::filesystem::path NextRecordingPath() const;
    void SetStatus(winrt::hstring const& s);
    void SetError(winrt::hstring const& s);
    // Read the last audio-core error, or `fallback` when there is none.
    static winrt::hstring LastCoreError(wchar_t const* fallback);
    // Rebuild the observable list from m_rows + m_filterText.
    void ProjectRecordings();
    // Mark the row whose file is loaded, and clear every other. Re-applied
    // after a re-projection, which builds fresh entries.
    void MarkPlayingRow();
    // Raise the whole transport at once. Playback moves as a unit — a position
    // without its label, or a label without its scrubber, is never useful.
    void RaisePlaybackProps();
    // Drop the transport back to its resting state and tell the view.
    void ClearPlaybackState();
    // Open `path` in audio-core from `startMs`. False, with the error shown and
    // the transport cleared, when it cannot be played.
    bool StartPlayer(std::filesystem::path const& path, uint64_t startMs);

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
    // When the dB labels last refreshed; Tick holds them to kLabelInterval.
    std::chrono::steady_clock::time_point m_labelStamp{};
    // When the peak-hold last fell; its fall is timed, not counted in ticks.
    std::chrono::steady_clock::time_point m_holdStamp{};
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

    // Playback. `m_playingPath` is the one loaded take; everything else is the
    // last snapshot audio-core handed back on a tick.
    std::optional<std::filesystem::path> m_playingPath;
    bool m_playbackLoaded{false};
    bool m_playbackPaused{false};
    // Loaded, but released by SuspendPlayback: the transport is the only record
    // of the take until play is pressed again.
    bool m_playbackSuspended{false};
    uint64_t m_positionMs{0};
    uint64_t m_durationMs{0};
    float m_playbackLevel{0.0f};
    winrt::hstring m_playingFileName{L""};
    winrt::hstring m_positionText{L"00:00"};
    winrt::hstring m_durationText{L"00:00"};
    ::yip::Settings m_settings{::yip::Settings::Load()};
};
} // namespace winrt::yip::viewmodels::implementation

namespace winrt::yip::viewmodels::factory_implementation {
struct DeviceEntry : DeviceEntryT<DeviceEntry, implementation::DeviceEntry> {};
struct RecordingEntry : RecordingEntryT<RecordingEntry, implementation::RecordingEntry> {};
struct MainViewModel : MainViewModelT<MainViewModel, implementation::MainViewModel> {};
} // namespace winrt::yip::viewmodels::factory_implementation
