#include <cmath>
#include <windows.h>
#include "mod.h"
#include "config.h"
#include "log.h"
#include "hooks.h"
#include "playermode.h"
#include "input_map.h"
#include "xinput_pad.h"
#include "splitscreen.h"
#include "guard.h"
#include "padrumble.h"
#include "dinocontrol.h"
#include "cullfix.h"
#include "gamectl.h"

// Bootstrap. Runs on its own thread so we never do real work inside the loader
// lock. The controller runs on this thread too (independent of the game hook), so
// native pad control of the menus works even before the player-mode hook matters.

namespace {
    HANDLE g_thread = nullptr;
    volatile bool g_run = false;

    bool GameHasFocus() {
        HWND fg = GetForegroundWindow();
        if (!fg) return false;
        DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }

    DWORD WINAPI ThreadMain(LPVOID) {
        // Give the game a moment to finish its own startup before we hook.
        Sleep(2500);

        const Config& c = config::Load();
        modlog::Init(c.log);
        LOG("mod: starting (enabled=%d)", c.enabled);
        if (!c.enabled) return 0;

        input_map::Init();
        PlayerMode::Init();

        // Install the frame hook. If it fails, the controller still runs below.
        guard::Install();
        splitscreen::Install();
        dinocontrol::Install();
        cullfix::Install();
        padrumble::Install();
        if (!hooks::Install())
            LOG("mod: frame hook not installed - controller still active, player-mode features off.");

        XPad pad0(0), pad1(1);
        DWORD lastSwitch = 0;
        int pendingBtn = -1, pendingFor = 0;
        bool prevBack2 = false;
        bool prevBack = false, prevConn = false, backFired = false;
        int sbOwner = -1, sbFor = 0; DWORD sbT0 = 0, sbT1 = 0; bool sbOpened = false, sbSeenMenu = false;   // JPOG59 Start borrow
        DWORD backT0 = 0;
        // [coop] handover = p1_back: player 1 holds Back 0.6 s = pass the HUD/cursor to the other player and back.
        bool hbPrev = false, hbUsed = false, hbOther = false; DWORD hbT0 = 0;
        const bool anyButton = (c.handover == "any_button");
        LOG("mod: split hand-over = %s", anyButton ? "any_button (any press takes the cursor)" : "p1_back (player 1 holds Back)");
        bool prevFocus = false;
        int  vpTick = 0;
        if (c.virtualPad) LOG("mod: DEBUG virtual pad on (jpogmod_pad.txt)");

        while (g_run) {
            if (c.virtualPad && ++vpTick >= 3) { vpTick = 0; virtualpad::Refresh(); }
            bool focus = GameHasFocus();
            if (focus != prevFocus) {
                prevFocus = focus;
                HWND fg = GetForegroundWindow(); char t[96]{}; if (fg) GetWindowTextA(fg, t, sizeof(t));
                LOG("focus: %s (fg=%p '%s')", focus ? "GAME" : "other", fg, t);
            }
            if (c.padEnabled && focus) {
                pad0.Poll(c.deadzone, c.triggerThreshold);
                if (pad0.Connected() != prevConn) {
                    prevConn = pad0.Connected();
                    LOG("pad0: %s%s", prevConn ? "connected" : "disconnected",
                        (prevConn && virtualpad::Get().active) ? " (virtual)" : "");
                }
                // Split-screen: ONE game cursor/camera, owned by whichever player is acting
                // (like two copies of the game sharing a park). The other player's pad drives
                // their own camera on the game thread (splitscreen.cpp).
                if (splitscreen::CanSwitch()) {
                    pad1.Poll(c.deadzone, c.triggerThreshold);
                    int act = splitscreen::ActivePlayer();
                    XPad& idle = act ? pad0 : pad1;
                    // Replay the button that handed control over, once the switch has happened.
                    if (pendingBtn >= 0 && splitscreen::ActivePlayer() == pendingFor) {
                        input_map::TapButton((PadButton)pendingBtn);
                        pendingBtn = -1;
                    } else if (pendingBtn >= 0 && GetTickCount() - lastSwitch > 500) pendingBtn = -1;
                    int pressed = -1;
                    auto wants = [&](const XPad& p) {
                        static const PadButton acts[] = { PAD_A, PAD_B, PAD_X, PAD_Y, PAD_START, PAD_LB, PAD_RB, PAD_BACK };
                        for (PadButton b : acts) if (p.Pressed(b)) { pressed = (int)b; return true; }
                        return p.RT() > 0.5f || p.LT() > 0.5f || fabsf(p.LX()) > 0.35f || fabsf(p.LY()) > 0.35f;
                    };
                    // JPOG30: a dino player's pad never takes the cursor (their stick/buttons drive the dino), the builder's may.
                    if (anyButton && idle.Connected() && !splitscreen::MenuLocked() && !dinocontrol::Controls(1 - act) &&
                        !(dinocontrol::Controller() < 0 && dinocontrol::Busy()) && wants(idle) && GetTickCount() - lastSwitch > 400) {
                        input_map::Shutdown();                 // release anything the old player held
                        splitscreen::RequestActive(1 - act);
                        lastSwitch = GetTickCount();
                        // Replay the press that handed control over - only buttons that mean the same in both control
                        // styles (console style: Y/LB/RB/Start are discs / modifiers, not their PC keys).
                        bool replay = pressed >= 0 && pressed != PAD_BACK &&
                            (!input_map::ConsoleMode() || pressed == PAD_A || pressed == PAD_B || pressed == PAD_X);
                        pendingBtn = replay ? pressed : -1; pendingFor = 1 - act;
                        LOG("pad: player %d takes control%s", 2 - act, pendingBtn >= 0 ? " (button replayed)" : "");
                    }
                }
                // JPOG59 (open list 3, owner 12:06): Start / pause / save / options = whoever presses Start. The player
                // without the HUD presses Start in the park view -> they get the HUD for that menu (the game has ONE
                // Start menu), the game's Start menu opens for them, and the HUD goes back to the other player as soon
                // as the park view is back (menu closed). [coop] start_borrow = 1.
                if (splitscreen::CanSwitch() && c.startBorrow) {
                    const int act = splitscreen::ActivePlayer();
                    XPad& idle = act ? pad0 : pad1;
                    const DWORD now = GetTickCount();
                    if (sbOwner < 0 && idle.Connected() && idle.Pressed(PAD_START) && gamectl::ParkViewActive() &&
                        !splitscreen::MenuLocked() && !dinocontrol::Controls(1 - act) && now - lastSwitch > 400) {
                        input_map::Shutdown();
                        splitscreen::RequestActive(1 - act);
                        sbOwner = act; sbFor = 1 - act; sbT0 = now; sbOpened = false; sbSeenMenu = false;
                        lastSwitch = now; pendingBtn = -1;
                        LOG("pad: player %d pressed Start - borrows the HUD for the Start menu", sbFor + 1);
                    }
                    if (sbOwner >= 0) {
                        const bool pv = gamectl::ParkViewActive();
                        if (!sbOpened && act == sbFor && now - sbT0 > 60) {
                            gamectl::RequestMenu(gamectl::MENU_START);
                            sbOpened = true; sbT1 = now;
                            LOG("pad: Start menu opened for player %d", sbFor + 1);
                        }
                        if (sbOpened && !pv) sbSeenMenu = true;
                        const bool closed = sbOpened && ((sbSeenMenu && pv && now - sbT1 > 300) || (!sbSeenMenu && now - sbT1 > 2500));
                        if (closed || now - sbT0 > 600000) {
                            if (splitscreen::ActivePlayer() == sbFor) { input_map::Shutdown(); splitscreen::RequestActive(sbOwner); }
                            LOG("pad: Start menu closed - HUD back to player %d%s", sbOwner + 1, sbSeenMenu ? "" : " (menu never showed)");
                            sbOwner = -1; lastSwitch = now;
                        }
                    }
                } else sbOwner = -1;
                // Player 1's Back in split-screen (p1_back): TAP = Research (when P1 has the HUD), HOLD 0.6 s = pass the HUD.
                const bool splitBack = splitscreen::CanSwitch() && !anyButton && pad0.Connected();
                if (splitBack) {
                    bool b = pad0.Down(PAD_BACK);
                    DWORD now = GetTickCount();
                    if (b && !hbPrev) { hbT0 = now; hbUsed = false; hbOther = false; }
                    if (b) {
                        static const PadButton others[] = { PAD_A, PAD_B, PAD_X, PAD_Y, PAD_START, PAD_LB, PAD_RB,
                                                            PAD_DUP, PAD_DDOWN, PAD_DLEFT, PAD_DRIGHT, PAD_LS, PAD_RS };
                        for (PadButton o : others) if (pad0.Pressed(o)) hbOther = true;   // e.g. debug Back + D-pad Right
                    }
                    if (b && !hbUsed && !hbOther && now - hbT0 >= 600 && !c.backHandover) {
                        hbUsed = true;                         // JPOG59 (open list 3): no more HUD hand-over on hold Back
                        LOG("pad: P1 hold Back - no hand-over any more ([coop] back_handover = 0); Start = whoever presses it");
                    }
                    if (b && !hbUsed && !hbOther && now - hbT0 >= 600) {
                        hbUsed = true;
                        int act = splitscreen::ActivePlayer();
                        if (splitscreen::MenuLocked()) {
                            LOG("pad: P1 hold Back - pass refused (a full-screen menu is open)");
                            gamectl::RequestHint(L"Close the menu first, then hold Back to pass building", 3.0f);
                        } else if (dinocontrol::Busy()) {
                            LOG("pad: P1 hold Back - ignored (a dino is controlled: P1=%d P2=%d)", dinocontrol::Controls(0), dinocontrol::Controls(1));
                        } else {
                            input_map::Shutdown();
                            splitscreen::RequestActive(1 - act);
                            lastSwitch = now; pendingBtn = -1;
                            LOG("pad: P1 hold Back -> player %d builds now", 2 - act);
                            gamectl::RequestHint(act == 0 ? L"Player 2 is building - Player 1: hold Back to take it back"
                                                          : L"Player 1 is building - hold Back to pass it to Player 2", 4.0f);
                        }
                    }
                    if (!b && hbPrev && !hbUsed && !hbOther && splitscreen::ActivePlayer() == 0 &&
                        PlayerMode::Current() == Mode::MANAGER && !dinocontrol::Controls(0))
                        input_map::TapKeyName(c.mgr_BACK_TAP);
                    hbPrev = b;
                }
                const bool p2Drives = splitscreen::CanSwitch() && splitscreen::ActivePlayer() == 1;
                if (p2Drives) {
                    if (PlayerMode::Current() == Mode::MANAGER) input_map::DriveManager(pad1, 1);
                    // Back tap = Research, same as player 1 (also when Back is what handed P2 control).
                    // JPOG31: not after a Back combo (e.g. debug Back + D-pad Right), same as player 1.
                    bool back2 = pad1.Down(PAD_BACK);
                    static bool b2Other = false;
                    if (back2 && !prevBack2) b2Other = false;
                    if (back2) {
                        static const PadButton others[] = { PAD_A, PAD_B, PAD_X, PAD_Y, PAD_START, PAD_LB, PAD_RB,
                                                            PAD_DUP, PAD_DDOWN, PAD_DLEFT, PAD_DRIGHT, PAD_LS, PAD_RS };
                        for (PadButton o : others) if (pad1.Pressed(o)) b2Other = true;
                    }
                    if (!back2 && prevBack2 && !b2Other && !dinocontrol::Controls(1)) input_map::TapKeyName(c.mgr_BACK_TAP);
                    prevBack2 = back2;
                } else
                if (pad0.Connected()) {
                    // Controller Back toggles player mode (same as the keyboard key).
                    // Back: TAP = Research screen (chart), HOLD 0.6 s = toggle Ranger mode.
                    bool back = pad0.Down(PAD_BACK) && !splitBack;   // split + p1_back: handled above
                    if (back && !prevBack) { backT0 = GetTickCount(); backFired = false; }
                    if (back && !backFired && c.rangerHoldBack && GetTickCount() - backT0 >= 600) {
                        backFired = true; PlayerMode::Toggle();
                    }
                    if (!back && prevBack && !backFired &&
                        PlayerMode::Current() == Mode::MANAGER)
                        input_map::TapKeyName(c.mgr_BACK_TAP);
                    prevBack = back;

                    // In MANAGER mode, drive the stock menus/camera with the pad.
                    if (PlayerMode::Current() == Mode::MANAGER)
                        input_map::DriveManager(pad0, 0);
                    // (Player/vehicle modes read the pad through PlayerMode once built.)
                }
                if (c.padPlayer2 && !splitscreen::CanSwitch()) pad1.Poll(c.deadzone, c.triggerThreshold);
            }
            Sleep(8); // ~120 Hz poll; smooth for menus, cheap.
        }

        input_map::Shutdown();
        hooks::Remove();
        PlayerMode::Shutdown();
        LOG("mod: stopped");
        modlog::Shutdown();
        return 0;
    }
}

namespace mod {
    void Start() {
        g_run = true;
        g_thread = CreateThread(nullptr, 0, ThreadMain, nullptr, 0, nullptr);
    }
    void Stop() {
        g_run = false;
        if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = nullptr; }
    }
}
