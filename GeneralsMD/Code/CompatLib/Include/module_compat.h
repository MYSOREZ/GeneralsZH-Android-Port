#pragma once

// GeneralsX @bugfix Android port 16/09/2026 Every name in this file --
// FARPROC, HMODULE, LoadLibrary, GetProcAddress, FreeLibrary -- is a real
// Win32 API name that real windows.h already declares on _WIN32; this file
// exists to stand in for them where there is no real windows.h at all. Redoing
// them unconditionally conflicts with the real ones the first time this
// header is compiled on a genuine _WIN32 target.
#ifndef _WIN32

class CComModule
{
  public:
    void Init(void*, HINSTANCE hInstance)
    {
      m_hInstance = hInstance;
    }
    void Term() {}

  private:
    HINSTANCE m_hInstance;
};

bool GetModuleFileName(HINSTANCE hInstance, char* buffer, int size);

typedef uintptr_t (*FARPROC)();
typedef HANDLE HMODULE;

HMODULE LoadLibrary(const char* lpFileName);
FARPROC GetProcAddress(HMODULE hModule, const char* lpProcName);
void FreeLibrary(HMODULE hModule);

#endif // _WIN32