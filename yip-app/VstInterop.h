#pragma once

// C++ RAII wrapper over the vst_host C ABI. Owns plugin handles, exposes
// parameter metadata + a normalized value vector per plugin, and runs the
// offline render. No WinRT types here — callers marshal to the UI thread.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "vst_host.h"

namespace yip::vst {

struct ParamInfo {
    uint32_t id{};
    std::wstring name;
    std::wstring unit;
    double default_normalized{ 0.0 };
    double min_plain{ 0.0 };
    double max_plain{ 1.0 };
    int32_t step_count{ 0 };
};

// A loaded plugin plus the current (editable) normalized value per parameter.
struct LoadedPlugin {
    void* handle{ nullptr };
    std::wstring path;
    std::wstring display_name;       // filename stem
    std::vector<ParamInfo> params;
    std::vector<double> values;      // normalized 0..1, parallel to params

    bool valid() const noexcept { return handle != nullptr; }
};

// Load a .vst3 (or, without SDK, any path → synthetic plugin). nullopt on error.
std::optional<LoadedPlugin> Load(const std::wstring& path);

// Release the handle. Safe on null.
void Unload(LoadedPlugin& plugin);

// Render input → output through the ordered chain. `progress` is called with
// 0..1 from the calling thread (run this on a background thread). Returns true
// on success; on failure, `error_out` (if non-null) receives the message.
bool Render(const std::wstring& input_wav, const std::wstring& output_wav,
            const std::vector<LoadedPlugin*>& chain,
            const std::function<void(float)>& progress,
            std::wstring* error_out);

}  // namespace yip::vst
