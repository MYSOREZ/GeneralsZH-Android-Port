/*
**	GeneralsX @build Android port 30/07/2026
**
**	Opt-in diagnostic tracing.
**
**	The [GX-TRACE] instrumentation in the font/text/GUI paths is invaluable
**	when chasing a hang, but several of its sites sit inside per-frame code
**	(Render2DSentenceClass::Render alone emits 4-12 lines every frame, each
**	followed by an fflush). Left permanently on, it produced tester logs
**	that were 100% trace spam: the engine log grows unbounded on disk, and
**	the in-app log export keeps only a bounded slice, so session startup --
**	device/GPU/driver banner, INI loading, DXVK init, i.e. the part that
**	actually explains a failure -- was always scrolled away by the time a
**	report was filed. A real case: issue #2's 2026-07-26 log from a Galaxy
**	S24+ contained 2,681 lines, every one of them trace output, and not a
**	single line about what went wrong.
**
**	So tracing is now off unless explicitly requested. Two ways to turn it
**	on, checked once on first use:
**
**	  - GX_TRACE environment variable, set to anything but empty or "0"
**	    (for developers: adb shell setprop / a wrapper script).
**	  - a file named "gx_trace.txt" in the game data folder (the process
**	    working directory by the time anything traces). This is the one to
**	    tell a tester about: no adb, no rebuild, they already manage that
**	    folder. Delete the file to turn tracing back off.
**
**	When disabled the macro costs one already-resolved bool load, so it is
**	safe to leave in per-frame paths.
*/

#pragma once

#include <cstdio>
#include <cstdlib>

namespace GXTrace
{

	inline bool computeEnabled()
	{
		const char *env = getenv("GX_TRACE");
		if (env != nullptr && env[0] != '\0' && env[0] != '0') {
			return true;
		}

		// Relative on purpose: the engine chdir()s into the selected game
		// data folder during startup, long before any traced code runs.
		FILE *marker = fopen("gx_trace.txt", "r");
		if (marker != nullptr) {
			fclose(marker);
			return true;
		}

		return false;
	}

	inline bool isEnabled()
	{
		// Function-local static: resolved once, thread-safe since C++11, and
		// shared across every translation unit that includes this header.
		static const bool enabled = computeEnabled();
		return enabled;
	}

}  // namespace GXTrace

// Usage: GX_TRACE("Some_Function: about to do the thing x=%d\n", x);
// The "[GX-TRACE] " prefix and the flush are supplied here.
#define GX_TRACE(...)                                     \
	do {                                                  \
		if (GXTrace::isEnabled()) {                        \
			fprintf(stderr, "[GX-TRACE] " __VA_ARGS__);    \
			fflush(stderr);                                \
		}                                                 \
	} while (0)
