/*
** mmsystem.h - Windows Multimedia System stub
**
** TheSuperHackers @build 15/12/2024
** Minimal stub for Linux builds. Download.cpp includes this on Windows only.
** For Linux builds with OpenAL, this provides empty type definitions.
*/

#pragma once

// GeneralsX @bugfix Android port 16/09/2026 The comment above says "Download.cpp
// includes this on Windows only", assuming the real system mmsystem.h would be
// what's found there -- but this file's own directory is on the include search
// path, so #include <mmsystem.h> resolves to this file first regardless of
// platform, and on _WIN32 the guard below produced an empty file: no
// timeGetTime declaration at all, on the very first real WIN32 build to reach
// it (matching the same self-shadowing bug already fixed for windows.h in
// CompatLib and mmsys.h's own real target). #include_next reaches MinGW's
// real system mmsystem.h instead.
#ifdef _WIN32
#include_next <mmsystem.h>
#elif defined(SAGE_USE_OPENAL)
    #ifndef _MMSYSTEM_H_
    #define _MMSYSTEM_H_

    // Empty stub - multimedia functions are not used in Linux builds
    // Download.cpp only uses this header preparation, actual multimedia
    // operations are stubbed or not called in Linux builds.

    #endif
#endif
