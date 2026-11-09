// detour.h - minimal 32-bit inline hook (5-byte E9 JMP patch + trampoline).
// Dependency-free so the groundwork needs nothing fetched. It copies the first
// whole instructions (>=5 bytes) to a trampoline so the original can still be called.
//
// LIMITATION: the trampoline copy is byte-length aware only via a tiny built-in
// length table covering the common prologue opcodes JPOG's functions use
// (push/mov/sub-esp/etc). If a target's first 5 bytes contain an opcode the table
// doesn't know, Install() returns false (logged) rather than corrupting the game -
// then either hook a cleaner address or drop in a full length-disassembler/MinHook.
// We only hook ARootTask::OnUpdate, whose prologue is standard, so this suffices.
#pragma once
#include <cstdint>

namespace detour {
    // target: address to hook. detour: your function. outTrampoline: receives a
    // pointer you can call to run the original. Returns false if it couldn't do it
    // safely (nothing is patched in that case).
    bool Install(void* target, void* detourFn, void** outTrampoline);
    void RemoveAll();
}
