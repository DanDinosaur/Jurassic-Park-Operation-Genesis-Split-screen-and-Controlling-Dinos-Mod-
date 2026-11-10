// partguard.h - JPOG73 (owner 14:07: crash ~45 s after the "twister" cheat, GitHub build JPOG69):
// crash SimJP.exe+0x2CA6A1 (fadds (%eax,%edx), data 44838C0C) inside an unnamed particle-emitter function 0x6C9170
// (vtable 0xD545C0 slot 4, right next to AShellParticles / AParticleSystem). It reads a grid table
// (shift [0x842C00], mask [0x842C20], rows at this+0x28) with a particle position - a particle outside the map (the
// tornado's debris) indexes past the table. The vtable slot now calls the original inside GUARDED, with the x87 state
// saved before and restored after a fault: a faulting particle frame is skipped instead of crashing. [display] particle_guard.
#pragma once
namespace partguard { void Install(); }
