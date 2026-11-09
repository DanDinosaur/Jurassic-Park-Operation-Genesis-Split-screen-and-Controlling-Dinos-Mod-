// padcursor.cpp - see padcursor.h (JPOG59, open list items 1 + 4).
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include "padcursor.h"
#include "config.h"
#include "game.h"
#include "gamectl.h"
#include "input_map.h"
#include "guard.h"
#include "log.h"

namespace padcursor {
namespace {
    template <class T = void*> T& F(void* o, int off) { return *reinterpret_cast<T*>((uint8_t*)o + off); }
    template <class T> T VFn(void* o, int off) { return reinterpret_cast<T>((*reinterpret_cast<void***>(o))[off / 4]); }

    using fn_b   = bool (__fastcall*)(void* self, void* edx);
    using fn_bb  = void (__fastcall*)(void* self, void* edx, bool);
    using fn_btn = bool (__fastcall*)(void* self, void* edx, void* ev, int action);

    fn_b  g_canFocus = nullptr; fn_bb g_setFocused = nullptr;
    bool  g_apiTried = false, g_apiOk = false;
    bool Api() {
        if (g_apiTried) return g_apiOk;
        g_apiTried = true;
        HMODULE gi = GetModuleHandleA("TGuiInterface.dll");
        if (gi) {
            g_canFocus   = reinterpret_cast<fn_b>(GetProcAddress(gi, "?CanFocus@TGUIWidget@Toshi@@QAE_NXZ"));
            g_setFocused = reinterpret_cast<fn_bb>(GetProcAddress(gi, "?SetFocused@TGUIWidget@Toshi@@QAEX_N@Z"));
        }
        g_apiOk = g_canFocus && g_setFocused;
        LOG("padcursor: GUI functions %s", g_apiOk ? "found" : "MISSING - pad cursor in windows off");
        return g_apiOk;
    }

    constexpr int kParent = 0x44, kFirst = 0x4C, kNext = 0x08, kState = 0x88;
    bool Visible(void* w) { return (F<uint32_t>(w, kState) & 5) == 5; }

    void AbsPos(void* w, int& x, int& y) {
        x = 0; y = 0;
        for (int i = 0; w && i < 32; ++i) { x += F<short>(w, 0x6C); y += F<short>(w, 0x6E); w = F(w, kParent); }
    }
    void Centre(void* w, int& x, int& y) { AbsPos(w, x, y); x += F<short>(w, 0x70) / 2; y += F<short>(w, 0x72) / 2; }

    // Deepest focusable widgets under w (a container that accepts focus only counts when nothing inside it does).
    int Collect(void* w, void** out, int n, int max, int depth) {
        if (!w || depth > 12 || n >= max) return n;
        const int before = n;
        void* sentinel = (uint8_t*)w + kParent;
        void* c = F(w, kFirst);
        for (int i = 0; c && c != sentinel && i < 256 && n < max; ++i) {
            if (Visible(c)) n = Collect(c, out, n, max, depth + 1);
            c = F(c, kNext);
        }
        if (n == before && depth > 0 && VFn<fn_b>(w, 0x40)(w, nullptr) && g_canFocus(w, nullptr)) out[n++] = w;
        return n;
    }

    bool InTree(void* root, void* w) {
        for (int i = 0; w && i < 32; ++i) { if (w == root) return true; w = F(w, kParent); }
        return false;
    }

    void Highlight(Cursor& c, void* w) {
        if (c.cur == w) return;
        if (c.cur) GUARDED("padcursor unhighlight", { g_setFocused(c.cur, nullptr, false); });
        c.cur = w;
        if (w) GUARDED("padcursor highlight", { g_setFocused(w, nullptr, true); });
    }

    void* PickFirst(void* root) {
        void* list[96]; int n = 0;
        if (!GUARDED("padcursor collect", { n = Collect(root, list, 0, 96, 0); }) || n == 0) return nullptr;
        // the window's own default focus first (TGUIWindow +0x8C), if it is one of ours
        void* def = F(root, 0x8C);
        for (int i = 0; i < n; ++i) if (list[i] == def) return def;
        void* best = nullptr; long bestV = 0x7FFFFFFF;
        for (int i = 0; i < n; ++i) {
            int x, y; Centre(list[i], x, y);
            const long v = (long)y * 4 + x;                  // top row first, then left
            if (v < bestV) { bestV = v; best = list[i]; }
        }
        return best;
    }
    // ---- JPOG62 item A: pickers (PGUISliderBar "tCycleSpinner" - hatchery Category / Species / Release mode, Number)
    // and list / table boxes take the D-pad / stick direction themselves before the highlight moves on.
    // PGUISliderBar (PGUICore): +0xA0 min, +0xA4 max, +0xA8 value, +0xAC step, +0xB0 orientation (0 horizontal, 1 vertical).
    // SetCurrentValue clamps + moves the knob but sends no event; ThrowValueChange (protected, exported) is what the dialog
    // listens to. Its own OnButtonPress calls the event's DEVICE (ev+0xC) for rumble -> never call that one.
    using fn_getf = float(__fastcall*)(void* self, void* edx);
    using fn_setf = void (__fastcall*)(void* self, void* edx, float v);
    using fn_v    = void (__fastcall*)(void* self, void* edx);
    void* g_vtSlider = nullptr; void* g_vtList = nullptr; void* g_vtTable = nullptr;
    fn_setf g_sliderSet = nullptr; fn_v g_sliderThrow = nullptr;
    bool g_spinTried = false, g_spinOk = false;
    bool SpinApi() {
        if (g_spinTried) return g_spinOk;
        g_spinTried = true;
        HMODULE pc = GetModuleHandleA("PGUICore.dll");
        if (pc) {
            g_vtSlider    = (void*)GetProcAddress(pc, "??_7PGUISliderBar@@6B@");
            g_vtList      = (void*)GetProcAddress(pc, "??_7PGUIListBox@@6B@");
            g_vtTable     = (void*)GetProcAddress(pc, "??_7PGUITableBox@@6B@");
            g_sliderSet   = reinterpret_cast<fn_setf>(GetProcAddress(pc, "?SetCurrentValue@PGUISliderBar@@QAEXM@Z"));
            g_sliderThrow = reinterpret_cast<fn_v>(GetProcAddress(pc, "?ThrowValueChange@PGUISliderBar@@IAEXXZ"));
        }
        g_spinOk = g_vtSlider && g_sliderSet && g_sliderThrow;
        LOG("padcursor: picker functions %s (slider %p list %p table %p)", g_spinOk ? "found" : "MISSING - pickers off",
            g_vtSlider, g_vtList, g_vtTable);
        return g_spinOk;
    }
    // Dummy input device for list / table boxes (they may call ev+0xC vfn +0x30 rumble(code, f, f) / +0x34 (code)).
    struct DummyDev { void** vt; };
    void __fastcall Dev_Ret0(void*, void*) {}
    void __fastcall Dev_Rumble(void*, void*, int, float, float) {}
    void __fastcall Dev_Repeat(void*, void*, int) {}
    void* g_devVt[32]; DummyDev g_dev{ g_devVt }; bool g_devInit = false;
    void* Dev() {
        if (!g_devInit) {
            for (auto& f : g_devVt) f = (void*)&Dev_Ret0;
            g_devVt[0x30 / 4] = (void*)&Dev_Rumble; g_devVt[0x34 / 4] = (void*)&Dev_Repeat;
            g_devInit = true;
        }
        return &g_dev;
    }
    void* VT(void* o) { return *reinterpret_cast<void**>(o); }
    // Offer the direction to w (or a parent up to 2 levels that is a picker). true = it used it.
    bool OfferDirection(Cursor& c, void* w, int dx, int dy) {
        if (!config::Get().windowSpinners || !SpinApi()) return false;
        for (int lvl = 0; w && lvl < 3; ++lvl, w = F(w, kParent)) {
            void* vt = VT(w);
            if (vt == g_vtSlider) {
                const int orient = F<int>(w, 0xB0);
                int dir = 0;
                if (orient == 0 && dx) dir = dx;          // horizontal: right = +, left = -
                else if (orient == 1 && dy) dir = -dy;    // vertical: up = +, down = -
                if (!dir) return false;
                const float mn = F<float>(w, 0xA0), mx = F<float>(w, 0xA4), cur = F<float>(w, 0xA8);
                float st = F<float>(w, 0xAC); if (!(st > 0.0f)) st = 1.0f;
                float nv = cur + dir * st;
                // JPOG69 (owner 17:20, hatchery species list "only switches between 2 or 3 at the end"): the hatchery's
                // Category / Species pickers are STEP pickers - the game puts the value back to 0 after every step and
                // narrows the range at the list ends (start 0..1, middle -1..1, end -1..0). Wrapping one of those turned
                // "right at the end" into -1 = one step BACK -> ping-pong between the last species. Step pickers (range
                // inside -1..1 around 0) now stop at the ends like the mouse arrows do; real small pickers still wrap.
                const bool stepPicker = mn >= -1.001f && mx <= 1.001f && mn <= 0.001f && mx >= -0.001f && fabsf(cur) < 0.001f;
                const bool cycle = !stepPicker && (mx - mn) / st <= 20.0f;   // small pickers wrap round (cycle feel), big sliders stop
                if (nv > mx + 0.001f) nv = cycle ? mn : mx;
                if (nv < mn - 0.001f) nv = cycle ? mx : mn;
                if (nv == cur) return true;
                bool ok = GUARDED("padcursor spinner", { g_sliderSet(w, nullptr, nv); g_sliderThrow(w, nullptr); });
                if (c.logs < 16) { ++c.logs; LOG("padcursor: %s - spinner %p %.0f -> %.0f (range %.0f..%.0f step %.0f)%s", c.who, w, cur, nv, mn, mx, st, ok ? "" : " FAULTED"); }
                return true;
            }
            if (vt && (vt == g_vtList || vt == g_vtTable)) {
                const int bit = dx > 0 ? 0x400 : dx < 0 ? 0x1000 : dy < 0 ? 0x200 : 0x800;
                struct Ev { int code; int type; uint8_t isFloat, hasMag, pad_[2]; void* dev; float m0, m1; };
                Ev ev{ 0x7FFF0002, 0, 0, 0, {}, Dev(), 0.0f, 0.0f };
                bool took = false;
                bool ok = GUARDED("padcursor list", {
                    took = VFn<fn_btn>(w, 0x60)(w, nullptr, &ev, bit);
                    if (took) { ev.type = 1; VFn<fn_btn>(w, 0x68)(w, nullptr, &ev, bit); }
                });
                if (c.logs < 16) { ++c.logs; LOG("padcursor: %s - list %p direction bit %X -> %s", c.who, w, bit, !ok ? "FAULTED" : took ? "used" : "not used"); }
                return ok && took;
            }
        }
        return false;
    }
} // namespace

bool Valid(const Cursor& c) { return c.root && F(c.root, kParent) && Visible(c.root); }

bool Begin(Cursor& c, void* root, const char* who) {
    if (!Api() || !root) return false;
    if (c.root == root && c.cur) return true;
    End(c);
    c.root = root; c.who = who; c.logs = 0;
    void* first = PickFirst(root);
    Highlight(c, first);
    LOG("padcursor: %s - window %p, first button %p", who, root, first);
    return true;
}

void End(Cursor& c) {
    if (c.cur && g_apiOk) GUARDED("padcursor end", { if (InTree(c.root, c.cur)) g_setFocused(c.cur, nullptr, false); });
    c.cur = nullptr; c.root = nullptr;
}

void Refresh(Cursor& c) {
    if (!c.root || !g_apiOk) return;
    bool ok = true;
    GUARDED("padcursor refresh", { ok = c.cur && InTree(c.root, c.cur) && g_canFocus(c.cur, nullptr); });
    if (!ok) { c.cur = nullptr; Highlight(c, PickFirst(c.root)); }
    else if (c.cur && !(F<uint32_t>(c.cur, kState) & 2)) GUARDED("padcursor re-highlight", { g_setFocused(c.cur, nullptr, true); });
}

bool Move(Cursor& c, int dx, int dy) {
    if (!c.root || !g_apiOk || (!dx && !dy)) return false;
    Refresh(c);
    if (!c.cur) return false;
    if (OfferDirection(c, c.cur, dx, dy)) return true;   // JPOG62: pickers / lists use the direction first
    void* list[96]; int n = 0;
    if (!GUARDED("padcursor collect", { n = Collect(c.root, list, 0, 96, 0); })) return false;
    int cx, cy; Centre(c.cur, cx, cy);
    void* best = nullptr; long bestS = 0x7FFFFFFF;
    for (int i = 0; i < n; ++i) {
        if (list[i] == c.cur) continue;
        int x, y; Centre(list[i], x, y);
        const int ax = (x - cx) * dx + (y - cy) * dy;              // distance along the pushed direction
        const int ox = dx ? abs(y - cy) : abs(x - cx);             // sideways distance
        if (ax <= 2) continue;
        const long s = (long)ax + (long)ox * 3;
        if (s < bestS) { bestS = s; best = list[i]; }
    }
    if (!best) return false;
    Highlight(c, best);
    if (c.logs < 8) { ++c.logs; LOG("padcursor: %s - highlight %p (pushed %d,%d)", c.who, best, dx, dy); }
    return true;
}

bool Click(Cursor& c) {
    if (!c.root || !g_apiOk) return false;
    Refresh(c);
    if (!c.cur) return false;
    struct Ev { int code; int type; uint8_t isFloat, hasMag, pad_[2]; void* dev; float m0, m1; };
    Ev ev{ 0x7FFF0001, 0, 0, 0, {}, nullptr, 0.0f, 0.0f };
    void* w = c.cur; void* took = nullptr;
    bool ok = GUARDED("padcursor click", {
        for (void* p = w; p && p != F(c.root, kParent); p = F(p, kParent)) {
            if (VFn<fn_btn>(p, 0x60)(p, nullptr, &ev, 1)) { took = p; break; }
        }
        if (took) { ev.type = 1; VFn<fn_btn>(took, 0x68)(took, nullptr, &ev, 1); }
    });
    LOG("padcursor: %s - A on %p -> %s", c.who, w, !ok ? "FAULTED" : took ? (took == w ? "clicked" : "clicked (parent)") : "nobody took it");
    if (!ok) c.cur = nullptr;
    return ok && took;
}

// ---------------------------------------------------------------------------------------------------------------
// Item 4: the HUD player's vanilla info bar (AUnitInfoState, dialog at state+0x84, its widget at dialog+0x2C).
namespace {
    constexpr uintptr_t kUnitInfoStateClass = 0x008B5520;   // AUnitInfoState::m_sClass
    Cursor g_hud;
    volatile LONG g_hudActive = 0;
    volatile LONG g_reqDx = 0, g_reqDy = 0, g_reqClick = 0;
    int g_hudLogs = 0;
}
bool HudActive() { return g_hudActive != 0; }
void RequestHud(int dx, int dy, bool click) {
    if (dx || dy) { InterlockedExchange(&g_reqDx, dx); InterlockedExchange(&g_reqDy, dy); }
    if (click) InterlockedExchange(&g_reqClick, 1);
}
void Tick(float) {
    void* root = nullptr;
    if (config::Get().infoBarPad && input_map::ConsoleActive() && gamectl::InPark()) {
        GUARDED("padcursor info state", {
            void* st = gamectl::CurrentState();
            if (st && gamectl::IsA(st, kUnitInfoStateClass)) {
                void* dlg = F(st, 0x84);
                void* w = dlg ? F(dlg, 0x2C) : nullptr;
                if (w && F(w, kParent) && Visible(w)) root = w;
            }
        });
    }
    if (!root) {
        if (g_hud.root) { End(g_hud); LOG("padcursor: info bar closed - pad cursor off"); }
        InterlockedExchange(&g_hudActive, 0);
        InterlockedExchange(&g_reqDx, 0); InterlockedExchange(&g_reqDy, 0); InterlockedExchange(&g_reqClick, 0);
        return;
    }
    if (g_hud.root != root) {
        Begin(g_hud, root, "info bar (HUD player)");
        if (g_hudLogs < 3) { ++g_hudLogs; gamectl::RequestHint(L"D-pad = pick a button, A = press it, B = close", 3.0f); }
    }
    InterlockedExchange(&g_hudActive, g_hud.cur ? 1 : 0);
    const int dx = InterlockedExchange(&g_reqDx, 0), dy = InterlockedExchange(&g_reqDy, 0);
    if (dx || dy) Move(g_hud, dx, dy);
    if (InterlockedExchange(&g_reqClick, 0)) Click(g_hud);
}
} // namespace padcursor
