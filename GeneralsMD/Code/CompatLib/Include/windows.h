#pragma once

// Linux/Unix compatibility shim for Windows.h
// GeneralsX @build BenderAI 11/02/2026 Windows API compatibility layer
// STRATEGY: Provide our own complete Windows types/functions FIRST.
// DXVK headers (d3d8.h) will include their own windows_base.h later.
// Our types take precedence via PCH (PreRTS.h includes windows_compat.h early).

// GeneralsX @bugfix Android port 16/09/2026 The comment above always claimed the
// _WIN32 branch would "use real Windows.h from SDK... only reached if no SDK
// windows.h exists", but the code never did that -- both branches included
// windows_compat.h regardless. On a real MinGW build this file itself is what
// shadows the real system windows.h (this Include/ directory is on the -I
// search path ahead of the compiler's own system directories, so
// #include <windows.h> resolves to this very file first), so
// windows_compat.h's _WIN32-gated pieces stayed permanently unreached and
// DXVK's d3d8.h -- which assumes WINBOOL, LARGE_INTEGER etc. already exist by
// the time it is included -- was left with an incomplete set of Windows types
// on the very first WIN32 build to reach it. #include_next is the standard
// way to say "the next windows.h after this one in the search path", i.e.
// MinGW's real system header, from inside a same-named project header
// without an include cycle.
#ifdef _WIN32
#include_next <windows.h>
#else
// Linux: Our compatibility layer only (NO DXVK headers here!)
// DXVK's windows_base.h will be included by d3d8.h when needed
#include "windows_compat.h"
#endif
