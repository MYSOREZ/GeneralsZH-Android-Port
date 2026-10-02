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
** gles_pipeline.cpp - D3D8 fixed function on WebGL2.
** GeneralsX @build web-port 05/07/2026 - Web port Phase 2
**
** See gles_pipeline.h. Correctness-first: GL state is (re)applied per draw,
** uniforms re-uploaded per draw; programs and texture objects are cached.
*/

// NOTE: this file is #included at the bottom of d3d8gles.cpp (single TU) so
// it can access the device/resource class internals defined there.
#include "gles_pipeline.h"
#include "gles_dispatch.h"
#include "gles_thread.h"

// Set by d3d8gles_SetTwoSidedStencil() (defined further down, with the rest of the exported
// engine hooks) and read by applyFixedState().
static bool s_gxGlesReady = false;
static bool s_gxTwoSidedStencil = false;
static DWORD s_gxTwoSidedBackPass = 0;
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <sys/stat.h>

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <sys/resource.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

// GeneralsX @performance Android port 01/10/2026 GPU time per frame (GL_EXT_disjoint_timer_query),
// to tell a GPU-bound frame from a CPU-bound one: a TIME_ELAPSED query spans from just after one
// swap to just before the next, and results are read a few frames later, when available, so
// nothing ever waits for the GPU. Runs where the GL calls run (the render thread when it is on).
namespace {
struct GpuFrameTimer
{
	typedef void (GL_APIENTRY *PFN_GenQueries)(GLsizei, GLuint *);
	typedef void (GL_APIENTRY *PFN_BeginQuery)(GLenum, GLuint);
	typedef void (GL_APIENTRY *PFN_EndQuery)(GLenum);
	typedef void (GL_APIENTRY *PFN_GetQueryObjectuiv)(GLuint, GLenum, GLuint *);
	static constexpr GLenum kTimeElapsed = 0x88BF;   // GL_TIME_ELAPSED_EXT
	static constexpr GLenum kResult = 0x8866;        // GL_QUERY_RESULT_EXT
	static constexpr GLenum kAvailable = 0x8867;     // GL_QUERY_RESULT_AVAILABLE_EXT
	static constexpr int kQueries = 4;
	PFN_BeginQuery begin = nullptr;
	PFN_EndQuery end = nullptr;
	PFN_GetQueryObjectuiv get = nullptr;
	GLuint queries[kQueries] = {};
	bool pending[kQueries] = {};
	int next = 0;
	bool active = false;
	bool ok = false;
	std::atomic<uint64_t> gpuNs{0};
	std::atomic<unsigned> frames{0};

	void frameEnd()
	{
		if (active) {
			end(kTimeElapsed);
			pending[next] = true;
			next = (next + 1) % kQueries;
			active = false;
		}
		for (int i = 0; i < kQueries; i++) {
			if (!pending[i])
				continue;
			GLuint available = 0;
			get(queries[i], kAvailable, &available);
			if (!available)
				continue;
			GLuint ns = 0;
			get(queries[i], kResult, &ns);
			gpuNs.fetch_add(ns, std::memory_order_relaxed);
			frames.fetch_add(1, std::memory_order_relaxed);
			pending[i] = false;
		}
	}
	void frameBegin()
	{
		if (pending[next])
			return; // still in flight: skip timing this frame rather than wait
		begin(kTimeElapsed, queries[next]);
		active = true;
	}
};
GpuFrameTimer s_gpuTimer;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static bool g_glTrace = false;

// 64-bit FNV-1a, chainable through `seed`: keys the on-disk program cache.
static uint64_t fnv1a64(const void *data, size_t size, uint64_t seed)
{
	const unsigned char *p = static_cast<const unsigned char *>(data);
	uint64_t h = seed;
	for (size_t i = 0; i < size; i++) {
		h ^= p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

// GeneralsX @build Android port GLES experiment 08/30/2026 Uniform Buffer
// Object binding point for the camera (view+proj) block -- see its use in
// getProgram()/applyUniforms() below for why this exists: a plain
// glUniformMatrix4fv upload is per-PROGRAM state (GL forgets it across a
// glUseProgram switch), so a scene that alternates shader programs a lot
// (different FVF/lighting/material/fog/texture-stage combos for terrain vs.
// units vs. particles vs. decals) was forced to re-upload the camera on
// every single program switch even when the camera hadn't moved at all --
// confirmed via a real device log's per-block uniform-cache breakdown
// dropping to ~50% mid-battle. A UBO's DATA lives independently of which
// program is currently bound; only the bound *buffer* at this binding
// point matters, and that's set once here, not per-draw or per-program.
// So the camera now only needs a real re-upload when its actual value
// changes (once per frame in the common case), never merely because the
// active program changed.
static const GLuint kViewProjUBOBinding = 0;

#define GLTRACE(...)                              \
	do {                                          \
		if (g_glTrace) {                          \
			fprintf(stderr, "[d3d8gles.gl] ");   \
			fprintf(stderr, __VA_ARGS__);         \
			fprintf(stderr, "\n");                \
		}                                         \
	} while (0)

// Log an unsupported state combination once per session.
#define WARN_ONCE(flagvar, ...)                       \
	do {                                              \
		static bool flagvar = false;                  \
		if (!flagvar) {                               \
			flagvar = true;                           \
			fprintf(stderr, "[d3d8gles] WARN: ");    \
			fprintf(stderr, __VA_ARGS__);             \
			fprintf(stderr, "\n");                    \
		}                                             \
	} while (0)

// Same, but keyed on a runtime value instead of a call site: warns once per
// distinct value (up to 8 of them), so one unimplemented enum does not drown
// out the next. Used for the texture-stage op fallback, where the whole point
// is to name WHICH op is missing.
#define WARN_ONCE_ARG(val, ...)                                          \
	do {                                                                  \
		static unsigned s_seen[8] = {0, 0, 0, 0, 0, 0, 0, 0};             \
		static int s_seenCount = 0;                                       \
		bool s_already = false;                                           \
		for (int s_i = 0; s_i < s_seenCount; s_i++) {                     \
			if (s_seen[s_i] == (unsigned)(val)) { s_already = true; break; } \
		}                                                                 \
		if (!s_already && s_seenCount < 8) {                              \
			s_seen[s_seenCount++] = (unsigned)(val);                      \
			fprintf(stderr, "[d3d8gles] WARN: ");                        \
			fprintf(stderr, __VA_ARGS__);                                 \
			fprintf(stderr, "\n");                                        \
		}                                                                 \
	} while (0)

static float dwordToFloat(DWORD v)
{
	float f;
	memcpy(&f, &v, sizeof(f));
	return f;
}

static void argbToFloats(uint32_t argb, float out[4])
{
	out[0] = ((argb >> 16) & 0xFF) / 255.0f;
	out[1] = ((argb >> 8) & 0xFF) / 255.0f;
	out[2] = (argb & 0xFF) / 255.0f;
	out[3] = ((argb >> 24) & 0xFF) / 255.0f;
}

static GLenum d3dCmpToGL(DWORD cmp)
{
	switch (cmp) {
	case D3DCMP_NEVER: return GL_NEVER;
	case D3DCMP_LESS: return GL_LESS;
	case D3DCMP_EQUAL: return GL_EQUAL;
	case D3DCMP_LESSEQUAL: return GL_LEQUAL;
	case D3DCMP_GREATER: return GL_GREATER;
	case D3DCMP_NOTEQUAL: return GL_NOTEQUAL;
	case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
	case D3DCMP_ALWAYS: default: return GL_ALWAYS;
	}
}

static GLenum d3dBlendToGL(DWORD b)
{
	switch (b) {
	case D3DBLEND_ZERO: return GL_ZERO;
	case D3DBLEND_ONE: return GL_ONE;
	case D3DBLEND_SRCCOLOR: return GL_SRC_COLOR;
	case D3DBLEND_INVSRCCOLOR: return GL_ONE_MINUS_SRC_COLOR;
	case D3DBLEND_SRCALPHA: return GL_SRC_ALPHA;
	case D3DBLEND_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
	case D3DBLEND_DESTALPHA: return GL_DST_ALPHA;
	case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
	case D3DBLEND_DESTCOLOR: return GL_DST_COLOR;
	case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
	case D3DBLEND_SRCALPHASAT: return GL_SRC_ALPHA_SATURATE;
	default: return GL_ONE;
	}
}

static GLenum d3dStencilOpToGL(DWORD op)
{
	switch (op) {
	case D3DSTENCILOP_KEEP: return GL_KEEP;
	case D3DSTENCILOP_ZERO: return GL_ZERO;
	case D3DSTENCILOP_REPLACE: return GL_REPLACE;
	case D3DSTENCILOP_INCRSAT: return GL_INCR;
	case D3DSTENCILOP_DECRSAT: return GL_DECR;
	case D3DSTENCILOP_INVERT: return GL_INVERT;
	case D3DSTENCILOP_INCR: return GL_INCR_WRAP;
	case D3DSTENCILOP_DECR: return GL_DECR_WRAP;
	default: return GL_KEEP;
	}
}

// FVF layout description.
struct FVFLayout {
	bool xyzrhw = false;
	bool hasNormal = false;
	bool hasPSize = false;
	bool hasDiffuse = false;
	bool hasSpecular = false;
	int texCount = 0;
	int texSize[8] = {2, 2, 2, 2, 2, 2, 2, 2}; // floats per set
	int posOffset = 0;
	int normalOffset = -1;
	int diffuseOffset = -1;
	int specularOffset = -1;
	int texOffset[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
	int stride = 0;
};

static bool parseFVF(unsigned fvf, FVFLayout *out)
{
	FVFLayout l;
	const unsigned pos = fvf & D3DFVF_POSITION_MASK;
	int off = 0;
	l.posOffset = 0;
	if (pos == D3DFVF_XYZ) {
		off = 12;
	} else if (pos == D3DFVF_XYZRHW) {
		l.xyzrhw = true;
		off = 16;
	} else {
		// XYZB1-5 blend weights unused by the engine's FF paths.
		return false;
	}
	if (fvf & D3DFVF_NORMAL) {
		l.hasNormal = true;
		l.normalOffset = off;
		off += 12;
	}
	if (fvf & D3DFVF_PSIZE) {
		l.hasPSize = true;
		off += 4;
	}
	if (fvf & D3DFVF_DIFFUSE) {
		l.hasDiffuse = true;
		l.diffuseOffset = off;
		off += 4;
	}
	if (fvf & D3DFVF_SPECULAR) {
		l.hasSpecular = true;
		l.specularOffset = off;
		off += 4;
	}
	l.texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	if (l.texCount > 8) l.texCount = 8;
	for (int i = 0; i < l.texCount; i++) {
		const unsigned fmt = (fvf >> (16 + i * 2)) & 0x3;
		int size = 2;
		switch (fmt) {
		case 0: size = 2; break; // D3DFVF_TEXTUREFORMAT2
		case 1: size = 3; break; // D3DFVF_TEXTUREFORMAT3
		case 2: size = 4; break; // D3DFVF_TEXTUREFORMAT4
		case 3: size = 1; break; // D3DFVF_TEXTUREFORMAT1
		}
		l.texSize[i] = size;
		l.texOffset[i] = off;
		off += size * 4;
	}
	l.stride = off;
	*out = l;
	return true;
}

// ---------------------------------------------------------------------------
// Program cache
// ---------------------------------------------------------------------------

struct WebGLPipeline::ProgramInfo {
	GLuint prog = 0;
	// uniforms
	GLint uWorld = -1;
	// uView/uProj moved into the ViewProjBlock UBO (kViewProjUBOBinding) --
	// no per-program location for them anymore, see getProgram()'s shader
	// declaration and the post-link glUniformBlockBinding() call below.
	GLint uViewportPos = -1;
	GLint uYFlip = -1;
	GLint uTex0 = -1, uTex1 = -1;
	GLint uTexMat0 = -1, uTexMat1 = -1;
	GLint uTFactor = -1;
	GLint uAlphaRef = -1;
	GLint uFogColor = -1, uFogParams = -1;
	// GeneralsX @performance Android port 01/10/2026 Material and lights packed into two vec4 arrays
	// (see getProgram()), so a change uploads with one call each instead of three and eight. Lights
	// change on ~15% of model draws (each object gets its own nearest lights).
	GLint uMat = -1; // [0] diffuse, [1] ambient, [2] emissive
	GLint uLit = -1; // [0] global ambient, [1].x light count, then 5 per light (see kLitStride)
	// The world matrix this program last received: uniform values live in the program, so a draw
	// that repeats it (particles, a mesh's further passes) needs no upload.
	float lastWorld[16] = {};
	bool haveWorld = false;
	// key fields needed at bind time
	int stageTci[2] = {0, 0};
	bool stageXform[2] = {false, false};
	int stagesUsed = 0;
};

// Stage portion of the program key.
struct StageKey {
	// GeneralsX @bugfix Android port 09/05/2026 arg0 is D3D8's THIRD combiner
	// argument (D3DTSS_COLORARG0/D3DTSS_ALPHAARG0), used only by the
	// multiply-accumulate and lerp ops. It was missing entirely, so
	// D3DTOP_MULTIPLYADD fell through combinerOp()'s silent `default:` and
	// was emitted as a plain modulate -- see the D3DTOP_MULTIPLYADD case
	// there for the UI bug that caused.
	unsigned colorOp, colorArg0, colorArg1, colorArg2;
	unsigned alphaOp, alphaArg0, alphaArg1, alphaArg2;
	unsigned tci;
	unsigned texgen; // 0=vertex uv set, 1=camera-space position
	bool xform;
};

static void getStageKey(WebGLDevice *dev, int stage, StageKey *k);
static std::string combinerArg(unsigned arg, const char *texExpr, int *usesTex);
static std::string combinerOp(unsigned op, const std::string &a0, const std::string &a1,
                              const std::string &a2, const char *texAlphaExpr);

WebGLPipeline *WebGLPipeline::get()
{
	static WebGLPipeline *s_instance = nullptr;
	if (!s_instance) {
		s_instance = new WebGLPipeline();
		g_glTrace = getenv("D3D8GLES_TRACE") != nullptr;
	}
	return s_instance;
}

bool WebGLPipeline::initContext(int w, int h, SDL_Window *window)
{
	if (m_ctxReady) {
		resize(w, h);
		return true;
	}

	m_window = window;

	// GeneralsX @build Android port GLES experiment - native GLES3 context via
	// SDL3 instead of an Emscripten/WebGL2 canvas context. SDL3 wraps EGL on
	// Android (SDL_GL_CreateContext/MakeCurrent/SwapWindow), the same way the
	// Vulkan path already leans on SDL3's Vulkan WSI instead of raw
	// vkCreateSwapchainKHR. The SDL_GL_SetAttribute calls that pick the
	// GLES3/depth/stencil config live in SDL3Main.cpp, BEFORE
	// SDL_CreateWindow -- SDL only applies them to windows created after the
	// call, so they can't live here (this runs well after the window exists).
	m_glContext = SDL_GL_CreateContext(window);
	if (!m_glContext) {
		fprintf(stderr, "[d3d8gles] FATAL: SDL_GL_CreateContext failed: %s\n", SDL_GetError());
		return false;
	}
	if (!SDL_GL_MakeCurrent(window, (SDL_GLContext)m_glContext)) {
		fprintf(stderr, "[d3d8gles] FATAL: SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
		return false;
	}
	// GeneralsX @bugfix Android port 09/04/2026 Diagnostic: a real-device
	// GL_VIEWPORT dump at present() time already confirmed our own viewport
	// bookkeeping (w/h passed in here, from _PresentParameters) is applied
	// correctly to GL every frame, yet a persistent edge strip remains --
	// meaning the ACTUAL EGL window surface's real pixel size may not match
	// w/h at all. eglCreateWindowSurface() (called inside SDL_GL_CreateContext
	// above) sizes the surface from the ANativeWindow directly, independent
	// of any width/height we pass anywhere -- if the window hadn't fully
	// settled by the time THIS call ran (a separate race from the
	// resolution-detection debounce in SDL3Main.cpp, which only guards
	// xres/yres computed much earlier, before device/context creation), the
	// EGL surface could be locked in at a stale size for its whole lifetime.
	// Query the window's size again right here, at the exact moment the EGL
	// surface actually gets created, to see whether it agrees with w/h.
	{
		int freshW = 0, freshH = 0;
		SDL_GetWindowSizeInPixels(window, &freshW, &freshH);
		fprintf(stderr, "[d3d8gles-diag] initContext(): requested w=%d h=%d, "
			"SDL_GetWindowSizeInPixels() right after SDL_GL_CreateContext=%dx%d\n",
			w, h, freshW, freshH);
	}
#if defined(__ANDROID__)
	// GeneralsX @bugfix Android port 09/04/2026 EXPERIMENT (low confidence):
	// force the underlying ANativeWindow's pixel format to RGBX_8888
	// (opaque -- the 4th byte is padding, explicitly ignored by
	// SurfaceFlinger/the compositor) rather than whatever format the chosen
	// EGL config implied. This is the Android-native equivalent of Vulkan's
	// VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, which DXVK's swapchain likely
	// declares explicitly -- EGL has no equivalent explicit knob, so on
	// Android the compositor's opaque-vs-blended decision is otherwise
	// inferred from the EGL config's alpha bits, which is comparatively
	// fragile. Explicitly setting RGBX_8888 here removes that inference
	// entirely, regardless of what SDL_GL_ALPHA_SIZE was requested and
	// regardless of whether any of this engine's own rendering incidentally
	// writes a non-1.0 alpha into the framebuffer somewhere. width/height=0
	// means "keep the ANativeWindow's current size", only the format
	// changes. Real-device diagnostics already ruled out every
	// application-level cause (viewport application, EGL surface size at
	// creation time) for a persistent edge strip seen only on GLES/ANGLE,
	// never Vulkan -- worth testing whether this is a compositor-level
	// alpha-blending artifact instead.
	{
		void *nativeWindowPtr = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
			SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
		if (nativeWindowPtr) {
			int rc = ANativeWindow_setBuffersGeometry((ANativeWindow *)nativeWindowPtr, 0, 0, WINDOW_FORMAT_RGBX_8888);
			fprintf(stderr, "[d3d8gles-diag] ANativeWindow_setBuffersGeometry(RGBX_8888) -> %d\n", rc);
		} else {
			fprintf(stderr, "[d3d8gles-diag] ANativeWindow_setBuffersGeometry skipped: no native window pointer\n");
		}
	}
#endif
	// GeneralsX @bugfix Android port 09/04/2026 EXPERIMENT (low confidence):
	// this call's return value was never checked. Reconsidering the reported
	// edge artifact from scratch: it's variable in *which* edge (right, then
	// also bottom), only shows on scenes with real on-screen motion (menu
	// water/ships, gameplay units, video playback), sometimes coincides with
	// UI briefly not appearing at all, and is completely absent on Vulkan/
	// DXVK -- all consistent with plain screen TEARING (GL swap running
	// without real vsync) rather than a rendering-geometry bug, which every
	// other diagnostic this branch has already ruled out (GL_VIEWPORT and
	// the EGL surface's actual size both confirmed correct on a real
	// device). DXVK's own present path is Vulkan-native (vkQueuePresentKHR
	// with its own FIFO/vsync semantics) and was never touched by this
	// SDL_GL_SetSwapInterval() call at all, so a vsync failure specific to
	// the GLES/EGL path would explain the Vulkan/GLES split directly. Log
	// the actual result and the interval SDL reports back afterward, since
	// silent failure here would otherwise be invisible.
	{
		bool intervalOk = SDL_GL_SetSwapInterval(1);
		int actualInterval = 0;
		bool queryOk = SDL_GL_GetSwapInterval(&actualInterval);
		fprintf(stderr, "[d3d8gles-diag] SDL_GL_SetSwapInterval(1) -> %d, "
			"SDL_GL_GetSwapInterval() -> ok=%d interval=%d\n",
			(int)intervalOk, (int)queryOk, actualInterval);
	}

	// GeneralsX @build Android port ANGLE experiment - resolve every gl*
	// entry point this module calls via gles_dispatch.cpp instead of relying
	// on direct DT_NEEDED linking against system libGLESv3.so (see
	// CMakeLists.txt for why). Must run after MakeCurrent (a context needs to
	// be current for the chosen implementation to hand back valid function
	// pointers) and before the very first gl* call below. Mirrors the
	// SDL_HINT_EGL_LIBRARY choice made in SDL3Main.cpp before SDL_InitSubSystem
	// -- both must agree on ANGLE vs. system GLES, or the EGL context and the
	// GL entry points come from two different, incompatible implementations.
	{
		// GeneralsX @build Android port render-backend picker 07/09/2026 -
		// must resolve ANGLE-vs-system exactly the way SDL3Main.cpp's
		// UseANGLE() (and dx8wrapper.cpp's Vulkan check) do -- delegates to
		// the single shared implementation, d3d8gles_ShouldUseANGLE() just
		// above in this same translation unit (d3d8gles.cpp #includes this
		// file), instead of re-reading render_backend.cfg here too. See
		// d3d8gles.h's comment on why three separate copies of this check
		// caused a real bug.
		const bool useANGLE = d3d8gles_ShouldUseANGLE();
		const char *libName = useANGLE ? "libGLESv2_angle.so" : "libGLESv3.so";
		if (!d3d8gles_LoadGLESDispatch(libName)) {
			if (useANGLE) {
				fprintf(stderr, "[d3d8gles] ANGLE GLES dispatch unavailable, falling back to system libGLESv3.so\n");
				libName = "libGLESv3.so";
			}
			if (!d3d8gles_LoadGLESDispatch(libName)) {
				fprintf(stderr, "[d3d8gles] FATAL: could not resolve any GLES implementation (tried %s)\n", libName);
				return false;
			}
		}
		fprintf(stderr, "[d3d8gles] GLES backend: %s\n", libName);
	}

	const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
	m_hasS3TC = extensions != nullptr &&
		(strstr(extensions, "GL_EXT_texture_compression_s3tc") != nullptr ||
		 strstr(extensions, "GL_EXT_texture_compression_dxt1") != nullptr);

	m_fbWidth = w;
	m_fbHeight = h;
	m_curRTWidth = w;
	m_curRTHeight = h;

	glGenBuffers(1, &m_upVBO);
	glGenBuffers(1, &m_upIBO);

	// GeneralsX @build Android port GLES experiment 08/30/2026 See
	// kViewProjUBOBinding's comment: allocate the camera UBO once here and
	// bind it to its binding point for the whole session. std140 layout of
	// two consecutive mat4 members needs no padding (each is a 64-byte,
	// 16-byte-aligned block) -- 128 bytes total, matching ViewProjKey's
	// `float view[16], proj[16]` byte-for-byte, same raw column-major
	// layout already assumed everywhere else this codebase uploads a D3D
	// matrix via glUniformMatrix4fv(..., GL_FALSE, ...).
	glGenBuffers(1, &m_viewProjUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, m_viewProjUBO);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(float) * 32, nullptr, GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER, kViewProjUBOBinding, m_viewProjUBO);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glDisable(GL_DITHER);

	loadOptimizationSwitches();
	{
		// Optional entry points come from the same GLES library as everything else, or from
		// the matching EGL (SDL_HINT_EGL_LIBRARY selects ANGLE's EGL together with ANGLE's
		// GLES), and only when the version or extension string says they exist: an
		// eglGetProcAddress result alone is not proof of support.
		auto optionalProc = [](const char *name) -> void * {
			void *p = d3d8gles_GetOptionalGLProc(name);
			if (!p)
				p = reinterpret_cast<void *>(SDL_GL_GetProcAddress(name));
			return p;
		};
		const char *version = (const char *)glGetString(GL_VERSION);
		int major = 0, minor = 0;
		if (version)
			sscanf(version, "OpenGL ES %d.%d", &major, &minor);
		const bool es32 = major > 3 || (major == 3 && minor >= 2);
		const char *baseVertexSource = "none";
		// GeneralsX @performance Android port 01/10/2026 On by default on Mali. Without it every
		// draw from the engine's shared dynamic buffer at a new offset re-points the vertex
		// attributes: ~470 refreshes of 4-5 calls a frame in the menu battle, ~18% of the render
		// thread's commands (logs-39 -> logs-40: 8.9 -> 7.3 GL calls per draw, no artifacts).
		// It stays off elsewhere: on an Adreno 8xx it once broke shadow volumes and UI widgets,
		// back when dynamic indices were rewritten in place (since replaced by the index stream).
		{
			const char *renderer = (const char *)glGetString(GL_RENDERER);
			if (!m_opt.baseVertexOff && renderer && strstr(renderer, "Mali"))
				m_opt.baseVertex = true;
		}
		if (m_opt.baseVertex) {
			if (es32) {
				m_glDrawElementsBaseVertex = reinterpret_cast<PFN_DrawElementsBaseVertex>(optionalProc("glDrawElementsBaseVertex"));
				baseVertexSource = "ES 3.2";
			}
			if (!m_glDrawElementsBaseVertex && extensions && strstr(extensions, "GL_OES_draw_elements_base_vertex")) {
				m_glDrawElementsBaseVertex = reinterpret_cast<PFN_DrawElementsBaseVertex>(optionalProc("glDrawElementsBaseVertexOES"));
				baseVertexSource = "OES";
			}
			if (!m_glDrawElementsBaseVertex && extensions && strstr(extensions, "GL_EXT_draw_elements_base_vertex")) {
				m_glDrawElementsBaseVertex = reinterpret_cast<PFN_DrawElementsBaseVertex>(optionalProc("glDrawElementsBaseVertexEXT"));
				baseVertexSource = "EXT";
			}
			if (!m_glDrawElementsBaseVertex)
				baseVertexSource = "unavailable";
		}

		// Core since ES 2.0; resolved here because the dispatch table does not carry it.
		m_glStencilOpSeparate = reinterpret_cast<PFN_StencilOpSeparate>(optionalProc("glStencilOpSeparate"));

		// GeneralsX @performance Android port 29/09/2026 Persistent mapping for dynamic VB/IB.
		const char *persistentState = "off";
		if (m_opt.persistent) {
			if (extensions && strstr(extensions, "GL_EXT_buffer_storage")) {
				m_glBufferStorage = reinterpret_cast<PFN_BufferStorage>(optionalProc("glBufferStorageEXT"));
				m_glFenceSync = reinterpret_cast<PFN_FenceSync>(optionalProc("glFenceSync"));
				m_glClientWaitSync = reinterpret_cast<PFN_ClientWaitSync>(optionalProc("glClientWaitSync"));
				m_glDeleteSync = reinterpret_cast<PFN_DeleteSync>(optionalProc("glDeleteSync"));
			}
			m_persistentOK = m_glBufferStorage && m_glFenceSync && m_glClientWaitSync && m_glDeleteSync;
			persistentState = m_persistentOK ? "on" : "unavailable";
		}

		if (m_opt.upRing) {
			glGenBuffers(1, &m_upRingVB);
			glBindBuffer(GL_COPY_WRITE_BUFFER, m_upRingVB);
			glBufferData(GL_COPY_WRITE_BUFFER, kUpRingVBBytes, nullptr, GL_STREAM_DRAW);
			glGenBuffers(1, &m_upRingIB);
			glBindBuffer(GL_COPY_WRITE_BUFFER, m_upRingIB);
			glBufferData(GL_COPY_WRITE_BUFFER, kUpRingIBBytes, nullptr, GL_STREAM_DRAW);
		}

		const char *programCacheState = "off";
		if (m_opt.programCache) {
			GLint binaryFormats = 0;
			glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &binaryFormats);
			m_glGetProgramBinary = reinterpret_cast<PFN_GetProgramBinary>(optionalProc("glGetProgramBinary"));
			m_glProgramBinary = reinterpret_cast<PFN_ProgramBinary>(optionalProc("glProgramBinary"));
			m_glProgramParameteri = reinterpret_cast<PFN_ProgramParameteri>(optionalProc("glProgramParameteri"));
			const char *home = getenv("HOME");
			if (binaryFormats > 0 && m_glGetProgramBinary && m_glProgramBinary && m_glProgramParameteri && home && home[0]) {
				std::string dir = std::string(home) + "/.cache";
				mkdir(dir.c_str(), 0755);
				dir += "/gx_gles_programs";
				mkdir(dir.c_str(), 0755);
				m_programCacheDir = dir;
				// A driver update or a switch between the system driver and ANGLE produces
				// binaries the other cannot load, so their identity is part of every key.
				std::string id = "gxpb1|";
				const char *vendor = (const char *)glGetString(GL_VENDOR);
				const char *renderer = (const char *)glGetString(GL_RENDERER);
				id += vendor ? vendor : "";
				id += "|";
				id += renderer ? renderer : "";
				id += "|";
				id += version ? version : "";
				m_driverHash = fnv1a64(id.data(), id.size(), 1469598103934665603ULL);
				programCacheState = "on";
			} else {
				m_glGetProgramBinary = nullptr;
				m_glProgramBinary = nullptr;
				m_glProgramParameteri = nullptr;
				programCacheState = binaryFormats > 0 ? "unavailable" : "no binary formats";
			}
		}
		fprintf(stderr, "[d3d8gles] optimizations: basevertex=%s upring=%d progcache=%s dxt16=%d persistent=%s "
			"(GL_VERSION=%s; gx_gles_noopt.txt turns them off)\n",
			m_opt.baseVertex ? baseVertexSource : "off", (int)m_opt.upRing, programCacheState,
			(int)(m_opt.dxt565 && !m_hasS3TC), persistentState, version ? version : "?");
	}

	m_ctxReady = true;
	s_gxGlesReady = true;
	// GeneralsX @build Android port 09/05/2026 Report the DEFAULT framebuffer's
	// actual bit depths, not the ones we asked SDL for. Stencil is the one that
	// matters: volumetric (stencil) shadows are enabled only at High detail and
	// above, which is exactly when the reported helicopter artifact appears, and
	// a config without stencil bits makes every stencil test pass silently --
	// which would draw W3DVolumetricShadowManager::renderStencilShadows()'s
	// darkening quad unmasked instead of only inside the shadow. SDL is free to
	// hand back a config that does not satisfy every requested attribute.
	{
		GLint rb = 0, gb = 0, bb = 0, ab = 0, db = 0, sb = 0;
		glGetIntegerv(GL_RED_BITS, &rb);
		glGetIntegerv(GL_GREEN_BITS, &gb);
		glGetIntegerv(GL_BLUE_BITS, &bb);
		glGetIntegerv(GL_ALPHA_BITS, &ab);
		glGetIntegerv(GL_DEPTH_BITS, &db);
		glGetIntegerv(GL_STENCIL_BITS, &sb);
		fprintf(stderr, "[d3d8gles] default framebuffer bits: r=%d g=%d b=%d a=%d depth=%d stencil=%d\n",
			(int)rb, (int)gb, (int)bb, (int)ab, (int)db, (int)sb);
	}
	fprintf(stderr, "[d3d8gles] GLES3 context ready %dx%d (s3tc=%d)\n", w, h, (int)m_hasS3TC);

	// GeneralsX @performance Android port 30/09/2026 From here on GL calls run on the render thread
	// (gles_thread.h). Everything above ran on this thread with the context current here.
	if (m_glClientWaitSync && m_glFenceSync && m_glDeleteSync)
		gxrt::setFenceProcs(m_glFenceSync, m_glClientWaitSync, m_glDeleteSync);
	{
		const char *ext = (const char *)glGetString(GL_EXTENSIONS);
		if (ext && strstr(ext, "GL_EXT_disjoint_timer_query")) {
			auto proc = [](const char *name) -> void * {
				void *p = d3d8gles_GetOptionalGLProc(name);
				return p ? p : reinterpret_cast<void *>(SDL_GL_GetProcAddress(name));
			};
			auto gen = reinterpret_cast<GpuFrameTimer::PFN_GenQueries>(proc("glGenQueriesEXT"));
			s_gpuTimer.begin = reinterpret_cast<GpuFrameTimer::PFN_BeginQuery>(proc("glBeginQueryEXT"));
			s_gpuTimer.end = reinterpret_cast<GpuFrameTimer::PFN_EndQuery>(proc("glEndQueryEXT"));
			s_gpuTimer.get = reinterpret_cast<GpuFrameTimer::PFN_GetQueryObjectuiv>(proc("glGetQueryObjectuivEXT"));
			if (gen && s_gpuTimer.begin && s_gpuTimer.end && s_gpuTimer.get) {
				gen(GpuFrameTimer::kQueries, s_gpuTimer.queries);
				s_gpuTimer.ok = true;
			}
		}
		fprintf(stderr, "[d3d8gles] GPU frame timer: %s\n", s_gpuTimer.ok ? "on" : "unavailable (no GL_EXT_disjoint_timer_query)");
	}
	if (m_opt.thread && window) {
		const bool threaded = gxrt::start(window);
		fprintf(stderr, "[d3d8gles] render thread: %s\n", threaded ? "on" : "unavailable, rendering on the main thread");
	} else {
		fprintf(stderr, "[d3d8gles] render thread: off (gx_gles_noopt.txt)\n");
	}
	return true;
}

// GeneralsX @performance Android port 27/09/2026 gx_gles_noopt.txt in the game folder turns the
// translator optimizations off: all of them when it is empty, or only those it names. Same
// convention as the other gx_*.txt switches -- presence (and content) is the switch. The one
// that is off by default (base-vertex draws, see OptimizationSwitches) is turned on by
// gx_gles_basevertex.txt instead.
void WebGLPipeline::loadOptimizationSwitches()
{
	if (FILE *optIn = fopen("gx_gles_persistentib.txt", "r")) {
		fclose(optIn);
		m_opt.persistentIB = true;
		fprintf(stderr, "[d3d8gles] gx_gles_persistentib.txt: persistent index buffers enabled (experimental)\n");
	}
	if (FILE *optIn = fopen("gx_gles_basevertex.txt", "r")) {
		fclose(optIn);
		m_opt.baseVertex = true;
		fprintf(stderr, "[d3d8gles] gx_gles_basevertex.txt: base-vertex draws enabled (experimental)\n");
	}
	FILE *f = fopen("gx_gles_noopt.txt", "r");
	if (!f)
		return;
	char buf[512] = { 0 };
	const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	bool named = false;
	for (const char *p = buf; *p; p++) {
		if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
			named = true;
			break;
		}
	}
	if (!named) {
		m_opt.baseVertex = m_opt.upRing = m_opt.programCache = m_opt.dxt565 = m_opt.persistent = m_opt.persistentIB = false;
		m_opt.thread = false;
		m_opt.baseVertexOff = true;
	} else {
		if (strstr(buf, "basevertex")) { m_opt.baseVertex = false; m_opt.baseVertexOff = true; }
		if (strstr(buf, "upring")) m_opt.upRing = false;
		if (strstr(buf, "progcache")) m_opt.programCache = false;
		if (strstr(buf, "dxt565")) m_opt.dxt565 = false;
		if (strstr(buf, "persistent")) m_opt.persistent = m_opt.persistentIB = false;
		if (strstr(buf, "thread")) m_opt.thread = false;
	}
	fprintf(stderr, "[d3d8gles] gx_gles_noopt.txt: basevertex=%d upring=%d progcache=%d dxt565=%d persistent=%d thread=%d\n",
		(int)m_opt.baseVertex, (int)m_opt.upRing, (int)m_opt.programCache, (int)m_opt.dxt565, (int)m_opt.persistent,
		(int)m_opt.thread);
}

void WebGLPipeline::resize(int w, int h)
{
	if (m_vbActive) {
		// The virtual backbuffer keeps the game's size; only the stretch's target changes.
		if (w != m_winW || h != m_winH)
			fprintf(stderr, "[d3d8gles] window resized to %dx%d (virtual backbuffer stays %dx%d)\n", w, h, m_vbW, m_vbH);
		m_winW = w;
		m_winH = h;
		return;
	}
	if (w == m_fbWidth && h == m_fbHeight) return;
	// The native window surface already tracks the real size on its own
	// (unlike a browser canvas, which needed an explicit element-size call);
	// nothing to resize here beyond our own bookkeeping.
	m_fbWidth = w;
	m_fbHeight = h;
	if (m_curFBO == 0) {
		m_curRTWidth = w;
		m_curRTHeight = h;
	}
	fprintf(stderr, "[d3d8gles] window resized to %dx%d\n", w, h);
}

// ---------------------------------------------------------------------------
// Shader generation
// ---------------------------------------------------------------------------

static void getStageKey(WebGLDevice *dev, int stage, StageKey *k)
{
	// Keep the FULL argument values: the low nibble selects the source and
	// bits 0x10/0x20 are the COMPLEMENT/ALPHAREPLICATE modifiers (the road
	// noise pass uses DIFFUSE|ALPHAREPLICATE to synthesize white).
	k->colorOp = dev->getStageState(stage, D3DTSS_COLOROP);
	k->colorArg1 = dev->getStageState(stage, D3DTSS_COLORARG1) & 0x3F;
	k->colorArg2 = dev->getStageState(stage, D3DTSS_COLORARG2) & 0x3F;
	k->alphaOp = dev->getStageState(stage, D3DTSS_ALPHAOP);
	k->alphaArg1 = dev->getStageState(stage, D3DTSS_ALPHAARG1) & 0x3F;
	k->alphaArg2 = dev->getStageState(stage, D3DTSS_ALPHAARG2) & 0x3F;
	// D3D8's documented default for ARG0 is D3DTA_CURRENT, not D3DTA_DIFFUSE
	// (which is what a never-set 0 would otherwise decode to here).
	const DWORD rawColorArg0 = dev->getStageState(stage, D3DTSS_COLORARG0);
	const DWORD rawAlphaArg0 = dev->getStageState(stage, D3DTSS_ALPHAARG0);
	k->colorArg0 = rawColorArg0 ? (unsigned)(rawColorArg0 & 0x3F) : (unsigned)D3DTA_CURRENT;
	k->alphaArg0 = rawAlphaArg0 ? (unsigned)(rawAlphaArg0 & 0x3F) : (unsigned)D3DTA_CURRENT;
	const DWORD tciRaw = dev->getStageState(stage, D3DTSS_TEXCOORDINDEX);
	k->texgen = 0;
	if (tciRaw & 0xFFFF0000u) {
		switch (tciRaw & 0xFFFF0000u) {
		case D3DTSS_TCI_CAMERASPACEPOSITION:
			// Terrain macro/cloud layers: uv = texture matrix * view-space pos.
			k->texgen = 1;
			break;
		case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR:
			// Environment maps: uv = texture matrix * reflect(eye, view normal).
			k->texgen = 2;
			break;
		case D3DTSS_TCI_CAMERASPACENORMAL:
			k->texgen = 3;
			break;
		default:
			WARN_ONCE(s_texgen, "texgen TEXCOORDINDEX flags 0x%x not implemented (stage %d)", (unsigned)tciRaw, stage);
			break;
		}
	}
	k->tci = tciRaw & 0x1;
	const DWORD ttf = dev->getStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS);
	k->xform = (ttf & 0xFF) != 0; // COUNT1..4 -> apply the stage matrix
	// Defaults per D3D8 when never set: stage0 MODULATE tex*diffuse, stage1 DISABLE.
	if (k->colorOp == 0) k->colorOp = (stage == 0) ? D3DTOP_MODULATE : D3DTOP_DISABLE;
	if (k->alphaOp == 0) k->alphaOp = (stage == 0) ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
	if (dev->getStageState(stage, D3DTSS_COLORARG1) == 0) k->colorArg1 = 2; // TEXTURE
	if (dev->getStageState(stage, D3DTSS_COLORARG2) == 0) k->colorArg2 = 0; // (CURRENT->DIFFUSE for st0)
	if (dev->getStageState(stage, D3DTSS_ALPHAARG1) == 0) k->alphaArg1 = 2;
}

// GeneralsX @bugfix Android port 09/05/2026 D3D8 still RUNS a texture stage
// whose op is not D3DTOP_DISABLE when no texture is bound at that stage, as
// long as the op's arguments do not source D3DTA_TEXTURE. This backend used to
// disable the entire stage on "no texture", which silently deleted a real pass.
//
// The case that exposed it: Render2DClass::Render()'s greyscale path (disabled
// command-bar buttons) puts D3DTOP_DOTPRODUCT3 on stage 1 with COLORARG1 =
// D3DTA_CURRENT and COLORARG2 = D3DTA_TFACTOR -- neither touches a texture, and
// the 2D path only ever binds stage 0. So the desaturation never ran, and the
// button came out as the bare stage-0 result (a MULTIPLYADD that brightens the
// icon by +0.25 -- reported as "the building icons became too light and I can
// no longer tell what is available"). Worse, it was intermittent: whenever a
// previous 3D pass happened to leave a texture bound on stage 1, the stage did
// run and every icon went greyscale at once -- which is the all-colour /
// all-grey flicker seen between otherwise identical frames.
//
// Collapse per channel, and only for the channel that actually reads the
// missing texture.
static bool stageChannelReadsTexture(unsigned op, unsigned arg0, unsigned arg1, unsigned arg2)
{
	if (op == D3DTOP_DISABLE) return false;
	// BLENDTEXTUREALPHA takes the texture's alpha implicitly, not through an
	// argument, so it reads the texture no matter what its args say.
	if (op == D3DTOP_BLENDTEXTUREALPHA) return true;
	auto isTex = [](unsigned a) { return (a & 0xF) == (unsigned)D3DTA_TEXTURE; };
	if (isTex(arg1) || isTex(arg2)) return true;
	// arg0 is only consulted by the two ops that take a third argument.
	if ((op == D3DTOP_MULTIPLYADD || op == D3DTOP_LERP) && isTex(arg0)) return true;
	return false;
}

static void collapseStageWithoutTexture(StageKey *k, int stage)
{
	if (stageChannelReadsTexture(k->colorOp, k->colorArg0, k->colorArg1, k->colorArg2)) {

		if (stage == 0) {
			k->colorOp = D3DTOP_SELECTARG2;
			k->colorArg2 = 0; // DIFFUSE
		} else {
			k->colorOp = D3DTOP_DISABLE;
		}
	}
	if (stageChannelReadsTexture(k->alphaOp, k->alphaArg0, k->alphaArg1, k->alphaArg2)) {
		if (stage == 0) {
			k->alphaOp = D3DTOP_SELECTARG2;
			k->alphaArg2 = 0;
		} else {
			k->alphaOp = D3DTOP_DISABLE;
		}
	}
}

uint64_t WebGLPipeline::computeProgramKey(WebGLDevice *dev, unsigned fvf) const
{
	FVFLayout l;
	parseFVF(fvf, &l);

	// FNV-1a over the full state values: argument MODIFIER bits (COMPLEMENT/
	// ALPHAREPLICATE) must differentiate programs and no longer fit a packed
	// 64-bit layout.
	uint64_t key = 0xcbf29ce484222325ull;
	auto put = [&](uint64_t v, int /*bits*/) {
		key ^= v + 0x9E37;
		key *= 0x100000001b3ull;
	};

	put(l.xyzrhw ? 1 : 0, 1);
	put(l.hasNormal ? 1 : 0, 1);
	put(l.hasDiffuse ? 1 : 0, 1);
	put(l.texCount > 2 ? 2 : l.texCount, 2);

	const bool lighting = dev->getRenderState(D3DRS_LIGHTING) != 0 && l.hasNormal && !l.xyzrhw;
	put(lighting ? 1 : 0, 1);

	// Material color sources (VertexMaterialClass::Apply drives these; the
	// W3D default is MATERIAL - skinned meshes carry diffuse=0 in the VB).
	if (lighting) {
		const bool cv = dev->getRenderState(D3DRS_COLORVERTEX) != 0 && l.hasDiffuse;
		put((cv && dev->getRenderState(D3DRS_DIFFUSEMATERIALSOURCE) == 1 /*COLOR1*/) ? 1 : 0, 1);
		put((cv && dev->getRenderState(D3DRS_AMBIENTMATERIALSOURCE) == 1) ? 1 : 0, 1);
		put((cv && dev->getRenderState(D3DRS_EMISSIVEMATERIALSOURCE) == 1) ? 1 : 0, 1);
	} else {
		put(0, 3);
	}

	const bool fog = dev->getRenderState(D3DRS_FOGENABLE) != 0 && !l.xyzrhw;
	put(fog ? 1 : 0, 1);

	const bool alphaTest = dev->getRenderState(D3DRS_ALPHATESTENABLE) != 0;
	put(alphaTest ? 1 : 0, 1);
	put(alphaTest ? (dev->getRenderState(D3DRS_ALPHAFUNC) & 0x7) : 0, 3);

	for (int s = 0; s < 2; s++) {
		StageKey sk;
		getStageKey(dev, s, &sk);
		// No texture bound: only the channels that source TEXTURE collapse.
		if (!dev->getTexture2D(s)) collapseStageWithoutTexture(&sk, s);
		put(sk.colorOp, 5);
		put(sk.colorArg0, 6);
		put(sk.colorArg1, 6);
		put(sk.colorArg2, 6);
		put(sk.alphaOp, 5);
		put(sk.alphaArg0, 6);
		put(sk.alphaArg1, 6);
		put(sk.alphaArg2, 6);
		put(sk.tci, 1);
		put(sk.texgen, 1);
		put(sk.xform ? 1 : 0, 1);
	}
	return key;
}

// Selector (arg & 0xF): 0=DIFFUSE 1=CURRENT 2=TEXTURE 3=TFACTOR 4=SPECULAR.
// Modifiers: D3DTA_COMPLEMENT (0x10) = 1-x, D3DTA_ALPHAREPLICATE (0x20) = x.aaaa
// (the road noise pass relies on DIFFUSE|ALPHAREPLICATE to build white).
static std::string combinerArg(unsigned arg, const char *texExpr, int *usesTex)
{
	const char *base;
	switch (arg & 0xF) {
	case 0: base = "vCol"; break;
	case 1: base = "cur"; break;
	case 2: *usesTex = 1; base = texExpr; break;
	case 3: base = "uTFactor"; break;
	case 4: base = "vSpec"; break;
	default: base = "vCol"; break;
	}
	std::string e = base;
	if (arg & 0x20) e = "vec4(" + e + ".a)";          // ALPHAREPLICATE first
	if (arg & 0x10) e = "(vec4(1.0) - " + e + ")";    // then COMPLEMENT
	return e;
}

static std::string combinerOp(unsigned op, const std::string &a0, const std::string &a1,
                              const std::string &a2, const char *texAlphaExpr)
{
	char buf[768];
	switch (op) {
	case D3DTOP_SELECTARG1: return a1;
	case D3DTOP_SELECTARG2: return a2;
	case D3DTOP_MODULATE:
		snprintf(buf, sizeof(buf), "(%s * %s)", a1.c_str(), a2.c_str());
		break;
	case D3DTOP_MODULATE2X:
		snprintf(buf, sizeof(buf), "min((%s * %s) * 2.0, vec4(1.0))", a1.c_str(), a2.c_str());
		break;
	case D3DTOP_MODULATE4X:
		snprintf(buf, sizeof(buf), "min((%s * %s) * 4.0, vec4(1.0))", a1.c_str(), a2.c_str());
		break;
	case D3DTOP_ADD:
		snprintf(buf, sizeof(buf), "min(%s + %s, vec4(1.0))", a1.c_str(), a2.c_str());
		break;
	case D3DTOP_ADDSIGNED:
		snprintf(buf, sizeof(buf), "clamp(%s + %s - 0.5, 0.0, 1.0)", a1.c_str(), a2.c_str());
		break;
	case D3DTOP_ADDSIGNED2X:
		snprintf(buf, sizeof(buf), "clamp((%s + %s - 0.5) * 2.0, 0.0, 1.0)", a1.c_str(), a2.c_str());
		break;
	case D3DTOP_SUBTRACT:
		snprintf(buf, sizeof(buf), "max(%s - %s, vec4(0.0))", a1.c_str(), a2.c_str());
		break;
	case D3DTOP_ADDSMOOTH:
		snprintf(buf, sizeof(buf), "(%s + %s - %s * %s)", a1.c_str(), a2.c_str(), a1.c_str(), a2.c_str());
		break;
	case D3DTOP_BLENDTEXTUREALPHA:
		snprintf(buf, sizeof(buf), "mix(%s, %s, %s)", a2.c_str(), a1.c_str(), texAlphaExpr);
		break;
	case D3DTOP_BLENDDIFFUSEALPHA:
		snprintf(buf, sizeof(buf), "mix(%s, %s, vCol.a)", a2.c_str(), a1.c_str());
		break;
	case D3DTOP_BLENDCURRENTALPHA:
		snprintf(buf, sizeof(buf), "mix(%s, %s, cur.a)", a2.c_str(), a1.c_str());
		break;
	case D3DTOP_BLENDFACTORALPHA:
		snprintf(buf, sizeof(buf), "mix(%s, %s, uTFactor.a)", a2.c_str(), a1.c_str());
		break;
	case D3DTOP_DOTPRODUCT3:
		snprintf(buf, sizeof(buf),
			"vec4(vec3(clamp(dot(%s.rgb - 0.5, %s.rgb - 0.5) * 4.0, 0.0, 1.0)), 1.0)",
			a1.c_str(), a2.c_str());
		break;
	// GeneralsX @bugfix Android port 09/09/2026 D3D8: SRGBA = Arg0 + Arg1 * Arg2,
	// i.e. ARG0 is the ADDEND and ARG1/ARG2 are the two multiplicands.
	//
	// This was implemented on 09/05/2026 as "Arg1 + Arg2 * Arg0" -- the operands
	// the wrong way round. It looked right on the one case it was written for
	// (Render2DClass::Render()'s greyscale command-bar buttons, which are bright)
	// and it is catastrophically wrong on anything dark. Both call sites in the
	// engine use the same pair of stages:
	//
	//     stage 0: MULTIPLYADD  ARG0 = ARG2 = TFACTOR|ALPHAREPLICATE, ARG1 = TEXTURE
	//     stage 1: DOTPRODUCT3  ARG1 = CURRENT, ARG2 = TFACTOR
	//     D3DRS_TEXTUREFACTOR = 0x80A5CA8E
	//
	// The constant is the proof of which order is real. TFACTOR.a = 0x80 = 0.502,
	// so the correct stage 0 is 0.502 + 0.502*tex, which lands in [0.5, 1.0]; the
	// DOT3 then subtracts 0.5 and scales by 4, giving 4*0.502*(tf.rgb - 0.5) . tex.
	// With tf.rgb = (0xA5, 0xCA, 0x8E)/255 those weights come out as
	// (0.295, 0.586, 0.114) -- Rec.601 luminance to within the rounding of a byte,
	// on all three channels. Westwood picked the constant by inverting exactly this
	// formula; no other operand order reproduces it.
	//
	// The old order gave tex + 0.502*0.502 = tex + 0.25, so the DOT3 computed
	// luminance(tex) - 0.49 instead: every texel darker than mid-grey clamped to
	// zero. Bright button icons survived (too dark, but visible), which is why it
	// passed as fixed; a whole night scene did not, and the scripted black-and-white
	// cinematic (ScreenBWFilterDOT3, W3DShaderManager.cpp) rendered SOLID BLACK.
	//
	// Cross-checked against DXVK's fixed-function translation, which emits
	// fma(arg[1], arg[2], arg[0]) with arg[0] = D3DTSS_COLORARG0 (d3d9_fixed_function
	// .cpp, D3DTOP_MULTIPLYADD), and saturates the result.
	case D3DTOP_MULTIPLYADD:
		snprintf(buf, sizeof(buf), "clamp(%s + %s * %s, vec4(0.0), vec4(1.0))",
			a0.c_str(), a1.c_str(), a2.c_str());
		break;
	// D3D8: SRGBA = Arg1 * Arg0 + Arg2 * (1 - Arg0), i.e. mix() with Arg0 as
	// the interpolant. Added alongside MULTIPLYADD because it is the other op
	// that reads ARG0 -- without it, ARG0 would still be write-only state.
	case D3DTOP_LERP:
		snprintf(buf, sizeof(buf), "mix(%s, %s, %s)",
			a2.c_str(), a1.c_str(), a0.c_str());
		break;
	default:
		// Silence here is how MULTIPLYADD survived: an unimplemented op used
		// to render as a modulate that looks plausible in most content. Say so
		// once per op instead, so the next missing one shows up in the device
		// log rather than as a mystery visual bug.
		// D3DTOP_DISABLE can reach here for a disabled stage 0 that sits below
		// an enabled stage 1; that is a known, harmless shape, not a gap.
		if (op != D3DTOP_DISABLE) {
			WARN_ONCE_ARG(op, "texture stage COLOROP/ALPHAOP %u not implemented, "
				"approximating as MODULATE", op);
		}
		snprintf(buf, sizeof(buf), "(%s * %s)", a1.c_str(), a2.c_str());
		break;
	}
	return buf;
}

static GLuint compileShader(GLenum type, const std::string &src)
{
	GLuint sh = glCreateShader(type);
	const char *cs = src.c_str();
	glShaderSource(sh, 1, &cs, nullptr);
	glCompileShader(sh);
	GLint ok = 0;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
		fprintf(stderr, "[d3d8gles] shader compile FAILED:\n%s\n--- source ---\n%s\n", log, cs);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

// GeneralsX @performance Android port 27/09/2026 Linked program binaries on disk, one file per
// generated program: <HOME>/.cache/gx_gles_programs/<hash>.bin = "GXPB", format, length, data.
// A binary the driver refuses (it may reject one it produced itself after an update the
// hash did not catch) is deleted and the program is compiled from source as before.
GLuint WebGLPipeline::loadCachedProgram(uint64_t sourceHash)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%016llx.bin", m_programCacheDir.c_str(), (unsigned long long)sourceHash);
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	uint32_t header[3] = { 0, 0, 0 };
	std::vector<uint8_t> data;
	bool readOk = fread(header, sizeof(header), 1, f) == 1 && header[0] == 0x42505847u /* "GXPB" */
		&& header[2] > 0 && header[2] < (64u << 20);
	if (readOk) {
		data.resize(header[2]);
		readOk = fread(data.data(), 1, data.size(), f) == data.size();
	}
	fclose(f);
	if (!readOk) {
		remove(path);
		return 0;
	}
	GLuint p = glCreateProgram();
	{
		PFN_ProgramBinary programBinary = m_glProgramBinary;
		gxrt::sync([&] { programBinary(p, (GLenum)header[1], data.data(), (GLsizei)data.size()); });
	}
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		glDeleteProgram(p);
		remove(path);
		return 0;
	}
	m_perfProgramCacheLoads++;
	return p;
}

void WebGLPipeline::saveCachedProgram(uint64_t sourceHash, GLuint program)
{
	GLint length = 0;
	glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
	if (length <= 0)
		return;
	std::vector<uint8_t> data((size_t)length);
	GLsizei written = 0;
	GLenum format = 0;
	{
		PFN_GetProgramBinary getProgramBinary = m_glGetProgramBinary;
		gxrt::sync([&] { getProgramBinary(program, length, &written, &format, data.data()); });
	}
	if (written <= 0)
		return;
	char path[1024], temp[1040];
	snprintf(path, sizeof(path), "%s/%016llx.bin", m_programCacheDir.c_str(), (unsigned long long)sourceHash);
	snprintf(temp, sizeof(temp), "%s.tmp", path);
	FILE *f = fopen(temp, "wb");
	if (!f)
		return;
	const uint32_t header[3] = { 0x42505847u, (uint32_t)format, (uint32_t)written };
	const bool ok = fwrite(header, sizeof(header), 1, f) == 1
		&& fwrite(data.data(), 1, (size_t)written, f) == (size_t)written;
	fclose(f);
	// Written to a temporary name first, so a crash mid-write never leaves a truncated
	// binary under the real name.
	if (ok && rename(temp, path) == 0)
		m_perfProgramCacheSaves++;
	else
		remove(temp);
}

WebGLPipeline::ProgramInfo *WebGLPipeline::getProgram(WebGLDevice *dev, unsigned fvf)
{
	const uint64_t key = computeProgramKey(dev, fvf);
	for (int i = 0; i < m_programCount; i++) {
		if (m_programs[i].key == key) return m_programs[i].prog;
	}

	FVFLayout l;
	parseFVF(fvf, &l);
	const bool lighting = dev->getRenderState(D3DRS_LIGHTING) != 0 && l.hasNormal && !l.xyzrhw;
	// D3DMCS_COLOR1 == 1; VertexMaterialClass::Apply drives these states and
	// the W3D default is MATERIAL (skinned meshes carry diffuse=0 in the VB).
	const bool cvOn = dev->getRenderState(D3DRS_COLORVERTEX) != 0 && l.hasDiffuse;
	const bool diffFromVertex = lighting && cvOn && dev->getRenderState(D3DRS_DIFFUSEMATERIALSOURCE) == 1;
	const bool ambFromVertex = lighting && cvOn && dev->getRenderState(D3DRS_AMBIENTMATERIALSOURCE) == 1;
	const bool emisFromVertex = lighting && cvOn && dev->getRenderState(D3DRS_EMISSIVEMATERIALSOURCE) == 1;
	const bool fog = dev->getRenderState(D3DRS_FOGENABLE) != 0 && !l.xyzrhw;
	const bool alphaTest = dev->getRenderState(D3DRS_ALPHATESTENABLE) != 0;
	const unsigned alphaFunc = dev->getRenderState(D3DRS_ALPHAFUNC) ? dev->getRenderState(D3DRS_ALPHAFUNC) : D3DCMP_ALWAYS;

	StageKey st[2];
	int stagesUsed = 0;
	for (int s = 0; s < 2; s++) {
		getStageKey(dev, s, &st[s]);
		// Must stay identical to computeProgramKey's handling above, or the
		// cache key and the generated shader describe different pipelines.
		if (!dev->getTexture2D(s)) collapseStageWithoutTexture(&st[s], s);
		if (st[s].colorOp != D3DTOP_DISABLE) stagesUsed = s + 1;
	}

	// ---------------- vertex shader ----------------
	std::string vs;
	vs += "#version 300 es\nprecision highp float;\n";
	vs += l.xyzrhw ? "layout(location=0) in vec4 aPos;\n" : "layout(location=0) in vec3 aPos;\n";
	if (l.hasNormal) vs += "layout(location=1) in vec3 aNormal;\n";
	if (l.hasDiffuse) vs += "layout(location=2) in vec4 aColor0;\n";
	if (l.hasSpecular) vs += "layout(location=3) in vec4 aColor1;\n";
	const int texIn = l.texCount > 2 ? 2 : l.texCount;
	for (int i = 0; i < texIn; i++) {
		char b[64];
		snprintf(b, sizeof(b), "layout(location=%d) in vec4 aUV%d;\n", 4 + i, i);
		vs += b;
	}
	vs += "uniform mat4 uWorld;\n";
	// GeneralsX @build Android port GLES experiment 08/30/2026 view/proj as a
	// uniform block (see kViewProjUBOBinding's comment) instead of two plain
	// uniforms -- block member names (uView/uProj) are referenced exactly
	// the same as before in the shader body below, no other change needed
	// there.
	vs += "uniform ViewProjBlock { mat4 uView; mat4 uProj; };\n";
	vs += "uniform vec4 uViewportPos;\n"; // x, y, w, h
	vs += "uniform float uYFlip;\n"; // +1 backbuffer, -1 render-to-texture
	vs += "uniform mat4 uTexMat0, uTexMat1;\n";
	vs += "out vec4 vCol;\nout vec4 vSpec;\nout vec2 vUV0;\nout vec2 vUV1;\nout float vFogDepth;\n";
	if (lighting) {
		// uMat: diffuse, ambient, emissive. uLit: [0] global ambient, [1].x light count, then per
		// light i at 2 + 5i: (direction, type 1=point), (position, -), diffuse, ambient,
		// (range, a0, a1, a2). See ProgramInfo::uMat/uLit.
		vs += "uniform vec4 uMat[3];\n";
		vs += "uniform vec4 uLit[22];\n";
	}
	vs += "void main() {\n";
	if (l.xyzrhw) {
		vs += "  vec4 vpos = vec4(0.0);\n";
		vs += "  float nx = ((aPos.x - uViewportPos.x - 0.5) / uViewportPos.z) * 2.0 - 1.0;\n";
		vs += "  float ny = 1.0 - ((aPos.y - uViewportPos.y - 0.5) / uViewportPos.w) * 2.0;\n";
		vs += "  gl_Position = vec4(nx, ny * uYFlip, aPos.z * 2.0 - 1.0, 1.0);\n";
		vs += "  vFogDepth = 0.0;\n";
	} else {
		vs += "  vec4 wpos = uWorld * vec4(aPos, 1.0);\n";
			vs += "  vec4 vpos = uView * wpos;\n";
			vs += "  vec4 cpos = uProj * vpos;\n";
			// GeneralsX @build Android port GLES experiment - this used to be
			// "-cpos.y * uYFlip" (an unconditional negation). D3D's clip.y=+1
			// already means "top of screen" by construction of D3D's own
			// viewport transform (verified with concrete NDC/window-coordinate
			// arithmetic), and GL's NDC.y=+1 independently also means "top of
			// screen" once GL's own viewport transform + display scanout are
			// accounted for -- so feeding D3D's clip.y into GL as-is needs NO
			// extra negation. The old blanket "-cpos.y" silently flipped every
			// draw through this path, which includes both real 3D camera
			// content AND Render2DClass's 2D UI trick (identity world/view/proj
			// matrices, with Y already pre-flipped screen-to-NDC on the CPU
			// side in Render2DClass::Convert_Vert, GeneralsMD/Code/.../
			// render2d.cpp) -- confirmed via a real device screenshot showing
			// the ENTIRE frame (menu buttons, logos, and the 3D background
			// scene) upside down in reversed top-to-bottom order versus a
			// known-correct reference.
			vs += "  gl_Position = vec4(cpos.x, cpos.y * uYFlip, cpos.z * 2.0 - cpos.w, cpos.w);\n";
			vs += "  vFogDepth = -vpos.z;\n";
	}
	// Diffuse color: vertex color (BGRA attribute swizzle) / lighting / white.
	if (lighting) {
		vs += "  vec3 wnrm = normalize(mat3(uWorld) * aNormal);\n";
		// Material color sources per D3DRS_*MATERIALSOURCE (COLOR1 = vertex).
		vs += diffFromVertex ? "  vec4 matDiff = aColor0.zyxw;\n"
		                     : "  vec4 matDiff = uMat[0];\n";
		vs += ambFromVertex ? "  vec3 matAmb = aColor0.zyx;\n"
		                    : "  vec3 matAmb = uMat[1].rgb;\n";
		vs += emisFromVertex ? "  vec3 matEmis = aColor0.zyx;\n"
		                     : "  vec3 matEmis = uMat[2].rgb;\n";
		vs += "  vec3 accum = matEmis + uLit[0].rgb * matAmb;\n";
		vs += "  int numLights = int(uLit[1].x + 0.5);\n";
		vs += "  for (int i = 0; i < numLights; i++) {\n";
		vs += "    int b = 2 + i * 5;\n";
		vs += "    vec4 dirType = uLit[b]; vec4 atn = uLit[b + 4];\n";
		vs += "    vec3 L; float atten = 1.0;\n";
		vs += "    if (dirType.w > 0.5) {\n"; // POINT
		vs += "      vec3 d = uLit[b + 1].xyz - wpos.xyz; float dist = length(d);\n";
		vs += "      if (dist > atn.x) { continue; }\n";
		vs += "      L = d / max(dist, 0.0001);\n";
		vs += "      atten = 1.0 / (atn.y + atn.z * dist + atn.w * dist * dist);\n";
		vs += "    } else { L = -dirType.xyz; }\n";
		vs += "    float ndl = max(dot(wnrm, L), 0.0);\n";
		vs += "    accum += uLit[b + 3].rgb * matAmb * atten;\n";
		vs += "    accum += uLit[b + 2].rgb * matDiff.rgb * ndl * atten;\n";
		vs += "  }\n";
		vs += "  vCol = vec4(clamp(accum, 0.0, 1.0), matDiff.a);\n";
	} else if (l.hasDiffuse) {
		vs += "  vCol = aColor0.zyxw;\n";
	} else {
		vs += "  vCol = vec4(1.0);\n";
	}
	vs += l.hasSpecular ? "  vSpec = aColor1.zyxw;\n" : "  vSpec = vec4(0.0);\n";

	// Per-stage texcoords (selected input set / texgen + optional transform).
	for (int s = 0; s < 2; s++) {
		char b[256];
		const int tci = (int)st[s].tci < texIn ? (int)st[s].tci : 0;
		if (st[s].texgen == 1 && !l.xyzrhw) {
			// D3DTSS_TCI_CAMERASPACEPOSITION: coordinates are the view-space
			// position run through the stage texture matrix.
			snprintf(b, sizeof(b), "  vUV%d = (uTexMat%d * vec4(vpos.xyz, 1.0)).xy;\n", s, s);
		} else if (st[s].texgen == 2 && !l.xyzrhw && l.hasNormal) {
			// D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR: environment mapping.
			snprintf(b, sizeof(b),
				"  vUV%d = (uTexMat%d * vec4(reflect(normalize(vpos.xyz), "
				"normalize(mat3(uView) * mat3(uWorld) * aNormal)), 1.0)).xy;\n", s, s);
		} else if (st[s].texgen == 3 && !l.xyzrhw && l.hasNormal) {
			// D3DTSS_TCI_CAMERASPACENORMAL.
			snprintf(b, sizeof(b),
				"  vUV%d = (uTexMat%d * vec4(normalize(mat3(uView) * mat3(uWorld) * aNormal), 1.0)).xy;\n", s, s);
		} else if (texIn == 0) {
			snprintf(b, sizeof(b), "  vUV%d = vec2(0.0);\n", s);
		} else if (st[s].xform) {
			snprintf(b, sizeof(b), "  vUV%d = (uTexMat%d * vec4(aUV%d.xy, 0.0, 1.0)).xy;\n", s, s, tci);
		} else {
			snprintf(b, sizeof(b), "  vUV%d = aUV%d.xy;\n", s, tci);
		}
		vs += b;
	}
	vs += "}\n";

	// ---------------- fragment shader ----------------
	std::string fs;
	fs += "#version 300 es\nprecision mediump float;\n";
	fs += "uniform sampler2D uTex0;\nuniform sampler2D uTex1;\n";
	fs += "uniform vec4 uTFactor;\nuniform float uAlphaRef;\n";
	fs += "uniform vec4 uFogColor;\nuniform vec2 uFogParams;\n"; // start, end
	fs += "in vec4 vCol;\nin vec4 vSpec;\nin vec2 vUV0;\nin vec2 vUV1;\nin float vFogDepth;\n";
	fs += "out vec4 fragColor;\n";
	fs += "void main() {\n";
	fs += "  vec4 cur = vCol;\n";
	for (int s = 0; s < stagesUsed; s++) {
		char texv[32], texa[32];
		snprintf(texv, sizeof(texv), "tex%d", s);
		snprintf(texa, sizeof(texa), "tex%d.a", s);
		char b[640];
		snprintf(b, sizeof(b), "  vec4 tex%d = texture(uTex%d, vUV%d);\n", s, s, s);
		fs += b;
		int usesTex = 0;
		std::string c0 = combinerArg(st[s].colorArg0, texv, &usesTex);
		std::string c1 = combinerArg(st[s].colorArg1, texv, &usesTex);
		std::string c2 = combinerArg(st[s].colorArg2, texv, &usesTex);
		std::string a0 = combinerArg(st[s].alphaArg0, texv, &usesTex);
		std::string a1 = combinerArg(st[s].alphaArg1, texv, &usesTex);
		std::string a2 = combinerArg(st[s].alphaArg2, texv, &usesTex);
		// At stage 0, D3DTA_CURRENT reads DIFFUSE.
		std::string colorExpr = combinerOp(st[s].colorOp, c0, c1, c2, texa);
		std::string alphaExpr =
			st[s].alphaOp == D3DTOP_DISABLE ? "cur" : combinerOp(st[s].alphaOp, a0, a1, a2, texa);
		snprintf(b, sizeof(b), "  cur = vec4((%s).rgb, (%s).a);\n", colorExpr.c_str(), alphaExpr.c_str());
		fs += b;
	}
	if (alphaTest) {
		const char *cmp = nullptr;
		switch (alphaFunc) {
		case D3DCMP_NEVER: cmp = "true"; break; // discard always
		case D3DCMP_LESS: cmp = "cur.a >= uAlphaRef"; break;
		case D3DCMP_EQUAL: cmp = "cur.a != uAlphaRef"; break;
		case D3DCMP_LESSEQUAL: cmp = "cur.a > uAlphaRef"; break;
		case D3DCMP_GREATER: cmp = "cur.a <= uAlphaRef"; break;
		case D3DCMP_NOTEQUAL: cmp = "cur.a == uAlphaRef"; break;
		case D3DCMP_GREATEREQUAL: cmp = "cur.a < uAlphaRef"; break;
		default: cmp = nullptr; break;
		}
		if (cmp) {
			fs += std::string("  if (") + cmp + ") discard;\n";
		}
	}
	if (fog) {
		fs += "  float f = clamp((uFogParams.y - vFogDepth) / max(uFogParams.y - uFogParams.x, 0.0001), 0.0, 1.0);\n";
		fs += "  cur.rgb = mix(uFogColor.rgb, cur.rgb, f);\n";
	}
	fs += "  fragColor = cur;\n";
	fs += "}\n";

	// GeneralsX @feature Android port 09/09/2026 One-shot, release-included dump of
	// the combiner this backend actually generates for the two-stage greyscale pair
	// (stage 0 MULTIPLYADD, stage 1 DOTPRODUCT3). It is used by exactly two places
	// -- Render2DClass::Render()'s disabled command-bar buttons and
	// ScreenBWFilterDOT3 (the scripted black-and-white cinematics) -- and both have
	// already been mis-rendered once by an operand-order bug in that combiner that
	// no log would have shown. Printed at most once per launch, at shader-build
	// time, never per frame. Safe to delete once the black-and-white shots are
	// confirmed good on a device.
	if (st[1].colorOp == D3DTOP_DOTPRODUCT3) {
		static bool s_dumpedDot3 = false;
		if (!s_dumpedDot3) {
			s_dumpedDot3 = true;
			fprintf(stderr, "[GX-FILTER] dot3 program: stage0 op=%u arg0=0x%x arg1=0x%x arg2=0x%x | "
				"stage1 op=%u arg1=0x%x arg2=0x%x | stagesUsed=%d fvf=0x%x\n",
				st[0].colorOp, st[0].colorArg0, st[0].colorArg1, st[0].colorArg2,
				st[1].colorOp, st[1].colorArg1, st[1].colorArg2, stagesUsed, fvf);
			fprintf(stderr, "[GX-FILTER] dot3 fragment shader:\n%s", fs.c_str());
			fflush(stderr);
		}
	}

	// ---------------- link ----------------
	const std::chrono::steady_clock::time_point buildStart = std::chrono::steady_clock::now();
	ProgramInfo *info = new ProgramInfo();
	// GeneralsX @performance Android port 27/09/2026 The generated source is the program's
	// identity (the state key above only selects which source to generate), so the on-disk
	// cache is keyed on the source text itself plus the driver.
	uint64_t sourceHash = 0;
	if (!m_programCacheDir.empty()) {
		sourceHash = fnv1a64(vs.data(), vs.size(), m_driverHash);
		sourceHash = fnv1a64("|", 1, sourceHash);
		sourceHash = fnv1a64(fs.data(), fs.size(), sourceHash);
		info->prog = loadCachedProgram(sourceHash);
	}
	GLuint vsh = 0, fsh = 0;
	if (info->prog == 0) {
		vsh = compileShader(GL_VERTEX_SHADER, vs);
		fsh = compileShader(GL_FRAGMENT_SHADER, fs);
	}
	if (info->prog == 0 && vsh && fsh) {
		GLuint p = glCreateProgram();
		glAttachShader(p, vsh);
		glAttachShader(p, fsh);
		if (!m_programCacheDir.empty())
			{
				PFN_ProgramParameteri programParameteri = m_glProgramParameteri;
				gxrt::post([programParameteri, p] { programParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE); });
			}
		glLinkProgram(p);
		GLint ok = 0;
		glGetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok) {
			char log[2048];
			glGetProgramInfoLog(p, sizeof(log), nullptr, log);
			fprintf(stderr, "[d3d8gles] program link FAILED: %s\n", log);
			glDeleteProgram(p);
			p = 0;
		}
		info->prog = p;
		if (p && !m_programCacheDir.empty())
			saveCachedProgram(sourceHash, p);
	}
	if (vsh) glDeleteShader(vsh);
	if (fsh) glDeleteShader(fsh);
	m_perfProgramBuilds++;
	m_perfProgramBuildUs += std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - buildStart).count();

	if (info->prog) {
		GLuint p = info->prog;
		info->uWorld = glGetUniformLocation(p, "uWorld");
		// GeneralsX @build Android port GLES experiment 08/30/2026 Bind this
		// program's ViewProjBlock to the shared UBO binding point once,
		// here at link time -- not per-draw. A program whose xyzrhw (2D)
		// vertex path never references uView/uProj (see the xyzrhw branch
		// just above, which only uses uYFlip/uViewportPos) has the whole
		// block optimized out entirely, so blockIdx is GL_INVALID_INDEX
		// there -- expected, not an error.
		{
			GLuint blockIdx = glGetUniformBlockIndex(p, "ViewProjBlock");
			if (blockIdx != GL_INVALID_INDEX) {
				glUniformBlockBinding(p, blockIdx, kViewProjUBOBinding);
			}
		}
		info->uViewportPos = glGetUniformLocation(p, "uViewportPos");
		info->uYFlip = glGetUniformLocation(p, "uYFlip");
		info->uTex0 = glGetUniformLocation(p, "uTex0");
		info->uTex1 = glGetUniformLocation(p, "uTex1");
		info->uTexMat0 = glGetUniformLocation(p, "uTexMat0");
		info->uTexMat1 = glGetUniformLocation(p, "uTexMat1");
		info->uTFactor = glGetUniformLocation(p, "uTFactor");
		info->uAlphaRef = glGetUniformLocation(p, "uAlphaRef");
		info->uFogColor = glGetUniformLocation(p, "uFogColor");
		info->uFogParams = glGetUniformLocation(p, "uFogParams");
		info->uMat = glGetUniformLocation(p, "uMat[0]");
		info->uLit = glGetUniformLocation(p, "uLit[0]");
	}
	info->stageTci[0] = st[0].tci;
	info->stageTci[1] = st[1].tci;
	info->stageXform[0] = st[0].xform;
	info->stageXform[1] = st[1].xform;
	info->stagesUsed = stagesUsed;

	if (m_programCount < kMaxPrograms) {
		m_programs[m_programCount].key = key;
		m_programs[m_programCount].prog = info;
		m_programCount++;
		GLTRACE("program cached (%d total), key=%llx", m_programCount, (unsigned long long)key);
	} else {
		WARN_ONCE(s_progOverflow, "program cache overflow (>%d)", kMaxPrograms);
	}
	return info;
}

// ---------------------------------------------------------------------------
// Texture upload / sampler state
// ---------------------------------------------------------------------------

// Converts one level's shadow bits into a GL-uploadable buffer.
// Returns internalFormat/format/type and (possibly converted) pixels.
struct UploadDesc {
	GLenum internalFormat, format, type;
	bool compressed;
	const uint8_t *pixels;
	uint32_t compressedSize;
	std::vector<uint8_t> converted;
};

// ---------------------------------------------------------------------------
// Software DXT/BC1-3 decompression fallback. Most Android GPUs (Mali,
// Adreno, PowerVR) don't expose GL_EXT_texture_compression_s3tc -- without
// this, every DXT-compressed asset (chiefly terrain .dds, not the mostly-
// uncompressed-TGA menu art) fell through to prepareLevelUpload's "unknown
// format" case and got uploaded as solid magenta (see uploadTexture below).
// This decodes one whole compressed level into a tightly packed RGBA8
// buffer, block by block per the standard S3TC/BC1-BC3 bit layout, so it
// can be uploaded via the ordinary uncompressed glTexImage2D path instead.
// One-time cost per texture level at upload time (uploadTexture runs on
// dirty, not per frame), so a straightforward scalar decode is fine.
// ---------------------------------------------------------------------------

static inline void UnpackRGB565(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
	*r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
	*g = (uint8_t)(((c >> 5)  & 0x3F) * 255 / 63);
	*b = (uint8_t)(( c        & 0x1F) * 255 / 31);
}

// Decodes one 8-byte BC1/DXT1-style color block (also the RGB half of
// BC2/BC3) into a 4x4 RGBA8 array (row-major, 16 texels, 4 bytes each).
// hasAlpha selects DXT1's punch-through-alpha special case (color0 <=
// color1 numerically => palette entries 2/3 collapse to a 50% blend +
// transparent black). BC2/BC3 callers pass hasAlpha=false since those
// formats carry alpha in a separate 8-byte block and always treat this
// block as opaque 4-color; the alpha channel written here gets overwritten
// by the caller afterwards.
static void DecodeBC1Block(const uint8_t block[8], bool hasAlpha, uint8_t outRGBA[16 * 4])
{
	const uint16_t c0 = (uint16_t)(block[0] | (block[1] << 8));
	const uint16_t c1 = (uint16_t)(block[2] | (block[3] << 8));
	uint8_t r0, g0, b0, r1, g1, b1;
	UnpackRGB565(c0, &r0, &g0, &b0);
	UnpackRGB565(c1, &r1, &g1, &b1);

	uint8_t pal[4][4]; // [index][RGBA]
	pal[0][0] = r0; pal[0][1] = g0; pal[0][2] = b0; pal[0][3] = 255;
	pal[1][0] = r1; pal[1][1] = g1; pal[1][2] = b1; pal[1][3] = 255;
	const bool punchThrough = hasAlpha && c0 <= c1;
	if (!punchThrough) {
		pal[2][0] = (uint8_t)((2 * r0 + r1) / 3);
		pal[2][1] = (uint8_t)((2 * g0 + g1) / 3);
		pal[2][2] = (uint8_t)((2 * b0 + b1) / 3);
		pal[2][3] = 255;
		pal[3][0] = (uint8_t)((r0 + 2 * r1) / 3);
		pal[3][1] = (uint8_t)((g0 + 2 * g1) / 3);
		pal[3][2] = (uint8_t)((b0 + 2 * b1) / 3);
		pal[3][3] = 255;
	} else {
		pal[2][0] = (uint8_t)((r0 + r1) / 2);
		pal[2][1] = (uint8_t)((g0 + g1) / 2);
		pal[2][2] = (uint8_t)((b0 + b1) / 2);
		pal[2][3] = 255;
		pal[3][0] = 0; pal[3][1] = 0; pal[3][2] = 0; pal[3][3] = 0; // transparent
	}

	const uint32_t indices = (uint32_t)block[4] | ((uint32_t)block[5] << 8) |
	                          ((uint32_t)block[6] << 16) | ((uint32_t)block[7] << 24);
	for (int i = 0; i < 16; i++) {
		const int idx = (indices >> (i * 2)) & 0x3;
		outRGBA[i * 4 + 0] = pal[idx][0];
		outRGBA[i * 4 + 1] = pal[idx][1];
		outRGBA[i * 4 + 2] = pal[idx][2];
		outRGBA[i * 4 + 3] = pal[idx][3];
	}
}

// Decodes one 8-byte BC3/DXT5-style interpolated alpha block into 16
// 8-bit alpha values (row-major, matches DecodeBC1Block's texel order).
static void DecodeBC3AlphaBlock(const uint8_t block[8], uint8_t outAlpha[16])
{
	const uint8_t a0 = block[0];
	const uint8_t a1 = block[1];
	uint8_t pal[8];
	pal[0] = a0;
	pal[1] = a1;
	if (a0 > a1) {
		for (int i = 1; i <= 6; i++)
			pal[1 + i] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
	} else {
		for (int i = 1; i <= 4; i++)
			pal[1 + i] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
		pal[6] = 0;
		pal[7] = 255;
	}
	// 6 bytes = 48 bits = 16 * 3-bit indices, little-endian bit-packed.
	uint64_t bits = 0;
	for (int i = 0; i < 6; i++)
		bits |= (uint64_t)block[2 + i] << (8 * i);
	for (int i = 0; i < 16; i++) {
		const int idx = (int)((bits >> (i * 3)) & 0x7);
		outAlpha[i] = pal[idx];
	}
}

// Decodes one whole DXT1/2/3/4/5-compressed level into a tightly packed
// w*h*4 RGBA8 buffer. Reads full 4x4 blocks from src (compressed data is
// always stored in whole blocks, even for the last row/column of a
// non-multiple-of-4 or sub-4x4 mip) but only writes the w x h texels that
// are actually in bounds into out. Returns false (leaving out alone) if
// srcSize is too small for the implied block count, so the caller can
// still fall back safely instead of reading out of bounds.
static bool DecodeDXTLevel(D3DFORMAT fmt, unsigned w, unsigned h,
                           const uint8_t *src, size_t srcSize,
                           std::vector<uint8_t> *out)
{
	const bool isDXT1 = (fmt == D3DFMT_DXT1);
	const unsigned blockBytes = isDXT1 ? 8 : 16;
	const unsigned blocksWide = (w + 3) / 4;
	const unsigned blocksHigh = (h + 3) / 4;
	const size_t needed = (size_t)blocksWide * blocksHigh * blockBytes;
	if (needed > srcSize) return false; // truncated/corrupt data, bail to magenta

	out->resize((size_t)w * h * 4);
	uint8_t *dst = out->data();

	for (unsigned by = 0; by < blocksHigh; by++) {
		for (unsigned bx = 0; bx < blocksWide; bx++) {
			const uint8_t *blockSrc = src + ((size_t)by * blocksWide + bx) * blockBytes;
			uint8_t rgba[16 * 4];
			uint8_t alpha[16]; // only used for BC2/BC3

			if (isDXT1) {
				DecodeBC1Block(blockSrc, /*hasAlpha=*/true, rgba);
			} else if (fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3) {
				// Explicit 4-bit-per-texel alpha: 8 bytes, 2 nibbles/byte,
				// texel order matches the RGB block's row-major layout.
				DecodeBC1Block(blockSrc + 8, /*hasAlpha=*/false, rgba);
				for (int i = 0; i < 16; i++) {
					const uint8_t nibble = (blockSrc[i / 2] >> ((i & 1) * 4)) & 0xF;
					alpha[i] = (uint8_t)(nibble * 17); // 4-bit -> 8-bit (0,17,...,255)
				}
				for (int i = 0; i < 16; i++) rgba[i * 4 + 3] = alpha[i];
			} else { // DXT4 / DXT5: interpolated alpha block
				DecodeBC3AlphaBlock(blockSrc, alpha);
				DecodeBC1Block(blockSrc + 8, /*hasAlpha=*/false, rgba);
				for (int i = 0; i < 16; i++) rgba[i * 4 + 3] = alpha[i];
			}

			// Scatter the 4x4 block into the w x h output, clipping at
			// the right/bottom edge for non-multiple-of-4 dimensions.
			const unsigned baseX = bx * 4, baseY = by * 4;
			const unsigned maxX = (baseX + 4 <= w) ? 4 : (w - baseX);
			const unsigned maxY = (baseY + 4 <= h) ? 4 : (h - baseY);
			for (unsigned ty = 0; ty < maxY; ty++) {
				for (unsigned tx = 0; tx < maxX; tx++) {
					const uint8_t *srcTexel = &rgba[(ty * 4 + tx) * 4];
					uint8_t *dstTexel = &dst[((size_t)(baseY + ty) * w + (baseX + tx)) * 4];
					dstTexel[0] = srcTexel[0];
					dstTexel[1] = srcTexel[1];
					dstTexel[2] = srcTexel[2];
					dstTexel[3] = srcTexel[3];
				}
			}
		}
	}
	return true;
}

static bool prepareLevelUpload(D3DFORMAT fmt, unsigned w, unsigned h,
                               const uint8_t *src, size_t srcSize, bool hasS3TC, UploadDesc *out)
{
	out->compressed = false;
	out->compressedSize = 0;
	out->pixels = src;
	switch (fmt) {
	case D3DFMT_A8R8G8B8:
	case D3DFMT_X8R8G8B8: {
		// BGRA bytes -> RGBA
		out->converted.resize((size_t)w * h * 4);
		const bool forceOpaque = (fmt == D3DFMT_X8R8G8B8);
		for (size_t i = 0; i < (size_t)w * h; i++) {
			out->converted[i * 4 + 0] = src[i * 4 + 2];
			out->converted[i * 4 + 1] = src[i * 4 + 1];
			out->converted[i * 4 + 2] = src[i * 4 + 0];
			out->converted[i * 4 + 3] = forceOpaque ? 255 : src[i * 4 + 3];
		}
		out->pixels = out->converted.data();
		out->internalFormat = GL_RGBA;
		out->format = GL_RGBA;
		out->type = GL_UNSIGNED_BYTE;
		return true;
	}
	case D3DFMT_R5G6B5:
		out->internalFormat = GL_RGB565;
		out->format = GL_RGB;
		out->type = GL_UNSIGNED_SHORT_5_6_5;
		return true;
	case D3DFMT_A4R4G4B4: {
		// ARGB4444 -> RGBA4444 (per-short component rotate)
		out->converted.resize((size_t)w * h * 2);
		const uint16_t *s = (const uint16_t *)src;
		uint16_t *d = (uint16_t *)out->converted.data();
		for (size_t i = 0; i < (size_t)w * h; i++) {
			const uint16_t v = s[i];
			const uint16_t a = (v >> 12) & 0xF, r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
			d[i] = (uint16_t)((r << 12) | (g << 8) | (b << 4) | a);
		}
		out->pixels = out->converted.data();
		out->internalFormat = GL_RGBA4;
		out->format = GL_RGBA;
		out->type = GL_UNSIGNED_SHORT_4_4_4_4;
		return true;
	}
	case D3DFMT_A1R5G5B5:
	case D3DFMT_X1R5G5B5: {
		out->converted.resize((size_t)w * h * 2);
		const uint16_t *s = (const uint16_t *)src;
		uint16_t *d = (uint16_t *)out->converted.data();
		const bool opaque = (fmt == D3DFMT_X1R5G5B5);
		for (size_t i = 0; i < (size_t)w * h; i++) {
			const uint16_t v = s[i];
			const uint16_t a = opaque ? 1 : ((v >> 15) & 0x1);
			const uint16_t r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
			d[i] = (uint16_t)((r << 11) | (g << 6) | (b << 1) | a);
		}
		out->pixels = out->converted.data();
		out->internalFormat = GL_RGB5_A1;
		out->format = GL_RGBA;
		out->type = GL_UNSIGNED_SHORT_5_5_5_1;
		return true;
	}
	case D3DFMT_L8:
		out->internalFormat = GL_LUMINANCE;
		out->format = GL_LUMINANCE;
		out->type = GL_UNSIGNED_BYTE;
		return true;
	case D3DFMT_A8:
		out->internalFormat = GL_ALPHA;
		out->format = GL_ALPHA;
		out->type = GL_UNSIGNED_BYTE;
		return true;
	case D3DFMT_A8L8:
		out->internalFormat = GL_LUMINANCE_ALPHA;
		out->format = GL_LUMINANCE_ALPHA;
		out->type = GL_UNSIGNED_BYTE;
		return true;
	case D3DFMT_DXT1:
	case D3DFMT_DXT2:
	case D3DFMT_DXT3:
	case D3DFMT_DXT4:
	case D3DFMT_DXT5: {
		if (!hasS3TC) {
			if (DecodeDXTLevel(fmt, w, h, src, srcSize, &out->converted)) {
				WARN_ONCE(s_dxtSoftDecode, "DXT texture but WEBGL_compressed_texture_s3tc "
					"missing; using software BC1-3 decode fallback");
				out->pixels = out->converted.data();
				out->internalFormat = GL_RGBA;
				out->format = GL_RGBA;
				out->type = GL_UNSIGNED_BYTE;
				return true;
			}
			WARN_ONCE(s_noS3tc, "DXT texture but WEBGL_compressed_texture_s3tc missing "
				"and software decode failed (truncated data?)");
			return false;
		}
		out->compressed = true;
		out->compressedSize = (uint32_t)srcSize;
		switch (fmt) {
		case D3DFMT_DXT1: out->internalFormat = 0x83F1; break; // COMPRESSED_RGBA_S3TC_DXT1_EXT
		case D3DFMT_DXT2:
		case D3DFMT_DXT3: out->internalFormat = 0x83F2; break; // DXT3
		default: out->internalFormat = 0x83F3; break;          // DXT5
		}
		return true;
	}
	default: {
		// Repeat (capped) so the offender survives the console ring buffer.
		static int s_fmtLogs = 0;
		if (s_fmtLogs < 20) {
			s_fmtLogs++;
			fprintf(stderr, "[d3d8gles] MAGENTA: texture format %d (0x%x) not implemented %ux%u\n",
				(int)fmt, (unsigned)fmt, w, h);
		}
		return false;
	}
	}
}

// GeneralsX @performance Android port 27/09/2026 A software-decoded DXT1 level repacked to
// 16 bits per texel. DXT1's colours are RGB565 endpoints (and their interpolations) with at
// most a 1-bit alpha, so RGB565 -- or RGB5_A1 when the texture uses its punch-through
// alpha -- keeps what the format can express at half the memory and bandwidth of the RGBA8
// the decoder produces; on a GPU without S3TC (Mali) that is still 4x the compressed size
// instead of 8x. DXT3/DXT5 keep RGBA8: their alpha gradients would band in 4 bits.
static void packRGBA8To16(UploadDesc *up, unsigned w, unsigned h, bool withAlpha)
{
	const size_t texels = (size_t)w * h;
	std::vector<uint8_t> packed(texels * 2);
	const uint8_t *src = up->converted.data();
	uint16_t *dst = reinterpret_cast<uint16_t *>(packed.data());
	for (size_t i = 0; i < texels; i++) {
		const uint8_t r = src[i * 4 + 0], g = src[i * 4 + 1], b = src[i * 4 + 2], a = src[i * 4 + 3];
		if (withAlpha)
			dst[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >= 128 ? 1 : 0));
		else
			dst[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
	}
	up->converted.swap(packed);
	up->pixels = up->converted.data();
	up->internalFormat = withAlpha ? GL_RGB5_A1 : GL_RGB565;
	up->format = withAlpha ? GL_RGBA : GL_RGB;
	up->type = withAlpha ? GL_UNSIGNED_SHORT_5_5_5_1 : GL_UNSIGNED_SHORT_5_6_5;
}

void WebGLPipeline::uploadTexture(WebGLTexture *tex)
{
	GLTextureState &g = tex->m_gl;
	if (g.name == 0) {
		glGenTextures(1, &g.name);
		g_texturesCreated++;
	}
	glBindTexture(GL_TEXTURE_2D, g.name);
	const int levels = (int)tex->m_levels.size();
	const bool isDXT = FormatIsDXT(tex->m_format);

	int uploaded = 0;
	// Decided on level 0 and kept for every level: a mip chain whose levels differ in
	// internal format is incomplete in GL and samples as black.
	const bool dxt16 = m_opt.dxt565 && !m_hasS3TC && tex->m_format == D3DFMT_DXT1;
	int dxt16Alpha = -1;
	for (int lvl = 0; lvl < levels; lvl++) {
		WebGLSurface *s = tex->m_levels[lvl];
		UploadDesc up;
		const bool prepared = prepareLevelUpload(tex->m_format, s->m_width, s->m_height,
		                        s->m_bits.data(), s->m_bits.size(), m_hasS3TC, &up);
		if (prepared && dxt16 && !up.compressed && up.type == GL_UNSIGNED_BYTE) {
			if (dxt16Alpha < 0) {
				dxt16Alpha = 0;
				for (size_t i = 3; i < up.converted.size(); i += 4) {
					if (up.converted[i] != 255) {
						dxt16Alpha = 1;
						break;
					}
				}
			}
			packRGBA8To16(&up, s->m_width, s->m_height, dxt16Alpha == 1);
			m_perfDxt16Levels++;
			m_perfDxt16SavedBytes += (double)s->m_width * s->m_height * 2;
		}
		if (!prepared) {
			// Unknown format: upload magenta so it is visible, not crashy.
			std::vector<uint8_t> mag((size_t)s->m_width * s->m_height * 4);
			for (size_t i = 0; i < mag.size(); i += 4) {
				mag[i] = 255; mag[i + 1] = 0; mag[i + 2] = 255; mag[i + 3] = 255;
			}
			glTexImage2D(GL_TEXTURE_2D, lvl, GL_RGBA, s->m_width, s->m_height, 0,
			             GL_RGBA, GL_UNSIGNED_BYTE, mag.data());
			uploaded = lvl + 1;
			continue;
		}
		if (up.compressed) {
			glCompressedTexImage2D(GL_TEXTURE_2D, lvl, up.internalFormat,
			                       s->m_width, s->m_height, 0, up.compressedSize, up.pixels);
			// Checked on the GL thread, right after the upload, without waiting for it.
			const unsigned cw = s->m_width, ch = s->m_height, cfmt = (unsigned)tex->m_format, csize = up.compressedSize;
			gxrt::post([lvl, cw, ch, cfmt, csize] {
				const GLenum cerr = gxrt::rawGetError();
				if (cerr != GL_NO_ERROR) {
					fprintf(stderr, "[d3d8gles] DXT upload error 0x%x lvl=%d %ux%u fmt=0x%x size=%u\n",
						cerr, lvl, cw, ch, cfmt, csize);
				}
			});
			uploaded = lvl + 1;
		} else {
			glTexImage2D(GL_TEXTURE_2D, lvl, up.internalFormat, s->m_width, s->m_height, 0,
			             up.format, up.type, up.pixels);
			uploaded = lvl + 1;
			if (!isDXT && levels > 1) {
				// The engine frequently fills only level 0 of uncompressed
				// textures; GPU-generate the chain instead of sampling the
				// empty (transparent black) shadow mips. DXT textures
				// (including software-decoded ones, which land in this same
				// branch) always come with a full pre-authored mip chain, so
				// this must keep looping for them instead of stopping after
				// level 0.
				break;
			}
		}
	}
	if (!isDXT && levels > 1) {
		glGenerateMipmap(GL_TEXTURE_2D);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
	} else {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, uploaded > 0 ? uploaded - 1 : 0);
	}
	// GeneralsX @build Android port 09/05/2026 A texture that received no
	// glTexImage2D at all is INCOMPLETE in GL and samples as opaque black --
	// silently, with no GL error. That is the exact signature of the reported
	// "half the textures are gone after restarting on Medium detail", and
	// nothing in this function would otherwise say it happened. Capped.
	if (uploaded == 0) {
		static int s_emptyUploads = 0;
		if (s_emptyUploads < 12) {
			s_emptyUploads++;
			fprintf(stderr, "[gxtex] EMPTY upload: texture %ux%u fmt=%d levels=%d "
				"-- incomplete, will sample as black\n",
				levels > 0 ? tex->m_levels[0]->m_width : 0,
				levels > 0 ? tex->m_levels[0]->m_height : 0,
				(int)tex->m_format, levels);
		}
	}
	g.dirty = false;
	g.samplerKey = ~0u; // force sampler reapply
	GLTRACE("texture %u uploaded (%dx%d fmt=%d levels=%d)", g.name,
	        tex->m_levels[0]->m_width, tex->m_levels[0]->m_height, (int)tex->m_format, levels);
}

void WebGLPipeline::applySamplerState(WebGLDevice *dev, unsigned stage, WebGLTexture *tex)
{
	const DWORD minf = dev->getStageState(stage, D3DTSS_MINFILTER);
	const DWORD magf = dev->getStageState(stage, D3DTSS_MAGFILTER);
	const DWORD mipf = dev->getStageState(stage, D3DTSS_MIPFILTER);
	const DWORD au = dev->getStageState(stage, D3DTSS_ADDRESSU);
	const DWORD av = dev->getStageState(stage, D3DTSS_ADDRESSV);
	const uint32_t key = (uint32_t)((minf & 7) | ((magf & 7) << 3) | ((mipf & 7) << 6) |
	                                ((au & 7) << 9) | ((av & 7) << 12));
	if (tex->m_gl.samplerKey == key) return;
	tex->m_gl.samplerKey = key;

	const bool hasMips = tex->m_levels.size() > 1;
	GLenum glMin;
	if (!hasMips || mipf == D3DTEXF_NONE) {
		glMin = (minf == D3DTEXF_POINT) ? GL_NEAREST : GL_LINEAR;
	} else if (mipf == D3DTEXF_POINT) {
		glMin = (minf == D3DTEXF_POINT) ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST;
	} else {
		glMin = (minf == D3DTEXF_POINT) ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR;
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, glMin);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
	                magf == D3DTEXF_POINT ? GL_NEAREST : GL_LINEAR);

	auto addr = [](DWORD m) -> GLenum {
		switch (m) {
		case D3DTADDRESS_MIRROR: return GL_MIRRORED_REPEAT;
		case D3DTADDRESS_CLAMP:
		case D3DTADDRESS_BORDER: return GL_CLAMP_TO_EDGE;
		case D3DTADDRESS_WRAP:
		default: return GL_REPEAT;
		}
	};
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, addr(au ? au : D3DTADDRESS_WRAP));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, addr(av ? av : D3DTADDRESS_WRAP));
}

void WebGLPipeline::invalidateTextureBinding(GLuint name)
{
	if (name == 0) return; // 0 already means "no texture" to the cache, never a real object
	for (int s = 0; s < 2; s++) {
		if (m_lastBoundTex[s] == name) m_lastBoundTex[s] = ~0u;
	}
}

void WebGLPipeline::bindTextures(WebGLDevice *dev, ProgramInfo *prog)
{
	for (int s = 0; s < 2; s++) {
		WebGLTexture *tex = dev->getTexture2D(s);
		glActiveTexture(GL_TEXTURE0 + s);
		if (tex) {
			if (tex->m_gl.dirty || tex->m_gl.name == 0) {
				uploadTexture(tex); // binds tex->m_gl.name as a side effect
				m_lastBoundTex[s] = tex->m_gl.name;
			} else if (m_lastBoundTex[s] != tex->m_gl.name) {
				glBindTexture(GL_TEXTURE_2D, tex->m_gl.name);
				m_lastBoundTex[s] = tex->m_gl.name;
			}
			applySamplerState(dev, s, tex);
		} else if (m_lastBoundTex[s] != 0) {
			glBindTexture(GL_TEXTURE_2D, 0);
			m_lastBoundTex[s] = 0;
		}
	}
	if (prog->uTex0 >= 0) glUniform1i(prog->uTex0, 0);
	if (prog->uTex1 >= 0) glUniform1i(prog->uTex1, 1);
}

// ---------------------------------------------------------------------------
// Fixed state + uniforms
// ---------------------------------------------------------------------------

void WebGLPipeline::applyFixedState(WebGLDevice *dev)
{
	const D3DVIEWPORT8 &vpKey = effectiveViewport(dev);
	FixedStateKey key{};
	key.zEnable = dev->getRenderState(D3DRS_ZENABLE);
	key.zWrite = dev->getRenderState(D3DRS_ZWRITEENABLE);
	key.zFunc = dev->getRenderState(D3DRS_ZFUNC);
	key.zBias = dev->getRenderState(D3DRS_ZBIAS);
	key.alphaBlend = dev->getRenderState(D3DRS_ALPHABLENDENABLE);
	key.srcBlend = dev->getRenderState(D3DRS_SRCBLEND);
	key.destBlend = dev->getRenderState(D3DRS_DESTBLEND);
	key.cullMode = dev->getRenderState(D3DRS_CULLMODE);
	key.colorWrite = dev->getRenderState(D3DRS_COLORWRITEENABLE);
	key.stencilEnable = dev->getRenderState(D3DRS_STENCILENABLE);
	key.stencilFunc = dev->getRenderState(D3DRS_STENCILFUNC);
	key.stencilRef = dev->getRenderState(D3DRS_STENCILREF);
	key.stencilMask = dev->getRenderState(D3DRS_STENCILMASK);
	key.stencilFail = dev->getRenderState(D3DRS_STENCILFAIL);
	key.stencilZFail = dev->getRenderState(D3DRS_STENCILZFAIL);
	key.stencilPass = dev->getRenderState(D3DRS_STENCILPASS);
	key.stencilWriteMask = dev->getRenderState(D3DRS_STENCILWRITEMASK);
	key.twoSided = s_gxTwoSidedStencil ? 1 : 0;
	key.stencilBackPass = s_gxTwoSidedStencil ? s_gxTwoSidedBackPass : 0;
	key.vpX = vpKey.X;
	key.vpY = vpKey.Y;
	key.vpW = vpKey.Width;
	key.vpH = vpKey.Height;
	key.vpMinZ = vpKey.MinZ;
	key.vpMaxZ = vpKey.MaxZ;

	if (m_haveFixedStateKey && key == m_lastFixedStateKey) {
		m_perfStateCacheHits++;
		return; // Nothing this function sets has changed since the last draw.
	}
	m_perfStateCacheMisses++;
	// GeneralsX @performance Android port 29/09/2026 Re-issue only the groups that changed. A
	// miss used to re-send every call below -- depth, blend, cull, bias, colour mask, stencil,
	// viewport -- when one field moved, and particles move the blend state on nearly every
	// draw: device logs showed ~2 depth and ~2 enable calls per particle draw on top of the
	// blend change, and ~23 us of driver time per particle draw against ~6 us for a model draw.
	// Drivers tend to treat any state call as "state dirty, revalidate at the next draw", and
	// the values being the same does not save that. `all` covers the first draw and every
	// invalidation (m_haveFixedStateKey reset after a clear, etc.), exactly as before.
	const bool all = !m_haveFixedStateKey;
	const FixedStateKey prev = m_lastFixedStateKey;
	m_lastFixedStateKey = key;
	m_haveFixedStateKey = true;

	// Depth
	const DWORD zEnable = dev->getRenderState(D3DRS_ZENABLE);
	if (all || key.zEnable != prev.zEnable) {
		if (zEnable) glEnable(GL_DEPTH_TEST);
		else glDisable(GL_DEPTH_TEST);
	}
	if (all || key.zWrite != prev.zWrite)
		glDepthMask(dev->getRenderState(D3DRS_ZWRITEENABLE) ? GL_TRUE : GL_FALSE);
	const DWORD zfunc = dev->getRenderState(D3DRS_ZFUNC);
	if (all || key.zFunc != prev.zFunc)
		glDepthFunc(d3dCmpToGL(zfunc ? zfunc : D3DCMP_LESSEQUAL));

	// Blend
	if (dev->getRenderState(D3DRS_ALPHABLENDENABLE)) {
		if (all || !prev.alphaBlend)
			glEnable(GL_BLEND);
		const DWORD sb = dev->getRenderState(D3DRS_SRCBLEND);
		const DWORD db = dev->getRenderState(D3DRS_DESTBLEND);
		// The blend function is only in effect while blending is on, so compare it against
		// the last one actually sent: prev's factors may belong to a draw that had it off.
		if (all || !prev.alphaBlend || key.srcBlend != m_lastSentSrcBlend || key.destBlend != m_lastSentDestBlend) {
			glBlendFunc(d3dBlendToGL(sb ? sb : D3DBLEND_ONE), d3dBlendToGL(db ? db : D3DBLEND_ZERO));
			m_lastSentSrcBlend = key.srcBlend;
			m_lastSentDestBlend = key.destBlend;
		}
	} else if (all || prev.alphaBlend) {
		glDisable(GL_BLEND);
	}

	// Cull. GeneralsX @build Android port GLES experiment - this used to
	// assume the vertex shader's clip-space y-negate flipped winding once
	// relative to D3D screen space (hence the swapped GL_FRONT/GL_BACK
	// below), compensating for that flip. Now that the y-negate is gone
	// (see the vertex shader's cpos.y comment), winding order matches D3D's
	// own directly with glFrontFace at its GL default (GL_CCW), so the
	// mapping no longer needs the swap: D3DCULL_CW -> GL_BACK,
	// D3DCULL_CCW -> GL_FRONT. The swapped mapping was silently culling
	// nearly everything (terrain, video quads) after the y-negate fix,
	// since front/back faces were now backwards relative to what D3D
	// intended.
	if (key.twoSided) {
		if (all || !prev.twoSided)
			glDisable(GL_CULL_FACE); // both faces drawn; the stencil op tells them apart
	} else if (all || key.cullMode != prev.cullMode || prev.twoSided)
	switch (dev->getRenderState(D3DRS_CULLMODE)) {
	case D3DCULL_CW:
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
		break;
	case D3DCULL_CCW:
		glEnable(GL_CULL_FACE);
		glCullFace(GL_FRONT);
		break;
	default:
		glDisable(GL_CULL_FACE);
		break;
	}

	// Depth bias (D3D8 ZBIAS 0..16 pulls towards the viewer)
	const DWORD zbias = dev->getRenderState(D3DRS_ZBIAS);
	if (all || key.zBias != prev.zBias) {
		if (zbias) {
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(-1.0f, -(float)zbias * 2.0f);
		} else {
			glDisable(GL_POLYGON_OFFSET_FILL);
		}
	}

	// Color mask. Zero is a real value: stencil shadow volumes render with
	// COLORWRITEENABLE=0 (stencil-only) - mapping it to "write everything"
	// made every shadow volume a visible black silhouette.
	const DWORD cw = dev->getRenderState(D3DRS_COLORWRITEENABLE);
	if (all || key.colorWrite != prev.colorWrite)
		glColorMask((cw & 1) != 0, (cw & 2) != 0, (cw & 4) != 0, (cw & 8) != 0);

	// Stencil
	const bool stencilChanged = all || key.stencilEnable != prev.stencilEnable ||
		key.stencilFunc != prev.stencilFunc || key.stencilRef != prev.stencilRef ||
		key.stencilMask != prev.stencilMask || key.stencilFail != prev.stencilFail ||
		key.stencilZFail != prev.stencilZFail || key.stencilPass != prev.stencilPass ||
		key.stencilWriteMask != prev.stencilWriteMask || key.twoSided != prev.twoSided ||
		key.stencilBackPass != prev.stencilBackPass;
	if (!stencilChanged) {
		// unchanged: nothing to send
	} else if (dev->getRenderState(D3DRS_STENCILENABLE)) {
		glEnable(GL_STENCIL_TEST);
		// GeneralsX @bugfix Android port 09/05/2026 Two deviations from D3D8
		// semantics used to live in these three calls, and together they broke
		// stencil shadow volumes on GLES while leaving DXVK unaffected.
		//
		// 1. The reference value was cast straight to a signed GLint.
		//    W3DVolumetricShadow's fill passes set D3DRS_STENCILREF =
		//    0x80808080, which as a GLint is -2139062144. GLES 3.0 4.1.4 says
		//    ref is clamped to [0, 2^s - 1], so a negative value clamps to 0.
		// 2. A D3DRS_STENCILMASK of 0 was treated as "never set" and replaced
		//    with 0xFFFFFFFF. Zero is a real D3D value meaning "compare no
		//    bits", i.e. the test always passes -- and it is exactly what the
		//    engine asks for here, since TheW3DShadowManager's stencil shadow
		//    mask is 0 (W3DShadow.cpp) when the player-colour occluder feature
		//    is off. Substituting a full mask turned a guaranteed pass into a
		//    real comparison.
		//
		// Together those made the fill passes run as
		// glStencilFunc(GL_GEQUAL, 0, 0xFF) -- "pass only where the stencil is
		// still 0" -- instead of "always pass". The INCR pass then raised the
		// whole volume silhouette to 1, and the DECR pass, meeting a buffer
		// that now held 1, failed the stencil test everywhere it had just
		// incremented (STENCILFAIL is KEEP, so nothing decremented). The far
		// half never cancelled the near half, the entire silhouette stayed at
		// 1, and the darkening quad painted all of it -- the dark slab
		// reported around aircraft.
		//
		// This also explains the two results that made no sense before:
		// inverting the cull face changed nothing (whichever half draws first
		// paints the silhouette to 1 and locks the other half out through the
		// stencil test, and both halves share a screen footprint), and the
		// occlusion probe still saw samples in the DECR pass (fringe pixels
		// and depth-failed regions were left at 0 and did survive).
		//
		// The masks are now passed through verbatim; the device seeds D3D8's
		// documented stencil defaults so a zero here really does mean the game
		// asked for zero (see WebGLDevice's constructor).
		glStencilFunc(d3dCmpToGL(dev->getRenderState(D3DRS_STENCILFUNC)),
		              (GLint)(dev->getRenderState(D3DRS_STENCILREF) & 0xFFu),
		              (GLuint)dev->getRenderState(D3DRS_STENCILMASK));
		if (key.twoSided) {
			// Front faces are the ones D3DCULL_CW leaves visible (see the cull mapping above).
			const GLenum sfail = d3dStencilOpToGL(dev->getRenderState(D3DRS_STENCILFAIL));
			const GLenum zfail = d3dStencilOpToGL(dev->getRenderState(D3DRS_STENCILZFAIL));
			const GLenum frontPass = d3dStencilOpToGL(dev->getRenderState(D3DRS_STENCILPASS));
			const GLenum backPass = d3dStencilOpToGL(key.stencilBackPass);
			PFN_StencilOpSeparate opSeparate = m_glStencilOpSeparate;
			gxrt::post([opSeparate, sfail, zfail, frontPass, backPass] {
				opSeparate(GL_FRONT, sfail, zfail, frontPass);
				opSeparate(GL_BACK, sfail, zfail, backPass);
			});
		} else {
			glStencilOp(d3dStencilOpToGL(dev->getRenderState(D3DRS_STENCILFAIL)),
			            d3dStencilOpToGL(dev->getRenderState(D3DRS_STENCILZFAIL)),
			            d3dStencilOpToGL(dev->getRenderState(D3DRS_STENCILPASS)));
		}
		glStencilMask((GLuint)dev->getRenderState(D3DRS_STENCILWRITEMASK));
	} else {
		glDisable(GL_STENCIL_TEST);
	}

	// GeneralsX @build Android port GLES experiment - glViewport's y is
	// measured from the BOTTOM of the render target in GL (always, this is
	// not related to the vertex shader's clip-space convention at all --
	// that's a separate, already-correct concern, see the vertex shader's
	// cpos.y comment); D3D's vp.Y is measured from the TOP. Converting via
	// RTH - vp.Y - vp.Height (the standard D3D->GL viewport translation)
	// gives the correct physical rectangle for both. Passing vp.Y through
	// unconverted only happens to work for a fullscreen viewport (Y=0,
	// Height=RT, where the formula reduces to 0 either way) -- exactly why
	// menus (which Render2DClass always renders through a fullscreen
	// viewport) looked fine while the actual in-game partial 3D viewport
	// (top black band, picking offset by the same amount) did not: confirmed
	// on a real device screenshot during live gameplay.
	const D3DVIEWPORT8 &vp = effectiveViewport(dev);
	GLint glvp[4];
	targetRect(vp, &glvp[0], &glvp[1], &glvp[2], &glvp[3]);
	if (all || memcmp(glvp, m_lastSentViewport, sizeof(glvp)) != 0)
		glViewport(glvp[0], glvp[1], (GLsizei)glvp[2], (GLsizei)glvp[3]);
	memcpy(m_lastSentViewport, glvp, sizeof(glvp));
	if (all || key.vpMinZ != prev.vpMinZ || key.vpMaxZ != prev.vpMaxZ)
		glDepthRangef(vp.MinZ, vp.MaxZ);

}

void WebGLPipeline::applyUniforms(WebGLDevice *dev, ProgramInfo *prog, unsigned fvf)
{
	if (prog->prog != m_lastProgram) {
		glUseProgram(prog->prog);
		m_lastProgram = prog->prog;
		// Uniform locations are per-program: a cache hit against a key
		// computed for the PREVIOUS program would wrongly skip uploading to
		// this one, leaving its uniforms unset. Force every sub-block below
		// to treat this draw as a first upload for the newly bound program.
		// GeneralsX @build Android port GLES experiment 08/30/2026
		// m_haveViewProjKey is deliberately NOT reset here anymore -- the
		// camera now lives in a UBO (kViewProjUBOBinding), whose DATA is
		// independent of which program is bound, so a program switch alone
		// no longer means the GPU forgot the camera. This was the single
		// biggest source of uniform-cache misses on a real device (a
		// per-block breakdown showed the combined rate collapsing to ~50%
		// mid-battle purely from program switches between terrain/unit/
		// particle/decal draws, even though the camera itself hadn't
		// changed at all).
		m_haveTexMatKey = m_haveMiscKey = m_haveMaterialKey = m_haveLightingKey = false;
	}

	// D3D row-major memory uploaded untransposed IS the transpose GL wants
	// for column-vector math (see plan notes). uWorld is deliberately never
	// cached -- it changes on nearly every draw in real battlefield
	// rendering (each object has its own transform) -- see ViewProjKey's
	// declaration for why the rest of this function's blocks are cached.
	if (prog->uWorld >= 0) {
		const float *world = (const float *)&dev->getTransform(D3DTS_WORLD);
		if (!prog->haveWorld || memcmp(prog->lastWorld, world, sizeof(prog->lastWorld)) != 0) {
			glUniformMatrix4fv(prog->uWorld, 1, GL_FALSE, world);
			memcpy(prog->lastWorld, world, sizeof(prog->lastWorld));
			prog->haveWorld = true;
			m_perfWorldUploads++;
		} else {
			m_perfWorldSkips++;
		}
	}

	// GeneralsX @build Android port GLES experiment 08/30/2026 view/proj now
	// live in a UBO (kViewProjUBOBinding), not per-program uniform
	// locations -- unconditional (no prog->uView/uProj gate; those fields
	// are gone from ProgramInfo, see the struct's comment) since the
	// buffer's binding is fixed for the whole session and every vertex
	// shader variant declares the same ViewProjBlock regardless of FVF.
	// Cheap to run even for xyzrhw (2D) draws that don't reference the
	// block at all in their shader body -- worst case a redundant upload
	// when content alternates between 3D and 2D, never a correctness
	// issue. Real re-upload (glBufferSubData) only happens when the value
	// actually differs from what's already on the GPU.
	{
		ViewProjKey key{};
		memcpy(key.view, &dev->getTransform(D3DTS_VIEW), sizeof(key.view));
		memcpy(key.proj, &dev->getTransform(D3DTS_PROJECTION), sizeof(key.proj));
		if (m_haveViewProjKey && key == m_lastViewProjKey) {
			m_perfUniformCacheHits++;
			m_perfUniformViewProjHits++;
		} else {
			m_perfUniformCacheMisses++;
			m_perfUniformViewProjMisses++;
			glBindBuffer(GL_UNIFORM_BUFFER, m_viewProjUBO);
			glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(key), &key);
			m_lastViewProjKey = key;
			m_haveViewProjKey = true;
		}
	}

	if (prog->uTexMat0 >= 0 || prog->uTexMat1 >= 0) {
		TexMatKey key{};
		memcpy(key.texMat0, &dev->getTransform(D3DTS_TEXTURE0), sizeof(key.texMat0));
		memcpy(key.texMat1, &dev->getTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + 1)), sizeof(key.texMat1));
		if (m_haveTexMatKey && key == m_lastTexMatKey) {
			m_perfUniformCacheHits++;
			m_perfUniformTexMatHits++;
		} else {
			m_perfUniformCacheMisses++;
			m_perfUniformTexMatMisses++;
			if (prog->uTexMat0 >= 0) glUniformMatrix4fv(prog->uTexMat0, 1, GL_FALSE, key.texMat0);
			if (prog->uTexMat1 >= 0) glUniformMatrix4fv(prog->uTexMat1, 1, GL_FALSE, key.texMat1);
			m_lastTexMatKey = key;
			m_haveTexMatKey = true;
		}
	}

	if (prog->uViewportPos >= 0 || prog->uYFlip >= 0 || prog->uTFactor >= 0 ||
	    prog->uAlphaRef >= 0 || prog->uFogColor >= 0) {
		const D3DVIEWPORT8 &vp = effectiveViewport(dev);
		MiscUniformKey key{};
		key.vpX = (float)vp.X; key.vpY = (float)vp.Y; key.vpW = (float)vp.Width; key.vpH = (float)vp.Height;
		key.yFlip = m_yFlip;
		argbToFloats(dev->getRenderState(D3DRS_TEXTUREFACTOR), key.tFactor);
		key.alphaRef = (float)(dev->getRenderState(D3DRS_ALPHAREF) & 0xFF) / 255.0f;
		argbToFloats(dev->getRenderState(D3DRS_FOGCOLOR), key.fogColor);
		key.fogStart = dwordToFloat(dev->getRenderState(D3DRS_FOGSTART));
		key.fogEnd = dwordToFloat(dev->getRenderState(D3DRS_FOGEND));

		if (m_haveMiscKey && key == m_lastMiscKey) {
			m_perfUniformCacheHits++;
			m_perfUniformMiscHits++;
		} else {
			m_perfUniformCacheMisses++;
			m_perfUniformMiscMisses++;
			if (prog->uViewportPos >= 0) glUniform4f(prog->uViewportPos, key.vpX, key.vpY, key.vpW, key.vpH);
			if (prog->uYFlip >= 0) glUniform1f(prog->uYFlip, key.yFlip);
			if (prog->uTFactor >= 0) glUniform4fv(prog->uTFactor, 1, key.tFactor);
			if (prog->uAlphaRef >= 0) glUniform1f(prog->uAlphaRef, key.alphaRef);
			if (prog->uFogColor >= 0) {
				glUniform4fv(prog->uFogColor, 1, key.fogColor);
				glUniform2f(prog->uFogParams, key.fogStart, key.fogEnd);
			}
			m_lastMiscKey = key;
			m_haveMiscKey = true;
		}
	}

	// NOTE: each uniform can be optimized out independently (a program whose
	// material sources are all vertex colors has NO uMat* uniforms but still
	// needs its lights). Never gate the light upload on a material location.
	if (prog->uMat >= 0) {
		const D3DMATERIAL8 &m = dev->getMaterial();
		MaterialKey key{};
		memcpy(key.diffuse, &m.Diffuse, sizeof(key.diffuse));
		memcpy(key.ambient, &m.Ambient, sizeof(key.ambient));
		memcpy(key.emissive, &m.Emissive, sizeof(key.emissive));
		if (m_haveMaterialKey && key == m_lastMaterialKey) {
			m_perfUniformCacheHits++;
			m_perfUniformMaterialHits++;
		} else {
			m_perfUniformCacheMisses++;
			m_perfUniformMaterialMisses++;
			float packed[12];
			memcpy(packed + 0, key.diffuse, 16);
			memcpy(packed + 4, key.ambient, 16);
			memcpy(packed + 8, key.emissive, 16);
			glUniform4fv(prog->uMat, 3, packed);
			m_lastMaterialKey = key;
			m_haveMaterialKey = true;
		}
	}
	if (prog->uLit >= 0) {
		LightingKey key{};
		argbToFloats(dev->getRenderState(D3DRS_AMBIENT), key.globalAmbient);
		int n = 0;
		for (unsigned i = 0; i < WebGLDevice::kMaxLights && n < 4; i++) {
			if (!dev->isLightEnabled(i)) continue;
			const D3DLIGHT8 &L = dev->getLight(i);
			key.types[n] = (L.Type == D3DLIGHT_POINT) ? 1 : 0;
			key.dirs[n * 3 + 0] = L.Direction.x;
			key.dirs[n * 3 + 1] = L.Direction.y;
			key.dirs[n * 3 + 2] = L.Direction.z;
			key.poss[n * 3 + 0] = L.Position.x;
			key.poss[n * 3 + 1] = L.Position.y;
			key.poss[n * 3 + 2] = L.Position.z;
			memcpy(&key.diff[n * 4], &L.Diffuse, 16);
			memcpy(&key.amb[n * 4], &L.Ambient, 16);
			key.att[n * 4 + 0] = L.Range;
			key.att[n * 4 + 1] = L.Attenuation0 > 0 ? L.Attenuation0 : 1.0f;
			key.att[n * 4 + 2] = L.Attenuation1;
			key.att[n * 4 + 3] = L.Attenuation2;
			n++;
		}
		key.numLights = n;

		if (m_haveLightingKey && key == m_lastLightingKey) {
			m_perfUniformCacheHits++;
			m_perfUniformLightingHits++;
		} else {
			m_perfUniformCacheMisses++;
			m_perfUniformLightingMisses++;
			// One call: only the lights in use are sent; the shader reads no further.
			float packed[22 * 4] = {};
			memcpy(packed + 0, key.globalAmbient, 16);
			packed[4] = (float)key.numLights;
			for (int i = 0; i < key.numLights; i++) {
				float *b = packed + (2 + i * 5) * 4;
				b[0] = key.dirs[i * 3 + 0]; b[1] = key.dirs[i * 3 + 1]; b[2] = key.dirs[i * 3 + 2];
				b[3] = key.types[i] == 1 ? 1.0f : 0.0f;
				b[4] = key.poss[i * 3 + 0]; b[5] = key.poss[i * 3 + 1]; b[6] = key.poss[i * 3 + 2];
				memcpy(b + 8, &key.diff[i * 4], 16);
				memcpy(b + 12, &key.amb[i * 4], 16);
				memcpy(b + 16, &key.att[i * 4], 16);
			}
			glUniform4fv(prog->uLit, 2 + key.numLights * 5, packed);
			m_lastLightingKey = key;
			m_haveLightingKey = true;
		}
	}
}

// ---------------------------------------------------------------------------
// Draw paths
// ---------------------------------------------------------------------------

static GLenum primModeGL(unsigned primType)
{
	switch (primType) {
	case D3DPT_POINTLIST: return GL_POINTS;
	case D3DPT_LINELIST: return GL_LINES;
	case D3DPT_LINESTRIP: return GL_LINE_STRIP;
	case D3DPT_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
	case D3DPT_TRIANGLEFAN: return GL_TRIANGLE_FAN;
	case D3DPT_TRIANGLELIST:
	default: return GL_TRIANGLES;
	}
}

static unsigned primVertexCount(unsigned primType, unsigned primCount)
{
	switch (primType) {
	case D3DPT_POINTLIST: return primCount;
	case D3DPT_LINELIST: return primCount * 2;
	case D3DPT_LINESTRIP: return primCount + 1;
	case D3DPT_TRIANGLESTRIP:
	case D3DPT_TRIANGLEFAN: return primCount + 2;
	case D3DPT_TRIANGLELIST:
	default: return primCount * 3;
	}
}

// GeneralsX @build Android port GLES experiment - perf pass, split from a
// single setupAttribs() after a real device log showed the VAO cache
// growing by thousands of entries within seconds in ordinary gameplay: the
// engine draws a lot of its content (see DX8Wrapper's BUFFER_TYPE_DYNAMIC_DX8
// pool) through one shared vertex buffer with a *base-vertex offset* that
// advances practically every draw, so folding `base` into the VAO's cached
// attribute pointers (as the first version of this cache did) meant that
// class of content got a brand-new VAO -- and GL object -- almost every
// single call, defeating the cache and leaking VAOs for the session's
// lifetime. Splitting the two concerns fixes both: attribute
// enable/disable state is genuinely per-(FVF layout) and only needs
// setting once when a VAO is first created (a fresh VAO starts with every
// attribute disabled, so enableAttribs() only ever turns bits on).
// Attribute *pointers* additionally encode `base` and DO need reissuing
// whenever it changes -- but that's a handful of glVertexAttribPointer
// calls against an already-bound, otherwise-unchanged VAO, not a new GL
// object plus the full disable/enable/pointer dance every time.
static void enableAttribs(const FVFLayout &l)
{
	for (int i = 0; i < 8; i++) glDisableVertexAttribArray(i);
	glEnableVertexAttribArray(0);
	if (l.hasNormal) glEnableVertexAttribArray(1);
	if (l.hasDiffuse) glEnableVertexAttribArray(2);
	if (l.hasSpecular) glEnableVertexAttribArray(3);
	const int texIn = l.texCount > 2 ? 2 : l.texCount;
	for (int i = 0; i < texIn; i++) glEnableVertexAttribArray(4 + i);
}

// Sets attribute pointers (format + base-relative offset) for the currently
// bound ARRAY_BUFFER and VAO. Safe to call on an already-enabled attribute
// -- glVertexAttribPointer only ever touches format/pointer state, never
// enabled/disabled state, regardless of how many times it's reissued.
static void setAttribPointers(const FVFLayout &l, unsigned stride, intptr_t base)
{
	glVertexAttribPointer(0, l.xyzrhw ? 4 : 3, GL_FLOAT, GL_FALSE, stride, (const void *)(base + l.posOffset));
	if (l.hasNormal)
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void *)(base + l.normalOffset));
	if (l.hasDiffuse)
		glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (const void *)(base + l.diffuseOffset));
	if (l.hasSpecular)
		glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (const void *)(base + l.specularOffset));
	const int texIn = l.texCount > 2 ? 2 : l.texCount;
	for (int i = 0; i < texIn; i++)
		glVertexAttribPointer(4 + i, l.texSize[i], GL_FLOAT, GL_FALSE, stride, (const void *)(base + l.texOffset[i]));
}

// Byte-wise FNV-1a over VAOKey's raw bytes. Safe: VAOKey is four 4-byte POD
// members (GLuint/unsigned), so there's no padding to worry about hashing
// garbage from. Same collision-tolerant precedent as computeProgramKey()
// -- see kMaxVAOs's declaration in gles_pipeline.h.
uint64_t WebGLPipeline::hashVAOKey(const VAOKey &k)
{
	uint64_t h = 0xcbf29ce484222325ull;
	const unsigned char *p = reinterpret_cast<const unsigned char *>(&k);
	for (size_t i = 0; i < sizeof(k); i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

void WebGLPipeline::evictVAOsForBuffer(GLuint name)
{
	bool evictedAny = false;
	for (auto it = m_vaoCache.begin(); it != m_vaoCache.end(); ) {
		if (it->second.key.vbo == name || it->second.key.ibo == name) {
			glDeleteVertexArrays(1, &it->second.vao);
			it = m_vaoCache.erase(it);
			evictedAny = true;
		} else {
			++it;
		}
	}
	// The "same key as last draw, skip everything" fast path in
	// bindVertexLayout() below trusts that the previously bound VAO is still
	// valid; if it just got deleted here, that trust would be wrong.
	if (evictedAny) m_haveLastVAOKey = false;
}

void WebGLPipeline::bindVertexLayout(const FVFLayout &l, GLuint vbo, GLuint ibo,
                                     unsigned fvf, unsigned stride, int base)
{
	VAOKey key{};
	key.vbo = vbo;
	key.ibo = ibo;
	key.fvf = fvf;
	key.stride = stride;
	// `base` is deliberately NOT part of the key -- see enableAttribs()'s
	// comment. It's still tracked below (m_lastVAOBase / VAOCacheEntry::
	// lastBase) since a VAO's *pointers* do encode it and must be kept
	// current even when the VAO object itself is being reused.

	if (m_haveLastVAOKey && key == m_lastVAOKey && base == m_lastVAOBase) {
		m_perfVAOCacheHits++;
		return; // the correct VAO -- attribs, pointers, and element-buffer binding alike -- is already bound
	}
	m_lastVAOKey = key;
	m_lastVAOBase = base;
	m_haveLastVAOKey = true;

	const uint64_t hash = hashVAOKey(key);
	auto it = m_vaoCache.find(hash);
	if (it != m_vaoCache.end()) {
		// GeneralsX @build Android port GLES experiment - this counts as a
		// hit, not a miss: the VAO *object* is reused either way, which is
		// the expensive part this cache exists to avoid (glGenVertexArrays,
		// plus the old per-draw glDisableVertexAttribArray x8 dance this
		// object now only ever pays once). A base change below is real but
		// comparatively cheap -- a handful of glVertexAttribPointer calls,
		// tracked separately (m_perfVAOPointerRefresh) so the perf log can
		// show how much of that residual cost is still coming from content
		// that draws through a shared/dynamic buffer pool at a
		// constantly-advancing offset, distinct from a true cache miss.
		m_perfVAOCacheHits++;
		glBindVertexArray(it->second.vao);
		if (it->second.lastBase != base) {
			// GL_ARRAY_BUFFER must be the right buffer for
			// glVertexAttribPointer to capture it correctly;
			// GL_ELEMENT_ARRAY_BUFFER is untouched since it doesn't encode
			// `base` at all.
			m_perfVAOPointerRefresh++;
			bindArrayBuffer(vbo);
			setAttribPointers(l, stride, base);
			it->second.lastBase = base;
		}
		return;
	}

	// True miss: this (vbo, ibo, fvf, stride) combination has never been
	// seen before. Build (or, past the sanity-backstop cap, temporarily
	// fall back to an uncached bind against) a VAO for it.
	m_perfVAOCacheMisses++;
	if (m_vaoCache.size() >= kMaxVAOs) {
		WARN_ONCE(s_vaoOverflow, "VAO cache at its %zu-entry sanity cap, no longer "
		          "caching new combinations this session", kMaxVAOs);
		glBindVertexArray(0);
		bindArrayBuffer(vbo);
		enableAttribs(l);
		setAttribPointers(l, stride, base);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
		m_haveLastVAOKey = false; // unknown/uncached state -- always re-decide next draw
		return;
	}

	GLuint vao = 0;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	bindArrayBuffer(vbo); // GL_ARRAY_BUFFER is not VAO state; safe to route through the skip cache
	enableAttribs(l);
	setAttribPointers(l, stride, base);
	// GL_ELEMENT_ARRAY_BUFFER, unlike GL_ARRAY_BUFFER, IS part of the
	// currently-bound VAO's state -- bind unconditionally (not through a
	// skip cache; see m_lastArrayBuffer's comment for why one would be
	// unsafe here) so it's captured into the VAO just created above. 0 is
	// the correct, valid binding for the non-indexed draw() path.
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);

	m_vaoCache.emplace(hash, VAOCacheEntry{key, vao, base});
}

// GeneralsX @perf Android port 09/05/2026 Draw-call breakdown by subsystem.
// Real-device logs show ~1200-2500 draws/frame in gameplay at 13-15 fps, i.e.
// roughly 27k draws/sec -- about what a mobile GLES driver sustains when each
// draw carries state changes, so we are draw-call bound. But "reduce draw
// calls" is useless without knowing WHERE they come from: the instancing
// attempt earlier only ever collapsed ~10% of them, because it targeted the
// rigid-model path while most draws apparently come from somewhere else.
//
// Engine code tags the few passes it can identify cheaply (see
// d3d8gles_SetDrawCategory's callers); everything else lands in OTHER
// (terrain, shadows, skinned meshes, ...). Callers save/restore the previous
// value, so nesting works.
enum GeneralsXDrawCategory {
	GX_DRAWCAT_OTHER = 0,
	GX_DRAWCAT_MODELS,   // DX8TextureCategoryClass::Render -- rigid HLod meshes
	GX_DRAWCAT_SORTED,   // SortingRendererClass::Flush -- particles, decals
	GX_DRAWCAT_2D,       // Render2DClass::Render -- all UI and video
	GX_DRAWCAT_TERRAIN,  // HeightMapRenderObjClass::Render
	GX_DRAWCAT_SHADOWS,  // W3DProjectedShadowManager::renderShadows
	GX_DRAWCAT_SKIN,     // DX8SkinFVFCategoryContainer::Render
	GX_DRAWCAT_COUNT
};
static int s_gxDrawCategory = GX_DRAWCAT_OTHER;
static unsigned s_gxDrawsByCategory[GX_DRAWCAT_COUNT] = {0, 0, 0, 0, 0, 0, 0};

// GeneralsX @perf Android port 09/05/2026 UI cost buckets, filled by engine
// code that times itself (see d3d8gles_AddUiTiming's callers). Reported next to
// the draw split so one log line shows both where the draws and where the CPU
// time go.
static double s_gxUiTimeUs[3] = {0.0, 0.0, 0.0};

// GeneralsX @performance Android port 28/09/2026 CPU time inside drawCommon() per draw source
// -- this layer's state translation plus the GL calls it makes -- and the part of it spent in
// the glDraw* call alone. Next to the engine's mainScene time ([GX-PERF-DISPLAY]) this says
// whether a slow frame is the engine walking its scene, this layer, or the driver.
static double s_gxDrawUsByCategory[GX_DRAWCAT_COUNT] = {0, 0, 0, 0, 0, 0, 0};
static double s_gxDrawCallUs = 0.0;
// The glDraw part per draw source, and split by whether the draw's vertex or index buffer was
// written just before it (the engine's dynamic ring: map, write, draw). Draws after a write
// costing far more than the rest would put the time in the driver's handling of freshly written
// buffers rather than in the draws themselves.
static double s_gxDrawCallUsByCategory[GX_DRAWCAT_COUNT] = {0, 0, 0, 0, 0, 0, 0};
static bool s_gxDrawAfterWrite = false;
// GeneralsX @performance Android port 29/09/2026 Running totals that never reset, read by the
// engine's scene phase timers ([GX-PERF-SCENE]) as before/after deltas. That splits each scene
// phase into its draw count, this layer's time and the driver's glDraw time, so the engine's own
// CPU per phase is the remainder. The per-source counters above cannot do this: most of the
// scene's draws fall into "other".
static unsigned long long s_gxTotalDraws = 0;
static double s_gxTotalDrawUs = 0.0;
static double s_gxTotalGlDrawUs = 0.0;
static double s_gxDrawCallUsAfterWrite = 0.0;
static unsigned s_gxDrawsAfterWrite = 0;
extern double d3d8gles_perfUploadUs;
extern unsigned d3d8gles_perfUploadCalls;
extern int d3d8gles_curDrawCategory;
extern unsigned d3d8gles_stateCalls[8][7];

namespace {
inline double gxNowUs()
{
	return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct GxDrawTimer
{
	double start = gxNowUs();
	~GxDrawTimer()
	{
		const double us = gxNowUs() - start;
		s_gxDrawUsByCategory[s_gxDrawCategory] += us;
		s_gxTotalDrawUs += us;
	}
};
}


extern "C" void d3d8gles_AddUiTiming(int bucket, double microseconds)
{
	if (bucket >= 0 && bucket < 3) s_gxUiTimeUs[bucket] += microseconds;
}

// GeneralsX @bugfix Android port 28/09/2026 Vsync on or off, applied at the next present on the
// GL thread. The engine asks for it off only while its frame-rate limit is above the display's
// refresh rate -- the skirmish Game Speed slider raised on the 60 Hz engine, which needs more
// than 60 frames for more than 60 logic frames a second (logic cannot outrun rendering here, see
// FramePacer). Android's compositor drops the surplus frames, so nothing tears.
static int s_gxWantUncappedPresent = 0;
extern "C" void d3d8gles_SetPresentUncapped(bool uncapped)
{
	s_gxWantUncappedPresent = uncapped ? 1 : 0;
}

// GeneralsX @performance Android port 29/09/2026 Two-sided stencil for the stencil shadow volumes.
// D3D8 has no two-sided stencil, so the engine draws every volume twice: front faces incrementing,
// then back faces decrementing (W3DVolumetricShadowManager::renderShadows). GL does both in one
// pass with culling off and a separate stencil op per face. While this is on, applyFixedState()
// disables culling and applies D3DRS_STENCILPASS to front faces and backPassOp to back faces.
// Returns 0 when the GLES backend is not the one rendering (DXVK), or the driver lacks
// glStencilOpSeparate, so the engine keeps its two passes.
extern "C" int d3d8gles_SetTwoSidedStencil(int enable, unsigned backPassOp)
{
	if (!s_gxGlesReady || WebGLPipeline::get()->m_glStencilOpSeparate == nullptr) {
		s_gxTwoSidedStencil = false;
		return 0;
	}
	s_gxTwoSidedStencil = enable != 0;
	s_gxTwoSidedBackPass = backPassOp;
	return 1;
}

extern "C" void d3d8gles_GetDrawTotals(unsigned long long *draws, double *drawUs, double *glDrawUs)
{
	*draws = s_gxTotalDraws;
	*drawUs = s_gxTotalDrawUs;
	*glDrawUs = s_gxTotalGlDrawUs;
}

// GeneralsX @feature Android port 01/10/2026 Render resolution below the screen's, on the native GLES
// backend: a virtual backbuffer. It has two sizes. The engine's (w x h) is the game's resolution, and
// all the engine ever sees: viewports, 2D coordinates and touch input stay in it. The rendered one
// (renderW x renderH) is that times the launcher's upscaler mode (Ultra Quality .. Performance, as
// FSR 1 names them), and only glViewport/glScissor know about it (targetRect). So the game runs at the
// screen's own resolution, like a PC game with FSR on, and the GPU shades fewer pixels. When the
// game's resolution is smaller than the window (a smaller Resolution from the game's Options), everything the
// engine draws to "the backbuffer" -- scene, interface, videos, loading screens -- goes into an
// offscreen framebuffer of the game's size, and present() stretches it over the whole window in
// one pass right before the swap, with Snapdragon GSR 1 or bilinear. The engine's own pillarbox
// (an offscreen target switched in and out around parts of the frame) is not used on GLES: there it
// showed frozen frames and flicker (logs-45/46) because not every draw path went through it. Here
// there is no such thing as a draw that misses the target. Returns 1 when active.
extern "C" int d3d8gles_SetVirtualBackbuffer(int w, int h, int renderW, int renderH, int gsr)
{
	if (!s_gxGlesReady)
		return 0;
	return WebGLPipeline::get()->setVirtualBackbuffer(w, h, renderW, renderH, gsr != 0) ? 1 : 0;
}

// The present pass's shaders. One full-screen triangle from gl_VertexID (no vertex buffer), and
// texture coordinates in the same orientation the scene was rendered in (an offscreen target's
// v = 1 is D3D's top row, see Pillarbox_End()'s flipForGLTextureStorage). The SGSR fragment
// stage is Qualcomm's sgsr1_shader_mobile.frag (RGBA mode, edge threshold 8/255, sharpness 2),
// BSD-3-Clause, Copyright (c) 2025 Qualcomm Innovation Center, Inc.
// (https://github.com/SnapdragonStudios/snapdragon-gsr, sgsr/v1), with its input names adapted,
// #version raised to 310 es for textureGather, and the `highp` dropped from three vec2
// constructor calls (a precision qualifier there is not valid GLSL ES; glslc rejects it).
static const char *kPresentVs300 =
	"#version 300 es\n"
	"out highp vec4 in_TEXCOORD0;\n"
	"void main() {\n"
	"  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
	"  in_TEXCOORD0 = vec4(p, 0.0, 0.0);\n"
	"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";
static const char *kPresentVs310 =
	"#version 310 es\n"
	"out highp vec4 in_TEXCOORD0;\n"
	"void main() {\n"
	"  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
	"  in_TEXCOORD0 = vec4(p, 0.0, 0.0);\n"
	"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";
static const char *kPresentPlainFs =
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform mediump sampler2D uTex0;\n"
	"in highp vec4 in_TEXCOORD0;\n"
	"layout(location=0) out vec4 out_Target0;\n"
	"void main() {\n"
	"  out_Target0 = vec4(texture(uTex0, in_TEXCOORD0.xy).rgb, 1.0);\n"
	"}\n";
static const char *kPresentGsrFs =
		"#version 310 es\n"
		"precision mediump float;\n"
		"precision highp int;\n"
		"uniform highp vec4 ViewportInfo[1];\n"
		"uniform mediump sampler2D uTex0;\n"
		"in highp vec4 in_TEXCOORD0;\n"
		"layout(location=0) out vec4 out_Target0;\n"
		"float fastLanczos2(float x) {\n"
		"  float wA = x - 4.0;\n"
		"  float wB = x * wA - wA;\n"
		"  wA *= wA;\n"
		"  return wB * wA;\n"
		"}\n"
		"vec2 weightY(float dx, float dy, float c, float std) {\n"
		"  float x = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * std, 0.0, 1.0);\n"
		"  float w = fastLanczos2(x);\n"
		"  return vec2(w, w * c);\n"
		"}\n"
		"void main() {\n"
		"  const int mode = 1;\n"
		"  float edgeThreshold = 8.0 / 255.0;\n"
		"  float edgeSharpness = 2.0;\n"
		"  vec4 color;\n"
		"  color.xyz = textureLod(uTex0, in_TEXCOORD0.xy, 0.0).xyz;\n"
		"  highp vec2 imgCoord = ((in_TEXCOORD0.xy * ViewportInfo[0].zw) + vec2(-0.5, 0.5));\n"
		"  highp vec2 imgCoordPixel = floor(imgCoord);\n"
		"  highp vec2 coord = (imgCoordPixel * ViewportInfo[0].xy);\n"
		"  vec2 pl = (imgCoord + (-imgCoordPixel));\n"
		"  vec4 left = textureGather(uTex0, coord, mode);\n"
		"  float edgeVote = abs(left.z - left.y) + abs(color[mode] - left.y) + abs(color[mode] - left.z);\n"
		"  if (edgeVote > edgeThreshold) {\n"
		"    coord.x += ViewportInfo[0].x;\n"
		"    vec4 right = textureGather(uTex0, coord + vec2(ViewportInfo[0].x, 0.0), mode);\n"
		"    vec4 upDown;\n"
		"    upDown.xy = textureGather(uTex0, coord + vec2(0.0, -ViewportInfo[0].y), mode).wz;\n"
		"    upDown.zw = textureGather(uTex0, coord + vec2(0.0, ViewportInfo[0].y), mode).yx;\n"
		"    float mean = (left.y + left.z + right.x + right.w) * 0.25;\n"
		"    left = left - vec4(mean);\n"
		"    right = right - vec4(mean);\n"
		"    upDown = upDown - vec4(mean);\n"
		"    color.w = color[mode] - mean;\n"
		"    float sum = (((((abs(left.x) + abs(left.y)) + abs(left.z)) + abs(left.w)) + (((abs(right.x) + abs(right.y)) + abs(right.z)) + abs(right.w))) + (((abs(upDown.x) + abs(upDown.y)) + abs(upDown.z)) + abs(upDown.w)));\n"
		"    float std = 2.181818 / sum;\n"
		"    vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, std);\n"
		"    aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, std);\n"
		"    aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, std);\n"
		"    aWY += weightY(pl.x, pl.y - 2.0, upDown.w, std);\n"
		"    aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, std);\n"
		"    aWY += weightY(pl.x, pl.y - 1.0, left.y, std);\n"
		"    aWY += weightY(pl.x, pl.y, left.z, std);\n"
		"    aWY += weightY(pl.x + 1.0, pl.y, left.w, std);\n"
		"    aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, std);\n"
		"    aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, std);\n"
		"    aWY += weightY(pl.x - 2.0, pl.y, right.z, std);\n"
		"    aWY += weightY(pl.x - 1.0, pl.y, right.w, std);\n"
		"    float finalY = aWY.y / aWY.x;\n"
		"    float maxY = max(max(left.y, left.z), max(right.x, right.w));\n"
		"    float minY = min(min(left.y, left.z), min(right.x, right.w));\n"
		"    finalY = clamp(edgeSharpness * finalY, minY, maxY);\n"
		"    float deltaY = finalY - color.w;\n"
		"    deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);\n"
		"    color.x = clamp((color.x + deltaY), 0.0, 1.0);\n"
		"    color.y = clamp((color.y + deltaY), 0.0, 1.0);\n"
		"    color.z = clamp((color.z + deltaY), 0.0, 1.0);\n"
		"  }\n"
		"  color.w = 1.0;\n"
		"  out_Target0 = color;\n"
		"}\n";

static GLuint buildPresentProgram(const char *vs, const char *fs)
{
	GLuint vsh = compileShader(GL_VERTEX_SHADER, vs);
	GLuint fsh = compileShader(GL_FRAGMENT_SHADER, fs);
	GLuint p = 0;
	if (vsh && fsh) {
		p = glCreateProgram();
		glAttachShader(p, vsh);
		glAttachShader(p, fsh);
		glLinkProgram(p);
		GLint ok = 0;
		glGetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok) {
			char log[2048];
			glGetProgramInfoLog(p, sizeof(log), nullptr, log);
			fprintf(stderr, "[d3d8gles] present program link FAILED: %s\n", log);
			glDeleteProgram(p);
			p = 0;
		}
	}
	if (vsh) glDeleteShader(vsh);
	if (fsh) glDeleteShader(fsh);
	return p;
}

const D3DVIEWPORT8 &WebGLPipeline::effectiveViewport(WebGLDevice *dev)
{
	const D3DVIEWPORT8 &vp = dev->getViewport();
	if (m_vbActive && m_curFBO == m_vbFBO &&
	    ((int)(vp.X + vp.Width) > m_vbW || (int)(vp.Y + vp.Height) > m_vbH)) {
		m_vbFullVp.X = 0;
		m_vbFullVp.Y = 0;
		m_vbFullVp.Width = (DWORD)m_vbW;
		m_vbFullVp.Height = (DWORD)m_vbH;
		m_vbFullVp.MinZ = vp.MinZ;
		m_vbFullVp.MaxZ = vp.MaxZ;
		return m_vbFullVp;
	}
	return vp;
}

void WebGLPipeline::targetRect(const D3DVIEWPORT8 &vp, GLint *x, GLint *y, GLsizei *w, GLsizei *h) const
{
	int x0 = (int)vp.X, y0 = (int)vp.Y;
	int x1 = x0 + (int)vp.Width, y1 = y0 + (int)vp.Height;
	int rtH = m_curRTHeight;
	if (m_vbActive && m_curFBO == m_vbFBO && (m_vbRW != m_vbW || m_vbRH != m_vbH)) {
		// Both edges scaled, so that rectangles sharing an edge in the engine's pixels still share
		// one here.
		x0 = (int)(((int64_t)x0 * m_vbRW + m_vbW / 2) / m_vbW);
		x1 = (int)(((int64_t)x1 * m_vbRW + m_vbW / 2) / m_vbW);
		y0 = (int)(((int64_t)y0 * m_vbRH + m_vbH / 2) / m_vbH);
		y1 = (int)(((int64_t)y1 * m_vbRH + m_vbH / 2) / m_vbH);
		rtH = m_vbRH;
	}
	*x = (GLint)x0;
	*y = (GLint)(rtH - y1);
	*w = (GLsizei)(x1 - x0);
	*h = (GLsizei)(y1 - y0);
}

bool WebGLPipeline::setVirtualBackbuffer(int w, int h, int renderW, int renderH, bool gsr)
{
	if (w <= 0 || h <= 0) {
		if (m_vbActive) {
			// Back to the window itself.
			m_vbActive = false;
			m_vbUpscaled = false;
			m_vbBypass = false;
			m_fbWidth = m_winW;
			m_fbHeight = m_winH;
			if (m_curFBO == m_vbFBO) {
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				m_curFBO = 0;
				m_curRTWidth = m_fbWidth;
				m_curRTHeight = m_fbHeight;
			}
			m_haveFixedStateKey = false;
			fprintf(stderr, "[d3d8gles] virtual backbuffer off: rendering at the window's %dx%d\n", m_fbWidth, m_fbHeight);
		}
		return false;
	}
	if (renderW <= 0 || renderH <= 0 || renderW > w || renderH > h) {
		renderW = w;
		renderH = h;
	}
	if (!m_vbActive) {
		m_winW = m_fbWidth;
		m_winH = m_fbHeight;
	}
	if (m_vbActive && w == m_vbW && h == m_vbH && renderW == m_vbRW && renderH == m_vbRH && gsr == m_vbGsr)
		return true;

	if (m_vbFBO == 0) {
		glGenFramebuffers(1, &m_vbFBO);
		glGenTextures(1, &m_vbTex);
		glGenRenderbuffers(1, &m_vbDepth);
	}
	glBindTexture(GL_TEXTURE_2D, m_vbTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, renderW, renderH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	m_lastBoundTex[0] = m_lastBoundTex[1] = ~0u;
	glBindRenderbuffer(GL_RENDERBUFFER, m_vbDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, renderW, renderH);
	glBindFramebuffer(GL_FRAMEBUFFER, m_vbFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_vbTex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_vbDepth);
	const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "[d3d8gles] virtual backbuffer %dx%d incomplete (0x%x); rendering at the window's size\n", renderW, renderH, status);
		glBindFramebuffer(GL_FRAMEBUFFER, m_curFBO == m_vbFBO ? 0 : m_curFBO);
		m_vbActive = false;
		return false;
	}

	if (m_presentPlainProg == 0) {
		m_presentPlainProg = buildPresentProgram(kPresentVs300, kPresentPlainFs);
		if (m_presentPlainProg)
			m_presentPlainTex = glGetUniformLocation(m_presentPlainProg, "uTex0");
		glGenVertexArrays(1, &m_presentVAO);
	}
	if (gsr && m_presentGsrProg == 0 && !m_presentGsrTried) {
		m_presentGsrTried = true;
		m_presentGsrProg = buildPresentProgram(kPresentVs310, kPresentGsrFs);
		if (m_presentGsrProg) {
			m_presentGsrTex = glGetUniformLocation(m_presentGsrProg, "uTex0");
			m_presentGsrInfo = glGetUniformLocation(m_presentGsrProg, "ViewportInfo[0]");
		}
		fprintf(stderr, "[d3d8gles] SGSR upscale %s\n", m_presentGsrProg ? "ready (Snapdragon Game Super Resolution 1)"
			: "unavailable; stretching with bilinear filtering");
	}
	if (m_presentPlainProg == 0) {
		fprintf(stderr, "[d3d8gles] virtual backbuffer: present program unavailable; rendering at the window's size\n");
		glBindFramebuffer(GL_FRAMEBUFFER, m_curFBO == m_vbFBO ? 0 : m_curFBO);
		m_vbActive = false;
		return false;
	}

	const bool wasBackbuffer = (m_curFBO == 0) || (m_vbActive && m_curFBO == m_vbFBO);
	m_vbUpscaled = false;
	m_vbBypass = false;
	m_vbActive = true;
	m_vbW = w;
	m_vbH = h;
	m_vbRW = renderW;
	m_vbRH = renderH;
	m_vbGsr = gsr && m_presentGsrProg != 0;
	m_fbWidth = w;
	m_fbHeight = h;
	if (wasBackbuffer) {
		m_curFBO = m_vbFBO;
		m_curRTWidth = w;
		m_curRTHeight = h;
	} else {
		glBindFramebuffer(GL_FRAMEBUFFER, m_curFBO);
	}
	m_haveFixedStateKey = false;
	fprintf(stderr, "[d3d8gles] virtual backbuffer %dx%d rendered at %dx%d (%d%%), stretched to the %dx%d window with %s\n",
		w, h, renderW, renderH, (int)((int64_t)renderW * 100 / w), m_winW, m_winH, m_vbGsr ? "SGSR" : "bilinear filtering");
	return true;
}

// The virtual backbuffer over the whole window: by present() right before the swap, or by
// upscaleSceneNow() once the scene is drawn. Leaves the window's framebuffer bound.
void WebGLPipeline::stretchVirtualBackbuffer()
{
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, m_winW, m_winH);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	const GLuint prog = m_vbGsr ? m_presentGsrProg : m_presentPlainProg;
	glUseProgram(prog);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_vbTex);
	glUniform1i(m_vbGsr ? m_presentGsrTex : m_presentPlainTex, 0);
	if (m_vbGsr && m_presentGsrInfo >= 0)
		glUniform4f(m_presentGsrInfo, 1.0f / (float)m_vbRW, 1.0f / (float)m_vbRH, (float)m_vbRW, (float)m_vbRH);
	glBindVertexArray(m_presentVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	// Whatever draws next starts from a clean slate of cached state.
	m_lastProgram = 0;
	m_haveFixedStateKey = false;
	m_haveLastVAOKey = false;
	m_lastBoundTex[0] = m_lastBoundTex[1] = ~0u;
}

bool WebGLPipeline::upscaleSceneNow()
{
	if (!m_ctxReady || !m_vbActive || m_vbUpscaled)
		return false;
	// A scene in this frame: the next frames render it below the game's resolution again.
	m_vbSceneSeen = true;
	if (m_curFBO != m_vbFBO)
		return false;
	// Only worth it, and only correct, when the scene was rendered below the game's resolution and
	// the game's resolution is the window's: the interface then draws into the window 1:1.
	if ((m_vbRW == m_vbW && m_vbRH == m_vbH) || m_winW != m_vbW || m_winH != m_vbH)
		return false;
	stretchVirtualBackbuffer();
	// The window's depth and stencil are not the scene's; the interface starts from cleared ones.
	glDepthMask(GL_TRUE);
	glStencilMask(0xFFFFFFFF);
	glClearDepthf(1.0f);
	glClearStencil(0);
	glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	static bool s_logged = false;
	if (!s_logged) {
		s_logged = true;
		fprintf(stderr, "[d3d8gles] scene upscaled %dx%d -> %dx%d before the interface; interface at full resolution\n",
			m_vbRW, m_vbRH, m_winW, m_winH);
	}
	m_vbUpscaled = true;
	m_curFBO = 0;
	m_curRTWidth = m_winW;
	m_curRTHeight = m_winH;
	return true;
}

extern "C" int d3d8gles_UpscaleSceneNow()
{
	if (!s_gxGlesReady)
		return 0;
	return WebGLPipeline::get()->upscaleSceneNow() ? 1 : 0;
}

// GeneralsX @bugfix Android port 01/10/2026 The clocks and temperature for [GX-PERF-THERMAL] are read
// on a thread of their own. They used to be read inside the perf report, on the engine's thread, every
// 2000 ms: a dozen cpufreq files and up to 64 thermal zones, some of which are sensors behind a slow
// bus that take milliseconds each to answer. The owner saw a micro-stutter about every two seconds
// at a high frame rate (after logs-47); the report's own cost is now printed too ("report took").
static std::mutex s_thermalMutex;
static std::string s_thermalLine;
static std::atomic<bool> s_thermalStarted{false};

static std::string ReadThermalLine()
{
	const auto t0 = std::chrono::steady_clock::now();
	char line[512];
	int len = snprintf(line, sizeof(line), "[GX-PERF-THERMAL] cpu MHz:");
	for (int cpu = 0; cpu < 12; cpu++) {
		char path[96];
		snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
		FILE *ff = fopen(path, "r");
		if (!ff)
			continue;
		long khz = 0;
		if (fscanf(ff, "%ld", &khz) == 1 && len < (int)sizeof(line) - 16)
			len += snprintf(line + len, sizeof(line) - len, " %ld", khz / 1000);
		fclose(ff);
	}
	long hottest = -1;
	for (int zone = 0; zone < 64; zone++) {
		char path[96];
		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", zone);
		FILE *ff = fopen(path, "r");
		if (!ff)
			continue;
		long t = 0;
		if (fscanf(ff, "%ld", &t) == 1) {
			if (t > 1000) t /= 1000; // millidegrees on most kernels
			if (t > hottest && t < 150) hottest = t;
		}
		fclose(ff);
	}
	const double readMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (hottest >= 0)
		snprintf(line + len, sizeof(line) - len, " | hottest zone %ld C (read in %.1f ms, off the game's thread)\n", hottest, readMs);
	else
		snprintf(line + len, sizeof(line) - len, " | temperature unreadable (read in %.1f ms)\n", readMs);
	return line;
}

static std::string ThermalLine()
{
	if (!s_thermalStarted.exchange(true)) {
		std::thread([] {
			setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
			for (;;) {
				std::string l = ReadThermalLine();
				{
					std::lock_guard<std::mutex> lock(s_thermalMutex);
					s_thermalLine.swap(l);
				}
				std::this_thread::sleep_for(std::chrono::seconds(2));
			}
		}).detach();
	}
	std::lock_guard<std::mutex> lock(s_thermalMutex);
	return s_thermalLine;
}

extern "C" int d3d8gles_SetDrawCategory(int category)
{
	const int prev = s_gxDrawCategory;
	s_gxDrawCategory = (category >= 0 && category < GX_DRAWCAT_COUNT) ? category : GX_DRAWCAT_OTHER;
	d3d8gles_curDrawCategory = s_gxDrawCategory;
	return prev;
}

void WebGLPipeline::drawCommon(WebGLDevice *dev, unsigned primType, unsigned primCount,
                               GLuint vbo, unsigned stride, unsigned fvf,
                               GLuint ibo, unsigned indexFormat,
                               unsigned startIndex, int baseVertexBytes, unsigned /*vertexCount*/,
                               int baseVertexIndex)
{
	GxDrawTimer drawTimer;
	FVFLayout l;
	if (!parseFVF(fvf, &l)) {
		WARN_ONCE(s_fvf, "unsupported FVF 0x%x", fvf);
		return;
	}
	if (stride == 0) stride = l.stride;

	ProgramInfo *prog = getProgram(dev, fvf);
	if (!prog || !prog->prog) return;

	applyFixedState(dev);
	applyUniforms(dev, prog, fvf);
	bindTextures(dev, prog);

	// GeneralsX @performance Android port 27/09/2026 D3D8's base vertex index (SetIndices'
	// second argument) is what glDrawElementsBaseVertex takes. Without it the offset has to be
	// baked into the attribute pointers, and the engine's shared dynamic vertex pool moves it on
	// most draws -- a real device log showed 26-60% of all draws re-issuing every
	// glVertexAttribPointer for that alone (the perf line's "ptr-refresh").
	const bool useBaseVertex = indexFormat != 0 && baseVertexIndex > 0 && m_glDrawElementsBaseVertex != nullptr;
	bindVertexLayout(l, vbo, ibo, fvf, stride, useBaseVertex ? 0 : baseVertexBytes);

	const GLenum mode = primModeGL(primType);
	const unsigned count = primVertexCount(primType, primCount);

	const double drawCallStart = gxNowUs();
	if (indexFormat != 0) {
		const GLenum itype = (indexFormat == D3DFMT_INDEX32) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
		const unsigned isize = (indexFormat == D3DFMT_INDEX32) ? 4 : 2;
		if (useBaseVertex) {
			{
				PFN_DrawElementsBaseVertex drawBV = m_glDrawElementsBaseVertex;
				const void *indices = (const void *)(intptr_t)(startIndex * isize);
				gxrt::post([drawBV, mode, count, itype, indices, baseVertexIndex] {
					gxrt::WorkTimer t(gxrt::kWorkDraw);
					drawBV(mode, count, itype, indices, baseVertexIndex);
				});
			}
			m_perfBaseVertexDraws++;
		} else {
			glDrawElements(mode, count, itype, (const void *)(intptr_t)(startIndex * isize));
		}
	} else {
		glDrawArrays(mode, startIndex, count);
	}
	const double drawCallUs = gxNowUs() - drawCallStart;
	s_gxDrawCallUs += drawCallUs;
	s_gxDrawCallUsByCategory[s_gxDrawCategory] += drawCallUs;
	s_gxTotalGlDrawUs += drawCallUs;
	++s_gxTotalDraws;
	if (s_gxDrawAfterWrite) {
		s_gxDrawCallUsAfterWrite += drawCallUs;
		++s_gxDrawsAfterWrite;
	}
	s_gxDrawAfterWrite = false;
	// GeneralsX @build Android port 09/05/2026 Dump the fixed-function state
	// actually in effect for the first terrain draw and the first model draw of
	// each log window. Every remaining theory for the black geometry on a
	// Low-detail start predicts a specific difference in these values between a
	// Low run and a High run, and NO counter in the build reports any of them --
	// which is why two logs of the same scene look identical everywhere the
	// fault has to live. Read from the device-side state, one line per category
	// per window, so it costs nothing.
	m_perfDrawsThisFrame++;
	s_gxDrawsByCategory[s_gxDrawCategory]++;
}

// Buffer objects (device-side shadow -> GL) helpers.
void WebGLPipeline::invalidateBufferBinding(GLuint name)
{
	if (name == 0) return; // 0 already means "no buffer" to the cache, never a real object
	if (m_lastArrayBuffer == name) m_lastArrayBuffer = ~0u;
	evictVAOsForBuffer(name);
}

void WebGLPipeline::bindArrayBuffer(GLuint name)
{
	if (m_lastArrayBuffer == name) return;
	glBindBuffer(GL_ARRAY_BUFFER, name);
	m_lastArrayBuffer = name;
}

// GeneralsX @build Android port GLES experiment - perf pass. GL_COPY_WRITE_BUFFER
// (GLES3 core) is a generic bind point no VAO or vertex-attrib state is ever
// defined in terms of, unlike GL_ARRAY_BUFFER/GL_ELEMENT_ARRAY_BUFFER --
// using it here for the actual data upload means a dirty VB/IB's glBufferData
// can never disturb whatever VAO bindVertexLayout() left bound from the
// previous draw (see that function's comment for why GL_ELEMENT_ARRAY_BUFFER
// specifically would be unsafe to touch mid-VAO otherwise).
// GeneralsX @perf Android port 09/05/2026 This used to respecify the ENTIRE
// buffer with glBufferData on every update. For the engine's shared dynamic VB
// (DEFAULT_VB_SIZE = 5000 vertices, ~220 KB at dynamic_fvf_type's 44
// bytes/vertex) that meant a full 220 KB re-upload for callers that wrote only
// a handful of quads into it -- Render2DClass::Render() does exactly that, and
// real-device timings measured it at 16-59 ms/frame across just 24-60 UI draws
// (~0.5-1 ms per draw, about half the frame). SortingRendererClass::Flush
// streams particles through the same buffer.
//
// Now: allocate storage once, then push only the byte range Lock/Unlock
// actually recorded (GLBufferState::markRange). Deliberately NOT orphaning
// (glBufferData with nullptr) before each glBufferSubData: this is a
// ring-style dynamic buffer where earlier sub-ranges belong to draws already
// submitted but not necessarily consumed yet, and orphaning would discard
// them. That matches D3D8's own NOOVERWRITE semantics for this usage.
// GeneralsX @performance Android port 29/09/2026 Upload cost split by kind, for the
// [d3d8gles] perf-upload line: the driver's upload entry points took 13-14 ms per frame in a
// heavy battle on the old Mali phone (perf-cpu "uploads"), with no way to tell which of these it
// was.
enum GxUploadKind { GX_UPLOAD_VB_FULL, GX_UPLOAD_VB_APPEND, GX_UPLOAD_IB_FULL, GX_UPLOAD_IB_APPEND, GX_UPLOAD_RING, GX_UPLOAD_KIND_COUNT };
static double s_gxUploadUs[GX_UPLOAD_KIND_COUNT] = {};
static double s_gxUploadBytes[GX_UPLOAD_KIND_COUNT] = {};
static unsigned s_gxUploadCount[GX_UPLOAD_KIND_COUNT] = {};
namespace {
struct GxUploadTimer
{
	int kind;
	double start;
	GxUploadTimer(int k, size_t bytes) : kind(k), start(gxNowUs())
	{
		s_gxUploadBytes[k] += (double)bytes;
		++s_gxUploadCount[k];
	}
	~GxUploadTimer() { s_gxUploadUs[kind] += gxNowUs() - start; }
};
}

// GeneralsX @performance Android port 29/09/2026 A full upload (first use, D3DLOCK_DISCARD, or
// no recorded range) respecifies the storage at its full size but only sends the bytes the engine
// has ever written. It used to send the whole buffer, and the engine's dynamic buffers are sized
// for the worst case -- the sorted-translucency pool is allocated for its maximum vertex count --
// while a frame writes a fraction of that, and DISCARD comes at least once a frame (the shadow
// and sorting code force it on purpose). The bytes kept are everything a later draw can
// reference, older ranges included, which is what the DISCARD handling above relies on.
void WebGLPipeline::fullBufferUpload(GLBufferState &gl, const unsigned char *bits, size_t size, int kind)
{
	const size_t valid = (gl.writtenEnd > 0 && gl.writtenEnd < size) ? gl.writtenEnd : size;
	const GxUploadTimer uploadTimer(kind, valid);
	if (valid == size) {
		glBufferData(GL_COPY_WRITE_BUFFER, size, bits, GL_DYNAMIC_DRAW);
	} else {
		glBufferData(GL_COPY_WRITE_BUFFER, size, nullptr, GL_DYNAMIC_DRAW);
		glBufferSubData(GL_COPY_WRITE_BUFFER, 0, valid, bits);
	}
	gl.allocated = true;
}


// GeneralsX @performance Android port 29/09/2026 Dynamic VB/IB updates without GL calls. In a heavy
// battle on the old Mali phone the engine's dynamic buffers took 400-600 map/unmap pairs per frame
// (skinned meshes, shadow volumes, the sorted pool, the UI), ~15 us each, 9-11 ms per frame
// ([d3d8gles] perf-upload vb-append/ib-append). With EXT_buffer_storage each copy of the buffer is
// mapped once, persistently and coherently, and an append is a memcpy into it: the D3D contract
// behind NOOVERWRITE (never touch bytes the GPU may still read) is exactly what makes writing
// into memory the GPU is using safe, and coherence makes the bytes visible to the draw issued
// next. DISCARD cannot respecify immutable storage, so it moves to another copy instead -- one
// whose fence says the GPU is done with it, or a new one (up to kMaxCopies), waiting only if all
// are busy -- and refills it with every byte ever written, the same content a full upload sends.
bool WebGLPipeline::persistentUpload(GLBufferState &gl, const unsigned char *bits, size_t size, bool isIndex)
{
	PersistentBufferSet *ps = gl.persistent;
	if (ps == nullptr) {
		// Only a buffer that has never been uploaded starts on this path; one that has stays
		// where it is. Once on it, it stays on it: its storage is immutable and cannot take an
		// ordinary glBufferData.
		if (!m_persistentOK || size == 0 || gl.name != 0)
			return false;
		ps = new PersistentBufferSet;
		ps->size = size;
		gl.persistent = ps;
	}

	// GeneralsX @bugfix Android port 29/09/2026 Reported with the first persistent build: UI,
	// buildings, units and effects flickering. Mobile GPUs are tile-based and run a frame's draws
	// after the frame is submitted, so a draw issued earlier reads the buffer as it is at the END
	// of the frame. The old path went through glMapBufferRange, where the driver quietly
	// preserves what earlier draws will read (copy-on-write); a persistent mapping has no such
	// protection, and any write to bytes an already-issued draw may read showed up in that draw.
	// NOOVERWRITE appends past everything drawn are safe; anything else that lands below the
	// highest byte an issued draw may read (gpuRefEnd, recorded by the draw calls) now moves to a
	// free copy first -- the copy-on-write the driver used to do.
	const bool haveRange = gl.dirtyBegin < gl.dirtyEnd;
	const bool hazard = haveRange && gl.dirtyBegin < gl.gpuRefEnd;
	const bool full = !gl.allocated || !haveRange || gl.pendingDiscard || hazard;
	const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;

	if (full) {
		const GxUploadTimer uploadTimer(isIndex ? GX_UPLOAD_IB_FULL : GX_UPLOAD_VB_FULL,
			(gl.writtenEnd > 0 && gl.writtenEnd < size) ? gl.writtenEnd : size);
		int next = -1;
		if (ps->cur >= 0) {
			// The draws that used the current copy are all issued: fence it.
			if (ps->fences[ps->cur])
				gxrt::deleteSync(ps->fences[ps->cur]);
			ps->fences[ps->cur] = gxrt::fenceSync();
			m_perfPersistentSwitches++;
			for (int k = 1; k <= ps->count && next < 0; k++) {
				const int i = (ps->cur + k) % ps->count;
				if (i == ps->cur)
					continue;
				if (ps->fences[i] == nullptr) {
					next = i;
				} else {
					const GLenum r = gxrt::clientWaitSync(ps->fences[i], 0, 0);
					if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) {
						gxrt::deleteSync(ps->fences[i]);
						ps->fences[i] = nullptr;
						next = i;
					}
				}
			}
		}
		if (next < 0 && m_persistentOK && ps->count < PersistentBufferSet::kMaxCopies) {
			// Every existing copy is busy (or there is none): make another.
			const int i = ps->count;
			glGenBuffers(1, &ps->names[i]);
			glBindBuffer(GL_COPY_WRITE_BUFFER, ps->names[i]);
			{
				PFN_BufferStorage storage = m_glBufferStorage;
				gxrt::post([storage, size, flags] { storage(GL_COPY_WRITE_BUFFER, (GLsizeiptr)size, nullptr, flags); });
			}
			ps->ptrs[i] = static_cast<unsigned char *>(glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, (GLsizeiptr)size, flags));
			if (ps->ptrs[i] == nullptr) {
				// The driver refused: give up on the persistent path for good, cleanly.
				glDeleteBuffers(1, &ps->names[i]);
				ps->names[i] = 0;
				fprintf(stderr, "[d3d8gles] persistent mapping refused by the driver; using ordinary uploads\n");
				m_persistentOK = false;
				if (ps->count == 0) {
					delete ps;
					gl.persistent = nullptr;
					return false;
				}
			} else {
				ps->count++;
				m_perfPersistentCopies++;
				next = i;
			}
		}
		if (next < 0) {
			// All copies busy and no room for another: wait for the oldest one.
			next = (ps->cur + 1) % ps->count;
			if (ps->fences[next]) {
				gxrt::clientWaitSync(ps->fences[next], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
				gxrt::deleteSync(ps->fences[next]);
				ps->fences[next] = nullptr;
			}
			m_perfPersistentWaits++;
		}
		ps->cur = next;
		const size_t valid = (gl.writtenEnd > 0 && gl.writtenEnd < size) ? gl.writtenEnd : size;
		// GeneralsX @bugfix Android port 30/09/2026 Refilled from the CPU. Copying the unchanged
		// part from the previous copy on the GPU (glCopyBufferSubData) broke the UI on the Mali
		// test phone -- quads from stale vertices, a giant logo, white bars (logs-37) -- and cost
		// frames besides: that driver does not keep a persistently mapped buffer and a GPU write
		// into it consistent. The bytes travel inline in the command ring (see writeMapped).
		gxrt::writeMapped(ps->ptrs[next], bits, valid);
		gl.name = ps->names[next];
		gl.allocated = true;
		gl.gpuRefEnd = 0; // nothing has been drawn from this copy since it was selected
	} else {
		const GxUploadTimer uploadTimer(isIndex ? GX_UPLOAD_IB_APPEND : GX_UPLOAD_VB_APPEND, gl.dirtyEnd - gl.dirtyBegin);
		gxrt::writeMapped(ps->ptrs[ps->cur] + gl.dirtyBegin, bits + gl.dirtyBegin, gl.dirtyEnd - gl.dirtyBegin);
	}
	gl.dirty = false;
	gl.pendingDiscard = false;
	gl.pendingSync = false;
	gl.clearRange();
	return true;
}

void WebGLPipeline::releaseBufferStorage(GLBufferState &gl)
{
	if (gl.persistent != nullptr) {
		PersistentBufferSet *ps = gl.persistent;
		for (int i = 0; i < ps->count; i++) {
			if (ps->fences[i] && m_glDeleteSync)
				gxrt::deleteSync(ps->fences[i]);
			glBindBuffer(GL_COPY_WRITE_BUFFER, ps->names[i]);
			glUnmapBuffer(GL_COPY_WRITE_BUFFER);
			glDeleteBuffers(1, &ps->names[i]);
			invalidateBufferBinding(ps->names[i]);
		}
		delete ps;
		gl.persistent = nullptr;
		gl.name = 0;
		return;
	}
	if (gl.name) {
		glDeleteBuffers(1, &gl.name);
		invalidateBufferBinding(gl.name);
		gl.name = 0;
	}
}

void WebGLPipeline::ensureVBUploaded(WebGLVertexBuffer *vb)
{
	if (vb->m_gl.dirty && (vb->m_usage & D3DUSAGE_DYNAMIC) &&
	    persistentUpload(vb->m_gl, vb->m_bits.data(), vb->m_bits.size(), false))
		return;
	if (vb->m_gl.name == 0) {
		glGenBuffers(1, &vb->m_gl.name);
		vb->m_gl.allocated = false;
	}
	if (vb->m_gl.dirty) {
		glBindBuffer(GL_COPY_WRITE_BUFFER, vb->m_gl.name);
		const bool haveRange = vb->m_gl.dirtyBegin < vb->m_gl.dirtyEnd;
		// GeneralsX @bugfix Android port 09/05/2026 D3DLOCK_DISCARD does a FULL
		// upload rather than orphaning with glBufferData(..., nullptr) and then
		// pushing only the fresh range. Orphaning leaves every byte outside
		// that range undefined, and the engine does not treat the ring's older
		// contents as dead: SortingRendererClass draws sorted translucent
		// geometry that can still reference vertex ranges written earlier in
		// the frame. Those ranges survive in m_bits but not in the newly
		// orphaned GL storage, so they rasterized as garbage -- confirmed on a
		// real device as stray grids and long diagonal lines across the scene.
		// glBufferData WITH data respecifies the storage too (so it does not
		// wait on the GPU either), it just also refills it. This only happens
		// when the ring wraps, so the ordinary NOOVERWRITE appends below stay
		// cheap.
		const bool fullUpload = !vb->m_gl.allocated || !haveRange || vb->m_gl.pendingDiscard;
		if (fullUpload) {
			// First use, or an update with no recorded range: full upload,
			// which also (re)allocates the GL storage.
			fullBufferUpload(vb->m_gl, vb->m_bits.data(), vb->m_bits.size(), GX_UPLOAD_VB_FULL);
		} else {
			const GxUploadTimer uploadTimer(GX_UPLOAD_VB_APPEND, vb->m_gl.dirtyEnd - vb->m_gl.dirtyBegin);
			// GeneralsX @perf Android port 09/05/2026 This is the
			// D3DLOCK_NOOVERWRITE case (the engine's ring buffer appending
			// past offset 0). glBufferSubData here makes the driver
			// synchronize against the GPU still reading earlier ranges of the
			// same buffer, and real-device timings showed exactly that: a flat
			// ~1 ms per Render2DClass::Render() call, tracking UI draw count
			// 1:1 (50.6 draws -> 49.3 ms, 65.2 -> 62.5) and independent of how
			// much data was written -- the signature of waiting, not working.
			// GL_MAP_UNSYNCHRONIZED_BIT is the exact translation of what
			// NOOVERWRITE promises: the caller guarantees it is not touching
			// bytes the GPU may still read, so no wait is needed.
			const GLintptr off = (GLintptr)vb->m_gl.dirtyBegin;
			const GLsizeiptr len = (GLsizeiptr)(vb->m_gl.dirtyEnd - vb->m_gl.dirtyBegin);
			if (vb->m_gl.pendingSync) {
				// A plain lock (no NOOVERWRITE): synchronized, as D3D would be.
				glBufferSubData(GL_COPY_WRITE_BUFFER, off, len, vb->m_bits.data() + vb->m_gl.dirtyBegin);
				m_perfSyncUploads++;
			} else {
				// Map, copy and unmap on the GL thread (falling back to glBufferSubData when the
				// driver refuses the mapping): see gxrt::bufferWrite.
				gxrt::bufferWrite(GL_COPY_WRITE_BUFFER, off, len, vb->m_bits.data() + vb->m_gl.dirtyBegin,
					GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
			}
		}
		vb->m_gl.dirty = false;
		vb->m_gl.pendingDiscard = false;
		vb->m_gl.pendingSync = false;
		vb->m_gl.clearRange();
	}
}

// GeneralsX @perf Android port 09/05/2026 Same range-only upload as
// ensureVBUploaded above -- see that comment for the reasoning and the
// measurements.
void WebGLPipeline::ensureIBUploaded(WebGLIndexBuffer *ib)
{
	if (m_opt.persistentIB && ib->m_gl.dirty && (ib->m_usage & D3DUSAGE_DYNAMIC) &&
	    persistentUpload(ib->m_gl, ib->m_bits.data(), ib->m_bits.size(), true))
		return;
	if (ib->m_gl.name == 0) {
		glGenBuffers(1, &ib->m_gl.name);
		ib->m_gl.allocated = false;
	}
	if (ib->m_gl.dirty) {
		glBindBuffer(GL_COPY_WRITE_BUFFER, ib->m_gl.name);
		const bool haveRange = ib->m_gl.dirtyBegin < ib->m_gl.dirtyEnd;
		// GeneralsX @bugfix Android port 09/05/2026 D3DLOCK_DISCARD does a FULL
		// upload rather than orphaning with glBufferData(..., nullptr) and then
		// pushing only the fresh range. Orphaning leaves every byte outside
		// that range undefined, and the engine does not treat the ring's older
		// contents as dead: SortingRendererClass draws sorted translucent
		// geometry that can still reference vertex ranges written earlier in
		// the frame. Those ranges survive in m_bits but not in the newly
		// orphaned GL storage, so they rasterized as garbage -- confirmed on a
		// real device as stray grids and long diagonal lines across the scene.
		// glBufferData WITH data respecifies the storage too (so it does not
		// wait on the GPU either), it just also refills it. This only happens
		// when the ring wraps, so the ordinary NOOVERWRITE appends below stay
		// cheap.
		const bool fullUpload = !ib->m_gl.allocated || !haveRange || ib->m_gl.pendingDiscard;
		if (fullUpload) {
			fullBufferUpload(ib->m_gl, ib->m_bits.data(), ib->m_bits.size(), GX_UPLOAD_IB_FULL);
		} else {
			const GxUploadTimer uploadTimer(GX_UPLOAD_IB_APPEND, ib->m_gl.dirtyEnd - ib->m_gl.dirtyBegin);
			// GeneralsX @perf Android port 09/05/2026 This is the
			// D3DLOCK_NOOVERWRITE case (the engine's ring buffer appending
			// past offset 0). glBufferSubData here makes the driver
			// synchronize against the GPU still reading earlier ranges of the
			// same buffer, and real-device timings showed exactly that: a flat
			// ~1 ms per Render2DClass::Render() call, tracking UI draw count
			// 1:1 (50.6 draws -> 49.3 ms, 65.2 -> 62.5) and independent of how
			// much data was written -- the signature of waiting, not working.
			// GL_MAP_UNSYNCHRONIZED_BIT is the exact translation of what
			// NOOVERWRITE promises: the caller guarantees it is not touching
			// bytes the GPU may still read, so no wait is needed.
			const GLintptr off = (GLintptr)ib->m_gl.dirtyBegin;
			const GLsizeiptr len = (GLsizeiptr)(ib->m_gl.dirtyEnd - ib->m_gl.dirtyBegin);
			if (ib->m_gl.pendingSync) {
				// A plain lock (no NOOVERWRITE): synchronized, as D3D would be.
				glBufferSubData(GL_COPY_WRITE_BUFFER, off, len, ib->m_bits.data() + ib->m_gl.dirtyBegin);
				m_perfSyncUploads++;
			} else {
				// Map, copy and unmap on the GL thread (falling back to glBufferSubData when the
				// driver refuses the mapping): see gxrt::bufferWrite.
				gxrt::bufferWrite(GL_COPY_WRITE_BUFFER, off, len, ib->m_bits.data() + ib->m_gl.dirtyBegin,
					GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
			}
		}
		ib->m_gl.dirty = false;
		ib->m_gl.pendingDiscard = false;
		ib->m_gl.pendingSync = false;
		ib->m_gl.clearRange();
	}
}

// GeneralsX @build Android port GLES experiment - perf pass. The *UP draw
// paths (drawUP/drawIndexedUP below) reuse one fixed streaming buffer
// (m_upVBO/m_upIBO) across every call, uploading fresh data every time --
// unlike the dirty-gated VB/IB path above, there is no "skip the upload"
// option here, the data really is new every call. A plain glBufferData with
// new contents into the SAME buffer object risks the driver having to stall
// the CPU until the GPU finishes consuming whatever THIS buffer held for the
// previous draw (which may still be in flight) before it can safely
// overwrite it. Explicitly orphaning first -- glBufferData with the same
// target/size and a null data pointer, requesting a fresh anonymous
// allocation with no dependency on the old one -- is the standard,
// driver-portable way to ask for a new backing allocation instead of
// waiting; mobile GL drivers are the ones most likely to need this spelled
// out rather than inferring it from the "same size, new data" pattern alone.
// One-off upload for a *UP draw that does not fit the streaming ring (or with the ring
// switched off). glBufferData with the data respecifies the storage by itself; the extra
// nullptr respecification that used to precede it only cost a second allocation per draw.
static void orphanAndUpload(GLenum target, GLuint buffer, size_t size, const void *data, GLenum usage)
{
	glBindBuffer(target, buffer);
	glBufferData(target, size, data, usage);
}

// GeneralsX @performance Android port 27/09/2026 Append to a streaming ring, the D3D "dynamic
// buffer" pattern ToGL and WineD3D use for DrawPrimitiveUP. An unsynchronized map is safe
// because a *UP draw's data is used by that draw only (the engine hands over a pointer per
// call and never refers back to it), so bytes behind the write position are dead for every
// later draw, and on wrap the storage is orphaned: draws still in flight keep the old block.
size_t WebGLPipeline::streamToRing(GLuint buffer, size_t capacity, size_t *offset,
                                   const void *data, size_t bytes, size_t align)
{
	if (bytes == 0 || bytes > capacity || align == 0)
		return (size_t)-1;
	size_t start = (*offset + align - 1) / align * align;
	glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
	if (start + bytes > capacity) {
		glBufferData(GL_COPY_WRITE_BUFFER, capacity, nullptr, GL_STREAM_DRAW);
		start = 0;
		m_perfUpRingWraps++;
	}
	const GxUploadTimer uploadTimer(GX_UPLOAD_RING, bytes);
	gxrt::bufferWrite(GL_COPY_WRITE_BUFFER, (GLintptr)start, (GLsizeiptr)bytes, data,
		GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
	*offset = start + bytes;
	m_perfUpRingBytes += (double)bytes;
	return start;
}

// GeneralsX @performance Android port 29/09/2026 Dynamic index data without per-draw GL calls,
// and without ever reusing a byte. Writing the engine's dynamic index buffers through a persistent
// mapping flickered on Mali (every dynamic draw; static-buffer terrain was fine; Adreno was fine):
// glDrawElements carries no index range, so the driver scans the indices for min/max and caches it
// per buffer range until a GL call modifies the buffer -- and the engine refills the same offsets
// every frame, so a memcpy left the cached range stale. Putting them back on map/unmap cured it
// and cost 3-6 ms per frame (~20 us per call). Here each draw's indices go to the next unused
// bytes of a stream buffer, so no (buffer, offset) pair is ever used with two different contents
// and there is nothing for a cache to get wrong. A full stream is replaced by a NEW buffer object
// (the old one is deleted; GL keeps its storage alive for the draws still in flight), so there
// is no fence and no wait either.
bool WebGLPipeline::streamIndices(const void *src, size_t bytes, GLuint *name, size_t *offset)
{
	if (m_indexStreamFailed || !m_persistentOK || !m_opt.persistent || bytes == 0 || bytes > kIndexStreamBytes)
		return false;
	size_t start = (m_indexStreamOffset + 3) & ~(size_t)3; // 4-byte aligned: fits 16- and 32-bit indices
	if (m_indexStream == 0 || start + bytes > kIndexStreamBytes) {
		if (m_indexStream != 0) {
			glBindBuffer(GL_COPY_WRITE_BUFFER, m_indexStream);
			glUnmapBuffer(GL_COPY_WRITE_BUFFER);
			const GLuint old = m_indexStream;
			glDeleteBuffers(1, &m_indexStream);
			invalidateBufferBinding(old);
			m_indexStream = 0;
			m_indexStreamPtr = nullptr;
			m_perfIndexStreamRenewals++;
		}
		const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
		glGenBuffers(1, &m_indexStream);
		glBindBuffer(GL_COPY_WRITE_BUFFER, m_indexStream);
		{
			PFN_BufferStorage storage = m_glBufferStorage;
			gxrt::post([storage, flags] { storage(GL_COPY_WRITE_BUFFER, (GLsizeiptr)kIndexStreamBytes, nullptr, flags); });
		}
		m_indexStreamPtr = static_cast<unsigned char *>(
			glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, (GLsizeiptr)kIndexStreamBytes, flags));
		if (m_indexStreamPtr == nullptr) {
			glDeleteBuffers(1, &m_indexStream);
			m_indexStream = 0;
			m_indexStreamFailed = true;
			fprintf(stderr, "[d3d8gles] index stream: persistent mapping refused; dynamic index buffers use ordinary uploads\n");
			return false;
		}
		start = 0;
	}
	gxrt::writeMapped(m_indexStreamPtr + start, src, bytes);
	m_indexStreamOffset = start + bytes;
	*name = m_indexStream;
	*offset = start;
	return true;
}

void WebGLPipeline::drawIndexed(WebGLDevice *dev, unsigned primType, unsigned minIndex,
                                unsigned numVertices, unsigned startIndex, unsigned primCount)
{
	if (!m_ctxReady) return;
	WebGLVertexBuffer *vb = dev->getStream0();
	WebGLIndexBuffer *ib = dev->getIndices();
	if (!vb || !ib) return;

	s_gxDrawAfterWrite = vb->m_gl.dirty || ib->m_gl.dirty;
	ensureVBUploaded(vb);

	// Dynamic index buffers go through the index stream (streamIndices) when it is available;
	// their own GL storage is then never uploaded, and stays fully dirty in case the stream
	// ever falls back.
	const size_t isz = (ib->m_format == D3DFMT_INDEX32) ? 4 : 2;
	const size_t indexBytes = (size_t)primVertexCount(primType, primCount) * isz;
	GLuint iboName = 0;
	unsigned drawStartIndex = startIndex;
	bool streamed = false;
	if ((ib->m_usage & D3DUSAGE_DYNAMIC) && (size_t)startIndex * isz + indexBytes <= ib->m_bits.size()) {
		const GxUploadTimer uploadTimer(GX_UPLOAD_RING, indexBytes);
		size_t streamOffset = 0;
		streamed = streamIndices(ib->m_bits.data() + (size_t)startIndex * isz, indexBytes, &iboName, &streamOffset);
		if (streamed)
			drawStartIndex = (unsigned)(streamOffset / isz);
	}
	if (!streamed) {
		ensureIBUploaded(ib);
		iboName = ib->m_gl.name;
	}

	const unsigned fvf = dev->getFVF() ? dev->getFVF() : vb->m_fvf;
	const unsigned stride = dev->getStream0Stride();
	const int baseBytes = (int)(dev->getBaseVertexIndex() * stride);
	// What this draw may read, for the persistent path's hazard check. D3D8's minIndex/numVertices
	// are the vertex range the indices address; without a count, assume the whole buffer.
	size_t vertexEnd = numVertices > 0 ? (size_t)minIndex + numVertices : (size_t)-1;
	// GeneralsX @bugfix Android port 30/09/2026 ...but D3D itself never enforces that range, so an
	// engine call that understates it draws fine there and here leaves gpuRefEnd short: a later
	// NOOVERWRITE write into the understated tail passes the hazard check and overwrites vertices
	// a queued draw still reads -- a triangle stretched across the screen. With the render thread
	// the draw runs later than it used to, so such a write lands first more often. For a
	// persistent VB, read the real highest index (the indices are right here in m_bits).
	if (vb->m_gl.persistent != nullptr && vertexEnd != (size_t)-1 &&
	    (size_t)startIndex * isz + indexBytes <= ib->m_bits.size()) {
		size_t maxIndex = 0;
		const unsigned char *idx = ib->m_bits.data() + (size_t)startIndex * isz;
		const size_t count = indexBytes / isz;
		if (isz == 2) {
			const uint16_t *p16 = reinterpret_cast<const uint16_t *>(idx);
			for (size_t i = 0; i < count; i++)
				if (p16[i] > maxIndex) maxIndex = p16[i];
		} else {
			const uint32_t *p32 = reinterpret_cast<const uint32_t *>(idx);
			for (size_t i = 0; i < count; i++)
				if (p32[i] > maxIndex) maxIndex = p32[i];
		}
		if (maxIndex + 1 > vertexEnd) {
			vertexEnd = maxIndex + 1;
			m_perfRangeUnderstated++;
		}
	}
	vb->m_gl.noteGpuRead(vertexEnd != (size_t)-1
		? ((size_t)dev->getBaseVertexIndex() + vertexEnd) * stride
		: vb->m_bits.size(), vb->m_bits.size());
	if (!streamed)
		ib->m_gl.noteGpuRead((size_t)startIndex * isz + indexBytes, ib->m_bits.size());
	drawCommon(dev, primType, primCount, vb->m_gl.name, stride, fvf,
	           iboName, ib->m_format, drawStartIndex, baseBytes, numVertices,
	           (int)dev->getBaseVertexIndex());
}

void WebGLPipeline::draw(WebGLDevice *dev, unsigned primType, unsigned startVertex, unsigned primCount)
{
	if (!m_ctxReady) return;
	WebGLVertexBuffer *vb = dev->getStream0();
	if (!vb) return;

	s_gxDrawAfterWrite = vb->m_gl.dirty;
	ensureVBUploaded(vb);

	const unsigned fvf = dev->getFVF() ? dev->getFVF() : vb->m_fvf;
	const unsigned stride = dev->getStream0Stride();
	vb->m_gl.noteGpuRead(((size_t)startVertex + primVertexCount(primType, primCount)) * stride, vb->m_bits.size());
	drawCommon(dev, primType, primCount, vb->m_gl.name, stride, fvf,
	           0, 0, startVertex, 0, 0);
}

void WebGLPipeline::drawUP(WebGLDevice *dev, unsigned primType, unsigned primCount,
                           const void *vertexData, unsigned stride)
{
	s_gxDrawAfterWrite = true; // streamed right before the draw
	if (!m_ctxReady || !vertexData) return;
	const unsigned fvf = dev->getFVF();
	FVFLayout l;
	if (!parseFVF(fvf, &l)) return;
	if (stride == 0) stride = l.stride;

	const unsigned vcount = primVertexCount(primType, primCount);
	if (m_opt.upRing && m_upRingVB) {
		const size_t offset = streamToRing(m_upRingVB, kUpRingVBBytes, &m_upRingVBOffset,
			vertexData, (size_t)vcount * stride, stride);
		if (offset != (size_t)-1) {
			m_perfUpRingDraws++;
			drawCommon(dev, primType, primCount, m_upRingVB, stride, fvf, 0, 0,
			           (unsigned)(offset / stride), 0, vcount);
			return;
		}
	}
	orphanAndUpload(GL_COPY_WRITE_BUFFER, m_upVBO, (size_t)vcount * stride, vertexData, GL_STREAM_DRAW);

	drawCommon(dev, primType, primCount, m_upVBO, stride, fvf, 0, 0, 0, 0, vcount);
}

void WebGLPipeline::drawIndexedUP(WebGLDevice *dev, unsigned primType, unsigned minVertexIdx,
                                  unsigned numVertices, unsigned primCount,
                                  const void *indexData, unsigned indexFormat,
                                  const void *vertexData, unsigned stride)
{
	s_gxDrawAfterWrite = true; // streamed right before the draw
	if (!m_ctxReady || !vertexData || !indexData) return;
	const unsigned fvf = dev->getFVF();
	FVFLayout l;
	if (!parseFVF(fvf, &l)) return;
	if (stride == 0) stride = l.stride;

	const unsigned isize = (indexFormat == D3DFMT_INDEX32) ? 4 : 2;
	const unsigned icount = primVertexCount(primType, primCount);
	if (m_opt.upRing && m_upRingVB && m_upRingIB) {
		const size_t vbytes = (size_t)(minVertexIdx + numVertices) * stride;
		const size_t ibytes = (size_t)icount * isize;
		if (vbytes <= kUpRingVBBytes && ibytes <= kUpRingIBBytes) {
			const size_t voffset = streamToRing(m_upRingVB, kUpRingVBBytes, &m_upRingVBOffset,
				vertexData, vbytes, stride);
			const size_t ioffset = streamToRing(m_upRingIB, kUpRingIBBytes, &m_upRingIBOffset,
				indexData, ibytes, isize);
			if (voffset != (size_t)-1 && ioffset != (size_t)-1) {
				m_perfUpRingDraws++;
				drawCommon(dev, primType, primCount, m_upRingVB, stride, fvf, m_upRingIB, indexFormat,
				           (unsigned)(ioffset / isize), (int)voffset, numVertices, (int)(voffset / stride));
				return;
			}
		}
	}
	orphanAndUpload(GL_COPY_WRITE_BUFFER, m_upVBO, (size_t)(minVertexIdx + numVertices) * stride,
	                vertexData, GL_STREAM_DRAW);
	orphanAndUpload(GL_COPY_WRITE_BUFFER, m_upIBO, (size_t)icount * isize, indexData, GL_STREAM_DRAW);

	drawCommon(dev, primType, primCount, m_upVBO, stride, fvf, m_upIBO, indexFormat, 0, 0, numVertices);
}

// ---------------------------------------------------------------------------
// Clear / present
// ---------------------------------------------------------------------------

void WebGLPipeline::clear(WebGLDevice *dev, unsigned flags, uint32_t argb, float z, unsigned stencil)
{
	if (!m_ctxReady) return;


	// D3D clears the viewport region only.
	const D3DVIEWPORT8 &vp = effectiveViewport(dev);
	const bool full = (vp.X == 0 && vp.Y == 0 &&
	                   (int)vp.Width == m_curRTWidth && (int)vp.Height == m_curRTHeight);
	if (!full) {
		glEnable(GL_SCISSOR_TEST);
		// Same D3D-top-to-GL-bottom Y conversion as applyFixedState's
		// glViewport call -- glScissor's y is bottom-origin in GL too.
		GLint sx, sy;
		GLsizei sw, sh;
		targetRect(vp, &sx, &sy, &sw, &sh);
		glScissor(sx, sy, sw, sh);
	}

	GLbitfield mask = 0;
	if (flags & D3DCLEAR_TARGET) {
		float c[4];
		argbToFloats(argb, c);
		// GeneralsX @bugfix Android port 09/04/2026 D3D8's Clear() alpha
		// component is meaningless on a real Windows backbuffer (no OS-level
		// compositing depends on it there), so callers have always felt free
		// to pass whatever's convenient -- e.g. W3DDisplay.cpp's main
		// per-frame Begin_Render() clear reuses TheWaterTransparency->
		// m_minWaterOpacity as this value, which has nothing to do with
		// wanting the actual screen transparent. That's harmless as long as
		// the default framebuffer has no alpha channel to write to, but
		// becomes a real hazard on Android if it ever does (SurfaceFlinger
		// can and does composite a GL-backed Surface's alpha against
		// whatever is behind it). Force full opacity on the DEFAULT
		// framebuffer specifically (m_curFBO==0, i.e. the real on-screen
		// backbuffer) regardless of whatever alpha the caller asked for --
		// offscreen render targets (m_curFBO!=0, e.g. water reflections)
		// keep using the caller's real alpha, since those aren't presented
		// directly to the OS compositor.
		// GeneralsX @bugfix Android port 02/10/2026 The virtual backbuffer is the backbuffer too:
		// cleared with the caller's alpha (m_minWaterOpacity) it left a destination alpha the window
		// never had, and every blend that reads it came out different with the upscaler on.
		if (m_curFBO == 0 || (m_vbActive && m_curFBO == m_vbFBO)) c[3] = 1.0f;
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClearColor(c[0], c[1], c[2], c[3]);
		mask |= GL_COLOR_BUFFER_BIT;
	}
	if (flags & D3DCLEAR_ZBUFFER) {
		glDepthMask(GL_TRUE);
		glClearDepthf(z);
		mask |= GL_DEPTH_BUFFER_BIT;
	}
	if (flags & D3DCLEAR_STENCIL) {
		glStencilMask(0xFFFFFFFF);
		glClearStencil((GLint)stencil);
		mask |= GL_STENCIL_BUFFER_BIT;
	}
	if (mask) glClear(mask);

	if (!full) glDisable(GL_SCISSOR_TEST);

	// GeneralsX @build Android port GLES experiment - clear() just forced
	// glColorMask/glDepthMask/glStencilMask to their clear-time values
	// (all-write) regardless of what D3D render state actually wants (e.g.
	// COLORWRITEENABLE=0 for stencil shadow volumes). applyFixedState()'s
	// redundant-state cache doesn't know that happened, so without this it
	// could see an unchanged D3DRS_* key and skip re-applying those masks,
	// leaving GL state out of sync with what the next draw actually needs.
	m_haveFixedStateKey = false;
}

void WebGLPipeline::setRenderTarget(WebGLDevice * /*dev*/, WebGLTexture *tex)
{
	if (!m_ctxReady) return;

	if (tex == nullptr) {
		glBindFramebuffer(GL_FRAMEBUFFER, backbufferFBO());
		m_curFBO = backbufferFBO();
		m_curRTWidth = m_fbWidth;
		m_curRTHeight = m_fbHeight;
		// GeneralsX @build Android port GLES experiment - this was +1.0f as
		// ported from the web build. On a browser canvas, the browser's own
		// canvas compositing absorbs part of the D3D-to-GL vertical
		// convention mismatch, so +1.0f was correct there. Android's raw
		// EGL/ANativeWindow presentation (via SDL_GL_SwapWindow) has no such
		// implicit correction, and +1.0f produced a confirmed whole-frame
		// vertical flip on a real device (menu buttons, logo, and the 3D
		// background all appeared upside down and in reversed top-to-bottom
		// order, matched against a known-correct reference screenshot).
		m_yFlip = 1.0f;
		return;
	}

	// The GL texture must exist before it can be an attachment.
	if (tex->m_gl.name == 0 || tex->m_gl.dirty) {
		uploadTexture(tex);
		// GeneralsX @build Android port GLES experiment - part 2/2 of the
		// texture-bind cache perf fix (see m_lastBoundTex's declaration for
		// part 1/2). uploadTexture() just did its own raw glBindTexture on
		// whatever unit bindTextures()'s last draw left active, behind
		// m_lastBoundTex's back -- GL_TEXTURE_BINDING_2D for that unit is now
		// this render-target texture, not whatever bindTextures() last
		// recorded. Without this, the next draw's bindTextures() could
		// wrongly conclude the intended sampler texture is "already bound"
		// (stale cache hit) and skip the real bind, sampling this RT texture
		// instead. Stomp both slots with an impossible GL name so the next
		// bindTextures() call is always forced to re-bind for real.
		m_lastBoundTex[0] = m_lastBoundTex[1] = ~0u;
	}

	const int w = (int)tex->m_levels[0]->m_width;
	const int h = (int)tex->m_levels[0]->m_height;

	if (tex->m_gl.fbo == 0) {
		glGenFramebuffers(1, &tex->m_gl.fbo);
		tex->m_gl.fboDepthGen = 0;
		tex->m_gl.fboStatus = 0;
		glBindFramebuffer(GL_FRAMEBUFFER, tex->m_gl.fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex->m_gl.name, 0);
	} else {
		glBindFramebuffer(GL_FRAMEBUFFER, tex->m_gl.fbo);
	}

	// Shared depth-stencil renderbuffer, recreated on size change.
	if (m_depthRB == 0 || m_depthRBW != w || m_depthRBH != h) {
		if (m_depthRB) glDeleteRenderbuffers(1, &m_depthRB);
		glGenRenderbuffers(1, &m_depthRB);
		glBindRenderbuffer(GL_RENDERBUFFER, m_depthRB);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
		m_depthRBGeneration++;
		m_depthRBW = w;
		m_depthRBH = h;
	}
	if (tex->m_gl.fboDepthGen != m_depthRBGeneration || tex->m_gl.fboStatus == 0) {
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_depthRB);
		tex->m_gl.fboDepthGen = m_depthRBGeneration;
		tex->m_gl.fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	}

	const GLenum status = tex->m_gl.fboStatus;
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		WARN_ONCE(s_fboIncomplete, "FBO incomplete: 0x%x (%dx%d fmt=%d)", status, w, h, (int)tex->m_format);
		glBindFramebuffer(GL_FRAMEBUFFER, backbufferFBO());
		m_curFBO = backbufferFBO();
		m_curRTWidth = m_fbWidth;
		m_curRTHeight = m_fbHeight;
		// GeneralsX @build Android port GLES experiment - this was +1.0f as
		// ported from the web build. On a browser canvas, the browser's own
		// canvas compositing absorbs part of the D3D-to-GL vertical
		// convention mismatch, so +1.0f was correct there. Android's raw
		// EGL/ANativeWindow presentation (via SDL_GL_SwapWindow) has no such
		// implicit correction, and +1.0f produced a confirmed whole-frame
		// vertical flip on a real device (menu buttons, logo, and the 3D
		// background all appeared upside down and in reversed top-to-bottom
		// order, matched against a known-correct reference screenshot).
		m_yFlip = 1.0f;
		return;
	}

	m_curFBO = tex->m_gl.fbo;
	m_curRTWidth = w;
	m_curRTHeight = h;
	// Same y-flip as the backbuffer: D3D's top row then lands in texel row 0,
	// which is exactly what engine UVs (v=0 = top) expect when sampling.
	m_yFlip = 1.0f;
	// Rendered content supersedes the CPU shadow from now on.
	tex->m_gl.dirty = false;
}

// GeneralsX @bugfix Android port 09/05/2026 See the declaration in
// gles_pipeline.h. Projected shadows (W3DProjectedShadow::updateTexture) render
// an object's silhouette into a 512x512 render target and then blit it into a
// permanent texture with SurfaceClass::Copy -> _Copy_DX8_Rects -> CopyRects.
// CopyRects is a CPU memcpy between shadow bits, and a render target's shadow
// bits were never written by anything, so the permanent shadow texture came out
// all zeroes. Drawn with _PresetMultiplicativeShader (dst *= src), an all-zero
// texture multiplies the terrain to black across the whole decal quad -- which
// is exactly the "strange" helicopter shadows reported on GLES and never on
// DXVK (DXVK's CopyRects does a real GPU copy). Only SHADOW_PROJECTION shadows
// are generated this way; ground units use artist-supplied decal textures,
// which is why the symptom looked aircraft-specific.
void WebGLPipeline::readbackRenderTarget(WebGLTexture *tex)
{
	if (!m_ctxReady || tex == nullptr || tex->m_gl.fbo == 0 || tex->m_levels.empty()) return;

	WebGLSurface *s = tex->m_levels[0];
	const int w = (int)s->m_width;
	const int h = (int)s->m_height;
	if (w <= 0 || h <= 0) return;

	// glReadPixels' guaranteed-supported combination in GLES3 is RGBA/UBYTE,
	// and the only render-target format this engine creates is A8R8G8B8
	// (DX8Wrapper::Create_Render_Target, with an X8R8G8B8 fallback).
	if (tex->m_format != D3DFMT_A8R8G8B8 && tex->m_format != D3DFMT_X8R8G8B8) {
		WARN_ONCE_ARG((unsigned)tex->m_format,
			"render-target readback for format %u not implemented",
			(unsigned)tex->m_format);
		return;
	}
	if (s->m_bits.size() < (size_t)h * s->m_pitch) return;

	// GeneralsX @performance Android port 27/09/2026 glReadPixels drains the GPU; counted and
	// timed in the perf log (perf-opt: rt-readback) to tell whether this is a per-frame cost.
	const std::chrono::steady_clock::time_point readStart = std::chrono::steady_clock::now();
	m_rtReadback.resize((size_t)w * h * 4);
	glBindFramebuffer(GL_FRAMEBUFFER, tex->m_gl.fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, m_rtReadback.data());
	glBindFramebuffer(GL_FRAMEBUFFER, m_curFBO);
	m_perfRTReadbacks++;
	m_perfRTReadbackUs += std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - readStart).count();

	// Row 0 of this FBO's texture attachment is D3D's top row (setRenderTarget
	// keeps m_yFlip at +1 precisely so it lands there), and glReadPixels
	// numbers rows from attachment row 0 upward -- so the rows come back in
	// the same order the CPU shadow stores them. No vertical flip here.
	// Bytes still need RGBA -> BGRA, the inverse of the conversion
	// prepareLevelUpload() does when pushing an A8R8G8B8 surface to GL.
	for (int y = 0; y < h; y++) {
		const uint8_t *src = m_rtReadback.data() + (size_t)y * w * 4;
		uint8_t *dst = s->m_bits.data() + (size_t)y * s->m_pitch;
		for (int x = 0; x < w; x++) {
			dst[x * 4 + 0] = src[x * 4 + 2]; // B
			dst[x * 4 + 1] = src[x * 4 + 1]; // G
			dst[x * 4 + 2] = src[x * 4 + 0]; // R
			dst[x * 4 + 3] = src[x * 4 + 3]; // A
		}
	}
}

void WebGLPipeline::debugSampleRenderTarget(WebGLTexture *tex, const char *tag)
{
	static int s_samplesLeft = 4;
	if (s_samplesLeft <= 0) return;
	if (!m_ctxReady || tex == nullptr || tex->m_gl.fbo == 0 || tex->m_levels.empty()) return;

	const int w = (int)tex->m_levels[0]->m_width;
	const int h = (int)tex->m_levels[0]->m_height;
	// Only the screen filters' target is backbuffer-sized; water reflections and
	// projected shadows are small and would drown this out.
	if (w != m_fbWidth || h != m_fbHeight || w <= 0 || h <= 0) return;
	s_samplesLeft--;

	const int bw = w < 64 ? w : 64;
	const int bh = h < 64 ? h : 64;
	const int bx = (w - bw) / 2;
	const int by = (h - bh) / 2;

	std::vector<uint8_t> px((size_t)bw * bh * 4);
	const GLuint prevFBO = m_curFBO;
	glBindFramebuffer(GL_FRAMEBUFFER, tex->m_gl.fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(bx, by, bw, bh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	const GLenum err = glGetError();
	glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);

	double sum[3] = {0.0, 0.0, 0.0};
	int mx[3] = {0, 0, 0};
	int nonBlack = 0;
	const int n = bw * bh;
	for (int i = 0; i < n; i++) {
		const uint8_t *p = px.data() + (size_t)i * 4;
		for (int c = 0; c < 3; c++) {
			sum[c] += p[c];
			if (p[c] > mx[c]) mx[c] = p[c];
		}
		if (p[0] || p[1] || p[2]) nonBlack++;
	}

	fprintf(stderr, "[GX-FILTER] rt-sample %s %dx%d block=%dx%d@%d,%d "
		"avg=(%.1f,%.1f,%.1f) max=(%d,%d,%d) nonblack=%d/%d glerr=0x%x\n",
		tag, w, h, bw, bh, bx, by,
		sum[0] / n, sum[1] / n, sum[2] / n, mx[0], mx[1], mx[2], nonBlack, n, (unsigned)err);
	fflush(stderr);
}


void WebGLPipeline::present()
{
	if (!m_ctxReady) return;
	m_frame++;


	// GeneralsX @performance Android port 27/09/2026 Asked once a second, not every frame: a
	// glGetError can make the driver flush its command queue, and only the once-a-second
	// answer was ever printed. GL keeps the error flag set until it is read.
	if ((m_frame % 60) == 1) {
		// Checked where the calls run, so it never waits for the render thread.
		const unsigned frame = m_frame;
		gxrt::post([frame] {
			const GLenum err = gxrt::rawGetError();
			if (err != GL_NO_ERROR)
				fprintf(stderr, "[d3d8gles] glGetError at frame %u: 0x%x\n", frame, err);
		});
	}

	// GeneralsX @build Android port GLES experiment - perf visibility.
	// Logged once every ~2s (not every frame, to keep this from becoming
	// its own source of overhead/spam) so real numbers -- FPS, draws/frame,
	// how often the applyFixedState redundant-state cache actually hits --
	// are available from a device log instead of judging smoothness by feel.
	m_perfFrameCount++;
	m_perfDrawAccum += m_perfDrawsThisFrame;
	m_perfDrawsThisFrame = 0;
	{
		const unsigned nowMs = SDL_GetTicks();
		if (m_perfLogLastMs == 0) {
			m_perfLogLastMs = nowMs;
		} else if (nowMs - m_perfLogLastMs >= 2000) {
			const auto reportStart = std::chrono::steady_clock::now();
			const float seconds = (nowMs - m_perfLogLastMs) / 1000.0f;
			const float fps = m_perfFrameCount / seconds;
			const float drawsPerFrame = m_perfFrameCount > 0
				? (float)m_perfDrawAccum / m_perfFrameCount : 0.0f;
			const int totalStateChecks = m_perfStateCacheHits + m_perfStateCacheMisses;
			const float cacheHitPct = totalStateChecks > 0
				? 100.0f * m_perfStateCacheHits / totalStateChecks : 0.0f;
			const int totalVAOChecks = m_perfVAOCacheHits + m_perfVAOCacheMisses;
			const float vaoHitPct = totalVAOChecks > 0
				? 100.0f * m_perfVAOCacheHits / totalVAOChecks : 0.0f;
			const int totalUniformChecks = m_perfUniformCacheHits + m_perfUniformCacheMisses;
			const float uniformHitPct = totalUniformChecks > 0
				? 100.0f * m_perfUniformCacheHits / totalUniformChecks : 0.0f;
			// GeneralsX @perf Android port 09/05/2026 Per-subsystem draw split,
			// averaged over the same window as draws/frame above.
			{
				const float f = m_perfFrameCount > 0 ? (float)m_perfFrameCount : 1.0f;
				fprintf(stderr, "[d3d8gles] perf-draws/frame by source: models=%.1f sorted(particles)=%.1f "
					"2d-ui=%.1f terrain=%.1f shadows=%.1f skin=%.1f other=%.1f\n",
					s_gxDrawsByCategory[GX_DRAWCAT_MODELS] / f,
					s_gxDrawsByCategory[GX_DRAWCAT_SORTED] / f,
					s_gxDrawsByCategory[GX_DRAWCAT_2D] / f,
					s_gxDrawsByCategory[GX_DRAWCAT_TERRAIN] / f,
					s_gxDrawsByCategory[GX_DRAWCAT_SHADOWS] / f,
					s_gxDrawsByCategory[GX_DRAWCAT_SKIN] / f,
					s_gxDrawsByCategory[GX_DRAWCAT_OTHER] / f);
				fprintf(stderr, "[d3d8gles] perf-ui ms/frame: text-raster=%.2f text-texture=%.2f 2d-submit=%.2f\n",
					s_gxUiTimeUs[0] / 1000.0 / f,
					s_gxUiTimeUs[1] / 1000.0 / f,
					s_gxUiTimeUs[2] / 1000.0 / f);
				double drawUs = 0.0;
				for (int i = 0; i < GX_DRAWCAT_COUNT; i++) drawUs += s_gxDrawUsByCategory[i];
				fprintf(stderr, "[d3d8gles] perf-cpu ms/frame: draws=%.2f (glDraw %.2f) models=%.2f particles=%.2f "
					"2d-ui=%.2f terrain=%.2f other=%.2f | uploads=%.2f (%.0f calls)\n",
					drawUs / 1000.0 / f, s_gxDrawCallUs / 1000.0 / f,
					s_gxDrawUsByCategory[GX_DRAWCAT_MODELS] / 1000.0 / f,
					s_gxDrawUsByCategory[GX_DRAWCAT_SORTED] / 1000.0 / f,
					s_gxDrawUsByCategory[GX_DRAWCAT_2D] / 1000.0 / f,
					s_gxDrawUsByCategory[GX_DRAWCAT_TERRAIN] / 1000.0 / f,
					(s_gxDrawUsByCategory[GX_DRAWCAT_OTHER] + s_gxDrawUsByCategory[GX_DRAWCAT_SHADOWS] +
					 s_gxDrawUsByCategory[GX_DRAWCAT_SKIN]) / 1000.0 / f,
					d3d8gles_perfUploadUs / 1000.0 / f, d3d8gles_perfUploadCalls / f);
				fprintf(stderr, "[d3d8gles] perf-gldraw ms/frame: models=%.2f particles=%.2f 2d-ui=%.2f terrain=%.2f other=%.2f | "
					"after-write draws=%.1f/frame (%.2f ms) other draws=%.1f/frame (%.2f ms)\n",
					s_gxDrawCallUsByCategory[GX_DRAWCAT_MODELS] / 1000.0 / f,
					s_gxDrawCallUsByCategory[GX_DRAWCAT_SORTED] / 1000.0 / f,
					s_gxDrawCallUsByCategory[GX_DRAWCAT_2D] / 1000.0 / f,
					s_gxDrawCallUsByCategory[GX_DRAWCAT_TERRAIN] / 1000.0 / f,
					(s_gxDrawCallUsByCategory[GX_DRAWCAT_OTHER] + s_gxDrawCallUsByCategory[GX_DRAWCAT_SHADOWS] +
					 s_gxDrawCallUsByCategory[GX_DRAWCAT_SKIN]) / 1000.0 / f,
					s_gxDrawsAfterWrite / f, s_gxDrawCallUsAfterWrite / 1000.0 / f,
					(m_perfDrawAccum - s_gxDrawsAfterWrite) / f, (s_gxDrawCallUs - s_gxDrawCallUsAfterWrite) / 1000.0 / f);
				{
					// GL state calls per draw, for the two sources that dominate a battle.
					const int cats[2] = { GX_DRAWCAT_SORTED, GX_DRAWCAT_MODELS };
					const char *names[2] = { "particles", "models" };
					char line[512];
					int len = snprintf(line, sizeof(line), "[d3d8gles] perf-state calls/draw:");
					for (int c = 0; c < 2; c++) {
						const unsigned *n = d3d8gles_stateCalls[cats[c]];
						const float draws = s_gxDrawsByCategory[cats[c]] > 0 ? (float)s_gxDrawsByCategory[cats[c]] : 1.0f;
						len += snprintf(line + len, sizeof(line) - len,
							" %s prog=%.2f tex=%.2f blend=%.2f depth=%.2f enable=%.2f uniform=%.2f attrib=%.2f%s",
							names[c], n[0] / draws, n[1] / draws, n[2] / draws, n[3] / draws, n[4] / draws,
							n[5] / draws, n[6] / draws, c == 0 ? " |" : "");
					}
					fprintf(stderr, "%s\n", line);
					memset(d3d8gles_stateCalls, 0, sizeof(d3d8gles_stateCalls));
				}
				for (int i = 0; i < GX_DRAWCAT_COUNT; i++) s_gxDrawCallUsByCategory[i] = 0.0;
				s_gxDrawCallUsAfterWrite = 0.0;
				s_gxDrawsAfterWrite = 0;
				{
					const char *names[GX_UPLOAD_KIND_COUNT] = { "vb-full", "vb-append", "ib-full", "ib-append", "ring+index-stream" };
					char line[512];
					int len = snprintf(line, sizeof(line), "[d3d8gles] perf-upload per frame:");
					double known = 0.0;
					for (int k = 0; k < GX_UPLOAD_KIND_COUNT; k++) {
						known += s_gxUploadUs[k];
						len += snprintf(line + len, sizeof(line) - len, " %s=%.2fms (%.1f, %.0fKB)", names[k],
							s_gxUploadUs[k] / 1000.0 / f, s_gxUploadCount[k] / f, s_gxUploadBytes[k] / 1024.0 / f);
						s_gxUploadUs[k] = s_gxUploadBytes[k] = 0.0;
						s_gxUploadCount[k] = 0;
					}
					// Everything else the driver-upload timer saw is texture uploads.
					snprintf(line + len, sizeof(line) - len, " textures+other=%.2fms",
						(d3d8gles_perfUploadUs - known) / 1000.0 / f);
					fprintf(stderr, "%s\n", line);
				}
				for (int i = 0; i < GX_DRAWCAT_COUNT; i++) s_gxDrawUsByCategory[i] = 0.0;
				s_gxDrawCallUs = 0.0;
				d3d8gles_perfUploadUs = 0.0;
				d3d8gles_perfUploadCalls = 0;
				for (int i = 0; i < 3; i++) s_gxUiTimeUs[i] = 0.0;
				for (int i = 0; i < GX_DRAWCAT_COUNT; i++) s_gxDrawsByCategory[i] = 0;
			}
			fprintf(stderr, "[d3d8gles] perf: %.1f fps, %.1f draws/frame, "
				"state-cache %.0f%% hit (%d/%d), vao-cache %.0f%% hit (%d/%d, %zu cached, "
				"%d ptr-refresh), uniform-cache %.0f%% hit (%d/%d), "
				"textures live=%ld (created=%ld deleted=%ld), "
				"programs built=%d (%.1fms total, %.2fms avg, %d cached)\n",
				fps, drawsPerFrame, cacheHitPct, m_perfStateCacheHits, totalStateChecks,
				vaoHitPct, m_perfVAOCacheHits, totalVAOChecks, m_vaoCache.size(), m_perfVAOPointerRefresh,
				uniformHitPct, m_perfUniformCacheHits, totalUniformChecks,
				g_texturesCreated - g_texturesDeleted, g_texturesCreated, g_texturesDeleted,
				m_perfProgramBuilds, m_perfProgramBuildUs / 1000.0,
				m_perfProgramBuilds > 0 ? m_perfProgramBuildUs / 1000.0 / m_perfProgramBuilds : 0.0,
				m_programCount);
			// GeneralsX @build Android port GLES experiment 08/30/2026 Per-
			// block breakdown of the combined uniform-cache rate above. A
			// real device log with this line's first version (transform =
			// view+proj+texMat0+texMat1 as one combined key) showed
			// transform/misc collapsing to ~35% hit rate mid-battle while
			// material/lighting held 82-88% -- suspicious, since the camera
			// (view/proj) is set once per frame and should hit on nearly
			// every draw. Split transform into separate viewproj/texmat
			// buckets (gles_pipeline.h's ViewProjKey/TexMatKey) to confirm
			// whether per-object texture-stage transforms (UV scroll/glow
			// animations) were invalidating the whole combined key on every
			// such draw and forcing a spurious view/proj re-upload too.
			{
				auto pct = [](int hits, int misses) {
					int total = hits + misses;
					return total > 0 ? 100.0f * hits / total : 0.0f;
				};
				fprintf(stderr, "[d3d8gles] uniform-cache breakdown: viewproj %.0f%% (%d/%d), "
					"texmat %.0f%% (%d/%d), misc %.0f%% (%d/%d), material %.0f%% (%d/%d), lighting %.0f%% (%d/%d)\n",
					pct(m_perfUniformViewProjHits, m_perfUniformViewProjMisses),
					m_perfUniformViewProjHits, m_perfUniformViewProjHits + m_perfUniformViewProjMisses,
					pct(m_perfUniformTexMatHits, m_perfUniformTexMatMisses),
					m_perfUniformTexMatHits, m_perfUniformTexMatHits + m_perfUniformTexMatMisses,
					pct(m_perfUniformMiscHits, m_perfUniformMiscMisses),
					m_perfUniformMiscHits, m_perfUniformMiscHits + m_perfUniformMiscMisses,
					pct(m_perfUniformMaterialHits, m_perfUniformMaterialMisses),
					m_perfUniformMaterialHits, m_perfUniformMaterialHits + m_perfUniformMaterialMisses,
					pct(m_perfUniformLightingHits, m_perfUniformLightingMisses),
					m_perfUniformLightingHits, m_perfUniformLightingHits + m_perfUniformLightingMisses);
			}
			// GeneralsX @bugfix Android port 09/04/2026 Diagnostic for a
			// persistent strip near the screen edge reported on GLES/ANGLE
			// (present at native/100% resolution too, so it's not a
			// pillarbox-blit artifact -- confirmed by real-device testing
			// that ruled out every pillarbox-side theory tried so far).
			// Everything on the C++ bookkeeping side (m_fbWidth/Height,
			// _PresentParameters, Get_Render_Target_Resolution) claims the
			// backbuffer is the full real window size, but that's all
			// self-reported -- this logs the ACTUAL GL state at present()
			// time (real glViewport, not what we think we set) to see
			// whether it agrees. If GL_VIEWPORT doesn't match
			// m_fbWidth/m_fbHeight here, the bug is in how a viewport call
			// got lost/cached wrong somewhere in this file. If it DOES
			// match, the bug is further upstream (EGL surface/window
			// geometry itself), outside this pipeline's control.
			// Read where the GL calls run, so the render thread is not made to drain for it.
			{
				const int fbW = m_fbWidth, fbH = m_fbHeight, rtW = m_curRTWidth, rtH = m_curRTHeight;
				const unsigned fbo = (unsigned)m_curFBO;
				const float yFlip = m_yFlip;
				gxrt::post([fbW, fbH, rtW, rtH, fbo, yFlip] {
					GLint vpDump[4] = {-1, -1, -1, -1};
					gxrt::rawGetIntegerv(GL_VIEWPORT, vpDump);
					fprintf(stderr, "[d3d8gles-diag] present(): GL_VIEWPORT=(%d,%d,%d,%d) m_fbWidth=%d m_fbHeight=%d "
						"m_curRTWidth=%d m_curRTHeight=%d m_curFBO=%u m_yFlip=%.1f\n",
						vpDump[0], vpDump[1], vpDump[2], vpDump[3], fbW, fbH, rtW, rtH, fbo, yFlip);
				});
			}
			DumpLiveTextureShapes();
			// GeneralsX @performance Android port 27/09/2026 What each translator optimization
			// did in this window: base-vertex draws (each one a pointer refresh saved), *UP draws
			// through the ring, program binaries loaded from and saved to disk, render-target
			// readbacks and their total stall, DXT1 levels uploaded at 16 bpp.
			const double frames = m_perfFrameCount > 0 ? (double)m_perfFrameCount : 1.0;
			fprintf(stderr, "[d3d8gles] perf-opt: basevertex=%.1f/frame upring=%.1f/frame (%.0f KB/frame, %d wraps) "
				"progcache load=%d save=%d rt-readback=%d (%.1f ms) dxt16 levels=%d (%.1f MB saved) "
				"persistent switches=%.1f/frame waits=%d new-copies=%d index-stream renewals=%d\n",
				m_perfBaseVertexDraws / frames, m_perfUpRingDraws / frames, m_perfUpRingBytes / 1024.0 / frames,
				m_perfUpRingWraps, m_perfProgramCacheLoads, m_perfProgramCacheSaves,
				m_perfRTReadbacks, m_perfRTReadbackUs / 1000.0,
				m_perfDxt16Levels, m_perfDxt16SavedBytes / (1024.0 * 1024.0),
				m_perfPersistentSwitches / frames, m_perfPersistentWaits, m_perfPersistentCopies,
				m_perfIndexStreamRenewals);
			if (m_perfSyncUploads > 0)
				fprintf(stderr, "[d3d8gles] perf-opt: %.1f/frame buffer updates from plain locks, uploaded synchronized\n",
					m_perfSyncUploads / frames);
			m_perfSyncUploads = 0;
			if (m_perfWorldUploads + m_perfWorldSkips > 0)
				fprintf(stderr, "[d3d8gles] perf-opt: world matrix %.1f/frame sent, %.1f/frame already in the program\n",
					m_perfWorldUploads / frames, m_perfWorldSkips / frames);
			m_perfWorldUploads = m_perfWorldSkips = 0;
			if (m_perfRangeUnderstated > 0)
				fprintf(stderr, "[d3d8gles] perf-opt: %d indexed draws read past their stated vertex range (hazard range widened)\n",
					m_perfRangeUnderstated);
			m_perfRangeUnderstated = 0;
			m_perfPersistentSwitches = m_perfPersistentWaits = m_perfPersistentCopies = m_perfIndexStreamRenewals = 0;
			// GeneralsX @performance Android port 30/09/2026 The render thread: how long it ran GL
			// calls, and how long the engine's thread waited -- for the previous frame's swap (the
			// render thread is the slower half), for a call's result (a round trip), or for room
			// in the command ring. With the thread working, waits are small and busy is roughly
			// what the GL calls used to cost the engine's thread.
			// GeneralsX @performance Android port 01/10/2026 Clocks and temperature, so a log can
			// tell throttling from a regression: in logs-38 the same spot of the menu battle ran at
			// ~60 fps at the start of the loop and ~40 a few minutes later with no more work per
			// frame. Current frequency of each CPU and the hottest readable thermal zone; either
			// may be unreadable under the device's SELinux policy, which is reported as such.
			{
				const std::string thermal = ThermalLine();
				if (!thermal.empty())
					fputs(thermal.c_str(), stderr);
			}
			{
				const gxrt::Stats rt = gxrt::takeStats();
				fprintf(stderr, "[d3d8gles] perf-thread: %s worker-busy=%.2f ms/frame waits: frame=%.2f sync=%.2f (%.1f calls) "
					"ring=%.2f ms/frame, commands=%.0f/frame (%.0f KB/frame)\n",
					gxrt::running() ? "on" : "off", rt.workerBusyUs / 1000.0 / frames, rt.frameWaitUs / 1000.0 / frames,
					rt.syncWaitUs / 1000.0 / frames, rt.syncCalls / frames, rt.ringWaitUs / 1000.0 / frames,
					rt.commands / frames, rt.commandBytes / 1024.0 / frames);
				// GeneralsX @performance Android port 01/10/2026 Render-thread time split, and GPU time.
				// A frame is GPU-bound when gpu is close to the frame time and swap is large (the swap
				// waits for the GPU); CPU-bound when draw/other dominate and gpu is well below.
				const unsigned gpuFrames = s_gpuTimer.frames.exchange(0, std::memory_order_relaxed);
				const uint64_t gpuNs = s_gpuTimer.gpuNs.exchange(0, std::memory_order_relaxed);
				char gpuText[64];
				if (gpuFrames > 0)
					snprintf(gpuText, sizeof(gpuText), "%.2f ms/frame (%u frames timed)", gpuNs / 1.0e6 / gpuFrames, gpuFrames);
				else
					snprintf(gpuText, sizeof(gpuText), "%s", s_gpuTimer.ok ? "no results yet" : "unavailable");
				const double other = rt.workerBusyUs - rt.drawUs - rt.swapUs - rt.uploadUs;
				fprintf(stderr, "[d3d8gles] perf-gpu: render thread ms/frame: draw=%.2f swap=%.2f upload=%.2f other=%.2f | gpu=%s\n",
					rt.drawUs / 1000.0 / frames, rt.swapUs / 1000.0 / frames, rt.uploadUs / 1000.0 / frames,
					(other > 0 ? other : 0.0) / 1000.0 / frames, gpuText);
			}
			fprintf(stderr, "[d3d8gles] perf report took %.2f ms on the game's thread\n",
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - reportStart).count());
			m_perfLogLastMs = nowMs;
			m_perfFrameCount = 0;
			m_perfDrawAccum = 0;
			m_perfStateCacheHits = 0;
			m_perfStateCacheMisses = 0;
			m_perfVAOCacheHits = 0;
			m_perfVAOCacheMisses = 0;
			m_perfVAOPointerRefresh = 0;
			m_perfUniformCacheHits = 0;
			m_perfUniformCacheMisses = 0;
			m_perfUniformViewProjHits = 0;
			m_perfUniformViewProjMisses = 0;
			m_perfUniformTexMatHits = 0;
			m_perfUniformTexMatMisses = 0;
			m_perfUniformMiscHits = 0;
			m_perfUniformMiscMisses = 0;
			m_perfUniformMaterialHits = 0;
			m_perfUniformMaterialMisses = 0;
			m_perfUniformLightingHits = 0;
			m_perfUniformLightingMisses = 0;
			m_perfProgramBuilds = 0;
			m_perfProgramBuildUs = 0.0;

			m_perfBaseVertexDraws = 0;
			m_perfUpRingDraws = 0;
			m_perfUpRingWraps = 0;
			m_perfUpRingBytes = 0.0;
			m_perfProgramCacheLoads = 0;
			m_perfProgramCacheSaves = 0;
			m_perfRTReadbacks = 0;
			m_perfRTReadbackUs = 0.0;
			m_perfDxt16Levels = 0;
			m_perfDxt16SavedBytes = 0.0;
		}
	}

	int swapInterval = -1;
	{
		static int s_appliedUncapped = 0;
		if (s_gxWantUncappedPresent != s_appliedUncapped) {
			s_appliedUncapped = s_gxWantUncappedPresent;
			swapInterval = s_appliedUncapped ? 0 : 1;
			fprintf(stderr, "[d3d8gles] vsync %s\n", s_appliedUncapped ? "off" : "on");
		}
	}

	// GeneralsX @build Android port GLES experiment - the browser build never
	// needed an explicit swap (the canvas presents implicitly when the game
	// pthread yields back to its rAF loop tick). Android/EGL has no such
	// implicit hook, so this call is new, not adapted from upstream.
	// GeneralsX @performance Android port 30/09/2026 Queued behind the frame's GL calls when the
	// render thread runs; waits for the previous frame's swap, not this one's.
	if (m_window) {
		if (m_vbActive) {
			if (!m_vbUpscaled && !m_vbBypass)
				stretchVirtualBackbuffer();
			// The next frame: into the virtual backbuffer again if this one had a scene, else (no
			// scene to upscale, and the window is the game's size) straight into the window.
			const bool wasBackbuffer = (m_curFBO == 0 || m_curFBO == m_vbFBO);
			m_vbUpscaled = false;
			m_vbBypass = !m_vbSceneSeen && (m_vbRW != m_vbW || m_vbRH != m_vbH) && m_winW == m_vbW && m_winH == m_vbH;
			m_vbSceneSeen = false;
			if (wasBackbuffer) {
				glBindFramebuffer(GL_FRAMEBUFFER, backbufferFBO());
				m_curFBO = backbufferFBO();
				m_curRTWidth = m_vbW;
				m_curRTHeight = m_vbH;
				m_haveFixedStateKey = false;
			} else {
				glBindFramebuffer(GL_FRAMEBUFFER, m_curFBO);
			}
		}
		if (s_gpuTimer.ok)
			gxrt::post([] { s_gpuTimer.frameEnd(); });
		gxrt::present(m_window, swapInterval);
		if (s_gpuTimer.ok)
			gxrt::post([] { s_gpuTimer.frameBegin(); });
	}
}
