#include <windows.h>
#include "input_map.h"
#include "kbm.h"
#include "padcursor.h"
#include "minipad.h"
#include "xinput_pad.h"
#include "config.h"
#include "log.h"
#include "gamectl.h"
#include "dinocontrol.h"
#include "splitscreen.h"
#include <string>
#include <cwchar>
#include <unordered_map>
#include <vector>
#include <cstdlib>
#include <cmath>

// Phase-1 controller: XInput pad -> SendInput keystrokes/mouse, so the game's
// stock keyboard/mouse control is driven by the controller. Works with no game
// internals, so it is the first milestone that runs on a real build.

namespace {
    // --- send helpers ---
    // The game reads the keyboard through DirectInput (TInputDXDeviceKeyboard, DIK
    // codes), which only sees hardware SCANCODES - a VK-only SendInput is invisible
    // to it. So send scancode events (+ extended flag for arrows/nav keys).
    bool IsExtended(WORD vk) {
        switch (vk) {
            case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
            case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
            case VK_PRIOR: case VK_NEXT: case VK_RCONTROL: case VK_RMENU: case VK_DIVIDE:
                return true;
        }
        return false;
    }
    // JPOG33 (owner bug 2: Windows "ding" from pad presses at start-up): keys/clicks only go out when the
    // game's own top-level window is in front, visible, ENABLED (a disabled window = a modal box is up) and
    // not minimised, and the game has rendered a few frames. Never send Alt / Windows / system keys.
    volatile LONG g_gameFrames = 0;
    HWND  g_lastBlockFg = (HWND)-1;
    bool  g_lastAllowed = true;
    bool  Allowed() {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid);
        const bool ours = fg && pid == GetCurrentProcessId();
        // JPOG52 (backlog 24, every launch): the game window was in front but had NO keyboard focus (started from the
        // .bat, Windows' foreground lock) - keys then arrive as WM_SYSKEYDOWN and DefWindowProc plays the "ding" until
        // the owner Alt+Tabs and clicks. GameFrame gives the window focus (keys are NOT held back meanwhile: DirectInput
        // reads them without a focus window, so holding them back could leave the pad dead if that ever failed).
        const bool ok = ours && g_gameFrames > 30 && IsWindowVisible(fg) && IsWindowEnabled(fg) && !IsIconic(fg) &&
                        !GetWindow(fg, GW_OWNER);
        if (!ok && (fg != g_lastBlockFg || g_lastAllowed)) {
            g_lastBlockFg = fg;
            char cls[64]{}, t[64]{}; if (fg) { GetClassNameA(fg, cls, sizeof(cls)); GetWindowTextA(fg, t, sizeof(t)); }
            LOG("input: pad keys held back (fg=%p class '%s' title '%s' ours=%d frames=%ld visible=%d enabled=%d owner=%p)",
                fg, cls, t, ours, (long)g_gameFrames, fg ? IsWindowVisible(fg) : 0, fg ? IsWindowEnabled(fg) : 0,
                fg ? GetWindow(fg, GW_OWNER) : nullptr);
        }
        if (ok && !g_lastAllowed) {
            char cls[64]{}; GetClassNameA(fg, cls, sizeof(cls));
            GUITHREADINFO gi{}; gi.cbSize = sizeof(gi); char fcls[64]{};
            if (GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gi) && gi.hwndFocus) GetClassNameA(gi.hwndFocus, fcls, sizeof(fcls));
            LOG("input: pad keys go to the game window again (fg=%p class '%s', keyboard focus %p class '%s')", fg, cls, gi.hwndFocus, fcls);
        }
        g_lastAllowed = ok;
        return ok;
    }
    bool IsSystemKey(WORD vk) {
        return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS ||
               vk == VK_F10 || vk == VK_SNAPSHOT || vk == VK_PAUSE || vk == VK_CANCEL;
    }
    void SendKey(WORD vk, bool up) {
        if (!vk || IsSystemKey(vk)) return;
        static bool s_down[256]{};
        if (up) { if (!s_down[vk & 0xFF]) return; s_down[vk & 0xFF] = false; }   // only release what we pressed
        else    { if (!Allowed()) return; s_down[vk & 0xFF] = true; }
        INPUT in{}; in.type = INPUT_KEYBOARD;
        in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0) |
                        (IsExtended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(1, &in, sizeof(in));
    }
    void KeyDown(WORD vk) { SendKey(vk, false); }
    void KeyUp(WORD vk)   { SendKey(vk, true);  }
    void MouseButton(bool left, bool down) {
        static bool s_down[2]{};
        bool& d = s_down[left ? 0 : 1];
        if (down) { if (!Allowed()) return; d = true; } else { if (!d) return; d = false; }
        INPUT in{}; in.type = INPUT_MOUSE;
        if (left)  in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN  : MOUSEEVENTF_LEFTUP;
        else       in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        SendInput(1, &in, sizeof(in));
    }
    void MouseMoveRel(int dx, int dy) {
        if (!dx && !dy) return;
        if (!Allowed()) return;
        INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_MOVE;
        in.mi.dx = dx; in.mi.dy = dy; SendInput(1, &in, sizeof(in));
    }

    // Resolve an action string from the ini ("ENTER","ESCAPE","B","MOUSE_L"...) to a
    // concrete emitter. We track held state so we only send edges.
    enum ActKind { AK_NONE, AK_KEY, AK_MOUSE_L, AK_MOUSE_R, AK_SPECIAL };
    struct Action { ActKind kind = AK_NONE; WORD vk = 0; std::string special; };

    WORD NamedVK(const std::string& s) {
        if (s == "ENTER")  return VK_RETURN;
        if (s == "ESCAPE") return VK_ESCAPE;
        if (s == "SPACE")  return VK_SPACE;
        if (s == "TAB")    return VK_TAB;
        // The game's own "step through" actions (Data\GUI\controls.ini, PC section):
        // PrevSelection = [, NextSelection = ], NextPage = \  (the Xbox version puts them on LB/RB).
        if (s == "LBRACKET")  return VK_OEM_4;
        if (s == "RBRACKET")  return VK_OEM_6;
        if (s == "BACKSLASH") return VK_OEM_5;
        if (s == "PGUP")      return VK_PRIOR;
        if (s == "PGDN")      return VK_NEXT;
        if (s == "DELETE")    return VK_DELETE;
        if (s.size() == 1) return (WORD)toupper(s[0]);
        if ((s.size() == 2 || s.size() == 3) && (s[0] == 'F' || s[0] == 'f')) {
            int f = atoi(s.c_str() + 1);
            if (f >= 1 && f <= 12) return (WORD)(VK_F1 + f - 1);
        }
        return 0;
    }
    Action Parse(const std::string& s) {
        Action a;
        if (s == "MOUSE_L") { a.kind = AK_MOUSE_L; return a; }
        if (s == "MOUSE_R") { a.kind = AK_MOUSE_R; return a; }
        if (s == "ARROWS" || s == "MOUSE_MOVE" || s == "CAMERA") { a.kind = AK_SPECIAL; a.special = s; return a; }
        WORD vk = NamedVK(s);
        if (vk) { a.kind = AK_KEY; a.vk = vk; }
        return a;
    }

    struct Binding { PadButton btn; Action act; };
    std::vector<Binding> g_btnBinds;
    Action g_lstick, g_rstick, g_dpadAct, g_rt, g_lt;
    bool   g_heldLT = false, g_heldRT = false;
    bool   g_rsHeld[4]{};
    bool   g_arrowHeld[4]{};                 // up, down, left, right (D-pad and, in console mode, left stick)
    volatile LONG g_console = -1;            // -1 = not read yet
    unsigned g_moveLog = 0;
    bool   g_dinoMode = false;             // this pad's player controls a dino (camera keys only)
    bool   g_rbCombo = false;              // RB was used as a modifier (no RB tap on release)
    // JPOG35 (owner 14:25 item 5): RT held in the park view = pan mode. RT's own action (console: Actions disc,
    // PC style: left click) fires on RELEASE if the left stick was not used while it was held.
    bool   g_rtPark = false;               // RT went down in the park view (deferred action)
    bool   g_miniTrig = false; int g_miniLogs = 0;   // JPOG59: triggers sent as mouse buttons in a mini game
    bool   g_rtAsA = false; int g_rtAsALogs = 0; // JPOG36: RT acts as A outside the park view (console style)
    bool   g_rtStick = false;              // the stick was used while RT was held (= it was a pan, no action)
    bool   g_rtPanning = false;
    int    g_discLogs = 0;
    bool   g_tabHeld = false;              // left-stick click = TAB (tactical map) held
    // JPOG39: console style on a screen with no focused widget = A clicks at the pad-driven pointer (gamectl::CursorFallback).
    // What A / RT-as-A pressed is remembered, so the release always matches the press even if the screen changes in between.
    const Action kClick{ AK_MOUSE_L, 0, "" };
    const Action* g_aDown = nullptr;        // what A pressed (ENTER binding or the click)
    const Action* g_rtADown = nullptr;      // what RT-as-A pressed
    unsigned g_fbMoveLog = 0;

    void Emit(const Action& a, bool down) {
        switch (a.kind) {
            case AK_KEY:     down ? KeyDown(a.vk) : KeyUp(a.vk); break;
            case AK_MOUSE_L: MouseButton(true,  down); break;
            case AK_MOUSE_R: MouseButton(false, down); break;
            default: break;
        }
    }
}

namespace input_map {

void Init() {
    const Config& c = config::Get();
    g_btnBinds = {
        { PAD_A,     Parse(c.mgr_A)     },
        { PAD_B,     Parse(c.mgr_B)     },
        { PAD_X,     Parse(c.mgr_X)     },
        { PAD_Y,     Parse(c.mgr_Y)     },
        { PAD_LB,    Parse(c.mgr_LB)    },
        { PAD_RB,    Parse(c.mgr_RB)    },
        { PAD_BACK,  Parse(c.mgr_BACK)  },
        { PAD_START, Parse(c.mgr_START) },
        { PAD_RS,    Parse(c.mgr_RS)    },
    };
    g_lstick  = Parse(c.mgr_LSTICK);
    g_rstick  = Parse(c.mgr_RSTICK);
    g_dpadAct = Parse(c.mgr_DPAD);
    g_rt      = Parse(c.mgr_RT);
    g_lt      = Parse(c.mgr_LT);
    LOG("input_map: manager bindings ready (%zu buttons)", g_btnBinds.size());
}

// Right stick -> camera rotate/zoom via the game's own keys (Options.ini: RotateLeft=Q RotateRight=E
// ZoomIn=R ZoomOut=F). Also used while a player controls a dino (orbit around the follow camera).
static void DriveCameraKeys(const XPad& pad, bool riding = false) {
    if (g_rstick.kind != AK_SPECIAL || g_rstick.special != "CAMERA") return;
    auto hold = [](bool on, bool& held, WORD vk) {
        if (on != held) { on ? KeyDown(vk) : KeyUp(vk); held = on; }
    };
    const float t = 0.45f;
    hold(pad.RX() < -t, g_rsHeld[0], 'Q');
    hold(pad.RX() >  t, g_rsHeld[1], 'E');
    hold(pad.RY() >  t || (riding && pad.Down(PAD_DRIGHT)), g_rsHeld[2], 'R');   // JPOG55 (backlog 13): D-pad Right/Left = camera
    hold(pad.RY() < -t || (riding && pad.Down(PAD_DLEFT)),  g_rsHeld[3], 'F');   //   up/down while riding (single player follow camera)
}

void DriveManager(const XPad& pad, int player) {
    const Config& c = config::Get();
    const bool console = ConsoleMode();
    // JPOG37 (owner bug 5): demolish tool in PC-cursor style = the cursor is centre-locked (splitscreen::Tick) and the left
    // stick pans the view like console style; the buttons keep their PC meaning (RT = click = demolish at the centre).
    // JPOG39: console style + a screen with no focused widget = pad cursor (stick = pointer, A = click), see gamectl.
    const bool fallback = console && gamectl::CursorFallback();
    // JPOG59 (open list 9): in a vehicle / mini game the sticks go straight to the game (minipad), not to arrow keys.
    const bool mini = console && minipad::Active();
    const bool stickPans = ((console && !fallback) || gamectl::DemolishActive()) && !mini;

    // This player controls a dino: only the camera keys (dinocontrol reads the rest of the pad).
    if (dinocontrol::Controls(player)) {
        if (!g_dinoMode) { Shutdown(); g_dinoMode = true; }
        // Split-screen: the dino player has their own chase camera; the game camera is the builder's.
        if (!splitscreen::CanSwitch()) DriveCameraKeys(pad, true);
        return;
    }
    if (g_dinoMode) { Shutdown(); g_dinoMode = false; }

    // RB is a modifier: RB + D-pad Up = control the selected dino. RB alone (tap, no D-pad used) keeps
    // its normal action, sent on release.
    // Debug ([debug] test_dino): Back + D-pad Right = spawn two test dinos and select one.
    if (!c.testDino.empty() && pad.Down(PAD_BACK) && pad.Pressed(PAD_DRIGHT)) {
        LOG("pad: Back + D-pad Right -> debug test dinos");
        dinocontrol::RequestTestSpawn();
        return;
    }
    if (pad.Down(PAD_RB)) {
        if (pad.Pressed(PAD_RB)) g_rbCombo = false;
        if (pad.Pressed(PAD_DUP)) {
            g_rbCombo = true;
            // JPOG55 (backlog 2): A on a dino already takes control; RB + D-pad Up = vanilla info bar again ([coop] rb_up_info).
            if (c.rbUpInfo) { LOG("pad: RB + D-pad Up -> info bar of the selected unit (player %d)", player + 1); dinocontrol::RequestInfo(player); }
            else { LOG("pad: RB + D-pad Up -> control the selected dinosaur (player %d)", player + 1); dinocontrol::RequestControl(player); }
            return;
        }
        if (pad.Pressed(PAD_DDOWN) || pad.Pressed(PAD_DLEFT) || pad.Pressed(PAD_DRIGHT)) g_rbCombo = true;
    }
    const bool rbTap = pad.Released(PAD_RB) && !g_rbCombo;
    // JPOG59 (open list 4 / B3): the vanilla info bar (Info / Summary / Orders) from the pad: D-pad = highlight a button,
    // A = press it (padcursor, game thread). D-pad and A are taken here while the bar's cursor is up.
    const bool infoBar = console && padcursor::HudActive() && !pad.Down(PAD_RB);
    if (infoBar) {
        // JPOG62 (owner 13:17): D-pad AND left stick, with repeat (0.4 s first, 0.22 s after) like player 2's windows.
        static DWORD s_next = 0;
        float x = pad.LX(), y = -pad.LY();
        if (pad.Down(PAD_DLEFT)) x = -1; if (pad.Down(PAD_DRIGHT)) x = 1;
        if (pad.Down(PAD_DUP)) y = -1;   if (pad.Down(PAD_DDOWN)) y = 1;
        const bool edge = pad.Pressed(PAD_DLEFT) || pad.Pressed(PAD_DRIGHT) || pad.Pressed(PAD_DUP) || pad.Pressed(PAD_DDOWN);
        int dx = 0, dy = 0;
        const DWORD now = GetTickCount();
        if (fabsf(x) > 0.5f || fabsf(y) > 0.5f) {
            if (edge || !s_next || (LONG)(now - s_next) >= 0) {
                s_next = now + ((edge || !s_next) ? 400 : 220);
                dx = fabsf(x) >= fabsf(y) ? (x > 0 ? 1 : -1) : 0;
                dy = dx ? 0 : (y > 0 ? 1 : -1);
            }
        } else s_next = 0;
        if (dx || dy || pad.Pressed(PAD_A)) padcursor::RequestHud(dx, dy, pad.Pressed(PAD_A));
    }

    // Right-stick click: switch console-style <-> PC cursor controls (saved to jpogmod.ini).
    // JPOG73 rev d (log 15:08:37): R3 in the helicopter switched the whole game to PC style mid-flight (mouse pointer back,
    // minipad off). No style switch while a vehicle / mini game is driven.
    if (pad.Pressed(PAD_RS)) { if (minipad::Active()) LOG("controls: R3 ignored while driving / flying (no style switch)"); else SetConsoleMode(!ConsoleMode()); }

    if (console) {
        // LB = [ (PrevSelection / rotate footprint), held; RB tap = ] (NextSelection).
        if (pad.Pressed(PAD_LB))  KeyDown(VK_OEM_4);
        if (pad.Released(PAD_LB)) KeyUp(VK_OEM_4);
        if (rbTap) { KeyDown(VK_OEM_6); Sleep(30); KeyUp(VK_OEM_6); }
        // HUD discs through the game's own toolbar (gamectl), only in the park view.
        const bool pv = gamectl::ParkViewActive();
        if (pv && pad.Pressed(PAD_A)) dinocontrol::NoteAPress(player);   // JPOG57: A on a dino = take control (HUD player too)
        if (pad.Pressed(PAD_Y) && pv) gamectl::RequestMenu(gamectl::MENU_MANAGER);
        if (pad.Pressed(PAD_START)) {
            if (pv) gamectl::RequestMenu(gamectl::MENU_START);
            else { KeyDown(VK_ESCAPE); Sleep(30); KeyUp(VK_ESCAPE); }   // close the open screen
        }
        // Left-stick click = TAB (tactical map), held.
        bool ls = pad.Down(PAD_LS);
        if (ls != g_tabHeld) { ls ? KeyDown(VK_TAB) : KeyUp(VK_TAB); g_tabHeld = ls; }
    } else if (rbTap) {
        for (auto& b : g_btnBinds) if (b.btn == PAD_RB) { Emit(b.act, true); Sleep(30); Emit(b.act, false); }
    }

    for (auto& b : g_btnBinds) {
        if (b.btn == PAD_RB || b.btn == PAD_RS) continue;
        if (console && (b.btn == PAD_Y || b.btn == PAD_LB || b.btn == PAD_START)) continue;
        if (pad.Pressed(b.btn))  LOG("pad: %d down", (int)b.btn);
        if (b.btn == PAD_A) {
            if (infoBar && pad.Pressed(PAD_A)) continue;   // JPOG59: A presses the highlighted info-bar button
            // JPOG39: A = click on a pointer screen (console style), else its binding (ENTER = select / place).
            if (pad.Pressed(PAD_A)) { g_aDown = fallback ? &kClick : &b.act; Emit(*g_aDown, true); }
            if (pad.Released(PAD_A) && g_aDown) { Emit(*g_aDown, false); g_aDown = nullptr; }
            continue;
        }
        if (pad.Pressed(b.btn))  Emit(b.act, true);
        if (pad.Released(b.btn)) Emit(b.act, false);
    }

    // Triggers. Console: RT = Actions disc, LT = Dinosaurs disc. PC: hold-buttons (RT = left click, LT = right).
    bool rt = pad.RT() > 0.5f, lt = pad.LT() > 0.5f;
    // JPOG59 (open list 9 / B22): vehicle / mini game = RT fire (mouse left, held), LT = mouse right (held).
    if (mini) {
        if (rt != g_heldRT) { MouseButton(true, rt);  if (g_miniLogs < 6) { ++g_miniLogs; LOG("pad: RT %s -> fire (mini game)", rt ? "down" : "up"); } }
        if (lt != g_heldLT) { MouseButton(false, lt); if (g_miniLogs < 6) { ++g_miniLogs; LOG("pad: LT %s -> right button (mini game)", lt ? "down" : "up"); } }
        g_heldRT = rt; g_heldLT = lt; g_rtPark = false; g_miniTrig = true;
        if (g_rtAsA) { g_rtAsA = false; if (g_rtADown) { Emit(*g_rtADown, false); g_rtADown = nullptr; } }
        for (int i = 0; i < 4; ++i) if (g_arrowHeld[i]) { static const WORD vk[4] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT }; KeyUp(vk[i]); g_arrowHeld[i] = false; }
        for (int i = 0; i < 4; ++i) if (g_rsHeld[i]) { static const WORD vk[4] = { 'Q', 'E', 'R', 'F' }; KeyUp(vk[i]); g_rsHeld[i] = false; }
        return;                                     // the sticks go to the game itself (minipad), no camera / arrow keys
    }
    if (g_miniTrig) {                               // left the mini game with a trigger held: release it
        g_miniTrig = false;
        if (g_heldRT) MouseButton(true, false);
        if (g_heldLT) MouseButton(false, false);
        g_heldRT = g_heldLT = false;
    }
    // JPOG35: hold RT + left stick in the park view = pan the camera directly (splitscreen::SetRtPan, game thread).
    if (rt && !g_heldRT) { g_rtPark = gamectl::ParkViewActive(); g_rtStick = false; }
    const bool rtPan = rt && g_rtPark;
    if (rtPan) {
        const float sx = pad.LX(), sy = pad.LY();
        if (sx * sx + sy * sy > 0.09f) g_rtStick = true;
        if (g_rtStick) {
            if (!g_rtPanning) { g_rtPanning = true; LOG("pad: RT held + stick -> pan the camera (player %d)", player + 1); }
            splitscreen::SetRtPan(true, sx, sy);
        }
    } else if (g_rtPanning) { g_rtPanning = false; splitscreen::SetRtPan(false, 0, 0); }
    const bool rtTapUp = !rt && g_heldRT && g_rtPark && !g_rtStick;   // released without panning
    const bool rtDeferred = g_rtPark;                                   // this RT press belongs to the park view
    if (console) {
        if (rtTapUp && gamectl::ParkViewActive()) { LOG("pad: RT tap -> Actions disc"); gamectl::RequestMenu(gamectl::MENU_ACTIONS); }
        // JPOG36 (owner item 7): outside the park view (menus, ring menus, dialogs, front end) RT = A (select).
        if (rt && !g_heldRT && !g_rtPark) {
            g_rtAsA = true;
            if (g_rtAsALogs < 4) { ++g_rtAsALogs; LOG("pad: RT -> A (select) outside the park view"); }
            g_rtADown = nullptr;
            if (fallback) g_rtADown = &kClick; else for (auto& b : g_btnBinds) if (b.btn == PAD_A) g_rtADown = &b.act;
            if (g_rtADown) Emit(*g_rtADown, true);
        } else if (!rt && g_rtAsA) {
            g_rtAsA = false;
            if (g_rtADown) { Emit(*g_rtADown, false); g_rtADown = nullptr; }
        }
        if (lt && !g_heldLT && gamectl::ParkViewActive()) gamectl::RequestMenu(gamectl::MENU_DINOSAURS);
        g_heldRT = rt; g_heldLT = lt;
        // JPOG35 (item 4): ring (disc) menus - Start, Manager, Actions... - pick their item from the direction of a
        // MOUSE move (PGUIDiscMenuDialog::OnAxisMove reads axis 0x20001 = mouse, or a DirectInput pad's stick; arrow
        // keys do nothing there - log 13:57 "NOTHING FOCUSED"). So D-pad / left stick = a small mouse move that way.
        // JPOG39: pointer screen (no focused widget) - the left stick moves the mouse like PC style.
        if (fallback && g_lstick.kind == AK_SPECIAL && g_lstick.special == "MOUSE_MOVE") {
            int dx = (int)(pad.LX() * 14.0f * c.cursorSpeed), dy = (int)(-pad.LY() * 14.0f * c.cursorSpeed);
            if ((dx || dy) && (g_fbMoveLog++ % 120) == 0) LOG("pad: pointer move %d,%d (screen without a focused button)", dx, dy);
            MouseMoveRel(dx, dy);
        }
        if (gamectl::DiscMenuFocused()) {
            float dx = pad.LX(), dy = -pad.LY();
            if (pad.Down(PAD_DLEFT)) dx = -1; if (pad.Down(PAD_DRIGHT)) dx = 1;
            if (pad.Down(PAD_DUP)) dy = -1;   if (pad.Down(PAD_DDOWN)) dy = 1;
            const float m = sqrtf(dx * dx + dy * dy);
            if (m > 0.5f) {
                MouseMoveRel((int)(dx / m * 12.0f), (int)(dy / m * 12.0f));
                if (g_discLogs < 6) { ++g_discLogs; LOG("pad: ring menu - pointing %.2f,%.2f (mouse move)", dx / m, dy / m); }
            }
        }
    } else {
        if (rtDeferred) {
            // park view: the click is sent whole on release, unless RT was used to pan
            if (rtTapUp) { LOG("pad: RT tap -> click"); Emit(g_rt, true); Sleep(30); Emit(g_rt, false); }
            g_heldRT = rt;
        } else if (rt != g_heldRT) { LOG("pad: RT %s", rt ? "down" : "up"); Emit(g_rt, rt); g_heldRT = rt; }
        if (lt != g_heldLT) { Emit(g_lt, lt); g_heldLT = lt; }
        // Left stick -> cursor (mouse move) in PC mode. In console mode the stick is the game's own
        // Up/Down/Left/Right (pans the camera, moves the highlight in the build ring / menus); the cursor
        // stays locked in the middle of the player's view (splitscreen::Tick) so A acts on what is there.
        if (g_lstick.kind == AK_SPECIAL && g_lstick.special == "MOUSE_MOVE" && !stickPans) {
            int dx = rtPan ? 0 : (int)(pad.LX() * 14.0f * c.cursorSpeed);
            int dy = rtPan ? 0 : (int)(-pad.LY() * 14.0f * c.cursorSpeed);
            if ((dx || dy) && (g_moveLog++ % 60) == 0) LOG("pad: cursor move %d,%d", dx, dy);
            MouseMoveRel(dx, dy);
        }
    }
    if (!rt) g_rtPark = false;
    DriveCameraKeys(pad);
    // D-pad (and the left stick in console mode) -> arrow keys (camera / menu nav).
    if (g_dpadAct.kind == AK_SPECIAL && g_dpadAct.special == "ARROWS") {
        const float t = 0.5f;
        // While RB is held the D-pad is RB's modifier (RB + D-pad Up = dino control), not arrows.
        const bool rbHeld = pad.Down(PAD_RB);
        // JPOG46 (backlog 5): in a placement tool the D-pad Left/Right rotate the building (Up/Down already do).
        const bool rotTool = gamectl::PlacementRotate() && !rbHeld;
        const bool padInfo = infoBar;   // JPOG59: the D-pad moves the info-bar highlight, not the camera
        if (rotTool && pad.Pressed(PAD_DLEFT))  gamectl::RequestRotate(-1);
        if (rotTool && pad.Pressed(PAD_DRIGHT)) gamectl::RequestRotate(1);
        const bool want[4] = {
            (pad.Down(PAD_DUP) && !rbHeld && !padInfo) || (stickPans && !rtPan && !padInfo && pad.LY() >  t),
            (pad.Down(PAD_DDOWN)  && !rbHeld && !padInfo) || (stickPans && !rtPan && !padInfo && pad.LY() < -t),
            (pad.Down(PAD_DLEFT)  && !rbHeld && !rotTool && !padInfo) || (stickPans && !rtPan && !padInfo && pad.LX() < -t),
            (pad.Down(PAD_DRIGHT) && !rbHeld && !rotTool && !padInfo) || (stickPans && !rtPan && !padInfo && pad.LX() >  t) };
        const WORD vks[4] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
        for (int i = 0; i < 4; ++i) if (want[i] != g_arrowHeld[i]) {
            if (want[i]) { LOG("pad: arrow %d down", i); KeyDown(vks[i]); } else KeyUp(vks[i]);
            g_arrowHeld[i] = want[i];
        }
    }
}

static void IniPathW(wchar_t* p) {
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    if (wchar_t* sl = wcsrchr(p, L'\\')) *(sl + 1) = 0;
    wcscat_s(p, MAX_PATH, L"jpogmod.ini");
}
// JPOG59 (open list 6 / B4): console style only while a controller is connected. With no pad the mouse is free
// (no centre lock, no aim circle, pointer shown) and the game's own PC prompts are what the player sees.
// [controller] pc_without_pad = 1.
bool ConsoleActive() {
    static int s_last = -1;
    const bool pad = padslots::SlotOf(0) >= 0 || padslots::SlotOf(1) >= 0;
    // JPOG70: player 1 switched to keyboard + mouse -> PC style while player 1 has the HUD (vanilla pointer + prompts).
    const bool p1Kbm = kbm::IsKbm(0) && (!splitscreen::CanSwitch() || splitscreen::ActivePlayer() == 0);
    const bool on = !p1Kbm && ConsoleMode() && (pad || !config::Get().pcWithoutPad);
    if ((int)on != s_last) {
        if (s_last >= 0 || !on) LOG("controls: %s", on ? "controller connected - console style" : "no controller - PC style (mouse + keyboard)");
        s_last = on;
    }
    return on;
}
bool ConsoleMode() {
    if (g_console < 0) {
        wchar_t ini[MAX_PATH]{}; IniPathW(ini);
        g_console = GetPrivateProfileIntW(L"controller", L"console_mode", 1, ini) ? 1 : 0;
        // JPOG39 (owner task 22:08): the console scheme (JPOG23 layout + centre-locked cursor + ground circle) is THE
        // control scheme now. Installs that were switched to PC style before JPOG39 are moved to console style ONCE
        // ([controller] scheme_version = 39 marks it); the right-stick click still switches, and that choice is kept.
        if (GetPrivateProfileIntW(L"controller", L"scheme_version", 0, ini) < 39) {
            const int was = g_console;
            g_console = 1;
            WritePrivateProfileStringW(L"controller", L"console_mode", L"1", ini);
            WritePrivateProfileStringW(L"controller", L"scheme_version", L"39", ini);
            LOG("controls: JPOG39 - console style set as the default scheme (ini had console_mode = %d)", was);
        }
        // JPOG72 (owner test 12:20:51 "controls: CONSOLE style" at split start with no controller): this is only the
        // CONTROLLER scheme read from the ini; the style in use is ConsoleActive() (its own "controls:" lines).
        LOG("controls: controller scheme = %s - used only while a controller drives (style in use = the 'controls: no controller / controller connected' lines)",
            g_console ? "console (stick = menus/camera, cursor locked to the middle; R-stick click = PC cursor)"
                      : "PC (stick = cursor; R-stick click = console style)");
    }
    return g_console != 0;
}
void SetConsoleMode(bool on) {
    Shutdown();                                 // release held keys/buttons from the old style
    g_console = on ? 1 : 0;
    wchar_t ini[MAX_PATH]{}; IniPathW(ini);
    WritePrivateProfileStringW(L"controller", L"console_mode", on ? L"1" : L"0", ini);
    LOG("controls: switched to %s", on ? "CONSOLE style" : "PC cursor style");
    gamectl::RequestHint(on ? L"Console controls: stick moves the view, A selects in the middle (click the right stick for a cursor)"
                            : L"PC controls: stick moves the cursor, RT clicks (click the right stick for console controls)", 4.0f);
}

void GameFrame() {
    const LONG n = InterlockedIncrement(&g_gameFrames);
    // JPOG52 (backlog 24): game thread = the window's thread. Every 15 frames: our window is in front but nothing in it
    // has keyboard focus -> SetFocus(it) (what the owner's Alt+Tab + click did). Only our own thread's window, so
    // Windows allows it without stealing the foreground from another app.
    if (n % 15) return;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0; const DWORD tid = fg ? GetWindowThreadProcessId(fg, &pid) : 0;
    if (!fg || pid != GetCurrentProcessId() || !IsWindowEnabled(fg) || GetWindow(fg, GW_OWNER)) return;
    GUITHREADINFO gi{}; gi.cbSize = sizeof(gi);
    if (!GetGUIThreadInfo(tid, &gi) || gi.hwndFocus) return;
    const DWORD me = GetCurrentThreadId();
    const bool attach = tid != me && AttachThreadInput(me, tid, TRUE);   // SetFocus works on our thread's input queue only
    SetFocus(fg);
    if (attach) AttachThreadInput(me, tid, FALSE);
    static int s_logs = 0;
    if (s_logs < 5) { ++s_logs; GUITHREADINFO g2{}; g2.cbSize = sizeof(g2); GetGUIThreadInfo(tid, &g2);
        LOG("input: game window %p was in front without keyboard focus (start-up) - focus given to it (now %p)", fg, g2.hwndFocus); }
}
bool SendAllowed() { return Allowed(); }

bool TapButton(PadButton btn) {
    for (auto& b : g_btnBinds) if (b.btn == btn) { Emit(b.act, true); Sleep(40); Emit(b.act, false); return true; }
    return false;
}

void TapKeyName(const std::string& name) {
    Action a = Parse(name);
    if (a.kind == AK_NONE) return;
    Emit(a, true); Sleep(40); Emit(a, false);
}

void Shutdown() {
    // release anything still held
    for (auto& b : g_btnBinds) Emit(b.act, false);
    const WORD rs[4] = {'Q','E','R','F'};
    for (int i = 0; i < 4; ++i) if (g_rsHeld[i]) { KeyUp(rs[i]); g_rsHeld[i] = false; }
    const WORD av[4] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
    for (int i = 0; i < 4; ++i) if (g_arrowHeld[i]) { KeyUp(av[i]); g_arrowHeld[i] = false; }
    if (g_tabHeld) { KeyUp(VK_TAB); g_tabHeld = false; }
    // In console mode the triggers open discs (nothing held); in PC mode they are mouse buttons.
    if (!ConsoleMode()) {
        if (g_heldLT) Emit(g_lt, false);
        if (g_heldRT && !g_rtPark) Emit(g_rt, false);   // a deferred (park view) RT never sent its button-down
    }
    if (g_aDown)   { Emit(*g_aDown, false);   g_aDown = nullptr; }     // JPOG39: the click variant of A
    if (g_rtADown) { Emit(*g_rtADown, false); g_rtADown = nullptr; }
    g_rtAsA = false;   // its A key-up went out with the button releases above
    if (g_rtPanning) { g_rtPanning = false; splitscreen::SetRtPan(false, 0, 0); }
    g_rtPark = false;
    g_heldLT = false; g_heldRT = false;
    g_rbCombo = false;
}

} // namespace input_map
