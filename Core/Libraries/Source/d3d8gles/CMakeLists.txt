# d3d8gles - D3D8 -> native GLES3 renderer for Android (experimental)
# GeneralsX @build Android port GLES experiment
#
# Ported from Lolendor/Generals-WebAssembly's d3d8webgl (D3D8 -> WebGL2 for
# Emscripten). Implements the subset of the Direct3D 8 API the engine uses
# (see Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp) on top of native
# GLES3 via SDL3's GL context API, as an alternative to DXVK's D3D8->Vulkan
# translation. Statically linked into libmain.so (unlike DXVK, which is
# dlopen'd at runtime) -- see DX8Wrapper::Init()'s backend switch.
#
# Android-only for now: this is an experimental branch, not wired into any
# other platform's build.

if(NOT ANDROID)
    return()
endif()

add_library(d3d8gles STATIC
    src/d3d8gles.cpp
)

target_include_directories(d3d8gles PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(d3d8gles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# D3D8 headers come from the same DXVK checkout our own D3D8 code already
# uses (d3d8lib, see GeneralsMD/Code/CompatLib/CMakeLists.txt); Win32 shim
# types (HWND, WINAPI, DWORD, ...) come from CompatLib transitively through
# it. sdl3lib provides GLES3/gl3.h (Android NDK) and the SDL3 GL context API
# (SDL_GL_CreateContext/MakeCurrent/SwapWindow) this module's context setup
# uses instead of Emscripten's WebGL2 canvas API. GLESv3 is the actual NDK
# system library the gl* symbols resolve against (sdl3lib only gives us the
# header + SDL's own context-management calls, not the GL entry points
# themselves -- those come from the platform, same as EGL would).
target_link_libraries(d3d8gles PRIVATE d3d8lib sdl3lib GLESv3)

target_compile_features(d3d8gles PRIVATE cxx_std_20)
