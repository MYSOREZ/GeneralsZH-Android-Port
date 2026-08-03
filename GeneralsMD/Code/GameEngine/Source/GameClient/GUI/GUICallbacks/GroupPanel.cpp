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
// panel: a small handle button that expands a row of 10 buttons (0-9) for
// control-group assignment/recall. GroupPanel.wnd is loaded from a loose
// file (Window\GroupPanel.wnd) rather than the game's own .big archives --
// see GameWindowManagerScript.cpp's Window\ path resolution, same trick
// already used for GeneralsOnline's own screens -- so this never touches
// the user's separately-owned game data.
//
// Quick tap (GBM_SELECTED): recall/select that group, same as a bare 0-9
// keypress -- unless the group is still empty, in which case tap assigns
// instead (an empty group has no useful "recall" meaning to begin with,
// and a first-time user has no reason to already know the
// long-press/right-click-to-assign convention -- this exact confusion was
// reported against the Android-overlay version of this feature).
// Two-finger-tap a group button (GBM_SELECTED_RIGHT -- GadgetPushButton's
// existing right-click handling, which the touch layer's two-finger-tap
// already produces at whatever screen position it lands on): always
// assign/replace, regardless of occupancy -- the deliberate, explicit
// version of the same action.
//
// GeneralsX @feature Android port 02/08/2026 Rusted Warfare-style hold
// gesture (short hold = add, long hold = clear), with two-stage radial
// visual feedback: press and hold a group button, a clock-wipe overlay
// (GadgetButtonDrawClock -- the same mechanism this engine already has for
// production-progress buttons, see W3DPushButton.cpp) fills clockwise in
// green over GROUP_HOLD_ADD_MS; release once it completes (the whole
// button reads solid green at percent=100) to ADD the current selection
// into the existing group instead of replacing it. Keep holding and the
// wipe restarts in red over the next GROUP_HOLD_CLEAR_MS; release once
// THAT completes (solid red) to CLEAR the group entirely. Releasing before
// the green wipe completes is just the normal tap above -- unchanged.
//
// Both new actions are built entirely from the existing, already
// network-replicated MSG_META_CREATE_TEAM<n> message (never a new message
// type): ADD temporarily also selects the group's current live members
// (in addition to whatever the player already has selected) before firing
// the same force-assign path used by two-finger-tap, so the resulting
// squad is the union of old + new; CLEAR temporarily deselects everything
// before firing it, so the resulting squad is empty. The player's actual
// on-screen selection is snapshotted first and restored afterward, so
// this never has a lasting side effect on what's selected in the game.
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
#include "GameClient/Color.h"
#include "GameClient/Drawable.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GameClient.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/InGameUI.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/WinInstanceData.h"
#include "GameLogic/Squad.h"

#include <vector>

static NameKeyType s_buttonHandleID = NAMEKEY_INVALID;
static NameKeyType s_groupRowID = NAMEKEY_INVALID;
static NameKeyType s_buttonGroupID[10];
static Bool s_groupRowExpanded = FALSE;

// GeneralsX @feature Android port 02/08/2026 Hold-gesture timings. 0 means
// "not currently pressed" for s_pressStartMs, so a genuine press start is
// never allowed to land exactly on timeGetTime()==0 in practice.
static const UnsignedInt GROUP_HOLD_ADD_MS = 600;
static const UnsignedInt GROUP_HOLD_CLEAR_MS = 600;
static UnsignedInt s_pressStartMs[10] = { 0 };

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
// Snapshots the drawables currently selected on the battlefield so a
// temporary selection change (below) can be undone afterward.
static void snapshotSelection(std::vector<Drawable*> &out)
{
	out.clear();
	if (!TheGameClient) {
		return;
	}
	for (Drawable *draw = TheGameClient->getDrawableList(); draw != nullptr; draw = draw->getNextDrawable()) {
		if (draw->isSelected()) {
			out.push_back(draw);
		}
	}
}

//-------------------------------------------------------------------------------------------------
static void restoreSelection(const std::vector<Drawable*> &saved)
{
	if (!TheInGameUI) {
		return;
	}
	TheInGameUI->deselectAllDrawables();
	for (size_t i = 0; i < saved.size(); ++i) {
		TheInGameUI->selectDrawable(saved[i]);
	}
}

//-------------------------------------------------------------------------------------------------
// Merges the group's current live members into the on-screen selection,
// force-assigns (so the squad becomes old+new), then restores whatever was
// actually selected before -- net effect: units get ADDED to the existing
// group without touching what the player had selected.
static void handleGroupAdd(Int group)
{
	if (!ThePlayerList || !TheInGameUI) {
		return;
	}
	std::vector<Drawable*> saved;
	snapshotSelection(saved);

	Player *player = ThePlayerList->getLocalPlayer();
	if (player) {
		Squad *squad = player->getHotkeySquad(group);
		if (squad) {
			VecObjectPtr objs = squad->getLiveObjects();
			for (size_t i = 0; i < objs.size(); ++i) {
				if (objs[i] && objs[i]->getDrawable()) {
					TheInGameUI->selectDrawable(objs[i]->getDrawable());
				}
			}
		}
	}

	handleGroupCommand(group, TRUE);
	restoreSelection(saved);
}

//-------------------------------------------------------------------------------------------------
// Deselects everything, force-assigns (so the squad becomes empty), then
// restores whatever was actually selected before -- net effect: the group
// is CLEARED without touching what the player had selected.
static void handleGroupClear(Int group)
{
	if (!TheInGameUI) {
		return;
	}
	std::vector<Drawable*> saved;
	snapshotSelection(saved);

	TheInGameUI->deselectAllDrawables();
	handleGroupCommand(group, TRUE);
	restoreSelection(saved);
}

//-------------------------------------------------------------------------------------------------
// Called every frame while the panel exists. Polls each group button's own
// WIN_STATE_SELECTED bit (set/cleared by GadgetPushButtonInput on
// GWM_LEFT_DOWN/GWM_LEFT_UP) rather than intercepting raw window messages,
// so the existing tap/click handling in GadgetPushButton.cpp is completely
// untouched -- this only observes it. Draws the green/red clock-wipe while
// held; GroupPanelSystem's GBM_SELECTED handler reads s_pressStartMs to
// decide what the release actually meant.
static void updateHoldVisuals()
{
	if (!TheWindowManager) {
		return;
	}
	UnsignedInt now = timeGetTime();
	for (Int i = 0; i < 10; ++i) {
		GameWindow *button = TheWindowManager->winGetWindowFromId(nullptr, s_buttonGroupID[i]);
		if (!button) {
			continue;
		}

		Bool pressed = BitIsSet(button->winGetInstanceData()->getState(), WIN_STATE_SELECTED);
		if (!pressed) {
			s_pressStartMs[i] = 0;
			continue;
		}

		if (s_pressStartMs[i] == 0) {
			s_pressStartMs[i] = now;
		}

		UnsignedInt elapsed = now - s_pressStartMs[i];
		if (elapsed < GROUP_HOLD_ADD_MS) {
			Int percent = 1 + (Int)((elapsed * 99u) / GROUP_HOLD_ADD_MS);
			if (percent > 100) percent = 100;
			GadgetButtonDrawClock(button, percent, GameMakeColor(90, 220, 100, 255));
		} else {
			UnsignedInt redElapsed = elapsed - GROUP_HOLD_ADD_MS;
			Int percent = 1 + (Int)((redElapsed * 99u) / GROUP_HOLD_CLEAR_MS);
			if (percent > 100) percent = 100;
			GadgetButtonDrawClock(button, percent, GameMakeColor(220, 60, 50, 255));
		}
	}
}

// GeneralsX @feature Android port 03/08/2026 See GUICallbacks.h's comment.
// Not calibrated until visible=TRUE AND animationSettled=TRUE both hold
// on the same frame -- the bar can be "visible" (WIN_STATUS_HIDDEN clear)
// for the entire duration of its own slide-in animation, not just once it
// settles, so visible alone isn't enough: calibrating mid-slide locks in
// an offset measured against a transient, wildly wrong bar position (real
// device report: the panel flew off the top of the screen). Resets
// whenever visible=FALSE, so a bad calibration never has to survive past
// the bar's next hide/show cycle -- worst case it just costs one more
// calibration the next time the bar settles.
static Bool s_followOffsetCalibrated = FALSE;
static Int s_followOffsetX = 0;
static Int s_followOffsetY = 0;

void GroupPanelFollowControlBar(Int barScreenX, Int barScreenY, Bool visible, Bool animationSettled)
{
	if (!visible) {
		s_followOffsetCalibrated = FALSE;
		return;
	}
	if (!TheWindowManager) {
		return;
	}
	GameWindow *handle = TheWindowManager->winGetWindowFromId(nullptr, s_buttonHandleID);
	GameWindow *row = TheWindowManager->winGetWindowFromId(nullptr, s_groupRowID);
	if (!handle || !row) {
		return;
	}

	if (!s_followOffsetCalibrated) {
		if (!animationSettled) {
			return;
		}
		Int handleX, handleY;
		handle->winGetScreenPosition(&handleX, &handleY);
		s_followOffsetX = handleX - barScreenX;
		s_followOffsetY = handleY - barScreenY;
		s_followOffsetCalibrated = TRUE;
		return;
	}

	Int targetX = barScreenX + s_followOffsetX;
	Int targetY = barScreenY + s_followOffsetY;

	Int curX, curY;
	handle->winGetScreenPosition(&curX, &curY);
	Int dx = targetX - curX;
	Int dy = targetY - curY;
	if (dx == 0 && dy == 0) {
		return;
	}

	Int hx, hy;
	handle->winGetPosition(&hx, &hy);
	handle->winSetPosition(hx + dx, hy + dy);

	Int rx, ry;
	row->winGetPosition(&rx, &ry);
	row->winSetPosition(rx + dx, ry + dy);
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
	updateHoldVisuals();
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
			// GeneralsX @bugfix Android port 03/08/2026 Used to also try to
			// hide GroupRow from here (matching window->winGetWindowId()
			// against s_groupRowID), but for a WINDOWTYPE=USER window
			// winSetWindowId() only runs AFTER winCreate() has already
			// dispatched this very GWM_CREATE (see createGadget()'s "USER"
			// branch in GameWindowManagerScript.cpp), so winGetWindowId()
			// read from in here was always still the pre-assignment
			// default and never matched -- moved the actual hide to
			// ControlBar::init(), right after winCreateLayout() returns,
			// where every window in the tree is guaranteed to already have
			// its real id. cacheWidgetIDs() is still safe and useful to run
			// this early since it only hashes names, independent of
			// whether the matching windows exist yet.
			cacheWidgetIDs();
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
				UnsignedInt now = timeGetTime();
				UnsignedInt elapsed = (s_pressStartMs[group] != 0) ? (now - s_pressStartMs[group]) : 0;
				s_pressStartMs[group] = 0;

				if (elapsed >= GROUP_HOLD_ADD_MS + GROUP_HOLD_CLEAR_MS) {
					handleGroupClear(group);
				} else if (elapsed >= GROUP_HOLD_ADD_MS) {
					handleGroupAdd(group);
				} else {
					handleGroupCommand(group, FALSE);
				}
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
