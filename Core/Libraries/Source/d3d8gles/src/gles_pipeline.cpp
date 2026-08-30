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
#include <chrono>

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static bool g_glTrace = false;

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
	GLint uWorld = -1, uView = -1, uProj = -1;
	GLint uViewportPos = -1;
	GLint uYFlip = -1;
	GLint uTex0 = -1, uTex1 = -1;
	GLint uTexMat0 = -1, uTexMat1 = -1;
	GLint uTFactor = -1;
	GLint uAlphaRef = -1;
	GLint uFogColor = -1, uFogParams = -1;
	GLint uMatDiffuse = -1, uMatAmbient = -1, uMatEmissive = -1;
	GLint uGlobalAmbient = -1;
	GLint uNumLights = -1;
	GLint uLightType = -1, uLightDir = -1, uLightPos = -1;
	GLint uLightDiffuse = -1, uLightAmbient = -1, uLightAtten = -1;
	// key fields needed at bind time
	int stageTci[2] = {0, 0};
	bool stageXform[2] = {false, false};
	int stagesUsed = 0;
};

// Stage portion of the program key.
struct StageKey {
	unsigned colorOp, colorArg1, colorArg2;
	unsigned alphaOp, alphaArg1, alphaArg2;
	unsigned tci;
	unsigned texgen; // 0=vertex uv set, 1=camera-space position
	bool xform;
};

static void getStageKey(WebGLDevice *dev, int stage, StageKey *k);
static std::string combinerArg(unsigned arg, const char *texExpr, int *usesTex);
static std::string combinerOp(unsigned op, const std::string &a1, const std::string &a2,
                              const char *texAlphaExpr);

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
	SDL_GL_SetSwapInterval(1);

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
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glDisable(GL_DITHER);

	m_ctxReady = true;
	fprintf(stderr, "[d3d8gles] GLES3 context ready %dx%d (s3tc=%d)\n", w, h, (int)m_hasS3TC);
	return true;
}

void WebGLPipeline::resize(int w, int h)
{
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
		if (!dev->getTexture2D(s)) {
			// No texture bound: any op sourcing TEXTURE collapses.
			if (s == 0) {
				sk.colorOp = D3DTOP_SELECTARG2;
				sk.colorArg2 = 0; // DIFFUSE
				sk.alphaOp = D3DTOP_SELECTARG2;
				sk.alphaArg2 = 0;
			} else {
				sk.colorOp = D3DTOP_DISABLE;
				sk.alphaOp = D3DTOP_DISABLE;
			}
		}
		put(sk.colorOp, 5);
		put(sk.colorArg1, 6);
		put(sk.colorArg2, 6);
		put(sk.alphaOp, 5);
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

static std::string combinerOp(unsigned op, const std::string &a1, const std::string &a2,
                              const char *texAlphaExpr)
{
	char buf[512];
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
	default:
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
		if (!dev->getTexture2D(s)) {
			if (s == 0) {
				st[s].colorOp = D3DTOP_SELECTARG2;
				st[s].colorArg2 = 0;
				st[s].alphaOp = D3DTOP_SELECTARG2;
				st[s].alphaArg2 = 0;
			} else {
				st[s].colorOp = D3DTOP_DISABLE;
				st[s].alphaOp = D3DTOP_DISABLE;
			}
		}
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
	vs += "uniform mat4 uWorld, uView, uProj;\n";
	vs += "uniform vec4 uViewportPos;\n"; // x, y, w, h
	vs += "uniform float uYFlip;\n"; // +1 backbuffer, -1 render-to-texture
	vs += "uniform mat4 uTexMat0, uTexMat1;\n";
	vs += "out vec4 vCol;\nout vec4 vSpec;\nout vec2 vUV0;\nout vec2 vUV1;\nout float vFogDepth;\n";
	if (lighting) {
		vs += "uniform vec4 uMatDiffuse, uMatAmbient, uMatEmissive, uGlobalAmbient;\n";
		vs += "uniform int uNumLights;\n";
		vs += "uniform int uLightType[4];\nuniform vec3 uLightDir[4];\nuniform vec3 uLightPos[4];\n";
		vs += "uniform vec4 uLightDiffuse[4];\nuniform vec4 uLightAmbient[4];\nuniform vec4 uLightAtten[4];\n"; // atten: range, a0, a1, a2
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
		                     : "  vec4 matDiff = uMatDiffuse;\n";
		vs += ambFromVertex ? "  vec3 matAmb = aColor0.zyx;\n"
		                    : "  vec3 matAmb = uMatAmbient.rgb;\n";
		vs += emisFromVertex ? "  vec3 matEmis = aColor0.zyx;\n"
		                     : "  vec3 matEmis = uMatEmissive.rgb;\n";
		vs += "  vec3 accum = matEmis + uGlobalAmbient.rgb * matAmb;\n";
		vs += "  for (int i = 0; i < uNumLights; i++) {\n";
		vs += "    vec3 L; float atten = 1.0;\n";
		vs += "    if (uLightType[i] == 1) {\n"; // POINT
		vs += "      vec3 d = uLightPos[i] - wpos.xyz; float dist = length(d);\n";
		vs += "      if (dist > uLightAtten[i].x) { continue; }\n";
		vs += "      L = d / max(dist, 0.0001);\n";
		vs += "      atten = 1.0 / (uLightAtten[i].y + uLightAtten[i].z * dist + uLightAtten[i].w * dist * dist);\n";
		vs += "    } else { L = -uLightDir[i]; }\n";
		vs += "    float ndl = max(dot(wnrm, L), 0.0);\n";
		vs += "    accum += uLightAmbient[i].rgb * matAmb * atten;\n";
		vs += "    accum += uLightDiffuse[i].rgb * matDiff.rgb * ndl * atten;\n";
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
		std::string c1 = combinerArg(st[s].colorArg1, texv, &usesTex);
		std::string c2 = combinerArg(st[s].colorArg2, texv, &usesTex);
		std::string a1 = combinerArg(st[s].alphaArg1, texv, &usesTex);
		std::string a2 = combinerArg(st[s].alphaArg2, texv, &usesTex);
		// At stage 0, D3DTA_CURRENT reads DIFFUSE.
		std::string colorExpr = combinerOp(st[s].colorOp, c1, c2, texa);
		std::string alphaExpr =
			st[s].alphaOp == D3DTOP_DISABLE ? "cur" : combinerOp(st[s].alphaOp, a1, a2, texa);
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

	// ---------------- link ----------------
	const std::chrono::steady_clock::time_point buildStart = std::chrono::steady_clock::now();
	GLuint vsh = compileShader(GL_VERTEX_SHADER, vs);
	GLuint fsh = compileShader(GL_FRAGMENT_SHADER, fs);
	ProgramInfo *info = new ProgramInfo();
	if (vsh && fsh) {
		GLuint p = glCreateProgram();
		glAttachShader(p, vsh);
		glAttachShader(p, fsh);
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
	}
	if (vsh) glDeleteShader(vsh);
	if (fsh) glDeleteShader(fsh);
	m_perfProgramBuilds++;
	m_perfProgramBuildUs += std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - buildStart).count();

	if (info->prog) {
		GLuint p = info->prog;
		info->uWorld = glGetUniformLocation(p, "uWorld");
		info->uView = glGetUniformLocation(p, "uView");
		info->uProj = glGetUniformLocation(p, "uProj");
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
		info->uMatDiffuse = glGetUniformLocation(p, "uMatDiffuse");
		info->uMatAmbient = glGetUniformLocation(p, "uMatAmbient");
		info->uMatEmissive = glGetUniformLocation(p, "uMatEmissive");
		info->uGlobalAmbient = glGetUniformLocation(p, "uGlobalAmbient");
		info->uNumLights = glGetUniformLocation(p, "uNumLights");
		info->uLightType = glGetUniformLocation(p, "uLightType[0]");
		info->uLightDir = glGetUniformLocation(p, "uLightDir[0]");
		info->uLightPos = glGetUniformLocation(p, "uLightPos[0]");
		info->uLightDiffuse = glGetUniformLocation(p, "uLightDiffuse[0]");
		info->uLightAmbient = glGetUniformLocation(p, "uLightAmbient[0]");
		info->uLightAtten = glGetUniformLocation(p, "uLightAtten[0]");
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
	for (int lvl = 0; lvl < levels; lvl++) {
		WebGLSurface *s = tex->m_levels[lvl];
		UploadDesc up;
		if (!prepareLevelUpload(tex->m_format, s->m_width, s->m_height,
		                        s->m_bits.data(), s->m_bits.size(), m_hasS3TC, &up)) {
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
			const GLenum cerr = glGetError();
			if (cerr != GL_NO_ERROR) {
				fprintf(stderr, "[d3d8gles] DXT upload error 0x%x lvl=%d %ux%u fmt=0x%x size=%u\n",
					cerr, lvl, s->m_width, s->m_height, (unsigned)tex->m_format, up.compressedSize);
			}
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
	const D3DVIEWPORT8 &vpKey = dev->getViewport();
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
	m_lastFixedStateKey = key;
	m_haveFixedStateKey = true;

	// Depth
	const DWORD zEnable = dev->getRenderState(D3DRS_ZENABLE);
	if (zEnable) glEnable(GL_DEPTH_TEST);
	else glDisable(GL_DEPTH_TEST);
	glDepthMask(dev->getRenderState(D3DRS_ZWRITEENABLE) ? GL_TRUE : GL_FALSE);
	const DWORD zfunc = dev->getRenderState(D3DRS_ZFUNC);
	glDepthFunc(d3dCmpToGL(zfunc ? zfunc : D3DCMP_LESSEQUAL));

	// Blend
	if (dev->getRenderState(D3DRS_ALPHABLENDENABLE)) {
		glEnable(GL_BLEND);
		const DWORD sb = dev->getRenderState(D3DRS_SRCBLEND);
		const DWORD db = dev->getRenderState(D3DRS_DESTBLEND);
		glBlendFunc(d3dBlendToGL(sb ? sb : D3DBLEND_ONE), d3dBlendToGL(db ? db : D3DBLEND_ZERO));
	} else {
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
	if (zbias) {
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(-1.0f, -(float)zbias * 2.0f);
	} else {
		glDisable(GL_POLYGON_OFFSET_FILL);
	}

	// Color mask. Zero is a real value: stencil shadow volumes render with
	// COLORWRITEENABLE=0 (stencil-only) - mapping it to "write everything"
	// made every shadow volume a visible black silhouette.
	const DWORD cw = dev->getRenderState(D3DRS_COLORWRITEENABLE);
	glColorMask((cw & 1) != 0, (cw & 2) != 0, (cw & 4) != 0, (cw & 8) != 0);

	// Stencil
	if (dev->getRenderState(D3DRS_STENCILENABLE)) {
		glEnable(GL_STENCIL_TEST);
		const DWORD func = dev->getRenderState(D3DRS_STENCILFUNC);
		glStencilFunc(d3dCmpToGL(func ? func : D3DCMP_ALWAYS),
		              (GLint)dev->getRenderState(D3DRS_STENCILREF),
		              dev->getRenderState(D3DRS_STENCILMASK) ? dev->getRenderState(D3DRS_STENCILMASK) : 0xFFFFFFFF);
		glStencilOp(d3dStencilOpToGL(dev->getRenderState(D3DRS_STENCILFAIL)),
		            d3dStencilOpToGL(dev->getRenderState(D3DRS_STENCILZFAIL)),
		            d3dStencilOpToGL(dev->getRenderState(D3DRS_STENCILPASS)));
		glStencilMask(dev->getRenderState(D3DRS_STENCILWRITEMASK) ? dev->getRenderState(D3DRS_STENCILWRITEMASK) : 0xFFFFFFFF);
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
	const D3DVIEWPORT8 &vp = dev->getViewport();
	const GLint glViewportY = (GLint)(m_curRTHeight - (int)vp.Y - (int)vp.Height);
	glViewport((GLint)vp.X, glViewportY, (GLsizei)vp.Width, (GLsizei)vp.Height);
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
		m_haveTransformKey = m_haveMiscKey = m_haveMaterialKey = m_haveLightingKey = false;
	}

	// D3D row-major memory uploaded untransposed IS the transpose GL wants
	// for column-vector math (see plan notes). uWorld is deliberately never
	// cached -- it changes on nearly every draw in real battlefield
	// rendering (each object has its own transform) -- see TransformKey's
	// declaration for why the rest of this function's blocks are cached.
	if (prog->uWorld >= 0)
		glUniformMatrix4fv(prog->uWorld, 1, GL_FALSE, (const float *)&dev->getTransform(D3DTS_WORLD));

	if (prog->uView >= 0 || prog->uProj >= 0 || prog->uTexMat0 >= 0 || prog->uTexMat1 >= 0) {
		TransformKey key{};
		memcpy(key.view, &dev->getTransform(D3DTS_VIEW), sizeof(key.view));
		memcpy(key.proj, &dev->getTransform(D3DTS_PROJECTION), sizeof(key.proj));
		memcpy(key.texMat0, &dev->getTransform(D3DTS_TEXTURE0), sizeof(key.texMat0));
		memcpy(key.texMat1, &dev->getTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + 1)), sizeof(key.texMat1));
		if (m_haveTransformKey && key == m_lastTransformKey) {
			m_perfUniformCacheHits++;
			m_perfUniformTransformHits++;
		} else {
			m_perfUniformCacheMisses++;
			m_perfUniformTransformMisses++;
			if (prog->uView >= 0) glUniformMatrix4fv(prog->uView, 1, GL_FALSE, key.view);
			if (prog->uProj >= 0) glUniformMatrix4fv(prog->uProj, 1, GL_FALSE, key.proj);
			if (prog->uTexMat0 >= 0) glUniformMatrix4fv(prog->uTexMat0, 1, GL_FALSE, key.texMat0);
			if (prog->uTexMat1 >= 0) glUniformMatrix4fv(prog->uTexMat1, 1, GL_FALSE, key.texMat1);
			m_lastTransformKey = key;
			m_haveTransformKey = true;
		}
	}

	if (prog->uViewportPos >= 0 || prog->uYFlip >= 0 || prog->uTFactor >= 0 ||
	    prog->uAlphaRef >= 0 || prog->uFogColor >= 0) {
		const D3DVIEWPORT8 &vp = dev->getViewport();
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
	if (prog->uMatDiffuse >= 0 || prog->uMatAmbient >= 0 || prog->uMatEmissive >= 0) {
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
			if (prog->uMatDiffuse >= 0) glUniform4fv(prog->uMatDiffuse, 1, key.diffuse);
			if (prog->uMatAmbient >= 0) glUniform4fv(prog->uMatAmbient, 1, key.ambient);
			if (prog->uMatEmissive >= 0) glUniform4fv(prog->uMatEmissive, 1, key.emissive);
			m_lastMaterialKey = key;
			m_haveMaterialKey = true;
		}
	}
	if (prog->uGlobalAmbient >= 0 || prog->uNumLights >= 0) {
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
			if (prog->uGlobalAmbient >= 0) glUniform4fv(prog->uGlobalAmbient, 1, key.globalAmbient);
			if (prog->uNumLights >= 0) {
				glUniform1i(prog->uNumLights, key.numLights);
				glUniform1iv(prog->uLightType, 4, key.types);
				glUniform3fv(prog->uLightDir, 4, key.dirs);
				glUniform3fv(prog->uLightPos, 4, key.poss);
				glUniform4fv(prog->uLightDiffuse, 4, key.diff);
				glUniform4fv(prog->uLightAmbient, 4, key.amb);
				glUniform4fv(prog->uLightAtten, 4, key.att);
			}
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

void WebGLPipeline::drawCommon(WebGLDevice *dev, unsigned primType, unsigned primCount,
                               GLuint vbo, unsigned stride, unsigned fvf,
                               GLuint ibo, unsigned indexFormat,
                               unsigned startIndex, int baseVertexBytes, unsigned /*vertexCount*/)
{
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

	bindVertexLayout(l, vbo, ibo, fvf, stride, baseVertexBytes);

	const GLenum mode = primModeGL(primType);
	const unsigned count = primVertexCount(primType, primCount);
	if (indexFormat != 0) {
		const GLenum itype = (indexFormat == D3DFMT_INDEX32) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
		const unsigned isize = (indexFormat == D3DFMT_INDEX32) ? 4 : 2;
		glDrawElements(mode, count, itype, (const void *)(intptr_t)(startIndex * isize));
	} else {
		glDrawArrays(mode, startIndex, count);
	}
	m_perfDrawsThisFrame++;
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
void WebGLPipeline::ensureVBUploaded(WebGLVertexBuffer *vb)
{
	if (vb->m_gl.name == 0) glGenBuffers(1, &vb->m_gl.name);
	if (vb->m_gl.dirty) {
		glBindBuffer(GL_COPY_WRITE_BUFFER, vb->m_gl.name);
		glBufferData(GL_COPY_WRITE_BUFFER, vb->m_bits.size(), vb->m_bits.data(), GL_DYNAMIC_DRAW);
		vb->m_gl.dirty = false;
	}
}

void WebGLPipeline::ensureIBUploaded(WebGLIndexBuffer *ib)
{
	if (ib->m_gl.name == 0) glGenBuffers(1, &ib->m_gl.name);
	if (ib->m_gl.dirty) {
		glBindBuffer(GL_COPY_WRITE_BUFFER, ib->m_gl.name);
		glBufferData(GL_COPY_WRITE_BUFFER, ib->m_bits.size(), ib->m_bits.data(), GL_DYNAMIC_DRAW);
		ib->m_gl.dirty = false;
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
static void orphanAndUpload(GLenum target, GLuint buffer, size_t size, const void *data, GLenum usage)
{
	glBindBuffer(target, buffer);
	glBufferData(target, size, nullptr, usage);
	glBufferData(target, size, data, usage);
}

void WebGLPipeline::drawIndexed(WebGLDevice *dev, unsigned primType, unsigned /*minIndex*/,
                                unsigned numVertices, unsigned startIndex, unsigned primCount)
{
	if (!m_ctxReady) return;
	WebGLVertexBuffer *vb = dev->getStream0();
	WebGLIndexBuffer *ib = dev->getIndices();
	if (!vb || !ib) return;

	ensureVBUploaded(vb);
	ensureIBUploaded(ib);

	const unsigned fvf = dev->getFVF() ? dev->getFVF() : vb->m_fvf;
	const unsigned stride = dev->getStream0Stride();
	const int baseBytes = (int)(dev->getBaseVertexIndex() * stride);
	drawCommon(dev, primType, primCount, vb->m_gl.name, stride, fvf,
	           ib->m_gl.name, ib->m_format, startIndex, baseBytes, numVertices);
}

void WebGLPipeline::draw(WebGLDevice *dev, unsigned primType, unsigned startVertex, unsigned primCount)
{
	if (!m_ctxReady) return;
	WebGLVertexBuffer *vb = dev->getStream0();
	if (!vb) return;

	ensureVBUploaded(vb);

	const unsigned fvf = dev->getFVF() ? dev->getFVF() : vb->m_fvf;
	const unsigned stride = dev->getStream0Stride();
	drawCommon(dev, primType, primCount, vb->m_gl.name, stride, fvf,
	           0, 0, startVertex, 0, 0);
}

void WebGLPipeline::drawUP(WebGLDevice *dev, unsigned primType, unsigned primCount,
                           const void *vertexData, unsigned stride)
{
	if (!m_ctxReady || !vertexData) return;
	const unsigned fvf = dev->getFVF();
	FVFLayout l;
	if (!parseFVF(fvf, &l)) return;
	if (stride == 0) stride = l.stride;

	const unsigned vcount = primVertexCount(primType, primCount);
	orphanAndUpload(GL_COPY_WRITE_BUFFER, m_upVBO, (size_t)vcount * stride, vertexData, GL_STREAM_DRAW);

	drawCommon(dev, primType, primCount, m_upVBO, stride, fvf, 0, 0, 0, 0, vcount);
}

void WebGLPipeline::drawIndexedUP(WebGLDevice *dev, unsigned primType, unsigned minVertexIdx,
                                  unsigned numVertices, unsigned primCount,
                                  const void *indexData, unsigned indexFormat,
                                  const void *vertexData, unsigned stride)
{
	if (!m_ctxReady || !vertexData || !indexData) return;
	const unsigned fvf = dev->getFVF();
	FVFLayout l;
	if (!parseFVF(fvf, &l)) return;
	if (stride == 0) stride = l.stride;

	orphanAndUpload(GL_COPY_WRITE_BUFFER, m_upVBO, (size_t)(minVertexIdx + numVertices) * stride,
	                vertexData, GL_STREAM_DRAW);

	const unsigned isize = (indexFormat == D3DFMT_INDEX32) ? 4 : 2;
	const unsigned icount = primVertexCount(primType, primCount);
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
	const D3DVIEWPORT8 &vp = dev->getViewport();
	const bool full = (vp.X == 0 && vp.Y == 0 &&
	                   (int)vp.Width == m_curRTWidth && (int)vp.Height == m_curRTHeight);
	if (!full) {
		glEnable(GL_SCISSOR_TEST);
		// Same D3D-top-to-GL-bottom Y conversion as applyFixedState's
		// glViewport call -- glScissor's y is bottom-origin in GL too.
		glScissor((GLint)vp.X, (GLint)(m_curRTHeight - (int)vp.Y - (int)vp.Height),
		          (GLsizei)vp.Width, (GLsizei)vp.Height);
	}

	GLbitfield mask = 0;
	if (flags & D3DCLEAR_TARGET) {
		float c[4];
		argbToFloats(argb, c);
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
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		m_curFBO = 0;
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
		m_depthRBW = w;
		m_depthRBH = h;
	}
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_depthRB);

	const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		WARN_ONCE(s_fboIncomplete, "FBO incomplete: 0x%x (%dx%d fmt=%d)", status, w, h, (int)tex->m_format);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		m_curFBO = 0;
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

void WebGLPipeline::present()
{
	if (!m_ctxReady) return;
	m_frame++;

	GLenum err = glGetError();
	if (err != GL_NO_ERROR && (m_frame % 60) == 1) {
		fprintf(stderr, "[d3d8gles] glGetError at frame %u: 0x%x\n", m_frame, err);
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
			// block breakdown of the combined uniform-cache rate above --
			// added to find out whether a lower-than-vao-cache hit rate
			// (real device logs: ~74% combined vs vao-cache 99.8%+) comes
			// from a block that's expected to miss a lot per-object
			// (material/lighting -- genuinely varies per unit) or one that
			// should be near-constant within a frame (transform/misc --
			// same camera/viewport for every draw in a frame, a low rate
			// there would point at a real inefficiency, e.g. something
			// invalidating the key unnecessarily between draws that
			// shouldn't differ).
			{
				auto pct = [](int hits, int misses) {
					int total = hits + misses;
					return total > 0 ? 100.0f * hits / total : 0.0f;
				};
				fprintf(stderr, "[d3d8gles] uniform-cache breakdown: transform %.0f%% (%d/%d), "
					"misc %.0f%% (%d/%d), material %.0f%% (%d/%d), lighting %.0f%% (%d/%d)\n",
					pct(m_perfUniformTransformHits, m_perfUniformTransformMisses),
					m_perfUniformTransformHits, m_perfUniformTransformHits + m_perfUniformTransformMisses,
					pct(m_perfUniformMiscHits, m_perfUniformMiscMisses),
					m_perfUniformMiscHits, m_perfUniformMiscHits + m_perfUniformMiscMisses,
					pct(m_perfUniformMaterialHits, m_perfUniformMaterialMisses),
					m_perfUniformMaterialHits, m_perfUniformMaterialHits + m_perfUniformMaterialMisses,
					pct(m_perfUniformLightingHits, m_perfUniformLightingMisses),
					m_perfUniformLightingHits, m_perfUniformLightingHits + m_perfUniformLightingMisses);
			}
			DumpLiveTextureShapes();
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
			m_perfUniformTransformHits = 0;
			m_perfUniformTransformMisses = 0;
			m_perfUniformMiscHits = 0;
			m_perfUniformMiscMisses = 0;
			m_perfUniformMaterialHits = 0;
			m_perfUniformMaterialMisses = 0;
			m_perfUniformLightingHits = 0;
			m_perfUniformLightingMisses = 0;
			m_perfProgramBuilds = 0;
			m_perfProgramBuildUs = 0.0;
		}
	}

	// GeneralsX @build Android port GLES experiment - the browser build never
	// needed an explicit swap (the canvas presents implicitly when the game
	// pthread yields back to its rAF loop tick). Android/EGL has no such
	// implicit hook, so this call is new, not adapted from upstream.
	if (m_window) {
		SDL_GL_SwapWindow(m_window);
	}
}
