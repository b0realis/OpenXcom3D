# OpenXcom3D manual

Version 1.01. OpenXcom3D is **OpenXcom Extended (OXCE)** with a first-person 3D battlescape: an engine add-on (`src/FirstPerson/`, with small hooks in the core) and a data mod (`OpenXcom3D`) that switches it on.

1. [Installing](#1-installing)
2. [Controls](#2-controls)
3. [The top bar and items](#3-the-top-bar-and-items)
4. [Settings](#4-settings)
5. [Fire modes per weapon (FPS-style shooting)](#5-fire-modes-per-weapon-fps-style-shooting)
6. [Night vision gear](#6-night-vision-gear)
7. [What you'll see](#7-what-youll-see)
8. [Performance](#8-performance)
9. [Known limits and ideas](#9-known-limits-and-ideas)
10. [For developers](#10-for-developers)
11. [Version history](#11-version-history)
12. [Thanks](#12-thanks)

## 1. Installing

You need your own copy of the original game data: *UFO: Enemy Unknown / X-COM: UFO Defense* and/or *X-COM: Terror From The Deep*, e.g. from Steam or GOG.

Add-on builds keep their own user and config folders, so an existing OXCE profile is never touched:

- the release's `user/` folder, when started with `play.sh`;
- `~/.local/share/openxcom3d` for the AppImage;
- otherwise `~/.local/share/openxcom3d` and `~/.config/openxcom3d`.

### Option A: the prebuilt release (Linux x86-64)

Download it from the project's [Releases](https://github.com/b0realis/OpenXcom3D/releases) page, in either form:

- **`OpenXcom3D-1.01-x86_64.AppImage`**: a single file. Make it executable (`chmod +x OpenXcom3D-1.01-x86_64.AppImage`, or *Properties → Permissions → executable* in a file manager) and run it. Its profile is in `~/.local/share/openxcom3d`.
- **`OpenXcom3D-1.01-linux-x86_64.tar.gz`**: unpack it anywhere and run `./play.sh`. Its profile is the `user/` folder inside.

**Nothing needs installing.** The game comes with its own libraries: SDL 1.2 (sdl12-compat on SDL2), SDL_image and SDL_mixer in `lib/`, with SDL_gfx built in. All it takes from the system is glibc 2.34 or newer (2021 on: Ubuntu 22.04 and newer, Debian 12, Fedora 35 and newer, RHEL 9, Mint 21, every current Arch, Manjaro or other rolling system, and SteamOS), plus the display, sound and OpenGL every desktop has. SDL opens X11 or Wayland and PulseAudio, PipeWire or ALSA when the game starts. On a system older than glibc 2.34, build it from source (option B).

The archive holds:

```
bin/openxcom              the engine (OXCE 8.7.1 + OpenXcom3D)
lib/                      the SDL libraries it comes with
data/common, standard     OXCE's own data
user/mods/OpenXcom3D      the mod
user/options.cfg          a starting profile: UFO + OpenXcom3D, 1280x800 window
play.sh                   starts the game (finds the game data, says what's missing)
steamdeck/                the Steam Deck controller layout, and the script that sets it
licenses/                 the licences of the libraries in lib/
install.py, README.md, OpenXcom3D_manual.md, openxcom3d.patch, LICENSE.txt
```

#### The game data

`play.sh` (and the AppImage) finds the original games by itself the first time, and links them into the profile. It never copies or changes them. It looks, in this order, for:

1. a `UFO` or `TFTD` folder already in the profile (`user/UFO`, or `~/.local/share/openxcom3d/UFO` for the AppImage);
2. a `UFO` or `TFTD` folder in the game's folder, next to it, or in your home folder;
3. *X-COM: UFO Defense* and *X-COM: Terror From The Deep* in a Steam library. That covers Steam itself, installed natively, as a Flatpak or as a Snap, and every other library it lists, such as a second disk or the Steam Deck's SD card.

A game folder is the one with `GEODATA`, `GEOGRAPH`, `MAPS` and `UFOGRAPH` in it (Steam's are `XCom UFO Defense/XCOM` and `X-COM Terror from the Deep/TFD`). A new profile switches on the game that's there, UFO if both are. If neither is found, it says where to put them.

#### With the installer

```sh
python3 install.py
```

It asks for:

- the install folder (default `~/Games/OpenXcom3D`);
- the original UFO data folder;
- the TFTD data folder.

Common Steam locations are found on their own.

It then copies the game and its libraries, **links** the game data folders into the profile, writes `options.cfg` and creates `play.sh`. Without asking:

```sh
python3 install.py --target ~/Games/OpenXcom3D --ufo /path/to/UFO --tftd /path/to/TFTD --yes [--desktop]
```

`--desktop` also adds a menu entry.

#### By hand

Unpack the archive anywhere and run `./play.sh`. To use game data it doesn't find, link it into the profile first:

```sh
ln -s /path/to/UFO  OpenXcom3D-1.01-linux-x86_64/user/UFO
ln -s /path/to/TFTD OpenXcom3D-1.01-linux-x86_64/user/TFTD
```

#### Steam Deck

1. In **Desktop Mode**, download the archive (or the AppImage) and unpack it, for example into `~/Games`. Run `play.sh` once; double-click it and choose *Execute*, or start it from Konsole. If *X-COM: UFO Defense* or *Terror From The Deep* is installed through Steam, it is found by itself. Otherwise copy the game's folder next to `play.sh` as `UFO` (or `TFTD`).
2. For **Game Mode**: Steam → *Games* → *Add a Non-Steam Game to My Library* → *Browse*, set the file type to *All Files*, pick `play.sh` (or the AppImage) and add it. It's then in your library under *Non-Steam*.
3. The **controller layout**: after adding the shortcut, run `steamdeck/add_controller_layout.sh` once in Desktop Mode (from Konsole, or double-click → *Execute*). It closes Steam for a moment, adds the layout to Steam's templates, sets it for your OpenXcom3D shortcuts (it keeps a copy of Steam's file it changes, `configset_controller_neptune.vdf.before-openxcom3d`), and starts Steam again. Back in Game Mode, OpenXcom3D uses it; it can also be picked by hand under *Edit Layout → Templates → OpenXcom3D*. Run the script again after adding another OpenXcom3D shortcut, or after updating OpenXcom3D (to get the new version's layout). The layout is Valve's *Keyboard (WASD) and Mouse* template with this game's keys:

| Deck | Key | Does |
|---|---|---|
| Left stick, click | W A S D, K | walk and step sideways; kneel |
| Right trackpad, click | mouse, left button | look (or move the cursor); click |
| Right stick, click | mouse, left button | look more finely; click |
| R2 / L2 | left / right mouse button | fire (click) / cancel (with a free cursor: hold to look) |
| L1 | C | mouse look ↔ free cursor |
| R1 | F8 | isometric map ↔ 3D |
| A / B / X / Y | Space / R / J / I | jump / reload / next fire mode / inventory |
| D-pad up / down | Page Up / Page Down | fly up / down, ride a grav lift |
| D-pad right / left | Tab / G | next soldier / switch hand |
| D-pad left / right, held | Y / F | akimbo on/off / the flashlight on/off |
| Left trackpad, clicked: up / right / down / left | Z / X / V / O | snap / aimed / auto shot / open the door |
| Left trackpad, clicked and held: left / right | , / . | snap turn 45° left / right |
| Menu (☰, right) / View (⧉, left) | Escape / H | the game's menu / classic HUD |
| View, held | P | 3D HUD on/off |
| L4 / R4, held | Left Shift + A / D | lean left / right (with the setting *Lean* on) |
| L5 / R5 | N / T | night vision / tactical flare |
| L5 / R5, held | L / Backspace | the suit light (the game's personal light) / end turn (held, so a press a little short never ends the turn) |

A button with something on a long press (held for 0.4 s) sends its own key when it's let go, a little later than the others. On the isometric map the left stick sends the game's own W, A and S (the special item, abort with a question first, stats) and X sends J (the kneel reserve): move the map with the trackpad there.

On the Deck a new profile plays fullscreen (1280×800, the Deck's own screen). Steam + X brings up the keyboard for anything else. Without the layout, Valve's *Keyboard (WASD) and Mouse* template works too, with F8 mapped to a spare button.

#### If it doesn't start

`play.sh` says what's wrong, in a window when it wasn't started from a terminal:

- *older than glibc 2.34*: build it from source (option B);
- *lacks libGL.so.1*: the system has no OpenGL library, which comes with the graphics drivers (`libgl1` on Debian and Ubuntu, `libglvnd` on Arch, `libglvnd-glx` on Fedora);
- *needs the data of the original game*: see [The game data](#the-game-data).

If the window closes at once with an X error about GLX, the system's OpenGL is broken. It runs without it like this:

```sh
SDL12COMPAT_OPENGL_SCALING=0 SDL_RENDER_DRIVER=software ./play.sh
```

### Option B: build from source

#### With the installer

```sh
python3 install.py --from-source [--workdir ~/.cache/openxcom3d-build] [--jobs 8]
```

1. It checks the build tools; if something is missing it prints the install command for Debian/Ubuntu, Arch or Fedora.
2. It clones `oxce-plus` from GitHub and checks out the commit above.
3. It applies `openxcom3d.patch` and builds with CMake (Release).
4. It installs the result like option A.

`--repo` uses another clone, e.g. a local mirror.

#### By hand

The build tools and libraries:

| System | Command |
|---|---|
| Debian, Ubuntu, Mint | `sudo apt install git cmake g++ make pkg-config libsdl1.2-dev libsdl-mixer1.2-dev libsdl-image1.2-dev libsdl-gfx1.2-dev zlib1g-dev libglu1-mesa-dev` |
| Arch, Manjaro | `sudo pacman -S --needed base-devel git cmake sdl12-compat sdl_image sdl_mixer sdl_gfx glu zlib` |
| Fedora | `sudo dnf install git cmake gcc-c++ make pkgconf sdl12-compat-devel SDL_image-devel SDL_mixer-devel SDL_gfx-devel zlib-devel mesa-libGLU-devel` |

Then:

```sh

git clone --branch oxce-plus https://github.com/MeridianOXC/OpenXcom.git
cd OpenXcom
git checkout f0134ef8065799c7723953d64b565f7832bfd782
git apply /path/to/openxcom3d.patch
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDEV_BUILD=OFF -DBUILD_PACKAGE=OFF
cmake --build build -- -j8
```

The binary and OXCE's data are in `build/bin/`, and the mod is in `mod/OpenXcom3D/`. To lay them out:

1. Put the mod in a profile's `mods/` folder.
2. Link `UFO`/`TFTD` into that profile.
3. Start with:
   ```sh
   build/bin/openxcom -data build/bin/ -user <profile>/ -cfg <profile>/
   ```

`install.py --from-source` does the same.

Notes:

- OXCE bundles rapidyaml, so yaml-cpp isn't needed (old forum guides say otherwise).
- On distributions with SDL3, `libsdl1.2-dev` is sdl12-compat over SDL2/3, and it works.

### Playing

- Start `./play.sh` (or the AppImage), begin a game (or **New Battle**), and in battle press **F8** or click the **3D** button at the top left.
- **Options → 3D VIEW** has the settings and the key bindings.
- [Controls](#2-controls) below.

#### TFTD

1. Open **Mods**.
2. Choose *Terror From The Deep* as the master.
3. Keep *OpenXcom3D* enabled.

#### Screen size

- The default is a 1280×800 window with the battlescape at half the screen size (640×400), which keeps 60 FPS on modest machines.
- Bigger screens work, but the game's **Battlescape scale** (video options) sets the 3D view's size. At *screen/2* (the default) a 1920×1080 screen gives a 960×540 view, twice the work of 640×400. At *screen* the view stays at 640×360 with its controls three times bigger, but the isometric map gets small.
- **Pixel size** in 3D VIEW trades detail for speed. Its default, **Auto**, takes care of big screens: on a 1920×1080 screen with the default scale it draws blocks of 2, which holds 60 FPS and looks almost the same.

### Uninstalling

Delete the install folder (and `~/.local/share/applications/openxcom3d.desktop` if you made a menu entry). Your game data folders were only linked, never changed.

## 2. Controls

All of these can be rebound in **Options → 3D VIEW → Controls**: click a key's row, then press the new key (Shift, Ctrl and Alt can't be bound); a right click clears it. That page also lists (under a heading) the game's own keys that work in 3D: turning, kneel, reload, inventory, hands, the suit light (the game's personal light), night vision, next/previous soldier, end turn, frame counter, quick save/load and the ten enemy keys.

| Input | Action |
|---|---|
| **F8** | Isometric ↔ 3D (or the ISO/3D button); back on the map, it's centred on your soldier |
| **Mouse** | Look around; the soldier turns with you for free (or by the rules: setting *Turning costs TU*) |
| **W A S D** | Walk and step sideways in real time. Each tile costs its normal TUs; walking into a door opens it. With *Movement: Tiles*, a press walks exactly one tile |
| **Numpad 8 2 4 6** | One tile ahead, back, left or right, almost at once, like a step on the isometric map (whatever the *Movement* setting; holding the key steps on). **7 / 9**: snap turn 45° left / right |
| **Left button** | Fire at the crosshair in the current fire mode |
| **Wheel / J** | Previous / next fire mode (snap, aimed, auto, throw, launch, melee, psi, use, prime) |
| **Z / X / V** | Snap / aimed / auto shot |
| **G** | Switch hand: the right hand, the left, and the built-in weapon (the classic HUD's SPECIAL, such as an armor's), in turn, each that gives something to use. The weapon moves to that side of the screen; with akimbo on, it picks which gun fires first. An empty hand gives the unit's empty-hand weapon, where a mod has one (fists, a claw: OXCE's `specialUseEmptyHand`); without one, G skips it, as an empty hand has nothing to use |
| **Y** | Akimbo off, and the same kind back on (full or realistic, see [Settings](#shooting)); the warning line says which |
| **O** | Open the door in front of you without walking through |
| **T** | Throw a tactical flare (with "Unlimited tactical flares" on) |
| **B** | Suit flight on/off (flying armor; SWIM under water) |
| **F** | The flashlight on/off (setting *Flashlight*): the 3D view's own light, separate from the suit light (L) |
| **Space** | Jump: a free hop. Walking into something low (a fence, table, crate, rock: up to waist height), it vaults over it, paying only the normal walk |
| **N** | Night vision on/off, with night vision gear (Scroll Lock, the game's own toggle, works too) |
| **Page Up / Page Down** | Fly up/down (flight on) or ride a grav lift. Standing on a lift, purple arrows with LIFT PLATFORM show which ways it goes; with a free cursor (C), click them to ride |
| **1 … 0** | Turn to visible enemy 1–10 (or click the red enemy buttons) |
| **Right button** | Cancel the current action, or forget a target marked for *Confirm fire*; with nothing to cancel, hold to zoom (setting *Zoom*). With a free cursor: hold to look |
| **U**, held | Zoom: the view glides in to 4 times closer, and back out when you let go (setting *Zoom*). **SCOPING 2.6x** at the right shows how far, and a thin blue cross runs through the crosshair. Aim, shots and hit chances don't change. In 3D, U then isn't the game's UFOpedia key (on the map it is) |
| **C** or **middle button** | Mouse look ↔ free cursor (to click the buttons, hover soldiers for their cards). With *Middle click identifies* on, a middle click on a unit shows it instead, as on the map |
| **H** | Classic HUD on/off: every button of it works in 3D ([below](#the-classic-hud-in-3d)) |
| **P** | 3D HUD on/off, for a clear view: the top bar, the info panel, the compass, the lift arrows and the enemy markers hide (and can't be clicked); the crosshair, the weapon in hand and the messages stay. On the isometric map the top bar shows as ever |
| **Arrow keys** | Turn |
| **, / .** | Snap turn 45° left / right, without stepping (the soldier turns with it) |
| **Left Shift + A / D**, held | Lean out left / right, to peek round a corner, with the setting *Lean* on (otherwise Shift does nothing and A / D strafe, as before): the eye moves to the side, stopping short of walls and of anyone in sight, and with *Tilt* the view tilts. A shot while leaning out starts from that side. Q and E stay the game's hand keys (they don't act while Shift is held, so a lean never opens a menu). The lean key is rebindable (*Lean (hold with strafe)* under Controls, Shift, Ctrl or Alt allowed), and each side can have a key of its own too (*Lean left / right*, unbound) |
| **Insert** | Cheats off, and the same ones back on (Doom mode, Doom IDKFA, Doom IDDQD: see [Settings](#battle)) |

The original hotkeys still work, except for W, A, S, D (on the map: the special item, abort and stats), U (with *Zoom* on) and the keys above. While you lean out (Shift held) they stay quiet, so Q or E don't open a hand's menu in the middle of a lean; the keys above work then too:

- K kneel, I inventory, R reload;
- Backspace end turn;
- Q / E use left / right hand;
- L the suit light: the game's personal light, for the whole squad (the flashlight is F).

A key that can't do its job just now says why on the game's warning line: no flashlight or no flares (their settings off), armor that can't fly, a fire mode the weapon doesn't have, no night vision gear. With the game's Ctrl or Alt held (Ctrl+S and its other shortcuts) the keys are the game's, and W A S D don't walk.

### The classic HUD in 3D

**H** (or the HUD button) puts the game's own bottom HUD over the 3D view, with a free cursor. Every button works there. Those that act on the isometric map are turned into what makes sense in 3D, and so are their keys, with or without the HUD shown:

| Button (key) | On the isometric map | In 3D |
|---|---|---|
| Soldier up / down | Up or down a level (a grav lift, flight) | As Page Up / Page Down in 3D: a lift ride, or flight, which must be switched on (SUIT FLY or B; otherwise "Flight off") |
| Map level up / down (Page Up / Page Down) | Shows the level above / below | Looks 30° up / down, towards that level: the 3D view shows every level anyway. The keys stay the soldier's up and down in 3D |
| Minimap (M) | The minimap, at the map's view | The minimap, on your soldier |
| Centre on soldier (Home) | Centres the map on the soldier | Turns the view back to where the soldier faces, level |
| Red enemy buttons (1 … 0) | Centre the map on that enemy; right click: select the soldier who sees it with the most time units left | Turn the view to that enemy; right click: switch to that soldier, looking at it |
| Layers (OXCE links menu) | The links menu, or (with links off) the multi-level view | The links menu works as on the map; the multi-level view only changes the map, and a line says so |
| Kneel (K) | Kneel / stand | The same; the view drops to kneeling height |
| Next soldier (Tab; right click: previous), next and done (`\`) | Select the next soldier (done: not again this turn) | The same; the 3D view looks through them |
| Hands (Q / E) | The action menu; a target is clicked on the map | The action menu; a shot picked becomes the fire mode and a click on the view fires it; other actions are aimed with a click on the view (a guided missile's waypoints and psi on a creature out of sight on the map). Right click: the hand for reaction fire; middle click: the Ufopaedia |
| Reserve TUs (F1 … F4, J), spend all TUs (Delete) | Time units kept for a shot or kneeling | The same (in 3D, J is the next fire mode: the kneel reserve is its classic HUD button); walking in 3D keeps the reserve |
| Inventory, options, end turn, abort, the soldier's name and bars, special, skills | Their screens and menus | The same |

## 3. The top bar and items

**Top bar**, from left to right:

- **ISO/3D** · **HUD** · **INV**;
- **◄ soldiers ►**: up to 10 shown, the window follows your soldier, and the mouse wheel over the bar scrolls; so does a right click on ◄ or ►, one soldier at a time, without changing who's selected (a left click selects the previous or next soldier). Hovering one shows a card with stats and a live helmet camera. A skull marks the dead, a caduceus the unconscious, and an orange alien face a soldier under alien mind control; A right click on a soldier's button opens their **SOLDIER CAM** (their helmet camera big; see *Soldier camera preview* in [Settings](#4-settings)).
- **SPECIAL** · **NIGHT VISION** · **SUIT LIGHT** · **SUIT FLY**, shown when the soldier has them (labels shorten to SPEC · NV · LIGHT · FLY on a narrow bar);
- **END** turn.

**Info panel**, bottom left (the classic HUD, H, has all of this in its own way):

- the soldier's name, and on the right what matters most just now: PANIC or BERSERK this turn, BURNING, WOUNDS (fatal wounds), or KNEELING;
- a line for each hand. First the weapon that fires, then whatever is in the other hand (grey, or as bright as the first when it fires along, akimbo). A gun's line tells its ammo, for example **`R: Rifle 17 | 2 clips (40)`**:
  - **17** is the rounds in the loaded clip, for the fire mode you picked (a gun with several ammo slots shows the slot that mode uses). **EMPTY** in red means no clip is loaded: the game throws a clip away when its last round is fired, so reload (R, from the belt) or load one in the inventory;
  - **2 clips (40)** is the spare ammo the soldier carries for that gun, anywhere in the inventory (belt, pockets, backpack, the other hand), and how many rounds they hold together. It counts every clip the gun can take by the game's rules, all kinds together (a heavy cannon's AP, HE and incendiary). **no clips** means there's nothing to reload from;
  - guns that never run out, like the lasers, show **∞**; a gun with its rounds built in shows just its count;
  - the numbers follow every round fired, in faithful and FPS-style shooting alike, and every reload, pick-up and drop. Two guns of the same kind (dual pistols) each count the same spare clips, since either could load them;
  - a primed grenade shows **PRIMED 0**: the turns its fuse has left (in Doom mode, the seconds: **PRIMED 2.4s**);
- time units, energy, health (with stun damage over it) and morale, as numbers and bars;
- the armor, and on the right DOOM MODE while that cheat is on (IDDQD and/or IDKFA for its extras; with IDDQD the four stats turn silver).

Hovering a soldier on the top bar shows the same weapon line on their card.

**Items:**

- Every item works from 3D. Its fire modes are on the wheel, the game's own: a gun's shots, and melee for anything with a melee cost (a stun rod, a knife, a gun with a butt stroke in a mod), the empty hand's fists and the built-in weapon included (G picks them).
- **A medi-kit** is used at once, no menu (it had one entry): the patient is the one you point at, among those the game allows: the unit in front of you in reach, its picture under the crosshair; looking down (more than 25 degrees), a body lying at your feet; aiming at nothing in reach, yourself. The crosshair says who: **USE: Fred**, **USE: SELF**. A kit of one kind (heal, stimulant or painkiller only, as in some mods) works there and then, as the game's menu does it; the full Medi-Kit opens the game's own screen on that patient, to pick the body part and the treatment. **The motion scanner** goes straight to its screen too. The mind probe opens its menu, then is aimed like a shot.
- **Anything in a hand shows in 3D**, built from its inventory picture like the weapon in hand: a grenade, a medi-kit, a scanner, a clip... sits in the bottom corner with its lower half out of sight. A unit's built-in items (a fixed weapon such as a tank's cannon, or an armor's special weapon such as a dog's bite) leave the hands empty: they'd look like a weapon held in empty hands. A mod can list ones to show anyway (`builtInItemsShown`, see [For developers](#10-for-developers)).
- **Melee weapons** are held tip up, the way the inventory pictures stand them (the stun rod's prongs, a blade's point, at the top), and a blow swings the tip down at the crosshair and back up.
- **Switching a light on and off** (an item primed to switch on, as in some mods): the fire modes offer **UNPRIME** once it's primed, as the game's menu does.
- **Throwing:** as on the isometric map, anything that isn't built in can be thrown (fire mode **throw**, the arc on the crosshair): a grenade, a clip for a squadmate, even a rifle. It flies along the game's arc as a 3D object, tumbling end over end, and lands where the game says. The game refuses a throw it can't make ("Unable to throw here!"): under a roof (inside the Skyranger, say), past the soldier's range, or with someone right in front.
- **Priming a grenade** (fire mode **prime**): the game's own menu and timer, exactly as on the isometric map: *Prime Grenade*, then the number of turns, 0 to 23. The panel then says **PRIMED** and the turns left. A primed grenade lying on the ground goes off at the end of a turn with its fuse at 0: **0** when you end this turn, **1** at the end of the aliens' turn, **2** at the end of your next one, and so on (every side's turn counts one down). The game's option *Instant grenades* makes a 0 go off on landing instead. A primed grenade still in a hand doesn't go off.
- **In Doom mode with *Doom real-time grenades*** grenades work in real time, like in a first-person shooter, going off a few seconds after they're primed (*Doom grenade fuse*, 3 s by default) wherever they are then. How they're primed is up to *Doom prime on throw*:
  - **YES** (the default): just **throw** one. It's armed as you throw it (you hear the pin) and burns from the moment it leaves your hand, so it never goes off in your hand. **Prime** becomes **FUSE 3s**: it opens a **SET FUSE** box like the map's timer, with 1s to 5s to pick (a click, or the keys 1 to 5), and the seconds you pick hold for every grenade after that until you change them (it's the same as the setting). The panel shows the fuse for a grenade in hand, then counts the seconds down once it's thrown (**PRIMED 2.4s**). A grenade whose throw the game refuses stays in your hand, armed but not burning, until it leaves it.
  - **NO**: **prime** opens the same **SET FUSE** box, and your pick pulls the pin (you hear it): the fuse burns from then on, in your hand too. Throw it in time, or cook it for a shorter flight, but not too long. The panel counts the seconds down.

  With *Doom free grenades with airburst* a grenade flies wherever the crosshair points, and one whose fuse runs out in the air bursts there; without it, the game's own throw, and a fuse that runs out in flight goes off as the grenade lands. Proximity grenades work as the game makes them. This is the first step towards a real-time mode (see [Known limits and ideas](#9-known-limits-and-ideas)).
- The **blaster launcher**, and psi or the mind probe aimed at no one in sight, switch to the isometric map to pick waypoints or a target, then come back.

## 4. Settings

**Options → 3D VIEW → Settings**, from the main menu or in a battle (Esc → Options). A left click flips YES / NO or raises a number; a right click lowers it (choices like Particles go round). The line at the bottom of the page gives a one-line reminder of the setting under the mouse; this is the full story, in the page's groups, with the default in brackets.

### Looking and moving

- **Soldier camera preview** (YES): hovering a soldier's button on the top bar shows a card with their name, stats and weapon. YES: the card also shows that soldier's helmet camera, live: what they are looking at right now (static if they're down). NO: the card without the camera (a camera popped up in the aliens' turn still shows its picture). Either way, a **right click** on a soldier's button opens their **SOLDIER CAM**: the helmet camera big, with a blinking red dot and a little TV static, so it's plain it's a camera, not your view; moving along the bar shows each soldier's in turn (a fallen soldier keeps the card with its static), and moving off the bar or a second right click closes it. You stay with your own soldier all the while.
- **Mouse sensitivity** (5): how fast the view turns with the mouse, from 1 (slow) to 20 (fast).
- **Invert mouse** (NO): YES: moving the mouse forward looks down instead of up, as in a flight simulator.
- **Turning costs TU** (NO): NO: the soldier turns with the view for free. YES: turning is the game's own: the view may look up to 45° off the way the soldier faces (they turn their head, for free); looking further turns the soldier 45° that way at the game's cost (1 TU for a soldier), as on the isometric map, and a turn stops when it spots a new enemy. Without the time units the view stops 45° off. The snap turn keys turn the soldier a step each.
- **Compass** (YES): a compass at the top right. The disc turns as you turn, with N, E, S and W going round it, under a fixed white arrow on top that shows your heading; a yellow triangle on the rim shows the way the soldier faces. With *Turning costs TU* on, a green arc shows how far the head turns for free (45° either side of the facing): within 10° of its end it turns yellow and says what looking further costs (**TURN 1 TU**), or red with **NO TU** when the soldier couldn't pay for the turn. And when a shot or a throw at what's under the crosshair would make the game turn the soldier first (aimed outside the eighth they face), a small orange triangle on the rim shows the way they'll turn, and the line under the dial says what the turn adds to the shot (**SHOT +1 TU**; red if the time units won't cover both). With free turning, the soldier faces the target for free before a shot, so that never costs anything.
- **Field of view** (75): how wide the 3D view is from side to side, in degrees, from 50 (narrow, like a zoom) to 110 (wide), in steps of 5. The weapon in hand keeps its size and place whatever you pick.
- **Zoom** (YES): hold **U**, or the right mouse button in mouse look (when there's no picked action to cancel), to look 4 times closer. The view glides in over about 0.7 seconds and back out when you let go; **SCOPING 2.6x** in blue at the right counts it up and down, a thin blue scope cross runs through the crosshair, the weapon in hand lowers out of the way, and the mouse turns the view that much slower. Only the picture changes: the aim, the shot and the hit chance are the same (the spread ring grows with the picture). NO: U and the right button are the game's again.
- **Middle click identifies** (YES): YES: the middle button works as on the isometric map: a click on a unit you can see (its picture under the crosshair, or the free cursor) opens the game's own unit screen, who it is and what it carries; anywhere else it switches between mouse look and the free cursor, as always. NO: the middle button only switches mouse look and the free cursor.
- **Movement** (Free): how W, A, S and D walk. **Free**: freely, as in a first-person shooter, stepping into the next tile when you cross into it. **Tiles**: a press walks exactly one tile, to the middle of the neighbouring tile that way (one of eight, relative to the view: W ahead, W and D together ahead-right), at walking speed, and holding the key walks on, a tile at a time. Either way the numpad hops a tile at a time, almost at once (8 ahead, 2 back, 4 left, 6 right; 7 and 9 turn), and doors, ledges, jumps, lifts and diagonal gaps work the same.
- **Walking speed** (4): how fast the soldier walks in 3D, in tiles per second, from 1 to 8. Only the pace changes: every tile entered still costs its normal time units.
- **Head bob** (YES): the view bobs gently with the soldier's steps. Turn it off if it makes you queasy.
- **Flight cost per level (TU)** (0): the time units a flying (or, under the sea, swimming) soldier pays to rise or sink one level with SUIT FLY switched on in 3D, from 0 (free) to 24. The original game charges about 8.
- **Flight cost overrides rules** (YES): YES: every flying soldier pays the flight cost above to rise or sink a level. NO: each unit pays what the game charges it (its armor's own cost, 8 for the flying suit), so units that fly slower stay slower.
- **Jump over low obstacles** (YES): YES: jumping (Space) while walking into something low (a fence, a table, a crate, a rock: up to waist height) takes the soldier over it. The jump itself is free: only the walk to the far side costs its normal time units. It bends the rules a little, which is why it's a setting. NO: Space is just a hop, and the original rules decide what blocks you.
- **Falling** (YES): YES: you can walk off a ledge and fall to the level below, by the game's own fall, hurt by the height (a little from one level, a lot from two or more; armor takes some of it). NO: ledges stop you, as the map's paths do.
- **Eye height lowered by** (3): X-COM soldiers fill a level almost to the ceiling, so the 3D camera sits this many voxels lower (about 8 cm each), from 0 to 6, and rooms feel less cramped. 0 is the game's own eye height. What the soldier can see by the game's rules doesn't change.

### Shooting

- **Faithful X-COM targeting** (YES): YES: every shot uses the game's own accuracy, and the hit chance shows by the crosshair. **NO: FPS-style shooting.** Rounds fly along the crosshair with a spread (the soldier's firing skill and kneeling tighten it; the dotted ring shows it), recoil lifts your aim with every round and eases back, and each round flies at a bullet's pace. The game's three fire-mode slots become: **quick = BURST** (one burst, the game's auto shot with its rounds; a gun without auto keeps its single quick shot), **AIMED** = one tight round, **AUTO** = hold the trigger to keep firing until you let go, the clip is empty or the time units run out. Each gun offers only the modes it has in the game ([table](#5-fire-modes-per-weapon-fps-style-shooting)). Time units, ammo and damage stay the game's.
- **Full akimbo** (NO): YES: a soldier with a gun in each hand fires both at the same moment, round for round, on every shot (each gun with its own fire mode, time units and ammo; in a burst the other gun's rounds go with the first ones), but each is half as accurate (FPS style: twice the spread). The other gun fires the same mode if it has it, otherwise its first shot, never more rounds than the chosen gun. Without the time units for both, only the chosen hand fires, at full accuracy. The 3D view shows the second gun on the other side, and the info panel shows both guns' ammo. Two-handed weapons also take the game's own one-handed penalty. NO: one hand at a time, as in the game (G switches hands).
- **Melee hits walls and downed** (NO): with no one standing in reach, a melee blow hits what the soldier faces, as the isometric map's terrain melee does (in a mod that switches it on, the 3D view does so whatever this setting): a wall, door or object straight ahead (not diagonally), damaged by the game's own terrain damage if the weapon can damage terrain (a stun rod can't). YES also switches that on for the original games, and lets a blow hit a unit lying unconscious in front of the soldier or at their feet (it can't dodge; the game's own damage, as a shot at the body would do), at the blow's usual cost.
- **Realistic akimbo** (NO): akimbo as above, but only with guns that aren't two-handed: two pistols fire together, while a rifle or a rocket launcher fires alone, at full accuracy, whatever is in the other hand. Full and realistic akimbo are two kinds of the same thing: switching one on switches the other off. In battle, **Y** switches akimbo off and the same kind back on.
- **Lean shots** (NO): YES: a soldier standing more than a couple of voxels to one side of the tile's middle (walk or step sideways within the tile), across the line to the target, shoots from that side, to peek round a corner: the game's own off-centre origins (the ones its "off-centre shooting" option tries), a little to the left or right. If that side has no line of fire to the unit aimed at, the shot comes from the middle as usual. It needs *Movement: Free* (with Tiles the eye always settles in the middle of the tile); leaning out (setting *Lean*) shoots from its side either way. NO: shots start from the middle, as in the game.
- **Lean** (Off): **Peek** or **Tilt**: holding **Left Shift** with **A** or **D** leans the soldier out to that side, to peek round a corner (A and D don't strafe while Shift is held; the lean key is rebindable, and each side can have a key of its own: *Lean (hold with strafe)*, *Lean left / right* under Controls). The eye moves up to six voxels to the side and a little lower, stopping short of walls and of anyone in sight (never into a wall or a teammate's head); letting go straightens up. With **Tilt** the view tilts about ten degrees with it, as a head does; with **Peek** it stays level. A shot fired while leaning out starts from that side (the same off-centre origins as *Lean shots*). The soldier stays on their tile, and who they see is still up to the game: the lean shows the street round the corner, not an alien the soldier hasn't spotted. **Off**: Shift does nothing and A and D strafe, as before. (In 0.9 the lean was on Q and E, which then weren't the hand keys; they are again.)

### Light, atmosphere and sound

- **Smooth lighting** (YES): YES: the battlescape's light (sun, flares, fires, personal lights) blends smoothly from tile to tile. NO: tile by tile, as the isometric map shades it.
- **Shadows** (YES): ray-traced shadows from the sun by day, from street lamps and your flashlight at night, and under every unit. Turn them off on slow machines.
- **Distance haze** (Normal): how soon far terrain fades into the haze, the sky's colour at the horizon. **Normal** from about half the view's reach (27 tiles), **Light** from three quarters, **Clear** only in the last tenth, so the world doesn't stop with a hard edge where the view ends. The fade blends the colours step by step, so far walls fade evenly. Under the sea the murk stays as it is.
- **Flashlight** (On): the 3D view's own light, on and off with **F** (every battle starts with it on). **On**: a wide beam from the soldier's shoulder that lights where you look, reaching 14 tiles, in the dark (night and dusk). **Dynamic**: a light on the weapon, a bright, tight round spot right on the crosshair with a faint halo round it, reaching 20 tiles, day or night, so a dark UFO, cellar or barn lights up by day too (in daylight it adds next to nothing). **Off**: no flashlight, and F does nothing. Only the 3D view is lit: what the soldiers see by the game's rules doesn't change. The suit light (L, or SUIT LIGHT) is separate: it is the game's personal light, round every soldier, by the game's rules (it lights their tiles, so aliens see them more easily at night).
- **Night vision needs gear** (YES): YES: in 3D only soldiers wearing or carrying night vision gear (power and flying suits, ion armor, hover tanks...: the list is in the mod, see [Night vision gear](#6-night-vision-gear)) and units that see in the dark, like aliens, get the NIGHT VISION button. NO: everyone has it, like on the isometric map.
- **Dynamic music** (YES): YES: while aliens are in sight the music switches to a tenser track (the base defence one), and goes back to the battle music once they've been out of sight for a few seconds. NO: the battle music all along (switching it off mid-battle goes straight back to it).
- **Hit, kill and swing sounds** (YES): a short tick when your shot or blow hits an enemy in sight, a double tick for a kill, and a swoosh for your melee swings: the sounds the 3D view adds. NO: silent (the crosshair still marks the hit), leaving the game's own sounds, such as death cries and a weapon's own hit sound, alone.
- **Smoke** (Modern): how smoke looks. **Modern**: the game's own smoke, made truly see-through. The original smoke is a stipple of dots that stood in for see-through smoke on the isometric map; here the dots are blurred into soft clouds (how densely they lie is how opaque the smoke is, their colours its colour, mostly grey), so the smoke keeps the game's shapes and colours but you see through it. Thin smoke, from a rocket say, is a light haze that's easy to spot and hides nothing; thicker smoke is a denser cloud. Where the soldier can't see through it by the game's own sight rules (every step through smoke uses up some of the soldier's sight, more in thicker smoke, less within two tiles of them or with heat vision), it turns solid, with a soft, billowy edge, as a smoke grenade's cloud should. The clouds change slowly and smoothly from frame to frame, sway and bob a little, and a few wisps rise off them. **Original**: the original games' own smoke, their dots in their own colours (UFO's greys and browns, TFTD's white under the sea), drawn see-through: more opaque the thicker the smoke, its frames changing smoothly; no grey, no wall and no wisps, and an explosion's flash and embers without our grey puffs (the smoke it leaves is the game's). **Solid**: the game's smoke pictures on every smoky tile, solid, as in 0.8.2. With Modern and Original, units the soldier can see stay clear through the smoke, and units the game hides stay hidden. Fire is drawn the same in all three. (0.9's *See-through smoke* gives the first value: YES Modern, NO Solid.)
- **Unlimited tactical flares** (YES): YES: T throws a red flare at the crosshair, free and as often as you like. It bounces off walls and burns with red fire and red light for about a minute; under the sea it only goes a metre or two and bubbles hard. It lights the 3D view only: what the soldiers can see by the game's rules doesn't change. NO: T does nothing.

### Looks

- **Weapon in hand** (YES): YES: the weapon in the soldier's hand shows at the bottom right of the view, Doom style, built in 3D from its inventory picture (little cubes, thickest in the middle, the barrel on the line to the crosshair): it bobs as you walk, kicks when you fire and dips out of sight while reloading, looking far down (from 45°, gone at 72°) and zoomed in. A melee weapon is held tip up and swung. Anything else in the hand (a grenade, a medi-kit, a clip) is built the same way and sits in the bottom corner, its lower half out of sight. With the classic HUD shown it stays hidden (the HUD covers the bottom of the view). NO: nothing drawn.
- **Sprite upscaling** (Smooth + detail): soldiers, aliens, items on the floor, explosions and smoke seen up close are scaled up the way pixel art scales well (xBRZ, in the game's own colours). **Smooth** rounds off the stair-stepped edges; **Smooth + detail** also sharpens the shading a little; **Off** keeps the original pixels.
- **Unit size** (100%): soldiers and aliens drawn a little smaller, 80 to 100% of the isometric map's size, standing on the same spot, so tall sprites fit under low ceilings. Only the picture changes: what shots hit stays the game's (see *Hitboxes*; at about 80 to 90% the sprites come closest to their hitboxes' heights).
- **Muzzle flashes** (YES): YES: gunfire lights up its surroundings for a blink, units flash when hit, the view flashes red when your soldier is hit (and at each shot of a berserk soldier), and a blast flashes. NO: none of these flashes, and UFO lights hold steady. Turn this off if flashing light bothers you (photosensitivity, epilepsy).
- **Particles** (2): **0** none; **1** effects (muzzle flashes, impacts, explosions, fire, smoke); **2** everything, plus dust, fireflies, bubbles and weather.
- **UFO lights and aura** (YES): UFOs stand out from the Earth around them. Their hulls stay still and only their lights glow (lit panels, lift pads, screens, navigation lights, found in the original animation frames), pulsing gently in a slow wave round the UFO. Each UFO casts its own two-coloured light (its hull's hue close in, its lights' hue further out) on the ground around it, its underside and its skin, strongest at night, and drifting motes, sparks and a beam of light up each grav lift wrap it. It all shows only once the UFO has been seen, so it never gives a UFO away; with Muzzle flashes off nothing pulses. The small scout's blocky hull is trimmed to its sprite's round outline (only the look: line of fire is still the game's). NO: the original animation frames (calmed, as everywhere in 3D).
- **Discreet map edge** (NO): YES: no yellow tape and posts round the map; small red dots on the floor along its edge, a tile apart, mark it instead, with the land beyond as before.
- **Pixel size** (Auto): **Auto** gives full detail up to a 640×400 view and blocks of 2 on bigger ones (a 1920×1080 screen at the default battlescape scale makes a 960×540 view); **1** full detail; **2** or **3**: each ray colours a block of 2×2 or 3×3 pixels, chunkier but much faster on big screens or slow machines.

### Crosshair

- **Crosshair** (YES): shows the crosshair. Its shape tells the fire mode: an open cross for snap, a scope ring for aimed, brackets with one dot per round for auto, an arc for a throw, a diamond for a launcher, claws for melee, rings for psi. NO: no crosshair (the shot info and the hit ticks stay).
- **Shot info** (YES): shows the fire mode, the hit chance and the time unit cost right of the crosshair (with *Crosshair* off, a little lower, so the middle of the view stays clear) (DUAL in front when both guns fire). Yellow: you can; red: you can't (not enough time units, no ammo, out of range).
- **Unit names** (YES): the name of the unit you point at (its picture under the crosshair, or the free cursor), just under the crosshair, as the game's unit screen names it: an alien by its race (or race and rank, where a mod shows those), a civilian by kind, a soldier by name, in green for X-COM, red for aliens, yellow for civilians.
- **Hitboxes** (Off): outlines, round every unit in sight, the voxels the game counts as that unit when a shot or a blow reaches it (its armor's footprint, from the floor to its height, kneeling or standing): green for X-COM, red for aliens, yellow for civilians. The sprites are only pictures, often taller or wider than what can be hit. **Blink**: the outlines fade in and out once a second; **Always**: they stay.
- **Downed unit icons** (Crosshair): small icons for a unit lying unconscious: a flame if it's burning, a drop of blood if it's bleeding (fatal wounds), a bolt if its health is draining away (shock), and Zz, out cold. Our own pixel art, in the order the game's own floor indicators take where a mod has them. **Crosshair**: for the body you point at (the crosshair, or the free cursor), just under the crosshair. **Always**: over every body in sight. **Off**: none. Only bodies you can see get them, not one behind a wall or in smoke too thick to see through.
- **Aim at exact point** (YES): YES: shots go for the exact point under the crosshair. NO: like the original game, shots go for the tile and the game picks the spot (the middle of a unit, the object, the wall...). The game's accuracy applies either way. (FPS-style shooting always aims at the exact point.)
- **Show trajectory** (Arcs): while aiming, the path the next throw or shot will take, as light cyan dashes fading away from you, with a diamond where it lands. It's the game's own path, worked out as the game will (the curve it picks for a throw, the line of fire for a shot), only without the accuracy roll: the real one scatters as the hit chance says. **Arcs**: throws and arcing shots (a spitting alien's weapon in a mod, say); nothing shows where the game couldn't throw. **All**: every shot's line too, up to the first thing it meets. **Off**: none.
- **Confirm fire** (NO): YES: the first click marks the target and shows its path (whatever *Show trajectory* says), with "Click again to fire"; a click on the same spot fires, a click elsewhere moves the mark, the right button forgets it. Holding the trigger after a confirmed burst keeps firing. NO: one click fires, as always.

### Battle

- **Alien turn in 3D** (YES): YES: stay in the 3D view during the alien turn, looking through your last soldier's eyes (HIDDEN MOVEMENT until an alien comes into sight). NO: switch back to the isometric map for the alien turn.
- **Aliens' turn cameras** (YES): with *Alien turn in 3D*, the view goes where the action is. When one of your soldiers fires back (reaction fire), it cuts to them at once, looking at their target. While an alien moves, it looks through the nearest soldier who sees it, staying at least a second and a half before cutting to another, and turns to the alien each time it reaches a new tile. A soldier who is shot at, hit or mind controlled pops up their helmet camera card for 3 seconds (TARGETED, HIT, MIND CONTROLLED), and one going down too (setting below); the soldier you look through going down gets the death camera, as before. Your turn then starts looking the way the soldier faces. The cards show the camera whatever *Soldier camera preview* says. NO: the view stays with your last soldier.
- **Camera when a soldier falls** (YES): with *Aliens' turn cameras*, a soldier of yours going down out of view (killed or knocked out) pops up their helmet camera card for 3 seconds: static, with DECEASED or UNCONSCIOUS. NO: only the soldier you look through gets the death camera, as before.
- **Start battles in 3D** (NO): YES: begin each battle in the first-person view instead of the isometric map.
- **Doom mode (debug)** (NO): a cheat for testing and fun, and a first try at real time: in the 3D view the selected soldier never runs out of time units or energy on your turn: every shot, throw, blow and item is free. Grenades are primed and thrown as usual (or in real time, below). Ammo still counts. DOOM MODE shows on the info panel while it's on.
- **Doom IDKFA** (NO): Doom mode plus endless ammo: the selected soldier's weapons spend no rounds in the 3D view, one-round weapons like the rocket launcher included, and loaded clips are filled up. IDKFA shows on the info panel.
- **Doom real-time grenades** (NO): in Doom mode, grenades go off in real time, a few seconds after they're primed, wherever they are then (see [Items](#3-the-top-bar-and-items)). NO: primed with the game's timer and thrown as usual.
- **Doom prime on throw** (YES): with *Doom real-time grenades*, a grenade is armed as you throw it and burns from the moment it leaves your hand (FUSE in battle sets the seconds). NO: *prime* sets the seconds and pulls the pin, and the fuse burns in your hand.
- **Doom grenade fuse (seconds)** (3): with *Doom real-time grenades*, how long a grenade burns, from 1 to 5 seconds. The **SET FUSE** box in battle (the prime mode) changes it too, and it holds for every grenade until changed.
- **Doom free grenades with airburst** (NO): in Doom mode, a grenade flies to wherever the crosshair points, by the 3D view's own throw instead of the game's: under a roof, over a wall, at a point in the air. It reaches as far as the soldier could throw it by the game's rules, but never further than a person can throw: about 40 m (25 tiles) for the strongest, less for weaker soldiers (the game's own throw reaches 21 tiles at strength 27 and 51 at 60). It flies a real arc and drops where it meets something, and with *Doom real-time grenades* it bursts **in the air** if its fuse runs out before it lands. NO: the game's throw (refused where the game can't throw), and a grenade whose fuse runs out in flight goes off the moment it lands.
- **Doom IDDQD** (NO): Doom mode plus invincible soldiers: X-COM's units take no damage at all (shots, explosions, fire, smoke, falls) and keep full morale, on either side's turn and in either view. Their four stats on the info panel turn silver, and IDDQD shows there. (Mind control still works: it does no damage.)
- Insert switches every cheat that's on off in battle, and the same ones back on (Doom mode alone if none was on); the warning line says which, and the settings keep it (as for Y, akimbo, and the SET FUSE box). The names are a tribute to Doom, whose cheat codes they borrow (see [Thanks](#12-thanks)).
- **Frame rate limit** (60): the game's own frame rate cap, the same setting as in Advanced options: 60, 75, 90, 120, 144, 165, 240 or unlimited. 60 is steady on modest machines at 1280×800; raise it for a 120 or 144 Hz screen if your computer keeps up (the frame counter shows it).
- **Frame counter** (NO): the game's own frames-per-second counter in the top left corner (the same as F7).

## 5. Fire modes per weapon (FPS-style shooting)

With **Options → 3D VIEW → Faithful X-COM targeting** off, the 3D view keeps the game's three fire-mode slots (keys Z / X / V, mouse wheel or J to cycle) but fires them FPS style. Each gun offers only the modes it has in the game; the table is generated from the game's rulesets (`items.rul`). Time units are the game's, as a percentage of the soldier's time units (a burst costs the game's auto shot).

- **quick (Z)**: a gun with auto fires one **BURST** (the game's auto: its rounds); without auto, its single **SNAP**
- **AIMED (X)**: one tight round
- **AUTO (V)**: a click fires a burst; **holding the trigger keeps firing** until you let go, the clip is empty or the time units run out
- guided launchers (Blaster Launcher, Disruptor Pulse Launcher, ...) place their waypoints on the isometric map, as before

With akimbo on (full or realistic), the other hand's gun fires at the same moment, round for round: the same mode if it has it, otherwise its first shot, never more rounds than the chosen gun fires (a pistol joins a rifle's auto with a snap that goes with the first round).

### UFO Defense
| Weapon | Game modes | Fun mode: quick | Aimed | Auto |
|---|---|---|---|---|
| Tank/Cannon | snap, aimed | SNAP (33 %TU) | AIMED (80 %TU) | — |
| Tank/Rocket Launcher | snap, aimed | SNAP (45 %TU) | AIMED (75 %TU) | — |
| Tank/Laser Cannon | snap, aimed | SNAP (33 %TU) | AIMED (75 %TU) | — |
| Hovertank/Plasma | snap, aimed | SNAP (30 %TU) | AIMED (60 %TU) | — |
| Hovertank/Launcher | guided launch | launch (map waypoints) | — | — |
| Pistol | snap, aimed | SNAP (18 %TU) | AIMED (30 %TU) | — |
| Rifle (2-handed) | snap, aimed, auto | BURST x3 (35 %TU) | AIMED (80 %TU) | AUTO hold (35 %TU per x3) |
| Heavy Cannon (2-handed) | snap, aimed | SNAP (33 %TU) | AIMED (80 %TU) | — |
| Auto-Cannon (2-handed) | snap, aimed, auto | BURST x3 (40 %TU) | AIMED (80 %TU) | AUTO hold (40 %TU per x3) |
| Rocket Launcher (2-handed) | snap, aimed | SNAP (45 %TU) | AIMED (75 %TU) | — |
| Laser Pistol | snap, aimed, auto | BURST x3 (25 %TU) | AIMED (55 %TU) | AUTO hold (25 %TU per x3) |
| Laser Rifle (2-handed) | snap, aimed, auto | BURST x3 (34 %TU) | AIMED (50 %TU) | AUTO hold (34 %TU per x3) |
| Heavy Laser (2-handed) | snap, aimed | SNAP (33 %TU) | AIMED (75 %TU) | — |
| Heavy Plasma (2-handed) | snap, aimed, auto | BURST x3 (35 %TU) | AIMED (60 %TU) | AUTO hold (35 %TU per x3) |
| Plasma Rifle (2-handed) | snap, aimed, auto | BURST x3 (36 %TU) | AIMED (60 %TU) | AUTO hold (36 %TU per x3) |
| Plasma Pistol | snap, aimed, auto | BURST x3 (30 %TU) | AIMED (60 %TU) | AUTO hold (30 %TU per x3) |
| Blaster Launcher (2-handed) | guided launch | launch (map waypoints) | — | — |
| Small Launcher (2-handed) | snap, aimed | SNAP (40 %TU) | AIMED (75 %TU) | — |
| CYBERDISC_WEAPON | snap, aimed | SNAP (30 %TU) | AIMED (60 %TU) | — |
| CELATID_WEAPON | snap, aimed | SNAP (30 %TU) | AIMED (60 %TU) | — |
| SECTOPOD_WEAPON | snap, aimed, auto | BURST x3 (35 %TU) | AIMED (60 %TU) | AUTO hold (35 %TU per x3) |

### Terror From The Deep
| Weapon | Game modes | Fun mode: quick | Aimed | Auto |
|---|---|---|---|---|
| Coelacanth/G. Cannon | snap, aimed | SNAP (30 %TU) | AIMED (80 %TU) | — |
| Coelacanth/Aqua Jet | snap, aimed | SNAP (45 %TU) | AIMED (75 %TU) | — |
| Coelacanth/Gauss | snap, aimed | SNAP (33 %TU) | AIMED (75 %TU) | — |
| Displacer/Sonic | snap, aimed | SNAP (30 %TU) | AIMED (60 %TU) | — |
| Displacer/P.W.T. | guided launch | launch (map waypoints) | — | — |
| Dart Gun | snap, aimed | SNAP (20 %TU) | AIMED (50 %TU) | — |
| Jet Harpoon (2-handed) | snap, aimed, auto | BURST x3 (40 %TU) | AIMED (70 %TU) | AUTO hold (40 %TU per x3) |
| Gas Cannon (2-handed) | snap, aimed | SNAP (40 %TU) | AIMED (75 %TU) | — |
| Hydro-Jet Cannon (2-handed) | snap, aimed, auto | BURST x3 (40 %TU) | AIMED (80 %TU) | AUTO hold (40 %TU per x3) |
| Torpedo Launcher (2-handed) | snap, aimed | SNAP (40 %TU) | AIMED (80 %TU) | — |
| Gauss Pistol | snap, aimed, auto | BURST x3 (30 %TU) | AIMED (50 %TU) | AUTO hold (30 %TU per x3) |
| Gauss Rifle (2-handed) | snap, aimed, auto | BURST x3 (40 %TU) | AIMED (60 %TU) | AUTO hold (40 %TU per x3) |
| Heavy Gauss (2-handed) | snap, aimed | SNAP (40 %TU) | AIMED (80 %TU) | — |
| Sonic Cannon (2-handed) | snap, aimed | SNAP (50 %TU) | AIMED (70 %TU) | — |
| Sonic-Blasta Rifle (2-handed) | snap, aimed | SNAP (40 %TU) | AIMED (60 %TU) | — |
| Sonic Pistol | snap, aimed | SNAP (30 %TU) | AIMED (50 %TU) | — |
| Disruptor Pulse Launcher (2-handed) | guided launch | launch (map waypoints) | — | — |
| Thermal Shok Launcher (2-handed) | snap, aimed | SNAP (50 %TU) | AIMED (70 %TU) | — |
| DEEP_ONE_WEAPON | snap, aimed, auto | BURST x3 (35 %TU) | AIMED (60 %TU) | AUTO hold (35 %TU per x3) |
| Alien Sonic Weapon | snap, aimed | SNAP (30 %TU) | AIMED (60 %TU) | — |
| XARQUID_WEAPON | snap, aimed | SNAP (30 %TU) | AIMED (60 %TU) | — |

## 6. Night vision gear

With **Options → 3D VIEW → Night vision needs gear** (on by default), the 3D view gives the **NIGHT VISION** button (and the N key) only to a soldier who:

1. wears an armor, or carries an item (any inventory slot), listed under `nightVision:` in `mod/OpenXcom3D/OpenXcom3D.rul`; or
2. sees as far in the dark as by day. That's the game's own rule: armor `visibilityAtDark`, which is 20 tiles for aliens by default. So an alien under your mind control always has it, and so does any modded armor with dark vision.

With the setting off, everyone has it, like the isometric map's night vision (Scroll Lock toggle, Space hold).

### Every armor and held item in the games, with a suggestion

**Bold = in the default list.** *Italics = a reasonable addition* if you want night vision earlier in a campaign. Everything else: no. Those are weapons, ammo, explosives and medical gear with nothing to look through.

#### UFO Defense: armors

| Armor | Night vision | Why |
|---|---|---|
| STR_NONE_UC (no armor) | no | |
| STR_PERSONAL_ARMOR_UC | *maybe* | human-made, 1990s tech; giving it night vision makes night missions much easier from the start |
| **STR_POWER_SUIT_UC** | **yes** | alien alloy helmet, elerium power: the first suit with real sensors |
| **STR_FLYING_SUIT_UC** | **yes** | the power suit's successor |
| TANK_ARMOR (tanks) | *maybe* | real tanks have thermal sights |
| **HOVERTANK_ARMOR** | **yes** | alien tech sensor suite |
| alien armors (sectoid, floater, snakeman, muton, ethereal, cyberdisc, sectopod, reaper, chryssalid, celatid, silacoid, zombie) | **yes, by rule 2** | aliens see in the dark (20 tiles) |
| civilians (CIVM, CIVF) | no | |

#### UFO Defense: held items

| Item | Night vision | Why |
|---|---|---|
| pistol, rifle, heavy cannon, auto-cannon, rocket launcher, their ammo | no | weapons |
| laser pistol, laser rifle, heavy laser | no | weapons |
| plasma pistol, plasma rifle, heavy plasma, their clips | no | weapons |
| small launcher, stun bomb, blaster launcher, blaster bomb | no | weapons |
| grenade, smoke grenade, proximity grenade, high explosive, alien grenade | no | explosives |
| stun rod | no | melee |
| STR_MOTION_SCANNER | *maybe* | a sensor, but it's a separate screen and not optics; leaving it out keeps the scanner's role |
| STR_MEDI_KIT | no | |
| STR_PSI_AMP | no | psi, not sight |
| STR_MIND_PROBE | *maybe* | alien sensor tech; thematically odd for seeing in the dark |
| STR_ELECTRO_FLARE | no | it lights the area instead (and the 3D view's T flares exist) |
| corpses, UFO parts, alien artefacts | no | not carried into battle as gear |

#### Terror From The Deep: armors

| Armor | Night vision | Why |
|---|---|---|
| STR_NONE_UC | no | |
| STR_PLASTIC_AQUA_ARMOR_UC | *maybe* | the first deep-sea suit; the deep is dark anyway |
| **STR_ION_ARMOR_UC** | **yes** | alien ion tech |
| **STR_MAGNETIC_ION_ARMOR_UC** | **yes** | the top suit |
| COELACANTH_ARMOR (SWS) | *maybe* | human-made, like the tank |
| **DISPLACER_ARMOR** | **yes** | the alien-tech submersible |
| alien armors (aquatoid, gillman, lobsterman, tasoth, calcinite, deep one, tentaculat, biodrone, hallucinoid, triscene, xarquid) | **yes, by rule 2** | |
| civilians (docker, surfer, sailor, bikini girl), zombie | no | |

#### Terror From The Deep: held items

| Item | Night vision | Why |
|---|---|---|
| dart pistol, jet harpoon, gas cannon, hydro-jet cannon, torpedo launcher, their ammo | no | weapons |
| gauss pistol/rifle/heavy, sonic pistol/blasta rifle/cannon, disruptor pulse launcher, thermal shok launcher | no | weapons |
| magna-blast grenade, dye grenade, particle disturbance grenade, magna-pack explosive, sonic pulser | no | explosives |
| thermal tazer, vibro blade, thermic lance, heavy thermic lance | no | melee |
| STR_PARTICLE_DISTURBANCE_SENSOR | *maybe* | TFTD's motion scanner, same reasoning |
| STR_MEDI_KIT, STR_MC_DISRUPTOR, STR_MC_READER | no | |
| STR_CHEMICAL_FLARE | no | lights the area instead |

### Changing it

Add or remove types in the list. It takes any armor or item type, mods included, e.g. a modded "NV goggles" item:

```yaml
firstPerson:
  nightVision:
    - STR_POWER_SUIT_UC
    - STR_FLYING_SUIT_UC
    - STR_PERSONAL_ARMOR_UC      # the "maybe" above
    - STR_NV_GOGGLES             # a mod's item, carried anywhere in the inventory
```

A later mod's list replaces an earlier one.

The same section can list built-in items (fixed weapons, armors' special weapons) that should show in the hands anyway, where a mod's picture of one looks right held (by default they don't show, see [Items](#3-the-top-bar-and-items)):

```yaml
firstPerson:
  builtInItemsShown:
    - STR_MECH_ARM_CANNON         # a mod's built-in weapon drawn like a held gun
```

## 7. What you'll see

- The map rebuilt in 3D from the game's own voxel data, with the original art and proportions.
- Doors that swing and UFO doors that slide.
- Animated machinery and light, calm and eased: animated terrain steps a frame about three times a second and cross-fades, so nothing large blinks (the photosensitivity guideline WCAG 2.3 allows at most three flashes a second).
- UFOs that stand out: steady hulls with glowing, softly pulsing lights, their own coloured light around them and an aura of drifting motes (the "UFO lights and aura" setting).
- Light and shadow: lamps, the sun, your flashlight, flares.
- Bloom, night vision, rain, snow and dust, and in TFTD bubbles, marine snow and caustics.
- Smoke, fire, explosions and animated machinery that flow from frame to frame.
- Lifts you ride: standing on a grav lift shows LIFT PLATFORM arrows, and the view rises or sinks slowly to the next level.
- Units seen from the side they face you.
- Sound that fades with distance (and is a little quieter behind you).
- Melee that lands: the weapon winds up and drives in towards the crosshair with a swish, the view lunges, the blow jolts, and the target flinches.
- Weapons:
  - trails by damage type: none for bullets, beams for lasers, glowing blobs for plasma, smoke for rockets, bubbles under the sea;
  - the weapon in hand built from voxels, its barrel aimed at the crosshair: your shots leave its muzzle;
  - recoil, muzzle flashes by weapon class (bullets, explosives, fire, lasers, plasma, stun), hitmarkers with sound, enemies that flinch;
  - bullet holes, scorch marks, blown-out walls in a burst of debris, burnt ground.
- When it gets tense:
  - enemy shots whizzing past you (you hear which side) with a flinch; a red arc on the screen's edge towards where a hit came from; blood on the view after a heavy hit or an alien's blow;
  - a red heartbeat at the screen edges when badly hurt;
  - going down: killed (red, then the helmet camera dies in static), unconscious (a fade to dark blue) or mind controlled (an orange haze), each with its message, before the next soldier's view comes in; killed by a Chryssalid (or a Tentaculat), TURNED INTO A ZOMBIE: down in red, then the body gets up again, lurching in a sickly green, until the camera gives out (their squad button shows a green skull, their camera says ZOMBIE);
  - on fire: the view in flames (a sheet of fire up from the bottom and the sides, embers drifting up, its light flickering), a BURNING banner and a flare-up when you catch fire, BURNING on the panel and the camera card; burning soldiers and aliens you see are in flames too;
  - breaking down at the start of a turn, seen through that soldier's eyes while they do what the rules make them do: panic (YOU HAVE A MENTAL BREAKDOWN, a fade to white, the heart racing, while they freeze, or drop their weapons and run) or berserk (BERSERK, white and red, every wild shot flashing red); then they come to, and their camera says PANIC or BERSERK for the rest of the turn;
  - tenser music while aliens are in sight;
  - "ENEMY SPOTTED!" and HIDDEN MOVEMENT banners.

## 8. Performance

The default 1280×800 window renders the 3D view at 640×400, shared out over every CPU thread the game may use (up to 32). On the test laptop (a Core Ultra 7 155H) a whole frame takes 9 to 10 ms there, 97 to 112 FPS with no frame limit, so the game's 60 FPS cap holds with room to spare.

On smaller machines it runs the same way on fewer threads. Measured by holding the game to some of the laptop's cores, at 1280×800:

| cores | pixel size 1 | pixel size 2 |
|---|---|---|
| 4 cores, 8 threads (the Steam Deck's count) | 57–60 FPS | 116 FPS |
| 4 efficiency cores | 41 FPS | 69–70 FPS |
| 2 low-power cores (2.5 GHz) | 19 FPS | 34 FPS |

So on a machine with four cores or fewer, set **pixel size** to 2: the picture gets a little blockier, and the frame rate nearly doubles.

The 3D view is drawn at the battlescape's size (the game's **Battlescape scale**), then magnified to the screen; its controls get bigger in steps from 720 lines (×2) and 1080 lines (×3). So the cost follows the battlescape's size, not the screen's. On a 1920×1080 screen (measured on the test laptop, a Core Ultra 7 155H, walking and turning in a town):

| Battlescape scale | 3D view | Frame rate |
|---|---|---|
| *screen/2* (default), pixel size **Auto** (default) | 960×540 (480×270 rays) | steady **60 FPS** (80–82 with no frame limit) |
| *screen/2*, pixel size 1 | 960×540 | 50–52 FPS |
| *screen* | 640×360, controls ×3 (small isometric map) | steady 60 FPS (70–74 with no frame limit) |
| *original* | 320×200 | steady 60 FPS (100 with no frame limit), chunky |

Sprite upscaling costs nothing measurable. Use **pixel size** 2 or 3 on slow machines too.

## 9. Known limits and ideas

- **A real-time mode** will be attempted in the future: the battle running continuously instead of turn by turn, for those who want it, as a setting like everything else. Doom mode's real-time grenades are the first piece of it.
- **A 3D battlescape** is another idea for the future: the same voxel renderer seen from a free camera above the battle (turn, zoom, pan), with the levels above cut away as on the isometric map, and orders given by clicking tiles and units, a 3D take on the isometric map beside the first-person view.
- **The art:** soldiers and aliens are 32×40 pixel sprites seen from the side they face you, terrain is the game's 16×16×24 voxel shapes coloured from the isometric pictures, all in 256 colours. Sides the isometric art never shows are filled in (crafts and UFOs from their mirrored side). Ideas that would add resolution without changing the style: rendering the view at a higher scale, smoother voxel shading, colour sampled at the exact hit point, units given volume like the weapon in hand. Good ideas are welcome.
- **Sound:** stereo can't tell front from back; sounds behind you are only a little quieter.
- **Not saved:** bullet holes, scorch marks, flares and each soldier's flight toggle are gone after a quickload or when the screen is rebuilt.
- **Leaning** shows what's round a corner, but the game's own sight still starts from the middle of the soldier's tile: an alien there shows once the soldier (or someone else) spots it. The weapon in hand stays level while the view tilts.
- **Leaning and fair play** (a player's note: the aliens can't lean by choice, so leaning gives the player a small edge). What the lean gives, exactly: it **shows no more than the game does**: the game's sight doesn't change, units it doesn't see aren't drawn, and the lean's room check leaves them out, so a peek shows terrain, never a hidden alien. A **shot** from a lean (and with *Lean shots*) starts from the game's own off-centre origins, a couple of voxels to the side. OXCE has a game option, **off-centre shooting** (`oxceEnableOffCentreShooting`), with which every unit, the aliens included, tries left and right of centre when the centre has no line of fire; with it on, the aliens have that reach too, and the player only gets to pick the side. For the most even game, play with that option on, or leave *Lean shots* off and lean only to look. An idea for later: shots from a lean only when that option is on. Letting the AI lean by choice belongs to the game itself (a fork such as Brutal-OXCE could), not to this add-on.
- **The game's own messages** (e.g. "has become unconscious") pause the 3D sequences behind them until you click OK, as all battlescape animation does.
- **Platforms:** built and tested on Linux only (x86-64, the Steam Deck included). The Windows project files list the add-on, untested; Windows and macOS builds from the community are very welcome.
- **Languages:** English (US and UK) only.

## 10. For developers

### What it is based on (exactly)

| | |
|---|---|
| Upstream repository | **https://github.com/MeridianOXC/OpenXcom.git** (OpenXcom Extended by Meridian) |
| Branch | **`oxce-plus`** |
| Commit | **`f0134ef8065799c7723953d64b565f7832bfd782`** — 2026-09-29 15:04:49 +0200, author Meridian, "Merge remote-tracking branch 'SupSuper/master' into oxce-plus" |
| Version string | **OpenXcom Extended 8.7.1 (v2026-09-19)** |
| OpenXcom3D | engine add-on and mod version **0.8** (`requiredExtendedEngine: OpenXcom3D`, `requiredExtendedVersion: "0.8"`) |

`openxcom3d.patch` holds every change on top of that commit:

- the core hooks: 39 files, about 570 lines;
- the new `src/FirstPerson/` add-on;
- the `mod/OpenXcom3D/` data mod.

It applies with `git apply` to a clean checkout of the commit above.

Built with `-DOXCE_FIRSTPERSON=OFF`, it is plain OXCE again: the add-on is left out and the hooks are no-ops.

To work on it: check out the commit above, `git apply openxcom3d.patch`, build (see [Option B](#option-b-build-from-source)), and make your changes; `git diff` (plus the new files under `src/FirstPerson/` and `mod/`) gives the next patch.

### Code layout

**`src/FirstPerson/`, about 19,800 lines**

Every file starts with the project's philosophy and a description of what's in it, and every function and block is commented. The two big classes are split into one file per topic, and their headers follow the same order.

| File | What |
|---|---|
| `FirstPersonApi.{h,cpp}` | **The only header core files include.** No-op inline stubs when built with `-DOXCE_FIRSTPERSON=OFF` (`OXCE_NO_FIRSTPERSON`). Also registers the add-on's options |
| `FirstPersonMode.h`, `FirstPersonModeShared.h` | **The controller and UI** (`FirstPersonMode`); layout constants, timings and colour helpers its files share |
| `FirstPersonMode.cpp` | Creating the view and the HUD, the canvas magnified on big screens, switching views, the frame (`think`), input, drawing the screen |
| `FirstPersonModeCamera.cpp` | Turning by mouse and keys, free facing, turning to an enemy, following the soldier's eye |
| `FirstPersonModeMovement.cpp` | Real-time walking (each tile through the game's own walking), doors, jumps and vaults, grav lifts, flying, falling, stopping when an enemy is spotted |
| `FirstPersonModeShooting.cpp` | Weapons and fire modes, aiming along the game's line of fire, firing through the game's own actions, FPS-style spread and recoil, dual wield, the crosshair, flares, aiming on the map |
| `FirstPersonModeWeaponInHand.cpp` | The weapon (or any other item) in the hands, made from its inventory picture (measured, then given volume as voxels), muzzle flashes, swings |
| `FirstPersonModeHud.cpp` | Top bar, squad buttons, info panel, soldier cards with the live helmet camera, banner, enemy markers |
| `FirstPersonModeSoldierStates.cpp` | Hurt, near misses, on fire, going down (killed, zombie, unconscious, mind control), breakdowns (panic, berserk) |
| `FirstPersonView.h`, `FirstPersonViewShared.h` | **The renderer** (`FirstPersonView`); voxel proportions, dithering, shading and noise helpers its files share |
| `FirstPersonView.cpp` | The camera, and drawing one frame (the order of a frame and the threading rule are explained at the top of `FirstPersonView.h`) |
| `FirstPersonViewTerrain.cpp` | Voxel models from the LOFT shapes coloured by the isometric sprites, crafts' mirrored sides, doors, the ground outside the map, the per-frame tile snapshot |
| `FirstPersonViewRayWalk.h` | The ray walk: tile by tile, then voxel by voxel in tiles that have something in them; a byte per tile and a bit per voxel let it skip the air (the renderer's hottest code: every pixel's ray and every shadow ray) |
| `FirstPersonViewRayCasting.cpp` | Colours for the time of day, light and shadow rays, shading what a ray hit, the border and the sky, the rays; the view's threads (one per CPU thread, up to 32, kept between frames), which also draw the units, items, see-through smoke and colour wash in bands of rows |
| `FirstPersonViewSprites.cpp` | Units and items as the game's own sprites standing in the world (worked out on the game's thread, recorded, then drawn on all threads in bands of rows); the see-through smoke; enemy markers |
| `FirstPersonViewUpscaling.cpp` | Optional smoother sprites up close (xBRZ on a worker thread) |
| `FirstPersonViewEffects.cpp` | Particles: explosions, fire, smoke, weather, UFO devices, bloom |
| `FirstPersonViewShots.cpp` | Shots in flight, thrown items as tumbling voxel models, trails, beams, impacts, bullet holes and scorches, debris |
| `FirstPersonViewFlares.cpp` | Tactical flares and their light |
| `FirstPersonViewUfo.cpp` | UFOs ("UFO lights and aura"): steady 3D-only models of UFO parts made from their frames (lights marked, tall parts trimmed to the sprite), the UFOs found on the map, their two-coloured light as a per-tile field, the lights' pulse, the aura |
| `FirstPersonViewScreenEffects.cpp` | On the screen itself: the hurt arc, blood, flames, colour washes, static |
| `FirstPersonVoxelModel.{h,cpp}` | An item's inventory picture made into a voxel model (the weapon or item in hand, thrown items) |
| `FirstPersonParticles.{h,cpp}` | The particle pool (sparks, smoke, weather...) |
| `FirstPersonSounds.{h,cpp}` | Sounds made in code (no files): hitmarker and kill ticks, heartbeat, dry-fire click, flare hiss, near-miss whizz, melee swoosh |
| `OptionsFirstPersonState.{h,cpp}` | 3D VIEW options tab: Settings / Controls sub-tabs, scrolling lists |
| `RuleFirstPerson.h` | `firstPerson:` ruleset section (header-only): enabled, view distance, window glass, weather, combat music, night vision gear, built-in items shown in hand |

What `FirstPersonView` draws:
- terrain from LOFT voxels with iso-sprite projection, animated frames, UFO doors cut out as they slide, doors swinging, window glass;
- smooth light plus sun face shading, Bayer dithering and horizon haze; colour washes (hurt, dying, unconscious, mind control, breakdown, zombie); night vision recolouring; the fire over the view of a burning soldier;
- the outside world: ground, X-COM tape and posts (or the discreet edge's dots); the sky;
- terrain not explored yet: plain slate-coloured shapes (never black, which reads as missing); a hole through the ground floor shows the earth below;
- cards for units, floor items, projectiles and explosions, facing the camera and leaning with its pitch only up to 45° looking down (so they stay above the floor seen from above) and 10° looking up (so a unit close by doesn't lurch over the camera), standing on the floor's top; the art of items and corpses below their anchor lies flat on the floor; units glide, and fade right in front of the camera;
- particles; red numbered markers over the visible enemies; the path a throw or shot would take (setting *Show trajectory*);
- UFO parts: the art's black holes (the inside of a curved hull, its black outline) painted like the voxels around them, and on a UFO a colour shaded past the darkest of its group stays that darkest colour instead of black; a craft's unseen sides borrow from its mirrored side, never black, and inside only from the same kind of tile part;
- the view tilted about its own axis when leaning (setting *Lean*: Tilt): the rays and everything projected take the tilt as it is, and the cards, which run row by row level, are then drawn pixel by pixel instead.

**Core changes** (see `openxcom3d.patch` for every line), placed where upstream rarely edits:

| File | Change |
|---|---|
| `CMakeLists.txt` (top) | `option(OXCE_FIRSTPERSON ... ON)` |
| `src/CMakeLists.txt` | Adds the `firstperson_src` list, or defines `OXCE_NO_FIRSTPERSON`; links Threads |
| `BattlescapeState.{h,cpp}` | `FirstPerson::create/destroy/think/handle/blit/wantsMouseCapture/resized`; `friend class FirstPersonMode` |
| `BattlescapeGame.h` | `BattleAction::targetVoxel` (exact aim point), `instantMove`, `freeAim` (FPS-style shot), `dualWield`/`dualWeapon`/`dualType` (dual wield) |
| `Projectile.h` | `getAmmo()` getter (trail style from the ammo's damage type) |
| `ProjectileFlyBState.{h,cpp}` | Uses `targetVoxel` when set (accuracy still applies); with `freeAim` asks the first-person view for each round's target; dual wield: the other gun's round fired, flown and landed with each round (like a shotgun's extra pellets), both half as accurate; a failed close-quarters check clears `targetVoxel` and `freeAim`, so the deflected shot goes where the game sends it; a lean side (`relativeOrigin`, setting *Lean shots*) with no line of fire falls back to the centre |
| `BattlescapeGame.cpp` | A soldier panicking or going berserk: in 3D the first-person view shows it (`FirstPerson::breakdown`, which holds the battle as long as the box would) instead of the "has panicked" box |
| `ExplosionBState.cpp` | FPS-style shooting: our own bullet hits play quicker (`FirstPerson::fastShots`) |
| `BattleItem.cpp` | `spendAmmoForAction`: no ammo spent with Doom IDKFA (`FirstPerson::endlessAmmo`), beside the game's own debug mode |
| `BattleUnit.cpp` | `damage`: X-COM's units take none with Doom IDDQD (`FirstPerson::invincible`) |
| `Projectile.cpp` | With `freeAim`: no accuracy roll, no line-of-fire refusal, the line carried on through the target, a bullet's speed |
| `UnitWalkBState.cpp` | `instantMove` walks take the no-animation path |
| `Map.{h,cpp}` | `getNightVisionColor()`, `isNightVisionOn()` getters; `setSoundListener()`: in 3D, `getSoundAngle` pans sounds for the 3D camera; `setNightVisionHoldBlocked()`: in 3D the hold key (Space) jumps; `getPartnerProjectile()`: the dual-wield round in flight |
| `Sound.{h,cpp}` | `packDistance()`: in 3D the listener packs the distance into the stereo angle, so sounds fade with distance without touching the call sites |
| `Pathfinding.{h,cpp}` | `setPath(unit, path)`: a step set by hand (the step off a ledge). `setFlightCost(unit, tu)`: the soldier flying in 3D with SUIT FLY on pays the "flight cost per level" setting for up/down (everyone else: the rules). `setVault(unit, parts)`: for the step a vault takes, those low tile parts don't block that one unit and cost nothing (`isBlocked`, the wall and object costs) |
| `main.cpp` | Window caption " - OpenXcom3D" (add-on builds) |
| `InteractiveSurface.cpp` | Upstream UB fixes found by UBSan: button 0 ignored (no shift by −1), `SDLK_ANY` inside the enum |
| `Script.h` | Upstream UB fix found by UBSan: `ScriptRef::compare` doesn't `memcmp` empty (null) refs |
| `TileEngine.cpp` | Upstream UB fix found by UBSan: `canTargetUnit` and `checkVoxelExposure` don't divide by zero when the shooter and the target stand in one column (the side scan lines then lie on the centre line) |
| `Explosion.{h,cpp}` | `getLastFrame()`: the last frame of an explosion's own animation (the 3D view's frame blend stops there) |
| `State.h`, `Game.{h,cpp}` | Generic hook: the top state may ask for mouse capture (relative mouse look); released with the cursor centred for every other state, each release counted (`getMouseReleases`) so the 3D view can ignore the cursor's jump |
| `UnitSprite.{h,cpp}` | `setViewRotation()`: draw a unit from any side |
| `Options.cpp` / `Options.inc.h` | `createOptionsOTHER()` → `FirstPerson::createOptions(_info)`; declarations at the end of the file |
| `OptionInfo.cpp` | Loading a key binding clears Shift, Alt and Ctrl, except for `keyFpsLeanHold` (the lean key, held with the strafe keys, Left Shift by default) |
| `OptionsBaseState.{h,cpp}` | 3D VIEW tab via `FirstPerson::hasOptionsTab/createOptionsTab` |
| `Mod.{h,cpp}` | Parses `firstPerson:` at the end of `Mod::loadFile` |
| `ModInfo.cpp` | Engine row `{"OpenXcom3D", 1.0.1}` (add-on builds only; 1.01) |
| `CrossPlatform.cpp` | Add-on builds use their own user/config folders (`~/.local/share/openxcom3d`, `~/.config/openxcom3d`, `Documents/OpenXcom3D`, …). Data folders are shared with stock OXCE |
| `OpenXcom.2010.vcxproj{,.filters}` | The add-on's files |

## 11. Version history

### 1.01 (2026-10-22)

Light fixes: the flashlight on its own, then every key and setting gone over once more (in two rounds of bug hunting), and the fixes for what turned up.

- **Flashlight:** one setting, **Flashlight**: Off, On (a wide beam from the shoulder, in the dark) or Dynamic (a spot on the crosshair, day or night), on and off with its own key, **F**, with a click; the suit light (L) is the game's personal light and nothing else. Next fire mode moved to **J** (and the wheel). A 1.0 options file carries over: *Tactical flashlight* YES becomes Dynamic, *Flashlight* NO Off, and a next fire mode key of your own stays.
- **Keys that can't act say why** on the warning line: the flashlight off or On by day, flares off, armor that can't fly (or swim, under the sea), a fire mode the weapon doesn't have, no night vision gear (Scroll Lock now checks it in 3D too). Insert names the cheats it switched. ISO/3D in the aliens' turn with *Alien turn in 3D* off does what it says (3D comes back with your turn).
- **Firing:** an alien weapon the base hasn't researched can't be used in 3D either; after a shot, the next right click (zoom), Q / E or classic HUD hand click works at once (the game's leftover aim is dropped); a melee refused for time units no longer swings.
- **Moving:** with a time unit reserve (F1 … F4) walking stops only as the game's own walk does; a landing (flight switched off in the air) goes on when you look through that soldier again, or at the next turn when it was held up; a numpad press in the aliens' turn, or a tile move begun by another soldier, is dropped.
- **Soldier cam:** a right click on a soldier's button on the top bar opens their helmet camera big (SOLDIER CAM, with a red dot and a little static), to look through anyone in the squad without switching to them; move along the bar to browse.
- **Settings:** *Shot info* and the hit ticks work without the crosshair (the lines then sit a little lower); cameras popped up in the aliens' turn always show their picture; *Muzzle flashes* off also stops the red hit flash, the berserk flash and the blast flash; Y, Insert and SET FUSE are kept in the options file, so the options screen's Cancel doesn't undo them, and an options change mid-battle keeps the burning Doom fuses and what Y and Insert switch back on; a new battle never takes over the last one's choices, while a mission's second stage does; descriptions that were cut off are shorter.
- **And:** Ctrl+S (the game's quick mode) and the game's other Ctrl and Alt shortcuts no longer walk you; the right button's zoom can't stick after a menu; the scope leaves with the 3D view, and a death or breakdown camera plays unzoomed; the map no longer drifts after F8 with an arrow key held; after watching an aliens' turn on the map, F8 no longer turns you to an alien; tanks' and aliens' armor shows no raw name; the mouse's thumb buttons pick soldiers in 3D as on the map.
- **Steam Deck:** X sends J (next fire mode), d-pad right held F (the flashlight), R5 a flare and held end turn (so a press a little short never ends the turn); the manual had the Menu and View buttons the wrong way round.

### 1.0 (2026-10-21)

The same veteran player's third and fourth play-test reports on 0.9 (26 more small patches, each tested on its own), and a deep dive into the renderer.

- **Seeing:** **Smoke** is Modern (the game's own smoke made see-through, solid where the soldier's sight runs out), Original or Solid; UFO walls without black holes and stripes, and a wall no longer takes the texture of what stands opposite it; a unit right beside you no longer lurches over the camera when you look up; bullet holes on every wall (the Skyranger's too); a hit's explosion no longer plays twice; doors never fold into their walls.
- **Aiming:** **Zoom** (hold U, or the right button: four times closer, SCOPING and a scope cross); **Show trajectory** (the path of a throw or an arcing shot, or every shot); **Confirm fire**; the **compass** shows what a shot's turn will cost (with free turning, the turn is free).
- **Fighting:** melee weapons held tip up and swung, the swing's sound under *Hit, kill and swing sounds*; fists and built-in weapons on G and the wheel; medi-kits and the scanner used straight from the crosshair; **UNPRIME** switches a light off again; built-in items leave the hands empty (no tank cannons or dog heads); in Doom mode every action is free and grenades are primed as usual; with **Doom real-time grenades** a grenade goes off a few seconds after it's primed: by default as it leaves your hand (**Doom prime on throw**, the seconds set with **FUSE** in battle or **Doom grenade fuse**), or with **prime** and a click, burning in your hand.
- **Moving:** stairs 0.9 couldn't climb; with *Movement: Tiles* and turning by the rules a step to the side keeps the facing; **Lean** is now held with **Left Shift + A / D**, so Q and E are the hand keys again; a useless built-in item on Q or E no longer turns the soldier.
- **The aliens' turn:** **Aliens' turn cameras**: the view cuts to the nearest soldier who sees the alien moving and to anyone firing back, and a soldier shot at, hit, mind controlled or going down (**Camera when a soldier falls**) pops up their helmet camera.
- **Light:** **Tactical flashlight**: a light on the weapon, a bright spot on the crosshair, day or night.
- **On screen:** **Downed unit icons** (burning, bleeding, shock, out cold); **Unit names** under the crosshair, and **Middle click identifies**; the weapon slides out of sight when you look far down.
- **Faster:** the same picture drawn 20–32% faster at 1280×800 (the ray walk 1.6–2× faster; units, items, smoke and the colour wash drawn on all threads), on every machine tried, from one core to twenty-two.
- **Upstream fix:** OXCE's `canTargetUnit` no longer divides by zero when two units stand in one column.

### 0.9 (2026-10-20)

A veteran player's two play-test reports on 0.8.2 (played with Piratez too), turned into small patches, each tested on its own, and a few additions of ours.

- **Moving:** walking off a ledge works wherever the ground below can also be reached another way (it was refused there; a blown-out wall upstairs too); a diagonal gap between two units or crates corner to corner can be walked through; **Movement: Tiles** (WASD a tile a press) and the **numpad**, a tile at a time in either mode, as on the isometric map; **Turning costs TU** with a **Compass**, and snap turns on **, .**; **Flight cost overrides rules**.
- **Seeing:** the sky no longer shimmers through buildings (our window list for city maps took wall paint for glass, and the distance haze dithered over far walls); **Distance haze** (normal, light, clear); **See-through smoke**, by the game's own sight rules; unexplored terrain in slate, never black; **Discreet map edge** (red dots instead of the tape); soldiers, corpses and items seen from above are real cards standing on the floor, and items lying on the floor show (they didn't in 0.8.2); the recolour scripts of the big mods (units blinking red or white) run in 3D as on the map; light no longer glows through units in front of it; **Hitboxes** and **Unit size**.
- **Fighting:** Doom mode counts ammo; **Doom IDKFA** (endless ammo) and **Doom IDDQD** (invincible soldiers) beside it, and Insert switches the cheats off and on; **Full akimbo** and **Realistic akimbo** (one-handed guns only), Y in battle; **Melee hits walls and downed**; a failed close-quarters check deflects the shot for real; **Lean shots**; and **Lean**: Q / E held lean out to peek round a corner, the view tilting with *Tilt*, a shot from there leaving from that side.
- **The HUD:** every button of the classic HUD now acts in 3D (centre, look a level up or down, the minimap on the soldier, soldier up and down, the enemy buttons; right click: the best spotter); **P** hides the 3D HUD; back on the map, it's centred on the soldier, and no old path arrows linger; a right click on the squad bar's arrows scrolls it.
- **Sound:** **Hit and kill sounds** can be switched off, and the combat music setting is now **Dynamic music**, so it's found.
- **Steam Deck:** the lean on L4 / R4; akimbo, snap turns, the 3D HUD, the light and the flare on long presses.
- **OpenXcom3D** in one word in the main menu, and a thank you to John Carmack and id Software.

### 0.8.2 (2026-10-19)

- **Download and play:** the Linux release comes with its own libraries in `lib/`: SDL 1.2 (sdl12-compat on SDL2, which opens X11, Wayland, PulseAudio, PipeWire or ALSA when the game starts), SDL_image and SDL_mixer, with SDL_gfx built in. Nothing has to be installed on Ubuntu 22.04 and newer, Debian 12, Fedora, Arch or the Steam Deck. It is also an **AppImage**, one file, with its profile in `~/.local/share/openxcom3d`. Played on Ubuntu 22.04's and Arch's own libraries with no SDL installed, and on a Steam Deck.
- **Finds the games by itself:** `play.sh` and the AppImage link *X-COM: UFO Defense* and *Terror From The Deep* from any Steam library (also Flatpak and Snap Steam, and other disks and SD cards Steam lists), or from a `UFO` / `TFTD` folder next to the game, and switch on the one that's there. They say what's missing in a window when there's no terminal ([The game data](#the-game-data)).
- **Steam Deck:** a new profile plays fullscreen, and a controller layout made for the game comes with it (`steamdeck/`): the left stick walks, the right trackpad looks, L1 switches mouse look and the free cursor, R1 the isometric map and 3D ([Steam Deck](#steam-deck)).

### 0.8.1 (2026-10-19)

- **The prebuilt release runs on old and new Linux systems**: 0.8's wouldn't start on Arch (it asked for SDL_gfx by its Debian file name) nor on systems older than the newest Ubuntu (it needed glibc 2.43). Now it needs glibc 2.34 or newer (Ubuntu 22.04, Debian 12, Fedora 35 and every newer or rolling system), uses the system's SDL libraries in whatever version is there, and opens whichever SDL_gfx is installed. Checked on Ubuntu 22.04's and Arch's own libraries. `play.sh` and the installer say what's missing and give the install command for Debian/Ubuntu, Arch or Fedora.
- **Pixel size Auto** (new default): blocks of 2 when the 3D view is bigger than 640×400, such as on a 1920×1080 screen, which then holds 60 FPS instead of 40–51; full detail otherwise ([Settings](#4-settings), [Performance](#8-performance)).
- **Everything in a hand is 3D**: a grenade, a medi-kit, a scanner, a clip... is built from its inventory picture like the weapon, and sits in the bottom corner with its lower half out of sight. **Thrown items fly as 3D objects**, tumbling end over end along the game's arc ([Items](#3-the-top-bar-and-items)).
- **Grenades:** a primed one shows PRIMED and its fuse on the panel. **In Doom mode they work in real time**: one click pulls the pin, and they go off 3 seconds later wherever they are, in your hand too: the first step towards a real-time mode, which will be attempted in the future.

### 0.8 (2026-10-19)

- **Fixed: UFO surfaces blinking** (a photosensitivity hazard). Animated terrain followed the game's 100 ms tick, and faces the isometric art never shows took the next frame's raw colour while dissolving into it: up close the small scout's hull sides and lilac floor flashed 5 to 10 times a second. Animated terrain now steps a frame every 320 ms on the view's own clock and cross-fades over the whole step, hidden faces dissolve through the same colour rules, a lamp's pool of light no longer jumps with its frames, and blinking status lights fade softly.
- **UFO lights and aura** (new setting, on): UFOs stand out from the Earth around them: steady hulls whose own lights glow and pulse gently, each UFO's two-coloured light on the ground and its hull, and an aura of drifting motes, sparks and grav-lift beams, once the UFO has been seen ([Settings](#4-settings)).
- **Ammo on the info panel**: a line for each hand, such as `R: Rifle 17 | 2 clips (40)`: the rounds in the loaded clip for the fire mode, then the spare clips carried for that gun and their rounds. It follows every round, reload, pick-up and drop, in faithful and FPS-style shooting alike; EMPTY in red, ∞ for lasers. The soldier card shows the same line.
- Fixed: with **Dual wield**, only the chosen gun fired from the second shot in a fire mode on (and on every held FPS-style burst after the first), though the crosshair said DUAL. Both guns now fire, and spend their rounds and time units, on every shot; the second gun pays its time units only when it really fires.
- **Options**: every description on the 3D VIEW page is one short line, so none runs into the buttons any more; the full story of every setting is in [Settings](#4-settings). The "3D weapons (voxel)" setting is gone: the weapon in hand is always built in 3D (the flat picture it switched to broke up into jagged stripes at the angle the weapon is held).
- **Faster**: the ray caster keeps its threads between frames and hands out rows as they go (a hybrid CPU's slow cores no longer hold a frame up), uses up to 16 threads, works out each flare's light once a frame, and the overlay and bloom skip what is empty. On a 1280×800 screen the 3D view takes 33 to 54% less time in complex scenes (urban by day 17.0 → 10.5 ms, 8 flares at night 24.6 → 13.8 ms), with the same picture.
- Fixed: turning to an enemy (1–0) tilted the view too far up or down; the crosshair said DUAL when the other gun couldn't join; a mod's penalty for shooting at what the soldier can't see wasn't in the hit chance; the weapon didn't dip when a spent clip was reloaded; the off-hand muzzle flash used the wrong gun's ammo; falls, fire and bleeding counted as melee hits; some destroyed walls threw grey chunks; the enemy marker's outline was open at the top; upscaled sprites had over-bright edges; two door swings in one tile erased each other; a frame rate limit off the list stepped to "unlimited".

### 0.7 (2026-10-18)

- **Breakdowns in 3D**: a soldier who panics or goes berserk at the start of a turn is seen through their own eyes instead of the game's box. Panic: the view fades to white, YOU HAVE A MENTAL BREAKDOWN, a racing heart, while they freeze, or drop their weapons and run. Berserk: BERSERK, white and red throbbing with each heartbeat, every wild shot flashing red. Then they come to. Their camera card and the panel say PANIC or BERSERK for the rest of the turn. (On the isometric map the game's own box, as before.)
- **Turned into a zombie**: a soldier killed by a Chryssalid (UFO) or a Tentaculat (TFTD) gets their own end in 3D: TURNED INTO A ZOMBIE, down in red, then up again, lurching in a sickly green, until the helmet camera gives out. A green skull on their squad button; their camera says ZOMBIE.
- **On fire**: the soldier's view in flames, rising from the bottom and the sides (heat rising cell by cell, as in the old fire effect of Doom), with embers and the light of it; catching fire makes them leap, with a BURNING banner. Their panel and camera card say BURNING. Burning soldiers and aliens in view burn in 3D too.
- Fixed: a soldier left behind in an earlier stage of a two-stage mission showed in the squad bar as unconscious; they have no button now.
- Fixed: a soldier killed by an overkill showed a health of 4294... on their card.
- **The code, restructured for people:** the add-on is split into one file per topic, with descriptive names, our philosophy at the top of every file, and a comment on every function and block. The compiled game is unchanged: every add-on file was proven to compile to the same machine code before and after the renaming and commenting.

### 0.6 (2026-10-18)

- **Sprite upscaling** (setting): soldiers, aliens, items, explosions and smoke up close scaled the way pixel art scales well (xBRZ, back into the game's palette, optionally sharpened), made on a worker thread so nothing stalls a frame.
- **Falling** (setting): walk off a ledge and the game's own fall takes the soldier down, hurt by the height.
- **FPS-style shooting** (setting "Faithful X-COM targeting" off): rounds fly along the crosshair with a spread, recoil climbs, quick = burst, aimed, auto while the trigger is held; each gun offers only the modes it has; rounds fly at a bullet's pace.
- **Dual wield** (setting): a gun in each hand, both fired at the same moment, round for round, each half as accurate; the second gun drawn on the other side.
- **Eye height** (setting): the 3D camera a little lower than the game's sight line, for headroom.
- Crafts and UFOs: the sides the isometric art never shows (the Skyranger's west flank, north faces) take their colours from the mirrored side it does show; ceilings under roof plates are plain; undersides show the pattern seen from above.
- Pistols and other short weapons drawn smaller (they were stretched to rifle length).
- Fixed: the fire mode jumping to AUTO after an FPS-style burst.

### 0.5 (2026-10-18)

- Your own shots leave the muzzle of the weapon in hand.
- **Jump** (Space) and **vault** over low obstacles (setting).
- **Night vision as gear**: a NIGHT VISION button and N for soldiers with suitable armor or items (setting; list in the mod).
- Every key that works in 3D on the Controls page.
- Walls seen from outside no longer black at night (per-part discovery).
- Smoke, fire, explosions and animated machinery dissolve from frame to frame.
- **Voxel weapons in hand**: the inventory picture given volume, the barrel on the line to the crosshair, muzzle flashes by weapon class.
- **Lift platforms**: purple LIFT PLATFORM arrows to click, and a slow glide between levels.
- **Going down in 3D**: killed (red, then static: SIGNAL LOST), unconscious (dark blue), mind controlled (orange), then the next soldier; ALL SOLDIERS LOST ends the battle.
- Sounds fade with distance; enemy shots whizz past; hit arcs and blood on the view; visceral melee.
- Every voxel model built before the battle starts; frame rate limit setting.
- Fixed: a crash on quickload in 3D.

### 0.4 (2026-10-17)

- Game feel: recoil, hitmarkers, flinching enemies, synthesized sounds, bullet holes and scorch marks, debris.
- Flight (SUIT FLY, flight cost setting), a reworked top bar (suit buttons, a window of 10 soldiers, cards for the fallen and the mind controlled), true proportions, big-screen scaling, a Doom-style weapon in hand.

### 0.1 – 0.3 (2026-10-17)

- The 3D view: a multi-threaded voxel ray caster over the game's own voxel shapes, coloured from the isometric art; smooth light, shadows, lamps, flashlight, flares, haze, sky, weather, TFTD water.
- The controls: mouse look, WASD at the original time-unit costs, doors, reaction fire, every fire mode and item, the soldier bar with live helmet cameras, the classic HUD on demand, the alien turn in 3D.
- Installer, release script and the one-file patch.

## 12. Thanks

To **John Carmack** and **id Software**, for spearheading 3D games. Wolfenstein 3D, Doom and Quake showed the world what a ray cast through a grid and a fast loop could do, and every first-person game since stands on their shoulders, this one included: its 3D view is a ray caster at heart. The cheats in the 3D view, **Doom mode**, **IDKFA** and **IDDQD**, are a small tribute to you. Thank you, guys!

And, as on the project page, to the GNU project and the open-source community, and to the OpenXcom and OpenXcom Extended developers, on whose work all of this stands.
