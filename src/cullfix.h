// cullfix.h - JPOG37: in split-screen a unit seen by EITHER half keeps the engine's full-rate update (see cullfix.cpp).
#pragma once
namespace cullfix {
    void Install();      // patch the ARenderer::Render frustum call site (once, at startup)
    void BeginFrame();   // splitscreen: before the first main-scene pass of a split frame
    void EndFrame();
    void SetPass(int half, void* rc);  // JPOG54: before each main-scene pass of a split frame (half = 0 player 1, 1 player 2)     // after the last pass: units inside any pass get SetRender(true) back
}
