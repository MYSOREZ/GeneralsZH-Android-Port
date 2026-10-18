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
// which is the standard bar. A tap on it opens the column at the screen's left, no taller than the
// bar, and narrows the control bar to the rest (ControlBar::setArmyColumnOpen,
// GXUiScale::setControlBarLeftEdge); "-" in the column's first row closes it, ">" beside it widens it
// with the army buttons. Every game starts with it closed.
//
// Groups: a tap selects the group, or assigns the selection to it while it is empty; a two-finger
// tap (right click) always assigns; holding a group button for GROUP_HOLD_MS (the red clock wipe)
// and releasing clears it, through MSG_CREATE_TEAM<n> with no objects (Player.cpp clears the squad
// first), so the battlefield selection is never touched. The army buttons start shown as the control
// scheme says (GXControlScheme::ARMY_BUTTONS).
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
static NameKeyType s_buttonArmyToggleID = NAMEKEY_INVALID;
static NameKeyType s_groupRowID = NAMEKEY_INVALID;
static NameKeyType s_buttonGroupID[10];
static NameKeyType s_armyButtonID[ARMY_BUTTON_COUNT];

static Bool s_drawerOpen = FALSE;
static Bool s_armyShown = FALSE;
static Bool s_labelsSet = FALSE;

static const UnsignedInt GROUP_HOLD_MS = 600;
static UnsignedInt s_pressStartMs[10] = { 0 };

static const UnsignedInt ARMY_GROUND_REPEAT_MS = 2000;
static UnsignedInt s_armyGroundPressMs = 0;

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
	s_buttonArmyToggleID = TheNameKeyGenerator->nameToKey("GroupPanel.wnd:ButtonArmyToggle");
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
// realkot's fitted labels, from a size that suits the button (about half its height, or the layout's
// own if larger) down until the text fits it. Only when the size or the text changed.
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
	Int size = max(smallest, max(state.base->pointSize, (Int)(height * 0.55f)));
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

static void gxPanelBackdropDraw(GameWindow *window, WinInstanceData *instData);

static void applyButtonFaces()
{
	if (GameWindow *parent = panelWindow(s_parentID)) {
		parent->winSetDrawFunc(gxPanelBackdropDraw);
	}
	applyButtonFace(panelWindow(s_buttonHandleID));
	applyButtonFace(panelWindow(s_buttonArmyToggleID));
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
// GeneralsX @feature Android port 18/10/2026 The open column's backing plate (owner, a30: the
// battlefield should not show behind the buttons or around them): from the screen's left edge to the
// control bar, from just above the column to the screen's foot, opaque dark steel with the gold
// trim of the game's frames along its top and right. The panel's root window covers it and takes
// the touches that land between buttons, so none falls through to the battlefield.
static Bool s_backdropValid = FALSE;
static Int s_backdropX = 0, s_backdropY = 0, s_backdropW = 0, s_backdropH = 0;

static void gxPanelBackdropDraw(GameWindow *window, WinInstanceData *instData)
{
	(void)window;
	(void)instData;
	if (!s_drawerOpen || !s_backdropValid || !TheDisplay) {
		return;
	}
	const Int trim = max(2, (Int)TheDisplay->getHeight() / 400);
	TheDisplay->drawFillRect(s_backdropX, s_backdropY, s_backdropW, s_backdropH, GameMakeColor(14, 18, 24, 255));
	// a lighter band under the trim, as the bar's own steel has
	TheDisplay->drawFillRect(s_backdropX, s_backdropY + trim, s_backdropW - trim, trim * 3, GameMakeColor(30, 37, 46, 255));
	TheDisplay->drawFillRect(s_backdropX, s_backdropY, s_backdropW, trim, GameMakeColor(196, 166, 104, 255));
	TheDisplay->drawFillRect(s_backdropX + s_backdropW - trim, s_backdropY, trim, s_backdropH, GameMakeColor(150, 126, 78, 255));
}

static void fitParentToVisibleContent()
{
	GameWindow *parent = panelWindow(s_parentID);
	if (!parent) {
		return;
	}
	Int loX = 0, loY = 0, hiX = 0, hiY = 0;
	Bool any = FALSE;
	if (s_drawerOpen && s_backdropValid) {
		loX = s_backdropX; loY = s_backdropY; hiX = s_backdropX + s_backdropW; hiY = s_backdropY + s_backdropH;
		any = TRUE;
	}
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

// GeneralsX @tweak Android port 18/10/2026 The column's geometry (owner, a31: the column was taller
// than the control bar, its buttons small and pushed right of an empty strip, "+" and "-" in one
// place forced it higher, and it took more room than a handful of buttons needs):
//   - it never rises above the control bar's top: six rows from the bar's top to the screen's foot,
//     the first holding "-" and the army switch, the other five the groups, two to a row;
//   - it starts at the screen's left edge and its buttons fill its plate;
//   - narrow by default, groups only; the army switch (">") widens it by a second column with the
//     army buttons, the control bar moving right by as much, and ("<") narrows it again;
//   - closed, "+" sits above the bar's left end, one cell wide.
struct ColumnGeometry
{
	Int margin, gap, x0, rowHeight, cellWidth, columnWidth;
	Int narrowRight, wideRight;	// where the control bar starts, open narrow / wide
};

static Int s_lastBarTop = 0;

static ColumnGeometry columnGeometry(Int barTop)
{
	ColumnGeometry g;
	const Int width = TheDisplay->getWidth();
	const Int height = TheDisplay->getHeight();
	const Real resolution = max(0.25f, min(width / 800.0f, height / 600.0f));
	g.margin = max(4, (Int)(6 * resolution));
	g.gap = max(2, (Int)(4 * resolution));
	g.x0 = g.margin;
	const Int bottom = height - GXSafeArea::bottomPx() - g.margin;
	if (barTop <= 0) {
		barTop = (Int)(height * 0.6f);
	}
	g.rowHeight = max((Int)(20 * resolution), (bottom - barTop - 5 * g.gap) / 6);
	g.cellWidth = (Int)(g.rowHeight * 1.3f);
	g.columnWidth = 2 * g.cellWidth + g.gap;
	g.narrowRight = g.x0 + g.columnWidth + g.margin;
	g.wideRight = g.x0 + 2 * g.columnWidth + g.gap + g.margin;
	return g;
}

// The left edge the control bar gets while the column is open, as a fraction of the screen's
// width; 0 on a screen too narrow for it (the column then opens above the bar instead).
static Real columnLeftEdge()
{
	if (!TheDisplay || TheDisplay->getHeight() <= 0
		|| (Real)TheDisplay->getWidth() / (Real)TheDisplay->getHeight() < 1.5f) {
		return 0.0f;
	}
	const ColumnGeometry g = columnGeometry(s_lastBarTop);
	return (Real)(s_armyShown ? g.wideRight : g.narrowRight) / (Real)TheDisplay->getWidth();
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

// The army buttons in or out: the open column widens or narrows, and the bar follows it.
static void setArmyShown(Bool shown)
{
	if (s_armyShown == shown) {
		return;
	}
	s_armyShown = shown;
	if (s_drawerOpen && TheControlBar) {
		TheControlBar->setArmyColumnOpen(FALSE, 0.0f);
		TheControlBar->setArmyColumnOpen(TRUE, columnLeftEdge());
	}
}

//-------------------------------------------------------------------------------------------------
void GroupPanelCalibrateFollowOffset(Int barScreenX, Int barScreenY)
{
	s_lastBarTop = barScreenY;
	fprintf(stderr, "[GX-UISCALE] group panel: control bar at %d,%d\n", barScreenX, barScreenY);
}

void GroupPanelCloseDrawer()
{
	setDrawer(FALSE);
}

void GroupPanelFollowControlBar(Int barScreenX, Int barScreenY, Bool visible)
{
	if (!visible || !TheWindowManager || !TheDisplay) {
		return;
	}
	GameWindow *parent = panelWindow(s_parentID);
	GameWindow *handle = panelWindow(s_buttonHandleID);
	GameWindow *toggle = panelWindow(s_buttonArmyToggleID);
	GameWindow *row = panelWindow(s_groupRowID);
	if (!parent || !handle || !toggle || !row) {
		return;
	}
	(void)barScreenX;
	s_lastBarTop = barScreenY;
	const Int height = TheDisplay->getHeight();
	const ColumnGeometry g = columnGeometry(barScreenY);
	const Bool besideBar = columnLeftEdge() > 0.0f;
	Int px, py;
	parent->winGetScreenPosition(&px, &py);

	if (!s_drawerOpen) {
		s_backdropValid = FALSE;
		parent->winSetStatus(WIN_STATUS_NO_INPUT);
		row->winHide(TRUE);
		toggle->winHide(TRUE);
		for (Int i = 0; i < ARMY_BUTTON_COUNT; ++i) {
			if (GameWindow *button = panelWindow(s_armyButtonID[i])) {
				button->winHide(TRUE);
			}
		}
		placeButton(handle, g.x0, barScreenY - g.rowHeight - g.gap, px, py, g.cellWidth, g.rowHeight);
		fitParentToVisibleContent();
		return;
	}

	// Beside the bar: from its top down. Without room beside it, the same block above it.
	const Int blockHeight = 6 * g.rowHeight + 5 * g.gap;
	const Int top = besideBar ? barScreenY : barScreenY - g.margin - blockHeight;
	const Int right = s_armyShown ? g.wideRight : g.narrowRight;

	s_backdropX = 0;
	s_backdropY = max(0, top - g.margin);
	s_backdropW = right;
	s_backdropH = (besideBar ? height : barScreenY) - s_backdropY;
	s_backdropValid = TRUE;
	parent->winClearStatus(WIN_STATUS_NO_INPUT);

	// First row: close, and the army switch.
	placeButton(handle, g.x0, top, px, py, g.cellWidth, g.rowHeight);
	toggle->winHide(FALSE);
	placeButton(toggle, g.x0 + g.cellWidth + g.gap, top, px, py, g.cellWidth, g.rowHeight);

	// The groups, two to a row.
	const Int groupsTop = top + g.rowHeight + g.gap;
	row->winHide(FALSE);
	row->winSetPosition(g.x0 - px, groupsTop - py);
	row->winSetSize(g.columnWidth, 5 * g.rowHeight + 4 * g.gap);
	for (Int i = 0; i < 10; ++i) {
		GameWindow *button = panelWindow(s_buttonGroupID[i]);
		if (!button) {
			continue;
		}
		button->winSetPosition((i % 2) * (g.cellWidth + g.gap), (i / 2) * (g.rowHeight + g.gap));
		button->winSetSize(g.cellWidth, g.rowHeight);
		fitButtonFont(button, g.cellWidth, g.rowHeight);
	}

	// The army: a second column as tall as the first, five buttons.
	const Int armyX = g.x0 + g.columnWidth + g.gap;
	const Int armyRow = (blockHeight - 4 * g.gap) / 5;
	for (Int i = 0; i < ARMY_BUTTON_COUNT; ++i) {
		GameWindow *button = panelWindow(s_armyButtonID[i]);
		if (!button) {
			continue;
		}
		button->winHide(!s_armyShown);
		if (s_armyShown) {
			placeButton(button, armyX, top + i * (armyRow + g.gap), px, py, g.columnWidth, armyRow);
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
	if (GameWindow *toggle = panelWindow(s_buttonArmyToggleID)) {
		const UnicodeString mark(s_armyShown ? L"<" : L">");
		if (toggle->winGetText() != mark) {
			GadgetButtonSetText(toggle, mark);
		}
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
				setDrawer(!s_drawerOpen);
				return MSG_HANDLED;
			}
			if (controlID == (Int)s_buttonArmyToggleID) {
				setArmyShown(!s_armyShown);
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
