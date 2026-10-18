## Important: cross-play with PC is turned off

**At the request of the GeneralsOnline developers, Android-PC cross-play is turned off for an indefinite
time, in every version of this port** (1.3.0 and 1.4.0 received the change as an engine update). Android
players cannot join games hosted on PC and PC players cannot join Android games. **Online games between
Android players are not affected.**

**To mod authors and anyone building this port from source:** do not turn cross-play back on in your
builds. GeneralsOnline treats an Android client in a PC game as a violation of its rules -- the accounts of
the players using such a build risk being banned on GeneralsOnline. You would be putting other people's
accounts at risk, not your own.

> **Важно: кроссплей с ПК выключен.** По требованию разработчиков GeneralsOnline игра Android с ПК
> выключена на неопределённое время во всех версиях порта (1.3.0 и 1.4.0 получили это обновлением движка).
> Игры по сети между игроками на Android работают как раньше.
> **Авторам модов и всем, кто собирает порт из исходников:** не включайте кроссплей в своих сборках.
> GeneralsOnline считает Android-клиент в PC-игре нарушением правил, и аккаунты игроков такой сборки
> рискуют получить бан на GeneralsOnline. Вы подвергаете опасности чужие аккаунты, а не свой.

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
- **Touch orders** (#25): force attack as a mode, scatter, formation, jump to the last event.
- **The control-group panel no longer swallows touches** under it (#33), and group 9 works again.

### Game fixes

- **Old saves load again** (they were misread after 1.4.0).
- **Campaign:** the second mission no longer starts underneath the main menu after mission 1.
- **Options no longer crash** with the original Generals 1.00 menu layouts kept by some disc editions.
- **Blank text** on devices where the fonts could not be copied into the game folder (#36): the game now
  finds the bundled fonts itself.
- **The money counter's description** no longer flickers when hovered.

### Online

- **Custom maps in online lobbies** work: the map and its preview reach every player.
- Connection order of the relay servers changed so networks that block one port connect in time (#37).
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

## Install / update

Install over 1.4.0 -- settings, sign-in and game folders are kept. Requires Android 9 or newer, 64-bit (arm64).

Thanks to everyone who reported problems on GitHub and 4PDA.

## ❤️ Heroes of the project

**Diaa Abu Jazar — $2.** Thank you! Every donation goes to the Claude Code subscription this port is built
with, and keeps the updates coming. The game and every build stay free; if you enjoy it and want to help, the
addresses are in the launcher (**Help → Support the project**) and in the README.

> **Герои проекта:** Diaa Abu Jazar — $2. Спасибо! Каждый донат идёт на подписку Claude Code, с которой
> делается порт, и помогает выпускать обновления. Игра и все сборки остаются бесплатными; поддержать можно
> в лаунчере (**Справка → Поддержать проект**) и по ссылкам в README.
