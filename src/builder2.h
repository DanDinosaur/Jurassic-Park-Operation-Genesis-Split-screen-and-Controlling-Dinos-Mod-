// builder2.h - JPOG46 (owner backlog item 1, stage 1): the player WITHOUT the HUD gets their own build tool,
// at the same time as the HUD player builds - the same trick as two players riding dinos at once.
//
// The game has ONE state chain (park view -> menu -> placement tool) and ONE HUD. Instead of that chain, this player
// gets a PRIVATE instance of the game's own placement tool (ABuildPlacementState, never inserted into the chain) and
// we drive it ourselves, like dinocontrol drives a dino's own steering:
//   ctor 0x52B3F0 (this, const char* unit), size 0x140, game tmalloc; vftable 0xD13800:
//     +0x08 scalar deleting dtor(flags)   +0x54 PlaceThing(bool& done) -> AUnitBuilding*
//     +0x58 DeterminePosition(const TVector3&) -> bool   +0x5C CalculateBlockFlags()   +0x60 LoadTraits() -> bool
//     +0x68 RotateThing(int dir)
//   CreateDecalArray 0x52E020, RenderUIMeshes 0x52E360 (queues the green/red footprint = ONE scene pass).
//   this+0x60 = PlacementInfo*: +0x2E rotation (0..3), +0x40 block flags (0 = can build).
// JPOG47 (stage 2a): the same driver also runs the tool's two subclasses - ARoadAddonPlacementState (bin, bench on a
// path; ctor 0x558DE0, 0x150 bytes, vftable 0xD1A2C0, cursor model at +0x140 freed by us like its OnRemoval but
// WITHOUT AGameState::OnRemoval) and AWallAddonPlacementState (gate 0 / vent 1 / safari 3 on a fence; ctor 0x569FD0
// (type, unit), 0x18C bytes, vftable 0xD1C920; LB/RB flip the side). Paths, fences, terrain, flora = stage 2b.
// JPOG48 (stage 2b): + the drag tools APathLayingState (path) and AWallLayingState (fence wt1/wt2/wt3): A start / A finish,
// B cancels the drag. Their OnUpdate/ProcessCommand run with this player's aim swapped into the ONE global aim (see
// AimSwap in builder2.cpp). Terrain, trees and the Manager/Dinosaurs/Actions discs = stage 2c.
// These were checked in the decompile: none of them touches the state chain, the camera or the HUD
// (RotateThing plays the root controller's transition sound only).
#pragma once
class XPad;
namespace builder2 {
    // Game thread, from splitscreen's inactive-player update. Returns true when the build tool used this frame's
    // buttons (the caller then skips the A dino pick and the D-pad left/right pan).
    bool Update(int who, XPad& pad, void* cam, float dt);
    bool Open(int who);
    void QueueFootprint(int who);
    void AfterPass(int who);        // JPOG52: render hook, right after that player's half (terrain preview taken back)   // render hook, main scene, right before that player's half is drawn
    void Close(int who, const char* why, bool worldGone = false);
    void CloseAll(const char* why, bool worldGone = false);
    // JPOG49: open the tool for an item picked on the player's own disc (p2menu); false = not supported yet.
    bool OpenFor(int who, const char* cmd, int itemType);
    // JPOG49: why the last disc-opened tool closed: 1 = B (back to the disc), 2 = X (done), 0 = other / none.
    int  TakeCloseReason(int who);
}
