#include <windows.h>
#include "hooks.h"
#include "input_map.h"
#include "game.h"
#include "detour.h"
#include "log.h"
#include "playermode.h"
#include "config.h"
#include "unlocks.h"
#include "padrumble.h"
#include "splitscreen.h"
#include "gamectl.h"
#include "dinocontrol.h"
#include "ridehud.h"
#include "padcursor.h"
#include "minipad.h"
#include "widescreen.h"
#include "extras.h"
#include <cstdint>

// Detour ARenderer::OnUpdate(float dt) - the per-frame render task (0x477370).
// JPOG1 targeted ARootTask::OnUpdate (0x47D3A0) but that is a base-class stub
// (mov eax,1 / ret 4) - not a frame tick and not detourable.
// Same signature: virtual bool __thiscall (float), ret 4. It's __thiscall (this in ECX). MSVC can't
// declare __thiscall for a free function, so we take the hook as __fastcall and
// ignore the EDX slot - ECX maps to the first __fastcall param.

namespace {
    game::fn_ARootTask_OnUpdate g_origOnUpdate = nullptr;

    volatile unsigned g_frames = 0;

    int __fastcall Hook_OnUpdate(void* self, void* edx, float dt) {
        int r = g_origOnUpdate ? g_origOnUpdate(self, edx, dt) : 0;
        unsigned f = ++g_frames;
        input_map::GameFrame();          // JPOG33: pad keys are only sent once the game renders
        if (f == 1 || f == 600 || f == 6000) LOG("hooks: frame tick #%u (dt=%.4f self=%p)", f, dt, self);
        if (f == 60) { widescreen::Install(); extras::Install(); }
        if (f > 60) extras::Tick(f);         // JPOG67: window (Alt+F4, borderless), cursor speed  // JPOG66: well after the *.asi plugins (proxy loads them on the first DirectInput8Create)
        // Keep the vanilla shoot-from-vehicle cheat bit set (game thread, so no race).
        if (config::Get().vehicleShoot) {
            volatile uint8_t* cheats = reinterpret_cast<volatile uint8_t*>(
                game::At(game::addr::AMiniGameController_m_uiCheats));
            if (!(*cheats & 1)) { *cheats |= 1; LOG("hooks: vehicle shoot mode forced ON"); }
        }
        unlocks::Tick();
        padrumble::Tick();
        splitscreen::Tick(self, dt);
        gamectl::Tick();
        dinocontrol::Tick(dt);
        ridehud::Tick();                 // JPOG62: rider's HUD = stats + minimap only
        padcursor::Tick(dt);             // JPOG59: info bar pad cursor (HUD player)
        minipad::Tick(dt);               // JPOG59: sticks -> helicopter / jeeps / mini games
        // Our per-frame logic runs AFTER the game has updated its world.
        PlayerMode::Tick(self, dt);
        return r;
    }
}

namespace hooks {

bool Install() {
    void* target = game::At(game::addr::ARenderer_OnUpdate);
    void* tramp = nullptr;
    if (!detour::Install(target, (void*)&Hook_OnUpdate, &tramp)) {
        LOG("hooks: FAILED to hook ARenderer::OnUpdate @ %p", target);
        return false;
    }
    g_origOnUpdate = reinterpret_cast<game::fn_ARootTask_OnUpdate>(tramp);
    LOG("hooks: ARenderer::OnUpdate hooked (frame tick live)");
    return true;
}

void Remove() {
    detour::RemoveAll();
    g_origOnUpdate = nullptr;
}

} // namespace hooks
