// dinocontrol.cpp - see dinocontrol.h. Recreated for JPOG25 from the JPOG24 binary (source was lost);
// logic, log lines, hints and ini keys match JPOG24.
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include "dinocontrol.h"
#include "kbm.h"
#include "input_map.h"
#include "gamectl.h"
#include "splitscreen.h"
#include "padrumble.h"
#include "xinput_pad.h"
#include "config.h"
#include "game.h"
#include "guard.h"
#include "log.h"
#include "detour.h"

namespace {
    // SimJP.exe (base 0x400000)
    constexpr uintptr_t kDinosaurClass     = 0x008B78C0;  // AUnitDinosaur::m_sClass
    constexpr uintptr_t kCreatureSteerClass= 0x008B1220;  // ACreatureSteer::m_sClass
    constexpr uintptr_t kDefaultStateClass = 0x008B6040;  // ADefaultState::m_sClass
    constexpr uintptr_t kFollowStateCtor   = 0x00537DD0;  // AFollowState(TManagedPtr<AUnitProxy>), size 0x6C
    constexpr uintptr_t kTransferControl   = 0x0058EC50;
    constexpr uintptr_t kReturnToState     = 0x0058EC30;  // ARootStateController::ReturnToState(const TClass&)
    constexpr uintptr_t kSteerStand        = 0x0046BC00;  // ASteer::Command_StandStill
    constexpr uintptr_t kSteerTarget       = 0x0046BC40;  // ASteer::Command_SetTargetPoint(const TVector3&, const TVector3*)
    constexpr uintptr_t kSteerFace         = 0x0046BB10;  // ASteer::Command_FaceDirection(const TVector3&)
    constexpr uintptr_t kSteerSpeedMod     = 0x0046BD80;  // ASteer::SetSpeedModifier(float)
    constexpr uintptr_t kDriveForceRefresh = 0x00490570;  // ADriveManager::ForceRefresh
    constexpr uintptr_t kUnitManager       = 0x0084C3E0;  // AUnitManager::ms_pSingleton
    constexpr uintptr_t kProximityCheck    = 0x004A0690;  // AUnitManager::UnitTypeProximityCheck
    constexpr uintptr_t kHurtUnit          = 0x004CB000;  // AUnitDinosaur::HurtUnit(float, type, AUnit*)
    constexpr int kUnitSteer = 0x10, kUnitFlags = 0x15, kUnitProxy = 0x24, kUnitDrive = 0xA0, kUnitHealth = 0x140;
    constexpr int kSteerPos = 0x64;
    constexpr int kVfnIsDead = 0xCC / 4;

    using fn_this0  = void (__fastcall*)(void* self, void* edx);
    using fn_thisB  = bool (__fastcall*)(void* self, void* edx);
    using fn_thisP  = void (__fastcall*)(void* self, void* edx, const void* p);
    using fn_target = bool (__fastcall*)(void* self, void* edx, const float* pt, const float* opt);
    using fn_face   = bool (__fastcall*)(void* self, void* edx, const float* dir);
    using fn_speed  = bool (__fastcall*)(void* self, void* edx, float);
    using fn_ctor   = void (__fastcall*)(void* self, void* edx, void* proxy);
    using fn_hurt   = void (__fastcall*)(void* self, void* edx, float dmg, int type, void* src);
    using fn_cb     = bool (__stdcall*)(void* unit, float distSq, const float* pos, void* user);
    using fn_prox   = bool (__fastcall*)(void* mgr, void* edx, const float* pos, float radius, const void* cls, fn_cb cb, void* user);

    constexpr uintptr_t kCreateUnit        = 0x0049F650;  // AUnitManager::CreateUnit(const char*, const TVector3& pos, const TVector3& dir)
    constexpr uintptr_t kGetCameraManager  = 0x0050CF80;
    constexpr uintptr_t kGetActiveCamera   = 0x0050CFD0;
    constexpr uintptr_t kCamTargetPos      = 0x0050C770;  // ACamera::GetTargetPosition(TVector3&)
    constexpr uintptr_t kUnitSelection     = 0x008500B0;
    constexpr uintptr_t kSetSelectedUnit   = 0x00526950;  // AUnitSelection::SetSelectedUnit(const AUnit*)
    constexpr uintptr_t kSteerSetPos       = 0x00498A70;  // ACreatureSteer::SetPosition(TVector3&)
    // JPOG36 (owner bug list 15:55, items 1-4) - all checked in the decompile + disassembly:
    constexpr uintptr_t kCSteerSetSpeed  = 0x00498970;  // ACreatureSteer::SetSpeed(float): +0x80 current = +0x88 desired = v (ret 4)
    constexpr uintptr_t kAutoBlend       = 0x00651D90;  // AAnimatableObject::SetAutoMovementBlending(bool) (ret 4; false = stops user anims)
    constexpr uintptr_t kAnimStateStop   = 0x00654D40;  // AAnimatableObject::StopAnimState(const ANamedAnimation&)
    constexpr uintptr_t kAnimStatePlay   = 0x00654D80;  // AAnimatableObject::IsAnimStatePlaying(const ANamedAnimation&)
    constexpr uintptr_t kBehaviourUpdate = 0x00663820;  // AUnitBehaviour::Update() - runs the unit's FSM (this+4 = unit)
    constexpr uintptr_t kToshiAnimSysVtbl= 0x00D449C0;  // AToshiAnimationSystem vftable; slot +0x94 = SetVisible 0x65B390 (ret 0xC)
    constexpr int kSteerMode = 0xCC;                    // ACreatureSteer command: 1 = target point, 3 = stand still
    using fn_create = void*(__fastcall*)(void* mgr, void* edx, const char* name, const float* pos, const float* dir);
    volatile LONG g_reqSpawn = 0;
    void*    g_testA = nullptr; void* g_testB = nullptr; DWORD g_testT0 = 0; int g_testLogs = 0;

    void TestSpawn() {
        const Config& c = config::Get();
        if (c.testDino.empty() || !gamectl::InPark()) return;
        void* mgr = *reinterpret_cast<void**>(game::At(kUnitManager));
        void* a = nullptr; void* b = nullptr; float t[3]{};
        bool ok = GUARDED("debug spawn test dinos", {
            void* cm = reinterpret_cast<void*(__stdcall*)()>(game::At(kGetCameraManager))();
            void* cam = cm ? reinterpret_cast<void*(__fastcall*)(void*, void*)>(game::At(kGetActiveCamera))(cm, nullptr) : nullptr;
            if (mgr && cam) {
                reinterpret_cast<void(__fastcall*)(void*, void*, float*)>(game::At(kCamTargetPos))(cam, nullptr, t);
                float pa[3] = { t[0] - 6.0f, t[1], t[2] }, pb[3] = { t[0] + 6.0f, t[1], t[2] }, dir[3] = { 0, 0, 1 };
                a = reinterpret_cast<fn_create>(game::At(kCreateUnit))(mgr, nullptr, c.testDino.c_str(), pa, dir);
                b = reinterpret_cast<fn_create>(game::At(kCreateUnit))(mgr, nullptr, c.testDino.c_str(), pb, dir);
                if (a) if (void* sel = *reinterpret_cast<void**>(game::At(kUnitSelection)))
                    reinterpret_cast<void(__fastcall*)(void*, void*, const void*)>(game::At(kSetSelectedUnit))(sel, nullptr, a);
            }
        });
        g_testA = a; g_testB = b; g_testT0 = GetTickCount(); g_testLogs = 0;
        LOG("debug: spawned test dinos '%s' %p + %p at (%.1f %.1f %.1f), first selected (ok=%d, selected now %p)",
            c.testDino.c_str(), a, b, t[0], t[1], t[2], ok, gamectl::SelectedUnit());
    }

    // JPOG31: one slot per player, so BOTH players can control a dinosaur at the same time (split-screen).
    // Split-screen never uses AFollowState (JPOG30): the root state stays with the builder, each dino player's
    // half is drawn with our own chase camera (inactive player: splitscreen's second view; active player: the
    // main view's matrix is swapped for the chase matrix while that half is drawn).
    struct Slot {
        void*    dino = nullptr;
        void*    proxy = nullptr;        // AUnitProxy (ref-counted at +4, unit at +0x14)
        void*    followState = nullptr;
        float    lastHealth = -1.0f, speed = 1.0f, attackCd = 0, steerTimer = 0, lastDir[2]{};
        unsigned frames = 0, chaseLogs = 0;
        bool     paused = false, moving = false, prevRT = false;
        bool     split = false, chaseInit = false, chaseValid = false;
        float    yaw = 0, pitch = 0, camPos[3]{}, zoom = 1.0f;
        float    lookOff = 0;
        int      stepRel = -10;        // JPOG69 R6: forces the first step-direction log line
        int      missLogs = 0;         // JPOG69 R7: bite-miss log lines
        float    rbT = 0;              // JPOG69 R2: RB held time (tap = finisher, hold = turn right)
        bool     rbTurning = false;
        bool     coneLogged = false, turnLogged = false;
        // JPOG77 (owner JPOG76 test): walk heading lock - set when the stick goes into the front half, changed only by the
        // right stick (camera turn) or an LB / RB turn; the chase camera never steers.
        bool     headLock = false, headRs = false, turnedSinceLock = false;
        float    head[2] = { 0, 0 };
        int      headLogs = 0;
        bool     human = false;
        int      refuseLogs = 0, stuckLogs = 0; float stuckPos[3] = { 0, 0, 0 }; DWORD stuckSince = 0;   // JPOG73 steering logs            // JPOG73: the grab's victim is a person (GrabPpl.fsm chain)   // JPOG73: log once per ride
        float    lbT = 0;              // JPOG73 (owner 13:29 #6): LB held = turn left (mirror of RB)
        bool     lbTurning = false;
        float    yT = -1.0f;
        bool     runOn = false;  // JPOG56 (owner list 11:51): run = left-stick click toggle, full stick = walk    // JPOG55 (backlog 20): Y held time (tap = call 1, hold = call 2)   // JPOG55 (backlog 13): D-pad Left/Right = camera rig down/up while riding
        float    upSign = -1.0f, fwdSign = 1.0f, r1Sign = -1.0f;   // world up = (0, upSign, 0); matrix row 2 = fwdSign * look dir
        float    chase[16]{};
        DWORD    lastHitT = 0;
        // JPOG32 actions: 0 none, 1 roar, 2 eat, 3 sleep start, 4 sleep loop, 5 sleep end, 6 finishing move
        int      act = 0;
        float    actT = 0, actAge = 0;
        char     anim[24]{}, fam[8]{}, vicAnim[24]{};
        void*    vic = nullptr; void* vicProxy = nullptr;
        // JPOG34 (owner bug 6): idle hold point + diagnostics
        float    hold[3]{}; bool holdValid = false; unsigned pins = 0, lodForced = 0; float standT = 0;
        // JPOG36: auto movement blend switched off for an action (the FSMs do the same), bite count, re-steer count
        bool     blendOff = false; unsigned bites = 0, restands = 0; bool wakeLogged = false;
        bool     biteHit = false;   // JPOG55 (backlog 19): the current bite had a target in range (blood allowed)
        // JPOG37: 9 swallow, 10 grab + swallow (CarnGrab.fsm), 11 raptor grapple (RaptHunt.fsm); phase inside an action
        int      phase = 0; float phaseT = 0, hurtT = 0;
        bool     locatorFix = false;      // root-motion attack: SetTransfromFromLocator("D") at its end (RaptAtk/PchyTrty/ToroTrty.fsm)
        unsigned locLogs = 0;            // JPOG44: first 3 locator-fix log lines
        bool     latched = false, targetSet = false;
        bool     zzz = false; uint8_t savedA4 = 0, savedA5 = 0;   // sleep drive id shown (icon) while asleep
        bool     restEdPending = false;   // JPOG57: herbivore get-up after sleep = PlayAnim("rest","ed") (H_Slp.fsm SleepCleanUpWait)
        bool     grappleKill = true, prevLT = false;   // JPOG38: LT pounce (no kill unless weak), RB finisher button
        char     step[16]{};                // JPOG59 (open list 7 / B14): strafe / backwards anim playing ("" = none)
        unsigned stepLogs = 0;
        unsigned backLogs = 0;            // JPOG60 (fix B): own log counter for walking backwards
        // JPOG65: line-up phase (act 13) before a paired kill / raptor latch, like SpecAttk.fsm / RaptHunt.fsm walk in first
        int      alKind = 0;                // 0 = finishing move, 1 = raptor grapple
        float    alFrom[3]{}, alTo[3]{}, alT = 0, alDur = 0, alDist0 = 0, alWant = 0;
        bool     alKill = true, interp = false;
    };
    Slot     g_s[2];
    enum AnimOp : int { A_LOADED, A_PLAY, A_PLAYING, A_STOP, A_STATE_PLAYING, A_STATE_STOP };
    bool Anim(void* unit, const char* name, AnimOp op, bool loop, bool lock);
    volatile LONG g_reqControl = -1, g_reqRelease = -1;
    volatile LONG g_reqInfo = -1;   // JPOG55 (backlog 2): RB + D-pad Up = the game's own info bar for the selected unit
    // JPOG34 (owner bug 2): the player WITHOUT the HUD picks a dino near the middle of their own view.
    volatile LONG g_reqPick = -1; float g_pickPt[2][3]{};
    volatile LONG g_reqSel = -1; float g_selPt[2][3]{};   // JPOG68
    volatile LONG g_ctl[2] = { 0, 0 };   // 1 = that player controls a dino
    void* volatile g_forceVisMI[2] = { nullptr, nullptr };   // JPOG36: model instances kept "visible" (Hook_SetVisible)
    XPad     g_pads[2] = { XPad(0), XPad(1) };

    void* Steering(void* unit) {
        void* s = unit ? *reinterpret_cast<void**>((uint8_t*)unit + kUnitSteer) : nullptr;
        return (s && gamectl::IsA(s, kCreatureSteerClass)) ? s : nullptr;
    }
    float Health(void* unit) { return *reinterpret_cast<float*>((uint8_t*)unit + kUnitHealth); }
    bool  Gone(void* unit) { return (*((uint8_t*)unit + kUnitFlags) & 2) != 0; }

    bool Dead(void* unit) {
        bool dead = true;   // a fault counts as dead
        GUARDED("dino is-dead", {
            dead = reinterpret_cast<fn_thisB>((*reinterpret_cast<void***>(unit))[kVfnIsDead])(unit, nullptr); });
        return dead;
    }
    bool DinoPos(void* unit, float* out) {
        void* s = Steering(unit);
        if (!s) return false;
        const float* p = reinterpret_cast<const float*>((uint8_t*)s + kSteerPos);
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
        return true;
    }
    void SetSpeed(Slot& s, void* unit, float v) {
        if (fabsf(v - s.speed) < 0.01f) return;
        s.speed = v;
        if (!unit) return;
        void* st = *reinterpret_cast<void**>((uint8_t*)unit + kUnitSteer);
        if (!st) return;
        reinterpret_cast<fn_speed>(game::At(kSteerSpeedMod))(st, nullptr, v);
    }
    void Release(void* proxy) {
        GUARDED("release unit proxy", {
            reinterpret_cast<fn_this0>((*reinterpret_cast<void***>(proxy))[1])(proxy, nullptr); });
    }

    void StopAction(int p, void* dino, const char* why);
    void ZzzOff(Slot& s, int p, void* dino);
    void End(int p, const char* why, bool restoreView) {
        Slot& s = g_s[p];
        void* dino = s.dino; void* proxy = s.proxy;
        bool alive = proxy && dino && *reinterpret_cast<void**>((uint8_t*)proxy + 0x14) == dino && !Gone(dino);
        if (s.step[0] && alive) { Anim(dino, s.step, A_STOP, false, false); s.step[0] = 0; }   // JPOG59 strafe / back
        if (s.act || s.blendOff) StopAction(p, alive ? dino : nullptr, why);
        ZzzOff(s, p, alive ? dino : nullptr);
        bool ok = GUARDED("end dino control", {
            void* ctl = gamectl::Ctl();
            if (ctl && restoreView && !s.split) {
                void* st = gamectl::CurrentState();
                if (st == s.followState || gamectl::IsFollowFamily(st))
                    reinterpret_cast<fn_thisP>(game::At(kReturnToState))(ctl, nullptr, game::At(kDefaultStateClass));
            }
            if (alive) {
                if (dino) if (void* drv = *reinterpret_cast<void**>((uint8_t*)dino + kUnitDrive))
                    reinterpret_cast<fn_this0>(game::At(kDriveForceRefresh))(drv, nullptr);   // AI back on
                s.speed = 0.0f;              // force the speed reset through
                SetSpeed(s, dino, 1.0f);
            }
        });
        if (proxy && --*reinterpret_cast<int*>((uint8_t*)proxy + 4) == 0) Release(proxy);
        const bool wasSplit = s.split;
        s = Slot{};
        g_forceVisMI[p] = nullptr;
        InterlockedExchange(&g_ctl[p], 0);
        if (!wasSplit) splitscreen::AdoptActiveCamera();
        LOG("dino: player %d let go (%s, ok=%d%s)", p + 1, why, ok, wasSplit ? ", split: own camera back to the world camera" : "");
        // Split: the other player still rides a dino and holds the HUD -> the free player builds now.
        if (wasSplit && g_ctl[1 - p] && splitscreen::CanSwitch() && splitscreen::ActivePlayer() == 1 - p) {
            splitscreen::RequestActive(p);
            LOG("dino: player %d still controls a dino - HUD goes to player %d", 2 - p, p + 1);
        }
        gamectl::RequestHintFor(p, L"Back to building the park", 2.5f);
    }

    // JPOG34 (owner bug 6): "controlled dinos walk on their own". The log showed the idle dino jumping ~15 m every
    // few seconds. Cause (decompile, AActiveUnit::Imp_UpdateControl 0x4BE800): a unit whose model is not flagged
    // visible (model instance +0x40 & 0x40000) is only updated once per second with dt = 1 s, and its behaviour
    // (AUnitBehaviour::Update - not the drive manager we freeze) keeps walking it. In split-screen the rider's dino is
    // usually off the ACTIVE (builder's) camera -> low-rate behaviour updates. Fix: (1) flag the controlled dino's
    // model as visible every frame (it IS visible - in the rider's half), (2) idle = pin the dino to the point where
    // the stick was let go (steering position) + StandStill every 0.1 s.
    void ForceFullUpdate(Slot& s, int p, void* dino) {
        void* ao = *reinterpret_cast<void**>((uint8_t*)dino + 0x30);
        void* mi = ao ? *reinterpret_cast<void**>((uint8_t*)ao + 0x80) : nullptr;
        if (!mi) return;
        volatile uint32_t* fl = reinterpret_cast<volatile uint32_t*>((uint8_t*)mi + 0x40);
        if (!(*fl & 0x40000)) {
            *fl |= 0x40000;
            if (s.lodForced++ < 2 || (s.lodForced % 600) == 0)
                LOG("dino: P%d dino was in low-rate (off-screen) update - forced full rate (%u times)", p + 1, s.lodForced);
        }
    }
    // JPOG36 (owner bugs 1 + 2): "keeps walking ~10 s after the stick is released" + "spasms". Decompile: StandStill only
    // sets the command (ACreatureSteer+0xCC = 3); ProcessTick then brakes the CURRENT speed (+0x80) towards 0 at the
    // species' Dec rate (T-Rex: run 9.5 x 1.6 run_mult, Dec 3.0 -> ~5 s of sliding). JPOG34 pinned the sliding dino back
    // every 0.75 m (log: thousands of "pinned back" lines) = the shaking. Now: stop at once (ACreatureSteer::SetSpeed(0)
    // = current + desired speed 0) and keep it at 0 while idle; stand still again only if something re-steered it;
    // no pinning (only a safety pin for a > 4 m jump).
    void CSteerSpeed0(void* steer) {
        reinterpret_cast<void(__fastcall*)(void*, void*, float)>(game::At(kCSteerSetSpeed))(steer, nullptr, 0.0f);
    }
    void StopMotion(void* dino) {
        void* steer = Steering(dino);
        if (!steer) return;
        GUARDED("dino stop motion", {
            reinterpret_cast<fn_thisB>(game::At(kSteerStand))(steer, nullptr);
            CSteerSpeed0(steer); });
    }
    void HoldStill(Slot& s, int p, void* dino, const float* pos, float dt, bool action) {
        void* steer = Steering(dino);
        if (!steer) return;
        if (!s.holdValid) {
            s.hold[0] = pos[0]; s.hold[1] = pos[1]; s.hold[2] = pos[2]; s.holdValid = true; s.standT = 0;
            if (!action) reinterpret_cast<fn_thisB>(game::At(kSteerStand))(steer, nullptr);
            CSteerSpeed0(steer);
        }
        // Something gave the idle dino a new command (an action's face-direction is fine): stand still again.
        const int mode = *reinterpret_cast<int*>((uint8_t*)steer + kSteerMode);
        if (!action && mode != 3) {
            reinterpret_cast<fn_thisB>(game::At(kSteerStand))(steer, nullptr);
            if (s.restands++ < 3 || (s.restands % 300) == 0)
                LOG("dino: P%d idle dino got steer command %d from the game - stood still again (%u times)", p + 1, mode, s.restands);
        }
        CSteerSpeed0(steer);     // no sliding: current + desired speed 0 every idle frame
        float dx = pos[0] - s.hold[0], dz = pos[2] - s.hold[2];
        if (!action && dx * dx + dz * dz > 4.0f * 4.0f) {
            float h[3] = { s.hold[0], pos[1], s.hold[2] };
            reinterpret_cast<void(__fastcall*)(void*, void*, float*)>(game::At(kSteerSetPos))(steer, nullptr, h);
            if (s.pins++ < 3 || (s.pins % 100) == 0)
                LOG("dino: P%d idle dino jumped %.1f m - put back to (%.1f %.1f) (%u times)", p + 1, sqrtf(dx * dx + dz * dz), s.hold[0], s.hold[2], s.pins);
        }
        (void)dt;
    }

    bool TryBreakFence(int p, void* dino, const float* pos, float dirx, float dirz, const char* how);   // JPOG69 R4
    bool Anim(void* unit, const char* name, AnimOp op, bool loop, bool lock);
    void BlendOff(Slot& s, void* unit);
    void BlendOn(Slot& s, void* unit);
    void StepEnd(Slot& s, void* dino) {
        if (!s.step[0]) return;
        char was[16]; strcpy(was, s.step);
        s.step[0] = 0;
        if (s.act != 0) return;          // an action took over (it owns the anim + blend now)
        Anim(dino, was, A_STOP, false, false);
        BlendOn(s, dino);
    }
    // One frame of strafe / backwards (JPOG61 rewrite, owner 13:11): the dino keeps the facing it has, the stick moves it
    // RELATIVE TO THE CAMERA exactly like a normal walk does (same right/forward vectors as Steer below), so left on the
    // stick = left on the screen, always. The steering gets StandStill (no seek target left that could turn it) + speed 0
    // every frame; the position is moved by hand. Anim: back-walk when the push is mostly backwards (walk_bck_lp if the
    // model has it, else walk_lp), walk_lp when it is mostly sideways.
    bool Step(Slot& s, int p, void* dino, const float* pos, void* steer, float sx, float sy, float mag,
              float crx, float crz, float cfx, float cfz, float dt) {
        const Config& c = config::Get();
        bool back = -sy >= fabsf(sx);
        // JPOG69 R6 (owner 16:54: "turn the camera all the way round with the right stick and left/right are inverted"):
        // the push still moves the dino along the SCREEN (camera right / forward, same as before), but WHAT the dino does
        // now depends on its own facing: a push that points the way the dino already faces = a normal forward walk (the
        // dino no longer back-walks towards the camera when the camera is in front of it), the opposite way = walk
        // backwards, across = side-step. [dino] step_by_facing = 0 = JPOG61-68 (stick down = back-walk, always).
        if (c.dinoStepByFacing) {
            float wx = crx * sx * c.dinoStepInvX + cfx * sy, wz = crz * sx * c.dinoStepInvX + cfz * sy;
            const float wl = sqrtf(wx * wx + wz * wz);
            float fx = 0, fz = 0;
            GUARDED("dino step facing", {
                fx = *reinterpret_cast<float*>((uint8_t*)steer + 0x70); fz = *reinterpret_cast<float*>((uint8_t*)steer + 0x74); });
            const float fl = sqrtf(fx * fx + fz * fz);
            if (wl > 1e-4f && fl > 1e-4f) {
                wx /= wl; wz /= wl; fx /= fl; fz /= fl;
                const float along = wx * fx + wz * fz;
                const float edge = s.step[0] ? 0.08f : 0.0f;
                if ((s.frames % 120) == 0 || s.stepRel < -5)
                    LOG("dino: P%d step stick (%.2f %.2f) -> world dir (%.2f %.2f), dino faces (%.2f %.2f), along %.2f, camera right (%.2f %.2f)",
                        p + 1, sx, sy, wx, wz, fx, fz, along, crx, crz);
                s.stepRel = 0;
                if (along > 0.70f + edge) { StepEnd(s, dino); return false; }   // the way it faces: normal walk
                back = along < -0.70f - edge;
            }
        }
        const char* want = "walk_lp";
        if (back && c.dinoBackAll == 0 && !Anim(dino, "walk_bck_lp", A_LOADED, false, false)) {
            if (s.backLogs++ < 2) LOG("dino: P%d no walk_bck_lp for this species - turns round ([dino] back_all = 0)", p + 1);
            return false;
        }
        if (back && Anim(dino, "walk_bck_lp", A_LOADED, false, false)) want = "walk_bck_lp";
        if (!s.step[0]) {
            // entering: drop whatever the steering was still heading for (that target was what turned the dino)
            GUARDED("dino step stand", { reinterpret_cast<fn_thisB>(game::At(kSteerStand))(steer, nullptr); });
        }
        if (strcmp(s.step, want) != 0) {
            StepEnd(s, dino);
            BlendOff(s, dino);
            if (!Anim(dino, want, A_PLAY, true, false)) {
                BlendOn(s, dino);
                if (s.backLogs++ < 4) LOG("dino: P%d strafe/back: anim %s refused - normal walk instead", p + 1, want);
                return false;
            }
            strncpy(s.step, want, sizeof s.step - 1);
            if (back) { if (s.backLogs++ < 6) LOG("dino: P%d walks backwards, facing forward (%s)", p + 1, want); }
            else if (s.stepLogs++ < 6) LOG("dino: P%d strafes %s, facing forward (%s)", p + 1, sx > 0 ? "right" : "left", want);
        }
        float vx = crx * sx * c.dinoStepInvX + cfx * sy, vz = crz * sx * c.dinoStepInvX + cfz * sy;
        const float vl = sqrtf(vx * vx + vz * vz);
        if (vl < 0.0001f) return true;
        vx /= vl; vz /= vl;
        const float m1 = mag > 1.0f ? 1.0f : mag;
        const float v = (back ? c.dinoBackSpeed : c.dinoStrafeSpeed) * m1 * dt;
        float np[3] = { pos[0] + vx * v, pos[1], pos[2] + vz * v };
        GUARDED("dino strafe / back", {
            const int mode = *reinterpret_cast<int*>((uint8_t*)steer + kSteerMode);
            if (mode != 3) reinterpret_cast<fn_thisB>(game::At(kSteerStand))(steer, nullptr);
            CSteerSpeed0(steer);
            reinterpret_cast<void(__fastcall*)(void*, void*, float*)>(game::At(kSteerSetPos))(steer, nullptr, np);
        });
        s.moving = true; s.holdValid = false;
        return true;
    }

    // Left stick -> steering target ahead of the dino, relative to the view (camera right/forward).
    void Steer(int p, void* dino, const float* pos, float dt) {
        const Config& c = config::Get();
        Slot& s = g_s[p];
        if (!dino) return;
        float sx = g_pads[p].LX() * c.dinoInvX, sy = g_pads[p].LY() * c.dinoInvY;
        if (c.dinoRunButton && g_pads[p].Pressed(PAD_LS)) {
            s.runOn = !s.runOn;
            LOG("dino: P%d run %s (left-stick click)", p + 1, s.runOn ? "ON" : "OFF");
            wchar_t rk[32], rt[96];   // JPOG80: the configured run key
            dinocontrol::KeyName(c.rideRun, rk, L"Right Shift");
            gamectl::WFmt(rt, 96, s.runOn ? L"Run ON (%ls to walk)" : L"Walk (%ls to run)", rk);
            gamectl::RequestHintFor(p, dinocontrol::PcPlayer(p) ? rt
                                             : (s.runOn ? L"Run ON (click the left stick to walk)" : L"Walk (click the left stick to run)"), 2.0f);
        }
        void* steer = *reinterpret_cast<void**>((uint8_t*)dino + kUnitSteer);
        if (!steer) return;
        float mag = sqrtf(sx * sx + sy * sy);
        s.steerTimer -= dt;
        if (mag < 0.2f) {
            StepEnd(s, dino);
            if (s.headLock) { s.headLock = false; if (s.headLogs++ < 300) LOG("dino: P%d walk heading unlocked (stick let go)", p + 1); }   // JPOG77
            // Stick idle: hold the dino where the stick was let go (JPOG34: pinned, see HoldStill).
            if (s.moving) { s.moving = false; s.holdValid = false; }
            GUARDED("dino hold still", { HoldStill(s, p, dino, pos, dt, false); });
            if ((s.frames % 300) == 0) LOG("dino: P%d idle hold at (%.1f %.1f %.1f)", p + 1, pos[0], pos[1], pos[2]);
            return;
        }
        s.holdValid = false;
        float m[16];
        if (s.split) {
            if (!s.chaseValid) return;
            // forward = where the chase camera looks (known exactly), right = matrix row 0 (screen right)
            for (int i = 0; i < 16; ++i) m[i] = s.chase[i];
            m[8] = sinf(s.yaw); m[9] = 0; m[10] = cosf(s.yaw);
        } else if (!splitscreen::ViewMatrix(m)) return;
        float fl = sqrtf(m[8] * m[8] + m[10] * m[10]);
        if (fl < 0.0001f) return;
        float rl = sqrtf(m[0] * m[0] + m[2] * m[2]);
        if (rl < 0.0001f) return;
        // JPOG61 (owner 13:11): forward part of the stick (inside 45 deg of straight ahead) = normal walk, the dino turns
        // to the camera-relative direction. Everything else (sideways, backwards, back-diagonals) = strafe / back-walk
        // with the facing kept. Small hysteresis so the dino doesn't flip between the two at the 45 deg line.
        // JPOG73 (owner 13:29 #7): wide forward cone. Stick up / up-left / up-right (within [dino] stick_cone degrees of
        // straight up) = walk STRAIGHT where the camera looks (look with the right stick, then push). Only a HARD left /
        // right ([dino] stick_hard_turn of full sideways, little up/down) turns the dino; the rest of the front half is
        // straight too. Back half: strafe / back-walk as before.
        bool hardTurn = false;
        bool lockStraight = false;   // JPOG77
        if (c.dinoStickLock) {
            // JPOG77 (owner after JPOG76: "a slight diagonal still turns the dino; walk STRAIGHT where the camera looks,
            // turning only by turning the camera or LB / RB"): the WHOLE front half walks straight (sideways part dropped),
            // only a near-pure sideways push ([dino] stick_side_band deg from sideways) or the back half strafes / back-walks.
            const float ang = atan2f(fabsf(sx), sy) * 57.2957795f;   // 0 = straight up, 90 = sideways, 180 = down
            if (ang <= 90.0f - c.dinoStickSide) { lockStraight = true; sx = 0.0f; sy = mag > 1.0f ? 1.0f : mag; }
        } else if (c.dinoStickCone > 0.0f) {
            const float ang = atan2f(fabsf(sx), sy) * 57.2957795f;   // 0 = straight up, 90 = sideways
            hardTurn = ang > c.dinoStickCone && fabsf(sx) >= c.dinoStickHardTurn && fabsf(sy) < 0.6f;
            // JPOG75 (owner 15:53: "no diagonal steer, but sideways = strafe"): straight only INSIDE the cone; outside it
            // (stick near full left / right) falls through to the strafe below. Hard turn is off by default (stick_hard_turn 2).
            if (!hardTurn && sy > 0.0f && ang <= c.dinoStickCone) {
                if (!s.coneLogged) { s.coneLogged = true; LOG("dino: P%d stick in the forward cone (%.0f deg) -> straight ahead (JPOG73, cone %.0f)", p + 1, ang, c.dinoStickCone); }
                sx = 0.0f; sy = mag > 1.0f ? 1.0f : mag;
            } else if (hardTurn && !s.turnLogged) { s.turnLogged = true; LOG("dino: P%d hard %s on the stick -> turn", p + 1, sx > 0 ? "right" : "left"); }
        }
        if (c.dinoStrafeBack && !hardTurn) {
            const float edge = s.step[0] ? 0.15f : 0.0f;
            const bool forward = sy > fabsf(sx) + edge;
            if (!forward && Step(s, p, dino, pos, steer, sx, sy, mag, m[0] / rl, m[2] / rl, m[8] / fl, m[10] / fl, dt)) return;
        }
        StepEnd(s, dino);
        float dx = (m[0] / rl) * sx + (m[8] / fl) * sy;
        float dz = (m[2] / rl) * sx + (m[10] / fl) * sy;
        float dl = sqrtf(dx * dx + dz * dz);
        dx /= dl; dz /= dl;
        if (lockStraight) {
            // JPOG77: the locked heading. First push: camera forward (or the dino's own facing right after an LB / RB turn).
            // Right stick moved while walking = the camera turns -> the heading follows the camera. Otherwise: fixed.
            const bool rs = fabsf(g_pads[p].RX()) > 0.25f;
            const int cap = 300;
            if (!s.headLock) {
                const char* from = "camera forward";
                if (s.turnedSinceLock) {
                    float fx = 0, fz = 0;
                    GUARDED("lock: dino facing", { fx = *reinterpret_cast<float*>((uint8_t*)steer + 0x70); fz = *reinterpret_cast<float*>((uint8_t*)steer + 0x74); });
                    const float ffl = sqrtf(fx * fx + fz * fz);
                    if (ffl > 1e-4f) { dx = fx / ffl; dz = fz / ffl; from = "dino facing after LB / RB"; }
                }
                s.headLock = true; s.turnedSinceLock = false; s.headRs = false;
                s.head[0] = dx; s.head[1] = dz;
                if (s.headLogs++ < cap) LOG("dino: P%d walk heading LOCKED (%.2f %.2f) = %.0f deg from %s (JPOG77 [dino] stick_lock)",
                    p + 1, dx, dz, atan2f(dx, dz) * 57.2957795f, from);
            } else if (rs) {
                s.head[0] = dx; s.head[1] = dz; s.headRs = true;   // camera turning: follow it
            } else {
                if (s.headRs) {
                    s.headRs = false;
                    if (s.headLogs++ < cap) LOG("dino: P%d walk heading CHANGED by the camera to (%.2f %.2f) = %.0f deg", p + 1, s.head[0], s.head[1], atan2f(s.head[0], s.head[1]) * 57.2957795f);
                }
                dx = s.head[0]; dz = s.head[1];
            }
        } else if (s.headLock) {
            s.headLock = false;
            if (s.headLogs++ < 300) LOG("dino: P%d walk heading unlocked (stick sideways / back)", p + 1);
        }
        // Don't re-steer every frame for the same direction (it makes the dino stutter).
        if (s.moving && s.steerTimer > 0 && s.lastDir[0] * dx + s.lastDir[1] * dz > 0.94f) return;
        s.lastDir[0] = dx; s.lastDir[1] = dz;
        s.steerTimer = 0.25f;
        // JPOG55 (backlog 15): "running starts too early" - below [dino] run_start the speed follows the stick from
        // walk_speed_min up to the species' normal pace (1.0); from run_start to a full press: up to run_mult.
        // JPOG56 (owner 11:51): with [dino] run_button = 1 the stick only walks (light push = slow) and the left-stick
        // click toggles running (full run_mult once the stick is past half); 0 = the JPOG55 stick curve.
        float sp;
        const float rs = c.dinoRunStart < 0.3f ? 0.3f : (c.dinoRunStart > 0.99f ? 0.99f : c.dinoRunStart);
        const float m1 = mag > 1.0f ? 1.0f : mag;
        if (c.dinoRunButton) {
            float w = (m1 - 0.2f) / 0.8f; w = w < 0 ? 0 : (w > 1 ? 1 : w);
            sp = s.runOn && m1 > 0.5f ? c.dinoRunMult : c.dinoWalkMin + (1.0f - c.dinoWalkMin) * w;
        } else if (m1 < rs) { float w = (m1 - 0.2f) / (rs - 0.2f); w = w < 0 ? 0 : (w > 1 ? 1 : w); sp = c.dinoWalkMin + (1.0f - c.dinoWalkMin) * w; }
        else { float t = (m1 - rs) / (1.0f - rs); t = t < 0 ? 0 : (t > 1 ? 1 : t); sp = 1.0f + (c.dinoRunMult - 1.0f) * t; }
        SetSpeed(s, dino, sp);   // full stick = run
        // The game refuses a target that is not passable (water, cliff, fence): try closer points.
        float target[3] = { pos[0], pos[1], pos[2] };
        bool accepted = false;
        for (float lead = c.dinoLead; lead >= 2.0f && !accepted; lead *= 0.5f) {
            target[0] = pos[0] + dx * lead; target[2] = pos[2] + dz * lead;
            GUARDED("dino steer", {
                accepted = reinterpret_cast<fn_target>(game::At(kSteerTarget))(steer, nullptr, target, nullptr); });
        }
        if (!s.moving || (s.frames % 240) == 0)
            LOG("dino: P%d steer dir(%.2f %.2f) target (%.1f %.1f %.1f) accepted=%d", p + 1, dx, dz, target[0], target[1], target[2], accepted);
        // JPOG73 (owner #8 / #10): refused or shortened seek points + "stuck while the stick is pushed" (the dino stopping
        // on paths / invisible walls) - logged with position, capped, so the next session sees the reason.
        {
            const float got = sqrtf((target[0]-pos[0])*(target[0]-pos[0]) + (target[2]-pos[2])*(target[2]-pos[2]));
            if ((!accepted || got < c.dinoLead * 0.6f) && s.refuseLogs < 40) {
                ++s.refuseLogs;
                LOG("dino: P%d seek point %s at (%.1f %.1f %.1f): wanted %.1f m ahead, got %.1f m (dir %.2f %.2f)",
                    p + 1, accepted ? "SHORTENED" : "REFUSED", pos[0], pos[1], pos[2], c.dinoLead, accepted ? got : 0.0f, dx, dz);
            }
            const float mv = sqrtf((pos[0]-s.stuckPos[0])*(pos[0]-s.stuckPos[0]) + (pos[2]-s.stuckPos[2])*(pos[2]-s.stuckPos[2]));
            const DWORD nowT = GetTickCount();
            if (mv > 0.5f || !s.stuckSince) { s.stuckPos[0] = pos[0]; s.stuckPos[2] = pos[2]; s.stuckSince = nowT; }
            else if ((LONG)(nowT - s.stuckSince) > 800 && s.stuckLogs < 30) {
                ++s.stuckLogs; s.stuckSince = nowT + 2000;   // next line at the earliest 2.8 s later
                float np[3]; void* nd = dinocontrol::NearestDino(pos, 25.0f, np);
                float rad = 0.0f;
                GUARDED("stuck: own radius", { rad = reinterpret_cast<float(__fastcall*)(void*, void*, int)>((*reinterpret_cast<void***>(dino))[0xB8 / 4])(dino, nullptr, 9); });
                float bd = 0.0f; void* nb = dinocontrol::NearestUnitOf(pos, 25.0f, 0x008B7A60, &bd);
                // JPOG74: + building centre distance, own radius and seek-point length -> decide the rider avoidance fix
                LOG("dino: P%d STOPPED with the stick pushed at (%.1f %.1f %.1f) dir (%.2f %.2f) accepted=%d seek %.1f m speed %.2f - nearest dino %p %.1f m, building %p %.1f m (own radius %.1f)",
                    p + 1, pos[0], pos[1], pos[2], dx, dz, accepted, sqrtf((target[0]-pos[0])*(target[0]-pos[0]) + (target[2]-pos[2])*(target[2]-pos[2])),
                    s.speed, nd, nd ? sqrtf((np[0]-pos[0])*(np[0]-pos[0]) + (np[2]-pos[2])*(np[2]-pos[2])) : 0.0f, nb, bd, rad);
            }
        }
        s.moving = true;
    }

    // JPOG36: the FSMs switch the automatic locomotion blend OFF before an action anim (C_Slp.fsm: SetAutoMovementBlend
    // (false) + PlayAnim("sleep","st"); C_Hngr.fsm eat) and back ON in their clean-up. With it on, the walk/idle blend is
    // mixed over our anims (half lying down, eating cut short). SetAutoMovementBlending(false) also stops all user anims,
    // so it is called BEFORE the action anim is played.
    void BlendOff(Slot& s, void* unit) {
        void* ao = unit ? *reinterpret_cast<void**>((uint8_t*)unit + 0x30) : nullptr;
        if (!ao || s.blendOff) return;
        GUARDED("dino blend off", { reinterpret_cast<bool(__fastcall*)(void*, void*, bool)>(game::At(kAutoBlend))(ao, nullptr, false); });
        s.blendOff = true;
    }
    void BlendOn(Slot& s, void* unit) {
        void* ao = unit ? *reinterpret_cast<void**>((uint8_t*)unit + 0x30) : nullptr;
        if (!s.blendOff) return;
        s.blendOff = false;
        if (ao) GUARDED("dino blend on", { reinterpret_cast<bool(__fastcall*)(void*, void*, bool)>(game::At(kAutoBlend))(ao, nullptr, true); });
    }
    bool Anim(void* unit, const char* name, AnimOp op, bool loop, bool lock);
    bool PickAnim(void* unit, const char* list, char* out, size_t n);
    bool TryFinisher(int p, void* dino, const float* pos, void* victim, bool forced = false);
    struct Nearest { void* self; void* best; float bestDistSq; int seen, skipped; bool skipCtl = false; const void* cls = nullptr; };
    // ---- JPOG37: call the game's own behaviour-script (FSM) actions directly ----
    // Every FSM action is "AParam AActiveUnit::X(const AParams&)" (thiscall, ret 8): AParams = { vftable, count,
    // AParam* }, AParam = { type, value } (1 bool, 2 int, 3 float, 7 TCString* - built by AParam(const char*) 0x65F850,
    // freed by ~AParam 0x660EE0). Lets us replay the scripts' exact steps (RaptAtk / RaptHunt / CarnGrab / C_Slp.fsm).
    struct FParam { int type; uint32_t v; };
    struct FParams { void* vtbl; int n; FParam* arr; };
    constexpr uintptr_t kAParamsVtbl = 0x00D461A0, kAParamStr = 0x0065F850, kAParamDtor = 0x00660EE0;
    constexpr uintptr_t kActSetTransformFromLocator = 0x004AD320, kActLatch = 0x004AA390, kActUnLatch = 0x004AA720,
        kActIsAttached = 0x004AA6D0, kActStopLook = 0x004B1E10, kActCanGrab = 0x004B52E0, kActPreGrab = 0x004B5560,
        kActGrab = 0x004B53C0, kActSwallowScale = 0x004B5AF0, kActCarnEat = 0x004B81B0, kActRelease = 0x004B5670,
        kActDestroyTarget = 0x004B61B0;
    constexpr uintptr_t kActAutoLinkOff = 0x004B0B80;
    // JPOG73 (owner #7): GenFSM\GrabPpl.fsm (CarnGrabHuman) - the vanilla "carnivore eats a person" chain.
    constexpr uintptr_t kActAttachTarget = 0x004B5900, kActDetachTarget = 0x004B5A00, kActKillHuman = 0x004AF900;
    constexpr uintptr_t kUnitClass = 0x008B7B20;          // AUnit::m_sClass (any unit; humans = flags byte +0x15 bit 0x10, KillHuman)    // SetAutoLinkAnimOff(family, suffix, bool)
    constexpr uintptr_t kFoodClass = 0x008B7880;          // AUnitFood::m_sClass (goat / cow = AUnitFoodCow)
    constexpr int kUnitTarget = 0xC8;                      // AActiveUnit target (TManagedPtr<AUnitProxy>, GetTargetedUnit 0x4A7B20)
    // spec: one char per param: 's' string, 'b' bool, 'f' float. Returns the action's bool result (false on a fault).
    bool Act(void* unit, uintptr_t fn, const char* what, const char* spec = "", const char* s0 = nullptr, const char* s1 = nullptr,
             int b = 0, float f0 = 0, float f1 = 0) {
        FParam arr[4] = {}; FParams ps{ game::At(kAParamsVtbl), 0, arr };
        uint32_t out[2] = { 0, 0 };
        bool ran = GUARDED(what, {
            const char* strs[2] = { s0, s1 }; int si = 0; float fl[2] = { f0, f1 }; int fi = 0;
            for (int i = 0; spec[i] && i < 4; ++i) {
                if (spec[i] == 's') reinterpret_cast<void(__fastcall*)(void*, void*, const char*)>(game::At(kAParamStr))(&arr[i], nullptr, strs[si++ & 1]);
                else if (spec[i] == 'b') { arr[i].type = 1; arr[i].v = b ? 1u : 0u; }
                else if (spec[i] == 'f') { arr[i].type = 3; memcpy(&arr[i].v, &fl[fi++ & 1], 4); }
                ps.n = i + 1;
            }
            reinterpret_cast<void(__fastcall*)(void*, void*, void*, const void*)>(game::At(fn))(unit, nullptr, out, &ps);
        });
        GUARDED("free action params", {
            for (int i = 0; i < ps.n; ++i) if (arr[i].type == 7) reinterpret_cast<void(__fastcall*)(void*, void*)>(game::At(kAParamDtor))(&arr[i], nullptr);
        });
        return ran && out[0] == 1 && (out[1] & 0xFF) != 0;
    }
    // unit's target = victim (what the FSMs' GetFlockTarget / SetTargetsTargetToMe do) - ref-counted proxy pointer.
    bool SetTarget(void* unit, void* victim) {
        void* np = victim ? *reinterpret_cast<void**>((uint8_t*)victim + kUnitProxy) : nullptr;
        void** slot = reinterpret_cast<void**>((uint8_t*)unit + kUnitTarget);
        if (*slot == np) return np != nullptr;
        if (np) ++*reinterpret_cast<int*>((uint8_t*)np + 4);
        void* old = *slot; *slot = np;
        if (old && --*reinterpret_cast<int*>((uint8_t*)old + 4) == 0) Release(old);
        return np != nullptr;
    }

    // JPOG37 (owner bug 1): nobody can pick / take the dinosaur the OTHER player (or they themselves) controls.
    bool Controlled(void* u) { return u && ((g_ctl[0] && g_s[0].dino == u) || (g_ctl[1] && g_s[1].dino == u)); }
    bool __stdcall NearestCb(void* unit, float distSq, const float* /*pos*/, void* user) {
        Nearest* n = static_cast<Nearest*>(user);
        ++n->seen;
        if (!unit || unit == n->self || Gone(unit) || Dead(unit) || (n->skipCtl && Controlled(unit))) { ++n->skipped; return false; }
        if (distSq < n->bestDistSq) { n->bestDistSq = distSq; n->best = unit; }
        return false;
    }

    float g_lastRange = 6.0f;   // JPOG69 R7: the range FindTarget used last (attack_range or the big-dino range)
    // Nearest other live dino within attack range (null = nobody).
    void* FindTarget(void* dino, const float* pos, float* distSq) {
        void* mgr = *reinterpret_cast<void**>(game::At(kUnitManager));
        if (!mgr) return nullptr;
        Nearest n{ dino, nullptr, 1e30f, 0, 0, false, nullptr };
        // JPOG55 (backlog 18): big dinos (Brachiosaurus stomp) reach beyond the fixed attack_range measured from their
        // centre: range = max(attack_range, own bounding radius (unit vfn 0xB8(9), as the renderer) + 4 m).
        float range = config::Get().dinoAttackRange, rad = 0.0f;
        if (config::Get().dinoSizeRange) {
            GUARDED("dino radius", { rad = reinterpret_cast<float(__fastcall*)(void*, void*, int)>((*reinterpret_cast<void***>(dino))[0xB8 / 4])(dino, nullptr, 9); });
            if (rad > 0.0f && rad < 60.0f && rad + 4.0f > range) range = rad + 4.0f;
        }
        static int s_rangeLogs = 0;
        if (s_rangeLogs < 6 && range > config::Get().dinoAttackRange) { ++s_rangeLogs; LOG("dino: big dino attack range %.1f m (radius %.1f)", range, rad); }
        GUARDED("find attack target", {
            reinterpret_cast<fn_prox>(game::At(kProximityCheck))(mgr, nullptr, pos, range,
                game::At(kDinosaurClass), &NearestCb, &n); });
        if (distSq) *distSq = n.bestDistSq;
        g_lastRange = range;
        return n.best;
    }
    void HurtTarget(int p, void* dino, void* victim, float distSq) {
        const Config& c = config::Get();
        float before = Health(victim);
        float dmg = before * c.dinoDamageFrac;
        if (dmg < c.dinoDamageMin) dmg = c.dinoDamageMin;
        bool ok = GUARDED("dino attack", { reinterpret_cast<fn_hurt>(game::At(kHurtUnit))(victim, nullptr, dmg, 3, dino); });
        LOG("dino: player %d attacks %p (dist %.1f) dmg %.1f health %.1f -> %.1f ok=%d", p + 1, victim, sqrtf(distSq), dmg, before, Health(victim), ok);
        padrumble::Pulse(p, 0.7f, 0.2f);
    }

    // ---- JPOG32: dino action buttons (the dinos' own animations, by name) ----
    // ANamedAnimation("call_01_lp"): the state comes from the name prefix (AAnimatableObjectType::FindState),
    // the name is matched case-insensitively against the model's anims. The unit's AAnimatableObject = unit+0x30
    // (AActiveUnit::PlayAnim 0x4AFF50 does the same). Anim names: Data\Units\<dino>.inx (DATA FIRST).
    constexpr uintptr_t kNamedCtor   = 0x00659CE0;  // ANamedAnimation::ANamedAnimation(const char*)
    constexpr uintptr_t kNamedDtor   = 0x0046B090;  // ANamedAnimation::~ANamedAnimation
    constexpr uintptr_t kAnimPlay    = 0x00651420;  // AAnimatableObject::PlayAnim(const ANamedAnimation&, bool loop, bool lockLastPose)
    constexpr uintptr_t kAnimPlaying = 0x00654DB0;  // AAnimatableObject::IsAnimPlaying(const ANamedAnimation&)
    constexpr uintptr_t kAnimStop    = 0x00654D60;  // AAnimatableObject::StopAnim(const ANamedAnimation&)
    constexpr uintptr_t kAnimLoaded  = 0x006521D0;  // AAnimatableObject::IsAnimLoaded(OBJECT_ANIM_STATES, const TCString&)
    constexpr int kUnitAnimObj = 0x30, kNamedState = 8, kNoState = 0x32;

    bool Anim(void* unit, const char* name, AnimOp op, bool loop, bool lock) {
        if (!unit || !name || !*name) return false;
        void* ao = *reinterpret_cast<void**>((uint8_t*)unit + kUnitAnimObj);
        if (!ao) return false;
        alignas(8) uint8_t na[32] = {};
        bool r = false;
        GUARDED("dino anim", {
            reinterpret_cast<void(__fastcall*)(void*, void*, const char*)>(game::At(kNamedCtor))(na, nullptr, name);
            int st = *reinterpret_cast<int*>(na + kNamedState);
            if (st != kNoState) {
                switch (op) {
                case A_LOADED:  r = reinterpret_cast<bool(__fastcall*)(void*, void*, int, const void*)>(game::At(kAnimLoaded))(ao, nullptr, st, na); break;
                case A_PLAY:    reinterpret_cast<void(__fastcall*)(void*, void*, const void*, bool, bool)>(game::At(kAnimPlay))(ao, nullptr, na, loop, lock); r = true; break;
                case A_PLAYING: r = reinterpret_cast<bool(__fastcall*)(void*, void*, const void*)>(game::At(kAnimPlaying))(ao, nullptr, na); break;
                case A_STOP:    reinterpret_cast<void(__fastcall*)(void*, void*, const void*)>(game::At(kAnimStop))(ao, nullptr, na); r = true; break;
                case A_STATE_PLAYING: r = reinterpret_cast<bool(__fastcall*)(void*, void*, const void*)>(game::At(kAnimStatePlay))(ao, nullptr, na); break;
                case A_STATE_STOP:    reinterpret_cast<void(__fastcall*)(void*, void*, const void*)>(game::At(kAnimStateStop))(ao, nullptr, na); r = true; break;
                }
            }
            reinterpret_cast<void(__fastcall*)(void*, void*)>(game::At(kNamedDtor))(na, nullptr);
        });
        return r;
    }
    // First name in a '|'-separated list that this unit's model has; "" = none.
    bool PickAnim(void* unit, const char* list, char* out, size_t n) {
        out[0] = 0;
        const char* p = list;
        while (*p) {
            const char* e = strchr(p, '|');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            char nm[32]; if (len >= sizeof nm) len = sizeof nm - 1;
            memcpy(nm, p, len); nm[len] = 0;
            if (Anim(unit, nm, A_LOADED, false, false)) { strncpy(out, nm, n - 1); out[n - 1] = 0; return true; }
            if (!e) break;
            p = e + 1;
        }
        return false;
    }
    void Stand(void* unit) {
        if (void* st = Steering(unit))
            GUARDED("dino stand", { reinterpret_cast<fn_thisB>(game::At(kSteerStand))(st, nullptr); });
    }
    void FaceTo(void* unit, const float* from, const float* to) {
        void* st = *reinterpret_cast<void**>((uint8_t*)unit + kUnitSteer);
        float dir[3] = { to[0] - from[0], 0.0f, to[2] - from[2] };
        if (st) GUARDED("dino face", { reinterpret_cast<fn_face>(game::At(kSteerFace))(st, nullptr, dir); });
    }
    void FreezeAI(void* unit) {
        if (void* drv = *reinterpret_cast<void**>((uint8_t*)unit + kUnitDrive))
            *reinterpret_cast<float*>((uint8_t*)drv + 0xC) = -1000.0f;
    }
    // ---- JPOG65: paired-kill alignment (owner 13:58: "bites the neck ~10 ft away") ----
    // Vanilla (GenFSM\SpecAttk.fsm / SpnoTrty.fsm): both units walk until the horizontal centre-to-centre distance is
    // inside [param7 - 0.5, param7) (WithinTargetDistanceParam 0x4B3770), FaceTarget each other, then both anims start
    // in the SAME frame with SetInterpolation(true) + SetAvoidanceSteer(false); the winner ends with
    // SetTransfromFromLocator("D", true). param7 ("req distance") / param8 ("actual perfect distance") come from the
    // dinosaur classes' SetFatalityAnims in Units.dll (vftable slot 0x26C; the SimJP AActiveUnit one is an empty stub):
    //   T-rex <-> Spino (Sp / Tx)   13.8 / 14.376      Tricera (Tr)  11.0 / 11.65
    //   Steg (St)                   11.0 / 11.65       Anky (An, plain attk01/dth01)  11.0 / 11.5
    constexpr uintptr_t kActSetInterpolation = 0x004B19C0, kActSetAvoidance = 0x004ACEB0;
    constexpr uintptr_t kSteerSetFacing = 0x00498A40;      // ACreatureSteer::SetFacingDir(TVector3&) (vfn +0x84, ret 4)
    constexpr uintptr_t kSteerRadius    = 0x0063DFA0;      // ACreatureSteer::Imp_GetCollisionRadius() (vfn +0xF8)
    float SteerRadius(void* unit) {
        float r = 0.0f; void* st = Steering(unit);
        if (st) GUARDED("steer radius", { r = reinterpret_cast<float(__fastcall*)(void*, void*)>(game::At(kSteerRadius))(st, nullptr); });
        return (r > 0.05f && r < 40.0f) ? r : 2.0f;
    }
    void FaceDirInstant(void* unit, float dx, float dz) {
        void* st = Steering(unit);
        const float l = sqrtf(dx * dx + dz * dz);
        if (!st || l < 0.001f) return;
        float dir[3] = { dx / l, 0.0f, dz / l };
        GUARDED("dino face instant", { reinterpret_cast<void(__fastcall*)(void*, void*, float*)>(game::At(kSteerSetFacing))(st, nullptr, dir); });
    }
    bool PlaceAt(void* unit, const float* p) {
        void* st = Steering(unit);
        if (!st) return false;
        float np[3] = { p[0], p[1], p[2] };
        return GUARDED("dino place", { reinterpret_cast<void(__fastcall*)(void*, void*, float*)>(game::At(kSteerSetPos))(st, nullptr, np); });
    }
    // Vanilla distance for a pair (both anim names); req = param 7, perf = param 8.
    void FinisherDistance(const char* aa, const char* va, float* req, float* perf) {
        auto has = [&](const char* code) {
            char a[24];
            snprintf(a, sizeof a, "lcarn_%s", code);
            return _strnicmp(aa, a, strlen(a)) == 0 || _strnicmp(va, a, strlen(a)) == 0;
        };
        if (has("Sp") || has("Tx"))      { *req = 13.8f; *perf = 14.376f; }
        else if (has("Tr") || has("St")) { *req = 11.0f; *perf = 11.65f; }
        else                             { *req = 11.0f; *perf = 11.5f; }   // Anky (An / plain attk01 dth01)
    }
    void KillInterp(void* unit, bool on, const char* who) {
        if (!unit || !config::Get().dinoFinisherInterp) return;
        // JPOG69 (crash 16:57:17, SimJP+0xBF079): interpolation mode 6 makes AActiveUnit::Imp_GetSteeringEffect read
        // [[unit+0xA8]+0x34]+0x40 every frame (the unit's fatality entry, set up by the game's own fatal-attack path).
        // A Triceratops that was never set up there had a null entry -> access violation in the next steering tick.
        // Only switch interpolation ON for a unit whose entry exists (OFF is always safe).
        if (on) {
            void* e = nullptr;
            GUARDED("finisher: interpolation entry check", {
                void* a8 = *reinterpret_cast<void**>((uint8_t*)unit + 0xA8);
                if (a8) e = *reinterpret_cast<void**>((uint8_t*)a8 + 0x34); });
            if (!e) {
                LOG("dino: finisher %s %p has no fatality entry ([+0xA8]+0x34 = 0) - interpolation skipped (JPOG69 crash fix)", who, unit);
                return;
            }
        }
        Act(unit, kActSetInterpolation, on ? "finisher: interpolation on" : "finisher: interpolation off", "b", nullptr, nullptr, on ? 1 : 0);
        Act(unit, kActSetAvoidance, on ? "finisher: avoidance off" : "finisher: avoidance on", "b", nullptr, nullptr, on ? 0 : 1);
        (void)who;
    }
    const char* ActName(int a) {
        static const char* n[] = { "none", "roar", "eat", "sleep(lie down)", "sleep", "end", "finishing move", "bite", "sleep(rest -> sleep)",
                                   "swallow", "grab + swallow", "raptor grapple", "drink", "lining up" };
        return (a >= 0 && a <= 13) ? n[a] : "?";
    }

    void DropVictim(Slot& s, bool aiBack) {
        if (s.vic && s.vicProxy && *reinterpret_cast<void**>((uint8_t*)s.vicProxy + 0x14) == s.vic && !Gone(s.vic) && aiBack)
            GUARDED("victim AI back", {
                if (void* drv = *reinterpret_cast<void**>((uint8_t*)s.vic + kUnitDrive))
                    reinterpret_cast<fn_this0>(game::At(kDriveForceRefresh))(drv, nullptr); });
        if (s.vicProxy && --*reinterpret_cast<int*>((uint8_t*)s.vicProxy + 4) == 0) Release(s.vicProxy);
        s.vic = nullptr; s.vicProxy = nullptr; s.vicAnim[0] = 0;
    }
    // JPOG68 (owner 16:28): each player's OWN dinosaur selection (A), taken with RB + D-pad Up. The game has ONE selection
    // (the HUD player's) and its pick under the centre cursor often missed; player 2 had no selection at all. Kept with a
    // proxy reference (proxy +0x14 = the unit while it lives), like the finisher victim.
    void* g_selU[2] = { nullptr, nullptr }; void* g_selP[2] = { nullptr, nullptr };
    unsigned g_selSerial[2] = { 0, 0 };
    DWORD g_selT[2] = { 0, 0 };                       // JPOG73: when this player's own selection was made
    void* volatile g_gameSelU = nullptr; volatile DWORD g_gameSelT = 0;   // JPOG73: last dino the GAME selection got (hook)
    void DropSel(int p) {
        if (g_selP[p] && --*reinterpret_cast<int*>((uint8_t*)g_selP[p] + 4) == 0) Release(g_selP[p]);
        g_selU[p] = g_selP[p] = nullptr;
    }
    void* SelOf(int p) {
        void* u = g_selU[p];
        if (!u) return nullptr;
        if (*reinterpret_cast<void**>((uint8_t*)g_selP[p] + 0x14) != u || Gone(u) || Dead(u)) { DropSel(p); return nullptr; }
        return u;
    }
    void SetSel(int p, void* u) {
        if (g_selU[p] == u) return;
        DropSel(p);
        void* pr = u ? *reinterpret_cast<void**>((uint8_t*)u + kUnitProxy) : nullptr;
        if (!pr) return;
        ++*reinterpret_cast<int*>((uint8_t*)pr + 4);
        g_selU[p] = u; g_selP[p] = pr;
        g_selT[p] = GetTickCount() | 1;
        ++g_selSerial[p];   // JPOG69 R1: a new selection -> that player's camera flies to it (splitscreen)
    }
    void GrabCleanup(Slot& s, void* dino, bool destroyed);
    void GrappleCleanup(Slot& s, void* dino);
    void EndBiteAnim(Slot& s, void* dino);
    void ZzzOff(Slot& s, int p, void* dino);
    // Immediate stop (B, let go, attack, stick on eat): anim off, locomotion blend back on.
    void StopAction(int p, void* dino, const char* why) {
        Slot& s = g_s[p];
        if (!s.act) { if (dino) BlendOn(s, dino); return; }
        if (s.act == 7 && s.locatorFix && dino) EndBiteAnim(s, dino);   // JPOG44: before the stop (pose still at the end)
        if (dino && s.anim[0]) Anim(dino, s.anim, A_STOP, false, false);
        if (dino && (s.act == 3 || s.act == 4 || s.act == 8)) { Anim(dino, "sleep_lp", A_STATE_STOP, false, false); Anim(dino, "rest_lp", A_STATE_STOP, false, false); }
        if (s.act == 6 || s.act == 13) {
            if (s.interp) { KillInterp(dino, false, "attacker"); if (s.vic && !Gone(s.vic)) KillInterp(s.vic, false, "victim"); s.interp = false; }
            if (s.act == 6) s.locatorFix = false;
            if (s.act == 13 && s.alKind == 1 && dino) SetTarget(dino, nullptr);
            DropVictim(s, true);
        }
        if (s.act == 10) GrabCleanup(s, dino, false);
        if (s.act == 11) GrappleCleanup(s, dino);
        LOG("dino: P%d action %s stopped (%s)", p + 1, ActName(s.act), why);
        s.act = 0; s.anim[0] = 0; s.fam[0] = 0;
        if (dino) BlendOn(s, dino);
    }
    // Graceful end (X again, LB again, stick while asleep): the game's own clean-up - StopAnimState lets the engine play
    // the end / get-up part; act 5 waits until the state stops, then the locomotion blend goes back on (C_Slp.fsm CleanUp).
    void EndAction(int p, void* dino, const char* why) {
        Slot& s = g_s[p];
        if (!s.act) return;
        if (s.anim[0]) Anim(dino, s.anim, A_STATE_STOP, false, false);
        const bool slept = s.act == 3 || s.act == 4 || s.act == 8;
        if (slept) { Anim(dino, "sleep_lp", A_STATE_STOP, false, false); Anim(dino, "rest_lp", A_STATE_STOP, false, false); }
        LOG("dino: P%d %s ending (%s)", p + 1, ActName(s.act), why);
        s.act = 5; s.actT = 6.0f; s.actAge = 0; s.wakeLogged = false;
        s.restEdPending = slept && config::Get().wakeInstant && Anim(dino, "rest_ed", A_LOADED, false, false);
    }
    constexpr uintptr_t kActPlayAnimState = 0x004AFC10, kActUpdateDelta = 0x004B28C0;
    constexpr uintptr_t kTerrainPtr = 0x0085006C, kIatIsWater = 0x007395E0;   // AParkManager::m_pSimJPTerrain, ASimJPTerrain::IsWater(uint, uint)
    bool WaterNear(void* dino, const float* pos, const float* dir, float* wx, float* wz) {
        void* t = *reinterpret_cast<void**>(game::At(kTerrainPtr));
        if (!t) return false;
        auto isWater = *reinterpret_cast<bool(__fastcall**)(void*, void*, unsigned, unsigned)>(game::At(kIatIsWater));
        float rad = 0.0f;
        GUARDED("drink radius", { rad = reinterpret_cast<float(__fastcall*)(void*, void*, int)>((*reinterpret_cast<void***>(dino))[0xB8 / 4])(dino, nullptr, 9); });
        if (!(rad > 0.5f && rad < 40.0f)) rad = 3.0f;
        float fx = dir[0], fz = dir[1]; const float fl = sqrtf(fx * fx + fz * fz);
        if (fl > 0.01f) { fx /= fl; fz /= fl; } else { fx = 0; fz = 1; }
        bool hit = false;
        GUARDED("drink water test", {
            const float rings[3] = { 0.0f, rad + 2.0f, rad + 6.0f };
            for (int r = 0; r < 3 && !hit; ++r) {
                const int n = r ? 12 : 1;
                for (int i = 0; i < n && !hit; ++i) {
                    float a = 6.2831853f * i / n;
                    // ahead of the dino first (its head), then all around
                    float dx = r ? (fx * cosf(a) - fz * sinf(a)) : 0, dz = r ? (fz * cosf(a) + fx * sinf(a)) : 0;
                    float x = pos[0] + dx * rings[r], z = pos[2] + dz * rings[r];
                    if (x < 0 || z < 0) continue;
                    if (isWater(t, nullptr, (unsigned)(x + 0.5f), (unsigned)(z + 0.5f))) { hit = true; *wx = x; *wz = z; }
                }
            }
        });
        return hit;
    }
    bool StartAnimAction(int p, void* dino, int act, const char* list, bool loop, float maxT) {
        Slot& s = g_s[p];
        char nm[24];
        if (!PickAnim(dino, list, nm, sizeof nm)) {
            LOG("dino: P%d %s - this dinosaur has none of %s", p + 1, ActName(act), list);
            return false;
        }
        StopMotion(dino);
        BlendOff(s, dino);
        bool ok = Anim(dino, nm, A_PLAY, loop, false);
        s.act = act; s.actT = maxT; s.actAge = 0;
        strncpy(s.anim, nm, sizeof s.anim - 1); s.anim[sizeof s.anim - 1] = 0;
        LOG("dino: P%d %s -> anim '%s' loop=%d ok=%d (auto movement blend off)", p + 1, ActName(act), nm, loop, ok);
        return ok;
    }

    // Finishing move: the game's paired kill animations (GenFSM\SpecAttk.fsm plays attacker "lcarn_<x>Atk01" and
    // victim "lcarn_<y>Dth01" with lock-last-pose, then kills the victim). Pairs in the data (.inx): T-rex vs
    // Tricera "Tr", T-rex vs Anky "An" (anky victim: lcarn_dth01), T-rex "Sp" <-> Spino "Tx", Steg "St".
    bool FindFinisher(void* a, void* v, char* aAnim, char* vAnim) {
        static const char* codes[] = { "Tr", "An", "St", "Sp", "Tx" };
        char nm[24], nv[24];
        for (const char* c : codes) {                    // 1. same code on both
            snprintf(nm, sizeof nm, "lcarn_%sAtk01", c); snprintf(nv, sizeof nv, "lcarn_%sDth01", c);
            if (Anim(a, nm, A_LOADED, false, false) && Anim(v, nv, A_LOADED, false, false)) { strcpy(aAnim, nm); strcpy(vAnim, nv); return true; }
        }
        const char* order[] = { "Sp", "Tx", "Tr", "An", "St" }; // 2. cross pair (T-rex <-> Spino)
        bool fa = false, fv = false;
        for (const char* c : order) if (!fa) { snprintf(nm, sizeof nm, "lcarn_%sAtk01", c); fa = Anim(a, nm, A_LOADED, false, false); }
        for (const char* c : order) if (!fv) { snprintf(nv, sizeof nv, "lcarn_%sDth01", c); fv = Anim(v, nv, A_LOADED, false, false); }
        if (!fv) { strcpy(nv, "lcarn_dth01"); fv = Anim(v, nv, A_LOADED, false, false); }
        if (fa && fv) { strcpy(aAnim, nm); strcpy(vAnim, nv); return true; }
        return false;
    }
    bool TryGrapple(int p, void* dino, const float* pos, void* victim, bool kill = true);
    bool TryGrappleNow(int p, void* dino, const float* pos, void* victim, bool kill);
    void StartFinisherAnims(int p, void* dino, void* victim, float d);
    bool TryFinisher(int p, void* dino, const float* pos, void* victim, bool forced) {
        Slot& s = g_s[p];
        const Config& c = config::Get();
        float h = Health(victim);
        // JPOG37 (owner bug 7): only on a victim with LOW health (finish_health_low, default 60; JPOG32-36 used 250 =
        // most dinos at full health got the finisher attempt).
        if (!forced && h >= c.dinoFinishLow) return false;   // JPOG38: RB = forced (any health)
        if (g_ctl[1 - p] && g_s[1 - p].dino == victim) return false;   // never on the other player's dino
        if (Anim(dino, "grapple_lp", A_LOADED, false, false) && Anim(dino, "leap_st", A_LOADED, false, false))
            return TryGrapple(p, dino, pos, victim, true);
        char aa[24], va[24];
        if (!FindFinisher(dino, victim, aa, va)) {
            LOG("dino: P%d finishing move - no paired kill animation for these two (victim health %.1f)", p + 1, h);
            return false;
        }
        void* vp = *reinterpret_cast<void**>((uint8_t*)victim + kUnitProxy);
        if (!vp) return false;
        ++*reinterpret_cast<int*>((uint8_t*)vp + 4);
        s.vic = victim; s.vicProxy = vp;
        float vpos[3];
        if (!DinoPos(victim, vpos)) { DropVictim(s, false); return false; }
        FreezeAI(victim);
        StopMotion(dino); StopMotion(victim);
        BlendOff(s, dino);
        strncpy(s.anim, aa, sizeof s.anim - 1); s.anim[sizeof s.anim - 1] = 0;
        strncpy(s.vicAnim, va, sizeof s.vicAnim - 1); s.vicAnim[sizeof s.vicAnim - 1] = 0;
        const float dx = pos[0] - vpos[0], dz = pos[2] - vpos[2], d = sqrtf(dx * dx + dz * dz);
        if (c.dinoFinisherAlign > 0) {
            float req = 0, perf = 0;
            FinisherDistance(aa, va, &req, &perf);
            float want = (c.dinoFinisherAlign == 2 ? req - 0.25f : perf) + c.dinoFinisherDistAdd;
            if (want < 1.0f) want = 1.0f;
            // attacker moves along the line it came from (vanilla: SeekTarget / StepBackFromTarget on that line)
            float nx = 0.0f, nz = 1.0f;
            if (d > 0.01f) { nx = dx / d; nz = dz / d; }
            s.alKind = 0; s.alKill = true;
            s.alFrom[0] = pos[0]; s.alFrom[1] = pos[1]; s.alFrom[2] = pos[2];
            s.alTo[0] = vpos[0] + nx * want; s.alTo[1] = pos[1]; s.alTo[2] = vpos[2] + nz * want;
            const float move = fabsf(d - want);
            s.alDur = move < 0.05f ? 0.0f : (move / 8.0f < 0.12f ? 0.12f : (move / 8.0f > 0.6f ? 0.6f : move / 8.0f));
            s.alT = 0; s.alDist0 = d; s.alWant = want;
            s.act = 13; s.actT = 3.0f; s.actAge = 0;
            FaceDirInstant(dino, -nx, -nz); FaceDirInstant(victim, nx, nz);
            LOG("dino: P%d finisher align: '%s' + victim '%s' - distance %.2f m, vanilla req %.2f perfect %.3f -> %.2f m (slide %.2f s, mode %d)",
                p + 1, aa, va, d, req, perf, want, s.alDur, c.dinoFinisherAlign);
            padrumble::Pulse(p, 0.6f, 0.2f);
            return true;
        }
        FaceTo(dino, pos, vpos); FaceTo(victim, vpos, pos);
        StartFinisherAnims(p, dino, victim, d);
        return true;
    }
    // Both anims in the same frame (SpecAttk.fsm StartFatalAttack / StartFatalDeath).
    void StartFinisherAnims(int p, void* dino, void* victim, float d) {
        Slot& s = g_s[p];
        const bool align = config::Get().dinoFinisherAlign > 0;
        if (align) { KillInterp(dino, true, "attacker"); KillInterp(victim, true, "victim"); s.interp = config::Get().dinoFinisherInterp; }
        bool ok1 = Anim(dino, s.anim, A_PLAY, false, false);
        bool ok2 = Anim(victim, s.vicAnim, A_PLAY, false, true);
        s.act = 6; s.actT = 12.0f; s.actAge = 0;
        s.locatorFix = align;
        LOG("dino: P%d FINISHING MOVE on %p (health %.1f, dist %.2f): '%s' + victim '%s' ok=%d/%d",
            p + 1, victim, Health(victim), d, s.anim, s.vicAnim, ok1, ok2);
        padrumble::Pulse(p, 1.0f, 0.5f);
    }
    // act 13: slide the attacker to the vanilla distance, both facing each other, then start the paired anims / latch.
    bool TickAlign(int p, void* dino, float dt) {
        Slot& s = g_s[p];
        void* v = s.vic;
        const bool vok = v && s.vicProxy && *reinterpret_cast<void**>((uint8_t*)s.vicProxy + 0x14) == v && !Gone(v) && !Dead(v);
        if (!vok) {
            LOG("dino: P%d %s line-up - victim gone", p + 1, s.alKind ? "pounce" : "finisher");
            if (s.alKind == 1) { SetTarget(dino, nullptr); s.targetSet = false; }
            DropVictim(s, true); s.act = 0; s.anim[0] = 0; BlendOn(s, dino);
            return false;
        }
        FreezeAI(v); StopMotion(v); StopMotion(dino);
        s.alT += dt;
        const float t = s.alDur > 0.0f ? (s.alT / s.alDur > 1.0f ? 1.0f : s.alT / s.alDur) : 1.0f;
        const float k = t * t * (3.0f - 2.0f * t);   // smoothstep
        float cur[3];
        if (!DinoPos(dino, cur)) cur[1] = s.alFrom[1];
        float np[3] = { s.alFrom[0] + (s.alTo[0] - s.alFrom[0]) * k, cur[1], s.alFrom[2] + (s.alTo[2] - s.alFrom[2]) * k };
        PlaceAt(dino, np);
        float vpos[3];
        if (DinoPos(v, vpos)) { FaceDirInstant(dino, vpos[0] - np[0], vpos[2] - np[2]); FaceDirInstant(v, np[0] - vpos[0], np[2] - vpos[2]); }
        if (t < 1.0f && s.actT > 0) return true;
        float a[3], b[3], d = -1.0f;
        if (DinoPos(dino, a) && DinoPos(v, b)) d = sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[2] - b[2]) * (a[2] - b[2]));
        if (s.alKind == 0) {
            LOG("dino: P%d finisher align done: distance %.2f -> %.2f m (wanted %.2f)", p + 1, s.alDist0, d, s.alWant);
            StartFinisherAnims(p, dino, v, d);
            return true;
        }
        // raptor: the vanilla latch (RaptHunt.fsm LatchCheck) at the right gap; the victim ref moves to TryGrapple's own
        void* victim = v;
        DropVictim(s, false);
        LOG("dino: P%d pounce align done: centre distance %.2f -> %.2f m (gap wanted %.2f)", p + 1, s.alDist0, d, config::Get().dinoPounceGap);
        s.act = 0;
        float pos[3]; DinoPos(dino, pos);
        if (!TryGrappleNow(p, dino, pos, victim, s.alKill)) {
            BlendOn(s, dino);
            LOG("dino: P%d pounce - latch refused after the line-up", p + 1);
            return false;
        }
        return true;
    }

    // JPOG36 (owner bugs 3 + 4): RT = a bite EVERY time (JPOG32-35 only played it when a victim was in range - the
    // "T-Rex bite doesn't play"); held RT = the next bite as soon as the last one ends (blend stays off in between,
    // no flicker). Bite anims per species from Data\Units\*.inx: snap_md_lp / snap_lo_lp (carnivores), lherb_atk01/02
    // (Pachy, Toro), atk_vis01 (Raptor), atk_stuc_lt (Brach, Camara), tail_def (Anky, Kentro, Steg).
    // JPOG37 (owner bug 7): atk_vis01 is the Raptor's ONLY normal attack in the data (RaptAtk.fsm uses it on every prey),
    // so it is no longer in the general list (Raptor = fallback below) - and it moves the raptor forward (root motion):
    // the script ends it with SetTransfromFromLocator("D", true) (else the raptor snaps back = the "glitch").
    // JPOG43 (owner request 2026-11-06 13:09): herbivores that defend themselves attack too. From the .inx files:
    // Tricera + Styrac have a head-butt (hbut_*), Tricera's own anti-carnivore charge lcarn_tratk01 is the fallback.
    // Steg/Anky/Kentro already used tail_def, Pachy/Toro lherb_atk01/02. PickAnim takes the first anim LOADED for the model.
    // JPOG44 (owner bug 13:14): Brach/Camara stomp never played - the .inx sound events say "atk_stuc_lt" (typo), but
    // the game's own script (BracStrs.fsm) plays PlayAnim("atk", "struc_lt") = atk_struc_lt. Brach fallback: spcl_gen_ru
    // (rear up, Large_impact on both hands in brach.inx).
    const char* kBiteList = "snap_md_lp|snap_lo_lp|lherb_atk01|lherb_atk02|atk_struc_lt|atk_stuc_lt|tail_def|hbut_lp|hbut_st|hbut_ed|lcarn_tratk01|spcl_gen_ru";
    bool TryGrab(int p, void* dino, const float* pos);
    void EndBiteAnim(Slot& s, void* dino) {
        if (s.locatorFix) {
            bool moved = Act(dino, kActSetTransformFromLocator, "attack: transform from locator D", "sb", "D", nullptr, 1);
            if (s.locLogs < 3) { s.locLogs++; LOG("dino: P%d attack '%s' ended - moved to locator D ok=%d", (int)(&s - g_s) + 1, s.anim, moved); }
            s.locatorFix = false;
        }
        if (s.anim[0]) Anim(dino, s.anim, A_STOP, false, false);
    }
    // JPOG69 R4 (owner 16:47): a PLAYER's dino breaks fences easily (AI dinos keep the vanilla rules). The game's own FSM
    // actions, as a library (the fence-break scripts): target = the fence's wall unit (what AActiveUnit::AcquireFence
    // does, without its "dino strength >= fence class" refusal), DamageFence 0x4B75E0 (amount, sparks, use-size = 0)
    // with a huge amount -> the section's health hits 0 and its "intact" flag 0x100 clears, then RamFence 0x4B6240
    // (false) = the vanilla break: sound, "fence broken" popup, particles, park events, AWallManager::BreakWallSection.
    // Closest intact section: AWallManager::GetClosestWall 0x5E4B60 (pos, radius, intact only, false) on
    // AParkManager::m_pWallManager 0x8500A8; node +0x0C / +0x14 = x / z; AWallUnitNode::GetWallUnit 0x5E2990 (fastcall).
    // Triggered by bites (RT) that hit no dino: [dino] fence_bites (5) bites on the same section break it (owner 17:09: never
    // by walking into it).
    DWORD g_fenceT[2] = { 0, 0 }; int s_fenceFarLogs = 0; unsigned s_fenceBreaks = 0;
    void* g_fenceNode[2] = { nullptr, nullptr }; int g_fenceBites[2] = { 0, 0 }; DWORD g_fenceBiteT[2] = { 0, 0 };
    bool TryBreakFence(int p, void* dino, const float* pos, float dirx, float dirz, const char* how) {
        const Config& c = config::Get();
        if (!c.dinoFenceBreak || !dino) return false;
        const DWORD now = GetTickCount();
        if (now - g_fenceT[p] < 700) return false;
        void* wm = *reinterpret_cast<void**>(game::At(0x008500A8));
        if (!wm) return false;
        float rad = 0.0f;
        GUARDED("fence: own radius", { rad = reinterpret_cast<float(__fastcall*)(void*, void*, int)>((*reinterpret_cast<void***>(dino))[0xB8 / 4])(dino, nullptr, 9); });
        if (!(rad > 0.0f && rad < 40.0f)) rad = 3.0f;
        const float reach = rad + c.dinoFenceReach;
        void* node = nullptr;
        GUARDED("fence: closest wall", {
            node = reinterpret_cast<void*(__fastcall*)(void*, void*, const float*, int, int, int)>(game::At(0x005E4B60))(wm, nullptr, pos, (int)(reach + 1.0f), 1, 0); });
        if (!node) return false;
        float wx = 0, wz = 0;
        GUARDED("fence: wall position", { wx = *reinterpret_cast<float*>((uint8_t*)node + 0x0C); wz = *reinterpret_cast<float*>((uint8_t*)node + 0x14); });
        const float dx = wx - pos[0], dz = wz - pos[2], d = sqrtf(dx * dx + dz * dz);
        const float dl = sqrtf(dirx * dirx + dirz * dirz);
        const float cosA = (d > 0.01f && dl > 1e-4f) ? (dx * dirx + dz * dirz) / (d * dl) : 1.0f;
        if (d > reach || (cosA < 0.2f && d > rad * 0.7f)) {
            if (s_fenceFarLogs++ < 4) LOG("dino: P%d fence section at %.1f m (reach %.1f, angle cos %.2f) - not in front, left alone", p + 1, d, reach, cosA);
            return false;
        }
        void* wu = nullptr;
        GUARDED("fence: wall unit", { wu = reinterpret_cast<void*(__fastcall*)(void*)>(game::At(0x005E2990))(node); });
        if (!wu || !*reinterpret_cast<void**>((uint8_t*)wu + kUnitProxy)) { LOG("dino: P%d fence section %p has no wall unit - can't break it", p + 1, node); return false; }
        // Owner 17:09: NOT by walking into it - only after ~5 bites on the same fence ([dino] fence_bites). Each bite counts
        // one (sparks + rumble, hint "Fence: 3 / 5"); a different section or > 20 s between bites starts again.
        if (g_fenceNode[p] != node || now - g_fenceBiteT[p] > 20000) { g_fenceNode[p] = node; g_fenceBites[p] = 0; }
        g_fenceBiteT[p] = now;
        const int need = c.dinoFenceBites < 1 ? 1 : c.dinoFenceBites;
        if (++g_fenceBites[p] < need) {
            LOG("dino: P%d bites the fence (%d / %d) - section %p at %.1f m", p + 1, g_fenceBites[p], need, node, d);
            wchar_t h[64]; swprintf(h, 64, L"Fence: %d / %d bites", g_fenceBites[p], need);
            gamectl::RequestHint(h, 1.5f); splitscreen::HintToHalf(p, 1.5f);
            padrumble::Pulse(p, 0.5f, 0.15f);
            return true;
        }
        g_fenceBites[p] = 0; g_fenceNode[p] = nullptr;
        g_fenceT[p] = now;
        SetTarget(dino, wu);
        const bool dmg = Act(dino, 0x004B75E0, "fence: DamageFence", "fb", nullptr, nullptr, 1, 1000000.0f);
        const bool ram = Act(dino, 0x004B6240, "fence: RamFence", "b", nullptr, nullptr, 0);
        SetTarget(dino, nullptr);
        s_fenceBreaks++;
        LOG("dino: P%d BREAKS the fence (%s) - section %p at %.1f m, wall unit %p: DamageFence ok=%d, RamFence ok=%d (%u so far)",
            p + 1, how, node, d, wu, dmg, ram, s_fenceBreaks);
        padrumble::Pulse(p, 0.8f, 0.3f);
        return true;
    }
    // JPOG73 (owner #10): buildings break like fences - [dino] building_bites (5) bites on the same building. Damage =
    // AUnitBuilding::HurtUnit 0x4C5530 (thiscall float, type, AUnit*; ret 0xC): amount < 0 = its full max health
    // (component 0 +0x3C health, static data +0x08 max). Each bite takes max / bites; at 0 the game closes it as
    // damaged (player 1 gets "Repair?", mechanics fix it). Nearest building whose BODY is within reach (the proximity
    // check counts the unit's size, so this measures from the building's edge).
    constexpr uintptr_t kBuildingCls = 0x008B7A60, kBuildingHurt = 0x004C5530, kGetComponent = 0x004E0BA0, kGetStaticData = 0x004F3DB0;
    void* g_bldU[2] = { nullptr, nullptr }; int g_bldBites[2] = { 0, 0 }; DWORD g_bldT[2] = { 0, 0 };
    bool TryBiteBuilding(int p, void* dino, const float* pos) {
        const Config& c = config::Get();
        if (c.dinoBuildingBites <= 0 || !dino) return false;
        float rad = 0.0f;
        GUARDED("building: own radius", { rad = reinterpret_cast<float(__fastcall*)(void*, void*, int)>((*reinterpret_cast<void***>(dino))[0xB8 / 4])(dino, nullptr, 9); });
        if (!(rad > 0.0f && rad < 40.0f)) rad = 3.0f;
        void* b = dinocontrol::NearestUnitOf(pos, rad + c.dinoFenceReach, kBuildingCls);
        if (!b) return false;
        int hp = -1, maxHp = -1;
        GUARDED("building: health", {
            void* comp = reinterpret_cast<void*(__fastcall*)(void*, void*, int)>(game::At(kGetComponent))(b, nullptr, 0);
            if (comp) {
                hp = *reinterpret_cast<int*>((uint8_t*)comp + 0x3C);
                void* sd = reinterpret_cast<void*(__fastcall*)(void*, void*)>(game::At(kGetStaticData))(comp, nullptr);
                if (sd) maxHp = *reinterpret_cast<int*>((uint8_t*)sd + 0x08);
            } });
        if (maxHp <= 0 || hp <= 0) { LOG("dino: P%d bites building %p - no health left (%d / %d)", p + 1, b, hp, maxHp); return false; }
        const DWORD now = GetTickCount();
        if (g_bldU[p] != b || now - g_bldT[p] > 20000) { g_bldU[p] = b; g_bldBites[p] = 0; }
        g_bldT[p] = now;
        const int need = c.dinoBuildingBites;
        const bool last = ++g_bldBites[p] >= need;
        const float dmg = last ? -1.0f : (float)(maxHp / need + 1);
        bool ok = GUARDED("building: hurt", {
            reinterpret_cast<void(__fastcall*)(void*, void*, float, int, void*)>(game::At(kBuildingHurt))(b, nullptr, dmg, 3, dino); });
        int after = -1;
        GUARDED("building: health after", {
            void* comp = reinterpret_cast<void*(__fastcall*)(void*, void*, int)>(game::At(kGetComponent))(b, nullptr, 0);
            if (comp) after = *reinterpret_cast<int*>((uint8_t*)comp + 0x3C); });
        LOG("dino: P%d bites building %p (%d / %d): health %d -> %d of %d, HurtUnit ok=%d", p + 1, b, g_bldBites[p], need, hp, after, maxHp, ok);
        wchar_t h[64];
        if (last || after == 0) { swprintf(h, 64, L"Building wrecked!"); g_bldBites[p] = 0; g_bldU[p] = nullptr; padrumble::Pulse(p, 0.9f, 0.4f); }
        else { swprintf(h, 64, L"Building: %d / %d bites", g_bldBites[p], need); padrumble::Pulse(p, 0.5f, 0.15f); }
        gamectl::RequestHint(h, 1.5f); splitscreen::HintToHalf(p, 1.5f);
        return true;
    }
    void StartBite(int p, void* dino, const float* pos) {
        Slot& s = g_s[p];
        const Config& c = config::Get();
        float d2 = 0;
        void* victim = FindTarget(dino, pos, &d2);
        // JPOG69 R7 (owner 16:55: "still blood when they bite the air"): the proximity check counts a dino whose BODY
        // reaches into the range (T-rex "attacks" a Brachiosaurus 32 m away, centre to centre) - the game then plays the
        // bite's blood although the mouth hits nothing. [dino] bite_front_only: a bite only hits a dino in FRONT of the
        // attacker (inside +-70 deg of its facing) whose centre is within the attack range + its own radius.
        if (victim && c.dinoBiteFrontOnly) {
            float vp[3], fx = 0, fz = 0, vr = 0;
            void* st = *reinterpret_cast<void**>((uint8_t*)dino + kUnitSteer);
            bool ok = DinoPos(victim, vp) && st;
            if (ok) GUARDED("bite facing", {
                fx = *reinterpret_cast<float*>((uint8_t*)st + 0x70); fz = *reinterpret_cast<float*>((uint8_t*)st + 0x74);
                vr = reinterpret_cast<float(__fastcall*)(void*, void*, int)>((*reinterpret_cast<void***>(victim))[0xB8 / 4])(victim, nullptr, 9); });
            if (!(vr > 0.0f && vr < 60.0f)) vr = 0.0f;
            const float dx = vp[0] - pos[0], dz = vp[2] - pos[2], d = sqrtf(dx * dx + dz * dz), fl = sqrtf(fx * fx + fz * fz);
            const float cosA = (d > 0.01f && fl > 1e-4f) ? (dx * fx + dz * fz) / (d * fl) : 1.0f;
            // JPOG73 (owner #10): reach measured to the victim EDGE (its whole radius, was capped at 4 m) + [dino] bite_extra
            const float reach = g_lastRange + (vr < 20.0f ? vr : 20.0f) + c.dinoBiteExtra;
            if (!ok || cosA < 0.34f || d > reach) {
                if (s.missLogs++ < 6) LOG("dino: P%d bite - %p at %.1f m (reach %.1f, angle cos %.2f) is not in front of the mouth: a miss, no blood, no damage",
                                          p + 1, victim, d, reach, cosA);
                victim = nullptr;
            }
        }
        s.biteHit = victim != nullptr;
        if (!victim) {   // JPOG69 R4: a bite at a fence breaks it
            void* st = *reinterpret_cast<void**>((uint8_t*)dino + kUnitSteer);
            float fx = 0, fz = 0;
            if (st) GUARDED("fence bite facing", { fx = *reinterpret_cast<float*>((uint8_t*)st + 0x70); fz = *reinterpret_cast<float*>((uint8_t*)st + 0x74); });
            if (!TryBreakFence(p, dino, pos, fx, fz, "bite") && !TryBiteBuilding(p, dino, pos)) {
                // JPOG73 (owner #10 "invisible walls"): a bite that hits nothing - log the nearest dino's distance against
                // both radii, so the next session sees how far the steering keeps the rider away.
                if (s.missLogs++ < 12) {
                    float np[3]; void* nd = dinocontrol::NearestDino(pos, 30.0f, np);
                    if (nd) {
                        float mr = 0, vr = 0;
                        GUARDED("miss radii", {
                            mr = reinterpret_cast<float(__fastcall*)(void*, void*, int)>((*reinterpret_cast<void***>(dino))[0xB8 / 4])(dino, nullptr, 9);
                            vr = reinterpret_cast<float(__fastcall*)(void*, void*, int)>((*reinterpret_cast<void***>(nd))[0xB8 / 4])(nd, nullptr, 9); });
                        const float cd = sqrtf((np[0]-pos[0])*(np[0]-pos[0]) + (np[2]-pos[2])*(np[2]-pos[2]));
                        LOG("dino: P%d bite hit NOTHING - nearest dino %p centre %.1f m, own radius %.1f, its radius %.1f, gap %.1f m (range %.1f)",
                            p + 1, nd, cd, mr, vr, cd - mr - vr, g_lastRange);
                    }
                }
            }
        }

        if (victim && c.dinoActions && TryFinisher(p, dino, pos, victim)) return;
        if (c.dinoActions && TryGrab(p, dino, pos)) return;
        char bite[24]{};
        bool have = c.dinoActions && PickAnim(dino, kBiteList, bite, sizeof bite);
        bool raptor = false;
        if (!have && c.dinoActions && Anim(dino, "atk_vis01", A_LOADED, false, false)) { strcpy(bite, "atk_vis01"); have = raptor = true; }
        StopMotion(dino);
        // JPOG37 (owner bug 2): no FaceTo any more - spamming RT turned the dino to whichever target was nearest.
        if (have) {
            BlendOff(s, dino);
            if (s.act == 7) EndBiteAnim(s, dino);
            // JPOG44 (owner bug 2): every attack except the carnivores' looping snap_* walks the dino forward (root motion);
            // the game's scripts end them with SetTransfromFromLocator("D", true) (PchyTrty/ToroTrty.fsm, like RaptAtk.fsm).
            // Keep the end pose, copy locator D into the unit position at the end - no snap back. 4 s safety cut (was 2 s,
            // which cut the anim before the fix could run).
            const bool rootMotion = raptor || strncmp(bite, "snap_", 5) != 0;
            bool ok = Anim(dino, bite, A_PLAY, false, rootMotion);   // keep the end pose until the locator fix
            s.locatorFix = rootMotion;
            s.act = 7; s.actT = rootMotion ? 4.0f : 2.0f; s.actAge = 0;
            strncpy(s.anim, bite, sizeof s.anim - 1); s.anim[sizeof s.anim - 1] = 0;
            if (s.bites++ < 4 || (s.bites % 50) == 0) LOG("dino: P%d bite #%u -> anim '%s' ok=%d, %s", p + 1, s.bites, bite, ok, victim ? "target in range" : "nobody in range");
        } else LOG("dino: P%d bite - no bite anim (%s)%s", p + 1, kBiteList, victim ? "" : ", nobody in range");
        if (victim) HurtTarget(p, dino, victim, d2);
    }

    // ---- JPOG37 (owner bug 6): big carnivore + RT next to a small animal (goat, cow) = grab + swallow ----
    // GenFSM\CarnGrab.fsm: CanGrabTarget(false) + IsTargetSmall -> PreGrabTarget, StopLook, PlayAnim grab st ->
    // (grab st playing) CanGrabTarget(true) -> GrabTarget, victim "cap st", 4-8 s -> (grab shake_lp playing) stop shake ->
    // swal_ed, victim cap swld_ed, SwallowScaleTarget(0.2, 0.7) while swal_ed plays -> CarnEatTarget(true),
    // ReleaseTarget, DestroyTargetUnit; clean-up StopAnimState(grab) + ReleaseTarget. Species with grab_st / grab_shake_lp /
    // grab_swal_ed (Data\Units\*.inx): Trex, Spino, Allo, Acro, Alberto, Carcha.
    void* NearestFood(const float* pos, float radius, float* d2) {
        void* mgr = *reinterpret_cast<void**>(game::At(kUnitManager));
        if (!mgr) return nullptr;
        Nearest n{ nullptr, nullptr, 1e30f, 0, 0, true, nullptr };
        GUARDED("find food animal", {
            reinterpret_cast<fn_prox>(game::At(kProximityCheck))(mgr, nullptr, pos, radius, game::At(kFoodClass), &NearestCb, &n); });
        if (d2) *d2 = n.bestDistSq;
        return n.best;
    }
    // JPOG40 (owner bug 1): X-eat only played anims, so the meat stayed. On "X again -> swallow" do what the game's
    // CarnGrab path does to its food: target = the meat, CarnEatTarget(true) (C_Hngr.fsm ChewWait eats with it),
    // DestroyTargetUnit. Meat = nearest food unit (dead or alive) in grab range, else the nearest dead dino (carcass).
    bool __stdcall MeatCb(void* unit, float distSq, const float* /*pos*/, void* user) {
        Nearest* n = static_cast<Nearest*>(user);
        ++n->seen;
        if (!unit || unit == n->self || Gone(unit) || Controlled(unit)) { ++n->skipped; return false; }
        if (n->cls && !Dead(unit)) { ++n->skipped; return false; }      // dinosaur class: carcasses only
        if (distSq < n->bestDistSq) { n->bestDistSq = distSq; n->best = unit; }
        return false;
    }
    void EatMeat(int p, void* dino, const float* pos) {
        void* mgr = *reinterpret_cast<void**>(game::At(kUnitManager));
        if (!mgr || !dino || !pos) return;
        const float r = config::Get().dinoGrabRange;
        Nearest n{ dino, nullptr, 1e30f, 0, 0, true, nullptr };
        GUARDED("find meat", {
            reinterpret_cast<fn_prox>(game::At(kProximityCheck))(mgr, nullptr, pos, r, game::At(kFoodClass), &MeatCb, &n); });
        const char* kind = "food";
        if (!n.best) {
            n = Nearest{ dino, nullptr, 1e30f, 0, 0, true, game::At(kDinosaurClass) };
            GUARDED("find carcass", {
                reinterpret_cast<fn_prox>(game::At(kProximityCheck))(mgr, nullptr, pos, r, game::At(kDinosaurClass), &MeatCb, &n); });
            kind = "carcass";
        }
        if (!n.best) { LOG("dino: P%d swallow - no meat / carcass within %.1f m (nothing removed)", p + 1, r); return; }
        void* meat = n.best;
        if (!SetTarget(dino, meat)) { LOG("dino: P%d swallow - could not target the %s %p", p + 1, kind, meat); return; }
        bool e = Act(dino, kActCarnEat, "eat: eat meat", "b", nullptr, nullptr, 1);
        bool d = Act(dino, kActDestroyTarget, "eat: destroy meat");
        SetTarget(dino, nullptr);
        LOG("dino: P%d ate the %s %p (%.1f m): CarnEatTarget ok=%d, DestroyTargetUnit ok=%d", p + 1, kind, meat, sqrtf(n.bestDistSq), e, d);
    }
    void GrabCleanup(Slot& s, void* dino, bool destroyed) {
        if (dino) {
            Anim(dino, "grab_st", A_STATE_STOP, false, false);
            if (!destroyed && s.targetSet) {
                if (s.human) Act(dino, kActDetachTarget, "grab person: detach");   // GrabPpl.fsm exit: DetachTarget
                else Act(dino, kActRelease, "grab: release target");
            }
            SetTarget(dino, nullptr);
        }
        s.targetSet = false; s.phase = 0; s.human = false;
    }
    bool __stdcall HumanCb(void* unit, float distSq, const float*, void* user) {
        Nearest* n = static_cast<Nearest*>(user);
        ++n->seen;
        if (!unit || unit == n->self || Gone(unit) || !(*((uint8_t*)unit + kUnitFlags) & 0x10)) { ++n->skipped; return false; }
        if (distSq < n->bestDistSq) { n->bestDistSq = distSq; n->best = unit; }
        return false;
    }
    void* NearestHuman(void* dino, const float* pos, float radius, float* d2) {
        void* mgr = *reinterpret_cast<void**>(game::At(kUnitManager));
        if (!mgr) return nullptr;
        Nearest n{ dino, nullptr, 1e30f, 0, 0, false, nullptr };
        GUARDED("find person", {
            reinterpret_cast<fn_prox>(game::At(kProximityCheck))(mgr, nullptr, pos, radius, game::At(kUnitClass), &HumanCb, &n); });
        if (d2) *d2 = n.bestDistSq;
        return n.best;
    }
    bool TryGrab(int p, void* dino, const float* pos) {
        Slot& s = g_s[p];
        if (!Anim(dino, "grab_st", A_LOADED, false, false)) return false;
        float d2 = 0;
        void* food = NearestFood(pos, config::Get().dinoGrabRange, &d2);
        s.human = false;
        if (!food && config::Get().dinoEatPeople) {
            float h2 = 0;
            void* person = NearestHuman(dino, pos, config::Get().dinoGrabRange, &h2);
            if (person) {
                SetTarget(dino, person); s.targetSet = true;
                if (!Act(dino, kActCanGrab, "grab person: can grab", "b", nullptr, nullptr, 0)) {
                    LOG("dino: P%d person %p at %.1f m - the game says it can't be grabbed (inside a building / too big) - normal bite", p + 1, person, sqrtf(h2));
                    SetTarget(dino, nullptr); s.targetSet = false;
                    return false;
                }
                StopMotion(dino);
                BlendOff(s, dino);
                Act(dino, kActStopLook, "grab person: stop look");
                bool ok = Anim(dino, "grab_st", A_PLAY, false, false);
                strcpy(s.anim, "grab_st");
                s.human = true;
                s.act = 10; s.phase = 0; s.actT = 20.0f; s.actAge = 0; s.phaseT = 3.0f;
                LOG("dino: P%d GRAB PERSON %p (%.1f m): 'grab_st' ok=%d (GrabPpl.fsm)", p + 1, person, sqrtf(h2), ok);
                padrumble::Pulse(p, 0.8f, 0.3f);
                return true;
            }
        }
        if (!food) return false;
        SetTarget(dino, food); s.targetSet = true;
        if (!Act(dino, kActCanGrab, "grab: can grab", "b", nullptr, nullptr, 0)) {
            LOG("dino: P%d grab - the game says this animal can't be grabbed (%p, %.1f m) - normal bite", p + 1, food, sqrtf(d2));
            GrabCleanup(s, dino, false);
            return false;
        }
        bool pre = Act(dino, kActPreGrab, "grab: pre-grab");
        StopMotion(dino);
        BlendOff(s, dino);
        Act(dino, kActStopLook, "grab: stop look");
        bool ok = Anim(dino, "grab_st", A_PLAY, false, false);
        strcpy(s.anim, "grab_st");
        s.act = 10; s.phase = 0; s.actT = 20.0f; s.actAge = 0; s.phaseT = 3.0f;
        LOG("dino: P%d GRAB %p (%.1f m): pre-grab ok=%d, 'grab_st' ok=%d (CarnGrab.fsm)", p + 1, food, sqrtf(d2), pre, ok);
        padrumble::Pulse(p, 0.8f, 0.3f);
        return true;
    }
    void* TargetUnit(void* unit) {
        void* px = *reinterpret_cast<void**>((uint8_t*)unit + kUnitTarget);
        return px ? *reinterpret_cast<void**>((uint8_t*)px + 0x14) : nullptr;
    }
    bool TickGrab(int p, void* dino, float dt) {
        Slot& s = g_s[p];
        s.phaseT -= dt;
        void* food = TargetUnit(dino);
        auto finish = [&](const char* why, bool destroyed) {
            LOG("dino: P%d grab finished (%s) after %.1f s", p + 1, why, s.actAge);
            GrabCleanup(s, dino, destroyed);
            s.act = 0; s.anim[0] = 0; BlendOn(s, dino);
            return false;
        };
        if (s.actT <= 0) return finish("timeout", false);
        switch (s.phase) {
        case 0:   // grab st playing -> grab
            if (!food) return finish("target gone", false);
            if (s.actAge > 0.1f && Anim(dino, "grab_st", A_PLAYING, false, false)) {
                if (!Act(dino, kActCanGrab, "grab: can grab (in reach)", "b", nullptr, nullptr, 1)) {
                    if (s.phaseT <= 0) return finish("never in reach", false);
                    return true;
                }
                bool g;
                if (s.human) {   // GrabPpl.fsm GrabCheck: AttachTarget, KillHuman, PlayAnimTarget("cap", "st")
                    g = Act(dino, kActAttachTarget, "grab person: attach");
                    bool k = Act(dino, kActKillHuman, "grab person: kill human");
                    LOG("dino: P%d person attached ok=%d, KillHuman ok=%d", p + 1, g, k);
                } else g = Act(dino, kActGrab, "grab: grab target");
                bool cap = Anim(food, "cap_st", A_LOADED, false, false) && Anim(food, "cap_st", A_PLAY, false, false);
                LOG("dino: P%d grabbed %p ok=%d (victim 'cap_st' %d)", p + 1, food, g, cap);
                if (!g) return finish("grab refused", false);
                s.phase = 1; s.phaseT = 5.0f;
            } else if (s.actAge > 1.5f && !Anim(dino, "grab_st", A_STATE_PLAYING, false, false)) return finish("grab anim did not play", false);
            return true;
        case 1:   // shaking 5 s, then swallow
            if (s.phaseT <= 0 && (Anim(dino, "grab_shake_lp", A_PLAYING, false, false) || s.phaseT < -3.0f)) {
                Anim(dino, "grab_shake_lp", A_STOP, false, false);
                bool sw = Anim(dino, "grab_swal_ed", A_PLAY, false, true);
                strcpy(s.anim, "grab_swal_ed");
                if (food && Anim(food, "cap_swld_ed", A_LOADED, false, false)) { Anim(food, "cap_shke_lp", A_STOP, false, false); Anim(food, "cap_swld_ed", A_PLAY, false, true); }
                LOG("dino: P%d swallowing ('grab_swal_ed' ok=%d)", p + 1, sw);
                s.phase = 2; s.phaseT = 8.0f; s.actAge = s.actAge;
            }
            return true;
        case 2: { // swal_ed: shrink the victim into the mouth, then eat + destroy it
            const bool sw = Anim(dino, "grab_swal_ed", A_PLAYING, false, false);
            if (sw && food) Act(dino, kActSwallowScale, "grab: swallow scale", "ff", nullptr, nullptr, 0, 0.2f, 0.7f);
            if ((!sw && s.phaseT < 7.5f) || s.phaseT <= 0) {
                bool e = Act(dino, kActCarnEat, "grab: eat target", "b", nullptr, nullptr, 1);
                bool r = s.human ? Act(dino, kActDetachTarget, "grab person: detach") : Act(dino, kActRelease, "grab: release");
                bool d = Act(dino, kActDestroyTarget, "grab: destroy target");
                LOG("dino: P%d ate the %s (eat %d, release %d, destroy %d)", p + 1, s.human ? "PERSON" : "animal", e, r, d);
                padrumble::Pulse(p, 0.6f, 0.4f);
                return finish("swallowed", true);
            }
            return true;
        }
        }
        return finish("?", false);
    }

    // ---- JPOG37 (owner bug 7): Raptor finishing move = the game's pounce + grapple (RaptHunt.fsm), only on a weak victim ----
    // LatchCheck: LatchOnTarget() -> StopAllUserAnimations, blend off, PlayAnim leap st (auto-link off) -> leap lp ->
    // (leap state ended) grapple lp looping 3-5 s with HurtTargetUnit -> StopAnimState(grapple), UnLatchFromTarget,
    // hit lt -> getup lt -> blend on.
    void GrappleCleanup(Slot& s, void* dino) {
        if (dino) {
            if (s.latched) { bool u = Act(dino, kActUnLatch, "grapple: unlatch"); LOG("dino: raptor unlatched ok=%d", u); }
            Anim(dino, "grapple_lp", A_STATE_STOP, false, false);
            SetTarget(dino, nullptr);
        }
        s.latched = false; s.targetSet = false;
        DropVictim(s, true);
    }
    // JPOG65: vanilla only latches when the gap (centre distance - both collision radii, InTargetProximityRadius
    // 0x4B39E0) is <= 3.5 m (RaptHunt.fsm). Further away: line up first (act 13), then latch.
    bool TryGrapple(int p, void* dino, const float* pos, void* victim, bool kill) {
        Slot& s = g_s[p];
        const Config& c = config::Get();
        float vpos[3];
        if (c.dinoFinisherAlign > 0 && DinoPos(victim, vpos)) {
            const float dx = pos[0] - vpos[0], dz = pos[2] - vpos[2], d = sqrtf(dx * dx + dz * dz);
            const float rr = SteerRadius(dino) + SteerRadius(victim);
            const float gap = d - rr;
            if (gap > c.dinoPounceGap + 0.25f) {
                void* vp = *reinterpret_cast<void**>((uint8_t*)victim + kUnitProxy);
                if (!vp) return false;
                ++*reinterpret_cast<int*>((uint8_t*)vp + 4);
                s.vic = victim; s.vicProxy = vp;
                FreezeAI(victim); StopMotion(dino); StopMotion(victim); BlendOff(s, dino);
                float nx = 0.0f, nz = 1.0f;
                if (d > 0.01f) { nx = dx / d; nz = dz / d; }
                const float want = rr + c.dinoPounceGap;
                s.alKind = 1; s.alKill = kill;
                s.alFrom[0] = pos[0]; s.alFrom[1] = pos[1]; s.alFrom[2] = pos[2];
                s.alTo[0] = vpos[0] + nx * want; s.alTo[1] = pos[1]; s.alTo[2] = vpos[2] + nz * want;
                const float move = d - want;
                s.alDur = move / 10.0f < 0.12f ? 0.12f : (move / 10.0f > 0.6f ? 0.6f : move / 10.0f);
                s.alT = 0; s.alDist0 = d; s.alWant = want;
                s.anim[0] = 0; s.act = 13; s.actT = 3.0f; s.actAge = 0;
                LOG("dino: P%d pounce align: centre distance %.2f m, radii %.2f, gap %.2f > %.2f -> moving to %.2f m (slide %.2f s)",
                    p + 1, d, rr, gap, c.dinoPounceGap, want, s.alDur);
                return true;
            }
            LOG("dino: P%d pounce: gap %.2f m (centre %.2f, radii %.2f) - close enough, latch now", p + 1, gap, d, rr);
        }
        return TryGrappleNow(p, dino, pos, victim, kill);
    }
    bool TryGrappleNow(int p, void* dino, const float* pos, void* victim, bool kill) {
        Slot& s = g_s[p];
        s.grappleKill = kill;
        void* vp = *reinterpret_cast<void**>((uint8_t*)victim + kUnitProxy);
        if (!vp) return false;
        SetTarget(dino, victim); s.targetSet = true;
        StopMotion(dino); StopMotion(victim);
        BlendOff(s, dino);
        Act(dino, kActStopLook, "grapple: stop look");
        s.latched = Act(dino, kActLatch, "grapple: latch on target");
        if (!s.latched) {
            LOG("dino: P%d raptor grapple - LatchOnTarget refused (no free locator on %p) - normal attack", p + 1, victim);
            SetTarget(dino, nullptr); s.targetSet = false;
            return false;
        }
        ++*reinterpret_cast<int*>((uint8_t*)vp + 4);
        s.vic = victim; s.vicProxy = vp;
        FreezeAI(victim);
        Act(dino, kActAutoLinkOff, "grapple: auto link off", "ssb", "leap", "st", 1);
        bool ok = Anim(dino, "leap_st", A_PLAY, false, false);
        strcpy(s.anim, "leap_st");
        s.act = 11; s.phase = 0; s.actT = 15.0f; s.actAge = 0; s.phaseT = 0; s.hurtT = 0;
        LOG("dino: P%d RAPTOR GRAPPLE on %p (health %.1f): latched, 'leap_st' ok=%d (RaptHunt.fsm)", p + 1, victim, Health(victim), ok);
        padrumble::Pulse(p, 1.0f, 0.4f);
        (void)pos;
        return true;
    }
    bool TickGrapple(int p, void* dino, float dt) {
        Slot& s = g_s[p];
        void* v = s.vic;
        const bool vok = v && s.vicProxy && *reinterpret_cast<void**>((uint8_t*)s.vicProxy + 0x14) == v && !Gone(v);
        s.phaseT -= dt;
        auto done = [&](const char* why) {
            LOG("dino: P%d raptor grapple finished (%s) after %.1f s", p + 1, why, s.actAge);
            GrappleCleanup(s, dino);
            s.act = 0; s.anim[0] = 0; s.phase = 0; BlendOn(s, dino);
            return false;
        };
        if (s.actT <= 0) return done("timeout");
        if (vok) FreezeAI(v);
        switch (s.phase) {
        case 0:   // leap st -> leap lp
            if (s.actAge > 0.2f && !Anim(dino, "leap_st", A_PLAYING, false, false)) {
                Act(dino, kActAutoLinkOff, "grapple: auto link off lp", "ssb", "leap", "lp", 1);
                Anim(dino, "leap_lp", A_PLAY, false, false); strcpy(s.anim, "leap_lp");
                s.phase = 1; s.phaseT = 3.0f;
            }
            return true;
        case 1:   // leap state ends -> grapple loop on the victim
            if ((!Anim(dino, "leap_lp", A_STATE_PLAYING, false, false) && s.phaseT < 2.8f) || s.phaseT <= 0) {
                bool att = Act(dino, kActIsAttached, "grapple: is attached");
                Anim(dino, "grapple_lp", A_PLAY, true, false); strcpy(s.anim, "grapple_lp");
                s.phase = 2; s.phaseT = 4.0f; s.hurtT = 0;
                LOG("dino: P%d grapple loop (attached=%d)", p + 1, att);
            }
            return true;
        case 2: { // grapple: hurt the victim every second, kill at the end
            s.hurtT -= dt;
            if (vok && s.hurtT <= 0) {
                s.hurtT = 1.0f;
                float h = Health(v), dmg = h * 0.5f < 15.0f ? 15.0f : h * 0.5f;
                GUARDED("grapple hurt", { reinterpret_cast<fn_hurt>(game::At(kHurtUnit))(v, nullptr, dmg, 3, dino); });
                padrumble::Pulse(p, 0.6f, 0.15f);
            }
            if (!vok || Dead(v) || s.phaseT <= 0) {
                if (vok && !Dead(v) && s.grappleKill) {
                    float before = Health(v);
                    GUARDED("grapple kill", { reinterpret_cast<fn_hurt>(game::At(kHurtUnit))(v, nullptr, before + 100.0f, 3, dino); });
                    LOG("dino: P%d grapple kill - victim health %.1f -> %.1f dead=%d", p + 1, before, Health(v), Dead(v));
                }
                Anim(dino, "grapple_lp", A_STATE_STOP, false, false);
                if (s.latched) { bool u = Act(dino, kActUnLatch, "grapple: unlatch"); s.latched = false; LOG("dino: P%d raptor unlatched ok=%d", p + 1, u); }
                Anim(dino, "hit_lt", A_PLAY, false, true); strcpy(s.anim, "hit_lt");
                s.phase = 3; s.phaseT = 3.0f;
            }
            return true;
        }
        case 3:   // fall off -> get up
            if ((s.phaseT < 2.8f && !Anim(dino, "hit_lt", A_PLAYING, false, false)) || s.phaseT <= 0) {
                Anim(dino, "hit_lt", A_STOP, false, false);
                Anim(dino, "getup_lt", A_PLAY, false, true); strcpy(s.anim, "getup_lt");
                s.phase = 4; s.phaseT = 3.0f;
            }
            return true;
        case 4:
            if ((s.phaseT < 2.8f && !Anim(dino, "getup_lt", A_PLAYING, false, false)) || s.phaseT <= 0) {
                Anim(dino, "getup_lt", A_STOP, false, false);
                return done("got up");
            }
            return true;
        }
        return done("?");
    }

    // ---- JPOG37 (owner bug 9): "Zzz" over a sleeping player dino ----
    // AActiveUnit::GetIconType 0x4A6BE0 picks the icon from unit+0xA5 (current behaviour = drive id; +0xA4 = requested,
    // AUnitBehaviour::SetBehaviour switches only when they differ). The sleep id comes from the game itself
    // (ADrivesEnumeration::GetDriveEnum("Sleep") 0x612CD0). While our dino sleeps both bytes = that id (no switch, its
    // script is paused anyway); the old values come back when it wakes up / is let go.
    int g_sleepId = -2, g_idleId = -2;
    int DriveId(const char* name) {
        int id = -1;
        GUARDED("drive id", { id = reinterpret_cast<int(__stdcall*)(const char*)>(game::At(0x00612CD0))(name) & 0xFF; });
        return id;
    }
    void LookupDriveIds() {
        if (g_sleepId == -2) {
            g_sleepId = DriveId("Sleep");
            LOG("dino: drive id of \"Sleep\" = %d (ADrivesEnumeration::GetDriveEnum)", g_sleepId);
        }
        // JPOG40 (owner bug 3): an awake id for a dino taken while asleep / woken by us after it was taken asleep -
        // otherwise +0xA4/+0xA5 stay on Sleep and the "Zzz" stays over an awake dino. First known name wins.
        if (g_idleId == -2) {
            g_idleId = -1;
            const char* names[] = { "Idle", "Wander", "Play" };
            for (const char* n : names) {
                int id = DriveId(n);
                if (id > 0 && id <= 40 && id != g_sleepId) { g_idleId = id; LOG("dino: awake drive id = %d (\"%s\")", id, n); break; }
            }
            if (g_idleId < 0) LOG("dino: no awake drive id found (Idle / Wander / Play)");
        }
    }
    void ZzzOn(Slot& s, int p, void* dino) {
        if (s.zzz || !dino) return;
        LookupDriveIds();
        if (g_sleepId <= 0 || g_sleepId > 40) return;
        uint8_t* b = (uint8_t*)dino;
        s.savedA4 = b[0xA4]; s.savedA5 = b[0xA5]; s.zzz = true;
        b[0xA4] = b[0xA5] = (uint8_t)g_sleepId;
        LOG("dino: P%d asleep - sleep icon on (behaviour id %d/%d -> %d)", p + 1, s.savedA4, s.savedA5, g_sleepId);
    }
    void ZzzKeep(Slot& s, void* dino) {
        if (s.zzz && dino) { uint8_t* b = (uint8_t*)dino; b[0xA4] = b[0xA5] = (uint8_t)g_sleepId; }
    }
    void ZzzOff(Slot& s, int p, void* dino) {
        if (!s.zzz) return;
        s.zzz = false;
        // JPOG40: a dino taken while asleep had Sleep saved - give it the awake id instead (else the "Zzz" stays).
        if (g_idleId > 0 && s.savedA4 == g_sleepId) s.savedA4 = (uint8_t)g_idleId;
        if (g_idleId > 0 && s.savedA5 == g_sleepId) s.savedA5 = (uint8_t)g_idleId;
        if (dino) { uint8_t* b = (uint8_t*)dino; b[0xA4] = s.savedA4; b[0xA5] = s.savedA5; }
        LOG("dino: P%d awake - sleep icon off (behaviour id back to %d/%d)", p + 1, s.savedA4, s.savedA5);
    }

    // JPOG40 (owner bug 3): taking control of a dino the GAME put to sleep left it in the sleeping pose and the stick slid
    // it along the ground. Wake it the way our own LB wake-up does: StopAnimState(sleep / rest) lets the engine play the
    // get-up, act 5 blocks the stick until the state has stopped, then the auto movement blend goes back on (the game's
    // sleep FSM switched it off - C_Slp.fsm / H_Slp.fsm SetAutoMovementBlend(false)); behaviour id -> awake (icon off).
    void WakeIfAsleep(int p, void* dino) {
        Slot& s = g_s[p];
        if (!dino) return;
        LookupDriveIds();
        uint8_t* b = (uint8_t*)dino;
        const bool byDrive = g_sleepId > 0 && (b[0xA5] == g_sleepId || b[0xA4] == g_sleepId);
        const bool slp = Anim(dino, "sleep_lp", A_STATE_PLAYING, false, false);
        const bool rst = Anim(dino, "rest_lp", A_STATE_PLAYING, false, false);
        if (!byDrive && !slp && !rst) return;
        if (slp) Anim(dino, "sleep_lp", A_STATE_STOP, false, false);
        if (rst) Anim(dino, "rest_lp", A_STATE_STOP, false, false);
        if (g_idleId > 0) { if (b[0xA4] == g_sleepId) b[0xA4] = (uint8_t)g_idleId; if (b[0xA5] == g_sleepId) b[0xA5] = (uint8_t)g_idleId; }
        s.blendOff = true;            // BlendOn at the end of act 5 switches the game's blend back on
        s.act = 5; s.actT = 6.0f; s.actAge = 0; s.wakeLogged = false; s.anim[0] = 0; s.fam[0] = 0;
        // JPOG57 (owner 12:05 "a sleeping dino you take must get up right away - it slides when you walk"): plant-eaters
        // lie in the REST pose after sleep_ed; vanilla then plays rest_ed (H_Slp.fsm SleepCleanUpWait). Done in act 5.
        // [dino] wake_instant: also a 3 s cap instead of 6 s, then the stick is free in any case.
        if (config::Get().wakeInstant) { s.actT = 3.0f; s.restEdPending = Anim(dino, "rest_ed", A_LOADED, false, false); }
        LOG("dino: P%d took a sleeping dino - waking it up first (sleep state %d, rest state %d, drive sleep %d -> id %d/%d); stick blocked until it stands",
            p + 1, slp, rst, byDrive, b[0xA4], b[0xA5]);
    }

    // Returns true when the stick must not move the dino this frame.
    bool TickAction(int p, void* dino, const float* pos, float dt, XPad& pad) {
        Slot& s = g_s[p];
        const Config& c = config::Get();
        const bool stick = fabsf(pad.LX()) > 0.3f || fabsf(pad.LY()) > 0.3f;
        const bool rt = pad.RT() > 0.5f;
        const bool asleep = s.act == 3 || s.act == 4 || s.act == 8;
        s.attackCd -= dt;
        // RT: bite (not while asleep / waking / in a finishing move; an active bite re-bites in case 7)
        if (rt && s.attackCd <= 0.0f && (s.act == 0 || s.act == 1 || s.act == 2)) {
            if (s.act) StopAction(p, dino, "bite");
            s.attackCd = c.dinoAttackCooldown;
            StartBite(p, dino, pos);
        }
        // JPOG38 (owner 21:58): separate buttons. RB = finishing move on the nearest dino in range at ANY health (RT still
        // does it by itself on a weak victim). LT = pounce (species with leap_st, e.g. Raptor): leap + latch + grapple on
        // the nearest dino (kills only a weak one), nobody in range = a leap forward.
        const bool canAct = s.act == 0 || s.act == 1 || s.act == 2;
        const bool lt = pad.LT() > 0.5f, ltPress = lt && !s.prevLT;
        s.prevLT = lt;
        // JPOG69 R2 (owner 16:47 + answer 16:5x: "RB turns right, LB stays sleep"): RB HELD = the dino turns right on the
        // spot (every species, both players; [dino] rb_turn_speed deg/s, rb_turn_invert test switch). RB TAPPED (released
        // within rb_tap_time) = the finishing move as before, now on release. [dino] rb_turn = 0 = JPOG68 (RB press = finisher).
        bool rbFinish = false;
        // JPOG73 (owner 13:29 #6): LB held = turn LEFT exactly like RB held turns right (same tap time / speed); sleep
        // moved to R3 (right stick click, unused while riding). [dino] lb_turn = 0 -> LB = sleep again (old layout).
        auto turnHold = [&](PadButton btn, float sign, float& t, bool& turning, const char* name, const char* dir) -> bool {
            bool tap = false;
            if (pad.Pressed(btn)) { t = 0.0f; turning = false; }
            if (pad.Down(btn)) {
                t += dt;
                if (t >= c.dinoRbTapTime && canAct && !stick) {
                    void* st = Steering(dino);
                    float fx = 0, fz = 0;
                    if (st) GUARDED("rb turn facing", {
                        fx = *reinterpret_cast<float*>((uint8_t*)st + 0x70); fz = *reinterpret_cast<float*>((uint8_t*)st + 0x74); });
                    const float fl = sqrtf(fx * fx + fz * fz);
                    if (fl > 1e-4f) {
                        fx /= fl; fz /= fl;
                        // the dino's right = (fz, -fx): the game camera's row 0 (screen right) for a camera looking along the facing
                        const float a = c.dinoRbTurnSpeed * 3.14159265f / 180.0f * dt * c.dinoRbTurnSign * sign;
                        const float ca = cosf(a), sa = sinf(a);
                        StopMotion(dino);
                        FaceDirInstant(dino, ca * fx + sa * fz, ca * fz - sa * fx);
                        s.holdValid = false;
                        // JPOG77: the next forward push walks the NEW way; the split chase camera turns with the dino
                        // (facing angle atan2(fx, fz) grows by a, the chase yaw uses the same convention)
                        if (c.dinoStickLock) { s.turnedSinceLock = true; s.headLock = false; if (s.split) s.yaw += a; }
                        if (!turning) { turning = true; LOG("dino: P%d %s held - turning %s (%.0f deg/s)", p + 1, name, dir, c.dinoRbTurnSpeed); }
                    }
                }
            }
            if (pad.Released(btn)) {
                if (t < c.dinoRbTapTime) tap = true;
                else if (turning) LOG("dino: P%d %s released - turn done (%.1f s)", p + 1, name, t);
                turning = false;
            }
            return tap;
        };
        if (c.dinoRbTurn) rbFinish = turnHold(PAD_RB, 1.0f, s.rbT, s.rbTurning, "RB", "right");
        else rbFinish = pad.Pressed(PAD_RB);
        if (c.dinoLbTurn) (void)turnHold(PAD_LB, -1.0f, s.lbT, s.lbTurning, "LB", "left");
        if (c.dinoActions && canAct && rbFinish) {
            float d2 = 0; void* v = FindTarget(dino, pos, &d2);
            if (!v) LOG("dino: P%d RB finishing move - nobody in range", p + 1);
            else {
                if (s.act) StopAction(p, dino, "finishing move");
                if (!TryFinisher(p, dino, pos, v, true))
                    LOG("dino: P%d RB finishing move - none for these two (no paired kill anim / latch refused) - victim %p health %.1f", p + 1, v, Health(v));
            }
        }
        if (c.dinoActions && ltPress && (s.act == 0 || s.act == 1 || s.act == 2)) {   // (re-checked: RB may have started one)
            if (!Anim(dino, "leap_st", A_LOADED, false, false)) {
                // JPOG55 (backlog 17): no pounce -> the species' special move (Brachiosaurus rear-up spcl_gen_ru, as its
                // NPC behaviour; other spcl_* / post anims if the model has them). First loaded name wins, logged.
                if (s.act) StopAction(p, dino, "special move");
                if (!StartAnimAction(p, dino, 1, "spcl_gen_ru|spcl_ru|spcl_gen_01|spcl_01|post_lp|post_st|spcl_gen", false, 6.0f))
                    LOG("dino: P%d LT - this dinosaur has no pounce and no special move anim", p + 1);
            } else {
                if (s.act) StopAction(p, dino, "pounce");
                float d2 = 0; void* v = FindTarget(dino, pos, &d2);
                const bool weak = v && Health(v) < c.dinoFinishLow;
                bool ok = v && !(g_ctl[1 - p] && g_s[1 - p].dino == v) && Anim(dino, "grapple_lp", A_LOADED, false, false) &&
                          TryGrapple(p, dino, pos, v, weak);
                if (!ok) {   // a leap forward on its own
                    StopMotion(dino); BlendOff(s, dino);
                    bool pl = Anim(dino, "leap_st", A_PLAY, false, false);
                    strcpy(s.anim, "leap_st"); s.act = 7; s.actT = 2.0f; s.actAge = 0; s.locatorFix = false;
                    s.attackCd = c.dinoAttackCooldown;
                    LOG("dino: P%d LT pounce - leap ('leap_st' ok=%d)%s", p + 1, pl, v ? " (grapple not possible)" : ", nobody in range");
                    if (v) HurtTarget(p, dino, v, d2);
                } else LOG("dino: P%d LT pounce on %p (health %.1f, kill only if weak: %d)", p + 1, v, Health(v), weak);
            }
        }
        // JPOG59 (open list 8 / B16): A = drink, only next to water (H_Thrs.fsm IsInWaterArea / DDrnk.fsm): the terrain's
        // own water test (ASimJPTerrain::IsWater, rounded world x/z like AActiveUnit::IsInWaterArea) at the dino and in a
        // ring around its head; then the script's own PlayAnimState("drink", true) (drink st -> lp, the engine links them),
        // A again / the stick / 8 s = StopAnimState (drink ed), UpdateDeltaVariable("Water", 1.0) like FinishedDrinking.
        if (c.dinoActions && c.dinoDrink && pad.Pressed(PAD_A)) {
            if (s.act == 12) EndAction(p, dino, "A again");
            else if (s.act == 0 || s.act == 1 || s.act == 2) {
                float wx = 0, wz = 0;
                if (!WaterNear(dino, pos, s.lastDir, &wx, &wz)) {
                    LOG("dino: P%d A drink - no water near (%.0f %.0f)", p + 1, pos[0], pos[2]);
                    gamectl::RequestHint(L"No water here - walk to a lake or river to drink", 2.5f);
                    if (s.split) splitscreen::HintToHalf(p, 2.5f);
                } else {
                    if (s.act) StopAction(p, dino, "drink");
                    StopMotion(dino); BlendOff(s, dino);
                    bool ok = Act(dino, kActPlayAnimState, "drink: PlayAnimState", "sb", "drink", nullptr, 1);
                    if (ok) { strcpy(s.anim, "drink_lp"); s.act = 12; s.actT = 8.0f; s.actAge = 0; }
                    else ok = StartAnimAction(p, dino, 12, "drink_st|drink_lp", false, 8.0f);
                    LOG("dino: P%d A drink at water (%.0f %.0f) ok=%d", p + 1, wx, wz, ok);
                    if (!ok) BlendOn(s, dino);
                }
            }
        }
        if (c.dinoActions) {
            // JPOG55 (backlog 20): tap Y = first call, hold Y (0.4 s) = second call (call_02 / call_f), for every species
            // that has them; a species with one call plays it either way.
            const PadButton sleepBtn = c.dinoLbTurn ? PAD_RS : PAD_LB;   // JPOG73: sleep = R3 (right stick click)
            if (pad.Pressed(PAD_Y) && (s.act == 0 || s.act == 2)) s.yT = 0.0f;
            const bool yHold = s.yT >= 0.0f && pad.Down(PAD_Y) && (s.yT += dt) >= 0.4f;
            const bool yTap  = s.yT >= 0.0f && !pad.Down(PAD_Y);
            if (yHold || yTap) s.yT = -1.0f;
            if ((yHold || yTap) && (s.act == 0 || s.act == 2)) {             // roar
                StopAction(p, dino, "roar");
                StartAnimAction(p, dino, 1, yHold ? "call_02_lp|call_f|call_01_lp" : "call_01_lp|call_02_lp|call_f", false, 6.0f);
                LOG("dino: P%d Y %s -> call", p + 1, yHold ? "held" : "tapped");
            } else if (pad.Pressed(PAD_X) && s.act == 2) {                      // eat off = swallow (JPOG37 owner bug 6)
                if (Anim(dino, "idle_chew_lp", A_LOADED, false, false)) {
                    Anim(dino, s.anim, A_STATE_STOP, false, false);
                    bool ok = Anim(dino, "idle_chew_lp", A_PLAY, false, false);
                    strcpy(s.anim, "idle_chew_lp"); s.act = 9; s.actT = 4.0f; s.actAge = 0;
                    LOG("dino: P%d X again -> swallow ('idle_chew_lp' ok=%d, C_Hngr.fsm)", p + 1, ok);
                    EatMeat(p, dino, pos);   // JPOG40 (owner bug 1): the meat disappears
                } else EndAction(p, dino, "X");
            } else if (pad.Pressed(PAD_X) && (s.act == 0 || s.act == 1)) {      // eat on
                StopAction(p, dino, "eat");
                StartAnimAction(p, dino, 2, "eat_lo_lp|eat_md_lp|eat_hi_lp", true, 600.0f);
            } else if (pad.Pressed(sleepBtn) && asleep) {                        // wake up
                EndAction(p, dino, c.dinoLbTurn ? "R3" : "LB");
            } else if (pad.Pressed(sleepBtn) && (s.act == 0 || s.act == 1 || s.act == 2)) {   // go to sleep (game's chain)
                StopAction(p, dino, "sleep");
                // C_Slp.fsm (carnivores): PlayAnim("sleep","st") -> the engine links to sleep lp. H_Slp.fsm (herbivores):
                // rest st -> rest lp -> StopAnimState(rest) -> sleep st -> sleep lp.
                const bool rest = Anim(dino, "rest_st", A_LOADED, false, false);
                // JPOG37 (owner bug 8): C_Slp.fsm StopLook()s before lying down - the head/eye look-at layer kept the
                // eyes open over the sleep anim (AActiveUnit::StopLook 0x4B1E10: AAnimatableObject::SeekEnd + ClearLookFSM).
                bool sl = Act(dino, kActStopLook, "sleep: stop look");
                LOG("dino: P%d sleep - StopLook ok=%d", p + 1, sl);
                if (StartAnimAction(p, dino, 3, rest ? "rest_st" : "sleep_st", false, 8.0f))
                    strcpy(s.fam, rest ? "rest" : "sleep");
                else if (StartAnimAction(p, dino, 4, "sleep_lp|idle_snz_lp|rest_lp", true, 600.0f))
                    strcpy(s.fam, "sleep");
            }
        }
        if (!s.act) return false;
        s.actAge += dt; s.actT -= dt;
        const bool playing = s.actAge < 0.3f || (s.anim[0] && Anim(dino, s.anim, A_PLAYING, false, false));
        switch (s.act) {
        case 1:   // roar: stand until it ends
            if (!playing || s.actT <= 0) { LOG("dino: P%d roar done", p + 1); s.act = 0; s.anim[0] = 0; BlendOn(s, dino); return false; }
            return true;
        case 2:   // eat: loops until X again (graceful) or the stick moves (at once)
            if (stick) { StopAction(p, dino, "moved"); return false; }
            return true;
        case 7:   // bite
            if (!playing || s.actT <= 0) {
                if (s.locatorFix) EndBiteAnim(s, dino);
                if (rt && s.attackCd <= 0.0f) { s.attackCd = c.dinoAttackCooldown; StartBite(p, dino, pos); return true; }
                if (rt) return true;                       // held: wait for the cooldown, keep the pose
                s.act = 0; s.anim[0] = 0; BlendOn(s, dino);
                return false;
            }
            return true;
        case 9:   // swallow (chew) after eating
            if (!playing || s.actT <= 0) { LOG("dino: P%d swallowed", p + 1); s.act = 0; s.anim[0] = 0; BlendOn(s, dino); return false; }
            return true;
        case 12:  // drink (JPOG59): until A again, the stick or 8 s; the engine plays drink_ed on StopAnimState
            if (stick || s.actT <= 0) {
                if (s.actAge > 2.0f) Act(dino, kActUpdateDelta, "drink: water +1", "sf", "Water", nullptr, 0, 1.0f);
                EndAction(p, dino, stick ? "moved" : "drank");
            }
            return true;
        case 10: return TickGrab(p, dino, dt);
        case 11: return TickGrapple(p, dino, dt);
        case 13: return TickAlign(p, dino, dt);
        case 3: {  // lying down (st)
            if (stick) { EndAction(p, dino, "moved"); return true; }
            const bool rest = strcmp(s.fam, "rest") == 0;
            const char* lp = rest ? "rest_lp" : "sleep_lp";
            if (s.actAge > 0.3f && Anim(dino, lp, A_PLAYING, false, false)) {
                if (rest && Anim(dino, "sleep_st", A_LOADED, false, false)) {
                    Anim(dino, "rest_lp", A_STATE_STOP, false, false);   // H_Slp.fsm RestWait
                    s.act = 8; s.actT = 6.0f; s.actAge = 0;
                    LOG("dino: P%d resting -> going to sleep", p + 1);
                } else { s.act = 4; s.actT = 600.0f; LOG("dino: P%d asleep ('%s' looping)", p + 1, lp); }
            } else if (!playing || s.actT <= 0) {
                // no linked loop: start one ourselves (or hold the last pose)
                char nm[24];
                if (PickAnim(dino, rest ? "rest_lp|sleep_lp|idle_snz_lp" : "sleep_lp|idle_snz_lp|rest_lp", nm, sizeof nm)) {
                    Anim(dino, nm, A_PLAY, true, false); strncpy(s.anim, nm, sizeof s.anim - 1);
                }
                s.act = 4; s.actT = 600.0f; s.actAge = 1.0f;
                LOG("dino: P%d asleep (started '%s' ourselves)", p + 1, s.anim);
            }
            return true;
        }
        case 8:   // herbivore: rest -> sleep
            if (stick) { EndAction(p, dino, "moved"); return true; }
            if ((s.actAge > 0.3f && !Anim(dino, "rest_lp", A_STATE_PLAYING, false, false)) || s.actT <= 0) {
                s.act = 0;   // StartAnimAction sets it again
                if (StartAnimAction(p, dino, 3, "sleep_st", false, 8.0f)) strcpy(s.fam, "sleep");
                else { s.act = 4; s.actT = 600.0f; }
            }
            return true;
        case 4:   // asleep until LB again or the stick
            if (stick) { EndAction(p, dino, "moved"); return true; }
            return true;
        case 5: { // ending (get up / stop eating): wait for the anim state to stop, then the locomotion blend back on
            const bool st = Anim(dino, s.anim[0] ? s.anim : "sleep_lp", A_STATE_PLAYING, false, false) ||
                            Anim(dino, "sleep_lp", A_STATE_PLAYING, false, false) || Anim(dino, "rest_lp", A_STATE_PLAYING, false, false);
            if (!s.wakeLogged && s.actAge > 0.4f) {
                s.wakeLogged = true;
                LOG("dino: P%d ending: state playing=%d sleep_ed=%d rest_ed=%d", p + 1, st,
                    Anim(dino, "sleep_ed", A_PLAYING, false, false), Anim(dino, "rest_ed", A_PLAYING, false, false));
            }
            if (s.restEdPending && s.actAge > 0.3f && !st && s.actT > 0) {   // JPOG57: herbivore stands up from rest
                s.restEdPending = false;
                if (Anim(dino, "rest_ed", A_PLAY, false, false)) {
                    strcpy(s.anim, "rest_ed"); s.actAge = 0.0f; s.actT = s.actT < 2.5f ? 2.5f : s.actT;
                    LOG("dino: P%d sleep over - playing rest_ed to stand up (H_Slp.fsm)", p + 1);
                    return true;
                }
            }
            if ((s.actAge > 0.3f && !st) || s.actT <= 0) {
                s.restEdPending = false;
                if (s.actT <= 0) {   // JPOG57: never leave the dino lying down - stop every rest/sleep state now
                    Anim(dino, "sleep_lp", A_STATE_STOP, false, false); Anim(dino, "rest_lp", A_STATE_STOP, false, false);
                    if (s.anim[0]) Anim(dino, s.anim, A_STOP, false, false);
                }
                LOG("dino: P%d action finished after %.1f s%s", p + 1, s.actAge, s.actT <= 0 ? " (timeout)" : "");
                s.act = 0; s.anim[0] = 0; s.fam[0] = 0; BlendOn(s, dino);
                return false;
            }
            return true;
        }
        case 6: {  // finishing move
            void* v = s.vic;
            bool vok = v && s.vicProxy && *reinterpret_cast<void**>((uint8_t*)s.vicProxy + 0x14) == v && !Gone(v);
            if (!vok) { LOG("dino: P%d finishing move - victim gone", p + 1); StopAction(p, dino, "victim gone"); return false; }
            FreezeAI(v);
            if (!playing || s.actT <= 0) {
                // JPOG65: SpecAttk.fsm FatalPosture - interpolation off, avoidance back on, winner to locator "D"
                // (root motion kept, no snap back), then stop the anim.
                if (s.interp) { KillInterp(dino, false, "attacker"); KillInterp(v, false, "victim"); s.interp = false; }
                if (s.locatorFix) {
                    float b4[3]{}, af[3]{};
                    DinoPos(dino, b4);
                    bool moved = Act(dino, kActSetTransformFromLocator, "finisher: transform from locator D", "sb", "D", nullptr, 1);
                    DinoPos(dino, af);
                    LOG("dino: P%d finisher end - moved to locator D ok=%d (%.2f m)", p + 1, moved,
                        sqrtf((af[0] - b4[0]) * (af[0] - b4[0]) + (af[2] - b4[2]) * (af[2] - b4[2])));
                    s.locatorFix = false;
                    if (s.anim[0]) Anim(dino, s.anim, A_STOP, false, false);
                }
                float before = Health(v);
                bool ok = GUARDED("finishing blow", {
                    reinterpret_cast<fn_hurt>(game::At(kHurtUnit))(v, nullptr, before + 100.0f, 3, dino); });
                LOG("dino: P%d finishing move done - victim %p health %.1f -> %.1f dead=%d ok=%d%s",
                    p + 1, v, before, Health(v), Dead(v), ok, s.actT <= 0 ? " (timeout)" : "");
                DropVictim(s, false);
                s.act = 0; s.anim[0] = 0; BlendOn(s, dino);
                padrumble::Pulse(p, 1.0f, 0.6f);
                return false;
            }
            return true;
        }
        }
        return false;
    }

    // JPOG34 (owner bug 3): the game's hint is drawn in the HUD = the builder's half. In split-screen the hint is
    // moved into the rider's own half while it shows (splitscreen::HintToHalf).
    const wchar_t* kSplitControls = L"Dino: LS move (click = run), RS camera, D-pad zoom, RT bite, RB hold = turn right (tap = finishing move), LB hold = turn left, LT pounce / special move, Y roar (hold = 2nd call), X eat, A drink (at water), R3 (right stick click) sleep, B let go";
    // JPOG78: per player (owner 17:12) - a keyboard + mouse rider sees the keyboard riding keys, with the real take / let-go key
    const wchar_t* ControlsText(int p = 0) {
        if (!dinocontrol::PcPlayer(p)) return kSplitControls;
        // player 2 on their own keyboard + mouse (kbm.cpp P2 pad): WASD + mouse, clicks bite / pounce, T or Esc let go
        if (p == 1) {   // JPOG80: run / turn / roar / eat / drink / sleep = the configured [keys] ride_* on player 2's keyboard (MergeKeyboard)
            const Config& c2 = config::Get();
            static wchar_t b2[400]; wchar_t k[6][32];
            dinocontrol::KeyName(c2.rideRun, k[0], L"Right Shift"); dinocontrol::KeyName(c2.rideFinish, k[1], L"H");
            dinocontrol::KeyName(c2.rideRoar, k[2], L"Y"); dinocontrol::KeyName(c2.rideEat, k[3], L"N");
            dinocontrol::KeyName(c2.rideDrink, k[4], L"G"); dinocontrol::KeyName(c2.rideSleep, k[5], L"Z");
            gamectl::WFmt(b2, 400, L"Riding (keyboard + mouse): WASD move, mouse camera, left click bite, middle click pounce / special, %ls run, %ls hold = turn right (tap = finishing move), %ls roar (hold = 2nd call), %ls eat, %ls drink, %ls sleep, T or Esc let go",
                k[0], k[1], k[2], k[3], k[4], k[5]);
            return b2;
        }
        // JPOG80: player 1's keyboard riding keys as configured in [keys] ride_* (names from the keyboard driver)
        const Config& c = config::Get();
        static wchar_t b[400];
        wchar_t k[17][32];
        dinocontrol::KeyName(c.rideFwd, k[0], L"I"); dinocontrol::KeyName(c.rideLeft, k[1], L"J"); dinocontrol::KeyName(c.rideBack, k[2], L"K");
        dinocontrol::KeyName(c.rideRight, k[3], L"L"); dinocontrol::KeyName(c.rideRun, k[4], L"Right Shift"); dinocontrol::KeyName(c.rideBite, k[5], L"U");
        dinocontrol::KeyName(c.rideFinish, k[6], L"H"); dinocontrol::KeyName(c.ridePounce, k[7], L"O"); dinocontrol::KeyName(c.rideRoar, k[8], L"Y");
        dinocontrol::KeyName(c.rideEat, k[9], L"N"); dinocontrol::KeyName(c.rideDrink, k[10], L"G"); dinocontrol::KeyName(c.rideSleep, k[11], L"Z");
        dinocontrol::KeyName(c.rideCamL, k[12], L"Num 4"); dinocontrol::KeyName(c.rideCamR, k[13], L"Num 6");
        wchar_t cu[32], cd[32]; dinocontrol::KeyName(c.rideCamU, cu, L"Num 8"); dinocontrol::KeyName(c.rideCamD, cd, L"Num 2");
        dinocontrol::KeyName(c.rideZoomIn, k[14], L"Num +"); dinocontrol::KeyName(c.rideZoomOut, k[15], L"Num -");
        gamectl::WFmt(b, 400, L"Riding (keyboard): %ls %ls %ls %ls move, %ls run, %ls bite, %ls hold = turn right (tap = finishing move), %ls pounce / special, %ls roar (hold = 2nd call), %ls eat, %ls drink, %ls sleep, %ls %ls %ls %ls camera (one player: Q E R F), %ls / %ls zoom, %ls let go",
            k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], k[8], k[9], k[10], k[11], k[12], k[13], cu, cd, k[14], k[15], dinocontrol::TakeKeyName());
        return b;
    }
    void ShowControls(int p) {
        const float secs = config::Get().riderHintSecs;
        gamectl::RequestHint(ControlsText(p), secs);
        splitscreen::HintToHalf(p, secs);
        LOG("dino: controls hint for player %d shown in their own half (%.0f s)", p + 1, secs);
    }
    // JPOG76 (owner JPOG74 test: "player 1 pressed RB + D-pad Up on the helicopter and PLAYER 2 got it"). The game has ONE
    // HUD (selection + game state + camera); flying takes it. JPOG73 only flew for the HUD player, and after a ride the HUD
    // stays with the other player (15:30:17 "player 2 now controls the cursor/camera"). Now the PRESSER gets it: the HUD is
    // handed to them first (splitscreen::RequestActive), the fly order runs once they own it (2 s give-up line).
    // [vehicle] heli_presser = 0 = JPOG75 behaviour.
    void* g_winU[2] = { nullptr, nullptr };   // last unit whose own window this player opened (p2menu)
    DWORD g_winT[2] = { 0, 0 };
    void* g_heliPend[2] = { nullptr, nullptr };
    DWORD g_heliPendT[2] = { 0, 0 };
    // helicopter itself, or a building -> the nearest helicopter within 40 m (its helipad)
    void* HeliFrom(void* u, int p, const char* src) {
        if (!u || Gone(u)) return nullptr;
        if (gamectl::IsA(u, 0x008B77E0)) return u;
        if (!gamectl::IsA(u, 0x008B7A60)) return nullptr;
        float bp[3]; bool have = false; void* h = nullptr;
        GUARDED("helipad position", { have = DinoPos(u, bp); });
        if (have) h = dinocontrol::NearestUnitOf(bp, 40.0f, 0x008B77E0);
        if (h) LOG("dino: P%d RB + D-pad Up on building %p (%s) - helicopter %p near it", p + 1, u, src, h);
        return h;
    }
    // Why IsOrderAllowed_FlyControlOrder said no. Its body (vtable 0xD00C60 slot 0x2AC = 0x4D28C0, read from SimJP.exe):
    // no if (+0x110 == 0 and +0x160 != 0) / dead (health +0xDC <= 0 or +0x80) / state +0x11C in {5, 0x1C} with +0x163 /
    // +0x111 / +0x162. Plain field reads, logged in both outcomes.
    void LogHeliFlags(int p, void* h, bool allowed) {
        uint8_t* b = (uint8_t*)h;
        void* slot = nullptr; uint8_t f110 = 0, f111 = 0, f160 = 0, f161 = 0, f162 = 0, f163 = 0, f80 = 0; int st = 0; float hp = 0;
        bool ok = GUARDED("helicopter flags", {
            slot = (*reinterpret_cast<void***>(h))[0x2AC / 4];
            f110 = b[0x110]; f111 = b[0x111]; f160 = b[0x160]; f161 = b[0x161]; f162 = b[0x162]; f163 = b[0x163]; f80 = b[0x80];
            st = *reinterpret_cast<int*>(b + 0x11C); hp = *reinterpret_cast<float*>(b + 0xDC); });
        LOG("dino: P%d helicopter %p allowed=%d - check fn %p (expect 004D28C0) +110=%u +160=%u health %.1f +80=%u state(+11C)=%d +163=%u +111=%u +161=%u +162=%u (read ok %d)%s",
            p + 1, h, allowed, slot, f110, f160, hp, f80, st, f163, f111, f161, f162, ok,
            allowed ? "" : (f110 == 0 && f160) ? " -> NO: +160 set (busy / already flown?)" : (hp <= 0 || f80) ? " -> NO: dead / destroyed"
                         : ((st == 5 || st == 0x1C) && f163) ? " -> NO: state 5/0x1C with +163" : f111 ? " -> NO: +111 set" : f162 ? " -> NO: +162 set" : " -> NO: other");
    }
    // Fly now (game thread; player p owns the HUD or there is no split).
    void HeliFlyNow(int p, void* heli) {
        bool allowed = false, ok = false, ran;
        if (void* sel = *reinterpret_cast<void**>(game::At(kUnitSelection))) {
            if (gamectl::SelectedUnit() != heli) {
                bool sok = GUARDED("select the helicopter", {
                    reinterpret_cast<void(__fastcall*)(void*, void*, const void*)>(game::At(kSetSelectedUnit))(sel, nullptr, heli); });
                LOG("dino: P%d helicopter %p made the game's selection (ok=%d)", p + 1, heli, sok);
            }
        }
        ran = GUARDED("helicopter fly order", {
            allowed = reinterpret_cast<bool(__fastcall*)(void*, void*)>(game::At(0x004D3510))(heli, nullptr);
            if (allowed) ok = reinterpret_cast<bool(__fastcall*)(void*, void*)>(game::At(0x004D3500))(heli, nullptr); });
        LOG("dino: P%d RB + D-pad Up -> helicopter %p FLY order: allowed=%d ok=%d ran=%d (HUD player %d, state now vtbl %p)", p + 1, heli, allowed, ok, ran,
            (splitscreen::Active() ? splitscreen::ActivePlayer() : 0) + 1, gamectl::CurrentState() ? *(void**)gamectl::CurrentState() : nullptr);
        LogHeliFlags(p, heli, allowed);
        if (!allowed) gamectl::RequestHintFor(p, L"The helicopter can't take off right now", 3.0f);
    }
    // The presser gets it: hand the HUD over first when another player has it.
    void HeliPress(int p, void* heli) {
        const bool split = splitscreen::CanSwitch();
        const int hud = split ? splitscreen::ActivePlayer() : 0;
        if (!split || hud == p) { HeliFlyNow(p, heli); return; }
        g_heliPend[p] = heli; g_heliPendT[p] = GetTickCount();
        splitscreen::RequestActive(p);
        LOG("dino: P%d pressed RB + D-pad Up on helicopter %p - player %d has the HUD -> handed to player %d first, then FLY (JPOG76 [vehicle] heli_presser)",
            p + 1, heli, hud + 1, p + 1);
    }
    bool HeliAtPick(int p, const float* pt) {
        const Config& c = config::Get();
        if (!c.heliPadFly || !c.heliPresser) return false;
        void* h = dinocontrol::NearestUnitOf(pt, c.pickRadius + 10.0f, 0x008B77E0);
        if (!h) h = HeliFrom(dinocontrol::NearestUnitOf(pt, c.pickRadius + 10.0f, 0x008B7A60), p, "aim");
        if (!h && g_winU[p] && GetTickCount() - g_winT[p] < 60000) h = HeliFrom(g_winU[p], p, "recent own window");
        if (!h) return false;
        HeliPress(p, h);
        return true;
    }
    void HeliPendingTick() {
        for (int p = 0; p < 2; ++p) {
            if (!g_heliPend[p]) continue;
            void* h = g_heliPend[p];
            if (Gone(h)) { LOG("dino: P%d helicopter %p gone before the HUD hand-over", p + 1, h); g_heliPend[p] = nullptr; continue; }
            if (splitscreen::ActivePlayer() == p) { g_heliPend[p] = nullptr; LOG("dino: P%d now has the HUD (%lu ms) - FLY", p + 1, GetTickCount() - g_heliPendT[p]); HeliFlyNow(p, h); }
            else if (GetTickCount() - g_heliPendT[p] > 2000) {
                g_heliPend[p] = nullptr;
                LOG("dino: P%d HUD hand-over for helicopter %p did NOT happen in 2 s (HUD player %d, menu locked %d) - gave up",
                    p + 1, h, splitscreen::ActivePlayer() + 1, splitscreen::MenuLocked());
                gamectl::RequestHintFor(p, L"The helicopter can't take off right now", 3.0f);
            }
        }
    }
    void Begin(int p, void* forced = nullptr) {
        void* ctl = gamectl::Ctl();
        void* st = gamectl::CurrentState();
        if (!ctl || !gamectl::InPark()) return;
        Slot& s = g_s[p];
        // Split-screen (JPOG30): no follow state - the other player keeps the HUD and builds.
        const bool split = splitscreen::CanSwitch();
        if (!split && (g_ctl[0] || g_ctl[1])) { LOG("dino: player %d asked for control - one dino at a time without split-screen", p + 1); return; }
        // JPOG68: this player's own selection (A). The HUD player: the game's selection when it is a dino, else ours.
        const int hudP = split ? splitscreen::ActivePlayer() : 0;
        void* unit = forced;
        // JPOG73 (owner #11 "RB + Up sometimes takes a DIFFERENT dino"): JPOG68 let the game's selection win for the HUD
        // player whenever it was a dino - a stale / redirected game selection then beat the dino the player picked. Now:
        // the player's OWN last selection, unless the game's selection is a dino selected MORE RECENTLY (seen in the
        // SetSelectedUnit hook). Logged every time.
        // JPOG73 (owner #6, helicopter with a controller): the HUD context buttons (take off) can't be pressed with a pad.
        // RB + D-pad Up on a selected HELICOPTER (or its helipad: the nearest helicopter within 40 m) = the game's own
        // "fly" order, AUnitHelicopterCommon::ExecuteOrder_FlyControlOrder 0x4D3500 (thiscall bool, no args), only when
        // IsOrderAllowed_FlyControlOrder 0x4D3510 says yes. [vehicle] heli_pad_fly = 0 = off.
        if (!forced && config::Get().heliPadFly && (p == hudP || config::Get().heliPresser)) {
            void* heli = nullptr;
            if (p == hudP) heli = HeliFrom(gamectl::SelectedUnit(), p, "game selection");
            else if (!SelOf(p) || (g_winU[p] && (LONG)(g_winT[p] - g_selT[p]) > 0)) {   // own dino picked later wins
                // JPOG76: the no-HUD presser - their own building window (last 60 s), else the game's selection when it is a
                // helicopter (their window's "jump to unit" puts it there).
                if (g_winU[p] && GetTickCount() - g_winT[p] < 60000) heli = HeliFrom(g_winU[p], p, "own window");
                if (!heli) { void* g = gamectl::SelectedUnit(); if (g && gamectl::IsA(g, 0x008B77E0)) heli = g; }
            }
            if (heli) { HeliPress(p, heli); return; }
        }
        void* own = forced ? nullptr : SelOf(p);
        void* gsel = nullptr;
        if (!unit && p == hudP) { void* g = gamectl::SelectedUnit(); if (g && gamectl::IsA(g, kDinosaurClass)) gsel = g; }
        if (!unit) {
            const bool gameNewer = gsel && gsel == g_gameSelU && (!own || (LONG)(g_gameSelT - g_selT[p]) > 0);
            unit = own ? (gameNewer ? gsel : own) : gsel;   // no own selection -> the game's (JPOG68 behaviour)
            float aim[3] = { 0, 0, 0 }; const bool hasAim = splitscreen::GameAim(aim);
            LOG("dino: P%d take request: own selection %p (%lu ms ago), game selection %p (hook %p, %lu ms ago), aim %s(%.1f %.1f %.1f) -> unit %p",
                p + 1, own, own ? GetTickCount() - g_selT[p] : 0ul, gsel, (void*)g_gameSelU, g_gameSelT ? GetTickCount() - g_gameSelT : 0ul,
                hasAim ? "" : "n/a ", aim[0], aim[1], aim[2], unit);
        }
        if (!unit || !gamectl::IsA(unit, kDinosaurClass)) {
            LOG("dino: player %d asked for control - no dinosaur selected (unit %p)", p + 1, unit);
            gamectl::RequestHintFor(p, dinocontrol::HintText(splitscreen::Active() ? p : -1, L"Select a dinosaur first: aim at it and press A, then RB + D-pad Up", L"Select a dinosaur first: click it, then press %ls"), 4.0f);
            return;
        }
        if (g_ctl[1 - p] && g_s[1 - p].dino == unit) {
            LOG("dino: player %d asked for dino %p - player %d already controls it", p + 1, unit, 2 - p);
            gamectl::RequestHintFor(p, dinocontrol::PcPlayer(p) ? (p ? L"Player 1 controls that dinosaur - click another one" : L"Player 2 controls that dinosaur - click another one")
                                             : (p ? L"Player 1 controls that dinosaur - select another one (aim + A)" : L"Player 2 controls that dinosaur - select another one (aim + A)"), 4.0f);
            return;
        }
        if (Gone(unit) || Dead(unit)) {
            gamectl::RequestHintFor(p, L"That dinosaur can't be controlled", 3.0f);
            return;
        }
        // JPOG68: the game's state is the HUD player's - it must not block the other player in split-screen.
        if (!(split && p != hudP) && !gamectl::IsDefaultState(st) && !gamectl::IsFollowFamily(st)) {
            LOG("dino: not in the park view (state vtbl %p) - ignored", st ? *(void**)st : nullptr);
            return;
        }
        void* proxy = *reinterpret_cast<void**>((uint8_t*)unit + kUnitProxy);
        if (!proxy) { LOG("dino: unit %p has no proxy", unit); return; }
        void* steer = Steering(unit);
        if (!steer) { LOG("dino: unit %p has no creature steering", unit); return; }
        const float* sp = reinterpret_cast<const float*>((uint8_t*)steer + kSteerPos);
        float x = sp[0], y = sp[1], z = sp[2];

        // The game's own follow camera on the dino (camera state machine) - single player only.
        void* ns = nullptr;
        bool ok = GUARDED("start dino control", {
            if (split) ns = st;
            else if (gamectl::IsFollowState(st)) ns = st;
            else {
                ns = gamectl::Alloc(0x6C);
                if (ns) {
                    ++*reinterpret_cast<int*>((uint8_t*)proxy + 4);   // TManagedPtr passed by value
                    reinterpret_cast<fn_ctor>(game::At(kFollowStateCtor))(ns, nullptr, proxy);
                    reinterpret_cast<fn_thisP>(game::At(kTransferControl))(ctl, nullptr, ns);
                }
            }
        });
        if (!ok || !ns) { LOG("dino: could not start the follow state (ok=%d)", ok); return; }

        ++*reinterpret_cast<int*>((uint8_t*)proxy + 4);   // our own reference
        if (g_selU[1 - p] == unit) { DropSel(1 - p); LOG("dino: P%d's stale selection of %p cleared (P%d takes it)", 2 - p, unit, p + 1); }
        s = Slot{};
        s.proxy = proxy; s.dino = unit; s.followState = split ? nullptr : ns;
        s.lastHealth = Health(unit);
        s.split = split;
        if (split) {
            // Which way does the game's camera matrix look? Row 2 vs (camera target - camera position).
            float m[16], t[3];
            bool ok2 = GUARDED("dino cam convention", {
                void* cm = reinterpret_cast<void*(__stdcall*)()>(game::At(kGetCameraManager))();
                void* cam = cm ? reinterpret_cast<void*(__fastcall*)(void*, void*)>(game::At(kGetActiveCamera))(cm, nullptr) : nullptr;
                if (cam && splitscreen::ViewMatrix(m)) {
                    reinterpret_cast<void(__fastcall*)(void*, void*, float*)>(game::At(kCamTargetPos))(cam, nullptr, t);
                    float d[3] = { t[0] - m[12], t[1] - m[13], t[2] - m[14] };
                    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    float dot = l > 0.01f ? (d[0] * m[8] + d[1] * m[9] + d[2] * m[10]) / l : 1.0f;
                    s.fwdSign = dot < 0 ? -1.0f : 1.0f;
                    // The world camera always looks down at the park: world up = against row 2's vertical part
                    // (JPOG: Y points down). Row 1 of the game's matrix points world-DOWN (log 10:10:53) - keep that.
                    s.upSign = (m[9] * s.fwdSign) > 0 ? -1.0f : 1.0f;
                    s.r1Sign = (m[5] * s.upSign) < 0 ? -1.0f : 1.0f;
                    // Chase camera starts behind the dino, looking the way this player looked.
                    float fx = m[8] * s.fwdSign, fz = m[10] * s.fwdSign;
                    s.yaw = (fx * fx + fz * fz > 1e-6f) ? atan2f(fx, fz) : 0.0f;
                    s.pitch = config::Get().dinoCamPitch * (3.14159265f / 180.0f);
                    s.camPos[0] = 1e30f;
                    s.chaseInit = true;
                    LOG("dino: split camera convention row2.toTarget=%.2f -> fwd sign %+.0f, world up = %+.0f Y, row1 %s",
                        dot, s.fwdSign, s.upSign, s.r1Sign > 0 ? "up" : "down");
                }
            });
            (void)ok2;
            // HUD goes to the other player (the builder) - unless they ride a dino too.
            if (splitscreen::ActivePlayer() == p && !g_ctl[1 - p]) splitscreen::RequestActive(1 - p);
        }
        InterlockedExchange(&g_ctl[p], 1);
        // Stop whatever the AI was walking to (frozen drives would otherwise finish the old path).
        GUARDED("dino stand", { reinterpret_cast<fn_thisB>(game::At(kSteerStand))(steer, nullptr); });
        const Config& c = config::Get();
        g_pads[p].Poll(c.deadzone, c.triggerThreshold);    // prime the edges (the RB/D-pad press is not a B press)
        LOG("dino: player %d controls dino %p at (%.1f %.1f %.1f) health %.1f AI frozen%s%s%s", p + 1, unit, x, y, z, s.lastHealth,
            forced ? " [own pick]" : "", split ? " [split: own chase camera, no follow state]" : "", g_ctl[1 - p] ? " - BOTH players control a dino" : "");
        WakeIfAsleep(p, unit);   // JPOG40 (owner bug 3)
        DropSel(p); if (g_selU[1 - p] == unit) DropSel(1 - p);   // JPOG68: selections done
        if (split) {
            // JPOG34 (owner bugs 4 + 7): the game's selection makes the HUD holder's camera a FOLLOW camera on this
            // dino (ADefaultState::UpdateSelectedUnit -> SetCameraType(AFollowCamera)). The rider has their own chase
            // camera, so clear the selection: the builder keeps a free world camera.
            void* sel = *reinterpret_cast<void**>(game::At(kUnitSelection));
            if (sel && gamectl::SelectedUnit() == unit) {
                GUARDED("clear selection for the rider", {
                    reinterpret_cast<void(__fastcall*)(void*, void*, const void*)>(game::At(kSetSelectedUnit))(sel, nullptr, nullptr); });
                LOG("dino: cleared the game's selection (the builder keeps a world camera)");
            }
            ShowControls(p);
        } else
            gamectl::RequestHintFor(p, ControlsText(p), 5.0f);
        padrumble::Pulse(p, 0.5f, 0.25f);
    }

    // ---- JPOG36 hooks (owner bugs 1-3) ----
    // (a) The controlled dino's behaviour script (FSM) keeps running with the drives frozen: its old state can steer,
    //     play anims and switch the movement blend back on (eat cut short, walking on its own). AUnitBehaviour::Update
    //     (0x663820, called from AActiveUnit::Imp_UpdateControl) is skipped for the controlled dinos and a finishing-move
    //     victim. On let-go the drive manager is refreshed (End) and the FSM continues.
    using fn_bupd = void(__fastcall*)(void* self, void* edx);
    fn_bupd g_origBUpd = nullptr; unsigned g_bSkips = 0;
    void __fastcall Hook_BehaviourUpdate(void* self, void* edx) {
        if (self && (g_ctl[0] || g_ctl[1]) && config::Get().dinoFreezeBehaviour) {   // JPOG37 perf: nobody rides = straight through
            void* unit = *reinterpret_cast<void**>((uint8_t*)self + 4);
            for (int p = 0; p < 2; ++p)
                if (unit && g_ctl[p] && (unit == g_s[p].dino || unit == g_s[p].vic)) {
                    if (g_bSkips++ < 2 || (g_bSkips % 6000) == 0) LOG("dino: behaviour script of %p paused while player %d controls it (%u frames)", unit, p + 1, g_bSkips);
                    return;
                }
        }
        g_origBUpd(self, edx);
    }
    // (b) Off the ENGINE's camera a unit's model is flagged not visible (AToshiAnimationSystem::SetVisible clears
    //     model+0x40 bit 0x40000) and the unit + its steering update at low rate (ACreatureSteer::ProcessTick moves it
    //     straight at max speed; AActiveUnit::Imp_UpdateControl once a second with dt 1). In split-screen the rider's
    //     dino is drawn in the rider's half but culled by the builder's camera every frame (JPOG35 log: "forced full
    //     rate" 60x a second) -> alternating update paths = the walking "spasms". SetVisible(false) is now ignored for
    //     the controlled dinos (vftable slot, only virtual calls exist).
    using fn_setvis = void(__fastcall*)(void* self, void* edx, void* mi, int vis, int upd);
    fn_setvis g_origSetVis = nullptr; unsigned g_visForced = 0;
    void __fastcall Hook_SetVisible(void* self, void* edx, void* mi, int vis, int upd) {
        if (!(vis & 0xFF) && mi && (mi == g_forceVisMI[0] || mi == g_forceVisMI[1])) {
            vis = 1;
            if (g_visForced++ == 0) LOG("dino: controlled dino kept 'visible' for the engine (full-rate update) - first time");
        }
        g_origSetVis(self, edx, mi, vis, upd);
    }
    // (c) JPOG37 (owner bug 1): the HUD player's aim + A selected the OTHER player's dino (and the game then made the
    //     builder's camera follow it). AUnitSelection::SetSelectedUnit 0x526950 (detour) refuses a dino that a player
    //     controls in split-screen (single-player follow mode still selects its own dino).
    using fn_setsel = void(__fastcall*)(void* self, void* edx, const void* unit);
    fn_setsel g_origSetSel = nullptr; DWORD g_selHintT = 0; unsigned g_selRefused = 0;
    unsigned g_selRedirect = 0; constexpr float kSelNeighbour = 8.0f;    // JPOG45: metres around the AIM POINT
    // The HUD player's aim point: ACameraManager (static 0x50CF80) -> active camera (0x50CFD0) -> GetTargetPosition (0x50C770).
    bool AimPoint(float* out) {
        void* mgr = reinterpret_cast<void*(__stdcall*)()>(game::At(0x0050CF80))();
        void* cam = mgr ? reinterpret_cast<void*(__fastcall*)(void*, void*)>(game::At(0x0050CFD0))(mgr, nullptr) : nullptr;
        if (!cam) return false;
        reinterpret_cast<void(__fastcall*)(void*, void*, float*)>(game::At(0x0050C770))(cam, nullptr, out);
        return out[0] == out[0] && out[2] == out[2];
    }
    void __fastcall Hook_SetSelectedUnit(void* self, void* edx, const void* unit) {
        for (int p = 0; p < 2 && unit; ++p)
            if (g_ctl[p] && g_s[p].split && g_s[p].dino == unit) {
                if (g_selRefused++ < 5 || (g_selRefused % 100) == 0)
                    LOG("dino: selection of %p refused - player %d controls it (%u times)", unit, p + 1, g_selRefused);
                // JPOG41 (owner bug 2): the game's pick hits the controlled dino first, so a dino standing right next to
                // it could not be selected at all. Select the nearest OTHER live dino next to it instead (only the
                // controlled unit itself stays blocked). Nothing near -> refused as before.
                // JPOG45 (owner 2026-11-09: "it teleports you to another one"): JPOG41 took the dino nearest to the
                // CONTROLLED dino (12 m around it) - often not the one the player aimed at. Now: the dino nearest to the
                // player's AIM POINT (the active camera's target = the centre-locked cursor's ground point, the same point
                // the other player's A uses), only within kSelNeighbour of it. Only the controlled unit itself is skipped.
                float at[3], np[3];
                void* alt = AimPoint(at) ? dinocontrol::NearestDino(at, kSelNeighbour, np) : nullptr;
                if (alt && alt != unit) {
                    if (g_selRedirect++ < 5 || (g_selRedirect % 100) == 0)
                        LOG("dino: selection of controlled %p -> the dino at the aim point %p (%.1f m from it, %u times)", unit, alt,
                            sqrtf((np[0]-at[0])*(np[0]-at[0]) + (np[2]-at[2])*(np[2]-at[2])), g_selRedirect);
                    g_origSetSel(self, edx, alt);
                    return;
                }
                if (GetTickCount() - g_selHintT > 3000) {
                    g_selHintT = GetTickCount();
                    gamectl::RequestHint(p ? L"Player 2 controls that dinosaur" : L"Player 1 controls that dinosaur", 2.5f);
                }
                return;
            }
        g_origSetSel(self, edx, unit);
        if (unit && gamectl::IsA(const_cast<void*>(unit), kDinosaurClass)) { g_gameSelU = const_cast<void*>(unit); g_gameSelT = GetTickCount() | 1; }
    }
    void* ModelInst(void* dino) {
        void* ao = dino ? *reinterpret_cast<void**>((uint8_t*)dino + 0x30) : nullptr;
        return ao ? *reinterpret_cast<void**>((uint8_t*)ao + 0x80) : nullptr;
    }

    // ---- JPOG63 (owner 13:44): keyboard riding keys for player 1 ([keys] ride_*). They "press" player 1's riding pad
    // buttons, so every riding feature works the same; keys the game doesn't use (controls.ini), only while the game is in front.
    bool GameFront() {
        HWND fg = GetForegroundWindow(); DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }
    // JPOG70: per player - with two keyboards each player's riding keys come from their own keyboard (kbm.cpp).
    bool Key(int vk, int p = 0) { return vk && kbm::KeyDown(p, vk); }
    bool g_toggleArmed = false;   // the take-control key must be let go once before it means "let go"
    int  g_kbLogs = 0;
    void MergeKeyboard(int p, XPad& pad) {
        const Config& c = config::Get();
        // JPOG70: player 2 too, when player 2 plays with their own keyboard + mouse (same keys, their keyboard).
        if (!c.rideKeys || !GameFront()) return;
        if (p == 1 && !kbm::IsKbm(1)) return;
        static bool s_armed[2] = { false, false };
        bool down[PAD_COUNT]{};
        const bool t = Key(c.rideToggle, p);
        bool& armed = p ? s_armed[1] : g_toggleArmed;
        static DWORD s_last[2] = { 0, 0 };
        const DWORD nowT = GetTickCount();
        if (p == 1 && nowT - s_last[1] > 250) armed = false;   // a new ride: the key that started it must be let go first
        s_last[p & 1] = nowT;
        if (!t) armed = true;
        down[PAD_B]     = t && armed;
        down[PAD_LS]    = Key(c.rideRun, p);
        down[PAD_RB]    = Key(c.rideFinish, p);
        down[PAD_Y]     = Key(c.rideRoar, p);
        down[PAD_X]     = Key(c.rideEat, p);
        down[PAD_A]     = Key(c.rideDrink, p);
        down[c.dinoLbTurn ? PAD_RS : PAD_LB] = Key(c.rideSleep, p);   // JPOG73: sleep follows the pad button (R3)
        down[PAD_DUP]   = Key(c.rideZoomIn, p);
        down[PAD_DDOWN] = Key(c.rideZoomOut, p);
        const float lx = (Key(c.rideRight, p) ? 1.0f : 0.0f) - (Key(c.rideLeft, p) ? 1.0f : 0.0f);
        const float ly = (Key(c.rideFwd, p) ? 1.0f : 0.0f) - (Key(c.rideBack, p) ? 1.0f : 0.0f);
        const float rx = (Key(c.rideCamR, p) ? 1.0f : 0.0f) - (Key(c.rideCamL, p) ? 1.0f : 0.0f);
        const float ry = (Key(c.rideCamU, p) ? 1.0f : 0.0f) - (Key(c.rideCamD, p) ? 1.0f : 0.0f);
        const float rt = Key(c.rideBite, p) ? 1.0f : 0.0f, lt = Key(c.ridePounce, p) ? 1.0f : 0.0f;
        bool any = lx || ly || rx || ry || rt || lt;
        for (bool b : down) any = any || b;
        if (!any) return;
        pad.Merge(down, lx, ly, rx, ry, lt, rt);
        if (g_kbLogs < 3) { ++g_kbLogs; LOG("dino: keyboard riding keys used by player %d (JPOG63/JPOG70, [keys] ride_*)", p + 1); }
    }

    void TickSlot(int p, float dt, LONG rel) {
        Slot& s = g_s[p];
        ++s.frames;
        if (!gamectl::InPark()) { End(p, "left the park", false); return; }
        void* dino = s.dino;
        void* pu = s.proxy ? *reinterpret_cast<void**>((uint8_t*)s.proxy + 0x14) : nullptr;
        if (!dino || pu != dino || Gone(dino)) { End(p, "dino gone", true); return; }
        if (Dead(dino)) { End(p, "dino died", true); return; }

        void* st = gamectl::CurrentState();
        if (!s.split && st != s.followState) {
            if (gamectl::IsDefaultState(st) || gamectl::IsFollowFamily(st)) { End(p, "follow view closed", false); return; }
            if (!s.paused) { s.paused = true; LOG("dino: paused (another screen is open, state vtbl %p)", st ? *(void**)st : nullptr); }
            return;
        }
        if (!s.split && s.paused) { s.paused = false; LOG("dino: resumed"); }
        if (s.split && !splitscreen::CanSwitch()) { End(p, "split-screen ended", false); return; }

        const Config& c = config::Get();
        XPad& pad = g_pads[p];
        pad.Poll(c.deadzone, c.triggerThreshold);
        MergeKeyboard(p, pad);   // JPOG63
        if (rel == p || pad.Pressed(PAD_B)) { End(p, "B", true); return; }

        // Keep the dino's own AI off (Ranger mod R25 method).
        if (void* drv = *reinterpret_cast<void**>((uint8_t*)dino + kUnitDrive))
            *reinterpret_cast<float*>((uint8_t*)drv + 0xC) = -1000.0f;
        if (s.split) GUARDED("dino full-rate update", { ForceFullUpdate(s, p, dino); });
        g_forceVisMI[p] = config::Get().dinoForceVisible ? ModelInst(dino) : nullptr;
        // JPOG34 (owner bug 3): Back / Start while riding = show the dino controls again (in the rider's half).
        if (s.split && (pad.Pressed(PAD_BACK) || pad.Pressed(PAD_START))) ShowControls(p);

        float pos[3];
        if (!DinoPos(dino, pos)) return;
        if (dt > 0.25f) dt = 1.0f / 60.0f;
        if (dt <= 0.0f) dt = 1.0f / 60.0f;
        const bool hold = TickAction(p, dino, pos, dt, pad);   // JPOG32 roar / eat / sleep / finishing move
        // JPOG37 (owner bug 9): sleep icon while lying asleep (acts 3 lie down, 4 asleep, 8 rest -> sleep)
        if (s.act == 3 || s.act == 4 || s.act == 8) { if (s.zzz) ZzzKeep(s, dino); else if (s.act == 4 || s.actAge > 1.0f) ZzzOn(s, p, dino); }
        else if (s.zzz) ZzzOff(s, p, dino);
        if (!hold) Steer(p, dino, pos, dt);
        else { if (s.moving) { s.moving = false; s.holdValid = false; } GUARDED("dino hold (action)", { HoldStill(s, p, dino, pos, dt, true); }); }

        // A real hit takes a chunk of health at once; slow loss (hunger, age, sickness) is not a hit.
        float h = Health(dino);
        if (s.lastHealth >= 0.0f && h < s.lastHealth - 1.0f && GetTickCount() - s.lastHitT > 400) {
            s.lastHitT = GetTickCount();
            LOG("dino: player %d's dino was hit (%.1f -> %.1f)", p + 1, s.lastHealth, h);
            padrumble::Pulse(p, 0.9f, 0.35f);
        }
        s.lastHealth = h;
    }
}

// JPOG55 (backlog 19): "blood from the mouth on bites that hit nothing". The bite anims fire a blood breakpoint
// (AUnitDinosaur::BloodCallback 0x4CB490, vftable slot 0xCFF6A4), which bleeds whenever the dino's own target proxy
// (+0xC8) holds a unit - also a stale AI target far away. While a player's dino bites with nobody in range: no blood.
namespace {
    using fn_blood = void(__fastcall*)(void*, void*, const void*);
    fn_blood g_origBlood = nullptr;
    int g_bloodLogs = 0;
    void __fastcall Hook_Blood(void* self, void* edx, const void* bp) {
        for (int p = 0; p < 2; ++p) {
            if (g_s[p].dino == self && g_s[p].act == 7 && !g_s[p].biteHit) {
                if (g_bloodLogs < 3) { ++g_bloodLogs; LOG("dino: P%d bite hit nothing - no blood", p + 1); }
                return;
            }
        }
        g_origBlood(self, edx, bp);
    }
}

// JPOG57 (owner 11:55: "when a dino bumps into a tree, the TREE spins around wildly"): ACreatureSteer::ProcessTick knocks
// every tree cell it moves through with ATreeManager::ApplyAnimation(x, z, 3 = knock, strength, pos, facing, 3.0) - call
// 0x6398EE (thiscall, 7 args, ret 0x1C). Vanilla dinos steer round trees (HardAvoidWorld); a player's dino is pushed into
// them, so the same cells get the knock re-applied every tick while the tree is still swinging. [dino] tree_knock_once:
// for a PLAYER's dino (position = its steering +0x64, which GetPosition copies) each cell is knocked at most once per 3 s.
// The first knocks are logged (strength, facing length) to show what else might drive the spin.
namespace {
    using fn_treeAnim = void(__fastcall*)(void*, void*, unsigned, unsigned, int, float, const float*, const float*, float);
    struct KnockCell { unsigned key; DWORD t; };
    KnockCell g_knock[64]; int g_knockN = 0; unsigned g_knockLogs = 0, g_knockSkips = 0;
    bool PlayerDinoAt(const float* pos, int* who) {
        for (int p = 0; p < 2; ++p) {
            void* d = g_ctl[p] ? g_s[p].dino : nullptr;
            void* st = d ? Steering(d) : nullptr;
            if (!st) continue;
            const float* sp = reinterpret_cast<const float*>((uint8_t*)st + kSteerPos);
            if (fabsf(sp[0] - pos[0]) < 0.05f && fabsf(sp[2] - pos[2]) < 0.05f) { *who = p; return true; }
        }
        return false;
    }
    void __fastcall Hook_TreeKnock(void* tm, void* edx, unsigned x, unsigned z, int type, float strength, const float* pos,
                                   const float* face, float radius) {
        int who = -1;
        if (config::Get().treeKnockOnce && type == 3 && pos && PlayerDinoAt(pos, &who)) {
            const unsigned key = ((x >> 3) & 0xFFFF) | (((z >> 3) & 0xFFFF) << 16);
            const DWORD now = GetTickCount();
            for (int i = 0; i < g_knockN; ++i) {
                if (g_knock[i].key != key) continue;
                if (now - g_knock[i].t < 3000) {
                    if (g_knockSkips++ < 3 || (g_knockSkips % 200) == 0)
                        LOG("dino: P%d tree cell %u,%u knocked again within 3 s - skipped (%u skips)", who + 1, x >> 3, z >> 3, g_knockSkips);
                    return;
                }
                g_knock[i].t = now; goto apply;
            }
            if (g_knockN < 64) g_knock[g_knockN++] = KnockCell{ key, now };
            else { for (int i = 1; i < 64; ++i) g_knock[i - 1] = g_knock[i]; g_knock[63] = KnockCell{ key, now }; }
        apply:
            if (g_knockLogs++ < 4) LOG("dino: P%d knocks tree cell %u,%u (strength %.2f, facing length %.2f, radius %.1f)", who + 1,
                x >> 3, z >> 3, strength, face ? sqrtf(face[0] * face[0] + face[1] * face[1] + face[2] * face[2]) : -1.0f, radius);
        }
        reinterpret_cast<fn_treeAnim>(game::At(0x005DC6B0))(tm, edx, x, z, type, strength, pos, face, radius);
    }
    bool PatchCallHere(uintptr_t site, uintptr_t expect, void* fn) {
        uint8_t* p = reinterpret_cast<uint8_t*>(game::At(site));
        if (p[0] != 0xE8 || (uintptr_t)p + 5 + *reinterpret_cast<int32_t*>(p + 1) != (uintptr_t)game::At(expect)) return false;
        DWORD old;
        if (!VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
        *reinterpret_cast<int32_t*>(p + 1) = (int32_t)((uintptr_t)fn - ((uintptr_t)p + 5));
        VirtualProtect(p + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
        return true;
    }
}

namespace dinocontrol {

void RequestControl(int player) { if (config::Get().dinoEnabled) InterlockedExchange(&g_reqControl, player); }
void NoteWindowUnit(int player, void* unit) {   // JPOG76: p2menu opened this player's own window for unit (game thread)
    if (player < 0 || player > 1) return;
    g_winU[player] = unit; g_winT[player] = GetTickCount();
}
bool HeliPressFromWindow(int player) {   // JPOG76: RB + D-pad Up while this player's own window is open
    const Config& c = config::Get();
    if (player < 0 || player > 1 || !c.heliPadFly || !c.heliPresser || !g_winU[player] || GetTickCount() - g_winT[player] > 60000) return false;
    void* h = HeliFrom(g_winU[player], player, "own window open");
    if (!h) return false;
    HeliPress(player, h);
    return true;
}
void RequestInfo(int player) { InterlockedExchange(&g_reqInfo, player); }
void RequestRelease(int player) { InterlockedExchange(&g_reqRelease, player); }
void RequestSelectAt(int player, const float* pt) {
    if (!config::Get().dinoEnabled || player < 0 || player > 1) return;
    g_selPt[player][0] = pt[0]; g_selPt[player][1] = pt[1]; g_selPt[player][2] = pt[2];
    InterlockedExchange(&g_reqSel, player);
}
bool HasSelection(int player) { return (player == 0 || player == 1) && g_selU[player] != nullptr; }
// JPOG69 R1: game thread. Position of this player's own selection (null/false when there is none or it died);
// *serial changes every time the player selects a dino.
bool SelectedPos(int player, float* out, unsigned* serial) {
    if (player < 0 || player > 1) return false;
    if (serial) *serial = g_selSerial[player];
    if (g_ctl[player]) return false;                    // riding: the chase camera owns the view
    void* u = SelOf(player);
    return u && DinoPos(u, out);
}
void RequestControlAt(int player, const float* pt) {
    if (!config::Get().dinoEnabled || player < 0 || player > 1) return;
    g_pickPt[player][0] = pt[0]; g_pickPt[player][1] = pt[1]; g_pickPt[player][2] = pt[2];
    InterlockedExchange(&g_reqPick, player);
}
// Nearest live dinosaur to pt within radius (not one a player already controls). Game thread, no GUARDED inside.
// JPOG57: nearest live unit of any class (buildings too: no IsDead virtual call - only the "gone" check).
namespace { bool __stdcall NearestAnyCb(void* unit, float distSq, const float*, void* user) {
    Nearest* n = static_cast<Nearest*>(user);
    if (!unit || Gone(unit)) return false;
    if (distSq < n->bestDistSq) { n->bestDistSq = distSq; n->best = unit; }
    return false;
} }
void* NearestUnitOf(const float* pt, float radius, uintptr_t tclassVa, float* outDist) {
    void* mgr = *reinterpret_cast<void**>(game::At(kUnitManager));
    if (!mgr) return nullptr;
    Nearest n{ nullptr, nullptr, 1e30f, 0, 0, false, nullptr };
    GUARDED("nearest unit of a class", {
        reinterpret_cast<fn_prox>(game::At(kProximityCheck))(mgr, nullptr, pt, radius, game::At(tclassVa), &NearestAnyCb, &n); });
    if (outDist) *outDist = n.best ? sqrtf(n.bestDistSq) : 0.0f;   // JPOG74: distance for the STOPPED log
    return n.best;
}
// JPOG57 (owner 12:05: "automatically take control when you click a dino, for both players"): the HUD player's A selects
// the unit the game's way (aim + A -> AUnitSelection); a press noted here + the selection CHANGING to a dinosaur within
// 1.2 s = take control (Tick). The selection at press time is kept so an old selection never counts.
volatile DWORD g_aT[2] = { 0, 0 }; void* volatile g_aSel[2] = { nullptr, nullptr };
void NoteAPress(int player) {
    if (player < 0 || player > 1 || !config::Get().dinoEnabled) return;   // JPOG68: also when A only selects
    void* sel = nullptr;
    GUARDED("selection at A press", { sel = gamectl::SelectedUnit(); });
    g_aSel[player] = sel;
    g_aT[player] = GetTickCount() | 1;
}
void* ZzzDino(int p) { return (p == 0 || p == 1) && g_ctl[p] && g_s[p].zzz ? g_s[p].dino : nullptr; }   // JPOG73
bool IsZzzUnit(const void* unit) {
    if (!unit || !config::Get().dinoZzzAlways) return false;
    for (int p = 0; p < 2; ++p) if (g_ctl[p] && g_s[p].dino == unit && g_s[p].zzz) return true;
    return false;
}
void* NearestDino(const float* pt, float radius, float* outPos) {
    void* mgr = *reinterpret_cast<void**>(game::At(kUnitManager));
    if (!mgr) return nullptr;
    Nearest n{ nullptr, nullptr, 1e30f, 0, 0, true, nullptr };   // JPOG37: controlled dinos skipped -> the next nearest
    reinterpret_cast<fn_prox>(game::At(kProximityCheck))(mgr, nullptr, pt, radius, game::At(kDinosaurClass), &NearestCb, &n);
    if (n.best && Controlled(n.best)) return nullptr;
    if (n.best && outPos && !DinoPos(n.best, outPos)) return nullptr;
    return n.best;
}
bool Busy() { return g_ctl[0] || g_ctl[1] || g_reqControl >= 0; }
int  Controller() { return g_ctl[0] ? 0 : (g_ctl[1] ? 1 : -1); }
bool Controls(int p) { return (p == 0 || p == 1) && g_ctl[p] != 0; }
// JPOG40 (owner bug 4): where the sound listener should be in split-screen while a player rides a dino (the dino's
// own position; player 1 first if both ride). False = leave the game's listener (the engine camera) alone.
// JPOG41 (owner bug 1, "no dino sound for either player"): use the EXACT position the dino's own sounds use
// (AVisibleUnit::GenSoundCallback 0x4DA7E0: steering vfn +0x38 GetPosition -> PlaySound3D), not steering+0x64,
// [coop] rider_listener = 1 turns the listener move on; DEFAULT 0 = TEST: the owner's prime suspect (JPOG40 patch) off.
// First calls log both positions either way.
static unsigned g_lpLogs = 0;
bool ListenerPos(float* out) {
    if (!splitscreen::Active()) return false;
    if (!config::Get().riderListener && g_lpLogs >= 3) return false;   // test default: nothing more runs once logged
    for (int p = 0; p < 2; ++p) {
        void* d = g_ctl[p] ? g_s[p].dino : nullptr;
        if (!d || Gone(d)) continue;
        void* st = Steering(d);
        if (!st) continue;
        float sp[3] = { 0, 0, 0 }, raw[3] = { 0, 0, 0 };
        bool ok = GUARDED("sound position of the rider's dino", {
            reinterpret_cast<void*(__fastcall*)(void*, void*, float*)>((*reinterpret_cast<void***>(st))[0x38 / 4])(st, nullptr, sp); });
        DinoPos(d, raw);
        if (g_lpLogs < 3) { ++g_lpLogs;
            LOG("dino: listener P%d sound pos (%.1f %.1f %.1f) ok=%d, steer+0x64 (%.1f %.1f %.1f)", p + 1, sp[0], sp[1], sp[2], ok,
                raw[0], raw[1], raw[2]); }
        if (!config::Get().riderListener) return false;   // JPOG41 default: game's listener (positions above still logged)
        if (!ok || !(sp[0] == sp[0] && sp[1] == sp[1] && sp[2] == sp[2])) continue;
        out[0] = sp[0]; out[1] = sp[1]; out[2] = sp[2];
        return true;
    }
    return false;
}
// JPOG42 (owner finding 2026-11-06 12:50): one sound listener (the engine/HUD camera). For PlaySound3D's detour in
// splitscreen.cpp: sound positions ("ears") of every ridden dino in split. Returns how many were written (0..2).
int RiderEars(float out[2][3], int who[2]) {
    int n = 0;
    if (!splitscreen::Active()) return 0;
    for (int p = 0; p < 2; ++p) {
        void* d = g_ctl[p] ? g_s[p].dino : nullptr;
        if (!d || Gone(d)) continue;
        void* st = Steering(d);
        if (!st) continue;
        float sp[3] = { 0, 0, 0 };
        bool ok = GUARDED("ears: sound position of a rider's dino", {
            reinterpret_cast<void*(__fastcall*)(void*, void*, float*)>((*reinterpret_cast<void***>(st))[0x38 / 4])(st, nullptr, sp); });
        if (!ok || !(sp[0] == sp[0] && sp[1] == sp[1] && sp[2] == sp[2])) continue;
        out[n][0] = sp[0]; out[n][1] = sp[1]; out[n][2] = sp[2]; who[n] = p; ++n;
    }
    return n;
}

// ---- JPOG78: hints follow the player's input (owner 17:12: keyboard + mouse players must see PC controls) ----
bool PcPlayer(int p) {
    p &= 1;
    if (kbm::IsKbm(p)) return true;
    return p == 0 && !input_map::ConsoleActive();   // player 1 without a controller / PC style
}
// JPOG80: any key's name as the keyboard driver shows it ("T", "Right Shift", "Num 4"); buf >= 32
const wchar_t* KeyName(int vk, wchar_t* buf, const wchar_t* fallback) {
    buf[0] = 0;
    UINT sc = vk ? MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC) : 0;
    LONG lp = (LONG)(sc << 16);
    if (vk == VK_RSHIFT) lp = (LONG)(0x36 << 16);
    if (vk == VK_DIVIDE || vk == VK_RCONTROL || vk == VK_RMENU || (vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE)
        lp |= 1 << 24;   // extended key
    if (!sc || !GetKeyNameTextW(lp, buf, 32)) { int i = 0; for (; fallback[i] && i < 31; ++i) buf[i] = fallback[i]; buf[i] = 0; }
    return buf;
}
const wchar_t* TakeKeyName() {
    static wchar_t s_name[32];
    return KeyName(config::Get().rideToggle, s_name, L"T");
}
const wchar_t* HintText(int who, const wchar_t* padText, const wchar_t* pcFmt) {
    // JPOG79: JPOG78 had a trailing // comment that swallowed the controller branch (body left uninitialised = the boxes
    // in the owner's photos 17:30, log "hint: Player 1: " + junk). Rewritten one statement per line, no swprintf.
    static wchar_t buf[2][256]; static int s_i = 0;   // game thread; two buffers so a caller may hold one result
    wchar_t* out = buf[s_i ^= 1];
    const bool pc = PcPlayer(who < 0 ? 0 : who);
    const wchar_t* key = (who == 1 && kbm::IsKbm(1)) ? L"T" : TakeKeyName();   // kbm.cpp: player 2's take key is T
    wchar_t body[220];
    if (pc) gamectl::WFmt(body, 220, pcFmt, key);
    else gamectl::WFmt(body, 220, L"%ls", padText);
    if (who >= 0 && body[0] >= L'A' && body[0] <= L'Z' && body[1] >= L'a' && body[1] <= L'z') body[0] = (wchar_t)(body[0] - L'A' + L'a');   // "Player 1: dinosaur ..."
    if (who >= 0) gamectl::WFmt(out, 256, L"Player %d: %ls", who + 1, body);
    else gamectl::WFmt(out, 256, L"%ls", body);
    return out;
}
void ForceRelease(const char* why) { for (int p = 0; p < 2; ++p) if (g_ctl[p]) End(p, why, true); }

// Game thread (splitscreen::Tick, before dinocontrol::Tick). Builds the dino player's view matrix.
bool ChaseView(int who, float dt, const float* ref, float* out) {
    if (who < 0 || who > 1 || !g_ctl[who]) return false;
    Slot& s = g_s[who];
    if (!s.split || !s.dino) return false;
    float pos[3];
    if (!DinoPos(s.dino, pos)) return false;
    const Config& c = config::Get();
    if (dt <= 0 || dt > 0.25f) dt = 1.0f / 60.0f;
    const float deg = 3.14159265f / 180.0f;
    const float* r0 = ref; const float* r1 = ref + 4; const float* r2 = ref + 8;
    if (!s.chaseInit) {
        // Start behind the dino, looking the way this player's camera looked.
        float fx = r2[0] * s.fwdSign, fz = r2[2] * s.fwdSign;
        s.yaw = (fx * fx + fz * fz > 1e-6f) ? atan2f(fx, fz) : 0.0f;
        s.pitch = c.dinoCamPitch * deg;
        s.chaseInit = true;
        s.camPos[0] = 1e30f;
    }
    XPad& pad = g_pads[who];
    s.yaw   += pad.RX() * c.dinoCamTurn * deg * dt * c.dinoCamInvX;
    s.pitch -= pad.RY() * c.dinoCamTurn * 0.6f * deg * dt * c.dinoCamInvY;
    if (s.pitch < 5.0f * deg) s.pitch = 5.0f * deg;
    if (s.pitch > 75.0f * deg) s.pitch = 75.0f * deg;
    if (pad.Down(PAD_DUP))   s.zoom -= 0.8f * dt;
    if (pad.Down(PAD_DDOWN)) s.zoom += 0.8f * dt;
    if (s.zoom < 0.4f) s.zoom = 0.4f;
    if (s.zoom > 2.5f) s.zoom = 2.5f;
    if (pad.Down(PAD_DRIGHT)) s.lookOff += 3.0f * dt;   // JPOG55 (backlog 13): camera up / down, e.g. to see small dinos
    if (pad.Down(PAD_DLEFT))  s.lookOff -= 3.0f * dt;
    if (s.lookOff < -c.dinoCamLookH + 0.3f) s.lookOff = -c.dinoCamLookH + 0.3f;
    if (s.lookOff > 10.0f) s.lookOff = 10.0f;
    const float dist = c.dinoCamDist * s.zoom;
    const float up[3] = { 0, s.upSign, 0 };
    const float look[3] = { pos[0], pos[1] + s.upSign * (c.dinoCamLookH + s.lookOff), pos[2] };
    float want[3] = { look[0] - sinf(s.yaw) * cosf(s.pitch) * dist,
                      look[1] + s.upSign * sinf(s.pitch) * dist,
                      look[2] - cosf(s.yaw) * cosf(s.pitch) * dist };
    if (s.camPos[0] > 1e29f) { s.camPos[0] = want[0]; s.camPos[1] = want[1]; s.camPos[2] = want[2]; }
    float k = 1.0f - expf(-10.0f * dt);
    for (int i = 0; i < 3; ++i) s.camPos[i] += (want[i] - s.camPos[i]) * k;
    float f[3] = { look[0] - s.camPos[0], look[1] - s.camPos[1], look[2] - s.camPos[2] };
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-4f) return false;
    for (float& v : f) v /= fl;
    auto cross = [](const float* a, const float* b, float* o) {
        o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; };
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    float r[3], u[3];
    cross(up, f, r);
    float rl = sqrtf(dot(r, r)); if (rl < 1e-4f) return false;
    for (float& v : r) v /= rl;
    cross(f, r, u);
    if (dot(u, up) * s.r1Sign < 0) { u[0] = -u[0]; u[1] = -u[1]; u[2] = -u[2]; }
    // Same handedness as the game's camera matrix: sign of (row0 x row1) . row2.
    float rc[3]; cross(r0, r1, rc);
    float h = dot(rc, r2) < 0 ? -1.0f : 1.0f;
    float row2[3] = { f[0] * s.fwdSign, f[1] * s.fwdSign, f[2] * s.fwdSign };
    float rc2[3]; cross(r, u, rc2);
    if (dot(rc2, row2) * h < 0) { r[0] = -r[0]; r[1] = -r[1]; r[2] = -r[2]; }
    for (int i = 0; i < 3; ++i) {
        out[i] = r[i]; out[4 + i] = u[i]; out[8 + i] = row2[i]; out[12 + i] = s.camPos[i];
    }
    out[3] = ref[3]; out[7] = ref[7]; out[11] = ref[11]; out[15] = ref[15];
    for (int i = 0; i < 16; ++i) s.chase[i] = out[i];
    s.chaseValid = true;
    if (s.chaseLogs < 2 || (s.frames % 600) == 0) {
        ++s.chaseLogs;
        LOG("dino: chase cam P%d dino (%.1f %.1f %.1f) cam (%.1f %.1f %.1f) yaw %.0f pitch %.0f dist %.1f hand %+.0f%s",
            who + 1, pos[0], pos[1], pos[2], s.camPos[0], s.camPos[1], s.camPos[2], s.yaw / deg, s.pitch / deg, dist, h,
            who == splitscreen::ActivePlayer() ? " (active half)" : "");
    }
    return true;
}

void RequestTestSpawn() { InterlockedExchange(&g_reqSpawn, 1); }

// JPOG36: behaviour-script pause (detour) + keep-visible (vftable slot) for the controlled dinos.
void Install() {
    void* tramp = nullptr;
    bool b = detour::Install(game::At(kBehaviourUpdate), (void*)&Hook_BehaviourUpdate, &tramp);
    if (b) g_origBUpd = reinterpret_cast<fn_bupd>(tramp);
    void** slot = reinterpret_cast<void**>(game::At(kToshiAnimSysVtbl + 0x94));
    bool v = false;
    if (*slot == game::At(0x0065B390)) {
        DWORD old;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            g_origSetVis = reinterpret_cast<fn_setvis>(*slot);
            InterlockedExchangePointer(slot, (void*)&Hook_SetVisible);
            VirtualProtect(slot, sizeof(void*), old, &old);
            v = true;
        }
    }
    if (config::Get().dinoBloodOnHit) {
        void** bs = reinterpret_cast<void**>(game::At(0x00CFF6A4));
        DWORD old;
        if (*bs == game::At(0x004CB490) && VirtualProtect(bs, sizeof(void*), PAGE_READWRITE, &old)) {
            g_origBlood = reinterpret_cast<fn_blood>(*bs);
            InterlockedExchangePointer(bs, (void*)&Hook_Blood);
            VirtualProtect(bs, sizeof(void*), old, &old);
            LOG("dino: blood only when a bite has a target ON ([dino] blood_only_on_hit)");
        } else LOG("dino: blood hook FAILED (slot 0xCFF6A4 changed?)");
    }
    if (config::Get().treeKnockOnce)
        LOG("dino: tree knock limit for player dinos %s ([dino] tree_knock_once, JPOG57)",
            PatchCallHere(0x006398EE, 0x005DC6B0, (void*)&Hook_TreeKnock) ? "ON" : "FAILED (call site changed?)");
    void* tramp2 = nullptr;
    bool sel = detour::Install(game::At(kSetSelectedUnit), (void*)&Hook_SetSelectedUnit, &tramp2);
    if (sel) g_origSetSel = reinterpret_cast<fn_setsel>(tramp2);
    LOG("dino: hooks - behaviour pause %s, keep-visible %s, selection guard %s", b ? "ON" : "FAILED", v ? "ON" : "FAILED (slot not 0x65B390)",
        sel ? "ON" : "FAILED");
}

void Tick(float dt) {
    // JPOG37 (owner bug 10): frame-time measurement - every 10 s: average / worst frame, dinosaurs in the park, split.
    {
        static double acc = 0; static float worst = 0; static unsigned n = 0; static DWORD t0 = 0;
        if (config::Get().perfLog && dt > 0 && dt < 2.0f) {
            acc += dt; ++n; if (dt > worst) worst = dt;
            DWORD now = GetTickCount(); if (!t0) t0 = now;
            if (now - t0 >= 10000 && n) {
                int dinos = -1;
                if (gamectl::InPark()) {
                    void* mgr = *reinterpret_cast<void**>(game::At(kUnitManager));
                    float far0[3] = { 0, 0, 0 };
                    Nearest nn{ nullptr, nullptr, 1e30f, 0, 0, false, nullptr };
                    if (mgr && GUARDED("perf: count dinos", {
                            reinterpret_cast<fn_prox>(game::At(kProximityCheck))(mgr, nullptr, far0, 100000.0f, game::At(kDinosaurClass), &NearestCb, &nn); }))
                        dinos = nn.seen;
                }
                LOG("perf: %.1f ms/frame avg (%.0f fps), worst %.1f ms, %u frames, dinosaurs %d, split %d, riders %d",
                    1000.0 * acc / n, n / acc, worst * 1000.0f, n, dinos, splitscreen::CanSwitch() ? 1 : 0, (int)g_ctl[0] + (int)g_ctl[1]);
                acc = 0; worst = 0; n = 0; t0 = now;
            }
        }
    }
    if (InterlockedExchange(&g_reqSpawn, 0)) TestSpawn();
    if (g_testA && g_testLogs < 12 && GetTickCount() - g_testT0 > (DWORD)(g_testLogs + 1) * 5000) {
        ++g_testLogs;
        float pa[3]{}, pb[3]{}; float ha = -1, hb = -1;
        GUARDED("debug test dino health", {
            ha = Health(g_testA); DinoPos(g_testA, pa);
            if (g_testB) { hb = Health(g_testB); DinoPos(g_testB, pb); } });
        auto tag = [](void* u) { return (g_ctl[0] && g_s[0].dino == u) ? "[P1]" : (g_ctl[1] && g_s[1].dino == u) ? "[P2]" : ""; };
        LOG("debug: test dinos  A health %.1f at (%.1f %.1f %.1f) %s   B health %.1f at (%.1f %.1f %.1f) %s",
            ha, pa[0], pa[1], pa[2], tag(g_testA), hb, pb[0], pb[1], pb[2], tag(g_testB));
    }
    // Debug only ([debug] test_dino): keep the 2nd test dino where it was spawned (AI held), so attacks and the
    // finishing move can be tested without it wandering off.
    if (g_testB && !(g_ctl[0] && g_s[0].dino == g_testB) && !(g_ctl[1] && g_s[1].dino == g_testB) &&
        !(g_ctl[0] && g_s[0].vic == g_testB) && !(g_ctl[1] && g_s[1].vic == g_testB))
        GUARDED("debug hold test dino", { if (!Gone(g_testB) && !Dead(g_testB)) { FreezeAI(g_testB); if (g_s[0].frames % 20 == 0) Stand(g_testB); } else g_testB = nullptr; });
    // JPOG55 (backlog 2): the vanilla info bar (Info / Summary / Orders) = AUnit::ExecuteOrder_Info 0x4C1860 (thiscall,
    // no args; what the game's Info order does: AUnitInfoState on the game's state chain = the HUD player's).
    LONG inf = InterlockedExchange(&g_reqInfo, -1);
    if (inf == 0 || inf == 1) {
        void* u = gamectl::SelectedUnit();
        void* st = gamectl::CurrentState();
        if (inf != splitscreen::ActivePlayer()) gamectl::RequestHintFor(inf, L"Only the player with the menus can open the info bar", 3.0f);
        else if (!u) gamectl::RequestHintFor(inf, PcPlayer(inf) ? L"Click a dinosaur or building for its info" : L"Select a dinosaur or building first, then RB + D-pad Up for its info", 3.0f);
        else if (!gamectl::IsDefaultState(st)) LOG("dino: info bar - not in the park view, ignored");
        else {
            bool ok = GUARDED("open info bar", { reinterpret_cast<bool(__fastcall*)(void*, void*)>(game::At(0x004C1860))(u, nullptr); });
            LOG("dino: player %d RB + D-pad Up -> info bar for %p (ok=%d)", (int)inf + 1, u, ok);
        }
    }
    // JPOG68 (owner 16:28: "A wouldn't select a lot of the time"): 0.25 s after the HUD player's A, the game's own
    // selection decides first (dino -> that one, a building -> leave it); nothing / the same unit as before -> the nearest
    // dinosaur to the game's aim point within [dino] select_radius becomes the selection (the game's too, so the vanilla
    // ring + info + our "RB + D-pad Up" hint follow). [dino] a_takes_control = 1 still takes control at once.
    for (int p = 0; p < 2; ++p) {
        const DWORD t = g_aT[p];
        if (!t) continue;
        if (g_ctl[p]) { g_aT[p] = 0; continue; }
        if (GetTickCount() - t < 250) continue;
        g_aT[p] = 0;
        void* u = nullptr;
        GUARDED("selection after A", { u = gamectl::SelectedUnit(); });
        const bool isDino = u && gamectl::IsA(u, kDinosaurClass);
        if (isDino && Controlled(u) && !(g_ctl[p] && g_s[p].dino == u)) {
            gamectl::RequestHintFor(p, L"Another player controls that dinosaur", 3.0f);
            continue;
        }
        if (isDino && (u != g_aSel[p] || !config::Get().aTakesControl)) {
            SetSel(p, u);
            if (config::Get().aTakesControl) {
                LOG("dino: player %d pressed A on dino %p - taking control (JPOG57, [dino] a_takes_control)", p + 1, u);
                Begin(p);
            } else LOG("dino: player %d selected dino %p (game selection)", p + 1, u);
            continue;
        }
        if (u && !isDino && u != g_aSel[p]) continue;   // the game selected a building / something else
        float aim[3];
        void* d = nullptr;
        if (splitscreen::GameAim(aim)) GUARDED("select nearest dino at the aim", { d = NearestDino(aim, config::Get().dinoSelectRadius, nullptr); });
        if (!d) { LOG("dino: player %d A - no dinosaur within %.0f m of the aim", p + 1, config::Get().dinoSelectRadius); continue; }
        SetSel(p, d);
        bool ok = false;
        if (void* sel = *reinterpret_cast<void**>(game::At(kUnitSelection)))
            ok = GUARDED("select dino for the HUD player", {
                reinterpret_cast<void(__fastcall*)(void*, void*, const void*)>(game::At(kSetSelectedUnit))(sel, nullptr, d); });
        LOG("dino: player %d A - the game picked %p, selected the nearest dino %p at the aim instead (game selection set %d)", p + 1, u, d, ok);
        if (config::Get().aTakesControl) Begin(p, d);
        else gamectl::RequestHintFor(p, HintText(splitscreen::Active() ? p : -1, L"Dinosaur selected - press RB + D-pad Up to control it", L"Dinosaur selected - press %ls to control it"), 4.0f);
    }
    // JPOG68: player 2 (no HUD) - A selects the dinosaur nearest the middle of their view (their own selection).
    LONG sp = InterlockedExchange(&g_reqSel, -1);
    if (sp == 0 || sp == 1) {
        const float* pt = g_selPt[sp];
        void* d = nullptr;
        GUARDED("select dino near the view middle", { d = NearestDino(pt, config::Get().pickRadius, nullptr); });
        if (d) {
            SetSel((int)sp, d);
            LOG("dino: player %d selected dino %p near (%.1f %.1f %.1f) - RB + D-pad Up takes it", (int)sp + 1, d, pt[0], pt[1], pt[2]);
            gamectl::RequestHintFor((int)sp, HintText((int)sp, L"dinosaur selected - press RB + D-pad Up to control it", L"dinosaur selected - press %ls to control it"), 4.0f);
        } else {
            LOG("dino: player %d A - no free dinosaur within %.0f m of (%.1f %.1f %.1f)", (int)sp + 1, config::Get().pickRadius, pt[0], pt[1], pt[2]);
            gamectl::RequestHintFor((int)sp, HintText((int)sp, L"no dinosaur there - aim at one and press A", L"no dinosaur there - click one"), 3.0f);
        }
    }
    // JPOG63 (owner 13:41): the old way back - selecting a dinosaur (aim/click + A, the game's own selection) shows a hint
    // with the button combo; console style and PC-cursor / no-pad style get their own wording.
    if (!config::Get().aTakesControl && config::Get().dinoSelectHint) {
        static void* s_lastSel = nullptr;
        void* u = nullptr;
        GUARDED("selection hint", { if (gamectl::ParkViewActive()) u = gamectl::SelectedUnit(); });
        const int hud = splitscreen::Active() ? splitscreen::ActivePlayer() : 0;
        if (u != s_lastSel) {
            s_lastSel = u;
            if (u && !g_ctl[hud] && gamectl::IsA(u, kDinosaurClass)) {
                const bool console = !PcPlayer(hud);   // JPOG78: the HUD player's own input
                gamectl::RequestHintFor(hud, HintText(splitscreen::Active() ? hud : -1, L"Dinosaur selected - press RB + D-pad Up to control it",
                                              L"Dinosaur selected - press %ls to control it"), 4.0f);
                LOG("dino: selection hint shown (%s style) for %p", console ? "console" : "PC", u);
            }
        }
    }
    // JPOG63: keyboard take-control key (default T) for player 1 = the same as RB + D-pad Up on the selected dinosaur.
    {
        static bool s_prevT = false;
        const Config& kc = config::Get();
        const bool t = kc.rideKeys && GameFront() && Key(kc.rideToggle);
        if (t && !s_prevT && !g_ctl[0] && gamectl::ParkViewActive()) {
            LOG("dino: player 1 pressed the take-control key (keyboard)");
            g_toggleArmed = false;
            InterlockedExchange(&g_reqControl, 0);
        }
        s_prevT = t;
    }
    HeliPendingTick();   // JPOG76
    LONG req = InterlockedExchange(&g_reqControl, -1);
    LONG rel = InterlockedExchange(&g_reqRelease, -1);
    if ((req == 0 || req == 1) && !g_ctl[req]) Begin((int)req);
    LONG pick = InterlockedExchange(&g_reqPick, -1);
    if ((pick == 0 || pick == 1) && !g_ctl[pick]) {
        const float* pt = g_pickPt[pick];
        float dp[3]{}; void* u = nullptr;
        GUARDED("pick dino near the view middle", { u = NearestDino(pt, config::Get().pickRadius, dp); });
        if (u) {
            LOG("dino: player %d picked dino %p at (%.1f %.1f %.1f), %.1f m from the middle of their view", (int)pick + 1, u,
                dp[0], dp[1], dp[2], sqrtf((dp[0] - pt[0]) * (dp[0] - pt[0]) + (dp[2] - pt[2]) * (dp[2] - pt[2])));
            Begin((int)pick, u);
        } else if (HeliAtPick((int)pick, pt)) {
            // JPOG76: no dino at the aim, but a helicopter / helipad there (or this player's recent helipad window) -> fly it
        } else {
            LOG("dino: player %d - no dinosaur within %.0f m of (%.1f %.1f %.1f)", (int)pick + 1, config::Get().pickRadius, pt[0], pt[1], pt[2]);
            gamectl::RequestHintFor((int)pick, HintText((int)pick, L"no dinosaur near the middle of your view - move closer and press A",
                                          L"no dinosaur there - click one, then press %ls"), 3.0f);
        }
    }
    for (int p = 0; p < 2; ++p) if (g_ctl[p]) TickSlot(p, dt, rel);
}

} // namespace dinocontrol
