#include <windows.h>
#include "config.h"
#include "log.h"
#include <string>
#include <cstdlib>

namespace {
    Config g_cfg;
    bool   g_loaded = false;

    void IniPath(wchar_t* out, int n) {
        GetModuleFileNameW(nullptr, out, n);
        wchar_t* s = wcsrchr(out, L'\\');
        if (s) *(s + 1) = 0;
        wcsncat_s(out, n, L"jpogmod.ini", _TRUNCATE);
    }
    std::string GetStr(const wchar_t* ini, const char* sec, const char* key, const char* def) {
        char buf[256]{};
        GetPrivateProfileStringA(sec, key, def, buf, sizeof(buf),
            // GetPrivateProfileString needs an ANSI path; build one.
            [&]{ static char p[MAX_PATH]; size_t c; wcstombs_s(&c, p, ini, MAX_PATH); return p; }());
        // Strip inline "; comment" and surrounding whitespace (Win32 keeps them).
        std::string s = buf;
        size_t sc = s.find(';');
        if (sc != std::string::npos) s.erase(sc);
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
    }
    float GetF(const wchar_t* ini, const char* sec, const char* key, float def) {
        std::string s = GetStr(ini, sec, key, "");
        return s.empty() ? def : (float)atof(s.c_str());
    }
    int GetI(const wchar_t* ini, const char* sec, const char* key, int def) {
        std::string s = GetStr(ini, sec, key, "");
        return s.empty() ? def : atoi(s.c_str());
    }
    // Resolve a key NAME (e.g. "F2", "Q", "V") to a virtual-key code.
    int VKFromName(const std::string& n) {
        if (n.size() == 1) {
            char c = (char)toupper(n[0]);
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
        }
        if ((n.size() == 2 || n.size() == 3) && (n[0]=='F'||n[0]=='f')) { int f = atoi(n.c_str()+1); if (f>=1&&f<=12) return 0x6F+f; }
        // JPOG63: more names for the riding keys
        std::string u; for (char ch : n) u += (char)toupper((unsigned char)ch);
        if (u.size() == 7 && u.compare(0, 6, "NUMPAD") == 0 && u[6] >= '0' && u[6] <= '9') return VK_NUMPAD0 + (u[6] - '0');
        struct { const char* s; int vk; } names[] = {
            {"ADD", VK_ADD}, {"NUMPADPLUS", VK_ADD}, {"SUBTRACT", VK_SUBTRACT}, {"NUMPADMINUS", VK_SUBTRACT},
            {"RSHIFT", VK_RSHIFT}, {"LSHIFT", VK_LSHIFT}, {"RCONTROL", VK_RCONTROL}, {"LCONTROL", VK_LCONTROL},
            {"SPACE", VK_SPACE}, {"TAB", VK_TAB}, {"BACKSPACE", VK_BACK}, {"HOME", VK_HOME}, {"END", VK_END},
            {"INSERT", VK_INSERT}, {"PAGEUP", VK_PRIOR}, {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT} };
        for (auto& e : names) if (u == e.s) return e.vk;
        return 0;
    }
}

namespace config {

const Config& Load() {
    wchar_t ini[MAX_PATH]{}; IniPath(ini, MAX_PATH);

    g_cfg.enabled          = GetI(ini, "general", "enabled", 1) != 0;
    g_cfg.log              = GetI(ini, "general", "log", 1) != 0;

    g_cfg.padEnabled       = GetI(ini, "controller", "enabled", 1) != 0;
    g_cfg.padPlayer2       = GetI(ini, "controller", "player2", 0) != 0;
    g_cfg.vibration        = GetI(ini, "controller", "vibration", 1) != 0;
    g_cfg.rumbleStrength   = GetF(ini, "controller", "rumble_strength", 1.0f);
    g_cfg.deadzone         = GetF(ini, "controller", "deadzone", 0.24f);
    g_cfg.triggerThreshold = GetF(ini, "controller", "trigger_threshold", 0.12f);
    g_cfg.lookSensitivity  = GetF(ini, "controller", "look_sensitivity", 1.0f);
    g_cfg.cursorSpeed      = GetF(ini, "controller", "cursor_speed", 1.0f);
    g_cfg.rtPanSpeed       = GetF(ini, "controller", "rt_pan_speed", 1.5f);
    g_cfg.renderActiveLast = GetI(ini, "coop", "render_active_last", 0) != 0;

    g_cfg.walkSpeed        = GetF(ini, "player", "walk_speed", 4.0f);
    g_cfg.runMult          = GetF(ini, "player", "run_mult", 1.8f);
    g_cfg.turnRate         = GetF(ini, "player", "turn_rate", 180.0f);
    g_cfg.cameraDistance   = GetF(ini, "player", "camera_distance", 4.5f);
    g_cfg.aimFovScale      = GetF(ini, "player", "aim_fov_scale", 0.6f);
    g_cfg.viewMode         = GetStr(ini, "player", "view_mode", "ots");
    g_cfg.avatarUnit       = GetStr(ini, "player", "avatar_unit", "ranger");
    g_cfg.weapon           = GetStr(ini, "player", "weapon", "tranq");
    g_cfg.vehicleShoot     = GetI(ini, "player", "vehicle_shoot", 1) != 0;

    g_cfg.splitOrientation = GetStr(ini, "coop", "split_orientation", "vertical");
    g_cfg.sharedBudget     = GetI(ini, "coop", "shared_budget", 1) != 0;
    g_cfg.hudPerHalf       = GetI(ini, "coop", "hud_per_half", 1) != 0;
    g_cfg.p2GameCamera     = GetI(ini, "coop", "p2_game_camera", 1) != 0;
    g_cfg.autoSplit        = GetI(ini, "coop", "auto_split", 1) != 0;
    g_cfg.handover         = GetStr(ini, "coop", "handover", "p1_back");
    g_cfg.optionsToggle    = GetI(ini, "coop", "options_menu_toggle", 1) != 0;
    g_cfg.p2MoveSpeed      = GetF(ini, "coop", "p2_move_speed", 30.0f);
    g_cfg.p2TurnRate       = GetF(ini, "coop", "p2_turn_rate", 110.0f);
    g_cfg.p2StartOffset    = GetF(ini, "coop", "p2_start_offset", 15.0f);
    g_cfg.hideCursor       = GetI(ini, "coop", "hide_cursor", 1) != 0;
    g_cfg.zoomOutMult      = GetF(ini, "coop", "zoom_out_mult", 1.5f);
    if (g_cfg.zoomOutMult < 1.0f) g_cfg.zoomOutMult = 1.0f; if (g_cfg.zoomOutMult > 3.0f) g_cfg.zoomOutMult = 3.0f;
    g_cfg.aimMarker        = GetI(ini, "coop", "aim_marker", 0) != 0;   // JPOG34: lines not visible yet (test only)
    g_cfg.pickRadius       = GetF(ini, "coop", "pick_radius", 15.0f);
    g_cfg.aimCircle        = GetI(ini, "controller", "aim_circle", 1) != 0;                 // JPOG39
    g_cfg.aimCircleSize    = GetF(ini, "controller", "aim_circle_size", 4.0f);
    if (!(g_cfg.aimCircleSize >= 1.0f)) g_cfg.aimCircleSize = 1.0f; if (g_cfg.aimCircleSize > 12.0f) g_cfg.aimCircleSize = 12.0f;
    g_cfg.aimPickCircle    = GetI(ini, "controller", "aim_pick_circle", 1) != 0;
    g_cfg.riderHintSecs    = GetF(ini, "coop", "rider_hint_secs", 30.0f);
    g_cfg.iconsActiveHalf  = GetI(ini, "coop", "icons_active_half", 1) != 0;
    g_cfg.riderListener    = GetI(ini, "coop", "rider_listener", 0) != 0;
    g_cfg.p2YawSign        = GetI(ini, "coop", "p2_invert_yaw", 0)   ? -1.0f : 1.0f;
    g_cfg.p2PitchSign      = GetI(ini, "coop", "p2_invert_pitch", 0) ? -1.0f : 1.0f;
    g_cfg.p2MoveSign       = GetI(ini, "coop", "p2_invert_move", 0)  ? -1.0f : 1.0f;
    g_cfg.p2DiscMenu       = GetI(ini, "coop", "p2_disc_menu", 1) != 0;
    g_cfg.p2Windows        = GetI(ini, "coop", "p2_windows", 1) != 0;
    g_cfg.hudCentreRay     = GetI(ini, "coop", "hud_centre_ray", 1) != 0;
    g_cfg.splitMoneyPopups = GetI(ini, "coop", "split_money_popups", 0) != 0;
    g_cfg.rcInvalidate     = GetI(ini, "coop", "rc_invalidate", 1) != 0;
    g_cfg.p2DiscStickY     = GetI(ini, "coop", "p2_disc_invert_stick", 0) ? 1.0f : -1.0f;
    g_cfg.p2UpSign         = GetI(ini, "coop", "p2_invert_updown", 0) ? -1.0f : 1.0f;

    g_cfg.keyTogglePlayer  = VKFromName(GetStr(ini, "keys", "toggle_player_mode", "F9"));
    g_cfg.keySpawnMenu     = VKFromName(GetStr(ini, "keys", "spawn_menu", "F10"));
    g_cfg.keySwapWeapon    = VKFromName(GetStr(ini, "keys", "swap_weapon", "X"));
    g_cfg.keyCycleView     = VKFromName(GetStr(ini, "keys", "cycle_view", "V"));
    g_cfg.keyToggleCoop    = VKFromName(GetStr(ini, "keys", "toggle_coop", "F11"));
    g_cfg.keyToggleSplit   = VKFromName(GetStr(ini, "keys", "toggle_split", "F12"));
    g_cfg.rideKeys         = GetI(ini, "keys", "ride_keys", 1) != 0;
    g_cfg.rideToggle = VKFromName(GetStr(ini, "keys", "ride_toggle", "T"));
    g_cfg.rideFwd = VKFromName(GetStr(ini, "keys", "ride_forward", "I"));
    g_cfg.rideBack = VKFromName(GetStr(ini, "keys", "ride_back", "K"));
    g_cfg.rideLeft = VKFromName(GetStr(ini, "keys", "ride_left", "J"));
    g_cfg.rideRight = VKFromName(GetStr(ini, "keys", "ride_right", "L"));
    g_cfg.rideRun = VKFromName(GetStr(ini, "keys", "ride_run", "RSHIFT"));
    g_cfg.rideBite = VKFromName(GetStr(ini, "keys", "ride_bite", "U"));
    g_cfg.rideFinish = VKFromName(GetStr(ini, "keys", "ride_finish", "H"));
    g_cfg.ridePounce = VKFromName(GetStr(ini, "keys", "ride_pounce", "O"));
    g_cfg.rideRoar = VKFromName(GetStr(ini, "keys", "ride_roar", "Y"));
    g_cfg.rideEat = VKFromName(GetStr(ini, "keys", "ride_eat", "N"));
    g_cfg.rideDrink = VKFromName(GetStr(ini, "keys", "ride_drink", "G"));
    g_cfg.rideSleep = VKFromName(GetStr(ini, "keys", "ride_sleep", "Z"));
    g_cfg.rideCamL = VKFromName(GetStr(ini, "keys", "ride_cam_left", "NUMPAD4"));
    g_cfg.rideCamR = VKFromName(GetStr(ini, "keys", "ride_cam_right", "NUMPAD6"));
    g_cfg.rideCamU = VKFromName(GetStr(ini, "keys", "ride_cam_up", "NUMPAD8"));
    g_cfg.rideCamD = VKFromName(GetStr(ini, "keys", "ride_cam_down", "NUMPAD2"));
    g_cfg.rideZoomIn = VKFromName(GetStr(ini, "keys", "ride_zoom_in", "ADD"));
    g_cfg.rideZoomOut = VKFromName(GetStr(ini, "keys", "ride_zoom_out", "SUBTRACT"));

    g_cfg.mgr_A      = GetStr(ini, "map.manager", "A", "ENTER");
    g_cfg.mgr_B      = GetStr(ini, "map.manager", "B", "ESCAPE");
    g_cfg.mgr_X      = GetStr(ini, "map.manager", "X", "B");
    g_cfg.mgr_Y      = GetStr(ini, "map.manager", "Y", "E");
    g_cfg.mgr_LB     = GetStr(ini, "map.manager", "LB", "F3");
    g_cfg.mgr_RB     = GetStr(ini, "map.manager", "RB", "F4");
    g_cfg.mgr_BACK   = GetStr(ini, "map.manager", "BACK", "");  // Back = player-mode toggle
    g_cfg.mgr_BACK_TAP = GetStr(ini, "map.manager", "BACK_TAP", "F2"); // Research
    g_cfg.mgr_RS     = GetStr(ini, "map.manager", "RS", "M");         // Messages
    g_cfg.mgr_START  = GetStr(ini, "map.manager", "START", "ESCAPE");
    g_cfg.mgr_RT     = GetStr(ini, "map.manager", "RT", "MOUSE_L");
    g_cfg.mgr_LT     = GetStr(ini, "map.manager", "LT", "MOUSE_R");
    g_cfg.mgr_DPAD   = GetStr(ini, "map.manager", "DPAD", "ARROWS");
    g_cfg.mgr_LSTICK = GetStr(ini, "map.manager", "LSTICK", "MOUSE_MOVE");
    g_cfg.mgr_RSTICK = GetStr(ini, "map.manager", "RSTICK", "CAMERA");

    g_cfg.unlockSiteB   = GetI(ini, "unlocks", "site_b", 1) != 0;
    g_cfg.allResearch   = GetI(ini, "unlocks", "all_research", 1) != 0;
    g_cfg.allDigSites   = GetI(ini, "unlocks", "all_dig_sites", 1) != 0;
    g_cfg.maxStars      = GetI(ini, "unlocks", "max_stars", 1) != 0;
    g_cfg.infiniteMoney = GetI(ini, "unlocks", "infinite_money", 1) != 0;
    g_cfg.moneyFloor    = GetI(ini, "unlocks", "money_floor", 1000000);
    g_cfg.moneyTopUp    = GetI(ini, "unlocks", "money_topup", 10000000);
    g_cfg.virtualPad = GetI(ini, "debug", "virtual_pad", 0) != 0;
    g_cfg.testDino   = GetStr(ini, "debug", "test_dino", "");
    g_cfg.rangerHoldBack = GetI(ini, "player", "ranger_hold_back", 0) != 0;
    g_cfg.hints          = GetI(ini, "hud", "hints", 1) != 0;
    g_cfg.dinoEnabled    = GetI(ini, "dino", "enabled", 1) != 0;
    g_cfg.dinoLead       = GetF(ini, "dino", "lead_distance", 14.0f);
    g_cfg.dinoRunMult    = GetF(ini, "dino", "run_mult", 1.6f);
    g_cfg.dinoSizeRange  = GetI(ini, "dino", "size_attack_range", 1) != 0;
    g_cfg.dinoBloodOnHit = GetI(ini, "dino", "blood_only_on_hit", 1) != 0;
    g_cfg.dinoRunButton  = GetI(ini, "dino", "run_button", 1) != 0;
    g_cfg.dinoRunStart   = GetF(ini, "dino", "run_start", 0.95f);
    g_cfg.dinoWalkMin    = GetF(ini, "dino", "walk_speed_min", 0.45f);
    g_cfg.dinoAttackRange    = GetF(ini, "dino", "attack_range", 10.0f);
    g_cfg.dinoDamageMin      = GetF(ini, "dino", "attack_damage_min", 8.0f);
    g_cfg.dinoDamageFrac     = GetF(ini, "dino", "attack_damage_frac", 0.2f);
    g_cfg.dinoAttackCooldown = GetF(ini, "dino", "attack_cooldown", 0.8f);
    g_cfg.dinoInvX = GetI(ini, "dino", "invert_x", 0) ? -1.0f : 1.0f;
    g_cfg.dinoInvY = GetI(ini, "dino", "invert_y", 0) ? -1.0f : 1.0f;
    g_cfg.dinoCamDist  = GetF(ini, "dino", "cam_distance", 13.0f);
    g_cfg.dinoCamLookH = GetF(ini, "dino", "cam_look_height", 2.5f);
    g_cfg.dinoCamPitch = GetF(ini, "dino", "cam_pitch", 22.0f);
    g_cfg.dinoCamTurn  = GetF(ini, "dino", "cam_turn_rate", 120.0f);
    g_cfg.dinoCamInvX  = GetI(ini, "dino", "cam_invert_x", 0) ? -1.0f : 1.0f;
    g_cfg.dinoCamInvY  = GetI(ini, "dino", "cam_invert_y", 0) ? -1.0f : 1.0f;
    g_cfg.dinoActions  = GetI(ini, "dino", "actions", 1) != 0;
    g_cfg.dinoFreezeBehaviour = GetI(ini, "dino", "freeze_behaviour", 1) != 0;
    g_cfg.dinoForceVisible    = GetI(ini, "dino", "force_visible", 1) != 0;
    g_cfg.dinoFinishHealth = GetF(ini, "dino", "finish_health", 250.0f);
    g_cfg.dinoFinishLow    = GetF(ini, "dino", "finish_health_low", 60.0f);
    g_cfg.dinoGrabRange    = GetF(ini, "dino", "grab_range", 7.0f);
    g_cfg.splitCullFix     = GetI(ini, "coop", "split_cull_fix", 1) != 0;
    g_cfg.moneyPopups      = GetI(ini, "coop", "money_popups", 1) != 0;
    g_cfg.rbUpInfo         = GetI(ini, "coop", "rb_up_info", 0) != 0;
    g_cfg.iconOwnProj      = GetI(ini, "coop", "icon_own_proj", 1) != 0;
    g_cfg.p2Commands       = GetI(ini, "coop", "p2_commands", 1) != 0;
    g_cfg.overlayOwnProj   = GetI(ini, "coop", "overlay_own_proj", 1) != 0;
    g_cfg.riderSoundFollow = GetI(ini, "coop", "rider_sound_follow", 1) != 0;
    g_cfg.aTakesControl    = GetI(ini, "dino", "a_takes_control", 0) != 0;
    g_cfg.unloadSplitOff   = GetI(ini, "coop", "unload_split_off", 1) != 0;
    g_cfg.stateGuard       = GetI(ini, "coop", "state_guard", 1) != 0;            // JPOG77
    g_cfg.fmodStreamGuard  = GetI(ini, "general", "fmod_stream_guard", 1) != 0;   // JPOG78
    g_cfg.stateGuardHud    = GetI(ini, "coop", "state_guard_handover", 1) != 0;   // JPOG77
    g_cfg.dinoZzzAlways    = GetI(ini, "dino", "zzz_always", 1) != 0;
    g_cfg.treeKnockOnce    = GetI(ini, "dino", "tree_knock_once", 1) != 0;
    g_cfg.wakeInstant      = GetI(ini, "dino", "wake_instant", 1) != 0;
    g_cfg.splitCullBoth    = GetI(ini, "coop", "split_cull_both", 1) != 0;
    g_cfg.perfLog          = GetI(ini, "debug", "perf_log", 1) != 0;
    g_cfg.p2WindowPad      = GetI(ini, "coop", "p2_window_pad", 1) != 0;
    g_cfg.p2FsKeepPark     = GetI(ini, "coop", "p2_fs_keep_park", 1) != 0;
    { std::string t = GetStr(ini, "coop", "cull_log_types", "rangerdp,scamera,camera,shelter");
      strncpy(g_cfg.cullLogTypes, t.c_str(), sizeof g_cfg.cullLogTypes - 1); g_cfg.cullLogTypes[sizeof g_cfg.cullLogTypes - 1] = 0; }
    g_cfg.p2Buildings      = GetI(ini, "coop", "p2_buildings", 1) != 0;
    g_cfg.backHandover     = GetI(ini, "coop", "back_handover", 0) != 0;
    g_cfg.startBorrow      = GetI(ini, "coop", "start_borrow", 1) != 0;
    g_cfg.infoBarPad       = GetI(ini, "controller", "info_bar_pad", 1) != 0;
    g_cfg.pcWithoutPad     = GetI(ini, "controller", "pc_without_pad", 1) != 0;
    g_cfg.dinoStrafeBack   = GetI(ini, "dino", "strafe_back", 1) != 0;
    g_cfg.dinoFenceBreak   = GetI(ini, "dino", "fence_break", 1) != 0;
    g_cfg.dinoFenceReach   = GetF(ini, "dino", "fence_reach", 5.0f);
    g_cfg.dinoFenceBites   = GetI(ini, "dino", "fence_bites", 5);
    g_cfg.dinoBiteFrontOnly = GetI(ini, "dino", "bite_front_only", 1) != 0;
    g_cfg.mrDna            = GetI(ini, "unlocks", "mr_dna", 1);
    g_cfg.p2SelectFly      = GetI(ini, "coop", "p2_select_fly", 1) != 0;
    g_cfg.p2SelectDist     = GetF(ini, "coop", "p2_select_dist", 30.0f);
    g_cfg.p2CamMatch       = GetI(ini, "coop", "p2_cam_match", 1) != 0;
    g_cfg.p2StickFull      = GetF(ini, "coop", "p2_stick_full", 0.7f);
    g_cfg.p2ZoomSpeed      = GetF(ini, "coop", "p2_zoom_speed", 1.0f);
    g_cfg.dinoRbTurn       = GetI(ini, "dino", "rb_turn", 1) != 0;
    g_cfg.dinoRbTapTime    = GetF(ini, "dino", "rb_tap_time", 0.25f);
    g_cfg.dinoRbTurnSpeed  = GetF(ini, "dino", "rb_turn_speed", 100.0f);
    g_cfg.dinoStickCone    = GetF(ini, "dino", "stick_cone", 60.0f);
    g_cfg.dinoStickLock    = GetI(ini, "dino", "stick_lock", 1) != 0;            // JPOG77
    g_cfg.dinoStickSide    = GetF(ini, "dino", "stick_side_band", 20.0f);        // JPOG77
    g_cfg.dinoStickHardTurn= GetF(ini, "dino", "stick_hard_turn", 2.0f);   // JPOG75: > 1 = never (sideways = strafe; turn = LB / RB / right stick)
    g_cfg.dinoBuildingBites= GetI(ini, "dino", "building_bites", 5);
    g_cfg.dinoBiteExtra    = GetF(ini, "dino", "bite_extra", 2.0f);
    g_cfg.p2PointerFovHorizontal = GetI(ini, "pc2ptr", "fov_horizontal", 0) != 0;
    g_cfg.p2PointerFovScale = GetF(ini, "pc2ptr", "fov_scale", 1.0f);
    g_cfg.vehicleHidePointer = GetI(ini, "vehicle", "hide_pointer", 1) != 0;
    g_cfg.p2GameArt        = GetI(ini, "pc2ptr", "game_art", 1) != 0;
    g_cfg.particleGuard    = GetI(ini, "display", "particle_guard", 1) != 0;
    g_cfg.p2Repair         = GetI(ini, "coop", "p2_repair", 1) != 0;
    g_cfg.heliPadFly       = GetI(ini, "vehicle", "heli_pad_fly", 1) != 0;
    g_cfg.heliPresser      = GetI(ini, "vehicle", "heli_presser", 1) != 0;
    g_cfg.dinoZzzHint      = GetI(ini, "dino", "zzz_hint_fallback", 1) != 0;
    g_cfg.dinoEatPeople    = GetI(ini, "dino", "eat_people", 1) != 0;
    g_cfg.dinoLbTurn       = GetI(ini, "dino", "lb_turn", 1) != 0;
    g_cfg.dinoRbTurnSign   = GetI(ini, "dino", "rb_turn_invert", 0) ? -1.0f : 1.0f;
    g_cfg.dinoStepByFacing = GetI(ini, "dino", "step_by_facing", 1) != 0;
    g_cfg.dinoStepInvX     = GetI(ini, "dino", "step_invert_x", 0) ? -1.0f : 1.0f;
    g_cfg.dinoSelectHint   = GetI(ini, "dino", "select_hint", 1) != 0;
    g_cfg.rideHud          = GetI(ini, "dino", "ride_hud", 0) != 0;
    { std::string m = GetStr(ini, "dino", "ride_hud_mask", ""); g_cfg.rideHudMask = m.empty() ? 0x1100 : (int)strtol(m.c_str(), nullptr, 0); }
    g_cfg.windowSpinners   = GetI(ini, "controller", "window_spinners", 1) != 0;
    g_cfg.popupCallerLog   = GetI(ini, "coop", "popup_caller_log", 1) != 0;
    g_cfg.frontendClear    = GetI(ini, "display", "frontend_clear", 1) != 0;
    g_cfg.p2RepairPopup    = GetI(ini, "coop", "p2_repair_popup", 0) != 0;
    g_cfg.guiFocusGuard    = GetI(ini, "coop", "gui_focus_guard", 1) != 0;
    g_cfg.iconHudReproj    = GetI(ini, "coop", "icon_hud_reproj", 1) != 0;
    g_cfg.dinoBackAll      = GetI(ini, "dino", "back_all", 1);
    g_cfg.dinoStrafeSpeed  = GetF(ini, "dino", "strafe_speed", 2.0f);
    g_cfg.dinoBackSpeed    = GetF(ini, "dino", "back_speed", 1.6f);
    g_cfg.dinoDrink        = GetI(ini, "dino", "drink", 1) != 0;
    g_cfg.dinoFinisherAlign   = GetI(ini, "dino", "finisher_align", 1);
    g_cfg.dinoFinisherDistAdd = GetF(ini, "dino", "finisher_dist_add", 0.0f);
    g_cfg.dinoFinisherInterp  = GetI(ini, "dino", "finisher_interp", 1) != 0;
    g_cfg.dinoPounceGap       = GetF(ini, "dino", "pounce_gap", 3.0f);
    g_cfg.dinoSelectRadius    = GetF(ini, "dino", "select_radius", 12.0f);
    g_cfg.mainMenuBothPads    = GetI(ini, "controller", "main_menu_both_pads", 1) != 0;
    g_cfg.ownWidescreen       = GetI(ini, "hud", "widescreen", 1) != 0;
    g_cfg.hudHorPlus          = GetI(ini, "hud", "hor_plus", 1) != 0;
    g_cfg.hudGuiFix           = GetI(ini, "hud", "gui_fix", 1) != 0;
    g_cfg.hudScale            = GetF(ini, "hud", "scale", 1.0f);
    g_cfg.hudSplitScale       = GetF(ini, "hud", "split_scale", 1.3f);
    g_cfg.hudPinEdges         = GetI(ini, "hud", "pin_edges", 1) != 0;
    g_cfg.freeKeyboard        = GetI(ini, "display", "free_keyboard", 1) != 0;
    g_cfg.altF4Quit           = GetI(ini, "display", "alt_f4_quit", 1) != 0;
    g_cfg.borderless          = GetI(ini, "display", "borderless", 0) != 0;
    g_cfg.mouseSpeed          = GetF(ini, "mouse", "cursor_speed", 0.5f);
    g_cfg.cursorNoAccel       = GetI(ini, "mouse", "no_accel", 1) != 0;
    g_cfg.camFixSmoothing     = GetI(ini, "camera", "fix_smoothing", 1) != 0;
    g_cfg.camSmoothingFps     = GetF(ini, "camera", "smoothing_fps", 30.0f);
    g_cfg.vehiclePad       = GetI(ini, "vehicle", "pad_sticks", 1) != 0;
    g_cfg.vehicleStickY    = GetI(ini, "vehicle", "pad_invert_y", 0) ? 1.0f : -1.0f;
    g_cfg.gunSound         = GetI(ini, "vehicle", "gun_sound", 1) != 0;
    g_cfg.soundLimit       = GetI(ini, "coop", "sound_limit", 3);
    g_cfg.gunSoundId       = GetI(ini, "vehicle", "gun_sound_id", 0x2E3);

    g_loaded = true;
    LOG("config loaded: enabled=%d pad=%d p2=%d view=%s weapon=%s split=%s",
        g_cfg.enabled, g_cfg.padEnabled, g_cfg.padPlayer2,
        g_cfg.viewMode.c_str(), g_cfg.weapon.c_str(), g_cfg.splitOrientation.c_str());
    return g_cfg;
}

const Config& Get() { return g_loaded ? g_cfg : Load(); }

} // namespace config
