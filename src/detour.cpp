#include <windows.h>
#include "detour.h"
#include "log.h"
#include <vector>

namespace {
    struct Patch { uint8_t* addr; uint8_t orig[16]; size_t len; };
    std::vector<Patch> g_patches;

    // Length of the x86 instruction at p, for the handful of prologue opcodes we
    // expect at a function entry. Returns 0 if unknown (caller aborts safely).
    // modrm (+SIB +disp) length for a 32-bit r/m operand, starting at the modrm byte. 0 = not handled.
    size_t ModRmLen(const uint8_t* m) {
        uint8_t mod = m[0] >> 6, rm = m[0] & 7;
        if (mod == 3) return 1;
        size_t n = 1;
        if (rm == 4) {                                   // SIB
            ++n;
            if (mod == 0 && (m[1] & 7) == 5) return n + 4;   // [index*s + disp32]
        }
        if (mod == 0) return (rm == 5) ? n + 4 : n;      // [disp32] / [reg]
        if (mod == 1) return n + 1;
        return n + 4;                                    // mod == 2
    }

    // Length of the x86 instruction at p, for the handful of prologue opcodes we
    // expect at a function entry. Returns 0 if unknown (caller aborts safely).
    // JPOG71: + B8-BF mov r32,imm32 (win32u syscall stubs), FF 25 jmp [abs32], E8/E9 rel32 (relocated),
    //         31/33/3B/39/2B/29/03/01 r/m like 8B, 83/81 with memory operands.
    size_t InsnLen(const uint8_t* p) {
        switch (p[0]) {
            case 0x50: case 0x51: case 0x52: case 0x53: // push eax..ebx
            case 0x54: case 0x55: case 0x56: case 0x57: // push esp..edi
            case 0x58: case 0x59: case 0x5A: case 0x5B: // pop
            case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            case 0x90:                                  // nop
            case 0xC3:                                  // ret
                return 1;
            case 0x6A:                                  // push imm8
                return 2;
            case 0x68:                                  // push imm32
            case 0xB8: case 0xB9: case 0xBA: case 0xBB: // mov r32, imm32
            case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            case 0xE8: case 0xE9:                       // call/jmp rel32 (fixed up when relocated)
                return 5;
            case 0x8B: case 0x89: case 0x85:            // mov / test
            case 0x31: case 0x33: case 0x3B: case 0x39: // xor / cmp
            case 0x2B: case 0x29: case 0x03: case 0x01: // sub / add
            case 0x8D:                                  // lea
                { size_t l = ModRmLen(p + 1); return l ? 1 + l : 0; }
            case 0x83:                                  // grp1 r/m32, imm8
                { size_t l = ModRmLen(p + 1); return l ? 1 + l + 1 : 0; }
            case 0x81:                                  // grp1 r/m32, imm32
                { size_t l = ModRmLen(p + 1); return l ? 1 + l + 4 : 0; }
            case 0xFF:                                  // FF 25 = jmp [abs32]; FF /6 push r/m; FF /2 call r/m (only [abs32]/reg)
                if (p[1] == 0x25 || p[1] == 0x35 || p[1] == 0x15) return 6;
                return 0;
        }
        return 0;
    }
}

namespace detour {

bool Install(void* target, void* detourFn, void** outTrampoline) {
    uint8_t* t = reinterpret_cast<uint8_t*>(target);

    // 1) measure >=5 bytes of whole instructions to relocate.
    size_t copied = 0;
    while (copied < 5) {
        size_t l = InsnLen(t + copied);
        if (l == 0) {
            LOG("detour: unknown opcode %02X at +%zu of %p - aborting (nothing patched)",
                t[copied], copied, target);
            return false;
        }
        copied += l;
    }

    // 2) build a trampoline: [copied original bytes][JMP back to target+copied].
    uint8_t* tramp = reinterpret_cast<uint8_t*>(
        VirtualAlloc(nullptr, copied + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) { LOG("detour: VirtualAlloc failed"); return false; }
    memcpy(tramp, t, copied);
    // JPOG71: relative call/jmp moved into the trampoline must still reach the same target
    for (size_t o = 0; o < copied; ) {
        size_t l = InsnLen(t + o);
        if (t[o] == 0xE8 || t[o] == 0xE9) {
            uint8_t* dest = t + o + 5 + *reinterpret_cast<const int32_t*>(t + o + 1);
            *reinterpret_cast<int32_t*>(tramp + o + 1) = (int32_t)(dest - (tramp + o + 5));
        }
        o += l;
    }
    tramp[copied] = 0xE9;
    *reinterpret_cast<int32_t*>(tramp + copied + 1) =
        (int32_t)((t + copied) - (tramp + copied + 5));

    // 3) write JMP detour at target.
    DWORD old;
    VirtualProtect(t, copied, PAGE_EXECUTE_READWRITE, &old);
    Patch p{}; p.addr = t; p.len = copied; memcpy(p.orig, t, copied);
    g_patches.push_back(p);

    t[0] = 0xE9;
    *reinterpret_cast<int32_t*>(t + 1) =
        (int32_t)((uint8_t*)detourFn - (t + 5));
    for (size_t i = 5; i < copied; ++i) t[i] = 0x90; // pad leftover with NOPs
    VirtualProtect(t, copied, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, copied);

    *outTrampoline = tramp;
    LOG("detour: hooked %p (relocated %zu bytes) tramp=%p", target, copied, tramp);
    return true;
}

void RemoveAll() {
    for (auto& p : g_patches) {
        DWORD old;
        VirtualProtect(p.addr, p.len, PAGE_EXECUTE_READWRITE, &old);
        memcpy(p.addr, p.orig, p.len);
        VirtualProtect(p.addr, p.len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p.addr, p.len);
    }
    g_patches.clear();
}

} // namespace detour
