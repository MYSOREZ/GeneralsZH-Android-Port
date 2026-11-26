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
| `GeneralsXZH-android-generals-a53.apk` | everyone, kingvladder (cheat request) | a52 + Cheats in the in-game Options against the computer only: +10000 money, instant build, reveal map, +1 General point |
| `GeneralsXZH-android-generals-a54.apk` | everyone, kingvladder (cheat request) | a53 + the Cheats button actually shown in games against the computer (the hidden observer no longer counted as a human), placed left of Extras |
| `GeneralsXZH-android-generals-a55.apk` | everyone, kingvladder (cheat request) | a54 + cheats actually applied (the paused Options no longer drop the cheat command) |
| `GeneralsXZH-android-generals-a56.apk` | #78 (generalmilk), everyone | a55 + loading a save no longer takes every object's sight off the map once more (#78) |
| `GeneralsXZH-android-generals-a57.apk` | #73, everyone | a56 + Hero and Camera reset buttons in the army column (#73) |
| `GeneralsXZH-android-generals-a58.apk` | everyone | a57 + fixes found in forks: multiplayer kept alive under the notification shade / overlays, LAN official-map path, terrain LOD typo, sorting pool overflow, foldables |
| `GeneralsXZH-android-generals-a59.apk` | #59 (nxtskippr), #76 (noconsole), everyone | a58 + no crash when a container dies with occupants and no owner (#59); `[GX-PERF-PRERTT]` splits the pre-render update for mods (#76) |
| `GeneralsXZH-android-generals-a60.apk` | #85, #93, everyone | 1.4.3 + network frame limit in the launcher (#93); a mod's own AI scripts over the stock loose ones (#85); Indonesian labels and translation fixes (#91, #92) |
