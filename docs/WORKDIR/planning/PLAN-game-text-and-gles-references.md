# Plan: game text in every launcher language, and GLES backend references

**Date:** 24/09/2026
**Status:** planned, not started

## 1. Game text translated into every launcher language

### Goal

The launcher UI exists in 13 languages (values, ar, b+isv, de, es, fa, fr, ko, pl, pt-rBR,
ru, uk, zh). The game's own text (`generals.csf`) is currently available in English and, as
downloadable packs, Russian and Ukrainian. Every launcher language should get a game-text pack.

Every pack is translated from the **English** original, not from another translation:
- no third-party localizer credits or studio mentions (for example the "Saberian" credit
  some Russian releases insert); where the English text has none, the pack has none;
- meaning, tone and length follow the English line, so buttons, tooltips and subtitles fit
  where the English ones do.

### Size (measured from `EnglishZH.big` / `Data\English\generals.csf`)

| | |
|---|---|
| Labels | 6421 |
| Words | ~52,800 |
| Largest groups | DIALOGEVENT 2455 (subtitles), GUI 916, CONTROLBAR 756, MAP 470, OBJECT 443, TOOLTIP 276, CREDITS 220 |
| Lines with printf arguments (`%s`, `%d`, ...) | 112 |
| Lines with `&` hotkey markers | 385 |

### Rules any translation pass must keep

- **Labels are keys**, never translated; only the text changes. The pack must contain every
  English label, or `fetch()` falls back and the player sees mixed languages.
- **printf arguments** stay, in the same order and type. A missing or reordered `%d` is a crash or
  garbage, not a style problem. Check every one of the 112 mechanically.
- **`&` hotkeys**: one per line, on a letter that exists in the translated word; hotkeys inside one
  menu or command set must not collide (the game resolves them per panel).
- **`\n` line breaks** and leading `*` on subtitles are markup: keep them.
- **Length**: command-bar buttons and menu buttons have fixed widths at 800x600; long
  translations wrap or clip (compare the `MISSING: 'GUI:CustomMission'` wrap on the Steam main
  menu). Flag lines more than ~30% longer than English for review.
- **CREDITS** (220 lines) keep the original team names; only role titles are translated.

### Open technical questions, to answer before translating

- **Arabic and Persian are right-to-left and need glyph shaping.** The engine draws text left to
  right, one glyph per code point, with no bidi reordering and no contextual Arabic forms. As is,
  an `ar`/`fa` pack would render as disconnected letters in reverse order. Needs either a shaping
  step in the text renderer (HarfBuzz + FriBiDi class of work) or pre-shaped, pre-reversed text
  in the pack (fragile: wraps break it). Decide before doing these two.
- **Korean and Chinese** need fonts with CJK glyphs. Official Korean/Chinese releases shipped
  their own; check what the Android port's bundled fonts (`assets/gamedata/fonts`) cover.
- **Interslavic (`isv`)**: pick one script (Latin or Cyrillic) for the game text.
- Pack format and delivery: the launcher's existing "download language" path
  (`GENERALSX_TEXT_LANGUAGE`, see `GameText.cpp` and the 09/09/2026 diary entry) should carry
  every new language without engine changes.

### Verification

- A script that diffs a pack against English: same label set, same printf signature per line,
  exactly one `&` per hotkey line, no empty strings.
- In game: main menu, skirmish setup, a command bar per faction, tooltips, one subtitle-heavy
  campaign mission.

## 2. Further GLES optimisation: what other projects did, checked

A suggestion listed three projects as "native D3D8/9 to GLES translators". Checked against what
those projects actually are:

| Project | What it really is | Useful to us for |
|---|---|---|
| **re3 / reVC (librw)** | Not a D3D8 translator. librw re-implements the **RenderWare API** with its own backends (D3D9, GL3, GLES2/3). The game calls RenderWare, not D3D. | How a 2001-era fixed-function engine maps materials, lighting and texture stages onto GLES shaders; its batching and state caching. |
| **Xash3D FWGS** | GoldSrc renders through **OpenGL 1.x**, not Direct3D. On Android it runs through **gl4es** (GL1.x on GLES, by ptitSeb) and later its own GLES renderers. | gl4es: fixed-function emulation on GLES, immediate-mode batching, state caching. |
| **ToGL (Valve)** | Real: a **D3D9 to desktop OpenGL** layer (open-sourced 2014), used for the Source-engine Linux/macOS ports. Not GLES. | Closest in shape to our `d3d8gles`: D3D9 state and shader translation, and how it avoided redundant state and buffer stalls. |

Two references the list missed, more relevant than any of the three:
- **WineD3D** (Wine): the most complete D3D8/9 to OpenGL/GLES translation in existence,
  including the fixed-function pipeline.
- **DXVK** itself: this port already uses it on Vulkan; its handling of `DISCARD`/`NOOVERWRITE`
  and of fixed-function state is the reference our GLES backend is measured against (see
  `LESSON-gles-dynamic-buffer-stalls.md`).

### Constraint: rendering never touches the simulation

Golden Rule 7. Every optimisation here is in `Core/Libraries/Source/d3d8gles` or the W3D device
layer and must not change anything the logic reads: no culling or LOD decisions that feed back
into game state, no timing that alters the logic frame rate. A replay check of `Global_War2.rep`
(1946/1946) before and after any change is the gate, together with the `[d3d8gles] perf` lines.

### How to start

Measure first: take a device log with the `[d3d8gles] perf-draws/frame by source` and
`perf-ui ms/frame` lines on a heavy scene, find the top cost, then look up how WineD3D, ToGL
or librw handle that specific thing. Do not port patterns wholesale.
