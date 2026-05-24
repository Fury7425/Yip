#include "pch.h"
#include "Markers.h"

#include <winrt/Windows.Data.Json.h>

#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
namespace wdj = winrt::Windows::Data::Json;

namespace yip::markers {
fs::path SidecarFor(const fs::path& wav_path)
{
    auto p = wav_path;
    p += L".markers.json";
    // Convention: keep the .wav extension visible so collation is obvious:
    //   yip-20260521-103200.wav
    //   yip-20260521-103200.wav.markers.json
    return p;
}

bool Append(const fs::path& wav_path, const Marker& m)
{
    const auto path = SidecarFor(wav_path);

    wdj::JsonObject root;
    wdj::JsonArray arr;

    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::stringstream buf;
        buf << in.rdbuf();
        const auto u8 = buf.str();
        if (!u8.empty()) {
            const auto wide = winrt::to_hstring(u8);
            wdj::JsonObject parsed{nullptr};
            if (wdj::JsonObject::TryParse(wide, parsed)) {
                root = parsed;
                if (parsed.HasKey(L"markers")) {
                    arr = parsed.GetNamedArray(L"markers");
                }
            }
        }
    }
    if (!arr) arr = wdj::JsonArray{};

    wdj::JsonObject entry;
    entry.SetNamedValue(L"t_ms", wdj::JsonValue::CreateNumberValue(static_cast<double>(m.t_ms)));
    if (m.label) {
        entry.SetNamedValue(L"label", wdj::JsonValue::CreateStringValue(*m.label));
    } else {
        entry.SetNamedValue(L"label", wdj::JsonValue::CreateNullValue());
    }
    arr.Append(entry);
    root.SetNamedValue(L"markers", arr);

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    const auto tmp = path.wstring() + L".tmp";
    const auto u8 = winrt::to_string(root.Stringify());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(u8.data(), static_cast<std::streamsize>(u8.size()));
        if (!out) return false;
    }
    return ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
} // namespace yip::markers
