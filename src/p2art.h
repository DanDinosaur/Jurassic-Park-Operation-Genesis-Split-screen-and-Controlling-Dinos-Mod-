// p2art.h - JPOG73 rev b (owner 14:19: "emulate the game art for those on-screen buttons and the mouse - look the same on
// both screens"): player 2's pointer + toolbar drawn with the GAME'S OWN pictures, read at runtime from the game's
// texture libraries (nothing of the game's art is stored in the mod):
//   Data\matlibs\gui_Main.tml   : gui1 (cursor_default 29,104 17x15), Menu_Buttons5 (Menu_Start_Options 128,192),
//                                 ingame (MailIconFlagLow 54,8 25x21)
//   Data\matlibs\gui_InGame.tml : Menu_Buttons1 (Build 0,0 / Dinosaurs 128,0 / Manager 192,64), Menu_Buttons7 (Actions 0,128)
// Section names + rects from Data\GUI\main.gui (texture_section lines), button order = player 1's PC toolbar.
// TML1 = header (+8 image count, +12 material count), images each "DDS " (DXT5) with the image id 28 bytes before,
// then 32-byte material names and 12-byte records (first dword = image id). The DXT5 blocks go straight into a
// D3DFMT_DXT5 managed texture (no decoding). Drawn as pre-transformed textured quads (alpha blend), every render
// and texture-stage state we touch saved and restored.
#pragma once
namespace p2art {
    // Game thread, inside the scene right after player 2's half was rendered. false = art not available (caller falls
    // back to the simple drawing). hover = kbm::P2B_* under the pointer.
    bool Draw(void* d3dDevice8, float hx, float hy, float hw, float hh, int hover, float ux, float uy, bool showButtons);
}
