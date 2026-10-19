## Important: cross-play with PC is turned off

**At the request of the GeneralsOnline developers, Android-PC cross-play is turned off for an indefinite
time, in every version of this port** (1.3.0 and 1.4.0 received the change as an engine update). Android
players cannot join games hosted on PC and PC players cannot join Android games. **Online games between
Android players are not affected.**

**To mod authors and anyone building this port from source:** do not turn cross-play back on in your
builds. GeneralsOnline treats an Android client in a PC game as a violation of its rules -- the accounts of
the players using such a build risk being banned on GeneralsOnline. You would be putting other people's
accounts at risk, not your own.

## Highlights

### Command & Conquer: Generals (the base game) on Android

The APK now carries **both games**. The launcher's Home page has a **Generals | Zero Hour** switch; Play,
the game folder and its checks follow it. Generals runs on the same renderer, controls and launcher as Zero
Hour, with its own game folder:

- Skirmish and the campaign, with the briefing voice at the right time.
- **Zero Hour's 14-cell command bar** in Generals too, with the same layout of build and unit buttons, the
  page arrow and the touch order buttons (force attack and the others).
- The general's power shortcut and the generals' powers usable by touch.
- Language packs work for Generals as well.

### Controls

- **Mouse and touchpad mode** (launcher → Interface → Controls, off by default): a pointer on screen. A
  connected mouse works as on a PC; without one your finger moves the cursor like a laptop touchpad -- tap to
  click, two-finger tap for a right click, two fingers to move and zoom the map. Edge scrolling, a cursor
  size slider, and menus, lobby and the group panel stay usable by finger directly.
- **Touch orders** (#25): force attack as a mode, scatter, formation.
- **Army column**: a **+** button above the left end of the control bar opens a column with the ten
  control groups (tap to select, or to assign an empty group; two-finger tap assigns; hold until the
  button fills red to clear -- the message line confirms it, and empty groups show dim). The control bar
  moves right while it is open. **>** widens it with the army buttons: every combat unit on screen or on
  the map, planes, helicopters, ground forces (press Ground twice for the whole map). The buttons are the
  game's own, in each faction's colours.
- **Control scheme: Classic or Tactical** (launcher → Interface → Control scheme). Classic is the original
  game. Tactical: combat units **attack on the move** (dozers and harvesters still just move), the army
  column opens with its **army buttons**, and a **higher camera** (zooms out up to four times further). Each
  one has its own switch too. The mouse and touchpad mode now sits in the same card.
- **Camera settings in Zero Hour** (Options → Extras): maximum and minimum height, pitch, scroll speed and
  draw distance, with -/+ buttons, exact values and presets.
- **Physical keyboard:** every key with its original code -- the keypad, punctuation and the hotkeys that
  did nothing before.
- **A real mouse scrolls the map** without the touchpad mode (#47): edge scroll and right-button drag; the
  wheel zooms smoothly; no lost button releases.

### Game fixes

- **Old saves load again** (they were misread after 1.4.0).
- **Campaign:** the second mission no longer starts underneath the main menu after mission 1.
- **Options no longer crash** with the original Generals 1.00 menu layouts kept by some disc editions.
- **Blank text** on devices where the fonts could not be copied into the game folder (#36): the game now
  finds the bundled fonts itself.
- **The money counter's description** no longer flickers when hovered.
- **Mods and language packs together:** a mod's own texts (its units, tooltips, menus) show as the mod
  wrote them and the rest of the game stays in your language, instead of MISSING everywhere.
- **Mod fonts** are found by the name inside the font file, also in `Fonts/` and the game folder.
- **Angry mob** members no longer appear in the map's corner (#30, #42).
- **Campaign:** the score screen's frames no longer stay over the next mission after Continue.
- **Mods that keep their text in `Data\Generals.str`** (Shockwave among them) show their own names instead of
  MISSING.
- **No more slowdown after a replay check:** a check left diagnostics switched on that slowed every later game.
- **The main menu's background** is filmed from the original height again; the Challenge loading animation
  plays in step with its sound.
- **Generals takes the pictures it lacks from Zero Hour** when both are installed (the force-attack button
  among them); the game folder card on Home shows where each game takes them from.

### Online

- **Custom maps in online lobbies** work: the map and its preview reach every player.
- Connection order of the relay servers changed so networks that block one port connect in time (#37).
- **Lobby and sign-in messages in your language** (they were English in every language), and the message
  shown when joining a PC game now says that cross-play is off.
- The launcher's GeneralsOnline screen no longer talks about PC play; its cross-play card stays, switched off,
  with the reason.

### Graphics

- **Vulkan is hidden** for now: since the OpenGL ES work of 1.4.0 it shows black textures on many devices.
  Anyone still on Vulkan is moved to OpenGL ES once, with a message. It can be brought back for testing in
  **Tools → Diagnostics → Experimental Vulkan renderer**.
- A crash on some Mali GPUs (Mali-G57) at match start is fixed (#35, #41).

### Launcher

- **Skip intro movies**: a switch under the game picker skips the logos and intro movies straight to loading.
- **The game folder one or two levels down**: pick the folder the archive was unpacked into, and the launcher
  finds the game inside it.
- Language packs: Zero Hour and Generals each fetch their own translation file.
- **Mods screen** (Home → Mods): several mods at once, each with its own switch, in the order you choose
  (the upper one wins where two change the same file), with the mod's picture and description. Import a
  mod from a folder, a `.big` or a `.zip` into the game's `mods` folder; a complete game picked by mistake,
  or a `.rar`, is explained instead of failing. Mods installed the old way (`!` archives in the game folder)
  keep working. Online (GeneralsOnline) is played without mods; LAN games work when everyone has the same
  mods. A mod can be moved between Generals and Zero Hour, and **Share the list of mods** sends the order and
  versions to the friends you play LAN with. Home says when mods are installed but all switched off.
- **Appearance** (Interface tab): the usual dark look, navy and gold, a light day look, or following the
  phone.

## Install / update

Install over 1.4.0 -- settings, sign-in and game folders are kept. Requires Android 9 or newer, 64-bit (arm64).

Thanks to everyone who reported problems on GitHub and 4PDA.

## ❤️ Heroes of the project

**Diaa Abu Jazar — $2.** Thank you! Every donation goes to the Claude Code subscription this port is built
with, and keeps the updates coming. The game and every build stay free; if you enjoy it and want to help, the
addresses are in the launcher (**Help → Support the project**) and in the README.
