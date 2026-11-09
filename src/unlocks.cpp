// unlocks.cpp - owner's always-on progression unlocks, applied from the frame hook
// (game thread). Every call goes through the game's OWN functions (the same ones its
// cheat menu uses), so saves/UI stay consistent. All addresses verified against
// SimJP.exe bytes + the decompile (see game.h addr:: block "unlocks").
#include <windows.h>
#include <cstdint>
#include "unlocks.h"
#include "game.h"
#include "config.h"
#include "log.h"

namespace {
    using fn_this0 = void(__fastcall*)(void* self, void* edx);
    using fn_thisI = void(__fastcall*)(void* self, void* edx, int);
    using fn_thisF = void(__fastcall*)(void* self, void* edx, float);

    template <class T> T Fn(uintptr_t a) { return reinterpret_cast<T>(game::At(a)); }
    void* Static(uintptr_t a) { return *reinterpret_cast<void**>(game::At(a)); }
    int   I32(void* base, int off) { return *reinterpret_cast<int*>((uint8_t*)base + off); }
    void* Ptr(void* base, int off) { return *reinterpret_cast<void**>((uint8_t*)base + off); }

    unsigned g_frame = 0;
    bool     g_missionsOpened = false;
    void*    g_park = nullptr;        // AParkInfo* of the park we're tuned to
    unsigned g_parkFrame0 = 0;        // frame the park appeared

    void OpenMissionsOnce() {
        if (g_missionsOpened) return;
        void* mc = Static(game::addr::AMissionController_s_pInstance);
        if (!mc || I32(mc, 0x68) <= 0) return;                 // missions not loaded yet
        Fn<fn_this0>(game::addr::AMissionController_OpenAllMissions)(mc, nullptr);
        g_missionsOpened = true;
        LOG("unlocks: all missions + Site B opened (%d missions)", I32(mc, 0x68));
    }

    using fn_thisP = void(__fastcall*)(void* self, void* edx, void*);

    // Unlock every dig-site ground AND register it with its ground set the way a player
    // pick does (AFossilHuntGroundSet::UserChosen), so the "Unlocked Sites x/y" counter and
    // the new-game site-choice flow complete normally. JPOG8 only called UnlockGround:
    // grounds showed ticked but the counter stayed 0/5 and the Site B setup stalled.
    void UnlockFossils(void* fh) {
        Fn<fn_this0>(game::addr::AFossilHuntManager_ForceStatusOpen)(fh, nullptr);
        // Ground sets: sentinel fh+0x30, first fh+0x38, next node+8.
        // Set: +0x3C status (1 choosing, 2 open, 3 done), +0x44 count, +0x4C ground array,
        //      +0x50 num chosen, +0x54 max. Ground: +0x75 unlocked flag.
        uint8_t* sentinel = (uint8_t*)fh + 0x30;
        void* set = Ptr(fh, 0x38);
        int unlocked = 0, chosen = 0, reenabled = 0, guard = 0;
        while (set && set != sentinel && guard++ < 64) {
            int count = I32(set, 0x44);
            void** arr = reinterpret_cast<void**>(Ptr(set, 0x4C));
            // Pass 1: unlock + (re)enable every ground BEFORE registering picks -
            // UserChosen disables (+0x74=0) any still-locked ground once a set's quota
            // is met, which in JPOG9 put red X's on 5 sites.
            int newly = 0;
            for (int i = 0; arr && i < count && i < 64; ++i) {
                void* g = arr[i];
                if (!g) continue;
                if (!*((uint8_t*)g + 0x75)) {
                    Fn<fn_this0>(game::addr::AFossilHuntGround_UnlockGround)(g, nullptr);
                    ++unlocked; ++newly;
                }
                if (!*((uint8_t*)g + 0x74)) { *((uint8_t*)g + 0x74) = 1; ++reenabled; }
            }
            // Pass 2: register one pick per newly unlocked ground (drives the x/y counter).
            for (int i = 0; arr && i < newly; ++i) {
                Fn<fn_thisP>(game::addr::AFossilHuntGroundSet_UserChosen)(set, nullptr, arr[0]);
                ++chosen;
            }
            // Grounds unlocked earlier (or by JPOG8) but never counted: top the set up.
            int spin = 0;
            while (arr && count > 0 && I32(set, 0x50) < I32(set, 0x54) && spin++ < 16) {
                Fn<fn_thisP>(game::addr::AFossilHuntGroundSet_UserChosen)(set, nullptr, arr[0]);
                ++chosen;
            }
            // UserChosen may have disabled grounds again: final re-enable pass.
            for (int i = 0; arr && i < count && i < 64; ++i)
                if (arr[i] && !*((uint8_t*)arr[i] + 0x74)) { *((uint8_t*)arr[i] + 0x74) = 1; ++reenabled; }
            set = Ptr(set, 8);
        }
        if (unlocked || chosen || reenabled) LOG("unlocks: dig sites - %d unlocked, %d picks registered, %d re-enabled", unlocked, chosen, reenabled);
    }
}

namespace unlocks {

void Tick() {
    const Config& c = config::Get();
    ++g_frame;
    if (g_frame % 30) return;                                  // ~2x per second is plenty

    if (c.unlockSiteB) OpenMissionsOnce();

    void* pi = Static(game::addr::AParkManager_m_pParkInfo);
    void* pl = Static(game::addr::AParkManager_m_pPlayerInfo);
    void* rm = Static(game::addr::AParkManager_m_pResearchManager);
    void* fh = Static(game::addr::AParkManager_m_pFossilHuntManager);
    if (!pi || !pl || !rm || !fh) { g_park = nullptr; return; } // not in a park

    if (pi != g_park) {                                        // new park loaded
        g_park = pi; g_parkFrame0 = g_frame;
        LOG("unlocks: park detected (ParkInfo=%p) - applying in ~3 s", pi);
        return;
    }
    if (g_frame - g_parkFrame0 < 180) return;                  // let the load settle
    const bool slow = (g_frame % 300) == 0;                    // ~every 5 s
    const bool first = (g_frame - g_parkFrame0) < 210;

    if (c.infiniteMoney) {
        int money = I32(pl, 8);
        if (money < c.moneyFloor) {
            Fn<fn_thisI>(game::addr::APlayerInfo_CheatGiveMoney)(pl, nullptr, c.moneyTopUp - money);
            LOG("unlocks: money %d -> %d", money, c.moneyTopUp);
        }
    }
    if (c.maxStars && (first || slow) && I32(pi, 0x144) < 10) {
        Fn<fn_thisF>(game::addr::AParkInfo_ForceSetEsat)(pi, nullptr, 0.96f);
        LOG("unlocks: stars forced to max (10-rating was %d)", I32(pi, 0x144));
    }
    if (c.allResearch && (first || slow))
        Fn<fn_this0>(game::addr::AResearchManager_CheatGiveAllResearch)(rm, nullptr);
    if (c.allDigSites && (first || slow))
        UnlockFossils(fh);
}

} // namespace unlocks
