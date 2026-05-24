// vst_host.h — C ABI surface for the offline VST3 host.
//
// Only C-compatible types cross this boundary. No C++ headers in the public
// surface so the WinUI 3 application can include this freely.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Return values from host operations. 0 == success.
typedef enum YipVstStatus {
    YIP_VST_OK = 0,
    YIP_VST_INVALID_ARG = 1,
    YIP_VST_LOAD_FAILED = 2,
    YIP_VST_FORMAT_MISMATCH = 3,
    YIP_VST_RENDER_FAILED = 4,
    YIP_VST_UNKNOWN = 99
} YipVstStatus;

// Smoke-test symbol. Returns 7. Lets the build wiring be verified before any
// VST3 SDK is integrated. Removed once render path is live.
int32_t yip_vst_dummy(void);

// --- placeholders, filled out in M5 ---
//
// YipVstStatus yip_vst_load(const wchar_t* path, void** out_handle);
// YipVstStatus yip_vst_unload(void* handle);
// YipVstStatus yip_vst_render(
//     const wchar_t* input_wav,
//     const wchar_t* output_wav,
//     void** chain,
//     uint32_t chain_len,
//     void (*progress_cb)(float, void*),
//     void* user
// );

#ifdef __cplusplus
} // extern "C"
#endif
