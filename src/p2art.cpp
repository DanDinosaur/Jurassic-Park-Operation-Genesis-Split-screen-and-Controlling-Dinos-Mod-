// p2art.cpp - see p2art.h (JPOG73 rev b).
#include <windows.h>
#include <d3d8.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include "p2art.h"
#include "kbm.h"
#include "log.h"

namespace p2art {
namespace {
    struct Sheet { const char* file; const char* name; IDirect3DTexture8* tex; float w, h; };
    Sheet g_sheets[] = {
        { "gui_Main.tml",   "gui1",          nullptr, 0, 0 },   // 0 cursor
        { "gui_InGame.tml", "Menu_Buttons1", nullptr, 0, 0 },   // 1 build / dinosaurs / manager
        { "gui_InGame.tml", "Menu_Buttons7", nullptr, 0, 0 },   // 2 actions
        { "gui_Main.tml",   "Menu_Buttons5", nullptr, 0, 0 },   // 3 options
        { "gui_Main.tml",   "ingame",        nullptr, 0, 0 },   // 4 mail flag
        { "gui_Main.tml",   "Menu_Buttons3", nullptr, 0, 0 },   // 5 rollover glow (DiscMenu_Hilight_1)
    };
    struct Sec { int sheet; float x, y, w, h; };
    // order = kbm::P2B_* (Build, Manager, Dinosaurs, Actions, Options, Mail); main.gui texture sections
    const Sec kBtn[6] = { { 1, 0, 0, 64, 64 }, { 1, 192, 64, 64, 64 }, { 1, 128, 0, 64, 64 },
                          { 2, 0, 128, 64, 64 }, { 3, 128, 192, 64, 64 }, { 4, 54, 8, 25, 21 } };
    const Sec kCursor = { 0, 29, 104, 17, 15 }, kGlow = { 5, 155, 39, 86, 86 };
    int g_state = 0;   // 0 not loaded, 1 ok, -1 failed (never retried)
    void* g_dev = nullptr;

    bool ReadFile(const char* rel, std::vector<uint8_t>& out) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* sl = strrchr(path, '\\'); if (sl) sl[1] = 0;
        strncat(path, "Data\\matlibs\\", MAX_PATH - strlen(path) - 1);
        strncat(path, rel, MAX_PATH - strlen(path) - 1);
        FILE* f = fopen(path, "rb");
        if (!f) { LOG("p2art: can't open %s", path); return false; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n <= 64) { fclose(f); return false; }
        out.resize((size_t)n);
        bool ok = fread(out.data(), 1, (size_t)n, f) == (size_t)n;
        fclose(f);
        return ok;
    }
    uint32_t U32(const std::vector<uint8_t>& d, size_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; }
    bool IsName(const std::vector<uint8_t>& d, size_t o) {   // a 32-byte slot holding a material name
        if (o + 32 > d.size()) return false;
        int n = 0;
        while (n < 32 && d[o + n]) { char c = (char)d[o + n]; if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false; ++n; }
        return n >= 2 && n < 32;
    }
    // Finds material `name` in a TML file, returns the offset of its "DDS " header (0 = not found).
    size_t FindImage(const std::vector<uint8_t>& d, const char* name) {
        const size_t ln = strlen(name);
        size_t a = 0;
        for (size_t o = d.size() - 32; o > 64; --o)                      // the name table is at the end
            if (d[o] == (uint8_t)name[0] && d[o - 1] == 0 && !memcmp(&d[o], name, ln) && d[o + ln] == 0 && IsName(d, o)) { a = o; break; }
        if (!a) return 0;
        size_t s = a; while (s >= 32 && IsName(d, s - 32)) s -= 32;
        size_t e = a; while (IsName(d, e + 32)) e += 32;
        const size_t idx = (a - s) / 32, recs = e + 32;
        if (recs + 12 * (idx + 1) > d.size()) return 0;
        const uint32_t id = U32(d, recs + 12 * idx);
        for (size_t o = 28; o + 128 < s; ++o) {
            if (d[o] != 'D' || memcmp(&d[o], "DDS ", 4) || U32(d, o + 4) != 124) continue;
            if (U32(d, o - 28) == id) return o;
        }
        return 0;
    }
    bool LoadSheet(IDirect3DDevice8* dev, Sheet& sh, std::vector<uint8_t>& d) {
        const size_t o = FindImage(d, sh.name);
        if (!o) { LOG("p2art: %s not found in %s", sh.name, sh.file); return false; }
        const uint32_t h = U32(d, o + 12), w = U32(d, o + 16), fourcc = U32(d, o + 84);
        if (fourcc != MAKEFOURCC('D', 'X', 'T', '5') || w < 4 || h < 4 || w > 2048 || h > 2048) { LOG("p2art: %s is not a DXT5 image (%ux%u)", sh.name, w, h); return false; }
        const size_t rowBytes = (size_t)((w + 3) / 4) * 16, rows = (h + 3) / 4;
        if (o + 128 + rowBytes * rows > d.size()) return false;
        IDirect3DTexture8* t = nullptr;
        if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_DXT5, D3DPOOL_MANAGED, &t)) || !t) { LOG("p2art: CreateTexture DXT5 %ux%u failed", w, h); return false; }
        D3DLOCKED_RECT lr{};
        if (FAILED(t->LockRect(0, &lr, nullptr, 0))) { t->Release(); return false; }
        for (size_t r = 0; r < rows; ++r) memcpy((uint8_t*)lr.pBits + r * lr.Pitch, &d[o + 128 + r * rowBytes], rowBytes);
        t->UnlockRect(0);
        sh.tex = t; sh.w = (float)w; sh.h = (float)h;
        return true;
    }
    bool Load(IDirect3DDevice8* dev) {
        std::vector<uint8_t> main, ingame;
        if (!ReadFile("gui_Main.tml", main) || !ReadFile("gui_InGame.tml", ingame)) return false;
        int ok = 0;
        for (auto& sh : g_sheets) ok += LoadSheet(dev, sh, !strcmp(sh.file, "gui_Main.tml") ? main : ingame) ? 1 : 0;
        LOG("p2art: the game's own toolbar + cursor pictures loaded (%d of %d sheets)", ok, (int)(sizeof g_sheets / sizeof g_sheets[0]));
        return g_sheets[0].tex && g_sheets[1].tex;   // at least the cursor + the main buttons
    }

    struct V { float x, y, z, rhw; DWORD c; float u, v; };
    void Quad(IDirect3DDevice8* dev, const Sec& s, float x0, float y0, float x1, float y1, DWORD col) {
        const Sheet& sh = g_sheets[s.sheet];
        if (!sh.tex) return;
        const float u0 = s.x / sh.w, v0 = s.y / sh.h, u1 = (s.x + s.w) / sh.w, v1 = (s.y + s.h) / sh.h;
        x0 -= 0.5f; y0 -= 0.5f; x1 -= 0.5f; y1 -= 0.5f;   // D3D8 texel centre
        V q[4] = { { x0, y0, 0, 1, col, u0, v0 }, { x1, y0, 0, 1, col, u1, v0 }, { x1, y1, 0, 1, col, u1, v1 }, { x0, y1, 0, 1, col, u0, v1 } };
        dev->SetTexture(0, sh.tex);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, q, sizeof(V));
    }
    const D3DRENDERSTATETYPE kRs[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
        D3DRS_CULLMODE, D3DRS_LIGHTING, D3DRS_ALPHATESTENABLE, D3DRS_FOGENABLE, D3DRS_FILLMODE, D3DRS_STENCILENABLE, D3DRS_COLORWRITEENABLE };
    const DWORD kRsVal[] = { FALSE, FALSE, TRUE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DCULL_NONE, FALSE, FALSE, FALSE, D3DFILL_SOLID, FALSE, 0xF };
    const D3DTEXTURESTAGESTATETYPE kTs[] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
        D3DTSS_MINFILTER, D3DTSS_MAGFILTER, D3DTSS_MIPFILTER, D3DTSS_ADDRESSU, D3DTSS_ADDRESSV, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS };
    const DWORD kTsVal[] = { D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE,
        D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, 0, D3DTTFF_DISABLE };
}

bool Draw(void* d3dDevice8, float hx, float hy, float hw, float hh, int hover, float ux, float uy, bool showButtons) {
    IDirect3DDevice8* dev = static_cast<IDirect3DDevice8*>(d3dDevice8);
    if (!dev || g_state < 0) return false;
    if (g_dev != d3dDevice8) {                      // first frame (or a new device): load the pictures once
        if (g_state == 1) for (auto& sh : g_sheets) if (sh.tex) { sh.tex->Release(); sh.tex = nullptr; }
        g_dev = d3dDevice8;
        g_state = Load(dev) ? 1 : -1;
        if (g_state < 0) { LOG("p2art: the game's pictures could not be loaded - simple drawing instead"); return false; }
    }
    // save every state we change
    const int nRs = sizeof kRs / sizeof kRs[0], nTs = sizeof kTs / sizeof kTs[0];
    DWORD rs[nRs], ts[nTs], ts1c = 0, ts1a = 0, vs = 0, ps = 0;
    for (int i = 0; i < nRs; ++i) dev->GetRenderState(kRs[i], &rs[i]);
    for (int i = 0; i < nTs; ++i) dev->GetTextureStageState(0, kTs[i], &ts[i]);
    dev->GetTextureStageState(1, D3DTSS_COLOROP, &ts1c); dev->GetTextureStageState(1, D3DTSS_ALPHAOP, &ts1a);
    IDirect3DBaseTexture8* tex0 = nullptr; dev->GetTexture(0, &tex0);
    dev->GetVertexShader(&vs); dev->GetPixelShader(&ps);

    for (int i = 0; i < nRs; ++i) dev->SetRenderState(kRs[i], kRsVal[i]);
    for (int i = 0; i < nTs; ++i) dev->SetTextureStageState(0, kTs[i], kTsVal[i]);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetPixelShader(0);
    dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

    if (showButtons) {
        const float pulse = 0.75f + 0.25f * sinf(GetTickCount() * 0.001f * 2.0f * 6.2831853f);   // TGUISineFunction 2 Hz, 0.75 +- 0.25
        for (int i = 0; i < 6; ++i) {
            float x0, y0, x1, y1;
            if (!kbm::P2ButtonRect(i, x0, y0, x1, y1)) continue;
            const float X0 = hx + x0 * hw, Y0 = hy + y0 * hh, X1 = hx + x1 * hw, Y1 = hy + y1 * hh;
            if (i == hover && i < 5 && g_sheets[kGlow.sheet].tex) {   // main.gui tMenuToolbarButton Rollover: DiscMenu_Hilight_1, margins -12 of 64
                const float m = (X1 - X0) * 12.0f / 64.0f;
                Quad(dev, kGlow, X0 - m, Y0 - m, X1 + m, Y1 + m, ((DWORD)(0xCA * pulse) << 24) | 0xFFFFFF);
            }
            Quad(dev, kBtn[i], X0, Y0, X1, Y1, i == hover ? 0xFFEAFFCA : 0xFFFFFFFF);   // Rollover tint 0xFFEAFFCA
        }
    }
    // the game's own cursor (cursor_default 17x15 in 1024x768 GUI units -> scaled with the half's height)
    const float cs = hh / 768.0f * kbm::P2CursorScale();
    const float cx = hx + ux * hw, cy = hy + uy * hh;
    Quad(dev, kCursor, cx, cy, cx + 17.0f * cs, cy + 15.0f * cs, 0xFFFFFFFF);

    // restore
    for (int i = 0; i < nRs; ++i) dev->SetRenderState(kRs[i], rs[i]);
    for (int i = 0; i < nTs; ++i) dev->SetTextureStageState(0, kTs[i], ts[i]);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, ts1c); dev->SetTextureStageState(1, D3DTSS_ALPHAOP, ts1a);
    dev->SetTexture(0, tex0); if (tex0) tex0->Release();
    dev->SetVertexShader(vs); dev->SetPixelShader(ps);
    return true;
}
}
