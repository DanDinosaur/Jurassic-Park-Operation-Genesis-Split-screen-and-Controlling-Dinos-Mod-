// hooks.h - installs the per-frame detour on ARootTask::OnUpdate(float).
// This is the single entry point the feature modules (player mode, camera,
// shooter, vehicles, split-screen) tick from. See game.h addr::ARootTask_OnUpdate.
#pragma once
namespace hooks {
    bool Install();   // returns true if the frame hook went in
    void Remove();
}
