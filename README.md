# Jurassic Park: Operation Genesis - Split-screen co-op & dinosaur riding mod (JPOG Co-op v3)

**2-player split-screen co-op on one PC for Jurassic Park: Operation Genesis (PC, 2003), and take control of any dinosaur to walk, fight, roar, eat and sleep. Play with Xbox controllers, keyboards + mice, or one of each.**

## ⬇️ [Download the latest release](https://github.com/DanDinosaur/jurassic-park-operation-genesis-split-screen-and-controlling-dinos-mod-/releases/latest) → `JPOG_Coop_v3.zip`

### Quick start
1. **You need your own copy of Jurassic Park: Operation Genesis for PC.** No game files are included in this mod.
2. Unzip `JPOG_Coop_v3.zip` anywhere and double-click **`install.bat`**. It finds the game by itself and asks once whether you want the "easy start".
3. Start the game with **`Launch JPOG.bat`** and load a park.
4. **Split-screen** - pick how player 2 plays:
   - **Controller:** switch on a second controller. The screen splits after about 2 seconds.
   - **Keyboard + mouse:** on a second keyboard press **Shift+F8**, then click once with a second mouse. Only needed the first time.
5. **To ride a dinosaur:** select it (controller: aim + **A**, keyboard: click it), then press **RB + D-pad Up** (keyboard: **T**).
   **B** (keyboard: **T**) lets go.

### Before you play - important
- 🎮⌨️ **Controllers or keyboard + mouse - your choice, per player.** Xbox (XInput) controllers, or a keyboard + mouse for
  each player, or one of each. PlayStation / Switch pads work through Steam Input.
- 💡 **On-screen hints follow each player's input:** a controller player sees buttons, a keyboard player sees keys.
- 👥 Split-screen starts when a second player joins (second controller, or Shift+F8 on a second keyboard).
- ↩️ To remove the mod, run **`uninstall.bat`**. It restores every game file the installer changed (they are backed up as `*.prejpogmod.bak`).
- 🐞 Problems? Open an [issue](https://github.com/DanDinosaur/jurassic-park-operation-genesis-split-screen-and-controlling-dinos-mod-/issues) and attach `jpogmod.log` from your game folder.

Full controls for controllers and keyboards are below ⬇️

---

## What the mod adds

- **Full Xbox-style controller play**: no mouse needed (built and tested on a ROG Ally docked to a TV).
- **Keyboard + mouse for both players**: player 2 can use a second keyboard + mouse with their own pointer and toolbar.
- **2-player split-screen co-op**: one shared park and budget. Each player has their own camera, their own build / Manager / Dinosaurs / Actions menus and their own windows.
- **Ride and fight as any dinosaur**: walk, run, strafe, bite, roar, eat, drink, sleep, pounce, finishing moves.
- **Helicopter and building modes for either player**: whoever presses gets the helicopter, safari / balloon track, turret or security camera.
- **Widescreen** (Hor+) with the HUD pinned to the screen edges, built in.
- Optional **easy start**: all research and dig sites, every dinosaur species in the hatchery, parks start at 5 stars,
  money never runs out.

> Not affiliated with or endorsed by Universal, Blue Tongue, Vivendi or anyone who owns Jurassic Park.

---

## Controllers, keyboards, or both

Each player picks their own input. Every mix works: two controllers, two keyboards + mice, or one of each.
Hints on screen always use the words of the player who sees them (buttons or keys).

### Two players on keyboards + mice (no controllers)
You need **two keyboards and two mice** plugged into the same PC (for example a laptop's keyboard + touchpad, and a USB
keyboard + mouse).
1. Start the game and load a park. **Player 1** plays with the first keyboard + mouse, exactly like the original game.
2. **Player 2 joins:** on the second keyboard press **Shift+F8**, then click once with the second mouse
   (player 1: don't click during those few seconds).
3. The screen splits. Each player has their own half, camera, pointer and menus.

**Only needed the first time:** the mod remembers player 2's keyboard + mouse (saved in `jpogmod.ini`, `[pc2]`) and
starts them by itself next time, as long as they stay in the same USB ports and fewer than two controllers are connected.
A new keyboard / mouse, or a different USB port, = Shift+F8 + one click again.

**F8** on a keyboard switches that player between keyboard + mouse and a controller at any time.

### Player 2 on keyboard + mouse - keys
Player 2 uses the game's normal PC keys on their own keyboard, in their own half:

| Key / mouse | Player 2 |
|---|---|
| W A S D / arrows | move the view (the pointer at the edge of your half scrolls too) |
| Q / E, R / F, mouse wheel | rotate, zoom |
| Right button drag / Ctrl + mouse | rotate and zoom with the mouse |
| Middle button drag / Shift + mouse | pan with the mouse |
| Left click | select / place / press a button in your menus and windows |
| Right click | back (in your menus and build tools) |
| Esc | close your menu / window / tool; with nothing open = the pause menu |
| B (or P) | your Build menu |
| F1 / F3 / F4 / F5 / M | your Manager menu (objectives, fossil hunt, DNA lab, fossil market, mail) |
| G | your Dinosaurs menu |
| Space / C | your Actions menu |
| [ / ] | previous / next |
| F2 | Research |
| Tab | tactical map |
| T | take control of your selected dinosaur (or open the info of what you point at) |

Player 2 also gets a round toolbar in their half (Build / Manager / Dinosaurs / Actions / Options + mail): click it.

### Player 1 on keyboard + mouse
Player 1's keyboard and mouse work exactly like the original game, plus **T** to take control of the selected dinosaur
and the riding keys (see "Riding dinosaurs"). With no controller connected the game plays like the normal PC game
(free mouse, PC menus); connect a controller and the mod switches to controller style by itself.

---

## Requirements

- Jurassic Park: Operation Genesis for PC (the retail `SimJP.exe`; the mod's engine addresses are for that version).
- Windows 10 / 11.
- For split-screen, one of: two Xbox (XInput) controllers, two keyboards + two mice, or a controller + a keyboard and mouse.
  Playing alone works with a controller or the normal keyboard + mouse.

## Install

1. Download the release zip and unzip it anywhere, for example next to your game folder.
2. Double-click **`install.bat`**. It finds `SimJP.exe` by itself, or asks you once for the folder.
   - The first time, it asks **"Easy start?"**: `Y` = all research and dig sites, every species in the hatchery, parks start
     at 5 stars, money never runs out.
     `N` = the normal game. You can change it later in `jpogmod.ini` (`[unlocks]`).
   - You can run it again any time (for example after updating). Your settings are kept, and new settings are added.
3. Play with **`Launch JPOG.bat`**, or start the game as usual.

**Uninstall:** `uninstall.bat` removes the mod and restores every original file.

### Widescreen
The mod has its own widescreen (Hor+ field of view, HUD and menus not stretched, HUD pinned to the screen edges, and
in split-screen to each player's half), plus a few comfort extras (Alt+F4, borderless window, mouse speed, smooth camera
at any frame rate). Settings: `[hud]`, `[display]`, `[mouse]`, `[camera]` in `jpogmod.ini`.

If you already use another widescreen plugin (`JPOGMouseFix.asi`), the mod's own widescreen and extras switch themselves
off while that file is in the game folder. Rename it to `JPOGMouseFix.asi.off` to use the mod's own.

---

## Split-screen co-op

- **Split-screen starts when a second player joins:** load a park, then switch on a second controller (the screen splits
  after about 2 seconds) or press **Shift+F8** on a second keyboard and click with a second mouse. When player 2's
  controller goes away, it goes back to one player.
- Default: **left / right (vertical)**. To change it: Start → Options → **"Split: Vertical / Horizontal"**, or press **F12**.
- Controllers follow **who is connected, not the USB / Bluetooth slot**. The first connected controller is player 1 and the
  second is player 2. If player 1's controller is replaced (for example a ROG Ally's built-in pad switches off when a
  Bluetooth pad connects), the new one simply becomes player 1. No remapping, and no accidental player 2.
- One shared park and one shared budget. Each player has their own camera and aiming circle (blue = player 1,
  yellow = player 2).
- **Player 1** has the game's own HUD and menus. **Player 2** has their own copy of the build, Manager, Dinosaurs and
  Actions menus and their own windows, in their half. You can both build at the same time.
- When one player rides a dinosaur, the other player gets the game HUD automatically.
- Hints and messages appear in the half of the player they are for.
- If both players want the same one-of-a-kind thing (the same window, or demolish mode), the game says
  "Player N is using X - only one player at a time".
- (F11 forces split-screen on or off for testing.)
- Player 2 can also play on their own keyboard + mouse: **Shift+F8** on the second keyboard (see "Controllers, keyboards, or both").

---

## Controller - building the park (console style, the default)

| Button | Player 1 (game HUD) | Player 2 |
|---|---|---|
| Left stick / D-pad | move the view; move the highlight in menus | move your own view; menus |
| Right stick | rotate (left/right) and zoom (up/down) | rotate / zoom your own view |
| A | select / place / confirm (aim with the middle of your view) | select the dinosaur (your camera flies to it) or open the building at the middle of your view; pick / place in your menus |
| B | back / cancel | back / close |
| X | Build menu | your Build menu |
| Y | Manager menu | your Manager menu |
| LT | Dinosaurs menu | your Dinosaurs menu |
| RT (tap) | Actions menu (in other screens: RT = select) | your Actions menu |
| RT (hold) + left stick | pan the camera quickly | - |
| LB / RB | previous / next, rotate a building while placing it | rotate a building / change brush size |
| Start | Start menu (save / load / options / quit) | Start menu (borrowed, then handed back to player 1) |
| Back (tap) | Research | - |
| Left-stick click | tactical map | - |
| Right-stick click | switch console style ↔ PC style (stick = mouse cursor, RT = left click, LT = right click) | - |
| RB + D-pad Up | **take control of the selected dinosaur** | take control of your selected dinosaur |

**Ring menus (Start, Manager, ...):** push the stick or D-pad toward an option, A = choose.

**Windows** (Finance, buildings, hatchery, info bar, ...): D-pad or left stick moves a highlight over the buttons,
**A** presses it, **B** closes the window. Holding a direction repeats. On pickers and sliders (for example the hatchery's
Category / Species / Release mode), **left / right changes the value**.

**Player 2 tools:** placing = D-pad ◀ ▶ change item, LB / RB rotate, A place, B close. Paths and fences = A start / A finish,
B cancel. Trees = A plant, Y remove, LB / RB brush size. Terrain = A shape the land, LB / RB size. Demolish = press A twice
to confirm.

**Screens with no buttons to highlight** (for example the fossil hunt map): the left stick moves a pointer and A clicks.

## Riding dinosaurs

**How to take control:**
1. Aim at a dinosaur and press **A** (keyboard + mouse: click it). It is selected, and a message tells you the button / key.
2. Press **RB + D-pad Up** (keyboard: **T**). You now control it, and the controls appear on screen.
3. **B** (keyboard: **T** again) lets go and takes you back to building.

Player 2 does the same with their own controller: aim + **A** selects (player 2's camera flies down to it and follows it
until you move the stick), then **RB + D-pad Up** takes control. On keyboard + mouse: click the dinosaur, then **T**.

| Controller | Keyboard (player 1) | Keyboard + mouse (player 2) | Action |
|---|---|---|---|
| Left stick | I / K / J / L | W / S / A / D | top half = walk straight where the camera looks (the heading stays until you turn with the camera or LB / RB); full left / right = side-step; down = step back |
| Left-stick click | Right Shift | Right Shift | run on / off |
| Right stick | Numpad 4 / 6 / 8 / 2 (playing alone: Q / E / R / F) | mouse | camera |
| D-pad Up / Down | Numpad + / - | mouse wheel | zoom in / out |
| D-pad Left / Right | - | - | camera up / down |
| RT | U | left click | bite / attack (a dinosaur in front of the mouth; bite a fence 5 times to break it) |
| RB (hold) | H (hold) | H (hold) | turn right on the spot |
| RB (tap) | H (tap) | H (tap) | finishing move (next to another dinosaur) |
| LT | O | middle click | pounce / special move (Raptor leap, rear-up, ...) |
| Y | Y | Y | roar (hold = second call) |
| X | N | N | eat (big carnivores next to a goat or cow: grab and swallow) |
| A | G | G | drink (next to water) |
| LB (hold) | - | - | turn left on the spot |
| Right-stick click | Z | Z | lie down and sleep |
| B | T | T or Esc | let go |

Riding ends by itself if the dinosaur dies, you leave the park, or split-screen ends. Nobody can take a dinosaur the
other player is riding. Both players can ride at the same time, and each gets their own chase camera.

## Vehicles and mini games

Ranger helicopter and jeeps: the sticks fly, drive and aim. **RT** = fire, **LT** = the right mouse button action.
To fly the helicopter with a controller: select the helicopter (or open its helipad's window) and press **RB + D-pad Up**.
In split-screen the player who presses gets it (the game HUD moves to them).
Building buttons that start a mode of their own (lay a safari or balloon track, ride photos, turret, security camera) work
from either player's window in split-screen: whoever presses the button gets that mode (the HUD moves to them).

## Keyboard keys added by the mod

| Key | What it does |
|---|---|
| T | take control of the selected dinosaur / let go (both players) |
| I J K L, Right Shift, U H O Y N G Z, Numpad 2 4 6 8 + - | riding (see the riding table) |
| F8 | switch this keyboard's player between controller and keyboard + mouse |
| Shift+F8 | on a second keyboard: player 2 joins on keyboard + mouse (first time only) |
| F11 | force split-screen on / off (testing) |
| F12 | split vertical ↔ horizontal |

Every other key works as in the original game (WASD / arrows = camera, Q / E rotate, R / F zoom, B build, F1-F5 menus, ...).
All of the mod's riding keys can be changed in `jpogmod.ini` → `[keys]`; the on-screen hints show the keys you set.

---

## Settings - `jpogmod.ini` (in the game folder)

Every setting is explained by a comment in the file. Useful ones:

- `[controller]`: `console_mode` (1 = controller style, 0 = PC style), `vibration`, `deadzone`, `window_spinners`
- `[coop]`: `split_orientation`, `auto_split`, `zoom_out_mult`, `p2_repair`, `state_guard` (1 = building buttons can never unload the park - crash fix), `state_guard_handover` (the presser gets the new mode)
- `[pc2]` / `[pc2ptr]` (new in v3): player 2 keyboard + mouse - `p1_input` / `p2_input`, the join key, the saved `p2_keyboard` / `p2_mouse`, pointer, toolbar
  (`hud_buttons`, `hud_scale`), `fov_horizontal` / `fov_scale` if the pointer picks beside the target
- `[dino]`: `a_takes_control` (1 = A takes control at once instead of RB + D-pad Up), `select_hint`, `attack_range`, `run_mult`,
  `rb_turn` / `rb_turn_speed`, `lb_turn`, `fence_break` / `fence_bites`, `building_bites`, `eat_people`, camera options,
  `stick_lock` (1 = the walk heading is locked when you push forward; only the right stick / LB / RB turn) / `stick_side_band`, `stick_cone` / `stick_hard_turn` (used when stick_lock = 0)
- `[vehicle]`: `heli_pad_fly` (RB + D-pad Up flies the helicopter), `heli_presser` (the presser gets it), `hide_pointer`
- `[hud]` / `[display]` / `[mouse]` / `[camera]`: widescreen, HUD size in split-screen, Alt+F4, borderless, mouse speed, camera smoothing
- `[unlocks]`: easy start on / off (`all_research`, `all_dig_sites`, `mr_dna` = every species in the hatchery, `infinite_money`, ...)
- `[keys]`: the keyboard keys above
- `[general]`: `fmod_stream_guard` (1 = survive a rare crash in the game's sound library), `log` - the mod writes `jpogmod.log` in the game folder. Include it when you report a bug.

---

## Known limits

- Another widescreen plugin (`JPOGMouseFix.asi`) in the game folder switches the mod's own widescreen off (see "Widescreen").
- Player 2 on keyboard + mouse needs a second physical keyboard AND mouse (tested on Windows 11 with a laptop keyboard +
  touchpad for player 1 and a USB keyboard + Bluetooth mouse for player 2). Two players on keyboards have been tested
  less than controllers - reports welcome.
- Some game windows exist only once in the game (for example the Start menu and full-screen reports), so only one player
  can have them open at a time.
- The safari jeep ride with a controller has not been fully tested yet - reports welcome.
- Built and tested on one setup (ROG Ally + TV, 1920x1080, Windows 11). Please report problems with your `jpogmod.log`.

## Building from source

See [BUILD.md](BUILD.md). The source is in `src/`. It is a `dinput8.dll` proxy that hooks the game at run time; no game
code or data is included.

## Credits

- Jurassic Park: Operation Genesis - Blue Tongue Entertainment / Universal Interactive (2003).
