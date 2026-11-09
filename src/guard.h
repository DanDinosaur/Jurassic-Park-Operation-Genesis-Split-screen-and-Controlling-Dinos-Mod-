// guard.h - run a risky game call; if it faults, log where and return false instead of crashing.
// (i686 mingw-clang has no __try; a vectored handler longjmps back to the guard.)
#pragma once
#include <setjmp.h>
namespace guard {
    void Install();                       // once at startup (also logs unhandled crashes)
    jmp_buf* Enter(const char* what);     // use via GUARDED() below
    void Leave();
}
// GUARDED("label", { code; })  -> evaluates to true if the code ran without a fault.
#define GUARDED(label, ...) ([&]() -> bool { jmp_buf* jb_ = guard::Enter(label); \
    if (setjmp(*jb_)) { guard::Leave(); return false; } { __VA_ARGS__ } guard::Leave(); return true; }())
