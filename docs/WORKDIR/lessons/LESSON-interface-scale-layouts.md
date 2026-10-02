# LESSON: Interface size — scale the layouts, not the resolution, and test against what is drawn

**Date**: 2026-10-02
**Phase**: Android port, launcher "Interface Size" (`GXUiScale`, Options.ini `GXUiScale`, 100-200%)
**Code**: `Core/GameEngine/Source/GameClient/GUI/GXUiScale.cpp` (+ `.h`), hooks in
`GameWindowManagerScript.cpp` (`parseScreenRect`, `parseFont`, `winCreateFromScript`),
`ControlBarScheme.cpp`, `ControlBar.cpp` (tactical view height), `InGameUI.cpp` (HUD fonts),
`GroupPanel.cpp` (follow offset)
**Device**: the owner's old Mali phone, 2340x1080 (2.17:1)

## TL;DR for the next agent

1. **No resolution trick makes a button bigger.** `parseScreenRect` stretches every `.wnd` from its
   800x600 creation resolution to the whole screen (x 2.9, y 1.8 on this phone). A lower game
   resolution, an upscaler, or drawing the UI in a separate pass all leave every button the same
   share of the screen. A previous "Interface Size" slider did exactly that and was removed
   (comment in `SetupActivity.java`). Bigger buttons = moving and resizing the windows themselves.
2. **One transform per layout, applied while the layout is parsed.** `winCreateFromScript` hands
   the whole file to `GXUiScale::beginLayout`, which pre-reads every window's rect and name, picks
   what scales, computes `X' = X*k + add` per axis, and `parseScreenRect` / `parseFont` apply it to
   each window in file order (one decision per window that has a SCREENRECT). Gadgets are created
   from the scaled rects, so listboxes, sliders etc. size themselves correctly.
3. **Code that places windows by hand must use the same transform.** `ControlBarScheme::init`
   positions a dozen control bar windows and draws the bar's art from its own coordinates; both go
   through `GXUiScale::forLayout("ControlBar.wnd")`. `GroupPanel` follows the bar by an offset
   calibrated at init, so it must load with the bar's transform (`sharedWith`).
4. **Test overlaps against the windows, never against a bounding box.** Every "it moved for no
   reason" in this work was a box: the content box takes in empty space, hidden submenus and the
   tallest variant; at 110% it "covered" the faction emblems with room to spare. Then the
   emblems' *Medium* copies (the size a picture grows to while highlighted) were tested instead
   of the pictures. Simulate on the retail `.wnd` (extract `WindowZH.big`, see below) before
   building: it found both in a minute.
5. **A clamp can silently undo the move it guards.** The power bar's frame starts at y 3 of 600;
   "keep the top on screen" after resting it on the bar shifted it back to where it was. Its
   buttons fill from the bottom, so the right answer was wrapping into a second column, not
   clamping or letting slots leave the screen.

## The rules (`kRules` in GXUiScale.cpp)

| Field | Meaning | Used by |
|---|---|---|
| `maxFracX/Y` | content may take at most this share of the screen after scaling | control bar 40% high; main menu 60% wide |
| `uniform` | same factor on both axes | everything but the full-width control bar (grows taller only) |
| `exclude` | windows (NAME after the colon, `*` = prefix) that keep place and size, with their children | main menu decoration |
| `sharedWith` | take another layout's transform as is (computed from its file if not loaded yet) | GroupPanel.wnd follows ControlBar.wnd |
| `restsOn` | bottom edge goes where that layout maps it, grows upward | power shortcut bars, in-game chat on the control bar |
| `moveAwayPrefix` | excluded decoration that is relaid when the content covers it: a column left of the menu, each group (picture + Small/Medium copies) moved and shrunk as one; off screen only if no room | main menu faction emblems |
| `wrapSlotPrefix` + `frameName` | slots stacked bottom-up wrap into another column to the left; the frame widens so taps reach them | power shortcut bars (`ButtonParent*`) |
| `growMarkerName` | a marker a transition grows into (`MainMenuScaleUpTransition` reads `WinGrowMarker`'s rect at start): moved into the free space beside the content | main menu |
| `margin` | 0 = 2% from the edges, content already past it may stay; non-zero = strict, everything inside (sides capped at 3%) | main menu inside its frame (6% top/bottom) |

Non-uniform scaling is only acceptable where the art is already stretched by the screen's aspect
(the control bar: x2.9 vs y1.8, growing y brings it *closer* to its designed proportions). Text in
a scaled layout scales by `fontK = min(ky, kx * aspectRoom)`.

## Things that are not in the layouts

- **Event messages and the corner HUD** (fps, clock, latency, game time, player list) are drawn by
  `InGameUI` with their own fonts: `gxHudFontSize()` scales them, max x2.
- **The battlefield height** follows the bar: `setScaledViewportHeight` maps
  `H * m_viewportHeightScale` through the bar's transform.
- **Input**: a transparent container covering the battlefield swallows taps (GroupPanelParent:
  units moved but would not attack there). Containers that only hold buttons are `NOINPUT`.
- **The camera cutout**: the group panel shifts right by `GXSafeArea::leftPx()` + 1%.

## Trade-offs the owner chose

- Main menu: buttons x1.35 (content capped at 60% width) so the side emblem grown during
  difficulty select keeps ~90% of its size. x1.5 left it 40%, x1.18 looked too small.
- The main menu's largest scale is ~x1.5 whatever the slider says: that is the screen's height.

## Tools

- Extract the retail layouts: `WindowZH.big` is a BIGF archive (header `BIGF`, count big-endian at
  8, entries `offset, size` big-endian + NUL-terminated name from 16). A 15-line Python reader
  is enough; the session's `wndtree.py` printed the window tree with rects and HIDDEN flags.
- `[GX-UISCALE]` in the log: every scaled layout with its factors, wraps, relaid decoration, the
  general's button rect and the group panel calibration.
