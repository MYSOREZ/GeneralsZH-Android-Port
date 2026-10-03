# Command & Conquer Generals: Zero Hour — Android

> **This is an unofficial, community-made fan port.** It is not affiliated
> with, endorsed by, or produced by Electronic Arts, Westwood Studios, or any
> other rights holder, and it is not an official Google Play / App Store
> release. It's a fork maintained by volunteers (see
> [Lineage & credits](#lineage--credits)) built from EA's own GPL v3 source
> release. If you saw this described anywhere as an "official" release, that's
> wrong — please don't spread it further.

### Generals — the front line just went mobile.

You thought this war was over? That the GLA was finished, the Particle Cannon had fallen silent
and the last SCUD Storm had burned out? Think again. The war has gone global — and commanding it
from a bunker, chained to a desk behind a stationary PC, is no longer enough. The front needs a
new breed of officer: forward commanders who deploy their HQ anywhere and lead the charge on the
move. **The world needs more Generals. Your time has come, Mobile Generals.**

*Command & Conquer: Generals – Zero Hour* launches a full-scale invasion of Android, and nine
Generals are waiting for your orders. Rule the skies with General Granger's air power, burn
through the enemy with General Alexander's lasers, or unleash General Townes' Particle Cannon for
the USA. Steamroll the line with General Kwai's Overlords, bring down General Tao's nuclear fire,
or bury the enemy under General Fai's endless infantry for China. Or fight the GLA way — poison
the battlefield with Dr. Thrax's toxins, rig it to blow with General Juhziz, and strike unseen
with Prince Kassad's stealth.

Build your Command Center, secure your supply lines and wipe the enemy off the map — straight
from your touchscreen. And the desk-bound commanders are no longer out of reach: **meet PC players
on the same online battlefield.**

*General, the uplink is ready. Target coordinates received — engage!*

<img width="500" height="281" alt="IMG_3457_500" src="https://github.com/user-attachments/assets/aeaf6692-36e6-40c8-b9f8-8066d014ec4b" />

**Zero Hour running natively on Android** — campaign, skirmish, Generals Challenge,
and full online multiplayer (GeneralsOnline: lobby, custom match, quickmatch,
persona/stats, friends & social), with touch controls built for RTS (tap-select,
drag-box, long-press deselect, two-finger pan, pinch/anchor zoom). No emulation:
this is the real 2003 engine compiled for ARM64, rendering DirectX 8 →
[DXVK](https://github.com/doitsujin/dxvk) → **Vulkan native** — no translation
layer beyond DXVK itself, since Android speaks Vulkan directly.

There is a **second, independent renderer**: DirectX 8 → **OpenGL ES 3.0**,
through a native translation layer written for this port (optionally routed
through [ANGLE](https://github.com/google/angle)), with **no DXVK and no
Vulkan involved at all**. It started as the way to run on phones whose Vulkan
driver can't carry DXVK and is now the default, because it has worked on every
device tested so far; Vulkan remains one tap away in the launcher.

Built on EA's GPL v3 source release, standing on a chain of community work —
[TheSuperHackers](https://github.com/TheSuperHackers/GeneralsGameCode),
[Fighter19's original Unix port](https://github.com/Fighter19/CnC_Generals_Zero_Hour), and
[fbraz3/GeneralsX](https://github.com/fbraz3/GeneralsX) — this fork is the
Android port: native touch controls, the in-app launcher, GeneralsOnline
multiplayer with cross-play against PC, and language packs. See
[Lineage & credits](#lineage--credits) for who built what, and
[the story of the port](docs/port/PORT_STORY.md) for how it went. The macOS
and iOS/iPadOS builds inherited from the original project are not maintained
here; their notes are in [docs/port/APPLE_PLATFORMS.md](docs/port/APPLE_PLATFORMS.md).

**No game assets are included or distributed.** You need your own copy
([Steam](https://store.steampowered.com/app/2732960/), ~$5 on sale).

## Status

| Feature | Status |
|---|---|
| Campaign / Skirmish / Generals Challenge | ✅ Working |
| Rendering — OpenGL ES (DirectX 8 → GLES 3.0, no DXVK) | ✅ Working — **the default.** A native backend written for this port; it works on every device tested so far, including ones whose Vulkan driver renders corrupted graphics. |
| Rendering — Vulkan (DirectX 8 → DXVK → Vulkan) | ✅ Working — selectable in Setup. Native Vulkan 1.3 (Adreno 7xx/8xx), adaptive Vulkan 1.1 fallback (Mali-G76/G57). Faster where the driver handles it; unusable on some (PowerVR BXM, Samsung Xclipse — see [Known issues](#known-issues)). |
| Audio | ✅ Working (OpenAL, OpenSL/AAudio backends) |
| Video / cutscenes | ✅ Working (FFmpeg) |
| Touch controls | ✅ Working — native touch, not mouse emulation (see [Touch controls](#touch-controls)) |
| Online multiplayer (GeneralsOnline) | ✅ Working — real matches between real players, P2P transport |
| Cross-play with PC (Windows) GeneralsOnline players | ✅ Working — since 1.3.0, against the current PC release (100126) since 1.4.0; the PC player turns their anti-cheat off. If a match goes out of sync, send the logs and the replay |
| LAN games between phones | ✅ Working — on the same Wi-Fi network |
| Game text languages | ✅ English plus 12 packs: Russian, Ukrainian, German, French, Spanish, Brazilian Portuguese, Polish, Interslavic, Simplified Chinese, Korean, Arabic, Persian — right-to-left layout and CJK glyphs included. [Add yours](languages/README.md) |
| Updates without a new APK | ✅ Signed engine builds and network settings straight from this repository (Home → Updates) |
| Simulation rate | ✅ 30 Hz (retail) or 60 Hz, chosen in the launcher — the APK carries both engines |
| Upscaling (OpenGL ES) | ✅ The 3D scene rendered below screen resolution and sharpened back (Snapdragon GSR), interface at full resolution — Launcher → Graphics |
| Interface size | ✅ Larger menus, command bar and HUD for small screens — Launcher → Interface |
| Text input | ✅ Chat, lobby names and passwords in an Android text field: cursor, selection, copy and paste |
| Performance on Vulkan-1.1-only GPUs (Mali) | ⚠️ Playable, but CPU-bound — expect lower FPS and occasional freezes on weaker/older phones |

- **Android**: primary target. Grab a prebuilt APK from
  [Releases](../../releases/latest) — no toolchain needed. Campaign, skirmish,
  and Generals Challenge run natively. **Online
  multiplayer works, including actual matches — and, since 1.3.0, against PC
  players**: [GeneralsOnline](https://www.playgenerals.online) (the community
  service by the GeneralsOnline Development Team that replaced the long-dead
  GameSpy servers; this port brings its client to Android) drives account login,
  the multiplayer lobby, Custom Match (create/browse/join, live room + player
  lists, chat), Quickmatch, My Persona (stats/rank), and Communicator
  (friends/social) — and matches now actually start and play: a P2P transport
  (Valve's [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets),
  built from source for Android with native ICE/STUN/TURN, no external WebRTC
  dependency) replaces the legacy transport that internet games never had a
  real implementation for. This has been shaken out against real players and
  real devices, including bug reports from outside testers via this repo's
  [issue tracker](../../issues) — see
  [`docs/port/ANDROID_PORT.md`](docs/port/ANDROID_PORT.md) for the device/driver
  matrix and the full bring-up log.

## Touch controls

Battlefield input is **native**: a finger is not turned into a mouse. Taps,
double taps, long presses, the two-finger cancel and ability targeting are
resolved directly against the engine's own decision-making — pick what is
under the finger, project it into the world, ask the engine what order that
means. The orders produced are identical to the ones the mouse path
produces, so multiplayer and replays are unaffected.

Taps on the control bar and menus are still ordinary clicks, deliberately: a
tap on a button *is* a click, and the window manager's handling of it is
correct. The window manager also gets first refusal on every battlefield tap,
so a tap on a panel never falls through to the map underneath.

| Gesture | Action |
|---|---|
| Tap on the map | Select what is under your finger, or — with units selected — give them the order that point implies (move, attack, enter, repair …) |
| Tap a unit you own | Select it. If the engine says your current selection has a *specific* interaction with it (get in, repair), that happens instead |
| Double-tap | Select every unit of that type on screen |
| Press and hold, then drag | Selection box |
| Drag | Pan the camera (direct, no mouse involved) |
| Two fingers | Pan and zoom together |
| Long press on the map (~0.6s, no movement) | Cancel an armed ability or a pending building; otherwise clear the selection |
| Two-finger tap | The same cancel |
| Tap a UI button | Press it |
| Hold a UI button | Read its description, without pressing it |
| **With an ability armed**: touch the map, drag, release | Aim it — the radius circle follows your finger; release fires it where you let go; a second finger cancels |
| **With a building picked**: tap where it goes | Place it. The ghost appears under your finger, not before you point |
| Drag inside a list (games, players, chat, maps) | Scroll it |
| Arrow button, bottom right of a builder's command bar | Flip to the second page of structures |
| Force-attack / waypoint buttons on the command bar | The touch equivalents of Ctrl-click and Alt-click |

More on how gestures are classified, why screen-edge scrolling is off on touch, the **Touch
input overlay** for reporting control problems, and how to add a gesture:
[`docs/port/TOUCH_CONTROLS.md`](docs/port/TOUCH_CONTROLS.md).

## Quick start — Android

DirectX 8 → DXVK → **Vulkan native**, or the OpenGL ES renderer below. DXVK's own minimum was lowered from Vulkan
1.3 to **Vulkan 1.1**, with an adaptive feature/extension fallback path, so
it now runs on a much wider range of hardware: Snapdragon with Adreno
7xx/8xx (native 1.3), older Adreno below 1.3 (via an optional bundled Mesa
Turnip fallback driver), and Vulkan-1.1-only Arm Mali GPUs (Mali-G76,
Mali-G57, and similar Bifrost/Valhall chips — e.g. the Redmi Note 8 Pro) are
all supported. A device with no usable Vulkan driver at all still gets a
clear on-screen message instead of silently closing. Frame rate on
Vulkan-1.1-only / older-CPU devices is CPU-bound, not GPU-bound — expect
lower FPS and occasional freezes on weaker phones; see the doc for the full
device/driver matrix and driver-replacement options.

**If Vulkan doesn't work on your phone, there is a second renderer.** The
Setup app has a **Render Backend** picker with three options: *Vulkan*
(DXVK), *OpenGL ES* (the default) and *OpenGL ES + ANGLE*. The GLES options do
not use DXVK or Vulkan at all — they run DirectX 8 through a translation
layer written for this port
([`Core/Libraries/Source/d3d8gles/`](Core/Libraries/Source/d3d8gles)) straight
onto OpenGL ES 3.0, which every Android GPU speaks. That covers devices whose
Vulkan driver exists but can't carry DXVK, which previously had nothing to
fall back on. Switching backends needs no rebuild: change it in Setup and
restart the game. GLES is the default because it works on every device tested
so far; Vulkan can be faster where the driver handles DXVK well.

**Simplest option — no build, no CI**: grab a prebuilt APK from the
[Releases page](../../releases/latest) and sideload it.

Every build is signed with the same committed key, so a newer APK installs
**over** an older one without uninstalling. A fork can also build in GitHub
Actions (**Actions tab → Build Android → Run workflow**, manual only), though
that workflow ships the 30 Hz engine only.

**No adb needed, for setup or logs**: the game's icon opens a launcher with an
in-app folder picker (point it at wherever you copied your own game files —
Downloads, an SD card, anywhere), a log viewer with Clear/Share buttons, the
GeneralsOnline account and network data, language packs and updates. See [docs/port/ANDROID_PORT.md §4](docs/port/ANDROID_PORT.md#4-game-data-and-first-run--the-in-app-setup-flow-no-adb-no-pc-needed)
for the full first-run walkthrough. A default log is small and readable —
if a bug report needs more (frame-timing breakdown, full trace, DXVK HUD
counters, Vulkan validation), see [**Diagnostic marker files**](docs/port/ANDROID_PORT.md#diagnostic-marker-files-opt-in-extra-logging)
for which plain-text file to drop into the game folder and what it turns on.

Building locally instead needs the Android NDK (r26+), vcpkg, meson/ninja:

```sh
cd GeneralsX
git submodule update --init references/fbraz3-dxvk
export ANDROID_NDK_HOME=~/Android/Sdk/ndk/<version>
./scripts/build/android/build-dual-hz.sh           # both engines (30 Hz + 60 Hz) and the APK
```

**→ The full guide (device/driver matrix, storage layout, multiplayer
architecture, bring-up log): [docs/port/ANDROID_PORT.md](docs/port/ANDROID_PORT.md)**

## Where things are

| Path | What it is |
|---|---|
| [`docs/port/ANDROID_PORT.md`](docs/port/ANDROID_PORT.md) | The Android port: architecture, GeneralsOnline multiplayer backend, device/driver matrix, build + bring-up log |
| [`docs/port/PORT_STORY.md`](docs/port/PORT_STORY.md) | How the port went: GameSpy's death, async callbacks, storage, the allocator hunt |
| [`docs/port/TOUCH_CONTROLS.md`](docs/port/TOUCH_CONTROLS.md) | How touch gestures are recognised, and the input overlay for bug reports |
| [`languages/`](languages/README.md) | Game-text language packs, and how to add one |
| [`docs/port/APPLE_PLATFORMS.md`](docs/port/APPLE_PLATFORMS.md) | The macOS / iOS / iPadOS builds inherited from the original project — not maintained here |
| `docs/port/RELEASE_CHECKLIST.md` | Gate for public release |
| `scripts/get-assets.sh` | Steam asset fetcher (your own copy; app 2732960) |
| `scripts/build/android/` | Android build and packaging |
| `android/` | Gradle shell app (SDLActivity) that packages `libmain.so` + DXVK into an APK, plus the Setup/FolderPicker/LogViewer/GeneralsOnline-account activities |
| `GeneralsMD/Code/GameEngine/Source/GameNetwork/GeneralsOnline/` | The GeneralsOnline multiplayer client: auth, lobby, rooms, stats, matchmaking, social — talks to a REST + WebSocket backend, not GameSpy |
| `Core/Libraries/Source/d3d8gles/` | The DirectX 8 → OpenGL ES 3.0 renderer: device/state emulation, fixed-function-to-GLSL shader generation, texture upload (including a software BC1-3 decoder for GPUs without S3TC) |
| `Patches/dxvk-android.patch` | DXVK changes the Android d3d8/d3d9 `.so` builds are built from |

## Known issues

- Android on Vulkan-1.1-only GPUs (Mali-G76, Mali-G57, and similar) is
  CPU-bound: expect lower frame rates and occasional freezes on weaker/older
  phones, especially during map/mission loading, and please share logs if you
  hit something worse than that — see
  [docs/port/ANDROID_PORT.md §2](docs/port/ANDROID_PORT.md#2-the-device--driver-matrix-read-this-before-filing-black-screen-bugs)
  for the device/driver matrix.
- Vulkan renders corrupted graphics on some GPU drivers, and no setting in
  Setup can fix it — the corruption is below the game. Magenta patches,
  distorted geometry and duplicated/overlapping menu elements have been
  reported on PowerVR BXM and Samsung Xclipse. This is why OpenGL ES is the
  default; if Vulkan looks wrong on your device, switch back to it in Setup →
  Render Backend, which also now shows your GPU's own name.
- Magenta textures are usually a game-files problem rather than a renderer
  one, and the build says which: `[gxmiss] magenta substituted (<reason>):
  <file>` names the file and why (no thumbnail, archive missing, unreadable),
  and `[d3d8gles] MAGENTA:` names an unsupported texture format. Zero Hour is
  an expansion — if base *Generals* archives (`Terrain.big`, `Textures.big`,
  `W3D.big`) are absent, most of the artwork has nowhere to come from. Setup
  lists any missing archive by name and can be pointed at a separate base-game
  folder.
- One GeneralsOnline account per device: the service keeps one active sign-in per
  account, so signing in on a second phone (or on the PC) with the same account signs
  the first one out, and its game then asks you to sign in again. Use a separate
  account on each device.
- Cross-play with PC players is new. Both sides must compute the same game
  frame by frame, and the remaining differences are found from real matches:
  if a game against a PC goes out of sync, please send the launcher's logs
  **and the replay** (yours and, if you can, the PC player's `.rep`) in an
  [issue](../../issues).

## Support the project

This port is developed with [Claude Code](https://claude.com/claude-code) (Anthropic's Claude),
and the subscription is paid out of pocket. If the port is useful to you and you'd like to help
keep it going, donations are welcome — they go to that subscription.

**[Boosty](https://boosty.to/antikeks_m/donate)** (a Patreon-like platform; sending a tip needs a
Boosty account) takes a bank card, from Russia and from abroad
(the amount is shown in your currency and converted automatically). Or send USDT:

| Currency and network | Address |
|---|---|
| **USDT — TRON (TRC20)** | `TAQHCF733ovKpvBjUgvkE6wHxkntnKZ6br` |
| **USDT — BSC (BEP20)** | `0x52c05c81485d68367385ff389cf19a453f036310` |
| **USDT — TON** (no memo needed) | `UQAOdBpFSPhlgbvUIJ2O2w2NuwWashaNjFWDsOirDqH9kGbR` |

Send **USDT only, and only over the network written next to the address** — any other token,
or USDT over a different network, will be lost. Donations support development only: the game
and every build stay free, and nothing is unlocked by donating. *Command & Conquer* is a
trademark of Electronic Arts; this is an unofficial fan project.

## Lineage & credits

This port is the newest link in a long chain, and the earlier links did foundational
work that this repo inherits everywhere:

- **Westwood / EA Pacific** — the game; **EA** — the GPL v3 source release
- **[TheSuperHackers/GeneralsGameCode](https://github.com/TheSuperHackers/GeneralsGameCode)** —
  the community mainline: build modernization, VC6→modern toolchain, and much of the
  cross-platform groundwork, including the FFmpeg video backend authored by
  **[feliwir](https://github.com/feliwir)** (of [OpenSAGE](https://github.com/OpenSAGE/OpenSAGE)),
  who also authored the OpenAL audio device work this port's audio stack builds on
- **[Fighter19/CnC_Generals_Zero_Hour](https://github.com/Fighter19/CnC_Generals_Zero_Hour)** —
  the original Unix/64-bit port: SDL3 platform management, C++17
  filesystem/threading, Freetype/Fontconfig text rendering, and the DXVK approach
  this renderer path descends from
- **[fbraz3/GeneralsX](https://github.com/fbraz3/GeneralsX)** — the macOS/Linux port,
  integrating and extending the above
- **[ammaarreshi/Generals-Mac-iOS-iPad](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad)** —
  the macOS / iOS / iPadOS port this repository was forked from (arm64-ios cross-build,
  DXVK on iOS, touch controls, app lifecycle, packaging)
- **[GeneralsOnline Development Team](https://github.com/GeneralsOnlineDevelopmentTeam)** —
  [GeneralsOnline](https://www.playgenerals.online), the online service and PC client
  whose client this port brings to Android, and whose community data patch and
  maps the launcher downloads
- **This fork** — the Android port (the GeneralsOnline client on Android and
  cross-play with PC, touch controls, in-app launcher, language packs, device
  bring-up), plus engine fixes throughout, offered upstream
- **DXVK, SDL, OpenAL Soft, FFmpeg, GameNetworkingSockets, Liberation Fonts** — the load-bearing walls

Not part of that chain: **[tarek369/GeneralsZH-Android](https://github.com/tarek369/GeneralsZH-Android)**
is a separate, independent Android port — this one is not built on it. Problems its users
report are checked against this port too, and fixed here when they turn out to exist here as
well; a couple of bugs (a duplicate-symbol build break, an INI-parsing gap) were traced faster
thanks to its published engineering log.

Engine code **GPL v3** (EA's source release → the chain above → this fork). Game
assets: not included, not licensed here.
