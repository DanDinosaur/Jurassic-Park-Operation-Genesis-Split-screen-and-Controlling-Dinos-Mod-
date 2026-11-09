// gamectl.cpp - see gamectl.h. Recreated for JPOG25 from the JPOG24 binary (JPOG24 source was lost).
#include <windows.h>
#include <cwchar>
#include <cstring>
#include "gamectl.h"
#include "game.h"
#include "guard.h"
#include "config.h"
#include "log.h"
#include "input_map.h"

namespace {
    constexpr uintptr_t kRootPtr            = 0x008B859C;  // AApplication(0x8B8580)+0x1C
    constexpr int       kRootCtlOff         = 0xD8;
    constexpr uintptr_t kGetCurrentState    = 0x0058EC80;
    constexpr uintptr_t kTransferControl    = 0x0058EC50;
    constexpr uintptr_t kDefaultStateVtbl   = 0x00D13EC0;
    constexpr uintptr_t kFollowStateVtbl    = 0x00D156A0;
    constexpr uintptr_t kFollowStateClass   = 0x008B5E20;
    constexpr uintptr_t kIatIsA             = 0x00739E20;
    constexpr uintptr_t kIatTmalloc         = 0x00739CA0;
    constexpr uintptr_t kIatGetLocale       = 0x00739CA8;
    constexpr uintptr_t kIatGetTemporary    = 0x00739C18;
    constexpr uintptr_t kUnitSelection      = 0x008500B0;
    constexpr uintptr_t kGetSelectedUnit    = 0x00526930;
    constexpr uintptr_t kParkInfo           = 0x00850048;
    constexpr uintptr_t kSetHintMessage     = 0x00672D90;
    constexpr uintptr_t kGameMenuStateCtor  = 0x00538630;
    constexpr uintptr_t kStartMenuStateCtor = 0x0055E2C0;

    using fn_this0   = void*(__fastcall*)(void* self, void* edx);
    using fn_thisP   = void (__fastcall*)(void* self, void* edx, void* p);
    using fn_isA     = bool (__fastcall*)(void* self, void* edx, const void* cls);
    using fn_tmalloc = void*(__stdcall*)(int, char*, int);
    using fn_locale  = void*(__stdcall*)();
    using fn_getTemp = void*(__fastcall*)(void* self, void* edx, void** sret, const wchar_t* text);
    using fn_setHint = void (__fastcall*)(void* hud, void* edx, void* tlstring, float secs);

    volatile LONG g_parkView = 0;          // written each frame on the game thread
    volatile LONG g_demolish = 0;          // JPOG37: demolish tool open (game thread)
    volatile LONG g_menuReq = -1;          // controller thread -> game thread
    volatile LONG g_placeRot = 0;          // JPOG46: a building/gate placement tool is open (game thread)
    volatile LONG g_rotReq = 0;            // JPOG46: rotate request -1 / +1 (controller thread -> game thread)
    volatile LONG g_discFocus = 0;         // JPOG35: a ring (disc) menu has the focus (game thread -> controller thread)

    const char* const kMenuNames[5] = { "Build", "Manager", "Dinosaurs", "Actions", "Start" };

    // Hint request (any thread -> game thread)
    CRITICAL_SECTION g_cs;
    bool     g_csInit = false;
    wchar_t  g_hintText[256];
    float    g_hintSecs = 0;
    volatile LONG g_hintPending = 0;
    unsigned g_hintLogs = 0;
    void InitCs() { if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; } }

    // Park-load hint (once per park, ~10 s after it loads)
    void*    g_hintPark = nullptr;
    unsigned g_hintFrames = 0;
    bool     g_hintShown = false;
    const wchar_t* const kParkHint =
        L"Pad: X Build  Y Manager  LT Dinosaurs  RT Actions  Start Menu  -  select a dino (A), then RB + D-pad Up to control it";

    bool InParkView(void* st) {
        if (!st) return false;
        return *reinterpret_cast<void**>(st) == game::At(kDefaultStateVtbl) || gamectl::IsA(st, kFollowStateClass);
    }

    // Open a HUD disc (Build/Manager/Dinosaurs/Actions = an AGameMenuState on that toolbar submenu)
    // or the Start menu (AStartMenuState) through the game's own state machine.
    bool OpenMenu(int menu, void*& state, void* toolbar, void* ctl) {
        return GUARDED("open park menu", {
            if (menu == gamectl::MENU_START) {
                state = gamectl::Alloc(0xCC);
                if (state) reinterpret_cast<fn_this0>(game::At(kStartMenuStateCtor))(state, nullptr);
            } else {
                state = gamectl::Alloc(0xB4);
                if (state) reinterpret_cast<fn_thisP>(game::At(kGameMenuStateCtor))(
                    state, nullptr, (uint8_t*)toolbar + 0x2C + menu * 4);
            }
            if (state) reinterpret_cast<fn_thisP>(game::At(kTransferControl))(ctl, nullptr, state);
        });
    }
}

namespace gamectl {

void* Ctl() {
    void* root = *reinterpret_cast<void**>(game::At(kRootPtr));
    return root ? *reinterpret_cast<void**>((uint8_t*)root + kRootCtlOff) : nullptr;
}

void* CurrentState() {
    void* root = *reinterpret_cast<void**>(game::At(kRootPtr));
    if (!root) return nullptr;
    void* ctl = *reinterpret_cast<void**>((uint8_t*)root + kRootCtlOff);
    if (!ctl || !*reinterpret_cast<void**>((uint8_t*)ctl + 0x24)) return nullptr;
    return reinterpret_cast<fn_this0>(game::At(kGetCurrentState))(ctl, nullptr);
}

bool IsDefaultState(void* st) { return st && *reinterpret_cast<void**>(st) == game::At(kDefaultStateVtbl); }
bool IsFollowState(void* st)  { return st && *reinterpret_cast<void**>(st) == game::At(kFollowStateVtbl); }
bool IsFollowFamily(void* st) { return st && IsA(st, kFollowStateClass); }

bool IsA(void* obj, uintptr_t tclassVa) {
    if (!obj) return false;
    auto isA = *reinterpret_cast<fn_isA*>(game::At(kIatIsA));
    return isA(obj, nullptr, game::At(tclassVa));
}

void* SelectedUnit() {
    void* sel = *reinterpret_cast<void**>(game::At(kUnitSelection));
    return sel ? reinterpret_cast<fn_this0>(game::At(kGetSelectedUnit))(sel, nullptr) : nullptr;
}

void* Alloc(int bytes) {
    auto tm = *reinterpret_cast<fn_tmalloc*>(game::At(kIatTmalloc));
    return tm(bytes, nullptr, -1);
}

bool InPark() { return *reinterpret_cast<void**>(game::At(kParkInfo)) != nullptr; }

bool ShowHintNow(const wchar_t* text, float seconds) {
    const Config& c = config::Get();
    if (!text || !c.hints) return false;
    if (!InPark()) return false;
    void* st = CurrentState();
    if (!st) return false;
    return GUARDED("hint message", {
        auto getLocale = *reinterpret_cast<fn_locale*>(game::At(kIatGetLocale));
        if (void* loc = getLocale()) {
            void* tl = nullptr;
            auto getTemp = *reinterpret_cast<fn_getTemp*>(game::At(kIatGetTemporary));
            getTemp(loc, nullptr, &tl, text);
            if (tl) reinterpret_cast<fn_setHint>(game::At(kSetHintMessage))((uint8_t*)st + 0x10, nullptr, tl, seconds);
        }
    });
}

bool ParkViewActive() { return g_parkView != 0; }
bool DemolishActive() { return g_demolish != 0; }
// JPOG49: open the game's own demolish tool for the HUD player (what the Build disc's Demolish item does:
// AGameMenuState::DoMenuCommand -> ADemolishState(0xF, true), 0xBC bytes, ctor 0x530700 thiscall ret 8). Park view only.
bool OpenDemolish() {
    void* ctl = Ctl(); void* st = CurrentState();
    if (!ctl || !InPark() || !IsDefaultState(st)) return false;
    void* ns = nullptr;
    bool ok = GUARDED("open demolish", {
        ns = Alloc(0xBC);
        if (ns) {
            reinterpret_cast<void(__fastcall*)(void*, void*, unsigned, bool)>(game::At(0x00530700))(ns, nullptr, 0xF, true);
            reinterpret_cast<fn_thisP>(game::At(kTransferControl))(ctl, nullptr, ns);
        }
    });
    LOG("gamectl: demolish tool opened for the HUD player (ok=%d state=%p)", ok, ns);
    return ok && ns;
}

void RequestMenu(int menu) { InterlockedExchange(&g_menuReq, menu); }

bool PlacementRotate() { return g_placeRot != 0; }
void RequestRotate(int dir) { InterlockedExchange(&g_rotReq, dir > 0 ? 1 : -1); }
void RequestHint(const wchar_t* text, float seconds) {
    InitCs();
    EnterCriticalSection(&g_cs);
    wcsncpy_s(g_hintText, 256, text, _TRUNCATE);
    g_hintSecs = seconds;
    LeaveCriticalSection(&g_cs);
    InterlockedExchange(&g_hintPending, 1);
}

// JPOG33: log every GUI focus change ("which screen / which widget has the keyboard focus") - this is the
// inventory of what the pad can reach: a screen whose log shows no focused widget can't be driven by keys.
static const char* WidgetName(void* w, char* buf, int n) {
    buf[0] = 0;
    if (!w) return buf;
    // (called inside the caller's GUARDED - guard.cpp has ONE jmp_buf per thread, so never nest GUARDED)
    uint8_t* pooled = *reinterpret_cast<uint8_t**>((uint8_t*)w + 0x68);          // TGUIWidget::GetName = +0x68 (TPCString)
    const char* str = pooled ? *reinterpret_cast<const char**>(pooled + 4) : nullptr;
    if (str) { int i = 0; for (; i < n - 1 && str[i] >= 0x20 && str[i] < 0x7F; ++i) buf[i] = str[i]; buf[i] = 0; }
    return buf;
}
// JPOG39: GUI focus facts for the console-style cursor fallback (written on the game thread, read by the pad thread).
static volatile LONG g_noFocus = 1;        // no widget has the keyboard focus (or only the screen itself)
static volatile LONG g_pointerWin = 0;     // the focused window is a screen made for the pointer (fossil hunt map)
static void LogGuiFocus() {
    static void* s_last = (void*)-1; static void* s_lastWin = (void*)-1;
    void* sys = reinterpret_cast<void*(__stdcall*)()>(game::At(0x0067E3F0))();      // AGUISystem::GetGUISystem
    void* sc = sys ? *reinterpret_cast<void**>((uint8_t*)sys + 0x28) : nullptr;       // TGUIScreen
    if (!sc) return;
    void* w   = *reinterpret_cast<void**>((uint8_t*)sc + 0xF4);                      // GetFocusedWidget
    void* win = *reinterpret_cast<void**>((uint8_t*)sc + 0xF0);                      // GetFocusedWindow
    InterlockedExchange(&g_noFocus, (!w || w == sc) ? 1 : 0);
    if (w == s_last && win == s_lastWin) return;
    s_last = w; s_lastWin = win;
    char a[48], b[48], p[48];
    void* parentWin = nullptr;                                                         // nearest named ancestor
    int depth = 0;
    for (void* q = w ? *reinterpret_cast<void**>((uint8_t*)w + 0x44) : nullptr; q && depth < 16;
         q = *reinterpret_cast<void**>((uint8_t*)q + 0x44), ++depth) {
        char t[48]; if (WidgetName(q, t, sizeof(t))[0]) { parentWin = q; break; }
    }
    WidgetName(win, a, sizeof(a));
    InterlockedExchange(&g_discFocus, strcmp(a, "CurrentDiscMenu") == 0 ? 1 : 0);
    // AFossilHuntDialog: the focus sits on an unnamed widget of the map panel - the dig sites are picked with the pointer.
    InterlockedExchange(&g_pointerWin, strcmp(a, "AFossilHuntDialog") == 0 ? 1 : 0);
    LOG("gui: focus -> window '%s' widget '%s' (in '%s') %s", a, WidgetName(w, b, sizeof(b)),
        WidgetName(parentWin, p, sizeof(p)), (!w || w == sc) ? "- NOTHING FOCUSED (keys can't drive this screen)" : "");
}

bool DiscMenuFocused() { return g_discFocus != 0; }
static volatile LONG g_worldAim = 0, g_cursorFallback = 0;
bool WorldAimActive() { return g_worldAim != 0; }
bool CursorFallback() { return g_cursorFallback != 0; }

void Tick() {
    InitCs();
    GUARDED("gui focus log", { LogGuiFocus(); });
    // 1) park view?
    LONG pv = 0;
    if (InPark()) pv = InParkView(CurrentState()) ? 1 : 0;
    InterlockedExchange(&g_parkView, pv);
    // JPOG39: a world "aim" state = the root state is an AScrollingState (TClass 0x8B5760: ADefaultState, AFollowState,
    // ADemolishState, ABuildPlacementState, APathLayingState, AWallLayingState, AGate/Flora/ArtifactPlacementState,
    // ASafariLayingState, ADinoMoveState, ATerraformPreset, ABalloonWaypointState, AScriptMoveState - all call
    // AScrollingState::OnUpdate 0x55CB40). There the console cursor is centre-locked (aim + ground circle).
    // Anywhere else (dialogs, front end, popups) with NO focused widget the keys reach nothing -> cursor fallback:
    // the pointer shows, the left stick moves it, A clicks (input_map / splitscreen read CursorFallback()).
    {
        LONG wa = 0;
        if (InPark()) if (void* st = CurrentState()) wa = IsA(st, 0x008B5760) ? 1 : 0;
        InterlockedExchange(&g_worldAim, wa);
        // debounced: ON only after 8 frames in a row (screen changes have a frame or two with no focus yet), OFF at once
        static int s_fbFrames = 0;
        // Never in states the pad DRIVES with the stick / keys (double check 22:30: driving the jeep has no focused
        // widget either - a pointer there would take the stick away from the steering): vehicle, turret, security
        // camera, mini games, building cam, tactical map, movies / demo / credits / loading / dissolve.
        static const uintptr_t kPadStates[] = { 0x008B5EE0 /*ADriveVehicleState*/, 0x008B5560 /*ATurretControlState*/,
            0x008B5720 /*ASecurityCameraState*/, 0x008B5B00 /*AMiniGameState*/, 0x008B5960 /*AMiniRgrAttackState*/,
            0x008B5920 /*AMiniSafariState*/, 0x008B5AC0 /*AMiniHuntingState*/, 0x008B5BE0 /*AMiniBalloonState*/,
            0x008B6100 /*ABuildingCamState*/, 0x008B5620 /*ATacMapState*/, 0x008B8300 /*AFrontEndDemoState*/,
            0x008B8240 /*AFrontEndMovieState*/, 0x008B58E0 /*AMoviePlayingState*/, 0x008B5FA0 /*ADemoState*/,
            0x008B6080 /*ACreditsState*/, 0x008B5DE0 /*AGameLoaderState*/, 0x008B48E0 /*ADissolveState*/ };
        bool padState = false;
        if (!wa) if (void* st = CurrentState()) for (uintptr_t k : kPadStates) if (IsA(st, k)) { padState = true; break; }
        const bool want = input_map::ConsoleActive() && !wa && !padState && !g_discFocus && (g_noFocus || g_pointerWin);
        s_fbFrames = want ? (s_fbFrames < 1000 ? s_fbFrames + 1 : s_fbFrames) : 0;
        const LONG fb = s_fbFrames >= 8 ? 1 : 0;
        if (InterlockedExchange(&g_cursorFallback, fb) != fb) {
            static int s_fbLogs = 0;
            if (s_fbLogs < 40) { ++s_fbLogs;
                LOG("controls: %s", fb ? "screen without a focused button - pad cursor ON (left stick = pointer, A = click)"
                                       : "pad cursor OFF (focused buttons / park view: stick = highlight / view, cursor centre-locked)"); }
        }
    }
    // JPOG37 (owner bug 5): demolish tool open? (ADemolishState::m_sClass 0x8B5FE0, the root state while it is active)
    {
        LONG dm = 0;
        if (InPark()) if (void* st = CurrentState()) dm = IsA(st, 0x008B5FE0) ? 1 : 0;
        if (InterlockedExchange(&g_demolish, dm) != dm)
            LOG("gamectl: demolish tool %s%s", dm ? "open - cursor centre-locked, stick pans the view" : "closed",
                dm && !input_map::ConsoleActive() ? " (PC-cursor style: centre lock only while demolishing)" : "");
    }

    // JPOG46 (backlog 5): building / gate placement tool open? (ABuildPlacementState::m_sClass 0x8B60C0,
    // AGatePlacementState 0x8B5D20). Its ProcessCommand 0x52B630 rotates on commands 0x1C (+1) / 0x1B (-1) - the
    // game maps only Up/Down to them, so the pad's D-pad Left/Right are sent here as the same commands.
    {
        LONG pr = 0; void* st = InPark() ? CurrentState() : nullptr;
        if (st) pr = (IsA(st, 0x008B60C0) || IsA(st, 0x008B5D20)) ? 1 : 0;
        if (InterlockedExchange(&g_placeRot, pr) != pr) LOG("gamectl: placement tool %s", pr ? "open - D-pad Left/Right rotate" : "closed");
        LONG rr = InterlockedExchange(&g_rotReq, 0);
        if (rr && pr && st) {
            int cmd = rr > 0 ? 0x1C : 0x1B;
            bool ok = GUARDED("rotate placement", {
                (*reinterpret_cast<bool(__fastcall***)(void*, void*, int, void*)>(st))[0x18 / 4](st, nullptr, cmd, nullptr); });
            LOG("gamectl: rotate placement %s (command 0x%X ok=%d)", rr > 0 ? "right" : "left", cmd, ok);
        }
    }

    // 2) HUD disc / Start menu request
    LONG req = InterlockedExchange(&g_menuReq, -1);
    if (req >= 0 && req <= 4 && InPark()) {
        int menu = (int)req;
        if (void* ctl = Ctl()) {
            void* st = CurrentState();
            if (!InParkView(st)) {
                LOG("menus: disc %d not opened - not in the park view (state vtbl %p)", menu, st ? *(void**)st : nullptr);
            } else {
                void* toolbar = *reinterpret_cast<void**>((uint8_t*)ctl + 0x2C);
                if (!toolbar && menu != MENU_START) LOG("menus: no toolbar yet");
                else {
                    void* ns = nullptr;
                    bool ok = OpenMenu(menu, ns, toolbar, ctl);
                    LOG("menus: opened %s disc (ok=%d state=%p)", kMenuNames[menu], ok, ns);
                }
            }
        }
    }

    // 3) the control-style hint, once per park (~10 s after it loads)
    void* park = *reinterpret_cast<void**>(game::At(kParkInfo));
    if (park != g_hintPark) { g_hintPark = park; g_hintFrames = 0; g_hintShown = false; }
    if (park && !g_hintShown && ++g_hintFrames == 600) {
        g_hintShown = true;
        RequestHint(kParkHint, 8.0f);
    }

    // 4) pending hint
    if (InterlockedExchange(&g_hintPending, 0)) {
        wchar_t text[256]; float secs;
        EnterCriticalSection(&g_cs);
        wcscpy_s(text, 256, g_hintText);
        secs = g_hintSecs;
        LeaveCriticalSection(&g_cs);
        bool ok = ShowHintNow(text, secs);
        if (g_hintLogs < 20) { ++g_hintLogs; LOG("hint: %ls (ok=%d)", text, ok); }
    }
}

} // namespace gamectl
