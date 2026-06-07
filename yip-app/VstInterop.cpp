#include "pch.h"
#include "VstInterop.h"

#include <filesystem>

namespace yip::vst {

std::optional<LoadedPlugin> Load(const std::wstring& path) {
    void* handle = nullptr;
    if (yip_vst_load(path.c_str(), &handle) != YIP_VST_OK || !handle) {
        return std::nullopt;
    }

    LoadedPlugin p;
    p.handle = handle;
    p.path = path;
    p.display_name = std::filesystem::path(path).stem().wstring();

    uint32_t count = 0;
    yip_vst_param_count(handle, &count);
    p.params.reserve(count);
    p.values.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        YipVstParam raw{};
        if (yip_vst_param_at(handle, i, &raw) != YIP_VST_OK) continue;
        ParamInfo info;
        info.id = raw.id;
        info.name = raw.name;
        info.unit = raw.unit;
        info.default_normalized = raw.default_normalized;
        info.min_plain = raw.min_plain;
        info.max_plain = raw.max_plain;
        info.step_count = raw.step_count;
        p.params.push_back(std::move(info));
        p.values.push_back(raw.default_normalized);
    }
    return p;
}

void Unload(LoadedPlugin& plugin) {
    if (plugin.handle) {
        yip_vst_unload(plugin.handle);
        plugin.handle = nullptr;
    }
}

bool Render(const std::wstring& input_wav, const std::wstring& output_wav,
            const std::vector<LoadedPlugin*>& chain,
            const std::function<void(float)>& progress,
            std::wstring* error_out) {
    // Flatten the chain into the C ABI structs. Storage must outlive the call.
    std::vector<std::vector<YipVstParamValue>> value_storage;
    value_storage.reserve(chain.size());
    std::vector<YipVstPluginSetting> settings;
    settings.reserve(chain.size());

    for (auto* p : chain) {
        if (!p || !p->valid()) continue;
        std::vector<YipVstParamValue> vals;
        vals.reserve(p->params.size());
        for (size_t i = 0; i < p->params.size(); ++i) {
            YipVstParamValue v{};
            v.param_id = p->params[i].id;
            v.normalized = p->values[i];
            vals.push_back(v);
        }
        value_storage.push_back(std::move(vals));
        YipVstPluginSetting s{};
        s.handle = p->handle;
        s.params = value_storage.back().data();
        s.param_count = static_cast<uint32_t>(value_storage.back().size());
        settings.push_back(s);
    }

    // Progress trampoline: C callback → std::function.
    struct Ctx { const std::function<void(float)>* cb; } ctx{ &progress };
    auto trampoline = [](float pr, void* user) {
        auto* c = static_cast<Ctx*>(user);
        if (c && c->cb && *c->cb) (*c->cb)(pr);
    };

    const YipVstStatus st = yip_vst_render(
        input_wav.c_str(), output_wav.c_str(),
        settings.empty() ? nullptr : settings.data(),
        static_cast<uint32_t>(settings.size()),
        trampoline, &ctx);

    if (st != YIP_VST_OK) {
        if (error_out) {
            const wchar_t* e = yip_vst_last_error();
            *error_out = e ? e : L"render failed";
        }
        return false;
    }
    return true;
}

}  // namespace yip::vst
