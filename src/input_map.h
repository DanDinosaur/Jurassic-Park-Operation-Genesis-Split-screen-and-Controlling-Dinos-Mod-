// input_map.h - phase-1 controller: translate an XInput pad into the keyboard/
// mouse the game already understands (via SendInput), per [map.manager] in the ini.
// This needs ZERO game offsets, so it is the first thing that can work on a live
// build - native Xbox control of the menus/manager mode with no external mapper.
//
// Later (engine-path controller) we instead feed the game's own input pipeline for
// lower latency + rumble tied to events; see design doc section 13. This module
// stays as the fallback and as the always-on menu driver.
#pragma once
#include "xinput_pad.h"
#include <string>

namespace input_map {
    void Init();
    // Called each frame with the polled pad of `player` (0/1, whoever owns the cursor); emits SendInput
    // events for manager/menus, opens HUD discs (console style), starts dino control (RB + D-pad Up).
    void DriveManager(const XPad& pad, int player);
    // Press+release one action by ini name ("F2", "ENTER", "MOUSE_L").
    void TapKeyName(const std::string& name);
    // Press+release whatever a pad button is bound to (used to replay the press that handed
    // split-screen control to the other player).
    bool TapButton(PadButton btn);
    bool ConsoleActive();   // JPOG59: ConsoleMode() and a controller connected ([controller] pc_without_pad)
    // Console-style controls (default, like the Xbox version) vs PC cursor controls.
    // Right-stick click toggles; saved as [controller] console_mode in jpogmod.ini.
    bool ConsoleMode();
    void SetConsoleMode(bool on);
    void Shutdown();
    // JPOG33: game thread, every frame (keys/clicks are only sent after the game has started rendering).
    void GameFrame();
    // JPOG33: true if pad keys may be sent now (SimJP's own enabled, visible, un-owned window is in front).
    bool SendAllowed();
}
