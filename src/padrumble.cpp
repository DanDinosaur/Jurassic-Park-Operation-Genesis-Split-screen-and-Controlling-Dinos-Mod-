#include <windows.h>
#include <xinput.h>
#include <cstdint>
#include <cmath>
#include "padrumble.h"
#include "splitscreen.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include "xinput_pad.h"

namespace {
    // ACamera::AddShake(const AShakeData&, bool) - thiscall. AShakeData (0x1C bytes, from AddShake
    // 0x50A9E0): +0 position (TVector3), +0xC radius, +0x10 amplitude, +0x14 ?, +0x18 duration (s).
    using fn_addShake = void(__fastcall*)(void* cam, void* edx, const float* data, bool b);
    using fn_pos      = const float*(__fastcall*)(void* cam, void* edx);
    constexpr uintptr_t kWorldCamVtbl = 0x00D12C20;
    constexpr int kSlotAddShake = 37, kSlotGetPosition = 52;
    fn_addShake g_origAddShake = nullptr;

    struct Motor { float level = 0; DWORD start = 0, end = 0; bool on = false; };
    Motor g_motor[2];
    unsigned g_logged = 0;

    float ShakeAt(void* cam, const float* d) {
        auto getPos = reinterpret_cast<fn_pos>((*reinterpret_cast<void***>(cam))[kSlotGetPosition]);
        const float* p = getPos(cam, nullptr);
        float dx = d[0] - p[0], dy = d[1] - p[1], dz = d[2] - p[2];
        float d2 = dx*dx + dy*dy + dz*dz, r2 = d[3] * d[3];
        if (r2 <= 0 || d2 >= r2) return 0.0f;
        return (1.0f - d2 / r2) * d[4];
    }
    void Kick(int pad, float strength, float seconds) {
        const Config& c = config::Get();
        if (!c.vibration || pad < 0 || pad > 1) return;
        float lvl = strength * c.rumbleStrength;
        if (lvl > 1.0f) lvl = 1.0f;
        if (lvl < 0.05f) return;
        if (seconds < 0.12f) seconds = 0.12f;
        if (seconds > 1.5f) seconds = 1.5f;
        Motor& m = g_motor[pad];
        DWORD now = GetTickCount();
        if (!m.on || lvl >= m.level) { m.level = lvl; m.start = now; m.end = now + (DWORD)(seconds * 1000.0f); }
        m.on = true;
    }
    void __fastcall Hook_AddShake(void* cam, void* edx, const float* data, bool b) {
        g_origAddShake(cam, edx, data, b);
        if (!data) return;
        int who = splitscreen::PlayerOfCamera(cam);
        float s = ShakeAt(cam, data);
        if (g_logged < 6) { ++g_logged; LOG("rumble: shake r=%.1f amp=%.2f dur=%.2f -> player %d strength %.2f", data[3], data[4], data[6], who + 1, s); }
        Kick(who, s, data[6]);
        // Split-screen: the other player's camera feels it too (from its own position).
        if (void* other = splitscreen::CameraOfPlayer(1 - who)) {
            if (other != cam) {
                g_origAddShake(other, edx, data, b);
                Kick(1 - who, ShakeAt(other, data), data[6]);
            }
        }
    }
}

namespace padrumble {

bool Install() {
    void** e = reinterpret_cast<void**>(game::At(kWorldCamVtbl)) + kSlotAddShake;
    if (reinterpret_cast<uintptr_t>(*e) != reinterpret_cast<uintptr_t>(game::At(0x0050A9E0))) {
        LOG("rumble: AWorldCamera::AddShake slot not as expected (%p) - rumble off", *e);
        return false;
    }
    DWORD old;
    VirtualProtect(e, sizeof(void*), PAGE_READWRITE, &old);
    g_origAddShake = reinterpret_cast<fn_addShake>(*e);
    *e = reinterpret_cast<void*>(&Hook_AddShake);
    VirtualProtect(e, sizeof(void*), old, &old);
    LOG("rumble: hooked camera shakes (strength %.2f)", config::Get().rumbleStrength);
    return true;
}

void Pulse(int player, float strength, float seconds) { Kick(player, strength, seconds); }

void Tick() {
    DWORD now = GetTickCount();
    for (int p = 0; p < 2; ++p) {
        Motor& m = g_motor[p];
        if (!m.on) continue;
        XINPUT_VIBRATION v{};
        if ((int)(now - m.end) >= 0) { m.on = false; m.level = 0; }
        else {
            float t = (float)(now - m.start) / (float)(m.end - m.start);   // linear fade-out
            float lvl = m.level * (1.0f - t);
            v.wLeftMotorSpeed  = (WORD)(lvl * 65535.0f);          // big motor: the thump
            v.wRightMotorSpeed = (WORD)(lvl * 0.6f * 65535.0f);   // small motor: the buzz
        }
        int slot = padslots::SlotOf(p);           // players follow connected pads, not fixed slots
        if (slot >= 0) XInputSetState(slot, &v);
    }
}

} // namespace padrumble
