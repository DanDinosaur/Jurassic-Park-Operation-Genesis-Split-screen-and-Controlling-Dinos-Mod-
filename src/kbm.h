// kbm.h - JPOG70: two keyboards + two mice on one PC (player 1 and player 2 each with their own keyboard + mouse),
// and a per-player switch "controller <-> keyboard + mouse".
//
// How it works (our own code, written from the Win32 Raw Input docs; Nucleus Co-op / ProtoInput only taught the idea):
//   * Windows merges every keyboard into one and every mouse into one. Only Raw Input (WM_INPUT + RAWINPUTHEADER.hDevice)
//     says which physical device sent an event.
//   * DirectInput 8 itself reads keyboard + mouse through Raw Input (its own hidden window), and Windows allows ONE raw
//     input window per device type per process. So we hook RegisterRawInputDevices / GetRawInputData in the real
//     dinput8.dll's import table: the registration goes to OUR hidden window, and we pass each event on to DirectInput's
//     window ONLY if it came from player 1's devices (or was injected, e.g. the controller's SendInput keys). Player 2's
//     keys and mouse never reach the game's own input - they become player 2's "virtual controller" instead (the same
//     code a real pad 2 drives: camera, discs, build tools, windows, dino riding).
//   * Capture only starts when player 2 wants keyboard + mouse (Shift+F8, Options menu, or saved in jpogmod.ini), so
//     single player stays vanilla. A watchdog undoes everything if DirectInput doesn't take the passed-on events.
#pragma once
#include "xinput_pad.h"

namespace kbm {
    // proxy.cpp, right after the real dinput8.dll is loaded / DirectInput8Create returned.
    void EarlyInstall(void* realDinput8Module);
    void OnDirectInput(void* di);              // wraps CreateDevice -> keyboard GetDeviceState/GetDeviceData mask (fallback)

    // Per-player input mode. false = controller (default), true = keyboard + mouse.
    bool IsKbm(int player);
    void SetKbm(int player, bool on, const char* why);   // saved as [pc2] p1_input / p2_input = pad | kbm
    void ToggleFromPad(int player);                       // pad combo (padslots): switch that player
    bool PadCombo();                                      // [pc2] pad_combo: hold Back + Start 1 s = switch

    // Player 2's keyboard/mouse as a controller (xinput_pad.cpp, XPad::Poll for player 2 in keyboard mode).
    // Returns false when player 2 has no keyboard/mouse yet (= no controller connected).
    bool FillPad2(bool* down, float& lx, float& ly, float& rx, float& ry, float& lt, float& rt);
    // Is virtual key vk held on player p's keyboard? (riding keys). Player 1 = every keyboard that is not player 2's.
    bool KeyDown(int player, int vk);

    void Tick();   // mod thread ~120 Hz: F8 / Shift+F8 before capture, hints

    // JPOG73 (owner: "player 2's OWN mouse pointer" + "player 1's PC HUD buttons in player 2's half"): player 2 on keyboard +
    // mouse gets a pointer in their half (0..1 of the half, y down) and a button row (Build / Manager / Dinosaurs / Actions /
    // Options + mail). Mouse = pointer, pointer at the half's edge = scroll, left click = button or the world under it.
    enum P2Btn { P2B_NONE = -1, P2B_BUILD, P2B_MANAGER, P2B_DINOS, P2B_ACTIONS, P2B_OPTIONS, P2B_MAIL, P2B_COUNT };
    bool P2PointerMode();                         // pointer shown + used right now (player 2 keyboard + mouse, park view, nothing open)
    bool P2Pointer(float& x, float& y);           // pointer in player 2's half (0..1)
    bool P2MenuPointer();                         // the pointer is in player 2's open disc / window (clicks go to p2menu)
    bool P2WorldPointer();                        // the pointer is over the park (aim / circles at it)
    bool P2TakeMenuClick();                       // game thread: a left click inside a menu (once)
    void P2SetHalfAspect(float wOverH);
    float P2CursorScale();                        // [pc2] cursor_scale (1 = the game's own cursor size)           // splitscreen: player 2's half width / height (button layout)
    bool P2ButtonRect(int id, float& x0, float& y0, float& x1, float& y1);   // button rect in the half (0..1)
    int  P2HoverButton();                          // button under the pointer (P2B_NONE = none)
    bool P2TakeWorldClick(float& x, float& y);    // game thread: a left click on the world (once)
}
