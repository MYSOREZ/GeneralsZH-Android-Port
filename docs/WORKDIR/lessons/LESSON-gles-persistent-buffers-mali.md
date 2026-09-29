# LESSON: Persistent-mapped dynamic buffers on Mali — measure the driver, and never let it cache a lie

**Date**: 2026-09-29
**Phase**: Android port, native GLES backend (`Core/Libraries/Source/d3d8gles/`)
**Device**: the owner's old test phone, Mali, driver `OpenGL ES 3.2 v1.r26p0` (the new phone is Adreno)
**Scene**: the main menu's background battle on the High preset (stencil shadows on), the heaviest
reproducible scene available without playing a match
**Result**: median 21-22 fps → **30**, bottom decile 16-17 → **26**, minimum 11-14 → **20**,
no visual change

Companion to `LESSON-gles-dynamic-buffer-stalls.md`, which translated D3D8's lock flags into
GL and brought the UI from 12-17 fps to 55-60. That lesson made each dynamic-buffer update cheap
in *kind* (no GPU sync); this one makes it cheap in *count* (no GL call at all).

## TL;DR for the next agent

1. **Split every cost until the next step is obvious, then change one thing.** Each fix in this
   series came from a counter added in the build before it, never from a guess. The counters are
   still in the tree and cheap; read them first (see "Instruments" below).
2. On this Mali driver a `glMapBufferRange`/`glUnmapBuffer` pair costs **~15-20 us**, and the
   engine does 500-700 of them per frame in a battle. Persistent mapping (`GL_EXT_buffer_storage`)
   turns a vertex append into a `memcpy`.
3. **Do not persistently map an index buffer that is refilled in place.** `glDrawElements` carries
   no index range, so the Mali driver scans the indices for min/max and caches it per buffer range
   until a GL call modifies the buffer. A `memcpy` into a persistent mapping is not such a call,
   the engine refills the same offsets every frame, the cached range goes stale, vertices go
   unshaded: every dynamic draw flickers. Adreno does not do this, so it looked fine there.
4. The fix that keeps the speed: **never reuse a byte of index memory.** Stream each draw's
   indices into the next unused bytes of one persistently mapped buffer and replace the buffer
   object (not its contents) when full. No `(buffer, offset)` pair ever holds two different index
   lists, so there is nothing for the driver to get wrong, and a new object needs no fence.

## The sequence, with the numbers that decided each step

| Build | Change | What the log showed |
|---|---|---|
| logs-20 | `[GX-PERF-SHADOW]` added | Shadows 5-8 ms, **not** the cause; particles 850-1190 draws, 21-28 ms |
| logs-21 | Translucent sort by 4-unit depth slab, then node (`sortingrenderer.cpp`) | Particle draws 1046 → 570-660 in a 3x heavier scene; driver uploads 13-14 ms appeared |
| logs-22 | Full uploads send only `[0, writtenEnd)`; `[d3d8gles] perf-upload` split | vb-full 0.5-0.8 ms (was most of it); **vb-append 6-8 ms, ib-append 2.5-3.4 ms** left |
| logs-23 | Persistent coherent mapping for dynamic VB **and** IB | fps +33%, **everything dynamic flickers**, terrain does not |
| logs-24 | Copy-on-write for writes below the highest byte an issued draw reads | Fired almost never; flicker unchanged → not a write-after-issue hazard |
| — | `GL_MAP_FLUSH_EXPLICIT_BIT` + `glFlushMappedBufferRange` | Flicker unchanged → not coherence either (reverted) |
| logs-25 | Dynamic IBs back on map/unmap, VBs persistent | Flicker **gone**, but ib-append 3-6 ms → fps back down |
| logs-26 | Index stream (`WebGLPipeline::streamIndices`) | No flicker, fps 30 median, particle draw 24 us → 9 us |

The decisive clue came from the owner, not from a counter: **"the map itself doesn't flicker"**.
Terrain uses static buffers; everything that flickered used dynamic ones. An independent read of
the code (a separate agent, given every failed attempt) then ruled out data hazards and found what
the persistent path had actually changed: the driver no longer saw a GL call when contents changed.

## Why the obvious fixes failed

- **Copy-on-write** assumed the tile-based GPU was reading bytes the CPU had since overwritten.
  Mali does run a frame's draws after submission, but the engine's D3D contract
  (`DISCARD` at offset 0, `NOOVERWRITE` beyond) already prevents that. The counter showed it.
- **Explicit flush** assumed the CPU writes were not reaching the GPU. They were; the problem was
  never the data, it was the driver's *derived* state about the data. `glFlushMappedBufferRange`
  publishes bytes, it does not tell the driver to recompute anything.

> Generalization: when a driver-level optimization breaks rendering, ask what the driver used
> to *learn* from the calls you removed, not only what data it used to receive.

## Why particles got 2.5x cheaper per draw

Nothing in the particle path changed in the last step, yet a particle draw went from ~24 us to
~9 us of driver time. The dynamic index buffers used to be updated by map/unmap before nearly every
particle draw, and each update invalidated the driver's cached index range, so each draw paid for
a rescan. The index stream never invalidates anything. That per-draw cost was the unexplained
"~23 us per particle draw" of the previous week — it was never about blend or texture state.

## Instruments (all in `present()`, once per ~2 s, `gx_perf.txt` only)

- `[d3d8gles] perf-draws/frame by source` — draw count per engine pass.
- `[d3d8gles] perf-cpu` — ms per frame inside `drawCommon` per source, glDraw share, driver uploads.
- `[d3d8gles] perf-gldraw` — glDraw time per source, and after-write vs other draws.
- `[d3d8gles] perf-state` — GL state calls per draw for particles and models.
- `[d3d8gles] perf-upload` — vb/ib full uploads and appends, ring + index stream, textures.
- `[d3d8gles] perf-opt` — persistent copy switches, waits, new copies, index-stream renewals.
- `[GX-PERF-SHADOW]` — shadow casters, silhouette rebuilds, volumes, update/incr/decr/darken ms.
- `[GX-PERF-AUDIO]` — audio cost split, sample cache hits/misses and their open/decode time.

Switches: `gx_gles_noopt.txt` containing `persistent` turns persistent VBs and the index stream
off without a rebuild; `gx_gles_persistentib.txt` opts dynamic IBs back into persistent copies
(for testing a GPU that does not cache index ranges).
