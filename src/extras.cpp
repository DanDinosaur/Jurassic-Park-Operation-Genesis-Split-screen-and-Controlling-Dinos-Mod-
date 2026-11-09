// extras.cpp - JPOG67: the comfort features the mod used to get from JPOG-Modernize, written from the game's own code /
// the DirectInput + Win32 APIs (that plugin's source/binary was not used). All off while JPOGMouseFix.asi is loaded.
//   [display] free_keyboard : the game opens the keyboard EXCLUSIVE through DirectInput (Windows key / Print Screen / Alt+Tab
//                             swallowed). Our dinput8 proxy wraps IDirectInput8::CreateDevice (vtable 3) and, for the keyboard
//                             device only, IDirectInputDevice8::SetCooperativeLevel (vtable 13): EXCLUSIVE|NOWINKEY -> NONEXCLUSIVE.
//   [display] alt_f4_quit   : game window subclassed; Alt+F4 ends the process (the game ignores WM_CLOSE).
//   [display] borderless    : when the game runs WINDOWED (Options.ini ScreenWindowed = true) the window loses its frame and
//                             covers its monitor.
//   [mouse] cursor_speed / no_accel : TGUIScreen pointer speed +0x11C (deltas * speed, TGUIScreen::UpdatePointerPosition) and
//                             acceleration mode +0x140 (TGUIScreen::ApplyPointerAccel doubles fast moves when != 0).
//   [camera] fix_smoothing  : AWorldCamera::OnUpdate 0x527C10 damps its push / rotate / zoom velocity ONCE PER FRAME
//                             (v = v * scm_fDampening [0xD12C00] + input * scm_fOMDampening [0x8B61F0]) - at 60 fps the
//                             speed-up and braking take half the time of the 30 fps the game was tuned for. For each call the
//                             two constants become D' = D^(fps_ref * dt), OM' = OM * (1 - D') / (1 - D) (same top speed),
//                             restored right after.
#include <windows.h>
#include <cstdint>
#include <cmath>
#include "extras.h"
#include "config.h"
#include "detour.h"
#include "guard.h"
#include "game.h"
#include "log.h"

namespace {
    bool Allowed() { return GetModuleHandleA("JPOGMouseFix.asi") == nullptr; }
    void* Ptr(void* p, int off) { return p ? *reinterpret_cast<void**>((uint8_t*)p + off) : nullptr; }

    // ---- DirectInput keyboard ----
    const GUID kSysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
    using fn_create = HRESULT(__stdcall*)(void* self, REFGUID g, void** dev, void* outer);
    using fn_coop   = HRESULT(__stdcall*)(void* self, HWND w, DWORD flags);
    fn_create g_origCreate = nullptr;
    fn_coop   g_origCoop = nullptr;
    void* volatile g_keyboard = nullptr;
    bool PatchSlot(void** vt, int i, void* fn, void** orig) {
        DWORD old;
        if (!VirtualProtect(&vt[i], 4, PAGE_EXECUTE_READWRITE, &old)) return false;
        if (orig) *orig = vt[i];
        vt[i] = fn;
        VirtualProtect(&vt[i], 4, old, &old);
        return true;
    }
    HRESULT __stdcall Hook_Coop(void* self, HWND w, DWORD flags) {
        if (self == g_keyboard && (flags & 0x1)) {
            DWORD f = (flags & ~(0x1u | 0x10u)) | 0x2u;   // EXCLUSIVE + NOWINKEY -> NONEXCLUSIVE
            LOG("extras: keyboard cooperative level 0x%lX -> 0x%lX (Windows key, Print Screen, Alt+Tab work)", flags, f);
            flags = f;
        }
        return g_origCoop(self, w, flags);
    }
    HRESULT __stdcall Hook_Create(void* self, REFGUID g, void** dev, void* outer) {
        HRESULT hr = g_origCreate(self, g, dev, outer);
        if (SUCCEEDED(hr) && dev && *dev && IsEqualGUID(g, kSysKeyboard)) {
            g_keyboard = *dev;
            if (!g_origCoop) {
                void* o = nullptr;
                if (PatchSlot(*reinterpret_cast<void***>(*dev), 13, (void*)&Hook_Coop, &o)) g_origCoop = (fn_coop)o;
                LOG("extras: DirectInput keyboard %p - SetCooperativeLevel wrapped (%d)", *dev, g_origCoop != nullptr);
            }
        }
        return hr;
    }

    // ---- window: Alt+F4, borderless ----
    HWND g_wnd = nullptr; WNDPROC g_origProc = nullptr; bool g_bordered = false;
    LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_SYSKEYDOWN && w == VK_F4 && config::Get().altF4Quit) {
            LOG("extras: Alt+F4 - closing the game");
            TerminateProcess(GetCurrentProcess(), 0);
        }
        return CallWindowProcA(g_origProc, h, m, w, l);
    }
    BOOL CALLBACK FindGameWnd(HWND h, LPARAM p) {
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
        RECT r; GetWindowRect(h, &r);
        if (r.right - r.left < 320) return TRUE;
        *reinterpret_cast<HWND*>(p) = h;
        return FALSE;
    }
    void WindowTick(unsigned frame) {
        const Config& c = config::Get();
        if (!g_wnd) {
            if (frame % 30) return;
            HWND h = nullptr; EnumWindows(FindGameWnd, (LPARAM)&h);
            if (!h) return;
            g_wnd = h;
            if (c.altF4Quit) {
                g_origProc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)&Proc);
                LOG("extras: game window %p - Alt+F4 closes the game", h);
            }
        }
        if (c.borderless && !g_bordered && (frame % 60) == 0) {
            LONG st = GetWindowLongA(g_wnd, GWL_STYLE);
            if (!(st & WS_CAPTION)) {
                static bool s_logged = false;
                if (!s_logged) { s_logged = true; LOG("extras: borderless asked, but the game runs fullscreen - set ScreenWindowed = true; in Options.ini"); }
                return;
            }
            HMONITOR mon = MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{}; mi.cbSize = sizeof mi; GetMonitorInfoA(mon, &mi);
            SetWindowLongA(g_wnd, GWL_STYLE, (st & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP);
            SetWindowPos(g_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            g_bordered = true;
            LOG("extras: borderless window %ldx%ld", mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
        }
    }

    // ---- GUI pointer speed / acceleration ----
    constexpr uintptr_t kGetRootTask = 0x00484DB0, kApp = 0x008B8580, kGetGuiSystem = 0x0047D370;
    float g_spd0 = -1, g_spdLast = -1;
    void PointerTick() {
        const Config& c = config::Get();
        void* root = reinterpret_cast<void*(__fastcall*)(void*, void*)>(game::At(kGetRootTask))(game::At(kApp), nullptr);
        void* sys = root ? reinterpret_cast<void*(__fastcall*)(void*, void*)>(game::At(kGetGuiSystem))(root, nullptr) : nullptr;
        uint8_t* sc = (uint8_t*)Ptr(sys, 0x28);
        if (!sc) return;
        float* spd = (float*)(sc + 0x11C);
        if (*spd != g_spdLast) {   // first time, or the game set it (options)
            g_spd0 = *spd;
            g_spdLast = g_spd0 * (c.mouseSpeed > 0.05f ? c.mouseSpeed : 0.05f);
            *spd = g_spdLast;
            static unsigned s_logs = 0;
            if (s_logs < 6) { ++s_logs; LOG("extras: cursor speed %.3f -> %.3f ([mouse] cursor_speed %.2f)", g_spd0, g_spdLast, c.mouseSpeed); }
        }
        if (c.cursorNoAccel) *(int*)(sc + 0x140) = 0;
    }

    // ---- world camera smoothing at any FPS ----
    constexpr uintptr_t kWorldCamUpdate = 0x00527C10, kDamp = 0x00D12C00, kOMDamp = 0x008B61F0;
    using fn_upd = bool(__fastcall*)(void*, void*, float);
    fn_upd g_origUpd = nullptr;
    float g_d0 = 0, g_om0 = 0; unsigned g_camLogs = 0;
    bool __fastcall Hook_CamUpdate(void* cam, void* edx, float dt) {
        float* d = (float*)game::At(kDamp); float* om = (float*)game::At(kOMDamp);
        const float fps = config::Get().camSmoothingFps;
        bool swapped = false;
        if (dt > 0.0f && dt < 0.5f && g_d0 > 0.0f && g_d0 < 1.0f && fps > 1.0f) {
            const float dn = powf(g_d0, fps * dt);
            *d = dn; *om = g_om0 * (1.0f - dn) / (1.0f - g_d0);
            swapped = true;
            if (g_camLogs < 2 || (g_camLogs % 20000) == 0)
                LOG("extras: camera smoothing dt %.4f: dampening %.3f -> %.3f, input %.3f -> %.3f", dt, g_d0, dn, g_om0, *om);
            ++g_camLogs;
        }
        bool r = g_origUpd(cam, edx, dt);
        if (swapped) { *d = g_d0; *om = g_om0; }
        return r;
    }
}

namespace extras {

void OnDirectInput(void* di) {
    static bool s_done = false;
    if (s_done || !di) return;
    s_done = true;
    if (!config::Get().freeKeyboard || !Allowed()) return;
    void* o = nullptr;
    if (PatchSlot(*reinterpret_cast<void***>(di), 3, (void*)&Hook_Create, &o)) g_origCreate = (fn_create)o;
}

void Install() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    if (!Allowed()) { LOG("extras: JPOGMouseFix.asi is loaded - our keyboard / window / cursor / camera extras stay off"); return; }
    const Config& c = config::Get();
    if (c.camFixSmoothing) {
        float* d = (float*)game::At(kDamp); float* om = (float*)game::At(kOMDamp);
        DWORD o1, o2;
        if (VirtualProtect(d, 4, PAGE_READWRITE, &o1) && VirtualProtect(om, 4, PAGE_READWRITE, &o2)) {
            g_d0 = *d; g_om0 = *om;
            void* t = nullptr;
            if (g_d0 > 0.0f && g_d0 < 1.0f && detour::Install(game::At(kWorldCamUpdate), (void*)&Hook_CamUpdate, &t)) {
                g_origUpd = (fn_upd)t;
                LOG("extras: park camera smoothing fixed to %.0f fps behaviour (dampening %.3f, input %.3f)", c.camSmoothingFps, g_d0, g_om0);
            } else LOG("extras: camera smoothing NOT hooked (dampening %.3f)", g_d0);
        }
    }
    LOG("extras: on - free keyboard %d, Alt+F4 %d, borderless %d, cursor speed %.2f, no accel %d",
        c.freeKeyboard, c.altF4Quit, c.borderless, c.mouseSpeed, c.cursorNoAccel);
}

void Tick(unsigned frame) {
    static int s_ok = -1;
    if (s_ok < 0) s_ok = Allowed() ? 1 : 0;
    if (!s_ok) return;
    GUARDED("extras window", { WindowTick(frame); });
    GUARDED("extras pointer", { PointerTick(); });
}

} // namespace extras
