// widescreen.cpp - JPOG66: our own Hor+ / unstretched GUI (see widescreen.h). Written from the game's own code:
//
// 3D (TRenderInterface.dll, Toshi::TCameraViewportHandler::OnBeginRenderEvent, export 0xEB60):
//   FOV mode 0 (horizontal, camera +0x94): scale = 0.5 * viewport width / tan(FOV/2) for x and y (y * pixel aspect).
//   A fixed HORIZONTAL FOV on a 16:9 viewport = the 4:3 picture with top and bottom cut off ("Vert-").
//   Hor+: for that one call the camera's FOV (+0x90) is widened so tan(f'/2) = tan(f/2) * aspect / (4/3), then restored.
//   The vertical FOV then equals the 4:3 one and the extra width shows more at the sides. Viewports narrower than 4:3
//   (a vertical split half) keep the vanilla maths (horizontal FOV kept, more shown top/bottom).
//
// GUI (PGUIRenderer.dll, PGUITRDisplayContext::BeginScene 0x100012E0):
//   ortho projection {centre 0,0; scale ScaleX, ScaleY} with ScaleX = viewport w / 800, ScaleY = viewport h / 600
//   (PGUITRRenderObject::SetScreen) = the GUI stretched to the screen. PGUIRenderer's ONLY call of
//   TRenderContext::SetProjectionParameters is that one, so its import slot is redirected: one uniform scale
//   s = min(Rw / 800, Rh / 600) * k, centred in R (R = full screen, or the HUD player's half in split-screen).
//   The GUI pointer is relative (mouse deltas in GUI units, TGUIScreen::UpdatePointerPosition), so cursor and clicks
//   follow automatically.
//
// The game's own conversions that assumed the stretch now use the same mapping:
//   ACamera::ConvertGuiScreenToRenderScreen 0x50BD60 / ConvertRenderScreenToGuiScreen 0x50BDD0 (stdcall, 2 args) - replaced;
//   ARenderer::ConvertToScreenCoords 0x477EC0 (unit icons, thought bubbles, highlight boxes through
//   ConvertBBoxToScreenCoords): vanilla output = viewport pixels * (800 / display width) for x AND y - converted after the
//   call into our mapping (the (-100,-100) "behind the camera" result is left alone).
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include "widescreen.h"
#include "splitscreen.h"
#include "config.h"
#include "detour.h"
#include "guard.h"
#include "game.h"
#include "log.h"

extern "C" __declspec(dllexport) int __stdcall JPOGMod_GetHudRect(float* rect4);

namespace {
    struct ProjParams { float cx, cy, sx, sy, nearZ, farZ; };
    using fn_setproj  = void(__fastcall*)(void* ctx, void* edx, const ProjParams* p);
    using fn_begin    = bool(__stdcall*)(void* handler, void* viewport, void* ev);
    using fn_toscreen = void(__fastcall*)(void* renderer, void* edx, float* v3);
    using fn_guisize  = void(__stdcall*)(float* out2);

    constexpr uintptr_t kConvGuiToRender = 0x0050BD60, kConvRenderToGui = 0x0050BDD0, kGetGuiScreenSize = 0x0050BCE0;
    constexpr uintptr_t kToScreenCoords = 0x00477EC0, kGetRootTask = 0x00484DB0, kApp = 0x008B8580;

    bool g_on = false, g_horPlus = false, g_guiFix = false;
    fn_setproj  g_origSetProj = nullptr;
    fn_begin    g_origBegin = nullptr;
    fn_toscreen g_origToScreen = nullptr;
    using fn_conv = void(__stdcall*)(const float*, float*);
    fn_conv g_origG2R = nullptr, g_origR2G = nullptr;
    void*       g_renderer = nullptr;            // ARenderer (frame hook "self"), for the main viewport

    // The mapping in use (screen pixels, absolute): gui (gx, gy) -> (g_ox + gx * g_s, g_oy + gy * g_s).
    volatile bool g_mapValid = false;
    float g_ox = 0, g_oy = 0, g_s = 1, g_gw = 800, g_gh = 600;
    unsigned g_logGui = 0, g_logHor = 0, g_logConv = 0;

    void* Ptr(void* p, int off) { return p ? *reinterpret_cast<void**>((uint8_t*)p + off) : nullptr; }

    // ---- JPOG67: HUD panels pinned to the real edges of the GUI rectangle ----
    // The game places every HUD element once by screen corner (AGUISystem::PositionHudElement 0x67C7A0: x / y from the
    // GUI size +0x70/+0x72 minus margins). With the GUI centred, the spare room (or the overhang, split_scale > 1) is
    // dx = (Rw / s - 800) / 2 GUI units per side. Before each GUI pass every HUD element's widget (AGUISystem +0xD0 list,
    // next +8, end = sys + 0xC8, widget = element +0x18; position shorts +0x6C/+0x6E, size +0x70/+0x72) is moved out by
    // dx / dy towards its nearest edge (left / right third; top / bottom third when there is vertical room). The game's own
    // value is remembered (a changed x/y = the game or our split code moved it = new original). The disc menus open at a
    // fixed bottom-left spot (AMenuState::OnActivate: x = [0x8AFAA8] = 4, y = screen h - [0x8AFAB4] = 32, ints) - moved too.
    constexpr uintptr_t kGetGuiSystem = 0x0047D370, kDiscX = 0x008AFAA8, kDiscYFromBottom = 0x008AFAB4;
    struct Pin { void* w; short ox, oy, lx, ly; };
    Pin g_pins[96]; int g_nPins = 0; int g_discX0 = -9999, g_discY0 = -9999; unsigned g_pinLogs = 0;
    Pin* PinFor(void* w) {
        for (int i = 0; i < g_nPins; ++i) if (g_pins[i].w == w) return &g_pins[i];
        if (g_nPins >= 96) g_nPins = 0;   // park reloads: start over
        Pin& p = g_pins[g_nPins++];
        short* xy = (short*)((uint8_t*)w + 0x6C);
        p.w = w; p.ox = p.lx = xy[0]; p.oy = p.ly = xy[1];
        return &p;
    }
    void PinHud(float dx, float dy, float gw, float gh) {
        void* root = reinterpret_cast<void*(__fastcall*)(void*, void*)>(game::At(kGetRootTask))(game::At(kApp), nullptr);
        uint8_t* sys = root ? (uint8_t*)reinterpret_cast<void*(__fastcall*)(void*, void*)>(game::At(kGetGuiSystem))(root, nullptr) : nullptr;
        if (!sys) return;
        const int idx = (int)lroundf(dx), idy = (int)lroundf(dy);
        int n = 0;
        for (uint8_t* e = (uint8_t*)Ptr(sys, 0xD0); e && e != sys + 0xC8 && n < 200; e = (uint8_t*)Ptr(e, 8), ++n) {
            void* w = Ptr(e, 0x18);
            if (!w) continue;
            short* xy = (short*)((uint8_t*)w + 0x6C);
            const short ww = xy[2], wh = xy[3];
            Pin* p = PinFor(w);
            if (xy[0] != p->lx) p->ox = xy[0];
            if (xy[1] != p->ly) p->oy = xy[1];
            const float cx = p->ox + ww * 0.5f, cy = p->oy + wh * 0.5f;
            int sx = 0, sy = 0;
            if (cx < gw * 0.4f) sx = -idx; else if (cx > gw * 0.6f) sx = idx;
            if (cy < gh * 0.4f) sy = -idy; else if (cy > gh * 0.6f) sy = idy;
            xy[0] = p->lx = (short)(p->ox + sx);
            xy[1] = p->ly = (short)(p->oy + sy);
            if (g_pinLogs < 24 && (sx || sy) && (g_pinLogs < 12 || (g_pinLogs % 4) == 0)) {
                ++g_pinLogs;
                LOG("widescreen: HUD panel %p (%d,%d %dx%d) pinned by %d,%d", w, p->ox, p->oy, ww, wh, sx, sy);
            }
        }
        int* dX = (int*)game::At(kDiscX); int* dY = (int*)game::At(kDiscYFromBottom);
        if (g_discX0 == -9999) { g_discX0 = *dX; g_discY0 = *dY; }
        *dX = g_discX0 - idx; *dY = g_discY0 - idy;
    }

    bool GuiSize(float& gw, float& gh) {
        float v[2] = { 0, 0 };
        GUARDED("gui screen size", { reinterpret_cast<fn_guisize>(game::At(kGetGuiScreenSize))(v); });
        if (v[0] < 16 || v[1] < 16) return false;
        gw = v[0]; gh = v[1];
        return true;
    }
    // Main 3D viewport origin + size (ARenderer +0x2C TViewport -> +0x34 TRenderContext, VIEWPORTPARAMS at +0x18).
    bool MainViewport(float& x, float& y, float& w, float& h) {
        uint8_t* rc = (uint8_t*)Ptr(Ptr(g_renderer, 0x2C), 0x34);
        if (!rc) return false;
        x = *(float*)(rc + 0x18); y = *(float*)(rc + 0x1C); w = *(float*)(rc + 0x20); h = *(float*)(rc + 0x24);
        return w > 1 && h > 1;
    }

    // ---- GUI projection (PGUIRenderer's SetProjectionParameters import) ----
    void __fastcall Hook_SetProj(void* ctx, void* edx, const ProjParams* p) {
        ProjParams q = *p;
        bool mapped = false;
        GUARDED("widescreen gui map", {
            float gw, gh;
            uint8_t* rc = (uint8_t*)ctx;
            const float vx = *(float*)(rc + 0x18), vy = *(float*)(rc + 0x1C), vw = *(float*)(rc + 0x20), vh = *(float*)(rc + 0x24);
            if (g_guiFix && vw > 1 && vh > 1 && GuiSize(gw, gh)) {
                float R[6] = { vx, vy, vw, vh, vw, vh };
                bool split = splitscreen::Active() && JPOGMod_GetHudRect(R) != 0;
                const Config& c = config::Get();
                float s = R[2] / gw; if (R[3] / gh < s) s = R[3] / gh;
                float k = split ? c.hudSplitScale : c.hudScale;
                if (k < 1.0f) k = 1.0f; if (k > 2.0f) k = 2.0f;
                if (k > 1.0f) { float kmax = R[2] / (s * gw * 0.5f); if (k > kmax) k = kmax; }   // keep >= half the GUI width
                s *= k;
                const float ox = R[0] + (R[2] - gw * s) * 0.5f, oy = R[1] + (R[3] - gh * s) * 0.5f;
                q.cx = ox - vx; q.cy = oy - vy; q.sx = s; q.sy = s;
                g_ox = ox; g_oy = oy; g_s = s; g_gw = gw; g_gh = gh; g_mapValid = true;
                const float pdx = c.hudPinEdges ? (R[2] / s - gw) * 0.5f : 0.0f, pdy = c.hudPinEdges ? (R[3] / s - gh) * 0.5f : 0.0f;
                GUARDED("widescreen pin hud", { PinHud(pdx, pdy, gw, gh); });
                mapped = true;
                if (g_logGui < 4 || (g_logGui < 40 && (g_logGui % 8) == 0)) {
                    LOG("widescreen: GUI %.0fx%.0f -> scale %.3f at (%.0f,%.0f) in %s rect %.0f,%.0f %.0fx%.0f (viewport %.0f,%.0f %.0fx%.0f, game scale %.3f x %.3f)",
                        gw, gh, s, ox, oy, split ? "the HUD player's" : "the full", R[0], R[1], R[2], R[3], vx, vy, vw, vh, p->sx, p->sy);
                }
                ++g_logGui;
            }
        });
        g_origSetProj(ctx, edx, mapped ? &q : p);
    }

    // ---- Hor+ (TCameraViewportHandler::OnBeginRenderEvent) ----
    bool __stdcall Hook_Begin(void* handler, void* viewport, void* ev) {
        float* fov = nullptr; float saved = 0;
        GUARDED("widescreen hor+", {
            uint8_t* cam = handler ? (uint8_t*)*(void**)handler : nullptr;
            uint8_t* rc = viewport ? (uint8_t*)*(void**)((uint8_t*)viewport + 0x34) : nullptr;
            if (g_horPlus && cam && rc && cam[0xB0] && *(int*)(cam + 0x94) == 0 && *(int*)(cam + 0x98) == 0) {
                const float w = *(float*)(rc + 0x20), h = *(float*)(rc + 0x24);
                const float aspect = h > 1 ? w / h : 0;
                if (aspect > 4.0f / 3.0f + 0.01f) {
                    fov = (float*)(cam + 0x90); saved = *fov;
                    const float t = tanf(saved * 0.5f) * aspect * 0.75f;
                    float wide = 2.0f * atanf(t);
                    if (wide > 3.0f) wide = 3.0f;          // < 172 degrees
                    *fov = wide;
                    if (g_logHor < 3 || (g_logHor % 3000) == 0)
                        LOG("widescreen: Hor+ camera %p viewport %.0fx%.0f (aspect %.3f): FOV %.1f -> %.1f deg for the projection",
                            cam, w, h, aspect, saved * 57.29578f, wide * 57.29578f);
                    ++g_logHor;
                }
            }
        });
        bool r = g_origBegin(handler, viewport, ev);
        if (fov) *fov = saved;
        return r;
    }

    // ---- the game's GUI <-> render-screen conversions ----
    void __stdcall Hook_GuiToRender(const float* in, float* out) {
        float vx = 0, vy = 0, vw = 0, vh = 0;
        if (!g_mapValid || !MainViewport(vx, vy, vw, vh)) { g_origG2R(in, out); return; }
        float x = g_ox + in[0] * g_s - vx, y = g_oy + in[1] * g_s - vy;
        out[0] = x < 0 ? 0 : (x > vw ? vw : x);
        out[1] = y < 0 ? 0 : (y > vh ? vh : y);
    }
    void __stdcall Hook_RenderToGui(const float* in, float* out) {
        float vx = 0, vy = 0, vw = 0, vh = 0;
        if (!g_mapValid || !MainViewport(vx, vy, vw, vh)) { g_origR2G(in, out); return; }
        float x = (in[0] + vx - g_ox) / g_s, y = (in[1] + vy - g_oy) / g_s;
        out[0] = x < 0 ? 0 : (x > g_gw ? g_gw : x);
        out[1] = y < 0 ? 0 : (y > g_gh ? g_gh : y);
    }
    // Vanilla: out = viewport pixels * (gui width / display width). Undo that factor, then our mapping.
    void __fastcall Hook_ToScreen(void* renderer, void* edx, float* v) {
        g_origToScreen(renderer, edx, v);
        if (!g_mapValid || !v || (v[0] == -100.0f && v[1] == -100.0f)) return;
        GUARDED("widescreen icon map", {
            int dispW = 0;
            void* root = reinterpret_cast<void*(__fastcall*)(void*, void*)>(game::At(kGetRootTask))(game::At(kApp), nullptr);
            if (void* o = Ptr(root, 0xD0)) {
                int* wh = reinterpret_cast<int*(__fastcall*)(void*, void*)>((*reinterpret_cast<void***>(o))[0x24 / 4])(o, nullptr);
                if (wh) dispW = wh[0];
            }
            float vx, vy, vw, vh;
            if (dispW > 0 && MainViewport(vx, vy, vw, vh)) {
                const float f = g_gw / (float)dispW;
                const float px = v[0] / f + vx, py = v[1] / f + vy;
                const float gx = (px - g_ox) / g_s, gy = (py - g_oy) / g_s;
                if (g_logConv < 3) { ++g_logConv; LOG("widescreen: icon projection game GUI %.0f,%.0f -> ours %.0f,%.0f", v[0], v[1], gx, gy); }
                v[0] = gx; v[1] = gy;
            }
        });
    }

    // Redirect one import of `module` (from `fromDll`) whose current value is `oldFn`.
    void** FindIat(HMODULE mod, const char* fromDll, void* oldFn) {
        if (!mod) return nullptr;
        auto* b = (uint8_t*)mod;
        auto* dos = (IMAGE_DOS_HEADER*)b;
        auto* nt = (IMAGE_NT_HEADERS*)(b + dos->e_lfanew);
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return nullptr;
        for (auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(b + dir.VirtualAddress); d->Name; ++d) {
            if (_stricmp((const char*)(b + d->Name), fromDll) != 0) continue;
            for (auto* t = (IMAGE_THUNK_DATA*)(b + d->FirstThunk); t->u1.Function; ++t)
                if ((void*)t->u1.Function == oldFn) return (void**)&t->u1.Function;
        }
        return nullptr;
    }
}

namespace widescreen {

bool Active() { return g_on && g_guiFix && g_mapValid; }
float SplitScale() { return config::Get().hudSplitScale; }

void Install() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    const Config& c = config::Get();
    if (!c.ownWidescreen) { LOG("widescreen: our own widescreen is off ([hud] widescreen = 0)"); return; }
    if (GetModuleHandleA("JPOGMouseFix.asi")) {
        LOG("widescreen: JPOGMouseFix.asi (JPOG-Modernize) is loaded - it handles Hor+ and the HUD, ours stays off");
        return;
    }
    // Renderer: the frame hook's "self" is ARenderer; GetRootTask()->renderer would also do, take it from the main scene
    // via the same object splitscreen uses (ARootTask +0xD4 = ARenderer, as ACamera::GetRenderScreenSize reads it).
    GUARDED("widescreen renderer", {
        void* root = reinterpret_cast<void*(__fastcall*)(void*, void*)>(game::At(kGetRootTask))(game::At(kApp), nullptr);
        g_renderer = Ptr(root, 0xD4);
    });
    HMODULE tri = GetModuleHandleA("TRenderInterface.dll"), gui = GetModuleHandleA("PGUIRenderer.dll");
    void* setProj = tri ? (void*)GetProcAddress(tri, "?SetProjectionParameters@TRenderContext@Toshi@@QAEXABUPROJECTIONPARAMS@12@@Z") : nullptr;
    void* begin = tri ? (void*)GetProcAddress(tri, "?OnBeginRenderEvent@TCameraViewportHandler@Toshi@@SG_NPAV12@PAVTViewport@2@PAVBeginRenderEvent@32@@Z") : nullptr;
    g_on = true;
    if (c.hudGuiFix) {
        void** slot = FindIat(gui, "TRenderInterface.dll", setProj);
        DWORD old;
        if (slot && VirtualProtect(slot, 4, PAGE_READWRITE, &old)) {
            g_origSetProj = (fn_setproj)setProj;
            *slot = (void*)&Hook_SetProj;
            VirtualProtect(slot, 4, old, &old);
            void* t1 = nullptr; void* t2 = nullptr; void* t3 = nullptr;
            bool a = detour::Install(game::At(kConvGuiToRender), (void*)&Hook_GuiToRender, &t1);
            bool b = detour::Install(game::At(kConvRenderToGui), (void*)&Hook_RenderToGui, &t2);
            bool d = detour::Install(game::At(kToScreenCoords), (void*)&Hook_ToScreen, &t3);
            g_origToScreen = (fn_toscreen)t3; g_origG2R = (fn_conv)t1; g_origR2G = (fn_conv)t2;
            g_guiFix = true;
            LOG("widescreen: GUI drawn unstretched (uniform scale, centred; split-screen: HUD player's half). Converters %d/%d, icons %d. Scale %.2f, split %.2f",
                a, b, d, c.hudScale, c.hudSplitScale);
        } else LOG("widescreen: PGUIRenderer import of SetProjectionParameters not found (gui=%p fn=%p) - GUI left stretched", gui, setProj);
    }
    if (c.hudHorPlus) {
        void* t = nullptr;
        if (begin && detour::Install(begin, (void*)&Hook_Begin, &t)) { g_origBegin = (fn_begin)t; g_horPlus = true; LOG("widescreen: Hor+ on (camera projection, TRenderInterface %p)", begin); }
        else LOG("widescreen: Hor+ hook FAILED (OnBeginRenderEvent %p)", begin);
    }
}

} // namespace widescreen
