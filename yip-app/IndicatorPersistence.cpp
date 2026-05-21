#include "pch.h"
#include "IndicatorPersistence.h"

#include <winrt/Windows.Data.Json.h>

#include <fstream>
#include <sstream>

namespace fs  = std::filesystem;
namespace wdj = winrt::Windows::Data::Json;

namespace
{
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

    yip::DockEdge ParseEdge(uint32_t v)
    {
        switch (v) {
            case 1: return yip::DockEdge::Top;
            case 2: return yip::DockEdge::Bottom;
            case 3: return yip::DockEdge::Left;
            case 4: return yip::DockEdge::Right;
            default: return yip::DockEdge::None;
        }
    }
}

namespace yip
{
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
        wdj::JsonObject obj{ nullptr };
        if (!wdj::JsonObject::TryParse(wide, obj)) return s;

        if (obj.HasKey(L"dock_edge")) {
            s.dock_edge = ParseEdge(static_cast<uint32_t>(obj.GetNamedNumber(L"dock_edge", 1.0)));
        }
        if (obj.HasKey(L"monitor_id")) {
            s.monitor_id = static_cast<uint64_t>(obj.GetNamedNumber(L"monitor_id", 0.0));
        }
        if (obj.HasKey(L"edge_offset")) {
            s.edge_offset = obj.GetNamedNumber(L"edge_offset", 0.5);
        }
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
        obj.SetNamedValue(L"dock_edge",
            wdj::JsonValue::CreateNumberValue(static_cast<double>(dock_edge)));
        obj.SetNamedValue(L"monitor_id",
            wdj::JsonValue::CreateNumberValue(static_cast<double>(monitor_id)));
        obj.SetNamedValue(L"edge_offset",
            wdj::JsonValue::CreateNumberValue(edge_offset));
        obj.SetNamedValue(L"click_through",
            wdj::JsonValue::CreateBooleanValue(click_through));
        obj.SetNamedValue(L"last_expanded",
            wdj::JsonValue::CreateBooleanValue(last_expanded));

        const auto path = FilePath();
        const auto tmp  = path.wstring() + L".tmp";
        const auto u8   = winrt::to_string(obj.Stringify());
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write(u8.data(), static_cast<std::streamsize>(u8.size()));
            if (!out) return false;
        }
        return ::MoveFileExW(tmp.c_str(), path.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
}
