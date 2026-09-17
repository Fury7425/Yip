#pragma once

// The recording formats Settings offers and what each means for file names
// and labels. The numeric values are audio-core's REC_FORMAT_* and are what
// settings.json stores; audio-core validates the combination again in
// rec_start, so this only has to keep the UI honest.

#include <cstdint>
#include <string>
#include <string_view>

namespace yip::audiofmt {
inline constexpr uint16_t kWav = 0;
inline constexpr uint16_t kFlac = 1;
inline constexpr uint16_t kMp3 = 2;
inline constexpr uint16_t kM4a = 3;

inline constexpr uint16_t kDefaultBitDepth = 32;
inline constexpr uint16_t kDefaultKbps = 192;

inline bool IsLossy(uint16_t format) noexcept
{
    return format == kMp3 || format == kM4a;
}

// File extension for a new take, dot included.
inline const wchar_t* Extension(uint16_t format) noexcept
{
    switch (format) {
        case kFlac:
            return L".flac";
        case kMp3:
            return L".mp3";
        case kM4a:
            return L".m4a";
        default:
            return L".wav";
    }
}

// True for every extension Yip writes. `ext` is lower-case, dot included.
inline bool IsRecordingExtension(std::wstring_view ext) noexcept
{
    return ext == L".wav" || ext == L".flac" || ext == L".mp3" || ext == L".m4a";
}

// The MP3 and AAC encoders only take 44.1 and 48 kHz; audio-core records any
// other rate at 48 kHz for them. Mirrored here so labels tell the truth.
inline uint32_t CaptureRate(uint16_t format, uint32_t sampleRate) noexcept
{
    if (IsLossy(format) && sampleRate != 44100 && sampleRate != 48000) return 48000;
    return sampleRate;
}

inline bool IsMp3Bitrate(uint16_t kbps) noexcept
{
    return kbps == 128 || kbps == 192 || kbps == 256 || kbps == 320;
}

inline bool IsAacBitrate(uint16_t kbps) noexcept
{
    return kbps == 96 || kbps == 128 || kbps == 160 || kbps == 192;
}

// Pull a stored combination back to one the dialog offers, so a hand-edited
// settings.json cannot make every take fail to start. Each field is kept when
// it is valid for *some* format, so switching format and back remembers it.
inline void Normalize(uint16_t& format, uint16_t& bitDepth, uint16_t& kbps) noexcept
{
    if (format > kM4a) format = kWav;

    if (bitDepth != 16 && bitDepth != 24 && bitDepth != 32) bitDepth = kDefaultBitDepth;
    if (format == kFlac && bitDepth == 32) bitDepth = 24;

    if (format == kMp3) {
        if (!IsMp3Bitrate(kbps)) kbps = kDefaultKbps;
    } else if (format == kM4a) {
        if (!IsAacBitrate(kbps)) kbps = kDefaultKbps;
    } else if (!IsMp3Bitrate(kbps) && !IsAacBitrate(kbps)) {
        kbps = kDefaultKbps;
    }
}

// "WAV 32-bit float", "FLAC 24-bit", "MP3 192 kbps".
inline std::wstring Describe(uint16_t format, uint16_t bitDepth, uint16_t kbps)
{
    switch (format) {
        case kFlac:
            return L"FLAC " + std::to_wstring(bitDepth) + L"-bit";
        case kMp3:
            return L"MP3 " + std::to_wstring(kbps) + L" kbps";
        case kM4a:
            return L"M4A " + std::to_wstring(kbps) + L" kbps";
        default:
            if (bitDepth == 32) return L"WAV 32-bit float";
            return L"WAV " + std::to_wstring(bitDepth) + L"-bit";
    }
}
} // namespace yip::audiofmt
