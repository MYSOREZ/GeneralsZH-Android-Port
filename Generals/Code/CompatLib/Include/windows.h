#pragma once

// Linux/Unix compatibility shim for Windows.h
// GeneralsX @build BenderAI 11/02/2026 Windows API compatibility layer
// STRATEGY: Provide our own complete Windows types/functions FIRST.
// DXVK headers (d3d8.h) will include their own windows_base.h later.
// Our types take precedence via PCH (PreRTS.h includes windows_compat.h early).

// GeneralsX @bugfix Android port 16/09/2026 See the identical fix in
// GeneralsMD/Code/CompatLib/Include/windows.h -- this file's _WIN32 branch
// never actually reached a real windows.h; it shadows it via -I search-path
// precedence and included windows_compat.h either way, leaving DXVK's d3d8.h
// without WINBOOL/LARGE_INTEGER/etc. on the first real WIN32 build to compile
// it. #include_next reaches MinGW's real system header instead.
#ifdef _WIN32
#include_next <windows.h>
#else
// Linux: Our compatibility layer only (NO DXVK headers here!)
// DXVK's windows_base.h will be included by d3d8.h when needed
#include "windows_compat.h"
#endif
