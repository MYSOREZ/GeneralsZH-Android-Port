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
** gles_pipeline.h - the D3D8 fixed-function -> WebGL2 translation core.
**
** GeneralsX @build web-port 05/07/2026 - Web port Phase 2
**
** The device/resource classes in d3d8webgl.cpp keep full CPU-side shadow
** state (render states, stage states, transforms, lights, buffer and texture
** bytes). This pipeline consumes that state at draw time: it generates and
** caches GLSL ES 3.00 programs keyed on the fixed-function state that affects
** shading, uploads dirty resources, and issues the GL draws.
*/

#pragma once

#include <GLES3/gl3.h>
#include <cstdint>
#include <cstring>
#include <unordered_map>

typedef struct SDL_Window SDL_Window;
// Not "typedef void *SDL_GLContext" here: SDL3's real header (SDL_video.h)
// declares SDL_GLContext as "struct SDL_GLContextState *", and a forward
// declaration must match exactly or every TU that includes both this header
// and <SDL3/SDL.h> gets a typedef-redefinition error. m_glContext below is
// plain void* instead; gles_pipeline.cpp (which does include <SDL3/SDL.h>)
// casts to/from the real SDL_GLContext where needed.

class WebGLDevice;
class WebGLTexture;
class WebGLVertexBuffer;
class WebGLIndexBuffer;
struct FVFLayout; // defined in gles_pipeline.cpp, only used by-reference here

// GL side of a texture: one GL object, recreated when the shadow bits change.
struct GLTextureState {
	GLuint name = 0;
	bool dirty = true;          // shadow bits changed since last upload
	uint32_t samplerKey = ~0u;  // last-applied filter/wrap state
	GLuint fbo = 0;             // lazily created when used as a render target
};

// GL side of a VB/IB.
struct GLBufferState {
	GLuint name = 0;
	bool dirty = true;
};

class WebGLPipeline {
public:
	static WebGLPipeline *get(); // created on first use (game pthread)

	// Context management. Returns false if GLES3 is unavailable.
	bool initContext(int backbufferWidth, int backbufferHeight, SDL_Window *window);
	void resize(int backbufferWidth, int backbufferHeight);
	bool ready() const { return m_ctxReady; }

	// D3D entry points (called from WebGLDevice with `this` device state).
	void clear(WebGLDevice *dev, unsigned flags, uint32_t argbColor, float z, unsigned stencil);
	void drawIndexed(WebGLDevice *dev, unsigned primType, unsigned minIndex,
	                 unsigned numVertices, unsigned startIndex, unsigned primCount);
	void draw(WebGLDevice *dev, unsigned primType, unsigned startVertex, unsigned primCount);
	void drawUP(WebGLDevice *dev, unsigned primType, unsigned primCount,
	            const void *vertexData, unsigned stride);
	void drawIndexedUP(WebGLDevice *dev, unsigned primType, unsigned minVertexIdx,
	                   unsigned numVertices, unsigned primCount,
	                   const void *indexData, unsigned indexFormat,
	                   const void *vertexData, unsigned stride);
	void present();

	// Render-target switch: tex==nullptr selects the canvas backbuffer.
	void setRenderTarget(WebGLDevice *dev, WebGLTexture *tex);

	bool hasS3TC() const { return m_hasS3TC; }

	// GeneralsX @build Android port GLES experiment - GL deletes a texture's
	// binding on every unit as a side effect of glDeleteTextures (the GL spec
	// guarantees this), but m_lastBoundTex doesn't see that -- and GL names
	// are commonly recycled by the driver, so a later, unrelated texture can
	// receive the exact name this cache still has recorded as "already bound"
	// for a stage. ~WebGLTexture() (d3d8gles.cpp) must call this right after
	// glDeleteTextures so a stale hit can never skip a real bind and leave
	// the wrong texture sampled.
	void invalidateTextureBinding(GLuint name);

	// Same rationale, covering both the GL_ARRAY_BUFFER skip cache
	// (m_lastArrayBuffer below) and the VAO cache (a cached VAO can name a
	// deleted VBO/IBO in its key, with the same reused-GL-name hazard):
	// ~WebGLVertexBuffer()/~WebGLIndexBuffer() (d3d8gles.cpp) must call this
	// right after glDeleteBuffers.
	void invalidateBufferBinding(GLuint name);

private:
	WebGLPipeline() = default;

	struct ProgramInfo;

	// Draw guts shared by the buffer and UP paths. vbo/ibo are explicit GL
	// object names (not read off some "currently bound" global) precisely
	// because with the VAO cache below, no per-draw call is guaranteed to
	// leave GL_ARRAY_BUFFER/GL_ELEMENT_ARRAY_BUFFER pointed at them -- a
	// cache hit skips touching those bindings entirely, and content uploads
	// (ensureVBUploaded/ensureIBUploaded) go through GL_COPY_WRITE_BUFFER,
	// never GL_ARRAY_BUFFER/GL_ELEMENT_ARRAY_BUFFER, for the same reason.
	void drawCommon(WebGLDevice *dev, unsigned primType, unsigned primCount,
	                GLuint vbo, unsigned stride, unsigned fvf,
	                GLuint ibo, unsigned indexFormat,
	                unsigned startIndex, int baseVertexBytes, unsigned vertexCount);

	ProgramInfo *getProgram(WebGLDevice *dev, unsigned fvf);
	void applyFixedState(WebGLDevice *dev);
	void applyUniforms(WebGLDevice *dev, ProgramInfo *prog, unsigned fvf);
	void ensureVBUploaded(WebGLVertexBuffer *vb);
	void ensureIBUploaded(WebGLIndexBuffer *ib);

	// GeneralsX @build Android port GLES experiment - perf pass. Only
	// GL_ARRAY_BUFFER gets a redundant-bind-skip cache: it is not part of
	// any VAO's state (a plain, VAO-independent global target, only ever
	// consulted transiently by glVertexAttribPointer/glBufferData), so
	// "was this name last bound here" is always a safe question to ask.
	// GL_ELEMENT_ARRAY_BUFFER is the opposite -- the GL/GLES spec makes it
	// part of *each* VAO's own state, so a single global "last bound"
	// answer can't say whether the *currently bound* VAO already has a
	// given index buffer captured; it is always bound unconditionally,
	// only from bindVertexLayout()'s VAO-creation path below. ~0u is an
	// impossible GL name, used as "unknown/force a real bind" the same way
	// m_lastBoundTex uses it for the RT-invalidation case; 0 is a real,
	// valid "unbound" state so it must round-trip too.
	GLuint m_lastArrayBuffer = ~0u;
	void bindArrayBuffer(GLuint name);

	// GeneralsX @build Android port GLES experiment - perf pass. No VAOs
	// existed anywhere in this backend: setupAttribs() ran in full (8x
	// glDisableVertexAttribArray, then glEnableVertexAttribArray +
	// glVertexAttribPointer for each active attribute) on every single draw,
	// unconditionally -- the one hot-path function bb4d069/this pass's other
	// caches never touched. A VAO fully captures that state (plus, per the
	// GL/GLES spec, the current GL_ELEMENT_ARRAY_BUFFER binding) once, keyed
	// on the exact combination that determines it: the VBO/IBO GL object
	// names (passed in explicitly by drawCommon's caller), the FVF (fully
	// determines the parsed layout), the stride, and the base-vertex byte
	// offset. Repeat draws using the same combination -- the overwhelmingly
	// common case, e.g. one mesh's VB/IB drawn every frame -- become a
	// single glBindVertexArray, or nothing at all if the same VAO is
	// already bound from the previous draw.
	struct VAOKey {
		GLuint vbo = 0;
		GLuint ibo = 0; // 0 for the non-indexed draw() path
		unsigned fvf = 0;
		unsigned stride = 0;
		int base = 0;

		bool operator==(const VAOKey &o) const {
			return memcmp(this, &o, sizeof(VAOKey)) == 0;
		}
	};
	// GeneralsX @build Android port GLES experiment - real device logs (a
	// battle scene, not just menus) showed draws/frame up to ~1250 and this
	// cache saturating a 256-entry cap within the first ~10 seconds --
	// unlike kMaxPrograms above, where 256 comfortably covers every
	// realistic shader permutation, real content needs one VAO per unique
	// *mesh instance/offset*, not per shader, and that count is nowhere
	// near as bounded. A fixed array with a linear scan doesn't just stop
	// helping past its cap, it becomes actively worse than no cache at all
	// once every draw pays for scanning all 256 entries AND then falls
	// through to the uncached path anyway. A hash map removes the size-vs-
	// scan-cost tradeoff entirely -- lookup cost doesn't grow with how much
	// is cached, so kMaxVAOs below exists only as a sanity backstop against
	// unbounded growth (e.g. content that draws through an ever-advancing
	// dynamic vertex-buffer-pool offset, where every draw is a genuinely
	// new base-vertex value), not a real expected ceiling. Keyed by a
	// byte-wise FNV-1a hash of VAOKey (hashVAOKey() below), same
	// collision-tolerant style as computeProgramKey()'s FNV-1a program key
	// -- no verification against a stored raw key on lookup, matching that
	// existing precedent in this file. The full VAOKey is still kept
	// alongside the VAO purely so evictVAOsForBuffer() can scan for
	// vbo/ibo matches; eviction (buffer deletion) is rare, so an O(n) scan
	// there is fine even though it would not be on the hot lookup path.
	static const size_t kMaxVAOs = 16384;
	struct VAOCacheEntry {
		VAOKey key;
		GLuint vao;
	};
	std::unordered_map<uint64_t, VAOCacheEntry> m_vaoCache;
	bool m_haveLastVAOKey = false;
	VAOKey m_lastVAOKey{};
	int m_perfVAOCacheHits = 0;
	int m_perfVAOCacheMisses = 0;
	// A free function couldn't name VAOKey (private nested type); a static
	// member can, same as any other member.
	static uint64_t hashVAOKey(const VAOKey &k);
	void bindVertexLayout(const FVFLayout &l, GLuint vbo, GLuint ibo, unsigned fvf, unsigned stride, int base);
	// A VBO/IBO's GL name can be recycled by the driver after deletion (same
	// hazard as invalidateTextureBinding/invalidateBufferBinding above); a
	// cached VAO keyed on that name would otherwise wrongly match whatever
	// unrelated buffer gets the reused name next. Called from
	// invalidateBufferBinding(), not directly -- deleting a VB/IB always
	// means "forget every GL-side cache entry that named this buffer."
	void evictVAOsForBuffer(GLuint name);

	// GeneralsX @build Android port GLES experiment - perf pass. The ported
	// pipeline was correctness-first: every draw re-applied all fixed GL
	// state and re-uploaded every uniform unconditionally (see the original
	// header comment above), which is fine for a browser tech demo but is
	// real, measurable per-draw driver overhead for an RTS scene with many
	// draws per frame. FixedStateKey mirrors every D3D render-state value
	// applyFixedState() reads (plus the viewport); when consecutive draws
	// share the same key, the whole function body -- a dozen-plus
	// glEnable/glDisable/glBlendFunc/... calls -- is skipped entirely.
	struct FixedStateKey {
		DWORD zEnable, zWrite, zFunc, zBias;
		DWORD alphaBlend, srcBlend, destBlend;
		DWORD cullMode, colorWrite;
		DWORD stencilEnable, stencilFunc, stencilRef, stencilMask;
		DWORD stencilFail, stencilZFail, stencilPass, stencilWriteMask;
		int vpX, vpY, vpW, vpH;
		float vpMinZ, vpMaxZ;

		bool operator==(const FixedStateKey &o) const {
			return memcmp(this, &o, sizeof(FixedStateKey)) == 0;
		}
	};
	bool m_haveFixedStateKey = false;
	FixedStateKey m_lastFixedStateKey{};
	GLuint m_lastProgram = 0;
	int m_perfStateCacheHits = 0;
	int m_perfStateCacheMisses = 0;

	// GeneralsX @build Android port GLES experiment - perf pass. applyUniforms()
	// used to re-upload every uniform on every single draw, even on an
	// m_lastProgram cache hit -- ~15-25 glUniform* calls per draw that are
	// pure waste whenever the values feeding them didn't change since the
	// last draw. Unlike FixedStateKey above, this can't be one monolithic
	// key: uWorld changes on nearly every draw in real battlefield rendering
	// (each object has its own transform), so a single all-or-nothing key
	// would rarely hit. Split into independently-cached sub-blocks instead,
	// matching applyUniforms()'s own existing comment that each uniform can
	// be optimized out independently. uWorld itself is deliberately NOT
	// cached (see above) and is always uploaded directly.
	//
	// Every sub-key must be invalidated when the bound program changes --
	// uniform locations are per-program, so a hit against a key computed for
	// a DIFFERENT program would wrongly skip uploading to this one. That
	// invalidation happens in the one place program switches are already
	// detected, at the top of applyUniforms() (see m_lastProgram above).
	struct TransformKey { // view+proj+texture matrices; not uWorld, see above
		float view[16], proj[16], texMat0[16], texMat1[16];
		bool operator==(const TransformKey &o) const {
			return memcmp(this, &o, sizeof(TransformKey)) == 0;
		}
	};
	bool m_haveTransformKey = false;
	TransformKey m_lastTransformKey{};

	struct MiscUniformKey { // viewport, yFlip, texture-factor, alpha ref, fog
		float vpX, vpY, vpW, vpH;
		float yFlip;
		float tFactor[4];
		float alphaRef;
		float fogColor[4];
		float fogStart, fogEnd;
		bool operator==(const MiscUniformKey &o) const {
			return memcmp(this, &o, sizeof(MiscUniformKey)) == 0;
		}
	};
	bool m_haveMiscKey = false;
	MiscUniformKey m_lastMiscKey{};

	struct MaterialKey { // VertexMaterialClass diffuse/ambient/emissive
		float diffuse[4], ambient[4], emissive[4];
		bool operator==(const MaterialKey &o) const {
			return memcmp(this, &o, sizeof(MaterialKey)) == 0;
		}
	};
	bool m_haveMaterialKey = false;
	MaterialKey m_lastMaterialKey{};

	struct LightingKey { // global ambient + up to 4 active lights
		float globalAmbient[4];
		int numLights;
		int types[4];
		float dirs[12], poss[12], diff[16], amb[16], att[16];
		bool operator==(const LightingKey &o) const {
			return memcmp(this, &o, sizeof(LightingKey)) == 0;
		}
	};
	bool m_haveLightingKey = false;
	LightingKey m_lastLightingKey{};
	// One combined hit/miss counter across all four sub-blocks above, logged
	// by present() the same way m_perfStateCacheHits/Misses is -- a per-block
	// breakdown would be more precise but four more numbers in an
	// already-dense perf line is not worth it for what is fundamentally one
	// question: "is the uniform cache doing anything."
	int m_perfUniformCacheHits = 0;
	int m_perfUniformCacheMisses = 0;

	// GeneralsX @build Android port GLES experiment - same redundant-state
	// rationale as m_lastProgram above, applied to bindTextures()'s two
	// texture stages: the D3D-era engine calls SetTexture() before every
	// draw regardless of whether the stage's texture actually changed (the
	// same "reapply unconditionally" style that motivated FixedStateKey and
	// m_lastProgram), so consecutive draws sharing a material/texture -- a
	// terrain tile batch, a run of UI glyphs off the same font sheet, a
	// string of units using the same skin -- previously re-issued
	// glBindTexture per stage per draw for no reason. 0 doubles as "no GL
	// texture bound" for both an unset stage and an explicitly-unbound one,
	// which is fine since both cases want the same skip-if-unchanged
	// behavior. Only the bind itself is skipped; glActiveTexture still runs
	// unconditionally so applySamplerState (called right after) always has
	// the correct unit current if it needs to touch sampler parameters.
	GLuint m_lastBoundTex[2] = {0, 0};

	// Perf counters logged once every couple of seconds by present(), not
	// per frame -- draws/frame and cache hit rate are the numbers that
	// actually say whether the state-cache above is doing anything, instead
	// of guessing from feel alone.
	int m_perfDrawsThisFrame = 0;
	int m_perfFrameCount = 0;
	int m_perfDrawAccum = 0;
	unsigned m_perfLogLastMs = 0;
	void bindTextures(WebGLDevice *dev, ProgramInfo *prog);
	void uploadTexture(WebGLTexture *tex);
	void applySamplerState(WebGLDevice *dev, unsigned stage, WebGLTexture *tex);

	uint64_t computeProgramKey(WebGLDevice *dev, unsigned fvf) const;

	bool m_ctxReady = false;
	bool m_hasS3TC = false;
	int m_fbWidth = 0;
	int m_fbHeight = 0;
	SDL_Window *m_window = nullptr;
	void *m_glContext = nullptr; // really an SDL_GLContext, see the comment above

	// Current render target (FBO rendering for SetRenderTarget).
	GLuint m_curFBO = 0;
	int m_curRTWidth = 0;
	int m_curRTHeight = 0;
	float m_yFlip = 1.0f; // +1 backbuffer (flip), -1 FBO (no flip)
	GLuint m_depthRB = 0; // shared depth-stencil renderbuffer for FBOs
	int m_depthRBW = 0, m_depthRBH = 0;

	// Streaming buffers for the UP draw paths.
	GLuint m_upVBO = 0;
	GLuint m_upIBO = 0;

	// Program cache: key -> program.
	static const int kMaxPrograms = 256;
	struct CacheEntry {
		uint64_t key;
		ProgramInfo *prog;
	};
	CacheEntry m_programs[kMaxPrograms];
	int m_programCount = 0;

	unsigned m_frame = 0;
};
