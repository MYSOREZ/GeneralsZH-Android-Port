## Highlights

**1.4.0 is about speed and fit:** a much faster OpenGL ES renderer, **upscaling** that renders the battlefield
below screen resolution while the interface stays sharp, and an **interface size** setting that makes menus and
buttons larger on small screens. Both settings are in the launcher:

| Setting | Where in the launcher |
|---|---|
| **Upscaling (Snapdragon GSR)** | **Graphics** page (shown with the OpenGL ES backends; Vulkan does not use it) |
| **Interface Size** | **Interface** page |

### Performance: a faster OpenGL ES renderer

The native OpenGL ES renderer (the default) got most of the work in this release:

- **Its own render thread.** The game no longer waits on the graphics driver for every draw: GL calls are
  queued and run on a separate thread.
- **Dynamic geometry streamed the way the driver expects** -- persistently mapped buffers, index data written
  into never-reused memory, fewer GL calls per draw, base-vertex draws and a shader program cache.
- **Cheaper translucent effects, sounds and file lookups**: particle and model draws with the same state are
  merged, `.wav` sounds are decoded natively instead of through FFmpeg, and the sound cache no longer drops
  sounds still in use.

**On the old Mali test phone, heavy battles went from about 23 to about 35 fps** -- before upscaling, which adds
on top of that.

### Upscaling: more frames, sharp interface

**Launcher → Graphics → Upscaling (Snapdragon GSR).** The 3D battlefield is rendered at a lower resolution and
sharpened back to full screen with Snapdragon Game Super Resolution; menus, the command bar and text are drawn at
full resolution afterwards, so they stay crisp. Four modes, as on PC: **Ultra Quality**, **Quality**, **Balanced**,
**Performance** -- the launcher shows the resolution each one renders at. On a phone whose GPU is the limit this
is the biggest single gain. Works with *OpenGL ES* and *OpenGL ES + ANGLE*; with *Vulkan* the option is hidden.

### Interface size: bigger buttons on small screens

**Launcher → Interface → Interface Size.** Menus, the command bar, the generals' power bar, the group panel and
the corner HUD can be made larger. The layouts themselves are scaled as they load, so buttons really grow --
larger touch targets, not just a larger picture -- and the main menu, emblems and power bar are kept inside the
screen at every size.

### Typing in the game: a real Android text field

Chat, lobby names and passwords are edited in an Android text field at the top of the screen: cursor,
selection, **copy and paste**, the keyboard's suggestions. OK or Enter sends, Back closes.

### Online (GeneralsOnline)

- **Cross-play with the current PC release (100126).** Every match against a 100126 PC went out of sync at
  frame 100; the game now follows the PC release's checksum rules on its own.
- **Sessions no longer die after 15 minutes.** The GeneralsOnline sign-in lasts fifteen minutes and the PC
  client renews it; the phone did not, so after a while every lobby request was refused and the lobby simply
  stopped changing. It now renews itself, during long lobby waits and long matches alike.
- **A failed online start gives the main menu back.** No connection, or a sign-in the server no longer
  accepts: the game says which (and asks you to sign in again in the launcher when that is the cause) and
  the main menu's buttons return -- no more killing the game.
- **Relay for the joining player** (#31). A player joining a lobby never got TURN relay credentials, so two
  players on "difficult" networks (mobile data, some Wi-Fi) could not connect. Connection retries and the
  server's mesh check follow the PC client's current rules.
- **The service's integrity checks are answered the way the PC client answers them**: loading-screen and
  score-screen screenshots, the match replay and the list of loaded libraries, uploaded to GeneralsOnline when
  the service asks for them.
- **Online game data**: the install no longer fails with "checksum mismatch", and the community data patch is
  not mounted over a mod (Contra and others start again); a launcher switch uses it with mods anyway.

**One account per device.** The service accepts one active sign-in per account: signing in with the same
account on a second phone (or on the PC) signs the first one out. Use a separate account on each device.

### LAN games between phones

LAN lobbies work on Android: the game listens for lobby broadcasts on the Wi-Fi network rather than on the
mobile-data interface, and holds the multicast lock Android requires. (Fix from the `android-lan-fix` branch
of JetErorr's fork.)

### Game speed and controls

- The skirmish **Game Speed** slider works again; its top position is four times normal speed.
- **Hold a command button to read its description** -- also on commands that are not available yet -- without
  buying or building anything.
- Smoother camera momentum after a drag on high-refresh screens.

### Launcher

- **Support the project** card on the Help page (Boosty -- like Patreon -- and USDT). Its contents come with the
  signed settings, so they are kept current without a new APK.
- Settings from the signed updates now also cover the data package's address and the PC checksum rules, so
  fixes like the "checksum mismatch" one reach older launchers too.
- List rows on the Tools and Help pages respond to a tap.

### Fixes

- An audio exception in looping and multi-part sounds, which could stop a sound or flood the log.
- A command-bar description title no longer jumps between two font sizes.
- Tokens are removed from the launcher's network log before it is shared.

**Full Changelog**: https://github.com/MYSOREZ/GeneralsZH-Android-Port/compare/v1.3.0...v1.4.0

## Support the project

This port is developed with Claude Code, and the subscription is paid out of pocket. If the port is useful to
you, donations are welcome -- on [Boosty](https://boosty.to/antikeks_m/donate) (a Patreon-like platform,
bank cards from anywhere) or in USDT, see the launcher's Help page or the
[README](https://github.com/MYSOREZ/GeneralsZH-Android-Port#support-the-project). The game and every build stay
free; nothing is unlocked by donating.
