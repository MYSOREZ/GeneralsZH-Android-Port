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


## Follow-up: owner feedback on r17

Owner reports longer waiting before gameplay and partial smoothness improvement.
The r17 opt-in disables video-advancing audio look-ahead, but frameDecompress was
still a no-op. Every overdue image therefore undergoes conversion and display;
when per-frame cost exceeds the media interval, wall-clock drift accumulates.
This is a demonstrated scheduling weakness, not a measured attribution of all
waiting time on the owner's tablet. Map loading and intro playback remain serial.

The follow-up catches up overdue Challenge images before conversion, with at
most eight advances per call, no seek, no decode beyond the final frame and a
no-progress/EOF exit. Crossed cue dispatch remains in place. It does not enable
look-ahead in update(), change ordinary movies, or modify map loading/gameplay.
It reduces redundant conversion/presentation work; it cannot guarantee smoothness
if decoding itself cannot sustain the source rate. Audio and portrait timing still
require device validation. No claim of 60/120 fps for a 30 fps source video.

Both Core and ZH actual-method C++ harnesses passed: silent audio ownership,
ordinary look-ahead behavior, on-time no-op, bounded late-frame recovery,
end-of-stream limit and stalled decoder. The clock reading alone is replaced by
a deterministic test clock. Existing crossed cue regression passed.
No replacement APK was built in this follow-up; r17 release assets are unchanged.

Android NDK z_gameenginedevice target compiled successfully in the preserved build tree.
