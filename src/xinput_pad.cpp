#include <windows.h>
#include <xinput.h>
#include "xinput_pad.h"
#include "log.h"
#include "kbm.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace {
    // Apply a radial deadzone to a stick and normalise to -1..1.
    void StickDZ(short ix, short iy, float dz, float& ox, float& oy) {
        float x = ix / 32767.0f, y = iy / 32767.0f;
        float mag = sqrtf(x*x + y*y);
        if (mag < dz) { ox = oy = 0.0f; return; }
        float scaled = (mag - dz) / (1.0f - dz);   // rescale past the deadzone
        if (scaled > 1.0f) scaled = 1.0f;
        ox = (x / mag) * scaled;
        oy = (y / mag) * scaled;
    }
}

namespace {
    // [0] = jpogmod_pad.txt (player 1), [1] = jpogmod_pad2.txt (player 2)
    VirtualPadState g_vps[2];
    FILETIME        g_vpTime[2]{};
    bool            g_vpHadFile[2]{};
}

namespace virtualpad {
const VirtualPadState& Get(int i) { return g_vps[i & 1]; }

static void RefreshOne(int idx) {
    VirtualPadState& g_vp = g_vps[idx];
    FILETIME& g_vpTimeR = g_vpTime[idx];
    bool& g_vpHad = g_vpHadFile[idx];
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* sl = wcsrchr(path, L'\\'); if (sl) *(sl + 1) = 0;
    wcscat_s(path, idx ? L"jpogmod_pad2.txt" : L"jpogmod_pad.txt");

    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        if (g_vpHad) { g_vp = VirtualPadState{}; g_vpHad = false; }
        return;
    }
    if (g_vpHad && CompareFileTime(&fa.ftLastWriteTime, &g_vpTimeR) == 0) return;
    g_vpTimeR = fa.ftLastWriteTime; g_vpHad = true;

    char buf[1024]{};
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = 0; ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr); CloseHandle(h);
    buf[n] = 0;

    VirtualPadState v; v.active = false;
    static const char* names[PAD_COUNT] = {"A","B","X","Y","LB","RB","BACK","START","LS","RS","UP","DOWN","LEFT","RIGHT"};
    char* ctx = nullptr;
    for (char* t = strtok_s(buf, " \t\r\n,", &ctx); t; t = strtok_s(nullptr, " \t\r\n,", &ctx)) {
        v.active = true;
        char* eq = strchr(t, '=');
        if (eq) {
            *eq = 0; float f = (float)atof(eq + 1);
            if      (!_stricmp(t,"LX")) v.lx=f; else if (!_stricmp(t,"LY")) v.ly=f;
            else if (!_stricmp(t,"RX")) v.rx=f; else if (!_stricmp(t,"RY")) v.ry=f;
            else if (!_stricmp(t,"LT")) v.lt=f; else if (!_stricmp(t,"RT")) v.rt=f;
            else if (!_stricmp(t,"FOR")) v.untilTick = GetTickCount() + (unsigned)atoi(eq + 1);
            continue;
        }
        for (int i = 0; i < PAD_COUNT; ++i) if (!_stricmp(t, names[i])) v.down[i] = true;
    }
    g_vp = v;
}

void Refresh() { RefreshOne(0); RefreshOne(1); }
} // namespace virtualpad

namespace padslots {
    namespace {
        struct CS { CRITICAL_SECTION c; CS() { InitializeCriticalSection(&c); } } g_cs;   // ready before any thread runs
        volatile LONG g_slot[2] = {-1, -1};
        volatile LONG g_since[2] = {0, 0};
        bool  g_conn[4]{};
        DWORD g_lastEmptyScan = 0;
        void Lock()   { EnterCriticalSection(&g_cs.c); }
        void Unlock() { LeaveCriticalSection(&g_cs.c); }
        void LogAssign(const char* why) {
            LOG("pads: P1=slot %d  P2=slot %d  (%s)", (int)g_slot[0], (int)g_slot[1], why);
        }
    }
    void Update() {
        Lock();
        DWORD now = GetTickCount();
        bool scanEmpty = now - g_lastEmptyScan >= 500;
        if (scanEmpty) g_lastEmptyScan = now;
        WORD btn[4]{};
        for (int i = 0; i < 4; ++i) {
            bool assigned = (g_slot[0] == i || g_slot[1] == i);
            if (!assigned && !scanEmpty && !g_conn[i]) continue;   // XInputGetState on an empty slot is slow
            XINPUT_STATE st{};
            g_conn[i] = XInputGetState(i, &st) == ERROR_SUCCESS;
            btn[i] = g_conn[i] ? st.Gamepad.wButtons : 0;
        }
        bool changed = false; const char* why = "";
        // JPOG70: a player who uses keyboard + mouse has no pad (their pad is free for the other player).
        const bool padMode[2] = { !kbm::IsKbm(0), !kbm::IsKbm(1) };
        for (int p = 0; p < 2; ++p)
            if (!padMode[p] && g_slot[p] >= 0) { g_slot[p] = -1; g_since[p] = 0; changed = true; why = p ? "P2 uses keyboard + mouse" : "P1 uses keyboard + mouse"; }
        for (int p = 0; p < 2; ++p)
            if (g_slot[p] >= 0 && !g_conn[g_slot[p]]) { g_slot[p] = -1; g_since[p] = 0; changed = true; why = p ? "P2 pad gone" : "P1 pad gone"; }
        // P1's pad left and P2's pad JUST arrived (< 5 s): that is a pad swap (ROG Ally built-in ->
        // Bluetooth), so the new pad becomes P1. In real co-op (P2 here for longer) P2 keeps their seat
        // and the next pad to connect fills P1 again.
        if (padMode[0] && g_slot[0] < 0 && g_slot[1] >= 0 && now - (DWORD)g_since[1] < 5000) {
            g_slot[0] = g_slot[1]; g_since[0] = now; g_slot[1] = -1; g_since[1] = 0; changed = true; why = "P2 pad promoted to P1";
        }
        // JPOG70: hold Back + Start 1 s = switch controls. On a player's pad -> that player uses keyboard + mouse;
        // on a free pad -> the keyboard player (player 1 first) takes this pad.
        static DWORD s_comboT[4]{}; static bool s_comboFired[4]{};
        const bool combo = kbm::PadCombo();
        for (int i = 0; i < 4 && combo; ++i) {
            const bool both = g_conn[i] && (btn[i] & XINPUT_GAMEPAD_BACK) && (btn[i] & XINPUT_GAMEPAD_START);
            if (!both) { s_comboT[i] = 0; s_comboFired[i] = false; continue; }
            if (!s_comboT[i]) { s_comboT[i] = now ? now : 1; continue; }
            if (s_comboFired[i] || now - s_comboT[i] < 1000) continue;
            s_comboFired[i] = true;
            const int owner = g_slot[0] == i ? 0 : g_slot[1] == i ? 1 : -1;
            if (owner >= 0) { kbm::ToggleFromPad(owner); LOG("pads: slot %d held Back + Start - player %d switches to keyboard + mouse", i, owner + 1); }
            else {
                const int p = kbm::IsKbm(0) ? 0 : kbm::IsKbm(1) ? 1 : -1;
                if (p >= 0) {
                    kbm::SetKbm(p, false, "free controller: hold Back + Start");
                    if (g_slot[p] < 0) { g_slot[p] = i; g_since[p] = now ? now : 1; changed = true; why = p ? "P2 took this pad (Back + Start)" : "P1 took this pad (Back + Start)"; }
                }
            }
        }
        const bool padNow[2] = { !kbm::IsKbm(0), !kbm::IsKbm(1) };
        for (int p = 0; p < 2; ++p) {
            if (g_slot[p] >= 0 || !padNow[p]) continue;
            for (int i = 0; i < 4; ++i)
                if (g_conn[i] && g_slot[0] != i && g_slot[1] != i && !s_comboT[i]) {
                    g_slot[p] = i; g_since[p] = now ? now : 1; changed = true; why = p ? "pad joined as P2" : "pad joined as P1"; break;
                }
        }
        if (changed) LogAssign(why);
        Unlock();
    }
    int SlotOf(int player) { return g_slot[player & 1]; }
    int ConnectedCount() { int n = 0; for (bool c : g_conn) n += c ? 1 : 0; return n; }
    unsigned Since(int player) { return (unsigned)g_since[player & 1]; }
}

bool XPad::Poll(float deadzone, float triggerThreshold) {
    for (int i = 0; i < PAD_COUNT; ++i) m_prev[i] = m_down[i];

    VirtualPadState& g_vp = g_vps[m_index & 1];
    if (m_index < 2 && g_vp.active) {
        m_connected = true;
        if (g_vp.untilTick && (int)(GetTickCount() - g_vp.untilTick) >= 0) {
            VirtualPadState neutral; neutral.active = true; g_vp = neutral;   // pulse over
        }
        for (int i = 0; i < PAD_COUNT; ++i) m_down[i] = g_vp.down[i];
        m_lx = g_vp.lx; m_ly = g_vp.ly; m_rx = g_vp.rx; m_ry = g_vp.ry;
        m_lt = g_vp.lt; m_rt = g_vp.rt;
        return true;
    }

    padslots::Update();
    // JPOG70: player in keyboard + mouse mode: player 2 = their own keyboard + mouse as a controller, player 1 = no pad
    // (player 1's keyboard + mouse go to the game itself, as in the vanilla game).
    if (m_index < 2 && kbm::IsKbm(m_index)) {
        bool d[PAD_COUNT]{}; float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
        m_kbm = true; m_backCombo = false;
        if (m_index == 1 && kbm::FillPad2(d, lx, ly, rx, ry, lt, rt)) {
            m_connected = true;
            for (int i = 0; i < PAD_COUNT; ++i) m_down[i] = d[i];
            m_lx = lx; m_ly = ly; m_rx = rx; m_ry = ry; m_lt = lt; m_rt = rt;
            return true;
        }
        m_connected = false;
        for (int i = 0; i < PAD_COUNT; ++i) m_down[i] = false;
        m_lx=m_ly=m_rx=m_ry=m_lt=m_rt=0;
        return false;
    }
    m_kbm = false;
    const int slot = padslots::SlotOf(m_index);
    XINPUT_STATE st{};
    if (slot < 0 || XInputGetState(slot, &st) != ERROR_SUCCESS) {
        m_connected = false;
        for (int i = 0; i < PAD_COUNT; ++i) m_down[i] = false;
        m_lx=m_ly=m_rx=m_ry=m_lt=m_rt=0;
        return false;
    }
    m_connected = true;
    const auto& g = st.Gamepad;
    auto bit = [&](WORD m){ return (g.wButtons & m) != 0; };

    m_down[PAD_A]     = bit(XINPUT_GAMEPAD_A);
    m_down[PAD_B]     = bit(XINPUT_GAMEPAD_B);
    m_down[PAD_X]     = bit(XINPUT_GAMEPAD_X);
    m_down[PAD_Y]     = bit(XINPUT_GAMEPAD_Y);
    m_down[PAD_LB]    = bit(XINPUT_GAMEPAD_LEFT_SHOULDER);
    m_down[PAD_RB]    = bit(XINPUT_GAMEPAD_RIGHT_SHOULDER);
    m_down[PAD_BACK]  = bit(XINPUT_GAMEPAD_BACK);
    m_down[PAD_START] = bit(XINPUT_GAMEPAD_START);
    m_down[PAD_LS]    = bit(XINPUT_GAMEPAD_LEFT_THUMB);
    m_down[PAD_RS]    = bit(XINPUT_GAMEPAD_RIGHT_THUMB);
    m_down[PAD_DUP]   = bit(XINPUT_GAMEPAD_DPAD_UP);
    m_down[PAD_DDOWN] = bit(XINPUT_GAMEPAD_DPAD_DOWN);
    m_down[PAD_DLEFT] = bit(XINPUT_GAMEPAD_DPAD_LEFT);
    m_down[PAD_DRIGHT]= bit(XINPUT_GAMEPAD_DPAD_RIGHT);

    StickDZ(g.sThumbLX, g.sThumbLY, deadzone, m_lx, m_ly);
    StickDZ(g.sThumbRX, g.sThumbRY, deadzone, m_rx, m_ry);

    m_lt = g.bLeftTrigger  / 255.0f;  if (m_lt < triggerThreshold) m_lt = 0;
    m_rt = g.bRightTrigger / 255.0f;  if (m_rt < triggerThreshold) m_rt = 0;
    // JPOG70: Back + Start = the switch-controls combo -> Start does nothing while Back is held (until Start is let
    // go), and Back's tap (Research) is skipped for that press.
    if (kbm::PadCombo()) {
        const bool back = m_down[PAD_BACK], start = m_down[PAD_START];
        if (back && !m_physBackPrev) m_backCombo = false;
        if (back && start) { m_backCombo = true; m_startMasked = true; }
        if (!start) m_startMasked = false;
        if (m_startMasked || (back && start)) m_down[PAD_START] = false;
        m_physBackPrev = back;
    }
    return true;
}

void XPad::SetRumble(float low, float high) {
    if (!m_connected) return;
    XINPUT_VIBRATION v{};
    auto clamp = [](float f){ return f<0?0.f:(f>1?1.f:f); };
    v.wLeftMotorSpeed  = (WORD)(clamp(low)  * 65535.0f);
    v.wRightMotorSpeed = (WORD)(clamp(high) * 65535.0f);
    { int slot = padslots::SlotOf(m_index); if (slot >= 0) XInputSetState(slot, &v); }
}
