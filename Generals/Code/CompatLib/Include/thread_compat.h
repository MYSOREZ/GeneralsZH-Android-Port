#pragma once

// GeneralsX @bugfix Android port 16/09/2026 This whole file is a pthread-based
// stand-in for the real Win32 threading API (CreateThread/TerminateThread/
// GetCurrentThreadId all exist for real on _WIN32, and MinGW's "-win32"
// threading model does not even ship <pthread.h> to redefine them against).
// Only GetCurrentThreadIdAsInt is genuinely cross-platform surface -- wwmemlog.cpp
// and wwprofile.cpp want a small int, not a THREAD_ID, to key a thread-local
// stack map -- so that is the only thing this provides on _WIN32, via the real
// API instead of a pthread reimplementation.
#ifdef _WIN32

#include <windows.h>

// GeneralsX @bugfix Android port 16/09/2026 wwprofile.cpp compares
// ::GetCurrentThreadId()'s real return type (DWORD) against a THREAD_ID it
// declares itself -- match that, the same way the pthread_t branch below
// matches pthread_self()'s return type.
typedef DWORD THREAD_ID;

// GeneralsX @bugfix Android port 16/09/2026 DWORD from ::GetCurrentThreadId()
// is already a small, stable-for-the-thread's-lifetime integer -- no mapping
// table needed the way the pthread_t branch below needs one.
inline int GetCurrentThreadIdAsInt()
{
	return (int)::GetCurrentThreadId();
}

#else

#include <pthread.h>
#include <stdint.h>
typedef pthread_t THREAD_ID;
typedef uint32_t (*start_routine)(void *);

// Avoid conflict with old Dependencies/Utility/Utility/thread_compat.h
// The old header defines int GetCurrentThreadId(), we define THREAD_ID GetCurrentThreadId()
// When both are included, we only declare if not already defined as the old int version
#if !defined(GetCurrentThreadId) || !defined(__THREAD_COMPAT_OLD_INCLUDED)
THREAD_ID GetCurrentThreadId();
#endif

// GeneralsX @feature BenderAI 24/02/2026 Phase 5 - Convert pthread_t to int for compatibility
// Maps pthread_t to unique integers (max 256 threads tracked)
int GetCurrentThreadIdAsInt();

void* CreateThread(void *lpSecure, size_t dwStackSize, start_routine lpStartAddress, void *lpParameter, unsigned long dwCreationFlags, unsigned long *lpThreadId);
int TerminateThread(void *hThread, unsigned long dwExitCode);

#endif // _WIN32