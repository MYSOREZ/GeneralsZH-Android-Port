// GeneralsX @performance Android port 30/09/2026 A render thread for the native GLES backend.
//
// In the heaviest reproducible scene on the old Mali test phone a frame was ~26 ms, split almost
// evenly between the engine building the scene (~9 ms), this translator's GL calls (~9 ms, ~6 of
// them inside the driver) and the swap (~2 ms), one after the other on one thread
// ([GX-PERF-SCENE-DRAWS], logs-29). With the GL calls and the swap on a second thread the two
// halves overlap: the engine builds frame N+1 while the driver consumes frame N.
//
// Every GL call this library makes already goes through gles_dispatch.cpp (and the few extension
// entry points through WebGLPipeline), so the thread sits exactly there. On the engine's thread a
// GL call becomes a small command in a single-producer/single-consumer ring; the render thread,
// which owns the GL context, runs them in order. A call whose result the caller needs (glGet*,
// glCreate*, a map, a readback) waits until the render thread has run everything before it and
// then the call itself. Pointer arguments are copied into the command, so the caller may reuse
// its memory at once, exactly as GL allows.
//
// Nothing here is seen by game logic: it changes when GL calls run, not which ones or in what
// order. Off with gx_gles_noopt.txt containing "thread".
#pragma once

#include <GLES3/gl3.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

struct SDL_Window;

namespace gxrt {

// True while GL calls from the engine's thread are queued for the render thread. Read and
// written on the engine's thread only.
extern bool g_active;

struct Cmd
{
	void (*run)(Cmd *); // nullptr: padding up to the end of the ring
	uint32_t bytes;
};

// Engine thread: room for one command of `bytes`, rounded up; waits while the ring is full.
void *allocCmd(size_t bytes, uint32_t *rounded);
// Engine thread: make the command just written visible to the render thread.
void commitCmd();
// Engine thread: run fn(ctx) on the render thread after everything queued before it, and wait.
void syncCall(void (*fn)(void *), void *ctx);

template <class F>
struct CmdT : Cmd
{
	F f;
	explicit CmdT(F &&fn) : f(std::move(fn)) {}
	static void exec(Cmd *c)
	{
		CmdT *t = static_cast<CmdT *>(c);
		t->f();
		t->~CmdT();
	}
};

// Queue f for the render thread, or run it now when the render thread is not in use.
template <class F>
inline void post(F f)
{
	if (!g_active) {
		f();
		return;
	}
	typedef CmdT<F> T;
	uint32_t rounded = 0;
	void *mem = allocCmd(sizeof(T), &rounded);
	T *t = new (mem) T(std::move(f));
	t->run = &T::exec;
	t->bytes = rounded;
	commitCmd();
}

// Run f on the render thread and wait for it, or run it now when the render thread is not in use.
template <class F>
inline void sync(F f)
{
	if (!g_active) {
		f();
		return;
	}
	syncCall([](void *ctx) { (*static_cast<F *>(ctx))(); }, &f);
}

// A copy of a pointer argument, owned by the command that carries it. Small copies live inside
// the command itself; larger ones on the heap, freed on the render thread after the call.
// A null source stays null.
template <size_t N>
struct Blob
{
	uint32_t size = 0;
	bool present = false;
	unsigned char *heap = nullptr;
	alignas(16) unsigned char inl[N > 0 ? N : 1];

	Blob(const void *src, size_t bytes)
	{
		if (src == nullptr)
			return;
		present = true;
		size = (uint32_t)bytes;
		unsigned char *dst = inl;
		if (bytes > N) {
			heap = static_cast<unsigned char *>(malloc(bytes));
			dst = heap;
		}
		if (bytes > 0)
			memcpy(dst, src, bytes);
	}
	Blob(Blob &&o) : size(o.size), present(o.present), heap(o.heap)
	{
		if (heap == nullptr && present && size > 0)
			memcpy(inl, o.inl, size);
		o.heap = nullptr;
	}
	Blob(const Blob &) = delete;
	Blob &operator=(const Blob &) = delete;
	~Blob() { free(heap); }
	const void *data() const { return present ? (heap ? heap : inl) : nullptr; }
};

// Starts the render thread and hands it the GL context current on the calling thread. Returns
// false (and leaves everything on the calling thread) when the thread could not take it.
bool start(SDL_Window *window);
// Drains the queue, stops the render thread and gives the context back to the engine's thread.
void stop();

// The swap, queued behind the frame's GL calls. At most one frame is in flight: presenting
// frame N+1 first waits for frame N's swap. swapInterval < 0 leaves vsync as it is.
void present(SDL_Window *window, int swapInterval);

// Buffer writes that would otherwise need a round trip for glMapBufferRange: the render thread
// maps, copies and unmaps (or falls back to glBufferSubData). The buffer bound to `target` at
// that point in the command stream is the one written.
void bufferWrite(GLenum target, GLintptr offset, GLsizeiptr length, const void *data, GLbitfield access);

// glGetError for code already running on the GL thread (inside a posted command), where the
// ordinary entry point would queue behind itself.
GLenum rawGetError();
void rawGetIntegerv(GLenum pname, GLint *data);

// Fences created by the engine's thread before the render thread has issued them. The handle is
// a proxy; the render thread creates the real fence in order and polls it while idle.
typedef GLsync (GL_APIENTRY *PFN_FenceSync)(GLenum condition, GLbitfield flags);
typedef GLenum (GL_APIENTRY *PFN_ClientWaitSync)(GLsync sync, GLbitfield flags, GLuint64 timeout);
typedef void (GL_APIENTRY *PFN_DeleteSync)(GLsync sync);
void setFenceProcs(PFN_FenceSync fence, PFN_ClientWaitSync wait, PFN_DeleteSync del);
GLsync fenceSync();
GLenum clientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
void deleteSync(GLsync sync);

// Per-frame counters for [d3d8gles] perf-thread; reset by the caller once reported.
struct Stats
{
	double workerBusyUs = 0.0;  // render thread running commands
	double frameWaitUs = 0.0;   // engine thread waiting for the previous frame's swap
	double syncWaitUs = 0.0;    // engine thread waiting for a call's result
	double ringWaitUs = 0.0;    // engine thread waiting for room in the ring
	unsigned syncCalls = 0;
	unsigned commands = 0;
	double commandBytes = 0.0;
};
Stats takeStats();
bool running();

} // namespace gxrt
