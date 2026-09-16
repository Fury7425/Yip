#include "pch.h"
#include "IndicatorPersistence.h"

#include <winrt/Windows.Data.Json.h>

#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
namespace wdj = winrt::Windows::Data::Json;

namespace {
fs::path LocalAppDataRoot()
{
    wchar_t* base = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base) {
        fs::path p(base);
        ::CoTaskMemFree(base);
        return p / L"Yip";
    }
    return fs::current_path() / L"yip-data";
}
} // namespace

namespace yip {
fs::path IndicatorPersistence::FilePath()
{
    const auto root = LocalAppDataRoot();
    std::error_code ec;
    fs::create_directories(root, ec);
    return root / L"indicator.json";
}

IndicatorPersistence IndicatorPersistence::Load()
{
    IndicatorPersistence s;
    std::ifstream in(FilePath(), std::ios::binary);
    if (!in) return s;

    std::stringstream buf;
    buf << in.rdbuf();
    const auto u8 = buf.str();
    if (u8.empty()) return s;

    const auto wide = winrt::to_hstring(u8);
    wdj::JsonObject obj{nullptr};
    if (!wdj::JsonObject::TryParse(wide, obj)) return s;

    if (obj.HasKey(L"click_through")) {
        s.click_through = obj.GetNamedBoolean(L"click_through", false);
    }
    if (obj.HasKey(L"last_expanded")) {
        s.last_expanded = obj.GetNamedBoolean(L"last_expanded", false);
    }
    return s;
}

bool IndicatorPersistence::Save() const
{
    wdj::JsonObject obj;
    obj.SetNamedValue(L"click_through", wdj::JsonValue::CreateBooleanValue(click_through));
    obj.SetNamedValue(L"last_expanded", wdj::JsonValue::CreateBooleanValue(last_expanded));

    const auto path = FilePath();
    const auto tmp = path.wstring() + L".tmp";
    const auto u8 = winrt::to_string(obj.Stringify());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(u8.data(), static_cast<std::streamsize>(u8.size()));
        if (!out) return false;
    }
    return ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
} // namespace yip
