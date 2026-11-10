// minipad.cpp - see minipad.h (JPOG59).
#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cstring>
#include "minipad.h"
#include "config.h"
#include "game.h"
#include "gamectl.h"
#include "input_map.h"
#include "splitscreen.h"
#include "xinput_pad.h"
#include "guard.h"
#include "log.h"

namespace minipad {
namespace {
    volatile LONG g_active = 0;
    XPad g_pad0(0), g_pad1(1);
    bool g_sent[2] = { false, false };
    int  g_logs = 0, g_faults = 0;
    struct Ev { int code; int type; uint8_t isFloat, hasMag, pad_[2]; void* dev; float m0, m1; };
    static_assert(sizeof(Ev) == 0x18, "InputEvent layout");
    // vehicle + mini game states (the same list gamectl keeps out of the pointer fallback)
    const uintptr_t kStates[] = { 0x008B5EE0 /*ADriveVehicleState*/, 0x008B5B00 /*AMiniGameState*/, 0x008B5960 /*AMiniRgrAttackState*/,
                                  0x008B5920 /*AMiniSafariState*/, 0x008B5AC0 /*AMiniHuntingState*/, 0x008B5BE0 /*AMiniBalloonState*/ };
}
bool Active() { return g_active != 0; }

// JPOG59 (open list 10 / B23): the helicopter gun makes no sound. AShootingUnitComponent::GetFireRoundSound 0x4F2640
// (stdcall, weapon) only knows weapons 1/2 (0x2E3) and 5 (0x2E6); every other weapon gets -1 = no sound. The function is
// a pure table (14 instructions), so it is replaced whole by a JMP to ours: same answers, plus - only while a player
// drives a vehicle / mini game (minipad active) - [vehicle] gun_sound_id for the unknown weapons. NPC shooters stay vanilla.
namespace {
    int g_gunLogs = 0;
    int __stdcall Hook_FireRoundSound(int weapon) {
        if (weapon == 5) return 0x2E6;
        if (weapon == 1 || weapon == 2) return 0x2E3;
        const Config& c = config::Get();
        const bool give = g_active && c.gunSound;
        if (g_gunLogs < 4) { ++g_gunLogs; LOG("minipad: gun fired with weapon %d - %s", weapon, give ? "sound added" : "no sound (vanilla)"); }
        return give ? c.gunSoundId : -1;
    }
    bool g_gunTried = false;
    void InstallGunSound() {
        if (g_gunTried) return;
        g_gunTried = true;
        if (!config::Get().gunSound) return;
        uint8_t* t = reinterpret_cast<uint8_t*>(game::At(0x004F2640));
        static const uint8_t kExpect[9] = { 0x8B, 0x44, 0x24, 0x04, 0xBA, 0xFF, 0xFF, 0xFF, 0xFF };
        if (memcmp(t, kExpect, sizeof kExpect) != 0) { LOG("minipad: GetFireRoundSound bytes differ - gun sound fix off"); return; }
        DWORD old = 0;
        if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old)) return;
        t[0] = 0xE9;
        *reinterpret_cast<int32_t*>(t + 1) = (int32_t)((uintptr_t)&Hook_FireRoundSound - ((uintptr_t)t + 5));
        VirtualProtect(t, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), t, 5);
        LOG("minipad: GetFireRoundSound replaced (helicopter / vehicle gun sound 0x%X while a player drives)", config::Get().gunSoundId);
    }
}

void Tick(float) {
    const Config& c = config::Get();
    InstallGunSound();
    void* st = nullptr;
    uintptr_t matchK = 0;   // JPOG76: which kStates class matched (safari jeep = 008B5920 AMiniSafariState, checked by the jeep code at 0x4D8609)
    if (c.vehiclePad && gamectl::InPark() && input_map::ConsoleActive()) {
        GUARDED("minipad state", {
            void* s = gamectl::CurrentState();
            if (s) for (uintptr_t k : kStates) if (gamectl::IsA(s, k)) { st = s; matchK = k; break; }
        });
    }
    // JPOG73 (owner #6): log every game state change while in the park (vtable + whether it is a vehicle state), so the
    // helicopter's flying state can be added to kStates if it is missing.
    {
        static void* s_lastVt = nullptr; static int s_stLogs = 0;
        void* cur = nullptr; void* vt = nullptr;
        if (gamectl::InPark()) GUARDED("minipad state log", { cur = gamectl::CurrentState(); vt = cur ? *(void**)cur : nullptr; });
        if (vt != s_lastVt) {
            s_lastVt = vt;
            // JPOG77: name the states met so far that are NOT vehicles (owner JPOG76: 00D21E00 = the bare ARootState left after
            // the park was unloaded - must never go into kStates; ASafariLayingState 0x8B57E0 = laying a safari track).
            const char* note = "";
            if (vt == game::At(0x00D21E00)) note = " = bare ARootState: NO park state (park unloaded)";
            else if (cur) GUARDED("minipad state name", {
                if (gamectl::IsA(cur, 0x008B57E0)) note = " = ASafariLayingState (laying a safari track)";
                else if (gamectl::IsA(cur, 0x008B5920)) note = " = AMiniSafariState (safari ride)"; });
            if (vt && (s_stLogs < 60 || st || *note)) { ++s_stLogs;   /* JPOG76: vehicle states always logged */ LOG("minipad: game state now vtbl %p (vehicle / mini game state: %d class %08lX, pad console style %d)%s", vt, st != nullptr, (unsigned long)matchK, (int)input_map::ConsoleActive(), note); }
        }
    }
    const LONG on = st ? 1 : 0;
    if (InterlockedExchange(&g_active, on) != on)
        LOG("minipad: %s", on ? "vehicle / mini game - sticks go to the game's own stick input (console layout), RT fire, LT right button"
                              : "vehicle / mini game ended");
    // JPOG73 (log 13:32:10 helicopter: 3 faults in ppropertyparser.dll+0x79D0 within 10 ms -> stick off for the WHOLE
    // session = no flying after that): the fault count now belongs to ONE vehicle / mini game state; a new state (next
    // flight / drive) tries again. Faulting state: sticks paused for 1.5 s, then retried (3 pauses -> off for that state).
    static void* s_faultSt = nullptr; static DWORD s_pauseUntil = 0; static int s_pauses = 0;
    if (st != s_faultSt) { if (g_faults) LOG("minipad: new vehicle / mini game state %p - stick input retried (was off after faults)", st); s_faultSt = st; g_faults = 0; s_pauses = 0; s_pauseUntil = 0; }
    if (st && g_faults >= 3) {
        if (s_pauses < 3 && !s_pauseUntil) { s_pauseUntil = GetTickCount() + 1500; ++s_pauses; LOG("minipad: stick paused 1.5 s after faults (pause %d / 3)", s_pauses); }
        if (s_pauseUntil && (LONG)(GetTickCount() - s_pauseUntil) >= 0) { s_pauseUntil = 0; g_faults = 0; }
    }
    if (!st || g_faults >= 3) { g_sent[0] = g_sent[1] = false; return; }
    const int pl = splitscreen::CanSwitch() ? splitscreen::ActivePlayer() : 0;
    XPad& pad = pl ? g_pad1 : g_pad0;
    pad.Poll(c.deadzone, c.triggerThreshold);
    if (!pad.Connected()) return;
    const float ys = c.vehicleStickY;
    const float v[2][2] = { { pad.LX(), ys * pad.LY() }, { pad.RX(), ys * pad.RY() } };
    for (int i = 0; i < 2; ++i) {
        const bool moved = fabsf(v[i][0]) > 0.01f || fabsf(v[i][1]) > 0.01f;
        if (!moved && !g_sent[i]) continue;                 // send the zero once after a release
        g_sent[i] = moved;
        Ev ev{ i ? 0x1001A : 0x10019, 3, 1, 1, {}, nullptr, v[i][0], v[i][1] };
        bool took = false;
        bool ok = GUARDED("minipad stick", {
            // JPOG73 rev d: the 2nd argument is the game's AInputMap* (ProcessInput(const InputEvent*, AInputMap*), e.g.
            // AMiniRgrAttackState 0x54E270 -> ARootState::IsInput). We passed 0 -> ppropertyparser.dll+0x79D0 read 0x84 = the
            // helicopter faults (logs 13:32 / 15:08). Now AInputMap::GetInputMap() 0x468E00 = [0x84D9C0], like the game.
            void* im = *reinterpret_cast<void**>(game::At(0x0084D9C0));
            took = (*reinterpret_cast<bool(__fastcall***)(void*, void*, void*, void*)>(st))[0x14 / 4](st, nullptr, &ev, im); });
        if (!ok) { if (++g_faults >= 3) { LOG("minipad: stick input faulted 3x in state %p (vtbl %p)", st, *(void**)st); break; } continue; }
        if (moved && g_logs < 8) { ++g_logs; LOG("minipad: %s stick %.2f,%.2f -> state %p took=%d", i ? "right" : "left", v[i][0], v[i][1], st, took); }
    }
}
} // namespace minipad
