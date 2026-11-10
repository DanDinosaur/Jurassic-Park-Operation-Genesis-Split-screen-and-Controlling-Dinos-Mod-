// builder2.cpp - see builder2.h. JPOG46, JPOG47 (+ road add-ons and fence add-ons), JPOG48 (+ paths and fences).
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "builder2.h"
#include "xinput_pad.h"
#include "game.h"
#include "gamectl.h"
#include "splitscreen.h"
#include "guard.h"
#include "log.h"

namespace builder2 {
namespace {
    constexpr uintptr_t kCtor = 0x0052B3F0, kCreateDecals = 0x0052E020, kRenderMeshes = 0x0052E360;
    // JPOG47: the two subclasses of the same tool (vftables 0xD1A2C0 / 0xD1C920 checked from the EXE: same slots
    // +0x08/+0x54/+0x58/+0x5C/+0x60/+0x68 as ABuildPlacementState).
    constexpr uintptr_t kRoadCtor = 0x00558DE0;   // ARoadAddonPlacementState(const char*), 0x150 bytes, ret 4
    constexpr uintptr_t kWallCtor = 0x00569FD0;   // AWallAddonPlacementState(WALLADDON_TYPE, const char*), 0x18C, ret 8
    constexpr uintptr_t kRoadSetVis = 0x005594F0; // ARoadAddonPlacementState::SetModelVisible(bool, bool), ret 8
    constexpr uintptr_t kApp = 0x008B8580, kGetRootTask = 0x00484DB0, kGetAnimSys = 0x0047D390, kDeInst = 0x00658AB0;
    using fn_wctor = void(__fastcall*)(void* self, void* edx, int type, const char* unit);
    using fn_vis   = void(__fastcall*)(void* self, void* edx, bool a, bool b);
    using fn_get   = void*(__fastcall*)(void* self, void* edx);
    using fn_deinst= void(__fastcall*)(void* self, void* edx, void* node, bool b);
    constexpr uintptr_t kGetTargetPos = 0x0050C770;   // ACamera::GetTargetPosition(TVector3*)
    using fn_ctor  = void(__fastcall*)(void* self, void* edx, const char* unit);
    using fn_v     = void(__fastcall*)(void* self, void* edx);
    using fn_b     = bool(__fastcall*)(void* self, void* edx);
    using fn_bp    = bool(__fastcall*)(void* self, void* edx, const float* pos);
    using fn_place = void*(__fastcall*)(void* self, void* edx, bool* done);
    using fn_rot   = void(__fastcall*)(void* self, void* edx, int dir);
    using fn_del   = void*(__fastcall*)(void* self, void* edx, unsigned flags);
    // JPOG48 (stage 2b): the drag tools. APathLayingState (ctor 0x5572B0 (ARoadPoint* = 0), 0xE4 bytes, vftable
    // 0xD1A0C0) and AWallLayingState (ctor 0x56AAA0 (WALLSIZE 0..2 = wt1/wt2/wt3, AWallUnitNode* = 0), 0x178 bytes,
    // vftable 0xD1CC20). Both: +0x08 deleting dtor, +0x18 ProcessCommand(cmd, widget) (0x71 = place / start, 0x72 =
    // cancel the drag), +0x1C OnUpdate(dt). Their OnUpdate reads the ONE global aim (ATarget::GetTargettedTerrain) and
    // AScrollingState::OnUpdate scrolls the ACTIVE camera - so around every call: the aim cache (ATarget +0x3C..+0x48,
    // +0x48 = "computed this frame") holds this player's aim point, the scrolling flags (+0x48 mouse edge, +0x4A keys)
    // are 0, the HUD money preview is off (APlayerInfo +0x0D), and the unit-selection type the fence tool sets while
    // hovering a fence (AUnitSelection +0x28) is put back. Everything is restored right after.
    constexpr uintptr_t kPathCtor = 0x005572B0, kPathCmd = 0x00557410, kPathUpd = 0x00557910;
    constexpr uintptr_t kWallCtor2 = 0x0056AAA0, kWallCmd = 0x0056B050, kWallUpd = 0x0056B630;
    constexpr uintptr_t kRootTaskPtr = 0x008B859C, kPlayerInfoPtr = 0x0085004C, kUnitSelPtr = 0x008500B0;
    constexpr uintptr_t kSetSelType = 0x00527850;   // AUnitSelection::SetSelectionType(type), thiscall ret 4
    using fn_pctor = void(__fastcall*)(void* self, void* edx, void* roadPoint);
    using fn_wlctor= void(__fastcall*)(void* self, void* edx, int size, void* node);
    using fn_cmd   = bool(__fastcall*)(void* self, void* edx, int cmd, void* widget);
    using fn_upd   = bool(__fastcall*)(void* self, void* edx, float dt);
    using fn_seti  = void(__fastcall*)(void* self, void* edx, int v);
    // JPOG73: player 2's build tool aims at their mouse pointer (vanilla PC placement) instead of the camera's middle.
    void TargetOf(void* cam, float* out) {
        reinterpret_cast<void(__fastcall*)(void*, void*, float*)>(game::At(kGetTargetPos))(cam, nullptr, out);
        float g[3];
        if (cam && cam == splitscreen::CameraOfPlayer(1) && splitscreen::PointerGround(g)) { out[0] = g[0]; out[1] = g[1]; out[2] = g[2]; }
    }
    struct AimSwap {
        uint8_t* tgt = nullptr; uint8_t saveT[16]{}; uint8_t* pi = nullptr; uint8_t savePI = 0;
        uint8_t* sel = nullptr; int saveSel = 0;
        bool unitT = false; uint8_t saveU[24]{}; int saveMode = 0;   // JPOG50: ATarget unit pick (+0x24 pos/ray, +0x4C mode)
        AimSwap(void* st, const float* pt, bool unitTarget = false) {
            *((uint8_t*)st + 0x48) = 0; *((uint8_t*)st + 0x4A) = 0;
            uint8_t* root = *reinterpret_cast<uint8_t**>(game::At(kRootTaskPtr));
            uint8_t* gc = root ? *reinterpret_cast<uint8_t**>(root + 0x2C) : nullptr;
            tgt = gc ? *reinterpret_cast<uint8_t**>(gc + 0x2C) : nullptr;
            if (tgt) { memcpy(saveT, tgt + 0x3C, 16); memcpy(tgt + 0x3C, pt, 12); tgt[0x48] = 1; }
            // JPOG50: ATarget::GetTargettedUnit 0x520170 = mode +0x4C: 0 = nearest unit to the position at +0x24 (radius 15),
            // 1 = ray from +0x24 along +0x30 (the HUD player's cursor). The demolish tool picks buildings with it.
            if (tgt && unitTarget) { unitT = true; memcpy(saveU, tgt + 0x24, 24); saveMode = *reinterpret_cast<int*>(tgt + 0x4C);
                                     memcpy(tgt + 0x24, pt, 12); *reinterpret_cast<int*>(tgt + 0x4C) = 0; }
            pi = *reinterpret_cast<uint8_t**>(game::At(kPlayerInfoPtr));
            if (pi) { savePI = pi[0x0D]; pi[0x0D] = 0; }
            sel = *reinterpret_cast<uint8_t**>(game::At(kUnitSelPtr));
            if (sel) saveSel = *reinterpret_cast<int*>(sel + 0x28);
        }
        ~AimSwap() {
            if (tgt) memcpy(tgt + 0x3C, saveT, 16);
            if (tgt && unitT) { memcpy(tgt + 0x24, saveU, 24); *reinterpret_cast<int*>(tgt + 0x4C) = saveMode; }
            if (pi) pi[0x0D] = savePI;
            if (sel && *reinterpret_cast<int*>(sel + 0x28) != saveSel)
                reinterpret_cast<fn_seti>(game::At(kSetSelType))(sel, nullptr, saveSel);
        }
        bool ok() const { return tgt != nullptr; }
    };
    template <class F> F V(void* o, int off) { return *reinterpret_cast<F*>(*reinterpret_cast<uint8_t**>(o) + off); }
    uint8_t* Info(void* st) { return *reinterpret_cast<uint8_t**>((uint8_t*)st + 0x60); }

    // kind 0 = stand-alone building (ABuildPlacementState), 1 = road add-on on a path (ARoadAddonPlacementState,
    // names from Data\roadadd.ini "manual types" + ARoadNetwork::GetBinRoadAddonIndex), 2 = fence add-on
    // (AWallAddonPlacementState; type from AWallManager::GetWallAddonType: gate 0, vent 1, safari 3).
    // Paths, fences, terrain and flora use their own drag tools (OnUpdate reads the ONE global target) - stage 2b.
    // kind 3 = path (drag tool), 4 = fence (drag tool, wallType = WALLSIZE 0..2 = the game's wt1/wt2/wt3 fences).
    struct Item { const char* unit; const wchar_t* name; int kind; int wallType; };
    const Item kItems[] = {
        { "path", L"Path", 3, 0 }, { "wt1", L"Fence (type 1)", 4, 0 }, { "wt2", L"Fence (type 2)", 4, 1 },
        { "wt3", L"Fence (type 3)", 4, 2 },
        { "kiosk", L"Kiosk", 0, 0 }, { "shop", L"Shop", 0, 0 }, { "toilet", L"Toilets", 0, 0 },
        { "picnic", L"Picnic area", 0, 0 }, { "fountn", L"Fountain", 0, 0 },
        { "bin", L"Bin (on a path)", 1, 0 }, { "bench", L"Bench (on a path)", 1, 0 },
        { "viewplat", L"Viewing platform", 0, 0 }, { "bllnride", L"Balloon ride", 0, 0 },
        { "gate", L"Gate (on a fence)", 2, 0 }, { "vent", L"Viewing vent (on a fence)", 2, 1 },
        { "safari", L"Safari ride depot (on a fence)", 2, 3 }, { "hvdome", L"Observation dome", 0, 0 },
        { "hatchery", L"Hatchery", 0, 0 },
        { "herbfdsp", L"Herbivore feeder", 0, 0 }, { "carnfdsp", L"Carnivore feeder", 0, 0 }, { "lurebld", L"Lure", 0, 0 },
        { "maintdep", L"Maintenance", 0, 0 }, { "rangerdp", L"Ranger station", 0, 0 }, { "shelter", L"Shelter", 0, 0 },
        { "scamera", L"Security camera", 0, 0 }, { "abeacbld", L"Beacon", 0, 0 }, { "turret", L"Turret", 0, 0 },
        { "Demolish", L"Demolish", 5, 0 },   // JPOG50: from the disc only (the X list skips it)
        // JPOG52 (owner order 10:36 item 1): trees and terrain, from the disc only (wallType = the ctor argument,
        // as AGameMenuState::DoMenuCommand 0x538C20 passes it).
        { "PaleoFlora", L"Prehistoric trees", 6, 1 }, { "ModernFlora", L"Modern trees", 6, 0 },
        { "Lake", L"Lake", 7, 0 }, { "Mountain", L"Mountain", 7, 1 }, { "Hill", L"Hill", 7, 2 }, { "Flatten", L"Flatten", 7, 3 },
    };
    // kind 5 = JPOG50 demolish (ADemolishState(0xF, true), ctor 0x530700, 0xBC bytes) - see Demolish() below.
    int Size(int kind) { return kind == 1 ? 0x150 : kind == 2 ? 0x18C : kind == 3 ? 0xE4 : kind == 4 ? 0x178 : kind == 5 ? 0xBC
                              : kind == 6 ? 0xE0 : kind == 7 ? 0xCC : 0x140; }
    // JPOG52 kind 6 = AFloraPlacementState (ctor 0x536920 (bool paleo), 0xE0): OnUpdate 0x536C70 = AScrollingState::OnUpdate
    // + the ONE global aim -> this+0x60/+0x64 (x/z) + UpdateCursor 0x537230 (one-pass AddUIFootPrint grid + money preview).
    // PlaceFlora 0x536EC0 (thiscall(cmd), ret 4 checked): 0x8E paleo / 0x8F modern trees, 0x91 removes trees; brush size
    // this+0xD4 (1..3). The static AFloraPlacementState::sm_bPaleo 0x84E554 (set by the ctor, read by UpdateCursor) is
    // player 1's too: swapped around every call. Never OnInsertion/OnActivate (HUD + camera).
    // kind 7 = ATerraformPreset (ctor 0x563850 (type 0 Lake / 1 Mountain / 2 Hill / 3 Flatten), 0xCC): OnUpdate 0x563DD0
    // reads the ONE aim, writes the preview into the ONE ATerrainSelection (RenderTerrainMesh: +0x74..+0x98, drawn by
    // ATerrainSelection::Render 0x522FE0 only while that task is active (+0x1C bit 2) and +0x94 == 1), and clears the HUD
    // hint (SetHintMessage(0,0)) whenever its timer +0xB0 runs out -> +0xB0 kept at 100 s. ProcessCommand 0x563A40 (ret 8):
    // 0x71 = terraform at the aim (one press: +0x98 = 1 since the ctor), 0x1B/0x1C = size. The selection state is saved and
    // put back around every call; for player 2's own pass the task bit is set and the preview queued (SelSave / AfterPass).
    constexpr uintptr_t kFloraCtor = 0x00536920, kFloraUpd = 0x00536C70, kFloraPlace = 0x00536EC0, kFloraPaleo = 0x0084E554;
    constexpr uintptr_t kTerraCtor = 0x00563850, kTerraUpd = 0x00563DD0, kTerraCmd = 0x00563A40;
    constexpr uintptr_t kTerrainSelPtr = 0x008500AC, kTerraformClass = 0x008B55E0;
    struct PaleoSwap { uint8_t* g; uint8_t save; PaleoSwap(bool paleo) : g((uint8_t*)game::At(kFloraPaleo)), save(*g) { *g = paleo ? 1 : 0; }
                       ~PaleoSwap() { *g = save; } };
    struct SelSave {   // the ONE terrain selection's terraform preview + task-active bit
        uint8_t* sel = nullptr; uint8_t mesh[0x28]{}; uint32_t flags = 0;
        SelSave() { sel = *reinterpret_cast<uint8_t**>(game::At(kTerrainSelPtr));
                    if (sel) { memcpy(mesh, sel + 0x74, sizeof mesh); flags = *reinterpret_cast<uint32_t*>(sel + 0x1C); } }
        void Restore() { if (sel) { memcpy(sel + 0x74, mesh, sizeof mesh); *reinterpret_cast<uint32_t*>(sel + 0x1C) = flags; } }
    };
    bool g_selHeld[2]{}; SelSave g_selKeep[2];
    bool Drag(int kind) { return kind == 3 || kind == 4; }
    bool Tool(int kind) { return kind == 6 || kind == 7; }   // JPOG52: trees / terrain (per-frame OnUpdate in the render hook)
    bool NoTraits(int kind) { return kind >= 3; }
    void Construct(void* mem, const Item& it) {
        if (it.kind == 6) {
            PaleoSwap ps(it.wallType != 0);   // the ctor writes the static - player 1's flora tool must keep its own
            reinterpret_cast<void(__fastcall*)(void*, void*, bool)>(game::At(kFloraCtor))(mem, nullptr, it.wallType != 0);
        } else if (it.kind == 7) {
            reinterpret_cast<void(__fastcall*)(void*, void*, int)>(game::At(kTerraCtor))(mem, nullptr, it.wallType);
        } else if (it.kind == 5) {
            reinterpret_cast<void(__fastcall*)(void*, void*, unsigned, bool)>(game::At(0x00530700))(mem, nullptr, 0xF, true);
        } else if (it.kind == 3) {
            reinterpret_cast<fn_pctor>(game::At(kPathCtor))(mem, nullptr, nullptr);
        } else if (it.kind == 4) {
            reinterpret_cast<fn_wlctor>(game::At(kWallCtor2))(mem, nullptr, it.wallType, nullptr);
            *((uint8_t*)mem + 0xD4) = 0;   // "first wall" flag: would call AGUIGameHUD::SetContextCommands (the HUD's)
        }
        else if (it.kind == 1)      reinterpret_cast<fn_ctor>(game::At(kRoadCtor))(mem, nullptr, it.unit);
        else if (it.kind == 2) reinterpret_cast<fn_wctor>(game::At(kWallCtor))(mem, nullptr, it.wallType, it.unit);
        else                   reinterpret_cast<fn_ctor>(game::At(kCtor))(mem, nullptr, it.unit);
    }
    // The road add-on's cursor model is freed in its OnRemoval, not its dtor. OnRemoval also runs
    // AGameState::OnRemoval (game-controller ref count) which a private tool must NOT run - so free the model
    // the same way ARoadAddonPlacementState::OnRemoval does (0x558E50), without the base call.
    void FreeRoadModel(void* st) {
        void*& model = *reinterpret_cast<void**>((uint8_t*)st + 0x140);
        if (!model) return;
        reinterpret_cast<fn_vis>(game::At(kRoadSetVis))(st, nullptr, false, false);
        void* root = reinterpret_cast<fn_get>(game::At(kGetRootTask))(reinterpret_cast<void*>(game::At(kApp)), nullptr);
        void* anim = reinterpret_cast<fn_get>(game::At(kGetAnimSys))(root, nullptr);
        void* repos = V<fn_get>(anim, 0xC4)(anim, nullptr);
        reinterpret_cast<fn_deinst>(game::At(kDeInst))(repos, nullptr, model, true);
        model = nullptr;
    }
    constexpr int kCount = sizeof(kItems) / sizeof(kItems[0]);
    bool g_bad[kCount]{};            // LoadTraits refused it this session
    // JPOG49: a building picked on player 2's disc that is not in kItems (item = -1): its unit name from the menu item.
    char g_dynUnit[2][32]{}; wchar_t g_dynName[2][32]{};
    Item g_dyn[2] = { { g_dynUnit[0], g_dynName[0], 0, 0 }, { g_dynUnit[1], g_dynName[1], 0, 0 } };

    struct Slot {
        void* st = nullptr; int item = 0; bool posOk = false, valid = false, meshOk = false;
        float last[3]{ 1e9f, 0, 1e9f }; int lastRot = -1; DWORD hintAt = 0; int placed = 0;
        float aim[3]{}; bool aimOk = false;   // JPOG48: drag tools - this player's aim, for the render-hook OnUpdate
        bool fromMenu = false; int closeReason = 0; int who = 0;   // JPOG49: opened from the player's own disc
    } g_s[2];
    const Item& It(const Slot& s) { return s.item < 0 ? g_dyn[s.who] : kItems[s.item]; }
    int g_logs = 0, g_fails = 0; bool g_off = false;

    void Destroy(Slot& s, bool worldGone) {
        if (!s.st) return;
        void* st = s.st; s.st = nullptr; s.meshOk = false; s.posOk = false;
        if (worldGone) { LOG("builder2: tool %p left behind (world already gone)", st); return; }
        const bool road = It(s).kind == 1;
        bool ok = GUARDED("builder2 destroy", { if (road) FreeRoadModel(st); V<fn_del>(st, 0x08)(st, nullptr, 1); });
        LOG("builder2: tool %p destroyed (ok=%d)", st, ok);
    }
    bool Create(Slot& s, int who, bool exact = false) {
        s.who = who;
        for (int n = 0; n < kCount; ++n, s.item = (s.item + 1) % kCount) {
            if (s.item >= 0 && g_bad[s.item]) { if (exact) return false; continue; }
            if (!exact && s.item >= 0 && kItems[s.item].kind >= 5) continue;   // demolish / trees / terrain only from the disc
            void* mem = gamectl::Alloc(Size(It(s).kind));
            if (!mem) { LOG("builder2: tmalloc failed"); return false; }
            bool traits = false;
            bool ok = GUARDED("builder2 create", {
                Construct(mem, It(s));
                if (NoTraits(It(s).kind)) traits = true;       // no traits / decal array for drag tools / demolish
                else {
                    traits = V<fn_b>(mem, 0x60)(mem, nullptr);          // LoadTraits
                    if (traits) reinterpret_cast<fn_v>(game::At(kCreateDecals))(mem, nullptr);
                }
            });
            if (ok && traits) {
                s.st = mem; s.posOk = s.valid = s.meshOk = s.aimOk = false; s.lastRot = -1; s.last[0] = 1e9f;
                LOG("builder2: player %d's own build tool = %s (%p)", who + 1, It(s).unit, mem);
                return true;
            }
            LOG("builder2: %s refused (ok=%d traits=%d) - skipped this session", It(s).unit, ok, traits);
            if (ok) GUARDED("builder2 free refused", { if (It(s).kind == 1) FreeRoadModel(mem); V<fn_del>(mem, 0x08)(mem, nullptr, 1); });
            if (!ok && ++g_fails >= 3) { g_off = true; LOG("builder2: faulted 3x - off for this session"); return false; }
            if (s.item >= 0) g_bad[s.item] = true;
            if (exact) return false;
        }
        return false;
    }
    // JPOG50 (owner 10:29): player 2 demolishes while player 1 is NOT in demolish mode. A private ADemolishState, used
    // ONE-SHOT per A press (never per frame - its selection lives in the game's ONE AUnitSelection, whose listeners drive
    // player 1's HUD/camera): AimSwap (terrain aim + unit pick at this player's aim) + the HUD player's selection saved,
    // OnUpdate 0x530C20 (picks what is under the aim, InitializeSelection), then the action, DeinitializeSelection,
    // selection put back - all inside one game frame. Vanilla asks "are you sure?" for buildings / fence add-ons with a
    // popup on the game's state chain (player 1's); player 2 confirms by pressing A twice instead, no popup.
    //   buildings: DemolishBuildingRefund 0x530FF0 (this, unit) + AUnitManager::DestroyUnit 0x49F7D0 + sound 5 (as
    //   OnDemolishBuilding 0x530F90 does); fences / fence add-ons: HasValidSelectedWall 0x5321E0 -> DestroySelectedWallRegion
    //   0x531E70; paths, trees: DestroySelection 0x5316E0 (false) (no popup on those branches).
    constexpr uintptr_t kDemUpd = 0x00530C20, kDemDeinit = 0x005315A0, kDemDestroy = 0x005316E0, kDemRefund = 0x00530FF0;
    constexpr uintptr_t kDemValidWall = 0x005321E0, kDemWallRegion = 0x00531E70, kDestroyUnit = 0x0049F7D0;
    constexpr uintptr_t kUnitMgr = 0x0084C3E0, kPlaySound = 0x00690080, kGetSelUnit = 0x00526930, kSetSelUnit = 0x00526950;
    constexpr uintptr_t kBuildingClass = 0x008B7A60;
    struct Confirm { void* unit = nullptr; DWORD t = 0; } g_conf[2];
    // returns: 0 nothing there, 1 removed, 2 building waiting for the 2nd A, 3 can't be demolished; -1 fault
    int Demolish(Slot& s, int who, const float* pt) {
        void* st = s.st;
        uint8_t* sel = *reinterpret_cast<uint8_t**>(game::At(kUnitSelPtr));
        if (!sel) return -1;
        auto getSel = reinterpret_cast<void*(__fastcall*)(void*, void*)>(game::At(kGetSelUnit));
        auto setSel = reinterpret_cast<void(__fastcall*)(void*, void*, const void*)>(game::At(kSetSelUnit));
        void* keepU = getSel(sel, nullptr); const int keepT = *reinterpret_cast<int*>(sel + 0x28);
        int res = -1; void* gone = nullptr;
        {
            AimSwap sw(st, pt, true);
            if (!sw.ok()) return -1;
            GUARDED("builder2 demolish", {
                *((uint8_t*)st + 0x48) = 0; *((uint8_t*)st + 0x4A) = 0;
                reinterpret_cast<fn_upd>(game::At(kDemUpd))(st, nullptr, 0.016f);
                void* u = getSel(sel, nullptr);
                res = 0;
                if (u && u != keepU && gamectl::IsA(u, kBuildingClass)) {
                    if (*((uint8_t*)u + 0xED)) res = 3;
                    else if (g_conf[who].unit != u || GetTickCount() - g_conf[who].t > 4000) { g_conf[who].unit = u; g_conf[who].t = GetTickCount(); res = 2; }
                    else {
                        g_conf[who].unit = nullptr;
                        reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(game::At(kDemRefund))(st, nullptr, u);
                        reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(game::At(kDestroyUnit))(*reinterpret_cast<void**>(game::At(kUnitMgr)), nullptr, u);
                        uint8_t* root = *reinterpret_cast<uint8_t**>(game::At(kRootTaskPtr));
                        if (root) reinterpret_cast<int(__fastcall*)(void*, void*, int)>(game::At(kPlaySound))(*reinterpret_cast<void**>(root + 0xDC), nullptr, 5);
                        gone = u; res = 1;
                    }
                } else if (reinterpret_cast<bool(__fastcall*)(void*, void*)>(game::At(kDemValidWall))(st, nullptr)) {
                    reinterpret_cast<void(__fastcall*)(void*, void*)>(game::At(kDemWallRegion))(st, nullptr); res = 1;
                } else {
                    const unsigned what = *reinterpret_cast<unsigned*>((uint8_t*)st + 0x90);
                    if (what) { reinterpret_cast<void(__fastcall*)(void*, void*, bool)>(game::At(kDemDestroy))(st, nullptr, false); res = 1; }
                }
                reinterpret_cast<void(__fastcall*)(void*, void*)>(game::At(kDemDeinit))(st, nullptr);
                *reinterpret_cast<unsigned*>((uint8_t*)st + 0x90) = 0;   // next press recomputes the selection from scratch
            });
        }
        GUARDED("builder2 demolish restore selection", {
            if (keepU == gone) keepU = nullptr;
            if (getSel(sel, nullptr) != keepU) setSel(sel, nullptr, keepU);
            if (*reinterpret_cast<int*>(sel + 0x28) != keepT) reinterpret_cast<fn_seti>(game::At(kSetSelType))(sel, nullptr, keepT);
        });
        return res;
    }

    bool Started(const Slot& s) {   // drag tools: a start point is set
        if (It(s).kind == 3) return (*reinterpret_cast<uint16_t*>((uint8_t*)s.st + 0xBA) & 1) != 0;
        if (It(s).kind == 4) return *((uint8_t*)s.st + 0x78) != 0;
        return false;
    }
    // JPOG52: one OnUpdate of the tree / terrain tool with this player's aim (render hook = queue for their own pass;
    // game thread = right before an action). The terrain preview is left in the selection only when keep = true.
    bool ToolUpdate(Slot& s, float dt) {
        if (!s.aimOk) return true;
        AimSwap sw(s.st, s.aim);
        if (!sw.ok()) return true;
        void* st = s.st; const Item& it = It(s);
        const float d = dt > 0.f && dt < 0.5f ? dt : 0.016f;
        if (it.kind == 6) {
            PaleoSwap ps(it.wallType != 0);
            return GUARDED("builder2 flora update", { reinterpret_cast<fn_upd>(game::At(kFloraUpd))(st, nullptr, d); });
        }
        *reinterpret_cast<float*>((uint8_t*)st + 0xB0) = 100.0f;   // no SetHintMessage(0,0) on the HUD player's hint
        return GUARDED("builder2 terrain update", { reinterpret_cast<fn_upd>(game::At(kTerraUpd))(st, nullptr, d); });
    }
    // The guard is INSIDE the swap: a fault longjmps out of the guard only, so ~AimSwap always restores the globals.
    // Both return false only on a fault.
    bool Cmd(Slot& s, int cmd) {
        if (!s.aimOk) return true;
        AimSwap sw(s.st, s.aim);
        if (!sw.ok()) return true;
        void* st = s.st; const uintptr_t f = It(s).kind == 3 ? kPathCmd : kWallCmd;
        return GUARDED("builder2 drag cmd", { reinterpret_cast<fn_cmd>(game::At(f))(st, nullptr, cmd, nullptr); });
    }
    bool DragUpdate(Slot& s, float dt) {
        if (!s.aimOk) return true;
        AimSwap sw(s.st, s.aim);
        if (!sw.ok()) return true;
        void* st = s.st; const uintptr_t f = It(s).kind == 3 ? kPathUpd : kWallUpd;
        const float d = dt > 0.f && dt < 0.5f ? dt : 0.016f;
        return GUARDED("builder2 drag update", { reinterpret_cast<fn_upd>(game::At(f))(st, nullptr, d); });
    }
    void Hint(int who, const Slot& s, const wchar_t* extra) {
        wchar_t t[200];
        if (s.fromMenu)   // JPOG49: picked on the player's own disc - short, the same buttons as the HUD player
            gamectl::WFmt(t, 200, It(s).kind == 5 ? L"Player %d: %ls%ls   (aim, A = knock down, B back)" :
                             It(s).kind == 6 ? L"Player %d: %ls%ls   (A plant, Y remove trees, LB/RB size, B back)" :
                             It(s).kind == 7 ? L"Player %d: %ls%ls   (A shape the land, LB/RB size, B back)" :
                             Drag(It(s).kind) ? L"Player %d: %ls%ls   (A start / A finish, B back)"
                                              : L"Player %d: %ls%ls   (A place, LB/RB rotate, B back)", who + 1,
                     It(s).name, extra ? extra : L"");
        else if (Drag(It(s).kind))
            gamectl::WFmt(t, 200, L"Player %d lays: %ls%ls   (A start / A finish, B cancel, D-pad < > change, X close)", who + 1,
                     It(s).name, extra ? extra : L"");
        else
        gamectl::WFmt(t, 200, L"Player %d builds: %ls%ls   (D-pad < > change, LB/RB rotate, A place, B close)", who + 1,
                 It(s).name, extra ? extra : L"");
        if (gamectl::ShowHintNow(t, 4.0f)) splitscreen::HintToHalf(who, 4.0f);
    }
}

bool Open(int who) { return who >= 0 && who < 2 && g_s[who].st != nullptr; }
void Close(int who, const char* why, bool worldGone) {
    if (who < 0 || who > 1 || !g_s[who].st) return;
    LOG("builder2: player %d closes the build tool (%s, %d placed)", who + 1, why, g_s[who].placed);
    Destroy(g_s[who], worldGone);
}
// JPOG49: open the tool for a leaf item picked on the player's own copy of the game's build disc. The mapping is the
// one AGameMenuState::DoMenuCommand 0x538C20 uses: "road" = path tool, wall addon names = fence add-on tool, wall section
// names (wt1sct90..) = fence tool, "bin"/"bench" = road add-on, item type 3 = building with the item's unit name.
bool OpenFor(int who, const char* cmd, int itemType) {
    if (who < 0 || who > 1 || g_off || !cmd || !*cmd) return false;
    Slot& s = g_s[who];
    if (s.st) Close(who, "new item from the disc");
    s.closeReason = 0;
    int idx = -2;
    char key[32]; strncpy(key, cmd, 31); key[31] = 0;
    if (!_stricmp(key, "road")) strcpy(key, "path");
    if (!_strnicmp(key, "wt", 2) && key[2] >= '1' && key[2] <= '3') key[3] = 0;     // wt1sct90 -> wt1
    for (int i = 0; i < kCount; ++i) if (!_stricmp(kItems[i].unit, key)) { idx = i; break; }
    if (idx < 0) {
        if (itemType != 3) return false;                  // not a building: Demolish, terrain, trees ... (stage 2c)
        idx = -1;
        strncpy(g_dynUnit[who], cmd, 31); g_dynUnit[who][31] = 0;
        gamectl::WFmt(g_dynName[who], 32, L"%hs", cmd);
    }
    s.item = idx; s.fromMenu = true;
    if (!Create(s, who, true)) { s.fromMenu = false; s.item = 0; LOG("builder2: player %d can't use '%s' from the disc", who + 1, cmd); return false; }
    s.placed = 0; s.hintAt = 0;
    return true;
}
int TakeCloseReason(int who) {
    if (who < 0 || who > 1) return 0;
    int r = g_s[who].closeReason; g_s[who].closeReason = 0;
    if (!g_s[who].st) g_s[who].fromMenu = false;
    return r;
}
void CloseAll(const char* why, bool worldGone) { Close(0, why, worldGone); Close(1, why, worldGone); }

bool Update(int who, XPad& pad, void* cam, float dt) {
    if (who < 0 || who > 1 || g_off) return false;
    Slot& s = g_s[who];
    if (!s.st) {
        if (!pad.Pressed(PAD_X) || !cam) return false;
        s.fromMenu = false; if (s.item < 0) s.item = 0;
        if (!Create(s, who)) return false;
        s.placed = 0; s.hintAt = 0;
        return true;
    }
    if (Drag(It(s).kind) && pad.Pressed(PAD_B) && Started(s)) {   // JPOG48: B first cancels the drag
        if (!Cmd(s, 0x72)) { Close(who, "faulted"); if (++g_fails >= 3) g_off = true; }
        return true;
    }
    if (s.fromMenu && (pad.Pressed(PAD_B) || pad.Pressed(PAD_X))) {   // JPOG49: B = back to the disc, X = done
        const int why = pad.Pressed(PAD_B) ? 1 : 2;
        Close(who, why == 1 ? "B (back to the disc)" : "X");
        s.closeReason = why;
        return true;
    }
    if (pad.Pressed(PAD_B) || pad.Pressed(PAD_X)) { Close(who, "B/X"); if (gamectl::ShowHintNow(L"Build tool closed", 1.5f)) splitscreen::HintToHalf(who, 1.5f); return true; }
    int step = s.fromMenu ? 0 : pad.Pressed(PAD_DRIGHT) ? 1 : pad.Pressed(PAD_DLEFT) ? -1 : 0;
    if (step) {
        const bool wasDrag = Drag(It(s).kind);
        int keepRot = (!wasDrag && Info(s.st)) ? Info(s.st)[0x2E] : 0;
        Destroy(s, false);
        s.item = (s.item + step + kCount) % kCount;
        if (step < 0) {   // Create() walks forward past refused items - walk backwards here
            for (int n = 0; n < kCount && g_bad[s.item]; ++n) s.item = (s.item - 1 + kCount) % kCount;
        }
        if (!Create(s, who)) return true;
        if (!Drag(It(s).kind)) { if (uint8_t* in = Info(s.st)) in[0x2E] = (uint8_t)(keepRot & 3); }
        s.hintAt = 0;
    }
    void* st = s.st;
    float pt[3]{};
    if (Tool(It(s).kind)) {   // JPOG52: trees / terrain
        const bool terra = It(s).kind == 7;
        float a[3]{};
        bool ok = GUARDED("builder2 tool aim", {
            TargetOf(cam, a); });
        s.aimOk = ok && std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]);
        if (s.aimOk) memcpy(s.aim, a, sizeof a);
        if (terra) {   // player 1 shaping the land at the same time: the ONE terrain selection would be shared
            void* cs = gamectl::CurrentState();
            if (cs && gamectl::IsA(cs, kTerraformClass)) {
                wchar_t t2[120]; gamectl::WFmt(t2, 120, L"Player %d is shaping the land - only one player at a time", 2 - who);
                Close(who, "the other player is terraforming"); s.closeReason = 1;
                if (gamectl::ShowHintNow(t2, 3.0f)) splitscreen::HintToHalf(who, 3.0f);
                return true;
            }
        }
        void* st = s.st; int act = 0;   // 1 = A, 2 = Y (remove trees), 3 = size
        if (pad.Pressed(PAD_RB) || pad.Pressed(PAD_LB)) {
            const bool up = pad.Pressed(PAD_RB);
            act = 3;
            if (!terra) { int& b = *reinterpret_cast<int*>((uint8_t*)st + 0xD4); b = up ? (b < 3 ? b + 1 : 3) : (b > 1 ? b - 1 : 1); }
            else ok = GUARDED("builder2 terrain size", { reinterpret_cast<fn_cmd>(game::At(kTerraCmd))(st, nullptr, up ? 0x1B : 0x1C, nullptr); });
        }
        if (ok && s.aimOk && (pad.Pressed(PAD_A) || (!terra && pad.Pressed(PAD_Y)))) {
            act = pad.Pressed(PAD_A) ? 1 : 2;
            SelSave keep;
            if (terra) ok = ToolUpdate(s, dt);   // aim -> the tool's own fields (flora: set directly below - its OnUpdate
                                                 // would queue its footprint grid into player 1's pass)
            if (ok) {
                AimSwap sw(st, s.aim);
                const Item& it = It(s);
                if (!terra) {
                    PaleoSwap ps(it.wallType != 0);
                    *reinterpret_cast<float*>((uint8_t*)st + 0x60) = s.aim[0]; *reinterpret_cast<float*>((uint8_t*)st + 0x64) = s.aim[2];
                    const int cmd = act == 2 ? 0x91 : (it.wallType ? 0x8E : 0x8F);
                    ok = GUARDED("builder2 flora place", { reinterpret_cast<bool(__fastcall*)(void*, void*, int)>(game::At(kFloraPlace))(st, nullptr, cmd); });
                } else {
                    *reinterpret_cast<float*>((uint8_t*)st + 0xB0) = 100.0f;
                    ok = GUARDED("builder2 terraform", { reinterpret_cast<fn_cmd>(game::At(kTerraCmd))(st, nullptr, 0x71, nullptr); });
                }
            }
            keep.Restore();   // player 1's terraform preview / task bit as they were
            if (ok && act == 1) ++s.placed;
            LOG("builder2: player %d %s %s at (%.1f %.1f) (ok=%d)", who + 1, act == 2 ? "removes trees with" : "uses", It(s).unit, s.aim[0], s.aim[2], ok);
            splitscreen::HintToHalf(who, 3.0f);   // the game's own "not enough money" / "can't" hint goes to this player's half
        }
        if (!ok) { Close(who, "faulted"); if (++g_fails >= 3) { g_off = true; LOG("builder2: faulted 3x - off for this session"); } return true; }
        DWORD now = GetTickCount();
        if (now - s.hintAt > 4000 && !act) { s.hintAt = now; Hint(who, s, nullptr); }
        return true;
    }
    if (It(s).kind == 5) {   // JPOG50: demolish (player 1 must not be in demolish mode at the same time)
        if (gamectl::DemolishActive()) {
            wchar_t t[120]; gamectl::WFmt(t, 120, L"Player %d is using Demolish - only one player can demolish at a time", 2 - who);
            Close(who, "the other player started demolishing"); s.closeReason = 1;
            if (gamectl::ShowHintNow(t, 3.0f)) splitscreen::HintToHalf(who, 3.0f);
            return true;
        }
        {   // JPOG52 (owner 10:42: no demolish circle for player 2): this player's aim every frame, for the circle
            float a[3]{};
            const bool okC = GUARDED("builder2 demolish circle aim", {
                TargetOf(cam, a); });
            s.aimOk = okC && std::isfinite(a[0]) && std::isfinite(a[2]);
            if (s.aimOk) memcpy(s.aim, a, sizeof a);
        }
        if (!pad.Pressed(PAD_A)) {
            DWORD now = GetTickCount();
            if (now - s.hintAt > 4000) { s.hintAt = now; Hint(who, s, nullptr); }
            return true;
        }
        bool okA = GUARDED("builder2 demolish aim", {
            TargetOf(cam, pt); });
        if (!okA || !std::isfinite(pt[0]) || !std::isfinite(pt[2])) return true;
        const int r = Demolish(s, who, pt);
        LOG("builder2: player %d demolish at (%.1f %.1f) -> %d", who + 1, pt[0], pt[2], r);
        if (r == 1) ++s.placed;
        const wchar_t* t = r == 2 ? L"Press A again to knock it down" : r == 3 ? L"That one can't be knocked down" : nullptr;
        if (t && gamectl::ShowHintNow(t, 3.0f)) splitscreen::HintToHalf(who, 3.0f);
        if (r < 0) { Close(who, "faulted"); if (++g_fails >= 3) { g_off = true; LOG("builder2: faulted 3x - off for this session"); } }
        return true;
    }
    if (Drag(It(s).kind)) {   // JPOG48: path / fence drag tool
        const bool fence = It(s).kind == 4;
        bool ok = GUARDED("builder2 drag aim", {
            TargetOf(cam, pt);
        });
        s.aimOk = ok && std::isfinite(pt[0]) && std::isfinite(pt[1]) && std::isfinite(pt[2]);
        if (s.aimOk) memcpy(s.aim, pt, sizeof pt);
        if (ok && fence) ok = DragUpdate(s, dt);     // the fence cursors are models: update on the game path
        if (ok && pad.Pressed(PAD_A)) {
            const bool was = Started(s);
            ok = Cmd(s, 0x71);
            const bool now = Started(s);
            if (ok && was) ++s.placed;               // a finished drag (the game itself refuses blocked / too long ones)
            LOG("builder2: player %d A on %s at (%.1f %.1f) start %d -> %d (ok=%d)", who + 1, It(s).unit, pt[0], pt[2], was, now, ok);
        }
        if (!ok) { Close(who, "faulted"); if (++g_fails >= 3) { g_off = true; LOG("builder2: faulted 3x - off for this session"); } return true; }
        DWORD now = GetTickCount();
        if (now - s.hintAt > 3000) { s.hintAt = now; Hint(who, s, Started(s) ? L" - aim at the end point" : nullptr); }
        return true;
    }
    bool ok = GUARDED("builder2 update", {
        TargetOf(cam, pt);
        if (pad.Pressed(PAD_RB)) { V<fn_rot>(st, 0x68)(st, nullptr, 1); s.last[0] = 1e9f; }   // fence add-ons flip side
        if (pad.Pressed(PAD_LB)) { V<fn_rot>(st, 0x68)(st, nullptr, -1); s.last[0] = 1e9f; }  // (rotation set by the fence)
        uint8_t* in = Info(st);
        const int rot = in[0x2E];
        const bool moved = fabsf(pt[0] - s.last[0]) > 0.25f || fabsf(pt[2] - s.last[2]) > 0.25f || rot != s.lastRot;
        if (moved && std::isfinite(pt[0]) && std::isfinite(pt[2])) {
            s.posOk = V<fn_bp>(st, 0x58)(st, nullptr, pt);                  // DeterminePosition
            if (s.posOk) V<fn_v>(st, 0x5C)(st, nullptr);                     // CalculateBlockFlags
            else *reinterpret_cast<int*>(in + 0x40) = -1;
            s.last[0] = pt[0]; s.last[1] = pt[1]; s.last[2] = pt[2]; s.lastRot = rot; s.meshOk = true;
        }
        s.valid = s.posOk && *reinterpret_cast<int*>(in + 0x40) == 0;
        if (pad.Pressed(PAD_A)) {
            if (s.valid) {
                bool done = false;
                void* b = V<fn_place>(st, 0x54)(st, nullptr, &done);         // PlaceThing (cost, levelling, unit)
                if (done) ++s.placed;
                LOG("builder2: player %d placed %s at (%.1f %.1f) rot %d -> %p done=%d", who + 1, It(s).unit, pt[0], pt[2], rot, b, done);
                s.last[0] = 1e9f;                                            // recheck the spot next frame
                *reinterpret_cast<int*>(Info(st) + 0x40) = 1;
                s.valid = false;
            } else if (g_logs < 20) { ++g_logs; LOG("builder2: player %d A on a blocked spot (flags %d)", who + 1, *reinterpret_cast<int*>(in + 0x40)); }
        }
    });
    if (!ok) {
        Close(who, "faulted");
        if (++g_fails >= 3) { g_off = true; LOG("builder2: faulted 3x - off for this session"); }
        return true;
    }
    DWORD now = GetTickCount();
    if (now - s.hintAt > 3000) { s.hintAt = now; Hint(who, s, s.valid ? nullptr : L" - can't build here"); }
    return true;
}

void QueueFootprint(int who) {
    if (who < 0 || who > 1 || !g_s[who].st) return;
    if (It(g_s[who]).kind == 3) {   // JPOG48: the path tool queues its one-pass footprints in OnUpdate
        if (!DragUpdate(g_s[who], 0.016f)) {
            LOG("builder2: path update faulted for player %d", who + 1);
            Close(who, "faulted");
            if (++g_fails >= 3) { g_off = true; LOG("builder2: faulted 3x - off for this session"); }
        }
        return;
    }
    if (Tool(It(g_s[who]).kind)) {   // JPOG52: trees (one-pass grid) / terrain (preview mesh, taken back in AfterPass)
        Slot& s = g_s[who];
        if (It(s).kind == 7) { g_selKeep[who] = SelSave(); g_selHeld[who] = g_selKeep[who].sel != nullptr; }
        if (!ToolUpdate(s, 0.016f)) {
            if (g_selHeld[who]) { g_selKeep[who].Restore(); g_selHeld[who] = false; }
            LOG("builder2: tool update faulted for player %d", who + 1);
            Close(who, "faulted");
            if (++g_fails >= 3) { g_off = true; LOG("builder2: faulted 3x - off for this session"); }
            return;
        }
        if (g_selHeld[who] && g_selKeep[who].sel && s.aimOk) *reinterpret_cast<uint32_t*>(g_selKeep[who].sel + 0x1C) |= 2;   // draw it in this pass
        return;
    }
    if (It(g_s[who]).kind == 5) {   // JPOG52: the demolish tool's own red 4.0 circle (ADemolishState::OnUpdate draws it the same way)
        if (g_s[who].aimOk) splitscreen::QueueGroundCircle(g_s[who].aim, 4.0f, reinterpret_cast<const float*>(game::At(0x008B6290)));
        return;
    }
    if (!g_s[who].meshOk) return;
    void* st = g_s[who].st;
    if (!GUARDED("builder2 footprint", { reinterpret_cast<fn_v>(game::At(kRenderMeshes))(st, nullptr); })) {
        g_s[who].meshOk = false;
        LOG("builder2: footprint faulted for player %d", who + 1);
    }
}
// JPOG52: right after that player's pass - the ONE terrain selection back to player 1's state.
void AfterPass(int who) {
    if (who < 0 || who > 1 || !g_selHeld[who]) return;
    g_selHeld[who] = false;
    g_selKeep[who].Restore();
}
} // namespace builder2
