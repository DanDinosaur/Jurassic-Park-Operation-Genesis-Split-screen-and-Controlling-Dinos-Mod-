// config.h - loads jpogmod.ini from the game folder into a settings struct.
#pragma once
#include <string>

struct Config {
    bool  enabled           = true;
    bool  log               = true;

    // controller
    bool  padEnabled        = true;
    bool  padPlayer2        = false;
    bool  vibration         = true;
    float rumbleStrength    = 1.0f;   // [controller] rumble_strength (multiplies the camera-shake amount)
    float deadzone          = 0.24f;
    float triggerThreshold  = 0.12f;
    float lookSensitivity   = 1.0f;
    float cursorSpeed       = 1.0f;
    float rtPanSpeed        = 1.5f;   // rt_pan_speed: hold RT + left stick in the park view = pan the camera directly
    bool  renderActiveLast  = false;  // [coop] render_active_last: JPOG36 default 0 = HUD half drawn FIRST (one-pass highlights like the
                                      // placement footprint show in the HUD half) + its projection restored after the other half; 1 = JPOG34/35 order

    // player (used once player mode is built)
    float walkSpeed         = 4.0f;
    float runMult           = 1.8f;
    float turnRate          = 180.0f;
    float cameraDistance    = 4.5f;
    float aimFovScale       = 0.6f;
    std::string viewMode    = "ots";
    std::string avatarUnit  = "ranger";
    std::string weapon      = "tranq";
    bool  vehicleShoot      = true;   // force the vanilla shoot-from-vehicle cheat on

    // coop
    std::string splitOrientation = "vertical";   // owner: 75" TV, side-by-side
    bool  sharedBudget      = true;
    bool  hudPerHalf        = true;   // draw the HUD inside the active player's half
    bool  p2GameCamera      = true;   // P2 uses a real AWorldCamera (0 = simple free cam)
    bool  autoSplit         = true;
    // [coop] handover = p1_back (player 1 holds Back 0.6 s to pass the HUD) | any_button (old: any press takes it)
    std::string handover    = "p1_back";
    bool  optionsToggle     = true;   // hook the Options popup "Split" toggle   // split as soon as controller 2 is seen in a loaded park
    float p2MoveSpeed       = 30.0f;  // player-2 camera, m/s
    float p2TurnRate        = 110.0f; // deg/s at full stick
    float p2StartOffset     = 15.0f;  // P2 starts this far to P1's side
    bool  hideCursor        = true;   // JPOG33 [coop] hide_cursor: no GUI pointer in console style with 2 pads / split
    // JPOG34 [coop]
    float zoomOutMult       = 1.5f;   // zoom_out_mult: world camera max distance / max altitude x this (1 = game default)
    bool  aimMarker         = false;  // aim_marker (EXPERIMENTAL, lines not visible yet): ground ring under each player's aim point (split-screen)
    // JPOG39 [controller]: demolish-style ground circle under the aiming point (console style)
    bool  aimCircle         = true;   // aim_circle
    float aimCircleSize     = 4.0f;   // aim_circle_size (the demolish tool uses 4)
    bool  aimPickCircle     = true;   // aim_pick_circle: split, player without the HUD - circle on the dino A would take
    float pickRadius        = 15.0f;  // pick_radius: player without the HUD: A takes the nearest dino within this (m)
    float riderHintSecs     = 30.0f;   // rider_hint_secs: dino controls hint shown in the rider's own half
    bool  riderListener     = false;   // rider_listener: split, a rider's dino = the sound listener (JPOG40/41)
    bool  iconsActiveHalf   = true;   // icons_active_half: visitor thought bubbles / unit icons only inside the HUD half
    float p2YawSign = 1, p2PitchSign = 1, p2MoveSign = 1, p2UpSign = 1;
    // JPOG49: the player without the HUD gets their own copy of the game's build disc ([coop] p2_disc_menu).
    bool  p2DiscMenu = true;
    // JPOG53: player 2 opens their own Manager / Dinosaurs / Actions windows in their half ([coop] p2_windows).
    bool  p2Windows = true;
    // JPOG52 (owner 10:44): split + centre-locked cursor - the HUD player's pick ray goes through the middle of their view
    // (camera position -> camera target), not through the GUI pointer conversion ([coop] hud_centre_ray).
    bool  hudCentreRay = true;
    // JPOG52 (backlog 8): visitors' floating "+money" pop-ups in split-screen ([coop] split_money_popups, 0 = off).
    bool  splitMoneyPopups = false;
    // JPOG52: invalidate the render context's cached frustum planes / inverse view around each split pass ([coop] rc_invalidate).
    bool  rcInvalidate = true;
    float p2DiscStickY = -1;   // [coop] p2_disc_invert_stick = 1 flips it (stick up must pick the top picture)

    // unlocks (owner: always on)
    bool  unlockSiteB       = true;
    bool  allResearch       = true;
    bool  allDigSites       = true;
    bool  maxStars          = true;
    bool  infiniteMoney     = true;
    int   moneyFloor        = 1000000;
    int   moneyTopUp        = 10000000;

    // [player] ranger_hold_back: hold Back 0.6 s = Ranger mode (experimental, off in Co-op v1)
    bool  rangerHoldBack    = false;
    // [hud] hints: short on-screen hints through the game's own hint message
    bool  hints             = true;
    // [dino] dino control (select a dino with A, RB + D-pad Up = control it, B = let go)
    bool  dinoEnabled       = true;
    float dinoLead          = 14.0f;  // lead_distance: steering target this far ahead of the dino (m)
    float dinoRunMult       = 1.6f;   // run_mult: speed multiplier at full stick
    bool  dinoSizeRange = true;     // [dino] size_attack_range (JPOG55): attack range grows with the dino's size
    bool  dinoBloodOnHit = true;    // [dino] blood_only_on_hit (JPOG55): no mouth blood when a bite hits nothing
    bool  dinoRunButton = true;     // [dino] run_button (JPOG56): run = left-stick click toggle, the stick only walks
    float dinoRunStart = 0.95f;     // [dino] run_start (JPOG55): stick amount where running begins
    float dinoWalkMin  = 0.45f;     // [dino] walk_speed_min (JPOG55): speed factor at a light push
    float dinoAttackRange   = 10.0f;  // attack_range (m)
    float dinoDamageMin     = 8.0f;   // attack_damage_min
    float dinoDamageFrac    = 0.2f;   // attack_damage_frac (of the target's health)
    float dinoAttackCooldown= 0.8f;   // attack_cooldown (s)
    float dinoInvX          = 1.0f;   // invert_x -> -1
    float dinoInvY          = 1.0f;   // invert_y -> -1
    // split-screen dino camera (JPOG30): the dino player's own chase camera, no game follow state
    float dinoCamDist       = 13.0f;  // cam_distance (m)
    float dinoCamLookH      = 2.5f;   // cam_look_height: aim point above the dino's feet (m)
    float dinoCamPitch      = 22.0f;  // cam_pitch: start angle above the horizon (deg)
    float dinoCamTurn       = 120.0f; // cam_turn_rate (deg/s at full stick)
    float dinoCamInvX       = 1.0f;   // cam_invert_x -> -1
    float dinoCamInvY       = 1.0f;   // cam_invert_y -> -1
    // JPOG32 dino action buttons: Y roar, X eat, hold LB sleep, RT on a weak dino = finishing move
    bool  dinoActions       = true;   // actions = 1
    bool  dinoFreezeBehaviour = true; // freeze_behaviour (JPOG36): the controlled dino's behaviour script (FSM) is paused
    bool  dinoForceVisible  = true;   // force_visible (JPOG36): the controlled dino always counts as on-screen (full-rate update)
    float dinoFinishHealth  = 250.0f;  // finish_health: (JPOG32-36, no longer used - replaced by finish_health_low)
    // JPOG37
    float dinoFinishLow     = 60.0f;   // finish_health_low: finishing move / raptor grapple only on a victim below this health
    float dinoGrabRange     = 7.0f;    // grab_range: big carnivore RT next to a goat / cow (m) = grab + swallow
    bool  moneyPopups       = true;    // [coop] money_popups (JPOG56): 1 = single player keeps them (owner 11:56); 0 = off everywhere
    bool  rbUpInfo          = false;   // [coop] rb_up_info (JPOG55): RB + D-pad Up = vanilla info bar (0 = take control, old)
    bool  iconOwnProj       = true;    // [coop] icon_own_proj (JPOG55): unit icons / rings / thought bubbles projected with the half's own pixel->GUI mapping
    bool  overlayOwnProj    = true;    // [coop] overlay_own_proj (JPOG57): highlight box / floating text with the half's own projection
    bool  riderSoundFollow  = true;    // [coop] rider_sound_follow (JPOG57): a rider's dino sounds stay at the same offset from the moving listener
    bool  aTakesControl     = false;   // [dino] a_takes_control (JPOG57): A on a dino = take control for the HUD player too
    bool  unloadSplitOff    = true;    // [coop] unload_split_off (JPOG58): end split-screen inside ARootTask::UnloadGameController, before the park is torn down
    bool  dinoZzzAlways     = true;    // [dino] zzz_always (JPOG57): the sleep icon over a controlled sleeping dino without holding the info key
    bool  treeKnockOnce     = true;    // [dino] tree_knock_once (JPOG57): a player's dino knocks each tree cell once per 3 s
    bool  wakeInstant       = true;    // [dino] wake_instant (JPOG57): a taken sleeping dino stands up at once (no slide)
    bool  p2Commands        = true;    // [coop] p2_commands (JPOG55): player 2's Actions disc commands (park entrance, emergency, build entrance)
    bool  splitCullBoth     = true;    // [coop] split_cull_both (JPOG54): each half also draws units the OTHER half sees (dome/security building)
    bool  splitCullFix      = true;    // [coop] split_cull_fix: units seen by EITHER half update at full rate (cullfix.cpp)
    // JPOG59 (final build, open list)
    bool  p2FsKeepPark      = true;
    char  cullLogTypes[128] = "rangerdp,scamera,camera,shelter";   // [coop] cull_log_types (JPOG60 item 12 diagnosis, "" = off)    // [coop] p2_fs_keep_park (JPOG60): player 2's full-screen windows never hide player 1's park
    bool  p2WindowPad       = true;    // [coop] p2_window_pad: player 2's pad inside their own windows (D-pad highlight, A press)
    bool  p2Buildings       = true;    // [coop] p2_buildings: player 2's A on a building = its own window in their half
    bool  backHandover      = false;   // [coop] back_handover: 1 = old "player 1 holds Back = pass the HUD"
    bool  startBorrow       = true;    // [coop] start_borrow: the player without the HUD presses Start = their Start menu
    bool  infoBarPad        = true;    // [controller] info_bar_pad: vanilla info bar buttons from the D-pad + A
    bool  pcWithoutPad      = true;    // [controller] pc_without_pad: no controller = PC style (free mouse, no centre lock)
    bool  dinoSelectHint    = true;    // [dino] select_hint (JPOG63): selecting a dino shows the RB + D-pad Up hint
    bool  rideHud           = false;    // [dino] ride_hud (JPOG62): the HUD player rides a dino = HUD shows only ride_hud_mask
    int   rideHudMask       = 0x1100;  // [dino] ride_hud_mask (JPOG62): HUD bits while riding (0x100 dino stats, 0x1000 minimap)
    bool  windowSpinners    = true;    // [controller] window_spinners (JPOG62): D-pad/stick left-right turn pickers/sliders in windows
    bool  popupCallerLog    = true;    // [coop] popup_caller_log (JPOG62): log who opens a message popup while player 2 has a window
    bool  iconHudReproj     = true;    // [coop] icon_hud_reproj (JPOG62): HUD half's icons/bubbles re-projected like the other half's
    int   dinoBackAll       = 1;       // [dino] back_all (JPOG60): stick down walks backwards for EVERY species (walk_lp if no walk_bck_lp)
    bool  dinoStrafeBack    = true;    // [dino] strafe_back: stick sideways = strafe, down = walk backwards (no running)
    float dinoStrafeSpeed   = 2.0f;    // [dino] strafe_speed (m/s at full stick)
    float dinoBackSpeed     = 1.6f;    // [dino] back_speed (m/s at full stick)
    bool  dinoDrink         = true;    // [dino] drink: A = drink, only next to water
    bool  vehiclePad        = true;    // [vehicle] pad_sticks: sticks drive the helicopter / jeeps / mini games (console layout)
    int   soundLimit        = 3;       // [coop] sound_limit: split only - max starts of one sound sample per second (0 = off)
    bool  gunSound          = true;    // [vehicle] gun_sound: player-driven vehicle guns without a sound get gun_sound_id
    int   gunSoundId        = 0x2E3;   // [vehicle] gun_sound_id (SFX id; 739 = rifle, 742 = weapon 5)
    float vehicleStickY     = -1.0f;   // [vehicle] pad_invert_y = 1 flips the stick Y sent to the game
    bool  perfLog           = true;    // [debug] perf_log: frame time + unit count every 10 s in jpogmod.log

    // debug: read jpogmod_pad.txt as a virtual controller (testing without a pad)
    bool  virtualPad        = false;
    // debug: [debug] test_dino = <unit name, e.g. tricera>. Hold Back + D-pad Right in a park = spawn two
    // of them at the camera target and select the first (to test dino control). Empty = off (release default).
    std::string testDino;

    // key toggles (virtual-key codes resolved from names)
    int  keyTogglePlayer    = VK_F1_PLACEHOLDER();
    int  keySpawnMenu       = 0;
    int  keySwapWeapon      = 0;
    int  keyCycleView       = 0;
    int  keyToggleCoop      = 0;
    int  keyToggleSplit     = 0;
    bool rideKeys = true;   // [keys] ride_keys (JPOG63): keyboard riding keys for player 1
    int  rideToggle = 0;
    int  rideFwd = 0;
    int  rideBack = 0;
    int  rideLeft = 0;
    int  rideRight = 0;
    int  rideRun = 0;
    int  rideBite = 0;
    int  rideFinish = 0;
    int  ridePounce = 0;
    int  rideRoar = 0;
    int  rideEat = 0;
    int  rideDrink = 0;
    int  rideSleep = 0;
    int  rideCamL = 0;
    int  rideCamR = 0;
    int  rideCamU = 0;
    int  rideCamD = 0;
    int  rideZoomIn = 0;
    int  rideZoomOut = 0;

    // raw [map.manager] button->action strings, resolved by input_map.
    std::string mgr_A, mgr_B, mgr_X, mgr_Y, mgr_LB, mgr_RB, mgr_BACK, mgr_START,
                mgr_RS, mgr_BACK_TAP, mgr_RT, mgr_LT, mgr_DPAD, mgr_LSTICK, mgr_RSTICK;

    static constexpr int VK_F1_PLACEHOLDER() { return 0x70; } // VK_F1
};

namespace config {
    // Loads jpogmod.ini from the exe's folder. Missing file = defaults.
    const Config& Load();
    const Config& Get();
}
