#pragma once

// Sidecar markers for mark-moment timestamps. Stored at
// `<recording-name>.markers.json` next to the WAV; the WAV stays pristine.
//
// Schema:
//   { "markers": [ { "t_ms": <uint64>, "label": <string|null> } ] }

#include <filesystem>
#include <optional>
#include <string>

namespace yip::markers {
struct Marker {
    uint64_t t_ms{0};
    std::optional<std::wstring> label;
};

// Sidecar path for a WAV at `wav_path`.
std::filesystem::path SidecarFor(const std::filesystem::path& wav_path);

// Append a marker. Creates the sidecar if missing; preserves any existing
// markers + unknown top-level keys. Returns false on filesystem error.
bool Append(const std::filesystem::path& wav_path, const Marker& m);
} // namespace yip::markers
