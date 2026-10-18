# Generals Challenge loading animation

## Report and evidence

Owner video Screenrecorder-2026-10-17-23-05-03-62.mp4 shows Generals Challenge (not the standard Skirmish lobby): general selection, then opponent-introduction portraits and teletype captions around seconds 10-17. Owner confirms the issue has existed since early builds on default settings. Recording: 1280x720, about 30 fps, 19.27 seconds. The local GC_Background.bik is 800x600, 30 fps, 6 seconds, with an audio stream. Recording and extracted frames stay local, outside Git.

## Confirmed code defects

FFmpegVideoStream::update decodes ahead to fill shared OpenAL buffers. This advances frameIndex and replaces the current video frame outside frameNext. ChallengeLoadScreen::init calls the player update while dispatching authored events through activatePieces(frame), whose switch cases use exact frame numbers (20, 24, 35, 98, 140). Invisible advancement can therefore drop visual frames, skip portrait/voice cues and reduce teletype updates. Independent of GSR or experimental rendering FPS.

Separately, constructing/destroying silent portrait or overlay streams resets the same movie-audio source used by the background video; their updates also service it without owning audio. Deterministic tests reproduce both defects against r16 source.

## Fix

- Optional provider-neutral setFrameAccuratePlayback policy (default no-op for Bink/other providers); FFmpeg defaults to normal audio look-ahead.
- Only the Android Challenge background movie opts into frame-accurate playback: audio maintenance continues without hidden video advancement. Other normal movies keep look-ahead.
- Dispatch every crossed Challenge cue once, including decoder packets delivering multiple frames. Repeated frame indices do not duplicate cues.
- Silent FFmpeg streams do not reset, start or service shared movie audio. Both Core and Zero Hour copies updated.
- No GSR, render FPS, simulation, AI, network or GeneralsOnline changes.

## Validation and limits

scripts/qa/test-challenge-video.py compiles and executes actual FFmpeg constructors/destructors/update against deterministic device stubs. Tests fail against r16 for silent audio ownership and frame-accurate advancement. Both Core and Zero Hour copies pass after the fix, retaining normal look-ahead. Actual cue loop passes repeated/jumped-frame coverage. Android NDK builds z_gameengine and z_gameenginedevice successfully (static libraries only).

The tests confirm these code defects and prevent regressions; they do not establish complete resolution of tablet-visible stutter. Need the next owner-authorised APK and actual displayed Challenge introduction on device, including narration, portraits, full captions, skip input, repeat entry, and ordinary campaign/intro audio regression. No new APK or release was built/published in this task; r16 remains unchanged. Build environment and previous artifacts retained. Revert this feature commit independently to roll back.
