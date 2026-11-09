// p2menu.h - JPOG49 (owner request 2026-11-09 10:00): the player WITHOUT the HUD gets the SAME build disc as the
// HUD player - the game's own disc menu, same pictures, same look - in their own half, driven by their own pad.
//
// How (option A of the kickoff doc, "a second copy of the disc widget"):
//   The disc is a PGUIMenuTree (AGUIMenu, 0x94 bytes, ctor 0x6779B0) - an INPUT MANAGER, not a widget. It clones its
//   dialogs (PGUIDiscMenuDialog) from the style templates the menu items point to, keeps them in its own list
//   (+0x24 count, +0x2C array) and adds them to the screen. Selection lives in the tree (+0x14 top, +0x18 shown menu,
//   +0x1C selected item), so a SECOND tree over the SAME menu items has its own selection and its own dialogs.
//   Player 2's tree: SetInterface + SetScreen copied from the game's tree (AGUISystem::GetMenu 0x67E500), its root
//   pointer (+0x10) = the game tree's root (UpdateMenu walks item parents up to it), margins copied.
//   Three things it must NOT take from player 1, handled around every call:
//   - InvokeMenu pushes the tree on the screen's input-manager stack (screen +0xCC count, +0xD4 slots): popped again.
//   - UpdateMenu calls RequestFocus on the screen and on its dialog: player 1's focused widget is restored after.
//   - CancelMenu pops the stack blindly: never called; closing = RemoveMenus + "invoked" flag (+0x65) off.
//   Its dialogs are then moved by the GUI offset between the halves (splitscreen::GuiOffsetTo); the HUD plugin does not
//   re-anchor them (it marks disc dialogs "self positioned").
// Pad: stick / D-pad = point at a picture (the dialog's own OnAxisMove, as the console versions did), A = pick (submenu
// opens, or the build tool starts via builder2::OpenFor), B = back (closes at the top), X = close. B in the build tool
// = back to the same submenu.
#pragma once
class XPad;
namespace p2menu {
    // Game thread, from splitscreen's inactive-player update, BEFORE builder2::Update. True = the disc used the pad.
    bool Update(int who, XPad& pad, void* cam, float dt);
    bool IsOpen(int who);
    bool InOwnWindowCall();   // JPOG60 (fix C): true while p2menu activates / deactivates player 2's own window
    bool WindowOpen(int who);   // JPOG59: player who has their own window open (the pad drives its highlight)
    void Close(int who, const char* why);
    void CloseAll(const char* why);
    int  OpenUnitWindow(int who, void* unit);   // JPOG57: the unit's own info window in player who's half (game thread)
}
