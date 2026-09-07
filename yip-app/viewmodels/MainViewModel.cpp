#include "pch.h"
#include "viewmodels/MainViewModel.h"
#include "viewmodels/MainViewModel.g.cpp"
#include "viewmodels/DeviceEntry.g.cpp"
#include "viewmodels/RecordingEntry.g.cpp"

#include "AudioCoreInterop.h"
#include "HotkeyManager.h"
#include "WavProbe.h"

#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <sstream>

using namespace std::chrono_literals;

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kMicGlyph[] = L"";     // microphone
constexpr wchar_t kSpeakerGlyph[] = L""; // speaker

winrt::hstring FormatDbFromAmplitude(float a)
{
    if (a <= 1e-6f) return L"-inf dB";
    const double db = 20.0 * std::log10(static_cast<double>(a));
    wchar_t buf[16];
    swprintf_s(buf, L"%+6.1f dB", db);
    return winrt::hstring{buf};
}

winrt::hstring FormatDuration(std::chrono::milliseconds ms)
{
    using namespace std::chrono;
    const auto total = ms.count();
    const auto secs = total / 1000;
    const auto mins = secs / 60;
    wchar_t buf[16];
    swprintf_s(buf, L"%02lld:%02lld", static_cast<long long>(mins), static_cast<long long>(secs % 60));
    return winrt::hstring{buf};
}

winrt::hstring FormatModified(const fs::file_time_type& t)
{
    using namespace std::chrono;
    const auto sctp =
        time_point_cast<system_clock::duration>(t - fs::file_time_type::clock::now() + system_clock::now());
    const auto tt = system_clock::to_time_t(sctp);
    std::tm tm{};
    if (localtime_s(&tm, &tt) != 0) return L"";
    wchar_t buf[64];
    wcsftime(buf, std::size(buf), L"%Y-%m-%d %H:%M", &tm);
    return winrt::hstring{buf};
}

} // namespace

namespace winrt::yip::viewmodels::implementation {
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
    const auto color = m_isRecording ? winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0xE5, 0x48, 0x4D)
                                     : winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x3D, 0x7A, 0xFF);
    return winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{color};
}

void MainViewModel::RefreshDevices()
{
    m_devices.Clear();
    auto devices = ::yip::interop::ListDevices();
    int defaultIdx = -1;
    int idx = 0;
    for (auto& d : devices) {
        auto entry = winrt::make<DeviceEntry>(winrt::hstring{d.id}, winrt::hstring{d.name},
                                              winrt::hstring{d.isCapture ? kMicGlyph : kSpeakerGlyph},
                                              d.isCapture, d.isDefault);
        m_devices.Append(entry);
        if (d.isDefault && defaultIdx < 0 && d.isCapture) {
            defaultIdx = idx;
        }
        ++idx;
    }
    if (m_selectedDeviceIndex < 0 && defaultIdx >= 0) {
        m_selectedDeviceIndex = defaultIdx;
        Raise(L"SelectedDeviceIndex");
    }
    Raise(L"CanRecord");
    SetStatus(devices.empty() ? winrt::hstring{L"No devices found"} : winrt::hstring{L"Ready"});
}

void MainViewModel::RefreshRecordings()
{
    m_recordings.Clear();
    const auto& folder = m_settings.output_folder;
    std::error_code ec;
    if (!fs::exists(folder, ec)) return;

    struct Row {
        fs::path path;
        fs::file_time_type modified;
        std::chrono::milliseconds duration;
    };
    std::vector<Row> rows;

    for (const auto& it : fs::directory_iterator(folder, ec)) {
        if (ec) break;
        if (!it.is_regular_file()) continue;
        const auto& p = it.path();
        if (p.extension() != L".wav") continue;

        Row r;
        r.path = p;
        r.modified = fs::last_write_time(p, ec);
        // Read the real fmt/data chunks. Deriving duration from file size and
        // an assumed 48 kHz stereo float32 is wrong for every other format,
        // and the format is now user-selectable.
        if (const auto info = ::yip::ProbeWav(p)) {
            r.duration = info->Duration();
        } else {
            r.duration = std::chrono::milliseconds{0};
        }
        rows.push_back(std::move(r));
    }

    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.modified > b.modified; });

    for (auto& r : rows) {
        auto entry = winrt::make<RecordingEntry>(winrt::hstring{r.path.wstring()},
                                                 winrt::hstring{r.path.filename().wstring()},
                                                 FormatDuration(r.duration), FormatModified(r.modified));
        m_recordings.Append(entry);
    }
}

void MainViewModel::PollPeak()
{
    const float p = rec_peak_level();
    if (std::abs(p - m_peakLevel) < 1e-4f) return;
    m_peakLevel = p;
    m_peakLabel = FormatDbFromAmplitude(p);
    Raise(L"PeakLevel");
    Raise(L"PeakLabel");
}

void MainViewModel::ToggleRecording()
{
    if (m_isRecording) {
        const auto status = rec_stop();
        m_isRecording = false;
        m_activeRecordingPath.reset();
        if (status != REC_STATUS_OK) {
            const auto* err = rec_last_error();
            std::wstring msg = L"Stop failed";
            if (err) {
                const int n = ::MultiByteToWideChar(CP_UTF8, 0, err, -1, nullptr, 0);
                if (n > 0) {
                    msg.resize(static_cast<size_t>(n) - 1);
                    ::MultiByteToWideChar(CP_UTF8, 0, err, -1, msg.data(), n);
                }
            }
            SetStatus(winrt::hstring{msg});
        } else {
            SetStatus(L"Stopped");
        }
        Raise(L"IsRecording");
        Raise(L"RecordButtonText");
        Raise(L"RecordButtonBrush");
        RefreshRecordings();
        return;
    }

    if (m_selectedDeviceIndex < 0 || static_cast<uint32_t>(m_selectedDeviceIndex) >= m_devices.Size()) {
        SetStatus(L"No device selected");
        return;
    }
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

    const auto status = rec_start(idUtf8.c_str(), pathUtf8.c_str(), cfg);
    if (status != REC_STATUS_OK) {
        const auto* err = rec_last_error();
        std::wstring msg = L"Start failed";
        if (err) {
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, err, -1, nullptr, 0);
            if (n > 0) {
                msg.resize(static_cast<size_t>(n) - 1);
                ::MultiByteToWideChar(CP_UTF8, 0, err, -1, msg.data(), n);
            }
        }
        SetStatus(winrt::hstring{msg});
        return;
    }

    m_isRecording = true;
    m_activeRecordingPath = path;
    SetStatus(L"Recording…");
    Raise(L"IsRecording");
    Raise(L"RecordButtonText");
    Raise(L"RecordButtonBrush");
}

void MainViewModel::ApplySettings(winrt::hstring const& folder, uint32_t sampleRate, uint16_t channels,
                                  uint32_t hotkeyMods, uint32_t hotkeyVk)
{
    m_settings.output_folder = std::wstring{folder};
    m_settings.sample_rate = sampleRate;
    m_settings.channels = channels;
    if (::yip::IsValidHotkey(hotkeyMods, hotkeyVk)) {
        m_settings.hotkey_mods = hotkeyMods;
        m_settings.hotkey_vk = hotkeyVk;
    }

    std::error_code ec;
    fs::create_directories(m_settings.output_folder, ec);
    (void)m_settings.Save();

    Raise(L"OutputFolder");
    Raise(L"SampleRate");
    Raise(L"Channels");
    Raise(L"HotkeyMods");
    Raise(L"HotkeyVk");
    Raise(L"HotkeyLabel");
    RefreshRecordings();
    SetStatus(L"Settings saved");
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
    SetStatus(winrt::hstring{L"Hotkey " + ::yip::FormatHotkey(m_settings.hotkey_mods, m_settings.hotkey_vk) +
                             L" is already taken by another app"});
}

void MainViewModel::RevealRecording(winrt::yip::viewmodels::RecordingEntry const& entry)
{
    if (!entry) return;
    std::wstring args = L"/select,\"";
    args += entry.FullPath().c_str();
    args += L"\"";
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
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
    wcsftime(name, std::size(name), L"yip-%Y%m%d-%H%M%S.wav", &tm);
    return m_settings.output_folder / name;
}

void MainViewModel::SetStatus(winrt::hstring const& s)
{
    if (m_statusText == s) return;
    m_statusText = s;
    Raise(L"StatusText");
}
} // namespace winrt::yip::viewmodels::implementation
