#include "pch.h"
#include "Settings.h"

#include <winrt/Windows.Data.Json.h>

#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;
namespace wdj = winrt::Windows::Data::Json;

namespace
{
    fs::path LocalAppData()
    {
        wchar_t* base = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base) {
            fs::path p(base);
            ::CoTaskMemFree(base);
            return p / L"Yip";
        }
        return fs::current_path() / L"yip-data";
    }

    fs::path DefaultOutputFolder()
    {
        wchar_t* base = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &base)) && base) {
            fs::path p(base);
            ::CoTaskMemFree(base);
            return p / L"Yip";
        }
        return fs::current_path() / L"Recordings";
    }
}

namespace yip
{
    fs::path Settings::SettingsPath()
    {
        const auto root = LocalAppData();
        std::error_code ec;
        fs::create_directories(root, ec);
        return root / L"settings.json";
    }

    Settings Settings::Defaults()
    {
        Settings s;
        s.output_folder = DefaultOutputFolder();
        s.sample_rate = 48000;
        s.channels = 2;
        s.format = 0;
        return s;
    }

    Settings Settings::Load()
    {
        const auto path = SettingsPath();
        std::ifstream in(path, std::ios::binary);
        if (!in) return Defaults();

        std::stringstream buf;
        buf << in.rdbuf();
        const auto u8 = buf.str();
        if (u8.empty()) return Defaults();

        const auto wide = winrt::to_hstring(u8);

        wdj::JsonObject obj{ nullptr };
        if (!wdj::JsonObject::TryParse(wide, obj)) return Defaults();

        Settings s = Defaults();
        if (obj.HasKey(L"output_folder")) {
            const auto folder = obj.GetNamedString(L"output_folder", L"");
            if (!folder.empty()) s.output_folder = std::wstring{ folder };
        }
        if (obj.HasKey(L"sample_rate")) {
            s.sample_rate = static_cast<uint32_t>(obj.GetNamedNumber(L"sample_rate", 48000.0));
        }
        if (obj.HasKey(L"channels")) {
            s.channels = static_cast<uint16_t>(obj.GetNamedNumber(L"channels", 2.0));
        }
        if (obj.HasKey(L"format")) {
            s.format = static_cast<uint16_t>(obj.GetNamedNumber(L"format", 0.0));
        }
        return s;
    }

    bool Settings::Save() const
    {
        wdj::JsonObject obj;
        obj.SetNamedValue(L"output_folder",
            wdj::JsonValue::CreateStringValue(output_folder.wstring()));
        obj.SetNamedValue(L"sample_rate",
            wdj::JsonValue::CreateNumberValue(static_cast<double>(sample_rate)));
        obj.SetNamedValue(L"channels",
            wdj::JsonValue::CreateNumberValue(static_cast<double>(channels)));
        obj.SetNamedValue(L"format",
            wdj::JsonValue::CreateNumberValue(static_cast<double>(format)));

        const auto path = SettingsPath();
        const auto tmp  = path;
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);

        const auto wide = obj.Stringify();
        const auto u8 = winrt::to_string(wide);
        const auto tmpPath = path.wstring() + L".tmp";

        {
            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write(u8.data(), static_cast<std::streamsize>(u8.size()));
            if (!out) return false;
        }
        // ReplaceFile-style atomic rename. Falls back to remove+rename.
        if (!::MoveFileExW(tmpPath.c_str(), path.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return false;
        }
        return true;
    }
}
