#pragma once

// Minimal RIFF/WAVE header reader. Used for the recordings list, which needs a
// real duration: sizing from `file_size / (48000 * 2 * 4)` is wrong for every
// recording that is not 48 kHz stereo float32.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace yip {
struct WavInfo {
    uint32_t sample_rate{0};
    uint16_t channels{0};
    uint16_t bits_per_sample{0};
    uint64_t data_bytes{0};

    // Duration from the header. Zero when the header is unusable.
    std::chrono::milliseconds Duration() const noexcept;
};

// Reads only the chunk headers, never the audio. Returns nullopt when the file
// is missing, truncated, or not a WAVE.
std::optional<WavInfo> ProbeWav(const std::filesystem::path& path);

// Convert a wide string to UTF-8. `std::filesystem::path::string()` uses the
// process ANSI codepage on Windows and mangles (or throws on) any path outside
// it — the FFI boundary expects UTF-8, so every path crossing it uses this.
std::string ToUtf8(std::wstring_view w);
} // namespace yip
