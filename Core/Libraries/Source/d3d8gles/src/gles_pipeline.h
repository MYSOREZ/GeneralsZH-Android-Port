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

private:
	WebGLPipeline() = default;

	struct ProgramInfo;

	// Draw guts shared by the buffer and UP paths.
	void drawCommon(WebGLDevice *dev, unsigned primType, unsigned primCount,
	                const uint8_t *vertexBase, unsigned stride, unsigned fvf,
	                const uint8_t *indexBase, unsigned indexFormat,
	                unsigned startIndex, int baseVertexBytes, unsigned vertexCount);

	ProgramInfo *getProgram(WebGLDevice *dev, unsigned fvf);
	void applyFixedState(WebGLDevice *dev);
	void applyUniforms(WebGLDevice *dev, ProgramInfo *prog, unsigned fvf);

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
