# PLAN-027: A mod system for the Android port (mods/ folder, alongside the legacy way)

Status: **step 1 in a28** (18/10/2026): `mods/` folders, several mods with switches and order,
pictures and mod.ini, import (folder / .big / .zip, a whole game and .rar recognised), the engine
mounting them above the game and legacy `!` archives (GXManagedMods.h, ArchiveFileSystem::loadMods),
community patch left out when one carries INI, mod texts counted as mods. Not yet: loose files in a
mod folder, conflict report, profiles, safe start.

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

Text a player reads can be given per language, so a mod page follows the launcher's language
(the 13 it ships): `Name.ru = ...` (implemented: Name, Version, Author, Description, Image, each with `.<lang>`), `Description.de = ...`, and `readme_<lang>.txt` beside
`readme.txt`; without the player's language, English, then whatever the mod has. Everything the
launcher itself says on the Mods screen -- labels, kinds, warnings, conflict lines, dialogs --
is launcher strings, in all 13 locales like every other string.

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

- **Mods screen** (owner's requirements: switches, reordering, the mods' pictures):
  - one card per managed mod: its picture, name, version, author, kind badge (total conversion /
    gameplay / cosmetic / maps / language), which game it is for, size, and an **on/off switch**;
  - **reordering** by dragging a card's handle (and up/down buttons for accessibility); the top
    card wins, and the screen says so in one line above the list;
  - the picture: `Image = ...` in `mod.ini`, else the first of `cover.*`, `preview.*`, `logo.*`,
    `icon.*` (png/jpg/webp) in the mod folder, else the mod's own main-menu or loading-screen art
    found in its archives (`Art/Textures/*mainmenu*`, `*loadscreen*`, read from the `.big` with
    the existing reader and decoded from TGA/DDS), else a placeholder with the mod's initials;
  - tapping a card opens its page: a larger picture and screenshots (`screenshots/` in the folder),
    the description (`Description` in `mod.ini`, or `readme.txt`), the files it changes grouped
    (INI, maps, art, window, text, fonts), its conflicts with the other enabled mods, and buttons:
    open its folder, export, delete;
  - a header with the active set's name and profile, and **Play** with that set;
  - filters: all / Zero Hour / Generals / enabled; search by name;
  - below the list, read-only, the legacy `!` archives found in the game folder with their kind,
    and a button to move one into `mods/` (it becomes a managed mod, with its order kept);
  - warnings inline on the cards (red: will not start together; amber: overlaps that change
    gameplay; grey: cosmetic overlaps), and a summary bar above Play.
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

## Online and LAN (owner, 18/10/2026: mods must work in multiplayer, across launcher versions)

What decides whether two players can play together is not the launcher but two numbers every
lobby already compares: `exe_crc` (on Android a fixed retail value + the game version + the two
multiplayer scripts) and `ini_crc` (the INI data the engine actually loaded). Text, fonts, art,
the control scheme and the launcher's version do not enter either, and do not desync. So:

1. **Same files -> same game, whichever way they were installed.** A mod mounted from `mods/`
   by a new launcher and the same mod as `!` archives under an old launcher must produce the same
   loaded data, so the same `ini_crc`, and must play together. Rules that follow:
   - the managed order of a single mod, or of mods that do not overlap, gives exactly the legacy
     result (tested by comparing `ini_crc` of both installs of Contra, ShockWave, Rise of the Reds);
   - form masking and duplicate-definition skipping (Engine side) change the loaded data only in
     combinations that crashed before -- they can never make a combination that worked under an
     old launcher load differently;
   - text and font layers stay client-only (they already are).
2. **Engine logic stays compatible between versions.** A lockstep game desyncs silently when two
   engines simulate differently, mods or not. A change to game logic (not rendering, input or
   text) is called out in the release notes and its diary entry, and kept behind the conditions
   that broke before (e.g. the angry-mob guard only touches a spawn left at the origin). If a
   real logic break is ever unavoidable, a "logic revision" number joins `exe_crc` so mixed
   versions are refused in the lobby with a clear message instead of desyncing mid-game.
3. **Mod set name in the lobby.** The launcher computes a mod-set fingerprint (names + hashes of
   the enabled mods' archives, order included) and hands it to the engine; the LAN lobby and the
   rooms show it next to each player, and on a CRC mismatch the message names what differs
   ("Contra 009 Final" vs "no mods") instead of the retail "different version".
4. **LAN:** works today with identical mods (the retail check). Added: the fingerprint above, and
   in the launcher "share this mod set" -- an export (zip of `mods/<name>` + `mods.cfg`) to send
   to a friend and import, so both have byte-identical files.
5. **GeneralsOnline:** the client reports `ini_crc` to GeneralsOnline at login, when creating a
   lobby and in quickmatch. A modded game reports a different number, and whether GeneralsOnline
   accepts modded lobbies is GeneralsOnline's rule, not ours -- the same as cross-play. Before
   any of this ships for online: ask the GeneralsOnline developers. Until they agree, the launcher
   says before connecting that online play is vanilla + community patch only, and the true
   `ini_crc` is always reported -- never a substituted one (that would be the kind of evasion
   that gets accounts banned, see the cross-play notice).
6. **Rooms (PLAN-023, paused):** if they return, they compare the fingerprint and can offer the
   missing mod's download link from `mod.ini` (`Url = ...`).

## Later: a modern mod format (owner, 18/10/2026 -- once mods are stable)

Owner: `.big` is uncomfortable for players and for the game; C&C 3 and Red Alert 3 moved to a newer
mod system -- modernise the same way, so the game loads faster and mods cause fewer problems.

What C&C 3 / RA3 actually changed (SAGE 2, the Mod SDK). The container stayed: their mods are still
`.big` files. What changed is what is inside and what describes it:

- **Compiled data, not text.** The SDK's build step (BinaryAssetBuilder) turns XML/INI definitions,
  models and textures into binary asset streams (`.manifest` + `.bin` / `.relo` / `.imp`). The game
  loads them almost as-is, with no INI parsing at start-up and no runtime surprises from a typo:
  errors are found when the mod is built, not when a player starts it.
- **A descriptor.** A `.skudef` file names the mod, its version and the archives it loads, in
  order, on top of the game's own -- the game does not guess from file names.
- **Mods live outside the game.** One folder per mod under the user's Documents (`Mods/<name>/`),
  chosen in the launcher; the game folder is never touched.

This port already has the last two in its own form: one folder per mod under `mods/`, `mod.ini` as
the descriptor, `mods_active.txt` as the explicit order (no `!` naming). What is left, in order:

1. **Loose folders without packing.** Mount a mod folder directly as a layer (a directory archive
   next to the BIG one), instead of `ModPacker` writing `!!loose_files.big`. Players unpack a mod
   and it works; no second copy on the phone.
2. **A compiled INI cache per mod set.** Parse the INI of game + mods once, store the parsed
   objects in a binary cache keyed by the mods' fingerprints (`ModAnalysis.fingerprint`) and the
   engine build; later starts load the cache. This is where start-up time and the "mod breaks on
   some INI line" problems go. Hard constraints:
   - the INI checksum (`ini_crc`, lobby gate, replays) must stay the checksum of the *source* INI,
     computed exactly as today -- the cache is a loading shortcut, never a different identity;
   - determinism (Golden rule 7): the cache must produce bit-identical objects to a parse, checked
     by loading both ways and comparing a CRC of the result in a debug build;
   - any mismatch (engine build, mod files changed, cache unreadable) falls back to parsing.
3. **A modern container, optional.** A compressed archive with an index (zip/zstd) accepted
   alongside `.big` -- smaller downloads and less storage. `.big` stays readable forever: the
   retail data, every existing PC mod, replays and multiplayer with PC players depend on it.
4. **A mod build step in the launcher or a desktop tool**, like the SDK's: checks a mod (INI
   references, missing textures, duplicate definitions) and writes the cache + descriptor, so
   errors show up before a match, not in it.

**Owner, 18/10/2026: the new format is the main one, `.big` becomes legacy, and the launcher
converts.** So:

- **Two formats, one engine path.** The engine reads the new format (folder layer + compiled
  cache, or the new container) as the primary mod format, and `.big` stays fully supported as the
  legacy one -- the retail data, `!` archives in the game folder, `-mod`, mods from PC sites.
  Neither is ever dropped from the engine.
- **A converter in the launcher (Mods screen).** For a mod made of `.big` files: "Convert to the
  new format" unpacks the archives into the new layout in the mod's folder, keeping the archives'
  priority as file order (the first archive in name order wins, exactly as mounted today), writes
  `mod.ini` if there is none, and builds the cache. Rules:
  - **Verified before anything is removed:** every file of every archive is compared (size + CRC)
    with the converted copy, and the game's INI checksum with the mod mounted both ways must be
    equal -- a converted mod is the same mod for the lobby, replays and LAN partners who still
    have the `.big`.
  - The `.big` files are kept until the check passes; then the player chooses whether to delete
    them (space) or keep them (sharing with PC players). Converting back (new -> `.big`) is the
    same writer `ModPacker` already has, for handing a mod to a PC player.
  - Progress and Cancel, in a foreground service like the import (a 500 MB mod takes a while);
    cancel leaves the mod as it was.
  - Import offers the conversion right after a `.big` mod is installed, never forces it.

Not before: the current system has been stable through a release or two, and the GeneralsOnline
question (modded online, above) is settled -- a format change must not split players again.

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
