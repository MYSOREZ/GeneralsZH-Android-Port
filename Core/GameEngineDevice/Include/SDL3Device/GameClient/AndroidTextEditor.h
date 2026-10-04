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
** AndroidTextEditor.h
**
** GeneralsX @feature Android port 02/10/2026 Text fields edited in a real Android EditText.
**
** The game's entry gadget only appends characters and deletes the last one: no cursor, no
** selection, no copy or paste. On a phone that makes chat painful -- a typo three words back
** means deleting the three words, and a link or a name cannot be pasted at all. Rewriting the
** gadget would change every text field of the retail UI; instead, while an entry field has the
** focus and the player has tapped it, an EditText bar is shown at the top of the screen (the
** keyboard covers the bottom) and the entry field becomes its mirror:
**
**   - the bar opens with the field's current text, limited to the field's length and kind
**     (password, digits only);
**   - every change in the bar replaces the field's text through the field's own character
**     path (GWM_IME_CHAR), so its filters and its owner's GEM_UPDATE_TEXT behave as if typed;
**   - "Done"/Enter in the bar sends the field the VK_RETURN that ends an edit (chat sends);
**   - Back or the field losing focus closes the bar.
**
** Calls from Java arrive on the UI thread and are only stored here; pump() applies them on the
** game thread once per frame.
*/

#pragma once

class GameWindow;

namespace AndroidTextEditor
{
	/// Show the bar for this entry field (or retarget it when another field took focus).
	/// False when the launcher has no bar (an APK older than this engine, updated over the air):
	/// the caller then falls back to SDL's own text input.
	bool open(GameWindow* field);
	/// Hide the bar if it is shown.
	void close();
	/// The field the bar currently edits, or nullptr.
	GameWindow* field();
	/// Apply what the bar sent since the last frame. Call once per frame on the game thread.
	void pump();
}
