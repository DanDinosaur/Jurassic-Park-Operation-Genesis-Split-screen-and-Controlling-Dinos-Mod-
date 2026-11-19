// treelean.cpp - see treelean.h
#include <windows.h>
#include <cstdint>
#include <cmath>
#include "treelean.h"
#include "config.h"
#include "game.h"
#include "guard.h"
#include "log.h"

namespace treelean {
namespace {
    constexpr uintptr_t kCacheMask16 = 0x008B2B30;   // ushort mask for the cache keys
    constexpr uintptr_t kKnockSound  = 0x005E08F0;   // ATreeManager::PlayTreeKnockSound(ATreeNode&) (thiscall, ret 4)
    struct Lean {
        uint8_t* node; void* tm; int idx; unsigned cx, cz;
        float q0[4]; float axis[3];
        float ang, vel; DWORD lastContact;
    };
    Lean g_l[160]; int g_n = 0;
    unsigned g_logs = 0, g_drops = 0, g_done = 0;

    bool Region(uint8_t* tm, unsigned x, unsigned z, int* idx, unsigned* cx, unsigned* cz) {
        const unsigned mask = *reinterpret_cast<uint32_t*>(tm + 0xC4);
        const unsigned width = *reinterpret_cast<uint16_t*>(tm + 0x160);
        const uint16_t* cache = *reinterpret_cast<uint16_t**>(tm + 0xC8);
        const uint16_t m = *reinterpret_cast<uint16_t*>(game::At(kCacheMask16));
        *cx = (x >> 3) >> 1; *cz = (z >> 3) >> 1;
        *idx = (int)((*cx & mask) + (*cz & mask) * width);
        return cache && (unsigned)(cache[*idx * 2] & m) == *cx && (unsigned)(cache[*idx * 2 + 1] & m) == *cz;
    }
    uint8_t* Head(uint8_t* tm, int idx) { return *reinterpret_cast<uint8_t**>(tm + 0xA4) + idx * 8; }
    bool InList(uint8_t* tm, int idx, uint8_t* node) {
        uint8_t* head = Head(tm, idx);
        int guardN = 0;
        for (uint8_t* n = *reinterpret_cast<uint8_t**>(head); n && n != head && guardN < 4096; n = *reinterpret_cast<uint8_t**>(n), ++guardN)
            if (n == node) return true;
        return false;
    }
    // q * r (Hamilton, x y z w)
    void Mul(const float* a, const float* b, float* o) {
        o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
        o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
        o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
        o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    }
    // the tree's own lean axis (local space): world axis = cross(up, push dir) (tips the top along the push), into the
    // tree's frame (R^T), with its trunk component removed (trunk = the local axis R maps closest to world up) - a pure
    // lean whichever quaternion convention Toshi uses (no twist round the trunk).
    bool LocalAxis(const float* q, float dx, float dz, float* out) {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        const float R[3][3] = {
            { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
            { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
            { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } };
        const float aw[3] = { dz, 0.0f, -dx };
        float al[3];
        for (int i = 0; i < 3; ++i) al[i] = R[0][i] * aw[0] + R[1][i] * aw[1] + R[2][i] * aw[2];
        int up = 0;
        for (int i = 1; i < 3; ++i) if (fabsf(R[1][i]) > fabsf(R[1][up])) up = i;
        al[up] = 0.0f;
        const float l = sqrtf(al[0] * al[0] + al[1] * al[1] + al[2] * al[2]);
        if (l < 1e-3f) {   // push along the trunk axis?? take any axis across the trunk
            al[0] = al[1] = al[2] = 0.0f; al[(up + 1) % 3] = 1.0f;
        } else { al[0] /= l; al[1] /= l; al[2] /= l; }
        out[0] = al[0]; out[1] = al[1]; out[2] = al[2];
        return true;
    }
    void Write(Lean& L, float ang) {
        const float h = ang * 0.5f, s = sinf(h);
        const float r[4] = { L.axis[0] * s, L.axis[1] * s, L.axis[2] * s, cosf(h) };
        float q[4];
        Mul(L.q0, r, q);
        memcpy(L.node + 0x14, q, 16);
    }
    void Drop(int i) { g_l[i] = g_l[--g_n]; }
}

bool Knock(void* treeMgr, unsigned x, unsigned z, const float* pos, const float* facing) {
    const Config& c = config::Get();
    if (!c.treeLean || !treeMgr || !pos) return false;
    uint8_t* tm = static_cast<uint8_t*>(treeMgr);
    // DOUBLE CHECK 08:55: tree_lean_all sends every dino's ~2000 calls/s per dino in trees - the same cell key and
    // position (0.5 m) handled < 60 ms ago is skipped (still "handled": lastContact is refreshed well inside 250 ms).
    {
        struct Seen { uint32_t key; int px, pz; DWORD t; };
        static Seen s_ring[64]; static unsigned s_next = 0; static unsigned s_skips = 0;
        const uint32_t key = (x >> 3) | ((z >> 3) << 16);
        const int px = (int)(pos[0] * 2.0f), pz = (int)(pos[2] * 2.0f);
        const DWORD t = GetTickCount();
        for (const Seen& s : s_ring)
            if (s.t && s.key == key && s.px == px && s.pz == pz && t - s.t < 60) { ++s_skips; return true; }
        s_ring[s_next++ & 63] = { key, px, pz, t ? t : 1 };
        static DWORD s_logT = 0;
        if (t - s_logT > 30000 && s_skips) { s_logT = t; LOG("treelean: %u repeat knocks skipped (same cell + spot < 60 ms)", s_skips); }
    }
    GUARDED("tree lean knock", { do {
        int idx; unsigned cx, cz;
        // OnUpdate skips an entry when FUN_005e1e10 (cell x & ushort 0x840C04 == 0) says 2 - same here
        if ((((x >> 3) & 0xFFFF) & *reinterpret_cast<uint16_t*>(game::At(0x00840C04))) == 0) break;
        if (!Region(tm, x, z, &idx, &cx, &cz)) break;
        const float* radii = *reinterpret_cast<float**>(tm + 0x98);
        uint8_t* head = Head(tm, idx);
        const DWORD now = GetTickCount();
        int guardN = 0;
        for (uint8_t* n = *reinterpret_cast<uint8_t**>(head); n && n != head && guardN < 4096; n = *reinterpret_cast<uint8_t**>(n), ++guardN) {
            const float* np = reinterpret_cast<float*>(n + 8);
            float tr = radii ? radii[n[0x29] & 0x7F] : 1.0f;
            if (!(tr >= 0.0f && tr < 15.0f)) tr = 1.0f;
            const float dx = np[0] - pos[0], dz = np[2] - pos[2], d2 = dx * dx + dz * dz, reach = tr + 3.0f;
            if (d2 >= reach * reach) continue;
            int found = -1;
            for (int i = 0; i < g_n; ++i) if (g_l[i].node == n) { found = i; break; }
            if (found >= 0) { g_l[found].lastContact = now; continue; }
            if (g_n >= 160) continue;
            Lean& L = g_l[g_n];
            L.node = n; L.tm = tm; L.idx = idx; L.cx = cx; L.cz = cz;
            memcpy(L.q0, n + 0x14, 16);
            float px = dx, pz = dz;
            const float d = sqrtf(d2);
            if (d < 0.05f && facing) { px = facing[0]; pz = facing[2]; }
            const float pl = sqrtf(px * px + pz * pz);
            if (pl < 1e-4f) { px = 0.0f; pz = 1.0f; } else { px /= pl; pz /= pl; }
            LocalAxis(L.q0, px, pz, L.axis);
            L.ang = 0.0f; L.vel = 0.0f; L.lastContact = now;
            ++g_n;
            if (c.treeLeanSound) GUARDED("tree knock sound", { reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(game::At(kKnockSound))(tm, nullptr, n); });
            if (g_logs < 6) {
                ++g_logs;
                LOG("treelean: tree %p (type %d, radius %.1f) at %.1f m leans away (q0 %.2f %.2f %.2f %.2f, local axis %.2f %.2f %.2f) - %d bending",
                    n, n[0x29] & 0x7F, tr, d, L.q0[0], L.q0[1], L.q0[2], L.q0[3], L.axis[0], L.axis[1], L.axis[2], g_n);
            }
        }
    } while (0); });
    return true;
}

void Tick(float dt) {
    if (!g_n) return;
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;
    const Config& c = config::Get();
    const float target = c.treeLeanDeg * 3.14159265f / 180.0f;
    const DWORD now = GetTickCount();
    for (int i = g_n - 1; i >= 0; --i) {
        Lean& L = g_l[i];
        bool ok = false, done = false;
        GUARDED("tree lean tick", { do {
            int idx; unsigned cx, cz;
            // the region must still hold the same cache key, and the node must still be in its list
            if (!Region(static_cast<uint8_t*>(L.tm), L.cx << 4, L.cz << 4, &idx, &cx, &cz) || idx != L.idx ||
                !InList(static_cast<uint8_t*>(L.tm), L.idx, L.node)) break;
            const bool touching = now - L.lastContact < 250;
            const float goal = touching ? target : 0.0f;
            // spring: quick to go down (stiff), softer coming back up with a little sway past upright
            const float k = touching ? 60.0f : 22.0f, damp = touching ? 2.0f * sqrtf(k) : 2.0f * sqrtf(k) * 0.45f;
            for (float rem = dt; rem > 0.0f; rem -= 1.0f / 120.0f) {
                const float h = rem < 1.0f / 120.0f ? rem : 1.0f / 120.0f;
                L.vel += (k * (goal - L.ang) - damp * L.vel) * h;
                L.ang += L.vel * h;
            }
            if (!touching && fabsf(L.ang) < 0.003f && fabsf(L.vel) < 0.02f) { memcpy(L.node + 0x14, L.q0, 16); done = true; }
            else Write(L, L.ang);
            ok = true;
        } while (0); });
        if (done) { ++g_done; Drop(i); continue; }
        if (!ok) { if (g_drops++ < 4) LOG("treelean: tree %p left the tree cache while bent - dropped (%u)", L.node, g_drops); Drop(i); }
    }
    static DWORD s_t = 0;
    if (now - s_t > 30000 && g_done) { s_t = now; LOG("treelean: %u trees bent and back upright so far, %d bending now", g_done, g_n); }
}

void Clear() { g_n = 0; }
}
