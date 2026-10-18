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
** SDL3Keyboard.cpp
**
** SDL3-based keyboard implementation for Linux builds.
**
** TheSuperHackers @feature CnC_Generals_Linux 10/02/2026 Bender
** Replaces Win32DIKeyboard with SDL3 keyboard APIs for Linux.
*/

#ifndef _WIN32

// GeneralsX @bugfix BenderAI 13/02/2026 Fix include path (fighter19 pattern)
#include "SDL3Device/GameClient/SDL3Keyboard.h"
#include <cstdio>
#include <cstring>
#include "Common/MessageStream.h"

/**
 * Constructor
 */
SDL3Keyboard::SDL3Keyboard(void)
	: Keyboard(),
	  m_nextFreeIndex(0),
	  m_nextGetIndex(0)
{
	// Initialize event buffer with SDL_EVENT_FIRST sentinel
	// GeneralsX @refactor felipebraz 16/02/2026 Fighter19 pattern
	memset(m_eventBuffer, 0, sizeof(m_eventBuffer));
}

/**
 * Destructor
 */
SDL3Keyboard::~SDL3Keyboard(void)
{
	// destructor
}

/**
 * Initialize keyboard subsystem
 */
void SDL3Keyboard::init(void)
{
	// Call parent init
	Keyboard::init();
	
	// Clear event buffer - Fighter19 pattern
	// GeneralsX @refactor felipebraz 16/02/2026
	memset(m_eventBuffer, 0, sizeof(m_eventBuffer));
	m_nextFreeIndex = 0;
	m_nextGetIndex = 0;

	// init complete
}

/**
 * Reset keyboard to default state
 */
void SDL3Keyboard::reset(void)
{
	Keyboard::reset();
	// GeneralsX @bugfix Codex 14/10/2026 The base reset() is empty; release its held-key cache too.
    if (TheMessageStream) {
        // Release all held bindings before clearing the base cache, including camera keys.
        for (Int i = 1; i < KEY_COUNT; ++i) {
            if ((m_keyStatus[i].state & KEY_STATE_DOWN) && i != KEY_LALT && i != KEY_RALT) {
                GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_RAW_KEY_UP);
                msg->appendIntegerArgument(i);
                msg->appendIntegerArgument(KEY_STATE_UP);
            }
        }
        resetKeys();
    } else {
        memset(m_keys, 0, sizeof(m_keys));
        memset(m_keyStatus, 0, sizeof(m_keyStatus));
        m_modifiers = 0;
    }
	
	// Clear event buffer - Fighter19 pattern
	// GeneralsX @refactor felipebraz 16/02/2026
	memset(m_eventBuffer, 0, sizeof(m_eventBuffer));
	m_nextFreeIndex = 0;
	m_nextGetIndex = 0;
}

/**
 * Update keyboard state (called per-frame)
 */
void SDL3Keyboard::update(void)
{
	// Call parent update (processes events from getKey())
	Keyboard::update();
}

/**
 * Get keyboard data structure
 * Called by game systems to query keyboard state
 */
KeyboardIO *SDL3Keyboard::getKeyboard(void)
{
	// GeneralsX @bugfix Codex 14/10/2026 Expose the base class event list, not a second state cache.
	return getFirstKey();
}

/**
 * Get Caps Lock state from SDL, including keyboards attached after startup.
 */
Bool SDL3Keyboard::getCapsState(void)
{
	return (SDL_GetModState() & SDL_KMOD_CAPS) != 0;
}

/**
 * Get next keyboard event (called by parent Keyboard::update())
 * Fighter19 pattern: check for SDL_EVENT_FIRST sentinel (means empty)
 */
void SDL3Keyboard::getKey(KeyboardIO *key)
{
    while (m_eventBuffer[m_nextGetIndex].type != SDL_EVENT_FIRST &&
           translateScanCodeToKeyVal(m_eventBuffer[m_nextGetIndex].key.scancode) == KEY_NONE) {
        m_eventBuffer[m_nextGetIndex].type = SDL_EVENT_FIRST;
        m_nextGetIndex = (m_nextGetIndex + 1) % MAX_SDL3_KEY_EVENTS;
    }
	// Check if buffer is empty: if event type is SDL_EVENT_FIRST (0), slot is empty
	// GeneralsX @refactor felipebraz 16/02/2026 Fighter19 pattern: sentinel check
	if (m_eventBuffer[m_nextGetIndex].type == SDL_EVENT_FIRST) {
		key->key = KEY_NONE;
		key->status = KeyboardIO::STATUS_UNUSED;
		key->state = 0;
		key->keyDownTimeMsec = 0;
		return;
	}
	
	// Only handle keyboard events
	if (m_eventBuffer[m_nextGetIndex].type != SDL_EVENT_KEY_DOWN &&
	    m_eventBuffer[m_nextGetIndex].type != SDL_EVENT_KEY_UP) {
		// Mark as processed (sentinel)
		m_eventBuffer[m_nextGetIndex].type = SDL_EVENT_FIRST;
		// Advance read position
		m_nextGetIndex = (m_nextGetIndex + 1) % MAX_SDL3_KEY_EVENTS;
		// Return empty
		key->key = KEY_NONE;
		key->status = KeyboardIO::STATUS_UNUSED;
		key->state = 0;
		key->keyDownTimeMsec = 0;
		return;
	}
	
	// Get keyboard event from buffer
	const SDL_KeyboardEvent& keyEvent = m_eventBuffer[m_nextGetIndex].key;
	
	// Translate SDL3 key code to game KeyDefType
	KeyDefType keyDef = translateScanCodeToKeyVal(keyEvent.scancode);
	
	key->key = keyDef;
	key->status = KeyboardIO::STATUS_UNUSED;
	key->state = keyEvent.down ? KEY_STATE_DOWN : KEY_STATE_UP;
	// GeneralsX @bugfix felipebraz 01/04/2026 Use the same timer domain as Keyboard::checkKeyRepeat() to avoid immediate duplicate repeats.
	key->keyDownTimeMsec = keyEvent.down ? timeGetTime() : 0;
	
	// Mark this slot as empty (sentinel)
	m_eventBuffer[m_nextGetIndex].type = SDL_EVENT_FIRST;
	
	// Advance read position (circular buffer)
	m_nextGetIndex = (m_nextGetIndex + 1) % MAX_SDL3_KEY_EVENTS;
}

/**
 * Add raw SDL_Event to keyboard event buffer
 * Fighter19 pattern: unified method that accepts any SDL_Event
 * Called by SDL3GameEngine::serviceWindowsOS() directly from event loop
 *
 * @param event Raw SDL_Event from SDL_PollEvent()
 *
 * GeneralsX @refactor felipebraz 16/02/2026 Fighter19 event model
 */
void SDL3Keyboard::addSDLEvent(SDL_Event *event)
{
	if (!event) {
		return;
	}
	
	// Filter only keyboard-related events
	if (event->type != SDL_EVENT_KEY_DOWN && event->type != SDL_EVENT_KEY_UP) {
		return;  // Not a keyboard event, ignore
	}

	// GeneralsX @bugfix felipebraz 01/04/2026 SDL key-repeat is redundant with engine repeat and can duplicate edits like Backspace.
	if (event->type == SDL_EVENT_KEY_DOWN && event->key.repeat) {
		return;
	}
	
	// Check if buffer is full
	UnsignedInt nextFreeIndex = (m_nextFreeIndex + 1) % MAX_SDL3_KEY_EVENTS;
	if (nextFreeIndex == m_nextGetIndex) {
		// GeneralsX @bugfix Codex 14/10/2026 Overflow must not lose a release and leave a key held.
		reset();
		nextFreeIndex = (m_nextFreeIndex + 1) % MAX_SDL3_KEY_EVENTS;
	}
	
	// Copy entire event to buffer
	m_eventBuffer[m_nextFreeIndex] = *event;
	
	// Advance write position (circular buffer)
	m_nextFreeIndex = nextFreeIndex;
}

/**
 * Add SDL3 keyboard event to buffer (LEGACY - kept for compatibility)
 * Called by SDL3GameEngine::handleKeyboardEvent()
 * TODO: Remove after transition to addSDLEvent() is complete
 */
void SDL3Keyboard::addSDL3KeyEvent(const SDL_KeyboardEvent& event)
{
	// Create SDL_Event wrapper around SDL_KeyboardEvent
	SDL_Event sdlEvent;
	memset(&sdlEvent, 0, sizeof(sdlEvent));
	sdlEvent.type = event.down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
	sdlEvent.key = event;
	
	// Add to buffer using unified method
	addSDLEvent(&sdlEvent);
}

/**
 * Translate SDL3 scancode to game KeyDefType
 * Physical keys use retail DirectInput values; Unicode text travels separately through the IME.
 */
KeyVal SDL3Keyboard::translateScanCodeToKeyVal(SDL_Scancode scan)
{
	// Quick mapping for essential keys
	// GeneralsX @bugfix felipebraz 01/04/2026 Restore editing/navigation keys required by GUI widgets.
	switch (scan) {
		// GeneralsX @feature Codex 14/10/2026 Complete the retail keyboard's punctuation and keypad.
		case SDL_SCANCODE_INSERT: return KEY_INS;
		case SDL_SCANCODE_MINUS: return KEY_MINUS;
		case SDL_SCANCODE_EQUALS: return KEY_EQUAL;
		case SDL_SCANCODE_LEFTBRACKET: return KEY_LBRACKET;
		case SDL_SCANCODE_RIGHTBRACKET: return KEY_RBRACKET;
		case SDL_SCANCODE_SEMICOLON: return KEY_SEMICOLON;
		case SDL_SCANCODE_APOSTROPHE: return KEY_APOSTROPHE;
		case SDL_SCANCODE_GRAVE: return KEY_TICK;
		case SDL_SCANCODE_BACKSLASH: return KEY_BACKSLASH;
		case SDL_SCANCODE_NONUSBACKSLASH: return KEY_102;
		case SDL_SCANCODE_COMMA: return KEY_COMMA;
		case SDL_SCANCODE_PERIOD: return KEY_PERIOD;
		case SDL_SCANCODE_SLASH: return KEY_SLASH;
		case SDL_SCANCODE_CAPSLOCK: return KEY_CAPS;
		case SDL_SCANCODE_NUMLOCKCLEAR: return KEY_NUM;
		case SDL_SCANCODE_SCROLLLOCK: return KEY_SCROLL;
		case SDL_SCANCODE_PRINTSCREEN: return KEY_SYSREQ;
		case SDL_SCANCODE_KP_0: return KEY_KP0;
		case SDL_SCANCODE_KP_1: return KEY_KP1;
		case SDL_SCANCODE_KP_2: return KEY_KP2;
		case SDL_SCANCODE_KP_3: return KEY_KP3;
		case SDL_SCANCODE_KP_4: return KEY_KP4;
		case SDL_SCANCODE_KP_5: return KEY_KP5;
		case SDL_SCANCODE_KP_6: return KEY_KP6;
		case SDL_SCANCODE_KP_7: return KEY_KP7;
		case SDL_SCANCODE_KP_8: return KEY_KP8;
		case SDL_SCANCODE_KP_9: return KEY_KP9;
		case SDL_SCANCODE_KP_PERIOD: return KEY_KPDEL;
		case SDL_SCANCODE_KP_MULTIPLY: return KEY_KPSTAR;
		case SDL_SCANCODE_KP_MINUS: return KEY_KPMINUS;
		case SDL_SCANCODE_KP_PLUS: return KEY_KPPLUS;
		case SDL_SCANCODE_KP_DIVIDE: return KEY_KPSLASH;
		case SDL_SCANCODE_ESCAPE: return KEY_ESC;      // GeneralsX @bugfix BenderAI 13/02/2026 Fix key constant name
		// GeneralsX @bugfix Android port 08/07/2026 Phone Back button/gesture ->
		// same key that already opens the in-game pause menu.
		case SDL_SCANCODE_AC_BACK: return KEY_ESC;
		case SDL_SCANCODE_RETURN: return KEY_ENTER;    // GeneralsX @bugfix BenderAI 13/02/2026 Fix key constant name
		case SDL_SCANCODE_KP_ENTER: return KEY_KPENTER;
		case SDL_SCANCODE_SPACE: return KEY_SPACE;
		case SDL_SCANCODE_TAB: return KEY_TAB;
		case SDL_SCANCODE_BACKSPACE: return KEY_BACKSPACE;
		case SDL_SCANCODE_DELETE: return KEY_DEL;
		case SDL_SCANCODE_HOME: return KEY_HOME;
		case SDL_SCANCODE_END: return KEY_END;
		case SDL_SCANCODE_PAGEUP: return KEY_PGUP;
		case SDL_SCANCODE_PAGEDOWN: return KEY_PGDN;
		case SDL_SCANCODE_LSHIFT: return KEY_LSHIFT;
		case SDL_SCANCODE_RSHIFT: return KEY_RSHIFT;
		case SDL_SCANCODE_LCTRL: return KEY_LCTRL;     // GeneralsX @bugfix BenderAI 13/02/2026 Fix key constant name
		case SDL_SCANCODE_RCTRL: return KEY_RCTRL;     // GeneralsX @bugfix BenderAI 13/02/2026 Fix key constant name
		case SDL_SCANCODE_LALT: return KEY_LALT;       // GeneralsX @bugfix BenderAI 13/02/2026 Fix key constant name
		case SDL_SCANCODE_RALT: return KEY_RALT;       // GeneralsX @bugfix BenderAI 13/02/2026 Fix key constant name
		
		// Arrow keys
		case SDL_SCANCODE_UP: return KEY_UP;
		case SDL_SCANCODE_DOWN: return KEY_DOWN;
		case SDL_SCANCODE_LEFT: return KEY_LEFT;
		case SDL_SCANCODE_RIGHT: return KEY_RIGHT;
		
		// Function keys
		case SDL_SCANCODE_F1: return KEY_F1;
		case SDL_SCANCODE_F2: return KEY_F2;
		case SDL_SCANCODE_F3: return KEY_F3;
		case SDL_SCANCODE_F4: return KEY_F4;
		case SDL_SCANCODE_F5: return KEY_F5;
		case SDL_SCANCODE_F6: return KEY_F6;
		case SDL_SCANCODE_F7: return KEY_F7;
		case SDL_SCANCODE_F8: return KEY_F8;
		case SDL_SCANCODE_F9: return KEY_F9;
		case SDL_SCANCODE_F10: return KEY_F10;
		case SDL_SCANCODE_F11: return KEY_F11;
		case SDL_SCANCODE_F12: return KEY_F12;
		
		// Numbers
		case SDL_SCANCODE_1: return KEY_1;
		case SDL_SCANCODE_2: return KEY_2;
		case SDL_SCANCODE_3: return KEY_3;
		case SDL_SCANCODE_4: return KEY_4;
		case SDL_SCANCODE_5: return KEY_5;
		case SDL_SCANCODE_6: return KEY_6;
		case SDL_SCANCODE_7: return KEY_7;
		case SDL_SCANCODE_8: return KEY_8;
		case SDL_SCANCODE_9: return KEY_9;
		case SDL_SCANCODE_0: return KEY_0;
		
		// Letters (A-Z)
		case SDL_SCANCODE_A: return KEY_A;
		case SDL_SCANCODE_B: return KEY_B;
		case SDL_SCANCODE_C: return KEY_C;
		case SDL_SCANCODE_D: return KEY_D;
		case SDL_SCANCODE_E: return KEY_E;
		case SDL_SCANCODE_F: return KEY_F;
		case SDL_SCANCODE_G: return KEY_G;
		case SDL_SCANCODE_H: return KEY_H;
		case SDL_SCANCODE_I: return KEY_I;
		case SDL_SCANCODE_J: return KEY_J;
		case SDL_SCANCODE_K: return KEY_K;
		case SDL_SCANCODE_L: return KEY_L;
		case SDL_SCANCODE_M: return KEY_M;
		case SDL_SCANCODE_N: return KEY_N;
		case SDL_SCANCODE_O: return KEY_O;
		case SDL_SCANCODE_P: return KEY_P;
		case SDL_SCANCODE_Q: return KEY_Q;
		case SDL_SCANCODE_R: return KEY_R;
		case SDL_SCANCODE_S: return KEY_S;
		case SDL_SCANCODE_T: return KEY_T;
		case SDL_SCANCODE_U: return KEY_U;
		case SDL_SCANCODE_V: return KEY_V;
		case SDL_SCANCODE_W: return KEY_W;
		case SDL_SCANCODE_X: return KEY_X;
		case SDL_SCANCODE_Y: return KEY_Y;
		case SDL_SCANCODE_Z: return KEY_Z;
		
		default:
			// No retail binding for this key (media/system keys, for example).
			return KEY_NONE;
	}
}

#endif // !_WIN32
