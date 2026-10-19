## Important: cross-play with PC is turned off

**At the request of the GeneralsOnline developers, Android-PC cross-play is turned off for an indefinite
time, in every version of this port** (1.3.0 and 1.4.0 received the change as an engine update). Android
players cannot join games hosted on PC and PC players cannot join Android games. **Online games between
Android players are not affected.**

**To mod authors and anyone building this port from source:** do not turn cross-play back on in your
builds. GeneralsOnline treats an Android client in a PC game as a violation of its rules -- the accounts of
the players using such a build risk being banned on GeneralsOnline. You would be putting other people's
accounts at risk, not your own.

## What's new since 1.4.0

### Command & Conquer: Generals (the base game) on Android

- The APK now carries **both games**, and the app is simply called **Generals**. The launcher's Home page
  has a **Generals | Zero Hour** switch; Play, the game folder and its checks follow it. Each game has its
  own folder, both its 30 Hz and 60 Hz engines.
- Generals runs on the same renderer, touch controls and launcher as Zero Hour: skirmish and the campaign
  (with the briefing voice at the right time), the general's power shortcut and powers usable by touch.
- **Zero Hour's 14-cell command bar** in Generals too, with the same layout of build and unit buttons, the
  order page arrow and the touch order buttons.
- Fixed in Generals: black terrain (fog of war over the whole map), game speed tied to the draw rate and
  flickering icons, power shortcut slots at the wrong size and place.
- Language packs work for Generals as well; Zero Hour and Generals each fetch their own translation file.
- **Generals takes the pictures it lacks from Zero Hour** when both are installed (the force-attack button
  among them; a drawn icon stands in otherwise). The game folder card on Home shows where each game takes
  what its folder lacks.

### Controls

- **Army column**: a **+** button above the left end of the control bar opens a column with the ten
  control groups; the control bar moves right while it is open, and **-** closes it.
  - Tap a group to select it (an empty group takes the current selection); a two-finger tap always assigns.
  - Hold a group until it fills red to clear it -- the message line confirms it, and empty groups show dim.
  - **>** widens the column with the **army buttons**: every combat unit on screen or on the whole map,
    planes, helicopters, ground forces (press Ground twice for the whole map). Dozers, workers, harvesters
    and buildings are never picked.
  - The buttons are the game's own, in each faction's colours.
- **Control scheme: Classic or Tactical** (launcher -> Interface -> Control scheme). Classic is the
  original game, and the army column opens with the control groups only. Tactical: combat units **attack
  on the move** (dozers and harvesters still just move), the army column opens with its army buttons, and a
  **higher camera** (zooms out up to four times further). Each one has its own switch too; **>** in the
  column still shows or hides the army buttons in any game.
- **Mouse and touchpad mode** (#39, in the same card, off by default): a pointer on screen. A connected
  mouse works as on a PC; without one your finger moves the cursor like a laptop touchpad -- tap to click,
  two-finger tap for a right click, two fingers to move and zoom the map. Edge scrolling and a cursor size
  slider; menus, lobby and the group column stay usable by finger directly.
- **Camera settings** in both games (Options -> Extras): maximum and minimum height, pitch, scroll speed
  and draw distance, with -/+ buttons, exact values and presets. The values are kept for the next start,
  and Defaults returns the game's own values (or a mod's). The window takes the look of the game's own
  Options screen -- and of a mod that restyles it -- and its text grows with the screen.
- **Touch orders** (#25): force attack as a mode, scatter, formation.
- **Physical keyboard:** every key with its original code -- the keypad, punctuation and hotkeys that did
  nothing before.
- **A real mouse scrolls the map** without the touchpad mode (#47): edge scroll and right-button drag; the
  wheel zooms smoothly; no lost button releases.
- The group controls no longer swallow touches meant for the command bar under them (#33).

### Game fixes

- **Old saves load again** (they were misread after 1.4.0).
- **Campaign:** the next mission no longer starts underneath the main menu after mission 1, and the score
  screen's frames no longer stay over it after Continue.
- **Options no longer crash** with the original Generals 1.00 menu layouts kept by some disc editions.
- **Blank text** on devices where the fonts could not be copied into the game folder (#36): the game now
  finds the bundled fonts itself.
- **Mods and language packs together:** a mod's own texts (its units, tooltips, menus) show as the mod
  wrote them and the rest of the game stays in your language, instead of MISSING everywhere -- also for
  mods that keep their text in `Data\Generals.str`.
- **Mod fonts** are found by the name inside the font file, also in `Fonts/` and the game folder.
- **Angry mob** members no longer appear in the map's corner (#30, #42).
- **No more slowdown after a replay check:** a check left diagnostics switched on that slowed every later
  game.
- **The main menu's background** is filmed from the original height again; the Challenge loading animation
  plays in step with its sound.
- **The money counter's description** no longer flickers when hovered.
- The line at the foot of Options shows only the game's version -- or a mod's name and version.

### Online

- **Custom maps in online lobbies** work: the map and its preview reach every player.
- The relay servers are tried in a new order, so networks that block one port connect in time (#37).
- **LAN over a virtual-LAN VPN** (#54): the address a virtual LAN such as MiniLAN or Tailscale gives the phone
  (100.x) can now be chosen under Options -> IP Address, to play LAN with friends on other networks (with
  Tailscale, join by address with Direct Connect). Wi-Fi stays the default.
- **Lobby and sign-in messages in your language** (they were English in every language); the message
  shown when trying to join a PC game says that cross-play is off.
- The launcher's GeneralsOnline screen no longer talks about PC play; its cross-play card stays, switched
  off, with the reason.

### Graphics and stability

- **Vulkan is hidden** for now: since the OpenGL ES work of 1.4.0 it shows black textures on many devices.
  Anyone still on Vulkan is moved to OpenGL ES once, with a message. It can be brought back for testing in
  **Tools -> Diagnostics -> Experimental Vulkan renderer**.
- A crash at match start on some Mali GPUs (Mali-G57) is fixed (#35, #41); GPU frame timing is now a
  diagnostics switch, off by default.
- A rare render-thread memory error fixed; crash logs say more about what the renderer was doing, and logs
  are shared as one zip.

### Launcher

- **Mods screen** (Home -> Mods): several mods at once, each with its own switch, in the order you choose
  (the upper one wins where two change the same file), with the mod's picture, description and its own page.
  Mods are checked for the game they belong to and for conflicts. Import a mod from a folder, a `.big` or a
  `.zip` into the game's `mods` folder; a complete game picked by mistake, or a `.rar`, is explained instead
  of failing. A mod can be moved between Generals and Zero Hour. **Share the list of mods** sends the order
  and versions to the friends you play LAN with. Each set of mods keeps its own saves and replays. If a game
  does not start twice in a row with the same mods, the launcher offers to start without them. Mods
  installed the old way (`!` archives in the game folder) keep working; online (GeneralsOnline) is played
  without mods. Home says when mods are installed but all switched off.
- **Appearance** (Interface tab): the usual dark look, navy and gold, a light day look, or following the
  phone.
- **Skip intro movies**: a switch under the game picker skips the logos and intro movies.
- **The game folder one or two levels down**: pick the folder the archive was unpacked into, and the
  launcher finds the game inside it. A complete Generals folder is no longer reported as incomplete.
- **Downloading language packs no longer fails with HTTP 403** on mobile networks.
- A support card and occasional announcements from the project.
- A sharper launcher icon.

## Install / update

Install over 1.4.0 -- settings, sign-in and game folders are kept. Requires Android 9 or newer, 64-bit (arm64).

Thanks to everyone who reported problems on GitHub and 4PDA.

## ❤️ Heroes of the project

**Diaa Abu Jazar — $2 via Boosty.** Thank you! Every donation goes to the Claude Code subscription this port is built
with, and keeps the updates coming. The game and every build stay free; if you enjoy it and want to help, the
addresses are in the launcher (**Help → Support the project**) and in the README.
