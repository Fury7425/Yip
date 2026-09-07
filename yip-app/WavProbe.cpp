#include "pch.h"
#include "WavProbe.h"

#include <cstdio>
#include <cstring>

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
            if (chunk_size > 16) {
                if (_fseeki64(f.get(), chunk_size - 16, SEEK_CUR) != 0) return std::nullopt;
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
