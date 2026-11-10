// stateguard.h - JPOG77: the park must not be unloaded by a building-window button pressed from a window that has no
// game window state under it (player without the HUD, own window in split-screen).
//
// Owner test JPOG76 16:33: P1 (no HUD) opened the safari station in their own window and pressed its button ->
// AVisSafariComponent::CreatePath 0x4FCD30 does GetCurrentGameState()->RemoveSelf() (vtbl +0x3C = ARootState::RemoveSelf
// 0x58E070) and then TransferControl(new ASafariLayingState). In vanilla the current state is the window's own
// AUnitInfoState (removed = window closed). Our own window has no such state, so the HUD player's ADefaultState - the
// LAST game state (AGameState counter [0x84E760] == 1) - was removed -> AGameState::OnRemoval -> ARootTask::
// UnloadGameController = park unloaded -> black screen (ARootState vtbl 00D21E00) -> CRASH SimJP+0x2E0063.
// 14 building / unit window handlers do the same (balloon / safari CreatePath, ride photos / purchase, security camera,
// turret, staff fire, dino retire / vaccinate ...). Fix: RemoveSelf of the last game state called from one of those 14
// call sites is skipped; the handler then pushes its new state on the park state = the vanilla result.
#pragma once
namespace stateguard {
    void Install();                 // game thread / init: detour ARootState::RemoveSelf 0x58E070 ([coop] state_guard)
    void BeginClick(int who);       // p2win: a no-HUD player's own-window click starts (logs every removal inside it)
    int  EndClick();                // returns the number of park-state removals kept during the click
    unsigned Kept();                // total kept removals (any caller)
}
