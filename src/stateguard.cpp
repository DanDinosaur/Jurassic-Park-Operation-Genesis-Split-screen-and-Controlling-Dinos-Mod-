// stateguard.cpp - see stateguard.h (JPOG77).
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "stateguard.h"
#include "config.h"
#include "game.h"
#include "gamectl.h"
#include "detour.h"
#include "guard.h"
#include "log.h"

namespace stateguard {
namespace {
    constexpr uintptr_t kRemoveSelf     = 0x0058E070;   // ARootState::RemoveSelf (thiscall, no args)
    constexpr uintptr_t kGameStateClass = 0x008B5D60;   // AGameState::m_sClass (GetClass 0x53AF20)
    constexpr uintptr_t kGameStateCount = 0x0084E760;   // AGameState::OnRemoval 0x53AE80: --[0x84E760] == 0 -> UnloadGameController
    // Return addresses of "GetCurrentGameState()->RemoveSelf()" (call [reg+0x3C], 3 bytes) in building / unit window handlers
    // (found by scanning SimJP.exe for call 0x58EC80 followed by call [reg+0x3C]; bytes checked FF 52 3C / FF 50 3C).
    const uintptr_t kSites[] = {
        0x004CE104, 0x004CE166,   // AUnitDinosaur::OnRetireConfirmMsgBoxHandler
        0x004CE761, 0x004CE7ED,   // AUnitDinosaur::OnVaccinationMsgBoxHandler
        0x004EB96D,               // AStaffController::OnFireConfirm
        0x004F0203,               // ASecurityCameraComponent::ViewCameraVision
        0x004F6E60,               // AVisBalloonComponent::CreatePath
        0x004FB0E1,               // AVisRideComponent::ViewPhotos
        0x004FBF75,               // AVisRideComponent::OnPurchaseMessage
        0x004FCDDA,               // AVisSafariComponent::CreatePath (owner crash 16:33)
        0x00506E9C,               // ATurretComponent::ControlTurret
        0x005857E7, 0x0058601F, 0x005860F9 };   // AUnitVaccinationOrderPopup
        // not listed (could run outside a park): AFossilHuntDialog::OnButtonBackAction 0x5908F9, APhotoAnalyserDialog::OnOKButton 0x5C2D40
    const char* kSiteNames[] = { "dino retire", "dino retire", "dino vaccinate", "dino vaccinate", "staff fire",
        "security camera view", "balloon track", "ride photos", "ride purchase", "safari track (CreatePath)",
        "turret control", "vaccination popup", "vaccination popup", "vaccination popup" };

    using fn_rs = void(__fastcall*)(void* self, void* edx);
    fn_rs g_orig = nullptr;
    volatile int g_clickWho = -1;
    int g_clickHits = 0, g_clickLogs = 0;
    unsigned g_kept = 0;

    void __fastcall Hook_RemoveSelf(void* self, void* edx) {
        const uintptr_t ret = (uintptr_t)__builtin_return_address(0);
        if (config::Get().stateGuard && self) {
            bool isGame = false; int cnt = -1; void* vt = nullptr;
            GUARDED("stateguard check", {
                vt = *reinterpret_cast<void**>(self);
                isGame = gamectl::IsA(self, kGameStateClass);
                cnt = *reinterpret_cast<int*>(game::At(kGameStateCount)); });
            int site = -1;
            for (int i = 0; i < (int)(sizeof kSites / sizeof kSites[0]); ++i)
                if (ret == (uintptr_t)game::At(kSites[i])) { site = i; break; }
            if (g_clickWho >= 0 && g_clickLogs < 20) {
                ++g_clickLogs;
                LOG("stateguard: player %d's own-window click removes state %p (vtbl %p, game state %d, game states %d, from %08lX %s)",
                    g_clickWho + 1, self, vt, isGame, cnt, (unsigned long)ret, site >= 0 ? kSiteNames[site] : "?");
            }
            if (isGame && cnt == 1 && site >= 0) {
                ++g_kept; if (g_clickWho >= 0) ++g_clickHits;
                LOG("stateguard: KEPT the park's last game state %p (vtbl %p) - '%s' button (from %08lX) would have unloaded the park "
                    "(no window state under the click); its new state goes on top of the park state like in vanilla (JPOG77 [coop] state_guard)",
                    self, vt, kSiteNames[site], (unsigned long)ret);
                return;
            }
        }
        g_orig(self, edx);
    }
}

void Install() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    if (!config::Get().stateGuard) { LOG("stateguard: off ([coop] state_guard = 0)"); return; }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(game::At(kRemoveSelf));
    static const uint8_t kExpect[6] = { 0x57, 0x53, 0x56, 0x8B, 0x79, 0x04 };
    if (memcmp(p, kExpect, sizeof kExpect) != 0) { LOG("stateguard: ARootState::RemoveSelf bytes differ - guard off"); return; }
    const bool ok = detour::Install(game::At(kRemoveSelf), (void*)&Hook_RemoveSelf, (void**)&g_orig);
    LOG("stateguard: window buttons can't unload the park %s (ARootState::RemoveSelf 0x58E070, %d call sites, JPOG77)",
        ok ? "on" : "FAILED", (int)(sizeof kSites / sizeof kSites[0]));
}
void BeginClick(int who) { g_clickWho = who; g_clickHits = 0; }
int  EndClick() { g_clickWho = -1; return g_clickHits; }
unsigned Kept() { return g_kept; }
} // namespace stateguard
