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
    if (c.vehiclePad && gamectl::InPark() && input_map::ConsoleActive()) {
        GUARDED("minipad state", {
            void* s = gamectl::CurrentState();
            if (s) for (uintptr_t k : kStates) if (gamectl::IsA(s, k)) { st = s; break; }
        });
    }
    const LONG on = st ? 1 : 0;
    if (InterlockedExchange(&g_active, on) != on)
        LOG("minipad: %s", on ? "vehicle / mini game - sticks go to the game's own stick input (console layout), RT fire, LT right button"
                              : "vehicle / mini game ended");
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
            took = (*reinterpret_cast<bool(__fastcall***)(void*, void*, void*, int)>(st))[0x14 / 4](st, nullptr, &ev, 0); });
        if (!ok) { if (++g_faults >= 3) LOG("minipad: stick input faulted 3x - off for this session"); continue; }
        if (moved && g_logs < 8) { ++g_logs; LOG("minipad: %s stick %.2f,%.2f -> state %p took=%d", i ? "right" : "left", v[i][0], v[i][1], st, took); }
    }
}
} // namespace minipad
