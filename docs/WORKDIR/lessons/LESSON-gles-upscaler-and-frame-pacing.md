# LESSON: Upscaling on the native GLES backend, and three costs that were not where they looked

**Date**: 2026-10-01 .. 2026-10-02
**Phase**: Android port, native GLES translator (`Core/Libraries/Source/d3d8gles/`)
**Code**: `gles_pipeline.cpp` (`setVirtualBackbuffer`, `targetRect`, `upscaleSceneNow`,
`stretchVirtualBackbuffer`, `present`), `dx8wrapper.cpp` (`Pillarbox_Setup`,
`Upscale_Scene_Begin_UI`), `W3DView.cpp` / `W3DDisplay.cpp` (the split point),
`W3DVolumetricShadow.cpp` (stencil passes)

## TL;DR for the next agent

1. **An upscaler is not a lower resolution.** The first version lowered the game's Resolution
   (exactly what the game's own Options do) and the owner rightly asked what the difference was.
   The game must keep the screen's resolution; only the translator renders fewer pixels.
2. **Two sizes, one place that knows both.** The virtual backbuffer has the engine's size (all
   viewports, 2D coordinates, touch) and a render size (x the FSR 1 mode: 77/67/59/50%). Only
   `targetRect()` -- every glViewport and glScissor into it -- scales. Shaders keep working in the
   engine's pixels (`uViewportPos`), so pre-transformed 2D needs nothing.
3. **Upscale the scene, not the frame.** `W3DView::draw` calls `Upscale_Scene_Begin_UI()` right
   after the 3D scene and its screen filters: the scene is stretched into the window with SGSR
   there and everything after (health bars, "Construction: 45%", the interface, the cursor) is
   drawn 1:1. A frame with no scene (loading screen, video) is drawn straight into the window
   (`m_vbBypass`, decided from the frame before), otherwise its text is stretched and pixelated.
4. **The backbuffer's clear must stay the backbuffer's.** The window's clear forced alpha 1; the
   virtual backbuffer took the caller's alpha (`m_minWaterOpacity`), a destination alpha the window
   never had. Any substitute target must reproduce every special case of the one it replaces.
5. **GLES and ANGLE only.** DXVK renders through another path; the launcher hides the card under
   Vulkan.

## Three costs that were elsewhere

### A stutter every 2 s was a sysfs read
The translator's perf report (unconditional, every 2000 ms, on the engine's thread) read a dozen
cpufreq files and up to 64 thermal zones. Moved to a low-priority thread, the read itself logs
**40-117 ms**. If a hitch has a fixed period, look for a timer before looking at the GPU.
`[GX-PERF-HITCH]` gives the worst frame per second with its phase split.

### Shadow columns under aircraft were a "safe" optimisation
29/09 drew stencil shadow volumes in one two-sided pass (INCR front / DECR back, both wrapping),
on the claim that every pixel meets at least as many front faces as back faces. The game's volumes
are open at the caster: a ray through a flying unit enters its column through the open top, meets
a back face first, counts -1, wraps to 255, and the darkening pass paints the whole column. The
two-pass INCR / DECRSAT clamps it to 0. Saturating ops cannot fix one pass (the faces' order within
a draw decides). Back to two passes. **An invariant argued from "this game's camera" needs a
counter-example search over the geometry, not the camera.**

### "60 fps cap" was the game speed
Offline, the frame limit *is* the Game Speed slider (30 x 2 on the 60 Hz engine) and logic steps
once per drawn frame. Above normal speed, a phone that cannot draw that fast now runs extra fixed
logic frames (up to 3 per drawn frame, 8 ms) -- the same frames in the same order, offline only.
Explain this before shipping it: the owner first read it as a speedhack.

## Measuring

`[d3d8gles] perf-gpu` (GPU timer per frame), `perf-thread`, `[GX-PERF-THERMAL]` (now with the read
time), `[GX-PERF-HITCH]`. Compare like with like: full resolution without the upscaler was 39-43
fps median in the menu battle before any of this (logs-38..42) and 46 after; 55-60 only ever came
from rendering below the screen's resolution.
