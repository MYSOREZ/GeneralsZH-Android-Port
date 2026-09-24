#include "string_compat.h"

#include <sstream>
#include <streambuf>
#include <ostream>

// GeneralsX @bugfix Android port 24/09/2026 itoa wrote nothing on Android.
//
// The previous version pointed a std::stringbuf at str with pubsetbuf() and streamed
// the number into it. Under libc++ basic_stringbuf does not override setbuf(), so
// pubsetbuf() is a no-op, the digits went into the stringbuf's own storage, and str
// kept whatever the caller's stack held. The game builds bone names with it
// (OpenContain: "FIREPOINT" + itoa(n), "ExitStart" + itoa(n), ...), so every such
// lookup failed on Android and left an uninitialized Matrix3D in a rider's transform,
// which is hashed into the lockstep checksum. This matches the MSVC CRT: base 10
// writes a sign for negative values, other bases treat the value as unsigned.
char* itoa(int value, char* str, int base)
{
  if (str == nullptr)
    return str;
  if (base < 2 || base > 36)
  {
    str[0] = '\0';
    return str;
  }

  char digits[34];
  int count = 0;
  const bool negative = (base == 10 && value < 0);
  unsigned int magnitude = negative ? 0u - (unsigned int)value : (unsigned int)value;
  do
  {
    const unsigned int digit = magnitude % (unsigned int)base;
    digits[count++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
    magnitude /= (unsigned int)base;
  } while (magnitude != 0);

  int out = 0;
  if (negative)
    str[out++] = '-';
  while (count > 0)
    str[out++] = digits[--count];
  str[out] = '\0';
  return str;
}

int _vsnwprintf(wchar_t* buffer, size_t count, const wchar_t* format, va_list args)
{
  std::wstring format_fixup(format);

  // Replace all %s with %ls
  size_t pos = format_fixup.find(L"%s", 0);
  while (pos != std::wstring::npos)
  {
    format_fixup.replace(pos, 2, L"%ls");
    pos += 3;
    pos = format_fixup.find(L"%s", pos);
  }

  // Replace all %S with %s
  pos = format_fixup.find(L"%S", 0);
  while (pos != std::wstring::npos)
  {
    format_fixup.replace(pos, 2, L"%s");
    pos += 2;
    pos = format_fixup.find(L"%S", pos);
  }


  return vswprintf(buffer, count, format_fixup.c_str(), args);
}

// Also defined in GameSpy gsplatformutil
__attribute__((weak))
char* _strlwr(char* str)
{
  for (int i = 0; str[i] != '\0'; i++)
  {
    str[i] = tolower(str[i]);
  }
  return str;
}

// GeneralsX @build fbraz 11/02/2026 BenderAI - Linux portability: uppercase string
__attribute__((weak))
char* _strupr(char* str)
{
  for (int i = 0; str[i] != '\0'; i++)
  {
    str[i] = toupper(str[i]);
  }
  return str;
}