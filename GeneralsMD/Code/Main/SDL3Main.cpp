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

/*
** SDL3Main.cpp
**
** Entry point for Linux builds using SDL3 windowing and DXVK graphics.
**
** TheSuperHackers @feature CnC_Generals_Linux 07/02/2026
** Entry point replaces WinMain() for Linux builds.
** Instantiates SDL3GameEngine and calls GameMain().
*/

#ifndef _WIN32

// SYSTEM INCLUDES
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
// GeneralsX @build Android port 06/07/2026 Shared guard for the touch-first
// mobile platforms (matches SDL3GameEngine.cpp). Both need SDL's entry-point
// wrapper, both synthesize mouse events from touch, both pick the internal
// resolution from the real display size.
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
#define SAGE_MOBILE_PLATFORM 1
#endif
#if defined(SAGE_MOBILE_PLATFORM)
// On iOS, SDL renames main() to SDL_main and provides its own UIApplicationMain
// bootstrap; the app lifecycle (suspend/resume, window) is owned by SDL.
// On Android the same include renames main() to SDL_main, which the SDLActivity
// Java shell (android/) invokes inside the app process after loading libmain.so.
#include <SDL3/SDL_main.h>
#include <cerrno>
#include <sys/stat.h>
#include <fcntl.h>
#include <filesystem>
#include <string>
#endif
#if defined(__ANDROID__)
// GeneralsX @feature Android port 10/07/2026 Optional custom Vulkan driver
// loading (Adreno/Turnip), see TryLoadCustomVulkanDriver() below.
#include <jni.h>
#include <dlfcn.h>
#include <adrenotools/driver.h>
#endif
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <unistd.h>   // _exit()
#include <glob.h>     // glob() for Vulkan ICD discovery

// USER INCLUDES (match WinMain.cpp pattern)
#include "Lib/BaseType.h"
#include "Common/CommandLine.h"
#include "Common/CriticalSection.h"
#include "Common/GlobalData.h"
#include "Common/GameEngine.h"
#include "Common/GameMemory.h"
#include "Common/Debug.h"
#include "Common/version.h"  // GeneralsX @bugfix BenderAI 14/02/2026 Version class + TheVersion extern
#include "SDL3GameEngine.h"
#include "GeneratedVersion.h"  // GeneralsX @feature Android port 12/07/2026 ANDROID_CI_BUILD_NUMBER

// DXVK WSI
#define DXVK_WSI_SDL3 1
#include <wsi/native_wsi.h>

// CRITICAL SECTIONS (Linux needs these too)
static CriticalSection critSec1;
static CriticalSection critSec2;
static CriticalSection critSec3;
static CriticalSection critSec4;
static CriticalSection critSec5;

// GLOBAL COMMAND LINE ARGUMENTS
// TheSuperHackers @build felipebraz 13/02/2026
// Store argc/argv from main() for use by CommandLine.cpp parseCommandLine() on Linux
// Windows provides these automatically; Linux needs explicit globals
int __argc = 0;          ///< global argument count
char** __argv = nullptr; ///< global argument vector

// GLOBAL WINDOW HANDLE
// TheSuperHackers @build felipebraz 13/02/2026
// ApplicationHWnd is declared extern in GeneralsMD/Code/Main/WinMain.h
// On Linux, we cast SDL_Window* to HWND type for compatibility
HWND ApplicationHWnd = nullptr;  ///< our application window handle

// GLOBAL SDL3 WINDOW
// GeneralsX @feature felipebraz 16/02/2026
// SDL3 window created in main() before GameMain(), stored globally for engine access
SDL_Window* TheSDL3Window = nullptr;

// GAME TEXT FILE PATHS
// TheSuperHackers @build felipebraz 13/02/2026
// GameText.cpp uses these paths to load CSF and STR files (game localization)
// Format %s is replaced with language code in GameTextManager::init()
// GeneralsX @bugfix BenderAI 13/02/2026 - Fix case-sensitivity on Linux (generals.csf vs Generals.csf)
const Char *g_csfFile = "data/%s/generals.csf";  ///< CSF file path (lowercase for Linux compatibility)
const Char *g_strFile = "data/Generals.str";     ///< STR file path

// Extern declarations (from GameMain.cpp)
extern Int GameMain();

/**
 * FilterSoftwareVulkanICDs
 *
 * Sets VK_DRIVER_FILES to only hardware Vulkan ICDs, excluding LLVMpipe/lavapipe.
 *
 * Workaround for Mesa/LLVM 20.x bug: libvulkan_lvp.so (LLVMpipe Vulkan ICD) crashes
 * during dlopen() static initialization with a null-ptr deref in llvm::Regex::Regex().
 * The Vulkan loader loads ALL ICDs found in the ICD directories when
 * vkEnumerateInstanceExtensionProperties() is called, which triggers the crash.
 * Filtering hardware-only ICDs via VK_DRIVER_FILES prevents loading libvulkan_lvp.so.
 *
 * Only applied when neither VK_DRIVER_FILES nor VK_ICD_FILENAMES is already set,
 * so the user can always override by setting those variables externally.
 *
 * GeneralsX @bugfix BenderAI 06/03/2026
 */
#if !defined(__ANDROID__)
static void FilterSoftwareVulkanICDs()
{
	if (getenv("VK_DRIVER_FILES") || getenv("VK_ICD_FILENAMES")) {
		return;
	}

	auto icd_is_software = [](const char *name) -> bool {
		char low[256] = "";
		for (int i = 0; name[i] && i < 255; ++i) {
			low[i] = (char)tolower((unsigned char)name[i]);
		}
		return strstr(low, "lvp") || strstr(low, "lavapipe") || strstr(low, "softpipe") || strstr(low, "llvmpipe");
	};

	static char hw_icds[4096] = "";
	const char *patterns[] = {
		"/usr/share/vulkan/icd.d/*.json",
		"/etc/vulkan/icd.d/*.json",
		nullptr
	};

	glob_t gl = {};
	int gflags = 0;
	for (int i = 0; patterns[i]; ++i) {
		if (glob(patterns[i], gflags, nullptr, &gl) == 0) {
			gflags = GLOB_APPEND;
		}
	}

	bool found_hw = false;
	for (size_t i = 0; i < gl.gl_pathc; ++i) {
		const char *path = gl.gl_pathv[i];
		const char *base = strrchr(path, '/');
		base = base ? base + 1 : path;
		if (icd_is_software(base)) {
			fprintf(stderr, "INFO: Vulkan ICD filter: skipping software ICD '%s'\n", base);
			continue;
		}
		if (found_hw) {
			strncat(hw_icds, ":", sizeof(hw_icds) - strlen(hw_icds) - 1);
		}
		strncat(hw_icds, path, sizeof(hw_icds) - strlen(hw_icds) - 1);
		found_hw = true;
	}
	globfree(&gl);

	if (found_hw) {
		setenv("VK_DRIVER_FILES", hw_icds, 1);
		fprintf(stderr, "INFO: Vulkan ICD filter: VK_DRIVER_FILES=%s\n", hw_icds);
	} else {
		fprintf(stderr, "WARNING: Vulkan ICD filter: no hardware ICDs found, LLVMpipe exclusion skipped\n");
		fprintf(stderr, "WARNING: If startup crashes in libvulkan_lvp.so, set VK_DRIVER_FILES manually\n");
	}
}
#endif // !__ANDROID__

/**
 * FilterPipeWireOpenAL
 *
 * Sets ALSOFT_DRIVERS to skip PipeWire, falling back to pulse/alsa.
 *
 * Workaround for openal-soft PipeWire backend crash: alcOpenDevice() segfaults
 * inside the PipeWire backend while opening the default playback device.
 * The crash occurs in PipeWire's stream/context internals and is unrecoverable
 * from userspace. Excluding PipeWire via ALSOFT_DRIVERS causes openal-soft to
 * fall back to the PulseAudio backend, which works correctly on PipeWire systems
 * via the PulseAudio compatibility layer.
 *
 * NOTE: openal-soft reads ALSOFT_DRIVERS from a static global constructor when
 * libopenal.so is loaded by the dynamic linker, which is before main() runs.
 * This function is therefore only effective for builds that use lazy
 * initialization. The authoritative fix is in the launch scripts (run-linux-zh.sh
 * etc.), which set ALSOFT_DRIVERS before the binary starts.
 *
 * Only applied when ALSOFT_DRIVERS is not already set by the user.
 *
 * GeneralsX @bugfix 09/03/2026
 */
static void FilterPipeWireOpenAL()
{
	// GeneralsX @bugfix Copilot 24/03/2026 PipeWire/OpenAL workaround is Linux-only; keep macOS CoreAudio backend selection untouched.
	// GeneralsX @build Android port 06/07/2026 Android defines __linux__ too, but has neither
	// PipeWire nor PulseAudio — forcing ALSOFT_DRIVERS=pulse,alsa here would silently mute the
	// device (openal-soft must pick its OpenSL/AAudio backends). Desktop Linux only.
	#if defined(__linux__) && !defined(__ANDROID__)
	// Crash: alcOpenDevice() hits 'movaps %xmm1,0x26260(%rbx)' — SSE movaps requires
	// 16-byte alignment; a misaligned ALCdevice struct faults regardless of backend.
	// Disabling CPU extensions forces openal-soft to use scalar code that has no
	// alignment requirements. Also exclude pipewire which has its own crash at
	// device-open time on PipeWire 1.4.x.
	// NOTE: these env vars are authoritative only when set before the binary loads
	// (openal-soft reads them from a static constructor). The launch scripts set them
	// first; this is a best-effort fallback for lazy-init builds.
	if (!getenv("ALSOFT_DISABLE_CPU_EXTS")) {
		setenv("ALSOFT_DISABLE_CPU_EXTS", "all", 1);
		fprintf(stderr, "INFO: OpenAL: ALSOFT_DISABLE_CPU_EXTS=all (movaps alignment crash workaround)\n");
	}
	if (!getenv("ALSOFT_DRIVERS")) {
		setenv("ALSOFT_DRIVERS", "pulse,alsa,oss,jack,null,wave", 1);
		fprintf(stderr, "INFO: OpenAL: ALSOFT_DRIVERS=pulse,alsa,oss,jack,null,wave (pipewire excluded)\n");
	}
	#else
	fprintf(stderr, "INFO: OpenAL: keeping default driver selection on non-Linux platform\n");
	#endif
}

#if defined(__ANDROID__)
/**
 * TryLoadCustomVulkanDriver
 *
 * GeneralsX @feature Android port 10/07/2026 Optional user-supplied Vulkan
 * driver (most commonly a Mesa Turnip build for Adreno GPUs), loaded via
 * libadrenotools (https://github.com/bylaws/libadrenotools, BSD-2-Clause).
 * adrenotools installs a linker-namespace-bypass hook that transparently
 * redirects EVERY later dlopen("libvulkan.so") call in this process to the
 * custom driver -- including DXVK's own internal load in
 * references/fbraz3-dxvk/src/vulkan/vulkan_loader.cpp, which is a separate
 * meson-built .so and is never patched or made aware of this. Must run
 * before that first happens, i.e. before SDL_Vulkan_LoadLibrary() below.
 *
 * The Setup app (SetupActivity.java) writes <internalPath>/custom_driver.cfg
 * (one line: the driver .so's soname) and unpacks the driver itself into
 * <internalPath>/custom_driver/ -- either because the user imported one via
 * its "Custom Vulkan Driver" section, or because
 * applyRecommendedDriverIfNeeded() auto-selected the bundled Turnip build
 * for an Adreno phone whose stock driver reports less than Vulkan 1.3. Both
 * files are absent when neither applies, in which case this is a no-op and
 * the stock vendor driver loads exactly as before.
 *
 * Adreno-only: Turnip has no Mali backend, so this cannot help phones whose
 * GPU is Mali (see the Mali-G76 case documented further down this file) --
 * those remain capped at whatever Vulkan version their proprietary driver
 * reports. What it DOES help: Adreno phones whose stock driver reports
 * Vulkan 1.1/1.2 while DXVK 2.6 needs 1.3.
 */
static void TryLoadCustomVulkanDriver(const char *internalPath)
{
	char cfgPath[1024];
	snprintf(cfgPath, sizeof(cfgPath), "%s/custom_driver.cfg", internalPath);
	FILE *cfg = fopen(cfgPath, "r");
	if (cfg == nullptr) {
		return;  // no custom driver configured -- stock driver loads as usual
	}
	char driverName[256] = {0};
	bool haveDriverName = (fgets(driverName, sizeof(driverName), cfg) != nullptr);
	fclose(cfg);
	if (!haveDriverName) {
		return;
	}
	size_t len = strlen(driverName);
	while (len > 0 && (driverName[len - 1] == '\n' || driverName[len - 1] == '\r')) {
		driverName[--len] = '\0';
	}
	if (len == 0) {
		return;
	}

	char driverDir[1024];
	snprintf(driverDir, sizeof(driverDir), "%s/custom_driver", internalPath);
	if (access(driverDir, R_OK) != 0) {
		fprintf(stderr, "WARNING: custom_driver.cfg names '%s' but %s doesn't exist -- using stock Vulkan driver\n",
		        driverName, driverDir);
		return;
	}

	// hookLibDir MUST be exactly what ApplicationInfo.nativeLibraryDir
	// returns -- it contains a random per-install path component on modern
	// Android and cannot be hardcoded, so fetch it via JNI from the JVM side
	// the app is already running in (SDLActivity).
	JNIEnv *jni = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
	jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
	if (jni == nullptr || activity == nullptr) {
		fprintf(stderr, "WARNING: custom Vulkan driver requested but no JNI environment -- using stock driver\n");
		return;
	}

	std::string hookLibDir;
	{
		jclass activityClass = jni->GetObjectClass(activity);
		jmethodID getApplicationInfo = jni->GetMethodID(
			activityClass, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
		jobject appInfo = jni->CallObjectMethod(activity, getApplicationInfo);
		jclass appInfoClass = jni->GetObjectClass(appInfo);
		jfieldID nativeLibraryDirField = jni->GetFieldID(appInfoClass, "nativeLibraryDir", "Ljava/lang/String;");
		jstring nativeLibraryDirStr = static_cast<jstring>(jni->GetObjectField(appInfo, nativeLibraryDirField));
		if (nativeLibraryDirStr != nullptr) {
			const char *chars = jni->GetStringUTFChars(nativeLibraryDirStr, nullptr);
			if (chars != nullptr) {
				hookLibDir = chars;
				jni->ReleaseStringUTFChars(nativeLibraryDirStr, chars);
			}
			jni->DeleteLocalRef(nativeLibraryDirStr);
		}
		jni->DeleteLocalRef(appInfo);
		jni->DeleteLocalRef(appInfoClass);
		jni->DeleteLocalRef(activityClass);
	}
	if (hookLibDir.empty()) {
		fprintf(stderr, "WARNING: could not resolve nativeLibraryDir -- using stock Vulkan driver\n");
		return;
	}

	void *mappingHandle = nullptr;
	void *lib = adrenotools_open_libvulkan(
		RTLD_NOW,
		ADRENOTOOLS_DRIVER_CUSTOM,
		/* tmpLibDir */ internalPath,
		hookLibDir.c_str(),
		driverDir,
		driverName,
		/* fileRedirectDir */ nullptr,
		&mappingHandle);

	if (lib == nullptr) {
		fprintf(stderr, "WARNING: adrenotools_open_libvulkan('%s') failed -- falling back to stock Vulkan driver\n",
		        driverName);
		return;
	}
	fprintf(stderr, "INFO: Loaded custom Vulkan driver '%s' via libadrenotools (hookLibDir=%s)\n",
	        driverName, hookLibDir.c_str());
}
#endif // __ANDROID__

/**
 * CreateGameEngine
 *
 * Factory function for SDL3GameEngine on Linux.
 * Called by GameMain() to instantiate platform-specific engine.
 *
 * @return SDL3GameEngine instance
 */
GameEngine *CreateGameEngine(void)
{
	fprintf(stderr, "INFO: CreateGameEngine() - Creating SDL3GameEngine for Linux\n");
	SDL3GameEngine *engine = NEW SDL3GameEngine();
	return engine;
}

/**
 * main
 *
 * Linux entry point (replaces WinMain on Windows).
 * Initializes subsystems and calls GameMain().
 *
 * @param argc Command line argument count
 * @param argv Command line arguments
 * @return Exit code (0 = success)
 */
int main(int argc, char* argv[])
{
	int exitcode = 1;

	// TheSuperHackers @build felipebraz 13/02/2026
	// Store command line arguments in globals for CommandLine.cpp parser
	__argc = argc;
	__argv = argv;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Diagnostic capture: an icon-launched app's stderr goes nowhere we can read,
	// so mirror it to a file in Library/Caches (purgeable, not user-visible). This
	// lets us pull a full engine log after an on-device session — essential for
	// debugging mode-specific issues (e.g. Generals Challenge radar/scripts) that
	// only the user can reproduce. Pull with: devicectl ... copy from
	// Library/Caches/generals-stderr.log. Remove once the relevant bugs are fixed.
	{
		// Quiet DXVK at the source: the d3d8 layer's per-call warns (e.g. an
		// unimplemented render state set every frame) wrote hundreds of MB per
		// long session. The shipped dxvk.conf also sets logLevel=none; the env
		// covers modules that read it before the config.
		setenv("DXVK_LOG_LEVEL", "none", 0);
		const char *diagHome = getenv("HOME");
		if (diagHome != nullptr) {
			char diagPath[1024];
			char prevPath[1024];
			// Documents, not Library/Caches: Caches is purgeable (a device restart or
			// storage pressure can empty it), and Documents is user-reachable via the
			// Files app since the bundle enables UIFileSharingEnabled.
			snprintf(diagPath, sizeof(diagPath), "%s/Documents/generals-stderr.log", diagHome);
			// Keep the previous session's log: a session that ends in a memory kill
			// leaves no OS crash report, so the prior log is often the only evidence.
			snprintf(prevPath, sizeof(prevPath), "%s/Documents/generals-stderr-prev.log", diagHome);
			rename(diagPath, prevPath);
			// Filtered + capped sink instead of a raw freopen: per-frame debug spam
			// (upstream [GX-ISSUE144] font traces, [INI] loader traces, residual DXVK
			// warns) is dropped, and the file stops growing at 8 MB so a marathon
			// session cannot eat device storage. funopen() is fine here: this is
			// Darwin-only code.
			static int s_logFd = -1;
			s_logFd = open(diagPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (s_logFd >= 0) {
				static size_t s_logWritten = 0;
				FILE *sink = funopen(nullptr,
					nullptr,
					[](void *, const char *buf, int len) -> int {
						static const size_t kLogCap = 8u * 1024u * 1024u;
						if (s_logFd < 0) return len;
						if (len > 13 &&
						    (memcmp(buf, "[GX-ISSUE144]", 13) == 0 ||
						     memcmp(buf, "[INI] ", 6) == 0 ||
						     memcmp(buf, "warn:  D3D8De", 13) == 0)) {
							return len;  // drop known per-frame spam, report consumed
						}
						if (s_logWritten >= kLogCap) {
							// past the cap, still record errors — the tail of a dying
							// session is this log's whole reason to exist
							static bool s_capMarked = false;
							if (!s_capMarked) {
								s_capMarked = true;
								const char *mark = "[log capped: non-error lines dropped from here]\n";
								write(s_logFd, mark, strlen(mark));
							}
							if (len > 4 && (memcmp(buf, "err:", 4) == 0 ||
							                memcmp(buf, "ERROR", 5) == 0 ||
							                memcmp(buf, "FATAL", 5) == 0)) {
								write(s_logFd, buf, (size_t)len);
							}
							return len;
						}
						ssize_t w = write(s_logFd, buf, (size_t)len);
						if (w > 0) s_logWritten += (size_t)w;
						return len;
					},
					nullptr, nullptr);
				if (sink != nullptr) {
					*stderr = *sink;  // classic Darwin stderr swap; stderr is a FILE, not a macro here
					setvbuf(stderr, nullptr, _IOLBF, 0);  // line-buffered so a crash still flushes recent lines
				}
			}
		}
	}

	// The engine resolves all game data relative to the working directory.
	// Preferred layout: assets ship read-only INSIDE the signed app bundle
	// (<bundle>/GameData), the iOS-sanctioned home for app resources — the
	// install is then fully self-contained. Dev builds packaged without
	// assets fall back to the Documents folder (Files-app accessible).
	// User data (saves, Options.ini) always lives in Library/Application
	// Support via the engine's user-data path; never in the bundle.
	{
		const char *home = getenv("HOME");

		// <bundle>/GameData, derived from the executable path (argv[0])
		char bundleData[1024] = {0};
		if (argc > 0 && argv[0] != nullptr) {
			const char *slash = strrchr(argv[0], '/');
			if (slash != nullptr) {
				const size_t dirLen = (size_t)(slash - argv[0]);
				if (dirLen < sizeof(bundleData) - 16) {
					memcpy(bundleData, argv[0], dirLen);
					snprintf(bundleData + dirLen, sizeof(bundleData) - dirLen, "/GameData");
				}
			}
		}

		bool usingBundleData = false;
		if (bundleData[0] != '\0' && access(bundleData, R_OK) == 0) {
			if (chdir(bundleData) == 0) {
				usingBundleData = true;
				fprintf(stderr, "INFO: iOS working directory (bundle): %s\n", bundleData);
			}
		}
		if (!usingBundleData && home != nullptr) {
			char docs[1024];
			snprintf(docs, sizeof(docs), "%s/Documents", home);
			if (chdir(docs) != 0) {
				fprintf(stderr, "WARNING: chdir(%s) failed: %s\n", docs, strerror(errno));
			} else {
				fprintf(stderr, "INFO: iOS working directory (Documents): %s\n", docs);
			}
		}

		if (home != nullptr) {
			// Keep DXVK's shader cache in Library/Caches: purgeable under
			// storage pressure, excluded from iCloud backup, invisible in the
			// Files app. Must be set before the d3d8 dylib loads.
			char cacheDir[1024];
			snprintf(cacheDir, sizeof(cacheDir), "%s/Library/Caches", home);
			mkdir(cacheDir, 0755);
			setenv("DXVK_STATE_CACHE_PATH", cacheDir, 0);

			if (usingBundleData) {
				// Seed default settings on first run (full detail instead of the
				// 2003 auto-detect, which drops unknown GPUs to Low).
				char userDataDir[1024], optionsPath[1024];
				snprintf(userDataDir, sizeof(userDataDir),
				         "%s/Library/Application Support/GeneralsX/GeneralsZH", home);
				snprintf(optionsPath, sizeof(optionsPath), "%s/Options.ini", userDataDir);
				if (access(optionsPath, F_OK) != 0 && access("DefaultOptions.ini", R_OK) == 0) {
					std::error_code fsError;
					std::filesystem::create_directories(userDataDir, fsError);
					std::filesystem::copy_file("DefaultOptions.ini", optionsPath, fsError);
					if (!fsError) {
						fprintf(stderr, "INFO: Seeded default Options.ini\n");
					}
				}

				// One-time tidy-up: remove asset copies from Documents now that
				// the bundle carries them. Guarded by a sentinel so it truly runs
				// once — Documents is exposed via the Files app, and anything the
				// user places there later (mods, custom maps) must never be touched.
				// "Maps" is deliberately NOT in the list: it is where user maps live.
				char docs[1024];
				snprintf(docs, sizeof(docs), "%s/Documents", home);
				char sentinel[1024];
				snprintf(sentinel, sizeof(sentinel), "%s/.bundle-assets-tidied", docs);
				if (access(sentinel, F_OK) != 0) {
					std::error_code fsError;
					for (const auto &entry : std::filesystem::directory_iterator(docs, fsError)) {
						const std::string name = entry.path().filename().string();
						const bool isShippedAsset =
							(name.size() > 4 && name.compare(name.size() - 4, 4, ".big") == 0) ||
							name == "Data" || name == "Window" || name == "ZH_Generals" ||
							name == "fonts" || name == "_CommonRedist" ||
							name == "dxvk.conf" || name == "GeneralsXZH.dxvk-cache" ||
							name == "GeneralsXZH_d3d9.log";
						if (isShippedAsset) {
							fprintf(stderr, "INFO: tidy-up removing shipped asset copy: %s\n", name.c_str());
							std::error_code removeError;
							std::filesystem::remove_all(entry.path(), removeError);
						}
					}
					if (!fsError) {  // a failed scan must retry next launch, not fail closed forever
						FILE *s = fopen(sentinel, "w");
						if (s) fclose(s);
					}
				}
			}
		}
	}
#endif

#if defined(__ANDROID__)
	// GeneralsX @build Android port 06/07/2026 Android process environment.
	//
	// An Android app starts with cwd "/" and no HOME. The engine resolves game
	// data relative to the working directory and user data via $HOME (XDG), so
	// both are pointed into the app's own storage before anything else runs:
	//
	//   game data  -> <external files dir>            (user drops the .big files
	//                  /storage/emulated/0/Android/data/<pkg>/files — reachable
	//                  over USB/MTP and adb; the Java shell extracts bundled
	//                  fonts/, dxvk.conf and DefaultOptions.ini there on launch)
	//   user data  -> <internal files dir>/.local/share/GeneralsX/GeneralsZH
	//                  (via HOME; survives reinstalls of external storage wipes,
	//                  never user-visible, holds Options.ini + saves + maps cache)
	//   DXVK cache -> <cache dir> (purgeable by the OS under storage pressure)
	{
		// GeneralsX @bugfix Android port 07/07/2026 "error", not "none": a
		// real-device run reached Direct3DCreate8() and threw with no
		// information beyond "unknown exception" — DXVK's own error log is
		// exactly what would explain a failure that deep (unsupported
		// instance/device extension, missing Vulkan feature, etc.) and
		// "none" was silencing it. "error" still excludes the per-frame
		// WARN-level render-state spam the iOS port's "none" setting was
		// actually chasing (hundreds of MB/session); only genuine failures
		// are rare enough not to reproduce that problem.
		setenv("DXVK_LOG_LEVEL", "error", 0);

		// GeneralsX @bugfix Android port 08/07/2026 Disable DXVK's background
		// memory defragmentation. Two real-device crash reports (screen
		// rotation in the lobby; unit creation mid-game) symbolized to
		// DxvkMemoryAllocator::createAllocation popping a corrupted
		// allocation-pool free-list head (fault_addr 0x2000000001 = a live
		// allocation's counters written over the recycled node's next
		// pointer) — a use-after-free consistent with the relocation/defrag
		// path moving allocations behind the app's back. Upstream DXVK itself
		// disables defrag on Intel ANV for "unknown reasons" breakage
		// (dxvk_memory.cpp, issue #4395); this Adreno driver appears to be
		// another such case. DXVK_CONFIG is parsed on top of dxvk.conf, so
		// this reaches existing installs whose game folder already carries an
		// older dxvk.conf. setenv(..., 0) keeps any user override in the
		// actual environment intact.
		setenv("DXVK_CONFIG", "dxvk.enableMemoryDefrag = False", 0);

		const char *internalPath = SDL_GetAndroidInternalStoragePath();
		const char *externalPath = SDL_GetAndroidExternalStoragePath();
		const char *cachePath    = SDL_GetAndroidCachePath();

		if (internalPath != nullptr) {
			// HOME drives the engine's user-data dir (GlobalData.cpp XDG branch)
			// and registry.ini location (registryini.cpp). Internal storage: it
			// must survive the user reshuffling game assets in external storage.
			setenv("HOME", internalPath, 1);
		}
		if (cachePath != nullptr) {
			// DXVK shader cache: regenerable, belongs in the OS-purgeable dir.
			// Must be set before the d3d8 .so loads.
			setenv("DXVK_STATE_CACHE_PATH", cachePath, 0);
		}

		// Mirror stderr into a file the user can pull without adb root: Android
		// sends native stderr to /dev/null (only SDL_Log reaches logcat), and an
		// engine this chatty is undebuggable blind. Keep the previous session's
		// log — a session that ends in a low-memory kill leaves no crash report,
		// so the prior log is often the only evidence.
		if (externalPath != nullptr) {
			char logPath[1024], prevPath[1024];
			snprintf(logPath, sizeof(logPath), "%s/generals-stderr.log", externalPath);
			snprintf(prevPath, sizeof(prevPath), "%s/generals-stderr-prev.log", externalPath);
			rename(logPath, prevPath);
			if (freopen(logPath, "w", stderr) != nullptr) {
				setvbuf(stderr, nullptr, _IOLBF, 0);  // line-buffered: a crash still flushes recent lines
			}
		}

		// Game data: highest priority is a folder the user picked in-app via
		// the GeneralsZH Setup app's folder browser (no adb needed) — it
		// writes the chosen absolute path as plain text into
		// <internal>/gamedata_path.txt before this activity ever starts.
		// Falls back to <external>/GameData (the adb-push convention this
		// port originally documented), then the external files dir itself.
		bool didChdir = false;
		if (internalPath != nullptr) {
			char markerPath[1024];
			snprintf(markerPath, sizeof(markerPath), "%s/gamedata_path.txt", internalPath);
			FILE *marker = fopen(markerPath, "r");
			if (marker != nullptr) {
				char customPath[900] = {0};
				if (fgets(customPath, sizeof(customPath), marker) != nullptr) {
					size_t len = strlen(customPath);
					while (len > 0 && (customPath[len - 1] == '\n' || customPath[len - 1] == '\r')) {
						customPath[--len] = '\0';
					}
					if (len > 0) {
						if (chdir(customPath) == 0) {
							didChdir = true;
							fprintf(stderr, "INFO: Android working directory (Setup-selected): %s\n", customPath);
						} else {
							fprintf(stderr, "WARNING: Setup-selected game folder '%s' set but chdir failed: %s\n",
							        customPath, strerror(errno));
						}
					}
				}
				fclose(marker);
			}
		}
		if (!didChdir && externalPath != nullptr) {
			char gameData[1024];
			snprintf(gameData, sizeof(gameData), "%s/GameData", externalPath);
			if (access(gameData, R_OK) == 0 && chdir(gameData) == 0) {
				didChdir = true;
				fprintf(stderr, "INFO: Android working directory (GameData): %s\n", gameData);
			} else if (chdir(externalPath) == 0) {
				didChdir = true;
				fprintf(stderr, "INFO: Android working directory (external files): %s\n", externalPath);
			}
		}
		if (!didChdir) {
			fprintf(stderr, "WARNING: could not enter game data directory (external storage unavailable?)\n");
		}

		// Seed default settings on first run (full detail instead of the 2003
		// GPU auto-detect, which drops unknown GPUs — "Adreno 830" included —
		// to Low LOD with quarter-res textures).
		if (internalPath != nullptr && access("DefaultOptions.ini", R_OK) == 0) {
			char userDataDir[1024], optionsPath[1024];
			snprintf(userDataDir, sizeof(userDataDir),
			         "%s/.local/share/GeneralsX/GeneralsZH", internalPath);
			snprintf(optionsPath, sizeof(optionsPath), "%s/Options.ini", userDataDir);
			if (access(optionsPath, F_OK) != 0) {
				std::error_code fsError;
				std::filesystem::create_directories(userDataDir, fsError);
				std::filesystem::copy_file("DefaultOptions.ini", optionsPath, fsError);
				if (!fsError) {
					fprintf(stderr, "INFO: Seeded default Options.ini\n");
				}
			}
		}
	}
#endif

	fprintf(stderr, "=================================================\n");
	fprintf(stderr, " Command & Conquer Generals: Zero Hour (Linux)\n");
	fprintf(stderr, " SDL3 + DXVK Build\n");
	// GeneralsX @feature Android port 12/07/2026 Print the CI run number
	// (matches the "runNNN" in the release APK's filename) so a crash log
	// or bug report unambiguously proves which build produced it -- we've
	// already misattributed a tester's log to the wrong build once because
	// there was no way to tell them apart.
	fprintf(stderr, " CI build number: %d\n", ANDROID_CI_BUILD_NUMBER);
	fprintf(stderr, "=================================================\n\n");

	try {
		// Initialize critical sections (required by game engine)
		TheAsciiStringCriticalSection = &critSec1;
		TheUnicodeStringCriticalSection = &critSec2;
		TheDmaCriticalSection = &critSec3;
		TheMemoryPoolCriticalSection = &critSec4;
		TheDebugLogCriticalSection = &critSec5;

		// Initialize memory manager early (required by NEW operator)
		initMemoryManager();

		// GeneralsX @bugfix BenderAI 14/02/2026 Initialize Version singleton
		// GameEngine::init() calls updateWindowTitle() which uses TheVersion
		// Must be created before GameMain() to avoid nullptr dereference
		TheVersion = NEW Version;

		// Parse command line (CommandLine class handles argc/argv internally)
		// TheSuperHackers @build felipebraz 10/02/2026 Phase 1.5
		// Store argc/argv for CommandLine parser to access via _NSGetArgc/_NSGetArgv or /proc/self/cmdline
		// For now, let CommandLine::parseCommandLineForStartup() handle this
		CommandLine::parseCommandLineForStartup();

		// GeneralsX @bugfix Copilot 17/05/2026 Skip SDL3 window bootstrap for CLI/headless replay execution.
		const bool isHeadlessMode = (TheGlobalData != nullptr && TheGlobalData->m_headless);
		if (isHeadlessMode) {
			fprintf(stderr, "INFO: Headless mode detected, skipping SDL3 video/Vulkan window initialization\n");
		} else {

		// GeneralsX @bugfix felipebraz 16/02/2026
		// Initialize SDL3 and Vulkan BEFORE creating GameEngine (fighter19 pattern)
		// This prevents LLVM SIGSEGV crash during Vulkan driver enumeration
		// Must be done here, not in SDL3GameEngine::init() which is too late
		fprintf(stderr, "INFO: Initializing SDL3 video subsystem...\n");
#if defined(SAGE_MOBILE_PLATFORM)
		// All mouse events are synthesized by the gesture translator in
		// SDL3GameEngine.cpp; SDL's automatic touch->mouse synthesis would
		// double-deliver finger 1 and fight the two-finger pan logic.
		SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
#if defined(__ANDROID__)
		// Keep running (paused by our own lifecycle gate) instead of blocking
		// inside SDL when the app loses focus; the render gate in
		// SDL3GameEngine::update() owns the pause so state stays consistent.
		SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
#endif
		if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
			fprintf(stderr, "FATAL: Failed to initialize SDL3: %s\n", SDL_GetError());
			return 1;
		}

		// Set DXVK WSI driver before loading Vulkan
		setenv("DXVK_WSI_DRIVER", "SDL3", 1);

		// GeneralsX @bugfix BenderAI 06/03/2026 - Exclude LLVMpipe Vulkan ICD before loading Vulkan.
		// libvulkan_lvp.so crashes during static initialization with LLVM 20.x when the Vulkan
		// loader enumerates all ICDs. Restrict to hardware ICDs first.
#if !defined(__ANDROID__)
		// Desktop-Mesa workaround; Android has no ICD JSON directories — the
		// system Vulkan loader picks the vendor driver (Adreno/Mali) itself.
		FilterSoftwareVulkanICDs();
#endif
		FilterPipeWireOpenAL();

#if defined(__ANDROID__)
		// Must run before SDL_Vulkan_LoadLibrary()/DXVK's own internal
		// dlopen("libvulkan.so") below -- see TryLoadCustomVulkanDriver().
		{
			const char *internalPath = SDL_GetAndroidInternalStoragePath();
			if (internalPath != nullptr) {
				TryLoadCustomVulkanDriver(internalPath);
			}
		}
#endif

		// Load Vulkan library for DXVK DirectX8→Vulkan translation
		fprintf(stderr, "INFO: Loading Vulkan library...\n");
		if (!SDL_Vulkan_LoadLibrary(nullptr)) {
			fprintf(stderr, "WARNING: Failed to load Vulkan: %s\n", SDL_GetError());
			fprintf(stderr, "WARNING: Continuing without Vulkan (may use software rendering)\n");
		}

		// Create SDL3 window with Vulkan support
		fprintf(stderr, "INFO: Creating SDL3 Vulkan window...\n");
		Uint32 windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;  // Start hidden, show after D3D init
#if defined(SAGE_MOBILE_PLATFORM)
		// Request a native-resolution drawable (e.g. 2868x1320 instead of the
		// 956x440 point size). Without this the swapchain renders at point size and
		// the display upscales 3x, visibly blurring textures and terrain.
		windowFlags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
#if defined(__ANDROID__)
		// Fullscreen on Android == immersive mode: hides the status and
		// navigation bars so the RTS UI owns the whole panel.
		windowFlags |= SDL_WINDOW_FULLSCREEN;
#endif
		TheSDL3Window = SDL_CreateWindow(
			"Command & Conquer Generals: Zero Hour",
			1024, 768,  // Default resolution
			windowFlags
		);

		if (!TheSDL3Window) {
			fprintf(stderr, "FATAL: Failed to create SDL3 window: %s\n", SDL_GetError());
			SDL_Quit();
			return 1;
		}

		// Store window handle globally (cast SDL_Window* to HWND for compatibility)
		ApplicationHWnd = (HWND)TheSDL3Window;
		fprintf(stderr, "INFO: SDL3 window created successfully\n");

#if defined(SAGE_MOBILE_PLATFORM)
		// Match the game's internal resolution to the phone screen's aspect ratio.
		// Without this the engine runs its 4:3 default inside the 19.5:9 display:
		// pillarboxed picture and a skewed window->game coordinate mapping. Height
		// stays at the engine's 600px design baseline (UI layouts assume >= 600);
		// width follows the real aspect. Injected as -xres/-yres argv entries so
		// the normal command-line path applies them (user-passed flags still win
		// because the parser lets later arguments override earlier ones... ours go
		// last, so only add them if the user didn't pass explicit -xres/-yres).
		{
			bool userSetRes = false;
			for (int i = 1; i < __argc; ++i) {
				if (strcmp(__argv[i], "-xres") == 0 || strcmp(__argv[i], "-yres") == 0) {
					userSetRes = true;
					break;
				}
			}
#if defined(__ANDROID__)
			// GeneralsX @bugfix Android port 07/07/2026 Even with a plain (non-sensor)
			// "landscape" manifest lock, WindowManager can take a handful of frames to
			// actually apply it to a freshly created Activity's window — especially
			// across the singleInstance task switch from SetupActivity. A single
			// snapshot right after SDL_CreateWindow can still catch a stale portrait
			// size, which then bakes a wrong -xres/-yres for the whole session
			// (confirmed on a real device: the very first screen after Setup rendered
			// pillarboxed into a portrait window while a later screen in the same
			// session was already correctly landscape). Poll briefly for the window
			// to actually report landscape before trusting its size.
			for (int attempt = 0; attempt < 20; ++attempt) {
				int w = 0, h = 0;
				SDL_GetWindowSizeInPixels(TheSDL3Window, &w, &h);
				if (w > h) break;
				SDL_PumpEvents();
				SDL_Delay(50);
			}
#endif
			// Use the pixel size of the high-density drawable: the game renders
			// 1:1 into the native-resolution swapchain, and fonts/UI rescale via
			// the engine's resolution-aware font scaling (GlobalLanguage).
			int winW = 0, winH = 0;
			SDL_GetWindowSizeInPixels(TheSDL3Window, &winW, &winH);
			if (!userSetRes && winW > 0 && winH > 0 && winW > winH) {
				static char xresVal[16], yresVal[16];
				static char xresFlag[] = "-xres";
				static char yresFlag[] = "-yres";
				// GeneralsX @bugfix Android port 08/07/2026 A prior version of this
				// code rendered at a REDUCED internal resolution and let the
				// pillarbox blit upscale to the full panel, expecting that to grow
				// buttons/controlbar geometry along with text. On real devices,
				// only text actually got bigger (GlobalLanguage's font-size scaling
				// is resolution-aware on its own); widget geometry did not change
				// size at all — the .wnd layout's own resolution-ratio math and the
				// pillarbox stretch cancel out, so shrinking the internal
				// resolution has no net visual effect on non-text UI. Reverted to
				// plain native-resolution rendering; only "Menu Text Size"
				// (ResolutionFontAdjustment, in the Settings app) actually changes
				// anything on screen.
				int yres = winH;
				int xres = winW;
				xres &= ~1;  // keep it even
				snprintf(xresVal, sizeof(xresVal), "%d", xres);
				snprintf(yresVal, sizeof(yresVal), "%d", yres);

				static char* newArgv[64];
				int n = 0;
				for (int i = 0; i < __argc && n < 59; ++i) {
					newArgv[n++] = __argv[i];
				}
				newArgv[n++] = xresFlag;
				newArgv[n++] = xresVal;
				newArgv[n++] = yresFlag;
				newArgv[n++] = yresVal;
				newArgv[n] = nullptr;
				__argv = newArgv;
				__argc = n;
				fprintf(stderr, "INFO: mobile internal resolution set to %sx%s (window %dx%d)\n",
				        xresVal, yresVal, winW, winH);
			}
		}
#endif
		}

		// Call cross-platform game entry point
		exitcode = GameMain();

		fprintf(stderr, "INFO: GameMain() returned with code %d\n", exitcode);

	} catch (const std::exception& e) {
		fprintf(stderr, "FATAL: Unhandled exception in main(): %s\n", e.what());
		exitcode = 1;
	} catch (...) {
		fprintf(stderr, "FATAL: Unknown exception in main()\n");
		exitcode = 1;
	}

	// Cleanup SDL3 resources
	if (TheSDL3Window) {
		SDL_DestroyWindow(TheSDL3Window);
		TheSDL3Window = nullptr;
		ApplicationHWnd = nullptr;
	}
	SDL_Quit();

	// GeneralsX @bugfix BenderAI 14/02/2026 Cleanup Version singleton
	if (TheVersion) {
		delete TheVersion;
		TheVersion = nullptr;
	}

	// GeneralsX @bugfix BenderAI 19/02/2026 Shutdown memory manager BEFORE nulling critical
	// sections. Without this, global pool destructors (ObjectPoolClass) crash during atexit()
	// because they call ::operator delete after the memory manager is already gone (SIGSEGV).
	// Matches WinMain.cpp cleanup order: TheVersion -> shutdownMemoryManager -> null critSecs.
	shutdownMemoryManager();

	// Cleanup critical sections (after memory manager, which may use them during shutdown)
	TheAsciiStringCriticalSection = nullptr;
	TheUnicodeStringCriticalSection = nullptr;
	TheDmaCriticalSection = nullptr;
	TheMemoryPoolCriticalSection = nullptr;
	TheDebugLogCriticalSection = nullptr;

	fprintf(stderr, "\nExiting with code %d\n", exitcode);

	// GeneralsX @bugfix BenderAI 25/02/2026 — use _exit() to skip C++ global destructors.
	// On macOS, __cxa_finalize_ranges runs ObjectPoolClass<X,256> global dtors after main() returns.
	// Those dtors crash with a corrupted BlockListHead (SIGSEGV at 0x4ade32ec4ade0018) because
	// pool block memory was already reused/overwritten during game shutdown.
	// Windows never had this problem — ExitProcess() terminates without running C++ global dtors.
	// _exit() matches that behavior. Explicit cleanup already done above (SDL_Quit, shutdownMemoryManager).
	_exit(exitcode);
}

#endif // !_WIN32
