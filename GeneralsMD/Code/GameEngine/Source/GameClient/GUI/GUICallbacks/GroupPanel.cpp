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

// FILE: GroupPanel.cpp ///////////////////////////////////////////////////////////////////////////
// GeneralsX @feature Android port 02/08/2026
//
// Native in-engine replacement for the Android-overlay unit-group touch
// panel: a small handle button that expands a row of 10 buttons (1-9, 0)
// for control-group assignment/recall. GroupPanel.wnd is loaded from a
// loose file (Window\GroupPanel.wnd) rather than the game's own .big
// archives -- see GameWindowManagerScript.cpp's Window\ path resolution,
// same trick already used for GeneralsOnline's own screens -- so this
// never touches the user's separately-owned game data.
//
// Tap a group button (GBM_SELECTED): recall/select that group, same as a
// bare 1-9/0 keypress -- unless the group is still empty, in which case
// tap assigns instead (an empty group has no useful "recall" meaning to
// begin with, and a first-time user has no reason to already know the
// long-press/right-click-to-assign convention -- this exact confusion was
// reported against the Android-overlay version of this feature).
// Two-finger-tap a group button (GBM_SELECTED_RIGHT -- GadgetPushButton's
// existing right-click handling, which the touch layer's two-finger-tap
// already produces at whatever screen position it lands on): always
// assign/replace, regardless of occupancy -- the deliberate, explicit
// version of the same action.
//
// None of the actual group logic (assignment, recall, double-press-to-
// recenter-camera) is reimplemented here -- SelectionXlat.cpp's
// onMetaCreateTeam()/onMetaSelectTeam() already do all of it; this only
// needs to feed the same MSG_META_CREATE_TEAM<n>/MSG_META_SELECT_TEAM<n>
// a keyboard shortcut would have.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/WindowLayout.h"
#include "GameLogic/Squad.h"

static NameKeyType s_buttonHandleID = NAMEKEY_INVALID;
static NameKeyType s_groupRowID = NAMEKEY_INVALID;
static NameKeyType s_buttonGroupID[10];
static Bool s_groupRowExpanded = FALSE;

//-------------------------------------------------------------------------------------------------
static void cacheWidgetIDs()
{
	if (s_buttonHandleID != NAMEKEY_INVALID) {
		return; // already cached -- GWM_CREATE fires once per window in this layout
	}
	s_buttonHandleID = TheNameKeyGenerator->nameToKey("GroupPanel.wnd:ButtonHandle");
	s_groupRowID = TheNameKeyGenerator->nameToKey("GroupPanel.wnd:GroupRow");
	char buf[40];
	for (Int i = 0; i < 10; ++i) {
		sprintf(buf, "GroupPanel.wnd:ButtonGroup%d", i);
		s_buttonGroupID[i] = TheNameKeyGenerator->nameToKey(buf);
	}
}

//-------------------------------------------------------------------------------------------------
static Int findGroupButtonIndex(Int controlID)
{
	for (Int i = 0; i < 10; ++i) {
		if (controlID == s_buttonGroupID[i]) {
			return i;
		}
	}
	return -1;
}

//-------------------------------------------------------------------------------------------------
static Bool isGroupEmpty(Int group)
{
	if (!ThePlayerList) {
		return TRUE;
	}
	Player *player = ThePlayerList->getLocalPlayer();
	if (!player) {
		return TRUE;
	}
	Squad *squad = player->getHotkeySquad(group);
	return !squad || squad->getLiveObjects().empty();
}

//-------------------------------------------------------------------------------------------------
static void handleGroupCommand(Int group, Bool forceAssign)
{
	if (!TheMessageStream || group < 0 || group > 9) {
		return;
	}
	Bool assign = forceAssign || isGroupEmpty(group);
	GameMessage::Type type = assign
		? (GameMessage::Type)(GameMessage::MSG_META_CREATE_TEAM0 + group)
		: (GameMessage::Type)(GameMessage::MSG_META_SELECT_TEAM0 + group);
	TheMessageStream->appendMessage(type);
}

//-------------------------------------------------------------------------------------------------
void GroupPanelInit(WindowLayout *layout, void *userData)
{
	(void)layout;
	(void)userData;
}

//-------------------------------------------------------------------------------------------------
void GroupPanelUpdate(WindowLayout *layout, void *userData)
{
	(void)layout;
	(void)userData;
}

//-------------------------------------------------------------------------------------------------
void GroupPanelShutdown(WindowLayout *layout, void *userData)
{
	(void)userData;
	if (layout) {
		layout->hide(TRUE);
	}
}

//-------------------------------------------------------------------------------------------------
WindowMsgHandledType GroupPanelSystem(GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2)
{
	(void)window;
	(void)mData2;

	switch (msg) {

		case GWM_CREATE:
		{
			cacheWidgetIDs();
			GameWindow *groupRow = TheWindowManager->winGetWindowFromId(nullptr, s_groupRowID);
			if (groupRow) {
				groupRow->winHide(TRUE);
			}
			s_groupRowExpanded = FALSE;
			break;
		}

		case GWM_DESTROY:
			break;

		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();

			if (controlID == s_buttonHandleID) {
				s_groupRowExpanded = !s_groupRowExpanded;
				GameWindow *groupRow = TheWindowManager->winGetWindowFromId(nullptr, s_groupRowID);
				if (groupRow) {
					groupRow->winHide(!s_groupRowExpanded);
				}
				return MSG_HANDLED;
			}

			Int group = findGroupButtonIndex(controlID);
			if (group >= 0) {
				handleGroupCommand(group, FALSE);
				return MSG_HANDLED;
			}
			break;
		}

		case GBM_SELECTED_RIGHT:
		{
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();

			Int group = findGroupButtonIndex(controlID);
			if (group >= 0) {
				handleGroupCommand(group, TRUE);
				return MSG_HANDLED;
			}
			break;
		}

	}

	return MSG_IGNORED;
}
