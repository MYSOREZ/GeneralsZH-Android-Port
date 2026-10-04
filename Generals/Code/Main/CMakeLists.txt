# GeneralsX @build Android port 04/10/2026 As for Zero Hour (GeneralsMD/Code/Main/CMakeLists.txt): on
# Android the game is a shared library that SDL3's Java shell loads and calls SDL_main in. The
# launcher picks libgenerals.so / libgenerals60.so for the base game, libmain.so / libmain60.so for
# Zero Hour.
if(ANDROID)
    add_library(g_generals SHARED)
else()
    add_executable(g_generals WIN32)
endif()

# Use a binary name that doesn't conflict with original game.
# GeneralsX @build BenderAI 15/02/2026 Linux branding update
if("${CMAKE_SYSTEM}" MATCHES "Windows")
    set_target_properties(g_generals PROPERTIES OUTPUT_NAME "generalsv${RTS_BUILD_OUTPUT_SUFFIX}")
elseif(ANDROID)
    set_target_properties(g_generals PROPERTIES OUTPUT_NAME generals)
else()
    # GeneralsX @build BenderAI 15/02/2026 Linux branding update
    set_target_properties(g_generals PROPERTIES OUTPUT_NAME GeneralsX)
endif()

# GeneralsX @build BenderAI 10/03/2026 Windows-specific libraries
if(WIN32)
    target_link_libraries(g_generals PRIVATE
        binkstub
        comctl32
        d3d8
        dinput8
        dxguid
        imm32
        milesstub
        vfw32
        winmm
    )
endif()

# Cross-platform core libraries
target_link_libraries(g_generals PRIVATE
    d3dx8
    g_gameengine
    g_gameenginedevice
    gi_always
)

# Linux SDL3 windowing/input
# GeneralsX @build BenderAI 11/02/2026
if(SAGE_USE_SDL3)
    target_link_libraries(g_generals PRIVATE sdl3lib)
endif()

# GeneralsX @build Android port 04/10/2026 The same native extras as Zero Hour: adrenotools (custom
# Vulkan driver before DXVK loads), the native GLES backend, and the engine's build number.
if(ANDROID)
    target_link_libraries(g_generals PRIVATE adrenotools d3d8gles)
endif()
if(NOT DEFINED ANDROID_CI_BUILD_NUMBER)
    set(ANDROID_CI_BUILD_NUMBER 0)
endif()

# TODO Originally referred to build host and user, replace with git info perhaps?
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/GeneratedVersion.h
"#pragma once

#define VERSION_LOCALBUILDNUM 0
#define VERSION_BUILDUSER \"\"
#define VERSION_BUILDLOC \"\"
#define ANDROID_CI_BUILD_NUMBER ${ANDROID_CI_BUILD_NUMBER}
"
)

# Based on original binary values for these variables.
if (IS_VS6_BUILD)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BuildVersion.h
"#pragma once

#define VERSION_MAJOR 1
#define VERSION_MINOR 7
#define VERSION_BUILDNUM 601
"
)
else()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BuildVersion.h
"#pragma once

#define VERSION_MAJOR 1
#define VERSION_MINOR 8
#define VERSION_BUILDNUM 601
"
)
endif()

if(MSVC)
    target_link_options(g_generals PRIVATE "/NODEFAULTLIB:libci.lib")
endif()

target_include_directories(g_generals PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/Code/Libraries
)

target_sources(g_generals PRIVATE
    $<$<BOOL:${WIN32}>:WinMain.cpp>
    $<$<BOOL:${WIN32}>:WinMain.h>
    # Linux SDL3 entry point
    $<$<NOT:$<BOOL:${WIN32}>>:${CMAKE_SOURCE_DIR}/Core/Main/SDL3Main.cpp>
    # Linux stubs
    $<$<NOT:$<BOOL:${WIN32}>>:LinuxStubs.cpp>
    # GeneralsX @build Android port 04/10/2026 Shared with Zero Hour (Core/Main): the native crash
    # handler, and the float-cast-overflow report handler plus reference float transcendentals that
    # every Android engine target needs (cmake/config-build.cmake turns that check on for them).
    $<$<BOOL:${ANDROID}>:${CMAKE_SOURCE_DIR}/Core/Main/AndroidCrashHandler.cpp>
    $<$<NOT:$<BOOL:${MSVC}>>:${CMAKE_SOURCE_DIR}/Core/Main/ReferenceFloatMath.cpp>
)

if(NOT MSVC)
    # See GeneralsMD/Code/Main/CMakeLists.txt: no builtins inside the reference math, and no PCH.
    set_source_files_properties(${CMAKE_SOURCE_DIR}/Core/Main/ReferenceFloatMath.cpp PROPERTIES
        COMPILE_OPTIONS "-fno-builtin"
        SKIP_PRECOMPILE_HEADERS ON)
endif()

# RC files are optional for MinGW builds
# Icon: LoadIcon() handles missing resource gracefully (uses default system icon)
# Manifest: DPI awareness metadata (nice to have but not essential)
# TYPELIB: Broken (0 bytes) and causes windres memory exhaustion errors
if(MSVC)
    # VS2005 and later adds default manifest, we need to turn it off to prevent conflict with custom manifest
    if(NOT IS_VS6_BUILD)
        target_link_options(g_generals PRIVATE
            "/MANIFEST:NO"
        )
    endif()

    target_sources(g_generals PRIVATE
        RTS.RC
    )
endif()

# Strip debug symbols to separate file for MinGW Release builds
# This creates generalsv${RTS_BUILD_OUTPUT_SUFFIX}.exe.debug (similar to MSVC .pdb files)
if(MINGW AND COMMAND add_debug_strip_target)
    add_debug_strip_target(g_generals)
endif()

# GeneralsX @build Android port 04/10/2026 Same reason as Zero Hour's (see
# GeneralsMD/Code/Main/CMakeLists.txt): keep the engine's operator new/delete out of the dynamic
# symbol table so DXVK, SDL3 and OpenAL do not bind to them.
if(ANDROID)
    set(_gx_hide_alloc_map "${CMAKE_CURRENT_BINARY_DIR}/gx_hide_alloc.map")
    file(WRITE "${_gx_hide_alloc_map}" [=[
{
  global:
    *;
  local:
    _Znw*;
    _Zna*;
    _Zdl*;
    _Zda*;
};
]=])
    target_link_options(g_generals PRIVATE
        "-Wl,--version-script,${_gx_hide_alloc_map}"
    )
endif()
