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

// GXReplayCheck.h ///////////////////////////////////////////////////////////
// GeneralsX @feature Android port 23/09/2026 Replay checking without watching.
//
// A replay can only be reproduced by simulating it from frame 0 -- it holds the
// commands, not the state -- so "start at frame 3400" really means "get to 3400
// fast". Two launch options, set by the launcher's Replay check screen and
// combined with the engine's own -replay:
//
//   -gxFastTo <frame>   run the logic without drawing until <frame>, then play
//                       normally; -1 means the whole replay
//   -gxAutoQuit         when the replay ends, or some frames after the first
//                       checksum mismatch, write gx_replay_check_result.txt in
//                       the user-data folder and quit back to the launcher
//
// Fast-forward runs extra logic frames between rendered ones, exactly what the
// engine's headless replay simulation does per frame (particles, then logic), so
// the simulation and its checksums are the same as when watching.
#pragma once

#include "Lib/BaseType.h"

namespace GXReplayCheck
{
	void setFastForwardTo( Int frame );
	void setAutoQuit( Bool autoQuit );
	Bool isActive();

	// GameEngine::update, after the regular logic update.
	void update();

	// RecorderClass, at every compared checkpoint.
	void noteCheckpoint( UnsignedInt frame, Bool matched, UnsignedInt ours, UnsignedInt recorded );
}
