// vst_host.h — C ABI surface for the offline VST3 host.
//
// Only C-compatible types cross this boundary. No C++ headers in the public
// surface so the WinUI 3 application can include this freely. Strings are
// fixed-size UTF-16 buffers to avoid alloc/free ownership across the boundary.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Return values from host operations. 0 == success.
typedef enum YipVstStatus {
    YIP_VST_OK              = 0,
    YIP_VST_INVALID_ARG     = 1,
    YIP_VST_LOAD_FAILED     = 2,
    YIP_VST_FORMAT_MISMATCH = 3,
    YIP_VST_RENDER_FAILED   = 4,
    YIP_VST_IO             = 5,
    YIP_VST_NO_SDK         = 6,  // built without the VST3 SDK (passthrough only)
    YIP_VST_UNKNOWN        = 99
} YipVstStatus;

// Parameter descriptor. Plain (real-world) range plus normalized default.
typedef struct YipVstParam {
    uint32_t id;
    wchar_t  name[128];
    wchar_t  unit[32];
    double   default_normalized;  // 0..1
    double   min_plain;
    double   max_plain;
    int32_t  step_count;          // 0 = continuous
} YipVstParam;

// One parameter override applied during render (normalized 0..1).
typedef struct YipVstParamValue {
    uint32_t param_id;
    double   normalized;
} YipVstParamValue;

// One node in the render chain: a loaded plugin + its parameter overrides.
typedef struct YipVstPluginSetting {
    void*                    handle;       // from yip_vst_load
    const YipVstParamValue*  params;       // may be null when param_count == 0
    uint32_t                 param_count;
} YipVstPluginSetting;

// Render progress in [0,1]. Invoked from the render worker thread.
typedef void (*YipVstProgress)(float progress, void* user);

// Smoke-test symbol. Returns 7.
int32_t yip_vst_dummy(void);

// Load a VST3 plugin. On success writes an opaque handle to *out_handle.
// When built without the SDK, returns a placeholder handle (so the UI flow
// and passthrough render remain usable) and the status is YIP_VST_OK.
YipVstStatus yip_vst_load(const wchar_t* path, void** out_handle);

// Release a handle from yip_vst_load. Null handle is a no-op.
YipVstStatus yip_vst_unload(void* handle);

// Parameter introspection.
YipVstStatus yip_vst_param_count(void* handle, uint32_t* out_count);
YipVstStatus yip_vst_param_at(void* handle, uint32_t index, YipVstParam* out);

// Offline render: input WAV → ordered plugin chain → output WAV (float32).
// Without the SDK this is an identity passthrough (copy with re-encode) so the
// end-to-end pipeline can be exercised; status is YIP_VST_OK.
YipVstStatus yip_vst_render(const wchar_t* input_wav, const wchar_t* output_wav,
                            const YipVstPluginSetting* chain, uint32_t chain_len,
                            YipVstProgress progress_cb, void* user);

// Thread-local last error message. Valid until the next call on this thread.
const wchar_t* yip_vst_last_error(void);

#ifdef __cplusplus
}  // extern "C"
#endif
