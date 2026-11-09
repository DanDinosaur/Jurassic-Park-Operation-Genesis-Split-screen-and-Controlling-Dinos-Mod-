// padrumble.h - controller rumble driven by the game's own camera shakes (main mod).
// Every dramatic moment the game wants you to feel (stomps, roars, storms, crashes) goes through
// ACamera::AddShake (vtable slot 37). We mirror each shake as rumble on the pad of the player whose
// camera it is, scaled by distance like the camera shake itself. In split-screen the other player's
// camera gets the shake too, so each player feels what happens near THEIR view.
#pragma once
namespace padrumble {
    bool Install();     // patch AWorldCamera's AddShake vtable slot
    void Tick();        // game thread, every frame: fade and stop motors
    void Pulse(int player, float strength, float seconds);   // one short rumble (dino control hits)
}
