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

// FILE: ExtrasMenu.cpp ////////////////////////////////////////////////////////////////////////////
// Desc:   Extras/QoL settings menu for SagePatch (camera, scroll, terrain, graphics)
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"
#include "Common/GXControlScheme.h"
#include "Common/Registry.h"
#include "GameClient/GXExtrasValues.h"

#include "GameClient/GameClient.h"
#include "Common/GlobalData.h"
#include "Common/OptionPreferences.h"
#include "Common/NameKeyGenerator.h"
#include "GameClient/Shell.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Display.h"
#include "GameClient/View.h"
#include "GameClient/Color.h"
#include "GameClient/GameFont.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GXExtrasValues.h"
#include "GameClient/GXWindowLook.h"
#include "GameLogic/GameLogic.h"
#include <cstdlib>
#include <cstring>

// Widget IDs and pointers
static NameKeyType sliderMaxCameraHeightID = NAMEKEY_INVALID;
static NameKeyType sliderMinCameraHeightID = NAMEKEY_INVALID;
static NameKeyType sliderCameraPitchID = NAMEKEY_INVALID;
static NameKeyType sliderScrollSpeedID = NAMEKEY_INVALID;
static NameKeyType sliderDrawDistanceID = NAMEKEY_INVALID;

static GameWindow *sliderMaxCameraHeight = nullptr;
static GameWindow *sliderMinCameraHeight = nullptr;
static GameWindow *sliderCameraPitch = nullptr;
static GameWindow *sliderScrollSpeed = nullptr;
static GameWindow *sliderDrawDistance = nullptr;

static OptionPreferences *pref = nullptr;

// GeneralsX @extras Codex 14/10/2026 Keep the Options modal and paused game beneath this popup.
static WindowLayout *s_extrasLayout = nullptr;
static OptionPreferences *s_optionsPreferences = nullptr;
// GeneralsX @feature Android port 18/10/2026 The maximum height is shown as the ceiling the player
// can zoom out to. With the Tactical scheme's higher camera (GXControlScheme::CAMERA_HIGH, View.cpp)
// that ceiling is four times the stored MaxCameraHeight (3600 at most), as in realkot's build, and
// the presets 400 / 1000 / 2000 / 3600 apply; without it the ceiling is the stored value itself.
static Int gxCameraScale()
{
	return GXControlScheme::enabled(GXControlScheme::CAMERA_HIGH) ? 4 : 1;
}
#define GX_EXTRA_CAMERA_SCALE gxCameraScale()

// GeneralsX @feature Android port 19/10/2026 The frame takes the look of a section box on the game's
// own Options screen (VideoParent) rather than a blue written here, so a mod that restyles Options
// restyles this too. Captured when the popup opens, while Options is up beneath it.
static WinDrawData s_extrasFrame = { nullptr, GameMakeColor(0, 0, 0, 255), GameMakeColor(255, 190, 1, 255) };

static void gxExtrasBackdropDraw(GameWindow *window, WinInstanceData *data)
{
    // GeneralsX @feature Codex 15/10/2026 Opaque, with the section rules of the stock Options.
    if (!TheDisplay) return;
    const Int w = TheDisplay->getWidth(), h = TheDisplay->getHeight();
    TheDisplay->drawFillRect(0, 0, w, h, GameMakeColor(0, 0, 0, 255));
    const Int x0 = w/10, y0 = h/20, x1 = w*9/10, y1 = h*18/20;
    if (s_extrasFrame.image)
        TheDisplay->drawImage(s_extrasFrame.image, x0, y0, x1, y1);
    else if (((s_extrasFrame.color >> 24) & 0xFF) != 0)
        TheDisplay->drawFillRect(x0, y0, x1 - x0, y1 - y0, s_extrasFrame.color);
    const Color rule = s_extrasFrame.borderColor;
    TheDisplay->drawOpenRect(x0, y0, x1 - x0, y1 - y0, 1.0f, rule);
    TheDisplay->drawLine(x0, h*95/600, x1, h*95/600, 1.0f, rule);
    TheDisplay->drawLine(x0, h*425/600, x1, h*425/600, 1.0f, rule);
}

// GeneralsX @feature Android port 19/10/2026 Buttons, sliders, number fields and labels take the
// look of their counterparts on the game's Options screen (GXWindowLook): the Extras layout's own
// colours only remain where Options has nothing of that kind.
static void gxExtrasTakeOptionsLook(GameWindow *parent)
{
    if (GameWindow *frame = GXWindowLook::exemplar("OptionsMenu.wnd:VideoParent", 0))
        s_extrasFrame = frame->winGetInstanceData()->m_enabledDrawData[0];
    else if (GameWindow *frame = GXWindowLook::exemplar("OptionsMenu.wnd:AudioParent", 0))
        s_extrasFrame = frame->winGetInstanceData()->m_enabledDrawData[0];
    GXWindowLook::copyToAll(parent, GWS_PUSH_BUTTON,
        GXWindowLook::exemplar("OptionsMenu.wnd:ButtonAccept", GWS_PUSH_BUTTON), TRUE);
    GXWindowLook::copyToAll(parent, GWS_HORZ_SLIDER,
        GXWindowLook::exemplar("OptionsMenu.wnd:SliderMusicVolume", GWS_HORZ_SLIDER), TRUE);
    GXWindowLook::copyToAll(parent, GWS_ENTRY_FIELD,
        GXWindowLook::exemplar("OptionsMenu.wnd:TextEntryHTTPProxy", GWS_ENTRY_FIELD), TRUE);
    GXWindowLook::copyToAll(parent, GWS_STATIC_TEXT,
        GXWindowLook::exemplar("OptionsMenu.wnd:MusicVolumeLabel", GWS_STATIC_TEXT), FALSE);
}

void CloseExtrasMenu()
{
    WindowLayout *layout = s_extrasLayout;
    s_extrasLayout = nullptr;
    s_optionsPreferences = nullptr;
    if (!layout) return;
    GameWindow *parent = TheWindowManager->winGetWindowFromId(nullptr,
        TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:ExtrasMenuParent"));
    if (parent) TheWindowManager->winUnsetModal(parent);
    layout->runShutdown(nullptr);
    layout->destroyWindows();
    deleteInstance(layout);
    GameWindow *options = TheWindowManager->winGetWindowFromId(nullptr,
        TheNameKeyGenerator->nameToKey("OptionsMenu.wnd:OptionsMenuParent"));
    if (options) TheWindowManager->winSetFocus(options);
}

void OpenExtrasMenu(OptionPreferences *optionsPreferences)
{
    if (s_extrasLayout || !TheWindowManager) return;
    WindowLayout *layout = TheWindowManager->winCreateLayout("ExtrasMenu.wnd");
    if (!layout) {
        fprintf(stderr, "[GX-EXTRAS] missing layout; Options remains usable\n");
        return;
    }
    GameWindow *parent = TheWindowManager->winGetWindowFromId(nullptr,
        TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:ExtrasMenuParent"));
    if (!parent) { layout->destroyWindows(); deleteInstance(layout); return; }
    s_extrasLayout = layout;
    s_optionsPreferences = optionsPreferences;
    parent->winSetDrawFunc(gxExtrasBackdropDraw);
    gxExtrasTakeOptionsLook(parent);
    layout->runInit(nullptr);
    layout->hide(FALSE);
    layout->bringForward();
    TheWindowManager->winSetModal(parent);
    TheWindowManager->winSetFocus(parent);
}

// Slider ranges (matching .wnd definitions)
#define SLIDER_MAX_CAMERA_HEIGHT_MIN (100 * GX_EXTRA_CAMERA_SCALE)
// GeneralsX @tweak Codex 15/10/2026 900 stored units correspond to manual height 3600.
#define SLIDER_MAX_CAMERA_HEIGHT_MAX (900 * GX_EXTRA_CAMERA_SCALE)
static const Int SLIDER_MIN_CAMERA_HEIGHT_MIN = 50;
static const Int SLIDER_MIN_CAMERA_HEIGHT_MAX = 300;
static const Int SLIDER_CAMERA_PITCH_MIN = 20;
static const Int SLIDER_CAMERA_PITCH_MAX = 60;
static const Int SLIDER_SCROLL_SPEED_MIN = 1;
static const Int SLIDER_SCROLL_SPEED_MAX = 200;
static const Int SLIDER_DRAW_DISTANCE_MIN = 100;
static const Int SLIDER_DRAW_DISTANCE_MAX = 200;

// GeneralsX @feature Codex 16/10/2026 Numeric edits and steppers share the slider draft.
static Int s_exactLast[5]={-1,-1,-1,-1,-1};
static Int s_exactMin[5]={400,50,20,1,100};
static Int s_exactMax[5]={3600,300,60,200,200};
static Int s_exactStep[5]={40,1,1,1,1};
static const Int s_cameraPresets[4]={400,1000,2000,3600};
static GameWindow *gxExactWindow(Int index,const char *suffix) {
    AsciiString name;name.format("ExtrasMenu.wnd:Exact%d%s",index,suffix);
    return TheWindowManager->winGetWindowFromId(nullptr,TheNameKeyGenerator->nameToKey(name));
}
static GameWindow *gxExactSlider(Int index) {
    GameWindow *sliders[]={sliderMaxCameraHeight,sliderMinCameraHeight,sliderCameraPitch,sliderScrollSpeed,sliderDrawDistance};
    return index>=0 && index<5 ? sliders[index] : nullptr;
}
static void gxSyncExactValues(Bool force=FALSE) {
    for(Int i=0;i<5;++i) {
        GameWindow *slider=gxExactSlider(i),*entry=gxExactWindow(i,"Value");
        if(!slider||!entry)continue;
        Int value=GXExtrasValues::normalize(GadgetSliderGetPosition(slider),s_exactMin[i],s_exactMax[i],i==0?GX_EXTRA_CAMERA_SCALE:1);
        if(force || value!=s_exactLast[i]) {
            UnicodeString text;text.format(L"%d",value);GadgetTextEntrySetText(entry,text);s_exactLast[i]=value;
        }
    }
}
static void gxCommitExactValue(Int i) {
    GameWindow *entry=gxExactWindow(i,"Value"),*slider=gxExactSlider(i);
    if(!entry||!slider)return;
    UnicodeString text=GadgetTextEntryGetText(entry);Int value=0;
    if(GXExtrasValues::digits(text.str(),text.getLength(),value)) {
        value=GXExtrasValues::normalize(value,s_exactMin[i],s_exactMax[i],i==0?GX_EXTRA_CAMERA_SCALE:1);
        GadgetSliderSetPosition(slider,value);
    }
    s_exactLast[i]=-1;
}

namespace
{
	struct ExtrasTexts { const char *language; const wchar_t *text[10]; };
	// Title, the five rows, Cancel, Defaults, Apply, and the Options button.
	const ExtrasTexts kExtrasTexts[] = {
		{ "english", { L"EXTRA OPTIONS", L"Maximum camera height", L"Minimum camera height", L"Camera pitch", L"Scroll speed", L"Terrain draw distance", L"Cancel", L"Defaults", L"Apply", L"Extras" } },
		{ "russian", { L"ДОПОЛНИТЕЛЬНЫЕ НАСТРОЙКИ", L"Максимальная высота камеры", L"Минимальная высота камеры", L"Наклон камеры", L"Скорость прокрутки", L"Дальность отрисовки", L"Отмена", L"По умолчанию", L"Применить", L"Дополнительно" } },
		{ "ukrainian", { L"ДОДАТКОВІ НАЛАШТУВАННЯ", L"Максимальна висота камери", L"Мінімальна висота камери", L"Нахил камери", L"Швидкість прокручування", L"Дальність відмальовування", L"Скасувати", L"За замовчуванням", L"Застосувати", L"Додатково" } },
		{ "interslavic", { L"DODATNE NASTAVLJENJA", L"Maksimalna visina kamery", L"Minimalna visina kamery", L"Naklon kamery", L"Brzina prokrutki", L"Daljina risovanja", L"Otměniti", L"Po umolčanju", L"Primeniti", L"Dodatno" } },
		{ "german", { L"WEITERE OPTIONEN", L"Maximale Kamerahöhe", L"Minimale Kamerahöhe", L"Kameraneigung", L"Scrollgeschwindigkeit", L"Geländesichtweite", L"Abbrechen", L"Standard", L"Übernehmen", L"Extras" } },
		{ "french", { L"OPTIONS SUPPLÉMENTAIRES", L"Hauteur max. de la caméra", L"Hauteur min. de la caméra", L"Inclinaison de la caméra", L"Vitesse de défilement", L"Distance d'affichage", L"Annuler", L"Par défaut", L"Appliquer", L"Extras" } },
		{ "spanish", { L"OPCIONES ADICIONALES", L"Altura máxima de cámara", L"Altura mínima de cámara", L"Inclinación de cámara", L"Velocidad de desplazamiento", L"Distancia de dibujado", L"Cancelar", L"Predeterminado", L"Aplicar", L"Extras" } },
		{ "brazilian", { L"OPÇÕES EXTRAS", L"Altura máxima da câmera", L"Altura mínima da câmera", L"Inclinação da câmera", L"Velocidade de rolagem", L"Distância de desenho", L"Cancelar", L"Padrão", L"Aplicar", L"Extras" } },
		{ "polish", { L"DODATKOWE USTAWIENIA", L"Maks. wysokość kamery", L"Min. wysokość kamery", L"Nachylenie kamery", L"Szybkość przewijania", L"Zasięg rysowania", L"Anuluj", L"Domyślne", L"Zastosuj", L"Dodatkowe" } },
		{ "chinese", { L"附加选项", L"镜头最大高度", L"镜头最小高度", L"镜头俯角", L"滚动速度", L"地形绘制距离", L"取消", L"默认", L"应用", L"附加" } },
		{ "korean", { L"추가 설정", L"최대 카메라 높이", L"최소 카메라 높이", L"카메라 각도", L"스크롤 속도", L"지형 표시 거리", L"취소", L"기본값", L"적용", L"추가" } },
		{ "arabic", { L"خيارات إضافية", L"أقصى ارتفاع للكاميرا", L"أدنى ارتفاع للكاميرا", L"ميل الكاميرا", L"سرعة التمرير", L"مسافة رسم التضاريس", L"إلغاء", L"الافتراضي", L"تطبيق", L"إضافات" } },
		{ "persian", { L"تنظیمات بیشتر", L"بیشترین ارتفاع دوربین", L"کمترین ارتفاع دوربین", L"شیب دوربین", L"سرعت پیمایش", L"فاصله ترسیم زمین", L"لغو", L"پیش‌فرض", L"اعمال", L"بیشتر" } },
	};

	const ExtrasTexts &gxExtrasTexts()
	{
		const char *pack = getenv("GENERALSX_TEXT_LANGUAGE");
		AsciiString language = (pack != nullptr && pack[0] != '\0') ? AsciiString(pack) : GetRegistryLanguage();
		language.toLower();
		for (Int i = 0; i < (Int)(sizeof(kExtrasTexts) / sizeof(kExtrasTexts[0])); ++i)
			if (language.compare(kExtrasTexts[i].language) == 0)
				return kExtrasTexts[i];
		return kExtrasTexts[0];
	}
}

UnicodeString ExtrasMenuButtonLabel()
{
	return UnicodeString(gxExtrasTexts().text[9]);
}

//-------------------------------------------------------------------------------------------------
void ExtrasMenuInit(WindowLayout *layout, void *userData)
{
	fprintf(stderr, "[GX-EXTRAS] initialize popup\n");
	sliderMaxCameraHeightID = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:SliderMaxCameraHeight");
	sliderMinCameraHeightID = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:SliderMinCameraHeight");
	sliderCameraPitchID = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:SliderCameraPitch");
	sliderScrollSpeedID = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:SliderScrollSpeed");
	sliderDrawDistanceID = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:SliderDrawDistance");

	sliderMaxCameraHeight = TheWindowManager->winGetWindowFromId(nullptr, sliderMaxCameraHeightID);
	sliderMinCameraHeight = TheWindowManager->winGetWindowFromId(nullptr, sliderMinCameraHeightID);
	sliderCameraPitch = TheWindowManager->winGetWindowFromId(nullptr, sliderCameraPitchID);
	sliderScrollSpeed = TheWindowManager->winGetWindowFromId(nullptr, sliderScrollSpeedID);
	sliderDrawDistance = TheWindowManager->winGetWindowFromId(nullptr, sliderDrawDistanceID);

	pref = NEW OptionPreferences;

    // GeneralsX @feature Android port 18/10/2026 The camera ranges for the scale in use, set on the
    // sliders before any position (the .wnd holds the Tactical range).
    s_exactMin[0] = SLIDER_MAX_CAMERA_HEIGHT_MIN;
    s_exactMax[0] = SLIDER_MAX_CAMERA_HEIGHT_MAX;
    s_exactStep[0] = 10 * GX_EXTRA_CAMERA_SCALE;
    {
        GameWindow *sliders[] = { sliderMaxCameraHeight, sliderMinCameraHeight, sliderCameraPitch, sliderScrollSpeed, sliderDrawDistance };
        for (Int i = 0; i < 5; ++i)
            if (sliders[i])
                TheWindowManager->winSendSystemMsg(sliders[i], GSM_SET_MIN_MAX, s_exactMin[i], s_exactMax[i]);
    }
    if (sliderMaxCameraHeight)
        GadgetSliderSetPosition(sliderMaxCameraHeight, (Int)(pref->getMaxCameraHeight() * GX_EXTRA_CAMERA_SCALE));
    if (sliderMinCameraHeight)
        GadgetSliderSetPosition(sliderMinCameraHeight, (Int)pref->getMinCameraHeight());
    if (sliderCameraPitch)
        GadgetSliderSetPosition(sliderCameraPitch, (Int)pref->getCameraPitch());
    if (sliderScrollSpeed)
        GadgetSliderSetPosition(sliderScrollSpeed, (Int)(pref->getScrollFactor() * 100.0f));
    if (sliderDrawDistance)
        GadgetSliderSetPosition(sliderDrawDistance, (Int)(pref->getTerrainDrawDistanceScale() * 100.0f));

    // GeneralsX @feature Android port 18/10/2026 Labels in the text language (a language pack's, else
    // the game's); realkot's build had English and Russian.
    const ExtrasTexts &t = gxExtrasTexts();
    const char *names[] = {"LabelTitle", "LabelMaxCameraHeight", "LabelMinCameraHeight", "LabelCameraPitch", "LabelScrollSpeed", "LabelDrawDistance"};
    for (Int i=0; i<6; ++i) {
        AsciiString id; id.format("ExtrasMenu.wnd:%s", names[i]);
        GameWindow *label = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(id));
        if (label) {
            GadgetStaticTextSetText(label, UnicodeString(t.text[i]));
        }
    }
    const char *buttons[] = {"ButtonBack", "ButtonDefaults", "ButtonAccept"};
    for (Int i=0; i<3; ++i) {
        AsciiString id; id.format("ExtrasMenu.wnd:%s", buttons[i]);
        GameWindow *button = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(id));
        if (button) {
            GadgetButtonSetText(button, UnicodeString(t.text[6 + i]));
        }
    }
    // The presets are heights the Tactical camera reaches; without it they are hidden.
    for (Int i = 0; i < 4; ++i) {
        AsciiString name; name.format("ExtrasMenu.wnd:CameraPreset%d", i);
        GameWindow *preset = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(name));
        if (preset)
            preset->winHide(GX_EXTRA_CAMERA_SCALE == 1);
    }

    for(Int i=0;i<5;++i) {
        GameWindow *minus=gxExactWindow(i,"Minus"),*plus=gxExactWindow(i,"Plus");
        if(minus)GadgetButtonSetText(minus,UnicodeString(L"−"));
        if(plus)GadgetButtonSetText(plus,UnicodeString(L"+"));
    }
    for(Int i=0;i<4;++i) {
        AsciiString name;name.format("ExtrasMenu.wnd:CameraPreset%d",i);
        GameWindow *button=TheWindowManager->winGetWindowFromId(nullptr,TheNameKeyGenerator->nameToKey(name));
        UnicodeString text;text.format(L"%d",s_cameraPresets[i]);
        if(button)GadgetButtonSetText(button,text);
    }
    for(Int &value:s_exactLast)value=-1;
    gxSyncExactValues(TRUE);
}

//-------------------------------------------------------------------------------------------------
void ExtrasMenuUpdate(WindowLayout *layout, void *userData)
{
    gxSyncExactValues();
}

//-------------------------------------------------------------------------------------------------
void ExtrasMenuShutdown(WindowLayout *layout, void *userData)
{
	sliderMaxCameraHeight = nullptr;
	sliderMinCameraHeight = nullptr;
	sliderCameraPitch = nullptr;
	sliderScrollSpeed = nullptr;
	sliderDrawDistance = nullptr;

	if (pref) {
		delete pref;
		pref = nullptr;
	}
}

//-------------------------------------------------------------------------------------------------
static void saveExtras()
{
	if (!pref || !TheWritableGlobalData || !sliderMaxCameraHeight || !sliderMinCameraHeight
        || !sliderCameraPitch || !sliderScrollSpeed || !sliderDrawDistance) return;

	Int val;

	val = max(SLIDER_MAX_CAMERA_HEIGHT_MIN, min(SLIDER_MAX_CAMERA_HEIGHT_MAX, GadgetSliderGetPosition(sliderMaxCameraHeight))) / GX_EXTRA_CAMERA_SCALE;
    val = max(val, max(SLIDER_MIN_CAMERA_HEIGHT_MIN, min(SLIDER_MIN_CAMERA_HEIGHT_MAX, GadgetSliderGetPosition(sliderMinCameraHeight))));
	if (val > 0) {
		if (GXExtrasValues::retailMaxCameraHeight() <= 0.0f)
			GXExtrasValues::retailMaxCameraHeight() = TheGlobalData->m_maxCameraHeight;
		TheWritableGlobalData->m_maxCameraHeight = (Real)val;
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["MaxCameraHeight"] = prefString;
	}

	val = max(SLIDER_MIN_CAMERA_HEIGHT_MIN, min(SLIDER_MIN_CAMERA_HEIGHT_MAX, GadgetSliderGetPosition(sliderMinCameraHeight)));
	if (val > 0) {
		TheWritableGlobalData->m_minCameraHeight = (Real)val;
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["MinCameraHeight"] = prefString;
	}

	val = max(SLIDER_CAMERA_PITCH_MIN, min(SLIDER_CAMERA_PITCH_MAX, GadgetSliderGetPosition(sliderCameraPitch)));
	if (val > 0) {
		TheWritableGlobalData->m_cameraPitch = (Real)val;
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["CameraPitch"] = prefString;
	}

	val = max(SLIDER_SCROLL_SPEED_MIN, min(SLIDER_SCROLL_SPEED_MAX, GadgetSliderGetPosition(sliderScrollSpeed)));
	if (val > 0) {
		TheWritableGlobalData->m_keyboardScrollFactor = val / 100.0f;
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["ScrollFactor"] = prefString;
	}

	val = max(SLIDER_DRAW_DISTANCE_MIN, min(SLIDER_DRAW_DISTANCE_MAX, GadgetSliderGetPosition(sliderDrawDistance)));
	if (val > 0) {
		TheWritableGlobalData->m_terrainDrawDistanceScale = val / 100.0f;
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["TerrainDrawDistanceScale"] = prefString;
	}
    if (s_optionsPreferences) {
        const char *keys[] = {"MaxCameraHeight", "MinCameraHeight", "CameraPitch", "ScrollFactor", "TerrainDrawDistanceScale"};
        for (const char *key : keys) (*s_optionsPreferences)[key] = (*pref)[key];
    }
    if (TheTacticalView) {
        TheTacticalView->setCameraHeightAboveGroundLimitsToDefault();
        if (TheGameLogic && TheGameLogic->isInInteractiveGame() && !TheTacticalView->isDoingScriptedCamera()) {
            TheTacticalView->setPitch(DEG_TO_RADF(TheWritableGlobalData->m_cameraPitch));
        }
    }

}

//-------------------------------------------------------------------------------------------------
static void setDefaults()
{
	if (sliderMaxCameraHeight)
		GadgetSliderSetPosition(sliderMaxCameraHeight, 500 * GX_EXTRA_CAMERA_SCALE);
	if (sliderMinCameraHeight)
		GadgetSliderSetPosition(sliderMinCameraHeight, 80);
	if (sliderCameraPitch)
		GadgetSliderSetPosition(sliderCameraPitch, 37);
	if (sliderScrollSpeed)
		GadgetSliderSetPosition(sliderScrollSpeed, 100);
	if (sliderDrawDistance)
		GadgetSliderSetPosition(sliderDrawDistance, 105);
}

//-------------------------------------------------------------------------------------------------
WindowMsgHandledType ExtrasMenuSystem(GameWindow *window, UnsignedInt msg,
																				WindowMsgData mData1, WindowMsgData mData2)
{
	static NameKeyType buttonBack = NAMEKEY_INVALID;
	static NameKeyType buttonDefaults = NAMEKEY_INVALID;
	static NameKeyType buttonAccept = NAMEKEY_INVALID;

	switch (msg) {

		case GWM_CREATE:
		{
			buttonBack = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:ButtonBack");
			buttonDefaults = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:ButtonDefaults");
			buttonAccept = TheNameKeyGenerator->nameToKey("ExtrasMenu.wnd:ButtonAccept");
			break;
		}

		case GWM_DESTROY:
			break;

		case GWM_INPUT_FOCUS:
		{
			if (mData1 == TRUE)
				*(Bool *)mData2 = TRUE;
			return MSG_HANDLED;
		}

        // Extras is an owned modal, not a shell layout: its periodic update is
        // not guaranteed. Slider input must refresh the draft before Apply reads it.
        case GSM_SLIDER_TRACK:
            for(Int i=0;i<5;++i)if((GameWindow*)mData1==gxExactSlider(i)) {
                s_exactLast[i]=-1;
                gxSyncExactValues();return MSG_HANDLED;
            }
            break;

        case GEM_EDIT_DONE:
            for(Int i=0;i<5;++i)if((GameWindow*)mData1==gxExactWindow(i,"Value")) {
                gxCommitExactValue(i);gxSyncExactValues();return MSG_HANDLED;
            }
            break;

		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();
            for(Int i=0;i<5;++i) {
                GameWindow *minus=gxExactWindow(i,"Minus"),*plus=gxExactWindow(i,"Plus");
                if(control==minus || control==plus) {
                    gxCommitExactValue(i);
                    GameWindow *slider=gxExactSlider(i);
                    if(slider)GadgetSliderSetPosition(slider,GXExtrasValues::normalize(
                        GadgetSliderGetPosition(slider)+(control==plus?s_exactStep[i]:-s_exactStep[i]),s_exactMin[i],s_exactMax[i],i==0?GX_EXTRA_CAMERA_SCALE:1));
                    gxSyncExactValues();return MSG_HANDLED;
                }
            }
            for(Int i=0;i<4;++i) {
                AsciiString name;name.format("ExtrasMenu.wnd:CameraPreset%d",i);
                if(controlID==TheNameKeyGenerator->nameToKey(name)) {
                    GadgetSliderSetPosition(sliderMaxCameraHeight,GXExtrasValues::normalize(s_cameraPresets[i],s_exactMin[0],s_exactMax[0],GX_EXTRA_CAMERA_SCALE));
                    gxSyncExactValues();return MSG_HANDLED;
                }
            }


			if (controlID == buttonBack) {
				CloseExtrasMenu();
			}
			else if (controlID == buttonAccept) {
                for(Int i=0;i<5;++i)gxCommitExactValue(i);
				saveExtras();
				if (pref) {
					pref->write();
				}
				CloseExtrasMenu();
			}
			else if (controlID == buttonDefaults) {
				setDefaults();
                gxSyncExactValues(TRUE);
			}
			break;
		}

		default:
			break;
	}

	return MSG_IGNORED;
}

//-------------------------------------------------------------------------------------------------
WindowMsgHandledType ExtrasMenuInput(GameWindow *window, UnsignedInt msg,
																				WindowMsgData mData1, WindowMsgData mData2)
{
	switch (msg) {

		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;

			switch (key) {

				case KEY_ESC:
				{
					if (BitIsSet(state, KEY_STATE_UP)) {
						CloseExtrasMenu();
					}
					return MSG_HANDLED;
				}

			}
		}

	}

	return MSG_IGNORED;
}
