// host.cpp — plugin lifetime + parameter introspection. M5.
//
// For M1 we expose only the smoke-test symbol so the wiring across CMake +
// MSBuild can be verified end-to-end.

#include "vst_host.h"

extern "C" int32_t yip_vst_dummy(void)
{
    return 7;
}
