// GeneralsX @performance Android port 30/09/2026 The render thread behind gles_thread.h: the
// command ring, the thread that owns the GL context, frame pacing, fence proxies, and handing the
// context back to the engine's thread around Android's pause/resume (SDL backs the context up and
// restores it on the thread that pumps events, see android_egl_context_backup/restore).
#include "gles_thread.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

namespace gxrt {

bool g_active = false;

namespace {

typedef std::chrono::steady_clock Clock;

inline double usSince(Clock::time_point t)
{
	return std::chrono::duration<double, std::micro>(Clock::now() - t).count();
}

// 16 MB: a heavy frame is ~5000 commands of mostly 32-96 bytes; large payloads (buffer and texture
// data) are on the heap, so the ring holds several frames with room to spare.
constexpr size_t kRingBytes = 16u << 20;
constexpr size_t kAlign = 16;

unsigned char *s_ring = nullptr;
size_t s_allocPos = 0;                  // engine thread: next free byte (monotonic)
std::atomic<size_t> s_writePos{0};      // published end of the queued commands (monotonic)
std::atomic<size_t> s_readPos{0};       // render thread: end of the commands already run

std::thread s_thread;
std::atomic<bool> s_stop{false};
std::atomic<bool> s_hasContext{false};  // the render thread owns the context right now

// Render thread sleeping on s_workCv; the engine thread wakes it only then.
std::mutex s_workMutex;
std::condition_variable s_workCv;
std::atomic<bool> s_workerSleeping{false};

// Engine thread waiting on s_doneCv for a sync call, a swap or ring space.
std::mutex s_doneMutex;
std::condition_variable s_doneCv;
std::atomic<bool> s_mainWaiting{false};

std::atomic<uint64_t> s_framesPresented{0};
uint64_t s_framesQueued = 0;

SDL_Window *s_window = nullptr;
SDL_GLContext s_context = nullptr;
SDL_ThreadID s_mainThread = 0;
bool s_watchInstalled = false;

std::atomic<uint64_t> s_workerBusyNs{0};
std::atomic<uint64_t> s_workNs[kWorkKinds];
Stats s_stats; // engine-thread fields only

// Fence proxies: created in order on the render thread, polled by it while idle.
struct FenceProxy
{
	GLsync real = nullptr;
	std::atomic<int> state{0}; // 0 not yet issued, 1 issued, 2 signaled
};
PFN_FenceSync s_fenceSync = nullptr;
PFN_ClientWaitSync s_clientWaitSync = nullptr;
PFN_DeleteSync s_deleteSync = nullptr;
std::mutex s_fenceMutex;
std::vector<FenceProxy *> s_pendingFences; // issued, not yet seen signaled

void wakeMain()
{
	if (s_mainWaiting.load(std::memory_order_seq_cst)) {
		std::lock_guard<std::mutex> lock(s_doneMutex);
		s_doneCv.notify_all();
	}
}

void wakeWorker()
{
	if (s_workerSleeping.load(std::memory_order_seq_cst)) {
		std::lock_guard<std::mutex> lock(s_workMutex);
		s_workCv.notify_one();
	}
}

// Engine thread: wait until pred() holds, sleeping between the render thread's notifications.
template <class P>
void waitMain(P pred, double *accumUs)
{
	if (pred())
		return;
	const Clock::time_point t0 = Clock::now();
	for (int spin = 0; spin < 16; spin++) {
		std::this_thread::yield();
		if (pred()) {
			*accumUs += usSince(t0);
			return;
		}
	}
	std::unique_lock<std::mutex> lock(s_doneMutex);
	s_mainWaiting.store(true, std::memory_order_seq_cst);
	while (!pred())
		s_doneCv.wait_for(lock, std::chrono::milliseconds(2));
	s_mainWaiting.store(false, std::memory_order_seq_cst);
	*accumUs += usSince(t0);
}

void pollFences()
{
	std::lock_guard<std::mutex> lock(s_fenceMutex);
	for (size_t i = 0; i < s_pendingFences.size();) {
		FenceProxy *f = s_pendingFences[i];
		const GLenum r = s_clientWaitSync(f->real, 0, 0);
		if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) {
			f->state.store(2, std::memory_order_release);
			s_pendingFences[i] = s_pendingFences.back();
			s_pendingFences.pop_back();
		} else {
			i++;
		}
	}
}

bool fencesPending()
{
	std::lock_guard<std::mutex> lock(s_fenceMutex);
	return !s_pendingFences.empty();
}

void workerMain()
{
	if (!SDL_GL_MakeCurrent(s_window, s_context)) {
		fprintf(stderr, "[d3d8gles] render thread: SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
		s_stop.store(true);
		s_hasContext.store(false);
		wakeMain();
		return;
	}
	s_hasContext.store(true, std::memory_order_release);
	wakeMain();

	// GeneralsX @performance Android port 30/09/2026 The same priority Android gives its own
	// RenderThread (THREAD_PRIORITY_DISPLAY and above). In logs-30 the render thread was busy
	// 19-28 ms a frame for GL calls that cost ~9 ms on the engine's thread, the signature of the
	// scheduler keeping a default-priority thread on the slow cores.
	const int prioResult = setpriority(PRIO_PROCESS, (id_t)gettid(), -10);
	fprintf(stderr, "[d3d8gles] render thread: priority -10 %s\n", prioResult == 0 ? "set" : "refused");

	size_t read = s_readPos.load(std::memory_order_relaxed);
	while (true) {
		size_t write = s_writePos.load(std::memory_order_acquire);
		if (read == write) {
			if (s_hasContext.load(std::memory_order_relaxed) && s_clientWaitSync)
				pollFences();
			// Spin briefly: the engine thread usually queues the next command within microseconds.
			// GeneralsX @performance Android port 01/10/2026 Bounded by time (30 us), not by 200
			// yields: the render thread catches up with the engine's thread many times a frame,
			// and spinning hundreds of microseconds each time kept a big core busy for nothing --
			// heat that the old phone paid back as throttling within minutes (logs-38).
			bool more = false;
			const Clock::time_point spinStart = Clock::now();
			while (!more && Clock::now() - spinStart < std::chrono::microseconds(30)) {
				std::this_thread::yield();
				more = s_writePos.load(std::memory_order_acquire) != read;
			}
			if (more)
				continue;
			if (s_stop.load())
				break;
			std::unique_lock<std::mutex> lock(s_workMutex);
			s_workerSleeping.store(true, std::memory_order_seq_cst);
			if (s_writePos.load(std::memory_order_seq_cst) == read && !s_stop.load()) {
				// With fences outstanding, wake up to poll them even when nothing is queued.
				const bool fences = s_hasContext.load(std::memory_order_relaxed) && s_clientWaitSync && fencesPending();
				s_workCv.wait_for(lock, fences ? std::chrono::milliseconds(1) : std::chrono::milliseconds(50));
			}
			s_workerSleeping.store(false, std::memory_order_seq_cst);
			continue;
		}
		const Clock::time_point busy = Clock::now();
		Clock::time_point lastPoll = busy;
		Clock::time_point busyMark = busy;
		unsigned sincePoll = 0;
		while (read != write) {
			Cmd *c = reinterpret_cast<Cmd *>(s_ring + read % kRingBytes);
			const uint32_t bytes = c->bytes;
			if (c->run)
				c->run(c);
			read += bytes;
			s_readPos.store(read, std::memory_order_release);
			wakeMain();
			write = s_writePos.load(std::memory_order_acquire);
			// Poll fences while busy too, at most every 0.5 ms. Polled only while idle, a busy
			// render thread never marked a persistent buffer's copies free, the engine's thread
			// ran out of copies and waited on a fence (logs-30: 3-8 ms a frame of sync waits).
			if (++sincePoll >= 64) {
				sincePoll = 0;
				const Clock::time_point now = Clock::now();
				// Counted as it goes: a render thread that never catches up never leaves this
				// loop, and busy time added only on leaving it read as zero (logs-33).
				s_workerBusyNs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now - busyMark).count(),
					std::memory_order_relaxed);
				busyMark = now;
				if (now - lastPoll >= std::chrono::microseconds(500)) {
					lastPoll = now;
					if (s_clientWaitSync && s_hasContext.load(std::memory_order_relaxed))
						pollFences();
				}
			}
		}
		s_workerBusyNs.fetch_add((uint64_t)(usSince(busyMark) * 1000.0), std::memory_order_relaxed);
	}
	if (s_hasContext.load())
		SDL_GL_MakeCurrent(s_window, nullptr);
	s_hasContext.store(false);
	wakeMain();
}

// Hand the context back and forth around Android's pause/resume. SDL's backup (on pause) reads
// the context current on the thread pumping events and releases it so the surface can be
// destroyed; its restore (on resume) makes it current there again -- possibly a new context.
// WILL_ENTER_BACKGROUND is sent just before the backup and DID_ENTER_FOREGROUND just after the
// restore, both from SDL_PumpEvents on the engine's thread.
void releaseToMain()
{
	if (!g_active)
		return;
	sync([] {
		SDL_GL_MakeCurrent(s_window, nullptr);
		s_hasContext.store(false);
	});
	g_active = false;
	SDL_GL_MakeCurrent(s_window, s_context);
}

void takeFromMain()
{
	if (g_active || !s_thread.joinable() || s_stop.load())
		return;
	SDL_GLContext current = SDL_GL_GetCurrentContext();
	if (current != nullptr)
		s_context = current;
	SDL_GL_MakeCurrent(s_window, nullptr);
	g_active = true;
	bool ok = false;
	sync([&ok] {
		ok = SDL_GL_MakeCurrent(s_window, s_context);
		s_hasContext.store(ok);
	});
	if (!ok) {
		fprintf(stderr, "[d3d8gles] render thread: could not take the context back (%s); rendering on the main thread\n",
			SDL_GetError());
		g_active = false;
		SDL_GL_MakeCurrent(s_window, s_context);
	}
}

bool SDLCALL lifecycleWatch(void *, SDL_Event *event)
{
	if (SDL_GetCurrentThreadID() != s_mainThread)
		return true;
	if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
		releaseToMain();
		fprintf(stderr, "[d3d8gles] render thread: context handed back for pause\n");
	} else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
		takeFromMain();
		fprintf(stderr, "[d3d8gles] render thread: context taken back after resume (%s)\n",
			g_active ? "threaded" : "main thread");
	}
	return true;
}

void stopAtExit()
{
	stop();
}

} // namespace

void *allocCmd(size_t bytes, uint32_t *rounded)
{
	// The ring has one producer. A GL call from any other thread while the render thread runs
	// would corrupt it; before the render thread such a call simply had no context. Say so once.
	if (SDL_GetCurrentThreadID() != s_mainThread) {
		static std::atomic<bool> s_warned{false};
		if (!s_warned.exchange(true))
			fprintf(stderr, "[d3d8gles] render thread: GL call from another thread (%llu); not supported\n",
				(unsigned long long)SDL_GetCurrentThreadID());
	}
	size_t n = (bytes + kAlign - 1) & ~(kAlign - 1);
	size_t phys = s_allocPos % kRingBytes;
	if (phys + n > kRingBytes) {
		// Not enough room before the end: pad to it and start again at the beginning.
		const size_t pad = kRingBytes - phys;
		waitMain([pad] { return s_allocPos + pad - s_readPos.load(std::memory_order_acquire) <= kRingBytes; },
			&s_stats.ringWaitUs);
		Cmd *p = reinterpret_cast<Cmd *>(s_ring + phys);
		p->run = nullptr;
		p->bytes = (uint32_t)pad;
		s_allocPos += pad;
		s_writePos.store(s_allocPos, std::memory_order_seq_cst);
	}
	waitMain([n] { return s_allocPos + n - s_readPos.load(std::memory_order_acquire) <= kRingBytes; },
		&s_stats.ringWaitUs);
	*rounded = (uint32_t)n;
	s_stats.commands++;
	s_stats.commandBytes += (double)n;
	return s_ring + s_allocPos % kRingBytes;
}

void commitCmd()
{
	const Cmd *c = reinterpret_cast<const Cmd *>(s_ring + s_allocPos % kRingBytes);
	s_allocPos += c->bytes;
	s_writePos.store(s_allocPos, std::memory_order_seq_cst);
	wakeWorker();
}

void syncCall(void (*fn)(void *), void *ctx)
{
	std::atomic<bool> done{false};
	post([fn, ctx, &done] {
		fn(ctx);
		done.store(true, std::memory_order_release);
	});
	s_stats.syncCalls++;
	waitMain([&done] { return done.load(std::memory_order_acquire); }, &s_stats.syncWaitUs);
}

// The bytes travel inside the command itself, right behind a small header: one copy into the ring
// (which stays in cache), one out of it, and no allocation. The first version carried them in a
// heap Blob; a full refill of a vertex copy is ~400 KB, so every one of those was an allocation
// large enough to be mmap'd and page-faulted fresh (logs-36: uploads ~1 ms a frame dearer).
namespace {
struct MappedWriteCmd : Cmd
{
	void *dst;
	size_t length;
	static void exec(Cmd *c)
	{
		MappedWriteCmd *w = static_cast<MappedWriteCmd *>(c);
		WorkTimer uploadTimer(kWorkUpload);
		memcpy(w->dst, reinterpret_cast<unsigned char *>(w) + sizeof(MappedWriteCmd), w->length);
	}
};
}

void writeMapped(void *dst, const void *src, size_t bytes)
{
	if (!g_active) {
		memcpy(dst, src, bytes);
		return;
	}
	// A quarter of the ring at most per command, so one write can never wait on room it needs
	// itself; larger ones are split.
	const size_t kMaxChunk = kRingBytes / 4;
	const unsigned char *from = static_cast<const unsigned char *>(src);
	unsigned char *to = static_cast<unsigned char *>(dst);
	while (bytes > 0) {
		const size_t n = bytes < kMaxChunk ? bytes : kMaxChunk;
		uint32_t rounded = 0;
		unsigned char *mem = static_cast<unsigned char *>(allocCmd(sizeof(MappedWriteCmd) + n, &rounded));
		MappedWriteCmd *w = new (mem) MappedWriteCmd;
		w->run = &MappedWriteCmd::exec;
		w->bytes = rounded;
		w->dst = to;
		w->length = n;
		memcpy(mem + sizeof(MappedWriteCmd), from, n);
		commitCmd();
		from += n;
		to += n;
		bytes -= n;
	}
}

bool start(SDL_Window *window)
{
	if (s_thread.joinable())
		return g_active;
	s_window = window;
	s_context = SDL_GL_GetCurrentContext();
	s_mainThread = SDL_GetCurrentThreadID();
	if (s_context == nullptr) {
		fprintf(stderr, "[d3d8gles] render thread: no current context to hand over\n");
		return false;
	}
	if (s_ring == nullptr)
		s_ring = static_cast<unsigned char *>(aligned_alloc(kAlign, kRingBytes));
	if (s_ring == nullptr)
		return false;

	SDL_GL_MakeCurrent(window, nullptr);
	s_stop.store(false);
	s_thread = std::thread(workerMain);
	double ignored = 0.0;
	waitMain([] { return s_hasContext.load() || s_stop.load(); }, &ignored);
	if (!s_hasContext.load()) {
		s_thread.join();
		SDL_GL_MakeCurrent(window, s_context);
		return false;
	}
	g_active = true;
	if (!s_watchInstalled) {
		SDL_AddEventWatch(lifecycleWatch, nullptr);
		std::atexit(stopAtExit);
		s_watchInstalled = true;
	}
	return true;
}

void stop()
{
	if (!s_thread.joinable())
		return;
	if (g_active) {
		sync([] {});
		g_active = false;
	}
	s_stop.store(true);
	{
		std::lock_guard<std::mutex> lock(s_workMutex);
		s_workCv.notify_one();
	}
	s_thread.join();
	if (s_window && s_context && SDL_GL_GetCurrentContext() == nullptr)
		SDL_GL_MakeCurrent(s_window, s_context);
}

void present(SDL_Window *window, int swapInterval)
{
	if (!g_active) {
		if (swapInterval >= 0)
			SDL_GL_SetSwapInterval(swapInterval);
		WorkTimer swapTimer(kWorkSwap);
		SDL_GL_SwapWindow(window);
		return;
	}
	post([window, swapInterval] {
		if (swapInterval >= 0)
			SDL_GL_SetSwapInterval(swapInterval);
		{
			WorkTimer swapTimer(kWorkSwap);
			SDL_GL_SwapWindow(window);
		}
		s_framesPresented.fetch_add(1, std::memory_order_release);
	});
	s_framesQueued++;
	// One frame in flight: the engine builds frame N+1 while the render thread finishes frame N.
	const uint64_t want = s_framesQueued - 1;
	waitMain([want] { return s_framesPresented.load(std::memory_order_acquire) >= want; }, &s_stats.frameWaitUs);
}

void setFenceProcs(PFN_FenceSync fence, PFN_ClientWaitSync wait, PFN_DeleteSync del)
{
	s_fenceSync = fence;
	s_clientWaitSync = wait;
	s_deleteSync = del;
}

GLsync fenceSync()
{
	FenceProxy *f = new FenceProxy;
	post([f] {
		f->real = s_fenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		f->state.store(1, std::memory_order_release);
		std::lock_guard<std::mutex> lock(s_fenceMutex);
		s_pendingFences.push_back(f);
	});
	return reinterpret_cast<GLsync>(f);
}

GLenum clientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
	FenceProxy *f = reinterpret_cast<FenceProxy *>(sync);
	if (f->state.load(std::memory_order_acquire) == 2)
		return GL_ALREADY_SIGNALED;
	// Polling (timeout 0) never waits: a fence the render thread has not issued, or has not seen
	// signaled yet, is simply not signaled. The render thread keeps polling issued ones.
	if (timeout == 0 && g_active)
		return GL_TIMEOUT_EXPIRED;
	GLenum r = GL_WAIT_FAILED;
	gxrt::sync([f, flags, timeout, &r] {
		r = s_clientWaitSync(f->real, flags, timeout);
		if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) {
			f->state.store(2, std::memory_order_release);
			std::lock_guard<std::mutex> lock(s_fenceMutex);
			for (size_t i = 0; i < s_pendingFences.size(); i++) {
				if (s_pendingFences[i] == f) {
					s_pendingFences[i] = s_pendingFences.back();
					s_pendingFences.pop_back();
					break;
				}
			}
		}
	});
	return r;
}

void deleteSync(GLsync sync)
{
	FenceProxy *f = reinterpret_cast<FenceProxy *>(sync);
	post([f] {
		{
			std::lock_guard<std::mutex> lock(s_fenceMutex);
			for (size_t i = 0; i < s_pendingFences.size(); i++) {
				if (s_pendingFences[i] == f) {
					s_pendingFences[i] = s_pendingFences.back();
					s_pendingFences.pop_back();
					break;
				}
			}
		}
		if (f->real)
			s_deleteSync(f->real);
		delete f;
	});
}

void addWork(int kind, uint64_t ns)
{
	s_workNs[kind].fetch_add(ns, std::memory_order_relaxed);
}

Stats takeStats()
{
	Stats s = s_stats;
	s.workerBusyUs = (double)s_workerBusyNs.exchange(0, std::memory_order_relaxed) / 1000.0;
	s.drawUs = (double)s_workNs[kWorkDraw].exchange(0, std::memory_order_relaxed) / 1000.0;
	s.swapUs = (double)s_workNs[kWorkSwap].exchange(0, std::memory_order_relaxed) / 1000.0;
	s.uploadUs = (double)s_workNs[kWorkUpload].exchange(0, std::memory_order_relaxed) / 1000.0;
	s_stats = Stats();
	return s;
}

bool running()
{
	return g_active;
}

} // namespace gxrt
