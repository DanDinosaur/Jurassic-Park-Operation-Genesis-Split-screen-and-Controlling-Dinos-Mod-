// mod.h - top-level bootstrap. Called once from DllMain on load.
#pragma once
namespace mod {
    void Start();   // spawns the init thread (safe outside loader lock)
    void Stop();
}
