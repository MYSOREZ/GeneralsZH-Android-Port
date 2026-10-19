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

// GeneralsX @feature Android port 19/10/2026 A window we add (the Extras popup, the Extras button on
// Options) takes its look from a window the game itself defines, instead of colours written here.
// The exemplar comes from the live Options layout, so whatever art and colours the game -- or a mod
// that restyles the interface -- gives its own Options screen, ours get the same: image or colour
// draw data for all three states, the text colours, the draw function chosen for that kind of
// gadget, and the font family (our sizes are kept, they are scaled for the screen).

#pragma once

#include "GameClient/Gadget.h"
#include "GameClient/GameFont.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "Common/NameKeyGenerator.h"

namespace GXWindowLook
{
	inline GameWindow *byName(const char *name)
	{
		if (!TheWindowManager || !TheNameKeyGenerator)
			return nullptr;
		return TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(name));
	}

	/// The first window under root (depth first) of the given gadget style, shown ones first.
	inline GameWindow *byStyle(GameWindow *root, UnsignedInt style, Bool allowHidden = FALSE)
	{
		for (GameWindow *w = root ? root->winGetChild() : nullptr; w; w = w->winGetNext())
		{
			if (!allowHidden && w->winIsHidden())
				continue;
			if (BitIsSet(w->winGetStyle(), style))
				return w;
			if (GameWindow *found = byStyle(w, style, allowHidden))
				return found;
		}
		return (root && !allowHidden) ? byStyle(root, style, TRUE) : nullptr;
	}

	/// The first window under root whose text label (the .wnd's TEXT, e.g. "GUI:Options") is label.
	inline GameWindow *byLabel(GameWindow *root, const char *label)
	{
		for (GameWindow *w = root ? root->winGetChild() : nullptr; w; w = w->winGetNext())
		{
			if (w->winGetInstanceData()->m_textLabelString.compareNoCase(label) == 0)
				return w;
			if (GameWindow *found = byLabel(w, label))
				return found;
		}
		return nullptr;
	}

	/// The topmost window of the live Options layout, or null when Options is not up.
	inline GameWindow *optionsRoot()
	{
		GameWindow *root = byName("OptionsMenu.wnd:OptionsMenuParent");
		while (root && root->winGetParent())
			root = root->winGetParent();
		return root;
	}

	/// The named window if it exists, else the first one of that style in the Options layout.
	inline GameWindow *exemplar(const char *name, UnsignedInt style)
	{
		if (GameWindow *w = byName(name))
			return w;
		return style ? byStyle(optionsRoot(), style) : nullptr;
	}

	/// The exemplar's font itself, size included: text then reads at the size the game's own
	/// screen uses for that kind of text.
	inline void copyFont(GameWindow *to, GameWindow *from)
	{
		if (to && from && from->winGetFont())
			to->winSetFont(from->winGetFont());
	}

	inline void copyFontFamily(GameWindow *to, GameWindow *from)
	{
		GameFont *ours = to->winGetFont();
		GameFont *theirs = from->winGetFont();
		if (!ours || !theirs || !TheFontLibrary || ours->nameString == theirs->nameString)
			return;
		if (GameFont *font = TheFontLibrary->getFont(theirs->nameString, ours->pointSize, ours->bold))
			to->winSetFont(font);
	}

	/// Text colours and font family; with drawData also the state images/colours, the draw function
	/// and the IMAGE status bit, and then the same for the children in order (a slider's thumb).
	inline void copy(GameWindow *to, GameWindow *from, Bool drawData)
	{
		if (!to || !from || to == from)
			return;
		WinInstanceData *dst = to->winGetInstanceData();
		WinInstanceData *src = from->winGetInstanceData();
		dst->m_enabledText = src->m_enabledText;
		dst->m_disabledText = src->m_disabledText;
		dst->m_hiliteText = src->m_hiliteText;
		copyFontFamily(to, from);
		if (!drawData)
			return;
		for (Int i = 0; i < MAX_DRAW_DATA; ++i)
		{
			dst->m_enabledDrawData[i] = src->m_enabledDrawData[i];
			dst->m_disabledDrawData[i] = src->m_disabledDrawData[i];
			dst->m_hiliteDrawData[i] = src->m_hiliteDrawData[i];
		}
		if (BitIsSet(from->winGetStatus(), WIN_STATUS_IMAGE))
			to->winSetStatus(WIN_STATUS_IMAGE);
		else
			to->winClearStatus(WIN_STATUS_IMAGE);
		to->winSetDrawFunc(from->winGetDrawFunc());
		GameWindow *child = to->winGetChild();
		GameWindow *model = from->winGetChild();
		for (; child && model; child = child->winGetNext(), model = model->winGetNext())
			copy(child, model, TRUE);
	}

	/// Every gadget of the given style under root takes the exemplar's look.
	inline void copyToAll(GameWindow *root, UnsignedInt style, GameWindow *from, Bool drawData)
	{
		if (!root || !from)
			return;
		for (GameWindow *w = root->winGetChild(); w; w = w->winGetNext())
		{
			if (BitIsSet(w->winGetStyle(), style))
				copy(w, from, drawData);
			else
				copyToAll(w, style, from, drawData);
		}
	}
}
