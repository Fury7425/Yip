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
endif()

# VST3 SDK is wired in at M5. Until then the library only exposes a dummy
# symbol that the app links to verify the toolchain end-to-end.
set_target_properties(vst_host PROPERTIES
    OUTPUT_NAME "vst_host"
    POSITION_INDEPENDENT_CODE ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
