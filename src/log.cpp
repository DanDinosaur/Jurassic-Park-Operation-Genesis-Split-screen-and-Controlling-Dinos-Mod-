#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <share.h>
#include <mutex>

namespace {
    bool        g_on = false;
    FILE*       g_fp = nullptr;
    std::mutex  g_mx;

    void OpenLog() {
        // Write next to the running exe (the game folder).
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) *(slash + 1) = 0;
        wcscat_s(path, L"jpogmod.log");
        // Shared-read so the log can be copied/staged while the game runs.
        g_fp = _wfsopen(path, L"w", _SH_DENYWR);
    }

    // Caller must hold g_mx.
    void WriteLocked(const char* fmt, va_list ap) {
        if (!g_on || !g_fp) return;
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(g_fp, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        vfprintf(g_fp, fmt, ap);
        fputc('\n', g_fp);
        fflush(g_fp);
    }
    void WriteLockedF(const char* fmt, ...) {
        va_list ap; va_start(ap, fmt); WriteLocked(fmt, ap); va_end(ap);
    }
}

namespace modlog {

// NOTE: std::mutex is not recursive. Never call Line() while holding g_mx
// (JPOG1 deadlocked here: Init held the lock and called Line).

void Init(bool enabled) {
    g_on = enabled;
    if (!g_on) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_fp) OpenLog();
    WriteLockedF("JPOG player-mode mod - log start");
}

void Line(const char* fmt, ...) {
    if (!g_on || !g_fp) return;
    std::lock_guard<std::mutex> lk(g_mx);
    va_list ap; va_start(ap, fmt);
    WriteLocked(fmt, ap);
    va_end(ap);
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_fp) { WriteLockedF("log end"); fclose(g_fp); g_fp = nullptr; }
}

} // namespace modlog
