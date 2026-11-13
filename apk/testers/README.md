# Builds people are testing

APKs that were handed to someone in an issue and may still be installed or downloaded from a
link there. A new build never replaces or deletes them; one is removed only when the repository
owner says so. The current build for the owner lives one level up, in `apk/`.

| APK | Given to | Why |
|---|---|---|
| `GeneralsXZH-android-diag35.apk` | #35 (mad963) | 1.4.0 + render-thread crash diagnostics |
| `GeneralsXZH-android-generals-a8.apk` | #35 (mad963) | GPU frame timer off; confirmed working on Mali-G57 r32p1 |
| `GeneralsXZH-android-generals-a43.apk` | #58, #61, #64, #65 (everyone) | Test build before 1.4.2: tank treads, mods in subfolders and their GameData.ini, Restart speed, AI scripts check, vsync, subtitles |
| `GeneralsXZH-android-generals-a45.apk` | #62, #65, #69 (everyone) | a43 + the screen rate asked from Android for the frame limit (120 Hz screens), Generals special powers of built buildings (battle plans, CIA Intelligence, spy satellite), tank and wheel dust at 60 Hz |
| `GeneralsXZH-android-generals-a46.apk` | #62, #65 (everyone) | a45 + special powers of games saved by earlier builds load ready; terrain shader / cloud texture in the log (`[GX-TERRAIN]`) |
| `GeneralsXZH-android-generals-a47.apk` | #60, #68, #71 (everyone) | 1.4.2 + Bluetooth mouse clicks as mouse clicks, no double click in mouse mode (#60); Generals' Challenge at normal speed on 60 Hz (#68) |
| `GeneralsXZH-android-generals-a48.apk` | #71 (Jayzer74) | a47 + `[GX-PAYLOAD]` in the log: every plane power, transport and command it receives (Generals) |
| `GeneralsXZH-android-generals-a49.apk` | #71, 4PDA (everyone) | a48 + Generals powers no longer fire from the supply plane (#71); system bars and shade usable, game can be minimized; both landscape sides with auto-rotate |
| `GeneralsXZH-android-generals-a50.apk` | #74, #77, #78 (everyone) | a49 + Extras draw distance kept (#74); a mod's Generals.str labels without a language pack (#77); `[GX-SHROUD]` diagnostic (#78) |
| `GeneralsXZH-android-generals-a51.apk` | #82 (everyone) | a50 + Extras "Starting camera height": the height a match starts at; default as the original, the maximum (#82) |
| `GeneralsXZH-android-generals-a52.apk` | everyone | a51 + the frame limiter sleeps to the deadline instead of spinning the last 2 ms of every frame (less heat and battery) |
