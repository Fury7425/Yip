#include "pch.h"
#include "WavProbe.h"

// INITGUID so the PKEY_* values are defined here (DECLSPEC_SELECTANY) rather
// than left for a library that may not carry them.
#include <initguid.h>
#include <propkey.h>
#include <propvarutil.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>

namespace {
struct FileCloser {
    void operator()(std::FILE* f) const noexcept
    {
        if (f) std::fclose(f);
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

bool ReadExact(std::FILE* f, void* dst, size_t n)
{
    return std::fread(dst, 1, n, f) == n;
}

constexpr uint16_t kWaveFormatIeeeFloat = 3;
constexpr uint16_t kWaveFormatExtensible = 0xFFFE;

// First four bytes of KSDATAFORMAT_SUBTYPE_IEEE_FLOAT; the rest of the GUID is
// the tail every audio sub-format shares.
constexpr uint32_t kSubtypeFloatTag = 3;

std::optional<uint32_t> ReadUInt32(IPropertyStore* store, REFPROPERTYKEY key)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (FAILED(store->GetValue(key, &pv))) return std::nullopt;
    ULONG value = 0;
    const bool ok = SUCCEEDED(PropVariantToUInt32(pv, &value));
    PropVariantClear(&pv);
    if (!ok) return std::nullopt;
    return value;
}

std::optional<uint64_t> ReadUInt64(IPropertyStore* store, REFPROPERTYKEY key)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (FAILED(store->GetValue(key, &pv))) return std::nullopt;
    ULONGLONG value = 0;
    const bool ok = SUCCEEDED(PropVariantToUInt64(pv, &value));
    PropVariantClear(&pv);
    if (!ok) return std::nullopt;
    return value;
}

// FLAC, MP3 and M4A through the shell's own media property handlers, so no
// container parser lives in the app. Only headers are read.
std::optional<yip::AudioInfo> ProbeShell(const std::filesystem::path& path, std::wstring_view ext)
{
    winrt::com_ptr<IPropertyStore> store;
    if (FAILED(::SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT,
                                                   IID_PPV_ARGS(store.put())))) {
        return std::nullopt;
    }

    yip::AudioInfo info{};
    info.sample_rate = ReadUInt32(store.get(), PKEY_Audio_SampleRate).value_or(0);
    info.channels = static_cast<uint16_t>(ReadUInt32(store.get(), PKEY_Audio_ChannelCount).value_or(0));
    if (const auto hns = ReadUInt64(store.get(), PKEY_Media_Duration)) {
        info.duration = std::chrono::milliseconds{static_cast<int64_t>(*hns / 10000)};
    }

    if (ext == L".flac") {
        info.quality = L"FLAC";
        if (const auto bits = ReadUInt32(store.get(), PKEY_Audio_SampleSize); bits && *bits > 0) {
            info.quality += L" " + std::to_wstring(*bits) + L"-bit";
        }
    } else {
        info.quality = ext == L".mp3" ? L"MP3" : L"M4A";
        if (const auto bps = ReadUInt32(store.get(), PKEY_Audio_EncodingBitrate); bps && *bps > 0) {
            info.quality += L" " + std::to_wstring((*bps + 500) / 1000) + L" kbps";
        }
    }
    return info;
}
} // namespace

namespace yip {
std::chrono::milliseconds WavInfo::Duration() const noexcept
{
    const uint64_t frame_bytes = static_cast<uint64_t>(channels) * (bits_per_sample / 8u);
    if (sample_rate == 0 || frame_bytes == 0) return std::chrono::milliseconds{0};
    const uint64_t frames = data_bytes / frame_bytes;
    return std::chrono::milliseconds{static_cast<int64_t>((frames * 1000ull) / sample_rate)};
}

std::optional<WavInfo> ProbeWav(const std::filesystem::path& path)
{
    std::FILE* raw = nullptr;
    if (_wfopen_s(&raw, path.c_str(), L"rb") != 0 || !raw) return std::nullopt;
    FilePtr f{raw};

    char tag[4]{};
    uint32_t riff_size = 0;
    if (!ReadExact(f.get(), tag, 4) || std::memcmp(tag, "RIFF", 4) != 0) return std::nullopt;
    if (!ReadExact(f.get(), &riff_size, 4)) return std::nullopt;
    if (!ReadExact(f.get(), tag, 4) || std::memcmp(tag, "WAVE", 4) != 0) return std::nullopt;

    WavInfo info{};
    bool have_fmt = false;

    while (ReadExact(f.get(), tag, 4)) {
        uint32_t chunk_size = 0;
        if (!ReadExact(f.get(), &chunk_size, 4)) break;

        if (std::memcmp(tag, "fmt ", 4) == 0) {
            // First 16 bytes of a fmt chunk are fixed; anything past that is
            // the EXTENSIBLE tail, which duration does not need.
            if (chunk_size < 16) return std::nullopt;
            uint16_t audio_format = 0;
            uint32_t byte_rate = 0;
            uint16_t block_align = 0;
            if (!ReadExact(f.get(), &audio_format, 2)) return std::nullopt;
            if (!ReadExact(f.get(), &info.channels, 2)) return std::nullopt;
            if (!ReadExact(f.get(), &info.sample_rate, 4)) return std::nullopt;
            if (!ReadExact(f.get(), &byte_rate, 4)) return std::nullopt;
            if (!ReadExact(f.get(), &block_align, 2)) return std::nullopt;
            if (!ReadExact(f.get(), &info.bits_per_sample, 2)) return std::nullopt;
            have_fmt = true;
            info.is_float = audio_format == kWaveFormatIeeeFloat;

            uint32_t consumed = 16;
            // EXTENSIBLE tail: cbSize(2) validBits(2) channelMask(4) SubFormat(16).
            // hound writes it for anything wider than 16 bits, float included.
            if (audio_format == kWaveFormatExtensible && chunk_size >= 40) {
                uint8_t tail[8]{};
                uint32_t sub_tag = 0;
                if (!ReadExact(f.get(), tail, sizeof(tail))) return std::nullopt;
                if (!ReadExact(f.get(), &sub_tag, 4)) return std::nullopt;
                info.is_float = sub_tag == kSubtypeFloatTag;
                consumed += 12;
            }
            if (chunk_size > consumed) {
                if (_fseeki64(f.get(), chunk_size - consumed, SEEK_CUR) != 0) return std::nullopt;
            }
        } else if (std::memcmp(tag, "data", 4) == 0) {
            if (!have_fmt) return std::nullopt;
            info.data_bytes = chunk_size;
            return info;
        } else {
            if (_fseeki64(f.get(), chunk_size, SEEK_CUR) != 0) return std::nullopt;
        }
        // RIFF chunks are word-aligned: an odd size carries a pad byte.
        if ((chunk_size & 1u) != 0) {
            if (_fseeki64(f.get(), 1, SEEK_CUR) != 0) return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<AudioInfo> ProbeAudio(const std::filesystem::path& path)
{
    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });

    if (ext != L".wav") return ProbeShell(path, ext);

    const auto wav = ProbeWav(path);
    if (!wav) return std::nullopt;
    AudioInfo info{};
    info.sample_rate = wav->sample_rate;
    info.channels = wav->channels;
    info.duration = wav->Duration();
    info.quality = L"WAV " + std::to_wstring(wav->bits_per_sample) + (wav->is_float ? L"-bit float" : L"-bit");
    return info;
}

std::string ToUtf8(std::wstring_view w)
{
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                        nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                          nullptr);
    return out;
}
} // namespace yip
