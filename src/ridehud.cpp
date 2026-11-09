// ridehud.cpp - JPOG62 item F. See ridehud.h.
// Engine (SimJP.exe): the park view states (ADefaultState / AFollowState) EMBED an AGUIGameHUD at state+0x10 whose
// mask is at state+0x18. AGUIGameHUD::SetHUD 0x673140 (thiscall, 1 arg, ret 4) stores the mask and calls
// AGUISystem::UpdateHUDComponents, which shows/hides every HUD element by its bit (SetHudVisibility, vfn +0x30).
// Bits: 0x100 AUnitStatsHudElement (needs tubes), 0x800 AUnitSelectionHudElement (highlight rings), 0x1000 AMinimap,
// 0x4000/0x8000 AToolbarHudElement. Vanilla park view mask = 0x59F4B (ADefaultState::OnInsertion 0x52FD51).
#include <windows.h>
#include <cstdint>
#include "ridehud.h"
#include "config.h"
#include "log.h"
#include "guard.h"
#include "game.h"
#include "gamectl.h"
#include "splitscreen.h"
#include "dinocontrol.h"

namespace ridehud {
namespace {
    constexpr uintptr_t kSetHUD = 0x00673140;
    using fn_setHud = void(__fastcall*)(void* hud, void* edx, uint32_t mask);
    void*    g_state = nullptr;   // state whose mask we changed
    uint32_t g_old   = 0;         // its mask before
    int      g_logs  = 0;

    uint32_t& MaskOf(void* st) { return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(st) + 0x18); }
    void Set(void* st, uint32_t m) {
        reinterpret_cast<fn_setHud>(game::At(kSetHUD))(reinterpret_cast<uint8_t*>(st) + 0x10, nullptr, m);
    }
    // Does the player who sees the game HUD ride a dino right now?
    bool HudPlayerRides() {
        if (splitscreen::Active()) return dinocontrol::Controls(splitscreen::ActivePlayer());
        return dinocontrol::Controls(0);
    }
    void Restore(const char* why) {
        if (!g_state) return;
        void* st = gamectl::CurrentState();
        if (st == g_state) {
            GUARDED("ride hud restore", { Set(st, g_old); });
            LOG("ridehud: HUD back to %05X (%s)", g_old, why);
        } else LOG("ridehud: record dropped, state %p no longer current (%s)", g_state, why);
        g_state = nullptr;
    }
}

void Tick() {
    const Config& c = config::Get();
    if (!c.rideHud) { if (g_state) Restore("ride_hud off"); return; }
    void* st = nullptr;
    GUARDED("ride hud state", { st = gamectl::CurrentState(); });
    const bool parkView = st && (gamectl::IsDefaultState(st) || gamectl::IsFollowState(st));
    const bool rides = HudPlayerRides();
    if (!rides) { if (g_state) Restore("let go"); return; }
    if (g_state && g_state != st) {
        // Another screen opened (menu) or the root state changed: forget the old record, never write a gone state.
        g_state = nullptr;
    }
    if (!parkView) return;
    const uint32_t want = (uint32_t)c.rideHudMask;
    uint32_t cur = 0;
    if (!GUARDED("ride hud read", { cur = MaskOf(st); })) return;
    if (cur == want) { if (!g_state) { g_state = st; g_old = 0x59F4B; } return; }
    if (!g_state) { g_state = st; g_old = cur; }
    bool ok = GUARDED("ride hud set", { Set(st, want); });
    if (g_logs < 8) { ++g_logs; LOG("ridehud: HUD player rides - HUD mask %05X -> %05X (ok=%d, state %p)", cur, want, ok, st); }
}

void OnUnload() {
    if (g_state) LOG("ridehud: park unload - record dropped");
    g_state = nullptr;
}
} // namespace ridehud
