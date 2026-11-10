#include <windows.h>
#include <psapi.h>
#include "guard.h"
#include "log.h"
#include "config.h"

namespace {
    // JPOG34: a small stack of jump buffers per thread, so GUARDED() may nest (JPOG33 had ONE buffer: an inner
    // GUARDED overwrote the outer one, e.g. Attack -> NearestCb -> Dead).
    constexpr int kMaxDepth = 8;
    thread_local jmp_buf     t_jb[kMaxDepth];
    thread_local const char* t_what[kMaxDepth] = {};
    thread_local int         t_depth = 0;

    void Where(void* addr, char* out, size_t n) {
        HMODULE m = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)addr, &m) && m) {
            char name[MAX_PATH]{}; GetModuleFileNameA(m, name, MAX_PATH);
            const char* b = strrchr(name, '\\'); b = b ? b + 1 : name;
            wsprintfA(out, "%s+0x%X", b, (unsigned)((BYTE*)addr - (BYTE*)m));
        } else wsprintfA(out, "%p", addr);
        (void)n;
    }

    // JPOG78 (owner crash 2026-11-10 17:12:28, CRASH fmod.dll+0x22995 data 000001DC, on FMOD's own stream thread): the
    // stream update loop (fmod 0x2297A, called by the stream thread at 0x23455) walks the stream list at
    // [0x100399C0]+0x1538 and met a NULL link (cmp byte [esi+0x1DC],0 with esi = 0) - the list was being changed by the
    // game thread at the same moment (FMOD 3 race; the mod never touches FMOD streams). Recovery: on exactly that
    // instruction with esi = 0, continue at 0x22AEA (pop edi / pop ebx / pop esi / leave / ret = the loop's normal end);
    // the next stream tick walks the list again. [general] fmod_stream_guard = 0 = off.
    LONG FmodStreamGuard(EXCEPTION_POINTERS* ep) {
        static HMODULE s_fm = nullptr; static int s_logs = 0;
        if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !config::Get().fmodStreamGuard) return 0;
        if (!s_fm) s_fm = GetModuleHandleA("fmod.dll");
        if (!s_fm) return 0;
        BYTE* at = (BYTE*)s_fm + 0x22995;
        if ((BYTE*)ep->ExceptionRecord->ExceptionAddress != at || ep->ContextRecord->Esi >= 0x10000) return 0;
        static const BYTE kCmp[7] = { 0x80, 0xBE, 0xDC, 0x01, 0x00, 0x00, 0x00 };
        if (memcmp(at, kCmp, sizeof kCmp) != 0) return 0;
        ep->ContextRecord->Eip = (DWORD)(uintptr_t)((BYTE*)s_fm + 0x22AEA);
        if (s_logs < 20) { ++s_logs; LOG("guard: FMOD stream list had a NULL link (fmod.dll+0x22995, esi=%08lX) - stream update skipped this tick, no crash (JPOG78 [general] fmod_stream_guard)", ep->ContextRecord->Esi); }
        return 1;
    }
    LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (FmodStreamGuard(ep)) return EXCEPTION_CONTINUE_EXECUTION;
        if (t_depth > 0 && (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                            code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO)) {
            char w[300]; Where(ep->ExceptionRecord->ExceptionAddress, w, sizeof w);
            const int d = (t_depth > kMaxDepth ? kMaxDepth : t_depth) - 1;
            LOG("guard: FAULT %08lX in [%s] at %s (addr %p, ecx=%08lX)", code, t_what[d] ? t_what[d] : "?",
                w, (void*)ep->ExceptionRecord->ExceptionInformation[1], ep->ContextRecord->Ecx);
            longjmp(t_jb[d], 1);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
    LONG WINAPI Unhandled(EXCEPTION_POINTERS* ep) {
        char w[300]; Where(ep->ExceptionRecord->ExceptionAddress, w, sizeof w);
        LOG("CRASH: exception %08lX at %s (data %p) eax=%08lX ecx=%08lX esp=%08lX",
            ep->ExceptionRecord->ExceptionCode, w, (void*)ep->ExceptionRecord->ExceptionInformation[1],
            ep->ContextRecord->Eax, ep->ContextRecord->Ecx, ep->ContextRecord->Esp);
        return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
    }
}

namespace guard {
void Install() {
    AddVectoredExceptionHandler(1, Veh);
    g_prevFilter = SetUnhandledExceptionFilter(Unhandled);
}
jmp_buf* Enter(const char* what) {
    int d = t_depth < kMaxDepth ? t_depth : kMaxDepth - 1;   // deeper than 8 shares the last buffer (never happens)
    t_what[d] = what; ++t_depth; return &t_jb[d];
}
void Leave() { if (t_depth > 0) --t_depth; }
}
