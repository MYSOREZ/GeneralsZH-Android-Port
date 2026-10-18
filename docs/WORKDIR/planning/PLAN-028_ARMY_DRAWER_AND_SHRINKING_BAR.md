# PLAN-028: Army buttons as a left drawer, with the control bar shrinking right

Status: **implemented for the next test build** (18/10/2026): realkot's column (GroupPanel.cpp,
GroupPanel.wnd) as a drawer -- "+" above the standard bar's left end opens it, the bar's windows
move right of it (`ControlBar::setArmyColumnOpen`, `GXUiScale::setControlBarLeftEdge`), "-" at the
column's foot closes it; closed at the start of every game. Column width: safe inset + 9.5% of the
screen (owner: narrower than the 4:3 bar left). See the diary, 18/10/2026.

## What exists (1.4.1)

The Tactical scheme's army buttons (Screen / Map / Planes / Helis / Ground) are one row on the
group panel, right of the handle and the last-event button, above the control bar. The control bar
keeps its retail size and place. realkot's fork instead puts a tall column of group and army
buttons in the left gap and squeezes the bar to the right; the owner finds that column takes too
much of the screen.

## The owner's idea

- One button at the left edge opens a **drawer** from the left side with the army buttons (and
  possibly the group grid).
- While the drawer is open, the control bar **shrinks to the right** to make room, as on realkot's
  screenshot.
- Drawer closed: the control bar has its standard look and place.

## Questions to answer first

- Moving/scaling `ControlBar.wnd` at run time: the bar is hand-placed by `GXUiScale` and the
  group panel follows it (`GroupPanelFollowControlBar`); shrinking means a second transform for
  the bar's windows, applied and reverted on toggle, and every hit test must follow
  (see LESSON-interface-scale-layouts.md -- test overlaps against the retail .wnd windows).
- Animate or snap? The retail bar already has a slide animation; a second one must not fight it.
- Does the radar keep its size (it is the most-used part on a phone)?
- Where does the drawer button sit so it does not collide with the group panel handle and the
  display cutout (`GXSafeArea`)?
- Is the drawer worth it over the current single row? Measure on a phone at 100% and 110% UI scale
  before building.
