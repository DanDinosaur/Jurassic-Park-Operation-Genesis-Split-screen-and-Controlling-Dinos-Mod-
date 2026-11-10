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
    // JPOG78 (owner 17:12): hints in the words of the player's own input - keyboard + mouse players get PC keys.
    bool PcPlayer(int player);                     // this player plays with keyboard + mouse
    const wchar_t* TakeKeyName();
    const wchar_t* KeyName(int vk, wchar_t* buf, const wchar_t* fallback);   // JPOG80: buf >= 32 wchar_t                  // [keys] ride_toggle as the keyboard shows it (default T)
    // pad / pc text with %s = take key; player prefix "Player N: " when who >= 0. Returns a per-call buffer.
    const wchar_t* HintText(int who, const wchar_t* padText, const wchar_t* pcFmt);
    void NoteWindowUnit(int player, void* unit);   // JPOG76: p2menu's own window of player for unit (game thread)
    bool HeliPressFromWindow(int player);          // JPOG76: RB + D-pad Up in that window on a helipad / helicopter -> fly (game thread)
    void RequestInfo(int player);      // JPOG55: RB + D-pad Up = the game's info bar for the selected unit
    void RequestControl(int player);   // controller thread: player 0/1 wants the selected dino
    void RequestRelease(int player);   // controller thread
    // JPOG34: the player without the HUD: control the nearest dino to pt (ground point under the middle of their view).
    void RequestControlAt(int player, const float* pt);
    void RequestSelectAt(int player, const float* pt);   // JPOG68: player's own selection (nearest dino to pt)
    bool HasSelection(int player);                       // JPOG68
    bool SelectedPos(int player, float* out, unsigned* serial);   // JPOG69 R1: own selection's position + change counter
    // JPOG34: nearest live, uncontrolled dinosaur to pt within radius (game thread). outPos may be null.
    void* NearestDino(const float* pt, float radius, float* outPos);
    void* ZzzDino(int p);   // JPOG73: this player's ridden dino while its sleep icon is on (else null)
    void* NearestUnitOf(const float* pt, float radius, uintptr_t tclassVa, float* outDist = nullptr);   // JPOG57: any live unit of a class (game thread)
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
