// cullfix.cpp - JPOG37 (owner bugs 4 + 3): "20-30 dinosaurs lag and teleport forward", "jeep wheels left behind".
//
// Decompile / disassembly (SimJP.exe):
//   ARenderer::Render 0x478E90 walks every AVisibleUnit (TClass 0x8B76E0) once per scene pass, tests its bounding
//   sphere against the 6 frustum planes of the CURRENT viewport and calls the unit's virtual SetRender(inside)
//   (vftable slot +0x1B8 = AVisibleUnit::SetRender 0x4DA280 -> AAnimatableObject::SetRender ->
//   AToshiAnimationSystem::SetVisible: model+0x40 bit 0x40000). The call site is 0x47909D (ff 90 b8 01 00 00).
//   The simulation reads that bit after rendering: AActiveUnit::Imp_UpdateControl updates a unit whose bit is clear
//   only once a second with dt = 1 s and ACreatureSteer::ProcessTick moves it in a straight line ("lag + teleport");
//   AToshiAnimationSystem::UpdateAttachments hides / freezes attachments of such units.
//   In split-screen the scene is drawn twice per frame - the LAST pass decides, so a unit seen only in the first half
//   stays "off-screen" for the simulation although a player is looking at it.
// Fix (split-screen only): the call site goes through Hook_CullSetRender, which records every unit that was inside
// ANY pass of this frame; after the last pass those units get SetRender(true) again. Each pass still culls for its
// own drawing (the engine re-tests every unit in every pass), only the simulation sees "visible if either half sees
// it". Single screen: nothing changes (the hook only forwards).
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "cullfix.h"
#include "config.h"
#include "game.h"
#include "guard.h"
#include "log.h"

namespace {
    constexpr uintptr_t kCallSite = 0x0047909D;     // ARenderer::Render: call dword ptr [eax+0x1B8]
    constexpr int kVfnSetRender = 0x1B8 / 4;
    using fn_setrender = void(__fastcall*)(void* unit, void* edx, int vis);

    bool g_installed = false;
    volatile LONG g_rec = 0;
    void* g_seen[1024]; unsigned g_nSeen = 0, g_overflow = 0;
    unsigned g_frames = 0, g_restoredTotal = 0, g_calls = 0, g_seenTotal = 0, g_logs = 0;
    DWORD g_t0 = 0;
    void* g_rcNow = nullptr;

    bool ModelVisible(void* unit) {
        void* ao = *reinterpret_cast<void**>((uint8_t*)unit + 0x30);
        void* mi = ao ? *reinterpret_cast<void**>((uint8_t*)ao + 0x80) : nullptr;
        return mi && (*reinterpret_cast<uint32_t*>((uint8_t*)mi + 0x40) & 0x40000) != 0;
    }

    // JPOG54 (owner 11:19: "the dome disappears on player 2's screen when player 1 isn't looking at it"; 10b security
    // building in player 1's half before JPOG52). Something reuses ONE half's visibility for the other half's drawing
    // (before JPOG52 the first pass culled with player 2's stale planes -> player 1 lost them; since JPOG52 the first
    // pass is player 1's -> player 2 loses them). Fix that does not depend on WHERE the reuse happens: in split-screen
    // every pass marks a unit visible if it is inside THIS half's frustum (the engine's own test) OR inside the OTHER
    // half's frustum (its planes captured in its own pass, this frame or the frame before). Drawing a unit that is off
    // this half's screen costs a little GPU time and is clipped by the viewport; nothing else changes.
    constexpr uintptr_t kIatGetWorldPlanes = 0x00739F50;   // TRenderContext::GetWorldPlanes(TPlane*, int) (thiscall, ret 8)
    constexpr int kVfnRadius = 0xB8 / 4;                   // unit vfn(9) -> bounding radius (as ARenderer::Render 0x479026)
    using fn_planes = void(__fastcall*)(void* rc, void* edx, float* out, int n);
    using fn_radius = float(__fastcall*)(void* unit, void* edx, int which);
    float g_planes[2][6][4]; bool g_havePlanes[2] = { false, false };
    int g_half = -1; bool g_needPlanes = false;
    unsigned g_promoted = 0, g_promotedTotal = 0, g_planeFails = 0;

    void* ModelOf(void* unit) {
        void* ao = *reinterpret_cast<void**>((uint8_t*)unit + 0x30);
        return ao ? *reinterpret_cast<void**>((uint8_t*)ao + 0x80) : nullptr;
    }
    void CapturePlanes() {
        g_needPlanes = false;
        if (g_half < 0 || g_half > 1) return;
        auto get = *reinterpret_cast<fn_planes*>(game::At(kIatGetWorldPlanes));
        void* rc = g_rcNow;
        if (!get || !rc) { ++g_planeFails; return; }
        float tmp[6][4];
        get(rc, nullptr, &tmp[0][0], 6);
        for (int i = 0; i < 6; ++i) for (int k = 0; k < 4; ++k) g_planes[g_half][i][k] = -tmp[i][k];   // the engine negates them too
        g_havePlanes[g_half] = true;
    }
    // ARenderer::Render's own test (0x479060 loop): inside if dot(n, c) - w >= -r for all 6 (negated) planes.
    bool InsideOther(void* unit) {
        const int o = 1 - g_half;
        if (g_half < 0 || !g_havePlanes[o]) return false;
        void* mi = ModelOf(unit);
        if (!mi || (*reinterpret_cast<uint32_t*>((uint8_t*)mi + 0x40) & 0x200) == 0) return false;   // engine: not cullable -> hidden
        const float* c = reinterpret_cast<const float*>((uint8_t*)mi + 0x14);
        float r = reinterpret_cast<fn_radius>((*reinterpret_cast<void***>(unit))[kVfnRadius])(unit, nullptr, 9);
        for (int i = 0; i < 6; ++i) {
            const float* p = g_planes[o][i];
            if (p[0] * c[0] + p[1] * c[1] + p[2] * c[2] - p[3] < -r) return false;
        }
        return true;
    }

    // JPOG60 (item 12, security building pops in player 1's half): log every pass's decision for units whose type name
    // is in [coop] cull_log_types (comma list, "" = off): engine result, other-half test, radius, flags. Only changes
    // are logged (per unit + half), max 80 lines per park, so the owner's test shows whether that building is culled
    // HERE (this loop) or somewhere else (it never shows up = another draw path).
    constexpr uintptr_t kTypeGetName = 0x0046CC40;          // AUnitFactoryUnitType::GetName() (thiscall) -> const char*
    struct Watch { void* unit; uint8_t last[2]; bool match; };
    Watch g_watch[64]; unsigned g_nWatch = 0, g_watchLogs = 0;
    const char* TypeName(void* unit) {
        void* t = *reinterpret_cast<void**>((uint8_t*)unit + 0x2C);
        if (!t) return "";
        const char* n = reinterpret_cast<const char*(__fastcall*)(void*, void*)>(game::At(kTypeGetName))(t, nullptr);
        return n ? n : "";
    }
    bool NameListed(const char* n) {
        const char* list = config::Get().cullLogTypes;
        if (!list[0] || !n[0]) return false;
        const size_t ln = strlen(n);
        for (const char* p = list; *p; ) {
            const char* e = strchr(p, ',');
            const size_t l = e ? (size_t)(e - p) : strlen(p);
            if (l == ln && _strnicmp(p, n, l) == 0) return true;
            if (!e) break;
            p = e + 1;
        }
        return false;
    }
    void WatchLog(void* unit, int engineVis, bool promoted, int finalVis) {
        if (g_half < 0 || g_half > 1 || g_watchLogs >= 80) return;
        Watch* w = nullptr;
        for (unsigned i = 0; i < g_nWatch; ++i) if (g_watch[i].unit == unit) { w = &g_watch[i]; break; }
        if (!w) {
            if (g_nWatch >= 64) return;
            w = &g_watch[g_nWatch++];
            w->unit = unit; w->last[0] = w->last[1] = 0xFF; w->match = false;
            GUARDED("cullfix type name", { w->match = NameListed(TypeName(unit)); });
        }
        if (!w->match) return;
        const uint8_t code = (uint8_t)((engineVis ? 1 : 0) | (promoted ? 2 : 0) | (finalVis ? 4 : 0));
        if (w->last[g_half] == code) return;
        w->last[g_half] = code;
        ++g_watchLogs;
        void* mi = ModelOf(unit);
        const float* c = mi ? reinterpret_cast<const float*>((uint8_t*)mi + 0x14) : nullptr;
        float r = 0;
        GUARDED("cullfix radius", { r = reinterpret_cast<fn_radius>((*reinterpret_cast<void***>(unit))[kVfnRadius])(unit, nullptr, 9); });
        LOG("cullfix watch: %s %p pass %s half: engine %s%s -> %s (centre %.1f %.1f %.1f, radius %.1f, model flags %08X)",
            TypeName(unit), unit, g_half == 0 ? "P1" : "P2", engineVis ? "IN" : "OUT", promoted ? " (other half sees it)" : "",
            finalVis ? "drawn" : "culled", c ? c[0] : 0.f, c ? c[1] : 0.f, c ? c[2] : 0.f, r,
            mi ? *reinterpret_cast<uint32_t*>((uint8_t*)mi + 0x40) : 0u);
    }
    // Replaces "call [eax+0x1B8]" (thiscall: ecx = unit, one stack arg) - fastcall pops the same 4 bytes.
    void __fastcall Hook_CullSetRender(void* unit, void* edx, int vis) {
        const int engineVis = vis & 0xFF;
        bool promoted = false;
        if (g_rec && config::Get().splitCullBoth) {
            if (g_needPlanes) CapturePlanes();
            if (!(vis & 0xFF) && InsideOther(unit)) { vis = (vis & ~0xFF) | 1; ++g_promoted; promoted = true; }
        }
        if (g_rec && config::Get().cullLogTypes[0]) WatchLog(unit, engineVis, promoted, vis & 0xFF);
        reinterpret_cast<fn_setrender>((*reinterpret_cast<void***>(unit))[kVfnSetRender])(unit, edx, vis);
        if (g_rec) {
            ++g_calls;
            if (vis & 0xFF) {
                for (unsigned i = 0; i < g_nSeen; ++i) if (g_seen[i] == unit) return;
                if (g_nSeen < sizeof g_seen / sizeof g_seen[0]) g_seen[g_nSeen++] = unit; else ++g_overflow;
            }
        }
    }
}

namespace cullfix {

void Install() {
    uint8_t* p = reinterpret_cast<uint8_t*>(game::At(kCallSite));
    static const uint8_t expect[6] = { 0xFF, 0x90, 0xB8, 0x01, 0x00, 0x00 };
    if (!config::Get().splitCullFix) { LOG("cullfix: off ([coop] split_cull_fix = 0)"); return; }
    if (memcmp(p, expect, 6) != 0) {
        LOG("cullfix: call site %p has unexpected bytes %02X %02X %02X - not installed", p, p[0], p[1], p[2]);
        return;
    }
    DWORD old;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) return;
    int32_t rel = (int32_t)((uintptr_t)&Hook_CullSetRender - ((uintptr_t)p + 5));
    p[0] = 0xE8; memcpy(p + 1, &rel, 4); p[5] = 0x90;
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    g_installed = true;
    LOG("cullfix: ARenderer::Render frustum SetRender call (0x47909D) hooked - split-screen: a unit seen by EITHER half stays full-rate");
}

void SetPass(int half, void* rc) {
    g_half = half; g_rcNow = rc; g_needPlanes = true;
}

void BeginFrame() {
    if (!g_installed) return;
    g_nSeen = 0;
    InterlockedExchange(&g_rec, 1);
}

void EndFrame() {
    if (!g_installed || !g_rec) return;
    InterlockedExchange(&g_rec, 0);
    g_half = -1; g_promotedTotal += g_promoted; g_promoted = 0;
    unsigned restored = 0;
    GUARDED("cullfix restore", {
        for (unsigned i = 0; i < g_nSeen; ++i) {
            void* u = g_seen[i];
            if (!ModelVisible(u)) {
                reinterpret_cast<fn_setrender>((*reinterpret_cast<void***>(u))[kVfnSetRender])(u, nullptr, 1);
                ++restored;
            }
        }
    });
    ++g_frames; g_restoredTotal += restored; g_seenTotal += g_nSeen;
    DWORD now = GetTickCount();
    if (!g_t0) g_t0 = now;
    if (now - g_t0 >= 10000 || g_logs == 0) {
        if (g_logs < 200)
            LOG("cullfix: %u split frames - avg %.1f units on screen in either half, %.1f per frame kept full-rate that the last pass had culled (%u SetRender calls), %.1f per frame drawn in both halves (split_cull_both, plane fails %u)%s",
                g_frames, g_frames ? (float)g_seenTotal / g_frames : 0.f, g_frames ? (float)g_restoredTotal / g_frames : 0.f, g_calls,
                g_frames ? (float)g_promotedTotal / g_frames : 0.f, g_planeFails,
                g_overflow ? " - LIST FULL" : "");
        ++g_logs;
        g_t0 = now; g_frames = 0; g_restoredTotal = 0; g_seenTotal = 0; g_calls = 0; g_overflow = 0; g_promotedTotal = 0;
    }
}

} // namespace cullfix
