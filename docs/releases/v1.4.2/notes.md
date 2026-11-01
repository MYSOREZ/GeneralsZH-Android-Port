## What's new since 1.4.1

### Game fixes

- **Generals: special powers of the buildings you build work again** (#62): Strategy Center battle plans,
  CIA Intelligence, the spy satellite and others only showed their tooltips, and their hotkeys did nothing.
  Games saved with the earlier versions get working powers when loaded.
- **Restart no longer doubles the game speed** (#58).
- **Tank treads move again** on the OpenGL ES renderer, together with other animated textures (#65).
- **Tanks and vehicles raise dust at 60 Hz** as they do at 30 Hz (#65).
- **The picture no longer freezes while the game and its sound keep running** on some phones (POCO /
  HyperOS) when the frame limit is above the screen's refresh rate.
- **The game asks Android for the screen rate it needs**: with no frame limit (LAN games) a 90 or 120 Hz
  screen is no longer held at 60 Hz (#69).
- No crash on maps with rally points that lack their node; a crash on a slow GeneralsOnline server when the
  room list arrived after the lobby was closed.
- The Replay check no longer slows down a skirmish played after it.

### Mods

- **Mods in subfolders** of `mods/`, with their own `GameData.ini`, and loose archives directly in `mods/`.
- An archive without `!` in its name that changes units or rules (such as `CWC.big`) counts as a mod, so the
  community patch stays out of its way (#55). `.gib` archives are renamed to `.big`.
- The Rise of the Reds shell map works (#61).

### Subtitles

- **Show subtitles** (launcher switch): the texts of the voice lines that the original data hides. On by
  default when a text language is installed; shown above the control bar, without the `**` marks.

### Launcher

- **The game in your phone's language** is offered on the first start (the language pack is downloaded and
  switched on, once per game), and an **update of the installed translation** is offered when a newer one is
  published.
- **File access** is asked for before the online data download, instead of failing with "could not create";
  Home warns when the access is missing.
- **AI scripts check** (#64): Home warns when the game folder lacks `Data\Scripts\SkirmishScripts.scb`, the
  file the skirmish AI needs (copy the whole game folder, not only the `.big` files).
- The system bars stay hidden after pulling down the notification shade.

## Cross-play with PC

Stays turned off, at the request of the GeneralsOnline developers. Online games between Android players are
not affected.

## Install / update

Install over 1.4.1 -- settings, sign-in and game folders are kept. Requires Android 9 or newer, 64-bit
(arm64).

Thanks to everyone who reported problems and tested the test builds on GitHub and 4PDA -- generalmilk,
Jayzer74, Piano23, Meto0401, Play4FunOnly and many others.

## ❤️ Heroes of October

Thank you to everyone who supported the project in October -- $18.73 of the $20 monthly subscription:

**Diaa Abu Jazar, hein htetsan, [UA]Den^, Zhask Dominator, Mukhiddin (WaruEdition), Shahre Masore, MAD ATOM
and an unknown general.**

Every donation goes to the Claude Code subscription this port is built with, and keeps the updates coming.
The game and every build stay free; if you enjoy it and want to help, the addresses are in the launcher
(**Help → Support the project**) and in the README.
