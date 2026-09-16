# On 64-bit Windows, macOS, and Linux: use our d3dx8 implementation (GLI/GLM for math helpers)
# GeneralsX @build BenderAI 24/02/2026 Phase 5 - Skip d3dx8 on macOS/Linux due to GLI/GLM ambiguity
# GeneralsX @build felipebraz 20/06/2025 - Also build d3dx8 STATIC on macOS (D3DX math/texture stubs required for link)
# GeneralsX @bugfix felipebraz 02/03/2026 - Build real d3dx8 STATIC on Linux too (D3DXGetFVFVertexSize / D3DXLoadSurfaceFromSurface required for link)
# d3dx8 is a math/texture helper stub; core game code doesn't require it
# GeneralsX @bugfix Android port 16/09/2026 This covered 64-bit Windows,
# Apple and Linux one at a time as each was brought up, and 32-bit Windows
# (MinGW i686) was never added -- it fell into the "Linux only" stub below
# by accident. Build the real one here instead, like every other platform --
# it is plain C++ math/texture code, nothing 64-bit-specific. This also
# covers SAGE_USE_DX8=ON (the real DirectX 8 SDK path, cmake/dx8.cmake): that
# SDK's own d3dx8core.h/tex.h/math.h declare the D3DX8 API but implement none
# of it (no DLL/import-lib shipped for it, unlike the core D3D8 interfaces),
# so building it ourselves is the only way to get it at all without pulling
# in an external d3dx8*.dll -- and MinGW-w64's own import lib for one only
# names the SDK's debug redistributable (d3dx8d.dll), which no real install
# has (see cmake/mingw.cmake, which used to alias d3dx8 to it).
if(WIN32 OR APPLE OR (UNIX AND NOT APPLE))
list(APPEND SOURCE_D3DX_COMPAT
    d3dx8_compat.cpp
    d3dx8math.cpp
)

list(TRANSFORM SOURCE_D3DX_COMPAT PREPEND "Source/")

add_library(d3dx8 STATIC ${SOURCE_D3DX_COMPAT})

# GLI requires GLM_ENABLE_EXPERIMENTAL for internal extensions (like gli/core/coord.hpp)
target_compile_definitions(d3dx8 PRIVATE GLM_ENABLE_EXPERIMENTAL)

# TheSuperHackers @build 09/02/2025 Bender
# Use vcpkg find_package (like fighter19) instead of FetchContent
# This ensures compatible GLM/GLI versions via vcpkg baseline
if(NOT SAGE_USE_GLM)
    message(FATAL_ERROR "GLM is required for d3dx8 compatibility layer")
endif()

find_package(glm CONFIG REQUIRED)
target_link_libraries(d3dx8 PUBLIC glm::glm)

# GeneralsX @build felipebraz 20/06/2025 GLI causes make_vec4 ambiguity with Apple Clang - only link on Windows
# GeneralsX @bugfix Android port 16/09/2026 - MinGW-w64/GCC hits the identical
# ambiguity (see d3dx8_compat.cpp), which now takes the same manual-fallback
# branch Apple already used, so it does not need gli linked either.
if(NOT APPLE AND NOT MINGW)
    find_package(gli CONFIG REQUIRED)
    target_link_libraries(d3dx8 PUBLIC gli)
endif()

# d3dx8 needs bittype.h from WWLib for Win32 types (DWORD, BYTE, UINT, etc.)
# Add only WWLib directory (not full core_wwcommon to avoid d3d8.h conflicts from WWAudio)
target_include_directories(d3dx8 PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/Include
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WWLib
)

# Link d3dx8 to d3d8lib to get DirectX8 headers (min-dx8-sdk or DXVK)
target_link_libraries(d3dx8 PUBLIC d3d8lib)

else()
# Nothing else currently reaches this branch (WIN32/APPLE/UNIX-non-APPLE
# covers every supported platform), kept as a safety net for any future one.
add_library(d3dx8 INTERFACE)
target_link_libraries(d3dx8 INTERFACE d3d8lib)
endif()

# GeneralsX @build BenderAI 10/02/2026
# Configure d3d8lib target globally (created early in Core/CMakeLists.txt)
# Fighter19's approach: Windows uses min-dx8-sdk, Linux uses ONLY DXVK headers
if(SAGE_USE_DX8)
    # Windows native: Use min-dx8-sdk headers and libs
    if(NOT dx8_SOURCE_DIR)
        message(FATAL_ERROR "DirectX 8 SDK (min-dx8-sdk) not fetched! Ensure cmake/dx8.cmake was included")
    endif()
    target_include_directories(d3d8lib INTERFACE
        ${dx8_SOURCE_DIR}/include
    )
    target_link_directories(d3d8lib INTERFACE
        ${dx8_SOURCE_DIR}/lib
    )
else()
    # Linux + macOS: Use ONLY DXVK headers (not min-dx8-sdk) to avoid conflicts
    # DXVK provides complete Wine-compatible DirectX 8 headers
    # CRITICAL: Linux uses a pre-built tarball (headers in include/dxvk/ flat)
    #           macOS builds DXVK from source (headers in include/native/windows + directx + vulkan)
    # GeneralsX @build BenderAI 24/02/2026 Phase 5 - DXVK v2.6 uses include/native/windows + include/native/directx
    # GeneralsX @build fbraz 24/02/2026 - Also need include/native for WSI headers (wsi/native_wsi.h)
    # GeneralsX @bugfix felipebraz 02/03/2026 - Restore Linux path (tarball = include/dxvk, not include/native)
    if(NOT dxvk_SOURCE_DIR)
        message(FATAL_ERROR "DXVK not fetched! Ensure SAGE_USE_DX8=OFF and cmake/dx8.cmake was included")
    endif()
    # GeneralsX @bugfix Android port 07/07/2026 The layout split is
    # source-build vs prebuilt-tarball, not macOS vs everything-else: Android
    # builds DXVK from the same git source as macOS (include/native layout);
    # only desktop Linux consumes the steamrt tarball (flat include/dxvk/).
    # Run #6 failed 164 compiles with "windows_base.h not found" because
    # Android fell into the tarball branch.
    if(APPLE OR ANDROID)
        # DXVK built from git source -- Wine-style include/native layout
        target_include_directories(d3d8lib INTERFACE
            ${dxvk_SOURCE_DIR}/include/native
            ${dxvk_SOURCE_DIR}/include/native/windows
            ${dxvk_SOURCE_DIR}/include/native/directx
            ${dxvk_SOURCE_DIR}/include/vulkan
        )
    else()
        # Linux: DXVK pre-built steamrt tarball -- all headers flat in include/dxvk/
        target_include_directories(d3d8lib INTERFACE
            ${dxvk_SOURCE_DIR}/include/dxvk
        )
    endif()
    # GeneralsX @bugfix BenderAI 25/02/2026 On macOS, DXVK dylibs live in the Meson build dir
    # (_deps/dxvk-build-macos/src/d3d*/), not in ${dxvk_SOURCE_DIR}/lib — that path does not exist.
    # Linux prebuilt DXVK native package does have a lib/ dir, so the guard is source-build-only
    # (macOS and Android both build from source; see above).
    # Passing a non-existent -L path produces: ld: warning: search path '.../dxvk-src/lib' not found
    if(NOT APPLE AND NOT ANDROID)
        target_link_directories(d3d8lib INTERFACE
            ${dxvk_SOURCE_DIR}/lib
        )
    endif()
endif()

if (UNIX)
set(SOURCE_COMPAT
    module_compat.cpp
    string_compat.cpp
    thread_compat.cpp
    time_compat.cpp
    wchar_compat.cpp
    wnd_compat.cpp
)

list(TRANSFORM SOURCE_COMPAT PREPEND "Source/")

add_library(CompatLib STATIC ${SOURCE_COMPAT})

# GeneralsX @build BenderAI 24/02/2026 Phase 5 - Require C++17 for std::filesystem in compat headers
set_property(TARGET CompatLib PROPERTY CXX_STANDARD 17)
set_property(TARGET d3dx8 PROPERTY CXX_STANDARD 17)

# GeneralsX @bugfix BenderAI 02/03/2026 - Enforce DXVK source availability before CompatLib compiles
# On macOS, DXVK is built via ExternalProject (Meson) which clones the source during the BUILD phase,
# not during configure. This means _deps/dxvk-src/include/native/windows/windows_base.h does not
# exist when d3dx8 or CompatLib start compiling unless we declare an explicit ordering dependency.
# On Linux, DXVK is a pre-built tarball fetched via FetchContent (available since configure), so
# no ordering dependency is needed there.
if(APPLE AND TARGET dxvk_macos_build)
    add_dependencies(d3dx8 dxvk_macos_build)
    add_dependencies(CompatLib dxvk_macos_build)
endif()

target_include_directories(CompatLib PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

# GeneralsX @build BenderAI 10/02/2026
# Fighter19 pattern: d3d8lib (INTERFACE) links TO CompatLib
# This makes CompatLib includes transitively available to all targets linking d3d8lib
# REVERSED from previous (CompatLib linking TO d3d8lib)
target_link_libraries(d3d8lib INTERFACE CompatLib)

# CompatLib needs d3dx8 for D3DX types
# SDL3 headers available via vcpkg but linking happens in GameEngineDevice/Main
target_link_libraries(CompatLib PUBLIC d3dx8)

endif()