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
// GeneralsX @feature Android port 18/10/2026 The army column, as a drawer.
//
// The column is realkot's (https://github.com/realkot/GeneralsX, r20, with the author's
// permission): control groups 0-9 in two columns, the army buttons (on screen, whole map, planes,
// helicopters, ground forces) and a handle. What this port adds is that it is a drawer (owner,
// 18/10/2026): in a game only the "+" handle shows, just above the left end of the control bar,
// which is the standard bar. A tap on it opens the column in the screen's left part and narrows the
// control bar to the rest (ControlBar::setArmyColumnOpen, GXUiScale::setControlBarLeftEdge); the
// handle, now "-" in the same place, closes it again. Every game starts with it closed.
//
// Groups: a tap selects the group, or assigns the selection to it while it is empty; a two-finger
// tap (right click) always assigns; holding a group button for GROUP_HOLD_MS (the red clock wipe)
// and releasing clears it, through MSG_CREATE_TEAM<n> with no objects (Player.cpp clears the squad
// first), so the battlefield selection is never touched. Long-pressing the handle shows or hides
// the army buttons; they start as the control scheme says (GXControlScheme::ARMY_BUTTONS).
//
// GroupPanel.wnd is a loose file (Window\GroupPanel.wnd) rather than part of the game's archives.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

#include "Common/GXControlScheme.h"
#include "Common/GXSafeArea.h"
#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Registry.h"
#include "GameClient/Color.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GameFont.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/GXUiScale.h"
#include "GameClient/Image.h"
#include "GameClient/InGameUI.h"
#include "GameClient/Shell.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/WinInstanceData.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Squad.h"
#include <map>

enum { ARMY_SCREEN, ARMY_MAP, ARMY_PLANES, ARMY_HELICOPTERS, ARMY_GROUND, ARMY_BUTTON_COUNT };

static NameKeyType s_parentID = NAMEKEY_INVALID;
static NameKeyType s_buttonHandleID = NAMEKEY_INVALID;
static NameKeyType s_groupRowID = NAMEKEY_INVALID;
static NameKeyType s_buttonGroupID[10];
static NameKeyType s_armyButtonID[ARMY_BUTTON_COUNT];

static Bool s_drawerOpen = FALSE;
static Bool s_armyShown = FALSE;
static Bool s_labelsSet = FALSE;
static UnsignedInt s_handlePressMs = 0;

static const UnsignedInt GROUP_HOLD_MS = 600;
static UnsignedInt s_pressStartMs[10] = { 0 };

static const UnsignedInt ARMY_GROUND_REPEAT_MS = 2000;
static UnsignedInt s_armyGroundPressMs = 0;

// How much of the screen's width the open column takes, safe-area inset included (owner: a little
// less than the first version's 4:3 bar left, so the control bar stays wider).
static const Real COLUMN_WIDTH_FRACTION = 0.095f;

//-------------------------------------------------------------------------------------------------
// Army selection, through the retail mass-selection path (selectAllUnitsByTypeAcross*), which
// sends the group to the logic as a drag-box does. Dozers, harvesters, structures and units that
// opt out of "select all" are never picked.
static Bool canSelectArmy()
{
	return TheMessageStream && TheInGameUI && TheInGameUI->getInputEnabled()
		&& TheGameLogic && TheGameLogic->isInInteractiveGame() && TheGameLogic->getFrame() > 0
		&& !TheGameLogic->isGamePaused() && !(TheShell && TheShell->isShellActive())
		&& ThePlayerList && ThePlayerList->getLocalPlayer()
		&& ThePlayerList->getLocalPlayer()->isPlayerActive();
}

static void selectArmy(Int which)
{
	if (!canSelectArmy()) {
		return;
	}
	KindOfMaskType required, excluded;
	excluded.set(KINDOF_DOZER);
	excluded.set(KINDOF_HARVESTER);
	excluded.set(KINDOF_IGNORES_SELECT_ALL);
	excluded.set(KINDOF_STRUCTURE);
	Bool acrossMap = TRUE;
	switch (which) {
		case ARMY_SCREEN:
			acrossMap = FALSE;
			break;
		case ARMY_MAP:
			break;
		case ARMY_PLANES:
		case ARMY_HELICOPTERS:
			// Helicopters are aircraft built at a helipad (the Comanche flies with JetAIUpdate too).
			// Transports stay: the armed Helix is one.
			required.set(KINDOF_AIRCRAFT);
			required.set(KINDOF_CAN_ATTACK);
			if (which == ARMY_HELICOPTERS) {
				required.set(KINDOF_PRODUCED_AT_HELIPAD);
			} else {
				excluded.set(KINDOF_PRODUCED_AT_HELIPAD);
			}
			break;
		case ARMY_GROUND:
		{
			// On screen; pressed again within two seconds, the whole map.
			required.set(KINDOF_CAN_ATTACK);
			excluded.set(KINDOF_AIRCRAFT);
			const UnsignedInt now = timeGetTime();
			acrossMap = s_armyGroundPressMs != 0 && now - s_armyGroundPressMs < ARMY_GROUND_REPEAT_MS;
			s_armyGroundPressMs = acrossMap ? 0 : now;
			break;
		}
		default:
			return;
	}
	TheInGameUI->deselectAllDrawables();
	if (acrossMap) {
		TheInGameUI->selectAllUnitsByTypeAcrossMap(required, excluded);
	} else {
		TheInGameUI->selectAllUnitsByTypeAcrossScreen(required, excluded);
	}
}

// A language pack may carry its own labels; otherwise the text language's short names.
static UnicodeString armyButtonLabel(Int index)
{
	static const char *const kLabelKeys[ARMY_BUTTON_COUNT] = {
		"GUI:ArmyScreenShort", "GUI:ArmyMapShort", "GUI:ArmyPlanesShort", "GUI:ArmyHelicoptersShort", "GUI:ArmyGround" };
	if (TheGameText) {
		Bool exists = FALSE;
		UnicodeString label = TheGameText->fetch(kLabelKeys[index], &exists);
		if (exists) {
			return label;
		}
	}
	struct Labels { const char *language; const wchar_t *text[ARMY_BUTTON_COUNT]; };
	static const Labels kLabels[] = {
		{ "english", { L"Screen", L"Map", L"Planes", L"Helis", L"Ground" } },
		{ "russian", { L"Экран", L"Карта", L"Самолёты", L"Вертол.", L"Наземные" } },
		{ "ukrainian", { L"Екран", L"Карта", L"Літаки", L"Гелікоп.", L"Наземні" } },
		{ "interslavic", { L"Ekran", L"Mapa", L"Aviony", L"Helikop.", L"Zemja" } },
		{ "arabic", { L"الشاشة", L"الخريطة", L"طائرات", L"مروحيات", L"برية" } },
		{ "brazilian", { L"Tela", L"Mapa", L"Aviões", L"Helicóp.", L"Terrest." } },
		{ "chinese", { L"屏幕", L"全图", L"战机", L"直升机", L"地面" } },
		{ "french", { L"Écran", L"Carte", L"Avions", L"Hélicos", L"Sol" } },
		{ "german", { L"Bild", L"Karte", L"Flieger", L"Helis", L"Boden" } },
		{ "korean", { L"화면", L"지도", L"전투기", L"헬기", L"지상군" } },
		{ "persian", { L"صفحه", L"نقشه", L"هواپیما", L"بالگرد", L"زمینی" } },
		{ "polish", { L"Ekran", L"Mapa", L"Samoloty", L"Śmigłow.", L"Lądowe" } },
		{ "spanish", { L"Pantalla", L"Mapa", L"Aviones", L"Helicóp.", L"Tierra" } },
	};
	const char *pack = getenv("GENERALSX_TEXT_LANGUAGE");
	AsciiString language = (pack != nullptr && pack[0] != '\0') ? AsciiString(pack) : GetRegistryLanguage();
	language.toLower();
	for (Int i = 0; i < (Int)(sizeof(kLabels) / sizeof(kLabels[0])); ++i) {
		if (language.compare(kLabels[i].language) == 0) {
			return UnicodeString(kLabels[i].text[index]);
		}
	}
	return UnicodeString(kLabels[0].text[index]);
}

//-------------------------------------------------------------------------------------------------
static void cacheWidgetIDs()
{
	if (s_buttonHandleID != NAMEKEY_INVALID) {
		return;
	}
	s_parentID = TheNameKeyGenerator->nameToKey("GroupPanel.wnd:GroupPanelParent");
	s_buttonHandleID = TheNameKeyGenerator->nameToKey("GroupPanel.wnd:ButtonHandle");
	s_groupRowID = TheNameKeyGenerator->nameToKey("GroupPanel.wnd:GroupRow");
	static const char *const kArmyNames[ARMY_BUTTON_COUNT] = {
		"GroupPanel.wnd:ArmyScreen", "GroupPanel.wnd:ArmyMap", "GroupPanel.wnd:ArmyPlanes",
		"GroupPanel.wnd:ArmyHelicopters", "GroupPanel.wnd:ArmyGround" };
	for (Int i = 0; i < ARMY_BUTTON_COUNT; ++i) {
		s_armyButtonID[i] = TheNameKeyGenerator->nameToKey(kArmyNames[i]);
	}
	char buf[40];
	for (Int i = 0; i < 10; ++i) {
		sprintf(buf, "GroupPanel.wnd:ButtonGroup%d", i);
		s_buttonGroupID[i] = TheNameKeyGenerator->nameToKey(buf);
	}
}

static GameWindow *panelWindow(NameKeyType id)
{
	return TheWindowManager ? TheWindowManager->winGetWindowFromId(nullptr, id) : nullptr;
}

static Int findGroupButtonIndex(Int controlID)
{
	for (Int i = 0; i < 10; ++i) {
		if (controlID == s_buttonGroupID[i]) {
			return i;
		}
	}
	return -1;
}

static Int findArmyButtonIndex(Int controlID)
{
	for (Int i = 0; i < ARMY_BUTTON_COUNT; ++i) {
		if (controlID == s_armyButtonID[i]) {
			return i;
		}
	}
	return -1;
}

static Bool isGroupEmpty(Int group)
{
	Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
	if (!player) {
		return TRUE;
	}
	Squad *squad = player->getHotkeySquad(group);
	return !squad || squad->getLiveObjects().empty();
}

static void handleGroupCommand(Int group, Bool forceAssign)
{
	if (!TheMessageStream || group < 0 || group > 9) {
		return;
	}
	const Bool assign = forceAssign || isGroupEmpty(group);
	TheMessageStream->appendMessage(assign
		? (GameMessage::Type)(GameMessage::MSG_META_CREATE_TEAM0 + group)
		: (GameMessage::Type)(GameMessage::MSG_META_SELECT_TEAM0 + group));
}

static void handleGroupClear(Int group)
{
	if (!TheMessageStream || group < 0 || group > 9) {
		return;
	}
	TheMessageStream->appendMessage((GameMessage::Type)(GameMessage::MSG_CREATE_TEAM0 + group));
}

// The red clock wipe on a group button held down; GBM_SELECTED reads how long it was held.
static void updateHoldVisuals()
{
	if (!s_drawerOpen) {
		return;
	}
	const UnsignedInt now = timeGetTime();
	for (Int i = 0; i < 10; ++i) {
		GameWindow *button = panelWindow(s_buttonGroupID[i]);
		if (!button) {
			continue;
		}
		if (!BitIsSet(button->winGetInstanceData()->getState(), WIN_STATE_SELECTED)) {
			s_pressStartMs[i] = 0;
			continue;
		}
		if (s_pressStartMs[i] == 0) {
			s_pressStartMs[i] = now;
		}
		Int percent = 1 + (Int)(((now - s_pressStartMs[i]) * 99u) / GROUP_HOLD_MS);
		if (percent > 100) {
			percent = 100;
		}
		GadgetButtonDrawClock(button, percent, GameMakeColor(220, 60, 50, 255));
	}
}

//-------------------------------------------------------------------------------------------------
// realkot's fitted labels: the layout's own font size (GroupPanel.wnd, scaled with the interface),
// made smaller until the text fits the button. Only when the size or the text changed.
struct PanelFontState
{
	GameFont *base = nullptr;
	Int width = 0, height = 0;
	UnicodeString text;
};
static std::map<GameWindow *, PanelFontState> s_panelFonts;

static void fitButtonFont(GameWindow *button, Int width, Int height)
{
	if (!button || !TheFontLibrary || !TheDisplayStringManager) {
		return;
	}
	PanelFontState &state = s_panelFonts[button];
	if (state.base == nullptr) {
		state.base = button->winGetFont();
	}
	const UnicodeString text = button->winGetText();
	if (state.base == nullptr || (state.width == width && state.height == height && state.text == text)) {
		return;
	}
	state.width = width;
	state.height = height;
	state.text = text;
	const Int smallest = max(8, (Int)(8 * min(TheDisplay->getWidth() / 800.0f, TheDisplay->getHeight() / 600.0f)));
	Int size = max(smallest, state.base->pointSize);
	GameFont *font = TheFontLibrary->getFont(state.base->nameString, size, state.base->bold);
	DisplayString *measure = TheDisplayStringManager->newDisplayString();
	if (measure) {
		measure->setText(text);
		measure->setWordWrap(0);
		while (font && size > smallest) {
			measure->setFont(font);
			Int w = 0, h = 0;
			measure->getSize(&w, &h);
			if (w <= width - 8 && h <= height - 4) {
				break;
			}
			--size;
			font = TheFontLibrary->getFont(state.base->nameString, size, state.base->bold);
		}
		TheDisplayStringManager->freeDisplayString(measure);
	}
	if (font) {
		button->winSetFont(font);
	}
}

// GeneralsX @feature Android port 18/10/2026 The button face drawn at display init (GXPanelButton*,
// W3DTouchButtonImages.cpp) instead of the .wnd's flat rectangles (owner, a30: "old-fashioned,
// unnatural buttons for a game"). The image draw path stretches it over the button and still draws
// the text and the hold clock.
static void applyButtonFace(GameWindow *button)
{
	if (!button || !TheMappedImageCollection || !TheWindowManager) {
		return;
	}
	const Image *rest = TheMappedImageCollection->findImageByName("GXPanelButton");
	const Image *hilite = TheMappedImageCollection->findImageByName("GXPanelButtonHilite");
	const Image *pressed = TheMappedImageCollection->findImageByName("GXPanelButtonPressed");
	GameWinDrawFunc draw = TheWindowManager->getPushButtonImageDrawFunc();
	if (!rest || !hilite || !pressed || !draw) {
		return;
	}
	GadgetButtonSetEnabledImage(button, rest);
	GadgetButtonSetEnabledSelectedImage(button, pressed);
	GadgetButtonSetHiliteImage(button, hilite);
	GadgetButtonSetHiliteSelectedImage(button, pressed);
	GadgetButtonSetDisabledImage(button, rest);
	button->winSetStatus(WIN_STATUS_IMAGE);
	button->winSetDrawFunc(draw);
}

static void applyButtonFaces()
{
	applyButtonFace(panelWindow(s_buttonHandleID));
	for (Int i = 0; i < 10; ++i) {
		applyButtonFace(panelWindow(s_buttonGroupID[i]));
	}
	for (Int i = 0; i < ARMY_BUTTON_COUNT; ++i) {
		applyButtonFace(panelWindow(s_armyButtonID[i]));
	}
}

static void placeButton(GameWindow *button, Int screenX, Int screenY, Int originX, Int originY, Int width, Int height)
{
	if (!button) {
		return;
	}
	button->winSetPosition(screenX - originX, screenY - originY);
	button->winSetSize(width, height);
	fitButtonFont(button, width, height);
}

// The panel's root covers exactly what is shown: the window manager hit-tests by top-level window,
// and an empty part of a larger root would keep the control bar's own buttons from a press
// (issue #33).
static void fitParentToVisibleContent()
{
	GameWindow *parent = panelWindow(s_parentID);
	if (!parent) {
		return;
	}
	Int loX = 0, loY = 0, hiX = 0, hiY = 0;
	Bool any = FALSE;
	for (GameWindow *child = parent->winGetChild(); child; child = child->winGetNext()) {
		if (child->winIsHidden()) {
			continue;
		}
		Int x, y, w, h;
		child->winGetScreenPosition(&x, &y);
		child->winGetSize(&w, &h);
		if (!any) {
			loX = x; loY = y; hiX = x + w; hiY = y + h;
			any = TRUE;
		} else {
			loX = min(loX, x); loY = min(loY, y); hiX = max(hiX, x + w); hiY = max(hiY, y + h);
		}
	}
	if (!any) {
		return;
	}
	Int px, py, pw, ph;
	parent->winGetScreenPosition(&px, &py);
	parent->winGetSize(&pw, &ph);
	if (px == loX && py == loY && pw == hiX - loX && ph == hiY - loY) {
		return;
	}
	// The children are placed relative to the root: move them back by however far the root moves.
	const Int dx = loX - px;
	const Int dy = loY - py;
	for (GameWindow *child = parent->winGetChild(); child; child = child->winGetNext()) {
		Int cx, cy;
		child->winGetPosition(&cx, &cy);
		child->winSetPosition(cx - dx, cy - dy);
	}
	Int ox, oy;
	parent->winGetPosition(&ox, &oy);
	parent->winSetPosition(ox + dx, oy + dy);
	parent->winSetSize(hiX - loX, hiY - loY);
}

// The left edge the control bar gets while the column is open, as a fraction of the screen's
// width; 0 on a screen too narrow for it (the column then opens above the bar instead).
static Real columnLeftEdge()
{
	if (!TheDisplay || TheDisplay->getHeight() <= 0
		|| (Real)TheDisplay->getWidth() / (Real)TheDisplay->getHeight() < 1.5f) {
		return 0.0f;
	}
	const Real resolution = max(0.25f, min(TheDisplay->getWidth() / 800.0f, TheDisplay->getHeight() / 600.0f));
	const Real margin = max(4.0f, 6.0f * resolution);
	return ((Real)GXSafeArea::leftPx() + 2.0f * margin) / (Real)TheDisplay->getWidth() + COLUMN_WIDTH_FRACTION;
}

static void setDrawer(Bool open)
{
	if (s_drawerOpen == open) {
		return;
	}
	s_drawerOpen = open;
	if (TheControlBar) {
		TheControlBar->setArmyColumnOpen(open, open ? columnLeftEdge() : 0.0f);
	}
	fprintf(stderr, "[GX-Controls] army column %s\n", open ? "opened" : "closed");
	fflush(stderr);
}

//-------------------------------------------------------------------------------------------------
void GroupPanelCalibrateFollowOffset(Int barScreenX, Int barScreenY)
{
	fprintf(stderr, "[GX-UISCALE] group panel: control bar at %d,%d\n", barScreenX, barScreenY);
}

void GroupPanelCloseDrawer()
{
	setDrawer(FALSE);
}

// Every frame the bar is shown. realkot's arrangement: the groups two to a row, the handle, the
// army buttons one to a row, each as wide as the column, rows 34 units high as far as the room
// allows. The handle never moves (owner, a30: "+" and "-" in one place, to open and close without
// looking): just above the top of the control bar at the screen's left, open or closed. Open, what
// sits beside the bar below it is the army buttons -- or the groups when the army buttons are
// hidden -- and the groups otherwise stack above the handle.
void GroupPanelFollowControlBar(Int barScreenX, Int barScreenY, Bool visible)
{
	if (!visible || !TheWindowManager || !TheDisplay) {
		return;
	}
	GameWindow *parent = panelWindow(s_parentID);
	GameWindow *handle = panelWindow(s_buttonHandleID);
	GameWindow *row = panelWindow(s_groupRowID);
	if (!parent || !handle || !row) {
		return;
	}
	(void)barScreenX;
	const Int width = TheDisplay->getWidth();
	const Int height = TheDisplay->getHeight();
	const Real resolution = max(0.25f, min(width / 800.0f, height / 600.0f));
	const Real unit = resolution * max(1.0f, GXUiScale::forLayout("ControlBar.wnd").fontK);
	const Int margin = max(4, (Int)(6 * resolution));
	const Int gap = max(2, (Int)(4 * resolution));
	const Int x0 = GXSafeArea::leftPx() + margin;
	const Int bottom = height - GXSafeArea::bottomPx() - margin;

	// The column's width: up to where the bar starts while it is open, the same in both states.
	const Real leftEdge = columnLeftEdge();
	const Int columnWidth = leftEdge > 0.0f
		? max((Int)(60 * resolution), (Int)(leftEdge * width) - margin - x0)
		: (Int)(150 * resolution);
	const Int cellWidth = max(1, (columnWidth - gap) / 2);

	// One row height for everything: 34 units, or what fits five rows beside the bar.
	const Int besideRoom = bottom - barScreenY;
	Int rowHeight = (Int)(34 * unit);
	if (leftEdge > 0.0f && besideRoom > 0) {
		rowHeight = min(rowHeight, (besideRoom - 4 * gap) / 5);
	}
	rowHeight = max(rowHeight, (Int)(20 * resolution));

	const Int handleY = barScreenY - rowHeight - gap;
	Int px, py;
	parent->winGetScreenPosition(&px, &py);
	placeButton(handle, x0, handleY, px, py, columnWidth, rowHeight);

	if (!s_drawerOpen) {
		row->winHide(TRUE);
		for (Int i = 0; i < ARMY_BUTTON_COUNT; ++i) {
			if (GameWindow *button = panelWindow(s_armyButtonID[i])) {
				button->winHide(TRUE);
			}
		}
		fitParentToVisibleContent();
		return;
	}

	// Beside the bar (or, without room beside it, above the handle): the army buttons, or the
	// groups when the army buttons are hidden.
	const Bool besideBar = leftEdge > 0.0f;
	const Int groupsHeight = 5 * rowHeight + 4 * gap;
	const Int armyHeight = 5 * rowHeight + 4 * gap;
	Int groupsTop, armyTop;
	if (besideBar) {
		armyTop = barScreenY;
		groupsTop = s_armyShown ? handleY - gap - groupsHeight : barScreenY;
	} else {
		armyTop = handleY - gap - armyHeight;
		groupsTop = (s_armyShown ? armyTop : handleY) - gap - groupsHeight;
	}

	row->winHide(FALSE);
	row->winSetPosition(x0 - px, groupsTop - py);
	row->winSetSize(columnWidth, groupsHeight);
	for (Int i = 0; i < 10; ++i) {
		GameWindow *button = panelWindow(s_buttonGroupID[i]);
		if (!button) {
			continue;
		}
		button->winSetPosition((i % 2) * (cellWidth + gap), (i / 2) * (rowHeight + gap));
		button->winSetSize(cellWidth, rowHeight);
		fitButtonFont(button, cellWidth, rowHeight);
	}

	for (Int i = 0; i < ARMY_BUTTON_COUNT; ++i) {
		GameWindow *button = panelWindow(s_armyButtonID[i]);
		if (!button) {
			continue;
		}
		button->winHide(!s_armyShown);
		if (s_armyShown) {
			placeButton(button, x0, armyTop + i * (rowHeight + gap), px, py, columnWidth, rowHeight);
		}
	}
	fitParentToVisibleContent();
}

//-------------------------------------------------------------------------------------------------
void GroupPanelInit(WindowLayout *layout, void *userData)
{
	(void)layout;
	(void)userData;
	s_drawerOpen = FALSE;
	s_armyShown = GXControlScheme::enabled(GXControlScheme::ARMY_BUTTONS);
	s_handlePressMs = 0;
	s_armyGroundPressMs = 0;
	s_panelFonts.clear();
}

void GroupPanelUpdate(WindowLayout *layout, void *userData)
{
	(void)layout;
	(void)userData;
	if (!TheWindowManager) {
		return;
	}
	updateHoldVisuals();
	GameWindow *handle = panelWindow(s_buttonHandleID);
	if (handle && BitIsSet(handle->winGetInstanceData()->getState(), WIN_STATE_SELECTED)) {
		if (s_handlePressMs == 0) {
			s_handlePressMs = timeGetTime();
		}
	} else {
		s_handlePressMs = 0;
	}

	const Bool enabled = canSelectArmy();
	for (Int i = 0; i < ARMY_BUTTON_COUNT; ++i) {
		if (GameWindow *button = panelWindow(s_armyButtonID[i])) {
			button->winEnable(enabled);
		}
	}
	if (handle) {
		const UnicodeString mark(s_drawerOpen ? L"\x2212" : L"+");
		if (handle->winGetText() != mark) {
			GadgetButtonSetText(handle, mark);
		}
	}
	if (s_labelsSet || !TheGameText) {
		return;
	}
	applyButtonFaces();
	for (Int i = 0; i < ARMY_BUTTON_COUNT; ++i) {
		if (GameWindow *button = panelWindow(s_armyButtonID[i])) {
			GadgetButtonSetText(button, armyButtonLabel(i));
		}
	}
	// Group digits are literal text, not localization keys.
	for (Int group = 0; group < 10; ++group) {
		if (GameWindow *button = panelWindow(s_buttonGroupID[group])) {
			UnicodeString digit;
			digit.format(L"%d", group);
			GadgetButtonSetText(button, digit);
		}
	}
	s_labelsSet = TRUE;
}

void GroupPanelShutdown(WindowLayout *layout, void *userData)
{
	(void)userData;
	s_panelFonts.clear();
	s_handlePressMs = 0;
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
			cacheWidgetIDs();
			break;

		case GWM_DESTROY:
			s_labelsSet = FALSE;
			s_panelFonts.clear();
			break;

		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			const Int controlID = control->winGetWindowId();
			if (controlID == (Int)s_buttonHandleID) {
				// Held: the army buttons in or out of the open column. Tapped: open or close it.
				if (s_drawerOpen && s_handlePressMs != 0 && timeGetTime() - s_handlePressMs >= GROUP_HOLD_MS) {
					s_armyShown = !s_armyShown;
				} else {
					setDrawer(!s_drawerOpen);
				}
				s_handlePressMs = 0;
				return MSG_HANDLED;
			}
			const Int army = findArmyButtonIndex(controlID);
			if (army >= 0) {
				selectArmy(army);
				return MSG_HANDLED;
			}
			const Int group = findGroupButtonIndex(controlID);
			if (group >= 0) {
				const UnsignedInt elapsed = s_pressStartMs[group] != 0 ? timeGetTime() - s_pressStartMs[group] : 0;
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
			const Int controlID = control->winGetWindowId();
			if (controlID == (Int)s_buttonHandleID) {
				if (s_drawerOpen) {
					s_armyShown = !s_armyShown;
				}
				return MSG_HANDLED;
			}
			const Int group = findGroupButtonIndex(controlID);
			if (group >= 0) {
				handleGroupCommand(group, TRUE);
				return MSG_HANDLED;
			}
			break;
		}

	}

	return MSG_IGNORED;
}
