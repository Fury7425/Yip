#pragma once

// Header probes for the recordings list, which needs a real duration and
// format: sizing from `file_size / (48000 * 2 * 4)` is wrong for every
// recording that is not 48 kHz stereo float32. WAV is read directly; FLAC, MP3
// and M4A go through the shell property store.

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
    // IEEE float samples, from the format tag or the EXTENSIBLE sub-format.
    bool is_float{false};
    uint64_t data_bytes{0};

    // Duration from the header. Zero when the header is unusable.
    std::chrono::milliseconds Duration() const noexcept;
};

// Reads only the chunk headers, never the audio. Returns nullopt when the file
// is missing, truncated, or not a WAVE.
std::optional<WavInfo> ProbeWav(const std::filesystem::path& path);

// What the recordings list shows for a take in any format Yip writes.
struct AudioInfo {
    uint32_t sample_rate{0};
    uint16_t channels{0};
    std::wstring quality; // "WAV 24-bit", "FLAC 16-bit", "MP3 192 kbps"
    std::chrono::milliseconds duration{0};
};

// Dispatches on the extension. Returns nullopt when nothing usable is known.
std::optional<AudioInfo> ProbeAudio(const std::filesystem::path& path);

// Convert a wide string to UTF-8. `std::filesystem::path::string()` uses the
// process ANSI codepage on Windows and mangles (or throws on) any path outside
// it — the FFI boundary expects UTF-8, so every path crossing it uses this.
std::string ToUtf8(std::wstring_view w);
} // namespace yip
