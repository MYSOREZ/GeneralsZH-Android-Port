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

// FILE: GUIUtil.h //////////////////////////////////////////////////////
// Author: Matthew D. Campbell, Sept 2002

#pragma once

class GameWindow;
class GameInfo;

void ShowUnderlyingGUIElements( Bool show, const char *layoutFilename, const char *parentName,
															 const char **gadgetsToHide, const char **perPlayerGadgetsToHide );

// GeneralsX @bugfix Android port 19/09/2026 Every multiplayer setup screen
// looks its controls up by name and guards the result with DEBUG_ASSERTCRASH,
// which Common/Debug.h:206 compiles to ((void)0) in the shipped RelWithDebInfo
// build -- so a control that isn't in the layout produces a null pointer that
// flows straight into the next line. It is not a hypothetical: winCreateLayout
// succeeds even when an individual control fails to parse, because
// GameWindowManagerScript.cpp:2899-2905 pushes whatever parseWindow returned,
// including nullptr, and keeps going. A tester's crash on 19/09/2026 --
// SIGSEGV at fault_addr=0x8 on pressing "create game" in the LAN lobby, right
// after winCreateLayout had returned a valid layout -- is that exact shape:
// GameWindow's first data member after the vptr is m_status at +0x8, and
// winGetEnabled()/winEnable() read nothing else.
//
// These two say which control is missing, by name, in the shipped build, so a
// log answers the question instead of a tester's phone having to reproduce it.
// gxFindControl returns nullptr for a missing control rather than asserting;
// the caller is expected to refuse to open the screen, not to soldier on.
GameWindow *gxFindControl( GameWindow *parent, const char *controlName, Bool *missingFlag );
Bool gxRequireControl( const GameWindow *control, const char *controlName, Bool *missingFlag );

void PopulateColorComboBox(Int comboBox, GameWindow *comboArray[], GameInfo *myGame, Bool isObserver = FALSE);
void PopulatePlayerTemplateComboBox(Int comboBox, GameWindow *comboArray[], GameInfo *myGame, Bool allowObservers );
void PopulateTeamComboBox(Int comboBox, GameWindow *comboArray[], GameInfo *myGame, Bool isObserver = FALSE);
void PopulateStartingCashComboBox(GameWindow *comboBox, GameInfo *myGame);

void EnableSlotListUpdates( Bool val );
Bool AreSlotListUpdatesEnabled();

void UpdateSlotList( GameInfo *myGame, GameWindow *comboPlayer[],
										GameWindow *comboColor[], GameWindow *comboPlayerTemplate[],
										GameWindow *comboTeam[], GameWindow *buttonAccept[],
										GameWindow *buttonStart, GameWindow *buttonMapStartPosition[] );

void EnableAcceptControls(Bool Enabled, GameInfo *myGame, GameWindow *comboPlayer[],
										GameWindow *comboColor[], GameWindow *comboPlayerTemplate[],
										GameWindow *comboTeam[], GameWindow *buttonAccept[], GameWindow *buttonStart,
										GameWindow *buttonMapStartPosition[], Int slotNum = -1);
