# PLAN-029: Launcher ideas from realkot's fork

Status: **in progress** -- theme and Mods screen taken 18/10/2026 (a27). Survey from 18/10/2026, owner: "his launcher is well made, take ideas"). realkot's
GeneralsX (https://github.com/realkot/GeneralsX) is a fork of this port; the author allowed us to
take his work, with credit. His launcher is ours plus the parts below. Nothing taken yet.

| His part | What it does | Take? |
|---|---|---|
| `ModManagerActivity`, `ModManager` | Mod library under `<game>/Mods/`, one active mod at a time (engine's `-mod <dir>`), shown on Home | **Taken** (a27): one library and active mod per game, from Home; next: several mods with order |
| `ModImportService` | Transactional import of a folder, `.big` or `.zip`: staged in `Mods/.installing/<uuid>`, path-checked, BIG headers validated, zip traversal rejected, atomic rename, cancellable | **Taken** (a27) as is |
| `ModHealthCheck` | Read-only check of an installed mod's structure, bounded disk walk | **Taken** (a27) |
| `ModLibraryInfo` | Size of the library | Yes |
| `ThemeHelper` | Launcher Light / Dark / System theme (game untouched) | **Taken** (a27) with his navy/gold palette and the emblem launch screen |
| `LauncherUi` + Home redesign | Navy/gold "tactical" Home, compact navigation rail, active mod on Home | Look at it on a device first; his banner art carries EA's Zero Hour logo and fits one game only |
| Transfer status kept between tabs | Update/download progress survives switching tabs | Yes |
| Audio readiness hint | Diagnostics says whether the game's audio files are really there | Not as is: it looks for fixed names (Audio/Speech/Music.big) that differ between editions and languages; would need the engine's own list |
| `UpdateTrust` | Pins *his* update key | No -- ours is GXSignedSettings / UpdateManager with our key |
| `-nologo` | Skip logos | Already have "Skip intro movies" |

Order: theme and the small Diagnostics/progress items can go into any build; the mod pieces go
with PLAN-027 step 3 (Mods screen), adapted from "one active mod" to "enabled mods in order".
Every string added goes into all 13 locales; credit realkot in code comments, notes and README.
