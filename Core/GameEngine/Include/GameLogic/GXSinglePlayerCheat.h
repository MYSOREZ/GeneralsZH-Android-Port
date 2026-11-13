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

// GeneralsX @feature Android port 13/11/2026 Cheats for games against the computer only (campaign,
// skirmish, Generals' Challenge), asked for by a player. The menu (CheatMenu.cpp) sends
// MSG_GX_SINGLE_PLAYER_CHEAT, and the game logic applies it (GameLogicDispatch.cpp): so the cheat is
// part of the game's record and a replay of the game repeats it. The logic refuses it whenever the
// game has more than one human player, live or in a replay, so a network game can never take one.
#pragma once

#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameLogic/GameLogic.h"

namespace GXSinglePlayerCheat
{
enum Cheat
{
	ADD_CASH = 0,          ///< +10000 money
	TOGGLE_INSTANT_BUILD,  ///< every unit, building and upgrade done at once (Player::buildsInstantly)
	REVEAL_MAP,            ///< the whole map revealed
	ADD_GENERAL_POINT,     ///< one General's point (science purchase point)
	COUNT
};

inline Int humanPlayers()
{
	Int humans = 0;
	if (ThePlayerList)
		for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
		{
			const Player *player = ThePlayerList->getNthPlayer(i);
			if (player && player->getPlayerType() == PLAYER_HUMAN)
				++humans;
		}
	return humans;
}

/// The logic's test, the same live and in a replay of the game.
inline Bool allowedInLogic()
{
	return TheGameLogic && !TheGameLogic->isInMultiplayerGame() && humanPlayers() <= 1;
}

/// The menu's test: a game being played (not watched) against the computer.
inline Bool menuAvailable()
{
	return TheGameLogic && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame()
		&& !TheGameLogic->isInReplayGame() && allowedInLogic();
}
}
