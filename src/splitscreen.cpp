// splitscreen.cpp - local 2-player split-screen, layer 1 (two live views).
//
// How the engine draws a frame (ARenderer::OnUpdate 0x477370, ARenderer::OnCreate 0x477BD0):
//   renderer+0x2C = "MainViewport" (TViewport*) of MainScene  (= TRenderInterface system resource 0x11)
//   renderer+0x30 = "SkyScene"   (TScene*),  renderer+0x34 = "SkyViewport"
//   scene+0x568   = the scene's TCameraObject*
//   Per frame: SkyScene Begin/Update/Render/End, then MainScene (camera matrix set from ACamera first).
//   For water reflections it already shrinks MainViewport to 256x256, swaps the camera matrix,
//   renders, and restores - byte 0x84B080 = 1 during that pass.
// We do the same for split-screen: SimJP's import of TScene::Render is redirected (IAT) to
// Hook_Render. For MainScene/SkyScene it draws the scene twice - P1 rect with the game's camera,
// then P2 rect with player 2's camera - and leaves the viewport on P1's rect so the game's own
// mouse picking matches what P1 sees. Hor+ (JPOG-Modernize) reads the viewport size, so both
// orientations get a correct FOV automatically.
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include "splitscreen.h"
#include "ridehud.h"
#include "builder2.h"
#include "p2menu.h"
#include "cullfix.h"
#include "xinput_pad.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include "input_map.h"
#include "guard.h"
#include "dinocontrol.h"
#include "minipad.h"
#include "gamectl.h"
#include "detour.h"

namespace {
    // ---- TRenderInterface.dll exports (thiscall -> __fastcall with dummy EDX) ----
    using fn_bool0   = bool (__fastcall*)(void* self, void* edx);
    using fn_getf    = float(__fastcall*)(void* self, void* edx);
    using fn_setf    = float(__fastcall*)(void* self, void* edx, float);
    using fn_ptr0    = void*(__fastcall*)(void* self, void* edx);
    using fn_ptrI    = void*(__fastcall*)(void* self, void* edx, int);
    using fn_setmat  = void (__fastcall*)(void* self, void* edx, const float* m);

    fn_bool0 pRender = nullptr, pBegin = nullptr, pEnd = nullptr;
    fn_setf  pSetX = nullptr, pSetY = nullptr, pSetW = nullptr, pSetH = nullptr;
    fn_getf  pGetW = nullptr, pGetH = nullptr;
    fn_ptr0  pCamGetTransform = nullptr;
    fn_ptr0  pGetMatrix = nullptr;          // returns const TMatrix44&
    fn_setmat pSetMatrix = nullptr;
    fn_ptrI  pGetSystemResource = nullptr;
    void**   pInterfaceVar = nullptr;        // &TRenderInterface::s_Interface
    void**   g_iatSlot = nullptr;

    // ---- state (game thread only) ----
    void* g_renderer = nullptr;
    void* g_mainScene = nullptr;
    bool  g_active = false;
    bool  g_forced = false;      // F11 / ini manual on (testing without a 2nd pad)
    bool  g_vertical = false;
    float g_W = 0, g_H = 0;      // full-screen viewport size
    volatile LONG g_activePlayer = 0;      // who owns the game's cursor/camera/picking right now
    // JPOG31: the ACTIVE player rides a dino too (both players control dinos): their half is drawn with this
    // chase matrix instead of the game camera (set per frame on the game thread in Tick).
    float g_actChase[16]; volatile bool g_actChaseOn = false; int g_actChaseLogs = 0;
    float g_p2[16];              // player 2 camera matrix (rows: right, up, fwd, pos - verified by log)
    XPad  g_pad2(1);
    unsigned g_frame = 0, g_parkFrame = 0;
    void* g_park = nullptr;
    bool  g_prevF11 = false, g_prevF12 = false;
    unsigned g_renderLogs = 0;
    volatile LONG g_menuOpen = 0;   // a full-screen menu is open in the active player's half
    void* GuiScreen();

    // ---- Options popup toggle (Data\GUI\main.gui patched by the installer: widget "JPOGModSplit"
    //      in AWindowsOptionsDialog; vtable 0xD1F760 slot 6 = OnActivate, 7 = OnDeactivate) ----
    using fn_find   = void*(__fastcall*)(void* self, void* edx, const char* name);
    using fn_setTog = void (__fastcall*)(void* self, void* edx, int on);
    using fn_isTog  = bool (__fastcall*)(void* self, void* edx);
    fn_find   pFindNamedChild = nullptr;
    fn_setTog pSetToggled = nullptr;
    fn_isTog  pIsToggled = nullptr;
    fn_bool0  g_origOptActivate = nullptr, g_origOptDeactivate = nullptr;
    void*     g_splitWidget = nullptr;
    void SetOrientation(bool vertical, const char* why);

    void PollOptionsToggle() {
        if (!g_splitWidget || !pIsToggled) return;
        bool v = pIsToggled(g_splitWidget, nullptr);
        if (v != g_vertical) SetOrientation(v, "options menu");
    }
    bool __fastcall Opt_OnActivate(void* self, void* edx) {
        LOG("split: options OnActivate %p -> original", self);
        bool r = g_origOptActivate(self, edx);
        LOG("split: options original done (%d) -> find", r);
        // The game looks its buttons up on the dialog's PGUIDialog at +0x2C (see 0x57E411:
        // mov ecx,[ebp+0x2C] before FindNamedChild), not on the AGameDialog itself.
        void* gui = *reinterpret_cast<void**>((uint8_t*)self + 0x2C);
        void* w = nullptr;
        LOG("split: options dialog gui=%p vt=%p", gui, gui ? *(void**)gui : nullptr);
        bool ok = GUARDED("FindNamedChild JPOGModSplit", { if (gui) w = pFindNamedChild(gui, nullptr, "JPOGModSplit"); });
        LOG("split: toggle widget %p (ok=%d)", w, ok);
        if (w) ok = GUARDED("SetToggled", { pSetToggled(w, nullptr, g_vertical ? 1 : 0); });
        if (w && ok) g_splitWidget = w;
        LOG("split: options menu opened (toggle set %d)", g_vertical);
        return r;
    }
    bool __fastcall Opt_OnDeactivate(void* self, void* edx) {
        PollOptionsToggle();
        g_splitWidget = nullptr;
        return g_origOptDeactivate(self, edx);
    }
    bool PatchVtable(uintptr_t vt, int slot, void* fn, void** orig) {
        void** e = reinterpret_cast<void**>(game::At(vt)) + slot;
        DWORD old;
        if (!VirtualProtect(e, sizeof(void*), PAGE_READWRITE, &old)) return false;
        *orig = *e; *e = fn;
        VirtualProtect(e, sizeof(void*), old, &old);
        return true;
    }

    template <class T> T Exp(HMODULE m, const char* n) {
        T f = reinterpret_cast<T>(GetProcAddress(m, n));
        if (!f) LOG("split: missing export %s", n);
        return f;
    }

    void* Ptr(void* b, int off) { return *reinterpret_cast<void**>((uint8_t*)b + off); }

    void* MainCamTransform(void* scene) {
        void* cam = Ptr(scene, 0x568);
        return cam ? pCamGetTransform(cam, nullptr) : nullptr;
    }

    void SetRect(void* vp, float x, float y, float w, float h) {
        pSetX(vp, nullptr, x); pSetY(vp, nullptr, y);
        pSetW(vp, nullptr, w); pSetH(vp, nullptr, h);
    }
    void RectFor(int player, float& x, float& y, float& w, float& h) {
        if (g_vertical) { w = floorf(g_W * 0.5f); h = g_H; x = player ? w : 0; y = 0; }
        else            { w = g_W; h = floorf(g_H * 0.5f); x = 0; y = player ? h : 0; }
    }
    void ApplyP1Rects() {
        if (!g_renderer) return;
        float x, y, w, h; RectFor((int)g_activePlayer, x, y, w, h);
        if (void* v = Ptr(g_renderer, 0x2C)) SetRect(v, x, y, w, h);
        if (void* v = Ptr(g_renderer, 0x34)) SetRect(v, x, y, w, h);
    }
    void RestoreFull() {
        if (!g_renderer || g_W <= 0) return;
        if (void* v = Ptr(g_renderer, 0x2C)) SetRect(v, 0, 0, g_W, g_H);
        if (void* v = Ptr(g_renderer, 0x34)) SetRect(v, 0, 0, g_W, g_H);
    }

    // ---- small vector helpers on matrix rows ----
    float* Row(float* m, int r) { return m + r * 4; }
    float Dot(const float* a, const float* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
    void Norm(float* v) { float l = sqrtf(Dot(v, v)); if (l > 1e-6f) { v[0]/=l; v[1]/=l; v[2]/=l; } }
    // Rodrigues: rotate v about unit axis k by angle a.
    void Rot(float* v, const float* k, float a) {
        float c = cosf(a), s = sinf(a), d = Dot(k, v);
        float x = k[1]*v[2] - k[2]*v[1], y = k[2]*v[0] - k[0]*v[2], z = k[0]*v[1] - k[1]*v[0];
        float r[3] = { v[0]*c + x*s + k[0]*d*(1-c), v[1]*c + y*s + k[1]*d*(1-c), v[2]*c + z*s + k[2]*d*(1-c) };
        v[0]=r[0]; v[1]=r[1]; v[2]=r[2];
    }
    void Orthonormalize(float* m) {
        float* r = Row(m,0); float* u = Row(m,1); float* f = Row(m,2);
        Norm(f);
        float d = Dot(u, f); for (int i=0;i<3;++i) u[i] -= d*f[i]; Norm(u);
        float d1 = Dot(r, f), d2 = Dot(r, u); for (int i=0;i<3;++i) r[i] -= d1*f[i] + d2*u[i]; Norm(r);
    }

    void LogMatrix(const char* tag, const float* m) {
        LOG("split: %s r0(%.2f %.2f %.2f %.2f) r1(%.2f %.2f %.2f %.2f) r2(%.2f %.2f %.2f %.2f) r3(%.1f %.1f %.1f %.2f)",
            tag, m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7], m[8],m[9],m[10],m[11], m[12],m[13],m[14],m[15]);
    }

    // ---- Player 2's REAL game camera (layer 2) --------------------------------------------
    // A second AWorldCamera made by the game's own ACameraManager::CreateCamera, seeded from
    // P1's camera (CopyFromPrevious) and driven by pad 2 through the camera's own analog
    // Input* functions, so it moves exactly like the game camera (same speed, damping, terrain
    // clipping, limits). We call its OnUpdate ourselves (only the ACTIVE camera is scheduled)
    // and build its view matrix the way ARenderer::OnUpdate does: quaternion + position.
    namespace cam {
        constexpr uintptr_t GetCameraManager = 0x0050CF80;  // static, returns ACameraManager*
        constexpr uintptr_t CreateCamera     = 0x0050CC40;  // thiscall(mgr, const TClass*) -> ACamera*
        constexpr uintptr_t DeleteCamera     = 0x0050CC90;  // thiscall(mgr, ACamera*)
        constexpr uintptr_t GetActiveCamera  = 0x0050CFD0;  // thiscall(mgr) -> ACamera*
        constexpr uintptr_t WorldCameraClass = 0x008B61C0;  // AWorldCamera::m_sClass
        // AWorldCamera vtable slots (vftable 0xD12C20, dumped from SimJP.exe)
        enum { OnUpdate = 6, CopyFromPrevious = 14, PushFwd = 18, PushBack = 19, PushRight = 20,
               PushLeft = 21, RotLeft = 25, RotRight = 27, RotUp = 29, RotDown = 31,
               ZoomOutF = 35, GetPosition = 52, GetOrientation = 53 };
        constexpr int VelFwd = 0x384, VelSide = 0x388, VelRot = 0x38C, VelPitch = 0x390;
    }
    using fn_mgr0  = void*(__stdcall*)();
    using fn_mgrP  = void*(__fastcall*)(void* self, void* edx, const void*);
    using fn_vf    = void (__fastcall*)(void* self, void* edx, float);
    using fn_vp    = void (__fastcall*)(void* self, void* edx, void*);
    using fn_bf    = bool (__fastcall*)(void* self, void* edx, float);
    using fn_mat0  = void (__fastcall*)(void* self, void* edx);
    using fn_matQ  = void*(__fastcall*)(void* self, void* edx, const void* q);
    using fn_matV  = void (__fastcall*)(void* self, void* edx, const void* v);
    fn_mat0 pMatIdentity = nullptr; fn_matQ pMatFromQuat = nullptr; fn_matV pMatSetTranslate = nullptr;
    void* g_cam2 = nullptr;      // player 2's AWorldCamera (null = fall back to the matrix free-cam)
    void* g_cam1 = nullptr;      // player 1's camera = the game's own camera when split started
    volatile LONG g_requestPlayer = -1;    // set by the controller thread, applied on the game thread
    int   g_cur[2][2]{}; bool g_curInit[2]{};
    XPad  g_pad1(0);             // player 1's pad, read here to drive P1's camera while P2 is active
    unsigned g_switchFrame = 0;
    void* g_camMgr = nullptr;

    template <class T> T VSlot(void* obj, int slot) { return reinterpret_cast<T>((*reinterpret_cast<void***>(obj))[slot]); }
    void* GameActiveCamera() {
        if (!g_camMgr) return nullptr;
        return reinterpret_cast<void*(__fastcall*)(void*, void*)>(game::At(cam::GetActiveCamera))(g_camMgr, nullptr);
    }
    bool CreateP2Camera() {
        if (!pMatIdentity || !pMatFromQuat || !pMatSetTranslate) return false;
        void* mgr = nullptr; void* c2 = nullptr; void* c1 = nullptr;
        bool ok = GUARDED("create P2 AWorldCamera", {
            mgr = reinterpret_cast<fn_mgr0>(game::At(cam::GetCameraManager))();
            if (mgr) {
                g_camMgr = mgr;
                c1 = GameActiveCamera();
                c2 = reinterpret_cast<fn_mgrP>(game::At(cam::CreateCamera))(mgr, nullptr, game::At(cam::WorldCameraClass));
                if (c2 && c1) VSlot<fn_vp>(c2, cam::CopyFromPrevious)(c2, nullptr, c1);
            }
        });
        LOG("split: P2 camera create ok=%d mgr=%p p1cam=%p p2cam=%p", ok, mgr, c1, c2);
        if (!ok || !c2) return false;
        g_cam2 = c2;
        return true;
    }
    void DeleteP2Camera() {
        if (!g_cam2 || !g_camMgr) { g_cam2 = nullptr; return; }
        void* c2 = g_cam2; g_cam2 = nullptr;
        bool ok = GUARDED("delete P2 camera", {
            reinterpret_cast<fn_vp>(game::At(cam::DeleteCamera))(g_camMgr, nullptr, c2); });
        LOG("split: P2 camera deleted (ok=%d)", ok);
    }
    // Dino control replaces the world camera with a follow camera (and back): the game's camera-type
    // switch deletes the old camera through ACameraManager::DeleteCamera (calls at 0x50C9C3 / 0x50CA2E).
    // In split-screen those cameras are the players' own - keep them alive (we delete P2's ourselves).
    using fn_delcam = void(__fastcall*)(void* mgr, void* edx, void* cam);
    volatile LONG g_adoptPending = 0;
    void __fastcall Hook_DeleteCamera(void* mgr, void* edx, void* victim) {
        if (victim && (victim == g_cam2 || (g_active && victim == g_cam1))) {
            LOG("split: kept player %d's camera alive across a camera-type switch", victim == g_cam2 ? 2 : 1);
            g_adoptPending = 1;   // the game now uses a new camera: make it the acting player's (Tick)
            if (*reinterpret_cast<void**>((uint8_t*)mgr + 0x24) == victim) {
                // JPOG34: deactivate its task like ACameraManager::SetActiveCamera would. JPOG33 only cleared the
                // pointer, so every kept camera stayed an ACTIVE task the scheduler kept updating - after quitting
                // the park that crashed in AWorldCamera::ClipFocalPoint (owner log 12:20:46, SimJP+0x128465).
                auto activate = *reinterpret_cast<void(__fastcall**)(void*, void*, bool)>(game::At(0x00739D48));   // TTask::Activate
                auto isActive = *reinterpret_cast<bool(__fastcall**)(void*, void*)>(game::At(0x0073988C));         // TTask::IsActive
                if (isActive(victim, nullptr)) activate(victim, nullptr, false);
                *reinterpret_cast<void**>((uint8_t*)mgr + 0x24) = nullptr;
            }
            return;
        }
        reinterpret_cast<fn_delcam>(game::At(cam::DeleteCamera))(mgr, edx, victim);
    }

    // ---- JPOG34 (owner bug 7): a player's FREE camera must be a world camera ----
    // Selecting a unit makes the game switch the active camera to an AFollowCamera; the acting player adopts it
    // (AdoptActiveCamera). If that player then loses the HUD (dino ride, hand-over) their own camera is a follow
    // camera and UpdateInactiveCamera drove it with AWorldCamera vtable slots -> nothing moved ("stuck").
    // Fix: before driving an inactive camera, check its class; not a world camera -> give that player a new
    // AWorldCamera copied from it (same as CreateP2Camera).
    // AFollowCamera DERIVES from AWorldCamera (TClass parent 0x8B61C0, set up at 0x51425C), so IsA(world) is true
    // for follow cameras too - compare the exact class (TObject vtable slot 0 = GetClass, as SetCameraType does).
    bool IsExactlyWorldCamera(void* c) {
        if (!c) return false;
        void* cls = VSlot<void*(__fastcall*)(void*, void*)>(c, 0)(c, nullptr);
        return cls == game::At(cam::WorldCameraClass);
    }
    // JPOG35: *(0x85006C) = AParkManager::m_pSimJPTerrain; +0x28 = its terrain data, read without a check by
    // AWorldCamera::ClipFocalPoint 0x528450 (mov eax,[ebx+0x28]; mov edx,[eax+0x38] - the 13:58:20 fault, addr 0x38).
    bool WorldReady() {
        void* w = *reinterpret_cast<void**>(game::At(0x0085006C));
        if (reinterpret_cast<uintptr_t>(w) < 0x10000) return false;
        return *reinterpret_cast<void**>((uint8_t*)w + 0x28) != nullptr;
    }
    void* DefaultCamera() { return *reinterpret_cast<void**>(game::At(0x0084D8A0)); }   // ACameraManager::sm_poDefaultCamera
    bool EnsureWorldCamera(int who) {
        void*& slot = who ? g_cam2 : g_cam1;
        void* c = slot;
        if (!c || !g_camMgr) return false;
        if (IsExactlyWorldCamera(c)) return true;
        void* nc = nullptr;
        bool ok = GUARDED("new world camera for a free player", {
            nc = reinterpret_cast<fn_mgrP>(game::At(cam::CreateCamera))(g_camMgr, nullptr, game::At(cam::WorldCameraClass));
            if (nc) VSlot<fn_vp>(nc, cam::CopyFromPrevious)(nc, nullptr, c);
        });
        if (!ok || !nc) { LOG("split: player %d's camera %p is not a world camera - could not make one (ok=%d)", who + 1, c, ok); return false; }
        slot = nc;
        void* other = who ? g_cam1 : g_cam2;
        const bool del = c != GameActiveCamera() && c != DefaultCamera() && c != other;
        if (del) GUARDED("delete the old non-world camera", {
            reinterpret_cast<fn_vp>(game::At(cam::DeleteCamera))(g_camMgr, nullptr, c); });
        LOG("split: player %d's camera %p was not a world camera (unit/follow camera) - player %d now has their own world camera %p%s",
            who + 1, c, who + 1, nc, del ? " (old one deleted)" : "");
        return true;
    }

    // ---- JPOG34 (owner bug 8): zoom out further. AWorldCamera's max distance = camera+0x3BC (ctor 0x527A40 copies
    // the global at 0x8C50D4; ConstMaxDistance 0x47D680 returns it) and AWorldCamera::scm_fMaxAlt (0xD12C14, -82:
    // the engine's Y points down). Both x [coop] zoom_out_mult, once; live world cameras get the new +0x3BC.
    float g_zoomOrigMax = 0, g_zoomNewMax = 0; bool g_zoomDone = false;
    void ApplyZoomGlobals() {
        if (g_zoomDone) return;
        const float k = config::Get().zoomOutMult;
        float* gmax = reinterpret_cast<float*>(game::At(0x008C50D4));
        float* galt = reinterpret_cast<float*>(game::At(0x00D12C14));
        float m0 = *gmax, a0 = *galt;
        if (k <= 1.001f) { g_zoomDone = true; LOG("zoom: zoom_out_mult = 1 - game default (max distance %.1f, max altitude %.1f)", m0, a0); return; }
        if (!(m0 > 1.0f && m0 < 100000.0f) || !(a0 < -1.0f && a0 > -100000.0f)) {
            static bool s_logged = false;
            if (!s_logged) { s_logged = true; LOG("zoom: camera limits not set yet (max distance %g, max altitude %g) - retrying", m0, a0); }
            if (gamectl::InPark()) { g_zoomDone = true; LOG("zoom: unexpected camera limits in the park (%g, %g) - zoom_out_mult not applied", m0, a0); }
            return; }
        g_zoomDone = true;
        DWORD old;
        if (VirtualProtect(gmax, 4, PAGE_READWRITE, &old)) { *gmax = m0 * k; VirtualProtect(gmax, 4, old, &old); }
        if (VirtualProtect(galt, 4, PAGE_READWRITE, &old)) { *galt = a0 * k; VirtualProtect(galt, 4, old, &old); }
        g_zoomOrigMax = m0; g_zoomNewMax = *gmax;
        LOG("zoom: world camera max distance %.1f -> %.1f, max altitude %.1f -> %.1f (zoom_out_mult %.2f)", m0, *gmax, a0, *galt, k);
    }
    void ZoomCamera(void* c) {
        if (!c || g_zoomNewMax <= 0 || !gamectl::IsA(c, cam::WorldCameraClass)) return;
        float* d = reinterpret_cast<float*>((uint8_t*)c + 0x3BC);
        if (fabsf(*d - g_zoomOrigMax) < 0.01f) { *d = g_zoomNewMax; LOG("zoom: camera %p max distance %.1f -> %.1f", c, g_zoomOrigMax, g_zoomNewMax); }
    }

    // ---- JPOG34 (owner bug 1): visitor thought bubbles / unit icons ----
    // AUnitIcons::UpdateIcons (0x49E3B0) projects each icon with ARenderer::ConvertToScreenCoords (call at 0x49E48B,
    // hooked by JPOGMouseFix.asi into HUD/GUI space). The GUI is the active player's half; icons outside that half
    // were drawn across the other player's half. Now: icons outside the active half (GUI space) are moved off
    // screen, and the active half is rendered LAST so the projection uses the active player's camera.
    float g_iconLo[2]{}, g_iconHi[2]{}, g_iconSz[2]{}; unsigned g_iconFrame = ~0u; int g_iconLogs = 0;
    void* g_iconRnd = nullptr;
    // JPOG55 (owner 6b + 7: highlight ring / thought bubbles far up-left of their unit): vanilla ConvertToScreenCoords
    // turns viewport pixels into GUI units with ONE factor guiW / render width - right for a full-screen view, wrong for
    // a half (the GUI is mapped onto the HUD half). [coop] icon_own_proj: the same projection as 0x477EC0 (view +0x8C via
    // TMatrix44::TransformVector IAT 0x739DF8, near +0x40, K [0xCEFA88]/z, scale +0x38/+0x3C, centre +0x30/+0x34), then
    // pixels -> GUI with the HUD half's own mapping (ConvertRenderScreenToGuiScreen of the half's corners = g_iconLo/Hi).
    int g_ownProjLogs = 0;
    bool OwnProject(void* rnd, const float* in, float* out2) {   // false = behind the camera
        uint8_t* rc = (rnd && Ptr(rnd, 0x2C)) ? (uint8_t*)Ptr(Ptr(rnd, 0x2C), 0x34) : nullptr;
        if (!rc || g_iconSz[0] < 1.0f || g_iconSz[1] < 1.0f) return false;
        float t[4]{};
        using fn_tv = void(__stdcall*)(float*, const void*, const float*);   // static, callee pops 3 args (0x477F10)
        reinterpret_cast<fn_tv>(*reinterpret_cast<void**>(game::At(0x00739DF8)))(t, rc + 0x8C, in);
        const float nearZ = *reinterpret_cast<float*>(rc + 0x40);
        if (t[2] < nearZ || t[2] <= 0.0f) return false;
        const float k = *reinterpret_cast<float*>(game::At(0x00CEFA88)) / t[2];
        const float px = t[0] * k * *reinterpret_cast<float*>(rc + 0x38) + *reinterpret_cast<float*>(rc + 0x30);
        const float py = t[1] * k * *reinterpret_cast<float*>(rc + 0x3C) + *reinterpret_cast<float*>(rc + 0x34);
        out2[0] = g_iconLo[0] + px * (g_iconHi[0] - g_iconLo[0]) / g_iconSz[0];
        out2[1] = g_iconLo[1] + py * (g_iconHi[1] - g_iconLo[1]) / g_iconSz[1];
        return true;
    }
    void IconRefreshHalf() {
        if (g_iconFrame == g_frame) return;
        g_iconFrame = g_frame;
        float sz[2]{}, a[2]{ 0, 0 }, b[2]{};
        reinterpret_cast<void(__stdcall*)(float*)>(game::At(0x0050BD30))(sz);                  // ACamera::GetRenderScreenSize
        b[0] = sz[0]; b[1] = sz[1]; g_iconSz[0] = sz[0]; g_iconSz[1] = sz[1];
        reinterpret_cast<void(__stdcall*)(const float*, float*)>(game::At(0x0050BDD0))(a, g_iconLo); // RenderScreen -> GuiScreen
        reinterpret_cast<void(__stdcall*)(const float*, float*)>(game::At(0x0050BDD0))(b, g_iconHi);
        if (g_iconLogs < 1) { ++g_iconLogs; LOG("split: unit icons limited to the HUD half (GUI %.0f,%.0f - %.0f,%.0f)", g_iconLo[0], g_iconLo[1], g_iconHi[0], g_iconHi[1]); }
    }
    void __fastcall Icon_ToScreen(void* rnd, void* edx, float* v) {
        g_iconRnd = rnd;   // JPOG40: the ARenderer for the second (other half) icon pass
        const float in[3] = { v[0], v[1], v[2] };
        reinterpret_cast<void(__fastcall*)(void*, void*, float*)>(game::At(0x00477EC0))(rnd, edx, v);
        if (!g_active || !config::Get().iconsActiveHalf) return;
        if (v[0] == -100.0f && v[1] == -100.0f) return;          // behind the camera (game's marker)
        if (g_menuOpen) { v[0] = v[1] = -10000.0f; return; }
        IconRefreshHalf();
        if (config::Get().iconOwnProj) {
            float o[2];
            if (!OwnProject(rnd, in, o)) { v[0] = v[1] = -10000.0f; return; }
            if (g_ownProjLogs < 4) { ++g_ownProjLogs; LOG("split: icon projection (6b/7): game GUI %.0f,%.0f -> own %.0f,%.0f", v[0], v[1], o[0], o[1]); }
            v[0] = o[0]; v[1] = o[1];
        }
        if (v[0] < g_iconLo[0] || v[0] > g_iconHi[0] || v[1] < g_iconLo[1] || v[1] > g_iconHi[1]) { v[0] = v[1] = -10000.0f; }
    }

    // JPOG57 (owner 12:05 "building and fence highlights still not accurate for player 1 or 2"; floating bubbles): the
    // building/fence highlight is NOT a unit icon (AUnit::GetIconType returns false for buildings). It is the selection box
    // of AUnitSelectionHudElement::DrawSelectionBoxItem 0x5B84B0 -> AMiniGameState::GetScreenBBox 0x544490 ->
    // ARenderer::ConvertBBoxToScreenCoords 0x4780C0, whose two ConvertToScreenCoords calls (0x4781AD / 0x4781B9) still used
    // the full-screen factor. Same for the floating text (AFloatHudElement::AddFloat 0x5AC387, SortTimedFloats 0x5AC75D) and
    // 0x51936B (FUN_5192F0, a world point -> GUI check). [coop] overlay_own_proj: those calls use OwnProject in split-screen
    // (no hiding outside the half: the callers clip themselves). Single player: the game's own call, unchanged.
    int g_ovlLogs = 0;
    void __fastcall Overlay_ToScreen(void* rnd, void* edx, float* v) {
        const float in[3] = { v[0], v[1], v[2] };
        reinterpret_cast<void(__fastcall*)(void*, void*, float*)>(game::At(0x00477EC0))(rnd, edx, v);
        if (!g_active || !config::Get().overlayOwnProj) return;
        if (v[0] == -100.0f && v[1] == -100.0f) return;          // behind the camera (game's marker)
        IconRefreshHalf();
        float o[2];
        if (!OwnProject(rnd, in, o)) return;
        if (g_ovlLogs < 4) { ++g_ovlLogs; LOG("split: overlay projection (highlight box / floats): game GUI %.0f,%.0f -> own %.0f,%.0f", v[0], v[1], o[0], o[1]); }
        v[0] = o[0]; v[1] = o[1];
    }

    // ---- JPOG40 (owner bug 5): unit icons ("Zzz", thought bubbles) in BOTH halves, each with its own view ----
    // UpdateIcons (above) projects with the HUD half's view and hides what is outside the HUD half. AUnitIcons::
    // RenderIcons 0x49E560 (stdcall, ret 0xC; GUI paint callback) draws the list (head 0x8B7D70: node +4 next, +0xC
    // proxy, +0x14/+0x18 GUI position, +0x1C icon type). Detour: draw it, then re-project every icon with the OTHER
    // half's render-context values (+0x30 centre/scale/near, +0x8C view - what ConvertToScreenCoords 0x477EC0 reads,
    // captured right after that half's own pass), shift by the half's offset in GUI units, draw again, restore.
    uint8_t g_otherRcA[0x14], g_otherRcB[0x40]; unsigned g_otherRcFrame = ~0u; float g_otherShift[2]{}; int g_iconPassLogs = 0;
    uint8_t g_hudRcA[0x14], g_hudRcB[0x40]; unsigned g_hudRcFrame = ~0u; int g_hudIconLogs = 0;   // JPOG62 item D
    using fn_renderIcons = bool(__stdcall*)(void*, void*, void*);
    fn_renderIcons g_origRenderIcons = nullptr;
    using fn_iconPos = bool(__fastcall*)(void* unit, void* edx, void* type, float* v, int* ox, int* oy, void* node);
    // JPOG57 (owner list 11:51 "Zzz over a sleeping controlled dino"): vanilla shows most drive icons - Sleep included
    // (drive 8 -> icon 3) - only while the "more info" key is held (AGameState::ProcessMoreInfo -> AUnitIcons::
    // SetShowInfoMode; GetIconType marks every drive except 4/0x10/0x11 "info mode only"). For a player's sleeping dino
    // ([dino] zzz_always) the node is drawn as fully shown: flags |= 0x11, alpha (+0x20) = 1 - what UpdateIcons sets for
    // a shown icon. Position and icon type (+0x14/+0x18/+0x1C) are UpdateIcons' own for this frame.
    int g_zzzLogs = 0;
    void ForceZzzIcons() {
        if (!config::Get().dinoZzzAlways) return;
        GUARDED("zzz icon", {
            uint8_t* head = (uint8_t*)game::At(0x008B7D70);
            int n = 0;
            for (uint8_t* nd = *(uint8_t**)(head + 4); nd && nd != head && n < 4096; nd = *(uint8_t**)(nd + 4), ++n) {
                void* px = *(void**)(nd + 0xC);
                void* unit = px ? *(void**)((uint8_t*)px + 0x14) : nullptr;
                if (!unit || !dinocontrol::IsZzzUnit(unit)) continue;
                *(uint32_t*)(nd + 0x10) |= 0x11; *(float*)(nd + 0x20) = 1.0f;
                if (g_zzzLogs < 2) { ++g_zzzLogs; LOG("dino: sleep icon of the player's dino shown (icon type %d at GUI %d,%d)",
                    *(int*)(nd + 0x1C), *(int*)(nd + 0x14), *(int*)(nd + 0x18)); }
            }
        });
    }
    // Re-project every icon node with the render-context values A/B (one half's own pass), shifted by gx,gy GUI units.
    // Saves the original node positions into saved[] (n). JPOG40 for the other half; JPOG62 also for the HUD half.
    struct SavedIcon { uint8_t* node; int x, y; };
    bool ReprojectIcons(uint8_t* rc, const uint8_t* A, const uint8_t* B, float gx, float gy, SavedIcon* saved, int& n, int& shown) {
        uint8_t keepA[0x14], keepB[0x40];
        memcpy(keepA, rc + 0x30, sizeof keepA); memcpy(keepB, rc + 0x8C, sizeof keepB);
        memcpy(rc + 0x30, A, sizeof keepA); memcpy(rc + 0x8C, B, sizeof keepB);
        n = 0; shown = 0;
        bool ok = GUARDED("icons: reproject", {
            uint8_t* head = (uint8_t*)game::At(0x008B7D70);
            for (uint8_t* nd = *(uint8_t**)(head + 4); nd && nd != head && n < 256; nd = *(uint8_t**)(nd + 4)) {
                saved[n].node = nd; saved[n].x = *(int*)(nd + 0x14); saved[n].y = *(int*)(nd + 0x18); ++n;
                int nx = -10000, ny = -10000;
                void* px = *(void**)(nd + 0xC);
                void* unit = px ? *(void**)((uint8_t*)px + 0x14) : nullptr;
                if (unit && (*(uint32_t*)((uint8_t*)unit + 0x14) & 0x100)) {
                    float v[3]{}; int ox = *(int*)game::At(0x008C550C), oy = *(int*)game::At(0x008C5510);
                    auto fn = reinterpret_cast<fn_iconPos>((*reinterpret_cast<void***>(unit))[0x24 / 4]);
                    if (fn(unit, nullptr, nd + 0x1C, v, &ox, &oy, nd)) {
                        bool behind;
                        if (config::Get().iconOwnProj) {   // JPOG55: same own projection as Icon_ToScreen
                            float o[2]; behind = !OwnProject(g_iconRnd, v, o); v[0] = o[0]; v[1] = o[1];
                        } else {
                            reinterpret_cast<void(__fastcall*)(void*, void*, float*)>(game::At(0x00477EC0))(g_iconRnd, nullptr, v);
                            behind = v[0] == -100.0f && v[1] == -100.0f;
                        }
                        if (!behind && v[0] >= g_iconLo[0] && v[0] <= g_iconHi[0] && v[1] >= g_iconLo[1] && v[1] <= g_iconHi[1]) {
                            nx = ox + (int)lroundf(v[0] + gx); ny = oy + (int)lroundf(v[1] + gy); ++shown;
                        }
                    }
                }
                *(int*)(nd + 0x14) = nx; *(int*)(nd + 0x18) = ny;
            }
        });
        memcpy(rc + 0x30, keepA, sizeof keepA); memcpy(rc + 0x8C, keepB, sizeof keepB);
        return ok;
    }
    void RestoreIcons(SavedIcon* saved, int n) {
        for (int i = 0; i < n; ++i) { *(int*)(saved[i].node + 0x14) = saved[i].x; *(int*)(saved[i].node + 0x18) = saved[i].y; }
    }
    bool __stdcall Hook_RenderIcons(void* a, void* screen, void* ev) {
        ForceZzzIcons();   // JPOG57
        const bool canSplit = g_active && config::Get().iconsActiveHalf && g_iconRnd && g_renderer &&
            g_frame - g_iconFrame <= 2 && g_iconSz[0] >= 1.0f && g_iconSz[1] >= 1.0f;
        void* vp = canSplit ? Ptr(g_renderer, 0x2C) : nullptr;
        uint8_t* rc = vp ? (uint8_t*)Ptr(vp, 0x34) : nullptr;
        static SavedIcon saved[256];
        int n = 0, shown = 0;
        bool r;
        // JPOG62 item D (owner 13:15: bubbles right in player 2's half, wrong in player 1's = the HUD half): the HUD half
        // now uses the SAME re-projection as the other half - render-context values captured right after its own pass.
        if (rc && config::Get().iconHudReproj && !g_menuOpen && g_frame - g_hudRcFrame <= 2) {
            bool ok = ReprojectIcons(rc, g_hudRcA, g_hudRcB, 0.0f, 0.0f, saved, n, shown);
            r = ok ? g_origRenderIcons(a, screen, ev) : false;
            RestoreIcons(saved, n);
            if (!ok) r = g_origRenderIcons(a, screen, ev);
            if (g_hudIconLogs < 2 && (shown || !ok)) { ++g_hudIconLogs;
                LOG("split: unit icons in the HUD half re-projected with its own pass (%d of %d in view, ok=%d, JPOG62 [coop] icon_hud_reproj)", shown, n, ok); }
        } else r = g_origRenderIcons(a, screen, ev);
        if (!rc || g_frame - g_otherRcFrame > 2) return r;
        const float sx = (g_iconHi[0] - g_iconLo[0]) / g_iconSz[0], sy = (g_iconHi[1] - g_iconLo[1]) / g_iconSz[1];
        const float gx = g_otherShift[0] * sx, gy = g_otherShift[1] * sy;
        bool ok = ReprojectIcons(rc, g_otherRcA, g_otherRcB, gx, gy, saved, n, shown);
        if (ok) g_origRenderIcons(a, screen, ev);
        RestoreIcons(saved, n);
        if (g_iconPassLogs < 2 && (shown || !ok)) { ++g_iconPassLogs;
            LOG("split: unit icons drawn in the other half too (%d of %d icons in view, GUI shift %.0f,%.0f, ok=%d)", shown, n, gx, gy, ok); }
        return r;
    }

    // ---- JPOG40 (owner bug 4): dino sounds silent in split-screen ----
    // ACamera::OnUpdate sets the listener (ASoundManager::SetListenerPosition 0x690110, call 0x50A8A5) to the ENGINE
    // camera = the builder's. ASoundManager::PlaySound3D 0x68ED40 does not even start a sound farther from the listener
    // than its max distance, so the rider's dino (anim sound events: AVisibleUnit::GenSoundCallback) was silent. While a
    // player rides a dino in split, the listener sits at that dino.
    int g_listenLogs = 0;
    void __fastcall Listener_SetPos(void* sm, void* edx, const float* pos) {
        float d[3];
        if (dinocontrol::ListenerPos(d)) {
            if (g_listenLogs < 3) { ++g_listenLogs; LOG("split: sound listener moved to the rider's dino (%.1f %.1f %.1f), camera was (%.1f %.1f %.1f)",
                d[0], d[1], d[2], pos[0], pos[1], pos[2]); }
            pos = d;
        }
        reinterpret_cast<void(__fastcall*)(void*, void*, const float*)>(game::At(0x00690110))(sm, edx, pos);
    }

    // ---- JPOG42 (owner finding 12:50): a ridden dino is heard only by the HUD player ----
    // ASoundManager::PlaySound3D 0x68ED40 (thiscall: int sample, const TVector3& pos, uint flags, float; ret 0x10) skips a
    // sound farther from the ONE listener (mgr+0x28 = engine/HUD camera) than its max distance, and FMOD has only that
    // listener. Every ridden dino is an extra "ear": a sound nearer to a rider's dino than to the listener is played at
    // the same offset from the listener as from that dino (pos' = pos - dino + listener). So each rider hears their own dino.
    // JPOG57: rider sounds that follow the listener. ASoundManager::SetPosition 0x68E410 (thiscall int ch, const TVector3&;
    // ret 8) refuses a channel whose s_aChannels slot ([0x852D0C] + (ch & 0xFFF) * 0xC) is -1; the slot's +4 = the full FMOD
    // handle, so a reused channel (other handle) or a stopped one is dropped. FSOUND_IsPlaying (fmod.dll) ends it early.
    struct RiderSnd { int ch; float off[3]; DWORD t0; };
    RiderSnd g_rs[16]; int g_rsN = 0; void* g_rsMgr = nullptr; int g_rsLogs = 0;
    using fn_isPlaying = signed char(__stdcall*)(int);
    fn_isPlaying g_fsIsPlaying = nullptr; bool g_fsTried = false;
    void TrackRiderSound(void* sm, int ch, float ox, float oy, float oz) {
        g_rsMgr = sm;
        for (int i = 0; i < g_rsN; ++i) if (g_rs[i].ch == ch) { g_rs[i] = g_rs[--g_rsN]; break; }
        if (g_rsN >= 16) { for (int i = 1; i < 16; ++i) g_rs[i - 1] = g_rs[i]; g_rsN = 15; }
        g_rs[g_rsN].ch = ch; g_rs[g_rsN].off[0] = ox; g_rs[g_rsN].off[1] = oy; g_rs[g_rsN].off[2] = oz; g_rs[g_rsN].t0 = GetTickCount(); ++g_rsN;
    }
    void RiderSoundsTick() {
        if (!g_rsN) return;
        if (!g_active || !g_rsMgr) { g_rsN = 0; return; }
        if (!g_fsTried) { g_fsTried = true; HMODULE fm = GetModuleHandleA("fmod.dll");
            if (fm) g_fsIsPlaying = reinterpret_cast<fn_isPlaying>(GetProcAddress(fm, "_FSOUND_IsPlaying@4")); }
        const float* L = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(g_rsMgr) + 0x28);
        uint8_t* chans = *reinterpret_cast<uint8_t**>(game::At(0x00852D0C));
        const DWORD now = GetTickCount();
        for (int i = 0; i < g_rsN; ) {
            RiderSnd& r = g_rs[i];
            bool keep = chans && now - r.t0 < 12000;
            if (keep) {
                const int* slot = reinterpret_cast<const int*>(chans + (r.ch & 0xFFF) * 0xC);
                keep = slot[0] != -1 && slot[1] == r.ch;
            }
            if (keep && g_fsIsPlaying) keep = g_fsIsPlaying(r.ch) != 0;
            if (keep) {
                float p[3] = { L[0] + r.off[0], L[1] + r.off[1], L[2] + r.off[2] };
                GUARDED("rider sound follows the listener", {
                    reinterpret_cast<bool(__fastcall*)(void*, void*, int, const float*)>(game::At(0x0068E410))(g_rsMgr, nullptr, r.ch, p); });
                if (g_rsLogs < 2 && now - r.t0 > 500) { ++g_rsLogs;
                    LOG("split: rider sound %d kept at its offset from the moving listener (%.1f %.1f %.1f)", r.ch, r.off[0], r.off[1], r.off[2]); }
                ++i;
            } else g_rs[i] = g_rs[--g_rsN];
        }
    }

    struct SndCount { int sample; DWORD t0; int n; unsigned total, dropped; bool used; };
    SndCount g_snd[32]{}; DWORD g_sndLogT = 0; unsigned g_sndDropped = 0; int g_sndLogs = 0;   // JPOG59 sound limiter
    using fn_playSound3D = int(__fastcall*)(void*, void*, int, const float*, unsigned, float);
    fn_playSound3D g_origPlaySound3D = nullptr;
    int g_earLogs = 0;
    int __fastcall Hook_PlaySound3D(void* sm, void* edx, int sample, const float* pos, unsigned flags, float f) {
        if (sm && pos && g_active) {
            float ears[2][3]; int who[2];
            int n = dinocontrol::RiderEars(ears, who);
            if (n > 0) {
                const float* L = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(sm) + 0x28);
                auto d2 = [&](const float* a) { float x = pos[0]-a[0], y = pos[1]-a[1], z = pos[2]-a[2]; return x*x + y*y + z*z; };
                float best = d2(L); int bi = -1;
                for (int i = 0; i < n; ++i) { float d = d2(ears[i]); if (d < best) { best = d; bi = i; } }
                if (bi >= 0) {
                    float moved[3] = { pos[0] - ears[bi][0] + L[0], pos[1] - ears[bi][1] + L[1], pos[2] - ears[bi][2] + L[2] };
                    if (g_earLogs < 3) { ++g_earLogs;
                        LOG("split: sound for P%d's dino moved to the listener (sample %d, (%.1f %.1f %.1f) -> (%.1f %.1f %.1f), %.1f m from the dino)",
                            who[bi] + 1, sample, pos[0], pos[1], pos[2], moved[0], moved[1], moved[2], sqrtf(best)); }
                    int ch = g_origPlaySound3D(sm, edx, sample, moved, flags, f);
                    // JPOG57 (owner list 11:51: "player 2's roar gets louder / quieter while player 1's camera moves"): the
                    // sound was placed next to the listener ONCE; the listener (the HUD player's camera) then moved away
                    // from it. Keep it at the same offset from the listener until it ends (RiderSoundsTick).
                    if (ch >= 0 && config::Get().riderSoundFollow) TrackRiderSound(sm, ch, pos[0] - ears[bi][0], pos[1] - ears[bi][1], pos[2] - ears[bi][2]);
                    return ch;
                }
            }
        }
        // JPOG59 (open list 11 / B9, owner 11:51: "too many goat sounds"): split-screen keeps more animals at full rate
        // (both halves' culling) = more anim sound events. Split only ([coop] sound_limit = max starts of ONE sample per
        // second, 0 = off): extra starts of the same sample in that second are skipped (PlaySound3D's own "too far"
        // answer, -1). Riders' sounds (above) are never limited. Every 10 s the most repeated samples are logged.
        if (g_active && config::Get().soundLimit > 0 && sample >= 0) {
            const DWORD now = GetTickCount();
            int slot = -1, empty = -1, oldest = 0;
            for (int i = 0; i < 32; ++i) {
                if (g_snd[i].sample == sample && g_snd[i].used) { slot = i; break; }
                if (!g_snd[i].used && empty < 0) empty = i;
                if (g_snd[i].t0 < g_snd[oldest].t0) oldest = i;
            }
            if (slot < 0) { slot = empty >= 0 ? empty : oldest; g_snd[slot] = SndCount{ sample, now, 0, 0, 0, true }; }
            SndCount& sc = g_snd[slot];
            if (now - sc.t0 >= 1000) { sc.t0 = now; sc.n = 0; }
            ++sc.total;
            if (++sc.n > config::Get().soundLimit) { ++sc.dropped; ++g_sndDropped; return -1; }
            if (now - g_sndLogT >= 10000) {
                g_sndLogT = now;
                int a = -1, b = -1;
                for (int i = 0; i < 32; ++i) if (g_snd[i].used) { if (a < 0 || g_snd[i].total > g_snd[a].total) { b = a; a = i; } else if (b < 0 || g_snd[i].total > g_snd[b].total) b = i; }
                if (a >= 0 && g_sndLogs < 30) { ++g_sndLogs;
                    LOG("split sounds (10 s): top sample %d x%u (%u skipped), next %d x%u (%u skipped), %u skipped in all (limit %d/s)",
                        g_snd[a].sample, g_snd[a].total, g_snd[a].dropped, b >= 0 ? g_snd[b].sample : -1, b >= 0 ? g_snd[b].total : 0,
                        b >= 0 ? g_snd[b].dropped : 0, g_sndDropped, config::Get().soundLimit); }
                for (int i = 0; i < 32; ++i) { g_snd[i].total = 0; g_snd[i].dropped = 0; }
                g_sndDropped = 0;
            }
        }
        return g_origPlaySound3D(sm, edx, sample, pos, flags, f);
    }

    // ---- JPOG40 (owner bug 2): ocean water animates / reflects too fast in split-screen ----
    // TTerrainShaderHAL::RenderWater (TTerrainShaderD3D.dll +0x7AF0) advances the water time itself on every call:
    // this+0x5AC (time accumulator), +0x14C2C4 (animation frame 0..31), +0x14C2E4 (reflection rotation). Split draws
    // the main scene twice a frame -> 2x speed. Detour: the 2nd+ call in the same frame puts the three values back.
    using fn_renderWater = void(__fastcall*)(void*, void*);
    fn_renderWater g_origRenderWater = nullptr;
    uint8_t* g_terrBase = nullptr; bool g_waterTried = false;
    unsigned g_waterFrame = ~0u; int g_waterCalls = 0, g_waterLogs = 0;
    void __fastcall Hook_RenderWater(void* self, void* edx) {
        if (g_waterFrame != g_frame) {
            if (g_waterLogs < 2 && g_waterFrame + 1 == g_frame && g_waterCalls > 1) { ++g_waterLogs;
                LOG("split: water drawn %d times in one frame (split %s)%s", g_waterCalls, g_active ? "on" : "off",
                    g_active ? " - its animation now advances once per frame" : " - game's own speed, left alone"); }
            g_waterFrame = g_frame; g_waterCalls = 0;
        }
        if (++g_waterCalls == 1 || !g_active) { g_origRenderWater(self, edx); return; }
        float* t = (float*)((uint8_t*)self + 0x5AC);
        uint32_t* fr = (uint32_t*)(g_terrBase + 0x14C2C4); float* rot = (float*)(g_terrBase + 0x14C2E4);
        const float t0 = *t; const uint32_t f0 = *fr; const float r0 = *rot;
        g_origRenderWater(self, edx);
        *t = t0; *fr = f0; *rot = r0;
    }
    void TryInstallWater() {
        if (g_waterTried) return;
        HMODULE m = GetModuleHandleA("TTerrainShaderD3D.dll");
        if (!m) return;
        g_waterTried = true;
        g_terrBase = (uint8_t*)m;
        static const uint8_t pro[10] = { 0x57, 0x56, 0x55, 0x53, 0x81, 0xEC, 0x64, 0x02, 0x00, 0x00 };
        uint8_t* fn = g_terrBase + 0x7AF0;
        if (memcmp(fn, pro, sizeof pro) != 0) { LOG("split: water hook NOT applied (RenderWater bytes differ)"); return; }
        bool ok = detour::Install(fn, (void*)&Hook_RenderWater, (void**)&g_origRenderWater);
        LOG("split: water once-per-frame hook %s (TTerrainShaderD3D.dll at %p)", ok ? "on" : "FAILED", m);
    }

    // ---- JPOG34 (owner bug 3): show the game's hint in a given player's half ----
    // The hint widget = AGUISystem+0x5C, centred by AGUISystem::UpdateHintMessage (x = (screenW - w) / 2,
    // TGUIWidget +0x6C x, +0x6E y, +0x70 w). While a rider's hint shows it is moved to the middle of their half.
    volatile LONG g_hintHalf = -1; DWORD g_hintUntil = 0; bool g_hintMoved = false;
    void InvalidateWidget(void* w) {
        auto inv = *reinterpret_cast<void(__fastcall**)(void*, void*, bool)>(game::At(0x00739830));   // TGUIWidget::InvalidateComponentCache
        if (inv && w) inv(w, nullptr, true);
    }
    void* HintWidget() {
        void* sys = reinterpret_cast<void*(__stdcall*)()>(game::At(0x0067E3F0))();
        return sys ? Ptr(sys, 0x5C) : nullptr;
    }
    // JPOG35: the hint the player SEES is the HUD hint element (AGUISystem::UpdateHintMessageText 0x67E3B0 ->
    // FindHudElement(TClass 0x8B37E0) -> 0x5AE2D0 sets its label; element+0x18 = its widget, placed at the bottom,
    // x kept). That is the wrapped box bottom-right of the HUD half - JPOG34 only moved the centred LoadStatusDialog
    // (+0x5C). Both are moved now. The GUI pass is not clipped to the HUD half (thought bubbles spilled over in JPOG33).
    void* HudHintWidget() {
        void* sys = reinterpret_cast<void*(__stdcall*)()>(game::At(0x0067E3F0))();
        if (!sys) return nullptr;
        void* el = reinterpret_cast<void*(__fastcall*)(void*, void*, const void*)>(game::At(0x0067C5C0))(sys, nullptr, game::At(0x008B37E0));
        return el ? Ptr(el, 0x18) : nullptr;
    }
    // JPOGMouseFix's AnchorHud treats a changed x as the panel's new "original" x and snaps it to the half's edge
    // again, so writing back the saved (already snapped) x could leave it shifted. Restore = write, check the painted
    // x on the next frames and correct by the difference (converges in 1-2 frames).
    struct HintMove { void* w = nullptr; short saved = 0, written = 0; int restore = 0; bool moved = false; };
    HintMove g_hudHint;
    void RestoreStep(HintMove& m) {
        if (!m.restore || !m.w) { m.restore = 0; return; }
        short* px = reinterpret_cast<short*>((uint8_t*)m.w + 0x6C);
        if (*px == m.saved) { m.restore = 0; return; }
        m.written = (short)(m.written + (m.saved - *px)); *px = m.written;
        if (--m.restore == 0) LOG("split: HUD hint x back to %d (now %d)", m.saved, *px);
    }
    void RectFor(int player, float& x, float& y, float& w, float& h);
    short HalfCentreGuiX(int hp, int act, short w) {
        float ox, oy, ow, oh, ax, ay, aw, ah;
        RectFor(hp, ox, oy, ow, oh); RectFor(act, ax, ay, aw, ah);
        float in[2] = { ox + ow * 0.5f - ax, oy + oh * 0.5f - ay }, out[2]{};
        reinterpret_cast<void(__stdcall*)(const float*, float*)>(game::At(0x0050BDD0))(in, out);
        return (short)(out[0] - w * 0.5f);
    }
    void TickHintHalf() {
        RestoreStep(g_hudHint);
        LONG hp = g_hintHalf;
        if (hp < 0) return;
        void* wdg = HintWidget(); void* sc = GuiScreen();
        if (!sc) return;
        void* hw = HudHintWidget();
        const short gw = *reinterpret_cast<short*>((uint8_t*)sc + 0x70);
        const int act = (int)g_activePlayer;
        if (!g_active || GetTickCount() > g_hintUntil || hp == act) {
            if (g_hintMoved && wdg) {
                short* px = reinterpret_cast<short*>((uint8_t*)wdg + 0x6C);
                *px = (short)((gw - *reinterpret_cast<short*>((uint8_t*)wdg + 0x70)) / 2); InvalidateWidget(wdg);
            }
            if (g_hudHint.moved && g_hudHint.w) {
                short* px = reinterpret_cast<short*>((uint8_t*)g_hudHint.w + 0x6C);
                g_hudHint.written = g_hudHint.saved; *px = g_hudHint.saved; g_hudHint.restore = 6; g_hudHint.moved = false;
                InvalidateWidget(g_hudHint.w);
            }
            if (g_hintMoved) { g_hintMoved = false; LOG("split: hint back in the HUD half"); }
            if (!g_active || GetTickCount() > g_hintUntil) g_hintHalf = -1;   // rider = HUD half: wait (hand-over comes next frame)
            return;
        }
        if (wdg) {
            short* px = reinterpret_cast<short*>((uint8_t*)wdg + 0x6C);
            const short w = *reinterpret_cast<short*>((uint8_t*)wdg + 0x70);
            short nx = HalfCentreGuiX(hp, act, w);
            if (*px != nx) {
                if (!g_hintMoved) LOG("split: hint moved into player %d's half (GUI x %d -> %d, y %d, w %d, screen %d x %d)", (int)hp + 1, *px, nx,
                    *reinterpret_cast<short*>((uint8_t*)wdg + 0x6E), w, gw, *reinterpret_cast<short*>((uint8_t*)sc + 0x72));
                *px = nx; InvalidateWidget(wdg);
            }
        }
        if (hw) {
            short* px = reinterpret_cast<short*>((uint8_t*)hw + 0x6C);
            const short w = *reinterpret_cast<short*>((uint8_t*)hw + 0x70);
            short nx = HalfCentreGuiX(hp, act, w);
            if (!g_hudHint.moved || g_hudHint.w != hw) {
                g_hudHint.w = hw; g_hudHint.saved = *px; g_hudHint.restore = 0; g_hudHint.moved = true;
                LOG("split: HUD hint box moved into player %d's half (GUI x %d -> %d, w %d)", (int)hp + 1, *px, nx, w);
            }
            if (*px != nx) { *px = nx; InvalidateWidget(hw); }
        }
        g_hintMoved = true;
    }

    // ---- JPOG39 (owner task 22:08 item 1): demolish-style GROUND CIRCLE under each player's aiming point.
    // The JPOG34 TLineShaderHAL ring drew nothing. Now the game's own terrain UI footprint, exactly as the demolish
    // tool draws it every frame (ADemolishState::OnUpdate 0x530C20):
    //   ATerrainSelection* = *(0x8500AC); AddUIFootPrint 0x522EC0 (thiscall, ret 0x18):
    //     (uint x, uint z, eUIRot 0, float size 4.0, const TVector3& colour 0..1, TMaterial*)
    //   material = AGameController::GetMaterial(0xF) 0x472740 (thiscall, ret 4); AGameController = root task+0x2C
    //   x/z = round(world x/z), clamped to [4, size-5] with the terrain size at *(0x85006C)+0x28 -> +0x38 / +0x3C.
    // RenderUIMeshes 0x5258A0 (inside every main scene pass) draws the queued footprints and puts the nodes back on the
    // free list (0x525971) = ONE pass. So a footprint is queued right BEFORE each half's own pass, for that half's player.
    // The HUD player's point = the game's own aim (ATarget::GetTargettedTerrain 0x5201C0, ATarget = AGameController+0x2C,
    // read in Tick on the update path) = what A acts on with the centre-locked cursor; the other player's = their camera
    // target (what their A pick uses) + a second, bigger circle on the dino their A would take.
    constexpr uintptr_t kTerrainSelPtr = 0x008500AC, kSimTerrainPtr = 0x0085006C, kRootTaskPtr = 0x008B859C;
    constexpr uintptr_t kAddUIFootPrint = 0x00522EC0, kGetMaterial = 0x00472740, kGetTargettedTerrain = 0x005201C0;
    using fn_addfp  = void(__fastcall*)(void* sel, void* edx, unsigned x, unsigned z, int rot, float size, const float* col, void* mat);
    using fn_getmat = void*(__fastcall*)(void* gc, void* edx, unsigned id);
    using fn_gettgt = const float*(__fastcall*)(void* tgt, void* edx);
    const float kCircleCol[2][3] = { { 0.22f, 0.58f, 0.89f },   // player 1: the game's own "valid" blue (sm_vValidCol)
                                     { 1.00f, 1.00f, 0.30f } }; // player 2: the game's yellow (0x8B62B0)
    const float kPickCol[3] = { 1.0f, 0.55f, 0.1f };            // the dino the non-HUD player's A would take
    bool g_circleOff = false; int g_circleLogs = 0, g_circleFails = 0;
    float g_hudAim[3]{}; bool g_hudAimOk = false;               // set in Tick (update path), used in the render hook
    bool g_circleQueued = false; int g_circleUnusedFrames = 0; bool g_circleHeld = false; int g_circleStateLogs = 0;
    bool g_circleDrawnLogged = false;
    void* g_pickCand[2]{}; float g_pickCandPos[2][3]{}; unsigned g_pickFrame[2]{};
    void* TerrainSel() { return *reinterpret_cast<void**>(game::At(kTerrainSelPtr)); }
    bool QueueFootprint(const float* pos, float size, const float* col) {
        void* sel = TerrainSel();
        void* ter = *reinterpret_cast<void**>(game::At(kSimTerrainPtr));
        void* root = *reinterpret_cast<void**>(game::At(kRootTaskPtr));
        if ((uintptr_t)sel < 0x10000 || (uintptr_t)ter < 0x10000 || (uintptr_t)root < 0x10000) return false;
        uint8_t* td = *reinterpret_cast<uint8_t**>((uint8_t*)ter + 0x28);
        void* gc = *reinterpret_cast<void**>((uint8_t*)root + 0x2C);
        if (!td || !gc) return false;
        const int dx = *reinterpret_cast<int*>(td + 0x38), dz = *reinterpret_cast<int*>(td + 0x3C);
        if (dx < 16 || dz < 16 || !std::isfinite(pos[0]) || !std::isfinite(pos[2])) return false;
        int x = (int)lroundf(pos[0]), z = (int)lroundf(pos[2]);
        if (x < 4) x = 4; if (x > dx - 5) x = dx - 5;
        if (z < 4) z = 4; if (z > dz - 5) z = dz - 5;
        void* mat = reinterpret_cast<fn_getmat>(game::At(kGetMaterial))(gc, nullptr, 0xF);
        if (!mat) return false;
        reinterpret_cast<fn_addfp>(game::At(kAddUIFootPrint))(sel, nullptr, (unsigned)x, (unsigned)z, 0, size, col, mat);
        return true;
    }
    // Game thread (Tick): the HUD player's aim point, only in the plain park view (not demolish / placement / follow /
    // menus - those draw their own footprint or have no aim).
    void UpdateHudAim() {
        g_hudAimOk = false;
        const Config& c = config::Get();
        if (g_circleOff || !c.aimCircle || !input_map::ConsoleActive() || !WorldReady() || !gamectl::InPark()) return;
        if (!gamectl::IsDefaultState(gamectl::CurrentState())) return;
        const int hud = g_active ? (int)g_activePlayer : 0;
        if (dinocontrol::Controls(hud)) return;
        bool ok = GUARDED("aim circle target", {
            void* root = *reinterpret_cast<void**>(game::At(kRootTaskPtr));
            void* gc = root ? *reinterpret_cast<void**>((uint8_t*)root + 0x2C) : nullptr;
            void* tgt = gc ? *reinterpret_cast<void**>((uint8_t*)gc + 0x2C) : nullptr;
            const float* t = tgt ? reinterpret_cast<fn_gettgt>(game::At(kGetTargettedTerrain))(tgt, nullptr) : nullptr;
            if (t && std::isfinite(t[0]) && std::isfinite(t[2])) { memcpy(g_hudAim, t, sizeof g_hudAim); g_hudAimOk = true; }
        });
        if (!ok && ++g_circleFails >= 3) { g_circleOff = true; LOG("aim circle: faulted 3x - circle off for this session"); }
    }
    // Render hook, main scene only, right BEFORE that half's pass. who = the player whose half is drawn next.
    void QueueCircle(int who, bool hudHalf) {
        const Config& c = config::Get();
        g_circleQueued = false;
        if (g_circleOff || g_circleHeld || !c.aimCircle || !input_map::ConsoleActive() || !WorldReady()) return;
        float pt[3]; bool have = false;
        if (hudHalf) { if (g_hudAimOk) { memcpy(pt, g_hudAim, sizeof pt); have = true; } }
        else if (g_active && !dinocontrol::Controls(who) && !builder2::Open(who) && gamectl::InPark()) {
            void* camObj = who ? g_cam2 : g_cam1;
            if (camObj) {
                GUARDED("aim circle camera target", {
                    reinterpret_cast<void(__fastcall*)(void*, void*, float*)>(game::At(0x0050C770))(camObj, nullptr, pt);  // ACamera::GetTargetPosition
                    have = std::isfinite(pt[0]) && std::isfinite(pt[2]);
                });
            }
        }
        if (!have) return;
        bool q1 = false, q2 = false;
        bool ok = GUARDED("aim circle", {
            q1 = QueueFootprint(pt, c.aimCircleSize, kCircleCol[who & 1]);
            if (c.aimPickCircle && (!hudHalf || !dinocontrol::Controls(who))) {   // JPOG52: the HUD player too (owner 10:44)
                // what this player's A takes: the nearest dino (refreshed every 10 frames)
                if (g_frame - g_pickFrame[who] >= 10) {
                    g_pickFrame[who] = g_frame;
                    g_pickCand[who] = dinocontrol::NearestDino(pt, c.pickRadius, g_pickCandPos[who]);
                }
                if (g_pickCand[who]) q2 = QueueFootprint(g_pickCandPos[who], c.aimCircleSize * 1.75f, kPickCol);
            }
        });
        if (!ok) { if (++g_circleFails >= 3) { g_circleOff = true; LOG("aim circle: faulted 3x - circle off for this session"); } return; }
        g_circleQueued = q1 || q2;
        if (g_circleQueued && g_circleLogs < 3) {
            ++g_circleLogs;
            LOG("aim circle: player %d at (%.1f %.1f %.1f)%s%s size %.1f", who + 1, pt[0], pt[1], pt[2], hudHalf ? " (HUD half, game aim)" : " (camera target)",
                q2 ? " + pick circle" : "", c.aimCircleSize);
        }
    }
    // Right AFTER the pass: did the game's RenderUIMeshes take the footprint(s) (active list empty again)? If the list
    // stays full for 3 passes, the terrain selection is not drawn in this state: stop queuing (no stale circles, no
    // exhausted node pool for the game's own footprints) until the game empties the list itself.
    void CheckCircleConsumed() {
        void* sel = TerrainSel();
        if ((uintptr_t)sel < 0x10000) return;
        uint8_t* head = (uint8_t*)sel + 0x9C;
        const bool empty = *reinterpret_cast<void**>(head) == head;
        if (g_circleHeld) {
            if (empty) { g_circleHeld = false; g_circleUnusedFrames = 0; if (g_circleStateLogs < 6) { ++g_circleStateLogs; LOG("aim circle: footprint list drawn again - circle back on"); } }
            return;
        }
        if (!g_circleQueued) return;
        if (empty) {
            if (!g_circleDrawnLogged && g_circleStateLogs < 6) { ++g_circleStateLogs; LOG("aim circle: drawn by the game (RenderUIMeshes took it in the same pass)"); }
            g_circleDrawnLogged = true;
            g_circleUnusedFrames = 0;
        } else if (++g_circleUnusedFrames >= 3) {
            g_circleHeld = true;
            if (g_circleStateLogs < 6) { ++g_circleStateLogs; LOG("aim circle: NOT drawn in this state (footprint list not emptied for 3 passes) - paused until the game draws it"); }
        }
        g_circleQueued = false;
    }
    // Drive the INACTIVE player's camera from their own pad (the active player's camera is the
    // game's camera, driven by the game's own input like single player). Same layout for both
    // players, matching the console chart: D-pad = pan, right stick X = rotate, right stick Y =
    // zoom (up = in). LB/RB/LS = 2.5x pan speed. The game passes the frame time as the input
    // amount (ADemoState::OnUpdate 0x532600), so a full stick/held d-pad = dt.
    int g_panLogs = 0;
    // JPOG33 (owner bug 4): the game sets the projection FOV from the ACTIVE camera every frame
    // (ARenderer::OnUpdate: TCameraObject::SetFOV(ACamera+0x134)), and AWorldCamera changes its FOV with the
    // zoom height - so one player's zoom changed the other half's FOV. Each half now uses its own camera's FOV.
    constexpr uintptr_t kIatSetFOV = 0x00739FB0, kIatGetFOV = 0x00739F38;   // SimJP imports of TCameraObject
    using fn_setfov = float(__fastcall*)(void* self, void* edx, float);
    using fn_getfov = float(__fastcall*)(void* self, void* edx);
    int g_fovLogs = 0;
    float CamFov(void* c, float fallback) {
        if (!c) return fallback;
        float f = *reinterpret_cast<float*>((uint8_t*)c + 0x134);
        return (f > 0.05f && f < 3.0f) ? f : fallback;     // radians, sane range only
    }
    void UpdateInactiveCamera(float dt) {
        if (!WorldReady()) return;   // JPOG35: never update cameras while the park world is being torn down
        int who = 1 - (int)g_activePlayer;
        void* c = who ? g_cam2 : g_cam1;
        XPad& pad = who ? g_pad2 : g_pad1;
        if (!c) return;
        // JPOG30: this player controls a dino (split mode) -> their half follows the dino, not the world camera.
        {
            float m[16]; bool used = false;
            GUARDED("dino chase camera", { used = dinocontrol::ChaseView(who, dt, g_p2, m); });
            if (used) { builder2::Close(who, "rides a dino"); p2menu::Close(who, "rides a dino"); memcpy(g_p2, m, sizeof(m)); return; }
        }
        const Config& cf0 = config::Get();
        // JPOG34 (owner bug 7): never drive a unit/follow camera with world-camera inputs.
        if (!EnsureWorldCamera(who)) return;
        c = who ? g_cam2 : g_cam1;
        ZoomCamera(c);
        // JPOG46 (backlog 1): X = this player's own build tool (buildings), at the same time as the HUD player.
        // JPOG49: X = this player's own copy of the game's build disc (p2menu); its items open builder2's tools.
        const bool menuUsed = p2menu::Update(who, pad, c, dt);   // true = the disc used this frame's buttons
        const bool menu = p2menu::IsOpen(who);
        const bool building = menuUsed || menu || builder2::Update(who, pad, c, dt) || builder2::Open(who);
        // JPOG34 (owner bug 2): A (or RB + D-pad Up) = control the nearest dino to the middle of THIS player's view.
        // JPOG57 (owner 12:05): RB + D-pad Up = the unit's vanilla info window in THIS player's half ([coop] rb_up_info);
        // A stays "take control". Nearest dino first, then the nearest building, around the middle of this view.
        if (!building && cf0.rbUpInfo && cf0.p2Windows && pad.Down(PAD_RB) && pad.Pressed(PAD_DUP)) {
            float t[3]{};
            bool okT = GUARDED("free player's aim point (info)", {
                reinterpret_cast<void(__fastcall*)(void*, void*, float*)>(game::At(0x0050C770))(c, nullptr, t); });
            void* u = okT ? dinocontrol::NearestUnitOf(t, cf0.pickRadius, 0x008B78C0) : nullptr;
            if (!u && okT) u = dinocontrol::NearestUnitOf(t, cf0.pickRadius + 10.0f, 0x008B7A60);
            LOG("split: player %d (no HUD) RB + D-pad Up - info window for unit %p near (%.1f %.1f %.1f)", who + 1, u, t[0], t[1], t[2]);
            if (u) p2menu::OpenUnitWindow(who, u);
            else gamectl::RequestHint(who ? L"Player 2: aim at a dinosaur or building, then RB + D-pad Up" : L"Player 1: aim at a dinosaur or building, then RB + D-pad Up", 3.0f);
            return;
        }
        // JPOG63 (owner 13:40 "go back to the old method"): with [dino] a_takes_control = 0, A no longer takes a dino -
        // only RB + D-pad Up does (A still opens a building's window).
        const bool aPick = pad.Pressed(PAD_A) && (cf0.aTakesControl || (cf0.p2Buildings && cf0.p2Windows));
        if (!building && (aPick || (!cf0.rbUpInfo && pad.Down(PAD_RB) && pad.Pressed(PAD_DUP)))) {
            float t[3]{};
            bool okT = GUARDED("free player's aim point", {
                reinterpret_cast<void(__fastcall*)(void*, void*, float*)>(game::At(0x0050C770))(c, nullptr, t); });
            LOG("split: player %d (no HUD) pressed %s - pick a dino near (%.1f %.1f %.1f) ok=%d", who + 1,
                pad.Pressed(PAD_A) ? "A" : "RB + D-pad Up", t[0], t[1], t[2], okT);
            // JPOG59 (open list 2 / 6c): no dino at the aim -> the nearest building's own window in this player's half
            // (repair, entry fee, shop prices ... pressed with player 2's own pad cursor, open list 1).
            void* bld = nullptr;
            if (okT && cf0.p2Buildings && cf0.p2Windows && pad.Pressed(PAD_A) &&
                !dinocontrol::NearestUnitOf(t, cf0.pickRadius, 0x008B78C0))
                bld = dinocontrol::NearestUnitOf(t, cf0.pickRadius + 10.0f, 0x008B7A60);
            if (bld) {
                LOG("split: player %d (no HUD) A on building %p - its own window", who + 1, bld);
                p2menu::OpenUnitWindow(who, bld);
            } else if (okT && (!pad.Pressed(PAD_A) || cf0.aTakesControl)) dinocontrol::RequestControlAt(who, t);
            else if (okT) LOG("split: player %d A - no building there; dinos are taken with RB + D-pad Up ([dino] a_takes_control = 0)", who + 1);
        }
        auto F = [&](int slot, float v) { if (v > 0.0f) VSlot<fn_vf>(c, slot)(c, nullptr, v * dt); };
        auto Zero = [&](int off) { *reinterpret_cast<float*>((uint8_t*)c + off) = 0.0f; };
        const Config& cf = config::Get();
        float boost = (pad.Down(PAD_LS) || (!building && (pad.Down(PAD_RB) || pad.Down(PAD_LB)))) ? 2.5f : 1.0f;
        float fy = (pad.Down(PAD_DUP) ? 1.0f : 0.0f) - (pad.Down(PAD_DDOWN) ? 1.0f : 0.0f);
        float fx = building ? 0.0f : (pad.Down(PAD_DRIGHT) ? 1.0f : 0.0f) - (pad.Down(PAD_DLEFT) ? 1.0f : 0.0f);
        // JPOG33 (owner bug 5): the left stick pans too (analog), like the builder's stick.
        if (fy == 0.0f) fy = pad.LY();
        if (fx == 0.0f) fx = pad.LX();
        if (menu || p2menu::WindowOpen(who)) fx = fy = 0.0f;   // JPOG49: stick and D-pad point at the disc's pictures (JPOG59: or the window's buttons)
        if (fy > 1.0f) fy = 1.0f; if (fy < -1.0f) fy = -1.0f; if (fx > 1.0f) fx = 1.0f; if (fx < -1.0f) fx = -1.0f;
        if ((fx != 0.0f || fy != 0.0f) && g_panLogs < 3) { ++g_panLogs; LOG("split: player %d pans their own view (stick %.2f,%.2f)", who + 1, fx, fy); }
        fy *= cf.p2MoveSign * boost; fx *= boost;
        float rx = pad.RX() * cf.p2YawSign, zoomIn = pad.RY() * cf.p2PitchSign;
        bool ok = GUARDED("inactive camera update", {
            if (fy > 0) F(cam::PushFwd, fy); else if (fy < 0) F(cam::PushBack, -fy); else Zero(cam::VelFwd);
            if (fx > 0) F(cam::PushRight, fx); else if (fx < 0) F(cam::PushLeft, -fx); else Zero(cam::VelSide);
            if (rx > 0) F(cam::RotRight, rx); else if (rx < 0) F(cam::RotLeft, -rx); else Zero(cam::VelRot);
            Zero(cam::VelPitch);
            if (zoomIn != 0.0f) VSlot<fn_vf>(c, cam::ZoomOutF)(c, nullptr, -zoomIn * dt);
            VSlot<fn_bf>(c, cam::OnUpdate)(c, nullptr, dt);
            const void* q   = VSlot<const void*(__fastcall*)(void*, void*)>(c, cam::GetOrientation)(c, nullptr);
            const void* pos = VSlot<const void*(__fastcall*)(void*, void*)>(c, cam::GetPosition)(c, nullptr);
            float m[16];
            pMatIdentity(m, nullptr); pMatFromQuat(m, nullptr, q); pMatSetTranslate(m, nullptr, pos);
            memcpy(g_p2, m, sizeof(m));
        });
        if (!ok) { LOG("split: camera update faulted - back to P1 + simple free camera"); g_activePlayer = 0; g_cam2 = nullptr; }
    }

    // ---- cursor (TGUIScreen: width/height shorts at +0x70/+0x72, pointer at +0x114/+0x118) ----
    using fn_getpp = void(__fastcall*)(void* self, void* edx, int* x, int* y);
    using fn_setpp = bool(__fastcall*)(void* self, void* edx, int x, int y);
    fn_getpp pGetPointer = nullptr; fn_setpp pSetPointer = nullptr;
    void* GuiScreen() {
        void* sys = reinterpret_cast<void*(__stdcall*)()>(game::At(0x0067E3F0))();   // AGUISystem::GetGUISystem
        return sys ? Ptr(sys, 0x28) : nullptr;
    }
    void SaveCursor(int p) {
        void* sc = GuiScreen(); if (!sc || !pGetPointer) return;
        pGetPointer(sc, nullptr, &g_cur[p][0], &g_cur[p][1]); g_curInit[p] = true;
    }
    void RestoreCursor(int p) {
        void* sc = GuiScreen(); if (!sc || !pSetPointer) return;
        if (!g_curInit[p]) {   // first time: centre of that player's half
            int gw = *reinterpret_cast<short*>((uint8_t*)sc + 0x70), gh = *reinterpret_cast<short*>((uint8_t*)sc + 0x72);
            g_cur[p][0] = g_vertical ? (p ? gw * 3 / 4 : gw / 4) : gw / 2;
            g_cur[p][1] = g_vertical ? gh / 2 : (p ? gh * 3 / 4 : gh / 4);
            g_curInit[p] = true;
        }
        pSetPointer(sc, nullptr, g_cur[p][0], g_cur[p][1]);
    }

    using fn_setcam = void(__fastcall*)(void* self, void* edx, void* cam);
    void ApplyP1Rects();
    void ApplyActiveRects() { ApplyP1Rects(); }
    void SwitchActive(int p) {
        if (p == (int)g_activePlayer || !g_cam1 || !g_cam2 || !g_camMgr) return;
        int from = (int)g_activePlayer;
        builder2::Close(p, "takes the HUD"); p2menu::Close(p, "takes the HUD");      // JPOG46: the HUD player builds with the game's own menus
        bool ok = GUARDED("switch active player", {
            SaveCursor(from);
            reinterpret_cast<fn_setcam>(game::At(0x0050CD50))(g_camMgr, nullptr, p ? g_cam2 : g_cam1); // SetActiveCamera
            g_activePlayer = p;
            ApplyActiveRects();
            RestoreCursor(p);
        });
        g_switchFrame = g_frame;
        LOG("split: player %d now controls the cursor/camera (ok=%d)", p + 1, ok);
    }

    bool InEnvMapPass() { return *reinterpret_cast<volatile uint8_t*>(game::At(0x0084B080)) != 0; }

    // Draw `scene` twice: P1 rect (game camera), P2 rect (player-2 camera).
    // ---- Full-screen menus (Research, Dinopedia, Fossil hunt...) in split-screen ----
    // AFullScreenDialog::OnActivate/OnDeactivate call AGameController::SetParkVisible(false/true)
    // (0x4719D0) at 0x573EBE / 0x573F63; with the park hidden the renderer draws no world at all, so
    // the OTHER player's half went blank. In split-screen we keep the park visible, draw it only in
    // the other player's half, and lock the cursor hand-over until the menu closes.
    using fn_setParkVisible = void(__fastcall*)(void* gc, void* edx, bool vis);
    void __fastcall FS_SetParkVisible(void* gc, void* edx, bool vis) {
        auto orig = reinterpret_cast<fn_setParkVisible>(game::At(0x004719D0));
        // JPOG60 (fix C): player 2's own full-screen window (p2win, e.g. the Performance report) is drawn in player 2's
        // half only; its OnActivate / OnDeactivate hide / show the park for "the" player - ignore both, player 1 keeps
        // the park and the HUD (JPOG59: player 1's half went blank).
        if (g_active && config::Get().p2FsKeepPark && p2menu::InOwnWindowCall()) {
            LOG("split: player 2's own window asked to %s the park - ignored (player 1 keeps playing)", vis ? "show" : "hide");
            return;
        }
        if (!vis && g_active) {
            g_menuOpen = 1;
            LOG("split: full-screen menu opened by player %d - other half keeps the park", (int)g_activePlayer + 1);
            return;                                   // park stays visible
        }
        if (vis && g_menuOpen) { g_menuOpen = 0; LOG("split: full-screen menu closed"); }
        orig(gc, edx, vis);
    }
    bool PatchCall(uintptr_t site, uintptr_t expectTarget, void* fn) {
        uint8_t* p = reinterpret_cast<uint8_t*>(game::At(site));
        if (p[0] != 0xE8) return false;
        uintptr_t tgt = (uintptr_t)p + 5 + *reinterpret_cast<int32_t*>(p + 1);
        if (tgt != (uintptr_t)game::At(expectTarget)) return false;
        DWORD old;
        if (!VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
        *reinterpret_cast<int32_t*>(p + 1) = (int32_t)((uintptr_t)fn - ((uintptr_t)p + 5));
        VirtualProtect(p + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
        return true;
    }

    // JPOG35 diagnostic (owner bug 2, hover highlights): the projection the engine keeps after each half's pass -
    // main viewport render context (*(viewport+0x34)): +0x30/+0x34 centre, +0x38/+0x3C scale. Mouse picking
    // (APointerController::RequestTargetPosition -> ARenderer::ConvertScreenCoordsToWorldCoords 0x478250) and the
    // icon projection (0x477EC0) use whatever the LAST pass left there. Logged a few times per split session.
    int g_projLogs = 0, g_projRestoreLogs = 0;
    void LogProj(bool isMain, int who, float x, float y, float w, float h) {
        if (!isMain || g_projLogs >= 4) return;
        void* vp = Ptr(g_renderer, 0x2C);
        uint8_t* rc = vp ? (uint8_t*)Ptr(vp, 0x34) : nullptr;
        if (!rc) return;
        ++g_projLogs;
        const float* f = reinterpret_cast<const float*>(rc + 0x30);
        LOG("split: proj after player %d's pass: rect %.0f,%.0f %.0fx%.0f centre %.1f,%.1f scale %.1f,%.1f (order: %s)", who + 1,
            x, y, w, h, f[0], f[1], f[2], f[3], config::Get().renderActiveLast ? "active last" : "active first");
    }
    // JPOG52 (owner 10:44 + backlog 6: "the highlight is way off for player 1 from where he is actually clicking").
    // The game's pointer ray (APointerController::RequestTargetPosition 0x51D800) converts the GUI pointer (centre-locked
    // in the HUD player's half) with ACamera::ConvertGuiScreenToRenderScreen + ARenderer::ConvertScreenCoordsToWorldCoords,
    // which assume the full screen -> in split the ray (hover highlight, terrain aim, the unit A selects) is off from the
    // cursor. Player 2's camera-target aim was accurate. So in split with the cursor centre-locked, its SetTargetRay call
    // (0x51D882) gets the ray through the middle of the HUD player's view instead: ACamera::GetPosition 0x50B0D0 ->
    // ACamera::GetTargetPosition 0x50C770 of the game's active camera (the HUD player's).
    using fn_setray = void(__fastcall*)(void* tgt, void* edx, const float* origin, const float* dir);
    unsigned g_rayLogs = 0;
    void __fastcall Hook_PointerRay(void* tgt, void* edx, const float* origin, const float* dir) {
        const bool centre = (input_map::ConsoleActive() && !gamectl::CursorFallback()) || gamectl::DemolishActive();
        if (g_active && centre && config::Get().hudCentreRay && gamectl::WorldAimActive()) {
            float o[3], tp[3], d[3]; bool ok = false;
            GUARDED("hud centre ray", {
                void* mgr = reinterpret_cast<void*(__stdcall*)()>(game::At(cam::GetCameraManager))();
                void* cm = mgr ? reinterpret_cast<void*(__fastcall*)(void*, void*)>(game::At(cam::GetActiveCamera))(mgr, nullptr) : nullptr;
                if (cm) {
                    const float* p = reinterpret_cast<const float*(__fastcall*)(void*, void*)>(game::At(0x0050B0D0))(cm, nullptr);
                    reinterpret_cast<void(__fastcall*)(void*, void*, float*)>(game::At(0x0050C770))(cm, nullptr, tp);
                    if (p) { o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
                        d[0] = tp[0] - o[0]; d[1] = tp[1] - o[1]; d[2] = tp[2] - o[2];
                        const float l = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                        if (l > 0.01f && std::isfinite(l)) { d[0] /= l; d[1] /= l; d[2] /= l; ok = true; } }
                }
            });
            if (ok) {
                if (g_rayLogs < 3) { ++g_rayLogs;
                    LOG("split: HUD player's pick ray through the middle of their view: from (%.1f %.1f %.1f) dir (%.2f %.2f %.2f); the pointer ray was dir (%.2f %.2f %.2f)",
                        o[0], o[1], o[2], d[0], d[1], d[2], dir ? dir[0] : 0.f, dir ? dir[1] : 0.f, dir ? dir[2] : 0.f); }
                reinterpret_cast<fn_setray>(game::At(0x00520DC0))(tgt, edx, o, d);
                return;
            }
        }
        reinterpret_cast<fn_setray>(game::At(0x00520DC0))(tgt, edx, origin, dir);
    }
    // JPOG52 (backlog 8, owner 10:38): the floating "+money" pop-ups from visitors buying things, OFF in split-screen only.
    // APlayerInfo::AddMoneyChange 0x5D6180 (thiscall(pos, amount, stack), ret 0xC) only shows the float
    // (AFloatHudElement::AddTimedFloat) - the money itself is booked elsewhere. Single player: unchanged.
    using fn_addmoney = void(__fastcall*)(void* pi, void* edx, const float* pos, int amount, bool stack);
    fn_addmoney g_origAddMoney = nullptr; unsigned g_moneyHidden = 0;
    void __fastcall Hook_AddMoneyChange(void* pi, void* edx, const float* pos, int amount, bool stack) {
        // JPOG56 (owner 11:51 "just disable all of them"): [coop] money_popups = 0 hides them in single player too.
        if ((g_active && !config::Get().splitMoneyPopups) || !config::Get().moneyPopups) {
            if (g_moneyHidden++ == 0) LOG("split: floating money pop-ups hidden (split %d, money_popups %d) (%d)", (int)g_active, (int)config::Get().moneyPopups, amount);
            return;
        }
        g_origAddMoney(pi, edx, pos, amount, stack);
    }
    // JPOG52 (owner 10:52: "rest area / security building invisible or visible depending on angle and height - player 2
    // sees them properly"; backlog 10/11 vents + dome). TRenderContext (+0x08 flags) CACHES what it derives from the view
    // matrix / projection: 0x08 = view->world inverse (+0x10C), 0x40 = world-space frustum planes (+0x1D4, GetWorldPlanes
    // = the planes the culling uses), bit 0x01 = dirty (view planes +0x174 recomputed). SetWorldViewMatrix clears 0xEC and
    // SetProjection/ViewportParameters set dirty - our memcpy restore of the HUD half's values did neither, so the HUD
    // half (drawn FIRST) could cull with the planes left over from the other half's pass. Invalidate the caches exactly
    // like those setters do, after the restore and before each half's pass.
    void InvalidateRc(uint8_t* rc) {
        if (!rc) return;
        uint32_t& f = *reinterpret_cast<uint32_t*>(rc + 0x08);
        f = (f & 0xFFFFFF03u) | 0x101u;
    }
    bool __fastcall Hook_Render(void* scene, void* edx) {
        // JPOG39: single screen - the aim circle for player 1 in the main scene pass (not the water reflection pass).
        if (!g_active && g_renderer && scene == g_mainScene && !InEnvMapPass()) {
            QueueCircle(0, true);
            bool r = pRender(scene, edx);
            CheckCircleConsumed();
            return r;
        }
        if (!g_active || !g_renderer || InEnvMapPass())
            return pRender(scene, edx);
        void* sky = Ptr(g_renderer, 0x30);
        bool isMain = (scene == g_mainScene), isSky = (scene == sky);
        if (!isMain && !isSky) return pRender(scene, edx);

        void* vp = Ptr(g_renderer, isMain ? 0x2C : 0x34);
        void* tr = MainCamTransform(scene);
        if (!vp || !tr) return pRender(scene, edx);

        float m1[16];
        memcpy(m1, pGetMatrix(tr, nullptr), sizeof(m1));

        const int act = (int)g_activePlayer, other = 1 - act;
        const bool menu = g_menuOpen != 0;          // act's half shows the full-screen menu
        float x, y, w, h;
        bool r = true;
        const bool actChase = g_actChaseOn && !menu;
        // JPOG34: the OTHER half first, the ACTIVE half last - so what the engine keeps from the last render pass
        // (the render context's view matrix, used by ConvertToScreenCoords for the unit icons / thought bubbles)
        // belongs to the active player, whose half the HUD is drawn in.
        void* camObj = Ptr(scene, 0x568);
        float fov1 = 0.0f, fov2 = 0.0f;
        auto setFov = *reinterpret_cast<fn_setfov*>(game::At(kIatSetFOV));
        auto getFov = *reinterpret_cast<fn_getfov*>(game::At(kIatGetFOV));
        if (camObj && setFov && getFov) {
            fov1 = getFov(camObj, nullptr);
            fov2 = CamFov(other ? g_cam2 : g_cam1, fov1);
            if (isMain && g_fovLogs < 2 && fabsf(fov2 - fov1) > 0.01f) { ++g_fovLogs; LOG("split: player %d's half uses its own FOV %.3f (active half %.3f)", other + 1, fov2, fov1); }
        }
        // JPOG35: [coop] render_active_last (default 1 = JPOG34 order). 0 = JPOG33 order (active half first) - a TEST
        // switch for the owner's "hover highlights in the wrong place" bug (no build needed to compare).
        float x2 = 0, y2 = 0, w2 = 0, h2 = 0;
        int passes = 0;
        if (isMain) cullfix::BeginFrame();   // JPOG37 (owner bugs 4 + 3): remember units inside ANY main pass
        auto drawOther = [&]() {
            if (camObj && setFov && fov2 != fov1) setFov(camObj, nullptr, fov2);
            float m2[16];
            memcpy(m2, g_p2, sizeof(m2));
            if (isSky) { m2[12] = m1[12]; m2[13] = m1[13]; m2[14] = m1[14]; }  // sky keeps its own origin
            pSetMatrix(tr, nullptr, m2);
            RectFor(other, x, y, w, h); SetRect(vp, x, y, w, h);
            if (passes++) { pEnd(scene, nullptr); pBegin(scene, nullptr); }
            if (isMain && config::Get().rcInvalidate) InvalidateRc((uint8_t*)Ptr(vp, 0x34));   // JPOG52
            if (isMain) cullfix::SetPass(other, Ptr(vp, 0x34));   // JPOG54: this pass's planes + the other half's test
            if (isMain) { builder2::QueueFootprint(other); QueueCircle(other, false); }   // JPOG39/46: queued right before this half's own pass
            r = pRender(scene, edx);
            if (isMain) { builder2::AfterPass(other); CheckCircleConsumed(); }
            LogProj(isMain, other, x, y, w, h);
            if (isMain) {   // JPOG40: the other half's projection, for its unit icon pass (Hook_RenderIcons)
                uint8_t* orc = (uint8_t*)Ptr(vp, 0x34);
                if (orc) {
                    memcpy(g_otherRcA, orc + 0x30, sizeof g_otherRcA); memcpy(g_otherRcB, orc + 0x8C, sizeof g_otherRcB);
                    float ax, ay, aw, ah; RectFor(act, ax, ay, aw, ah);
                    g_otherShift[0] = x - ax; g_otherShift[1] = y - ay; g_otherRcFrame = g_frame;
                }
            }
            x2 = x; y2 = y; w2 = w; h2 = h;
            if (camObj && setFov && fov2 != fov1) setFov(camObj, nullptr, fov1);
        };
        auto drawActive = [&]() {
            if (actChase) {
                float ma[16]; memcpy(ma, g_actChase, sizeof(ma));
                if (isSky) { ma[12] = m1[12]; ma[13] = m1[13]; ma[14] = m1[14]; }   // sky keeps its own origin
                pSetMatrix(tr, nullptr, ma);
                if (g_actChaseLogs < 2 && isMain) { ++g_actChaseLogs; LOG("split: player %d's half drawn with their dino chase camera (both ride dinos)", act + 1); }
            } else pSetMatrix(tr, nullptr, m1);
            RectFor(act, x, y, w, h); SetRect(vp, x, y, w, h);
            // During a menu the menu player's half still gets the SKY pass: it clears that half every frame
            // (otherwise the last park frame + HUD stay frozen around the menu panel).
            if (!menu || isSky) {
                if (passes++) { pEnd(scene, nullptr); pBegin(scene, nullptr); }
                if (isMain && config::Get().rcInvalidate) InvalidateRc((uint8_t*)Ptr(vp, 0x34));   // JPOG52
                if (isMain) cullfix::SetPass(act, Ptr(vp, 0x34));   // JPOG54
                if (isMain && !menu) QueueCircle(act, true);
                r = pRender(scene, edx);
                if (isMain && !menu) CheckCircleConsumed();
                LogProj(isMain, act, x, y, w, h);
                if (isMain && !menu) {   // JPOG62 item D: the HUD half's own projection, for its icon pass
                    uint8_t* hrc = (uint8_t*)Ptr(vp, 0x34);
                    if (hrc) { memcpy(g_hudRcA, hrc + 0x30, sizeof g_hudRcA); memcpy(g_hudRcB, hrc + 0x8C, sizeof g_hudRcB); g_hudRcFrame = g_frame; }
                }
            }
        };
        if (config::Get().renderActiveLast) { drawOther(); drawActive(); }
        else {
            // JPOG36 (owner item 5, default): the HUD half FIRST. Transient world highlights (building placement
            // footprint, hover highlight) live for ONE scene pass - drawn first = drawn for the HUD player, who is the
            // one placing / pointing. Then the other half, then the projection the engine keeps from the last pass
            // (render context *(MainViewport+0x34): +0x30 centre, +0x38 scale, +0x40 near, +0x8C view matrix - read by
            // ARenderer::ConvertToScreenCoords 0x477EC0 and picking 0x478250) is put back to the HUD half's values,
            // so icons / bubbles / mouse picking still use the HUD player's view (why JPOG34 drew it last).
            uint8_t* rc = isMain ? (uint8_t*)Ptr(vp, 0x34) : nullptr;
            uint8_t saveA[0x14], saveB[0x40];
            drawActive();
            const bool snap = rc && !menu;
            if (snap) { memcpy(saveA, rc + 0x30, sizeof saveA); memcpy(saveB, rc + 0x8C, sizeof saveB); }
            drawOther();
            if (snap) {
                memcpy(rc + 0x30, saveA, sizeof saveA); memcpy(rc + 0x8C, saveB, sizeof saveB);
                if (config::Get().rcInvalidate) InvalidateRc(rc);   // JPOG52: derived matrices / planes recomputed from these
                if (g_projRestoreLogs < 2) { ++g_projRestoreLogs; LOG("split: HUD half drawn first, its projection restored after player %d's pass (centre %.1f,%.1f)", other + 1,
                    *reinterpret_cast<float*>(rc + 0x30), *reinterpret_cast<float*>(rc + 0x34)); }
            }
            RectFor(act, x, y, w, h); SetRect(vp, x, y, w, h);   // leave the viewport on the HUD half (mouse picking)
        }
        pSetMatrix(tr, nullptr, m1);
        if (isMain) cullfix::EndFrame();     // ... and keep them full-rate for the simulation
        if (g_renderLogs < 2 && isMain) { ++g_renderLogs; LOG("split: drew main scene twice (P2 rect %.0f,%.0f %.0fx%.0f)", x2, y2, w2, h2); }
        return r;
    }

    bool PatchIAT(void* oldFn, void* newFn) {
        auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
            if (_stricmp(reinterpret_cast<char*>(base + d->Name), "TRenderInterface.dll")) continue;
            for (auto* t = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk); t->u1.Function; ++t) {
                if ((void*)t->u1.Function != oldFn) continue;
                DWORD old;
                VirtualProtect(&t->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
                g_iatSlot = reinterpret_cast<void**>(&t->u1.Function);
                InterlockedExchangePointer(g_iatSlot, newFn);
                VirtualProtect(&t->u1.Function, sizeof(void*), old, &old);
                return true;
            }
        }
        return false;
    }

    bool KeyEdge(int vk, bool& prev, bool focused) {
        bool d = vk && focused && (GetAsyncKeyState(vk) & 0x8000);
        bool e = d && !prev; prev = d; return e;
    }
    bool GameFocused() {
        HWND fg = GetForegroundWindow(); DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }

    void SaveOrientation() {
        wchar_t ini[MAX_PATH]{};
        GetModuleFileNameW(nullptr, ini, MAX_PATH);
        if (wchar_t* sl = wcsrchr(ini, L'\\')) *(sl + 1) = 0;
        wcscat_s(ini, L"jpogmod.ini");
        WritePrivateProfileStringW(L"coop", L"split_orientation", g_vertical ? L"vertical" : L"horizontal", ini);
    }

    void SetOrientation(bool vertical, const char* why) {
        g_vertical = vertical;
        SaveOrientation();
        LOG("split: orientation -> %s (%s)", g_vertical ? "vertical" : "horizontal", why);
        if (g_active) ApplyP1Rects();
        if (g_splitWidget && pSetToggled && pIsToggled && pIsToggled(g_splitWidget, nullptr) != g_vertical)
            pSetToggled(g_splitWidget, nullptr, g_vertical ? 1 : 0);
    }

    void Activate() {
        void* tr = g_mainScene ? MainCamTransform(g_mainScene) : nullptr;
        void* vp = Ptr(g_renderer, 0x2C);
        if (!tr || !vp) return;
        g_W = pGetW(vp, nullptr); g_H = pGetH(vp, nullptr);
        if (g_W < 64 || g_H < 64) return;
        memcpy(g_p2, pGetMatrix(tr, nullptr), sizeof(g_p2));
        LogMatrix("P1 camera at split start", g_p2);
        // Start P2 a little to the side of P1 so the two views are distinguishable.
        const Config& c = config::Get();
        for (int i = 0; i < 3; ++i) g_p2[12 + i] += g_p2[i] * c.p2StartOffset;
        g_activePlayer = 0; g_requestPlayer = -1; g_curInit[0] = g_curInit[1] = false; g_projLogs = 0;
        if (c.p2GameCamera && CreateP2Camera()) {
            g_cam1 = GameActiveCamera();
            UpdateInactiveCamera(1.0f / 60.0f);
            LogMatrix("P2 game camera (built from quaternion+position)", g_p2);
        }
        g_active = true;
        // A full-screen menu that was already open when split-screen started (e.g. the 5-star
        // "Congratulations" popup right after a park loads) hid the park: show it again and treat
        // the menu as player 1's, so player 2's half has the park.
        if (void* root = *reinterpret_cast<void**>(game::At(0x008B859C))) {          // AApplication(0x8B8580)+0x1C
            void* gc = Ptr(root, 0x2C);                                              // AGameController
            if (gc && !*reinterpret_cast<volatile uint8_t*>((uint8_t*)gc + 0xD8)) {   // IsParkVisible() == false
                reinterpret_cast<fn_setParkVisible>(game::At(0x004719D0))(gc, nullptr, true);
                g_menuOpen = 1;
                LOG("split: a full-screen menu was already open - park shown in player 2's half");
            }
        }
        ApplyP1Rects();
        LOG("split: ON (%s, screen %.0fx%.0f, pad2=%d forced=%d)", g_vertical ? "vertical" : "horizontal",
            g_W, g_H, g_pad2.Connected(), g_forced);
    }
    // JPOG36 (owner item 6): after split-screen the front end draws only its 4:3 area; the old island frame stayed
    // visible at the sides (owner photo, JPOG35). D3D8 Clear(0, NULL) clears the current VIEWPORT, so: full-size
    // viewport, clear the target to black, restore - for a few frames (every back buffer of the swap chain).
    volatile LONG g_clearFrames = 0; int g_clearLogs = 0;
    struct D3DVP8 { DWORD X, Y, Width, Height; float MinZ, MaxZ; };
    void ClearFullBackBuffer() {
        if (!pInterfaceVar || !*pInterfaceVar || g_W < 16 || g_H < 16) return;
        static void* (__fastcall* getDev)(void*, void*) = nullptr;
        if (!getDev) {
            HMODULE rd = GetModuleHandleA("TRenderD3DInterface.dll");
            if (rd) getDev = reinterpret_cast<void*(__fastcall*)(void*, void*)>(GetProcAddress(rd, "?GetD3DDevice@TRenderD3DInterface@Toshi@@QAEPAUIDirect3DDevice8@@XZ"));
            if (!getDev) { g_clearFrames = 0; LOG("split: GetD3DDevice export not found - no back-buffer clear"); return; }
        }
        bool ok = GUARDED("clear full back buffer", {
            void* dev = getDev(*pInterfaceVar, nullptr);
            if (dev) {
                void** vt = *reinterpret_cast<void***>(dev);
                using fn_clear = long(__stdcall*)(void*, DWORD, const void*, DWORD, DWORD, float, DWORD);
                using fn_vp    = long(__stdcall*)(void*, D3DVP8*);
                D3DVP8 old{}, full{ 0, 0, (DWORD)g_W, (DWORD)g_H, 0.0f, 1.0f };
                reinterpret_cast<fn_vp>(vt[41])(dev, &old);                       // GetViewport
                reinterpret_cast<fn_vp>(vt[40])(dev, &full);                      // SetViewport
                long hr = reinterpret_cast<fn_clear>(vt[36])(dev, 0, nullptr, 1 /*D3DCLEAR_TARGET*/, 0xFF000000u, 1.0f, 0);
                reinterpret_cast<fn_vp>(vt[40])(dev, &old);
                if (g_clearLogs < 2) { ++g_clearLogs; LOG("split: cleared the whole back buffer %.0fx%.0f after split-screen (hr 0x%08lX, viewport was %lu,%lu %lux%lu)", g_W, g_H, (unsigned long)hr, old.X, old.Y, old.Width, old.Height); }
            }
        });
        if (!ok) g_clearFrames = 0;
    }

    void Deactivate(const char* why) {
        dinocontrol::ForceRelease("split-screen ended");
        builder2::CloseAll(why, !WorldReady()); p2menu::CloseAll(why);   // JPOG46: never touch the terrain decals once the world is gone
        if (!g_active) return;
        // JPOG57 (owner log 12:06:03, CRASH SimJP+0x128465 AWorldCamera::ClipFocalPoint): quitting while PLAYER 2 had the
        // HUD - SwitchActive(0) re-activated player 1's world camera (SetActiveCamera) after the park terrain was gone,
        // and its task then clipped against the missing terrain. World gone = only the bookkeeping, no SetActiveCamera.
        if (g_activePlayer != 0) {
            if (WorldReady()) SwitchActive(0);
            else { g_activePlayer = 0; LOG("split: park world gone - HUD back to player 1 without re-activating their camera (JPOG57)"); }
        }
        g_active = false;
        g_actChaseOn = false;
        RestoreFull();
        DeleteP2Camera();
        // JPOG34: player 1's camera is not the game's any more (kept alive across a type switch) -> delete it.
        if (g_cam1 && g_camMgr && g_cam1 != GameActiveCamera() && g_cam1 != DefaultCamera()) {
            void* c1 = g_cam1;
            bool okd = GUARDED("delete P1's kept camera", {
                reinterpret_cast<fn_vp>(game::At(cam::DeleteCamera))(g_camMgr, nullptr, c1); });
            LOG("split: player 1's kept camera %p deleted (ok=%d)", c1, okd);
        }
        g_cam1 = nullptr;
        InterlockedExchange(&g_clearFrames, 12);   // JPOG36: black out the old frame around the 4:3 front end
        g_clearLogs = 0;
        LOG("split: OFF (%s)", why);
    }
    // ---- JPOG58 (owner log 12:19:53, CRASH TSpriteShaderD3D+0x2A78 = TSpriteShaderHAL::Render, mesh material null) ----
    // Quitting the park runs ARootTask::UnloadGameController 0x47B550 in ONE call: DestroyAllUnits, SetDefaultCamera
    // (our delete guard kept player 2's camera alive), StopAllParticles/DestroyModels, unload the material libraries.
    // Split-screen only noticed it frames later (WorldReady) and kept drawing a second pass with the kept camera ->
    // a sprite whose material was already unloaded. Same family as the JPOG56 quit crash. Fix: end split-screen at the
    // START of the unload, while the world, cameras and materials are all still alive, and stay off until the next park.
    using fn_unloadGame = void(__fastcall*)(void* self, void* edx);
    fn_unloadGame g_origUnloadGame = nullptr;
    void* g_unloadBlockPark = nullptr; bool g_unloadBlock = false;
    void __fastcall Hook_UnloadGame(void* self, void* edx) {
        GUARDED("ridehud unload", { ridehud::OnUnload(); });   // JPOG62
        if (config::Get().unloadSplitOff) {
            void* park = *reinterpret_cast<void**>(game::At(game::addr::AParkManager_m_pParkInfo));
            g_unloadBlock = true; g_unloadBlockPark = park;
            if (g_active) {
                LOG("split: park unload starting (ARootTask::UnloadGameController) - ending split-screen first (JPOG58, [coop] unload_split_off)");
                GUARDED("split off before unload", { Deactivate("park unload (quit / load)"); });
            }
        }
        g_origUnloadGame(self, edx);
    }


    void UpdateP2Camera(float dt) {
        if (dt <= 0 || dt > 0.25f) dt = 1.0f / 60.0f;
        const Config& c = config::Get();
        float* r0 = Row(g_p2, 0); float* r1 = Row(g_p2, 1); float* r2 = Row(g_p2, 2); float* p = Row(g_p2, 3);
        const float worldY[3] = {0, 1, 0};

        float yaw = g_pad2.RX() * c.p2TurnRate * dt * (3.14159265f / 180.0f) * c.p2YawSign;
        if (yaw != 0) { Rot(r0, worldY, yaw); Rot(r1, worldY, yaw); Rot(r2, worldY, yaw); }

        float pitch = g_pad2.RY() * c.p2TurnRate * 0.6f * dt * (3.14159265f / 180.0f) * c.p2PitchSign;
        if (pitch != 0) {
            float u[3] = {r1[0], r1[1], r1[2]}, f[3] = {r2[0], r2[1], r2[2]}, ax[3] = {r0[0], r0[1], r0[2]};
            Norm(ax); Rot(u, ax, pitch); Rot(f, ax, pitch);
            if (fabsf(f[1]) < 0.97f) { memcpy(r1, u, 12); memcpy(r2, f, 12); }
        }
        Orthonormalize(g_p2);

        float speed = c.p2MoveSpeed * (g_pad2.Down(PAD_LS) || g_pad2.Down(PAD_RB) ? 3.0f : 1.0f);
        float fw[3] = {r2[0], 0, r2[2]}; Norm(fw);
        float rt[3] = {r0[0], 0, r0[2]}; Norm(rt);
        float mv = g_pad2.LY() * c.p2MoveSign, sd = g_pad2.LX();
        for (int i = 0; i < 3; ++i) p[i] += (fw[i] * mv + rt[i] * sd) * speed * dt;
        // Triggers: RT up, LT down (engine Y axis points down, so "up" = -Y).
        p[1] += (g_pad2.LT() - g_pad2.RT()) * speed * 0.6f * dt * c.p2UpSign;
    }
} // namespace

namespace splitscreen {

bool Active() { return g_active; }
int  ActivePlayer() { return g_active ? (int)g_activePlayer : 0; }
bool MenuLocked() { return g_active && g_menuOpen != 0; }
bool CanSwitch() { return g_active && g_cam2 != nullptr; }
void RequestActive(int p) { if (g_active) InterlockedExchange(&g_requestPlayer, p); }
void* CameraOfPlayer(int p) { if (!g_active || !g_cam2) return nullptr; return p ? g_cam2 : g_cam1; }
int   PlayerOfCamera(void* cam) { return (g_active && cam && cam == g_cam2) ? 1 : 0; }

bool ViewMatrix(float* out) {
    if (!g_mainScene || !pGetMatrix) return false;
    void* camObj = Ptr(g_mainScene, 0x568);
    if (!camObj) return false;
    void* tr = pCamGetTransform(camObj, nullptr);
    if (!tr) return false;
    memcpy(out, pGetMatrix(tr, nullptr), 16 * sizeof(float));
    return true;
}

// JPOG35 (owner 14:25 item 5): hold RT + left stick in the park view = pan the HUD player's camera directly
// (the camera's own Push* vtable slots, like UpdateInactiveCamera; the engine runs the active camera's OnUpdate).
namespace { volatile LONG g_rtPanOn = 0; volatile float g_rtPanX = 0, g_rtPanY = 0; int g_rtPanLogs = 0; }
void SetRtPan(bool on, float x, float y) { g_rtPanX = x; g_rtPanY = y; InterlockedExchange(&g_rtPanOn, on ? 1 : 0); }
static void TickRtPan(float dt) {
    if (!g_rtPanOn || !g_camMgr || !gamectl::ParkViewActive()) return;
    if (dt <= 0 || dt > 0.25f) dt = 1.0f / 60.0f;
    void* c = GameActiveCamera();
    if (!c || !IsExactlyWorldCamera(c)) return;          // never push a unit / follow camera
    const float k = config::Get().rtPanSpeed;
    float fy = g_rtPanY, fx = g_rtPanX;
    if (fy > 1) fy = 1; if (fy < -1) fy = -1; if (fx > 1) fx = 1; if (fx < -1) fx = -1;
    fy *= k; fx *= k;
    auto F = [&](int slot, float v) { if (v > 0.0f) VSlot<fn_vf>(c, slot)(c, nullptr, v * dt); };
    bool ok = GUARDED("RT camera pan", {
        if (fy > 0) F(cam::PushFwd, fy); else if (fy < 0) F(cam::PushBack, -fy);
        if (fx > 0) F(cam::PushRight, fx); else if (fx < 0) F(cam::PushLeft, -fx);
    });
    if (g_rtPanLogs < 3) { ++g_rtPanLogs; LOG("pan: RT + stick pans camera %p (stick %.2f,%.2f x%.1f) ok=%d", c, g_rtPanX, g_rtPanY, k, ok); }
}


// JPOG49: GUI units from the HUD player's half to player p's half. The HUD plugin (JPOGMouseFix GetHudMap) draws the
// GUI (gw x gh) uniformly scaled by s = min(rw/gw, rh/gh) * HUDScaleSplit (clamped 1..2 and so the HUD keeps at least half
// the GUI width), centred in the HUD player's rectangle - so one GUI unit = s pixels everywhere on screen.
bool GuiOffsetTo(int p, int& dx, int& dy) {
    dx = dy = 0;
    if (!g_active || g_W <= 0) return false;
    void* sc = GuiScreen();
    if (!sc) return false;
    const float gw = *reinterpret_cast<short*>((uint8_t*)sc + 0x70), gh = *reinterpret_cast<short*>((uint8_t*)sc + 0x72);
    if (gw < 16 || gh < 16) return false;
    static float s_k = -1.0f;
    if (s_k < 0.0f) {
        char path[MAX_PATH], buf[32]; GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* sl = strrchr(path, '\\'); if (sl) sl[1] = 0;
        strcat_s(path, "JPOGMouseFix.ini");
        GetPrivateProfileStringA("Widescreen", "HUDScaleSplit", "1.0", buf, sizeof(buf), path);
        s_k = (float)atof(buf);
        LOG("split: HUD scale in split-screen = %.2f (JPOGMouseFix.ini)", s_k);
    }
    float ox, oy, ow, oh, ax, ay, aw, ah;
    RectFor(p, ox, oy, ow, oh); RectFor((int)g_activePlayer, ax, ay, aw, ah);
    float s = aw / gw; if (ah / gh < s) s = ah / gh;
    float k = s_k; if (k < 1.0f) k = 1.0f; if (k > 2.0f) k = 2.0f;
    if (k > 1.0f) { float kmax = aw / (s * gw * 0.5f); if (k > kmax) k = kmax; }
    s *= k;
    if (s <= 0.01f) return false;
    dx = (int)lroundf((ox - ax) / s); dy = (int)lroundf((oy - ay) / s);
    return true;
}

bool QueueGroundCircle(const float* pos, float size, const float* col) {
    if (!pos || !col) return false;
    bool q = false;
    if (!GUARDED("ground circle", { q = QueueFootprint(pos, size, col); })) return false;
    return q;
}
void HintToHalf(int p, float seconds) {
    if (p < 0 || p > 1) return;
    g_hintUntil = GetTickCount() + (DWORD)(seconds * 1000.0f);
    InterlockedExchange(&g_hintHalf, p);
}

void AdoptActiveCamera() {
    if (!g_active || !g_camMgr) return;
    void* cur = GameActiveCamera();
    if (!cur) return;
    if (g_activePlayer != 0) {
        if (cur == g_cam2) return;
        void* old = g_cam2;
        g_cam2 = cur;
        void* defCam = *reinterpret_cast<void**>(game::At(0x0084D8A0));   // ACameraManager::sm_poDefaultCamera
        if (old && old != g_cam1 && old != defCam)
            GUARDED("delete old P2 camera", {
                reinterpret_cast<fn_vp>(game::At(cam::DeleteCamera))(g_camMgr, nullptr, old); });
        LOG("split: player 2 now uses camera %p (old %p)", cur, old);
    } else {
        if (cur == g_cam1) return;
        void* old = g_cam1;
        g_cam1 = cur;
        // JPOG34: delete the camera the game's type switch wanted to delete (we only kept it alive for the switch),
        // like player 2's - before, every select / deselect leaked one camera.
        void* defCam = *reinterpret_cast<void**>(game::At(0x0084D8A0));
        if (old && old != g_cam2 && old != defCam)
            GUARDED("delete old P1 camera", {
                reinterpret_cast<fn_vp>(game::At(cam::DeleteCamera))(g_camMgr, nullptr, old); });
        LOG("split: player 1 now uses camera %p (was %p)", cur, old);
    }
}

bool Install() {
    HMODULE ri = GetModuleHandleA("TRenderInterface.dll");
    if (!ri) { LOG("split: TRenderInterface.dll not loaded - split-screen off"); return false; }
    pRender  = Exp<fn_bool0>(ri, "?Render@TScene@Toshi@@QAE_NXZ");
    pBegin   = Exp<fn_bool0>(ri, "?Begin@TScene@Toshi@@QAE_NXZ");
    pEnd     = Exp<fn_bool0>(ri, "?End@TScene@Toshi@@QAE_NXZ");
    pSetX    = Exp<fn_setf>(ri, "?SetX@TViewport@Toshi@@QAEMM@Z");
    pSetY    = Exp<fn_setf>(ri, "?SetY@TViewport@Toshi@@QAEMM@Z");
    pSetW    = Exp<fn_setf>(ri, "?SetWidth@TViewport@Toshi@@QAEMM@Z");
    pSetH    = Exp<fn_setf>(ri, "?SetHeight@TViewport@Toshi@@QAEMM@Z");
    pGetW    = Exp<fn_getf>(ri, "?GetWidth@TViewport@Toshi@@QAEMXZ");
    pGetH    = Exp<fn_getf>(ri, "?GetHeight@TViewport@Toshi@@QAEMXZ");
    pCamGetTransform = Exp<fn_ptr0>(ri, "?GetTransformObject@TCameraObject@Toshi@@QAEPAVTTransformObject@2@XZ");
    pGetMatrix = Exp<fn_ptr0>(ri, "?GetMatrix@TTransformObject@Toshi@@QAEABVTMatrix44@2@XZ");
    pSetMatrix = Exp<fn_setmat>(ri, "?SetMatrix@TTransformObject@Toshi@@QAEXABVTMatrix44@2@@Z");
    pGetSystemResource = Exp<fn_ptrI>(ri, "?GetSystemResource@TRenderInterface@Toshi@@QAEPAVTResource@2@W4SYSRESOURCES@12@@Z");
    pInterfaceVar = Exp<void**>(ri, "?s_Interface@TRenderInterface@Toshi@@0PAV12@A");
    if (!pRender || !pBegin || !pEnd || !pSetX || !pSetY || !pSetW || !pSetH || !pGetW || !pGetH ||
        !pCamGetTransform || !pGetMatrix || !pSetMatrix || !pGetSystemResource || !pInterfaceVar) return false;
    if (!PatchIAT((void*)pRender, (void*)&Hook_Render)) {
        LOG("split: TScene::Render import not found in SimJP.exe - split-screen off");
        return false;
    }
    for (const char* dll : {"TKernelInterface.dll", "TRenderInterface.dll", "TMathInterface.dll"}) {
        HMODULE m = GetModuleHandleA(dll); if (!m) continue;
        if (!pMatIdentity)     pMatIdentity     = (fn_mat0)GetProcAddress(m, "?Identity@TMatrix44@Toshi@@QAEXXZ");
        if (!pMatFromQuat)     pMatFromQuat     = (fn_matQ)GetProcAddress(m, "?SetFromQuaternion@TMatrix44@Toshi@@QAEAAV12@ABVTQuaternion@2@@Z");
        if (!pMatSetTranslate) pMatSetTranslate = (fn_matV)GetProcAddress(m, "?SetTranslate@TMatrix44@Toshi@@QAEXABVTVector3@2@@Z");
    }
    if (HMODULE tg = GetModuleHandleA("TGuiInterface.dll")) {
        pGetPointer = (fn_getpp)GetProcAddress(tg, "?GetPointerPosition@TGUIScreen@Toshi@@QBEXAAH0@Z");
        pSetPointer = (fn_setpp)GetProcAddress(tg, "?SetPointerPosition@TGUIScreen@Toshi@@QAE_NHH@Z");
    }
    LOG("split: TMatrix44 helpers %s", (pMatIdentity && pMatFromQuat && pMatSetTranslate) ? "found" : "MISSING (P2 uses free cam)");
    const Config& c = config::Get();
    g_vertical = (c.splitOrientation == "vertical");

    // Options-menu toggle (optional: only works once main.gui has the JPOGModSplit widget).
    HMODULE gi = GetModuleHandleA("TGuiInterface.dll"), pc = GetModuleHandleA("PGUICore.dll");
    if (gi) pFindNamedChild = Exp<fn_find>(gi, "?FindNamedChild@TGUIWidget@Toshi@@QBEPAV12@PBD@Z");
    if (pc) { pSetToggled = Exp<fn_setTog>(pc, "?SetToggled@PGUIToggleButton@@QAEX_N@Z");
              pIsToggled  = Exp<fn_isTog>(pc, "?IsToggled@PGUIToggleButton@@QBE_NXZ"); }
    if (c.optionsToggle && pFindNamedChild && pSetToggled && pIsToggled &&
        PatchVtable(0x00D1F760, 6, (void*)&Opt_OnActivate, (void**)&g_origOptActivate) &&
        PatchVtable(0x00D1F760, 7, (void*)&Opt_OnDeactivate, (void**)&g_origOptDeactivate))
        LOG("split: options-menu toggle hooked (AWindowsOptionsDialog vtable slots 6/7)");
    {
        // JPOG34: unit icons / thought bubbles only inside the HUD half (AUnitIcons::UpdateIcons projection call).
        bool ic = PatchCall(0x0049E48B, 0x00477EC0, (void*)&Icon_ToScreen);
        LOG("split: unit icon projection hook %s", ic ? "on" : "FAILED (call site changed?)");
        {   // JPOG57: highlight box + floating text + 0x51936B with the half's own projection
            const uintptr_t sites[] = { 0x004781AD, 0x004781B9, 0x005AC387, 0x005AC75D, 0x0051936B };
            int n = 0; for (uintptr_t a : sites) n += PatchCall(a, 0x00477EC0, (void*)&Overlay_ToScreen) ? 1 : 0;
            LOG("split: highlight box / floating text projection hook on %d of 5 call sites (JPOG57, [coop] overlay_own_proj)", n);
        }
        bool ri = detour::Install(game::At(0x0049E560), (void*)&Hook_RenderIcons, (void**)&g_origRenderIcons);
        LOG("split: unit icons in both halves %s", ri ? "on (JPOG40)" : "FAILED");
        bool li = PatchCall(0x0050A8A5, 0x00690110, (void*)&Listener_SetPos);
        LOG("split: sound listener follows a rider's dino %s", li ? "on (JPOG40)" : "FAILED (call site changed?)");
        bool ps = detour::Install(game::At(0x0068ED40), (void*)&Hook_PlaySound3D, (void**)&g_origPlaySound3D);
        LOG("split: every rider's dino is an extra sound ear %s", ps ? "on (JPOG42)" : "FAILED");
        bool pr = PatchCall(0x0051D882, 0x00520DC0, (void*)&Hook_PointerRay);
        LOG("split: HUD player's pick ray through the middle of their view %s", pr ? "on (JPOG52, [coop] hud_centre_ray)" : "FAILED (call site changed?)");
        bool ug = detour::Install(game::At(0x0047B550), (void*)&Hook_UnloadGame, (void**)&g_origUnloadGame);
        LOG("split: split-screen ends before the park unload %s", ug ? "on (JPOG58, [coop] unload_split_off)" : "FAILED");
        bool mp = detour::Install(game::At(0x005D6180), (void*)&Hook_AddMoneyChange, (void**)&g_origAddMoney);
        LOG("split: money pop-ups off in split-screen %s", mp ? "on (JPOG52, [coop] split_money_popups)" : "FAILED");
        bool a = PatchCall(0x0050C9C3, cam::DeleteCamera, (void*)&Hook_DeleteCamera);
        bool b = PatchCall(0x0050CA2E, cam::DeleteCamera, (void*)&Hook_DeleteCamera);
        LOG("split: camera-switch delete guard %s", (a && b) ? "on" : "FAILED");
    }
    {
        bool a = PatchCall(0x00573EBE, 0x004719D0, (void*)&FS_SetParkVisible);
        bool b = PatchCall(0x00573F63, 0x004719D0, (void*)&FS_SetParkVisible);
        LOG("split: full-screen menu hook %s", (a && b) ? "on (other player keeps the park)" : "NOT applied (bytes differ)");
    }
    LOG("split: installed (TScene::Render IAT slot %p, default %s, auto=%d)", g_iatSlot,
        g_vertical ? "vertical" : "horizontal", c.autoSplit);
    return true;
}

void Tick(void* renderer, float dt) {
    if (!g_iatSlot) return;
    ++g_frame;
    RiderSoundsTick();   // JPOG57
    g_renderer = renderer;
    TryInstallWater();   // JPOG40: the terrain shader DLL loads after us
    if (!g_mainScene && *pInterfaceVar)
        g_mainScene = pGetSystemResource(*pInterfaceVar, nullptr, 0x11);   // "MainScene"

    if (!g_active && g_clearFrames > 0) { InterlockedDecrement(&g_clearFrames); ClearFullBackBuffer(); }
    const Config& c = config::Get();
    g_pad2.Poll(c.deadzone, c.triggerThreshold);

    // Console-style controls: the cursor is a fixed aiming point in the middle of the acting
    // player's view (the GUI is mapped into that player's half), so A = Select/Place acts there.
    // JPOG37 (owner bug 5): the demolish tool is centre-locked in PC-cursor style too (gamectl::DemolishActive).
    // JPOG39: not on a pointer screen (console style, no focused widget) - there the pad moves the pointer (gamectl::CursorFallback).
    if (((input_map::ConsoleActive() && !gamectl::CursorFallback()) || gamectl::DemolishActive()) && !minipad::Active() && pSetPointer) {
        if (void* sc = GuiScreen()) {
            int gw = *reinterpret_cast<short*>((uint8_t*)sc + 0x70), gh = *reinterpret_cast<short*>((uint8_t*)sc + 0x72);
            if (gw > 0 && gh > 0) pSetPointer(sc, nullptr, gw / 2, gh / 2);
        }
    }

    // JPOG33 (owner bug 3): no GUI mouse pointer in console style once both pads are connected / split is on
    // (TGUIScreen+0x130 = pointer visible, read by TGUIScreen::Paint). Back on in PC-cursor style / 1 pad.
    {
        static int s_saved = -1;                     // pointer flag before we hid it (-1 = not hidden)
        static bool s_seenOn = false;                // the game (or the HUD plugin) turned it on while we hid it
        // JPOG35 (owner item 4): also hidden in every menu / screen that is not the park view (Start / pause / ring
        // menus, front end) while a pad is connected - the menus are driven by the pad's highlight there.
        const bool padOn = padslots::SlotOf(0) >= 0 || padslots::SlotOf(1) >= 0;
        bool hide = c.hideCursor && input_map::ConsoleActive() && !gamectl::CursorFallback() && !minipad::Active() && (g_active || g_pad2.Connected() || (padOn && !gamectl::ParkViewActive()));
        if (void* sc = GuiScreen()) {
            volatile uint8_t* vis = (uint8_t*)sc + 0x130;
            if (hide) {
                if (s_saved < 0) { s_saved = *vis; LOG("split: mouse pointer hidden (console style, 2 pads / split / menu; was %d)", s_saved); }
                if (*vis) { s_seenOn = true; *vis = 0; }
            } else if (s_saved >= 0) {
                *vis = (uint8_t)((s_saved || s_seenOn) ? 1 : 0);
                LOG("split: mouse pointer shown again (%s)", input_map::ConsoleActive() ? "1 pad, park view" : "PC cursor style");
                s_saved = -1; s_seenOn = false;
            }
        }
    }

    UpdateHudAim();   // JPOG39: the HUD player's aim point for the ground circle (update path, not mid-render)
    GUARDED("RT pan", { TickRtPan(dt); });
    // JPOG34 (owner bug 8): zoom out further (globals once; every world camera the game uses).
    ApplyZoomGlobals();
    if (!g_camMgr) g_camMgr = reinterpret_cast<fn_mgr0>(game::At(cam::GetCameraManager))();
    if (g_camMgr) GUARDED("zoom camera limits", { ZoomCamera(GameActiveCamera()); if (g_active) { ZoomCamera(g_cam1); ZoomCamera(g_cam2); } });
    // JPOG34 (owner bug 3): rider's hint in the rider's half.
    GUARDED("hint in the rider's half", { TickHintHalf(); });
    // JPOG34 (owner bug 4): "player 1 can't cancel a dino selection with B". Log the selection before / after the
    // HUD holder's B in the park view; if the game did not deselect within ~0.2 s, deselect with its own call.
    if (g_active) {
        static void* s_bSel = nullptr; static unsigned s_bFrame = 0; static int s_bLogs = 0;
        XPad& ap = g_activePlayer ? g_pad2 : g_pad1;
        if (ap.Pressed(PAD_B) && !dinocontrol::Controls((int)g_activePlayer) && gamectl::ParkViewActive()) {
            void* sel = gamectl::SelectedUnit();
            if (sel) {
                s_bSel = sel; s_bFrame = g_frame;
                void* ac = GameActiveCamera();
                if (s_bLogs < 20) { ++s_bLogs; LOG("select: player %d pressed B with unit %p selected (state vtbl %p, camera %p %s)", (int)g_activePlayer + 1, sel,
                    gamectl::CurrentState() ? *(void**)gamectl::CurrentState() : nullptr, ac, (ac && IsExactlyWorldCamera(ac)) ? "world" : "unit/follow"); }
            }
        }
        if (s_bSel && g_frame - s_bFrame >= 12) {
            void* now = gamectl::SelectedUnit();
            if (now == s_bSel) {
                void* sel = *reinterpret_cast<void**>(game::At(0x008500B0));
                bool okD = sel && GUARDED("B deselect", {
                    reinterpret_cast<void(__fastcall*)(void*, void*, const void*)>(game::At(0x00526950))(sel, nullptr, nullptr); });
                LOG("select: B did not deselect %p - deselected with AUnitSelection::SetSelectedUnit(null) (ok=%d)", s_bSel, okD);
            } else if (s_bLogs < 20) { ++s_bLogs; LOG("select: B deselected %p (game, now %p)", s_bSel, now); }
            s_bSel = nullptr;
        }
    }

    // In a park? (same test as unlocks.cpp)
    void* park = *reinterpret_cast<void**>(game::At(game::addr::AParkManager_m_pParkInfo));
    if (park != g_park) { g_park = park; g_parkFrame = g_frame; }
    bool parkReady = park && (g_frame - g_parkFrame) > 180;   // ~3 s after the level loads
    // JPOG35 (owner 14:25: quitting the park in split left player 2's half on the island map, flashing): while the
    // park is torn down m_pParkInfo is still set, but the world the cameras clip against (*(0x85006C)+0x28, read by
    // AWorldCamera::ClipFocalPoint 0x528450) is gone -> the inactive camera update faulted (log 13:58:20) and split
    // stayed on in the front end. Split now ends as soon as that world pointer is gone.
    const bool world = WorldReady();

    bool focused = GameFocused();
    if (KeyEdge(c.keyToggleCoop, g_prevF11, focused)) {
        g_forced = !g_forced;
        LOG("split: manual toggle -> %s", g_forced ? "forced on" : "auto");
        if (!g_forced && !(c.autoSplit && g_pad2.Connected())) Deactivate("manual off");
    }
    // Orientation: Options menu (Start -> Options -> Split) or F12. Back is Research for both players.
    if (KeyEdge(c.keyToggleSplit, g_prevF12, focused)) SetOrientation(!g_vertical, "F12");
    PollOptionsToggle();

    // Player 2 only counts after their pad has been connected for 2 s: when the ROG Ally swaps its
    // built-in pad for a Bluetooth one, both can be connected for a moment - that must not split.
    unsigned p2since = padslots::Since(1);
    bool pad2Stable = g_pad2.Connected() && (p2since == 0 || GetTickCount() - p2since > 2000);
    // JPOG58: after a park unload stay off until the park pointer changes / goes away or the world is gone.
    if (g_unloadBlock && (!park || park != g_unloadBlockPark || !world)) { g_unloadBlock = false; LOG("split: park unload finished - split-screen may start again"); }
    bool want = parkReady && world && !g_unloadBlock && (g_forced || (c.autoSplit && pad2Stable));
    if (want && !g_active) Activate();
    else if (!want && g_active) Deactivate(g_unloadBlock ? "park unload" : !park ? "left park" : !world ? "park world gone (quit / loading)" : "player 2 controller gone");

    if (g_active) {
        if (dt <= 0 || dt > 0.25f) dt = 1.0f / 60.0f;
        g_pad1.Poll(c.deadzone, c.triggerThreshold);
        LONG req = InterlockedExchange(&g_requestPlayer, -1);
        if (req >= 0 && g_cam2) SwitchActive((int)req);
        // After a camera-type switch the acting player's camera is the game's new active camera
        // (not while a dino is controlled: dinocontrol adopts when it lets go).
        // JPOG30: the dino player (split mode) is never the acting player, so the builder can always adopt.
        if (g_adoptPending && !dinocontrol::Controls((int)g_activePlayer)) { g_adoptPending = 0; splitscreen::AdoptActiveCamera(); }
        if (g_cam2) UpdateInactiveCamera(dt); else UpdateP2Camera(dt);
        // JPOG31: the active player controls a dino as well -> their half follows their dino too.
        {
            bool on = false; const int act = (int)g_activePlayer;
            if (g_cam2 && dinocontrol::Controls(act)) {
                float ref[16];
                if (splitscreen::ViewMatrix(ref))
                    GUARDED("active dino chase camera", { on = dinocontrol::ChaseView(act, dt, ref, g_actChase); });
            }
            if (!on) g_actChaseLogs = 0;
            g_actChaseOn = on;
        }
        if ((g_frame % 600) == 0) LogMatrix(g_cam2 ? "P2 (game camera)" : "P2 (free cam)", g_p2);
    }
}

} // namespace splitscreen

// Read by JPOGMouseFix.asi (our build of JPOG-Modernize): while split-screen is on, the whole GUI -
// HUD, menus, cursor and clicks - is drawn inside the half of the player who owns the cursor.
extern "C" __declspec(dllexport) int __stdcall JPOGMod_GetHudRect(float* rect4 /* [6]: x y w h fullW fullH */) {
    static bool s_firstCall = true, s_firstOn = true;
    if (s_firstCall) { s_firstCall = false; LOG("split: HUD plugin connected (JPOGMod_GetHudRect called)"); }
    if (!g_active || !rect4 || g_W <= 0) return 0;
    if (s_firstOn) { s_firstOn = false; LOG("split: HUD now drawn in the active player's half"); }
    if (!config::Get().hudPerHalf) return 0;
    RectFor((int)g_activePlayer, rect4[0], rect4[1], rect4[2], rect4[3]);
    rect4[4] = g_W; rect4[5] = g_H;          // real screen size (the game's "render size" is the viewport)
    return 1;
}
