// playermode.h - the mode state machine ticked from the frame hook.
// MANAGER (stock RTS) <-> ON_FOOT <-> AIMING, plus IN_VEHICLE. Mirrors the VC
// ped/vehicle state split (design doc sec 2). Feature work lands in features.*.
#pragma once

enum class Mode { MANAGER, ON_FOOT, AIMING, ENTERING, IN_VEHICLE, EXITING };

namespace PlayerMode {
    void Init();
    // Called every frame from hooks.cpp after the game's own OnUpdate.
    // self = the ARootTask*, dt = frame delta seconds.
    void Tick(void* rootTask, float dt);
    void Shutdown();

    Mode Current();
    void Toggle();   // MANAGER <-> ON_FOOT (bound to a key / Back button)
}
