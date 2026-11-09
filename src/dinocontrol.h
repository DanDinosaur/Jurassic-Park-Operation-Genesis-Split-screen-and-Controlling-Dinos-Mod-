// dinocontrol.h - DINO CONTROL (main mod, Co-op v1). Recreated for JPOG25 from the JPOG24 binary.
//
// Owner's design: aim at a dino and press A (the game's own unit selection), then RB + D-pad Up =
// take control of it. B = let go and return to zoo-builder mode. Works per player in split-screen.
// While controlling: the game's own follow camera on the dino (AFollowState through the root state
// machine), LS = move (creature steering towards a point lead_distance ahead, relative to the view),
// RS = camera orbit (the game's own Q/E/R/F camera keys), RT = attack the nearest dino in range
// (AUnitDinosaur::HurtUnit, hurt type 3, so the victim reacts). The dino's AI is held off by
// keeping its drive manager (unit+0xA0) +0xC at -1000 every frame (Ranger mod R25 method).
// Ends on: B, dino died / gone, follow view closed, left the park, split-screen ended. Another
// screen opening pauses it (resumes when the follow view is back).
// JPOG30: in split-screen the dino player does NOT use AFollowState: the root state stays the builder's
// (ADefaultState), the HUD goes to the other player, and the dino player's half is drawn with our own chase
// camera (ChaseView, right stick orbit, D-pad Up/Down zoom).
#pragma once
#include <cstdint>
namespace dinocontrol {
    void RequestInfo(int player);      // JPOG55: RB + D-pad Up = the game's info bar for the selected unit
    void RequestControl(int player);   // controller thread: player 0/1 wants the selected dino
    void RequestRelease(int player);   // controller thread
    // JPOG34: the player without the HUD: control the nearest dino to pt (ground point under the middle of their view).
    void RequestControlAt(int player, const float* pt);
    // JPOG34: nearest live, uncontrolled dinosaur to pt within radius (game thread). outPos may be null.
    void* NearestDino(const float* pt, float radius, float* outPos);
    void* NearestUnitOf(const float* pt, float radius, uintptr_t tclassVa);   // JPOG57: any live unit of a class (game thread)
    void  NoteAPress(int player);      // JPOG57: the HUD player pressed A in the park view (controller thread)
    bool  IsZzzUnit(const void* unit); // JPOG57: a player's dino that sleeps now (sleep icon forced visible)
    bool Busy();                       // a dino is controlled (or a request is pending)
    int  Controller();                 // first player controlling a dino, -1 = none
    int  RiderEars(float out[2][3], int who[2]);   // JPOG42: ridden dinos' sound positions in split (game thread)
    bool ListenerPos(float* out);      // JPOG40: rider dino position for the sound listener in split (game thread)
    bool Controls(int player);         // JPOG31: this player controls a dino (both can, in split-screen)
    void ForceRelease(const char* why);// game thread (e.g. split-screen ended)
    void Tick(float dt);               // game thread, every frame
    void RequestTestSpawn();
    void Install();                    // JPOG36: behaviour pause + keep-visible hooks (call once at start-up)
    // JPOG30 split-screen: the dino player's own chase camera. Returns false when `who` isn't controlling a
    // dino in split mode. ref = that player's current view matrix (for conventions / starting yaw).
    bool ChaseView(int who, float dt, const float* ref, float* out);
    // RequestTestSpawn = debug ([debug] test_dino): spawn two dinos at the camera target, select one
}
