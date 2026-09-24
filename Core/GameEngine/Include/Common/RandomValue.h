/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// RandomValue.h
// Random number generation system
// Author: Michael S. Booth, January 1998

#pragma once

#include "Lib/BaseType.h"

extern void InitRandom();
extern void InitRandom( UnsignedInt seed );
extern UnsignedInt GetGameLogicRandomSeed();   ///< Get the seed (used for replays)
extern UnsignedInt GetGameLogicRandomSeedCRC();///< Get the seed (used for CRCs)
extern UnsignedInt GXGameLogicRandomSeedCRCAfter( Int draws );///< GeneralsX diagnostic, see RandomValue.cpp

// Lets a helper be told which stream to draw from instead of hardcoding one. The client uses
// this to keep purely cosmetic draws (particle placement) off the logic stream: the logic RNG
// state is hashed into the lockstep CRC, so an extra draw there desynchronises the game even
// though the value is only ever used to place a puff of smoke.
struct RandomValueClass
{
	virtual Int GetRandomValueInt( Int lo, Int hi, const char *file, Int line ) const = 0;
	virtual Real GetRandomValueReal( Real lo, Real hi, const char *file, Int line ) const = 0;
};
struct LogicRandomValueClass final : RandomValueClass
{
	virtual Int GetRandomValueInt( Int lo, Int hi, const char *file, Int line ) const override;
	virtual Real GetRandomValueReal( Real lo, Real hi, const char *file, Int line ) const override;
};
struct ClientRandomValueClass final : RandomValueClass
{
	virtual Int GetRandomValueInt( Int lo, Int hi, const char *file, Int line ) const override;
	virtual Real GetRandomValueReal( Real lo, Real hi, const char *file, Int line ) const override;
};

// GeneralsX @feature Android port 15/09/2026 Logic-RNG call-site tally.
//
// The logic seed is hashed straight into the lockstep checksum, so a single
// extra or missing draw on one machine desynchronises the match. When the two
// machines disagree, the seed alone says the streams diverged, never who drew.
// Every draw already carries __FILE__ and __LINE__ for the debug logging, so
// the same two values can be tallied per call site and printed at the frames a
// checksum is generated -- turning "the seeds differ" into a short list of the
// call sites that consumed randomness in between. Recording costs nothing
// unless the gx_net_trace.txt marker is present.
extern void GameLogicRandomTallyDump( UnsignedInt frame );
// GeneralsX @feature Android port 20/09/2026 Clear the tally when a game starts.
// The count is cumulative between dumps, and nothing cleared it at game start, so
// draws made by a previous game in the same process were still in the first frame's
// figure: the same replay reported 1142 draws in one run and 881 in the next while
// every checksum was identical. A number that changes when the simulation does not
// is worse than no number.
extern void GameLogicRandomTallyReset( void );

// use these macros to access the random value functions
#define RandomValueInt(randomValueClass, lo, hi) randomValueClass.GetRandomValueInt( lo, hi, __FILE__, __LINE__ )
#define RandomValueReal(randomValueClass, lo, hi) randomValueClass.GetRandomValueReal( lo, hi, __FILE__, __LINE__ )

//--------------------------------------------------------------------------------------------------------------
