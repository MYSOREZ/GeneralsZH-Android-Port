# PLAN-030: One base of pictures for Generals and Zero Hour

Status: **step 1 for the next test build** (18/10/2026).

## Owner's request (18/10/2026)

Generals showed a purple square where Zero Hour shows the touch force-attack button's art. "Why draw
it when Zero Hour has it? Take it from there." And more generally: put the icons and pictures of
both games into one base that both releases take everything from, so later work is simpler.

## What already holds

- Zero Hour already reads the base game: it is an expansion, and loads the Generals archives below
  its own (`loadBaseGeneralsAssetsForZH`, StdBIGFileSystem.cpp). So everything Generals has, Zero
  Hour can show.
- The pictures this port adds (scatter, formation, builder page arrows, the force-attack crosshair)
  are drawn by one shared file for both engines (W3DTouchButtonImages.cpp).
- Nothing of the games' data is shipped in the APK; it all comes from the player's own folders.

## Step 1 (done): Generals borrows Zero Hour's textures

The launcher writes `zerohour_art_path.txt` (the Zero Hour folder) when Generals starts;
SDL3Main.cpp turns it into `GENERALSX_ZH_ART_PATH`; StdBIGFileSystem mounts that folder's
`TexturesZH.big` **below** every Generals archive (no overwrite). A texture Generals has stays its
own; one it lacks -- SCCAttack's -- comes from Zero Hour. Without a Zero Hour folder the force-attack
button falls back to the drawn crosshair.

Rules that keep this safe:
- Textures only. No INI, map, script (.scb) or model (W3D) of Zero Hour reaches Generals: those are
  gameplay, and the lobby and replay checksums (ini_crc, exe_crc) cover INI and scripts.
- Below, never above: Generals must look like Generals. Zero Hour's interface art for the same names
  (control bar, buttons) must not replace Generals'.

## Later

1. If more Generals gaps show up (a cameo, a button, a mapped image), check whether
   `W3DZH.big` / `WindowZH.big` carry them, and mount only what is needed, the same way.
2. One place in the launcher that says which folders feed which game (Generals base for Zero Hour,
   Zero Hour pictures for Generals), with a check that both are found.
3. This port's own pictures stay drawn in code (one file, both engines) or move to one loose
   folder in the APK (`gamedata/Art/`), mounted by both engines -- never into the player's game data.
