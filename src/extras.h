// extras.h - JPOG67: free keyboard, Alt+F4, borderless, cursor speed / acceleration, camera smoothing at any FPS.
#pragma once
namespace extras {
    void OnDirectInput(void* di);   // proxy: right after the real DirectInput8Create
    void Install();                 // game thread, frame 60
    void Tick(unsigned frame);      // game thread, every frame
}
