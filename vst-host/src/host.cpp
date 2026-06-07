// host.cpp — plugin lifetime + parameter introspection (C ABI).
//
// SDK-gated. Without YIP_HAVE_VST3_SDK the loader still returns a usable
// handle carrying synthetic parameters, so the M6 UI + passthrough render
// work end-to-end before the Steinberg SDK is dropped in.

#include "vst_host.h"
#include "vst_internal.h"

#include <cwchar>
#include <memory>
#include <new>

namespace yip::vst {

namespace {
    thread_local std::wstring g_last_error;

    void copy_field(wchar_t* dst, size_t cap, const wchar_t* src) {
        if (!dst || cap == 0) return;
        if (!src) { dst[0] = L'\0'; return; }
        std::wcsncpy(dst, src, cap - 1);
        dst[cap - 1] = L'\0';
    }

    // Synthetic params used when no SDK is present — lets the UI exercise
    // sliders/edits against a deterministic set.
    std::vector<YipVstParam> synthetic_params() {
        std::vector<YipVstParam> p;
        YipVstParam gain{};
        gain.id = 0;
        copy_field(gain.name, 128, L"Gain");
        copy_field(gain.unit, 32, L"dB");
        gain.default_normalized = 0.5;
        gain.min_plain = -24.0;
        gain.max_plain = 24.0;
        gain.step_count = 0;
        p.push_back(gain);

        YipVstParam mix{};
        mix.id = 1;
        copy_field(mix.name, 128, L"Mix");
        copy_field(mix.unit, 32, L"%");
        mix.default_normalized = 1.0;
        mix.min_plain = 0.0;
        mix.max_plain = 100.0;
        mix.step_count = 0;
        p.push_back(mix);
        return p;
    }
}  // namespace

void set_last_error(std::wstring msg) { g_last_error = std::move(msg); }

}  // namespace yip::vst

using yip::vst::Plugin;

extern "C" int32_t yip_vst_dummy(void) { return 7; }

extern "C" YipVstStatus yip_vst_load(const wchar_t* path, void** out_handle) {
    if (!path || !out_handle) {
        yip::vst::set_last_error(L"yip_vst_load: null argument");
        return YIP_VST_INVALID_ARG;
    }
    *out_handle = nullptr;

    auto plugin = std::make_unique<Plugin>();
    plugin->path = path;

#ifdef YIP_HAVE_VST3_SDK
    // M5 SDK path: load module, instantiate component + controller, connect,
    // and enumerate IEditController parameters into plugin->params.
    // Implemented when the Steinberg SDK is present under third_party/.
    if (!yip::vst::load_with_sdk(*plugin)) {
        return YIP_VST_LOAD_FAILED;  // load_with_sdk sets last_error
    }
#else
    plugin->params = yip::vst::synthetic_params();
#endif

    *out_handle = plugin.release();
    return YIP_VST_OK;
}

extern "C" YipVstStatus yip_vst_unload(void* handle) {
    if (!handle) return YIP_VST_OK;
    auto* plugin = static_cast<Plugin*>(handle);
#ifdef YIP_HAVE_VST3_SDK
    yip::vst::unload_with_sdk(*plugin);
#endif
    delete plugin;
    return YIP_VST_OK;
}

extern "C" YipVstStatus yip_vst_param_count(void* handle, uint32_t* out_count) {
    if (!handle || !out_count) return YIP_VST_INVALID_ARG;
    *out_count = static_cast<uint32_t>(static_cast<Plugin*>(handle)->params.size());
    return YIP_VST_OK;
}

extern "C" YipVstStatus yip_vst_param_at(void* handle, uint32_t index, YipVstParam* out) {
    if (!handle || !out) return YIP_VST_INVALID_ARG;
    auto* plugin = static_cast<Plugin*>(handle);
    if (index >= plugin->params.size()) return YIP_VST_INVALID_ARG;
    *out = plugin->params[index];
    return YIP_VST_OK;
}

extern "C" const wchar_t* yip_vst_last_error(void) {
    return yip::vst::g_last_error.empty() ? nullptr : yip::vst::g_last_error.c_str();
}
