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

// GeneralsX @feature Android port 13/11/2026 The single-player cheat menu, opened by the Cheats button
// of the in-game Options (OptionsMenu.cpp), which exists only in a game against the computer
// (GXSinglePlayerCheat::menuAvailable). Each button sends MSG_GX_SINGLE_PLAYER_CHEAT; the game logic
// applies it (GameLogicDispatch.cpp). The popup is built here rather than from a .wnd, in the look of
// the Options screen beneath it (GXWindowLook), as the Extras popup is.

#include "PreRTS.h"
#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Registry.h"
#include "GameClient/Display.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameFont.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/GXWindowLook.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/GXSinglePlayerCheat.h"
#include <cstdlib>
#include <cstring>

namespace
{
	// Title, +money, instant build on, instant build off, reveal map, +1 General's point, Close, and
	// the Options button.
	struct CheatTexts { const char *language; const wchar_t *text[8]; };
	const CheatTexts kCheatTexts[] = {
		{ "english", { L"CHEATS", L"+10000 money", L"Instant build: ON", L"Instant build: OFF", L"Reveal map", L"+1 General's point", L"Close", L"Cheats" } },
		{ "russian", { L"ЧИТЫ", L"+10000 денег", L"Мгновенная постройка: ВКЛ", L"Мгновенная постройка: ВЫКЛ", L"Открыть карту", L"+1 очко генерала", L"Закрыть", L"Читы" } },
		{ "ukrainian", { L"ЧИТИ", L"+10000 грошей", L"Миттєве будівництво: УВІМК", L"Миттєве будівництво: ВИМК", L"Відкрити карту", L"+1 очко генерала", L"Закрити", L"Чити" } },
		{ "interslavic", { L"ČITY", L"+10000 pěnęzej", L"Momentalna gradnja: VKL", L"Momentalna gradnja: IZKL", L"Odkryti kartu", L"+1 točka generala", L"Zatvoriti", L"Čity" } },
		{ "german", { L"CHEATS", L"+10000 Geld", L"Sofortbau: AN", L"Sofortbau: AUS", L"Karte aufdecken", L"+1 Generalspunkt", L"Schließen", L"Cheats" } },
		{ "french", { L"TRICHES", L"+10000 crédits", L"Construction instantanée : OUI", L"Construction instantanée : NON", L"Révéler la carte", L"+1 point de général", L"Fermer", L"Triches" } },
		{ "spanish", { L"TRUCOS", L"+10000 de dinero", L"Construcción instantánea: SÍ", L"Construcción instantánea: NO", L"Revelar mapa", L"+1 punto de general", L"Cerrar", L"Trucos" } },
		{ "brazilian", { L"TRAPAÇAS", L"+10000 de dinheiro", L"Construção instantânea: SIM", L"Construção instantânea: NÃO", L"Revelar mapa", L"+1 ponto de general", L"Fechar", L"Trapaças" } },
		{ "polish", { L"KODY", L"+10000 pieniędzy", L"Natychmiastowa budowa: WŁ.", L"Natychmiastowa budowa: WYŁ.", L"Odkryj mapę", L"+1 punkt generała", L"Zamknij", L"Kody" } },
		{ "chinese", { L"作弊", L"+10000 资金", L"瞬间建造：开", L"瞬间建造：关", L"显示全图", L"+1 将军点数", L"关闭", L"作弊" } },
		{ "korean", { L"치트", L"+10000 자금", L"즉시 건설: 켜짐", L"즉시 건설: 꺼짐", L"지도 공개", L"+1 장군 포인트", L"닫기", L"치트" } },
		{ "arabic", { L"الغش", L"+10000 مال", L"بناء فوري: تشغيل", L"بناء فوري: إيقاف", L"كشف الخريطة", L"+1 نقطة جنرال", L"إغلاق", L"غش" } },
		{ "persian", { L"تقلب", L"+10000 پول", L"ساخت فوری: روشن", L"ساخت فوری: خاموش", L"نمایش کل نقشه", L"+1 امتیاز ژنرال", L"بستن", L"تقلب" } },
	};

	const CheatTexts &cheatTexts()
	{
		const char *pack = getenv("GENERALSX_TEXT_LANGUAGE");
		AsciiString language = (pack != nullptr && pack[0] != '\0') ? AsciiString(pack) : GetRegistryLanguage();
		language.toLower();
		for (Int i = 0; i < (Int)(sizeof(kCheatTexts) / sizeof(kCheatTexts[0])); ++i)
			if (language.compare(kCheatTexts[i].language) == 0)
				return kCheatTexts[i];
		return kCheatTexts[0];
	}

	const Int kButtons = GXSinglePlayerCheat::COUNT + 1; // the cheats and Close
	GameWindow *s_popup = nullptr;
	GameWindow *s_hiddenOptions = nullptr;
	GameWindow *s_buttons[kButtons] = {};
	IRegion2D s_panel = { { 0, 0 }, { 0, 0 } };
	Color s_dim = GameMakeColor(0, 0, 0, 160);
	WinDrawData s_frame = { nullptr, GameMakeColor(0, 0, 0, 255), GameMakeColor(255, 190, 1, 255) };
	// Instant build as the menu shows it: the logic applies a toggle on its next frame, and the game is
	// paused under Options, so toggles sent on this frame are counted here until the frame advances.
	Int s_pendingToggles = 0;
	UnsignedInt s_pendingFrame = 0;

	Bool instantBuildShown()
	{
		Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
		Bool on = player && player->buildsInstantly();
		if (TheGameLogic && TheGameLogic->getFrame() == s_pendingFrame && (s_pendingToggles & 1))
			on = !on;
		return on;
	}

	void setInstantLabel()
	{
		const CheatTexts &t = cheatTexts();
		if (GameWindow *button = s_buttons[GXSinglePlayerCheat::TOGGLE_INSTANT_BUILD])
			GadgetButtonSetText(button, UnicodeString(instantBuildShown() ? t.text[2] : t.text[3]));
	}

	void popupDraw(GameWindow *, WinInstanceData *)
	{
		if (!TheDisplay)
			return;
		TheDisplay->drawFillRect(0, 0, TheDisplay->getWidth(), TheDisplay->getHeight(), s_dim);
		const Int x0 = s_panel.lo.x, y0 = s_panel.lo.y;
		const Int w = s_panel.hi.x - x0, h = s_panel.hi.y - y0;
		if (s_frame.image)
			TheDisplay->drawImage(s_frame.image, x0, y0, x0 + w, y0 + h);
		else if (((s_frame.color >> 24) & 0xFF) != 0)
			TheDisplay->drawFillRect(x0, y0, w, h, s_frame.color);
		TheDisplay->drawOpenRect(x0, y0, w, h, 1.0f, s_frame.borderColor);
	}

	void sendCheat(Int cheat)
	{
		if (!GXSinglePlayerCheat::menuAvailable() || !TheMessageStream)
			return;
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_GX_SINGLE_PLAYER_CHEAT);
		msg->appendIntegerArgument(cheat);
		const CheatTexts &t = cheatTexts();
		if (cheat == GXSinglePlayerCheat::TOGGLE_INSTANT_BUILD)
		{
			const UnsignedInt frame = TheGameLogic->getFrame();
			if (frame != s_pendingFrame)
			{
				s_pendingFrame = frame;
				s_pendingToggles = 0;
			}
			++s_pendingToggles;
			setInstantLabel();
		}
		const wchar_t *said = cheat == GXSinglePlayerCheat::ADD_CASH ? t.text[1]
			: cheat == GXSinglePlayerCheat::TOGGLE_INSTANT_BUILD ? (instantBuildShown() ? t.text[2] : t.text[3])
			: cheat == GXSinglePlayerCheat::REVEAL_MAP ? t.text[4] : t.text[5];
		if (TheInGameUI)
			TheInGameUI->messageNoFormat(UnicodeString(said));
		fprintf(stderr, "[GX-CHEAT] menu sent cheat %d\n", cheat);
	}

	WindowMsgHandledType popupSystem(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
	{
		switch (msg)
		{
			case GWM_INPUT_FOCUS:
				if (mData1 == TRUE)
					*(Bool *)mData2 = TRUE;
				return MSG_HANDLED;
			case GBM_SELECTED:
			{
				GameWindow *control = (GameWindow *)mData1;
				for (Int i = 0; i < kButtons; ++i)
				{
					if (control != s_buttons[i])
						continue;
					if (i == GXSinglePlayerCheat::COUNT)
						CloseCheatMenu();
					else
						sendCheat(i);
					return MSG_HANDLED;
				}
				break;
			}
			default:
				break;
		}
		return MSG_IGNORED;
	}
}

UnicodeString CheatMenuButtonLabel()
{
	return UnicodeString(cheatTexts().text[7]);
}

Bool CheatMenuAvailable()
{
	return GXSinglePlayerCheat::menuAvailable();
}

void CloseCheatMenu()
{
	if (!s_popup)
		return;
	GameWindow *popup = s_popup;
	s_popup = nullptr;
	TheWindowManager->winUnsetModal(popup);
	TheWindowManager->winDestroy(popup);
	for (Int i = 0; i < kButtons; ++i)
		s_buttons[i] = nullptr;
	if (s_hiddenOptions)
	{
		s_hiddenOptions->winHide(FALSE);
		s_hiddenOptions = nullptr;
	}
	if (GameWindow *options = GXWindowLook::byName("OptionsMenu.wnd:OptionsMenuParent"))
		TheWindowManager->winSetFocus(options);
}

void OpenCheatMenu()
{
	if (s_popup || !TheWindowManager || !TheDisplay || !CheatMenuAvailable())
		return;
	const Int width = TheDisplay->getWidth(), height = TheDisplay->getHeight();

	// The look of the Options screen: its dimming, a section box, its Accept button and title.
	if (GameWindow *options = GXWindowLook::optionsRoot())
		s_dim = options->winGetInstanceData()->m_enabledDrawData[0].color;
	GameWindow *frame = GXWindowLook::exemplar("OptionsMenu.wnd:VideoParent", 0);
	if (!frame)
		frame = GXWindowLook::exemplar("OptionsMenu.wnd:AudioParent", 0);
	if (frame)
		s_frame = frame->winGetInstanceData()->m_enabledDrawData[0];
	GameWindow *buttonLook = GXWindowLook::exemplar("OptionsMenu.wnd:ButtonAccept", GWS_PUSH_BUTTON);
	GameWindow *titleLook = GXWindowLook::byLabel(GXWindowLook::optionsRoot(), "GUI:Options");

	WinInstanceData inst;
	inst.init();
	inst.m_style = GWS_USER_WINDOW;
	s_popup = TheWindowManager->winCreate(nullptr, WIN_STATUS_ENABLED, 0, 0, width, height, popupSystem, &inst);
	if (!s_popup)
		return;
	s_popup->winSetDrawFunc(popupDraw);

	// One column in the middle of the screen: the title, the cheats, a gap, Close.
	const Int rowH = height * 34 / 600, gap = height * 10 / 600, margin = height * 14 / 600;
	const Int colW = width * 300 / 800;
	const Int rows = 1 + kButtons;
	const Int panelH = margin * 2 + rows * rowH + (rows - 1) * gap + gap;
	const Int x0 = (width - colW) / 2, y0 = (height - panelH) / 2;
	s_panel.lo.x = x0 - margin; s_panel.lo.y = y0;
	s_panel.hi.x = x0 + colW + margin; s_panel.hi.y = y0 + panelH;
	const CheatTexts &t = cheatTexts();
	GameFont *font = buttonLook ? buttonLook->winGetFont() : nullptr;

	Int y = y0 + margin;
	{
		WinInstanceData titleInst;
		TextData textData;
		titleInst.init();
		memset(&textData, 0, sizeof(textData));
		textData.centered = TRUE;
		textData.centeredVertically = TRUE;
		titleInst.m_style = GWS_STATIC_TEXT;
		GameWindow *title = TheWindowManager->gogoGadgetStaticText(s_popup, WIN_STATUS_ENABLED, x0, y, colW, rowH,
			&titleInst, &textData, font, FALSE);
		if (title)
		{
			GXWindowLook::copy(title, titleLook, FALSE);
			GXWindowLook::copyFont(title, titleLook);
			GadgetStaticTextSetText(title, UnicodeString(t.text[0]));
		}
	}
	y += rowH + gap;
	for (Int i = 0; i < kButtons; ++i)
	{
		if (i == GXSinglePlayerCheat::COUNT)
			y += gap;
		WinInstanceData buttonInst;
		buttonInst.init();
		buttonInst.m_style = GWS_PUSH_BUTTON | GWS_MOUSE_TRACK;
		GameWindow *button = TheWindowManager->gogoGadgetPushButton(s_popup, WIN_STATUS_ENABLED, x0, y, colW, rowH,
			&buttonInst, font, FALSE);
		if (button)
		{
			GXWindowLook::copy(button, buttonLook, TRUE);
			const wchar_t *label = i == GXSinglePlayerCheat::ADD_CASH ? t.text[1]
				: i == GXSinglePlayerCheat::REVEAL_MAP ? t.text[4]
				: i == GXSinglePlayerCheat::ADD_GENERAL_POINT ? t.text[5]
				: i == GXSinglePlayerCheat::COUNT ? t.text[6] : L"";
			GadgetButtonSetText(button, UnicodeString(label));
		}
		s_buttons[i] = button;
		y += rowH + gap;
	}
	setInstantLabel();

	s_hiddenOptions = GXWindowLook::optionsRoot();
	if (s_hiddenOptions && !s_hiddenOptions->winIsHidden())
		s_hiddenOptions->winHide(TRUE);
	else
		s_hiddenOptions = nullptr;
	s_popup->winBringToTop();
	TheWindowManager->winSetModal(s_popup);
	TheWindowManager->winSetFocus(s_popup);
	fprintf(stderr, "[GX-CHEAT] menu opened\n");
}
