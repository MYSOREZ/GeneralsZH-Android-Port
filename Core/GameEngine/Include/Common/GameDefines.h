/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "WWDefines.h"

// Note: Retail compatibility must not be broken before this project officially does.
// Use RETAIL_COMPATIBLE_CRC and RETAIL_COMPATIBLE_XFER_SAVE to guard breaking changes.

#ifndef PRESERVE_BUILDING_RESUMPTION_DELAY
#define PRESERVE_BUILDING_RESUMPTION_DELAY (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef PRESERVE_CHINOOK_PASSENGER_DUMPING
#define PRESERVE_CHINOOK_PASSENGER_DUMPING (1)
#endif

#ifndef PRESERVE_HARDCODED_BLACK_LOTUS_CASH_HACK
#define PRESERVE_HARDCODED_BLACK_LOTUS_CASH_HACK (1)
#endif

#ifndef PRESERVE_MULTI_CRATE_PICKUP
#define PRESERVE_MULTI_CRATE_PICKUP (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef PRESERVE_NO_XP_FROM_FLAME_KILLS
#define PRESERVE_NO_XP_FROM_FLAME_KILLS (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef PRESERVE_NO_XP_FROM_OCL_KILLS
#define PRESERVE_NO_XP_FROM_OCL_KILLS (1)
#endif

// GeneralsX @note Android port 15/09/2026 Three of the switches below are written
// in the client as a choice between (0) and (1) on
// defined(GENERALS_ONLINE) && defined(GENERALS_ONLINE_COMMUNITY_PATCH_CHANGES),
// and are flattened to (1) here. That is not a simplification of the behaviour, it
// is the behaviour: the client defines GENERALS_ONLINE from CMake but
// GENERALS_ONLINE_COMMUNITY_PATCH_CHANGES only inside NextGenMP_defines.h, and this
// file is reached through PreRTS.h -> Common/GameCommon.h long before any
// translation unit includes that header. The second condition is therefore always
// false where it is evaluated, so the client compiles the (1) arm every time.
// Two of the three feed the lockstep checksum -- experience from poison kills and
// the Battle Bus death frame -- so if that macro ever does reach this file, these
// must move with it or cross-play desynchronises.

#ifndef PRESERVE_NO_XP_FROM_POISON_KILLS
#define PRESERVE_NO_XP_FROM_POISON_KILLS (1)
#endif

#ifndef PRESERVE_OCCUPANT_DETECTION_VIA_DRAG_SELECTION
#define PRESERVE_OCCUPANT_DETECTION_VIA_DRAG_SELECTION (1)
#endif

#ifndef PRESERVE_PERPETUAL_HORDE_BONUS
#define PRESERVE_PERPETUAL_HORDE_BONUS (1)
#endif

#ifndef PRESERVE_PREMATURE_BATTLE_BUS_DEATH
#define PRESERVE_PREMATURE_BATTLE_BUS_DEATH (1)
#endif

#ifndef PRESERVE_RADAR_WARNING_SUPPRESSION
#define PRESERVE_RADAR_WARNING_SUPPRESSION (1)
#endif

#ifndef PRESERVE_STRUCTURE_STEALTH_DURING_REPAIR
#define PRESERVE_STRUCTURE_STEALTH_DURING_REPAIR (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef PRESERVE_TUNNEL_HEAL_STACKING
#define PRESERVE_TUNNEL_HEAL_STACKING (1)
#endif

#ifndef PRESERVE_UNRELIABLE_FIRESTORMS
#define PRESERVE_UNRELIABLE_FIRESTORMS (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef PRESERVE_RETAIL_SCRIPTED_CAMERA
#define PRESERVE_RETAIL_SCRIPTED_CAMERA (1) // Retain scripted camera behavior present in retail Generals 1.08 and Zero Hour 1.04
#endif

// GeneralsX @bugfix Android port 13/09/2026 Match the simulation switches the
// GeneralsOnline client is built with, so a match against one can agree with it.
//
// A PC-hosted match now starts and runs, and then the two simulations disagree
// at the first CRC exchange: frame 100, this device AD768702, the PC 9EAFC02B.
// The cause is not the architecture. Upstream guards these defaults with
// defined(GENERALS_ONLINE) and turns them all off for its own client; this
// port, whose GameDefines.h came from TheSuperHackers where that macro does not
// exist, left them on. They are not cosmetic -- RETAIL_COMPATIBLE_CRC alone
// changes which bytes go into the per-frame checksum (m_objectUpgradesCompleted
// is hashed as eight bytes with it on, and as the full bit field with it off),
// so two simulations doing exactly the same thing still report different
// numbers. The other three change pathfinding allocation, AIGroup behaviour and
// save/xfer layout, all of which feed the simulation.
//
// Set unconditionally rather than by mirroring upstream's GENERALS_ONLINE
// guard, for the reason the networking switch below gives: that macro is added
// by GeneralsMD's CMakeLists and never reaches Core's targets, and a switch
// this deep resolving differently per target would be far worse than either
// value. This port is only ever a GeneralsOnline client -- retail's servers are
// gone -- so there is nothing on the other side of these to stay compatible
// with.
//
// The three PRESERVE_* switches above are deliberately NOT changed. Upstream
// gates those on GENERALS_ONLINE_COMMUNITY_PATCH_CHANGES as well, and that is
// defined in NextGenMP_defines.h, which GameDefines.h does not include and
// PreRTS.h does not pull in first -- so in the shipped client they resolve to
// the retail branch, which is what this file already has.
//
// Changing these changes this build's checksums: an older build and this one
// will now disagree with each other exactly as this port and the PC did.

#ifndef RETAIL_COMPATIBLE_CRC
#define RETAIL_COMPATIBLE_CRC (0) // GeneralsOnline builds with this off; see the note below
#endif

#ifndef RETAIL_COMPATIBLE_XFER_SAVE
#define RETAIL_COMPATIBLE_XFER_SAVE (0) // GeneralsOnline builds with this off; see the note below
#endif

// This is here to easily toggle between the retail compatible with fixed pathfinding fallback and pure fixed pathfinding mode
//
// The GeneralsOnline client never defines this symbol at all, so every #if on it in
// AIPathfind.cpp - 93 of them - takes the false arm there. It was (1) here, which changed the
// A* search itself: at AIPathfind.cpp:1309 the start cell is opened with m_open = TRUE on this
// side and FALSE on the client's, because s_useFixedPathfinding is initialised false and only
// the retail arm consults it. A different start node expands differently, which gives a
// different path, which puts units in different places - and unit positions are hashed.
#ifndef RETAIL_COMPATIBLE_PATHFINDING
#define RETAIL_COMPATIBLE_PATHFINDING (0)
#endif

// This is here to easily toggle between the retail compatible pathfinding memory allocation and the new static allocated data mode
#ifndef RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION
#define RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION (0) // GeneralsOnline builds with this off; see the note below
#endif

#ifndef RETAIL_COMPATIBLE_CIRCLE_FILL_ALGORITHM
#define RETAIL_COMPATIBLE_CIRCLE_FILL_ALGORITHM (1) // Use the original circle fill algorithm, which is more efficient but less accurate
#endif

// Disable non retail fixes in the networking, such as putting more data per UDP packet
//
// GeneralsX @bugfix Android port 13/09/2026 Default to the non-retail wire
// format, because that is the only one anything this port can talk to speaks.
//
// GeneralsOnline builds with this off (upstream guards the default with
// !defined(GENERALS_ONLINE)), which makes its UDP payloads up to 1100 bytes
// against retail's 476, its TransportMessage::data correspondingly wider, and
// its command-id ordering overflow-safe. This port had it on, so a PC-hosted
// match and this device disagreed about the size and shape of every game
// packet -- and nothing catches that: the lobby compares INI and EXE
// checksums, not the transport, so the match starts, the loading screen sits
// at 0%, and the peer eventually times out. A device log of exactly that shows
// 268 packets from the PC rejected as "Is NOT a generals packet".
//
// Set here rather than by mirroring upstream's GENERALS_ONLINE guard on
// purpose: that macro is added by GeneralsMD's CMakeLists and so does not
// reach Core's own targets, which compile NetworkDefs.h too. Guarding on it
// would give one binary two different TransportMessage layouts depending on
// which target a translation unit landed in, which is worse than either value.
//
// Retail networking is not a mode this port can use anyway -- retail's
// GameSpy servers are long gone -- so there is nothing on the other side of
// this switch to stay compatible with.
#ifndef RETAIL_COMPATIBLE_NETWORKING
#define RETAIL_COMPATIBLE_NETWORKING (0)
#endif

// This is essentially synonymous for RETAIL_COMPATIBLE_CRC. There is a lot wrong with AIGroup, such as use-after-free, double-free, leaks,
// but we cannot touch it much without breaking retail compatibility. Do not shy away from using massive hacks when fixing issues with AIGroup,
// but put them behind this macro.

#ifndef RETAIL_COMPATIBLE_AIGROUP
#define RETAIL_COMPATIBLE_AIGROUP (0) // GeneralsOnline builds with this off; see the note below
#endif

#ifndef ENABLE_GAMETEXT_SUBSTITUTES
#define ENABLE_GAMETEXT_SUBSTITUTES (1) // The code can provide substitute texts when labels and strings are missing in the STR or CSF translation file
#endif

#ifndef ALLOW_MONEY_PER_MINUTE_FOR_PLAYER
#define ALLOW_MONEY_PER_MINUTE_FOR_PLAYER (0) // When enabled, a money-per-minute stat is calculated and displayed in-game
#endif

// Previously the configurable shroud sat behind #if defined(RTS_DEBUG)
// Enable the configurable shroud to properly draw the terrain in World Builder without RTS_DEBUG compiled in.
// Disable the configurable shroud to make shroud hacking a bit less accessible in Release game builds.
#ifndef ENABLE_CONFIGURABLE_SHROUD
#define ENABLE_CONFIGURABLE_SHROUD (1) // When enabled, the GlobalData contains a field to turn on/off the shroud, otherwise shroud is always enabled
#endif

// Enable buffered IO in File System. Was disabled in retail game.
// Buffered IO generally is much faster than unbuffered for small reads and writes.
#ifndef USE_BUFFERED_IO
#define USE_BUFFERED_IO (1)
#endif

// Enable cache for local file existence. Reduces amount of disk accesses for better performance,
// but decreases file existence correctness and runtime stability, if a cached file is deleted on runtime.
#ifndef ENABLE_FILESYSTEM_EXISTENCE_CACHE
#define ENABLE_FILESYSTEM_EXISTENCE_CACHE (1)
#endif

// Enable prioritization of textures by size. This will improve the texture quality of 481 textures in Zero Hour
// by using the larger resolution textures from Generals. Content wise these textures are identical.
#ifndef PRIORITIZE_TEXTURES_BY_SIZE
#define PRIORITIZE_TEXTURES_BY_SIZE (1)
#endif

// Enable obsolete code. This mainly refers to code that existed in Generals but was removed in GeneralsMD.
// Disable and remove this when Generals and GeneralsMD are merged.
#if RTS_GENERALS
#ifndef USE_OBSOLETE_GENERALS_CODE
#define USE_OBSOLETE_GENERALS_CODE (1)
#endif
#endif

// Overwrite window settings until wnd data files are adapted or fixed.
#ifndef ENABLE_GUI_HACKS
#define ENABLE_GUI_HACKS (1)
#endif

// Tell our computer identity in the LAN lobby. Disable for privacy.
// Was enabled in the retail game and exposed the computer login and host names.
#ifdef RTS_DEBUG
#ifndef TELL_COMPUTER_IDENTITY_IN_LAN_LOBBY
#define TELL_COMPUTER_IDENTITY_IN_LAN_LOBBY (1)
#endif
#endif

#define MIN_DISPLAY_BIT_DEPTH       16
#define DEFAULT_DISPLAY_BIT_DEPTH   32
#define DEFAULT_DISPLAY_WIDTH      800 // The standard resolution this game was designed for
#define DEFAULT_DISPLAY_HEIGHT     600 // The standard resolution this game was designed for
