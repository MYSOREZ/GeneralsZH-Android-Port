# PLAN-030: One base of pictures for Generals and Zero Hour

Status: **done for the next test build** (18/10/2026): steps 1 and the launcher view; further archives only on evidence.

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

## Step 2 (done): one place in the launcher

The game folder card on Home says where the selected game takes what its folder lacks, checked:
for Generals, the Zero Hour folder and whether it has TexturesZH.big (green), or that no Zero Hour
folder is chosen / the archive is missing (amber); for Zero Hour, the chosen original Generals
folder, or where it is looked for when none is chosen.

## Step 3: the rest of the gaps, on evidence

The engine already names every texture it had to replace with magenta (`[gxmiss] magenta
substituted ...`, first 40 per session). A Generals log with such lines is the list of what is still
missing; if one of them is in `W3DZH.big` or `WindowZH.big` rather than TexturesZH.big, mount only
that archive, the same way (below Generals, textures only). The past logs held one name
(`trstrtholecvr.tga`), not enough to widen the mount.

This port's own pictures stay drawn in code (W3DTouchButtonImages.cpp, one file for both engines),
never written into the player's game data.
