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
		// D3D clip z in [0,w] -> GL [-w,w]; screen y flip.
		vs += "  gl_Position = vec4(cpos.x, -cpos.y * uYFlip, cpos.z * 2.0 - cpos.w, cpos.w);\n";
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
			WARN_ONCE(s_noS3tc, "DXT texture but WEBGL_compressed_texture_s3tc missing");
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
			if (levels > 1) {
				// The engine frequently fills only level 0 of uncompressed
				// textures; GPU-generate the chain instead of sampling the
				// empty (transparent black) shadow mips.
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

void WebGLPipeline::bindTextures(WebGLDevice *dev, ProgramInfo *prog)
{
	for (int s = 0; s < 2; s++) {
		WebGLTexture *tex = dev->getTexture2D(s);
		glActiveTexture(GL_TEXTURE0 + s);
		if (tex) {
			if (tex->m_gl.dirty || tex->m_gl.name == 0) {
				uploadTexture(tex);
			} else {
				glBindTexture(GL_TEXTURE_2D, tex->m_gl.name);
			}
			applySamplerState(dev, s, tex);
		} else {
			glBindTexture(GL_TEXTURE_2D, 0);
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

	// Cull. The uniform clip-space y-negate flips winding exactly once
	// relative to D3D screen space, so with glFrontFace(GL_CCW):
	// D3D-CW-culled triangles are GL front faces and vice versa.
	switch (dev->getRenderState(D3DRS_CULLMODE)) {
	case D3DCULL_CW:
		glEnable(GL_CULL_FACE);
		glCullFace(GL_FRONT);
		break;
	case D3DCULL_CCW:
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
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

	// Viewport position. The uniform clip-space y-negate makes every render
	// target D3D-oriented (texel row 0 = top row of the D3D frame), so the
	// viewport rectangle must be placed from the TOP, i.e. at vp.Y directly.
	// The classic GL bottom-up flip here double-flipped partial viewports:
	// full-screen ones (Y=0, H=RT) are unaffected, but the in-game 3D view
	// (top portion of the screen) rendered shifted DOWN by RTH-H, leaving a
	// black band on top and offsetting picking by the same amount.
	const D3DVIEWPORT8 &vp = dev->getViewport();
	glViewport((GLint)vp.X, (GLint)vp.Y, (GLsizei)vp.Width, (GLsizei)vp.Height);
	glDepthRangef(vp.MinZ, vp.MaxZ);
}

void WebGLPipeline::applyUniforms(WebGLDevice *dev, ProgramInfo *prog, unsigned fvf)
{
	glUseProgram(prog->prog);

	// D3D row-major memory uploaded untransposed IS the transpose GL wants
	// for column-vector math (see plan notes).
	if (prog->uWorld >= 0)
		glUniformMatrix4fv(prog->uWorld, 1, GL_FALSE, (const float *)&dev->getTransform(D3DTS_WORLD));
	if (prog->uView >= 0)
		glUniformMatrix4fv(prog->uView, 1, GL_FALSE, (const float *)&dev->getTransform(D3DTS_VIEW));
	if (prog->uProj >= 0)
		glUniformMatrix4fv(prog->uProj, 1, GL_FALSE, (const float *)&dev->getTransform(D3DTS_PROJECTION));
	if (prog->uTexMat0 >= 0)
		glUniformMatrix4fv(prog->uTexMat0, 1, GL_FALSE, (const float *)&dev->getTransform(D3DTS_TEXTURE0));
	if (prog->uTexMat1 >= 0)
		glUniformMatrix4fv(prog->uTexMat1, 1, GL_FALSE, (const float *)&dev->getTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + 1)));

	if (prog->uViewportPos >= 0) {
		const D3DVIEWPORT8 &vp = dev->getViewport();
		glUniform4f(prog->uViewportPos, (float)vp.X, (float)vp.Y, (float)vp.Width, (float)vp.Height);
	}
	if (prog->uYFlip >= 0) glUniform1f(prog->uYFlip, m_yFlip);

	float c[4];
	argbToFloats(dev->getRenderState(D3DRS_TEXTUREFACTOR), c);
	if (prog->uTFactor >= 0) glUniform4fv(prog->uTFactor, 1, c);

	if (prog->uAlphaRef >= 0)
		glUniform1f(prog->uAlphaRef, (float)(dev->getRenderState(D3DRS_ALPHAREF) & 0xFF) / 255.0f);

	if (prog->uFogColor >= 0) {
		argbToFloats(dev->getRenderState(D3DRS_FOGCOLOR), c);
		glUniform4fv(prog->uFogColor, 1, c);
		glUniform2f(prog->uFogParams,
		            dwordToFloat(dev->getRenderState(D3DRS_FOGSTART)),
		            dwordToFloat(dev->getRenderState(D3DRS_FOGEND)));
	}

	// NOTE: each uniform can be optimized out independently (a program whose
	// material sources are all vertex colors has NO uMat* uniforms but still
	// needs its lights). Never gate the light upload on a material location.
	{
		const D3DMATERIAL8 &m = dev->getMaterial();
		if (prog->uMatDiffuse >= 0) glUniform4fv(prog->uMatDiffuse, 1, (const float *)&m.Diffuse);
		if (prog->uMatAmbient >= 0) glUniform4fv(prog->uMatAmbient, 1, (const float *)&m.Ambient);
		if (prog->uMatEmissive >= 0) glUniform4fv(prog->uMatEmissive, 1, (const float *)&m.Emissive);
	}
	if (prog->uGlobalAmbient >= 0) {
		argbToFloats(dev->getRenderState(D3DRS_AMBIENT), c);
		glUniform4fv(prog->uGlobalAmbient, 1, c);
	}
	if (prog->uNumLights >= 0) {
		int types[4] = {0, 0, 0, 0};
		float dirs[12] = {0}, poss[12] = {0}, diff[16] = {0}, amb[16] = {0}, att[16] = {0};
		int n = 0;
		for (unsigned i = 0; i < WebGLDevice::kMaxLights && n < 4; i++) {
			if (!dev->isLightEnabled(i)) continue;
			const D3DLIGHT8 &L = dev->getLight(i);
			types[n] = (L.Type == D3DLIGHT_POINT) ? 1 : 0;
			dirs[n * 3 + 0] = L.Direction.x;
			dirs[n * 3 + 1] = L.Direction.y;
			dirs[n * 3 + 2] = L.Direction.z;
			poss[n * 3 + 0] = L.Position.x;
			poss[n * 3 + 1] = L.Position.y;
			poss[n * 3 + 2] = L.Position.z;
			memcpy(&diff[n * 4], &L.Diffuse, 16);
			memcpy(&amb[n * 4], &L.Ambient, 16);
			att[n * 4 + 0] = L.Range;
			att[n * 4 + 1] = L.Attenuation0 > 0 ? L.Attenuation0 : 1.0f;
			att[n * 4 + 2] = L.Attenuation1;
			att[n * 4 + 3] = L.Attenuation2;
			n++;
		}
		glUniform1i(prog->uNumLights, n);
		glUniform1iv(prog->uLightType, 4, types);
		glUniform3fv(prog->uLightDir, 4, dirs);
		glUniform3fv(prog->uLightPos, 4, poss);
		glUniform4fv(prog->uLightDiffuse, 4, diff);
		glUniform4fv(prog->uLightAmbient, 4, amb);
		glUniform4fv(prog->uLightAtten, 4, att);
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

// Sets up vertex attribute pointers for the currently bound ARRAY_BUFFER.
static void setupAttribs(const FVFLayout &l, unsigned stride, intptr_t base)
{
	for (int i = 0; i < 8; i++) glDisableVertexAttribArray(i);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, l.xyzrhw ? 4 : 3, GL_FLOAT, GL_FALSE, stride, (const void *)(base + l.posOffset));
	if (l.hasNormal) {
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void *)(base + l.normalOffset));
	}
	if (l.hasDiffuse) {
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (const void *)(base + l.diffuseOffset));
	}
	if (l.hasSpecular) {
		glEnableVertexAttribArray(3);
		glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (const void *)(base + l.specularOffset));
	}
	const int texIn = l.texCount > 2 ? 2 : l.texCount;
	for (int i = 0; i < texIn; i++) {
		glEnableVertexAttribArray(4 + i);
		glVertexAttribPointer(4 + i, l.texSize[i], GL_FLOAT, GL_FALSE, stride, (const void *)(base + l.texOffset[i]));
	}
}

void WebGLPipeline::drawCommon(WebGLDevice *dev, unsigned primType, unsigned primCount,
                               const uint8_t * /*vertexBase*/, unsigned stride, unsigned fvf,
                               const uint8_t * /*indexBase*/, unsigned indexFormat,
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

	setupAttribs(l, stride, baseVertexBytes);

	const GLenum mode = primModeGL(primType);
	const unsigned count = primVertexCount(primType, primCount);
	if (indexFormat != 0) {
		const GLenum itype = (indexFormat == D3DFMT_INDEX32) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
		const unsigned isize = (indexFormat == D3DFMT_INDEX32) ? 4 : 2;
		glDrawElements(mode, count, itype, (const void *)(intptr_t)(startIndex * isize));
	} else {
		glDrawArrays(mode, startIndex, count);
	}
}

// Buffer objects (device-side shadow -> GL) helpers.
static void ensureVBUploaded(WebGLVertexBuffer *vb)
{
	if (vb->m_gl.name == 0) glGenBuffers(1, &vb->m_gl.name);
	glBindBuffer(GL_ARRAY_BUFFER, vb->m_gl.name);
	if (vb->m_gl.dirty) {
		glBufferData(GL_ARRAY_BUFFER, vb->m_bits.size(), vb->m_bits.data(), GL_DYNAMIC_DRAW);
		vb->m_gl.dirty = false;
	}
}

static void ensureIBUploaded(WebGLIndexBuffer *ib)
{
	if (ib->m_gl.name == 0) glGenBuffers(1, &ib->m_gl.name);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->m_gl.name);
	if (ib->m_gl.dirty) {
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, ib->m_bits.size(), ib->m_bits.data(), GL_DYNAMIC_DRAW);
		ib->m_gl.dirty = false;
	}
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
	drawCommon(dev, primType, primCount, nullptr, stride, fvf,
	           nullptr, ib->m_format, startIndex, baseBytes, numVertices);
}

void WebGLPipeline::draw(WebGLDevice *dev, unsigned primType, unsigned startVertex, unsigned primCount)
{
	if (!m_ctxReady) return;
	WebGLVertexBuffer *vb = dev->getStream0();
	if (!vb) return;

	ensureVBUploaded(vb);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	const unsigned fvf = dev->getFVF() ? dev->getFVF() : vb->m_fvf;
	const unsigned stride = dev->getStream0Stride();
	drawCommon(dev, primType, primCount, nullptr, stride, fvf,
	           nullptr, 0, startVertex, 0, 0);
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
	glBindBuffer(GL_ARRAY_BUFFER, m_upVBO);
	glBufferData(GL_ARRAY_BUFFER, (size_t)vcount * stride, vertexData, GL_STREAM_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	drawCommon(dev, primType, primCount, nullptr, stride, fvf, nullptr, 0, 0, 0, vcount);
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

	glBindBuffer(GL_ARRAY_BUFFER, m_upVBO);
	glBufferData(GL_ARRAY_BUFFER, (size_t)(minVertexIdx + numVertices) * stride, vertexData, GL_STREAM_DRAW);

	const unsigned isize = (indexFormat == D3DFMT_INDEX32) ? 4 : 2;
	const unsigned icount = primVertexCount(primType, primCount);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_upIBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)icount * isize, indexData, GL_STREAM_DRAW);

	drawCommon(dev, primType, primCount, nullptr, stride, fvf, nullptr, indexFormat, 0, 0, numVertices);
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
		// Same top-origin placement as the glViewport call in applyFixedState.
		glScissor((GLint)vp.X, (GLint)vp.Y, (GLsizei)vp.Width, (GLsizei)vp.Height);
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
		m_yFlip = -1.0f;
		return;
	}

	// The GL texture must exist before it can be an attachment.
	if (tex->m_gl.name == 0 || tex->m_gl.dirty) {
		uploadTexture(tex);
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
		m_yFlip = -1.0f;
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
	// GeneralsX @build Android port GLES experiment - the browser build never
	// needed an explicit swap (the canvas presents implicitly when the game
	// pthread yields back to its rAF loop tick). Android/EGL has no such
	// implicit hook, so this call is new, not adapted from upstream.
	if (m_window) {
		SDL_GL_SwapWindow(m_window);
	}
}
