# Jurassic Park: Operation Genesis - Split-screen co-op & dinosaur riding mod (JPOG Co-op v2)

**Play JPOG (PC, 2003) like a console game: Xbox controller support, 2-player split-screen on one PC, and take control of any dinosaur to walk, fight, roar, eat and sleep.**

## ⬇️ [Download the latest release](https://github.com/DanDinosaur/jurassic-park-operation-genesis-split-screen-and-controlling-dinos-mod-/releases/latest) → `JPOG_Coop_v2.zip`

### Quick start
1. **You need your own copy of Jurassic Park: Operation Genesis for PC.** No game files are included in this mod.
2. Unzip `JPOG_Coop_v2.zip` anywhere and double-click **`install.bat`**. It finds the game by itself and asks once whether you want the "easy start".
3. Start the game with **`Launch JPOG.bat`**.
4. **For split-screen, connect TWO controllers.** Load a park, then switch on the second controller. The screen splits after about 2 seconds.
5. **To ride a dinosaur:** aim at it, press **A** to select it, then press **RB + D-pad Up** (keyboard: **T**). **B** (or T) lets go.

### Before you play - important
- 🎮 **Built for Xbox (XInput) controllers, NOT keyboard and mouse.** Keyboard and mouse still work like the original game, but the
  mod's features are made for controllers. **Player 2 must use a controller.** PlayStation / Switch pads: use Steam Input.
- 👥 **Split-screen only turns on when two controllers are connected.** With one controller you play alone.
- ↩️ To remove the mod, run **`uninstall.bat`**. It restores every game file the installer changed (they are backed up as `*.prejpogmod.bak`).
- 🐞 Problems? Open an [issue](https://github.com/DanDinosaur/jurassic-park-operation-genesis-split-screen-and-controlling-dinos-mod-/issues) and attach `jpogmod.log` from your game folder.

Full controls for controller and keyboard are below ⬇️

---

## What the mod adds

- **Full Xbox-style controller play**: no mouse needed (built and tested on a ROG Ally docked to a TV).
- **2-player split-screen co-op**: one shared park and budget. Each player has their own camera, their own build / Manager / Dinosaurs / Actions menus and their own windows.
- **Ride and fight as any dinosaur**: walk, run, strafe, bite, roar, eat, drink, sleep, pounce, finishing moves.
- **Widescreen** (Hor+) with the HUD pinned to the screen edges, built in.
- Optional **easy start**: all research and dig sites, every dinosaur species in the hatchery, parks start at 5 stars,
  money never runs out.

> Not affiliated with or endorsed by Universal, Blue Tongue, Vivendi or anyone who owns Jurassic Park.

---

## ⚠️ Made for controllers, not keyboard and mouse

This mod was designed and tested for **Xbox controllers (XInput)**. The keyboard and mouse still work the way the
original game does, and a few keyboard keys were added for riding dinosaurs. But many parts of the mod
(split-screen, player 2, menus, dinosaur riding) are built around controllers and work best with them.

- **Player 2 is controller only.** The game has only one mouse cursor and one keyboard, and they always belong to player 1.
- **No controller connected** = the game plays like the normal PC game (free mouse, PC menus). The mod switches back to
  controller style by itself when you connect one.
- PlayStation / Switch controllers: use them through **Steam Input** or another XInput translator.

---

## Requirements

- Jurassic Park: Operation Genesis for PC (the retail `SimJP.exe`; the mod's engine addresses are for that version).
- Windows 10 / 11.
- One Xbox (XInput) controller, two for split-screen.

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

- **Split-screen starts only when TWO controllers are connected.** Load a park, then connect or switch on the second
  controller. After about 2 seconds the screen splits. Unplug or switch off a controller and it goes back to one player.
- Default: **left / right (vertical)**. To change it: Start → Options → **"Split: Vertical / Horizontal"**, or press **F12**.
- Controllers follow **who is connected, not the USB / Bluetooth slot**. The first connected controller is player 1 and the
  second is player 2. If player 1's controller is replaced (for example a ROG Ally's built-in pad switches off when a
  Bluetooth pad connects), the new one simply becomes player 1. No remapping, and no accidental player 2.
- One shared park and one shared budget. Each player has their own camera and aiming circle (blue = player 1,
  yellow = player 2).
- **Player 1** has the game's own HUD and menus. **Player 2** has their own copy of the build, Manager, Dinosaurs and
  Actions menus and their own windows, in their half. You can both build at the same time.
- When one player rides a dinosaur, the other player gets the game HUD automatically.
- If both players want the same one-of-a-kind thing (the same window, or demolish mode), the game says
  "Player N is using X - only one player at a time".
- (F11 forces split-screen on or off for testing, but player 2 can only play with a controller.)

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
1. Aim at a dinosaur (or click it) and press **A**. It is selected, and a message tells you the button combo.
2. Press **RB + D-pad Up** (keyboard: **T**). You now control it, and the controls appear on screen.
3. **B** (keyboard: **T** again) lets go and takes you back to building.

Player 2 does the same with their own controller: aim + **A** selects (player 2's camera flies down to it and follows it
until you move the stick), then **RB + D-pad Up** takes control.

| Controller | Keyboard (player 1) | Action |
|---|---|---|
| Left stick | I / K / J / L | walk (the dinosaur turns to where the camera looks) / step back / step sideways |
| Left-stick click | Right Shift | run on / off |
| Right stick | Numpad 4 / 6 / 8 / 2 (playing alone: Q / E / R / F) | camera |
| D-pad Up / Down | Numpad + / - | zoom in / out |
| D-pad Left / Right | - | camera up / down |
| RT | U | bite / attack (a dinosaur in front of the mouth; bite a fence 5 times to break it) |
| RB (hold) | H (hold) | turn right on the spot |
| RB (tap) | H (tap) | finishing move (next to another dinosaur) |
| LT | O | pounce / special move (Raptor leap, rear-up, ...) |
| Y | Y | roar (hold = second call) |
| X | N | eat (big carnivores next to a goat or cow: grab and swallow) |
| A | G | drink (next to water) |
| LB | Z | lie down and sleep |
| B | T | let go |

Riding ends by itself if the dinosaur dies, you leave the park, or split-screen ends. Nobody can take a dinosaur the
other player is riding. Both players can ride at the same time, and each gets their own chase camera.

## Vehicles and mini games

Ranger helicopter and jeeps: the sticks fly, drive and aim. **RT** = fire, **LT** = the right mouse button action.

## Keyboard keys added by the mod

| Key | What it does |
|---|---|
| T | take control of the selected dinosaur / let go |
| I J K L, Right Shift, U H O Y N G Z, Numpad 2 4 6 8 + - | riding (see the riding table) |
| F11 | force split-screen on / off (testing) |
| F12 | split vertical ↔ horizontal |

Every other key works as in the original game (WASD / arrows = camera, Q / E rotate, R / F zoom, B build, F1-F5 menus, ...).
All of the mod's keys can be changed in `jpogmod.ini` → `[keys]`.

---

## Settings - `jpogmod.ini` (in the game folder)

Every setting is explained by a comment in the file. Useful ones:

- `[controller]`: `console_mode` (1 = controller style, 0 = PC style), `vibration`, `deadzone`, `window_spinners`
- `[coop]`: `split_orientation`, `auto_split`, `zoom_out_mult`
- `[dino]`: `a_takes_control` (1 = A takes control at once instead of RB + D-pad Up), `select_hint`, `attack_range`, `run_mult`,
  `rb_turn` / `rb_turn_speed`, `fence_break` / `fence_bites`, camera options
- `[hud]` / `[display]` / `[mouse]` / `[camera]`: widescreen, HUD size in split-screen, Alt+F4, borderless, mouse speed, camera smoothing
- `[unlocks]`: easy start on / off (`all_research`, `all_dig_sites`, `mr_dna` = every species in the hatchery, `infinite_money`, ...)
- `[keys]`: the keyboard keys above
- `[general]`: `log` - the mod writes `jpogmod.log` in the game folder. Include it when you report a bug.

---

## Known limits

- Another widescreen plugin (`JPOGMouseFix.asi`) in the game folder switches the mod's own widescreen off (see "Widescreen").
- Player 2 has no keyboard or mouse controls.
- Some game windows exist only once in the game (for example the Start menu and full-screen reports), so only one player
  can have them open at a time.
- Built and tested on one setup (ROG Ally + TV, 1920x1080, Windows 11). Please report problems with your `jpogmod.log`.

## Building from source

See [BUILD.md](BUILD.md). The source is in `src/`. It is a `dinput8.dll` proxy that hooks the game at run time; no game
code or data is included.

## Credits

- Jurassic Park: Operation Genesis - Blue Tongue Entertainment / Universal Interactive (2003).
