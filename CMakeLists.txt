cmake_minimum_required(VERSION 3.27)

project(yip
    LANGUAGES CXX
    VERSION 0.1.0
    DESCRIPTION "Yip — native Windows audio recorder"
)

# ----------------------------------------------------------------------------
# Global config
# ----------------------------------------------------------------------------

if(NOT WIN32)
    message(FATAL_ERROR "Yip is Windows-only. Configure on a Windows host.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Resolve platform = Win32 (x64) or ARM64 for MSBuild orchestration.
if(CMAKE_GENERATOR_PLATFORM)
    set(YIP_PLATFORM ${CMAKE_GENERATOR_PLATFORM})
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    set(YIP_PLATFORM "ARM64")
else()
    set(YIP_PLATFORM "x64")
endif()

set(YIP_CONFIG $<IF:$<CONFIG:Debug>,Debug,Release>)

# ----------------------------------------------------------------------------
# audio-core (Rust staticlib) via cargo
# ----------------------------------------------------------------------------

find_program(CARGO_EXE cargo REQUIRED
    HINTS "$ENV{USERPROFILE}/.cargo/bin"
    DOC "Rust cargo executable (install via rustup)."
)

set(AUDIO_CORE_DIR ${CMAKE_SOURCE_DIR}/audio-core)
set(AUDIO_CORE_TARGET_DIR ${CMAKE_BINARY_DIR}/cargo-target)
set(AUDIO_CORE_PROFILE_DIR $<IF:$<CONFIG:Debug>,debug,release>)

# Always build with an explicit --target so the ARM64 preset cross-compiles
# instead of silently producing an x64 .lib that fails to link. An explicit
# --target also nests the output under the triple, so the path below must
# include it for both platforms.
if(YIP_PLATFORM STREQUAL "ARM64")
    set(RUST_TARGET_TRIPLE "aarch64-pc-windows-msvc")
else()
    set(RUST_TARGET_TRIPLE "x86_64-pc-windows-msvc")
endif()

set(AUDIO_CORE_LIB
    ${AUDIO_CORE_TARGET_DIR}/${RUST_TARGET_TRIPLE}/${AUDIO_CORE_PROFILE_DIR}/audio_core.lib)
set(AUDIO_CORE_HEADER ${AUDIO_CORE_DIR}/include/audio_core.h)

set(CARGO_PROFILE_FLAG $<IF:$<CONFIG:Debug>,--profile=dev,--release>)

add_custom_target(audio_core_build ALL
    COMMAND ${CARGO_EXE} build ${CARGO_PROFILE_FLAG}
            --manifest-path "${AUDIO_CORE_DIR}/Cargo.toml"
            --target ${RUST_TARGET_TRIPLE}
            --target-dir "${AUDIO_CORE_TARGET_DIR}"
    BYPRODUCTS ${AUDIO_CORE_LIB} ${AUDIO_CORE_HEADER}
    WORKING_DIRECTORY ${AUDIO_CORE_DIR}
    COMMENT "cargo build audio-core (${RUST_TARGET_TRIPLE})"
    VERBATIM
)

# Imported target so vst-host or yip-app can link via target_link_libraries.
add_library(audio_core STATIC IMPORTED GLOBAL)
set_target_properties(audio_core PROPERTIES
    IMPORTED_LOCATION ${AUDIO_CORE_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${AUDIO_CORE_DIR}/include
)
add_dependencies(audio_core audio_core_build)

# ----------------------------------------------------------------------------
# vst-host (C++ static lib)
# ----------------------------------------------------------------------------

add_subdirectory(vst-host)

# ----------------------------------------------------------------------------
# yip-app (WinUI 3 via MSBuild .vcxproj)
# ----------------------------------------------------------------------------

find_program(MSBUILD_EXE msbuild
    HINTS
        "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin"
        "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Professional/MSBuild/Current/Bin"
        "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Enterprise/MSBuild/Current/Bin"
        "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin"
    DOC "MSBuild from Visual Studio 2022"
    REQUIRED
)

set(YIP_APP_DIR ${CMAKE_SOURCE_DIR}/yip-app)
set(YIP_APP_PROJ ${YIP_APP_DIR}/yip-app.vcxproj)
set(YIP_APP_OUT ${CMAKE_BINARY_DIR}/yip-app)

# Restore and build are separate msbuild invocations: PackageReference restore
# has to finish before the build reads the generated props. Do not pass
# NuGetTargetMoniker here — the WinUI 3 project globals already resolve it, and
# forcing "UAP,Version=v10.0" makes NuGet's restore reject the version string.
add_custom_target(yip_app ALL
    COMMAND ${MSBUILD_EXE}
        "${YIP_APP_PROJ}"
        -t:Restore
        -p:Configuration=${YIP_CONFIG}
        -p:Platform=${YIP_PLATFORM}
        -v:minimal
        -nologo
    COMMAND ${MSBUILD_EXE}
        "${YIP_APP_PROJ}"
        -t:Build
        -p:Configuration=${YIP_CONFIG}
        -p:Platform=${YIP_PLATFORM}
        -p:OutDir=${YIP_APP_OUT}/${YIP_CONFIG}/
        -p:IntDir=${YIP_APP_OUT}/${YIP_CONFIG}/obj/
        -p:YipAudioCoreLib=${AUDIO_CORE_LIB}
        -p:YipVstHostLib=$<TARGET_FILE:vst_host>
        -p:YipAudioCoreInclude=${AUDIO_CORE_DIR}/include
        -p:YipVstHostInclude=${CMAKE_SOURCE_DIR}/vst-host/include
        -m
        -v:minimal
        -nologo
    DEPENDS audio_core_build vst_host
    COMMENT "msbuild yip-app.vcxproj (restore + build)"
    VERBATIM
)

# ----------------------------------------------------------------------------
# Convenience: print summary
# ----------------------------------------------------------------------------

message(STATUS "")
message(STATUS "Yip build configured")
message(STATUS "  generator       : ${CMAKE_GENERATOR}")
message(STATUS "  platform        : ${YIP_PLATFORM}")
message(STATUS "  cargo           : ${CARGO_EXE}")
message(STATUS "  rust target     : ${RUST_TARGET_TRIPLE}")
message(STATUS "  msbuild         : ${MSBUILD_EXE}")
message(STATUS "  audio_core lib  : ${AUDIO_CORE_LIB}")
message(STATUS "  yip-app proj    : ${YIP_APP_PROJ}")
message(STATUS "")
