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
against the PC's, and no hidden draw happens before frame 0. `startPos = -1` and
`colour = -1` are resolved by the block in `GameLogic.cpp` around lines 830-1140,
identical to the client's modulo whitespace; it draws one integer for the colour
(`GameLogic.cpp:843`) and one for the start position (`:1036`), both inside the 109.

*Correction:* an earlier version of this paragraph said the block draws nothing
"which the tally confirms". The tally lists eleven call sites and I had printed
the first seven. Read the whole list before citing it.

**Also verified rather than assumed: my own instrumentation is not in the
checksum.** `AI::crc` is the one `crc()` function that differs from the client's,
and the diff is trace lines and counters only -- no `xfer` call added, removed or
reordered.

## Three checkpoints prove where the difference is not

Dumping the whole stream at frames 100, 200 and 300 of the same replay gave the
strongest result so far, and it came from an observation about **our** side first:
between those frames exactly **one** word of our 85538 changes -- word 6402, the
logic RNG state. Everything else is static: the dozer has arrived, nothing moves.

That makes a proof available. Walk each of the PC's three checkpoint values back
through our words, stopping at the end of the objects section. The walk crosses
79135 suffix words (fog of war, player list, AI) plus a *different* RNG word on
every frame. If anything in that suffix, or the RNG, differed from the PC, the
three walks would land on three unrelated values. They land on the same one:

```
frame 100: their value at end of Objects = 3091A52B   ours = 3091963C
frame 200: their value at end of Objects = 3091A52B   ours = 3091963C
frame 300: their value at end of Objects = 3091A52B   ours = 3091963C
walked-back values over the whole Objects section identical on all three: True
```

So, as a measurement rather than an inference:

- **the fog of war, the player list and the AI are identical to the PC's** -- 92%
  of the checksum, cleared;
- **the RNG state matches the PC's on all three frames**, including the two draws
  per frame the map's `CounterRandom` group makes; our own RNG word is reached from
  the replay's seed after exactly 310, 510 and 710 draws;
- **the difference is entirely inside the 6402-word objects section**, and that
  section does not change between frames 100 and 300.

The same numbers show up another way: the word the PC would need at 6402 is ours
plus the constant `0x1DDE` on all three frames, which is what a difference before
6402 looks like when everything after it agrees. Note that "the top 18 bits agree"
argument earlier in this file reached this same conclusion from bad reasoning; the
conclusion has now been earned.

**A blind spot this exposed.** Before this, the one hypothesis nothing had tested
was "the PC made a different number of RNG draws". Both filters were blind to it
by construction: the frame-independence filter demands the same implied word on
every checkpoint, and an RNG word legitimately changes every frame; the
plausibility classifier looks for fields that differ by a few units, and an RNG
state CRC is an arbitrary 32-bit value. Tested directly now -- the word the PC
would need is not a reachable RNG state within 20 million draws of its seed -- and
closed. **When a filter rejects a candidate, check that it could have accepted the
right answer.**

## Inside a static section, more checkpoints add nothing

The objects section is identical on all three frames, so all three give the same
single equation for it: forward from 0 over their 6402 words must reach
`3091A52B`. One 32-bit constraint cannot localise a difference of two or more
words, and more checkpoints of a *static* section do not add constraints. What was
tested against that one equation, exactly (window tests, no assumptions):

| hypothesis | tests | hits |
|---|---|---|
| any integer-valued word in any object, any delta up to +/-100000 | part of 3.3G | aliasing only (below) |
| two integer words of one object moved by the same delta (two stamps of one late event), up to +/-3600 frames | part of 3.3G | aliasing only |
| two integer words of one object, independent deltas +/-64 | part of 3.3G | aliasing only |
| three integer words of one object, same delta up to +/-600 | part of 3.3G | aliasing only |

376 "hits" against 0.78 expected, every one of them in the last ~35 objects of the
section and none in the first 130 -- including none in the dozer at word 256. That
is the period-32 aliasing again: near the end of the section the walked-back
difference is small and nearly unrotated, so a small change almost anywhere nearby
"explains" it. A hit deep in the section would mean something; there are none.

**The end difference being small is weak evidence, measured.** Their and our
end-of-objects values differ by 3823 -- twelve bits. Perturbing one random word
deep in the section lands within 3823 in 0.1% of trials; perturbing one of the last
22 words, in 0.6%. A factor of six, not a localisation.

**Checked and closed on the way:** `Object::crc` feeds
`m_objectUpgradesCompleted` into the checksum as raw memory of size
`sizeof(BitFlags<512>)`, which is exactly the kind of line that differs between a
32-bit MSVC and a 64-bit libc++ build. It does not here: `std::bitset<512>` is
64 bytes, sixteen words, on both.

## Two recordings of one map: two equations

A second PC recording on the same map (seed 1816235999; the dozer drives and
builds) behaves the same way: the three-checkpoint proof holds again, their
end-of-objects value is `70C9ACB0` against our `70C99DFD`, and the difference is
again inside the objects section. At frame 100 the two recordings' objects
sections differ in exactly **two** words -- the dozer's X and Y -- so any static
explanation has to satisfy both recordings at once. That is the second equation
the static section could not give on its own.

**What two equations excluded, exactly:**

| hypothesis | result |
|---|---|
| the dozer's transform alone: X, Y, Z within +/-200 ULP; another pathfind cell +/-60 with Z free within +/-4096 ULP; a facing angle up to +/-0.2 rad | 370M tests, **0 hits in either recording** |
| any static single word, required delta equal in both recordings | only in the last ~900 words |
| any static pair within 256 words, deltas +/-4096 | **none in words 0..5500**; tens of thousands after |

The dozer row matters twice. It retires the terrain-height hypothesis properly --
and it exposed that my earlier transform sweep never tested it: X = 1905, Y = 1845
and Z = 30 are exact integers, and the "only perturb arithmetically-derived
elements" filter excluded them. **The plausibility filter excluded the one
hypothesis an outside reviewer asked about.**

**Where two recordings stop helping.** A static change produces the same end
difference in both recordings only when it sits in the last few hundred words;
anywhere earlier, carries through the high bit make the propagation depend on the
running values, which differ after the dozer's X and Y. Measured on random
single-word changes: equal end differences in 0/200 trials for words 0..6000,
35/200 for 6000..6300, 162/200 for 6300..6402. So the two-recording filter is sharp
over the first ~6000 words and blunt over the tail -- which is why the pair hits
pile up there. (An earlier reading of "the end differences are 3823 and 3763, so
the cause must be the dozer" was wrong for the same reason.)

**The candidate.** Restricting to one natural family -- the same field of every
object of one template shifted by the same amount -- left exactly two hypotheses
out of 406016, both on the two `AmericaCheckpoint` objects and both reproducing
**both** recordings to the bit:

```
AmericaCheckpoint +11 (m22)  field -1   -> 1.0 becomes 0.99999994   physically natural
AmericaCheckpoint +27        field -2   -> a zero mask word becomes 0xFFFFFFFE   the alias
```

Both checkpoints are quarter-turned (m00 = `(float)cos(PI/2)` = -4.37e-08), and a
one-ULP difference in a computed rotation element is the textbook x87-versus-ARM
result. But the alias fits too, and other quarter-turned objects exist
(`SecretResearchLab` x2, `ToxicSupplyTruck` x2) without needing the same patch, so
this is a candidate, not a finding.

**How it gets decided, with no new recording.** The second recording creates a
building at frame ~400, and from then on the objects section differs -- every
running value through the checkpoints changes. A real explanation keeps matching;
an alias stops. So the engine now applies each hypothesis to its own captured
words at every mismatching checkpoint and prints
`crc hyp frame N: <hypothesis> ... -> MATCHES THE PC | no`. Replay the same
recording, read seventeen post-build verdicts. It also dumps the whole stream
once more whenever the object list changes, so the offline tools get a
different-list checkpoint too.

## Found: WWMath::Inv_Sqrt is an x87 approximation on the PC

**The cause of the Casino desync, proven from both ends.** `wwmath.h` has a
`#if defined(_MSC_VER) && defined(_M_IX86)` branch for `Inv_Sqrt`: a magic-constant
first guess (`0xBE6EB508`) refined by three Newton-Raphson steps in inline x87
assembly. The PC client is 32-bit MSVC, so it takes that branch. Every other build
took the `#else`: an exact `1.0f / sqrt()`. Same source line, different number.

It reaches the checksum through `Vector3::Normalize()` and
`Normalized_Cross_Product()`: the terrain normal in
`BaseHeightMapRenderObjClass::getHeightMapHeight`, and every direction the
locomotor, physics, dozer AI, missiles and production exits normalise.

**How it was found, which is the reusable part:**

1. Three full dumps of a static stretch proved the difference was in the objects
   section and nowhere else (see "Three checkpoints prove...").
2. A second recording of the same map gave a second equation, and the only
   natural family of multi-object difference that fit both was "both
   `AmericaCheckpoint` objects carry m22 = `0x3F7FFFFF` on the PC where we have
   `0x3F800000`".
3. A stock map where the player stands still matched the PC at every checkpoint.
   It had no `AmericaCheckpoint`. The dozer's own start-of-game snap to a cell
   centre happened there too, and matched -- so movement was not it.
4. The data said why only those two objects: `AmericaCheckpoint` is the one
   quarter-turned object on the map with `KindOf = ... STICK_TO_TERRAIN_SLOPE`, so
   `Thing::setOrientation` builds its matrix with `alignOnTerrain`, whose third
   column **is** the terrain normal. `SecretResearchLab` and `ToxicSupplyTruck`,
   also quarter-turned, lack the flag -- which is why "every quarter-turned object"
   had been rejected.
5. Flat ground gives the cross product `(0, 0, 1024)`. Exact normalisation gives
   `1.0`. The x86 routine, emulated step by step under the client's
   `setFPMode()` (`_PC_24`, `_RC_NEAR`), gives `0.99999994` = `0x3F7FFFFF`: the
   number the checksum demanded.

**Verified before building:**

- the portable replacement against real x87 arithmetic on this machine (x87 with
  the control word set as `setFPMode()` sets it): 6 918 440 inputs from 2^-40 to
  2^40, **0 differences**; the checkpoint case gives `3F7FFFFF` on both;
- a full emulation of `makeAlignToNormalMatrix` for both checkpoints: the old code
  reproduces our dump exactly, including a `-0`; the new code reproduces exactly
  the matrix the PC's checksum requires, with only m22 changed.

**The same branch hid three more functions**, all fixed together:

| function | PC (`_M_IX86`) | was here | now |
|---|---|---|---|
| `Inv_Sqrt` | magic constant + 3 Newton steps, x87 at `_PC_24` | exact `1/sqrt` | the same algorithm in float, bit-exact |
| `Float_To_Long` | `fld`/`fistp` under `_RC_NEAR`: rounds | C cast: truncates (2.7 -> 2) | `lrintf` / `lrint` |
| `Sin`, `Cos` | `fsin`/`fcos`, full internal precision, rounded once | `sinf`/`cosf` | `(float)sin((double)x)`; 20M angles, 0 differ from `fsin`/`fcos` (`sinf` differs on 1.3%) |
| `Sqrt` | `fsqrt` | `sqrt` | unchanged: both correctly rounded |

`BaseType.h`'s `fast_float2long_round` and friends were checked too: their asm is
guarded by `_MSC_VER < 1300` (VC6), so the modern-MSVC PC client uses the same
`lroundf` we do.

**Confirmed on the device.** With the `wwmath.h` fix, the idle Casino recording
(`7.rep`) matches the PC at all eleven checkpoints, and the driving one (`8.rep`)
matches at 100, 200 and 300 -- it diverged at 100 before. It still diverges from
400, which is when its dozer starts to drive and turn.

**The second cause is the same shape: float CRT transcendentals.** The reference
is 32-bit MSVC built `/arch:SSE2`, so ordinary float and double arithmetic is IEEE
SSE on both sides; only explicit x87 assembly differs. But 32-bit MSVC's CRT has
**no float variants** of the transcendentals: `sinf(x)` there is an inline
`(float)sin((double)x)`. bionic's `sinf` is its own single-precision algorithm,
different in the last bit on about 1.2% of angles. `matrix3d.h`, `matrix3.h` and
`vector3.h` call `sinf`/`cosf` directly in every rotation helper (20 sites), and
those run every frame for a moving unit: `PhysicsUpdate`'s `Rotate_X/Y/Z`, the
locomotor's `In_Place_Pre_Rotate_Z`. At ~1% per call a turning vehicle is
expected to leave the PC within a few dozen frames -- exactly the window between
300 and 400. Fixed by routing them through `WWMath::Sin/Cos` (double, rounded once),
plus `TanTrig` and two calls in `DynamicShroudClearingRangeUpdate`. Not verified
before building the way the first fix was: there is no single frozen value to
compare, because the effect compounds along the dozer's path. The replay decided:
**it changed nothing.** Our checksums at 400, 500 and 600 came out identical to the
previous build's, bit for bit, so those rotation helpers either did not run in that
window or bionic happened to agree on every call. The change is still correct and
stays, but it was not the cause. **An identical checksum after a fix is a
measurement, and a cheap one: always compare the new build's numbers with the old
build's before reading the PC comparison.**

**The real second cause: C++ overloads hide the float calls.** The first sweep
grepped for `sinf(`. But in C++ a plain `atan2(dy, dx)` with `Real` arguments
resolves to the float overload, and the compiler emits `atan2f` -- checked on the
NDK's clang: `atan2(float, float)` compiles to `b atan2f`, `sin(float)` to
`b sinf`, and only `(float)atan2((double)y,(double)x)` calls the double `atan2`.
None of those call sites say `atan2f`. There are 59 in `GameLogic` and `Common`
alone, eight of them in `Locomotor`, which steers every moving unit every frame.
The replay's own commands (parsed from `8.rep`) put the dozer's build order at
frame 342, so it drove for 58 frames before the first mismatch.

Rather than edit every site and hope none is hidden behind an overload or a
template, `GeneralsMD/Code/Main/ReferenceFloatMath.cpp` gives the binary its own
`sinf`, `cosf`, `tanf`, `asinf`, `acosf`, `atanf`, `atan2f`, `sinhf`, `coshf`,
`tanhf`, `expf`, `logf`, `log10f` and `powf` -- each "double, then round once",
which is what 32-bit MSVC's CRT does -- with hidden visibility, so every call inside
the binary binds to them at link time. Compiled with `-fno-builtin` so the compiler
cannot turn a body back into a call to itself; the object file shows `sinf`
calling `sin` and `atan2f` calling `atan2`. `sqrtf` and `fmodf` are left to the
platform: both are correctly rounded everywhere. Check after building:
`readelf --dyn-syms libmain.so` must no longer import those names from `LIBC`.

Searched and clean for this class: every float transcendental reachable from
logic is now in `Trig.cpp` (fixed earlier) or goes through `WWMath`; the remaining
hits are diagnostics (`SimulationMathCrc`), Intel-compiler-only code (`vp.cpp`) and
network latency maths (`NetworkMesh.cpp`). Every other `__asm` in Core is dead code
(`#if 0`, VC6-only, or `__ICL`-only).

**The general lesson.** "Identical source" is not identical code when the
reference is 32-bit MSVC. Two things run differently there: every
`#if defined(_MSC_VER) && defined(_M_IX86)` branch, and every float CRT
transcendental, which that CRT silently implements as the double function rounded
to float. Grep for both in anything the simulation calls before hunting elsewhere.

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

**The next input has to be a different replay, not a different instrument.** The
objects section is static from frame 100 on, so this replay has given everything
it can: one equation. Two short recordings on the same map, made on the PC, split
the remaining space in half each:

1. **Solo, no commands at all**, run for a minute. If it matches, every object's
   initial state is right and the difference comes from something that moved --
   in this replay, only the dozer, 56 words at 256..312. If it diverges, movement
   is irrelevant and the difference is in the initial state the map and its 852
   scripts produce.
2. **Solo, one short dozer move at the start, nothing else.** Pairs with (1): the
   difference between the two is exactly what one move leaves behind.

Anything that changes the objects section *between* checkpoints turns more
checkpoints back into more equations, which is what a multi-word hypothesis needs.
A `DEBUG_CRC` build of the PC client would still beat all of this, and is still
out of reach.

## After both fixes: 7 and 8 match, 1.rep breaks at a cancelled building (22/09/2026)

With `Inv_Sqrt` and `ReferenceFloatMath` in, `7.rep` (idle) and `8.rep` (dozer
driving and building) match the PC at every checkpoint. `1.rep` -- Tournament B,
the user against three easy AIs -- matches for 19 checkpoints and diverges at
2000. The replay's own commands (`repparse.py 1.rep 1850 2120`) say what is new in
that window: at 1908 the user cancels a barracks under construction
(`MSG_DOZER_CANCEL_CONSTRUCT`), at 1954 the dozer drives into the site. The RNG
tally shows what that set off -- `ObjectCreationList.cpp:1355/1159-1161/1177-1179`
39 times each (39 `GenericDebris` thrown out with random spin and force), then
`SlowDeathBehavior.cpp:258/512` once per piece as each one comes to rest and dies
(`KillWhenRestingOnGround`). Frame 1900's tally had zero draws; nothing like this
happened in the 1900 frames that matched.

What is established:

- `onDozerCancelConstruct`, `ObjectCreationList`, `PhysicsUpdate`,
  `SlowDeathBehavior`, `LifetimeUpdate`, `DestroyDie`, `CreateObjectDie` and
  `Object` differ from the client only in whitespace, logging, or code that is
  equivalent.
- **It is not rounding noise in one object.** `deb3.c` perturbed every object's
  transform in the 2000 dump: any 3 of the 12 matrix words by +/-8 ULP, and the
  position by +/-64 ULP per axis -- 1.2 billion window tests, 0 hits. `sweep2.c`
  (any two words within 32, +/-4) also 0. So either several objects differ, or the
  object set differs, or the RNG was drawn a different number of times -- and a
  different draw count moves the seed word, which leaves the objects section
  unconstrained. One equation per checkpoint cannot go further here.
- **Uninitialised members are not the explanation, though one exists.**
  `PhysicsBehavior::m_originalAllowBounce` is never initialised, in the client
  too, and `handleBounce` reads it. It is zero on both sides: the PC's pool
  allocator zeroes blocks, and Android builds with `RTS_GAMEMEMORY_ENABLE=OFF`,
  whose `GameMemoryNull.cpp` `operator new` zeroes as well. Check that it is really
  the one in use: `libmain.so` neither imports nor exports `_Znwm`, and the
  `GameMemoryNull.cpp.o` in the build defines it, so every allocation in the binary
  binds to the zeroing one.
- **The static LOD reaches logic too, harmlessly.** `GameLogic.cpp` turns "fluff"
  map objects into client-only props when the static LOD is below High -- and
  Android defaults to Low -- but a multiplayer game or replay forces
  `forceFluffToProp = TRUE` regardless.

**Found on the way: the simulation reads the frame rate.** `isDebrisSkipped()`
(the debris OCL) and `getSlowDeathScale()` (every slow death, at its start and on
each update) come from the *dynamic* LOD tier, and `W3DDisplay::draw` resets that
tier from the measured average FPS every frame. The GeneralsOnline client does the
same, plus `updateGraphicsQualityState`. So on either machine, rendering below the
VeryHigh tier's `MinimumFPS` changes how many debris objects exist and how long
dying things stay. That is a desync between any two peers whose frame rates fall on
different sides of a threshold -- and phones are the peers that fall. Fixed in
`GameLOD.h`: `isLogicLODPinned()` is true in a multiplayer game or any replay, and
then both functions read the VeryHigh tier (what a PC rendering at full speed uses);
particles and shadows still follow the frame rate. **This is a real fix but not
shown to be this replay's cause:** the device played `1.rep` at 57-60 fps, so it was
almost certainly on VeryHigh already (the tier thresholds live in `GameLOD.ini`, which
is not in the tree -- the new trace line prints the tier and settles it), and then the
pin changes nothing there. What it would explain is the
PC having dipped during the recording, and that the next step can test.

New trace line, printed on every tier change:
`lod frame N: dynamic LOD High -> VeryHigh (debrisSkipMask=0 slowDeathScale=1.00) -- logic stays on VeryHigh (lockstep game or replay)`.

**Ask for these next:**

1. **Play `1.rep` on the PC itself.** If the PC also reports a mismatch at 2000, the
   recording cannot be reproduced even by the machine that made it, and the cause
   is on the recording side (a frame-rate-dependent tier during the live game). If
   the PC replays it cleanly, the difference is ours.
2. **A quiet recording of the same event:** Casino (where `7.rep` matches), solo,
   place one structure with the dozer, cancel it, wait a minute. That isolates
   debris and slow death from three AIs. If it matches, the debris path is clean
   and the cause lies with the AIs; if it diverges, it is the debris path.

**Cancelling a building is destroying it.** `onDozerCancelConstruct`
(`GameLogicDispatch.cpp`) refunds the cost and calls `building->kill()`, and
`Object::kill` (`Object.cpp`) is an ordinary `attemptDamage` for the full max health
with `m_kill = TRUE`. From there it is the same death any destroyed structure goes
through: the die modules, the debris OCL, `SlowDeathBehavior`, debris physics until
each piece rests and dies. So:

- The frame-rate LOD pin covers every destroyed building and dying unit, not only
  cancellations. Wherever a fight's desync came from FPS-dependent debris or slow
  deaths, this build fixes it.
- Whatever else broke `1.rep` sits on the same path, and would show up just the same
  when a building is destroyed in combat. A cancellation is simply the cheapest way
  to trigger it on purpose, so a "place and cancel" recording is the right test for
  destruction in general.
- The difference that remains is the death type. A cancellation dies with
  `DAMAGE_UNRESISTABLE` / `DEATH_NORMAL` and no source object. Combat supplies its
  own damage and death types, and those choose which die modules and OCLs run. If
  the cancel recording matches, also ask for one with a building destroyed by
  weapons.

**Measured the next day: the LOD pin is a no-op with this data, and the theory is
dead.** The trace line printed the tiers as the game walked through them at start-up:
`Low`, `Medium`, `High` and `VeryHigh` all carry `debrisSkipMask=0
slowDeathScale=1.00`. `GameLOD.ini` is part of the checksummed INI set and the INI
CRC matches the PC's (`81FB5632`), so the PC has the same table. **With that data
the frame rate cannot change the simulation on either machine.** Our checksum at
2000 came out `6AD123D1` again, identical to the build before, as it had to. The pin
stays because it costs nothing and protects against a mod whose `GameLOD.ini` does
skip debris. It explains nothing here. Lesson: the per-tier values were
the first thing to print, one line, before reasoning about thresholds.

Also checked and clean this round:

- **Where the client defines its switches.** In the client,
  `GENERALS_ONLINE_HIGH_FPS_SERVER` and `GENERALS_ONLINE_COMMUNITY_PATCH_CHANGES`
  come from `NextGenMP_defines.h`, not CMake. A header define only reaches the
  files that include it, so this had to be checked. It turns out to be harmless:
  `Core/GameEngine/Include/Common/GameCommon.h` includes that header right after
  `GameDefines.h`, and every simulation file includes `GameCommon.h` through
  `PreRTS.h`. So the 60 Hz arms are live across the client's simulation, as ours
  are. The only `.cpp` use of `COMMUNITY_PATCH_CHANGES` is the community INI BIG in
  `ArchiveFileSystem::loadMods`, which this port loads too (hence the matching INI
  CRC).
- **`GENERALS_ONLINE` in the client.** `add_compile_definitions` in
  `GeneralsMD/Code/GameEngine/CMakeLists.txt` also reaches the Core simulation
  sources, because the client compiles them inside `z_gameengine`
  (`corei_gameengine_private` is an INTERFACE library). So its
  `RETAIL_COMPATIBLE_*` resolve to 0 there, the same as ours. The Core targets that
  miss the define (`Lib/BaseType.h`, `W3D*`) use the switches only in rendering or
  save-game code.
- The 60 Hz macro usage counts, file by file, match the client everywhere except
  networking, UI and rendering files.

Nothing on the phone side is left that one checkpoint's checksum can decide. The two
PC-side inputs above are now the only way forward.

### Compiler differences on the death path, checked (23/09/2026)

Once the libm functions agree, two IEEE machines doing the same float operations
agree. `-ffp-contract=off` is set here, and the client's `/arch:SSE2 /fp:precise` does
no contraction. So the remaining candidates are the places where **MSVC and clang
are allowed to compile the same source differently**:

| candidate | how it was checked | result |
|---|---|---|
| order of evaluating arguments / operands with two logic RNG draws in one statement (MSVC tends right-to-left, clang left-to-right) | every statement, across line breaks, in `GameEngine` sources with two `GameLogicRandomValue*` calls | none |
| uninitialised locals (stack garbage differs per compiler) | every simulation TU recompiled from `compile_commands.json` with `-Wuninitialized -Wsometimes-uninitialized -Wconditional-uninitialized` (442 files) | 14 hits, none on the debris/death/physics path; the "closest distance" loops are safe (first iteration assigns) |
| `WWMath` headers vs the client | full diff | only the intentional `Inv_Sqrt`/`Sin`/`Cos`/`Tan`/`Float_To_Long` fixes, plus `Inverse_Lerp` guarding `a == b` (no simulation caller) |

**Port changes that moved enum numbering.** Both are real, and neither has a CRC path
here:

- `DAMAGE_FLESHY_SNIPER` is compiled into Zero Hour here, while the client keeps it
  under `#if RTS_GENERALS`. So every damage type from `DAMAGE_SUBDUAL_MISSILE` on is
  one higher here (client 31, here 32). `KINDOF_AIRFIELD` plus
  `KINDOF_RESERVED_SPARE_1` shift the `KindOf` bits the same way. Behaviour goes
  through names parsed from INI and is unaffected. The object checksum carries no
  damage type or `KindOf` mask. Revisit if a checksum ever differs in a word holding
  one.
- `ThingTemplate.cpp`'s legacy shim, which is dead in the client's Zero Hour build,
  runs here. Every `DRONE` gets `NO_SELECT`, and `AIRFIELD` implies `FS_AIRFIELD`.
  `NO_SELECT` is only read by `CommandXlat` (what a click turns into). A replay
  replays recorded commands, so it cannot desync one. In a live game it makes the
  phone's player issue different commands than a PC player would for the same click,
  which is a behaviour difference, not a desync.

**Next, as with the dozer:** a PC recording that isolates the event. On Casino, solo
(where `7.rep` and `8.rep` match): place one structure with the dozer, cancel it at
once, then do nothing for a minute. A second recording with the structure sold
instead of cancelled splits "the death itself" from "the debris it throws".

## Found: the compiler merged sin/cos pairs into bionic's sincosf (23/09/2026)

`ReferenceFloatMath.cpp` gives the binary its own `sinf`, `cosf` and the rest. But the
check that should have followed it was never run: **list what `libmain.so` still
imports from libm.**

```
llvm-nm -D --undefined-only libmain.so | grep -E 'sin|cos|tan|exp|log|pow'
  ... sin@LIBC  cos@LIBC  sincos@LIBC  sincosf@LIBC  exp2f@LIBC  log2f@LIBC ...
```

`sincosf` appears nowhere in the source. When clang sees `sin(a)` and `cos(a)` of the
same float argument, it merges them into a single `sincosf` call. That call goes to the
platform libm, whose single-precision routine is exactly what `ReferenceFloatMath`
exists to avoid. Finding the objects that ask for it takes one loop over the build tree
(`llvm-nm --undefined-only` on every `.o`). Four are simulation code:

| object | code | what it decides |
|---|---|---|
| `Geometry.cpp` | `GeometryInfo::get2DBounds`, box case | which partition cells a box-shaped object (building, construction site, many vehicles) occupies |
| `BuildAssistant.cpp` | `(Real)cos(angle)` / `(Real)sin(angle)` twice | whether a structure may be placed there, where its factory exit is |
| `AISkirmishPlayer.cpp` | base-defence placement, twice | where the skirmish AI puts buildings |
| `AIGroup.cpp` | formation offset | where a group's members are sent |

`1.rep` is the first recording with an AI building a base, a structure placed and then
removed, and objects entering and leaving the partition around a building footprint.
All of it runs through these calls.

Fix: `ReferenceFloatMath.cpp` now also defines `sincosf` (double, then round once, like
`sinf`/`cosf`) and `sincos` (plain `sin` + `cos`). Every merge the compiler makes, now or
in future, lands on the reference semantics. **After every build, the import list is
the check:** no float transcendental other than the ones that are correctly rounded
everywhere may be imported from `LIBC`. `exp2f`/`log2f` remain, from `d3dx8_compat.cpp`
and `mapper.cpp` (rendering, not simulation).

The general lesson extends the previous one. Wrapping the functions the source calls is
not enough. The compiler emits libm calls of its own (`sincosf`, and on other targets
`__sincosf_stret`, `exp10f`), so verify against the binary's imports, not the source.

### Instrument: who calls libm, and where a different libm could matter

Fixing `sincosf` left `1.rep`'s checksum at 2000 bit-for-bit unchanged (`6AD123D1`), so
nothing on that path produced a different bit in this window. Rather than audit more
source, the binary now reports what it actually calls.
`ReferenceFloatMath.cpp` defines the double libm entry points (`sin`, `cos`, `tan`,
`asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `exp`, `log`, `log10`, `pow`) as
hidden forwarders to the platform's own (`dlsym(RTLD_NEXT)`). Every call from the binary
passes through them: plain double calls, the double inside our `sinf`, and merged
`sincos`. `GameLogic::update` passes the frame number in. For a window of logic frames
(default 1900..1999; a file `gx_math_trace.txt` with "FROM TO" moves it), each call on the
logic thread is counted against its three innermost return addresses. A call is flagged
**fragile** when the exact double result lies within 4 double ULPs of a float rounding
midpoint. Only then can the PC's CRT, which is also within a fraction of an ULP, round the
float the other way. At the end of the window:

```
[GX-NET] math trace frames 1900..1999: N libm calls on the logic thread, S call sites, F fragile ...
[GX-NET] math site cos   calls=... fragile=... callers=libmain+0x... < 0x... < 0x...
```

Addresses are file offsets in `libmain60.so` (or `libmain.so` for the 30 Hz engine). Build
with `GX_KEEP_SYMBOLS_DIR=<dir> ./scripts/build/android/build-dual-hz.sh` to keep a copy
with the symbol table (`libmain60.sym.so`, same `.text` as the shipped library, checked).
Then `llvm-symbolizer --obj=libmain60.sym.so 0x...` names each site.

Reading it: **zero fragile calls in the window acquits libm for this divergence**, and
the hunt moves to non-maths state. A non-zero count names the exact sites to compare
against the client.

**Measured on `1.rep` (23/09/2026): libm is acquitted.** Frames 1900..1999 made 52,824
libm calls on the logic thread from 123 call sites: `Locomotor`, `PhysicsBehavior`
(`update`, `setAngles`, `handleBounce`), `Thing::setTransformMatrix`, the collision
tests (`collideTest_Box_Box`, `xy_collideTest_Rect_Rect`/`Circle_Rect`),
`PartitionManager::findPositionAround`, `Pathfinder::classifyObjectFootprint`, the
debris OCL, plus client-side drawing and audio on the same thread. **None was
fragile.** No result came within 4 double ULPs of a float rounding midpoint, so no
libm that is accurate to within an ULP, the PC's included, can round any of those floats
differently. The checksum at 2000 was `6AD123D1` again.

Also cleared this round, by normalised diff against the client (whitespace,
`NULL`/`nullptr`, brace style and trace lines removed): all of `GameLogic` and `Common`,
the device-side terrain height path (`W3DTerrainLogic` → `BaseHeightMap`; the
`m_useHalfHeightMap` terrain LOD only affects tree panels), the legacy-frame (30 Hz)
logic, and the contact list (hashed by object ID). Local-player checks in the
simulation only drive UI: EVA sounds, marking the control bar dirty, and the
retaliation-mode message, which is itself recorded.

With both the arithmetic and the code measured equal, the one input not yet measured
is the recording itself: **whether the PC reproduces its own recording.** A replay
records the checksums of a live game, where frame pacing, input timing and the local
player differ from playback. One PC playback of `1.rep` answers it.

### Compiler assumptions: strict aliasing, signed overflow, null checks (23/09/2026)

With libm measured innocent and the source identical, the remaining difference
between "the same source on both machines" is what each **compiler assumes about
undefined behaviour**. MSVC performs no type-based alias analysis, lets signed integers
wrap in practice, and keeps a null check that follows a dereference. clang at `-O2`
assumes the opposite on all three. This engine reads floats through
`*(unsigned *)&f` (`BaseType.h`: `fast_float_floor`/`fast_float_trunc` behind
`REAL_TO_INT_FLOOR`/`CEIL`, used by terrain lookups and partition cells), hashes with
`Int` arithmetic, and tests pointers after using them.

Measured first: every simulation file was compiled twice, with and without
`-fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks`, and the disassembly
compared with addresses and symbol names removed. **276 of 441 files produced
different machine code**, so clang does act on those assumptions here. Different code
is not necessarily different behaviour. The flags are now global for non-MSVC builds
(`cmake/compilers.cmake`), which makes the port's semantics the reference compiler's.
`1.rep` decides whether any of it ran in the diverging window: if the checksum at 2000
changes, it did.

**Measured: compiler assumptions acquitted too.** With
`-fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks` the checksum at 2000 was
still `6AD123D1`. None of the 276 changed files changed behaviour in the window. The
flags stay: they are the reference compiler's semantics.

### Instrument: floating-point exception flags per logic frame

What remains is where x86 and ARM differ **with identical instructions**:

- denormals, if either machine runs with flush-to-zero. Drivers and audio mixers set
  it; debris spin rates decay geometrically towards zero every frame and cross into
  the denormal range;
- converting NaN or an out-of-range float to an integer: 0x80000000 on x86 SSE,
  saturated or 0 on ARM64.

The CRC dump cannot show these. The spin rates are not hashed, and the object words
that looked denormal turned out to be IDs and frame numbers. So `GameLogic::update` now
reads `fetestexcept` at the end of every logic frame. `setFPMode()` clears the flags at
the start, so the reading covers exactly one frame of simulation. It prints

```
[GX-NET] fp flags frame N: underflow(denormal) invalid(NaN) divbyzero(inf) overflow(inf)
[GX-NET] fp flags frames A..B: underflow in U frames, NaN in V, div-by-zero in W, overflow in X
```

The math trace saves and restores the flags around its own bookkeeping, so it does not
pollute the reading. A frame in the diverging window that raises `invalid` or
`underflow` names the class; if nothing is raised in 1900..1999 while earlier windows
are equally clean, this class is out too.

**Measured on `1.rep`:** no denormal in any frame of the replay, so the flush-to-zero
theory is out. `invalid` is raised often: in 16 frames of 1900..1999, and also in
windows that match the PC (35 frames of 400..499, 40 of 900..999, and more). `invalid`
means a NaN, an ordered comparison with a NaN, or a float converted to an integer out of
range. The last is where x86 and ARM produce **different integers from the same
instruction**. That it also happens in matching windows does not clear it: the result
may be unused there and used here.

Next instrument: `GameLogic::update` checks the flag after every phase (scripts,
terrain, commands, AI, build assistant, partition, destroy list, stores, disabled
status) and after **every module update**, then names the module and the object and
clears the flag:

```
[GX-NET] fp invalid frame N: update PhysicsBehavior on GenericDebris id=351
[GX-NET] fp invalid frame N: AI - on - id=0 (first of this kind)
```

Every occurrence in 1880..2010 is printed, and elsewhere the first of each
(phase, module, template) kind.

## Found: NaN converted to an integer when debris comes to rest (23/09/2026)

The attribution trace answered it. In `1.rep`, `invalid` came from **`PhysicsBehavior`
on `GenericDebris`, exactly once per piece**, from frame 1951 on: the frame each piece
came to rest and `KillWhenRestingOnGround` killed it. That matches the user's
observation that the mismatch appears when the debris lands, not when the building
breaks. Every other module appeared once in the whole game, in windows that match.

The chain: `obj->kill()` → `SlowDeathBehavior::onDie` sums
`getProbabilityModifier()` over the applicable slow-death modules, then calls
`GameLogicRandomValue(1, total)`. `getProbabilityModifier` does

```cpp
Int  overkillDamage   = dealt - clipped;                    // 0
Real overkillPercent  = (float)overkillDamage / maxHealth;  // debris: 0/0 = NaN
Int  overkillModifier = overkillPercent * bonus;            // NaN -> Int
return max(m_probabilityModifier + overkillModifier, 1);
```

- **PC (32-bit MSVC, SSE2 `CVTTSS2SI`):** NaN becomes 0x80000000, the sum is negative
  and clamps to 1, and `GameLogicRandomValue(1, 1)` returns at `lo >= hi` **without
  drawing**.
- **ARM64 (`FCVTZS`):** NaN becomes 0, the sum stays at `m_probabilityModifier`, and
  the call **draws one logic random value**.

Every resting piece of debris drew one extra value on the phone. From the first one the
seeds diverged, and everything random afterwards differed. The tally had shown it all
along (`SlowDeathBehavior.cpp:512 drew 18`), but a draw count of our own proves nothing
without the PC's to compare.

Fix: `realToIntTruncRef()` in `Lib/BaseType.h` truncates with the reference's
semantics: in-range values truncate, NaN or out-of-range give 0x80000000. Both
conversions in `getProbabilityModifier` use it.

**The general lesson:** any float-to-integer conversion that can see a NaN or an
out-of-range value is platform-dependent, even with identical arithmetic before it. It
is invisible to libm checks and compiler flags, and appears in no CRC word. The way to
find one is the `invalid` flag, attributed per module. The other modules that raised it
once in this replay (`WorkerAIUpdate`, `DozerAIUpdate`, `AIUpdateInterface` on several
units, `SupplyTruckAIUpdate`, `DynamicShroudClearingRangeUpdate`) did not change the
checksum here. They are the next candidates if a later replay diverges during combat
or economy.

**Confirmed on the device (23/09/2026):** with `realToIntTruncRef` in
`getProbabilityModifier`, `1.rep` matches the PC through the debris window and up to
**4300**. The NaN-to-int conversion was the cause. The next divergence is at **4400**.
The replay's commands in that window: at 4109 and 4170 the user queues units at their
`AmericaSupplyCenter` (id 381, i.e. Chinooks), and at 4188 sets its rally point to
(1413.22, 729.77). New objects around then are Chinooks (390, 391, 398), a supply truck,
Rangers and war factories.

`invalid` is raised in 36 frames of 4300..4399. The attribution trace printed only the
first of each kind outside 1880..2010, so it cannot say which kind is new there. It now
also prints **per 100-frame window, every (phase, module, template) kind with its
count**:

```
[GX-NET] fp invalid frames 4300..4399: update ChinookAIUpdate AmericaVehicleChinook x36
```

Comparing the diverging window's list with the matching windows before it names the
new kind.

**4400: not a NaN.** The per-window table shows no new kind in 4300..4399. The only
kinds raising `invalid` there are `AIUpdateInterface` on `AmericaInfantryRanger` and
`WorkerAIUpdate` on `GLAInfantryWorker`, and both did the same in the matching windows
3600..4299. The user watched it happen: the mismatch comes when a Chinook has loaded
its supply boxes and turns back to base, not while it is loading. The supply code
(`SupplyTruckAIUpdate`, `ChinookAIUpdate`, the dock updates, `Money`) is identical to
the client's, and the deposit arithmetic is `UnsignedInt`. What is new in that window
is **helicopter flight** (the hover/thrust locomotor: `acos`, `atan2`, `tan`), and the
math trace had only covered 1900..1999, when no helicopter existed.

The math trace now runs **for the whole game**. Every libm call on the logic thread is
checked for fragility, and the fragile ones are kept with frame, argument and callers.
Every 100 frames:

```
[GX-NET] math window frames 4300..4399: N libm calls on the logic thread, F fragile
[GX-NET] math fragile frame 4371: acos(0.99999...) = ... callers=libmain+0x... < ...
```

The detailed per-site table for `gx_math_trace.txt`'s window is still printed as before.

**Measured: libm is clean for the whole of `1.rep`.** Every 100-frame window, 0..5199,
had zero fragile calls, including 4300..4399 with the Chinooks flying. Helicopter
flight maths is not the cause.

That leaves the kinds that raise `invalid` in the diverging window: `AIUpdateInterface`
on `AmericaInfantryRanger` (36 frames) and `WorkerAIUpdate` on `GLAInfantryWorker`. That
they also raised it in matching windows does not clear them; the debris NaN also only
mattered once `total` was used. One conversion is a particular suspect: `REAL_TO_INT`
goes through `lroundf`. For NaN or infinity, 32-bit MSVC's 32-bit `long` gives
0x80000000, while bionic's `long` is 64 bits, so after truncation to `Int` it gives
**0**.

`AIUpdateInterface::update` now reports the flag separately after the state machine
(`AI state <id that ran>`), after the path/turret bookkeeping (`AI misc`) and after
`doLocomotor` (`AI locomotor`), through `gxFpCheckpoint()` in `GameLogic.cpp`. The
per-window kind table therefore names the AI state or the locomotor.

**`1.rep` at 4400, measured further:** inside `AIUpdateInterface::update`, the Rangers
raise `invalid` in the **locomotor** and the GLA workers in **AI state 14**, in the
diverging window and in the matching ones alike. The flag cannot say whether that is a
harmless ordered comparison with a NaN (same `false` on both machines) or a NaN
converted to an integer (different integers).

### `USA.rep`: a second, early divergence (23/09/2026)

A new recording, solo on Tournament B, diverges already at **frame 100**. It was made
by a **newer PC client** (`build='Sep 23 2026 00:07:44'`, exe CRC `FBA615FA`, against
`Aug 28` / `B9DB8815` for `1.rep`), but that is not the cause. The client's `main`
(fetched: `1f9491c`, 22/09) differs from the 12/09 snapshot only in lobby, score screen,
list box and stats code. The repository history was squashed on 12/09
("networking.cmake / optimized target"), and the optimized preset is
`/O2 /Ob3 /Oi /Ot /GL /LTCG /arch:SSE2` with no `/fp` change, so the float semantics are
the same.

What was ruled out for frames 0..99 of `USA.rep`:

- no `invalid`, no denormal, zero fragile libm calls;
- **not an extra or missing logic random draw.** The RNG was rebuilt from the replay
  header's `SD=1018613559` (`rngsim.py`: `seedRandom`, `randomValue`, the byte-wise seed
  CRC). It reproduces our seed CRCs exactly (80 draws at frame 0, 81 at frame 100).
  Substituting the seed after any of 0..400 draws does not produce the PC's checksum
  with everything else equal;
- no single static word, frame-consistent across the dumps at 100/200/300/600 (words
  aligned by object id, and the tail from the end);
- not the pathfinder snap of the 10 civilian cars and the dozer to cell centres (the
  same objects snap identically in `1.rep`, which matches at 100). Moving any of them
  by multiples of half a cell, or leaving it unsnapped, does not reconcile;
- not the sign of zero in the car matrices (155 `-0.0` words, flipped all together or
  by matrix offset).

The only player action before 100 is selecting the dozer at 61 (plus retaliation mode,
which `1.rep` also has). Selection creates a temporary `AIGroup` that is freed before
the checksum, identical to the client.

### Instrument: every NaN or out-of-range float-to-integer conversion, by file and line

`-fsanitize=float-cast-overflow -fsanitize-recover=float-cast-overflow` on the engine's
own targets (`core_config` in `cmake/config-build.cmake`; 1410 instrumented conversions
in `libmain60.so`). The handler in `ReferenceFloatMath.cpp` prints each site once:

```
[GX-NET] float->int out of range frame N (logic): Locomotor.cpp:1234:17 float nan -> int
[GX-NET] float->int window frames 4300..4399: Locomotor.cpp:1234 x36
```

That separates a harmful NaN conversion, where x86 gives 0x80000000 and ARM gives
0/saturation, from the harmless comparisons the `invalid` flag also reports. The
handler is reachable only from `libmain`; third-party libraries are not instrumented.

**`USA.rep`, measured with the float-cast detector:** in frames 0..99 the only NaN or
out-of-range float-to-int conversions are client-side (`W3DParticleSys.cpp:236`, a
colour to `unsigned char`, and `GadgetVerticalSlider.cpp:387/388` in the UI). Nothing
in the simulation. That class is out for this divergence too.

More hypotheses for `USA.rep` at frame 100, each tested against the PC checksum over the
full dump and rejected:

- **Shroud layout.** Each partition cell is 16 player words plus X, Y. Our replay
  observer is player 3 (`FFFF0000` in every cell); players 4..15 hold the default
  `01000000`. Moving the observer's column to any index 4..15, duplicating it, or
  removing it: no match at 100 or 200.
- **Player count.** `ThePlayerList` is the count followed by 3 words per player
  (battle-plan bool, skill points, science purchase points). Counts 2..9, with any one
  player dropped or default players added: no match at 100, 200 or 300.
- **The dozer's start position, jointly with the RNG.** The starting dozer appears at
  different places in the two recordings (`1.rep` (1408.82, 739.92), `USA.rep`
  (1427.03, 784.97), with the same command centre), so its placement is computed. Every
  position within ±300 on a 2.5 grid, combined with the seed after 0..400 draws
  (required state computed backwards from the PC checksum through the known words):
  no match.

What is known about the first 100 frames of `USA.rep`: 11 objects change (the dozer and
10 civilian cars snap to pathfind cells, identically to `1.rep`), 81 logic draws in
total, and the one player action is the dozer selection at 61. A targeted recording is
the fastest way on from here, as with `7.rep`/`8.rep`: the same solo start with **no
input for the first 200 frames**. If it matches at 100 and 200, the selection (or what
the live client does around it) is the trigger. If it does not, the difference is in
the solo start itself, and one AI added would say whether empty slots matter.

## One log, five replays: which ones match, and what the client update did (23/09/2026)

To tell runs apart in a log that holds several replays, use the
`crc players frame 100` line (the player list). The replay header does not help, since
every Tournament B recording has the same one. Results: `3.rep` (solo) and `4.rep`
(1 AI, **8500 frames**) match to the end; `5.rep` (2 hard AIs) diverges at 3700;
`1.rep` diverges at 4400; `USA.rep` diverges at 100.

**`USA.rep` goes with the client version, not with the map or the solo start.** `3.rep`
has the same map and the same solo start from the 28 August client, and it matches.
`USA.rep` is the 23 September client. What is known about that update:

- The client history was squashed on 12/09 into a commit titled "optimized target". It
  adds `win32-vcpkg-optimized` (`/O2 /Ob3 /Oi /Ot /Gy /Gw /GL /arch:SSE2`, `/LTCG`,
  optional PGO) with `RTS_MEMORYPOOL_DEBUG` off. The code changed between 28/08 and 12/09
  is invisible.
- Everything after 12/09 (`1f9491c`) is lobby, score screen, list box, stats, and a
  `GameInfo` destructor that clears `TheGameInfo`. None of it runs in the simulation.
- **Memory-pool debug is not it.** The init filler (`s_initFillerValue`) exists only when
  `MEMORYPOOL_DEBUG` is defined, and `GameMemory.h` defines that only under `RTS_DEBUG`.
  Release builds never had it, on either side. Pool blocks and global `new` are zeroed
  anyway; only `malloc` is not.
- An uninitialised-variable sweep (`-Wuninitialized -Wsometimes-uninitialized
  -Wconditional-uninitialized` over all 428 simulation files) found 14 warnings. Each is
  a false positive or sits on a path not taken here: `Weapon::onWeaponBonusChange` is
  guarded by `needUpdate`, and the rest are closest-distance loops and bezier or
  stealth code.

The fastest next step is one recording from the **new** client: solo, Tournament B,
**no input at all** for ~15 seconds. If it diverges at 100, the build itself computes
differently and the difference is in the solo start. If it matches, the dozer selection
at frame 61 is the trigger.

**`5.rep` at 3700: an AI group appears on the checkpoint, and it is not the whole
difference.** `afterGroups` went from 1 group to 2 exactly at the diverging checkpoint.
The new group (id 63) holds one USA AI's command center, barracks, power plant and dozer.
In the same frame two `SpySatellitePing` objects were created (a satellite scan fired by
the AIs; `AIPlayer.cpp:1214`, the `computeSuperweaponTarget` grid direction, drew twice
on frame 3700). The AI section is the last thing in the stream, so a groups-only
difference can be tested exhaustively. Tested and rejected: no second group; any
ordered subset of its four members; any id from 55 to 99; either dirty flag, on either
group. So the difference lies (at least also) earlier in the stream.

Why a single dump cannot settle a multi-word difference: 1418 positions each reconcile
alone (the checksum difference behaves like one power of two), because a delta `d` at
word `i` equals `2d` at word `i+1`. Rank candidates by what changed, not by the
arithmetic.

**Instrument: `crc since frame P`.** At the first mismatch the engine takes the previous
snapshot from the capture ring (`RING = 4`), which is the last checkpoint that
*matched*: both machines held exactly those words. Any difference now must lie among
words that changed in between. It pairs spans (objects by id, sections by label) and
prints every `new`, `gone`, `resized` or `changed` span with `+offset old>new` words. It
tests whether undoing each single change, and each pair, gives the PC's checksum
(`<== UNDOING THIS ALONE GIVES THE PC's CHECKSUM`). The printed words allow any partial
hypothesis offline, such as "the PC moved this unit, but to a different place".

### `New_clear.rep`: the new client diverges with no input at all (23/09/2026)

The user recorded on the 23 September client: solo, Tournament B, start 3, **no input**.
It diverges at frame 100. Ours is `C09877CC` at 100, 200, 300, 400 and 500; the PC's is
`075B2848` at all five. Both states freeze after frame 100 (no draws, no movement), so
the difference is static and is set up during frames 0..100, or already at frame 0.
Together with `3.rep` (the same setup on the 28 August client, which matches), this
settles it: **the new client build computes a solo start differently.** It is not the
dozer selection and not the map.

The `crc since` instrument listed everything that changes on our side between frame 0
and frame 100: the dozer and 10 civilian cars snapping to cell centres (x and y, one z),
plus the seed. Tested against the PC checksum and rejected:

- undoing any subset of those 12 changes (all 4096 combinations);
- the seed after any number of draws 0..2999 from `SD=894280602`, alone or combined
  with any subset of the snaps (all draw counts up to 600);
- a single-word difference common to `New_clear.rep` and `USA.rep`. These are the same
  client and start with different seeds; 151453 of their 151538 words are identical,
  and they differ only in the dozer's spawn, the shroud around it and the seed. No
  position has the same implied word, or the same delta, in both;
- whole-column shroud variants: human player not revealed, observer not revealed,
  civilian fogged, unused players fogged.

A search for arguments or operands evaluated in an unspecified order (two
`GameLogicRandomValue` or two `getValue()` in one statement), where MSVC and clang could
draw in a different order, found none.

What the client update contains is not visible in its repository: history before
12/09 is squashed, and releases are built by hand (no workflow builds the release
preset). The most useful artefact now is **the new `generalszh.exe` itself**. It is
PE32, and disassembling it answers what reading the source cannot: whether
`sin`/`cos`/`atan2` go to `__libm_sse2_*` or x87, whether `WWMath::Inv_Sqrt` is still
the x87 asm, and whether float code is SSE2 or x87.

## Found: the new client appends a logic-CRC revision tag (23/09/2026)

The 23 September divergence was never in the simulation. Here is how it was found.

1. **The data pack is not it.** The GeneralsOnline data pack is a public zip listed
   in `https://cdn.playgenerals.online/manifest.json`. Comparing the old one
   (`082826_QFE1`) with the new one (`092226_QFE1`): all 174 data files are
   byte-identical. Only the exe and its DLLs changed. So "update the data pack on the
   phone" is not the fix, even though the phone was on the old one.
2. **Both exes are PE32 with no symbols**, so they can be compared directly. Both are
   linked with MSVC 14.51 and import the same `_libm_sse2_*_precise`, `_CIatan2` and
   `_ftol`. `WWMath::Inv_Sqrt` is the same x87 asm, inlined in 212 places in each.
   Float codegen statistics are nearly equal. The new exe does route indirect calls
   through Control Flow Guard (`call [__guard_check_icall_fptr]`).
3. **The checksum function is found by its strings.** `push offset "MARKER:Objects"`
   appears in exactly one function. Diffing that function's normalised instructions
   between the two exes shows one real change. After `MARKER:TheAI`/`TheAI` and the
   `GameSave` block, the new exe does
   `xferAsciiString("MARKER:OfficialLogicCRCRevision")` followed by
   `xferUnsignedInt(0x474F0001)` ("GO", revision 1), on every checksum.
4. **Verified offline to the bit.** Appending those words to our own captured stream
   reproduces the PC's recorded checksum exactly: `075B2848` for the no-input
   `New_clear.rep` and `737E0FCA` for `USA.rep`.

So the port simulated the new client's game correctly all along. It just did not tag
the checksum. `GameLogic::getCRC` now appends the tag (`GO_LOGIC_CRC_REVISION`) and
keeps both variants of the last 8 checkpoints. During playback,
`RecorderClass::handleCRCMessage` adopts whichever variant the recording uses at the
first checkpoint, so replays from the 28/08 client still compare correctly.
`GameLogic::reset` restores the tag for the next game, so live matches always send it.

**Lesson:** when a new client version diverges on the *first* checkpoint of a
*no-input* game and stays at a constant pair of checksums, suspect *what* is hashed
before *what* is simulated. And when the other side's binary is available, the checksum
function is the first thing to diff: it is easy to find through its `MARKER:` strings.
The rest of the tool chain (`crc since`, the two-replay test, the shroud footprint
model) ruled out the state and pointed away from the simulation, which is what made
the binary worth reading.

### After the revision tag: `USA.rep` matches to 3500; the next class is NaN bits (23/09/2026)

With the revision tag in place, `New_clear.rep` matches to its end (500) and `USA.rep`
matches up to 3500 (it used to break at 100). At 3600, `crc since` names two Rangers
(366, 367) walking, a new Ranger (368), the seed, 476 shroud cells and the AI group
counter. No single undo or pair of undos reconciles. In every diverging window of
`1.rep`, `5.rep` and `USA.rep`, the fp trace attributes the invalid flag to
`AI locomotor AmericaInfantryRanger`.

That points to a class the float-cast sanitizer cannot see: **code that reads a NaN's
bits, or rounds through the CRT.**

- `REAL_TO_INT_FLOOR/CEIL(x)` is `fast_float2long_round(fast_float_floor/ceil(x))`.
  `fast_float_floor` tests the sign *bit*, and `fast_float_trunc` masks the exponent,
  which turns a NaN into -inf or +inf depending on that bit. The default NaN of an
  invalid operation is `0xFFC00000` on x86 SSE and `0x7FC00000` on ARM64, so the same
  source line gets -inf on the PC and +inf here.
- `fast_float2long_round` is `lroundf` on both sides, but the client's comes from the
  Windows UCRT (imported by `GeneralsOnlineZH_60.exe`). There `long` is 32-bit, and
  NaN, infinities and out-of-range results return 0. bionic's `long` is 64-bit and it
  saturates, so after the `Int` narrowing every caller performs, the result is -1 or a
  truncated value rather than 0.

Fix: `refFloatBits` (BaseType.h) maps ARM's default NaN to x86's before any sign-bit
test (`fast_float_trunc/floor/ceil`, `WWMath::Fast_Is_Float_Positive`,
`Float_To_Int_Chop/Floor`). `fast_float2long_round` now follows the UCRT contract,
returning 0 outside the 32-bit range, and reports each call site once:
`[GX-NET] lround out of range frame N (logic): ... from libmain+0x...`, which can be
symbolised with the kept `.sym.so`.

### The NaN-bits fix was not live; per-frame movement ring (23/09/2026)

On the next run no `lround out of range` line was printed, and `USA.rep` still diverges
at 3600, so the NaN-bits fix, while correct, did not act here. The 3500..3599 window
also raises no invalid flag. Between 3500 (matched) and 3600 the player issued no
command. Barracks production made Ranger 368 at frame **3595** (its weapon timestamps
read 3595), and Rangers 366 and 367 walked towards the rally point. Tested and
rejected: ±64 ULP on either Ranger's x/y; ±16 ULP on their rotation; the pathfinder
queue counters (AI +523/+524) at any value 0..63; Ranger 368's weapon timestamps at any
pair of frames 3500..3600.

Also checked, as possible ARM-vs-x86 differences at the C++ level:

- `char` is unsigned on Android and no `-fsigned-char` is set, while `Byte` is
  `typedef char` (formerly `SignedByte`). In the simulation it only holds 0/1 flags
  (`DataChunkInput::readByte` for build lists, scripts, triggers) and small template
  bytes, so it is not live here. It is not changed globally either: port code that
  treats `char` as unsigned for UTF-8 would break.
- A search of the logic for `std::sort` and pointer-ordered containers found only
  sorts in `PartitionSolver` and `MultiplayerSettings`. The pointer-keyed maps
  (`AttackPriorityMap`, `ScoreKeeper`) are used for lookup or statistics only.

New instrument: `gxObjTraceFrame` keeps, in memory, the last 101 frames of every object
whose transform changed, plus the seed per frame. `gxObjTraceDump` prints them at the
first mismatch (`[GX-NET] obj trace frame N: id=... m=<12 words>`, the state at the
start of frame N). This allows offline tests such as "368 left the barracks d frames
earlier or later on the PC", using this device's own intermediate states.

### `USA.rep` at 3600: the Rangers are not the explanation; watch the Chinook (23/09/2026)

The movement ring shows the whole 3500..3600 window. Ranger 366 stopped at 3513 (a seed
change at 3514 is its idle-state draw). Ranger 367 walked every frame. Ranger 368 was
created at 3596. The CRC at frame N equals the ring's state at the start of frame N,
i.e. before that frame's update. Tested against the PC checksum and rejected: Ranger
367 at any of our own states from frame 3560 to 3601, combined with Ranger 368 created
0..5 frames earlier or 1 frame later (matrix and weapon timestamps), combined with the
seed ±12 draws.

The user's screenshots of this moment show the Chinook hovering over the supply dock,
loading. In our run it does not move at all in 3500..3600, so it is absent from the
`crc since` list. That is the same situation as `1.rep` ("it took the money and was
about to fly home") and `5.rep` (a supply centre and a Chinook just created). If the
PC's Chinook finishes loading and leaves earlier, the difference exists as
AI/supply-truck state (neither is hashed) until the Chinook moves. The ring now also
records each object's AI state id and, for supply gatherers, the boxes carried. It keeps
printing live for 600 frames after the first mismatch, so our own Chinook's departure
and route can be replayed as "the PC left k frames earlier".

## Tool: the launcher's Replay check screen (23/09/2026)

A replay holds commands, not state, so it cannot be cut to start at a later frame.
Resuming from a save game is no substitute: loading is not bit-exact for every
cache, so it would create desyncs of its own. What can be done is to get to the frame
faster. **Launcher → Logs → Replay check** lists `Replays/*.rep`, newest first, and
starts the game with the engine's own `-replay <name>` plus two options
(`GXReplayCheck.h`):

- `-gxFastTo <frame|-1>`: between rendered frames the engine runs extra logic frames
  for up to 40 ms, doing exactly what the headless simulation does per frame
  (`updateHeadless()` then `GameLogic::UPDATE()`). It does this until the frame, or to
  the end with -1. Checksums and every trace are those of a normal playback.
- `-gxAutoQuit`: stops 700 frames after the first mismatch (enough for the 600-frame
  movement trace) or at the end. It then writes `gx_replay_check_result.txt`
  (`status`, `frames`, `checkpoints`, `matched`, `last_matched`, `first_mismatch`,
  `seconds`) to the user-data folder and quits back to the launcher, which shows the
  summary.

"Run through" = `-gxFastTo -1 -gxAutoQuit`. "Watch from frame N" = `-gxFastTo N`. Both
switch `gx_net_trace.txt` on. The engine's true `-headless` mode is deliberately not
used: it has never been brought up on Android, while the windowed path is the one
every replay here has already used.

First run of Replay check: `USA.rep`, 812 frames in 2.6 s (about 310 logic frames per
second, roughly 5x real time). But it reported "0/5 matched, first mismatch at 112",
and every "ours" equalled the *previous* checkpoint's recorded value. The local checksum
travels as `MSG_LOGIC_CRC` through `TheMessageStream`, which `GameEngine::update`
propagates before each logic frame. The fast-forward loop skipped
`propagateMessages()`, so our checksums reached the comparison one checkpoint late.
(`GameLogic` routes them straight into `TheCommandList` only in
`RECORDERMODETYPE_SIMULATION_PLAYBACK`.) The loop now runs `updateHeadless()`,
`propagateMessages()`, then `GameLogic::UPDATE()`, the same order the engine uses.

## The PC's own event record: `-headless -replay <rep> -exportStats` (23/09/2026)

The replay gives one PC checksum per 100 frames, and the release exe has no
`-ReplayCRCInterval` (it is in a debug-only block). But the GeneralsOnline client can
simulate a replay headless and write `Replays/<name>.gamestats.json.gz`
(`StatsExporter.cpp`). The file holds every build with **frame and x/y**, every kill
and capture with frame and position, energy, rank, skill and science events, and each
player's money every 30 frames. It is the PC's *own* simulation, observed. This is not
"replaying on the PC to see whether it desyncs" (it cannot); the output is data to diff
against ours.

This port now writes the same record: `StatsExporter.cpp` is ported (uncompressed
`.gamestats.json`, no upload), hooked where the client hooks it (`Player::onUnitCreated`,
`onStructureConstructionComplete`, `Object::scoreTheKill`, `Object::onCapture`), and
driven by the Replay check (begin before the game starts, a snapshot after every logic
frame, export at the end). The log-share zip includes it.
`scripts/tooling/replay/compare_gamestats.py PC.json.gz ANDROID.json` prints the first
frame at which the two records differ, for example Ranger 368 built on another frame or
at another spot, or money diverging at a snapshot because a Chinook delivered earlier.

## Isolated: supply, and a purchased Chinook's first delivery (23/09/2026)

The user recorded two targeted replays on the new client. **Barracks only** (Rangers
walking to a far rally point) matches to the end. **Supply only** diverges at 5900
(matches to 5800) and runs through in 3.3 s. The event record dates the deliveries:
money earned rises at 3900 and 5160 (the free Chinook 323 that comes with the supply
centre) and at 5940. The last one is Chinook 324, **bought** at 4576 and produced at the
supply centre, making its **first** delivery. The divergence falls inside that delivery.
In `USA.rep` the diverging window (3500..3600) is likewise a Chinook's first loading,
at the warehouse.

Between 5800 and 5900 only three things change on our side: Chinook 324's y moves by
one ULP (0x442D0314 → 0x442D0315, at frame 5801/5802, as it lands at the dock), the spy
drone bobs, and the shroud changes in 952 words (some cells lose up to 4 lookers at
once). There are no draws, no NaN and no fragile libm calls in the window. Tested and
rejected:
- Chinook 324 x/y ±64 ULP;
- x, y and rotation jointly ±8 ULP;
- either Chinook at any of our own states 5800..6444 (the PC leaving earlier or later);
- the spy drone's shroud radius ±1 or ±2 cells, for every player column.

Next instrument: every shroud look and unlook (`doShroudReveal`/`undoShroudReveal`/
`doShroudCover`/`undoShroudCover`) goes into an 8192-event ring with frame, world x/y,
radius, cell, cell radius and player mask. `gxShroudTraceDump` prints the last 110
frames at the first mismatch, and `crc since` now prints every changed partition word,
up to 6000. Together they say whose look changed the fog, and with what numbers.

### `USA_Supply.rep` at 5900: it is the spy drone's height, not the Chinook (23/09/2026)

The shroud trace closed the fog question. In 5800..5900 there are no looks at all, only
six queued unlooks (radius 600, player 2), all around the warehouse where the Chinooks
flew earlier. Applying them as full circles reproduces every one of the 952 changed
partition words exactly. So the fog changes are fully explained, and removing any subset
of the unlooks does not give the PC's number.

The explanation comes from the rest of the dump at 5900, **6000 and 6100**. At each of
the three checkpoints the PC's checksum is exactly ours with **one word changed: the spy
drone's z translation**, by −2, −6 and +6 ULP. Everything else matches the PC at
6000 and 6100, including both Chinooks flying off after the delivery. The Chinook was a
red herring. It was simply the only other thing moving.

**Beware the checksum's equivalence classes.** The checksum is `ROL1(crc) + word`, and
ROL1 is multiplication by 2 modulo 2^32−1. So a change of δ in word j is almost
indistinguishable from δ·2^k in word j+k, and the pattern repeats every 32 words. A
single-word scan at 5900 therefore "hits" in about 250 places: "−1 at word 155", "−2^25
at word 180" (the drone's z, −2 ULP), and so on. One checkpoint cannot tell them apart.
What settles it is intersecting the **natural** candidates (small integer or ULP steps
of a field, or of either 16-bit half) over several checkpoints. Only word 180 (drone z)
survives all three, plus two nonsense words (the drone's id and a power plant's y).
Tools: `natcand.c` (natural-delta scan, filtered by the equivalence class so a full dump
takes 0.1 s) and `oneword.c`.

**The drone's height is fully modelled offline.** `AmericaVehicleSpyDrone`:
`SpyDroneLocomtor` (sic), `Lift 120`, `PreferredHeight 90`, `SURFACE_RELATIVE_HEIGHT`,
`HOVER`, `Apply2DFrictionWhenAirborne`, `Mass 50`, and `Gravity −64` at 60 Hz. The model is
`Locomotor::calcLiftToUseAtPt`, then `applyMotiveForce`, gravity, the velocity clamp
(`|v| < 0.001 → 0`) and `z += v`. With the terrain height fitted
(`S = 15.6249962`, 0x4179FFFC), it reproduces **all 697** traced frames (5801..6500)
bit for bit. On our side the height is a **period-78 limit cycle**: frame 5879 repeats
5801 exactly.

What the model rules out, starting from our state at every frame 5804..5899:
- a one-off z/v nudge (±64 ULP each), a skipped clamp, or no clamp from then on;
- a persistent change of the terrain height (±20000 ULP), lift, gravity, preferred
  height or mass (±2000 ULP), or the damaged lift;
- any hidden velocity at any frame (grid 1e−7 over ±0.03);
- arithmetic variants: `dz` without the `z + (P−z) − z` round trip, doubles in the brake
  test and the lift, `2dz − 2v`, the mass cancelled out. All of these give our exact
  trajectory, because the dynamics saturate: the lift is 0 or maximal almost every frame.

A −2 ULP nudge at 5900 grows to +894 ULP by 6000, while the PC stays within 6 ULP for 200
frames. So the PC is not our dynamics plus a perturbation. Its drone follows the same
cycle, and the checkpoints see a height that differs by a few ULP. Whatever does that
acts on the height the checksum reads, not (or not visibly) on the dynamics.

Next instruments, in the build after `shroudtrace-i18n`:
- `phys trace` ('P': height before, acceleration, velocity before and after the clamp,
  height after, motive, braking; 'L': the lift inputs and output), per object per frame;
- `cmd trace`: every network command the replay executes, with its group's object ids
  and raw arguments, for the last 400 frames before the mismatch. This is in case the
  recording orders the drone (or a building next to it) and this build resolves the
  selection differently;
- the movement ring is widened to 400 frames.

### Correction: it is supply after all; the drone was an artefact (23/09/2026)

The user disagreed with the drone reading and recorded two control replays on the PC:
- **`USA_Drone_Clear.rep`**: a spy drone and nothing else, 14500 frames. It **matches at
  all 145 checkpoints**.
- **`USA_Supply_Clear.rep`**: supply only, no drone. It diverges at **3600** and matches
  to 3500.

So the drone hovers identically on both machines, and the "drone z −2/−6/+6 ULP" reading
was wrong. How it went wrong is the lesson. The natural-candidate intersection only
considers **single-word** changes, and the checksum's equivalence classes (see above)
let a multi-word difference, such as a Chinook a few ULP off in x, y and heading, alias
onto a small change in some unrelated word. Consistency over three checkpoints did not
protect against that, because the true difference was a slowly evolving multi-word one.
**Treat a single-word explanation as proven only when the model behind it reproduces
the values.** Here it could not: the drone's exact hover model reached none of the PC
values, and that should have ended the drone hypothesis.

`USA_Supply_Clear.rep`, 3500..3600: the only moving object is the free Chinook 321, which
carries 8 boxes from the warehouse to the supply centre. In the window it turns onto its
final approach, brakes, and then alternates between accelerating and braking every frame
(3593, 3597, 3601..3605), because its distance to the goal hovers around the slow-down
distance. The replay executes **no commands** in the window. Searching the Chinook's
heading (float angle ± a few ULP, with `cos`/`sin` recomputed) together with x/y offsets
gives the PC's checksum at every checkpoint with the heading unchanged and a small x/y
offset. But the x/y class is ambiguous (`dy + 16·dx` is what the checksum sees), so
this only says "the Chinook's position, not its heading", not by how much.

Where a first delivery can differ: `DockUpdate::loadDockPositions` reads the docking
positions (`DockStart`/`DockAction`/`DockEnd`/`DockWaiting`) **once, on first use**, from
the building model's pristine bones through the W3D render object. That is render-side
code feeding the logic (TheSuperHackers left a note there: "We shouldn't depend on bones
of a drawable here!"). A one-bit difference in a bone position would change every later
approach path, and would first show exactly at a first delivery.

Next build (`docktrace`):
- `dock trace` prints the bone positions when loaded and each computed approach position,
  as raw bits;
- `phys trace` 'M' records each hover/other movement decision (position, goal, on-path
  distance vs slow-down distance, speeds, heading, force direction);
- 'T' records each heading step (goal, position, current and desired angle, turn amount,
  maximum turn rate).

### `USA_Supply_Clear.rep` at 3600: an exact model of the Chinook's approach (23/09/2026)

The user confirmed that the game files are identical on the PC and the phone (same
archive sizes, same builds), so **"different data" is ruled out. Do not ask again.**

The `docktrace` build logged the dock bones and every movement decision. What it showed:
- **The dock point is not the cause.** The world dock point (1538.5306, 694.0756) is
  `supplyCenter.transform × bone(DockAction)`. Recomputing it in float32 from the logged
  bone and the checksummed building matrix gives the same bits. The bone is read once, at
  frame 3374, from the model's pristine pose. The files are the same and the float code
  is the same, so it is the same on the PC.
- **atan2/sin/cos all go through double here, as on the PC.** Every logged heading and
  cached angle (487 frames) equals `(float)atan2((double)y, (double)x)`, not `atan2f`
  (which differs in 103 of them). Every rotation matrix is `(float)cos/sin((double)θ)`.
- **The Chinook's flight is now modelled exactly.** `chinook_approach_model.c` (in
  `scripts/tooling/replay/`) reproduces **every** frame 3501..3700 bit for bit (position,
  heading, velocity, acceleration). It is built from the traced inputs plus these rules:
  - `locoUpdate_moveTowardsPosition`;
  - `moveTowardsPositionOther` (the force uses the heading **before** the turn);
  - the braking "exact movement" (`pos += dir·|forwardSpeed2D|` while
    `OBJECT_STATUS_BRAKING`, with physics then leaving x/y alone);
  - `maintainCurrentPositionHover` on the one idle frame (3592, between approach and
    dock);
  - 2D lateral friction projected by `applyForce` while motive;
  - the 60 Hz rule that skips the x/y velocity clamp while motive;
  - the cached angle re-derived by `Get_Z_Rotation` after every physics step.

  The maximum turn rate must be computed exactly as `ConvertAngularVelocity…`
  (`180·(SPF·RPD)` = 0x3D567750, not π/60 = 0x3D567751). One ULP there already breaks
  the model within two frames.
- **What the PC checksum is consistent with.** At 3600 it equals ours with the Chinook
  offset in position only (class `16·dx + dy = 10` ULP), from the start of frame 3592 on.
  None of these reproduce 3600 and 3700 **together**:
  - a single perturbation of position or velocity at any frame 3501..3599 (±64 ULP);
  - a different constant (braking, acceleration, turn rate, minimum velocity, slow-down
    fudge, lateral friction; ±64 ULP);
  - a different approach or dock goal (±300 ULP);
  - one frame of timing at the approach→dock hand-over (3591..3593);
  - a flipped braking status at any frame.

  One approach-goal offset (−16, −132 ULP) matched 3600 alone and failed at 3700: a
  coincidence, which is why any hypothesis must now pass two checkpoints.

So the PC is not "our dynamics with one different number". Either its rules differ
somewhere in this flight in a way not yet modelled, or something besides the Chinook
differs at 3700.

**Dock bones verified from the model file (23/09/2026).** The user published the model
archives as a GitHub release (`MYSOREZ/Generals-ZeroHour`, tag `assaets-v1`; the assets
download through the API URL with `Accept: application/octet-stream`, because the
browser URL returns 404 through the proxy). `ABSUPPLYCT.W3D` is in the **base-game**
`W3D.big` (Zero Hour's `W3DZH.big` only has the `_A2*` variants). Its hierarchy has the
dock bones as plain children of the root (`DOCKACTION`, `DOCKSTART`, `DOCKEND`,
`DOCKWAITING01..09`). The animation (`ABSUPPLYCT`, 41 frames at 30 fps) only rotates
three fans. `scripts/tooling/replay/w3d_pristine_bones.py` rebuilds the pristine pose in
float32, exactly as `HTreeClass::read_pivots`, `Build_Matrix3D` and
`Anim_Update_Without_Interpolation` do. **Every dock bone equals the device's logged
value bit for bit.** The docking geometry is closed as a suspect.

**The new client's machine code checked against the source for the Chinook's path
(23/09/2026).** The 23 September exe is built from a non-public branch: the revision
tag is not in the public source. So each function on the Chinook's flight was read in
its disassembly (`/tmp/claude-0/goexe/dis.txt`, located via unique `.rdata`
constants). Identical to the public source:
- the braking "exact movement" (MIN_VEL at `0x9efd64`; `inv = 1/dist`,
  `pos += dx·inv·vel`, `vel = clamp(|fwd|, MIN_VEL, dist)`);
- `maintainCurrentPositionHover` (1e−10 at `0x9efd54`);
- `getForwardSpeed2D` (same odd per-axis formula);
- `applyMotiveForce` (expiry = frame + 20);
- `PhysicsBehavior::update`'s integration and velocity clamp (the x/y clamp is skipped
  while motive, exactly as in the source's 60 Hz branch);
- `Thing::getUnitDirectionVector2D` (`(float)cos/sin((double)θ)` through
  `_libm_sse2_cos/sin_precise`).

`atan2` on the PC goes through `_CIatan2` (x87 `fpatan`), not an SSE2 routine. Not yet
read: `moveTowardsPositionOther`, `rotateObjAroundLocoPivot`/`normalizeAngle`,
`Thing::setOrientation`, `Matrix3D::Get_Z_Rotation`, `applyFrictionalForces`/
`applyForce`, `calcSlowDownDist`.

**The rest of the Chinook's path in the new client's machine code.** Also identical to the
source:
- `rotateTowardsPosition`/`rotateObjAroundLocoPivot` (the turn rate is doubled only
  under `ULTRA_ACCURATE`);
- `normalizeAngle`;
- `applyFrictionalForces` (YPR damping 0.85; lateral friction projected, with the
  factors commuted);
- `applyForce` (the contained-items mass added; the lateral projection while motive).

Every libm result on the approach (487 `atan2`, the cached-angle `cos`/`sin`) was checked
with 200-bit arithmetic. The closest one to a float rounding boundary is 1.2·10⁻⁴ ULP
away, far outside any libm's error, so **no libm difference can act here**. The code is
the same and the arithmetic is the same, so the PC's difference must come from an
**input**. Checkpoints 100 frames apart cannot say which.

## Tool: a frame-exact comparison run by the PC itself (23/09/2026)

The replay header's `C=` field is the checksum interval the **player** uses. The recorder
writes the checksum of frame F as a `MSG_LOGIC_CRC` (type 1095, arguments: integer and
boolean) at frame F+1. While playing, the client queues each recorded checksum and
compares it with its own one at every C-th frame. At the first mismatch it pauses and
shows `InGame:… Replay:… Frame:N`. So:
1. The launcher's Replay check screen has a switch, **Checksum of every frame**
   (`-gxCrcEveryFrame`). With it, the engine also computes the checksum on every frame,
   at the recorder's instant, and prints `crc every frame N: X`. Nothing is sent or
   compared. The launcher also writes a `gx_crc_every_frame.txt` marker in the game
   folder, because the launch argument was once lost on the way to the engine.
   Check the log's `replay check …: started` line: it must end with
   `checksum of every frame on`. If it says `off`, the log is useless for step 2.
2. `scripts/tooling/replay/rep_crc_every_frame.py IN.rep generals-stderr.log OUT.rep`
   rewrites the recording:
   - the header gets `C=001` (same length, `atoi` reads 1);
   - every checksum record is replaced by this device's checksum of every frame;
   - frame 0 is left out when the original has no record for it. A multiplayer
     recording never has one, and its player drops its own first checksum and pairs
     the rest by arrival order. With frame 0 included, every pair is off by one frame
     and the replay "desyncs" at frame 2 on any machine.

   `--roundtrip` checks the parser: it rewrites the original byte for byte.
3. First play OUT.rep on the phone. It must match itself at every frame, which proves
   the alignment. Then play it on the **PC**: the PC pauses at the first frame where
   it disagrees with the phone.
