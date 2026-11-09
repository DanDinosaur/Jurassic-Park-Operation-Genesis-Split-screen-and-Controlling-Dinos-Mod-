// ridehud.h - JPOG62 (owner 13:23-13:25): while the HUD player rides a dino, the game HUD shows only the dino
// stats panel + the minimap ([dino] ride_hud_mask, default 0x1100). Restored on let-go.
#pragma once
namespace ridehud {
    void Tick();       // game thread, after dinocontrol::Tick
    void OnUnload();   // game thread, start of ARootTask::UnloadGameController: forget the record
}
