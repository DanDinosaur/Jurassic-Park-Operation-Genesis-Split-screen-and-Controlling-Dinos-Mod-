// gamectl.h - small helpers that ASK THE GAME what is going on (no tracking of our own), plus
// game-thread requests: open a HUD disc, show a hint. Recreated for JPOG25 from the JPOG24 binary.
//
// Game pieces used (SimJP.exe, base 0x400000):
//   AApplication (0x8B8580) +0x1C -> root task (pointer at 0x8B859C); +0xD8 = ARootStateController
//   ARootStateController::GetCurrentGameState 0x58EC80 (valid when ctl+0x24 != 0)
//   ARootStateController::TransferControl     0x58EC50 (ctl, ARootState*)
//   ctl+0x2C = the toolbar (park HUD); toolbar+0x2C = TPCString[4] submenu names (Build/Manager/Dinosaurs/Actions)
//   AGameMenuState ctor 0x538630 (this, const TPCString&), size 0xB4   -> a HUD disc
//   AStartMenuState ctor 0x55E2C0 (this), size 0xCC                     -> Start menu
//   ADefaultState vtable 0xD13EC0, AFollowState vtable 0xD156A0, AFollowState::m_sClass 0x8B5E20
//   TObject::IsA (IAT 0x739E20), tmalloc (IAT 0x739CA0)
//   AParkManager::m_pUnitSelection 0x8500B0 -> AUnitSelection::GetSelectedUnit 0x526930
//   AParkManager::m_pParkInfo 0x850048 (non-null = in a park)
//   AGUIGameHUD::SetHintMessage 0x672D90 (state+0x10, TLString, float secs); TLString via
//   TSystem::GetLocale (IAT 0x739CA8) + TLocale::GetTemporary (IAT 0x739C18)
#pragma once
#include <cstdint>

namespace gamectl {
    enum Menu { MENU_BUILD = 0, MENU_MANAGER = 1, MENU_DINOSAURS = 2, MENU_ACTIONS = 3, MENU_START = 4 };

    void* Ctl();                     // ARootStateController*, or nullptr
    void* CurrentState();            // current ARootState*, or nullptr
    bool  IsDefaultState(void* st);  // the normal park view
    bool  IsFollowState(void* st);   // exactly AFollowState
    bool  IsFollowFamily(void* st);  // AFollowState or a subclass
    bool  IsA(void* obj, uintptr_t tclassVa);   // TObject::IsA with a design-time TClass address
    void* SelectedUnit();            // the game's own unit selection (aim + A)
    void* Alloc(int bytes);          // the game's tmalloc
    bool  InPark();
    bool  DemolishActive();
    bool  OpenDemolish();            // JPOG49: the game's own demolish tool for the HUD player (park view only; game thread)          // JPOG37: the demolish tool is open (cursor centre-locked, stick pans)
    bool  ParkViewActive();          // last game-thread check: park view (default or follow), no other screen
    // Controller thread -> game thread (applied in Tick):
    void  RequestMenu(int menu);
    // JPOG35: a ring (disc) menu has the GUI focus (window 'CurrentDiscMenu': Start, Manager, Actions...).
    bool  DiscMenuFocused();
    // JPOG39: root state is a world aim state (AScrollingState family: park view, follow, demolish, placement tools).
    bool  WorldAimActive();
    // JPOG39: console style on a screen where no widget has the focus (or the fossil-hunt map) = pad drives a pointer.
    bool  CursorFallback();
    void  RequestHint(const wchar_t* text, float seconds);
    // JPOG79: a hint for ONE player - shown in that player's half in split-screen (p < 0 = the HUD half, as RequestHint).
    void  RequestHintFor(int p, const wchar_t* text, float seconds);
    // JPOG79: safe wide formatter (llvm-mingw ucrt swprintf printed garbage for %ls in JPOG78). Supports %d %ls %hs %%.
    void  WFmt(wchar_t* out, size_t n, const wchar_t* fmt, ...);
    // JPOG46 (backlog 5): a building / gate placement tool is open; D-pad Left/Right rotate it (game's own commands).
    bool  PlacementRotate();
    void  RequestRotate(int dir);    // -1 / +1, applied on the game thread
    bool  ShowHintNow(const wchar_t* text, float seconds);   // game thread only
    void  Tick();                    // game thread, every frame
}
