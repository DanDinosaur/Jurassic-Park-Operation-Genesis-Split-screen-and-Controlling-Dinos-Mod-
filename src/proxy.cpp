// proxy.cpp - dinput8.dll runtime proxy. We export the real DirectInput entry
// points and forward them to the genuine system dinput8.dll (loaded by absolute
// path). A 32-bit process is file-system-redirected, so System32 resolves to the
// real 32-bit dinput8 in SysWOW64 - no local dinput8_real.dll copy is required.
//
// extern "C" + __declspec(dllexport) + the linker's --kill-at give undecorated
// export names ("DirectInput8Create", ...) exactly as the game imports them.
#include <windows.h>
#include <cstring>
#include "extras.h"
#include "kbm.h"

namespace {
    HMODULE g_real = nullptr;
    FARPROC Real(const char* name) {
        if (!g_real) {
            char path[MAX_PATH];
            UINT n = GetSystemDirectoryA(path, MAX_PATH); // -> real 32-bit dir via WoW64 redirect
            if (n && n < MAX_PATH - 16) {
                lstrcatA(path, "\\dinput8.dll");
                g_real = LoadLibraryA(path);
            }
            if (!g_real) g_real = LoadLibraryA("dinput8_real.dll"); // optional local fallback
        }
        return g_real ? GetProcAddress(g_real, name) : nullptr;
    }
}

// Load every *.asi plugin in the game folder once, on the game's first DirectInput8Create
// call (outside the loader lock, before input/render init) - same as JPOG-Modernize's proxy.
// Used for JPOGMouseFix.asi (Hor+ widescreen, unstretched HUD, borderless, free keyboard).
static void LoadAsiPlugins() {
    static bool done = false;
    if (done) return;
    done = true;
    char dir[MAX_PATH]; GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* sl = strrchr(dir, '\\'); if (sl) sl[1] = 0;
    char pat[MAX_PATH]; lstrcpyA(pat, dir); lstrcatA(pat, "*.asi");
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { char full[MAX_PATH]; lstrcpyA(full, dir); lstrcatA(full, fd.cFileName); LoadLibraryA(full); }
    while (FindNextFileA(h, &fd));
    FindClose(h);
}

extern "C" {

__declspec(dllexport) HRESULT WINAPI
DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, void* outer) {
    typedef HRESULT (WINAPI *P)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);
    static P p = (P)Real("DirectInput8Create");
    kbm::EarlyInstall(g_real);   // JPOG71: raw input / cursor hooks BEFORE the .asi plugins load, so their calls are seen too
    LoadAsiPlugins();
    HRESULT hr = p ? p(hinst, ver, riid, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) extras::OnDirectInput(*out);   // JPOG67: free keyboard
    if (SUCCEEDED(hr) && out && *out) kbm::OnDirectInput(*out);      // JPOG70: keyboard mask fallback (chains after extras)
    return hr;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) {
    typedef HRESULT (WINAPI *P)(void);
    static P p = (P)Real("DllCanUnloadNow");
    return p ? p() : S_FALSE;
}

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    typedef HRESULT (WINAPI *P)(REFCLSID, REFIID, LPVOID*);
    static P p = (P)Real("DllGetClassObject");
    return p ? p(clsid, riid, out) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI DllRegisterServer(void) {
    typedef HRESULT (WINAPI *P)(void);
    static P p = (P)Real("DllRegisterServer");
    return p ? p() : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI DllUnregisterServer(void) {
    typedef HRESULT (WINAPI *P)(void);
    static P p = (P)Real("DllUnregisterServer");
    return p ? p() : E_FAIL;
}

__declspec(dllexport) void* WINAPI GetdfDIJoystick(void) {
    typedef void* (WINAPI *P)(void);
    static P p = (P)Real("GetdfDIJoystick");
    return p ? p() : nullptr;
}

} // extern "C"
