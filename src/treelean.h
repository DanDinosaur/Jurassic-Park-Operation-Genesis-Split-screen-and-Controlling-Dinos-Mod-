// treelean.h - JPOG81 (owner 2026-11-14 07:46 "trees should look natural when they go down and come back up instead of
// wildly spinning"): a PLAYER's dinosaur bends the trees it walks through itself, instead of the engine's knock.
//
// Why: ATreeManager::ApplyAnimation(type 3 = knock) makes ATreeManager::OnUpdate (0x5DD430) rotate each tree's quaternion
// EVERY FRAME by angle(t) = strength * typeFactor * (1 - t/1.5) * sin(2t) * 0.8 * 0.5 for 2 s (QuickRotateQuatAroundPOSZ
// SinCos + RotateAroundAxis, no dt) - incremental and frame-rate dependent; the sum over 2 s is not zero
// (integral of (1 - t/1.5) sin 2t = 0.52), so at today's 60+ fps the tree ends twisted and keeps turning on every new knock.
// Vanilla dinos steer round trees (HardAvoidWorld), so they rarely knock; a ridden dino ploughs through them.
//
// Here: the knock call for a player's dino (ACreatureSteer::ProcessTick 0x6398EE -> Hook_TreeKnock) is handled by Knock():
// every tree of that cache region within its type radius + 3 m (as the engine tests) leans AWAY from the dino on a spring
// (tree_lean_deg), stays bent while the dino touches it, springs back with a small sway and gets its ORIGINAL quaternion
// back exactly. Node layout (from OnUpdate): +0x08 pos x,y,z, +0x14 quaternion x,y,z,w, byte +0x29 & 0x7F = tree type
// (radius table tm+0x98). Region list: tm+0xA4 + idx*8, idx = (cx & tm+0xC4) + (cz & tm+0xC4) * ushort tm+0x160,
// cx = (x >> 3) >> 1; cached when ushort pair tm+0xC8[idx] & ushort 0x8B2B30 == (cx, cz). Every frame each bent tree is
// re-validated (region still cached, node still in its list) before it is written - a re-filled cache drops it.
#pragma once
namespace treelean {
    // true = handled (the engine knock must NOT run). Game thread.
    bool Knock(void* treeMgr, unsigned x, unsigned z, const float* pos, const float* facing);
    void Tick(float dt);        // game thread, every frame
    void Clear();               // leaving the park
}
