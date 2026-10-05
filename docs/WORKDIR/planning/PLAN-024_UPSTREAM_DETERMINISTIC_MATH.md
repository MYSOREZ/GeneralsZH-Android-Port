# PLAN-024: TheSuperHackers deterministic math (PR #2670) and cross-play

Status: **watching** (recorded 04/10/2026). Nothing to do until the PR is merged upstream.

## What is coming

TheSuperHackers/GeneralsGameCode PR #2670 by Okladnoj, "feat(math): Route game logic math through
WWMath with 3-mode deterministic support". On 04/10/2026 it had one approving review from a
maintainer, all 25 checks green and no conflicts, so it can be merged at any time.

- About 200 direct C math calls in game logic (pathfinding, physics, weapons, ...) go through
  `WWMath` wrappers, with three modes: **VC6** (inline x87, retail), **CRT** (platform libm), and
  **GameMath** (fdlibm-based, bit-identical everywhere; FetchContent).
- `USE_DETERMINISTIC_MATH` (GameMath) is **on by default for every non-VC6 build**.
- `-ffp-contract=off`, `WWMath::Div_Safe()`, `Trig.cpp` redirected to `WWMath`.
- The author measured Windows x86 MSVC and macOS ARM64 with fdlibm at the same CRC (`76B53840`).

This port is cited in the PR thread as a case study (cross-play with the GeneralsOnline PC client
on native math). The owner posted a correction: we match the **VC6 client**, which needed
`(float)sin((double)x)` instead of bionic's `sinf` — see
`docs/WORKDIR/lessons/LESSON-cross-play-desync-method.md`.

## Update 05/10/2026 (from the PR thread)

- Okladnoj: GameMath is used **only when `RETAIL_COMPATIBLE_CRC` is off**; `BaseDefines.h` disables
  `USE_DETERMINISTIC_MATH` for a retail-compatible build. This port builds with
  `RETAIL_COMPATIBLE_CRC 0` (as the GeneralsOnline client), so **taking the PR switches us to
  GameMath automatically** -- step 1 below is not optional.
- His measurements match ours: native math on ARM64 (macOS clang) and Windows x64 gives
  0 differing lines; against 32-bit x86 (x87) it differs -- 22 lines at `_PC_53`, 338 at `_PC_24`.
  The GeneralsOnline PC client is VC6 x87 at `_PC_24`, the worst row; this port matches it by
  evaluating the hashed transcendentals in double and narrowing once (the lesson). Tests:
  https://github.com/Okladnoj/GeneralsGameCode/tree/okji/test/deterministic-math-v2.2.8/tests
- xezon doubts native math is enough across all CPUs and libms; fbraz3 (who cited this port)
  said it was raised for discussion only.

## Update 05/10/2026 (OmniBlade, TheSuperHackers)

- Our double-and-narrow works because the double CRT result is computed at higher precision and
  then rounded to float: as long as the rounding mode is the same and the double function's error
  is below a float's precision, the float comes out the same. It does **not** make transcendental
  functions return the same value on every platform and C runtime, and nothing can guarantee that.
  This matches what we measured, and it names the residual risk: a double result that lands within
  its error of a float rounding boundary narrows differently. Rare, and not seen in the 1946
  checkpoints of `Global_War*.rep`, but possible -- and it depends on bionic's libm, which could
  change between Android versions.
- **Long-term compatibility with the VC6 clients is not a TheSuperHackers goal**: it constrains
  engine improvements too much. So upstream will move to GameMath and stay there; holding the VC6
  contract is this port's job alone, for as long as the GeneralsOnline PC client is VC6.
- GameMath's performance is now close to the CRT's (sqrt intrinsics) -- so cost is no argument
  against switching once the PC client does.

Consequence for step 4 below: the day GeneralsOnline's Windows client moves to GameMath, this port
should switch at the same time; until then, keep the VC6 contract on the hashed path.

## Why it matters here

Cross-play with the PC works because this port reproduces what the GeneralsOnline Windows client
computes: VC6 CRT on 32-bit x87 (`sinf` promoted to double, `_PC_24`). The PC client does not use
GameMath or fdlibm. If the merge brings GameMath in as the default, our game logic stops computing
what the PC computes, and every Android-to-PC match desyncs — while Android-to-Android and replays
recorded on Android still agree, so it would not show up in local testing.

## What to do when it is merged

1. **Do not take the default blindly in the upstream sync.** AGENTS.md says "game logic: keep
   theirs"; for this PR that rule is wrong for us. Keep our math path matching the VC6 client:
   the CRT-style mode, with `Lib/trig.h` / `Trig.cpp` evaluating in double and narrowing once.
2. **Check what the PR's CRT mode actually does on bionic.** If its CRT mode calls `sinf`/`cosf`,
   it is not the VC6 contract; route the hashed path through the double-and-narrow pattern
   (lesson: "the rule for anything on the hashed path").
3. **Prove it with the replay check before shipping**: `Global_War.rep` and `Global_War2.rep`
   (1946 checkpoints) must still match the PC; then a live match against a PC.
4. **Ask the GeneralsOnline team** whether their Windows client will adopt GameMath. If it does,
   switch to GameMath on our side at the same time and re-run step 3 against their new build —
   then the double-and-narrow patches become unnecessary.
5. Retail replays stay on the VC6 contract either way.

## Links

- https://github.com/TheSuperHackers/GeneralsGameCode/pull/2670
- `docs/WORKDIR/lessons/LESSON-cross-play-desync-method.md` ("Deterministic math / fdlibm",
  "The libm behind the source, not the source")
