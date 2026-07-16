/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// GeneralsX @build Android port 07/07/2026
//
// A crash during dlopen()'s ELF constructors or early in SDL_main runs
// before SDL3Main.cpp's own stderr-redirect logic ever executes — on
// stock Android that class of crash is invisible without adb (only a
// system tombstone under /data/tombstones, unreadable without root, and
// a logcat FATAL SIGNAL line that scrolls away in seconds). Users
// testing this port sideloaded, without a PC handy, had no way to tell
// us what broke.
//
// Fix: install a signal handler as an ELF constructor — it runs the
// instant the dynamic linker loads this library, before any engine code,
// before JNI_OnLoad, before main(). On a fatal signal it appends a
// one-line summary to a fixed path using only raw POSIX syscalls (no
// libc buffered I/O, no malloc) so it stays usable even with a corrupted
// heap, then chains to whatever handler was previously installed so the
// OS's own tombstone/logcat capture still happens too.
//
// The log path is the app's internal storage (getFilesDir() on the Java
// side) — always private + writable to this app's UID on every Android
// version, no permission needed, unlike external/shared storage. Android's
// SDL3/LogViewerActivity reads the same file to show it in-app without adb.

#if defined(__ANDROID__)

#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#if defined(__aarch64__)
#include <ucontext.h>
#endif

namespace {

// GeneralsX @bugfix Android port 07/07/2026 A bare signal+fault_addr line
// wasn't enough to diagnose a crash reported as happening "on pressing a
// button in-game": two separate sessions hit the exact same fault_addr
// (0x2000000001, a fixed non-null garbage-looking value — not a plain
// null-deref), which pointed at a deterministic bug but gave no way to
// find WHERE. Resolve the crashing PC to "library+file-offset" by walking
// /proc/self/maps with raw syscalls only (open/read/close are POSIX
// async-signal-safe; no malloc, no strtok/libc line-buffering). The
// resulting offset is directly usable with `addr2line -e libmain.so
// <offset>` against the same CI build's unstripped .so.
int hexDigitValue(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return 0;
}

// GeneralsX @bugfix Android port 12/07/2026 - 64KB was too small: a build
// this size (DXVK, GNS + its ICE/signaling worker threads, curl, freetype,
// SDL3, etc.) routinely produces a /proc/self/maps well past 64KB, so the
// read loop below silently truncated before reaching the entries needed to
// resolve the crash PC/LR/early backtrace frames -- confirmed by comparing
// a real device crash log (PC, LR and backtrace[0..3] all "no matching
// entry") against the same build's unstripped libmain.so, where backtrace[4]
// onward (present in the captured portion) resolved cleanly to ordinary,
// uncorrupted call frames (SDL_main -> GameMain -> SDL3GameEngine::execute),
// i.e. the process wasn't corrupted, the maps read just ran out of buffer.
static char s_mapsBuf[1 << 20];

bool findLibraryForAddress(uintptr_t pc, char *outName, size_t outNameLen, uintptr_t *outOffset) {
	int fd = open("/proc/self/maps", O_RDONLY);
	if (fd < 0) {
		return false;
	}
	ssize_t total = 0;
	ssize_t n;
	while (total < (ssize_t)sizeof(s_mapsBuf) - 1 &&
	       (n = read(fd, s_mapsBuf + total, sizeof(s_mapsBuf) - 1 - (size_t)total)) > 0) {
		total += n;
	}
	close(fd);
	if (total <= 0) {
		return false;
	}
	s_mapsBuf[total] = '\0';

	char *line = s_mapsBuf;
	char *bufEnd = s_mapsBuf + total;
	while (line < bufEnd) {
		char *lineEnd = line;
		while (lineEnd < bufEnd && *lineEnd != '\n') {
			lineEnd++;
		}

		uintptr_t start = 0, end = 0, fileOffset = 0;
		char *p = line;
		while (p < lineEnd && *p != '-') { start = (start << 4) | (uintptr_t)hexDigitValue(*p); p++; }
		if (p < lineEnd) p++; // skip '-'
		while (p < lineEnd && *p != ' ') { end = (end << 4) | (uintptr_t)hexDigitValue(*p); p++; }
		while (p < lineEnd && *p == ' ') p++;
		while (p < lineEnd && *p != ' ') p++; // skip perms field
		while (p < lineEnd && *p == ' ') p++;
		while (p < lineEnd && *p != ' ') { fileOffset = (fileOffset << 4) | (uintptr_t)hexDigitValue(*p); p++; }

		if (pc >= start && pc < end) {
			char *pathStart = lineEnd;
			while (pathStart > line && *(pathStart - 1) != ' ') {
				pathStart--;
			}
			size_t pathLen = (size_t)(lineEnd - pathStart);
			if (pathLen > 0 && pathLen < outNameLen) {
				memcpy(outName, pathStart, pathLen);
				outName[pathLen] = '\0';
			} else {
				outName[0] = '\0';
			}
			*outOffset = (pc - start) + fileOffset;
			return true;
		}

		line = lineEnd + 1;
	}
	return false;
}

// GeneralsX @bugfix Android port 07/07/2026 Context.getFilesDir() (what
// SDL_GetAndroidInternalStoragePath() and this path both need to agree with)
// resolves under /data/user/<userId>/<pkg>/files on modern Android; the
// /data/data/<pkg> shortcut is only a symlink to user 0's data and silently
// resolves to a WRONG, nonexistent-for-this-run location on a work profile,
// a secondary user, or guest mode. Android multi-user UIDs are always
// userId*100000 + appId (a stable, documented convention), so derive the
// real user id from getuid() instead of assuming user 0.
char s_crashLogPath[256];

void computeCrashLogPath() {
	int userId = (int)(getuid() / 100000);
	snprintf(s_crashLogPath, sizeof(s_crashLogPath),
		"/data/user/%d/com.generalsx.zerohour/files/crash.log", userId);
}

struct sigaction s_prevHandlers[NSIG];
bool s_haveHandlerFor[NSIG] = {};

// Appends a message using only async-signal-safe primitives (open/write/close
// are POSIX async-signal-safe; snprintf into a stack buffer with no malloc
// path is the one pragmatic exception here — bionic's implementation does
// not lock or allocate for a plain format string like this one, and every
// widely-shipped Android crash handler makes the same trade-off).
void appendCrashLog(const char *message, size_t length) {
	if (s_crashLogPath[0] == '\0') {
		return;
	}
	int fd = open(s_crashLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0) {
		return;
	}
	ssize_t written = 0;
	while (written < (ssize_t)length) {
		ssize_t n = write(fd, message + written, length - (size_t)written);
		if (n <= 0) {
			break;
		}
		written += n;
	}
	close(fd);
}

// GeneralsX @bugfix Android port 11/07/2026 Two real-device crashes in a row
// resolved to "no matching /proc/self/maps entry" for PC alone -- a wild
// jump (corrupted vtable/function pointer or a call through freed memory),
// not something a single address can diagnose. Log LR (x30, the return
// address at the moment of the bad call/branch) and walk the AArch64
// frame-pointer chain (x29 -> [saved x29, saved x30]) a few frames up: even
// when the jump target itself is garbage, the caller that made the bad call
// is usually still a valid, resolvable address, since the CPU sets LR
// before branching. Frame-pointer chains are kept by default by the NDK
// clang toolchain for arm64, so this is a plain, no-libunwind backtrace.
void logResolvedAddress(const char *label, uintptr_t addr) {
	char buf[256];
	if (addr == 0) {
		return;
	}
	char libName[192];
	uintptr_t libOffset = 0;
	int len;
	if (findLibraryForAddress(addr, libName, sizeof(libName), &libOffset)) {
		len = snprintf(buf, sizeof(buf), "%s=%p is in %s+0x%lx\n",
			label, (void *)addr, libName, (unsigned long)libOffset);
	} else {
		len = snprintf(buf, sizeof(buf), "%s=%p (no matching /proc/self/maps entry)\n", label, (void *)addr);
	}
	if (len > 0) {
		appendCrashLog(buf, (size_t)len < sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
	}
}

void androidCrashHandler(int sig, siginfo_t *info, void *ucontext) {
	char buf[512];
	const void *faultAddr = (info != nullptr) ? info->si_addr : nullptr;
	int len = snprintf(buf, sizeof(buf),
		"\n=== NATIVE CRASH === signal=%d (%s) fault_addr=%p pid=%d tid=%d ===\n"
		"If you're reading this: open the GeneralsZH Setup app -> View Logs,\n"
		"and share this file's contents.\n",
		sig, strsignal(sig), faultAddr, (int)getpid(), (int)gettid());
	if (len > 0) {
		appendCrashLog(buf, (size_t)len < sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
	}

#if defined(__aarch64__)
	uintptr_t pc = (ucontext != nullptr) ? (uintptr_t)((ucontext_t *)ucontext)->uc_mcontext.pc : 0;
	uintptr_t lr = (ucontext != nullptr) ? (uintptr_t)((ucontext_t *)ucontext)->uc_mcontext.regs[30] : 0;
	uintptr_t fp = (ucontext != nullptr) ? (uintptr_t)((ucontext_t *)ucontext)->uc_mcontext.regs[29] : 0;
	logResolvedAddress("crash PC", pc);
	logResolvedAddress("crash LR", lr);

	if (fp != 0) {
		uintptr_t frame = fp;
		for (int i = 0; i < 16; ++i) {
			if (frame == 0 || (frame & 0xF) != 0) {
				break;
			}
			// A corrupted fp could point anywhere; this read can itself fault.
			// That's acceptable here -- every frame resolved before that point
			// has already been flushed to disk by appendCrashLog(), so a second
			// fault just ends the backtrace early instead of losing everything.
			uintptr_t *pFrame = (uintptr_t *)frame;
			uintptr_t savedFP = pFrame[0];
			uintptr_t savedLR = pFrame[1];
			if (savedLR == 0) {
				break;
			}
			char label[24];
			snprintf(label, sizeof(label), "backtrace[%d]", i);
			logResolvedAddress(label, savedLR);
			if (savedFP <= frame || savedFP - frame > (1u << 20)) {
				break;
			}
			frame = savedFP;
		}
	}
#endif

	// Chain to whatever handler was previously installed (Android's own
	// debuggerd hook in the common case) so the system tombstone and the
	// logcat "FATAL SIGNAL" block still get generated too — this handler
	// only ADDS a no-adb-needed diagnostic, it doesn't replace the OS one.
	if (sig >= 0 && sig < NSIG && s_haveHandlerFor[sig]) {
		struct sigaction &prev = s_prevHandlers[sig];
		if ((prev.sa_flags & SA_SIGINFO) != 0 && prev.sa_sigaction != nullptr) {
			prev.sa_sigaction(sig, info, ucontext);
			return;
		}
		if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler != nullptr) {
			prev.sa_handler(sig);
			return;
		}
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

// GeneralsX @bugfix Android port 13/07/2026 GitHub issue #2: several reports
// show the engine log stopping mid-line, deep inside recursive/nested INI
// block parsing (e.g. an object's ParticleSysBone sub-blocks), with VmRSS
// still low (~247MB on a report from a 12-24GB device -- nowhere near an
// OOM-kill threshold) and zero output from this very handler. A SIGSEGV
// caused by exhausting the thread's own stack (deep recursion) delivers the
// signal onto that SAME already-exhausted stack when no alternate signal
// stack is registered -- the handler then immediately faults again trying
// to push its own locals, and the process dies silently before a single
// byte reaches appendCrashLog(). sigaltstack() + SA_ONSTACK gives the
// handler a separate, always-valid stack to run on regardless of what state
// the crashing thread's own stack is in, so a stack-overflow SIGSEGV
// produces the same PC/LR/backtrace crash.log entry as any other fault
// instead of just going silent.
char s_altStack[64 * 1024];

__attribute__((constructor))
void installAndroidCrashHandler() {
	computeCrashLogPath();

	stack_t ss;
	ss.ss_sp = s_altStack;
	ss.ss_size = sizeof(s_altStack);
	ss.ss_flags = 0;
	sigaltstack(&ss, nullptr);

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = androidCrashHandler;
	sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);

	const int signals[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };
	for (int s : signals) {
		if (sigaction(s, &sa, &s_prevHandlers[s]) == 0) {
			s_haveHandlerFor[s] = true;
		}
	}

	// A signpost written unconditionally at load time: if crash.log exists
	// but ends right after this line, the crash happened somewhere between
	// here and the next log line the engine itself writes (SDL3Main.cpp) —
	// i.e. during the rest of dlopen's constructors or very early SDL_main,
	// still before the regular stderr log redirect is set up.
	time_t now = time(nullptr);
	char stamp[256];
	// GeneralsX @feature Android port 16/07/2026 Stamp the exact build (compile
	// date/time) into both crash.log and stderr at load. Reporters can't tell
	// which APK a given log came from otherwise (issue #2, requested by a tester)
	// -- and that ambiguity made it impossible to trust whether a diagnostic
	// change was actually in the tested binary. __DATE__/__TIME__ uniquely
	// identify each CI build.
	int len = snprintf(stamp, sizeof(stamp),
		"\n=== libmain.so loaded, crash handler installed (t=%ld) [build compiled %s %s] ===\n",
		(long)now, __DATE__, __TIME__);
	if (len > 0) {
		appendCrashLog(stamp, (size_t)len < sizeof(stamp) ? (size_t)len : sizeof(stamp) - 1);
	}
	fprintf(stderr, "[GX-BUILD] libmain.so compiled %s %s\n", __DATE__, __TIME__);
	fflush(stderr);
}

} // namespace

#endif // __ANDROID__
