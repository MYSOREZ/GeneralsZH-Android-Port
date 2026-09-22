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

This port had that rule wrong for weeks, from a frame-matching scheme written to
avoid guessing whether the recording's first checksum survived — a guess that was
never needed, because the recording says so itself. Fixed 20/09/2026.

**But fixing it changed nothing, and that matters.** The same replay still reported
`ours=EEE6D2B8 recorded=2B2071D1` at frame 100: the frame-matching rule it replaced
happened to select the same queue entry on this recording. So the off-by-one was a
real defect in the rule and **not** the cause of the symptoms. The divergence is
genuine — do not re-explain it as a pairing artefact.

**Alignment is now provable from the log, so prove it instead of assuming.** Every
comparison prints the frame the recorded value arrived on, the frame ours was
queued at, and how many of ours remain queued:

```
replay crc for frame 100: ours=... recorded=... (recorded arrived on frame 101, ours queued at 101, 0 still queued)
```

Two things follow. "Ours queued at 101" means the drop happened — without it the
first entry would be the frame-0 one, queued at 1. And the recorded value arriving
at 101, with no comparison at frame 1, means the recording holds no CRC message at
frame 1: the PC's frame-0 checksum was never transmitted, so dropping ours is
right. Like compared with like.

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

**The game data. Compute it, do not ask for hashes.** `exe_crc` on Android is
`CRC(version) + CRC(SkirmishScripts.scb) + CRC(MultiplayerScripts.scb)` — the
executable is not hashed on this platform. Recomputing that from the data
repository with the engine's own algorithm (`crc.cpp`: `crc = ROL(crc,1) + byte`)
gives **4265514697**, exactly what the device reports before `gx_pc_compat.txt`
overrides it. So the `.scb` files are byte-identical to the repository's, and the
root Zero Hour set is the one in use (the `ZH_Generals` base-game set would give
3727096529). `ini_crc` matches the PC GeneralsOnline value **genuinely**, with no
override, so the INI data is identical too — including `Multiplayer.ini`, since
`TheMultiplayerSettings` is initialised at `GameEngine.cpp:645`, inside the
`xferCRC.open(510)..close(815)` window and passed `&xferCRC`.

**Every behaviour switch.** Preprocess both trees' `GameDefines.h` with the same
defines and diff the resulting macro *values* rather than reading them: 21 shared
`RETAIL_COMPATIBLE_*` / `PRESERVE_*` macros, all identical. The two we define that
the client does not — `RETAIL_COMPATIBLE_NETWORKING` and
`RETAIL_COMPATIBLE_PATHFINDING`, both `(0)` — are undefined there, which evaluates
the same in every `#if`. `RETAIL_COMPATIBLE_PATHFINDING` being `(1)` here *was* a
real bug once (it changed the A* start cell) and is fixed; do not re-open it.

**What gets hashed.** All 370 `crc()` functions in the simulation tree are
byte-identical to the client's after comment and whitespace normalisation, bar
`AI::crc` (our trace counters; the xfer stream is unchanged) and
`ReplaceObjectUpgrade::crc` (whitespace). A difference in *what* is hashed is
invisible to every behavioural audit, so it was worth checking once. Clean.

**Object destruction.** `destroyObject`, `processDestroyList`, `registerObject` and
`removeObjectFromLookupTable` differ from the client only in `nullptr`-versus-`NULL`
and spacing. Worth checking because destruction is the one thing the failing map
does that the thousand-frame map does not.

**The frame order.** `GameLogic::update` is behaviourally identical; only formatting
and two divide-by-zero guards differ.

**The fog of war, code and state.** Every function in `PartitionManager.cpp` matches
after normalisation. `MAX_PLAYER_COUNT` is 16 and `ShroudLevel` is two `Short`s on
both sides, so the stream has the same shape: 4356 cells of 18 words.
`RETAIL_COMPATIBLE_CIRCLE_FILL_ALGORITHM` is `(1)` on both. The shroud and
map-reveal script actions are identical, and `doNamedMapReveal`,
`doRevealMapAtWaypointPermanent` and `getPlayerFromAsciiString` differ only in
`nullptr`/`NULL`. The measured state is sane too — see below.

**The player list and the replay observer.** The observer is added unconditionally
by the same code in both trees, so the recording machine had it. During playback it
is the *local* player, which is expected. The client reads slots from a local
`game` where we read the global `TheGameInfo`; that is an alias — the client
assigns `TheGameInfo = game = ...` in every branch, including
`TheRecorder->getGameInfo()` for playback, and our replay path sets it the same way.

## Claims made here and then disproved

**"Twelve supply piles on frame 0 where the map holds six, so a script runs
twice."** The map holds twelve. Its own scripts do all of it, on frame 0:
`Setup Scripts` creates six, `Supply Spawn` creates six more at the same
coordinates, `Supply Remove` destroys the first six. The PC client runs the same
three scripts. Measured with the creation/destruction attribution trace, which
exists because of this question.

**"The shell map's scripts leak into the game and spawn 44 phantom objects."**
They do not. The shell map behind the main menu is itself a running game with
its own frame 0, 1, 2, so a trace keyed on the frame number alone emits the shell
map's opening frames and the replay's opening frames under the same label. I
segmented one log's "frame 0" into blocks and read three different games as one.
Scoped to the replay's own run, its frame 0 evaluates 869 scripts: the map's 852
plus 4, plus 86 from `SkirmishScripts.scb` / `MultiplayerScripts.scb` that the
engine loads by design, and **zero** from the shell map. The traces now print
`mode <GameMode>` alongside the frame for exactly this reason. **Any
frame-numbered trace in this engine must say which game the frame belongs to.**


Keep this list. Each was asserted with apparent evidence and then killed by a
measurement, and each is the sort of thing that gets re-derived.

**"The pairing is off by one, and that explains it."** The rule was wrong and is
fixed. It explained nothing — the numbers did not move.

**"A candidate whose implied word is identical at every checkpoint must be the real
one."** Plausible: a spurious candidate should move as both checksums move. Three
were stable across all eleven checkpoints. Tested on a synthetic stream with one
known difference and an evolving region *before* reporting it: **9000 positions**
showed an identical implied value across all eleven, every one ahead of the part
that changed. Stability locates "before the changing region", nothing more.

**"Arithmetic is excluded — zero float-rounding candidates out of 85538, twice."**
Zero does not mean that. The locator tests **one word at a time**, and a rounding
difference inside a transform moves up to twelve words at once (a `Matrix3D` is
twelve words here). A multi-word rounding difference gives exactly the observed
result: no single word reconciles the two, and no float candidate. Platform
arithmetic is **not** excluded for a multi-word field.

**"`RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION` differs."** It does not; the client
defines `(0)` under `GENERALS_ONLINE`, which is defined.

**"`if (game)` versus `if (TheGameInfo)` is a real difference."** An alias.

## The locator: what it can and cannot do

`Common/GXCrcStream.{h,cpp}`. The checksum takes one 32-bit word at a time and the
step is invertible, so the recording's single number can be walked backwards
through our words:

```
forward:     C[i] = ROL(C[i-1], 1) + W[i-1]      (mod 2^32)
inverse:     C[i-1] = ROR(C[i] - W[i-1], 1)
implied[i] = D[i+1] - ROL(C[i-1], 1)
```

`implied[i]` is the one word that would reconcile our stream with theirs if the
difference were at `i` alone. At the true position it **is** their word; elsewhere
it is arbitrary. Candidates are filtered by what a real difference looks like: a few
bits apart (a flag), a small integer apart (a counter or id), zero on one side, or a
rounded float.

**Verified before use** on synthetic streams of twenty thousand words in the
simulation's value ranges: a single one-ULP difference leaves about twenty-five
candidates and the true one ranks first or third, across five positions and deltas.

**Its limit, and it is hard: one word.** It cannot localise a difference spanning
several words and says so (`no single word reconciles the two`). A `Coord3D` is
three words, a transform twelve, a differing object count changes the length
outright.

**Two of its filters were wrong and are fixed.** The zero test was
`ours == 0 || theirs == 0`, and the stream is full of zero words, so wherever ours
was zero *any* implied value passed — the category claimed 7665 of 85538 positions.
Now `theirs == 0 && ours != 0`. And the section printer took each section's end from
the next mark, which is an object's, so the objects section reported four words
instead of six thousand.

**A single difference shows up many times over, 32 positions apart.** The implied
delta at position `i` is exactly `D[i+1] - C[i+1]`, and going backwards that
difference is rotated right one bit per word. Thirty-two words later the rotation
has come full circle, so wherever the words in between are static the *same* delta
reappears. Measured on the objects dump: positions 5650, 5682, 5714 and 5746 all
carry delta `5B030000` — the same `+859` after the byte swap — and the locator duly
printed four separate "small integer apart" candidates for what is one hypothesis.
**Count families, not candidates.** (The rotation is not exact: `delta[i] ==
ror1(delta[i+1])` held at 2212 of 6402 positions, the rest being the carry term,
which is why the walk still depends on the words.)

**Therefore a small delta at a section boundary says nothing about where the
difference is.** The walk rotates a difference rather than shrinking it, so "their
walked-back value and ours agree in the top 18 bits at the end of the objects
section" is just as consistent with one small word difference anywhere in the 79136
words after it. This was briefly believed and is wrong; it is the reason the dump is
now the whole stream rather than one section.

**The filter that is actually strong: the same implied word at every checkpoint.**
At the true position the implied delta is frame-independent — both machines agree up
to it, so the delta *is* the word difference, whatever frame it is measured on. At
every other position the implied value is built from that frame's words and moves.
Eleven checkpoints of one replay, 10-12 candidates each, intersected on
(position, our word, implied word):

```
3 positions survive all eleven
  word 5992  object id=11 AmericaInfantryBiohazardTech +8   1980.27917 -> -982.014587
  word 6188  object id=6  GuardTower +2                    -0.499999821 -> -0.241668612
  word 6368  object id=1  GuardTower +2                    -0.819152057 -> 1.29759892e-05
```

Offsets `+1..+12` of an object are its transform and `+2` and `+8` are elements of
it, which cannot differ on their own: change one element of a rotation and the other
five change with it. **So no single static word difference explains this mismatch.**
The intersection costs one `grep` over a log that already exists — do it before any
multi-word search.

**With the whole stream in hand, a windowed hypothesis costs its own width, not
the stream's.** If a difference is confined to `[a, b)`, then everything after `b`
is shared, so their value at `b` is already known from the inverse walk, and the
test is: start at our value at `a`, apply the candidate words, compare with
`back[b]`. Twelve steps for a transform, not eighty-five thousand -- and it needs
no assumption about the rest of the stream. That is what made these exhaustive:

```
every object's 12 transform words, each element +/-1..3 ULP   4 783 637 tests, 0 hits
every plausibly-perturbable pair within 32 words, +/-1..4   171 692 437 tests, 0 hits
```

The same identity makes a deletion an O(1) test rather than a search: if the
streams agree from `j` on, their value at `j` must equal ours at `p`.

**Past that limit, move the search off the phone.** On the first mismatch the engine
dumps one named section's words in hex, its start and end running values, and the
inverse walk of the recording's checksum back to the section's end. That last number
makes the rest computable: with it and our words, every intermediate value on *both*
machines inside the section can be reconstructed offline and any multi-word
hypothesis tested without another build. For a twelve-word transform with each
element within one rounding step, fixing eleven determines the twelfth — 3^11 per
object, 172 objects, minutes of search.

## What an object looks like in the stream

Worked out from the dump and verified against all 166 objects, because every offline
hypothesis needs it:

```
mark +0        one word, zero on everything seen
mark +1..+12   Matrix3D, row-major 3x4: m00 m01 m02 x | m10 m11 m12 y | m20 m21 m22 z
mark +13       object id          (checked: matches the traced id for all 166)
then           module state, variable length -- 36 words for scenery, 56-87 for units
```

Words are stored byte-swapped (`htobe`), so `0000803F` is `1.0f` and `0020EE44` is
`1905.0f`. Strings go in as raw characters: the section markers the client writes
(`"MARKER:Objects"`, `"MARKER:ThePartitionManager"`) are the first words of each
section, which is a free check that a dump is aligned, and a weapon template name
appears in the middle of an object as plain ASCII.

`TheModuleFactory` is xferred only under `DEBUG_CRC` and contributes nothing, which
the arithmetic confirms: the partition section's 78415 words are exactly 7 marker
words plus 4356 cells x 18.

## Where the checksum actually lives

Measure this before theorising. On the failing map, frame 100:

```
Objects                words 0..6402      (6402, for 172 objects)
logic random seed      word  6402
ThePartitionManager    words 6403..84818  (78415)
ThePlayerList          words 84818..84854 (36)
TheAI                  words 84854..85538 (684)
```

**92% of the lockstep checksum is fog of war.** `PartitionCell::crc` hashes
`m_shroudLevel` per player plus the cell's grid coordinates, and those coordinates
are constants every machine computes alike — so a difference in that section can
only be a shroud level.

The shroud reports itself now, three counts per player per checkpoint. On the
failing map it is sane, which is how that lead was closed:

```
frame 0    p1 clear=991  shrouded=3365 | p8 clear=232  fogged=4124 | p9 clear=4356
frame 100  p1 clear=1028 shrouded=3328 | p8 clear=1988 fogged=2368 | p9 clear=4356
```

Slot 9 is the replay observer, permanently revealed, entirely clear. Slot 8 is the
playing human: `shrouded=0` because `revealMapForPlayer` ran for its occupied slot,
which also proves the slot loop executed and `TheGameInfo` was set; its clear count
grows 232 → 1988 between frames 0 and 100, which is the four radius-450 map reveals
firing at frame 2. Slot 1 is the civilians — vision but no whole-map reveal, correct
for a map side rather than an occupied slot. Slots 0 and 2..7 print nothing, being
fully shrouded.

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

## The shroud, decoded

Needed for any hypothesis about the 78408 words that are fog of war. Per cell,
18 words: 16 shroud entries indexed by player, then two coordinate words (those
two are **not** byte-swapped, unlike everything else -- read them raw).

| value | meaning |
|---|---|
| > 0 | shrouded: never seen |
| 0 | fogged: seen once, nobody looking now |
| < 0 | clear: -n means n things are looking |

Validated against the independent shroud counters in the same log: reading the
words gives player 8 exactly 2368 zeros and 1988 negatives, and the counters say
`fogged=2368 clear=1988`. Player 9, the replay observer, is -1 in all 4356 cells.

## The replay header, and what it settles

`.rep` files carry their `GameInfo` as readable text near the start. One `strings`
or `head` is enough, and it closes several arguments at once:

```
M=...casino 2v2 - resurrection four v3;  MC=4518BD2D;  MS=132597;
SD=976636386;  C=100;  SR=0;  SC=20000;  O=N;
S=HMYSOREZ,0,0,TT,-1,2,-1,0,1:X:X:X:X:X:X:X:;
```

Slot fields after the name are ip, port, two flags, then colour, playerTemplate,
startPos, team, NAT (`GameInfo.cpp` around the `case 'H'` parse). So: **one human,
seven closed slots, no AI, colour and startPos both unassigned (-1), 20000
starting cash, seed 976636386, CRC interval 100.** The recording had exactly one
participant, which retires "the PC hashed other players' buildings and fog"
without a build.

**The seed is verifiable offline, and it verifies.** `seedRandom` and the draw
ladder in `RandomValue.cpp` are twenty lines of portable integer arithmetic, and
`GetGameLogicRandomSeedCRC` is a byte-wise CRC over the six-word state. Port both
to a script, seed with the replay's `SD`, and step:

```
seedRandom(976636386) then 109 draws -> CRC DA7DB690
the log's own frame-0 value          -> DA7DB690      and its RNG tally says draws=109
```

So the replay's seed reaches the logic RNG intact, the generator is bit-exact
against the PC's, and no hidden draw happens before frame 0. `startPos = -1` is
resolved by the block in `GameLogic.cpp` around lines 900-1140, which is identical
to the client's modulo whitespace -- and in this game it takes the branch that
draws nothing, which the tally confirms (no `GameLogic.cpp` call site appears).

**Also verified rather than assumed: my own instrumentation is not in the
checksum.** `AI::crc` is the one `crc()` function that differs from the client's,
and the diff is trace lines and counters only -- no `xfer` call added, removed or
reordered.

## Where it stands (22/09/2026)

**The single-number method is now exhausted, and that is a measurement, not a
mood.** The whole 85538-word stream for the first diverging checkpoint is dumped
and reparses exactly (our forward walk reproduces `ourAtEnd` to the bit). Every
hypothesis a single checksum per checkpoint can decide has been decided:

| hypothesis | how it was tested | result |
|---|---|---|
| one static word anywhere | implied word must be frame-independent; intersect 11 checkpoints | 3 survivors, all single elements of a rotation matrix — impossible alone |
| a transform rounded differently | all 12 elements, +/-1..3 ULP, arithmetically-derived elements only | 0 hits in 1.37M combinations |
| any 3-word field | every run of 3 in the objects section, +/-1, +/-2, +/-4 | 0 hits |
| the same field off by a constant in every object | 36 offsets x deltas +/-1..8 | 0 hits |
| object ids shifted by a creation we do and they do not | every threshold x -12..+12 | 0 hits |
| **we carry words they do not** — any contiguous run, any position, any length | our value at p must equal their walked-back value at j; hash join over all 85538 positions | **1 candidate, chance level (0.85 expected), and it spans 71067 words from inside a rock to inside the fog — meaningless** |

That last row is the important one, and it is exhaustive rather than sampled: a
deletion hypothesis is an O(1) test, not a search, because if the streams agree
from `j` onwards then their value at `j` must equal ours at `p`. **So the streams
are not our stream minus anything.** Either they are the same length and differ in
at least two non-adjacent words, or they are longer — and a difference where they
carry words we do not is the one case this method cannot compute, because the
content of those words is exactly what we do not have.

**So stop asking the checksum and go get more input.** In order of value:

1. ~~**The `.rep` file itself.**~~ Read — see the section above. The recorded
   game had one participant, the seed reaches our RNG intact, and the setup path
   matches. It did not find the bug, but it removed the largest remaining unknown
   and it cost no build.
1. **Full dumps at three checkpoints instead of one.** A single dump cannot judge
   a two-word hypothesis: one equation, two unknowns, so a solution exists almost
   everywhere. An exact sweep over the whole stream (window tests, see below)
   returned 23 such "hits" where chance predicts 0.05 -- and every one of them
   needed a structurally-zero word to become `0xFFFFFFFC` or a byte inside a
   weapon's name to change. With the plausibility filter on, zero. Three dumps let
   the frame-independence filter apply to multi-word hypotheses, which is the same
   filter that took twelve single-word candidates to three. Shipped; `GX_CRC_DUMPS`
   sets the count.
2. A `DEBUG_CRC` build of the PC client, for `CRCGEN_LOG` or `XferDeepCRC`. Worth
   more than any tool on this side, and currently out of reach.
3. Nothing else. More instrumentation on this side measures the same 32 bits again.
