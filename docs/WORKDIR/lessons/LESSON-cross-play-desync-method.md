# Cross-play desync: start here, not from the top

If you have been handed "Android and the PC client desync in GeneralsOnline",
read this before forming a hypothesis. Most of the obvious theories have been
tested and killed with measurements, and the instrument that actually finds
these is already built.

## The one fact that reframes everything

**The PC client's full source is on the dev machine**, at
`/home/user/generalsonlinedevelopmentteam/gameclient`. It has the same
`GeneralsMD/` and `Core/` layout, so files correspond path for path.

This is not a numerical-analysis problem. It is a diff. Every divergence found
so far was found by diffing our simulation against that tree, not by reasoning
about floating point.

## Before you trust a single number: check the pairing rule

**Read this section first. It invalidated months of readings.**

A replay's checksums have to be paired with the locally computed ones, and the
pairing is not "same frame". `RecorderClass::CRCInfo` in `Recorder.cpp` is a
queue, and the PC client empties it with two rules that are easy to lose:

```cpp
m_skippedOne = !isMultiplayer;                         // constructor
if (!m_skippedOne) { m_skippedOne = TRUE; return; }    // addCRC drops the first
```

In a network game the first `MSG_LOGIC_CRC` never reaches the wire, so a
multiplayer recording's checksum stream starts one interval later than the local
stream during playback, and the client discards this device's first local
checksum to compensate. `isMultiplayer` comes from the mode stored in the
recording's own header:

```cpp
Bool isMultiplayer = (m_originalGameMode == GAME_INTERNET || m_originalGameMode == GAME_LAN);
```

which is read *after* `difficulty`, `m_originalGameMode`, `rankPoints` and
`maxFPS`, so `CRCInfo` must be constructed after those reads, not before.

Get this wrong by one interval and the log lies in a way that looks exactly like
a real bug:

- a map whose state changes between checkpoints reports a mismatch at the very
  first comparison, always at frame 100;
- a map where nothing moves reports ten consecutive perfect matches, because
  consecutive checkpoints hold the same value.

This port had both of those for weeks, from a frame-matching scheme written to
avoid guessing whether the recording's first checksum survived — a guess that was
never needed, because the recording says so itself. Fixed 20/09/2026. If you are
reading an older log, or any log whose header line does not print
`originalGameMode=` and `isMultiplayer=`, treat its verdicts as unmeasured.

## A map is not a neutral test harness

Before concluding "this map desyncs", read the map's scripts. They are easy to
read: a `.map` is an `EAR\0` header, a RefPack stream, then the `CkMp` chunk tree
whose layout is written out plainly in `Common/System/DataChunk.cpp`
(`DataChunkTableOfContents::read`, `openDataChunk`, `readDict`) and
`ScriptEngine/Scripts.cpp` (`Script::ParseScript`, `ScriptAction::ParseAction`,
`Parameter::ReadParameter`). The chunk-name table at the front of the file is
already a summary: it lists every script action the map uses.

The `Casino - Resurrection` pack, which is where this port's desync reports came
from, holds a group called `CounterRandom` with two non-one-shot scripts that
each call `SET_RANDOM_TIMER` on both branches. They run every logic frame, so the
map draws from the shared logic RNG twice per frame from frame 0. That is what
`ScriptEngine.cpp:6769 drew 200` means in a frame-100 tally: two draws times a
hundred frames, not an anomaly.

A map like that amplifies any divergence into the checksum immediately. A still
map hides one for a thousand frames. So "it desyncs on this map and not that one"
is usually a statement about how hard the map leans on the RNG, not about the
map needing its own fix. Check the script tally before promising a per-map hunt.

## The reference is the replay, not a Windows build

A `.rep` recorded on the PC **carries the x86 checksums inside it**. Playing it
back on Android compares them frame by frame against ours:

```
[GX-NET] replay crc for frame 100: ours=C979BA3B recorded=C979BA3B
[GX-NET] replay crc for frame 200: ours=4979BA3B recorded=4979BA3B
```

So you do not need a Windows machine, a Windows build, or an x86 reference
build of any kind. A MinGW build was brought up for exactly this and abandoned;
do not rebuild it. Ask for a PC recording instead.

Enable the traces with a `gx_net_trace.txt` marker file in the game data folder
(the Setup app has a switch), or `GX_NET_TRACE=1`.

## The instrument, and how to aim it

Four traces, all in `GameLogic::getCRC` and `RandomValue.cpp`:

| trace | what it narrows |
|---|---|
| `crc parts frame N` | which of four stages: objects / partition / player list / AI |
| `crc obj frame N` | **which objects**, with position and orientation |
| `crc rng frame N` | which call sites drew from the logic RNG, and how many times |
| `replay crc for frame N` | ours vs the PC's, per checkpoint |

`crc obj` is the sharp one. Diff the per-object lines between two checkpoints:
the objects whose own checksum moved are the only ones that can carry the
divergence. On an idle map that is 11 objects out of 317. It took the first
investigation from "somewhere in the simulation" to "civilian traffic" in one
step.

Its frame window is `GX_TRACE_OBJ_FRAMES`, default 500. Raise it if the first
diverging checkpoint is later than that; the cap used to be 100 and hid exactly
the window that mattered.

**Checkpoints are every 100 frames** and the interval is baked into the
recording, so that is the finest granularity the data offers. Compensate by
asking for replays that isolate one mechanic rather than by trying to sample
more often.

## Ask for the right replays

A replay is a probe, not a patient. The fix is always global; the map only
decides which code runs. Do not audit maps one at a time — ask for recordings
that isolate one subsystem:

- you alone, no building, no bots — movement and ambient traffic
- one bot, same faction as you, that only builds — construction
- one bot that only gathers — supply
- you alone, shooting — weapons and damage

A replay that matches proves only what it exercised. A thousand matching frames
on an idle map say nothing about combat.

The long-term goal is a corpus of these run automatically. Factorio reached
ARM/x86 cross-play by comparing the state checksum of every tick of 2417 tests
between the two, not by reasoning about arithmetic.

## What is already ruled out — do not re-derive these

**The architecture.** aarch64 and x86_64 produce bit-identical results for the
engine's transcendental set, in single and double precision, and for the
pathfinder's `10*sqrt(dx²+dy²)` narrowed to int. `-ffp-contract=off` is on all
1476 Android translation units and no `-ffast-math` anywhere. "ARM computes
differently" is false here.

**`_PC_24`.** The client is 32-bit x87 and `setFPMode()` clamps the mantissa to
24 bits, so its `double` arithmetic carries only float precision. Real, but
nearly irrelevant: the code on the movement path declares **no doubles at all**
(`Locomotor.cpp` and `PhysicsUpdate.cpp` have zero). It only bites where a
float meets a `double` literal, and `PI` is declared `3.14159265359f`.

**Deterministic math / fdlibm.** `SAGE_USE_DETERMINISTIC_MATH` is OFF and its
`wwmath.h` wrappers are `#ifdef` stubs with identical arms. Do not switch it
on: **the PC client does not use GameMath or fdlibm** (verified by searching
its tree), so routing our side through them would replace a divergence measured
at zero with an unmeasured one.

**`(float)atan2((double)y,(double)x)`.** Already bit-exact with the client's
x87 at PC=24. Do not "fix" it to `atan2f` — that would *introduce* ~15%
disagreement on unit facing.

**Audited clean against the client:** `ScriptEngine.cpp`, `GameLogic.cpp`,
`Locomotor.cpp`, `PhysicsUpdate.cpp`, `AIUpdate.cpp`, `AIStates.cpp`,
`AIPathfind.cpp`, `TerrainLogic.cpp`, `DozerAIUpdate.cpp`, `WorkerAIUpdate.cpp`,
`AIGroup.cpp`, `AIDock.cpp`, `Object.cpp`, `ScriptActions.cpp`, `AIPlayer.cpp`,
`AISkirmishPlayer.cpp`, `BuildAssistant.cpp`, `StructureBody.cpp`,
`ActiveBody.cpp`, `PartitionManager.cpp`, `GameCommon.cpp`. Most differences in
these are `static_cast<float>(LOGICFRAMES_PER_SECOND)` (no-ops: the operand is
already float), null guards before `deleteInstance`, `nullptr`, and 64-bit
pointer casts.

## What was actually wrong, and the shape of it

**The libm behind the source, not the source.** `Thing::setOrientation` builds
the transform that `Object::crc` hashes, via the engine's own `Cos`/`Sin` from
`Lib/trig.h` (`GameEngine/Source/Common/System/Trig.cpp`) — **not** through
`WWMath`. Those called `sinf`/`cosf`, character-identical to the client. But
the client is VC6 for 32-bit x86, whose CRT has no real single-precision
transcendentals: `sinf` promotes to double and evaluates on x87. bionic has
real ones, with their own error.

Measured against the client's contract — x87 with the control word
`setFPMode()` installs — over 62801 angles:

```
x87 fsin  vs  sinf                      776 differ  (1.236%)
x87 fcos  vs  cosf                      768 differ  (1.223%)
x87 fsin  vs  (float)sin((double)x)       0 differ
x87 fcos  vs  (float)cos((double)x)       0 differ
```

One angle in eighty, on a function that rebuilds the rotation of every moving
object every frame. Fixed by evaluating in double and narrowing once, off MSVC
— the pattern the same files already use for `Acos`, `Asin` and `Sqrt`.

**Generalise this.** When our source and the client's are identical and the
results differ, suspect the library, not the arithmetic. The rule for anything
on the hashed path: match VC6's CRT, which means evaluate in double and narrow
once, never a single-precision libm entry point.

**The other shape**: a `#if RETAIL_COMPATIBLE_CRC / #else / #endif` strip that
ate a function call and its braces (`HelixContain::removeFromContain`), and
`#if` arms taken differently because the macro is 0 on both sides. Grep for
`RETAIL_COMPATIBLE_*` when a file looks structurally odd.

## Traps that cost time here

- **Diffing comment-stripped streams gives useless line numbers.** Diff the
  files as they are and filter the diff output instead.
- **A test can be a tautology.** Verifying "round-to-24 emulation matches x87"
  in a build that is *already* x87 proves nothing. Make the two sides actually
  different.
- **`WWMath::Sin` is not the engine's `Sin`.** The simulation uses
  `Lib/trig.h`. Changing `wwmath.h` changed 42 call sites and moved no
  checksum at all.
- **Confirm which binary produced the log.** The `[build compiled ...]` stamp
  can be stale (ccache replays cached objects). The crash log now carries the
  ELF build id and CI run number; check them before drawing conclusions.
- **Confirm the tick rate.** `[GX-BUILD] sim tick: 60 Hz, client id
  gen_online_60hz` must be in the log. A 30 Hz engine cannot match a 60 Hz
  recording, for reasons that have nothing to do with the bug you are chasing.

## Where it stands

Closed: ambient traffic and general movement. An idle-map PC replay matches for
1000 frames, ten consecutive checkpoints.

Open: a dozer driving. With two AI opponents, frame 100 matches and frame 200
does not, and in that window **exactly two objects of 324 change — both
`AmericaVehicleDozer`, moving**. Civilian vehicles reach identical coordinates
in both replays, so it is not general movement. The obvious suspects on that
path are all audited clean, so the next step is data: a replay of one bot doing
nothing but drive, and one doing nothing but build.
