#include <windows.h>
#include "features.h"
#include "game.h"
#include "config.h"
#include "log.h"

// Scaffold bodies. Each logs once what it will do and the exact decompiled read it
// needs, then returns safely. This keeps every build runnable while features are
// filled in one at a time. Replace a body's TODO with the real implementation on
// its milestone; the addresses are already in game.h::addr.

namespace {
    template <class Tag> void Once(const char* msg) {
        static bool done = false;
        if (!done) { done = true; LOG("%s", msg); }
    }
    struct TAvatarSpawn{}; struct TAvatarTick{}; struct TCamTick{}; struct TCamCycle{};
    struct TShootTick{};  struct TShootSwap{}; struct TVehMenu{};  struct TVehTick{}; struct TSplit{};

    std::string g_view = "ots";
    std::string g_weapon = "tranq";
    bool g_splitOn = false, g_splitVertical = false;
}

namespace features {

void Init() {
    const Config& c = config::Get();
    g_view   = c.viewMode;
    g_weapon = c.weapon;
    g_splitVertical = (c.splitOrientation == "vertical");
    LOG("features: init (view=%s weapon=%s)", g_view.c_str(), g_weapon.c_str());
}
void Shutdown() {}

namespace avatar {
    bool Spawn() {
        Once<TAvatarSpawn>("[P1] avatar::Spawn - TODO: create AUnitStaff(ranger) at camera focus "
                           "via AUnitStaff::CreateObject (0x4D4C90); place with CmdGoTo (0x4D4C30). "
                           "[CONFIRM] unit world-pos offset.");
        return true; // allow mode toggle during bring-up
    }
    void Despawn() { Once<struct TDespawn>("[P1] avatar::Despawn - TODO: remove/hide the avatar unit."); }
    void Tick(float) {
        Once<TAvatarTick>("[P2] avatar::Tick - TODO: write pos/heading from input; drive walk/idle anim. "
                          "[CONFIRM] unit pos/heading offset (read AUnitStaff::CmdGoTo 0x4D4C30).");
    }
}

namespace camera {
    void Tick(float, Mode) {
        Once<TCamTick>("[P2] camera::Tick - TODO: 3rd-person follow via AFollowCamera / ACamera "
                       "(OnUpdate 0x50A660, SetFollowDistance 0x479EC0). FP uses GetBoneTransform 0x50E970. "
                       "[CONFIRM] active-camera install (ACameraManager::OnUpdate 0x50CF00).");
    }
    void CycleView() {
        g_view = (g_view=="ots") ? "scope" : (g_view=="scope") ? "firstperson" : "ots";
        LOG("camera: view -> %s", g_view.c_str());
    }
}

namespace shooter {
    void Tick(float, Mode) {
        Once<TShootTick>("[P4] shooter::Tick - TODO: aim ray -> dino under reticle; on fire reuse "
                         "AMiniGameState::ShootWeapon (0x5467D0) / apply AUnitDinosaur::HurtUnit (0x4CB000). "
                         "[CONFIRM] hit-apply call + UnitHurtType (sedate vs lethal).");
    }
    void SwapWeapon() {
        g_weapon = (g_weapon=="tranq") ? "lethal" : "tranq";
        LOG("shooter: weapon -> %s", g_weapon.c_str());
    }
}

namespace vehicle {
    void OpenSpawnMenu() {
        Once<TVehMenu>("[P5] vehicle::OpenSpawnMenu - TODO: TGUIWidget menu -> spawn jeep (car) or "
                       "rgrcopt (helicopter) via AUnitFactory; copter uses GetHelipadPosition (0x4C5AF0).");
    }
    void Tick(float) {
        Once<TVehTick>("[P5] vehicle::Tick - TODO: player steer (subclass ASafariSteer); drive/fly from input. "
                       "[CONFIRM] vehicle velocity/heading fields (ASafariSteer::SeekDrive 0x466570).");
    }
}

namespace splitscreen {
    void Tick(float) {
        if (!g_splitOn) return;
        Once<TSplit>("[P8] splitscreen::Tick - TODO: render world twice into two view rects; "
                     "per-player camera + HUD. [CONFIRM] world-render entry + camera view-rect apply.");
    }
    void SetEnabled(bool on) { g_splitOn = on; LOG("splitscreen: %s", on?"on":"off"); }
    void SetOrientationVertical(bool v) { g_splitVertical = v; LOG("splitscreen: %s", v?"vertical":"horizontal"); }
}

} // namespace features
