// padcursor.h - JPOG59: a pad cursor over ANY game window, without the screen's one keyboard focus.
//
// Used for (open list items 1 + 4):
//   1. Player 2's own windows (Manager / Dinosaurs / Actions windows, unit + building windows): D-pad / stick = move the
//      highlight between the window's buttons, A = click. Player 1's focus is never taken (Keep in p2menu restores it).
//   4. The HUD player's vanilla info bar (AUnitInfoState's dialog: Info / Summary / Orders ...): D-pad = highlight,
//      A = click, in console style, instead of switching to the mouse.
// How (TGuiInterface / PGUICore, checked in the decompile):
//   - children: list head at widget+0x44 (first child = [+0x4C], next sibling = [child+8], end = parent+0x44),
//     parent = [+0x44], position +0x6C/+0x6E (relative to the parent), size +0x70/+0x72, state flags +0x88
//     (1 enabled, 2 focused look, 4 visible).
//   - candidates = deepest widgets with AcceptsFocus (vfn +0x40) and CanFocus (enabled + visible up the chain).
//   - move = nearest candidate in that direction (GUI coordinates; no focus-map numbering needed).
//   - highlight = TGUIWidget::SetFocused(true) (state bit 2 = the widget's own "focused" look), off again on move/end.
//   - click = what TGUIScreen::DoButtonPress/Release do for the focused widget: vfn +0x60 OnButtonPress(ev, 1) up the
//     parent chain until one takes it, then vfn +0x68 OnButtonRelease(ev, 1) on the same widget (PGUIButton: action
//     bit 1 = click on release -> vfn +0xA8, its "pressed" emitter).
#pragma once
namespace padcursor {
    struct Cursor {
        void* root = nullptr;   // the window's top widget
        void* cur  = nullptr;   // highlighted widget (nullptr = none yet)
        int   logs = 0;
        const char* who = "";
    };
    // Game thread only.
    bool Begin(Cursor& c, void* root, const char* who);   // picks the window's default focus / the top-left button
    void End(Cursor& c);                                  // removes the highlight
    bool Move(Cursor& c, int dx, int dy);                 // dx/dy = -1/0/1 (GUI y grows downwards)
    bool Click(Cursor& c);                                // true = a widget took the press
    bool Valid(const Cursor& c);                          // root still on the screen
    void Refresh(Cursor& c);                              // re-pick if the highlighted widget vanished / got disabled

    // JPOG59 item 4: the HUD player's vanilla info bar (AUnitInfoState). Tick = game thread.
    void Tick(float dt);
    bool HudActive();                                     // controller thread: the info bar cursor is up
    void RequestHud(int dx, int dy, bool click);          // controller thread: queue a move / click
}
