// p2menu.cpp - see p2menu.h (JPOG49).
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "p2menu.h"
#include "builder2.h"
#include "xinput_pad.h"
#include "config.h"
#include "game.h"
#include "gamectl.h"
#include "splitscreen.h"
#include "guard.h"
#include "detour.h"
#include "log.h"
#include "padcursor.h"

namespace p2menu {
namespace {
    constexpr uintptr_t kGetGUISystem = 0x0067E3F0;   // AGUISystem::GetGUISystem (stdcall)
    constexpr uintptr_t kGetMenu      = 0x0067E500;   // AGUISystem::GetMenu() -> the game's AGUIMenu (tree)
    constexpr uintptr_t kMenuCtor     = 0x006779B0;   // AGUIMenu::AGUIMenu(), 0x94 bytes
    constexpr uintptr_t kFindChild    = 0x00674DB0;   // AGUIMenuItem::FindChildItem(const TPCString&, bool), ret 8
    constexpr uintptr_t kIatGetString = 0x00739D1C;   // TCString::GetString(int) (thiscall)
    constexpr uintptr_t kIatIsA       = 0x00739E20;   // TObject::IsA(TClass*)
    constexpr uintptr_t kMenuPosX     = 0x008AFAA8;   // AMenuState::OnActivate, position 2 (game disc): x = [8AFAA8],
    constexpr uintptr_t kMenuPosB     = 0x008AFAB4;   //   y = screen height - [8AFAB4]

    using fn_v      = void (__fastcall*)(void* self, void* edx);
    using fn_i      = void (__fastcall*)(void* self, void* edx, int v);
    using fn_p      = void (__fastcall*)(void* self, void* edx, void* p);
    using fn_pget   = void*(__fastcall*)(void* self, void* edx);
    using fn_invoke = bool (__fastcall*)(void* self, void* edx, void* item, int x, int y);
    using fn_axis   = bool (__fastcall*)(void* self, void* edx, void* ev, int n);
    using fn_setpos = void (__fastcall*)(void* self, void* edx, int x, int y);
    using fn_find   = void*(__fastcall*)(void* self, void* edx, const void* name, bool recurse);
    using fn_str    = const char*(__fastcall*)(void* self, void* edx, int);
    using fn_isA    = bool (__fastcall*)(void* self, void* edx, const void* cls);

    struct Api {
        fn_invoke invoke = nullptr; fn_p setScreen = nullptr, setIface = nullptr, setSel = nullptr;
        fn_i scroll = nullptr; fn_v select = nullptr, back = nullptr, remove = nullptr;
        fn_axis discAxis = nullptr; const void* discClass = nullptr;
        fn_v requestFocus = nullptr; fn_pget focused = nullptr; fn_p releaseFocus = nullptr; fn_setpos setPos = nullptr;
    } A;
    bool g_apiTried = false, g_apiOk = false, g_failed = false;
    int  g_fails = 0;

    template <class T> T Exp(HMODULE m, const char* n) { return m ? reinterpret_cast<T>(GetProcAddress(m, n)) : nullptr; }
    bool LoadApi() {
        if (g_apiTried) return g_apiOk;
        g_apiTried = true;
        HMODULE pc = GetModuleHandleA("PGUICore.dll"), gi = GetModuleHandleA("TGuiInterface.dll");
        A.invoke    = Exp<fn_invoke>(pc, "?InvokeMenu@PGUIMenuTree@@QAE_NPAVPGUIMenuTreeItem@@HH@Z");
        A.setScreen = Exp<fn_p>(pc, "?SetScreen@PGUIMenuTree@@QAEXPAVTGUIScreen@Toshi@@@Z");
        A.setIface  = Exp<fn_p>(pc, "?SetInterface@PGUIMenuTree@@UAEXPAVTGUIInterface@Toshi@@@Z");
        A.setSel    = Exp<fn_p>(pc, "?SetSelectedItem@PGUIMenuTree@@QAEXPAVPGUIMenuTreeItem@@@Z");
        A.scroll    = Exp<fn_i>(pc, "?ScrollMenu@PGUIMenuTree@@QAEXH@Z");
        A.select    = Exp<fn_v>(pc, "?SelectMenu@PGUIMenuTree@@QAEXXZ");
        A.back      = Exp<fn_v>(pc, "?BackMenu@PGUIMenuTree@@QAEXXZ");
        A.remove    = Exp<fn_v>(pc, "?RemoveMenus@PGUIMenuTree@@IAEXXZ");
        A.discAxis  = Exp<fn_axis>(pc, "?OnAxisMove@PGUIDiscMenuDialog@@UAE_NPAVInputEvent@TInputInterface@Toshi@@H@Z");
        A.discClass = Exp<const void*>(pc, "?m_sClass@PGUIDiscMenuDialog@@2VTClass@Toshi@@A");
        A.requestFocus = Exp<fn_v>(gi, "?RequestFocus@TGUIWidget@Toshi@@QAEXXZ");
        A.focused   = Exp<fn_pget>(gi, "?GetFocusedWidget@TGUIScreen@Toshi@@QBEPAVTGUIWidget@2@XZ");
        A.releaseFocus = Exp<fn_p>(gi, "?ReleaseFocus@TGUIScreen@Toshi@@QAEXPAVTGUIWidget@2@@Z");
        A.setPos    = Exp<fn_setpos>(gi, "?SetPosition@TGUIWidget@Toshi@@QAEXHH@Z");
        g_apiOk = A.invoke && A.setScreen && A.setIface && A.setSel && A.scroll && A.select && A.back && A.remove &&
                  A.discAxis && A.discClass && A.requestFocus && A.focused && A.releaseFocus && A.setPos;
        LOG("p2menu: GUI functions %s", g_apiOk ? "found" : "MISSING - player 2 keeps the old build list");
        return g_apiOk;
    }

    template <class T = void*> T& F(void* o, int off) { return *reinterpret_cast<T*>((uint8_t*)o + off); }

    void* g_tree = nullptr;        // player 2's own AGUIMenu (kept for the whole session)
    void* g_screen = nullptr;
    int   g_who = -1;
    bool  g_open = false, g_toolOpen = false, g_hinted = false;
    void* g_lastSel = nullptr;     // the item whose tool is open (B in the tool = back to it)
    int   g_menu = gamectl::MENU_BUILD;
    float g_repeat = 0.0f;
    int   g_logs = 0;
    struct Pos { void* d; short x, y; int ox, oy; };
    Pos g_pos[16]; int g_nPos = 0;

    void* GameTree() {
        void* sys = reinterpret_cast<void*(__stdcall*)()>(game::At(kGetGUISystem))();
        return sys ? reinterpret_cast<fn_pget>(game::At(kGetMenu))(sys, nullptr) : nullptr;
    }

    bool EnsureTree() {
        void* t1 = nullptr;
        if (!GUARDED("p2menu game tree", { t1 = GameTree(); }) || !t1) return false;
        void* sc = F(t1, 0x0C);
        if (!sc || !F(t1, 0x10)) return false;
        if (!g_tree) {
            void* mem = gamectl::Alloc(0x94);
            if (!mem) return false;
            bool ok = GUARDED("p2menu create tree", {
                reinterpret_cast<fn_v>(game::At(kMenuCtor))(mem, nullptr);
                A.setIface(mem, nullptr, F(t1, 0x08));
                A.setScreen(mem, nullptr, sc);
                for (int o = 0x54; o <= 0x60; o += 4) F<int>(mem, o) = F<int>(t1, o);   // screen margins
                F(mem, 0x10) = F(t1, 0x10);    // the game's root item: UpdateMenu walks item parents up to it
            });
            if (!ok) { g_failed = true; LOG("p2menu: creating player 2's menu faulted - old build list instead"); return false; }
            g_tree = mem; g_screen = sc;
            LOG("p2menu: player 2's own disc menu %p (game's %p, screen %p, margins %d %d %d %d)", mem, t1, sc,
                F<int>(mem, 0x54), F<int>(mem, 0x58), F<int>(mem, 0x5C), F<int>(mem, 0x60));
        }
        if (sc != g_screen) {
            GUARDED("p2menu new screen", { A.setScreen(g_tree, nullptr, sc); });
            LOG("p2menu: GUI screen changed %p -> %p", g_screen, sc);
            g_screen = sc;
        }
        F(g_tree, 0x10) = F(t1, 0x10);
        return true;
    }

    // Everything player 1 must keep: the screen's input-manager stack and the focused widget.
    struct Keep {
        void* sc; void* w = nullptr; int cnt = 0;
        explicit Keep(void* s) : sc(s) { w = A.focused(sc, nullptr); cnt = F<int>(sc, 0xCC); }
        void Restore() {
            int c = F<int>(sc, 0xCC);
            if (c == cnt + 1) F<int>(sc, 0xCC) = cnt;            // InvokeMenu pushed our tree: pop it again
            else if (c != cnt && g_logs < 10) { ++g_logs; LOG("p2menu: input stack %d -> %d (left alone)", cnt, c); }
            void* now = A.focused(sc, nullptr);
            if (now != w) {
                if (w) A.requestFocus(w, nullptr);
                else if (now) A.releaseFocus(sc, nullptr, now);
            }
        }
    };

    void Hide() {
        if (!g_tree) return;
        if (g_open) GUARDED("p2menu remove", { A.remove(g_tree, nullptr); F<uint8_t>(g_tree, 0x65) = 0; });
        g_open = false; g_nPos = 0;
    }

    // Move every on-screen dialog of our tree from the HUD half into this player's half. UpdateMenu re-places them
    // (clamped to the HUD half) whenever the selection changes; anything at the spot we set is already moved.
    void Reposition() {
        if (!g_open || !g_tree) return;
        int dx = 0, dy = 0;
        if (!splitscreen::GuiOffsetTo(g_who, dx, dy)) return;
        const int n = F<int>(g_tree, 0x24);
        void** arr = F<void**>(g_tree, 0x2C);
        if (!arr || n <= 0 || n > 16) return;
        Pos np[16]; int nn = 0;
        for (int i = 0; i < n; ++i) {
            void* d = arr[i];
            if (!d || !F(d, 0x44)) continue;                         // not on the screen
            short x = F<short>(d, 0x6C), y = F<short>(d, 0x6E);
            const Pos* r = nullptr;
            for (int k = 0; k < g_nPos; ++k) if (g_pos[k].d == d) { r = &g_pos[k]; break; }
            int bx = x, by = y;
            if (r && r->x == x && r->y == y) {
                if (r->ox == dx && r->oy == dy) { np[nn++] = *r; continue; }
                bx = x - r->ox; by = y - r->oy;                       // halves swapped: undo the old offset
            }
            A.setPos(d, nullptr, bx + dx, by + dy);
            np[nn++] = Pos{ d, F<short>(d, 0x6C), F<short>(d, 0x6E), dx, dy };
            if (g_logs < 6) { ++g_logs; LOG("p2menu: dialog %p moved to player %d's half (%d,%d -> %d,%d)", d, g_who + 1, bx, by, bx + dx, by + dy); }
        }
        memcpy(g_pos, np, sizeof(Pos) * nn); g_nPos = nn;
    }

    template <class Fn> bool Call(const char* what, Fn fn) {
        Keep k(g_screen);
        bool ok = GUARDED(what, { fn(); });
        bool rk = GUARDED("p2menu restore player 1's focus", { k.Restore(); });
        bool rp = GUARDED("p2menu reposition", { Reposition(); });
        if (!ok || !rk || !rp) {
            LOG("p2menu: %s faulted (call %d, restore %d, move %d)", what, ok, rk, rp);
            Hide();
            if (++g_fails >= 3) { g_failed = true; LOG("p2menu: faulted 3x - player 2 keeps the old build list"); }
            return false;
        }
        return true;
    }

    bool Enabled(void* item) { return item && (F<uint32_t>(item, 0x14) & 5) == 5; }
    void* FirstChild(void* item) {
        void* c = item ? F(item, 0x24) : nullptr;
        return (c && c != (uint8_t*)item + 0x1C) ? c : nullptr;
    }
    void* CurrentDialog() {
        const int n = F<int>(g_tree, 0x24);
        void** arr = F<void**>(g_tree, 0x2C);
        if (!arr || n <= 0 || n > 16) return nullptr;
        for (int i = 0; i < n; ++i) if (arr[i] && F<int>(arr[i], 0xD0) == 1) return arr[i];
        return nullptr;
    }
    bool IsDisc(void* d) {
        auto isA = *reinterpret_cast<fn_isA*>(game::At(kIatIsA));
        return d && isA(d, nullptr, A.discClass);
    }
    const char* CmdOf(void* item, char* buf, int n) {
        buf[0] = 0;
        void* e = F(item, 0x30);                                     // TPCString: refcount, then the TCString
        if (!e) return buf;
        const char* s = nullptr;
        auto gs = *reinterpret_cast<fn_str*>(game::At(kIatGetString));
        GUARDED("p2menu item name", { s = gs((uint8_t*)e + 4, nullptr, 0); });
        if (s) { strncpy(buf, s, n - 1); buf[n - 1] = 0; }
        return buf;
    }

    // Show the disc whose top item is the toolbar menu `menu`; select `sel` (deeper item) or the first picture.
    bool Show(int who, int menu, void* sel) {
        if (!EnsureTree()) return false;
        void* ctl = gamectl::Ctl();
        void* toolbar = ctl ? F(ctl, 0x2C) : nullptr;
        if (!toolbar) { LOG("p2menu: no toolbar yet"); return false; }
        void* root = F(g_tree, 0x10); void* top = nullptr;
        GUARDED("p2menu find menu", {
            top = reinterpret_cast<fn_find>(game::At(kFindChild))(root, nullptr, (uint8_t*)toolbar + 0x2C + menu * 4, true); });
        if (!top) { LOG("p2menu: menu %d not found", menu); return false; }
        g_who = who; g_menu = menu; g_nPos = 0;
        const int x = *reinterpret_cast<int*>(game::At(kMenuPosX));
        const int y = F<short>(g_screen, 0x72) - *reinterpret_cast<int*>(game::At(kMenuPosB));
        bool shown = false;
        if (!Call("p2menu invoke", [&] { shown = A.invoke(g_tree, nullptr, top, x, y); })) return false;
        if (!shown) { LOG("p2menu: the game refused to open menu %d (no enabled item)", menu); return false; }
        g_open = true;
        void* pick = Enabled(sel) ? sel : FirstChild(top);
        if (Enabled(pick)) Call("p2menu first pick", [&] { A.setSel(g_tree, nullptr, pick); });
        LOG("p2menu: player %d opened their own %s disc (top %p, at %d,%d%s)", who + 1, menu == 0 ? "Build" : "game", top, x, y,
            sel ? ", back from a tool" : "");
        if (!g_hinted) {
            g_hinted = true;
            if (gamectl::ShowHintNow(L"Player 2: push the stick at a picture, A = pick, B = back", 5.0f)) splitscreen::HintToHalf(who, 5.0f);
        }
        return g_open;
    }

    // JPOG50 (owner 10:29): Demolish works for this player as long as the HUD player is NOT in demolish mode
    // (builder2 kind 5, a private one-shot ADemolishState). If they are: a message, one player at a time.
    void HintFor(int who, const wchar_t* t, float secs) {
        if (gamectl::ShowHintNow(t, secs) && who != splitscreen::ActivePlayer()) splitscreen::HintToHalf(who, secs);
    }

    int DlgOpen(int who, const char* cls);   // JPOG53, below: 0 = no, 1 = open, 2 = player 1 has it (message shown)
    int DlgShow(int who, void* d, const char* cls);

    // JPOG55 (3b): player 2's Actions-disc commands, without the game's own states/popups on player 1's chain.
    //   GotoParkEntrance: AUnitManager::GetUnitType 0x46C080 ("welccntr") + FindClosestUnitOfType 0x4A0D10 -> model centre
    //   (unit+0x30 -> +0x80 -> +0x14) -> ACamera::SetFocalPoint 0x50B260 on player 2's own camera.
    //   EmergencyOn/Off: 2-press confirm (3 s), then AParkManager::ToggleParkEmergencyMode 0x5D2420 (what the popup's OK does).
    //   BuildParkEntrance: builder2 placement of "welccntr" (as ABuildPlacementState("welccntr")).
    //   Dinopedia / GotoChopper / GotoSafari: whole-screen states -> message.
    DWORD g_emergAsk = 0;
    int Command(int who, const char* cmd) {   // 0 = not a command, 1 = handled (close the disc), 2 = handled (keep it open)
        if (!config::Get().p2Commands) return 0;
        if (!_stricmp(cmd, "GotoParkEntrance")) {
            void* cam = splitscreen::CameraOfPlayer(who);
            using fn_mgr  = void*(__cdecl*)();
            using fn_type = void*(__fastcall*)(void*, void*, const char*);
            using fn_near = void*(__fastcall*)(void*, void*, void*, const float*);
            using fn_pos  = const float*(__fastcall*)(void*, void*);
            using fn_foc  = void(__fastcall*)(void*, void*, const float*, bool);
            void* unit = nullptr; float c[3]{};
            bool ok = cam && GUARDED("p2 goto entrance", {
                void* mgr = reinterpret_cast<fn_mgr>(game::addr::AUnitManager_GetUnitManager)();
                void* type = mgr ? reinterpret_cast<fn_type>(0x0046C080)(mgr, nullptr, "welccntr") : nullptr;
                const float* from = reinterpret_cast<fn_pos>((*reinterpret_cast<void***>(cam))[52])(cam, nullptr);
                unit = (type && from) ? reinterpret_cast<fn_near>(0x004A0D10)(mgr, nullptr, type, from) : nullptr;
                if (unit) {
                    void* m = F(F(unit, 0x30), 0x80);
                    if (m) { c[0] = F<float>(m, 0x14); c[1] = F<float>(m, 0x18); c[2] = F<float>(m, 0x1C); }
                    else unit = nullptr;
                }
                if (unit) reinterpret_cast<fn_foc>(0x0050B260)(cam, nullptr, c, false);
            });
            LOG("p2menu: player %d Go to park entrance: ok=%d unit=%p pos=(%.0f,%.0f,%.0f)", who + 1, ok, unit, c[0], c[1], c[2]);
            if (!unit) HintFor(who, ok ? L"No park entrance yet" : L"Can't go to the park entrance now", 2.5f);
            return 1;
        }
        if (!_stricmp(cmd, "EmergencyOn") || !_stricmp(cmd, "EmergencyOff")) {
            void* pm = *reinterpret_cast<void**>(0x00850030);
            if (!pm) return 2;
            const DWORD now = GetTickCount();
            if (!g_emergAsk || now - g_emergAsk > 3000) {
                g_emergAsk = now;
                HintFor(who, !_stricmp(cmd, "EmergencyOn") ? L"Press A again to turn Emergency mode ON" : L"Press A again to turn Emergency mode OFF", 3.0f);
                return 2;
            }
            g_emergAsk = 0;
            using fn_v0 = void(__fastcall*)(void*, void*);
            using fn_b0 = bool(__fastcall*)(void*, void*);
            bool on = false;
            bool ok = GUARDED("p2 emergency", {
                reinterpret_cast<fn_v0>(0x005D2420)(pm, nullptr);
                on = reinterpret_cast<fn_b0>(0x005CEB30)(pm, nullptr);
            });
            LOG("p2menu: player %d toggled Emergency mode: ok=%d now %s", who + 1, ok, on ? "ON" : "OFF");
            HintFor(who, on ? L"Emergency mode is ON" : L"Emergency mode is OFF", 2.5f);
            return 1;
        }
        if (!_stricmp(cmd, "BuildParkEntrance")) {
            bool ok = builder2::OpenFor(who, "welccntr", 3);
            LOG("p2menu: player %d Build park entrance -> %d", who + 1, ok);
            if (ok) { g_toolOpen = true; return 1; }
            HintFor(who, L"Can't build a park entrance now", 2.5f);
            return 2;
        }
        if (!_stricmp(cmd, "Dinopedia") || !_stricmp(cmd, "GotoChopper") || !_stricmp(cmd, "GotoSafari")) {
            HintFor(who, L"This one fills the whole screen - player 1 has to start it", 3.0f);
            LOG("p2menu: player %d picked '%s' - whole-screen, player 1 only", who + 1, cmd);
            return 2;
        }
        return 0;
    }

    void Pick(int who) {
        void* sel = F(g_tree, 0x1C);
        if (!sel) return;
        if (F<int>(sel, 0x2C) >= 1 || !Enabled(sel)) {                // a submenu (or a greyed item: fail sound)
            Call("p2menu select", [&] { A.select(g_tree, nullptr); });
            return;
        }
        char cmd[48]; CmdOf(sel, cmd, sizeof cmd);
        const int type = F<int>(sel, 0x40);
        Call("p2menu select", [&] { A.select(g_tree, nullptr); });  // the game's "select" sound
        if (!_stricmp(cmd, "Demolish") && gamectl::DemolishActive()) {
            wchar_t t[120]; swprintf(t, 120, L"Player %d is using Demolish - only one player can demolish at a time", splitscreen::ActivePlayer() + 1);
            HintFor(who, t, 3.0f);
            LOG("p2menu: player %d wants Demolish - player %d is in demolish mode", who + 1, splitscreen::ActivePlayer() + 1);
            return;
        }
        if (int r = Command(who, cmd)) {
            if (r == 1) { g_lastSel = sel; Hide(); }
            return;
        }
        if (builder2::OpenFor(who, cmd, type)) {
            LOG("p2menu: player %d picked '%s' (type %d) - their own tool is open", who + 1, cmd, type);
            g_lastSel = sel; g_toolOpen = true;
            Hide();
        } else if (int r = (type == 1 ? DlgOpen(who, cmd) : 0)) {
            if (r == 1) { g_lastSel = sel; Hide(); }
        } else {
            LOG("p2menu: player %d picked '%s' (type %d) - not for player 2 yet", who + 1, cmd, type);
            // JPOG52: Manager / Dinosaurs / Actions windows and commands open full-screen states on the game's ONE state
            // chain (AParkManagerDialogState -> AModalDialogState -> AGUISystem::ActivateDialog) - player 2's own copy is
            // the next stage; until then a clear message instead of opening it on player 1's screen.
            if (gamectl::ShowHintNow(type == 1 ? L"Player 2 can't open this window" : L"Player 2 can't use this one yet", 2.5f))
                splitscreen::HintToHalf(who, 2.5f);
        }
    }

    void Point(float x, float y) {   // the dialog's own direction pick, like a pad stick on the console versions
        void* d = CurrentDialog();
        if (!d) return;
        struct Ev { int code; int type; uint8_t isFloat, hasMag, pad_[2]; void* dev; float m0, m1; } ev{ 0x10019, 0, 1, 1, {}, nullptr, x, y };
        static_assert(sizeof(Ev) == 0x18, "InputEvent layout");
        if (IsDisc(d)) Call("p2menu point", [&] { A.discAxis(d, nullptr, &ev, 0); });
    }

    // =====================================================================================================
    // JPOG53 (owner order 10:36 item 2 stage 2): player 2's OWN Manager / Dinosaurs / Actions WINDOWS, in player 2's
    // half, never on player 1's screen.
    // The game keeps ONE AGameDialog per class (AGUISystem::GetCachedGameDialog) and ONE widget per dialog (a top-level
    // GUI instance, no clone function), so "separately" = the window is player 2's while player 1 doesn't have it open;
    // the same class at the same time = the real conflict -> a message (same rule as Demolish, owner 10:29).
    // Opening: AGUISystem::ActivateDialog (the game's own placement + OnActivate) called WITHOUT the modal game state that
    // normally wraps it (no HUD hiding, no background picture, no context buttons, no focus = player 1 keeps all that),
    // then undone where it touches shared state: our dialog is unlinked again from the system's active-dialog list (so
    // player 1's dialogs push/pop as before) and PopDialog re-enables player 1's top dialog that PushDialog greyed.
    // Then the widget is moved by GuiOffsetTo into player 2's half (every frame, like the disc), OnUpdate(dt) every
    // 0.2 s like AGUISystem::OnUpdate does for listed dialogs. Closing: OnDeactivate + RemoveDialog.
    // A detour on ActivateDialog closes player 2's copy first when player 1 opens the same window.
    // =====================================================================================================
    constexpr uintptr_t kActivateDialog = 0x0067CDC0;  // AGUISystem::ActivateDialog(AGameDialog*, DialogLocation), ret 8
    constexpr uintptr_t kPopDialog      = 0x0067D420;  // AGUISystem::PopDialog()
    constexpr uintptr_t kRemoveDialog   = 0x0067E410;  // AGUISystem::RemoveDialog(TGUIWidget*), ret 4
    constexpr uintptr_t kGetCachedDlg   = 0x0067CBA0;  // AGUISystem::GetCachedGameDialog(const TClass*), ret 4
    constexpr uintptr_t kGameDialogCls  = 0x008B5240;  // AGameDialog::m_sClass
    constexpr uintptr_t kDetachUnit     = 0x0057B020;  // AUnitDialog::DetachUnit() (JPOG60)
    void* g_badUnits[8] = {};
    unsigned g_badNext = 0;
    using fn_activate = bool (__fastcall*)(void* sys, void* edx, void* dlg, int loc);
    using fn_cached   = void*(__fastcall*)(void* sys, void* edx, const void* cls);
    using fn_tfind    = const void*(__stdcall*)(const char* name, const void* base);
    using fn_bool     = bool (__fastcall*)(void* self, void* edx);
    using fn_upd      = bool (__fastcall*)(void* self, void* edx, float dt);

    fn_activate g_origActivate = nullptr;
    volatile LONG g_p2Call = 0;       // JPOG60 (fix C): >0 while WE activate / deactivate player 2's window
    bool  g_hookTried = false;
    void* g_dlg = nullptr;            // player 2's open window (an AGameDialog), or nullptr
    int   g_dlgWho = -1;
    char  g_dlgName[48] = {};
    float g_dlgTick = 0.0f;
    Pos   g_dlgPos{};                 // where we put the widget last (d = widget)
    int   g_dlgLogs = 0;
    int   g_dlgHintWho = -1;          // player whose window player 1 just took (message pending)
    padcursor::Cursor g_dlgCur;       // JPOG59 (open list 1): player 2's highlight inside their own window
    float g_dlgRepeat = 0.0f;

    void* GuiSystem() { return reinterpret_cast<void*(__stdcall*)()>(game::At(kGetGUISystem))(); }
    template <class T> T VFn(void* o, int off) { return reinterpret_cast<T>((*reinterpret_cast<void***>(o))[off / 4]); }

    void DlgClose(const char* why, bool hint = false) {
        if (!g_dlg) return;
        void* d = g_dlg; void* w = F(d, 0x2C);
        g_dlg = nullptr;
        padcursor::End(g_dlgCur);
        void* sys = nullptr;
        bool ok = GUARDED("p2win close", {
            sys = GuiSystem();
            void* sc = sys ? F(sys, 0x28) : nullptr;
            void* keep = sc ? A.focused(sc, nullptr) : nullptr;
            ++g_p2Call;
            VFn<fn_bool>(d, 0x1C)(d, nullptr);                                   // OnDeactivate
            --g_p2Call;
            if (sys && w && F(w, 0x44)) reinterpret_cast<fn_p>(game::At(kRemoveDialog))(sys, nullptr, w);
            if (sc && keep && keep != w && A.focused(sc, nullptr) != keep) A.requestFocus(keep, nullptr);
        });
        LOG("p2win: player %d's window %s closed (%s)%s", g_dlgWho + 1, g_dlgName, why, ok ? "" : " - FAULTED");
        if (hint) g_dlgHintWho = g_dlgWho;   // shown from the next update: never set a hint while the game inserts a state
    }

    // JPOG62 item C: an AMessagePopup takes the focus ~30 ms after player 2's hatchery window opens. Log every
    // ActivateDialog for 3 s after player 2 opened a window: dialog, vtable, and the SimJP return addresses on the stack
    // (resolve them offline with llvm-objdump to find who raises the popup).
    DWORD g_p2OpenT = 0; int g_popLogs = 0;
    void LogActivateCaller(void* dlg, int loc) {
        if (!config::Get().popupCallerLog || !g_p2OpenT || GetTickCount() - g_p2OpenT > 3000 || g_popLogs >= 12 || g_p2Call) return;
        ++g_popLogs;
        uintptr_t ra[8]; int n = 0;
        uintptr_t* sp = reinterpret_cast<uintptr_t*>(__builtin_frame_address(0));
        GUARDED("popup caller stack", {
            for (int i = 0; i < 256 && n < 8; ++i) {
                const uintptr_t v = sp[i];
                if (v >= 0x00401000 && v < 0x00739000) ra[n++] = v;
            }
        });
        void* vt = nullptr; GUARDED("popup vt", { vt = dlg ? *reinterpret_cast<void**>(dlg) : nullptr; });
        LOG("p2win: ActivateDialog %p (vtable %p, loc %d) %u ms after player 2's window - stack %08X %08X %08X %08X %08X %08X %08X %08X",
            dlg, vt, loc, (unsigned)(GetTickCount() - g_p2OpenT),
            n > 0 ? ra[0] : 0, n > 1 ? ra[1] : 0, n > 2 ? ra[2] : 0, n > 3 ? ra[3] : 0,
            n > 4 ? ra[4] : 0, n > 5 ? ra[5] : 0, n > 6 ? ra[6] : 0, n > 7 ? ra[7] : 0);
    }

    bool __fastcall Hook_ActivateDialog(void* sys, void* edx, void* dlg, int loc) {
        if (dlg && dlg == g_dlg) DlgClose("player 1 opened the same window", true);   // the real conflict: player 1 wins
        LogActivateCaller(dlg, loc);
        return g_origActivate(sys, edx, dlg, loc);
    }

    bool EnsureHook() {
        if (g_hookTried) return g_origActivate != nullptr;
        g_hookTried = true;
        void* tramp = nullptr;
        if (detour::Install(game::At(kActivateDialog), (void*)&Hook_ActivateDialog, &tramp)) g_origActivate = reinterpret_cast<fn_activate>(tramp);
        LOG("p2win: ActivateDialog hook %s", g_origActivate ? "on" : "FAILED - player 2's windows stay off");
        return g_origActivate != nullptr;
    }

    void DlgReposition() {
        if (!g_dlg) return;
        void* w = F(g_dlg, 0x2C);
        if (!w || !F(w, 0x44)) return;
        int dx = 0, dy = 0;
        if (!splitscreen::GuiOffsetTo(g_dlgWho, dx, dy)) return;
        short x = F<short>(w, 0x6C), y = F<short>(w, 0x6E);
        int bx = x, by = y;
        if (g_dlgPos.d == w && g_dlgPos.x == x && g_dlgPos.y == y) {
            if (g_dlgPos.ox == dx && g_dlgPos.oy == dy) return;
            bx = x - g_dlgPos.ox; by = y - g_dlgPos.oy;                        // halves swapped: undo the old offset
        }
        A.setPos(w, nullptr, bx + dx, by + dy);
        g_dlgPos = Pos{ w, F<short>(w, 0x6C), F<short>(w, 0x6E), dx, dy };
        if (g_dlgLogs < 6) { ++g_dlgLogs; LOG("p2win: window %s moved to player %d's half (%d,%d -> %d,%d)", g_dlgName, g_dlgWho + 1, bx, by, bx + dx, by + dy); }
    }

    int DlgOpen(int who, const char* cls) {
        if (!config::Get().p2Windows || !EnsureHook()) return 0;
        if (g_dlg) DlgClose("another window");
        void* sys = nullptr; void* d = nullptr; const void* tc = nullptr;
        static fn_tfind s_find = nullptr;
        if (!s_find) s_find = reinterpret_cast<fn_tfind>(GetProcAddress(GetModuleHandleA("TKernelInterface.dll"), "?Find@TClass@Toshi@@SGPBV12@PBDPBV12@@Z"));
        if (!s_find) { LOG("p2win: TClass::Find missing"); return 0; }
        if (!GUARDED("p2win find", {
                sys = GuiSystem();
                tc = s_find(cls, game::At(kGameDialogCls));
                if (sys && tc) d = reinterpret_cast<fn_cached>(game::At(kGetCachedDlg))(sys, nullptr, tc);
            }) || !d) {
            LOG("p2win: no window class '%s' (class %p, dialog %p)", cls, tc, d);
            return 0;
        }
        const int r = DlgShow(who, d, cls);
        return r < 0 ? 0 : r;
    }

    // JPOG57: show an already-found AGameDialog in player who's half (shared by DlgOpen and OpenUnitWindow).
    int DlgShow(int who, void* d, const char* cls) {
        void* sys = GuiSystem();
        if (!sys || !d) return 0;
        void* w = F(d, 0x2C);
        if (!w) { LOG("p2win: window %s has no widget", cls); return 0; }
        if (F(d, 0x04) || F(w, 0x44)) {                                         // already on the screen = player 1's
            HintFor(who, L"Player 1 has this window open - only one player at a time", 3.0f);
            LOG("p2win: player %d wants %s - player 1 has it open", who + 1, cls);
            return 2;
        }
        void* sc = F(sys, 0x28);
        Keep k(sc);
        const int cnt0 = F<int>(sys, 0xE8);
        bool shown = false;
        bool ok = GUARDED("p2win activate", {
            ++g_p2Call;
            shown = g_origActivate(sys, nullptr, d, F<int>(d, 0x54));
            --g_p2Call;
            if (F(d, 0x04) == (uint8_t*)sys + 0xDC) {                           // unlink it again (= DeactivateDialog's list part)
                F(d, 0x04) = nullptr;
                F(F(d, 0x0C), 0x08) = F(d, 0x08);
                F(F(d, 0x08), 0x0C) = F(d, 0x0C);
                F(d, 0x08) = d; F(d, 0x0C) = d;
                F<int>(sys, 0xE8) -= 1;
                reinterpret_cast<fn_v>(game::At(kPopDialog))(sys, nullptr); // re-enable player 1's top dialog
            }
        });
        GUARDED("p2win restore player 1's focus", { k.Restore(); });
        if (!ok || !shown) {
            g_p2Call = 0;                                                        // a fault skipped the decrement
            LOG("p2win: opening %s %s (shown %d, list %d -> %d)", cls, ok ? "refused" : "FAULTED", shown, cnt0, F<int>(sys, 0xE8));
            // JPOG60 (fix D): a fault may leave the dialog linked in the system's active list (JPOG59 crash: list 0 -> 1,
            // then the game updated the half-set-up dialog) - undo everything ActivateDialog did, guarded piece by piece.
            GUARDED("p2win undo list", {
                if (F(d, 0x04) == (uint8_t*)sys + 0xDC) {
                    F(d, 0x04) = nullptr;
                    F(F(d, 0x0C), 0x08) = F(d, 0x08);
                    F(F(d, 0x08), 0x0C) = F(d, 0x0C);
                    F(d, 0x08) = d; F(d, 0x0C) = d;
                    F<int>(sys, 0xE8) -= 1;
                    reinterpret_cast<fn_v>(game::At(kPopDialog))(sys, nullptr);
                    LOG("p2win: %s unlinked again from the active-dialog list (list now %d)", cls, F<int>(sys, 0xE8));
                }
            });
            if (F(w, 0x44)) GUARDED("p2win undo", { reinterpret_cast<fn_p>(game::At(kRemoveDialog))(sys, nullptr, w); });
            return ok ? 0 : -1;
        }
        g_dlg = d; g_dlgWho = who; g_dlgTick = 0.0f; g_dlgPos = Pos{}; g_dlgLogs = 0;
        strncpy(g_dlgName, cls, sizeof g_dlgName - 1);
        g_p2OpenT = GetTickCount(); g_popLogs = 0;   // JPOG62 item C
        GUARDED("p2win reposition", { DlgReposition(); });
        LOG("p2win: player %d opened their own window %s (dialog %p, widget %p, list %d -> %d)", who + 1, cls, d, w, cnt0, F<int>(sys, 0xE8));
        bool any = false;
        if (config::Get().p2WindowPad) { padcursor::Begin(g_dlgCur, w, "player 2's window"); g_dlgRepeat = 0.0f; any = g_dlgCur.cur != nullptr; }
        // JPOG60: a window with nothing to press (e.g. the Performance report: its tab code is a stub on PC and the tab
        // panel is hidden in main.gui) just says how to close it.
        HintFor(who, any ? L"Player 2: D-pad = pick a button, A = press it, B = close" : L"Player 2: B = close the window", 3.0f);
        return 1;
    }

    void DlgUpdate(float dt) {
        if (!g_dlg) return;
        g_dlgTick -= dt;
        if (g_dlgTick <= 0.0f) {
            g_dlgTick = 0.2f;
            void* d = g_dlg;
            if (!GUARDED("p2win update", { VFn<fn_upd>(d, 0x20)(d, nullptr, 0.2f); })) { DlgClose("update faulted"); return; }
        }
        GUARDED("p2win reposition", { DlgReposition(); });
    }

    // JPOG59 (open list 1 / 3a): player 2's pad INSIDE their own window. The highlight is padcursor's (the widget's own
    // focused look, no screen focus); A = the widget's own press/release. Player 1's focus, input stack and focus grab
    // are restored around the click (Keep), like every p2menu call.
    void WindowPad(int who, XPad& pad, float dt) {
        float x = pad.LX(), y = -pad.LY();
        if (pad.Down(PAD_DLEFT)) x = -1; if (pad.Down(PAD_DRIGHT)) x = 1;
        if (pad.Down(PAD_DUP)) y = -1;   if (pad.Down(PAD_DDOWN)) y = 1;
        const bool edge = pad.Pressed(PAD_DLEFT) || pad.Pressed(PAD_DRIGHT) || pad.Pressed(PAD_DUP) || pad.Pressed(PAD_DDOWN);
        if (fabsf(x) > 0.5f || fabsf(y) > 0.5f) {
            g_dlgRepeat -= dt;
            if (edge || g_dlgRepeat <= 0.0f) {
                g_dlgRepeat = edge ? 0.4f : 0.22f;
                const int dx = fabsf(x) >= fabsf(y) ? (x > 0 ? 1 : -1) : 0;
                const int dy = dx ? 0 : (y > 0 ? 1 : -1);
                GUARDED("p2win pad move", { padcursor::Move(g_dlgCur, dx, dy); });
            }
        } else g_dlgRepeat = 0.0f;
        if (!pad.Pressed(PAD_A)) { GUARDED("p2win refresh", { padcursor::Refresh(g_dlgCur); }); return; }
        void* sys = GuiSystem();
        void* sc = sys ? F(sys, 0x28) : nullptr;
        if (!sc) return;
        void* d = g_dlg; void* w = F(d, 0x2C);
        Keep k(sc);
        const int grabs = F<int>(sc, 0x100);
        bool took = false;
        GUARDED("p2win pad click", { took = padcursor::Click(g_dlgCur); });
        GUARDED("p2win restore player 1's focus", {
            if (F<int>(sc, 0x100) > grabs) LOG("p2win: focus grab left by the click (%d -> %d)", grabs, F<int>(sc, 0x100));
            k.Restore();
        });
        // The game may have closed the window itself (OK / Close buttons): then it is no longer ours to close.
        if (g_dlg == d && (!F(w, 0x44) || !(F<uint32_t>(w, 0x88) & 4))) {
            LOG("p2win: player %d's window %s closed by its own button", who + 1, g_dlgName);
            g_dlg = nullptr; g_dlgCur = padcursor::Cursor{};
            return;
        }
        if (!took) HintFor(who, L"That one can't be pressed", 1.5f);
    }
} // namespace

bool IsOpen(int who) { return g_open && g_who == who; }
bool InOwnWindowCall() { return g_p2Call > 0; }   // JPOG60 (fix C)
bool WindowOpen(int who) { return g_dlg && g_dlgWho == who; }   // JPOG59

// JPOG57 (owner 12:05: "RB + D-pad Up = the vanilla menu options for BOTH players"; 6c first step): the unit's own
// window (AUnit vfn +0x18 GetDialog: dinosaur -> AUnitDinosaurDialog, building -> its unit dialog / "GUIDialog"
// attribute) gets its unit like AUnitInfoState's ctor does (dialog vfn +0x44 with the proxy, refcount +4 raised by the
// caller), then is shown in player who's half with the JPOG53 window path (no state on the shared chain).
int OpenUnitWindow(int who, void* unit) {
    if (!config::Get().p2Windows || !unit || !EnsureHook()) return 0;
    if (!LoadApi()) return 0;
    if (g_open) Close(who, "unit window");
    if (g_dlg) DlgClose("another window");
    // JPOG60 (fix D): a unit whose window faulted once (JPOG59: a hatchery without its component -> RefreshDinoType on
    // null) is never tried again; the dialog is detached from it so the game never touches the half-set-up state.
    for (int i = 0; i < 8; ++i) if (g_badUnits[i] == unit) {
        HintFor(who, L"No info for that one", 2.0f);
        LOG("p2win: player %d - unit %p is marked unusable (its window faulted before)", who + 1, unit);
        return 0;
    }
    void* d = nullptr; void* px = *reinterpret_cast<void**>((uint8_t*)unit + 0x24);
    bool attached = false;
    bool ok = GUARDED("p2win unit dialog", {
        d = VFn<void*(__fastcall*)(void*, void*)>(unit, 0x18)(unit, nullptr);
        if (d && px) {
            // the same set-up AUnitInfoState does (ctor 0x568A10 / UpdateSelection 0x568E20): caller raises the proxy's
            // refcount, then AUnitDialog::AttachUnit (vfn +0x44) - nothing else touches the dialog there (OnInsertion =
            // HUD/context buttons/ActivateDialog/RequestFocus = shared state we skip on purpose).
            ++*reinterpret_cast<int*>((uint8_t*)px + 4);
            VFn<void(__fastcall*)(void*, void*, void*)>(d, 0x44)(d, nullptr, px);
            attached = true;
        }
    });
    auto markBad = [&](const char* why) {
        g_badUnits[g_badNext++ & 7] = unit;
        if (d) GUARDED("p2win detach", { reinterpret_cast<fn_v>(game::At(kDetachUnit))(d, nullptr); });
        LOG("p2win: player %d - unit %p window %s: dialog %p detached, unit marked unusable", who + 1, unit, why, d);
        HintFor(who, L"No info for that one", 2.0f);
    };
    if (!ok) { markBad("set-up FAULTED"); return 0; }
    if (!d) { LOG("p2win: player %d - unit %p has no window", who + 1, unit); HintFor(who, L"No info for that one", 2.0f); return 0; }
    const int r = DlgShow(who, d, "unit window");
    LOG("p2win: player %d unit window for %p (dialog %p, proxy %p, attached %d) -> %d", who + 1, unit, d, px, attached, r);
    if (r < 0) { markBad("activate FAULTED"); return 0; }
    return r;
}

void Close(int who, const char* why) {
    if (g_dlg && who == g_dlgWho) DlgClose(why);
    if (who != g_who || (!g_open && !g_toolOpen)) return;
    if (g_open) LOG("p2menu: player %d's disc closed (%s)", who + 1, why);
    Hide();
    g_toolOpen = false;
}
void CloseAll(const char* why) { if (g_dlg) DlgClose(why); if (g_who >= 0) Close(g_who, why); }


bool Update(int who, XPad& pad, void* cam, float dt) {
    if (!config::Get().p2DiscMenu || g_failed || !cam) return false;
    if (!gamectl::InPark()) { CloseAll("left the park"); return false; }
    if (!LoadApi()) return false;
    if (g_who != who && (g_open || g_toolOpen)) Close(g_who, "the other player has no HUD now");
    if (g_dlg && g_dlgWho != who) DlgClose("the other player has no HUD now");
    if (g_dlgHintWho == who) { g_dlgHintWho = -1; HintFor(who, L"Player 1 opened the same window - it moved to player 1's screen", 3.0f); }
    if (builder2::Open(who)) return false;                           // the build tool drives the pad
    if (g_dlg && g_dlgWho == who) {                                  // JPOG53: player 2's own window
        if (pad.Pressed(PAD_B) || pad.Pressed(PAD_X)) {
            DlgClose(pad.Pressed(PAD_B) ? "B" : "X");
            if (pad.Pressed(PAD_B) && g_lastSel) Show(who, g_menu, g_lastSel);   // B = back to the same disc, like the tools
            return true;
        }
        if (config::Get().p2WindowPad && g_dlgCur.root) WindowPad(who, pad, dt);
        if (g_dlg) DlgUpdate(dt);
        return true;
    }
    if (g_toolOpen) {                                                // the tool picked on the disc just closed
        g_toolOpen = false;
        const int r = builder2::TakeCloseReason(who);
        if (r == 1) { Show(who, g_menu, g_lastSel); return true; }   // B = back to the same submenu
        if (r == 2) return true;                                     // X = done (swallow this X)
    }
    // JPOG52 (owner order 10:36 item 2): this player's own Manager / Dinosaurs / Actions discs, the same buttons as
    // player 1's (Y Manager, LT Dinosaurs, RT Actions; triggers = press edge past half way).
    static bool s_lt[2]{}, s_rt[2]{};
    const bool ltDown = pad.LT() > 0.5f, rtDown = pad.RT() > 0.5f;
    const bool ltEdge = ltDown && !s_lt[who & 1], rtEdge = rtDown && !s_rt[who & 1];
    s_lt[who & 1] = ltDown; s_rt[who & 1] = rtDown;
    if (!g_open) {
        if (pad.Pressed(PAD_Y))  return Show(who, gamectl::MENU_MANAGER, nullptr);
        if (ltEdge)              return Show(who, gamectl::MENU_DINOSAURS, nullptr);
        if (rtEdge)              return Show(who, gamectl::MENU_ACTIONS, nullptr);
        if (!pad.Pressed(PAD_X)) return false;
        return Show(who, gamectl::MENU_BUILD, nullptr);              // false = fall back to builder2's list
    }
    if (pad.Pressed(PAD_X)) { Close(who, "X"); return true; }
    if (pad.Pressed(PAD_B)) {
        if (F(g_tree, 0x18) == F(g_tree, 0x14)) Close(who, "B at the top");
        else Call("p2menu back", [&] { A.back(g_tree, nullptr); });
        return true;
    }
    if (pad.Pressed(PAD_A)) { Pick(who); if (!g_open) return true; }
    if (pad.Pressed(PAD_RB)) Call("p2menu next", [&] { A.scroll(g_tree, nullptr, 1); });
    if (pad.Pressed(PAD_LB)) Call("p2menu prev", [&] { A.scroll(g_tree, nullptr, -1); });
    if (!g_open) return true;
    // Stick or D-pad: point at a picture (disc) / move up and down (list menus).
    float x = pad.LX(), y = pad.LY();
    if (pad.Down(PAD_DLEFT)) x = -1; if (pad.Down(PAD_DRIGHT)) x = 1;
    if (pad.Down(PAD_DUP)) y = 1;    if (pad.Down(PAD_DDOWN)) y = -1;
    const float mag = sqrtf(x * x + y * y);
    void* d = CurrentDialog();
    if (mag > 0.5f && d) {
        if (IsDisc(d)) Point(x / mag, config::Get().p2DiscStickY * y / mag);
        else if (fabsf(y) > 0.5f) {
            g_repeat -= dt;
            if (g_repeat <= 0.0f) { g_repeat = 0.25f; const int dir = y > 0 ? -1 : 1; Call("p2menu scroll", [&] { A.scroll(g_tree, nullptr, dir); }); }
        }
    } else g_repeat = 0.0f;
    GUARDED("p2menu reposition", { Reposition(); });                // the HUD plugin or a layout pass may move them
    return true;
}
} // namespace p2menu
