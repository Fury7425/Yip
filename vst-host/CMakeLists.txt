cmake_minimum_required(VERSION 3.27)

project(vst_host
    LANGUAGES CXX
    VERSION 0.1.0
    DESCRIPTION "Offline VST3 host for Yip"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(vst_host STATIC
    src/host.cpp
    src/render.cpp
)

target_include_directories(vst_host
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(vst_host
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
)

if(MSVC)
    target_compile_options(vst_host PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    # _wfopen_s + secure CRT in render.cpp.
    target_compile_definitions(vst_host PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Optional VST3 SDK. Drop the Steinberg SDK under vst-host/third_party/vst3sdk/
# (gitignored) after signing the developer agreement. When present we compile
# the real host path; when absent the C ABI falls back to passthrough render
# so the UI pipeline still builds and runs.
set(YIP_VST3_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk")
if(EXISTS "${YIP_VST3_SDK_DIR}/CMakeLists.txt")
    message(STATUS "vst-host: VST3 SDK found at ${YIP_VST3_SDK_DIR}")
    target_compile_definitions(vst_host PRIVATE YIP_HAVE_VST3_SDK)
    target_include_directories(vst_host PRIVATE "${YIP_VST3_SDK_DIR}")
    # SDK target wiring (sdk, base, pluginterfaces) is added when the SDK is
    # dropped in; kept out of source control here.
    add_subdirectory("${YIP_VST3_SDK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/vst3sdk" EXCLUDE_FROM_ALL)
    target_link_libraries(vst_host PRIVATE sdk)
else()
    message(STATUS "vst-host: VST3 SDK not found — building passthrough host")
endif()

set_target_properties(vst_host PROPERTIES
    OUTPUT_NAME "vst_host"
    POSITION_INDEPENDENT_CODE ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
