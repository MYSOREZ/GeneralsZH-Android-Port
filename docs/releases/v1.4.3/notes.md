## What's new since 1.4.2

### Game fixes

- **Fog of war no longer gets stuck after loading a save** (#78): standing units and buildings went under
  fog for good, could not be selected and did not fire. Loading a save took every object's sight off the map
  once more. A save made after such a load keeps its broken fog; new saves are fine.
- **No crash when a building or turret with units inside is destroyed** (#59).
- **Generals: special powers no longer fly with the supply plane** (#71): a fuel air bomb and a paradrop
  ordered from the shortcut came as one plane and dropped both at the second target.
- **Generals' Challenge runs at normal speed on the 60 Hz engine** (#68).
- **The Extras draw distance is kept** as set, instead of coming back at the farthest (#74).
- **Mods' own names (armies, units) show without a language pack** (#77).
- The terrain is no longer drawn shifted after changing the terrain detail, and very large battles can no
  longer overrun a vertex buffer.

### New

- **Cheats in games against the computer**: in the in-game Options of a campaign, skirmish or Generals'
  Challenge game, **Cheats** gives +10000 money, instant build, reveal map and +1 General's point. They are
  part of the game's replay, and they do not exist in network games (LAN or online).
- **Starting camera height** (#82): a new row in Options → Extras sets the height every match starts at.
  By default it stays the maximum, as in the original game.
- **Hero and Camera buttons** in the army column (#73): select your hero (Ctrl+H) and reset the camera (Num5)
  without a keyboard.

### Android

- **The notification shade no longer drops you from a network game**: any overlay (the shade, a chat bubble,
  an incoming-call banner) stopped the whole game, network included, and the other players' timers ran out.
  The game now stops only when it is really sent to the background.
- **LAN: no more "you do not have the map" for official maps** when the host's phone writes the map path
  differently.
- **Bluetooth mice click as mice** (#60, #83): clicks no longer need a long press, and edge scrolling works.
- **The system bars and the notification shade can be used**, the game can be minimized, and it turns to both
  landscape sides when auto-rotate is on.
- **Folding a foldable no longer closes the game.**
- **Less heat and battery use**: the frame limiter now sleeps until the next frame instead of spinning the CPU
  for the last 2 ms of every frame.

## Cross-play with PC

Stays turned off, at the request of the GeneralsOnline developers. Online games between Android players are
not affected.

## Install / update

Install over 1.4.2 -- settings, sign-in and game folders are kept. Requires Android 9 or newer, 64-bit
(arm64).

Thanks to everyone who reported problems and tested the test builds on GitHub and 4PDA -- generalmilk,
Jayzer74, GitEin11, noconsole, Play4FunOnly, nxtskippr, Pixxy-13, BertTWO and many others.

## ❤️ Heroes of November

Thank you to everyone who supported the project in November so far -- $6.83 of the $20 monthly subscription:

**Diaa Abu Jazar, Nox and Iki GAMING, Sammy Denilson and an unknown general.**
