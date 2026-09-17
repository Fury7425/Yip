#include "pch.h"
#include "viewmodels/MainViewModel.h"
#include "viewmodels.MainViewModel.g.cpp"
#include "viewmodels.DeviceEntry.g.cpp"
#include "viewmodels.RecordingEntry.g.cpp"

#include "AudioCoreInterop.h"
#include "HotkeyManager.h"
#include "Markers.h"
#include "RecordingFormat.h"
#include "ThemeColors.h"
#include "WavProbe.h"

#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <sstream>

using namespace std::chrono_literals;

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kMicGlyph[] = L"\uE720";     // microphone
constexpr wchar_t kSpeakerGlyph[] = L"\uE7F5"; // speaker
constexpr wchar_t kPlayGlyph[] = L"\uE768";    // play, on the row being heard
constexpr wchar_t kPauseGlyph[] = L"\uE769";   // pause, on the row being held
constexpr wchar_t kDot[] = L" \u00B7 ";        // separator used in list subtitles

// Bottom of the meter scale, in dBFS. Matches YipMeterFloorDb in App.xaml —
// the tick marks there are drawn on this same curve.
constexpr double kMeterFloorDb = -60.0;

// Below this amplitude there is nothing to show; log10 of it is noise.
constexpr float kSilenceFloor = 1e-7f;

// How fast the peak-hold marker falls, in meter units per tick. At the 16 ms
// focused tick that is a full sweep in roughly 0.8 s: long enough to read a
// transient, short enough not to lie about the current level.
constexpr float kHoldFallPerTick = 0.02f;

// Don't re-raise a binding for movement the eye cannot resolve.
constexpr float kMeterEpsilon = 1.0f / 512.0f;

/// Map an amplitude onto the meter's logarithmic 0..1 travel. A linear bar
/// spends its whole length in the bottom fifth and tells you nothing.
float MeterNorm(float amplitude)
{
    if (!(amplitude > kSilenceFloor)) return 0.0f;
    const double db = 20.0 * std::log10(static_cast<double>(amplitude));
    const double n = (db - kMeterFloorDb) / (0.0 - kMeterFloorDb);
    return static_cast<float>(std::clamp(n, 0.0, 1.0));
}

winrt::hstring FormatDbFromAmplitude(float a)
{
    if (!(a > kSilenceFloor)) return winrt::hstring{L"-\u221E dB"};
    const double db = 20.0 * std::log10(static_cast<double>(a));
    wchar_t buf[24];
    swprintf_s(buf, L"%+.1f dB", db);
    return winrt::hstring{buf};
}

/// "MM:SS.T" under an hour, "H:MM:SS.T" past it. Monospaced at the call site,
/// so the width only changes when the hour rolls over.
std::wstring FormatElapsed(uint64_t ms)
{
    const uint64_t tenths = (ms / 100) % 10;
    const uint64_t totalSecs = ms / 1000;
    const uint64_t secs = totalSecs % 60;
    const uint64_t mins = (totalSecs / 60) % 60;
    const uint64_t hours = totalSecs / 3600;
    wchar_t buf[32];
    if (hours > 0) {
        swprintf_s(buf, L"%llu:%02llu:%02llu.%llu", hours, mins, secs, tenths);
    } else {
        swprintf_s(buf, L"%02llu:%02llu.%llu", mins, secs, tenths);
    }
    return buf;
}

std::wstring FormatDuration(std::chrono::milliseconds ms)
{
    const auto total = ms.count();
    const auto secs = total / 1000;
    const auto mins = secs / 60;
    wchar_t buf[24];
    if (mins >= 60) {
        swprintf_s(buf, L"%lld:%02lld:%02lld", static_cast<long long>(mins / 60),
                   static_cast<long long>(mins % 60), static_cast<long long>(secs % 60));
    } else {
        swprintf_s(buf, L"%02lld:%02lld", static_cast<long long>(mins), static_cast<long long>(secs % 60));
    }
    return buf;
}

std::wstring FormatSize(uint64_t bytes)
{
    constexpr double kKiB = 1024.0;
    const double b = static_cast<double>(bytes);
    wchar_t buf[32];
    if (b < kKiB) {
        swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(bytes));
    } else if (b < kKiB * kKiB) {
        swprintf_s(buf, L"%.0f KB", b / kKiB);
    } else if (b < kKiB * kKiB * kKiB) {
        swprintf_s(buf, L"%.1f MB", b / (kKiB * kKiB));
    } else {
        swprintf_s(buf, L"%.2f GB", b / (kKiB * kKiB * kKiB));
    }
    return buf;
}

/// Wall-clock age of a file, phrased the way a person would say it. Falls back
/// to an absolute stamp once "N days ago" stops being useful.
std::wstring FormatModified(const fs::file_time_type& t)
{
    using namespace std::chrono;
    const auto sctp =
        time_point_cast<system_clock::duration>(t - fs::file_time_type::clock::now() + system_clock::now());
    const auto age = system_clock::now() - sctp;
    const auto mins = duration_cast<minutes>(age).count();

    wchar_t buf[64];
    if (age < 0s || mins < 1) return L"just now";
    if (mins < 60) {
        swprintf_s(buf, L"%lld min ago", static_cast<long long>(mins));
        return buf;
    }
    const auto hrs = duration_cast<hours>(age).count();
    if (hrs < 24) {
        swprintf_s(buf, L"%lld h ago", static_cast<long long>(hrs));
        return buf;
    }
    if (hrs < 24 * 7) {
        swprintf_s(buf, L"%lld d ago", static_cast<long long>(hrs / 24));
        return buf;
    }

    const auto tt = system_clock::to_time_t(sctp);
    std::tm tm{};
    if (localtime_s(&tm, &tt) != 0) return L"";
    wcsftime(buf, std::size(buf), L"%Y-%m-%d %H:%M", &tm);
    return buf;
}

/// "48 kHz · stereo · WAV 32-bit float · 6.2 MB" — the facts you need before
/// handing a take to a plugin chain.
std::wstring FormatSubtitle(const std::optional<::yip::AudioInfo>& info, uint64_t sizeBytes)
{
    std::wstring out;
    if (info && info->sample_rate > 0) {
        wchar_t buf[32];
        if (info->sample_rate % 1000 == 0) {
            swprintf_s(buf, L"%u kHz", info->sample_rate / 1000);
        } else {
            swprintf_s(buf, L"%.1f kHz", static_cast<double>(info->sample_rate) / 1000.0);
        }
        out += buf;

        out += kDot;
        if (info->channels == 1) {
            out += L"mono";
        } else if (info->channels == 2) {
            out += L"stereo";
        } else {
            swprintf_s(buf, L"%u ch", static_cast<unsigned>(info->channels));
            out += buf;
        }

        if (!info->quality.empty()) {
            out += kDot;
            out += info->quality;
        }
        out += kDot;
    }
    out += FormatSize(sizeBytes);
    return out;
}

std::wstring ToLower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring Utf8ToWide(const char* utf8)
{
    if (!utf8) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out;
    out.resize(static_cast<size_t>(n) - 1);
    ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
    return out;
}

} // namespace

namespace winrt::yip::viewmodels::implementation {
// ============================================================ RecordingEntry

void RecordingEntry::IsPlaying(bool v)
{
    if (m_isPlaying == v) return;
    m_isPlaying = v;
    // The glyph is cleared rather than left behind a zero opacity: a row that
    // is not playing has nothing to say, not something invisible to say.
    m_playGlyph = v ? winrt::hstring{kPlayGlyph} : winrt::hstring{L""};
    Raise(L"IsPlaying");
    Raise(L"PlayGlyph");
    Raise(L"PlayingOpacity");
}

void RecordingEntry::SetPlaybackGlyph(bool paused)
{
    if (!m_isPlaying) return;
    const winrt::hstring glyph{paused ? kPauseGlyph : kPlayGlyph};
    if (m_playGlyph == glyph) return;
    m_playGlyph = glyph;
    Raise(L"PlayGlyph");
}

void RecordingEntry::Raise(winrt::hstring const& name)
{
    m_propertyChanged(*this, winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{name});
}

// ============================================================ MainViewModel

MainViewModel::MainViewModel()
{
    // Ensure output folder exists.
    std::error_code ec;
    fs::create_directories(m_settings.output_folder, ec);
}

void MainViewModel::SelectedDeviceIndex(int32_t v)
{
    if (m_selectedDeviceIndex == v) return;
    m_selectedDeviceIndex = v;
    Raise(L"SelectedDeviceIndex");
    Raise(L"CanRecord");
}

winrt::Microsoft::UI::Xaml::Media::Brush MainViewModel::RecordButtonBrush() const
{
    // Both values live in App.xaml's theme dictionaries, so light and dark each
    // get their own and nothing is written twice. Cached because the button's
    // visual states read this often.
    if (m_isRecording) {
        if (!m_recordBrush) m_recordBrush = ::yip::theme::Brush(L"YipRecordFillLiveBrush");
        return m_recordBrush;
    }
    if (!m_idleBrush) m_idleBrush = ::yip::theme::Brush(L"YipRecordFillIdleBrush");
    return m_idleBrush;
}

void MainViewModel::InvalidateThemeBrushes()
{
    m_idleBrush = nullptr;
    m_recordBrush = nullptr;
    Raise(L"RecordButtonBrush");
}

void MainViewModel::RefreshDevices()
{
    // Keep the user's pick across a device-change notification: re-selecting
    // by identity beats resetting them to the default every time a headset
    // wakes up.
    winrt::hstring previousId;
    if (m_selectedDeviceIndex >= 0 && static_cast<uint32_t>(m_selectedDeviceIndex) < m_devices.Size()) {
        previousId = m_devices.GetAt(static_cast<uint32_t>(m_selectedDeviceIndex)).Id();
    }

    auto devices = ::yip::interop::ListDevices();
    std::vector<winrt::yip::viewmodels::DeviceEntry> entries;
    entries.reserve(devices.size());

    int defaultIdx = -1;
    int restoredIdx = -1;
    int idx = 0;
    for (auto& d : devices) {
        auto id = winrt::to_hstring(d.id);
        entries.push_back(winrt::make<DeviceEntry>(id, winrt::to_hstring(d.name),
                                                   winrt::hstring{d.isCapture ? kMicGlyph : kSpeakerGlyph},
                                                   d.isCapture, d.isDefault));
        if (d.isDefault && defaultIdx < 0 && d.isCapture) defaultIdx = idx;
        if (!previousId.empty() && id == previousId) restoredIdx = idx;
        ++idx;
    }
    m_devices.ReplaceAll(entries);

    // ReplaceAll makes the ComboBox write -1 back through the two-way binding,
    // so decide from `previousId`, captured before the swap, not from the
    // index. Falling back to the default matters: keeping a stale index would
    // silently point the next take at whichever endpoint slid into that slot.
    int32_t wanted = -1;
    if (restoredIdx >= 0) {
        wanted = restoredIdx;
    } else if (!previousId.empty() || m_selectedDeviceIndex < 0) {
        wanted = defaultIdx;
    }
    if (wanted != m_selectedDeviceIndex) {
        m_selectedDeviceIndex = wanted;
        Raise(L"SelectedDeviceIndex");
    }
    Raise(L"CanRecord");
    if (devices.empty()) {
        SetStatus(L"No input devices found");
    } else if (!m_isRecording) {
        SetStatus(L"Ready");
    }
}

void MainViewModel::RefreshRecordings()
{
    m_rows.clear();

    const auto& folder = m_settings.output_folder;
    std::error_code ec;
    if (fs::exists(folder, ec)) {
        for (const auto& it : fs::directory_iterator(folder, ec)) {
            if (ec) break;
            if (!it.is_regular_file()) continue;
            const auto& p = it.path();
            if (!::yip::audiofmt::IsRecordingExtension(ToLower(p.extension().wstring()))) continue;

            Row r;
            r.path = p;
            r.fileName = p.filename().wstring();
            r.lowerName = ToLower(r.fileName);
            r.sizeBytes = static_cast<uint64_t>(fs::file_size(p, ec));
            if (ec) r.sizeBytes = 0;

            r.modified = fs::last_write_time(p, ec);
            r.modifiedAt = ec ? std::wstring{} : FormatModified(r.modified);

            // Read the real headers. Deriving duration from file size and an
            // assumed 48 kHz stereo float32 is wrong for every other format,
            // and the format is user-selectable.
            const auto info = ::yip::ProbeAudio(p);
            r.duration = FormatDuration(info ? info->duration : std::chrono::milliseconds{0});
            r.subtitle = FormatSubtitle(info, r.sizeBytes);
            m_rows.push_back(std::move(r));
        }
    }

    // Newest first, by mtime rather than by name: the folder also holds
    // anything the user dropped in, which does not carry Yip's timestamped
    // naming.
    std::sort(m_rows.begin(), m_rows.end(),
              [](const Row& a, const Row& b) { return a.modified > b.modified; });

    ProjectRecordings();
}

void MainViewModel::ProjectRecordings()
{
    const std::wstring needle = ToLower(std::wstring{m_filterText});

    std::vector<winrt::yip::viewmodels::RecordingEntry> items;
    items.reserve(m_rows.size());
    uint64_t shownBytes = 0;
    for (const auto& r : m_rows) {
        if (!needle.empty() && r.lowerName.find(needle) == std::wstring::npos) continue;
        shownBytes += r.sizeBytes;
        items.push_back(winrt::make<RecordingEntry>(winrt::hstring{r.path.wstring()},
                                                    winrt::hstring{r.fileName}, winrt::hstring{r.duration},
                                                    winrt::hstring{r.modifiedAt},
                                                    winrt::hstring{r.subtitle}));
    }

    // One Reset instead of a VectorChanged per row: the ListView rebuilds once
    // rather than animating an insert for every recording.
    m_recordings.ReplaceAll(items);

    std::wstring summary;
    wchar_t buf[96];
    if (m_rows.empty()) {
        summary = L"";
    } else if (items.size() == m_rows.size()) {
        swprintf_s(buf, L"%zu recording%s", m_rows.size(), m_rows.size() == 1 ? L"" : L"s");
        summary = buf;
        summary += kDot;
        summary += FormatSize(shownBytes);
    } else {
        swprintf_s(buf, L"%zu of %zu", items.size(), m_rows.size());
        summary = buf;
    }
    m_recordingsSummary = winrt::hstring{summary};

    // Fresh entries, so the marker has to be put back on whichever row is
    // loaded — filtering must not silence the list's account of what is
    // playing.
    MarkPlayingRow();

    // Not Raise(L"Recordings"): the observable vector already published one
    // Reset above, and re-setting ItemsSource would rebuild the list twice.
    Raise(L"IsEmpty");
    Raise(L"RecordingsSummary");
}

void MainViewModel::FilterText(winrt::hstring const& v)
{
    if (m_filterText == v) return;
    m_filterText = v;
    Raise(L"FilterText");
    ProjectRecordings();
}

void MainViewModel::Tick()
{
    RecMeter snapshot{};
    if (rec_meter(&snapshot) != REC_STATUS_OK) return;

    const float peak = MeterNorm(snapshot.peak);
    const float rms = MeterNorm(snapshot.rms);

    // Peak hold: jump to a new peak at once, fall back linearly. Without it a
    // transient is a single frame nobody sees. Once capture ends the timer
    // stops, so the marker has to be cleared here or it hangs on screen.
    float hold = 0.0f;
    if (snapshot.recording != 0) {
        hold = (peak >= m_meterHold) ? peak : std::max(peak, m_meterHold - kHoldFallPerTick);
    }

    if (std::abs(peak - m_meterPeak) >= kMeterEpsilon) {
        m_meterPeak = peak;
        m_peakLabel = FormatDbFromAmplitude(snapshot.peak);
        Raise(L"MeterPeak");
        Raise(L"PeakLabel");
    }
    if (std::abs(rms - m_meterRms) >= kMeterEpsilon) {
        m_meterRms = rms;
        m_rmsLabel = FormatDbFromAmplitude(snapshot.rms);
        Raise(L"MeterRms");
        Raise(L"RmsLabel");
    }
    if (std::abs(hold - m_meterHold) >= kMeterEpsilon) {
        m_meterHold = hold;
        Raise(L"MeterHold");
    }

    if (snapshot.clip_count != m_clipCount) {
        const bool was = m_clipCount > 0;
        m_clipCount = snapshot.clip_count;
        if (was != (m_clipCount > 0)) Raise(L"HasClipped");
    }
    if (snapshot.overrun_count != m_dropoutCount) {
        const bool was = m_dropoutCount > 0;
        m_dropoutCount = snapshot.overrun_count;
        Raise(L"DropoutCount");
        if (was != (m_dropoutCount > 0)) Raise(L"HasDropouts");
        if (m_dropoutCount > 0) {
            SetStatus(L"Disk could not keep up \u2014 audio was dropped");
        }
    }

    // The timer only ever moves in tenths; re-formatting at 60 Hz would churn
    // the binding nine times out of ten for no visible change.
    auto text = winrt::hstring{FormatElapsed(snapshot.elapsed_ms)};
    if (text != m_elapsedText) {
        m_elapsedText = text;
        Raise(L"ElapsedText");
    }
}

void MainViewModel::ToggleRecording()
{
    if (m_isRecording) {
        const auto status = rec_stop();
        m_isRecording = false;
        m_activeRecordingPath.reset();
        if (status != REC_STATUS_OK) {
            SetError(LastCoreError(L"Stop failed"));
        } else {
            SetStatus(L"Saved");
        }
        Raise(L"IsRecording");
        Raise(L"RecordButtonText");
        Raise(L"RecordButtonBrush");
        RefreshRecordings();
        return;
    }

    if (m_selectedDeviceIndex < 0 || static_cast<uint32_t>(m_selectedDeviceIndex) >= m_devices.Size()) {
        SetError(L"Pick an input device first");
        return;
    }
    // A take being recorded and one playing out loud do not belong in the same
    // room: the microphone would hear it.
    StopPlayback();
    auto dev = m_devices.GetAt(static_cast<uint32_t>(m_selectedDeviceIndex));
    const auto path = NextRecordingPath();

    const auto idUtf8 = winrt::to_string(dev.Id());
    // Not path.string(): that encodes with the ANSI codepage and hands
    // audio-core bytes it rejects as invalid UTF-8 for any non-ASCII folder.
    const auto pathUtf8 = ::yip::ToUtf8(path.wstring());

    RecConfig cfg{};
    cfg.sample_rate = m_settings.sample_rate;
    cfg.channels = m_settings.channels;
    cfg.format = m_settings.format;
    cfg.bit_depth = m_settings.bit_depth;
    cfg.bitrate_kbps = m_settings.bitrate_kbps;

    const auto status = rec_start(idUtf8.c_str(), pathUtf8.c_str(), cfg);
    if (status != REC_STATUS_OK) {
        SetError(LastCoreError(L"Start failed"));
        return;
    }

    DismissError();
    m_isRecording = true;
    m_activeRecordingPath = path;
    m_meterHold = 0.0f;
    m_clipCount = 0;
    m_dropoutCount = 0;
    SetStatus(winrt::hstring{L"Recording to " + path.filename().wstring()});
    Raise(L"IsRecording");
    Raise(L"RecordButtonText");
    Raise(L"RecordButtonBrush");
    Raise(L"HasClipped");
    Raise(L"HasDropouts");
}

void MainViewModel::ApplySettings(winrt::hstring const& folder, uint32_t sampleRate, uint16_t channels,
                                  uint16_t format, uint16_t bitDepth, uint16_t bitrateKbps,
                                  uint32_t hotkeyMods, uint32_t hotkeyVk, bool pillDot, bool pillBottom)
{
    m_settings.output_folder = std::wstring{folder};
    m_settings.sample_rate = sampleRate;
    m_settings.channels = channels;
    ::yip::audiofmt::Normalize(format, bitDepth, bitrateKbps);
    m_settings.format = format;
    m_settings.bit_depth = bitDepth;
    m_settings.bitrate_kbps = bitrateKbps;
    if (::yip::IsValidHotkey(hotkeyMods, hotkeyVk)) {
        m_settings.hotkey_mods = hotkeyMods;
        m_settings.hotkey_vk = hotkeyVk;
    }
    m_settings.pill_dot = pillDot;
    m_settings.pill_bottom = pillBottom;

    std::error_code ec;
    fs::create_directories(m_settings.output_folder, ec);
    if (!m_settings.Save()) {
        SetError(L"Could not write settings.json");
    }

    Raise(L"OutputFolder");
    Raise(L"SampleRate");
    Raise(L"Channels");
    Raise(L"Format");
    Raise(L"BitDepth");
    Raise(L"BitrateKbps");
    Raise(L"FormatLabel");
    Raise(L"HotkeyMods");
    Raise(L"HotkeyVk");
    Raise(L"HotkeyLabel");
    Raise(L"PillDot");
    Raise(L"PillBottom");
    RefreshRecordings();
    if (!HasError()) SetStatus(L"Settings saved");
}

winrt::hstring MainViewModel::FormatLabel() const
{
    // The rate shown is the one the take will really have: MP3 and M4A record
    // 88.2 and 96 kHz settings at 48 kHz.
    wchar_t const* channels = m_settings.channels == 1   ? L"mono"
                              : m_settings.channels == 2 ? L"stereo"
                                                         : L"multi";
    const auto rate = ::yip::audiofmt::CaptureRate(m_settings.format, m_settings.sample_rate);
    const auto quality =
        ::yip::audiofmt::Describe(m_settings.format, m_settings.bit_depth, m_settings.bitrate_kbps);
    wchar_t buf[96];
    if (rate % 1000 == 0) {
        swprintf_s(buf, L"%u kHz \u00B7 %s \u00B7 %s", rate / 1000, channels, quality.c_str());
    } else {
        swprintf_s(buf, L"%.1f kHz \u00B7 %s \u00B7 %s", static_cast<double>(rate) / 1000.0, channels,
                   quality.c_str());
    }
    return winrt::hstring{buf};
}

winrt::hstring MainViewModel::HotkeyLabel() const
{
    return winrt::hstring{::yip::FormatHotkey(m_settings.hotkey_mods, m_settings.hotkey_vk)};
}

void MainViewModel::SyncRecordingState(bool recording)
{
    if (m_isRecording == recording) return;
    m_isRecording = recording;
    if (!recording) {
        m_activeRecordingPath.reset();
        RefreshRecordings();
    }
    Raise(L"IsRecording");
    Raise(L"RecordButtonText");
    Raise(L"RecordButtonBrush");
    Raise(L"CanRecord");
}

void MainViewModel::ReportHotkeyConflict()
{
    SetError(winrt::hstring{L"Hotkey " + ::yip::FormatHotkey(m_settings.hotkey_mods, m_settings.hotkey_vk) +
                            L" is already taken by another app"});
}

void MainViewModel::DismissError()
{
    if (m_errorText.empty()) return;
    m_errorText = L"";
    Raise(L"ErrorText");
    Raise(L"HasError");
}

void MainViewModel::AcknowledgeClip()
{
    rec_reset_clip();
    if (m_clipCount == 0) return;
    m_clipCount = 0;
    Raise(L"HasClipped");
}

void MainViewModel::RevealRecording(winrt::yip::viewmodels::RecordingEntry const& entry)
{
    if (!entry) return;
    std::wstring args = L"/select,\"";
    args += entry.FullPath().c_str();
    args += L"\"";
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void MainViewModel::OpenRecordingExternally(winrt::yip::viewmodels::RecordingEntry const& entry)
{
    if (!entry) return;
    ::ShellExecuteW(nullptr, L"open", entry.FullPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool MainViewModel::DeleteRecording(winrt::yip::viewmodels::RecordingEntry const& entry)
{
    if (!entry) return false;
    const fs::path path{std::wstring{entry.FullPath()}};

    // Windows will not delete a file the decoder still has open, and a take
    // that is playing is one the user is deleting on purpose.
    if (m_playingPath && *m_playingPath == path) StopPlayback();

    std::error_code ec;
    if (!fs::remove(path, ec) || ec) {
        SetError(L"Could not delete that recording");
        return false;
    }
    // The marker sidecar is meaningless without its take.
    std::error_code sidecarEc;
    fs::remove(::yip::markers::SidecarFor(path), sidecarEc);

    std::erase_if(m_rows, [&path](const Row& r) { return r.path == path; });
    ProjectRecordings();
    SetStatus(winrt::hstring{L"Deleted " + path.filename().wstring()});
    return true;
}

void MainViewModel::CopyRecordingPath(winrt::yip::viewmodels::RecordingEntry const& entry)
{
    if (!entry) return;
    winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
    package.RequestedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
    package.SetText(entry.FullPath());
    winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
    SetStatus(L"Path copied");
}

// ============================================================ Playback

void MainViewModel::ActivateRecording(winrt::yip::viewmodels::RecordingEntry const& entry)
{
    if (!entry) return;
    const fs::path path{std::wstring{entry.FullPath()}};
    // Clicking the row you are already hearing holds it, rather than starting
    // it over: the second click on a thing is never a request to repeat it.
    if (m_playbackLoaded && m_playingPath && *m_playingPath == path) {
        TogglePlayback();
        return;
    }
    PlayRecording(entry);
}

void MainViewModel::PlayRecording(winrt::yip::viewmodels::RecordingEntry const& entry)
{
    if (!entry) return;
    const fs::path path{std::wstring{entry.FullPath()}};
    // Not path.string(): that encodes with the ANSI codepage and hands
    // audio-core bytes it rejects as invalid UTF-8 for any non-ASCII folder.
    const auto pathUtf8 = ::yip::ToUtf8(path.wstring());

    if (play_start(pathUtf8.c_str()) != REC_STATUS_OK) {
        SetError(LastCoreError(L"Could not play that recording"));
        ClearPlaybackState();
        return;
    }

    DismissError();
    m_playingPath = path;
    m_playbackLoaded = true;
    m_playbackPaused = false;
    m_positionMs = 0;
    m_durationMs = 0;
    m_playbackLevel = 0.0f;
    m_playingFileName = entry.FileName();
    m_positionText = L"00:00";
    m_durationText = L"00:00";
    SetStatus(winrt::hstring{L"Playing " + path.filename().wstring()});
    MarkPlayingRow();
    RaisePlaybackProps();
}

void MainViewModel::TogglePlayback()
{
    if (!m_playbackLoaded) return;
    const auto status = m_playbackPaused ? play_resume() : play_pause();
    if (status != REC_STATUS_OK) {
        SetError(LastCoreError(L"Playback could not be held"));
        return;
    }
    m_playbackPaused = !m_playbackPaused;
    SetStatus(m_playbackPaused ? L"Paused" : L"Playing");
    MarkPlayingRow();
    Raise(L"IsPlaybackPlaying");
}

void MainViewModel::StopPlayback()
{
    // Ask audio-core too: a take that ran out has already released its threads
    // on one side and still needs joining on the other.
    if (!m_playbackLoaded && play_is_playing() == 0) return;
    const auto status = play_stop();
    ClearPlaybackState();
    if (status != REC_STATUS_OK) {
        SetError(LastCoreError(L"Playback failed"));
    }
}

void MainViewModel::SeekPlayback(uint64_t positionMs)
{
    if (!m_playbackLoaded) return;
    if (play_seek_ms(positionMs) != REC_STATUS_OK) return;

    // Report the target straight away. audio-core coalesces seeks, so the
    // position it reports lags the drag by a tick or two and the scrubber
    // would fight the thumb.
    m_positionMs = positionMs;
    m_positionText =
        winrt::hstring{FormatDuration(std::chrono::milliseconds{static_cast<long long>(positionMs)})};
    Raise(L"PlaybackPositionMs");
    Raise(L"PlaybackPositionText");
}

void MainViewModel::PlaybackTick()
{
    PlayState snapshot{};
    if (play_state(&snapshot) != REC_STATUS_OK) return;

    if (snapshot.playing == 0) {
        // The file ran out, or the endpoint gave up. Either way the threads are
        // done and want joining; `finished` tells the two apart.
        const bool finished = snapshot.finished != 0;
        const auto status = play_stop();
        ClearPlaybackState();
        if (status != REC_STATUS_OK) {
            SetError(LastCoreError(L"Playback stopped"));
        } else if (finished) {
            SetStatus(L"Played to the end");
        }
        return;
    }

    const bool paused = snapshot.paused != 0;
    if (paused != m_playbackPaused) {
        m_playbackPaused = paused;
        MarkPlayingRow();
        Raise(L"IsPlaybackPlaying");
    }

    if (snapshot.duration_ms != m_durationMs) {
        m_durationMs = snapshot.duration_ms;
        m_durationText = winrt::hstring{
            FormatDuration(std::chrono::milliseconds{static_cast<long long>(m_durationMs)})};
        Raise(L"PlaybackDurationMs");
        Raise(L"PlaybackDurationText");
    }

    if (snapshot.position_ms != m_positionMs) {
        // The label only moves in whole seconds; re-formatting every tick would
        // churn the binding nine times out of ten for no visible change.
        const bool secondRolled = snapshot.position_ms / 1000 != m_positionMs / 1000;
        m_positionMs = snapshot.position_ms;
        Raise(L"PlaybackPositionMs");
        if (secondRolled) {
            m_positionText = winrt::hstring{
                FormatDuration(std::chrono::milliseconds{static_cast<long long>(m_positionMs)})};
            Raise(L"PlaybackPositionText");
        }
    }

    const float level = MeterNorm(snapshot.level);
    if (std::abs(level - m_playbackLevel) >= kMeterEpsilon) {
        m_playbackLevel = level;
        Raise(L"PlaybackLevelPercent");
    }
}

void MainViewModel::MarkPlayingRow()
{
    const bool loaded = m_playbackLoaded && m_playingPath.has_value();
    for (uint32_t i = 0; i < m_recordings.Size(); ++i) {
        auto entry = m_recordings.GetAt(i);
        auto row = winrt::get_self<RecordingEntry>(entry);
        const bool isThisOne = loaded && fs::path{std::wstring{entry.FullPath()}} == *m_playingPath;
        row->IsPlaying(isThisOne);
        if (isThisOne) row->SetPlaybackGlyph(m_playbackPaused);
    }
}

void MainViewModel::ClearPlaybackState()
{
    m_playingPath.reset();
    m_playbackLoaded = false;
    m_playbackPaused = false;
    m_positionMs = 0;
    m_durationMs = 0;
    m_playbackLevel = 0.0f;
    m_playingFileName = L"";
    m_positionText = L"00:00";
    m_durationText = L"00:00";
    MarkPlayingRow();
    RaisePlaybackProps();
}

void MainViewModel::RaisePlaybackProps()
{
    Raise(L"IsPlaybackLoaded");
    Raise(L"IsPlaybackPlaying");
    Raise(L"PlayingFileName");
    Raise(L"PlaybackPositionText");
    Raise(L"PlaybackDurationText");
    Raise(L"PlaybackPositionMs");
    Raise(L"PlaybackDurationMs");
    Raise(L"PlaybackLevelPercent");
}

winrt::event_token MainViewModel::PropertyChanged(
    winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler)
{
    return m_propertyChanged.add(handler);
}

void MainViewModel::PropertyChanged(winrt::event_token const& token) noexcept
{
    m_propertyChanged.remove(token);
}

void MainViewModel::Raise(winrt::hstring const& name)
{
    m_propertyChanged(*this, winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{name});
}

std::filesystem::path MainViewModel::NextRecordingPath() const
{
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &tt);
    wchar_t name[64];
    wcsftime(name, std::size(name), L"yip-%Y%m%d-%H%M%S", &tm);
    return m_settings.output_folder / (std::wstring{name} + ::yip::audiofmt::Extension(m_settings.format));
}

void MainViewModel::SetStatus(winrt::hstring const& s)
{
    if (m_statusText == s) return;
    m_statusText = s;
    Raise(L"StatusText");
}

void MainViewModel::SetError(winrt::hstring const& s)
{
    if (m_errorText != s) {
        m_errorText = s;
        Raise(L"ErrorText");
        Raise(L"HasError");
    }
    SetStatus(s);
}

winrt::hstring MainViewModel::LastCoreError(wchar_t const* fallback)
{
    auto msg = Utf8ToWide(rec_last_error());
    if (msg.empty()) msg = fallback;
    return winrt::hstring{msg};
}
} // namespace winrt::yip::viewmodels::implementation
