// vst_internal.h — host-private types shared by host.cpp + render.cpp.
// Not part of the public C ABI.

#pragma once

#include "vst_host.h"

#include <string>
#include <vector>

namespace yip::vst {

// Thread-local error sink for the C ABI.
void set_last_error(std::wstring msg);

// Opaque plugin handle. Without the SDK it carries only the path + a small
// set of synthetic parameters so the UI has something to render. With the SDK
// (#ifdef YIP_HAVE_VST3_SDK) it additionally owns the live component pointers.
struct Plugin {
    std::wstring path;
    std::vector<YipVstParam> params;

#ifdef YIP_HAVE_VST3_SDK
    // M5 SDK path: live Steinberg component handles live here. Declared as
    // void* to keep this header SDK-free; the .cpp casts them back.
    void* module = nullptr;     // VST3::Hosting::Module
    void* component = nullptr;  // Steinberg::Vst::IComponent*
    void* controller = nullptr; // Steinberg::Vst::IEditController*
    void* processor = nullptr;  // Steinberg::Vst::IAudioProcessor*
#endif
};

}  // namespace yip::vst
