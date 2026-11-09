#include <windows.h>
#include <psapi.h>
#include "guard.h"
#include "log.h"

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

    LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
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
