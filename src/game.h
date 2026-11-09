// game.h - verified SimJP.exe (retail) addresses, typedefs, and the [CONFIRM] list.
// Addresses are RVAs off the module base (preferred base 0x400000). We resolve them
// against the real runtime base so a relocated/patched EXE still works.
//
// Everything here with a plain address was confirmed from the decompile this session
// (see ..\symbols\SimJP.functions.csv). Items tagged [CONFIRM] are struct offsets /
// vtable slots that must be read from ..\src\SimJP.c before they are relied on; each
// names the exact function to read. Do NOT invent these - read them.
#pragma once
#include <cstdint>
#include <windows.h>

namespace game {

// Runtime module base of SimJP.exe (normally 0x400000; resolved for safety).
inline uintptr_t Base() {
    static uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return b;
}
// Convert a design-time VA (based at 0x400000) to a runtime pointer.
inline void* At(uintptr_t va_at_400000) {
    return reinterpret_cast<void*>(Base() + (va_at_400000 - 0x00400000u));
}

// ---- Verified function addresses (design-time VAs, base 0x400000) ----
namespace addr {
    // Frame / app
    constexpr uintptr_t ARootTask_OnUpdate          = 0x0047D3A0; // STUB (mov eax,1; ret 4) - NOT a frame tick, do not hook
    constexpr uintptr_t ARootTask_OnCreate          = 0x0047A580;
    constexpr uintptr_t ARootTask_GetInputSystem    = 0x0047D360; // -> AInputHandler*
    constexpr uintptr_t ARootTask_GetARenderer      = 0x00466140;
    constexpr uintptr_t AApplication_OnCreate       = 0x004846C0;
    constexpr uintptr_t AConstants_Initialise       = 0x0046E7D0;
    constexpr uintptr_t ARootTask_ResetOptions      = 0x0047BCE0;

    // Camera
    constexpr uintptr_t ACamera_OnUpdate            = 0x0050A660;
    constexpr uintptr_t ACamera_GetPosition         = 0x0050B0D0;
    constexpr uintptr_t ACamera_GetTargetPosition   = 0x0050C770;
    constexpr uintptr_t ACamera_GetFollowUnit       = 0x00479ED0;
    constexpr uintptr_t ACamera_SetFollowDistance   = 0x00479EC0;
    constexpr uintptr_t ACameraManager_ReqTargetPos = 0x0050CBC0;
    constexpr uintptr_t ACameraManager_OnUpdate     = 0x0050CF00;
    constexpr uintptr_t ACameraMove_SetFrom         = 0x00511C00; // SetFrom(TMatrix44&)
    constexpr uintptr_t ACameraMove_SetPosition     = 0x005126D0;
    constexpr uintptr_t ACameraMove_GetBoneXform    = 0x0050E970; // AttachmentTransition::GetBoneTransform

    // Shooter minigame (reused for the player gun)
    constexpr uintptr_t AMiniGameState_OnUpdate     = 0x005436A0;
    constexpr uintptr_t AMiniGameState_ShootWeapon  = 0x005467D0; // virtual bool()
    constexpr uintptr_t AMiniGameState_ReloadWeapon = 0x00546A20;
    constexpr uintptr_t AMiniGameState_UpdCamera    = 0x005466B0;
    constexpr uintptr_t AMiniGameState_UpdateTarget = 0x00548410;
    constexpr uintptr_t AMiniGameState_ScopeZoom    = 0x005487D0;
    constexpr uintptr_t AMiniRgrAttack_ctor         = 0x0054CF30; // (AUnit*)
    constexpr uintptr_t AMiniRgrAttack_ProcessInput = 0x0054E270;
    constexpr uintptr_t AMiniRgrAttack_ProcShoot    = 0x0054E3B0;

    // Damage
    constexpr uintptr_t AUnit_HurtUnit              = 0x004C28F0; // virtual (empty base)
    constexpr uintptr_t AUnitDinosaur_HurtUnit      = 0x004CB000; // __thiscall (dino,float dmg,int type,void* src)
    constexpr uintptr_t AUnitDinosaur_GetHealth     = 0x004CC880;
    constexpr uintptr_t AUnitDinosaur_SetHealthFlash= 0x004CEB40;

    // Unit position / targeting (SOLVED - see CONFIRM_FINDINGS.md)
    constexpr uintptr_t AUnit_SetPosition           = 0x004C2A50; // __thiscall (unit, const TVector3*)
    constexpr uintptr_t AActiveUnit_GetTargetPos    = 0x004681D0; // returns unit+0xBC
    constexpr uintptr_t AActiveUnit_SetTargetPos    = 0x004C0800; // virtual (const TVector3&)
    constexpr uintptr_t AMiniGame_GetShootAimUnit   = 0x0054C280; // -> AUnit* under reticle

    // Spawning / enumerating units (SOLVED)
    constexpr uintptr_t AUnitManager_GetUnitManager = 0x0046BE30; // static -> AUnitManager* (singleton 0x84C3E0)
    constexpr uintptr_t AUnitManager_GetUnitList    = 0x0046BE20; // -> TNodeList<AUnit>& (embedded at mgr+0x19074)
    constexpr uintptr_t AUnitManager_GetUnitFactory = 0x0046BE40; // -> AUnitFactory*
    constexpr uintptr_t AUnitManager_ProxCheck      = 0x004A0690; // nearest-of-class(pos,radius,TClass,cb,ctx)
    constexpr uintptr_t AUnitFactory_CreateUnitName = 0x0046C340; // CreateUnit(TPCString& name) -> AUnit*
    constexpr uintptr_t g_AUnitManager_singleton    = 0x0084C3E0; // AUnitManager::ms_pSingleton

    // Camera attach (how the minigame installs its camera onto a unit)
    constexpr uintptr_t AMiniGame_GetScopeCamera    = 0x00546520;
    constexpr uintptr_t AMiniGame_GetFollowCamera   = 0x00546550;
    constexpr uintptr_t AScopeCamera_AttachToUnit   = 0x0051DEC0; // (AUnit*, TCString&, bool, bool)
    constexpr uintptr_t ACamera_SetAutoFieldOfView  = 0x0050C6D0;

    // Tranquilizer / comatose (tranq path, != HurtUnit)
    constexpr uintptr_t AUnitDinosaur_IsComatose    = 0x004CD0E0;
    constexpr uintptr_t AUnitDinosaur_ClearComatose = 0x004CD020;
    constexpr uintptr_t AMiniRgr_SetSedateFlags     = 0x00553BE0;

    // Avatar (ranger/staff) + vehicles
    constexpr uintptr_t AUnitStaff_CmdGoTo          = 0x004D4C30; // (TVector3&)
    constexpr uintptr_t AUnitStaff_CreateObject     = 0x004D4C90;
    constexpr uintptr_t AUnitBuilding_GetHelipadPos = 0x004C5AF0;
    constexpr uintptr_t ASafariSteer_SeekDrive      = 0x00466570;
    constexpr uintptr_t ASafariSteer_FaceDirection  = 0x00466560;
    constexpr uintptr_t ASafariSteer_SetSteerState  = 0x004664D0;

    // Vehicle steering (SOLVED - player-drive via these)
    constexpr uintptr_t ASteer_Update               = 0x0046BC70; // Update(float)
    constexpr uintptr_t ASteer_SetPosition          = 0x0046B9F0; // (TVector3&)
    constexpr uintptr_t ASteer_Command_SeekDrive    = 0x0046BB20; // (uchar)
    constexpr uintptr_t ASteer_SetSpeedModifier     = 0x0046BD80; // (float)
    constexpr uintptr_t ASteer_SetToFullSpeed       = 0x0046BD20;
    constexpr uintptr_t ACreatureSteer_SetSpeed     = 0x00498970; // virtual (float)
    constexpr uintptr_t ACreatureSteer_GetVelocity  = 0x004989E0; // virtual -> TVector3
    constexpr uintptr_t ACreatureSteer_GetMoveDir   = 0x004989C0; // virtual -> TVector3
    constexpr uintptr_t ACreatureSteer_SetPosition  = 0x00498A70; // virtual (TVector3&)

    // Renderer / viewport (split-screen P8)
    // Vanilla "shoot from vehicle" cheat flag: AMiniGameController::m_uiCheats (static byte),
    // bit 0 = vehicle shoot mode (toggled by ToggleCheatVehicleShootMode @0x691B00). Verified
    // from EXE bytes: 0f b6 05 40 2d 85 00 a8 01 ...
    // --- unlocks (owner request 2026-11-03). Statics are pointers (deref once). ---
    constexpr uintptr_t AParkManager_m_pParkInfo          = 0x00850048; // AParkInfo*  (+0x144 Current10Rating, +0x148 Highest10Rating)
    constexpr uintptr_t AParkManager_m_pPlayerInfo        = 0x0085004C; // APlayerInfo* (+0x08 money)
    constexpr uintptr_t AParkManager_m_pFossilHuntManager = 0x00850078;
    constexpr uintptr_t AParkManager_m_pResearchManager   = 0x00850080;
    constexpr uintptr_t AMissionController_s_pInstance    = 0x00852E44; // (+0x68 num missions)
    constexpr uintptr_t APlayerInfo_CheatGiveMoney           = 0x005D6680; // thiscall(int)
    constexpr uintptr_t AResearchManager_CheatGiveAllResearch = 0x0060B070; // thiscall()
    constexpr uintptr_t AMissionController_OpenAllMissions   = 0x006A4AD0; // thiscall() - Site B shows when last mission is open
    constexpr uintptr_t AFossilHuntManager_ForceStatusOpen   = 0x005979C0; // thiscall()
    constexpr uintptr_t AFossilHuntGround_UnlockGround       = 0x00593BC0; // thiscall(), idempotent (+0x75 flag)
    constexpr uintptr_t AFossilHuntGroundSet_UserChosen     = 0x00596780; // thiscall(AFossilHuntGround*)
    constexpr uintptr_t AParkInfo_ForceSetEsat               = 0x005CB810; // thiscall(float); 10-rating = round(esat*10+0.5)
    constexpr uintptr_t AMiniGameController_m_uiCheats = 0x00852D40;
    constexpr uintptr_t ARenderer_OnUpdate          = 0x00477370; // virtual bool(float) - render update  <- MAIN FRAME HOOK (JPOG2+)
    constexpr uintptr_t ARenderer_GetViewport       = 0x00472A10; // -> Toshi::TViewport*
    constexpr uintptr_t ARenderer_SetFarClip        = 0x00472A30;

    // Attachment system (first-person viewmodel / bone attach, P4b)
    constexpr uintptr_t AAnimatableObject_AttachXform = 0x0046EFA0; // -> AAttachedTransform&
    constexpr uintptr_t AAnimSystem_UpdateAttachments = 0x00479F60;

    // Input
    constexpr uintptr_t AInputMap_GetInputMap       = 0x00468E00; // static AInputMap*()
}

// ---- Function pointer typedefs for the few we hook / call directly ----
// __fastcall is how MSVC lets us pass ECX (this) + the stack arg for a __thiscall.
using fn_ARootTask_OnUpdate = int(__fastcall*)(void* self, void* edx, float dt);

// ============================================================================
//  [CONFIRM] before the milestone that needs each - read the named function in
//  ..\src\SimJP.c (or the vtable in ..\symbols\SimJP.data.csv). Fill these in
//  as they are verified; do not guess.
// ============================================================================
namespace confirm {
    // ---- SOLVED from the decompile (see CONFIRM_FINDINGS.md) ----

    // P1/P2: move the avatar via AUnit_SetPosition(unit, &TVector3) - no raw offset.
    //   The unit's scene node is at unit+0x10 (vtable+0x100 does the move).
    inline constexpr int kUnitNodeOffset       = 0x10;

    // P4: dino health (float) and the lethal hurt-type set.
    inline constexpr int   kDinoHealthOffset   = 0x140;  // *(float*)(dino+0x140)
    inline constexpr int   kHurtTypeLethal     = 3;      // {3,4,5,10} = kill/slaughter
    inline constexpr int   kDinoSlaughterFlag  = 0x22c;  // flag byte set on slaughter
    // Aim target: AMiniGame_GetShootAimUnit (0x54C280). ShootWeapon applies via vtable+0xD8;
    // we instead call AUnitDinosaur_HurtUnit(dino, dmg, kHurtTypeLethal, avatar) directly.

    // P2/P4b: camera field offsets (ACamera*).
    inline constexpr int kCamPosOffset         = 0x4C;   // TVector3 world pos
    inline constexpr int kCamOrientOffset      = 0x5C;   // orientation quaternion
    inline constexpr int kCamFocalXOffset      = 0xDC;   // desired focal (x/z/y at DC/E0/E4)

    // AActiveUnit target-position vector.
    inline constexpr int kActiveUnitTargetPos  = 0xBC;

    // ---- STILL TO READ (named function to open) ----
    // TRANQ sedation setter (tranq != HurtUnit): read the dinovet/ranger-depot dart path.
    // P2 camera install: read AMiniGameState::UpdateCameraSettings (0x5466B0).
    // P4b bone index for FP viewmodel: read ACameraMove GetBoneTransform (0x50E970) + AVisibleUnit sockets.
    // P5 vehicle velocity/heading fields: read base ASteer/ACreatureSteer + AUnitVehicle.
    // P7 task-tree insertion: read ARootTask::OnCreate (0x47A580) + AApplication::GetUpdate*RootTask.
    // P8 world-render entry + camera view-rect apply: read ARenderer / TRenderD3DInterface viewport.
    // Controller engine-path doodad ids: TInputDXDeviceController::TranslateDXButtonToDoodad (DLL 0x100015B0).
}

} // namespace game
