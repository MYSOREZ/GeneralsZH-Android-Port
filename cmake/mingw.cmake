# TheSuperHackers @build JohnsterID 05/01/2026 Add MinGW-w64 cross-compilation support
# MinGW-w64 specific compiler and linker configurations

if(MINGW)
    message(STATUS "Configuring MinGW-w64 build settings")
    
    # Detect if this is 32-bit or 64-bit MinGW
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(IS_MINGW32 TRUE)
        message(STATUS "MinGW-w64 32-bit (i686) detected")
    else()
        message(FATAL_ERROR "MinGW-w64 64-bit (x86_64) detected, but this project only supports 32-bit builds. Use the i686-w64-mingw32 toolchain.")
    endif()
    
    # Windows subsystem
    add_link_options(-mwindows)
    
    # Static linking of GCC runtime libraries
    # This embeds libgcc and libstdc++ into the executable to avoid DLL dependencies
    add_link_options(-static-libgcc -static-libstdc++)
    
    # Compatibility flags for legacy code
    add_compile_options(
        -fno-strict-aliasing        # Avoid type-punning issues with DX8/COM
    )
    
    # MSVC compatibility macros for MinGW
    # Note: MinGW already defines _cdecl and _stdcall correctly, so we only add __forceinline
    # The escaped syntax below expands to: -D__forceinline="inline __attribute__((always_inline))"
    # Escaping rules: \( \) = literal parentheses, \ (backslash-space) = space in definition
    add_compile_definitions(
        __forceinline=inline\ __attribute__\(\(always_inline\)\)
        __int64=long\ long
        _int64=long\ long
    )

    # GeneralsX @bugfix Android port 16/09/2026 core_wwcommon already carries
    # NOMINMAX as an INTERFACE define, but that only reaches targets that
    # link core_wwcommon -- z_gameengine (Recorder.cpp -> NGMPGame.h ->
    # <chrono>) does not, and <windows.h>'s min/max macros make <bits/chrono.h>
    # fail to parse at all the moment both get included in the same
    # translation unit. Make it universal instead of per-target.
    add_compile_definitions(
        NOMINMAX
    )

    # Enable math constants in MinGW's <math.h>
    # MinGW provides M_PI, M_E, etc. in <math.h>, but only when -std=c++XX is NOT used (strict ANSI mode),
    # or when _USE_MATH_DEFINES is defined. Since we compile with -std=c++20, we need this define.
    # The header mingw.h (included via always.h) provides the missing constants (M_1_SQRTPI, M_SQRT_2 alias).
    add_compile_definitions(
        _USE_MATH_DEFINES
    )
    
    # Ensure proper calling conventions are defined
    # MinGW-w64 should define these, but verify they exist
    include(CheckCXXSymbolExists)
    check_cxx_symbol_exists(STDMETHODCALLTYPE "windows.h" HAVE_STDMETHODCALLTYPE)
    if(NOT HAVE_STDMETHODCALLTYPE)
        add_compile_definitions(
            STDMETHODCALLTYPE=__stdcall
            STDMETHODIMP=HRESULT\ __stdcall
        )
    endif()
    
    # Required Windows libraries for DX8 + COM
    link_libraries(
        uuid        # COM GUIDs
        ole32       # COM runtime
        oleaut32    # COM automation
        gdi32       # GDI
        user32      # User interface
        comctl32    # Common controls
        winmm       # Multimedia (timeGetTime, etc.)
        vfw32       # Video for Windows (AVIFile functions)
        d3d8        # Direct3D 8
        dinput8     # DirectInput 8
        dxguid      # GUID definitions dinput8 needs (IID_IDirectInput8, GUID_SysKeyboard,
                    # c_dfDIKeyboard, ...) -- dinput.h only declares them extern
        dsound      # DirectSound
        imm32       # Input Method Manager (IME)
    )
    
    # Note: MinGW-w64 does not provide comsuppw (COM support utilities library).
    # COM support utilities (_com_util::ConvertStringToBSTR, ConvertBSTRToString)
    # are provided by Dependencies/Utility/Utility/comsupp_compat.h as header-only
    # implementations. No library linking required.
    
    # GeneralsX @bugfix Android port 16/09/2026 This used to alias d3dx8 to
    # MinGW-w64's own libd3dx8d.a -- the only D3DX8 import lib MinGW ships --
    # but that lib's DLL name is literally "d3dx8d.dll", the DirectX SDK's
    # DEBUG redistributable, which no real end-user install (retail or
    # otherwise) ships; a build linked against it fails to even start
    # ("d3dx8d.dll not found") on a real machine that plainly doesn't have
    # d3dx8.dll either, since the D3DX8 utility DLL was never a Windows
    # component -- it is SDK/redistributable-only.
    #
    # CompatLib's own d3dx8_compat.cpp/d3dx8math.cpp already implement every
    # non-inline D3DX8 function this codebase actually calls (verified
    # against both the real min-dx8-sdk's d3dx8core.h/tex.h/math.h "non-inline"
    # section and this codebase's own D3DX* call sites) -- everything else
    # (D3DXMatrixIdentity, D3DXVec4Dot, ...) is header-only inline in the real
    # SDK's own d3dx8math.inl, needing no implementation at all. Building our
    # own from-source d3dx8 for MinGW too, the same as every other platform,
    # removes the external-DLL dependency entirely instead of trading one
    # missing DLL for a differently-named one.

    message(STATUS "MinGW-w64 configuration complete")
endif()
