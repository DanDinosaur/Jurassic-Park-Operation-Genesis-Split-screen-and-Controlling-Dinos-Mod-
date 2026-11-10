// kbm.cpp - JPOG70: player 1 and player 2 each with their own keyboard + mouse, and the per-player
// "controller <-> keyboard + mouse" switch. See kbm.h for the idea. All settings: jpogmod.ini [pc2].
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <xinput.h>
#include "kbm.h"
#include "log.h"
#include "gamectl.h"
#include "dinocontrol.h"
#include "input_map.h"
#include "p2menu.h"
#include "builder2.h"
#include "guard.h"
#include "xinput_pad.h"
#include "detour.h"
#include "splitscreen.h"

namespace {
    // ------------------------------------------------------------------ settings ([pc2] in jpogmod.ini)
    wchar_t g_ini[MAX_PATH]{};
    void IniInit() {
        if (g_ini[0]) return;
        GetModuleFileNameW(nullptr, g_ini, MAX_PATH);
        if (wchar_t* s = wcsrchr(g_ini, L'\\')) *(s + 1) = 0;
        wcscat_s(g_ini, MAX_PATH, L"jpogmod.ini");
    }
    int IniI(const wchar_t* key, int def) { IniInit(); return (int)GetPrivateProfileIntW(L"pc2", key, def, g_ini); }
    void IniS(const wchar_t* key, char* out, int n, const char* def) {
        IniInit(); wchar_t w[300]{}, d[64]{};
        MultiByteToWideChar(CP_ACP, 0, def, -1, d, 64);
        GetPrivateProfileStringW(L"pc2", key, d, w, 300, g_ini);
        if (wchar_t* sc = wcschr(w, L';')) *sc = 0;           // inline comment
        int len = (int)wcslen(w); while (len && (w[len - 1] == L' ' || w[len - 1] == L'\t')) w[--len] = 0;
        wchar_t* b = w; while (*b == L' ' || *b == L'\t') ++b;
        WideCharToMultiByte(CP_ACP, 0, b, -1, out, n, nullptr, nullptr);
    }
    void IniW(const wchar_t* key, const char* val) {
        IniInit(); wchar_t w[300]{}; MultiByteToWideChar(CP_ACP, 0, val, -1, w, 300);
        WritePrivateProfileStringW(L"pc2", key, w, g_ini);
    }
    struct Settings {
        bool enabled = true, kbMask = true, mouseMask = true, padCombo = true, watchdog = true, mousePans = true, zoomInvert = false;
        int  toggleVk = VK_F8;
        int  virtualCursor = 2;   // JPOG71: 0 off, 1 on, 2 auto (on if the game reads the Windows cursor)
        float mouseSpeed = 1.0f;
        bool  pointer = true, hudButtons = true;   // JPOG73 [pc2] pointer / hud_buttons
        float pointerSpeed = 1.0f, edgeScroll = 0.02f, hudScale = 0.10f, cursorScale = 1.0f;
        char p2Kb[260]{}, p2Mouse[260]{};
    } S;
    bool g_loaded = false;
    void Load() {
        if (g_loaded) return;
        g_loaded = true;
        S.enabled   = IniI(L"enabled", 1) != 0;
        S.kbMask    = IniI(L"kb_mask", 1) != 0;
        S.mouseMask = IniI(L"mouse_mask", 1) != 0;
        { char vc[16]; IniS(L"virtual_cursor", vc, sizeof vc, "auto"); S.virtualCursor = !_stricmp(vc, "auto") ? 2 : atoi(vc) ? 1 : 0; }
        S.padCombo  = IniI(L"pad_combo", 1) != 0;
        S.watchdog  = IniI(L"watchdog", 1) != 0;
        S.mousePans = IniI(L"mouse_pans", 1) != 0;
        S.zoomInvert= IniI(L"zoom_invert", 0) != 0;
        char t[32]; IniS(L"toggle_key", t, sizeof t, "F8");
        if ((t[0] == 'F' || t[0] == 'f') && atoi(t + 1) >= 1 && atoi(t + 1) <= 12) S.toggleVk = VK_F1 + atoi(t + 1) - 1;
        char ms[32]; IniS(L"mouse_speed", ms, sizeof ms, "1.0"); S.mouseSpeed = (float)atof(ms); if (S.mouseSpeed < 0.1f) S.mouseSpeed = 0.1f;
        S.pointer    = IniI(L"pointer", 1) != 0;
        S.hudButtons = IniI(L"hud_buttons", 1) != 0;
        { char hs[32]; IniS(L"hud_scale", hs, sizeof hs, "0.10"); S.hudScale = (float)atof(hs); if (S.hudScale < 0.04f || S.hudScale > 0.25f) S.hudScale = 0.10f; }
        { char cs[32]; IniS(L"cursor_scale", cs, sizeof cs, "1.0"); S.cursorScale = (float)atof(cs); if (S.cursorScale < 0.3f || S.cursorScale > 4.0f) S.cursorScale = 1.0f; }
        { char ps[32]; IniS(L"pointer_speed", ps, sizeof ps, "1.0"); S.pointerSpeed = (float)atof(ps); if (S.pointerSpeed < 0.1f) S.pointerSpeed = 0.1f; }
        { char es[32]; IniS(L"edge_scroll", es, sizeof es, "0.02"); S.edgeScroll = (float)atof(es); if (S.edgeScroll < 0.0f) S.edgeScroll = 0.0f; }
        IniS(L"p2_keyboard", S.p2Kb, sizeof S.p2Kb, "");
        IniS(L"p2_mouse", S.p2Mouse, sizeof S.p2Mouse, "");
    }

    // ------------------------------------------------------------------ shared state
    struct CS { CRITICAL_SECTION c; CS() { InitializeCriticalSection(&c); } } g_cs;
    struct Lock { Lock() { EnterCriticalSection(&g_cs.c); } ~Lock() { LeaveCriticalSection(&g_cs.c); } };

    volatile LONG g_mode[2] = {0, 0};            // 1 = keyboard + mouse
    volatile LONG g_modeChanged[2] = {0, 0};     // side effects (hint, release keys) on the mod thread
    volatile LONG g_capturing = 0, g_broken = 0, g_wantCapture = 0;
    // JPOG73: the mode was chosen in THIS run (F8 / Shift+F8 / Options / Back + Start). Saved ini modes alone never take a
    // controller away: with TWO controllers connected both players use them (JPOG69 behaviour, owner rule).
    volatile LONG g_runSet[2] = {0, 0};
    volatile LONG g_bootPads = -1; DWORD g_bootT = 0;
    void MarkChanged(int p) { p &= 1; InterlockedExchange(&g_runSet[p], 1); InterlockedExchange(&g_modeChanged[p], 1); }
    int RawPadCount() {
        int n = 0;
        for (DWORD i = 0; i < 4; ++i) { XINPUT_STATE st{}; if (XInputGetState(i, &st) == ERROR_SUCCESS) ++n; }
        return n;
    }
    HWND  g_wnd = nullptr;
    DWORD g_captureT0 = 0;

    // devices
    struct Dev { HANDLE h; DWORD type; char name[260]; };
    Dev   g_devs[48]; int g_ndev = 0;
    HANDLE g_p2Kb = nullptr, g_p2Mouse = nullptr;
    DWORD g_waitMouseUntil = 0, g_waitKbUntil = 0;

    // keys per player (VK) + scancodes (DIK numbering: make code, +0x80 for E0 keys)
    volatile bool g_vk[2][256]{};
    DWORD g_vkT[2][256]{};                        // press tick (a tap shorter than a frame still shows for 60 ms)
    volatile bool g_scan[2][256]{};
    // player 2's mouse
    bool  g_mb[5]{}; DWORD g_mbT[5]{}, g_mbUpT[5]{}; bool g_rmbMoved = false; int g_rmbTravel = 0;
    struct Move { DWORD t; int dx, dy; } g_mv[128]; int g_mvI = 0;
    // JPOG73: player 2's pointer (0..1 of their half), moved by every raw mouse event while pointer mode is on
    volatile float g_ptr[2] = { 0.5f, 0.5f }; volatile LONG g_ptrMode = 0; volatile float g_p2Aspect = 1.0f;
    volatile LONG g_worldClick = 0; volatile float g_clickPt[2] = { 0, 0 };
    volatile LONG g_ptrMenu = 0, g_menuClick = 0; bool s_lastMenu = false;   // JPOG73: pointer inside player 2's menus
    DWORD g_btnPulseT[8]{};   // P2B_* -> pulse the matching virtual pad button until this time
    struct Wheel { DWORD t; int d; } g_wh[16]; int g_whI = 0;
    // JPOG71: DirectInput mouse mask (when nobody's raw input registration can be forwarded): player 2's movement since
    // the game's last read is taken out of what DirectInput reports; buttons only player 2 holds are cleared.
    int  g_acc[3]{}; DWORD g_accT = 0;
    bool g_p1mb[5]{}; DWORD g_p1mbUpT[5]{};
    // JPOG71: player 1's own cursor position (virtual_cursor) - only player 1's mouse moves it
    POINT g_vc{}; bool g_vcInit = false; float g_vcScale = 1.0f; volatile LONG g_cursorGame = 0;

    // JPOG71: lines written before the log file is open (it opens ~2.5 s after start) are kept and written later
    volatile LONG g_logOpen = 0;
    char g_early[4096]{}; int g_earlyN = 0;
    void ELog(const char* fmt, ...) {
        char b[300]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
        if (g_logOpen) { LOG("%s", b); return; }
        Lock l;
        int n = (int)strlen(b);
        if (g_earlyN + n + 2 < (int)sizeof g_early) { memcpy(g_early + g_earlyN, b, n); g_earlyN += n; g_early[g_earlyN++] = '\n'; g_early[g_earlyN] = 0; }
    }
    void FlushEarly() {
        char copy[sizeof g_early];
        { Lock l; memcpy(copy, g_early, sizeof copy); g_earlyN = 0; g_early[0] = 0; InterlockedExchange(&g_logOpen, 1); }
        for (char* line = copy; *line; ) { char* e = strchr(line, '\n'); if (e) *e = 0; LOG("%s", line); if (!e) break; line = e + 1; }
    }

    // hints (raw thread / any -> mod thread). JPOG71: every hint is logged (rev b: a hint nobody saw, nothing in the log)
    wchar_t g_hint[200]{}; volatile LONG g_hintPending = 0; float g_hintSecs = 4.0f;
    void Hint(const wchar_t* t, float secs = 4.0f) {
        { Lock l; wcsncpy_s(g_hint, t, _TRUNCATE); g_hintSecs = secs; g_hintPending = 1; }
        ELog("pc2: hint queued: %ls", t);
    }

    // JPOG71: which module called a hooked function (first few distinct return addresses per function)
    void ModName(const void* a, char* out, int n, bool* inGameDir = nullptr) {
        HMODULE m = nullptr; out[0] = 0; if (inGameDir) *inGameDir = false;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) || !m) {
            strcpy_s(out, n, "(no module)"); return; }
        char path[MAX_PATH]; GetModuleFileNameA(m, path, MAX_PATH);
        const char* b = strrchr(path, '\\'); strncpy_s(out, n, b ? b + 1 : path, _TRUNCATE);
        if (inGameDir) {
            char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
            char* e = strrchr(exe, '\\'); if (e) e[1] = 0;
            *inGameDir = !_strnicmp(path, exe, strlen(exe)) && m != GetModuleHandleA("dinput8.dll");
        }
    }
    struct CallerLog { const char* fn; const void* seen[12]; int n; };
    bool NewCaller(CallerLog& c, const void* ret) {
        for (int i = 0; i < c.n; ++i) if (c.seen[i] == ret) return false;
        if (c.n >= 12) return false;
        c.seen[c.n++] = ret; return true;
    }
    HMODULE g_user32 = nullptr, g_self = nullptr;
    bool InMod(const void* a, HMODULE m) {   // range check, no loader lock (GetCursorPos can be called thousands of times a second)
        if (!m) return false;
        auto* nt = (IMAGE_NT_HEADERS*)((BYTE*)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
        return (const BYTE*)a >= (const BYTE*)m && (const BYTE*)a < (const BYTE*)m + nt->OptionalHeader.SizeOfImage;
    }

    // ------------------------------------------------------------------ DirectInput's raw input registration
    using fn_reg  = BOOL (WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);
    using fn_data = UINT (WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
    using fn_buf  = UINT (WINAPI*)(PRAWINPUT, PUINT, UINT);
    fn_reg  g_realReg = nullptr;      // JPOG71: trampolines of the inline hooks (user32 export, or what it forwards to)
    fn_data g_realData = nullptr;
    fn_buf  g_realBuf = nullptr;
    fn_reg  g_w32Reg = nullptr;       // win32u!NtUser* (only when user32's export is a different function)
    fn_data g_w32Data = nullptr;
    fn_buf  g_w32Buf = nullptr;
    CallerLog g_clReg{"RegisterRawInputDevices"}, g_clData{"GetRawInputData"},
              g_clW32{"NtUserRegisterRawInputDevices"}, g_clHook{"SetWindowsHookEx"}, g_clCur{"GetCursorPos"};
    volatile LONG g_regCalls = 0;
    struct DiReg { bool on; HWND hwnd; DWORD flags; } g_di[2]{};   // [0] mouse (usage 2), [1] keyboard (usage 6)
    volatile LONG g_fwd = 0, g_served = 0, g_bufCalls = 0;
    volatile LONG g_fwdU[2] = {0, 0}, g_servedU[2] = {0, 0};   // per usage: [0] mouse, [1] keyboard
    int UsageIdx(USHORT page, USHORT usage) { return page == 1 ? (usage == 2 ? 0 : usage == 6 ? 1 : -1) : -1; }

    // passed-on events: DirectInput's window gets a pointer into this ring as its "HRAWINPUT"; our GetRawInputData
    // hook in dinput8's import table hands it the copy.
    struct Fwd { DWORD magic; UINT size; BYTE buf[64]; };
    Fwd g_ring[512]; volatile LONG g_ringI = 0;
    constexpr DWORD kMagic = 0x4A50474Fu;   // "JPGO"
    Fwd* AsFwd(HRAWINPUT h) {
        uintptr_t a = (uintptr_t)h, b = (uintptr_t)&g_ring[0], e = (uintptr_t)&g_ring[512];
        if (a < b || a >= e || (a - b) % sizeof(Fwd)) return nullptr;
        Fwd* f = (Fwd*)h; return f->magic == kMagic ? f : nullptr;
    }

    BOOL RegisterCore(PCRAWINPUTDEVICE d, UINT n, UINT cb, const void* ret);
    BOOL WINAPI Hook_Register(PCRAWINPUTDEVICE d, UINT n, UINT cb) {
        return RegisterCore(d, n, cb, __builtin_return_address(0));
    }
    BOOL RegisterCore(PCRAWINPUTDEVICE d, UINT n, UINT cb, const void* ret) {
        InterlockedIncrement(&g_regCalls);
        {   // JPOG71: who registers raw input in this process (DirectInput? JPOGMouseFix? the game?)
            bool fresh; { Lock l; fresh = NewCaller(g_clReg, ret); }
            if (fresh) {
                char mod[96]; ModName(ret, mod, sizeof mod);
                char what[200] = ""; int w = 0;
                for (UINT i = 0; i < n && d && cb >= sizeof(RAWINPUTDEVICE) && w < (int)sizeof what - 1; ++i) {
                    int k = snprintf(what + w, sizeof what - w, " [page %u usage %u flags 0x%lX hwnd %p]", d[i].usUsagePage, d[i].usUsage, d[i].dwFlags, d[i].hwndTarget);
                    if (k < 0) break; w += k; }
                ELog("pc2: RegisterRawInputDevices called by %s (ret %p):%s", mod, ret, what);
            }
        }
        bool mine = false;
        for (UINT i = 0; i < n && d && cb >= sizeof(RAWINPUTDEVICE); ++i) {
            int u = UsageIdx(d[i].usUsagePage, d[i].usUsage);
            if (u < 0) continue;
            mine = true;
            Lock l;
            if (d[i].dwFlags & RIDEV_REMOVE) g_di[u].on = false;
            else { g_di[u].on = true; g_di[u].hwnd = d[i].hwndTarget; g_di[u].flags = d[i].dwFlags; }
            static int s_logs = 0;
            if (s_logs < 12) { ++s_logs; ELog("pc2: raw input registration (game side) %s: hwnd %p flags 0x%lX%s", u ? "keyboard" : "mouse",
                d[i].hwndTarget, d[i].dwFlags, g_capturing ? " (kept on our window, events passed on)" : ""); }
        }
        if (!mine || !g_capturing) return g_realReg(d, n, cb);
        // capturing: everything that isn't keyboard/mouse goes through; ours stays registered with merged flags
        RAWINPUTDEVICE rest[8]; UINT m = 0;
        for (UINT i = 0; i < n && m < 8; ++i) if (UsageIdx(d[i].usUsagePage, d[i].usUsage) < 0) rest[m++] = d[i];
        if (m) g_realReg(rest, m, sizeof(RAWINPUTDEVICE));
        if (g_wnd) PostMessageW(g_wnd, WM_APP + 2, 0, 0);
        return TRUE;
    }
    UINT ServeFwd(Fwd* f, UINT cmd, LPVOID data, PUINT size);
    UINT WINAPI Hook_GetData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT cbh) {
        if (Fwd* f = AsFwd(h)) return ServeFwd(f, cmd, data, size);
        const void* ret = __builtin_return_address(0);
        bool fresh; { Lock l; fresh = NewCaller(g_clData, ret); }
        if (fresh) { char mod[96]; ModName(ret, mod, sizeof mod); ELog("pc2: GetRawInputData called by %s (ret %p)", mod, ret); }
        return g_realData(h, cmd, data, size, cbh);
    }
    UINT ServeFwd(Fwd* f, UINT cmd, LPVOID data, PUINT size) {
        {
            InterlockedIncrement(&g_served);
            InterlockedIncrement(&g_servedU[((RAWINPUTHEADER*)f->buf)->dwType == RIM_TYPEKEYBOARD ? 1 : 0]);
            if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return (UINT)-1; }
            UINT need = (cmd == RID_HEADER) ? (UINT)sizeof(RAWINPUTHEADER) : f->size;
            if (!data) { *size = need; return 0; }
            if (*size < need) { *size = need; SetLastError(ERROR_INSUFFICIENT_BUFFER); return (UINT)-1; }
            memcpy(data, f->buf, need);
            return need;
        }
    }
    UINT WINAPI Hook_GetBuffer(PRAWINPUT d, PUINT size, UINT cbh) {
        const void* ret = __builtin_return_address(0);
        if (InterlockedIncrement(&g_bufCalls) <= 3) { char mod[96]; ModName(ret, mod, sizeof mod);
            ELog("pc2: GetRawInputBuffer called by %s (passed-on events are NOT seen this way)", mod); }
        return g_realBuf(d, size, cbh);
    }
    // win32u level: callers that skip user32 (calls coming FROM user32 are already handled above)
    BOOL WINAPI Hook_W32Reg(PCRAWINPUTDEVICE d, UINT n, UINT cb) {
        const void* ret = __builtin_return_address(0);
        if (InMod(ret, g_user32) || InMod(ret, g_self)) return g_w32Reg(d, n, cb);
        bool fresh; { Lock l; fresh = NewCaller(g_clW32, ret); }
        if (fresh) { char mod[96]; ModName(ret, mod, sizeof mod); ELog("pc2: NtUserRegisterRawInputDevices called DIRECTLY by %s - handled like user32's", mod); }
        return RegisterCore(d, n, cb, ret);
    }
    UINT WINAPI Hook_W32Data(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT cbh) {
        if (Fwd* f = AsFwd(h)) return ServeFwd(f, cmd, data, size);
        return g_w32Data(h, cmd, data, size, cbh);
    }
    UINT WINAPI Hook_W32Buf(PRAWINPUT d, PUINT size, UINT cbh) {
        const void* ret = __builtin_return_address(0);
        if (!InMod(ret, g_user32) && InterlockedIncrement(&g_bufCalls) <= 3) { char mod[96]; ModName(ret, mod, sizeof mod);
            ELog("pc2: NtUserGetRawInputBuffer called DIRECTLY by %s", mod); }
        return g_w32Buf(d, size, cbh);
    }

    // log-only: low-level keyboard / mouse hooks (the other way DirectInput can read keyboard + mouse)
    using fn_hookA = HHOOK (WINAPI*)(int, HOOKPROC, HINSTANCE, DWORD);
    fn_hookA g_realHookA = nullptr, g_realHookW = nullptr;
    void LogHook(int id, DWORD tid, const void* ret, char aw) {
        bool fresh; { Lock l; fresh = NewCaller(g_clHook, ret); }
        if (fresh || id == WH_KEYBOARD_LL || id == WH_MOUSE_LL) {
            static volatile LONG s_n = 0; if (InterlockedIncrement(&s_n) > 24) return;
            char mod[96]; ModName(ret, mod, sizeof mod);
            ELog("pc2: SetWindowsHookEx%c(%d%s, thread %lu) called by %s", aw, id,
                 id == WH_KEYBOARD_LL ? " = LOW-LEVEL KEYBOARD" : id == WH_MOUSE_LL ? " = LOW-LEVEL MOUSE" : "", tid, mod);
        }
    }
    HHOOK WINAPI Hook_HookA(int id, HOOKPROC p, HINSTANCE h, DWORD t) { LogHook(id, t, __builtin_return_address(0), 'A'); return g_realHookA(id, p, h, t); }
    HHOOK WINAPI Hook_HookW(int id, HOOKPROC p, HINSTANCE h, DWORD t) { LogHook(id, t, __builtin_return_address(0), 'W'); return g_realHookW(id, p, h, t); }

    // ------------------------------------------------------------------ devices
    Dev* FindDev(HANDLE h, DWORD type) {
        for (int i = 0; i < g_ndev; ++i) if (g_devs[i].h == h) return &g_devs[i];
        if (g_ndev >= 48) return nullptr;
        Dev& d = g_devs[g_ndev++]; d.h = h; d.type = type; d.name[0] = 0;
        UINT n = sizeof d.name - 1;
        if (GetRawInputDeviceInfoA(h, RIDI_DEVICENAME, d.name, &n) == (UINT)-1) strcpy(d.name, "?");
        LOG("pc2: %s seen: %s", type == RIM_TYPEKEYBOARD ? "keyboard" : "mouse", d.name);
        // saved player 2 devices (same name = same device / port)
        if (type == RIM_TYPEKEYBOARD && !g_p2Kb && S.p2Kb[0] && !_stricmp(S.p2Kb, d.name)) { g_p2Kb = h; LOG("pc2: player 2's saved keyboard is back"); }
        if (type == RIM_TYPEMOUSE && !g_p2Mouse && S.p2Mouse[0] && !_stricmp(S.p2Mouse, d.name)) { g_p2Mouse = h; LOG("pc2: player 2's saved mouse is back"); }
        return &d;
    }
    void SetP2Kb(Dev* d) {
        g_p2Kb = d->h; strncpy_s(S.p2Kb, d->name, _TRUNCATE); IniW(L"p2_keyboard", d->name);
        LOG("pc2: player 2's keyboard = %s", d->name);
    }
    void SetP2Mouse(Dev* d) {
        g_p2Mouse = d->h; strncpy_s(S.p2Mouse, d->name, _TRUNCATE); IniW(L"p2_mouse", d->name);
        LOG("pc2: player 2's mouse = %s", d->name);
    }
    bool GameFront() {
        HWND fg = GetForegroundWindow(); DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }

    int FixVk(const RAWKEYBOARD& k) {
        int vk = k.VKey;
        const bool e0 = (k.Flags & RI_KEY_E0) != 0;
        if (vk == VK_SHIFT)   vk = (k.MakeCode == 0x36) ? VK_RSHIFT : VK_LSHIFT;
        if (vk == VK_CONTROL) vk = e0 ? VK_RCONTROL : VK_LCONTROL;
        if (vk == VK_MENU)    vk = e0 ? VK_RMENU : VK_LMENU;
        return (vk > 0 && vk < 255) ? vk : 0;
    }
    bool ShiftHeld(int p) { return g_vk[p][VK_LSHIFT] || g_vk[p][VK_RSHIFT]; }

    // returns true = pass the event on to the game (player 1 / injected)
    bool OnKeyboard(HANDLE h, const RAWKEYBOARD& k) {
        const int vk = FixVk(k);
        const bool up = (k.Flags & RI_KEY_BREAK) != 0;
        const int scan = (k.MakeCode & 0x7F) | ((k.Flags & RI_KEY_E0) ? 0x80 : 0);
        Lock l;
        Dev* d = h ? FindDev(h, RIM_TYPEKEYBOARD) : nullptr;
        const DWORD now = GetTickCount();
        bool pass = true;
        int p = (h && h == g_p2Kb) ? 1 : 0;
        // join: "Player 2: press F8 on your keyboard" (after Shift+F8 was pressed before capture started)
        if (!up && vk == S.toggleVk && d && g_waitKbUntil && (int)(now - g_waitKbUntil) < 0) {
            g_waitKbUntil = 0;
            SetP2Kb(d); p = 1;
            InterlockedExchange(&g_mode[1], 1); MarkChanged(1);
            if (!g_p2Mouse) { g_waitMouseUntil = now + 20000; Hint(L"Player 2: keyboard set - now click with YOUR mouse (player 1: don't click)", 6.0f); }
            return false;
        }
        if (vk) {
            const bool was = g_vk[p][vk];
            g_vk[p][vk] = !up; if (!up && !was) g_vkT[p][vk] = now;
            if (!up && !was && vk == S.toggleVk && d && GameFront()) {
                if (p == 0 && g_mode[0] == 2) g_mode[0] = padslots::ConnectedCount() < 2 ? 1 : 0;   // auto -> fixed, then switch
                if (ShiftHeld(p)) {                     // Shift+F8 on a keyboard = "this is player 2's keyboard"
                    if (p == 0) { g_vk[0][vk] = false; SetP2Kb(d); p = 1; g_vk[1][vk] = true; }
                    InterlockedExchange(&g_mode[1], 1); MarkChanged(1);
                    g_waitMouseUntil = now + 20000;   // (again) pick player 2's mouse
                    Hint(L"Player 2: this is your keyboard now - click with YOUR mouse (player 1: don't click)", 6.0f);
                } else {                                // F8 = switch this keyboard's player
                    const LONG m = kbm::IsKbm(p) ? 0 : 1;   // JPOG73: from what the player has NOW
                    InterlockedExchange(&g_mode[p], m); MarkChanged(p);
                }
                pass = (p == 0);
            }
        }
        if (scan) g_scan[p][scan] = !up;
        return pass && p == 0;
    }

    bool OnMouse(HANDLE h, const RAWMOUSE& m) {
        const DWORD now = GetTickCount();
        Lock l;
        Dev* d = h ? FindDev(h, RIM_TYPEMOUSE) : nullptr;
        if (d && g_waitMouseUntil && (int)(now - g_waitMouseUntil) < 0 && (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)) {
            g_waitMouseUntil = 0;
            SetP2Mouse(d);
            Hint(L"Player 2: keyboard + mouse ready (F8 on your keyboard = back to the controller)", 5.0f);
            return false;
        }
        if (!h || h != g_p2Mouse) {               // player 1's mouse (or injected): the game gets it
            static const USHORT d1[5] = { RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_3_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN };
            static const USHORT u1[5] = { RI_MOUSE_BUTTON_1_UP, RI_MOUSE_BUTTON_2_UP, RI_MOUSE_BUTTON_3_UP, RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP };
            for (int i = 0; i < 5; ++i) { if (m.usButtonFlags & d1[i]) g_p1mb[i] = true; if (m.usButtonFlags & u1[i]) { g_p1mb[i] = false; g_p1mbUpT[i] = now; } }
            if (g_vcInit) {
                if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {
                    const bool virt = (m.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
                    const int x0 = virt ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0, y0 = virt ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
                    const int w = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN), hh = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
                    g_vc.x = x0 + (int)((long long)m.lLastX * w / 65536); g_vc.y = y0 + (int)((long long)m.lLastY * hh / 65536);
                } else { g_vc.x += (LONG)lroundf(m.lLastX * g_vcScale); g_vc.y += (LONG)lroundf(m.lLastY * g_vcScale); }
                const int x0 = GetSystemMetrics(SM_XVIRTUALSCREEN), y0 = GetSystemMetrics(SM_YVIRTUALSCREEN);
                const int x1 = x0 + GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1, y1 = y0 + GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1;
                if (g_vc.x < x0) g_vc.x = x0; if (g_vc.x > x1) g_vc.x = x1; if (g_vc.y < y0) g_vc.y = y0; if (g_vc.y > y1) g_vc.y = y1;
            }
            return true;
        }
        if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            if (now - g_accT > 150) { g_acc[0] = g_acc[1] = g_acc[2] = 0; }
            g_acc[0] += m.lLastX; g_acc[1] += m.lLastY; g_accT = now;
        }
        if (m.usButtonFlags & RI_MOUSE_WHEEL) { if (now - g_accT > 150) g_acc[2] = 0; g_acc[2] += (SHORT)m.usButtonData; g_accT = now; }
        static const USHORT dn[5] = { RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_3_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN };
        static const USHORT upf[5] = { RI_MOUSE_BUTTON_1_UP, RI_MOUSE_BUTTON_2_UP, RI_MOUSE_BUTTON_3_UP, RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP };
        for (int i = 0; i < 5; ++i) {
            if (m.usButtonFlags & dn[i]) { g_mb[i] = true; g_mbT[i] = now; if (i == 1) { g_rmbMoved = false; g_rmbTravel = 0; } }
            if (m.usButtonFlags & upf[i]) { g_mb[i] = false; g_mbUpT[i] = now; }
        }
        if (m.usButtonFlags & RI_MOUSE_WHEEL) { g_wh[g_whI++ & 15] = { now, (int)(SHORT)m.usButtonData }; }
        if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX || m.lLastY)) {
            g_mv[g_mvI++ & 127] = { now, (int)m.lLastX, (int)m.lLastY };
            if (g_ptrMode && !g_mb[1] && !g_mb[2]) {   // JPOG73: the pointer (right / middle held = camera as before)
                const float sp = 0.0011f * S.pointerSpeed;
                float px = g_ptr[0] + m.lLastX * sp / (g_p2Aspect > 0.2f ? g_p2Aspect : 1.0f), py = g_ptr[1] + m.lLastY * sp;
                g_ptr[0] = px < 0 ? 0 : px > 1 ? 1 : px; g_ptr[1] = py < 0 ? 0 : py > 1 ? 1 : py;
            }
            if (g_mb[1]) { g_rmbTravel += abs((int)m.lLastX) + abs((int)m.lLastY); if (g_rmbTravel > 12) g_rmbMoved = true; }
        }
        return false;
    }

    // ------------------------------------------------------------------ our raw input window (own thread)
    void RegisterOurs() {
        RAWINPUTDEVICE r[2];
        for (int u = 0; u < 2; ++u) {
            DWORD keep = g_di[u].on ? (g_di[u].flags & (RIDEV_NOLEGACY | RIDEV_NOHOTKEYS | RIDEV_APPKEYS)) : 0;
            r[u].usUsagePage = 1; r[u].usUsage = u ? 6 : 2;
            r[u].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY | keep;
            r[u].hwndTarget = g_wnd;
        }
        BOOL ok = g_realReg(r, 2, sizeof(RAWINPUTDEVICE));
        static int s_logs = 0;
        if (s_logs < 8) { ++s_logs; LOG("pc2: our raw input window %p registered (keyboard flags 0x%lX, mouse flags 0x%lX): %d (err %lu)",
            g_wnd, r[1].dwFlags, r[0].dwFlags, ok, ok ? 0 : GetLastError()); }
    }
    void Undo(const char* why) {
        LOG("pc2: TURNING THE KEYBOARD/MOUSE SPLIT OFF - %s. The game gets every keyboard + mouse again (player 2 keyboard mode off).", why);
        RAWINPUTDEVICE r[2]; UINT n = 0;
        for (int u = 0; u < 2; ++u) {
            r[n].usUsagePage = 1; r[n].usUsage = u ? 6 : 2;
            if (g_di[u].on) { r[n].dwFlags = g_di[u].flags; r[n].hwndTarget = g_di[u].hwnd; }
            else            { r[n].dwFlags = RIDEV_REMOVE; r[n].hwndTarget = nullptr; }
            ++n;
        }
        InterlockedExchange(&g_capturing, 0); InterlockedExchange(&g_broken, 1);
        for (UINT i = 0; i < n; ++i) {
            BOOL ok = g_realReg(&r[i], 1, sizeof(RAWINPUTDEVICE));
            LOG("pc2: %s handed back to DirectInput (hwnd %p flags 0x%lX): %d (err %lu)", i ? "keyboard" : "mouse", r[i].hwndTarget, r[i].dwFlags, ok, ok ? 0 : GetLastError());
        }
        if (g_mode[1]) { InterlockedExchange(&g_mode[1], 0); MarkChanged(1); }
        Hint(L"Two keyboards/mice could not be split on this PC - see jpogmod.log", 5.0f);
    }

    LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_INPUT) {
            alignas(8) BYTE buf[128]; UINT sz = sizeof buf;
            UINT got = g_realData((HRAWINPUT)lp, RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER));
            if (got != (UINT)-1 && got >= sizeof(RAWINPUTHEADER)) {
                RAWINPUT* ri = (RAWINPUT*)buf;
                bool pass = true; int u = -1;
                if (ri->header.dwType == RIM_TYPEKEYBOARD) { u = 1; pass = OnKeyboard(ri->header.hDevice, ri->data.keyboard); }
                else if (ri->header.dwType == RIM_TYPEMOUSE) { u = 0; pass = OnMouse(ri->header.hDevice, ri->data.mouse); }
                if (pass && u >= 0 && g_capturing && got <= sizeof(Fwd::buf)) {
                    HWND t; DWORD fl; bool on;
                    { Lock l; t = g_di[u].hwnd; fl = g_di[u].flags; on = g_di[u].on; }
                    if (on && t && (GET_RAWINPUT_CODE_WPARAM(wp) == RIM_INPUT || (fl & RIDEV_INPUTSINK))) {
                        Fwd& f = g_ring[(InterlockedIncrement(&g_ringI) - 1) & 511];
                        f.magic = 0; memcpy(f.buf, buf, got); f.size = got; f.magic = kMagic;
                        if (PostMessageW(t, WM_INPUT, wp, (LPARAM)&f)) { InterlockedIncrement(&g_fwd); InterlockedIncrement(&g_fwdU[u]); }
                    }
                }
            }
            return DefWindowProcW(w, msg, wp, lp);   // frees the real raw input
        }
        if (msg == WM_INPUT_DEVICE_CHANGE) {
            HWND t[2]; bool on[2]; DWORD fl[2];
            { Lock l; for (int u = 0; u < 2; ++u) { t[u] = g_di[u].hwnd; on[u] = g_di[u].on; fl[u] = g_di[u].flags; } }
            for (int u = 0; u < 2; ++u) if (on[u] && t[u] && (fl[u] & RIDEV_DEVNOTIFY)) PostMessageW(t[u], msg, wp, lp);
            if (wp == GIDC_REMOVAL) {
                Lock l;
                if ((HANDLE)lp == g_p2Kb)    { g_p2Kb = nullptr; LOG("pc2: player 2's keyboard unplugged"); }
                if ((HANDLE)lp == g_p2Mouse) { g_p2Mouse = nullptr; LOG("pc2: player 2's mouse unplugged"); }
                for (int i = 0; i < g_ndev; ++i) if (g_devs[i].h == (HANDLE)lp) { g_devs[i] = g_devs[--g_ndev]; break; }
            }
            return 0;
        }
        if (msg == WM_APP + 1) {   // start capture
            if (!g_capturing && !g_broken) {
                InterlockedExchange(&g_fwd, 0); InterlockedExchange(&g_served, 0);
                for (int u = 0; u < 2; ++u) { InterlockedExchange(&g_fwdU[u], 0); InterlockedExchange(&g_servedU[u], 0); }
                g_captureT0 = GetTickCount();
                InterlockedExchange(&g_capturing, 1);
                RegisterOurs();
                LOG("pc2: keyboard/mouse split ON (game-side raw input: keyboard %s, mouse %s; registrations seen so far %ld)",
                    g_di[1].on ? "registered -> passed on" : "none -> DirectInput keyboard mask",
                    g_di[0].on ? "registered -> passed on" : "none -> DirectInput mouse mask", (long)g_regCalls);
                {   // JPOG71: every keyboard / mouse Windows knows (laptop keyboard + touchpad + USB keyboard + Bluetooth mouse = 4)
                    UINT n = 0; GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
                    RAWINPUTDEVICELIST list[48]; if (n > 48) n = 48;
                    n = GetRawInputDeviceList(list, &n, sizeof(RAWINPUTDEVICELIST));
                    for (UINT i = 0; i < n && n != (UINT)-1; ++i) {
                        if (list[i].dwType == RIM_TYPEHID) continue;
                        char name[260]; UINT nn = sizeof name - 1;
                        if (GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICENAME, name, &nn) == (UINT)-1) strcpy(name, "?");
                        LOG("pc2: device %u: %s %p %s", i, list[i].dwType == RIM_TYPEKEYBOARD ? "keyboard" : "mouse", list[i].hDevice, name);
                    }
                }
            }
            return 0;
        }
        if (msg == WM_APP + 2) { if (g_capturing) RegisterOurs(); return 0; }
        if (msg == WM_TIMER) {
            if (g_capturing && S.watchdog) {
                const LONG f = g_fwd, s = g_served;
                for (int u = 0; u < 2 && g_capturing; ++u)
                    if (g_di[u].on && g_fwdU[u] >= 40 && g_servedU[u] == 0 && GetTickCount() - g_captureT0 > 3000) {
                        char why[160]; sprintf_s(why, "DirectInput did not read the %ld passed-on %s events", (long)g_fwdU[u], u ? "keyboard" : "mouse");
                        Undo(why);
                    }
                static DWORD s_last = 0;
                if (GetTickCount() - s_last > 60000) { s_last = GetTickCount();
                    LOG("pc2: passed on %ld events to DirectInput, it read %ld (buffer calls %ld)", (long)f, (long)s, (long)g_bufCalls); }
            }
            return 0;
        }
        return DefWindowProcW(w, msg, wp, lp);
    }

    DWORD WINAPI RawThread(LPVOID) {
        WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"JPOGModPc2";
        RegisterClassW(&wc);
        HWND w = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!w) { LOG("pc2: hidden window failed (%lu) - two keyboards/mice off", GetLastError()); InterlockedExchange(&g_broken, 1); return 0; }
        g_wnd = w;
        SetTimer(w, 1, 500, nullptr);
        LOG("pc2: ready (hidden raw input window %p). Player 2 joins with Shift+%s on their own keyboard.", w, "F8");
        if (g_wantCapture && !g_runSet[0] && !g_runSet[1]) {
            const int pads = RawPadCount();
            InterlockedExchange(&g_bootPads, pads); g_bootT = GetTickCount();
            if (pads >= 2) {
                InterlockedExchange(&g_wantCapture, 0);
                LOG("pc2: start-up: %d controllers connected -> both players use controllers, saved keyboard + mouse NOT started (JPOG73; Shift+F8 still joins)", pads);
            } else LOG("pc2: start-up: %d controller(s) connected -> saved player 2 keyboard + mouse starts", pads);
        }
        if (g_wantCapture) PostMessageW(w, WM_APP + 1, 0, 0);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        return 0;
    }
    DWORD WINAPI StartThread(LPVOID) {
        Sleep(2900);
        FlushEarly();   // JPOG71: the hook results + every early caller line
        if (g_broken) LOG("pc2: raw input hooks missing - player 2 keyboard + mouse NOT available (controllers unaffected)");
        else RawThread(nullptr);
        return 0;
    }
    void StartCapture(const char* why) {
        if (g_capturing || g_broken || !S.enabled) return;
        if (InterlockedExchange(&g_wantCapture, 1) == 0) LOG("pc2: keyboard/mouse split requested (%s)", why);
        if (g_wnd) PostMessageW(g_wnd, WM_APP + 1, 0, 0);
    }

    // ------------------------------------------------------------------ DirectInput keyboard + mouse masks
    // JPOG71: per device vtable (keyboard and mouse may share one), the originals of GetDeviceState / GetDeviceData /
    // SetCooperativeLevel. Keyboard: player 2's keys hidden. Mouse: player 2's movement / buttons taken out - only while
    // the mouse is NOT passed on through raw input (nobody registered it). Also: the game's own view of F8 / Shift, so
    // F8 and Shift+F8 work before the split even if DirectInput holds the keyboard exclusively.
    const GUID kSysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
    const GUID kSysMouse    = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
    using fn_create = HRESULT(__stdcall*)(void*, REFGUID, void**, void*);
    using fn_state  = HRESULT(__stdcall*)(void*, DWORD, void*);
    using fn_ddata  = HRESULT(__stdcall*)(void*, DWORD, void*, DWORD*, DWORD);
    using fn_coop   = HRESULT(__stdcall*)(void*, HWND, DWORD);
    fn_create g_origCreate = nullptr;
    struct VtHook { void** vt; fn_state st; fn_ddata dd; fn_coop coop; } g_vth[4]{}; volatile LONG g_nvth = 0;
    VtHook* FindVt(void* self) {
        void** vt = *reinterpret_cast<void***>(self);
        for (LONG i = 0; i < g_nvth; ++i) if (g_vth[i].vt == vt) return &g_vth[i];
        return nullptr;
    }
    void* volatile g_kbDev = nullptr; void* volatile g_msDev = nullptr;
    bool g_diSeen[256]{}, g_diMSeen[8]{};
    volatile bool g_diKey[256]{};          // the game's own keyboard state (before masking), for F8 / Shift before the split
    volatile LONG g_masked = 0, g_mmasked = 0;
    bool P2Only(int scan) { return g_scan[1][scan] && !g_scan[0][scan]; }
    bool MouseMaskOn() { return g_capturing && g_p2Mouse && S.mouseMask && !g_di[0].on; }
    bool P2Btn(int i, DWORD now) {
        if (i >= 5) return false;
        const bool p2 = g_mb[i] || (g_mbUpT[i] && now - g_mbUpT[i] < 100);
        const bool p1 = g_p1mb[i] || (g_p1mbUpT[i] && now - g_p1mbUpT[i] < 100);
        return p2 && !p1;
    }
    void Take(LONG& v, int& a) {
        if (!v || !a || ((v > 0) != (a > 0))) return;
        const LONG t = (labs(a) < labs(v)) ? a : v;
        v -= t; a -= (int)t;
    }
    HRESULT __stdcall Hook_State(void* self, DWORD cb, void* data) {
        VtHook* vh = FindVt(self);
        if (!vh || !vh->st) return E_FAIL;
        HRESULT hr = vh->st(self, cb, data);
        if (FAILED(hr) || !data) return hr;
        if (self == g_kbDev) {
            BYTE* b = (BYTE*)data;
            for (DWORD i = 0; i < cb && i < 256; ++i) g_diKey[i] = (b[i] & 0x80) != 0;
            if (g_capturing && g_p2Kb && S.kbMask)
                for (DWORD i = 0; i < cb && i < 256; ++i) if ((b[i] & 0x80) && P2Only((int)i)) {
                    b[i] = 0; if (InterlockedIncrement(&g_masked) <= 3) LOG("pc2: player 2's key 0x%02lX kept away from the game (DirectInput state)", i); }
        } else if (self == g_msDev && cb >= 16 && MouseMaskOn()) {
            LONG* ax = (LONG*)data; BYTE* bt = (BYTE*)data + 12; const DWORD nb = cb >= 20 ? 8 : 4;
            const DWORD now = GetTickCount();
            Lock l;
            if (now - g_accT > 150) g_acc[0] = g_acc[1] = g_acc[2] = 0;
            const LONG before = labs(ax[0]) + labs(ax[1]);
            for (int k = 0; k < 3; ++k) Take(ax[k], g_acc[k]);
            for (DWORD i = 0; i < nb; ++i) if ((bt[i] & 0x80) && P2Btn((int)i, now)) bt[i] = 0;
            if (before != labs(ax[0]) + labs(ax[1]) && InterlockedIncrement(&g_mmasked) <= 3)
                LOG("pc2: player 2's mouse movement taken out of the game's mouse (DirectInput state)");
        }
        return hr;
    }
    HRESULT __stdcall Hook_Data(void* self, DWORD cbObj, void* rg, DWORD* inout, DWORD flags) {
        VtHook* vh = FindVt(self);
        if (!vh || !vh->dd) return E_FAIL;
        HRESULT hr = vh->dd(self, cbObj, rg, inout, flags);
        if (FAILED(hr) || !rg || !inout || !*inout || cbObj < 8) return hr;
        BYTE* p = (BYTE*)rg;
        if (self == g_kbDev) {
            if (!(flags & 1)) for (DWORD i = 0; i < *inout; ++i) { BYTE* e = p + i * cbObj; g_diKey[*(DWORD*)e & 0xFF] = (*(DWORD*)(e + 4) & 0x80) != 0; }
            if ((flags & 1 /*PEEK*/) || !g_capturing || !g_p2Kb || !S.kbMask) return hr;
            DWORD out = 0;
            for (DWORD i = 0; i < *inout; ++i) {
                BYTE* e = p + i * cbObj;
                const DWORD ofs = *(DWORD*)e & 0xFF; const bool press = (*(DWORD*)(e + 4) & 0x80) != 0;
                bool keep = press ? !P2Only((int)ofs) : g_diSeen[ofs];
                if (press && keep) g_diSeen[ofs] = true;
                if (!press) g_diSeen[ofs] = false;
                if (!keep) { if (InterlockedIncrement(&g_masked) <= 3) LOG("pc2: player 2's key 0x%02lX kept away from the game (DirectInput buffer)", ofs); continue; }
                if (out != i) memmove(p + out * cbObj, e, cbObj);
                ++out;
            }
            *inout = out;
        } else if (self == g_msDev && !(flags & 1) && MouseMaskOn()) {
            const DWORD now = GetTickCount();
            Lock l;
            if (now - g_accT > 150) g_acc[0] = g_acc[1] = g_acc[2] = 0;
            DWORD out = 0;
            for (DWORD i = 0; i < *inout; ++i) {
                BYTE* e = p + i * cbObj;
                const DWORD ofs = *(DWORD*)e; bool keep = true;
                if (ofs == 0 || ofs == 4 || ofs == 8) {
                    LONG v = *(LONG*)(e + 4); Take(v, g_acc[ofs / 4]); *(LONG*)(e + 4) = v; keep = v != 0;
                    if (!keep && InterlockedIncrement(&g_mmasked) <= 3) LOG("pc2: player 2's mouse movement taken out of the game's mouse (DirectInput buffer)");
                } else if (ofs >= 12 && ofs < 20) {
                    const int bi = (int)ofs - 12; const bool press = (*(DWORD*)(e + 4) & 0x80) != 0;
                    keep = press ? !P2Btn(bi, now) : g_diMSeen[bi];
                    if (press && keep) g_diMSeen[bi] = true;
                    if (!press) g_diMSeen[bi] = false;
                }
                if (!keep) continue;
                if (out != i) memmove(p + out * cbObj, e, cbObj);
                ++out;
            }
            *inout = out;
        }
        return hr;
    }
    HRESULT __stdcall Hook_Coop(void* self, HWND w, DWORD fl) {
        VtHook* vh = FindVt(self);
        if (!vh || !vh->coop) return E_FAIL;
        if (self == g_kbDev || self == g_msDev)
            ELog("pc2: DirectInput %s SetCooperativeLevel(hwnd %p, 0x%lX =%s%s%s%s)", self == g_kbDev ? "keyboard" : "mouse", w, fl,
                 (fl & 1) ? " EXCLUSIVE" : "", (fl & 2) ? " NONEXCLUSIVE" : "", (fl & 4) ? " FOREGROUND" : "", (fl & 8) ? " BACKGROUND" : "");
        return vh->coop(self, w, fl);
    }
    bool PatchSlot(void** vt, int i, void* fn, void** orig) {
        DWORD old;
        if (!VirtualProtect(&vt[i], 4, PAGE_EXECUTE_READWRITE, &old)) return false;
        if (orig) *orig = vt[i];
        vt[i] = fn;
        VirtualProtect(&vt[i], 4, old, &old);
        return true;
    }
    HRESULT __stdcall Hook_Create(void* self, REFGUID g, void** dev, void* outer) {
        HRESULT hr = g_origCreate(self, g, dev, outer);
        const bool kb = IsEqualGUID(g, kSysKeyboard), ms = IsEqualGUID(g, kSysMouse);
        if (SUCCEEDED(hr) && dev && *dev && (kb || ms)) {
            if (kb) g_kbDev = *dev; else g_msDev = *dev;
            void** vt = *reinterpret_cast<void***>(*dev);
            if (!FindVt(*dev) && g_nvth < 4) {
                VtHook& h = g_vth[g_nvth]; h.vt = vt; void* o = nullptr;
                if (PatchSlot(vt, 9, (void*)&Hook_State, &o)) h.st = (fn_state)o;
                if (PatchSlot(vt, 10, (void*)&Hook_Data, &o)) h.dd = (fn_ddata)o;
                if (PatchSlot(vt, 13, (void*)&Hook_Coop, &o)) h.coop = (fn_coop)o;
                InterlockedIncrement(&g_nvth);
            }
            ELog("pc2: DirectInput %s %p (vtable %p) - player 2's %s masked from it while split", kb ? "keyboard" : "mouse", *dev, vt,
                 kb ? "keys" : "movement + buttons");
        }
        return hr;
    }

    // ------------------------------------------------------------------ Windows cursor (JPOGMouseFix / the game)
    // JPOG71: does the game's pointer follow the Windows cursor? If yes (virtual_cursor auto/1), player 1 gets their own
    // cursor position that only player 1's mouse moves (player 2's mouse still moves the real arrow, but the game
    // doesn't see it).
    using fn_gcp = BOOL (WINAPI*)(LPPOINT);
    using fn_scp = BOOL (WINAPI*)(int, int);
    fn_gcp g_realGcp = nullptr; fn_scp g_realScp = nullptr;
    volatile LONG g_gcpGame = 0;
    bool GameFront();
    bool VcActive() {
        return g_capturing && g_p2Mouse && (S.virtualCursor == 1 || (S.virtualCursor == 2 && g_cursorGame)) && GameFront();
    }
    BOOL WINAPI Hook_Gcp(LPPOINT pt) {
        const void* ret = __builtin_return_address(0);
        BOOL ok = g_realGcp(pt);
        if (!InMod(ret, g_self)) {
            bool fresh; { Lock l; fresh = NewCaller(g_clCur, ret); }
            if (fresh) {
                char mod[96]; bool game = false; ModName(ret, mod, sizeof mod, &game);
                ELog("pc2: GetCursorPos called by %s%s", mod, game ? " (game folder: the game reads the Windows cursor)" : "");
                if (game) InterlockedExchange(&g_gcpGame, 1);
            }
            if (g_gcpGame && !g_cursorGame) { InterlockedExchange(&g_cursorGame, 1); }
        }
        if (!ok || !pt || InMod(ret, g_self)) return ok;
        Lock l;
        if (!VcActive()) { g_vcInit = false; return ok; }
        if (!g_vcInit) {
            g_vc = *pt; g_vcInit = true;
            int sp = 10; SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &sp, 0); g_vcScale = sp / 10.0f;
            LOG("pc2: player 1's own cursor on at %ld,%ld (speed %.2f) - player 2's mouse no longer moves player 1's pointer", g_vc.x, g_vc.y, g_vcScale);
        }
        *pt = g_vc;
        return ok;
    }
    BOOL WINAPI Hook_Scp(int x, int y) {
        { Lock l; if (g_vcInit) { g_vc.x = x; g_vc.y = y; } }
        return g_realScp(x, y);
    }

    // install one inline hook on an exported function; returns the trampoline (or nullptr)
    template <class F> F HookExport(HMODULE m, const char* name, void* fn, const char* tag) {
        void* a = m ? (void*)GetProcAddress(m, name) : nullptr;
        if (!a) { ELog("pc2: %s!%s not found", tag, name); return nullptr; }
        void* tr = nullptr;
        if (!detour::Install(a, fn, &tr)) {
            const BYTE* b = (const BYTE*)a;
            ELog("pc2: could NOT hook %s!%s at %p (bytes %02X %02X %02X %02X %02X %02X)", tag, name, a, b[0], b[1], b[2], b[3], b[4], b[5]);
            return nullptr;
        }
        char mod[96]; ModName(a, mod, sizeof mod);
        ELog("pc2: hooked %s!%s at %p (in %s)", tag, name, a, mod);
        return (F)tr;
    }

    // ------------------------------------------------------------------ player 2's virtual controller
    bool Held(int vk, DWORD now) { return g_vk[1][vk] || (g_vkT[1][vk] && now - g_vkT[1][vk] < 60); }
    float Clamp1(float v) { return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v; }
}

namespace kbm {

void EarlyInstall(void* realDinput8) {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    Load();
    IniInit();
    char m[16];
    IniS(L"p1_input", m, sizeof m, "auto"); g_mode[0] = !_stricmp(m, "kbm") ? 1 : !_stricmp(m, "pad") ? 0 : 2;
    IniS(L"p2_input", m, sizeof m, "pad"); g_mode[1] = !_stricmp(m, "kbm") ? 1 : 0;
    if (!S.enabled) { g_mode[1] = 0; return; }
    (void)realDinput8;
    // JPOG71: rev b decided "broken" from dinput8's import table (0 / 0 / 0 on the owner's Windows 11) and switched the
    // whole feature off. Now: inline hooks on the functions themselves (every caller, however it found them), callers
    // logged, and the split works with our own raw input window either way (passed on, or DirectInput masks).
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&EarlyInstall, &g_self);
    HMODULE u32 = GetModuleHandleA("user32.dll"); if (!u32) u32 = LoadLibraryA("user32.dll");
    g_user32 = u32;
    void* uReg  = (void*)GetProcAddress(u32, "RegisterRawInputDevices");
    void* uData = (void*)GetProcAddress(u32, "GetRawInputData");
    void* uBuf  = (void*)GetProcAddress(u32, "GetRawInputBuffer");
    g_realReg  = HookExport<fn_reg >(u32, "RegisterRawInputDevices", (void*)&Hook_Register, "user32");
    g_realData = HookExport<fn_data>(u32, "GetRawInputData", (void*)&Hook_GetData, "user32");
    g_realBuf  = HookExport<fn_buf >(u32, "GetRawInputBuffer", (void*)&Hook_GetBuffer, "user32");
    if (HMODULE w32 = GetModuleHandleA("win32u.dll")) {   // Windows 10/11: callers that skip user32
        if ((void*)GetProcAddress(w32, "NtUserRegisterRawInputDevices") != uReg)
            g_w32Reg = HookExport<fn_reg>(w32, "NtUserRegisterRawInputDevices", (void*)&Hook_W32Reg, "win32u");
        if ((void*)GetProcAddress(w32, "NtUserGetRawInputData") != uData)
            g_w32Data = HookExport<fn_data>(w32, "NtUserGetRawInputData", (void*)&Hook_W32Data, "win32u");
        if ((void*)GetProcAddress(w32, "NtUserGetRawInputBuffer") != uBuf)
            g_w32Buf = HookExport<fn_buf>(w32, "NtUserGetRawInputBuffer", (void*)&Hook_W32Buf, "win32u");
    } else ELog("pc2: no win32u.dll (older Windows) - user32 hooks only");
    g_realHookA = HookExport<fn_hookA>(u32, "SetWindowsHookExA", (void*)&Hook_HookA, "user32");
    g_realHookW = HookExport<fn_hookA>(u32, "SetWindowsHookExW", (void*)&Hook_HookW, "user32");
    if (S.virtualCursor) {
        g_realGcp = HookExport<fn_gcp>(u32, "GetCursorPos", (void*)&Hook_Gcp, "user32");
        g_realScp = HookExport<fn_scp>(u32, "SetCursorPos", (void*)&Hook_Scp, "user32");
    }
    if (!g_realReg || !g_realData) {
        // without these we can't even read devices safely through our hooks: use the plain functions, no passing on
        ELog("pc2: raw input hooks failed - the split can still run with the DirectInput masks");
        if (!g_realReg)  g_realReg  = (fn_reg)uReg;
        if (!g_realData) g_realData = (fn_data)uData;
        if (!g_realBuf)  g_realBuf  = (fn_buf)uBuf;
        if (!g_realReg || !g_realData) InterlockedExchange(&g_broken, 1);
    }
    // saved player 2 devices / mode -> capture from the start
    if (!g_broken && (g_mode[1] || S.p2Kb[0] || S.p2Mouse[0])) g_wantCapture = 1;
    HANDLE t = CreateThread(nullptr, 0, StartThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

void OnDirectInput(void* di) {
    static bool s_done = false;
    if (s_done || !di) return;
    s_done = true;
    Load();
    if (!S.enabled) return;
    void* o = nullptr;
    if (PatchSlot(*reinterpret_cast<void***>(di), 3, (void*)&Hook_Create, &o)) g_origCreate = (fn_create)o;
}

// JPOG71 (owner 10:57): player 1 "auto" (default) = keyboard + mouse unless TWO controllers are connected -> with only one
// controller it is player 2's. F8 / Options / Back + Start set player 1 to a fixed pad or kbm ([pc2] p1_input).
bool IsKbm(int p) {
    p &= 1;
    int pads = padslots::ConnectedCount();
    if (pads == 0 && g_bootPads > 0 && GetTickCount() - g_bootT < 20000) pads = g_bootPads;   // before the first pad poll
    bool r;
    if (pads >= 2 && !g_runSet[p]) r = false;   // JPOG73: two controllers = JPOG69 behaviour unless switched in THIS run
    else if (p) r = g_mode[1] != 0;
    else { const LONG m = g_mode[0]; r = m == 1 || (m == 2 && pads < 2); }
    static volatile LONG s_last[2] = {-1, -1};
    if (InterlockedExchange(&s_last[p], r ? 1 : 0) != (r ? 1 : 0))
        LOG("pc2: input owner: player %d = %s (controllers %d, ini mode %ld, set this run %ld)", p + 1, r ? "KEYBOARD + MOUSE" : "CONTROLLER", pads, (long)g_mode[p], (long)g_runSet[p]);
    // JPOG73 double check: the start-up skipped the saved capture (2 controllers) and a controller went away later ->
    // player 2 is keyboard + mouse again, so start the capture now (else player 2 would have no input at all).
    if (p == 1 && r && !g_capturing && !g_broken && S.enabled) StartCapture("player 2 keyboard + mouse (fewer than 2 controllers)");
    return r;
}

void SetKbm(int p, bool on, const char* why) {
    p &= 1;
    if (IsKbm(p) == on && !(p == 0 && g_mode[0] == 2)) return;
    if (p == 1 && on && (g_broken || !S.enabled)) {
        LOG("pc2: player 2 keyboard mode refused (%s) - the split is off on this PC", why);
        Hint(L"Player 2 keyboard + mouse is not available on this PC (see jpogmod.log)", 4.0f);
        return;
    }
    InterlockedExchange(&g_mode[p], on ? 1 : 0);
    MarkChanged(p);
    LOG("pc2: player %d -> %s (%s)", p + 1, on ? "KEYBOARD + MOUSE" : "CONTROLLER", why);
}

bool PadCombo() { Load(); return S.enabled && S.padCombo; }

void ToggleFromPad(int p) { SetKbm(p, !IsKbm(p), "controller: hold Back + Start"); }

bool FillPad2(bool* down, float& lx, float& ly, float& rx, float& ry, float& lt, float& rt) {
    if (!g_capturing) return false;
    const DWORD now = GetTickCount();
    bool p2Open = false;   // player 2 has a disc / window / build tool open (Esc = close it, else Esc = pause menu)
    bool p2Tool = false;   // JPOG73: player 2's build tool (placement keeps the old mouse = camera / stick)
    GUARDED("pc2 p2 menu state", { p2Tool = builder2::Open(1); p2Open = p2menu::IsOpen(1) || p2menu::WindowOpen(1) || p2Tool; });
    Lock l;
    if (!g_p2Kb && !g_p2Mouse) return false;
    const bool front = GameFront();
    for (int i = 0; i < PAD_COUNT; ++i) down[i] = false;
    lx = ly = rx = ry = lt = rt = 0.0f;
    if (!front) return true;   // connected, but nothing pressed while another window is in front
    const bool riding = dinocontrol::Controls(1);
    auto K = [&](int vk) { return Held(vk, now); };
    // JPOG71 (owner 10:57): player 2 = the SAME keys + mouse as the vanilla PC game (Data\GUI\controls.ini);
    // only the dinosaur keys are the mod's own. Mouse movement over the last 50 ms -> stick.
    float mx = 0, my = 0;
    for (auto& m : g_mv) if (m.t && now - m.t < 50) { mx += m.dx; my += m.dy; }
    const float k = 0.035f * S.mouseSpeed;
    mx = Clamp1(mx * k); my = Clamp1(my * k);
    if (fabsf(mx) < 0.08f) mx = 0.0f; if (fabsf(my) < 0.08f) my = 0.0f;   // a resting / nudged mouse doesn't drift the view
    int wheel = 0; for (auto& w : g_wh) if (w.t && now - w.t < 150) wheel += w.d;
    const bool lmb = g_mb[0] || (g_mbT[0] && now - g_mbT[0] < 60);
    const bool ctrl = K(VK_LCONTROL) || K(VK_RCONTROL), shift = K(VK_LSHIFT) || K(VK_RSHIFT);
    // camera: Up/Down/Left/Right = W/S/A/D + arrows, RotateLeft/Right = Q/E + Delete/PgDn, ZoomIn/Out = R/F + wheel
    lx = (K('D') || K(VK_RIGHT) ? 1.0f : 0.0f) - (K('A') || K(VK_LEFT) ? 1.0f : 0.0f);
    ly = (K('W') || K(VK_UP) ? 1.0f : 0.0f) - (K('S') || K(VK_DOWN) ? 1.0f : 0.0f);
    rx = (K('E') || K(VK_NEXT) ? 1.0f : 0.0f) - (K('Q') || K(VK_DELETE) ? 1.0f : 0.0f);
    float zoom = (K('R') ? 1.0f : 0.0f) - (K('F') ? 1.0f : 0.0f);
    if (wheel) zoom += wheel > 0 ? 1.0f : -1.0f;
    // JPOG73: pointer mode (park view, nothing of player 2's open, not riding): the mouse moves player 2's own pointer;
    // pointer at the half's edge scrolls; right / middle button (or Ctrl / Shift) still turn / pan the camera.
    const bool ptrBase = S.pointer && !riding && splitscreen::Active() && splitscreen::ActivePlayer() == 0;
    const bool ptrWorld = ptrBase && (!p2Open || p2Tool);   // build tool: placement at the pointer (vanilla PC)
    const bool ptrMenu  = ptrBase && p2Open && !p2Tool;   // JPOG73 (owner 14:28 priority): the pointer clicks INSIDE player 2's discs / windows
    const bool ptrMode = ptrWorld || ptrMenu;
    InterlockedExchange(&g_ptrMenu, ptrMenu ? 1 : 0);
    if (!ptrMenu) InterlockedExchange(&g_menuClick, 0);   // double check: no stale click carried into the next menu
    if (InterlockedExchange(&g_ptrMode, ptrMode ? 1 : 0) != (ptrMode ? 1 : 0) || (ptrMenu != s_lastMenu)) LOG("pc2: player 2 pointer %s", ptrMenu ? "ON in a menu / window (left click = pick, right click = back)" : ptrMode ? "ON (mouse = pointer)" : "off (mouse = camera / build tool)");
    s_lastMenu = ptrMenu;
    if (riding || (g_mb[1] && !ptrMenu && !p2Tool) || ctrl) { rx = Clamp1(rx + mx); zoom = Clamp1(zoom - my); }   // ModifierZoomRotate = Ctrl / right button
    else if (g_mb[2] || shift || (S.mousePans && !ptrMode)) { lx = Clamp1(lx + mx); ly = Clamp1(ly - my); }   // ModifierTranslate = Shift / middle
    if (ptrWorld && S.edgeScroll > 0.0f) {
        const float e = S.edgeScroll;
        if (g_ptr[0] <= e) lx = -1.0f; else if (g_ptr[0] >= 1.0f - e) lx = 1.0f;
        if (g_ptr[1] <= e) ly = 1.0f;  else if (g_ptr[1] >= 1.0f - e) ly = -1.0f;
    }
    ry = S.zoomInvert ? -zoom : zoom;
    // Select / Place = left button, Enter, keypad Enter
    down[PAD_A] = K(VK_RETURN) || (!riding && lmb && !ptrMenu && (!ptrMode || P2HoverButton() < 0));   // JPOG73: pointer = A at the pointer; in a menu the click is p2menu's
    // JPOG73: pointer mode - a left click on a HUD button pulses that button's pad action; elsewhere = a click on the world
    static bool s_lmbPrev = false;
    const bool lmbEdge = g_mb[0] && !s_lmbPrev; s_lmbPrev = g_mb[0];
    if (ptrMode && lmbEdge) {
        const int b = P2HoverButton();
        if (b >= 0) { g_btnPulseT[b] = now + 150; LOG("pc2: player 2 clicked HUD button %d", b); }
        else if (ptrMenu) InterlockedExchange(&g_menuClick, 1);
        else { g_clickPt[0] = g_ptr[0]; g_clickPt[1] = g_ptr[1]; InterlockedExchange(&g_worldClick, 1); LOG("pc2: player 2 clicked the world at pointer %.2f,%.2f", g_ptr[0], g_ptr[1]); }
    }
    static bool s_rmbPrev = false;
    const bool rmbEdge = g_mb[1] && !s_rmbPrev; s_rmbPrev = g_mb[1];
    if ((ptrMenu || (ptrWorld && p2Tool)) && rmbEdge) g_btnPulseT[7] = now + 150;   // right click in a menu / the build tool = back (B)
    auto pulse = [&](int b) { return g_btnPulseT[b] && (LONG)(g_btnPulseT[b] - now) > 0; };
    if (pulse(7)) down[PAD_B] = true;
    // Back / Close / Cancel = Esc: closes player 2's disc / window / tool; with nothing open = the pause menu.
    // Decided when Esc goes down (so the frame after a close doesn't open the pause menu).
    static DWORD s_escT = 0; static int s_escAs = 0;
    if (g_vkT[1][VK_ESCAPE] != s_escT) { s_escT = g_vkT[1][VK_ESCAPE]; s_escAs = (p2Open || riding) ? 1 : 2; }
    if (K(VK_ESCAPE)) { if (s_escAs == 1) down[PAD_B] = true; else down[PAD_START] = true; }
    down[PAD_X]  = K('B') || K('P') || pulse(P2B_BUILD);   // Build / PathMode -> player 2's Build disc
    if (pulse(P2B_OPTIONS)) down[PAD_START] = true;        // JPOG73 HUD button: Options (pause menu)
    down[PAD_LB] = K(VK_OEM_4);                            // PrevSelection [
    down[PAD_RB] = K(VK_OEM_6) || K(VK_OEM_5);             // NextSelection ]  NextPage backslash
    down[PAD_BACK] = K(VK_F2);                             // GotoResearch F2 (Back tap = Research)
    down[PAD_LS] = K(VK_TAB);                              // TacMapActivate Tab
    down[PAD_RS] = false;
    if (!riding) {
        // Objectives F1, Fossil hunt F3, DNA lab F4, Fossil market F5, Mail M: those screens are on player 2's Manager disc
        down[PAD_Y] = K(VK_F1) || K(VK_F3) || K(VK_F4) || K(VK_F5) || K('M') || pulse(P2B_MANAGER) || pulse(P2B_MAIL);
        rt = (K(VK_SPACE) || K('C') || pulse(P2B_ACTIONS)) ? 1.0f : 0.0f;   // SelectionHelp Space / C -> the Actions disc for the selection
        lt = (K('G') || pulse(P2B_DINOS)) ? 1.0f : 0.0f;  // (no vanilla key) Dinosaurs disc = G
        down[PAD_DLEFT] = K(VK_OEM_COMMA); down[PAD_DRIGHT] = K(VK_OEM_PERIOD);   // (no vanilla key) build piece < >
    }
    // dinosaur keys (the mod's own): T = take control (RB + D-pad Up); riding: left click bites, middle click pounces
    static bool s_tBlock = false;                           // after riding, T must be let go before it takes control again
    if (riding) s_tBlock = true; else if (!K('T')) s_tBlock = false;
    if (!riding && !s_tBlock && K('T')) { down[PAD_RB] = true; down[PAD_DUP] = true; }
    if (riding) { if (lmb) rt = 1.0f; if (g_mb[2]) lt = 1.0f; }
    return true;
}

bool P2PointerMode() { return g_ptrMode != 0 && g_capturing; }
bool P2MenuPointer() { return g_ptrMenu != 0 && P2PointerMode(); }
bool P2WorldPointer() { return g_ptrMenu == 0 && P2PointerMode(); }
bool P2TakeMenuClick() { return InterlockedExchange(&g_menuClick, 0) != 0; }
bool P2Pointer(float& x, float& y) { x = g_ptr[0]; y = g_ptr[1]; return P2PointerMode(); }
float P2CursorScale() { return S.cursorScale; }
void P2SetHalfAspect(float a) { if (a > 0.2f && a < 5.0f) g_p2Aspect = a; }
// Layout = player 1's PC toolbar (owner photo JPOG72_owner_test_player1_hud_buttons_closeup.jpg): five round buttons in
// an arc rising from bottom-left (Build, Manager, Dinosaurs, Actions, red Options), mail flag bottom-right. Button
// diameter D = [pc2] hud_scale of the half's height; centres measured off the photo in units of D (x from the left
// edge, y up from the bottom edge).
namespace { const float kArcX[5] = { 0.75f, 1.80f, 2.82f, 3.76f, 4.57f }, kArcY[5] = { 1.80f, 1.82f, 1.61f, 1.18f, 0.62f }; }
bool P2ButtonRect(int id, float& x0, float& y0, float& x1, float& y1) {
    if (id < 0 || id >= P2B_COUNT || !S.hudButtons) return false;
    const float a = g_p2Aspect > 0.2f ? g_p2Aspect : 1.0f, D = S.hudScale, Dx = D / a;
    float cx, cy, hx, hy;
    if (id == P2B_MAIL) { hx = Dx * 0.30f; hy = D * 0.25f; cx = 1.0f - Dx * 0.55f; cy = 1.0f - D * 0.55f; }   // 25x21 flag, 1.5x
    else { hx = Dx * 0.5f; hy = D * 0.5f; cx = kArcX[id] * Dx; cy = 1.0f - kArcY[id] * D; }
    x0 = cx - hx; x1 = cx + hx; y0 = cy - hy; y1 = cy + hy;
    return true;
}
int P2HoverButton() {
    if (!P2PointerMode()) return P2B_NONE;
    const float a = g_p2Aspect > 0.2f ? g_p2Aspect : 1.0f;
    for (int i = 0; i < P2B_COUNT; ++i) {
        float x0, y0, x1, y1;
        if (!P2ButtonRect(i, x0, y0, x1, y1)) continue;
        const float px = g_ptr[0], py = g_ptr[1];
        if (i == P2B_MAIL) { if (px >= x0 && px <= x1 && py >= y0 && py <= y1) return i; continue; }
        const float dx = (px - (x0 + x1) * 0.5f) * a, dy = py - (y0 + y1) * 0.5f, r = (y1 - y0) * 0.5f;
        if (dx * dx + dy * dy <= r * r) return i;            // round buttons: round hit area
    }
    return P2B_NONE;
}
bool P2TakeWorldClick(float& x, float& y) {
    if (!InterlockedExchange(&g_worldClick, 0)) return false;
    x = g_clickPt[0]; y = g_clickPt[1];
    return true;
}

bool KeyDown(int p, int vk) {
    if (vk <= 0 || vk >= 256) return false;
    if (!g_capturing) return p == 0 ? (GetAsyncKeyState(vk) & 0x8000) != 0 : false;
    p &= 1;
    if (vk == VK_SHIFT)   return g_vk[p][VK_LSHIFT] || g_vk[p][VK_RSHIFT];
    if (vk == VK_CONTROL) return g_vk[p][VK_LCONTROL] || g_vk[p][VK_RCONTROL];
    if (vk == VK_MENU)    return g_vk[p][VK_LMENU] || g_vk[p][VK_RMENU];
    return g_vk[p][vk] || (g_vkT[p][vk] && GetTickCount() - g_vkT[p][vk] < 60);
}

void Tick() {
    Load();
    if (!S.enabled) return;
    // before the split runs: F8 (player 1) / Shift+F8 (player 2 joins) through the normal keyboard state
    static bool s_prevF8 = false;
    // JPOG71: also the game's own DirectInput keyboard state (rev b: Shift+F8 did nothing, no log line - an exclusive
    // DirectInput keyboard can keep keys from GetAsyncKeyState). DIK_F1..F10 = 0x3B..0x44, F11 0x57, F12 0x58.
    const int vkN = S.toggleVk - VK_F1 + 1;
    const int dik = vkN <= 10 ? 0x3A + vkN : vkN == 11 ? 0x57 : 0x58;
    const bool f8 = !g_capturing && GameFront() && ((GetAsyncKeyState(S.toggleVk) & 0x8000) != 0 || g_diKey[dik]);
    if (f8 && !s_prevF8) {
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 || g_diKey[0x2A] || g_diKey[0x36];
        LOG("pc2: %sF8 pressed (before the split; Windows %d, DirectInput %d)", shift ? "Shift+" : "", (GetAsyncKeyState(S.toggleVk) & 0x8000) != 0, (int)g_diKey[dik]);
        if (shift) {
            if (g_broken) Hint(L"Player 2 keyboard + mouse is not available on this PC (see jpogmod.log)", 4.0f);
            else {
                StartCapture("Shift+F8");
                { Lock l; g_waitKbUntil = GetTickCount() + 20000; }
                Hint(L"Player 2: press F8 on YOUR keyboard (then click with your mouse)", 6.0f);
            }
        } else SetKbm(0, !IsKbm(0), "F8");
    }
    s_prevF8 = f8;
    // mode changes: release held keys, hint, save
    for (int p = 0; p < 2; ++p) {
        if (!InterlockedExchange(&g_modeChanged[p], 0)) continue;
        const bool on = IsKbm(p);
        IniW(p ? L"p2_input" : L"p1_input", on ? "kbm" : "pad");
        input_map::Shutdown();
        if (p == 1 && on) {
            StartCapture("player 2 keyboard mode");
            bool have; { Lock l; have = g_p2Kb || g_p2Mouse; }
            if (!have) {
                { Lock l; g_waitKbUntil = GetTickCount() + 20000; }
                Hint(L"Player 2: press F8 on YOUR keyboard (then click with your mouse)", 6.0f);
                continue;
            }
        }
        Hint(p ? (on ? L"Player 2: keyboard + mouse (F8 = controller)" : L"Player 2: controller (F8 on your keyboard = keyboard + mouse)")
               : (on ? L"Player 1: keyboard + mouse (F8 = controller)" : L"Player 1: controller (F8 = keyboard + mouse)"), 3.5f);
        LOG("pc2: player %d now uses %s", p + 1, on ? "keyboard + mouse" : "the controller");
    }
    {   // mouse join timed out
        Lock l;
        if (g_waitMouseUntil && (int)(GetTickCount() - g_waitMouseUntil) >= 0) {
            g_waitMouseUntil = 0;
            if (!g_p2Mouse) { wcsncpy_s(g_hint, L"Player 2: no mouse picked - press Shift+F8 on your keyboard to try again", _TRUNCATE); g_hintSecs = 4.0f; g_hintPending = 1; }
        }
        if (g_waitKbUntil && (int)(GetTickCount() - g_waitKbUntil) >= 0) g_waitKbUntil = 0;
    }
    if (InterlockedExchange(&g_hintPending, 0)) {
        wchar_t t[200]; float s; { Lock l; wcsncpy_s(t, g_hint, _TRUNCATE); s = g_hintSecs; }
        // JPOG79: in the half of the player the hint is about ("Player 1: ..." / "Player 2: ...")
        const int who = !wcsncmp(t, L"Player 2", 8) ? 1 : !wcsncmp(t, L"Player 1", 8) ? 0 : -1;
        gamectl::RequestHintFor(who, t, s);
    }
}

} // namespace kbm
