#include <windows.h>
#include "playermode.h"
#include "features.h"
#include "config.h"
#include "log.h"

// The state machine. Feature bodies live in features.cpp and are scaffolds today:
// they log what they WOULD do and name the [CONFIRM] offset they need, so the
// build runs safely before each feature is implemented. Flip them on milestone by
// milestone (P1..P11) as the offsets in game.h::confirm are filled in.

namespace {
    Mode  g_mode = Mode::MANAGER;
    bool  g_prevToggle = false, g_prevSwap = false, g_prevView = false, g_prevSpawn = false;

    bool KeyEdge(int vk, bool& prev) {
        bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool edge = now && !prev;
        prev = now;
        return edge;
    }
}

namespace PlayerMode {

void Init() {
    features::Init();
    LOG("playermode: init (mode=MANAGER)");
}

Mode Current() { return g_mode; }

void Toggle() {
    if (g_mode == Mode::MANAGER) {
        if (features::avatar::Spawn()) { g_mode = Mode::ON_FOOT; LOG("playermode: -> ON_FOOT"); }
    } else {
        features::avatar::Despawn();
        g_mode = Mode::MANAGER; LOG("playermode: -> MANAGER");
    }
}

void Tick(void* rootTask, float dt) {
    const Config& c = config::Get();
    if (!c.enabled) return;

    // Global hotkeys (keyboard; controller Back is handled in the controller path).
    if (KeyEdge(c.keyTogglePlayer, g_prevToggle)) Toggle();

    switch (g_mode) {
        case Mode::MANAGER:
            // stock game; nothing to do here yet.
            break;

        case Mode::ON_FOOT:
        case Mode::AIMING:
            features::avatar::Tick(dt);
            features::camera::Tick(dt, g_mode);
            features::shooter::Tick(dt, g_mode);
            if (KeyEdge(c.keySwapWeapon, g_prevSwap)) features::shooter::SwapWeapon();
            if (KeyEdge(c.keyCycleView,  g_prevView)) features::camera::CycleView();
            if (KeyEdge(c.keySpawnMenu,  g_prevSpawn)) features::vehicle::OpenSpawnMenu();
            break;

        case Mode::ENTERING:
        case Mode::IN_VEHICLE:
        case Mode::EXITING:
            features::vehicle::Tick(dt);
            features::camera::Tick(dt, g_mode);
            break;
    }

    features::splitscreen::Tick(dt);  // no-op until co-op is enabled
}

void Shutdown() { features::Shutdown(); }

} // namespace PlayerMode
