#include <windows.h>
#include "detour.h"
#include "log.h"
#include <vector>

namespace {
    struct Patch { uint8_t* addr; uint8_t orig[16]; size_t len; };
    std::vector<Patch> g_patches;

    // Length of the x86 instruction at p, for the handful of prologue opcodes we
    // expect at a function entry. Returns 0 if unknown (caller aborts safely).
    size_t InsnLen(const uint8_t* p) {
        switch (p[0]) {
            case 0x50: case 0x51: case 0x52: case 0x53: // push eax..ebx
            case 0x54: case 0x55: case 0x56: case 0x57: // push esp..edi
            case 0x90:                                  // nop
            case 0xC3:                                  // ret
                return 1;
            case 0x6A:                                  // push imm8
                return 2;
            case 0x68:                                  // push imm32
                return 5;
            case 0x8B:                                  // mov r32, r/m32
            case 0x89:                                  // mov r/m32, r32
            case 0x85:                                  // test r/m32, r32
                // modrm follows; handle common reg/reg and [reg]/[reg+disp8].
                {
                    uint8_t modrm = p[1];
                    uint8_t mod = modrm >> 6, rm = modrm & 7;
                    if (mod == 3) return 2;                    // reg,reg
                    if (mod == 0) return (rm == 4) ? 3 : 2;    // [reg] (+SIB)
                    if (mod == 1) return (rm == 4) ? 4 : 3;    // [reg+disp8]
                    if (mod == 2) return (rm == 4) ? 7 : 6;    // [reg+disp32]
                }
                return 0;
            case 0x83:                                  // sub/add/... r/m32, imm8
                {
                    uint8_t mod = p[1] >> 6;
                    if (mod == 3) return 3;                    // e.g. sub esp, imm8
                }
                return 0;
            case 0x81:                                  // sub/add r/m32, imm32
                {
                    uint8_t mod = p[1] >> 6;
                    if (mod == 3) return 6;
                }
                return 0;            case 0x8D:                                  // lea
                {
                    uint8_t modrm = p[1]; uint8_t mod = modrm >> 6, rm = modrm & 7;
                    if (mod == 1) return (rm==4)?4:3;
                    if (mod == 2) return (rm==4)?7:6;
                    if (mod == 0) return (rm==4)?3:2;
                }
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
