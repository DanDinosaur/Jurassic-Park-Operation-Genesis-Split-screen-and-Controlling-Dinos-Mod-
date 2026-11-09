// widescreen.h - JPOG66: our own widescreen support (replaces the third-party JPOG-Modernize plugin for what the mod needs).
//   * Hor+ : wide screens show more at the sides instead of cropping top/bottom (TCameraViewportHandler::OnBeginRenderEvent).
//   * GUI  : the 800x600 GUI is drawn with ONE uniform scale, centred in the full screen - or, in split-screen, in the HUD
//            player's half (PGUIRenderer's import of TRenderContext::SetProjectionParameters).
//   * The game's own GUI <-> screen conversions (3D picking, unit icons, highlight boxes) use the same mapping.
// Off automatically when JPOGMouseFix.asi (JPOG-Modernize) is loaded, so the two never fight.
#pragma once
namespace widescreen {
    void Install();          // game thread, first frame (after *.asi plugins are loaded)
    bool Active();           // our GUI mapping is in charge
    float SplitScale();      // HUD size factor in split-screen ([hud] split_scale)
}
