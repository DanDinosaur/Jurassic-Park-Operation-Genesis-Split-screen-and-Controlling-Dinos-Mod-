// minipad.h - JPOG59 (open list 9 / B22): helicopter, safari jeep, drivable jeep and the other mini games from the pad.
// The PC exe still has the console versions' stick handling in these states: ProcessInput (vfn +0x14) takes
// InputEvents 0x10019 (left stick) and 0x1001a (right stick), type 3 (axis), two float magnitudes
// (ADriveVehicleState::ProcessInput 0x536290: right stick = throttle/steer, left stick = look; AMiniGameState::
// ProcessInput_Shoot 0x5450C0: left stick moves the scope / gun camera). We send the pad's sticks there every frame,
// like the console's input system did. Buttons: RT = mouse left (fire), LT = mouse right (input_map), the centre lock
// and the stick-to-arrow-keys mapping are off while a mini game runs.
#pragma once
namespace minipad {
    void Tick(float dt);    // game thread
    bool Active();          // any thread: a pad-driven mini game / vehicle state is on
}
