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

// GeneralsX @feature Android port 19/10/2026 The popup is drawn as the game draws its Options screen:
// the scene behind dimmed by the colour of the Options layout's own full-screen window, and a panel
// in the look of an Options section box (VideoParent), around what the popup actually shows rather
// than a fixed rectangle, with a rule under the title and one over the buttons. Options itself is
// hidden meanwhile, so it does not show through. All of it is taken when the popup opens.
static WinDrawData s_extrasFrame = { nullptr, GameMakeColor(0, 0, 0, 255), GameMakeColor(255, 190, 1, 255) };
static Color s_extrasDim = GameMakeColor(2, 2, 2, 126);
static IRegion2D s_extrasPanel = { { 0, 0 }, { 0, 0 } };
static Int s_extrasRuleTop = 0;
static Int s_extrasRuleBottom = 0;
static GameWindow *s_hiddenOptions = nullptr;

static void gxExtrasBackdropDraw(GameWindow *window, WinInstanceData *data)
{
    if (!TheDisplay) return;
    TheDisplay->drawFillRect(0, 0, TheDisplay->getWidth(), TheDisplay->getHeight(), s_extrasDim);
    const Int x0 = s_extrasPanel.lo.x, y0 = s_extrasPanel.lo.y;
    const Int w = s_extrasPanel.hi.x - x0, h = s_extrasPanel.hi.y - y0;
    if (s_extrasFrame.image)
        TheDisplay->drawImage(s_extrasFrame.image, x0, y0, x0 + w, y0 + h);
    else if (((s_extrasFrame.color >> 24) & 0xFF) != 0)
        TheDisplay->drawFillRect(x0, y0, w, h, s_extrasFrame.color);
    const Color rule = s_extrasFrame.borderColor;
    TheDisplay->drawOpenRect(x0, y0, w, h, 1.0f, rule);
    TheDisplay->drawLine(x0, s_extrasRuleTop, x0 + w, s_extrasRuleTop, 1.0f, rule);
    TheDisplay->drawLine(x0, s_extrasRuleBottom, x0 + w, s_extrasRuleBottom, 1.0f, rule);
}

static GameWindow *gxExtrasChild(const char *name)
{
    AsciiString id; id.format("ExtrasMenu.wnd:%s", name);
    return GXWindowLook::byName(id.str());
}

// GeneralsX @feature Android port 19/10/2026 Buttons, sliders, number fields and labels take the
// look and the fonts of their counterparts on the game's Options screen (GXWindowLook), the title
// that of the Options title: the Extras layout's own colours only remain where Options has nothing
// of that kind. Labels read from the left edge, as on Options.
static void gxExtrasTakeOptionsLook(GameWindow *parent)
{
    GameWindow *options = GXWindowLook::optionsRoot();
    if (options)
        s_extrasDim = options->winGetInstanceData()->m_enabledDrawData[0].color;
    GameWindow *frame = GXWindowLook::exemplar("OptionsMenu.wnd:VideoParent", 0);
    if (!frame)
        frame = GXWindowLook::exemplar("OptionsMenu.wnd:AudioParent", 0);
    if (frame)
        s_extrasFrame = frame->winGetInstanceData()->m_enabledDrawData[0];

    GameWindow *button = GXWindowLook::exemplar("OptionsMenu.wnd:ButtonAccept", GWS_PUSH_BUTTON);
    GameWindow *slider = GXWindowLook::exemplar("OptionsMenu.wnd:SliderMusicVolume", GWS_HORZ_SLIDER);
    GameWindow *entry = GXWindowLook::exemplar("OptionsMenu.wnd:TextEntryHTTPProxy", GWS_ENTRY_FIELD);
    GameWindow *label = GXWindowLook::exemplar("OptionsMenu.wnd:MusicVolumeLabel", GWS_STATIC_TEXT);
    GameWindow *title = GXWindowLook::byLabel(options, "GUI:Options");
    GXWindowLook::copyToAll(parent, GWS_PUSH_BUTTON, button, TRUE);
    GXWindowLook::copyToAll(parent, GWS_HORZ_SLIDER, slider, TRUE);
    GXWindowLook::copyToAll(parent, GWS_ENTRY_FIELD, entry, TRUE);
    GXWindowLook::copyToAll(parent, GWS_STATIC_TEXT, label, TRUE);

    for (GameWindow *child = parent->winGetChild(); child; child = child->winGetNext())
    {
        const UnsignedInt style = child->winGetStyle();
        if (BitIsSet(style, GWS_STATIC_TEXT))
        {
            GXWindowLook::copyFont(child, label);
            if (TextData *text = (TextData *)child->winGetUserData())
                text->centered = FALSE;
        }
    }
    const char *bottom[] = { "ButtonAccept", "ButtonDefaults", "ButtonBack" };
    for (const char *name : bottom)
        GXWindowLook::copyFont(gxExtrasChild(name), button);
    // The number fields, the small -/+ and the preset buttons keep the Extras layout's own size
    // (scaled for the screen) in Options' font family: Options' button font is made for buttons four
    // times their width, and Generals' text fields use an 8-point font (owner, a38: tiny digits).
    for (Int i = 0; i < 5; ++i)
    {
        AsciiString minus, plus; minus.format("Exact%dMinus", i); plus.format("Exact%dPlus", i);
        if (GameWindow *w = gxExtrasChild(minus.str())) GXWindowLook::copyFontFamily(w, entry ? entry : button);
        if (GameWindow *w = gxExtrasChild(plus.str())) GXWindowLook::copyFontFamily(w, entry ? entry : button);
    }
    for (Int i = 0; i < 4; ++i)
    {
        AsciiString preset; preset.format("CameraPreset%d", i);
        if (GameWindow *w = gxExtrasChild(preset.str())) GXWindowLook::copyFontFamily(w, entry ? entry : button);
    }
    if (GameWindow *ours = gxExtrasChild("LabelTitle"))
    {
        GXWindowLook::copy(ours, title ? title : label, TRUE);
        GXWindowLook::copyFont(ours, title ? title : label);
        if (TextData *text = (TextData *)ours->winGetUserData())
            text->centered = FALSE;
    }
}

// The first row stands a preset row's height above the others to leave room for the height presets;
// without them (the presets are hidden unless the Tactical camera is on) it moves down to the
// others' spacing, instead of leaving a gap.
static void gxExtrasCloseUpRows()
{
    GameWindow *preset = gxExtrasChild("CameraPreset0");
    GameWindow *first = gxExtrasChild("SliderMaxCameraHeight");
    GameWindow *second = gxExtrasChild("SliderMinCameraHeight");
    GameWindow *third = gxExtrasChild("SliderCameraPitch");
    if (!preset || !preset->winIsHidden() || !first || !second || !third)
        return;
    Int x, y1, y2, y3;
    first->winGetScreenPosition(&x, &y1);
    second->winGetScreenPosition(&x, &y2);
    third->winGetScreenPosition(&x, &y3);
    const Int shift = (y2 - y1) - (y3 - y2);
    if (shift <= 0)
        return;
    const char *row[] = { "LabelMaxCameraHeight", "SliderMaxCameraHeight", "Exact0Minus", "Exact0Value", "Exact0Plus" };
    for (const char *name : row)
    {
        GameWindow *window = gxExtrasChild(name);
        if (!window)
            continue;
        Int wx, wy;
        window->winGetPosition(&wx, &wy);
        window->winSetPosition(wx, wy + shift);
    }
}

// The panel encloses every window the popup shows, with a margin; the rules sit halfway between the
// title and the first row, and between the last row and the buttons.
static void gxExtrasPlacePanel(GameWindow *parent)
{
    if (!TheDisplay) return;
    const Int margin = TheDisplay->getHeight() * 12 / 600;
    Bool any = FALSE;
    IRegion2D box = { { 0, 0 }, { 0, 0 } };
    Int titleBottom = 0, rowsTop = TheDisplay->getHeight(), rowsBottom = 0, buttonsTop = TheDisplay->getHeight();
    GameWindow *title = gxExtrasChild("LabelTitle");
    GameWindow *accept = gxExtrasChild("ButtonAccept");
    Int acceptX = 0, acceptY = TheDisplay->getHeight();
    if (accept)
        accept->winGetScreenPosition(&acceptX, &acceptY);
    for (GameWindow *child = parent->winGetChild(); child; child = child->winGetNext())
    {
        if (child->winIsHidden())
            continue;
        Int x, y, w, h;
        child->winGetScreenPosition(&x, &y);
        child->winGetSize(&w, &h);
        if (!any) { box.lo.x = x; box.lo.y = y; box.hi.x = x + w; box.hi.y = y + h; any = TRUE; }
        box.lo.x = min(box.lo.x, x); box.lo.y = min(box.lo.y, y);
        box.hi.x = max(box.hi.x, x + w); box.hi.y = max(box.hi.y, y + h);
        if (child == title)
            titleBottom = y + h;
        else if (y >= acceptY)
            buttonsTop = min(buttonsTop, y);
        else
        {
            rowsTop = min(rowsTop, y);
            rowsBottom = max(rowsBottom, y + h);
        }
    }
    if (!any) return;
    s_extrasPanel.lo.x = box.lo.x - margin; s_extrasPanel.lo.y = box.lo.y - margin;
    s_extrasPanel.hi.x = box.hi.x + margin; s_extrasPanel.hi.y = box.hi.y + margin;
    s_extrasRuleTop = (titleBottom + rowsTop) / 2;
    s_extrasRuleBottom = (rowsBottom + buttonsTop) / 2;
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
    if (s_hiddenOptions) {
        s_hiddenOptions->winHide(FALSE);
        s_hiddenOptions = nullptr;
    }
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
    gxExtrasCloseUpRows();
    gxExtrasPlacePanel(parent);
    s_hiddenOptions = GXWindowLook::optionsRoot();
    if (s_hiddenOptions && !s_hiddenOptions->winIsHidden())
        s_hiddenOptions->winHide(TRUE);
    else
        s_hiddenOptions = nullptr;
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
        GadgetSliderSetPosition(sliderScrollSpeed, (Int)(pref->getScrollFactor() * 100.0f + 0.5f));
    if (sliderDrawDistance)
        GadgetSliderSetPosition(sliderDrawDistance, (Int)(pref->getTerrainDrawDistanceScale() * 100.0f + 0.5f));

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
	// GeneralsX @bugfix Android port 19/10/2026 The values GameData.ini gives (the game's or a mod's,
	// GXExtrasValues::iniCamera), not fixed numbers: realkot's 500 / 80 / 37 / 100 / 105 were neither
	// Zero Hour's nor Generals' own (owner, a37).
	const GXExtrasValues::CameraDefaults &ini = GXExtrasValues::iniCamera();
	const Real maxHeight = ini.known ? ini.maxHeight : TheGlobalData->m_maxCameraHeight;
	const Real minHeight = ini.known ? ini.minHeight : TheGlobalData->m_minCameraHeight;
	const Real pitch = ini.known ? ini.pitch : TheGlobalData->m_cameraPitch;
	const Real drawDistance = ini.known ? ini.drawDistanceScale : TheGlobalData->m_terrainDrawDistanceScale;
	if (sliderMaxCameraHeight)
		GadgetSliderSetPosition(sliderMaxCameraHeight, (Int)(maxHeight + 0.5f) * GX_EXTRA_CAMERA_SCALE);
	if (sliderMinCameraHeight)
		GadgetSliderSetPosition(sliderMinCameraHeight, (Int)(minHeight + 0.5f));
	if (sliderCameraPitch)
		GadgetSliderSetPosition(sliderCameraPitch, (Int)(pitch + 0.5f));
	if (sliderScrollSpeed)
		GadgetSliderSetPosition(sliderScrollSpeed, (Int)(TheGlobalData->m_keyboardDefaultScrollFactor * 100.0f + 0.5f));
	if (sliderDrawDistance)
		GadgetSliderSetPosition(sliderDrawDistance, (Int)(drawDistance * 100.0f + 0.5f));
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
