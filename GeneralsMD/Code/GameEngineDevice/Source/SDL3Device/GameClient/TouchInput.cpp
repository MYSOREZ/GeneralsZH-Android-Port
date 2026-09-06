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

/*
** TouchInput.cpp -- see TouchInput.h for why this exists.
*/

#include "SDL3Device/GameClient/TouchInput.h"

#include "Common/GameType.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/ThingTemplate.h"
#include "Common/GlobalData.h"
#include "GameClient/CommandXlat.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/InGameUI.h"
#include "GameClient/SelectionInfo.h"
#include "GameClient/SelectionXlat.h"
#include "GameClient/View.h"
#include "GameLogic/Object.h"

namespace
{

	/**
		The object under a screen point, as the ORDER path sees it.

		Mirrors what CommandXlat does before every evaluateContextCommand() call: pick with
		the context's pick types, then discard a corpse. A dead unit left in the pick result
		blocks positional orders -- you would aim past the wreck and the order would be read
		as "interact with the wreck" instead of "go there".
	*/
	Drawable *pickForOrder(const ICoord2D &pixel)
	{
		if (TheTacticalView == nullptr || TheInGameUI == nullptr)
			return nullptr;

		const Bool forceAttack = TheInGameUI->isInForceAttackMode();
		const UnsignedInt pickType = getPickTypesForContext(forceAttack);
		Drawable *draw = TheTacticalView->pickDrawable(&pixel, forceAttack, (PickType)pickType);

		const Object *obj = draw ? draw->getObject() : nullptr;
		if (obj == nullptr || (obj->isEffectivelyDead() && !obj->isKindOf(KINDOF_ALWAYS_SELECTABLE)))
			return nullptr;

		return draw;
	}

	/// The object under a screen point that the player could SELECT, or null.
	Drawable *pickForSelection(const ICoord2D &pixel)
	{
		if (TheTacticalView == nullptr)
			return nullptr;

		Drawable *draw = TheTacticalView->pickDrawable(&pixel, FALSE, PICK_TYPE_SELECTABLE);
		if (!CanSelectDrawable(draw, FALSE))
			return nullptr;

		return draw;
	}

	/// Replace the whole selection with this one drawable, and tell the logic about it.
	void selectOnly(Drawable *draw)
	{
		if (draw == nullptr || TheInGameUI == nullptr)
			return;

		TheInGameUI->deselectAllDrawables();
		TheInGameUI->selectDrawable(draw);

		// The selection itself is game state, not a local highlight: it travels over the
		// network and into replays as MSG_CREATE_SELECTED_GROUP_NO_SOUND. Skipping this
		// would make the unit look selected and take no orders.
		Object *obj = draw->getObject();
		if (obj != nullptr && TheMessageStream != nullptr)
		{
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND);
			msg->appendBooleanArgument(TRUE);
			msg->appendObjectIDArgument(obj->getID());
		}
	}

	/**
		Does the current selection have a MORE SPECIFIC interaction with this object than
		simply moving to where it stands?

		This is the one genuinely ambiguous tap on a touchscreen. Tapping something the
		player owns usually means "select that instead". But tapping your own transport
		while infantry is selected means "get in", and tapping your own damaged tank while
		a repair vehicle is selected means "repair it" -- and on a mouse those are separate
		buttons, which a finger does not have.

		Rather than guess, ask the engine. evaluateContextCommand in EVALUATE_ONLY mode
		returns the order that WOULD be issued without issuing it. If that order is a plain
		move (or nothing), the tap carries no special meaning and is a selection. If it is
		anything else, the engine has already decided the two objects interact, and that is
		what the player meant.
	*/
	Bool selectionInteractsWith(Drawable *draw, const Coord3D &pos)
	{
		if (draw == nullptr || TheGameClient == nullptr)
			return FALSE;

		const GameMessage::Type t =
			TheGameClient->evaluateContextCommand(draw, &pos, CommandTranslator::EVALUATE_ONLY);

		switch (t)
		{
			case GameMessage::MSG_INVALID:
			case GameMessage::MSG_DO_MOVETO:
			case GameMessage::MSG_DO_ATTACKMOVETO:
				return FALSE;
			default:
				return TRUE;
		}
	}

	/// Issue the context order for a point, exactly as the click path does.
	void issueContextOrder(Drawable *draw, const Coord3D &pos)
	{
		if (TheGameClient == nullptr || TheInGameUI == nullptr)
			return;

		// No force-attack branch: force attack is the Ctrl modifier on a click, and a
		// finger has no modifier keys. If a keyboard is ever attached, its own click path
		// still handles it -- this module only ever sees touches.
		TheGameClient->evaluateContextCommand(draw, &pos, CommandTranslator::DO_COMMAND);

		TheInGameUI->clearAttackMoveToMode();
	}

}  // anonymous namespace

namespace TouchInput
{

	Bool hasArmedCommand()
	{
		return (TheInGameUI != nullptr && TheInGameUI->getGUICommand() != nullptr);
	}

	Bool hasControllableSelection()
	{
		return (TheInGameUI != nullptr && TheInGameUI->areSelectedObjectsControllable());
	}

	//-------------------------------------------------------------------------------------
	Bool skipMovieIfPlaying()
	{
		// GeneralsX @bugfix Android port 06/09/2026 Reported: the intro could no longer be
		// skipped by tapping, only with the system Back gesture.
		//
		// Skipping a movie was never handled by the translator chain at all -- WindowXlat
		// watches for a raw MSG_RAW_MOUSE_LEFT_BUTTON_DOWN that nothing else consumed and
		// calls stopMovie() on it (WindowXlat.cpp, the movie branch). The moment battlefield
		// taps stopped synthesizing that button, the only way to reach that branch was gone.
		// Handled here instead, before anything else: while a movie is up, the only thing a
		// tap anywhere can mean is "skip it".
		if (TheDisplay != nullptr && TheDisplay->isMoviePlaying() &&
				TheGlobalData != nullptr && TheGlobalData->m_allowExitOutOfMovies)
		{
			TheDisplay->stopMovie();
			return TRUE;
		}
		return FALSE;
	}

	//-------------------------------------------------------------------------------------
	void beginAiming(Int x, Int y)
	{
		// GeneralsX @bugfix Android port 06/09/2026 Reported: no circle when aiming an
		// ability -- the satellite scan simply went off with no radius shown.
		//
		// The radius decal is created by setRadiusCursor(), which on the mouse path is
		// called from createCommandHint() every time a MSG_RAW_MOUSE_POSITION produces a
		// fresh hint. Native aiming sends no position messages, so no hint was ever
		// created, so setRadiusCursor() was never called and m_curRadiusCursor stayed
		// empty. Create it here, at the moment a finger actually starts aiming -- which is
		// also the right moment: not when the button is pressed (there is nowhere to draw
		// it yet) and not on every motion (it only needs creating once).
		if (TheInGameUI == nullptr)
			return;

		const CommandButton *command = TheInGameUI->getGUICommand();
		if (command == nullptr)
			return;

		TheInGameUI->setRadiusCursor(command->getRadiusCursorType(),
																 command->getSpecialPowerTemplate(),
																 command->getWeaponSlot());
		TheInGameUI->setTouchAimPoint(x, y, armedTargetValid(x, y));
	}

	//-------------------------------------------------------------------------------------
	void tap(Int x, Int y)
	{
		if (skipMovieIfPlaying())
			return;

		if (TheInGameUI == nullptr || TheTacticalView == nullptr)
			return;

		ICoord2D pixel;
		pixel.x = x;
		pixel.y = y;

		// 1. An armed command owns the tap outright.
		if (hasArmedCommand())
		{
			fireArmed(x, y);
			return;
		}

		Coord3D pos;
		const Bool onTerrain = TheTacticalView->screenToTerrain(&pixel, &pos);

		// 2. Something of the player's own under the finger.
		Drawable *selectable = pickForSelection(pixel);
		if (selectable != nullptr)
		{
			const Object *obj = selectable->getObject();
			const Bool isOwn = (obj != nullptr && obj->isLocallyControlled());

			if (isOwn)
			{
				const Bool interacts = onTerrain && hasControllableSelection() &&
															 !selectable->isSelected() &&
															 selectionInteractsWith(selectable, pos);
				if (!interacts)
				{
					selectOnly(selectable);
					return;
				}
				// else: fall through and let it be an order onto that object
			}
		}

		if (!onTerrain)
			return;

		// 3. An order, if there is anything to order.
		if (hasControllableSelection())
		{
			issueContextOrder(pickForOrder(pixel), pos);
			return;
		}

		// 4. Nothing selectable, nothing to order: a tap on empty ground clears the
		//    selection, the same as a click on empty ground does.
		if (selectable != nullptr)
			selectOnly(selectable);
		else
			TheInGameUI->deselectAllDrawables();
	}

	//-------------------------------------------------------------------------------------
	void doubleTap(Int x, Int y)
	{
		if (skipMovieIfPlaying())
			return;

		if (TheInGameUI == nullptr || TheTacticalView == nullptr)
			return;

		if (hasArmedCommand())
		{
			fireArmed(x, y);
			return;
		}

		ICoord2D pixel;
		pixel.x = x;
		pixel.y = y;

		Drawable *picked = pickForSelection(pixel);
		Object *obj = picked ? picked->getObject() : nullptr;

		// Only the player's own, mass-selectable units answer to this. Anything else falls
		// back to an ordinary tap so a double tap is never worse than a single one.
		if (picked == nullptr || obj == nullptr || !obj->isLocallyControlled() ||
				!picked->isMassSelectable())
		{
			tap(x, y);
			return;
		}

		selectOnly(picked);
		TheInGameUI->selectMatchingAcrossScreen();
	}

	//-------------------------------------------------------------------------------------
	Bool armedTargetValid(Int x, Int y)
	{
		if (!hasArmedCommand() || TheGameClient == nullptr || TheTacticalView == nullptr)
			return FALSE;

		ICoord2D pixel;
		pixel.x = x;
		pixel.y = y;

		Coord3D pos;
		if (!TheTacticalView->screenToTerrain(&pixel, &pos))
			return FALSE;

		// EVALUATE_ONLY: the same question the engine asks itself to pick between the
		// valid and invalid cursor art, asked directly instead of read back off a bitmap.
		const GameMessage::Type t =
			TheGameClient->evaluateContextCommand(pickForOrder(pixel), &pos, CommandTranslator::EVALUATE_ONLY);

		return (t != GameMessage::MSG_INVALID);
	}

	//-------------------------------------------------------------------------------------
	void fireArmed(Int x, Int y)
	{
		if (TheTacticalView == nullptr || TheInGameUI == nullptr)
			return;

		ICoord2D pixel;
		pixel.x = x;
		pixel.y = y;

		Coord3D pos;
		if (!TheTacticalView->screenToTerrain(&pixel, &pos))
			return;

		issueContextOrder(pickForOrder(pixel), pos);

		// The aim is spent: drop the radius decal and forget the point, so neither is left
		// hanging over the map after the ability goes off.
		TheInGameUI->setRadiusCursorNone();
		TheInGameUI->clearTouchAimPoint();
	}

	//-------------------------------------------------------------------------------------
	void reportUiHold(Int x, Int y, Bool held)
	{
		if (TheControlBar != nullptr)
			TheControlBar->setTouchHoldPoint(x, y, held);
	}

	//-------------------------------------------------------------------------------------
	void cancelOrDeselect()
	{
		if (TheInGameUI == nullptr)
			return;

		// Same precedence a right-click has, minus the right-click: back out of the most
		// recent commitment first, and only clear the selection if there is nothing to
		// back out of.
		if (TheInGameUI->getGUICommand() != nullptr)
		{
			TheInGameUI->setGUICommand(nullptr);
			return;
		}

		if (TheInGameUI->getPendingPlaceSourceObjectID() != INVALID_ID)
		{
			TheInGameUI->placeBuildAvailable(nullptr, nullptr);
			TheInGameUI->setPreventLeftClickDeselectionInAlternateMouseModeForOneClick(FALSE);
			return;
		}

		TheInGameUI->deselectAllDrawables();
	}

}  // namespace TouchInput
