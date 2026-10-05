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
// GeneralsX @feature Android port 02/08/2026, simplified 03/08/2026 Hold
// gesture: press and hold a group button, a clock-wipe overlay
// (GadgetButtonDrawClock -- the same mechanism this engine already has for
// production-progress buttons, see W3DPushButton.cpp) fills clockwise in
// red over GROUP_HOLD_MS; release once it completes (the whole button
// reads solid red at percent=100) to CLEAR the group entirely. Releasing
// before the wipe completes is just the normal tap above -- unchanged.
// (A first version also had a shorter green "add to group" stage before
// the red one, dropped once real use showed it was redundant: a plain tap
// on a non-empty group already assigns/replaces it with the current
// selection, which covers the same "grow this group" need a two-finger-
// tap/force-assign already handles explicitly.)
//
// CLEAR is built from the existing, already network-replicated
// MSG_CREATE_TEAM<n> message (never a new message type), sent directly
// with zero object-ID arguments -- Player::processCreateTeamGameMessage()
// (Player.cpp) always clears the hotkey squad first regardless, so an
// argument-less message just clears it. This never touches the player's
// on-screen battlefield selection at all (an earlier version tried
// building the empty team via the SELECTION-driven MSG_META_CREATE_TEAM<n>
// instead -- deselect everything, fire it, restore afterward -- which hit
// a string of bugs: the restore raced the message translator, then broke
// move orders for the restored units by only touching the client-side
// selection list and not the player's real AIGroup, then still left the
// selection visibly flickering/disappearing for the reselect's deferred
// frame; sending the concrete message directly sidesteps all of it).
//
// None of the actual group logic (assignment, recall, double-press-to-
// recenter-camera) is reimplemented here -- SelectionXlat.cpp's
// onMetaCreateTeam()/onMetaSelectTeam() already do the tap/two-finger-tap
// paths; this only needs to feed the same MSG_META_CREATE_TEAM<n>/
// MSG_META_SELECT_TEAM<n>/MSG_CREATE_TEAM<n> a keyboard shortcut or a
// real click-based force-assign would have.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

#include "Common/GXSafeArea.h"
#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameClient/Color.h"
#include "GameClient/Display.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/WinInstanceData.h"
#include "GameLogic/Squad.h"

static NameKeyType s_parentID = NAMEKEY_INVALID;
static NameKeyType s_buttonHandleID = NAMEKEY_INVALID;
// GeneralsX @feature Android port 05/10/2026 The space bar for a finger (issue #25): "!" beside the
// handle centres the view on the last radar event, the way MSG_META_VIEW_LAST_RADAR_EVENT does --
// a camera button, so it sits with the camera-side controls here rather than on a unit's bar, and
// is there with nothing selected. Optional: an older GroupPanel.wnd simply has no such window.
static NameKeyType s_buttonLastEventID = NAMEKEY_INVALID;
static NameKeyType s_groupRowID = NAMEKEY_INVALID;
static NameKeyType s_buttonGroupID[10];
static Bool s_groupRowExpanded = FALSE;

// GeneralsX @feature Android port 02/08/2026 Hold-gesture timing. 0 means
// "not currently pressed" for s_pressStartMs, so a genuine press start is
// never allowed to land exactly on timeGetTime()==0 in practice.
static const UnsignedInt GROUP_HOLD_MS = 600;
static UnsignedInt s_pressStartMs[10] = { 0 };

//-------------------------------------------------------------------------------------------------
static void cacheWidgetIDs()
{
	if (s_buttonHandleID != NAMEKEY_INVALID) {
		return; // already cached -- GWM_CREATE fires once per window in this layout
	}
	s_parentID = TheNameKeyGenerator->nameToKey("GroupPanel.wnd:GroupPanelParent");
	s_buttonHandleID = TheNameKeyGenerator->nameToKey("GroupPanel.wnd:ButtonHandle");
	s_buttonLastEventID = TheNameKeyGenerator->nameToKey("GroupPanel.wnd:ButtonLastEvent");
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
// GeneralsX @bugfix Android port 04/08/2026 Two earlier versions of this
// built the empty team via MSG_META_CREATE_TEAM<n>, which has no
// arguments of its own -- SelectionTranslator::onMetaCreateTeam()
// (SelectionXlat.cpp) always builds it from whatever's CURRENTLY selected
// on the battlefield at translation time, so getting an EMPTY team out of
// it meant deselecting everything first, then restoring the player's
// actual selection afterward. That restore was a real source of bugs:
// doing it synchronously raced the translator (it hadn't run yet, so it
// saw the restored selection, not the empty one -- team never actually
// got cleared); deferring it a frame fixed that but reportedly used
// TheInGameUI->selectDrawable() to reselect, which never rebuilds the
// player's real (network-replicated) AIGroup, only the client-local
// selection list, so the "restored" units looked selected but weren't
// actually commandable; fixing THAT to send MSG_CREATE_SELECTED_GROUP
// instead fixed commandability, but the player's selection still
// visibly disappeared for a frame (and, per a later report, sometimes
// longer) between the deselect and the deferred restore.
//
// All of that complexity existed only to build an EMPTY team through a
// message that has no way to say "empty" directly. The underlying
// concrete message, MSG_CREATE_TEAM<n>, doesn't have that limitation --
// Player::processCreateTeamGameMessage() (Player.cpp) always clears the
// hotkey squad first, then adds whatever object-ID arguments the message
// carries; sent with ZERO of them, it just clears. Sending that directly
// means CLEAR never has to touch the player's on-screen selection at
// all -- nothing to snapshot, nothing to restore, nothing that can
// flicker or desync.
static void handleGroupClear(Int group)
{
	if (!TheMessageStream || group < 0 || group > 9) {
		return;
	}
	TheMessageStream->appendMessage((GameMessage::Type)(GameMessage::MSG_CREATE_TEAM0 + group));
}

//-------------------------------------------------------------------------------------------------
// Called every frame while the panel exists. Polls each group button's own
// WIN_STATE_SELECTED bit (set/cleared by GadgetPushButtonInput on
// GWM_LEFT_DOWN/GWM_LEFT_UP) rather than intercepting raw window messages,
// so the existing tap/click handling in GadgetPushButton.cpp is completely
// untouched -- this only observes it. Draws the red clock-wipe while held;
// GroupPanelSystem's GBM_SELECTED handler reads s_pressStartMs to decide
// what the release actually meant.
static void updateHoldVisuals()
{
	// GeneralsX @bugfix Android port 06/08/2026 This ran unconditionally
	// every frame of every match regardless of whether the group row was
	// even expanded -- 10 recursive TheWindowManager->winGetWindowFromId()
	// tree walks/frame for buttons that are hidden (and therefore
	// unpressable, so WIN_STATE_SELECTED could never be set on them) the
	// vast majority of the time, since s_groupRowExpanded defaults FALSE
	// and the row is only shown while the spoiler handle is held open.
	// Skip the whole thing when the row is collapsed, matching the same
	// s_groupRowExpanded gate the row's own winHide() already uses.
	if (!s_groupRowExpanded) {
		return;
	}
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
		Int percent = 1 + (Int)((elapsed * 99u) / GROUP_HOLD_MS);
		if (percent > 100) percent = 100;
		GadgetButtonDrawClock(button, percent, GameMakeColor(220, 60, 50, 255));
	}
}

// GeneralsX @feature Android port 03/08/2026 See GUICallbacks.h's comment.
// The offset is captured exactly ONCE, at ControlBar::init() time, before
// the real bar has ever run a single show/hide slide animation -- at that
// moment both this panel's handle and the real ControlBarParent are still
// sitting at their plain authored .wnd resting positions, so the
// screen-space delta between them is the true, permanent spatial
// relationship, independent of any animation timing. Earlier attempts
// calibrated lazily at runtime (once "visible", or once "visible AND the
// bar's slide-in animation had settled") -- both were timing-dependent:
// the first could sample a transient mid-slide bar position (real device
// report: the panel flew off the top of the screen); the second was
// correct but only ever moved the panel AFTER the bar's animation
// finished, so the panel just sat static and "already in place" for the
// whole slide instead of sliding in together with the bar, which is the
// actual "become an inseparable part of the bar" behavior asked for.
// Since the offset never needs to change, tracking by it unconditionally
// every frame (see GroupPanelFollowControlBar below) makes the panel
// mirror the bar's live screen position on every single frame, including
// every frame of its slide-in/out animation.
// GeneralsX @bugfix Android port 04/10/2026 Issue #33: the panel's root window must cover exactly
// what is shown -- the handle, plus the row while it is open -- and nothing else.
//
// The window manager hit-tests by top-level window: the first one whose rectangle contains the
// point is searched for a child, and the search stops there (getWindowUnderCursor(),
// findWindowUnderMouse()). GroupPanelParent is authored as one rectangle around the handle and the
// whole row, and it is NOINPUT, so over its empty parts the answer was "no window" -- and the
// windows underneath it, the command bar's own buttons (the general's promotions among them), were
// never reached. Worse, the cutout margin (02/10/2026) moved the handle and the row right but not
// their parent, so the "9" button ended up outside the rectangle that is searched and could not be
// pressed at all. Fitting the root to its visible children after every move and every open/close
// fixes both: a point outside the buttons is outside the panel.
static void fitParentToVisibleContent()
{
	if (!TheWindowManager) {
		return;
	}
	GameWindow *parent = TheWindowManager->winGetWindowFromId(nullptr, s_parentID);
	GameWindow *handle = TheWindowManager->winGetWindowFromId(nullptr, s_buttonHandleID);
	GameWindow *row = TheWindowManager->winGetWindowFromId(nullptr, s_groupRowID);
	if (!parent || !handle || !row) {
		return;
	}
	Int hx, hy, hw, hh;
	handle->winGetScreenPosition(&hx, &hy);
	handle->winGetSize(&hw, &hh);
	Int loX = hx, loY = hy, hiX = hx + hw, hiY = hy + hh;
	GameWindow *lastEvent = TheWindowManager->winGetWindowFromId(nullptr, s_buttonLastEventID);
	if (lastEvent && !lastEvent->winIsHidden()) {
		Int ex, ey, ew, eh;
		lastEvent->winGetScreenPosition(&ex, &ey);
		lastEvent->winGetSize(&ew, &eh);
		loX = min(loX, ex);
		loY = min(loY, ey);
		hiX = max(hiX, ex + ew);
		hiY = max(hiY, ey + eh);
	}
	if (!row->winIsHidden()) {
		Int rx, ry, rw, rh;
		row->winGetScreenPosition(&rx, &ry);
		row->winGetSize(&rw, &rh);
		loX = min(loX, rx);
		loY = min(loY, ry);
		hiX = max(hiX, rx + rw);
		hiY = max(hiY, ry + rh);
	}
	Int px, py, pw, ph;
	parent->winGetScreenPosition(&px, &py);
	parent->winGetSize(&pw, &ph);
	if (px == loX && py == loY && pw == hiX - loX && ph == hiY - loY) {
		return;
	}
	// The children are placed relative to the root: move them back by however far the root moves,
	// so nothing on screen shifts.
	const Int dx = loX - px;
	const Int dy = loY - py;
	Int cx, cy;
	handle->winGetPosition(&cx, &cy);
	handle->winSetPosition(cx - dx, cy - dy);
	row->winGetPosition(&cx, &cy);
	row->winSetPosition(cx - dx, cy - dy);
	if (lastEvent) {
		lastEvent->winGetPosition(&cx, &cy);
		lastEvent->winSetPosition(cx - dx, cy - dy);
	}
	Int ox, oy;
	parent->winGetPosition(&ox, &oy);
	parent->winSetPosition(ox + dx, oy + dy);
	parent->winSetSize(hiX - loX, hiY - loY);
}

static Bool s_followOffsetCalibrated = FALSE;
static Int s_followOffsetX = 0;
static Int s_followOffsetY = 0;

void GroupPanelCalibrateFollowOffset(Int barScreenX, Int barScreenY)
{
	if (!TheWindowManager) {
		return;
	}
	GameWindow *handle = TheWindowManager->winGetWindowFromId(nullptr, s_buttonHandleID);
	if (!handle) {
		return;
	}
	Int handleX, handleY;
	handle->winGetScreenPosition(&handleX, &handleY);
	s_followOffsetX = handleX - barScreenX;
	s_followOffsetY = handleY - barScreenY;
	s_followOffsetCalibrated = TRUE;
	// GeneralsX @feature Android port 02/10/2026 Where the row's handle and the control bar are when
	// the offset is taken, to check the interface scale moved both (GXUiScale.h).
	fprintf(stderr, "[GX-UISCALE] group panel handle at %d,%d, control bar at %d,%d\n",
		handleX, handleY, barScreenX, barScreenY);
}

void GroupPanelFollowControlBar(Int barScreenX, Int barScreenY, Bool visible)
{
	if (!visible || !s_followOffsetCalibrated) {
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

	// GeneralsX @tweak Android port 02/10/2026 Clear of the display cutout: at the bar's left edge
	// the camera hole covered the handle and the "0" button (owner's photo). The launcher's safe
	// inset (GXSafeArea.h) plus a little.
	Int targetX = barScreenX + s_followOffsetX + GXSafeArea::leftPx() + TheDisplay->getWidth() / 100;
	Int targetY = barScreenY + s_followOffsetY;

	Int curX, curY;
	handle->winGetScreenPosition(&curX, &curY);
	Int dx = targetX - curX;
	Int dy = targetY - curY;
	if (dx != 0 || dy != 0) {
		Int hx, hy;
		handle->winGetPosition(&hx, &hy);
		handle->winSetPosition(hx + dx, hy + dy);

		Int rx, ry;
		row->winGetPosition(&rx, &ry);
		row->winSetPosition(rx + dx, ry + dy);

		GameWindow *lastEvent = TheWindowManager->winGetWindowFromId(nullptr, s_buttonLastEventID);
		if (lastEvent) {
			Int ex, ey;
			lastEvent->winGetPosition(&ex, &ey);
			lastEvent->winSetPosition(ex + dx, ey + dy);
		}
	}
	fitParentToVisibleContent();
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
				fitParentToVisibleContent();
				return MSG_HANDLED;
			}

			if (controlID == s_buttonLastEventID) {
				TheMessageStream->appendMessage(GameMessage::MSG_META_VIEW_LAST_RADAR_EVENT);
				return MSG_HANDLED;
			}

			Int group = findGroupButtonIndex(controlID);
			if (group >= 0) {
				UnsignedInt now = timeGetTime();
				UnsignedInt elapsed = (s_pressStartMs[group] != 0) ? (now - s_pressStartMs[group]) : 0;
				s_pressStartMs[group] = 0;

				if (elapsed >= GROUP_HOLD_MS) {
					handleGroupClear(group);
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
