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
#ifndef GL_MAP_PERSISTENT_BIT_EXT
#define GL_MAP_PERSISTENT_BIT_EXT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT_EXT
#define GL_MAP_COHERENT_BIT_EXT 0x0080
#endif
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

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
	// GeneralsX @performance Android port 30/09/2026 Which depth renderbuffer (by generation, since
	// GL may reuse a deleted name) was last attached to fbo, and
	// the completeness the driver reported for that combination. glCheckFramebufferStatus returns
	// a value, so with the render thread it waits for every queued call; it is asked only when
	// an attachment actually changed, not on every render-target switch.
	GLuint fboDepthGen = 0;
	GLenum fboStatus = 0;
};

// GeneralsX @performance Android port 29/09/2026 Persistently mapped storage for a dynamic VB/IB
// (EXT_buffer_storage): a few GL buffers, each mapped once for good, so an append is a memcpy and
// no GL call at all. D3DLOCK_DISCARD moves to a copy the GPU has finished with (fenced) instead
// of respecifying storage. See WebGLPipeline::persistentUpload().
struct PersistentBufferSet {
	static const int kMaxCopies = 8;
	GLuint names[kMaxCopies] = {};
	unsigned char *ptrs[kMaxCopies] = {};
	GLsync fences[kMaxCopies] = {};
	int count = 0;
	int cur = -1;
	size_t size = 0;
};

// GL side of a VB/IB.
struct GLBufferState {
	GLuint name = 0;
	bool dirty = true;
	// GeneralsX @perf Android port 09/05/2026 Byte range actually written since
	// the last upload, accumulated across Lock/Unlock pairs (see
	// WebGLVertexBuffer::Lock/Unlock in d3d8gles.cpp). ensureVBUploaded/
	// ensureIBUploaded push only this range instead of respecifying the whole
	// buffer. begin >= end means "nothing recorded" -> fall back to a full
	// upload. `allocated` tracks whether GL storage exists yet, since
	// glBufferSubData needs a sized buffer to write into.
	size_t dirtyBegin = (size_t)-1;
	size_t dirtyEnd = 0;
	bool allocated = false;
	// GeneralsX @perf Android port 09/05/2026 Set when the engine locked with
	// D3DLOCK_DISCARD (dx8vertexbuffer.cpp uses it at ring offset 0, and
	// NOOVERWRITE for the appends after it). Translated as buffer orphaning:
	// glBufferData(..., nullptr) hands back a fresh block, so neither that
	// upload nor the NOOVERWRITE appends that follow in the same ring cycle
	// have to wait on the GPU still reading the old contents.
	bool pendingDiscard = false;
	// GeneralsX @bugfix Android port 30/09/2026 Set when a lock since the last upload had neither
	// DISCARD nor NOOVERWRITE. Such an update must not be written unsynchronized: D3D makes a plain
	// lock wait for (or rename away from) draws still reading the buffer. Written that way, bytes
	// a previous frame's draw was still reading changed under it -- long stretched triangles and
	// passes vanishing for a frame, hidden until the render thread let the CPU get ahead of the GPU.
	bool pendingSync = false;
	// GeneralsX @performance Android port 29/09/2026 One past the highest byte the engine has
	// ever written (0 = nothing tracked yet). A full upload only needs [0, writtenEnd): bytes
	// beyond it were never written, so no draw can reference them.
	size_t writtenEnd = 0;
	PersistentBufferSet *persistent = nullptr; // set once a dynamic buffer takes the persistent path
	// Persistent path only: one past the highest byte of the current copy that a draw issued since
	// the copy was selected may read. Writing below it is a hazard (see persistentUpload()).
	size_t gpuRefEnd = 0;
	void noteGpuRead(size_t end, size_t size)
	{
		if (persistent == nullptr) return;
		if (end > size) end = size;
		if (end > gpuRefEnd) gpuRefEnd = end;
	}

	void markRange(size_t begin, size_t end)
	{
		dirty = true;
		if (end <= begin) return;
		if (begin < dirtyBegin) dirtyBegin = begin;
		if (end > dirtyEnd) dirtyEnd = end;
		if (end > writtenEnd) writtenEnd = end;
	}
	void clearRange()
	{
		dirtyBegin = (size_t)-1;
		dirtyEnd = 0;
	}
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

	// GeneralsX @bugfix Android port 09/05/2026 Pull a render-target texture's
	// GPU contents back into its CPU shadow bits. Everything else in this
	// backend treats the CPU bits as the source of truth and pushes them to
	// GL; a render target is the one case where GL holds content the CPU side
	// has never seen. CopyRects (the D3D8 surface->surface blit) is a plain
	// memcpy between shadow bits, so a blit whose SOURCE is a render target
	// copied zeroes -- see the call site in d3d8gles.cpp's CopyRects for the
	// visual bug that caused.
	void readbackRenderTarget(WebGLTexture *tex);

	// GeneralsX @feature Android port 09/09/2026 One-shot diagnostic: read a small
	// block out of a full-screen render target and print what is actually in it.
	// The scripted screen filters (W3DShaderManager) render the scene into a
	// full-screen render target and then sample it back as a texture; when that
	// came out black on a device there was no way to tell an empty render target
	// from a bad combiner without a number. Prints at most a few times per launch
	// and ignores every render target that is not backbuffer-sized (water
	// reflections, projected shadows).
	void debugSampleRenderTarget(WebGLTexture *tex, const char *tag);

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
	void fullBufferUpload(GLBufferState &gl, const unsigned char *bits, size_t size, int kind);
	// Persistent path for dynamic buffers; FALSE when unavailable (the caller then uploads as before).
	bool persistentUpload(GLBufferState &gl, const unsigned char *bits, size_t size, bool isIndex);
	// Deletes a buffer's GL storage, persistent copies included (VB/IB destructors).
	void releaseBufferStorage(GLBufferState &gl);

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
	// baseVertexIndex is the same offset as baseVertexBytes, in vertices: with
	// glDrawElementsBaseVertex available the offset goes into the draw call and the VAO's
	// attribute pointers stay at 0 (see m_glDrawElementsBaseVertex).
	void drawCommon(WebGLDevice *dev, unsigned primType, unsigned primCount,
	                GLuint vbo, unsigned stride, unsigned fvf,
	                GLuint ibo, unsigned indexFormat,
	                unsigned startIndex, int baseVertexBytes, unsigned vertexCount,
	                int baseVertexIndex = 0);

	ProgramInfo *getProgram(WebGLDevice *dev, unsigned fvf);
public:
	// GeneralsX @feature Android port 01/10/2026 Virtual backbuffer for rendering below the window's
	// resolution, stretched to the window at present() with SGSR or bilinear (see
	// d3d8gles_SetVirtualBackbuffer). w or h <= 0 turns it off.
	bool setVirtualBackbuffer(int w, int h, int renderW, int renderH, bool gsr);
	bool upscaleSceneNow();
private:
	void stretchVirtualBackbuffer();
	// After upscaleSceneNow() "the backbuffer" is the window itself until the frame is presented.
	GLuint backbufferFBO() const { return (m_vbActive && !m_vbUpscaled && !m_vbBypass) ? m_vbFBO : 0; }
	// The viewport to apply: the device's, except that one larger than the virtual backbuffer
	// (the engine still knows the window's size) covers the whole virtual backbuffer instead.
	const D3DVIEWPORT8 &effectiveViewport(WebGLDevice *dev);
	// GeneralsX @performance Android port 01/10/2026 A rectangle in the current target's pixels (the
	// engine's, top-left origin) as GL wants it (bottom-left origin), scaled down to the virtual
	// backbuffer's render size while that is the target. Every glViewport and glScissor goes
	// through here; the shaders keep working in the engine's pixels (uViewportPos).
	void targetRect(const D3DVIEWPORT8 &vp, GLint *x, GLint *y, GLsizei *w, GLsizei *h) const;
	bool m_vbActive = false;
	bool m_vbGsr = false;
	bool m_vbUpscaled = false; // this frame's scene is already in the window (upscaleSceneNow)
	// GeneralsX @feature Android port 02/10/2026 A frame with no 3D scene in it (loading screens,
	// videos, menus over no battle) is drawn straight into the window at full resolution: there is
	// no scene to upscale, and stretched whole its text came out pixelated (owner report, loading
	// screen). Decided per frame from the frame before: m_vbSceneSeen is set by upscaleSceneNow().
	bool m_vbBypass = false;
	bool m_vbSceneSeen = false;
	int m_vbW = 0, m_vbH = 0;   // what the engine sees (its backbuffer)
	int m_vbRW = 0, m_vbRH = 0; // what is rendered: the texture stretched at present
	int m_winW = 0, m_winH = 0;
	GLuint m_vbFBO = 0, m_vbTex = 0, m_vbDepth = 0;
	D3DVIEWPORT8 m_vbFullVp = {};
	GLuint m_presentPlainProg = 0, m_presentGsrProg = 0, m_presentVAO = 0;
	GLint m_presentPlainTex = -1, m_presentGsrTex = -1, m_presentGsrInfo = -1;
	bool m_presentGsrTried = false;
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
	// caches never touched. A VAO captures that state (plus, per the
	// GL/GLES spec, the current GL_ELEMENT_ARRAY_BUFFER binding) once, keyed
	// on the combination that determines the *enabled-attribute set*: the
	// VBO/IBO GL object names (passed in explicitly by drawCommon's caller)
	// and the FVF (fully determines the parsed layout and its stride).
	//
	// Deliberately NOT keyed on the base-vertex byte offset, unlike this
	// cache's first version: a real device log (a battle scene, not even a
	// real skirmish) showed draws/frame up to ~1250 and this cache growing
	// by thousands of entries within seconds, one new VAO -- and GL object
	// -- for nearly every draw. Root cause: a lot of this engine's content
	// draws through one shared/dynamic vertex-buffer pool (DX8Wrapper's
	// BUFFER_TYPE_DYNAMIC_DX8) whose base-vertex offset advances on
	// practically every call, so folding it into the key meant that class
	// of content got treated as brand-new every single time, defeating the
	// cache and leaking VAOs for the session's lifetime. `base` is still
	// tracked (VAOCacheEntry::lastBase, m_lastVAOBase below) since a VAO's
	// *pointers* do encode it and must stay current -- see
	// enableAttribs()/setAttribPointers() in the .cpp for the split this
	// enables: attribute enable/disable state only needs setting once per
	// VAO (a fresh one starts fully disabled), pointers get reissued
	// whenever `base` changes for an otherwise-identical, already-cached
	// VAO -- a handful of glVertexAttribPointer calls, not a new GL object
	// plus the full disable/enable/pointer dance every time.
	struct VAOKey {
		GLuint vbo = 0;
		GLuint ibo = 0; // 0 for the non-indexed draw() path
		unsigned fvf = 0;
		unsigned stride = 0;

		bool operator==(const VAOKey &o) const {
			return memcmp(this, &o, sizeof(VAOKey)) == 0;
		}
	};
	// Keyed by a byte-wise FNV-1a hash of VAOKey (hashVAOKey() below), same
	// collision-tolerant style as computeProgramKey()'s FNV-1a program key
	// -- no verification against a stored raw key on lookup, matching that
	// existing precedent in this file. Lookup cost doesn't grow with how
	// much is cached, so kMaxVAOs below is a sanity backstop against
	// pathological growth (now bounded by unique (vbo,ibo,fvf,stride)
	// combos rather than every base-vertex value ever seen -- expected to
	// be a much smaller, stable number once a level's content has been
	// drawn once), not a real expected ceiling.
	static const size_t kMaxVAOs = 16384;
	struct VAOCacheEntry {
		VAOKey key; // kept only so evictVAOsForBuffer() can scan for vbo/ibo matches
		GLuint vao;
		int lastBase; // base-vertex offset currently baked into this VAO's attribute pointers
	};
	std::unordered_map<uint64_t, VAOCacheEntry> m_vaoCache;
	bool m_haveLastVAOKey = false;
	VAOKey m_lastVAOKey{};
	int m_lastVAOBase = 0;
	int m_perfVAOCacheHits = 0;
	int m_perfVAOCacheMisses = 0;
	// Of the hits above, how many still needed a base-vertex pointer
	// refresh (see bindVertexLayout()'s comment) -- the residual per-draw
	// cost a dynamic/shared vertex-buffer pool leaves behind even once its
	// VAO object itself is being fully reused.
	int m_perfVAOPointerRefresh = 0;
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
		DWORD twoSided, stencilBackPass; // d3d8gles_SetTwoSidedStencil(), not a D3D8 state
		int vpX, vpY, vpW, vpH;
		float vpMinZ, vpMaxZ;

		bool operator==(const FixedStateKey &o) const {
			return memcmp(this, &o, sizeof(FixedStateKey)) == 0;
		}
	};
	bool m_haveFixedStateKey = false;
	FixedStateKey m_lastFixedStateKey{};
	DWORD m_lastSentSrcBlend = 0, m_lastSentDestBlend = 0; // glBlendFunc as last sent (applyFixedState)
	GLint m_lastSentViewport[4] = {0, 0, 0, 0};                // glViewport as last sent (it depends on the RT's height and scale)
	GLuint m_lastProgram = 0;
	int m_perfStateCacheHits = 0;
	int m_perfStateCacheMisses = 0;

	// GeneralsX @build Android port ANGLE experiment - a program-cache miss in
	// getProgram() means an actual glCreateProgram/glLinkProgram (this module
	// caches by full render-state key, so a new key = a genuinely new shader
	// variant). Under ANGLE's Vulkan backend this can mean synchronous
	// VkPipeline creation, which is far pricier than the equivalent state
	// change on a native GLES driver -- suspected cause of the multi-second
	// freeze reported when opening menus with many distinct widget/state
	// combinations (e.g. SkirmishGameOptionsMenu) for the first time. Counts
	// and total time are folded into the existing "[d3d8gles] perf: ..." line
	// in present() to confirm/refute this without guessing further.
	int m_perfProgramBuilds = 0;
	double m_perfProgramBuildUs = 0.0;

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
	// GeneralsX @build Android port GLES experiment 08/30/2026 Split from one
	// combined "TransformKey" (view+proj+texMat0+texMat1 as a single cache
	// slot) after a real device log's new per-block uniform-cache breakdown
	// showed the transform bucket collapsing to ~35% hit rate mid-battle,
	// far below vao-cache's 99.8%+ -- suspicious, since the camera
	// (view/proj) is set once per frame and every draw that frame should
	// share it. The likely culprit: per-object texture-stage transforms
	// (UV scroll/glow animations on individual units) invalidating the
	// WHOLE combined key on every draw that uses them, forcing a spurious
	// view/proj re-upload too even though the camera hadn't changed at all.
	// Splitting into two independent caches lets view/proj stay cached
	// across a whole frame regardless of what any one object's texMat is
	// doing -- can only raise the hit rate, never lower it, since it's the
	// same comparisons just no longer coupled together.
	struct ViewProjKey {
		float view[16], proj[16];
		bool operator==(const ViewProjKey &o) const {
			return memcmp(this, &o, sizeof(ViewProjKey)) == 0;
		}
	};
	bool m_haveViewProjKey = false;
	ViewProjKey m_lastViewProjKey{};

	struct TexMatKey {
		float texMat0[16], texMat1[16];
		bool operator==(const TexMatKey &o) const {
			return memcmp(this, &o, sizeof(TexMatKey)) == 0;
		}
	};
	bool m_haveTexMatKey = false;
	TexMatKey m_lastTexMatKey{};

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
	// GeneralsX @build Android port GLES experiment 08/30/2026 The combined
	// counters above answer "is the cache doing anything" but not "which of
	// the four blocks is dragging the average down" -- real device logs
	// (Redmi Note 8 Pro) showed the combined rate stuck around 74% while
	// vao-cache sits at 99.8%+, and there's no way to tell from that one
	// number whether the miss-heavy block is one that's genuinely
	// per-object-variant (material/lighting -- expected to miss a lot in a
	// battle with many differently-colored/lit units) or one that SHOULD be
	// near-constant within a frame (transform/misc -- a real inefficiency if
	// it's missing a lot). Split out so the next log settles which.
	int m_perfUniformViewProjHits = 0, m_perfUniformViewProjMisses = 0;
	int m_perfUniformTexMatHits = 0, m_perfUniformTexMatMisses = 0;
	int m_perfUniformMiscHits = 0, m_perfUniformMiscMisses = 0;
	int m_perfUniformMaterialHits = 0, m_perfUniformMaterialMisses = 0;
	int m_perfUniformLightingHits = 0, m_perfUniformLightingMisses = 0;

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
	unsigned m_depthRBGeneration = 0; // bumped whenever m_depthRB is recreated

	// GeneralsX @build Android port GLES experiment 08/30/2026 Camera
	// (view+proj) uniform buffer -- see kViewProjUBOBinding's comment in
	// gles_pipeline.cpp for why this exists instead of plain glUniform*
	// calls. Created once in initContext(), bound to kViewProjUBOBinding
	// for the whole session; only its DATA changes, via applyUniforms()'s
	// glBufferSubData when the CPU-side ViewProjKey actually differs from
	// what's currently uploaded.
	GLuint m_viewProjUBO = 0;

	// Streaming buffers for the UP draw paths.
	// Scratch for readbackRenderTarget(); a member so the 1 MB staging buffer
	// is allocated once instead of per shadow update.
	std::vector<uint8_t> m_rtReadback;
	GLuint m_upVBO = 0;
	GLuint m_upIBO = 0;

	// GeneralsX @performance Android port 27/09/2026 Techniques other D3D->GL translators rely
	// on (Valve's ToGL, WineD3D, ANGLE), each switchable off from the game folder so a
	// regression on one device can be isolated from a log without a new build: the file
	// gx_gles_noopt.txt disables every one of them when empty, or only the ones it names
	// (basevertex, upring, progcache, dxt565). See loadOptimizationSwitches() in the .cpp.
	struct OptimizationSwitches {
		// Off by default: on an Adreno 8xx (ES 3.2 core entry point) it broke stencil shadow
		// volumes into long streaks and made UI widgets flicker out, confirmed by switching it
		// alone off with gx_gles_noopt.txt. The likely mechanism is the driver's cached index
		// range going stale under the unsynchronized dynamic-buffer writes (see
		// LESSON-gles-dynamic-buffer-stalls.md). Opt in with gx_gles_basevertex.txt to test
		// it on another GPU.
		bool baseVertex = false;  // glDrawElementsBaseVertex instead of re-pointing attributes
		bool baseVertexOff = false; // gx_gles_noopt.txt turned it off: overrides the per-GPU default
		bool upRing = true;       // one streaming ring buffer for the *UP draws
		bool programCache = true; // linked program binaries kept on disk between launches
		bool dxt565 = true;       // DXT1 decoded to 16 bpp, not 32, where S3TC is missing
		bool persistent = true;   // dynamic VBs persistently mapped (EXT_buffer_storage)
		// Dynamic IBs too: off by default. With them mapped, the old Mali phone flickered on every
		// dynamic draw (UI, units, buildings, effects) while static-buffer terrain did not. The
		// driver scans an index buffer for the draw's index range and caches the answer until a
		// GL call modifies the buffer; a memcpy into a persistent mapping is not such a call, and
		// the dynamic IBs refill the same offsets every frame. Opt in with
		// gx_gles_persistentib.txt to test another GPU.
		bool persistentIB = false;
		// GL calls and the swap on a render thread (gles_thread.h), overlapping the driver's work
		// with the engine building the next frame.
		bool thread = true;
	};
	OptimizationSwitches m_opt;
	void loadOptimizationSwitches();

	typedef void (GL_APIENTRY *PFN_DrawElementsBaseVertex)(GLenum mode, GLsizei count, GLenum type,
		const void *indices, GLint basevertex);
	PFN_DrawElementsBaseVertex m_glDrawElementsBaseVertex = nullptr;
	typedef void (GL_APIENTRY *PFN_BufferStorage)(GLenum target, GLsizeiptr size, const void *data, GLbitfield flags);
	typedef GLsync (GL_APIENTRY *PFN_FenceSync)(GLenum condition, GLbitfield flags);
	typedef GLenum (GL_APIENTRY *PFN_ClientWaitSync)(GLsync sync, GLbitfield flags, GLuint64 timeout);
	typedef void (GL_APIENTRY *PFN_DeleteSync)(GLsync sync);
	PFN_BufferStorage m_glBufferStorage = nullptr;
	PFN_FenceSync m_glFenceSync = nullptr;
	PFN_ClientWaitSync m_glClientWaitSync = nullptr;
	PFN_DeleteSync m_glDeleteSync = nullptr;
	typedef void (GL_APIENTRY *PFN_StencilOpSeparate)(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
public:
	PFN_StencilOpSeparate m_glStencilOpSeparate = nullptr;
private:
	bool m_persistentOK = false;
	// GeneralsX @performance Android port 29/09/2026 Index stream for dynamic index buffers: each
	// indexed draw's indices are copied into the next unused bytes of one persistently mapped
	// buffer, and a full buffer is replaced by a new GL buffer object. See streamIndices().
	static const size_t kIndexStreamBytes = 4u << 20;
	GLuint m_indexStream = 0;
	unsigned char *m_indexStreamPtr = nullptr;
	size_t m_indexStreamOffset = 0;
	bool m_indexStreamFailed = false;
	int m_perfIndexStreamRenewals = 0;
	int m_perfWorldUploads = 0, m_perfWorldSkips = 0; // world matrix sent vs. already in the program
	int m_perfSyncUploads = 0; // appends from plain locks, uploaded with glBufferSubData
	int m_perfRangeUnderstated = 0; // indexed draws whose indices reach past minIndex+numVertices
	bool streamIndices(const void *src, size_t bytes, GLuint *name, size_t *offset);
	int m_perfPersistentSwitches = 0;
	int m_perfPersistentWaits = 0;
	int m_perfPersistentCopies = 0;
	int m_perfBaseVertexDraws = 0;

	// Streaming ring for DrawPrimitiveUP/DrawIndexedPrimitiveUP: appended to with an
	// unsynchronized map, orphaned only when it wraps -- the D3D "dynamic buffer" pattern
	// ToGL and WineD3D use, instead of two glBufferData respecifications per draw.
	static const size_t kUpRingVBBytes = 4u << 20;
	static const size_t kUpRingIBBytes = 1u << 20;
	GLuint m_upRingVB = 0;
	GLuint m_upRingIB = 0;
	size_t m_upRingVBOffset = 0;
	size_t m_upRingIBOffset = 0;
	// Returns the byte offset the data landed at, or (size_t)-1 when it cannot go through
	// the ring (larger than the ring itself); the caller then takes the one-off path.
	size_t streamToRing(GLuint buffer, size_t capacity, size_t *offset,
	                    const void *data, size_t bytes, size_t align);
	int m_perfUpRingDraws = 0;
	int m_perfUpRingWraps = 0;
	double m_perfUpRingBytes = 0.0;

	// Program binaries on disk (glGetProgramBinary/glProgramBinary, core in GLES 3.0): a
	// shader variant is compiled once per install instead of once per launch, which is
	// where the first-appearance hitches of an effect come from.
	typedef void (GL_APIENTRY *PFN_GetProgramBinary)(GLuint program, GLsizei bufSize, GLsizei *length,
		GLenum *binaryFormat, void *binary);
	typedef void (GL_APIENTRY *PFN_ProgramBinary)(GLuint program, GLenum binaryFormat,
		const void *binary, GLsizei length);
	typedef void (GL_APIENTRY *PFN_ProgramParameteri)(GLuint program, GLenum pname, GLint value);
	PFN_GetProgramBinary m_glGetProgramBinary = nullptr;
	PFN_ProgramBinary m_glProgramBinary = nullptr;
	PFN_ProgramParameteri m_glProgramParameteri = nullptr;
	std::string m_programCacheDir;
	uint64_t m_driverHash = 0;
	GLuint loadCachedProgram(uint64_t sourceHash);
	void saveCachedProgram(uint64_t sourceHash, GLuint program);
	int m_perfProgramCacheLoads = 0;
	int m_perfProgramCacheSaves = 0;

	// Render-target readbacks (glReadPixels, a full GPU drain each): counted and timed so a
	// log says whether this is a per-frame cost before anything replaces it.
	int m_perfRTReadbacks = 0;
	double m_perfRTReadbackUs = 0.0;

	int m_perfDxt16Levels = 0;
	double m_perfDxt16SavedBytes = 0.0;

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
