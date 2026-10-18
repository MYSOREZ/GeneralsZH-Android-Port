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

// GeneralsX @bugfix Codex 14/10/2026 Use DirectInput scan codes, not ordinal values.
// In particular NUMPAD0 must not alias KEY_NONE (0), nor OEM102 another key.
// Values cross-checked against https://github.com/wine-mirror/wine/blob/master/include/dinput.h.
enum DInputKeys
{
  // keypad keys ----------------------------------------------------------------
  DIK_NUMPAD0 = 0x52,
  DIK_NUMPAD1 = 0x4F,
  DIK_NUMPAD2 = 0x50,
  DIK_NUMPAD3 = 0x51,
  DIK_NUMPAD4 = 0x4B,
  DIK_NUMPAD5 = 0x4C,
  DIK_NUMPAD6 = 0x4D,
  DIK_NUMPAD7 = 0x47,
  DIK_NUMPAD8 = 0x48,
  DIK_NUMPAD9 = 0x49,
  DIK_NUMPADPERIOD = 0x53,
  DIK_NUMPADSTAR = 0x37,
  DIK_NUMPADMINUS = 0x4A,
  DIK_NUMPADPLUS = 0x4E,
  DIK_ESCAPE = 0x01,
  DIK_BACK = 0x0E,
  DIK_RETURN = 0x1C,
  DIK_SPACE = 0x39,
  DIK_TAB = 0x0F,

  DIK_F1 = 0x3B,
  DIK_F2 = 0x3C,
  DIK_F3 = 0x3D,
  DIK_F4 = 0x3E,
  DIK_F5 = 0x3F,
  DIK_F6 = 0x40,
  DIK_F7 = 0x41,
  DIK_F8 = 0x42,
  DIK_F9 = 0x43,
  DIK_F10 = 0x44,
  DIK_F11 = 0x57,
  DIK_F12 = 0x58,
  DIK_A = 0x1E,
  DIK_B = 0x30,
  DIK_C = 0x2E,
  DIK_D = 0x20,
  DIK_E = 0x12,
  DIK_F = 0x21,
  DIK_G = 0x22,
  DIK_H = 0x23,
  DIK_I = 0x17,
  DIK_J = 0x24,
  DIK_K = 0x25,
  DIK_L = 0x26,
  DIK_M = 0x32,
  DIK_N = 0x31,
  DIK_O = 0x18,
  DIK_P = 0x19,
  DIK_Q = 0x10,
  DIK_R = 0x13,
  DIK_S = 0x1F,
  DIK_T = 0x14,
  DIK_U = 0x16,
  DIK_V = 0x2F,
  DIK_W = 0x11,
  DIK_X = 0x2D,
  DIK_Y = 0x15,
  DIK_Z = 0x2C,
  DIK_1 = 0x02,
  DIK_2 = 0x03,
  DIK_3 = 0x04,
  DIK_4 = 0x05,
  DIK_5 = 0x06,
  DIK_6 = 0x07,
  DIK_7 = 0x08,
  DIK_8 = 0x09,
  DIK_9 = 0x0A,
  DIK_0 = 0x0B,
  DIK_MINUS = 0x0C,
  DIK_EQUALS = 0x0D,
  DIK_LBRACKET = 0x1A,
  DIK_RBRACKET = 0x1B,
  DIK_SEMICOLON = 0x27,
  DIK_APOSTROPHE = 0x28,
  DIK_GRAVE = 0x29,
  DIK_BACKSLASH = 0x2B,
  DIK_COMMA = 0x33,
  DIK_PERIOD = 0x34,
  DIK_SLASH = 0x35,

  // special keys ---------------------------------------------------------------
  DIK_SYSRQ = 0xB7,

  DIK_CAPSLOCK = 0x3A,
  DIK_NUMLOCK = 0x45,
  DIK_SCROLL = 0x46,
  DIK_LCONTROL = 0x1D,
  DIK_LALT = 0x38,
  DIK_LSHIFT = 0x2A,
  DIK_RSHIFT = 0x36,

  DIK_UPARROW = 0xC8,
  DIK_DOWNARROW = 0xD0,
  DIK_LEFTARROW = 0xCB,
  DIK_RIGHTARROW = 0xCD,
  DIK_RALT = 0xB8,
  DIK_RCONTROL = 0x9D,
  DIK_HOME = 0xC7,
  DIK_END = 0xCF,
  DIK_PGUP = 0xC9,
  DIK_PGDN = 0xD1,
  DIK_INSERT = 0xD2,
  DIK_DELETE = 0xD3,
  DIK_NUMPADENTER = 0x9C,
  DIK_NUMPADSLASH = 0xB5,
};

typedef struct DIRECTINPUT8 *LPDIRECTINPUT8;
typedef struct DIRECTINPUTDEVICE8 *LPDIRECTINPUTDEVICE8;
typedef struct DIDEVICEOBJECTDATA DIDEVICEOBJECTDATA;

#endif // _WIN32