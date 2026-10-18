# PLAN-027: A mod system for the Android port (mods/ folder, alongside the legacy way)

Status: **design** (18/10/2026, owner's request). Nothing implemented yet.

## Where we are

Mods are installed the retail way: their `.big` archives go into the game folder, named with a
leading `!` (or `!!`, `!!!`) so they sort ahead of the retail archives, which all start with a
letter. The engine mounts every `.big` in the folder sorted by name and the first archive that has
a file wins (`ArchiveFileSystem::loadIntoDirectoryTree`). `-mod <dir|big>` (`m_modDir`,
`m_modBIG`) mounts one more archive or folder with the highest priority. On top of that this port
mounts the GeneralsOnline community patch (`500_900_CommunityPatch_CoreINI.big`) from the
user-data folder, and since 27/09 skips it when a `!` archive carries `Data\INI\...` (Contra 007
stopped starting with it, see below).

What is wrong with that, for players:

- Order is the file name. Two mods both called `!Something.big` decide by spelling.
- A mod cannot be switched off without moving files out of the game folder.
- Saves, replays and options of a modded game land in the same folders as the vanilla game's,
  and a modded save loaded in vanilla crashes.
- Nothing says what a mod changes, or which of two mods wins on a file.
- The language packs and fonts do not know mods exist (MISSING strings, PLAN in progress).

## Goals

1. A `mods/` folder: one sub-folder per mod, holding its `.big` files and/or loose files laid out
   as in the game (`Data/INI/...`, `Art/...`, `Window/...`), plus an optional `mod.ini`.
2. The legacy way keeps working unchanged: `!` archives in the game folder, `-mod`.
3. The two mix without surprises, with one documented priority order.
4. Mods that are known to break each other, or the community patch, are recognised before the
   game starts, and the game does not crash on a bad combination: it starts without the part
   that cannot be combined, and says so.
5. Each set of mods gets its own saves, replays and options.

## Layout on disk

```
<game folder>/
  !LegacyMod.big                 legacy, unchanged
  mods/
    Contra 009/                  one mod = one folder
      mod.ini                    optional
      !Contra009.big             any number of .big files (names no longer matter)
      Data/INI/...               and/or loose files
    HD Control Bar/
      ...
<user data>/
  mods.cfg                       written by the launcher: the enabled mods, in order
  Profiles/<profile>/            saves, replays, Options.ini of a mod set (see Profiles)
```

`mod.ini` (all keys optional; a folder without it is a mod named after the folder):

```
Name = Contra 009
Version = 009 Final
Game = ZeroHour            ; ZeroHour | Generals | Both
Kind = TotalConversion     ; TotalConversion | Gameplay | Cosmetic | Maps | Language
Requires = ...             ; other mod names
ConflictsWith = ...        ; other mod names
CommunityPatch = No        ; may the GeneralsOnline patch be mounted with it
Languages = english russian
```

`Kind` is detected when missing (below), so `mod.ini` is only needed to override a guess.

## Priority (highest first)

1. Loose files in the game folder (retail rule, unchanged).
2. **Managed mods** from `mods/`, in the order of `mods.cfg` (the launcher's list, top wins).
3. `-mod` (legacy command line), as today.
4. **Legacy mods**: `!`-archives in the game folder, by name, as today.
5. The community patch, when allowed (see Conflicts).
6. The retail archives.

Managed mods sit above legacy ones because their order is an explicit choice. A launcher switch
("Legacy mods first") reverses 2 and 4 for anyone whose setup depends on the old order. A legacy
archive that is also inside a managed mod's folder (copied twice) is mounted once, as managed.

## Engine side

- `ArchiveFileSystem::loadMods()` reads `mods.cfg` and mounts each enabled mod: its `.big` files
  (sorted by name inside the mod) and its loose tree, as one *layer* with an explicit priority,
  instead of relying on the archive names. The existing `-mod` path becomes "one more layer".
- Every file in the directory tree remembers its layer, so the log and the launcher can say which
  mod a file came from (also what `[GX-TEXT]` needs for mod strings).
- **Form masking** -- the general fix for the Contra/community patch crash. Zero Hour's INI sets
  exist in two forms: one file (`Data\INI\CommandButton.ini`) or a directory
  (`Data\INI\CommandButton\*.ini`); the loader reads both and merges them. When a higher layer
  provides one form of a set and a lower layer the other form, the lower layer's form of that set
  is masked. Contra replaces the single files, the patch provides directories: the patch's
  directories for those sets are masked, its other files stay, and the definitions are no longer
  loaded twice. A mod that only *adds* files to a set in the same form still merges, as retail.
- **Duplicate definitions do not crash**: where the INI parser meets an object/weapon/etc. defined
  twice by two layers, the higher layer's definition is kept and the lower one skipped with a
  log line, instead of the assertion path.
- Text (in progress, a27): string tables per layer -- the language pack, the mod's tables in the
  pack's language and in its own, the game's -- with the "mod changed or added this label" rule.
  Fonts: indexed by their internal family name across the game folder, `fonts/` and each mod.

## Launcher side

- **Mods screen**: the list of managed mods (enable switch, drag to reorder, size, kind, game) and,
  read-only below it, the legacy `!` archives found in the game folder.
- **Import**: a `.zip` or a folder through the system picker, unpacked into `mods/<name>/`. `.rar`
  and `.exe` installers cannot be opened on Android; the screen says so and how to repack.
- **Conflict report** before launch, from the archives' file lists (the launcher already reads
  `.big` directories, `DataPackInstaller.findDataMod`):
  - two TotalConversion mods together -> refused (pick one);
  - which files of mod A are overridden by mod B -> listed; INI overlaps are flagged as risky,
    art/window overlaps as cosmetic;
  - `Requires` / `ConflictsWith` from `mod.ini`;
  - community patch with a mod that carries INI -> patch off for this set (as today), unless
    `CommunityPatch = Yes`.
- **Known incompatibilities from the signed settings**: a list of mod pairs (by name and the hash
  of their main archive) that are known to break, published like the feature switches
  (`update/features.json` mechanism), so a newly found bad combination is blocked for everyone
  without a new APK.
- **Safe start**: the launcher writes a "starting with set X" marker; the engine removes it at the
  main menu (as `boot_pending` does for engines). Two failed starts in a row with the same set ->
  the launcher offers: start without mods, or without the last mod added (and remembers which
  mod it was).

## Profiles

A mod set gets its own `Profiles/<name>/` for saves, replays and `Options.ini` (the vanilla game
keeps the current folders). Loading a save from another set is impossible by construction, and
options a mod does not understand stay in the vanilla profile.

## Online and LAN

- GeneralsOnline: unchanged -- vanilla data plus the community patch; any INI-changing mod makes
  the INI checksum differ, and the launcher says so before the player tries.
- LAN and rooms: the lobby already compares checksums; the launcher shows the active mod set's
  name so players know what to match. The rooms protocol compares game files and can name the
  missing mod.

## Order of work

1. Engine: layers, `mods.cfg`, managed mods mounted with explicit priority; file -> layer map;
   log of what each layer provides. Legacy untouched.
2. Engine: form masking and duplicate-definition safety (fixes Contra + patch generally).
3. Launcher: Mods screen with list/enable/order, legacy list, import from zip/folder.
4. Conflict report, known-incompatibility list in the signed settings, safe start.
5. Profiles.
6. Text and fonts per layer (partly in a27).

Reference: koreazolfakar-droid/realkot have a Mod Manager (`ModManagerActivity`,
`ModImportService`, `ModHealthCheck`) built around one active mod (`-mod`); parts of its import
and health check may be reused, with attribution.
