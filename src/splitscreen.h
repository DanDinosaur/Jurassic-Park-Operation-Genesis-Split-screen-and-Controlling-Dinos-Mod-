// splitscreen.h - local 2-player split-screen co-op (layer 1: two live views, P2 camera on pad 2).
#pragma once
namespace splitscreen {
    bool Install();                        // redirect SimJP's TScene::Render import; hook Options toggle
    void Tick(void* renderer, float dt);   // game thread, from the ARenderer::OnUpdate hook
    bool Active();
    // Two-player control of the ONE game cursor/camera: whoever is acting owns it.
    int  ActivePlayer();          // 0 = player 1, 1 = player 2 (always 0 when not split)
    bool MenuLocked();            // a full-screen menu is open: no cursor hand-over until it closes
    bool CanSwitch();             // split on and player 2 has a real game camera
    void RequestActive(int p);    // controller thread -> applied on the game thread next frame
    // Cameras per player (rumble uses them): nullptr when that player has no camera of their own.
    void* CameraOfPlayer(int p);
    int   PlayerOfCamera(void* cam);   // 1 for player 2's camera, else 0
    // The matrix of the camera the main scene is drawn with right now (the active player's view).
    bool  ViewMatrix(float* out16);
    // After a camera-type switch (dino control's follow camera -> back to the world camera), make the
    // game's new active camera the acting player's own camera.
    void  AdoptActiveCamera();
    // JPOG34: show the game's hint text in player p's half for `seconds` (moved back to the HUD half after).
    void  HintToHalf(int p, float seconds);
    // JPOG52: queue the game's terrain UI footprint (one scene pass) - render hook only, right before that half's pass.
    bool  QueueGroundCircle(const float* pos, float size, const float* col);
    // JPOG35: hold RT + left stick = pan the HUD player's camera directly (controller thread -> game thread).
    void  SetRtPan(bool on, float x, float y);
    // JPOG49: GUI-unit offset from the HUD player's half to player p's half (for player 2's own disc menu).
    bool  GuiOffsetTo(int p, int& dx, int& dy);
}
