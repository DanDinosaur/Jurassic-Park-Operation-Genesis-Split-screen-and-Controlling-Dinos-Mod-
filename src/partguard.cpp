// partguard.cpp - see partguard.h
#include <windows.h>
#include <cstdint>
#include "partguard.h"
#include "config.h"
#include "game.h"
#include "guard.h"
#include "log.h"

namespace partguard {
namespace {
    using fn_upd = void(__fastcall*)(void* self, void* edx);
    fn_upd g_orig = nullptr;
    volatile LONG g_faults = 0;
    void __fastcall Hook(void* self, void* edx) {
        alignas(16) uint8_t fpu[108];
        __asm__ __volatile__("fnsave %0\n\tfrstor %0" : "+m"(fpu));   // keep a copy of the x87 state, unchanged
        if (GUARDED("particle update (0x6C9170)", { g_orig(self, edx); })) return;
        __asm__ __volatile__("frstor %0" : : "m"(fpu));                // a fault left values on the x87 stack
        const LONG n = InterlockedIncrement(&g_faults);
        if (n <= 5 || (n % 500) == 0) LOG("partguard: particle emitter %p faulted (%ld times) - that frame skipped instead of a crash (twister debris off the map?)", self, n);
    }
}
// JPOG73 rev d (log 15:03 "vtable slot is 00000000"): the vtable is filled by the game's own start-up code AFTER the mod
// installs - so Install() is called every frame (mod.cpp start-up + splitscreen::Tick) until the slot is there.
void Install() {
    static int s_state = 0;   // 0 waiting, 1 done, -1 off
    if (s_state) return;
    if (!config::Get().particleGuard) { s_state = -1; return; }
    // JPOG74 (log 15:17 rev d: no partguard line, CRASH SimJP+0x2CA6A1 again): 0xD545C0 IS the slot (slot 4 of the vtable
    // at 0xD545B0; the file image holds 0x006C9170 there) - rev c/d added 4*4 a second time and read 0xD545D0 (always 0).
    void** slot = reinterpret_cast<void**>(game::At(0x00D545C0));
    static DWORD s_t0 = 0;
    if (!s_t0) s_t0 = GetTickCount() | 1;
    if (!*slot) {                                           // not filled yet - try again next frame
        if ((LONG)(GetTickCount() - s_t0) > 60000) { s_state = -1; LOG("partguard: slot 0xD545C0 still 0 after 60 s - OFF (twister can crash)"); }
        return;
    }
    if (*slot != game::At(0x006C9170)) { s_state = -1; LOG("partguard: vtable slot is %p, not 0x6C9170 - off", *slot); return; }
    DWORD old = 0;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) { s_state = -1; return; }
    g_orig = reinterpret_cast<fn_upd>(*slot);
    *slot = reinterpret_cast<void*>(&Hook);
    VirtualProtect(slot, 4, old, &old);
    s_state = 1;
    LOG("partguard: particle emitter update 0x6C9170 guarded (crash fix SimJP+0x2CA6A1)");
}
}
