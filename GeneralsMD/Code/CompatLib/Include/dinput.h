#pragma once

// GeneralsX @bugfix Android port 16/09/2026 Same shadowing issue as
// windows.h/mmsystem.h/d3dx8core.h/d3dx8math.h: MinGW-w64 ships a real,
// complete dinput.h (DIERR_*, IDirectInput8, DIDEVICEOBJECTDATA, etc. -- this
// shim only forward-declares DIRECTINPUT8/DIRECTINPUTDEVICE8 with no bodies),
// and this shim's directory sits earlier on the include search path, so the
// real one is never reached on a real _WIN32 build. Win32DIKeyboard.h already
// does the standard #define DIRECTINPUT_VERSION 0x800 before #include
// <dinput.h> -- #include_next lets that reach the real header.
#ifdef _WIN32

#include_next <dinput.h>

#else

enum DInputKeys
{
  // keypad keys ----------------------------------------------------------------
  DIK_NUMPAD0,
  DIK_NUMPAD1,
  DIK_NUMPAD2,
  DIK_NUMPAD3,
  DIK_NUMPAD4,
  DIK_NUMPAD5,
  DIK_NUMPAD6,
  DIK_NUMPAD7,
  DIK_NUMPAD8,
  DIK_NUMPAD9,
  DIK_NUMPADPERIOD,
  DIK_NUMPADSTAR,
  DIK_NUMPADMINUS,
  DIK_NUMPADPLUS,
  DIK_ESCAPE,
  DIK_BACK,
  DIK_RETURN,
  DIK_SPACE,
  DIK_TAB,

  DIK_F1,
  DIK_F2,
  DIK_F3,
  DIK_F4,
  DIK_F5,
  DIK_F6,
  DIK_F7,
  DIK_F8,
  DIK_F9,
  DIK_F10,
  DIK_F11,
  DIK_F12,
  DIK_A,
  DIK_B,
  DIK_C,
  DIK_D,
  DIK_E,
  DIK_F,
  DIK_G,
  DIK_H,
  DIK_I,
  DIK_J,
  DIK_K,
  DIK_L,
  DIK_M,
  DIK_N,
  DIK_O,
  DIK_P,
  DIK_Q,
  DIK_R,
  DIK_S,
  DIK_T,
  DIK_U,
  DIK_V,
  DIK_W,
  DIK_X,
  DIK_Y,
  DIK_Z,
  DIK_1,
  DIK_2,
  DIK_3,
  DIK_4,
  DIK_5,
  DIK_6,
  DIK_7,
  DIK_8,
  DIK_9,
  DIK_0,
  DIK_MINUS,
  DIK_EQUALS,
  DIK_LBRACKET,
  DIK_RBRACKET,
  DIK_SEMICOLON,
  DIK_APOSTROPHE,
  DIK_GRAVE,
  DIK_BACKSLASH,
  DIK_COMMA,
  DIK_PERIOD,
  DIK_SLASH,

  // special keys ---------------------------------------------------------------
  DIK_SYSRQ,

  DIK_CAPSLOCK,
  DIK_NUMLOCK,
  DIK_SCROLL,
  DIK_LCONTROL,
  DIK_LALT,
  DIK_LSHIFT,
  DIK_RSHIFT,

  DIK_UPARROW,
  DIK_DOWNARROW,
  DIK_LEFTARROW,
  DIK_RIGHTARROW,
  DIK_RALT,
  DIK_RCONTROL,
  DIK_HOME,
  DIK_END,
  DIK_PGUP,
  DIK_PGDN,
  DIK_INSERT,
  DIK_DELETE,
  DIK_NUMPADENTER,
  DIK_NUMPADSLASH,
};

typedef struct DIRECTINPUT8 *LPDIRECTINPUT8;
typedef struct DIRECTINPUTDEVICE8 *LPDIRECTINPUTDEVICE8;
typedef struct DIDEVICEOBJECTDATA DIDEVICEOBJECTDATA;

#endif // _WIN32