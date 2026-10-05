# PLAN-025: APK size

Status: **parked (05/10/2026)** by the owner's decision. Option A is the one to consider first when
this comes back.

## Where the size is (test build a10, 92.2 MB)

| Part | Compressed in the APK |
|---|---|
| 4 engines: Zero Hour 30/60 Hz (19.5 MB each), Generals 30/60 Hz (14.4 MB each) | 67.8 MB |
| `classes.dex` (stored, not compressed) | 9.5 MB |
| Turnip driver for Adreno 7xx (`assets/default_driver`) | 3.8 MB |
| ANGLE, DXVK, SDL, OpenAL, libc++ and the rest | ~7 MB |
| resources, fonts | ~3 MB |

1.4.0 was 63.2 MB; the +29 MB since is exactly the two Generals engines (the rooms work added
~1.3 MB and was reverted). Each engine carries its own copy of FFmpeg (built with nearly every
codec), OpenSSL, curl, GameNetworkingSockets/protobuf; 30 and 60 Hz are two full builds because
the tick rate is a compile-time constant. The launcher is packaged with `assembleDebug`, so
Material/AndroidX/Kotlin go into the dex unshrunk -- our own Java is a few hundred KB.

## Options (estimates, to be measured when done)

| | Saving | Risk / cost |
|---|---|---|
| **A. Launcher as a release build with R8** (same keystore, so it still installs over existing versions) | 7-8 MB | low: keep rules for everything native code calls through JNI (`org.libsdl.app.**`, `GeneralsZHActivity` callbacks such as `setLanMulticastLock`, `showTextEditor`, the text-editor natives) |
| B. FFmpeg with only what the game plays (Bink video + its audio) | 5-7 MB | low; check every movie on a device |
| C. FFmpeg, OpenSSL, curl, GNS as shared `.so` used by all four engines | 8-12 MB | medium: build restructuring |
| D. 60 Hz engines downloaded on demand (the signed engine-update path exists) | ~34 MB | medium: 60 Hz unavailable offline until fetched |

A + B ≈ 75 MB; A-C ≈ 63 MB (1.4.0's size, with both games); with D under 35 MB.
