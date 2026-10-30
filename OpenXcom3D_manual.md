# OpenXcom3D manual

Version 1.09. OpenXcom3D is **OpenXcom Extended (OXCE)** with a first-person 3D battlescape: an engine add-on (`src/FirstPerson/`, with small hooks in the core) and a data mod (`OpenXcom3D`) that switches it on.

In short, with pictures: the [play guide](https://b0realis.github.io/OpenXcom3D/play.html) has [every key](https://b0realis.github.io/OpenXcom3D/play.html#keys), [every setting](https://b0realis.github.io/OpenXcom3D/play.html#settings) and how each plays, what's on the screen, and every feature. This manual has the whole story.

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

Step by step, for each system: the [play guide](https://b0realis.github.io/OpenXcom3D/play.html) (the downloads on Linux, Windows, the Steam Deck and macOS, and where your game data goes) and the [build guide](https://b0realis.github.io/OpenXcom3D/) (compiling it).

Add-on builds keep their own user and config folders, so an existing OXCE profile is never touched:

- the release's `user/` folder, when started with `play.sh` (or `play.bat` on Windows);
- `~/.local/share/openxcom3d` for the AppImage;
- `~/Library/Application Support/OpenXcom3D` for the macOS app;
- otherwise `~/.local/share/openxcom3d` and `~/.config/openxcom3d`.

### Option A: the prebuilt release (Linux x86-64)

Download it from the project's [Releases](https://github.com/b0realis/OpenXcom3D/releases) page, in either form:

- **`OpenXcom3D-1.07-x86_64.AppImage`**: a single file. Make it executable (`chmod +x OpenXcom3D-1.07-x86_64.AppImage`, or *Properties → Permissions → executable* in a file manager) and run it. Its profile is in `~/.local/share/openxcom3d`.
- **`OpenXcom3D-1.07-linux-x86_64.tar.gz`**: unpack it anywhere and run `./play.sh`. Its profile is the `user/` folder inside.

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
ln -s /path/to/UFO  OpenXcom3D-1.07-linux-x86_64/user/UFO
ln -s /path/to/TFTD OpenXcom3D-1.07-linux-x86_64/user/TFTD
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
| View, held | P | 3D HUD on/off (on the map: the map camera) |
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

### Option A on Windows: the prebuilt release (64-bit)

Download **`OpenXcom3D-1.07-windows-x86_64.zip`** from the [Releases](https://github.com/b0realis/OpenXcom3D/releases) page and unpack it somewhere of yours: Documents, the Desktop or `C:\Games`. Not *Program Files*: the game writes its saves and options into its `user` folder, and can't there. Then double-click **`play.bat`**.

**Nothing needs installing.** It runs on 64-bit Windows 10 and 11. The game comes with SDL 1.2 (sdl12-compat on SDL2) as `SDL.dll` and `SDL2.dll`, with SDL_image, SDL_mixer, SDL_gfx and the C++ runtime built in, and takes from Windows only what every Windows has. The game isn't signed, so Windows may warn the first time ("Windows protected your PC"): click *More info*, then *Run anyway*.

The zip holds:

```
play.bat                  starts the game (finds the game data, says what's missing)
bin\openxcom.exe          the engine (OXCE 8.7.1 + OpenXcom3D)
bin\SDL.dll, SDL2.dll     SDL 1.2 (sdl12-compat) on SDL2
data\common, standard     OXCE's own data
user\mods\OpenXcom3D      the mod
licenses\                 the licences of the libraries and of the compiler's runtime
README-Windows.txt, README.md, OpenXcom3D_manual.md, openxcom3d.patch, LICENSE.txt
```

`play.bat` looks for the games' data as `play.sh` does:

1. a `UFO` or `TFTD` folder already in `user\`;
2. a `UFO` or `TFTD` folder in the game's folder or next to it;
3. *X-COM: UFO Defense* and *X-COM: Terror From The Deep* in Steam: its own folder (from the registry) and every other library it lists, such as a second disk.

It links what it finds into `user\` with a directory junction, which needs no administrator and leaves the data as it is. On a disk that can't hold a junction (FAT32, exFAT, a network drive) it copies the data instead. The first time, it also writes the starting options: the game that's there (UFO if both), and a 1280×800 window if the screen has room for one (at least 1300×900), else the full screen. Your saves, the options and the log (`openxcom.log`) are in `user\`. Extra arguments go to the game: `play.bat -load mysave.sav`.

The Windows build is made on Linux with llvm-mingw (Clang for Windows), from the same sources as the Linux release, except sdl12-compat: 1.2.74 instead of 1.2.72, which crashes on Windows. **It's played under Wine** (11.17) here: the launcher in every case above, and battles in 3D in UFO and TFTD. On a real Windows (GitHub's), the [automatic builds](#automatic-builds) check every change: the game starts, and `play.bat` finds the games in Steam's libraries (a second library on another disk too), links them and starts the game. Nobody has played a battle on a real Windows machine yet, so reports are welcome.

### Option A on macOS: a first, untested build

The [automatic builds](#automatic-builds) make a macOS app on GitHub's Macs: **`OpenXcom3D-<version>-macos.zip`**, for Apple silicon and Intel, macOS 11 (Big Sur) and newer. Nobody here has a Mac: it has been built, checked and started there (the game says its version, natively and under Rosetta, and the app's starter sets up its profile), but never played. Reports are very welcome.

To get it: open the latest run of [Build](https://github.com/b0realis/OpenXcom3D/actions/workflows/build.yml?query=branch%3Amain) and, under *Artifacts*, take **macos-release** (a GitHub account is needed; each run keeps it for 30 days). Inside is the zip; unpack it.

**Opening it the first time:** the app is signed ad hoc, not by Apple, so macOS stops it the first time. Open it once, then *System Settings → Privacy & Security → Open Anyway*; or in Terminal, `xattr -dr com.apple.quarantine /path/to/OpenXcom3D.app`. After that it opens like any other app.

**Nothing needs installing.** The app holds the game (OXCE 8.7.1 + OpenXcom3D), SDL 1.2 (sdl12-compat) and SDL2 in `Contents/Frameworks`, with SDL_image, SDL_mixer and SDL_gfx built in, OXCE's data and the mod; zlib is the system's.

**The game data:** the first time, it looks for the games in a `UFO` or `TFTD` folder next to the app or in your home folder, in your Steam libraries, and in apps named after them in Applications (GOG's keep the DOS game inside), and links what it finds into its profile, changing nothing there. Otherwise copy the game's folder to `~/Library/Application Support/OpenXcom3D/UFO` (or `TFTD`); the app says so in a window if it finds none. Your saves, the options and the log (`openxcom.log`) are in `~/Library/Application Support/OpenXcom3D`.

### Option B: build from source

Every command, step by step, for Linux, for Windows with MinGW (MSYS2), and for the Windows build made on Linux: the **[build guide](https://b0realis.github.io/OpenXcom3D/)**.

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

1. Put the mod in a profile's `mods/` folder (it switches itself on the first time the game finds it).
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

- Start `./play.sh` (or the AppImage; `play.bat` on Windows), begin a game (or **New Battle**), and in battle press **F8** or click the **3D** button at the top left.
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

On Windows, delete the folder. The game data in `user\` is only a junction (deleting it removes the link, never the data it points to), or a copy where it had to be copied.

## 2. Controls

All of these can be rebound in **Options → 3D VIEW → Controls**: click a key's row, then press the new key (Shift, Ctrl and Alt can't be bound); a right click clears it. That page also lists (under a heading) the game's own keys that work in 3D: turning, kneel, reload, inventory, hands, the suit light (the game's personal light), night vision, next/previous soldier, end turn, frame counter, quick save/load and the ten enemy keys.

| Input | Action |
|---|---|
| **F8** | Isometric ↔ 3D (or the ISO/3D button); back on the map, it's centred on your soldier |
| **Mouse** | Look around, all but straight up and down (89°); the soldier turns with you for free (or by the rules: setting *Turning costs TU*) |
| **W A S D** | Walk and step sideways in real time. Each tile costs its normal TUs; walking into a door opens it. With *Movement: Tiles*, a press walks exactly one tile. On a grav lift or a ladder, or flying with flight on, **W** looking up (more than 30°) climbs (flies straight up) and looking down goes down, as on Half-Life's ladders (**S** the other way round); held, it climbs on level by level, each paid like Page Up / Page Down. Arrows left of the compass show where it can go, the one W takes now lit yellow. Looking straight ahead, or at the top or the foot (a ceiling, the ground), W walks off as usual |
| **Caps Lock** | Sprint on/off: each step is the game's own run (Ctrl on the map), faster. By the default rules a run costs three quarters of a walk's TUs and half as much energy again, and only armor that allows running runs. **SPRINT** shows under the compass, green while the soldier runs and grey while they can't and walk (their armor, or no breath left for a run) |
| **`** (left of 1) | En passant on/off: an enemy coming into sight doesn't stop the walk, as moving with Shift held doesn't on the map; ENEMY SPOTTED still shows. **EN PASSANT** shows under the compass |
| **Ctrl+Shift + click** | With the auto shot of a spray weapon (`sprayWaypoints`, X-Com Files' miniguns): marks a spray burst's first point, the tile under the crosshair; each click after marks the next, and the last fires the burst along them, as on the map (a Ctrl+Shift+click fires early). The right button takes back the last point. The points show as light green poles with a mini target and their number. |
| **Numpad 8 2 4 6** | One tile ahead, back, left or right, almost at once, like a step on the isometric map; two pressed together (8 and 4) a diagonal tile, as W and A make one (whatever the *Movement* setting; holding the key steps on, but stops at a closed door: press again to go through). **7 / 9**: snap turn 45° left / right |
| **Left button** | Fire at the crosshair in the current fire mode |
| **Wheel / J** | Previous / next fire mode (snap, aimed, auto, launch, melee, psi, use, throw, prime) |
| **Z / X / V** | Snap / aimed / auto shot |
| **G** | Switch hand: the right hand, the left, and the built-in weapon (the classic HUD's SPECIAL, such as an armor's), in turn, each that gives something to use. The weapon moves to that side of the screen; with akimbo on, it picks which gun fires first. An empty hand gives the unit's empty-hand weapon, where a mod has one (fists, a claw: OXCE's `specialUseEmptyHand`); without one, G skips it, as an empty hand has nothing to use |
| **Y** | Akimbo off, and back on to the kind the setting *Akimbo* says (Full or One-hand, see [Settings](#aiming-and-combat)); with the setting Off it does nothing; the warning line says which. The *Akimbo style* key (unbound unless you set one) switches two guns between taking turns round by round and both at once (*Akimbo volley*) |
| **O** | Open a door without walking through: the one you look at, if it's within reach (the sides of your tile, or a wall meeting one of its corners, as a right-click on the map reaches), else the nearest one the view looks towards. The soldier turns to it first, by the rules (*Turning costs TU*: a paid turn, then the door) |
| **T** | Throw a tactical flare (with "Unlimited tactical flares" on) |
| **B** | Suit flight on/off (flying armor; SWIM under water) |
| **F** | The flashlight on/off (setting *Flashlight*): the 3D view's own light, separate from the suit light (L) |
| **Space** | Jump: a free hop. Walking into something low (a fence, table, crate, rock: up to waist height), it vaults over it, paying only the normal walk |
| **N** | Night vision on/off, with night vision gear (Scroll Lock, the game's own toggle, works too); at any time, also during explosions, shots and the aliens' turn |
| **Page Up / Page Down** | Fly up/down (flight on), or ride a grav lift or climb a ladder. Standing on one, purple arrows with LIFT PLATFORM show which ways it goes; with a free cursor (C), click them to ride. With flight off, a soldier in flying armor keeps to the lift as anyone else does (not on up into the air from its top: the message says which key switches flight on); one in the air all the same comes down to land, by itself or with Page Down. Off a lift or ladder, Page Up with flight off switches flight on and flies up, and Page Down with flight on, standing on the ground, switches it off |
| **1 … 0** | Turn to visible enemy 1–10 (or click the red enemy buttons) |
| **Right button** | Cancel the current action, or forget a target marked for *Confirm fire*; with nothing to cancel, hold to zoom (setting *Zoom*). With a free cursor: hold to look |
| **U**, held | Zoom: the view glides in to 4 times closer, and back out when you let go (setting *Zoom*). **SCOPING 2.6x** at the right shows how far, and a thin blue cross runs through the crosshair. Aim, shots and hit chances don't change. In 3D, U then isn't the game's UFOpedia key (on the map it is) |
| **Left Alt**, held | Show facing: a ring round every unit in sight, with an arrow the way it faces, in its side's colour (X-COM green, the aliens red, civilians yellow), drawn over everything, for units whose sprite doesn't show it |
| **C** or **middle button** | Mouse look ↔ free cursor (to click the buttons, hover soldiers for their cards). With *Middle click identifies* on, a middle click on a unit shows it instead, as on the map |
| **H** | Classic HUD on/off: every button of it works in 3D ([below](#the-classic-hud-in-3d)) |
| **P** | The 3D HUD in three steps. Once, a clear view: the top bar, the info panel, the compass, the lift arrows and the enemy markers hide (and can't be clicked); the crosshair stays. Twice, a clean view: the crosshair, its lines (mode, hit chance, costs), the accuracy line and the name under it go too. Three times, all back. The weapon in hand and the trajectory keep their own settings, and the messages still show. On the isometric map the top bar shows as ever, and P shows or hides the map camera instead (see *Map camera*) |
| **Arrow keys** | Turn |
| **, / .** | Snap turn 45° left / right, without stepping (the soldier turns with it) |
| **Left Shift + A / D**, held | Lean out left / right, to peek round a corner, with the setting *Lean* on (otherwise Shift does nothing and A / D strafe, as before): the eye moves to the side, stopping short of walls and of anyone in sight, and with *Tilt* the view tilts. A shot while leaning out starts from that side. Q and E stay the game's hand keys (they don't act while Shift is held, so a lean never opens a menu). The lean key is rebindable (*Lean (hold with strafe)* under Controls, Shift, Ctrl or Alt allowed), and each side can have a key of its own too (*Lean left / right*, unbound) |
| **Insert** | Cheats off, and the same ones back on (Doom mode, Doom IDKFA, Doom IDDQD: see [Settings](#cheats)) |

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

### Akimbo on the map

On the isometric map a soldier with a gun in each hand can fire both too: hold **Ctrl+Shift** and click either hand's weapon. Each click steps the pair on: **both at once** (a mini target in each hand's box), **in turn** (two in each), and off. A shot fired on the map then fires both guns as akimbo does in 3D: pick the mode from the clicked gun's menu as usual, and the other gun fires its mode of the same kind (snap with snap, auto with auto), else its first, both 30% less accurate, for the more expensive gun's shot and a tenth (the other gun fires only if the time units are there for its share). An aimed shot fires alone, as in 3D. It works with akimbo off (the mark is the choice; with akimbo *One-hand* a two-handed gun doesn't pair up, and a gun that blocks both hands, the game's red 2, never does) and is kept with the soldier in the save; it goes once a gun leaves a hand. Shots in 3D keep their own akimbo (the settings and the style key).

## 3. The top bar and items

**Top bar**, from left to right:

- **ISO/3D** · **HUD** · **INV**, at the left edge (on the map HUD hides and INV takes its place);
- **◄ soldiers ►**, in the middle of the screen: 10 shown (on a wide screen, as many as fit at full size), the window follows your soldier, and the mouse wheel over ◄ or ► scrolls; so does a right click on ◄ or ►, one soldier at a time, without changing who's selected (a left click selects the previous or next soldier). Hovering one shows a card with stats and a live helmet camera. A skull marks the dead, a caduceus the unconscious, and an orange alien face a soldier under alien mind control; a unit of the other side under *your* mind control gets a button too, after your soldiers, its number in purple (looking through it, the view's edges are washed purple, breathing gently, as is its helmet camera). A right click on a soldier's button opens their **SOLDIER CAM** (their helmet camera big; see *Soldier camera preview* in [Settings](#4-settings)). The **mouse wheel over a soldier's button** steps the card, or the soldier cam, on to the next soldier (up: the previous one), round the whole squad and scrolling the bar along, without moving the mouse; moving onto another button shows that one again.
- **SPECIAL** · **NIGHT VISION** · **SUIT LIGHT** · **SUIT FLY**, shown when the soldier has them (labels shorten to SPEC · NV · LIGHT · FLY on a narrow bar);
- **END** turn, at the right edge.

**Info panel**, bottom left (the classic HUD, H, has all of this in its own way):

- the soldier's rank and name (the game's own rank badge, made small, before the name; a mod's own tiny ranks if it has them, as XPiratez does), and on the right what matters most just now: PANIC or BERSERK this turn, BURNING, WOUNDS (fatal wounds), or KNEELING;
- a line for each hand. First the weapon that fires, then whatever is in the other hand (grey, or as bright as the first when it fires along, akimbo). A gun's line tells its ammo, for example **`R: Rifle 17 | 2 clips (40)`**:
  - **17** is the rounds in the loaded clip, for the fire mode you picked (a gun with several ammo slots shows the slot that mode uses). **EMPTY** in red means no clip is loaded: the game throws a clip away when its last round is fired, so reload (R, from the belt) or load one in the inventory;
  - **2 clips (40)** is the spare ammo the soldier carries for that gun, anywhere in the inventory (belt, pockets, backpack, the other hand), and how many rounds they hold together. It counts every clip the gun can take by the game's rules, all kinds together (a heavy cannon's AP, HE and incendiary). **no clips** means there's nothing to reload from;
  - guns that never run out, like the lasers, show **∞**; a gun with its rounds built in shows just its count;
  - the numbers follow every round fired, in faithful and FPS-style shooting alike, and every reload, pick-up and drop. Two guns of the same kind (dual pistols) each count the same spare clips, since either could load them;
  - a primed grenade shows the turns its fuse has left: **PRIMED 3 turns**, **PRIMED 1 turn**, **PRIMED this turn** (it goes off when this turn ends); in Doom mode grenades burn in real time, so it shows the seconds: **PRIMED 2.4s**;
- time units, energy, health (with stun damage over it) and morale, as numbers and bars;
- the armor, and on the right DOOM MODE while that cheat is on (IDDQD and/or IDKFA for its extras; with IDDQD the four stats turn silver).

Hovering a soldier on the top bar shows the same weapon line on their card.

**Items:**

- Every item works from 3D. Its fire modes are on the wheel, the game's own: a gun's shots, and melee for anything with a melee cost (a stun rod, a knife, a gun with a butt stroke in a mod), the empty hand's fists and the built-in weapon included (G picks them). A blow strikes what the crosshair is on: a unit beside you (you turn to them first), or a wall, door or object within reach; with it on none of these, whatever stands the way you face.
- **A medi-kit** is used at once, no menu (it had one entry): the patient is the one under the crosshair, among those the game allows: a unit standing on any tile next to you, diagonally too (you turn to face them first: free, or with *Turning costs TU* an eighth of a turn's cost each, as on the map); a body lying at your feet, its picture under the crosshair or simply looking down (more than 25 degrees); aiming at nothing, yourself, if the kit may treat its user (a mod's `medikitTargetSelf`; the stock Medi-Kit can't), or else the game's own order. A unit farther off is **Out of Range!**, and a body that isn't at your feet says to stand over it (the game treats bodies only there). The crosshair says who: **USE: Fred**, **USE: SELF**, **USE: -** for nobody. A kit of one kind (heal, stimulant or painkiller only, as in some mods) works there and then, as the game's menu does it; the full Medi-Kit opens the game's own screen on that patient, to pick the body part and the treatment. **The motion scanner** is held up in the view: used (its time units, as the game's), its round screen, the game's own, shows at the bottom of the 3D view, about a third of its height across (just right of the info panel on the original 320×200), the weapon in hand making way for it, while the game goes on round it. It turns as you look round, the way you look up (its N, E, S and W go round with it); a ring sweeps out from the middle every two seconds with a whoop, and each movement it finds lights up as the ring reaches it, with a pip (higher the nearer, from its side), fading till the next sweep. It shows what the game's scanner finds (the units that moved this turn, up to 9 tiles across and along, but for the square's corners beyond the round screen), from where you stand now. It stays up while it's the item in hand: used again, or another item or hand, and it's put away. On the isometric map the game's own scanner, as ever. The mind probe opens its menu, then is aimed like a shot.
- **Anything in a hand shows in 3D**, built from its inventory picture like the weapon in hand: a grenade, a medi-kit, a scanner, a clip... sits in the bottom corner with its lower half out of sight. A unit's built-in items (a fixed weapon such as a tank's cannon, or an armor's special weapon such as a dog's bite) leave the hands empty: they'd look like a weapon held in empty hands. A mod can list ones to show anyway (`builtInItemsShown`, see [For developers](#10-for-developers)).
- **Melee weapons** are held tip up, the way the inventory pictures stand them (the stun rod's prongs, a blade's point, at the top), and a blow swings the tip down at the crosshair and back up.
- **Switching a light on and off** (an item primed to switch on, as in some mods): the fire modes offer **UNPRIME** once it's primed, as the game's menu does.
- **Throwing:** as on the isometric map, anything that isn't built in can be thrown (fire mode **throw**, the arc on the crosshair): a grenade, a clip for a squadmate, even a rifle. It flies along the game's arc as a 3D object, tumbling end over end, and lands where the game says. The game refuses a throw it can't make ("Unable to throw here!"): under a roof (inside the Skyranger, say), past the soldier's range, or with someone right in front.
- **Priming a grenade** (fire mode **prime**): a click opens the game's own timer at once (the number of turns, 0 to 23), as on the isometric map after *Prime Grenade* (the fire mode already says that much, so its menu isn't asked again); a right click on the timer cancels. A grenade with no timer to set (a mod's) is primed at once. The panel then says **PRIMED** and the turns left (**PRIMED 3 turns**). A primed grenade lying on the ground goes off at the end of a turn with its fuse at 0: **0** when you end this turn, **1** at the end of the aliens' turn, **2** at the end of your next one, and so on (every side's turn counts one down). The game's option *Instant grenades* makes a 0 go off on landing instead. A primed grenade still in a hand doesn't go off.
- **In Doom mode** grenades work in real time, like in a first-person shooter, going off a few seconds after they're primed (*Doom grenade fuse*, 3 s by default) wherever they are then. How they're primed is up to *Doom prime on throw*:
  - **YES** (the default): just **throw** one. It's armed as you throw it (you hear the pin) and burns from the moment it leaves your hand, so it never goes off in your hand. **Prime** becomes **FUSE 3s**: it opens a **SET FUSE** box like the map's timer, with 1s to 5s to pick (a click, or the keys 1 to 5), and the seconds you pick hold for every grenade after that until you change them (it's the same as the setting). The panel shows the fuse for a grenade in hand, then counts the seconds down once it's thrown (**PRIMED 2.4s**). A grenade whose throw the game refuses stays in your hand, armed but not burning, until it leaves it.
  - **NO**: **prime** opens the same **SET FUSE** box, and your pick pulls the pin (you hear it): the fuse burns from then on, in your hand too. Throw it in time, or cook it for a shorter flight, but not too long. The panel counts the seconds down.

  With *Doom free grenades with airburst* a grenade flies wherever the crosshair points, and one whose fuse runs out in the air bursts there; without it, the game's own throw, and a fuse that runs out in flight goes off as the grenade lands. Proximity grenades work as the game makes them. This is the first step towards a real-time mode (see [Known limits and ideas](#9-known-limits-and-ideas)).
- The **blaster launcher**, and psi or the mind probe aimed at no one in sight, switch to the isometric map to pick waypoints or a target, then come back. A psi amp's or a mind probe's **use** is a fire mode like panic and mind control: a click on a creature uses it on them, no menu first.

## 4. Settings

**Options → 3D VIEW → Settings**, from the main menu or in a battle (Esc → Options). A left click flips YES / NO or raises a number; a right click lowers it (choices like Particles go round). The line at the bottom of the page gives a one-line reminder of the setting under the mouse; this is the full story, in the page's groups, with the default in brackets.

The settings are listed under the same headings as in the game.

### View and camera

- **Soldier camera preview** (YES): hovering a soldier's button on the top bar shows a card with their name and rank, stats and weapon. YES: the card also shows that soldier's helmet camera, live: what they are looking at right now (static if they're down). Like a real digital feed, a helmet camera now and then has a bad signal for a moment: a few patches of the picture fall into coarse blocks, a green square or two shows, the picture tears (bands of it slide sideways), or it freezes for up to half a second (every few seconds and brief on most missions; every second or so, longer and torn more often on the hardest ones and inside UFOs, see [Bad signal by mission](#bad-signal-by-mission); never with *Muzzle flashes* off). NO: the card without the camera (a camera popped up in the aliens' turn still shows its picture). Either way, a **right click** on a soldier's button opens their **SOLDIER CAM**: the helmet camera big, with a blinking red dot and now and then a spell of TV static (from nothing up to a snowstorm and back over a few seconds, each at its own pace, 10 seconds to 2 minutes apart; on the hardest missions a little static all the time and the spells more often), so it's plain it's a camera, not your view; moving along the bar, or the mouse wheel over it, shows each soldier's in turn (a fallen soldier keeps the card with its static), and moving off the bar or a second right click closes it. You stay with your own soldier all the while.
- **Map camera** (Off): on the isometric map in your turn, the selected soldier's helmet camera in a window at the top left, under the top bar, with their name over it: a hybrid of the map and the soldier's own sight. It looks the way the soldier faces; while you aim a shot, a throw or a psi attack on the map, it turns to the tile under the cursor (at the unit there, if you see one), so you see the target through the soldier's eyes. **Small**, **Medium** or **Large**: 30%, 40% or half of the width our top bar is laid out on (on the original 320×200 battlescape, Medium is 128×80, doubled on the screen). A **click** on it goes to the 3D view; a **right click** opens the SOLDIER CAM in its place (moving off it closes it); clicks on it never reach the map under it, and the map's cursor stays where it was while the mouse is over it. **P** (the *3D HUD on/off* key) shows or hides it on the map, coming back at the size it had. With OXCE's touch buttons (Ctrl, Alt, Shift and the mouse buttons, on the left edge) switched on, it keeps right of them; hovering a soldier's button shows their card over it. Hidden in 3D and in the aliens' turn (their cameras pop up instead, see *Aliens' turn cameras*). It's a live feed like the SOLDIER CAM: a blinking red dot before the name, a spell of TV static now and then and the bad signal's glitches.
- **Mouse sensitivity** (5): how fast the view turns with the mouse, from 1 (slow) to 20 (fast).
- **Invert mouse** (NO): YES: moving the mouse forward looks down instead of up, as in a flight simulator.
- **Field of view** (75): how wide the 3D view is from side to side, in degrees, from 50 (narrow, like a zoom) to 110 (wide), in steps of 5. The weapon in hand keeps its size and place whatever you pick.
- **Zoom** (YES): hold **U**, or the right mouse button in mouse look (when there's no picked action to cancel), to look 4 times closer. The view glides in over about 0.7 seconds and back out when you let go; **SCOPING 2.6x** in blue at the right counts it up and down, a thin blue scope cross runs through the crosshair, the weapon in hand lowers out of the way, and the mouse turns the view that much slower. Only the picture changes: the aim, the shot and the hit chance are the same (the spread ring grows with the picture). NO: U and the right button are the game's again.
- **Start battles in 3D** (NO): YES: begin each battle in the first-person view instead of the isometric map.
- **Alien turn in 3D** (YES): YES: stay in the 3D view during the alien turn, looking through your last soldier's eyes (HIDDEN MOVEMENT until an alien comes into sight, glowing red on the aliens' turn and green on the civilians', as the map's turn bar). NO: switch back to the isometric map for the alien turn.
- **Aliens' turn cameras** (YES): with *Alien turn in 3D*, the view goes where the action is. When one of your soldiers fires back (reaction fire), it cuts to them at once, looking at their target. While an alien moves, it looks through the nearest soldier who sees it, staying at least a second and a half before cutting to another, and turns to the alien each time it reaches a new tile. When an alien none of your soldiers sees fires at one of them (at night, say), it cuts to the soldier shot at, turned to where the fire comes from. A soldier who is shot at, hit or mind controlled pops up their helmet camera card for 3 seconds (TARGETED, looking at the shooter; HIT; MIND CONTROLLED), and one going down too (setting below); the soldier you look through going down gets the death camera, as before. Your turn then starts looking the way the soldier faces. With the aliens' turn on the isometric map (*Alien turn in 3D* off, or you're on the map), the same cards pop up over the map, and so does whoever the 3D view would cut to: a soldier firing back (FIRING, looking at the target) and the nearest soldier who sees the alien moving (WATCHING, looking at it, for as long as they see it), one card at a time, the newest news first. The cards show the camera whatever *Soldier camera preview* says. NO: the view stays with your last soldier, and nothing pops up.
- **Camera when a soldier falls** (YES): with *Aliens' turn cameras*, a soldier of yours going down out of view (killed or knocked out) pops up their helmet camera card for 3 seconds: static, with DECEASED or UNCONSCIOUS. NO: only the soldier you look through gets the death camera, as before.
- **Eye height lowered by** (3): X-COM soldiers fill a level almost to the ceiling, so the 3D camera sits this many voxels lower (about 8 cm each), from 0 to 6, and rooms feel less cramped. 0 is the game's own eye height. What the soldier can see by the game's rules doesn't change. Standing on something tall right under a ceiling (a stack of crates, stairs under a floor), the game pushes the soldier's sight down under the floor above; there the camera sits only 1 voxel under the game's eye, so the room stays in view instead of a slit over what they stand on.
- **Head bob** (YES): the view bobs gently with the soldier's steps. Turn it off if it makes you queasy.

### Movement

- **Turning costs TU** (NO): NO: the soldier turns with the view for free. YES: turning is the game's own: the view may look up to 45° off the way the soldier faces (they turn their head, for free); looking further turns the soldier 45° that way at the game's cost (1 TU for a soldier), as on the isometric map, and a turn stops when it spots a new enemy. Without the time units the view stops 45° off, and so it does while a shot is under way (yours, a burst, its hits, or anyone's): no turns pile up to be paid for after it. The snap turn keys turn the soldier a step each.
- **Movement** (Tiles): how W, A, S and D walk. **Free**: freely, as in a first-person shooter, stepping into the next tile when you cross into it. **Tiles**: a press walks exactly one tile, to the middle of the neighbouring tile that way (one of eight, relative to the view: W ahead, W and D together ahead-right), at walking speed, and holding the key walks on, a tile at a time. A held key stops at a closed door: press again to open it and walk through (**O** opens it without walking in). While a shot or another action is going on, presses do nothing, and a key held through it needs a fresh press. Either way the numpad hops a tile at a time, almost at once (8 ahead, 2 back, 4 left, 6 right; 7 and 9 turn; a press during a shot is dropped too), and doors, ledges, jumps, lifts and diagonal gaps work the same.
- **Walking speed** (4): how fast the soldier walks in 3D, in tiles per second, from 1 to 8. Only the pace changes: every tile entered still costs its normal time units. It sets the pace up and down too, at half of it: a level up or down a lift or a ladder, flying or falling, takes as long as walking a tile and a half at half the speed (0.75 s at 4; a fall speeds up as it goes, and the view dips a little on landing).
- **Flight cost per level (TU)** (0): the time units a flying (or, under the sea, swimming) soldier pays to rise or sink one level with SUIT FLY switched on in 3D, from 0 (free) to 24. The original game charges about 8.
- **Flight cost overrides rules** (NO): YES: every flying soldier pays the flight cost above to rise or sink a level. NO: each unit pays what the game charges it (its armor's own cost, 8 for the flying suit), so units that fly slower stay slower.
- **Jump over low obstacles** (NO): YES: jumping (Space) while walking into something low (a fence, a table, a crate, a rock: up to waist height) takes the soldier over it. The jump itself is free: only the walk to the far side costs its normal time units. It bends the rules a little, which is why it's a setting. NO: Space is just a hop, and the original rules decide what blocks you.
- **Falling** (YES): YES: you can walk off a ledge and fall to the level below, by the game's own fall, hurt by the height (a little from one level, a lot from two or more; armor takes some of it). NO: ledges stop you, as the map's paths do.
- **Check the landing** (YES): with *Falling*, a step off a ledge first looks where the soldier lands. Onto ground no one can walk on (a blood moat, the sea, the craft's hull: places the map never lets a soldier walk onto) or where not a single step leads on, the soldier stops at the edge with **UNSAFE LANDING! PUSH AGAIN** (a player's report: X-Com Files' vampire castle, a soldier soft-locked in its blood moat). Let go of the movement key and push that way again within 6 seconds, or jump (Space) while pushing, to go over anyway. NO: every ledge can be stepped off, as before.

### Aiming and combat

- **FPS-style shooting (easier)** (NO): NO: every shot uses the game's own accuracy, and the hit chance shows by the crosshair (this is the setting called *Faithful X-COM targeting* before 1.09, shown the other way round: a player saw new players switch it, thinking it the faithful way). **YES: FPS-style shooting**, far easier than the game. Rounds fly along the crosshair with a spread (the soldier's firing skill and kneeling tighten it; the accuracy line under the crosshair shows it, see *Accuracy line*), recoil lifts your aim with every round and eases back, and each round flies at a bullet's pace. Each round leaves from your eye's place on the tile (leaning out too), at the gun's height, so what you see past a corner you can hit: the game's own shots leave from a muzzle point on the tile's edge towards the target, up to seven voxels from where you look from, and past a corner the crosshair cleared they met the wall (a player's screenshot: the laser into the near wall). The game's three fire-mode slots become: **SNAP** = one round from the hip, **AIMED** = one tight round, **AUTO** = hold the trigger to keep firing until you let go, the clip is empty or the time units run out. Each gun offers only the modes it has in the game ([table](#5-fire-modes-per-weapon-fps-style-shooting)). Time units, ammo and damage stay the game's.
- **Akimbo** (Off): **Full** or **One-hand**: a soldier with a gun in each hand fires both on every shot, the two taking turns round by round (each gun with its own fire mode and ammo, see *Akimbo volley*), but each is 30% less accurate (FPS style: the spread about 1.4 times as wide), and the pair costs the more expensive gun's shot and a tenth more (two pistols: one pistol's snap shot and a tenth, not two; energy the same way). A shotgun in either hand fires its pellets. The other gun fires the same mode if it has it, otherwise its first shot, and its whole burst however long the chosen gun's is (a 4-round and a 5-round gun: 9 rounds; the crosshair counts both). No aimed shots with two guns that pair (who aims two guns at once?): Aimed isn't offered (the wheel skips it, and its key says so); **Y** switches akimbo off to aim. An aimed shot picked on the classic HUD all the same fires the chosen gun alone. Without the time units for both, only the chosen hand fires, at full accuracy. When either gun's shot arcs (a grenade launcher or a Panzerfaust in the big mods), the two fire in turn instead: the chosen gun first, and once its shot has landed the other one at the same target, each at full accuracy and its own cost (the crosshair says IN TURN). The 3D view shows the second gun on the other side, and the info panel shows both guns' ammo. Two-handed weapons also take the game's own one-handed penalty, and a gun that blocks both hands (the game's red 2: it won't fire with the other hand full) never pairs. **One-hand**: only with guns that aren't two-handed: two pistols fire together, while a rifle or a rocket launcher fires alone, at full accuracy, whatever is in the other hand. In battle, **Y** switches akimbo off and back on to the kind set here; with the setting **Off** it does nothing (a player: a finger slipping onto Y switched akimbo on). **Off**: one hand at a time, as in the game (G switches hands). (Before 1.09 these were two settings, *Full akimbo* and *Realistic akimbo*, now Full and One-hand; the one that was on carries over.)
- **Akimbo volley** (NO): how two guns fire. Either way each is 30% less accurate, and the pair costs the more expensive gun's shot and a tenth. NO: they take turns round by round, the chosen gun first (A, B, A, B...; the gun with the longer burst finishes alone), each round its own as an auto burst's: aimed as it leaves, its hit, deaths and destroyed walls worked out before the next one leaves, so a burst can kill one alien, then go on through to the next behind it; the crosshair says DUAL. YES: both at once, a volley: the other gun's round flies beside the chosen one's and their hits land together, as a shotgun's pellets do (a round that gets there first shows its hit at once); the crosshair says VOLLEY. The rounds fly at the game's own speed either way. (Arcing guns, which always fire gun after gun, keep their own rule: see *Full akimbo*.) The *Akimbo style* key (unbound: set one under 3D VIEW) switches it in battle, and the warning line says which.
- **Lean shots** (NO): YES: a soldier standing more than a couple of voxels to one side of the tile's middle (walk or step sideways within the tile), across the line to the target, shoots from that side, to peek round a corner: the game's own off-centre origins (the ones its "off-centre shooting" option tries), a little to the left or right. If that side has no line of fire to the unit aimed at, the shot comes from the middle as usual. It needs *Movement: Free* (with Tiles the eye always settles in the middle of the tile); leaning out (setting *Lean*) shoots from its side either way. NO: shots start from the middle, as in the game. (With the game's own targeting; FPS-style rounds always leave from the eye.)
- **Lean** (Off): **Peek** or **Tilt**: holding **Left Shift** with **A** or **D** leans the soldier out to that side, to peek round a corner (A and D don't strafe while Shift is held; the lean key is rebindable, and each side can have a key of its own: *Lean (hold with strafe)*, *Lean left / right* under Controls). The eye moves up to six voxels to the side and a little lower, stopping short of walls and of anyone in sight (never into a wall or a teammate's head); letting go straightens up. With **Tilt** the view tilts about ten degrees with it, as a head does; with **Peek** it stays level. A shot fired while leaning out starts from that side (the same off-centre origins as *Lean shots*). The soldier stays on their tile, and who they see is still up to the game: the lean shows the street round the corner, not an alien the soldier hasn't spotted. **Off**: Shift does nothing and A and D strafe, as before. (In 0.9 the lean was on Q and E, which then weren't the hand keys; they are again.)
- **Melee hits walls and downed** (NO): a melee blow at the wall, door or object under the crosshair strikes it, by the isometric map's own terrain melee (in a mod that switches it on, the 3D view does so whatever this setting): within a tile and a half of the eye, on the soldier's own tile or one next to it, diagonally and a level up or down too (on stairs, the walls of both floors ahead; the map's own strikes only straight ahead), damaged by the game's own terrain damage if the weapon can damage terrain (a stun rod can't). Floors can't be struck, as on the map. YES also switches that on for the original games, and lets a blow hit a unit lying unconscious in front of the soldier or at their feet (it can't dodge; the game's own damage, as a shot at the body would do), at the blow's usual cost.
- **Melee hits floors** (YES): with *Melee hits walls and downed* on (or the mod's terrain melee), a blow at the floor or ceiling under the crosshair within reach strikes it too: the floor at your feet or around, the ceiling above. The game's own hit: its damage, the floor's armour, and if it breaks, the game's falling (a body lying there takes the blow first, as with a shot at the floor). NO: walls, doors and objects only, as the game's terrain melee.
- **Aim at exact point** (YES): YES: shots go for the exact point under the crosshair; on the open sky, the point of air on the crosshair's line, up to 20 tiles out (nearer where the line leaves the map or the weapon's range ends): the game takes an empty tile as a target as readily, and the round flies on past the point along the same line. If the line from your weapon to the point meets something else first (the gun sits on the tile's edge, beside your eye), it's tried from the game's other muzzle points, the middle and, with OXCE's *Off-centre shooting* on, left and right of it (as the game tries them for a unit), and fires from the one that clears; if none does, the shot goes as the game's own aim would instead: at any part of the unit (or the object, the wall) a line reaches, from the middle, and left and right of it with OXCE's *Off-centre shooting* on; the path shows the line it takes. An arcing shot (a grenade launcher, a Panzerfaust in the big mods) goes for the exact point too, a spot on the floor rather than the tile's middle; where no curve reaches it, the game's own points on the tile. NO: like the original game, shots go for the tile and the game picks the spot (the middle of a unit, the object, the wall...; an empty tile's middle on the open sky). The game's accuracy applies either way, and the hit chance shows on the open sky too. (FPS-style shooting always aims at the exact point.)
- **Show trajectory** (Arcs): while aiming, the path the next throw or shot will take: a throw's or an arcing shot's curve as light cyan dashes fading away from you, with a diamond where it lands (yellow for a forced shot, see *Force arcing shots*); a straight shot's line as a thin red laser from your weapon, with a brighter dot where it meets something; a shot the game will refuse (its own targeting: no line of fire) shows its laser dimmed, a dotted dark red, so "No line of fire" never comes as a surprise. Under night vision the path is light in the goggles, as a laser sight is: a white-green core in a green glare, the dot white in the middle; a forced arc is dotted there, since colour can't tell it apart. It's the game's own path, worked out as the game will (the curve it picks for a throw, the line of fire for a shot), only without the accuracy roll: the real one scatters as the hit chance says. It starts from the side the shot will leave from, so leaning (see *Lean shots*, *Lean*) moves it with the shot. **Arcs**: throws and arcing shots (a grenade launcher or a Panzerfaust in the big mods, a spitting alien's weapon); nothing shows where the game couldn't throw. **All**: every shot's line too, up to the first thing it meets. **Off**: none.
- **Confirm fire** (NO): YES: the first click marks the target and shows its path (whatever *Show trajectory* says), with "Click again to fire"; a click on the same spot fires, a click elsewhere moves the mark, the right button forgets it. Holding the trigger after a confirmed burst keeps firing. NO: one click fires, as always.
- **Force arcing shots** (YES): an arcing shot the game would refuse (none of its own curves lands on the tile you aim at, as at the side of a building) fires anyway, as with Ctrl held (OXCE's force fire): at that tile (at the exact point with *Aim at exact point*, where the game takes the tile's corner), whatever the curve meets on the way, and its path shows yellow ("at your own risk"). Where the game's own aim works (a unit, an object, a wall, the floor), it's kept. NO: the game refuses such a shot, as on the isometric map without Ctrl.
- **Force blocked shots** (NO): with the game's own targeting (*FPS-style shooting* off), a straight shot with no line of fire, neither to the point under the crosshair nor to any part of the target (off-centre too, with OXCE's option), fires anyway, as with Ctrl held: along the line to the crosshair, the accuracy roll as always, so a round may clear thin cover (a fence top, a crate's edge) or hit it. Its path shows yellow, its dot on what's in the way. NO: the game refuses it ("No line of fire").
- **Shotgun pellets in flight** (YES): YES: a shotgun's pellets fly beside its round, in 3D and on the isometric map alike, each to where the game's spread sends it (traced as it's fired, the same targets and spread), all drawn as the round is (on the map with the round's bullet sprite; shotgun ammo that has none, as some big mods' shotguns, borrows the one its damage type's ammo uses most). With *Each pellet on its own* each hits as it gets there, else as the round lands. NO: the game's own way: the whole blast lands the moment it's fired. Either way every pellet lands with its hit sound (a few to a volley) and, in 3D, its sparks and bullet hole.
- **Each pellet on its own** (YES): YES: a shotgun's pellets, and the two rounds of an akimbo volley, each land on their own, in either view and whoever fires. In OXCE the game traces only the round (the lead pellet): the others are traced and hit as it lands, and when it flies off the map (over a target into the sky, past one at the map's edge) none is traced at all, every pellet lost with it (a player's report: the "shotgun void"). Now none is lost, and with pellets in flight each hits as it arrives; the deaths come with the last to land in the map, as the game's come with its round. Misses still flying then fly on to the map's edge, only to be seen; nothing waits for them. NO: the game's own way, the lead pellet deciding for the rest.
- **Full sprite hitboxes** (NO): YES: a unit is hit wherever its full sprite (its picture) shows, not by the game's voxel shapes: in every line of fire (everyone's shots, the AI's and reaction fire's checks, "no line of fire", and so sight too, which the game checks along the same lines: a unit is seen when its picture is), in 3D and on the map alike, the unit is a card through its middle facing whoever fires, its picture the frame seen from that side (its turn, kneeling, its animation) at the *Unit size*; a shot meets it on the picture's solid pixels and passes through the see-through ones (between legs, round arms). The hit's damage, armour side and body part are the game's. The *Hitboxes* outlines aren't drawn then: the picture is the hitbox. Melee reach stays the game's, by its own shapes, so a blow reaches a big unit as before. NO: the game's own hit shapes.
- **Quick deaths** (NO): YES: a unit killed or knocked out by a shot or a blow is down at once for the battle, as one blasted by an explosion always was, so nothing waits for its fall: a burst fires on (its next rounds fly on to whoever is behind), several units fall at the same time, and the battle goes on at once. The fall still shows, on its own: the game's own fall frames at the game's own pace (about 0.3 s; nothing waits for them), with no turn to face the screen first, in 3D and on the map, its body and what it dropped appearing as it lands. When the soldier you look through goes down, their fall plays on their helmet camera card (the view keeling over, washing red, or dark blue if knocked out, then the static of a dead camera, for 3 seconds) while the view goes on to the next soldier at once, out of a moment's static, and the game selects that soldier as soon as it is free. With nobody left standing, the full death camera as before (the battle is lost). NO: the game's own deaths, one after another, each held while the unit turns and falls.
- **Auto-equip guns first** (NO): YES: when the game equips soldiers who have no saved loadout (a New Battle's, or soldiers never equipped), it hands out guns before melee weapons, in the mod's own item order: two-handed guns, one-handed guns, then two-handed and one-handed melee weapons, ammo and the rest. OXCE counts a two-handed melee weapon as a rifle and hands those out first, so in XPiratez or X-Com Files soldiers often start with staffs, claws, shovels and swords. A gun here is a firearm with a shot reaching past 12 tiles (some mods' melee weapons are firearms with a one-tile "shot"). NO: OXCE's own auto-equip, unchanged, as modders rank their items for it.
- **Unlimited tactical flares** (YES): YES: T throws a red flare at the crosshair, free and as often as you like. It bounces off walls and burns with red fire and red light for about a minute; under the sea it only goes a metre or two and bubbles hard. It lights the 3D view only: what the soldiers can see by the game's rules doesn't change. NO: T does nothing.
- **Night vision needs gear** (NO): YES: in 3D only soldiers wearing or carrying night vision gear (power and flying suits, ion armor, hover tanks...: the list is in the mod, see [Night vision gear](#6-night-vision-gear)) and units that see in the dark, like aliens, get the NIGHT VISION button. NO: everyone has it, like on the isometric map.

### Graphics and light

- **Smooth lighting** (YES): YES: the battlescape's light (sun, flares, fires, personal lights) blends smoothly from tile to tile. NO: tile by tile, as the isometric map shades it.
- **Shadows** (YES): ray-traced shadows from the sun by day, from street lamps and your flashlight at night, and under every unit. Turn them off on slow machines.
- **Distance haze** (Normal): how soon far terrain fades into the haze, the sky's colour at the horizon. **Normal** from about half the view's reach (27 tiles), **Light** from three quarters, **Clear** only in the last tenth, so the world doesn't stop with a hard edge where the view ends. The fade blends the colours step by step, so far walls fade evenly. Under the sea the murk stays as it is.
- **Flashlight** (On): the 3D view's own light, on and off with **F** (every battle starts with it on). **On**: a wide beam from the soldier's shoulder that lights where you look, reaching 14 tiles, in the dark (night and dusk). **Dynamic**: a light on the weapon, a bright, tight round spot right on the crosshair with a faint halo round it, reaching 20 tiles, day or night, so a dark UFO, cellar or barn lights up by day too (in daylight it adds next to nothing). **Off**: no flashlight, and F does nothing. Only the 3D view is lit: what the soldiers see by the game's rules doesn't change. The suit light (L, or SUIT LIGHT) is separate: it is the game's personal light, round every soldier, by the game's rules (it lights their tiles, so aliens see them more easily at night).
- **Smoke** (Modern): how smoke looks, in two ways. **Modern**: the game's own smoke, made truly see-through. The original smoke is a stipple of dots that stood in for see-through smoke on the isometric map; here the dots are blurred into soft clouds (how densely they lie is how opaque the smoke is, their colours its colour, mostly grey), so the smoke keeps the game's shapes and colours but you see through it. Thin smoke, from a rocket say, is a light haze that's easy to spot and hides nothing; thicker smoke is a denser cloud. Where the soldier can't see through it by the game's own sight rules (every step through smoke uses up some of the soldier's sight, more in thicker smoke, less within two tiles of them or with heat vision), it turns solid, with a soft, billowy edge, as a smoke grenade's cloud should. Eyes with heat vision (an armor's `heatVision`, as the big mods' drones and scout gear have) see all of the smoke thinner too, in either look: 50% heat vision, half as thick (never less than a fifth, so it stays in sight). The clouds change slowly and smoothly from frame to frame, sway and bob a little, and a few wisps rise off them. **Original**: the original games' own smoke animation, their dots in their own colours (UFO's greys and browns, TFTD's white under the sea), its frames melting smoothly into each other instead of blinking. Where the soldier sees through the smoke it is one see-through cloud, more opaque the thicker the smoke; where they can't, by the same sight rules, the animation is stacked on itself, out of step and set off a little, until its dots close up and the cloud hides what's behind it (fewer layers on the cloud's outer tiles and where the sight only just runs out, so its outline stays loose and smoke-like). No grey, no wisps, and an explosion's flash and embers without our grey puffs (the smoke it leaves is the game's). With both, units the soldier can see stay clear through the smoke, and units the game hides stay hidden. Fire is drawn the same in both. (0.9's *See-through smoke* gives the first value: YES Modern, NO Original; 1.0's *Solid* becomes Original.)
- **Smoke opacity** (45%): how solid smoke you can still see through looks: the thinnest smoke's opacity, thicker smoke more in the same proportion, its wisps too (45% is the look before the setting). Where your soldier's sight runs out in the smoke it stays solid, by the game's rules; heat vision still sees it thinner. 5 to 80%, in steps of 5.
- **Night smoke opacity** (25%): the same in the dark, about half by default (a player: "non full smoke can be blinding for some players"): on a night mission, however lamps or the suit lights light the smoke, and by day where it hangs in the dark (an alien base, an unlit room), blended with the day value in between.
- **Weapon in hand** (YES): YES: the weapon in the soldier's hand shows at the bottom right of the view (bottom left in the left hand), Doom style, built in 3D from its inventory picture (little cubes, thickest in the middle). Every weapon is held barrel first with the barrel on the line to the crosshair, pistols, cannons and launchers as well as rifles (the games draw their weapons standing up, muzzle at the top), and your shots leave its muzzle. It bobs as you walk, kicks when you fire and dips out of sight while reloading, looking far down (from 45°, gone at 72°) and zoomed in. A melee weapon is held tip up and swung; a sword the art draws leaning (the big mods' swords) comes down edge first (its edge is the side lower in the picture). Anything else in the hand (a grenade, a medi-kit, a clip) is built the same way and sits in the bottom corner, its lower half out of sight. With the classic HUD shown it stays hidden (the HUD covers the bottom of the view). NO: nothing drawn.
- **Sprite upscaling** (Smooth + detail): soldiers, aliens, items on the floor, explosions and smoke seen up close are scaled up the way pixel art scales well (xBRZ, in the game's own colours). **Smooth** rounds off the stair-stepped edges; **Smooth + detail** also sharpens the shading a little; **Off** keeps the original pixels.
- **Unit size** (100%): soldiers and aliens drawn a little smaller, 80 to 100% of the isometric map's size, standing on the same spot, so tall sprites fit under low ceilings. Corpses and items on the floor shrink with them, so a soldier going down doesn't grow into a bigger corpse. Only the picture changes: what shots hit stays the game's (see *Hitboxes*; at about 80 to 90% the sprites come closest to their hitboxes' heights).
- **Muzzle flashes** (YES): YES: gunfire lights up its surroundings for a blink, units flash when hit, the view flashes red when your soldier is hit (and at each shot of a berserk soldier), and a blast flashes. NO: none of these flashes, UFO lights hold steady, and the helmet cameras' bad signal (coarse blocks, green squares, torn bands, a frozen picture) and their spells of static stay away. Turn this off if flashing light bothers you (photosensitivity, epilepsy).
- **Particles** (All): **Off** none; **Effects** sparks, debris, smoke, fire and the shots' trails (a rocket's smoke and the white specks it leaves hanging for about ten seconds, embers, the glow behind plasma); **All** also dust, fireflies, bubbles and weather. Shots look the same in all three: the rounds, beams and blobs, their start at the muzzle, the flash on the weapon, the kick and the marks they leave aren't particles.
- **UFO lights and aura** (YES): UFOs stand out from the Earth around them. Their hulls stay still and only their lights glow (lit panels, lift pads, screens, navigation lights, found in the original animation frames), pulsing gently in a slow wave round the UFO. Each UFO casts its own two-coloured light (its hull's hue close in, its lights' hue further out) on the ground around it, its underside and its skin, strongest at night, and colours its inside evenly, a fifth of the way to its light by day (each colour kept as bright as it was, strong colours kept almost as they are, so the art stays sharp); drifting motes and sparks wrap it from outside, never on its own tiles, and a beam of light rises up each grav lift. It all shows only once the UFO has been seen, so it never gives a UFO away; with Muzzle flashes off nothing pulses. The small scout's blocky hull is trimmed to its sprite's round outline (only the look: line of fire is still the game's). NO: the original animation frames (calmed, as everywhere in 3D).
- **Parts without shape** (Lifts): some parts the map draws have no shape in the game's data, so the 3D view had nothing to show: the wooden ladders of the big mods (Piratez), the upper pads of the UFOs' grav lifts (they showed as holes) and some lifts' pads in TFTD. **Lifts** shows those with the grav lift flag (lifts and ladders), and the cages of bars round some lifts (Cydonia's pyramids), as see-through ghosts of their art: a ladder stands upright where the map draws it, a pad lies on the floor, a cage's bars stand on all four sides of the tile (the map draws only the back two); dotted, a little thicker up close (half the pixels) than from afar (a third), so you see through a lift pad to whoever is above or below it. They are only a picture: shots, sight and walking go through them as the game says. **All** does the same for every part without a shape (some mods draw laser walls or decorations that way); **Off** shows none. The original games' own such parts that stand in the way have shapes made for them in the mod (`firstPerson: shapes:`, boxes of voxels cut to the art's outline): with **Lifts** or **All** UFO's shop racks, and in TFTD a platform in the liner's hold, the foam along its hull, the sunken plane's door frames, the box on the Triton's back, a stalk in the alien ship and a clamp in the base's hangar, stand as ghosts in their real shape, at the *Ghost opacity* setting like every ghost. A floor the map draws as a box of liquid, slowing whoever wades through it, gets a ghost surface at the box's height with **Lifts** or **All**: XPiratez's sewers under its bases are a green block of sludge about half a level tall (the game gives it only a floor's thin slab), and a soldier wading through it shows through the surface from the thighs down, as the map draws them sunk in it; kneeling, your eye is just under it. Found by the art alone (a floor drawn as a tile-wide box, voxels only in its slab, walking slower than on a plain floor), so any mod drawing liquid that way gets it; in UFO and TFTD no floor is drawn so. And a floor or wall the game gives only a few voxels where its art draws a whole grating, railing or mesh, so shots go through (XPiratez's catwalks in Chem Town, nine voxels a layer; railings with posts on every other layer; TFTD's cargo ship and port railings and chain-link fences; UFO's iron railings and hedges), has the rest of its art drawn as a ghost in its own plane (the floor's, the wall's edge), cut to the art, so a grating keeps its holes; its voxels stay solid, as the game has them. Found by the art too: the voxels all in the part's plane, filling under half of what the art draws there; not doors, whose open art shows the leaves swung. TFTD's own floors without a shape (the sea round islands, ports and ships, a strip of the cargo ship and T'leth's great lid) are drawn solid as on the map, with any setting.
- **Ghost opacity** (50%): how solid the parts without shape look up close, from 10% (a faint dotting) to 100% (solid); far off, 60% of that. 50% is as they always were.
- **See-through corpses** (NO): YES: a corpse or item whose picture is under the crosshair (or the free cursor) is drawn faint, at the ghost opacity, while you aim past it, at a unit, a wall or another floor: the game's shots pass through items, and a tall corpse (XPiratez's big beasts) hid what they reach. Aimed at the floor it lies on, it stays solid. NO: corpses and items always solid.
- **Discreet map edge** (NO): YES: no yellow tape and posts round the map; small red dots on the floor along its edge, a tile apart, mark it instead, with the land beyond as before.
- **Pixel size** (Auto): **Auto** gives full detail up to a 640×400 view and blocks of 2 on bigger ones (a 1920×1080 screen at the default battlescape scale makes a 960×540 view); **1** full detail; **2** or **3**: each ray colours a block of 2×2 or 3×3 pixels, chunkier but much faster on big screens or slow machines.
- **Frame rate limit** (60): the game's own frame rate cap, the same setting as in Advanced options: 60, 75, 90, 120, 144, 165, 240 or unlimited. 60 is steady on modest machines at 1280×800; raise it for a 120 or 144 Hz screen if your computer keeps up (the frame counter shows it).
- **Frame counter** (NO): the game's own frames-per-second counter in the top left corner (the same as F7).

### HUD

- **Crosshair** (YES): shows the crosshair. Its shape tells the fire mode: an open cross for snap, a scope ring for aimed, brackets with one dot per round for auto, an arc for a throw, a diamond for a launcher, claws for melee, rings for psi. NO: no crosshair (the shot info and the hit ticks stay).
- **Shot info** (YES): shows the fire mode, the hit chance and the time unit cost right of the crosshair (with *Crosshair* off, a little lower, so the middle of the view stays clear) (DUAL in front when both guns fire). Yellow: you can; red: you can't (not enough time units, no ammo, out of range).
- **Expanded crosshair info** (YES): under the shot info, the action's other costs, those it has: **EN** energy, **HP** health, **STUN**, **MOR** morale, **MP** mana ("EN 6  STUN 2"; a gain, a mod's negative cost, with a plus). Both guns' with dual wield, as the game charges them. A cost the soldier can't pay is red, and so is the crosshair: the game would refuse it. The original games' weapons cost only time units, so there it shows nothing; the big mods' weapons often cost energy. (A unit's name under the crosshair moves a line lower while it shows.)
- **Accuracy line** (YES): a green line under the crosshair, with a small notch up at each end, as wide as a shot can stray to either side (grey while it's not your move). With the game's own targeting (*FPS-style shooting* off), for a straight shot: as far as a **successful** accuracy roll can send the round sideways from the point under the crosshair, by the game's own roll (OXCE's `Projectile::applyAccuracy`). It is short: a roll under the accuracy strays at most half of a deviation that grows with the range (about 1.3 degrees to either side for a shot north or south on the map, half that east or west, unless OXCE's *Uniform shooting spread* is on; up or down far less), whatever the soldier's skill; how likely that is, the hit chance says, and a miss strays far wider. In FPS style: the spread, which 95% of rounds land within, widening with the bloom as you fire. While it shows, a pointed unit's name sits under it. NO: no line.
- **Unit names** (YES): the name of the unit you point at (the unit whose hitbox the game's line through the crosshair, or the free cursor, meets, as a shot would; else whose picture is there), just under the crosshair, as the game's unit screen names it: an alien by its race (or race and rank, where a mod shows those), a civilian by kind, a soldier by name, in green for X-COM, red for aliens, yellow for civilians.
- **Stat bars** (Scaled): how the bars of time units, energy, health and morale read, on the map and in 3D. The game draws a pixel per point and cuts a longer stat off at the bar's end (a player: a 300-HP unit with 100 health gone still showed a full bar; in 3D, whose bars are short, morale above 52 always looked full). **Scaled**: a pixel per point while every maximum of the stat among your units fits the bar, else one smaller scale for all of them, so a longer maximum is still a longer bar and two soldiers stay comparable (every vanilla soldier fits the map's bars, which look as before; the 3D view's are short, so there most stats shrink, all soldiers' alike). **Percentage**: each bar full at its own maximum. **Game's own**: a pixel per point, cut off at the bar's end. The numbers beside the bars always show the real values.
- **Hitboxes** (Aimed at): outlines round the units,  the voxels the game counts as that unit when a shot or a blow reaches it (its armor's footprint, from the floor to its height, kneeling or standing): green for X-COM, red for aliens, yellow for civilians. The sprites are only pictures, often taller or wider than what can be hit. **Aimed at**: only the unit you point at (its hitbox or its picture under the crosshair, as for its name), steady, so a shot at its picture that would miss shows at once; **Blink**: every unit in sight, the outlines fading in and out once a second; **Always**: every unit in sight, steady; **Off**: none.
- **Glowing outlines** (NO): YES: the hitboxes' outlines and the path of a throw glow, spilling a soft light round them (the bloom). NO: plain lines.
- **Downed unit icons** (Crosshair): small icons for a unit lying unconscious: a flame if it's burning, a drop of blood if it's bleeding (fatal wounds), a bolt if its health is draining away (shock), and Zz, out cold. Our own pixel art, in the order the game's own floor indicators take where a mod has them. **Crosshair**: for the body you point at (the crosshair, or the free cursor), just under the crosshair. **Always**: over every body in sight. **Off**: none. Only bodies you can see get them, not one behind a wall or in smoke too thick to see through.
- **Compass** (YES): a compass at the top right. The disc turns as you turn, with N, E, S and W going round it, under a fixed white arrow on top that shows your heading; a yellow triangle on the rim shows the way the soldier faces. With *Turning costs TU* on, a green arc shows how far the head turns for free (45° either side of the facing): within 10° of its end it turns yellow and says what looking further costs (**TURN 1 TU**), or red with **NO TU** when the soldier couldn't pay for the turn. And when a shot or a throw at what's under the crosshair would make the game turn the soldier first (aimed outside the eighth they face), a small orange triangle on the rim shows the way they'll turn, and the line under the dial says what the turn adds to the shot (**SHOT +1 TU**; red if the time units won't cover both). With free turning, the soldier faces the target for free before a shot, so that never costs anything.
- **Middle click identifies** (YES): YES: the middle button works as on the isometric map: a click on a unit you can see (its hitbox or its picture under the crosshair, or the free cursor) opens the game's own unit screen, who it is and what it carries; anywhere else it switches between mouse look and the free cursor, as always. NO: the middle button only switches mouse look and the free cursor.

### Sound

- **Dynamic music** (YES): YES: while aliens are in sight the music switches to a tenser track (the base defence one), and goes back to the battle music once they've been out of sight for a few seconds. NO: the battle music all along (switching it off mid-battle goes straight back to it).
- **Hit, kill and swing sounds** (YES): a short tick when your shot or blow hits an enemy in sight, a double tick for a kill, and a swoosh for your melee swings: the sounds the 3D view adds. NO: silent (the crosshair still marks the hit), leaving the game's own sounds, such as death cries and a weapon's own hit sound, alone. It also gives the heartbeat you hear when badly hurt: under half health, for the soldier you control on your turn, once they have been hurt in this battle (health carried over from earlier fights, as the big mods keep it, doesn't start it).

### Cheats

- **Doom mode (debug)** (NO): a cheat for testing and fun, and a first try at real time: in the 3D view the selected soldier never runs out of time units or energy on your turn: every shot, throw, blow and item is free. Grenades go off in real time, a few seconds after they're primed, wherever they are then (see [Items](#3-the-top-bar-and-items) and the settings below). Ammo still counts. DOOM MODE shows on the info panel while it's on.
- **Doom IDKFA** (NO): Doom mode plus endless ammo: the selected soldier's weapons spend no rounds in the 3D view, one-round weapons like the rocket launcher included, and loaded clips are filled up. IDKFA shows on the info panel.
- **Doom IDDQD** (NO): Doom mode plus invincible soldiers: X-COM's units take no damage at all (shots, explosions, fire, smoke, falls) and keep full morale, on either side's turn and in either view. Their four stats on the info panel turn silver, and IDDQD shows there. (Mind control still works: it does no damage.)
- **Doom prime on throw** (YES): in Doom mode, a grenade is armed as you throw it and burns from the moment it leaves your hand (FUSE in battle sets the seconds). NO: *prime* sets the seconds and pulls the pin, and the fuse burns in your hand.
- **Doom free grenades with airburst** (NO): in Doom mode, a grenade flies to wherever the crosshair points, by the 3D view's own throw instead of the game's: under a roof, over a wall, at a point in the air. It reaches as far as the soldier could throw it by the game's rules, but never further than a person can throw: about 40 m (25 tiles) for the strongest, less for weaker soldiers (the game's own throw reaches 21 tiles at strength 27 and 51 at 60). It flies a real arc and drops where it meets something, and it bursts **in the air** if its fuse runs out before it lands. NO: the game's throw (refused where the game can't throw), and a grenade whose fuse runs out in flight goes off the moment it lands.
- **Doom grenade fuse (seconds)** (3): in Doom mode, how long a grenade burns, from 1 to 5 seconds. The **SET FUSE** box in battle (the prime mode) changes it too, and it holds for every grenade until changed.
- Insert switches every cheat that's on off in battle, and the same ones back on (Doom mode alone if none was on); the warning line says which, and the settings keep it (as for Y, akimbo, and the SET FUSE box). The names are a tribute to Doom, whose cheat codes they borrow (see [Thanks](#12-thanks)).

## 5. Fire modes per weapon (FPS-style shooting)

With **Options → 3D VIEW → FPS-style shooting (easier)** on, the 3D view keeps the game's three fire-mode slots (keys Z / X / V, mouse wheel or J to cycle) but fires them FPS style. Each gun offers only the modes it has in the game; the table is generated from the game's rulesets (`items.rul`). Time units are the game's, as a percentage of the soldier's time units.

- **SNAP (Z)**: the game's single snap shot, from the hip (a wider spread than aimed)
- **AIMED (X)**: one tight round
- **AUTO (V)**: a click fires a burst; **holding the trigger keeps firing** until you let go, the clip is empty or the time units run out
- **sweeping a burst**: each round goes where the crosshair is as it fires, so dragging the mouse during a burst sweeps it along the drag. The game's two-point spray (Ctrl+Shift and two clicks, OXCE's) stays on the isometric map (F8 there and back)
- guided launchers (Blaster Launcher, Disruptor Pulse Launcher, ...) place their waypoints on the isometric map, as before

With akimbo on (Full or One-hand), the other hand's gun fires too, the two taking turns round by round (or as a volley, *Akimbo volley*): the same mode if it has it, otherwise its first shot, its whole burst (a pistol joins a rifle's auto with a snap, after the rifle's first round). There's no AIMED with a gun in each hand (Y switches akimbo off to aim), and with an arcing gun the two fire in turn (see *Full akimbo*).

### UFO Defense
| Weapon | Game modes | Quick | Aimed | Auto |
|---|---|---|---|---|
| Tank/Cannon | snap, aimed | SNAP (33 %TU) | AIMED (80 %TU) | — |
| Tank/Rocket Launcher | snap, aimed | SNAP (45 %TU) | AIMED (75 %TU) | — |
| Tank/Laser Cannon | snap, aimed | SNAP (33 %TU) | AIMED (75 %TU) | — |
| Hovertank/Plasma | snap, aimed | SNAP (30 %TU) | AIMED (60 %TU) | — |
| Hovertank/Launcher | guided launch | launch (map waypoints) | — | — |
| Pistol | snap, aimed | SNAP (18 %TU) | AIMED (30 %TU) | — |
| Rifle (2-handed) | snap, aimed, auto | SNAP (25 %TU) | AIMED (80 %TU) | AUTO hold (35 %TU per x3) |
| Heavy Cannon (2-handed) | snap, aimed | SNAP (33 %TU) | AIMED (80 %TU) | — |
| Auto-Cannon (2-handed) | snap, aimed, auto | SNAP (33 %TU) | AIMED (80 %TU) | AUTO hold (40 %TU per x3) |
| Rocket Launcher (2-handed) | snap, aimed | SNAP (45 %TU) | AIMED (75 %TU) | — |
| Laser Pistol | snap, aimed, auto | SNAP (20 %TU) | AIMED (55 %TU) | AUTO hold (25 %TU per x3) |
| Laser Rifle (2-handed) | snap, aimed, auto | SNAP (25 %TU) | AIMED (50 %TU) | AUTO hold (34 %TU per x3) |
| Heavy Laser (2-handed) | snap, aimed | SNAP (33 %TU) | AIMED (75 %TU) | — |
| Heavy Plasma (2-handed) | snap, aimed, auto | SNAP (30 %TU) | AIMED (60 %TU) | AUTO hold (35 %TU per x3) |
| Plasma Rifle (2-handed) | snap, aimed, auto | SNAP (30 %TU) | AIMED (60 %TU) | AUTO hold (36 %TU per x3) |
| Plasma Pistol | snap, aimed, auto | SNAP (30 %TU) | AIMED (60 %TU) | AUTO hold (30 %TU per x3) |
| Blaster Launcher (2-handed) | guided launch | launch (map waypoints) | — | — |
| Small Launcher (2-handed) | snap, aimed | SNAP (40 %TU) | AIMED (75 %TU) | — |
| CYBERDISC_WEAPON | snap, aimed | SNAP (30 %TU) | AIMED (60 %TU) | — |
| CELATID_WEAPON | snap, aimed | SNAP (30 %TU) | AIMED (60 %TU) | — |
| SECTOPOD_WEAPON | snap, aimed, auto | SNAP (30 %TU) | AIMED (60 %TU) | AUTO hold (35 %TU per x3) |

### Terror From The Deep
| Weapon | Game modes | Quick | Aimed | Auto |
|---|---|---|---|---|
| Coelacanth/G. Cannon | snap, aimed | SNAP (30 %TU) | AIMED (80 %TU) | — |
| Coelacanth/Aqua Jet | snap, aimed | SNAP (45 %TU) | AIMED (75 %TU) | — |
| Coelacanth/Gauss | snap, aimed | SNAP (33 %TU) | AIMED (75 %TU) | — |
| Displacer/Sonic | snap, aimed | SNAP (30 %TU) | AIMED (60 %TU) | — |
| Displacer/P.W.T. | guided launch | launch (map waypoints) | — | — |
| Dart Gun | snap, aimed | SNAP (20 %TU) | AIMED (50 %TU) | — |
| Jet Harpoon (2-handed) | snap, aimed, auto | SNAP (35 %TU) | AIMED (70 %TU) | AUTO hold (40 %TU per x3) |
| Gas Cannon (2-handed) | snap, aimed | SNAP (40 %TU) | AIMED (75 %TU) | — |
| Hydro-Jet Cannon (2-handed) | snap, aimed, auto | SNAP (35 %TU) | AIMED (80 %TU) | AUTO hold (40 %TU per x3) |
| Torpedo Launcher (2-handed) | snap, aimed | SNAP (40 %TU) | AIMED (80 %TU) | — |
| Gauss Pistol | snap, aimed, auto | SNAP (25 %TU) | AIMED (50 %TU) | AUTO hold (30 %TU per x3) |
| Gauss Rifle (2-handed) | snap, aimed, auto | SNAP (30 %TU) | AIMED (60 %TU) | AUTO hold (40 %TU per x3) |
| Heavy Gauss (2-handed) | snap, aimed | SNAP (40 %TU) | AIMED (80 %TU) | — |
| Sonic Cannon (2-handed) | snap, aimed | SNAP (50 %TU) | AIMED (70 %TU) | — |
| Sonic-Blasta Rifle (2-handed) | snap, aimed | SNAP (40 %TU) | AIMED (60 %TU) | — |
| Sonic Pistol | snap, aimed | SNAP (30 %TU) | AIMED (50 %TU) | — |
| Disruptor Pulse Launcher (2-handed) | guided launch | launch (map waypoints) | — | — |
| Thermal Shok Launcher (2-handed) | snap, aimed | SNAP (50 %TU) | AIMED (70 %TU) | — |
| DEEP_ONE_WEAPON | snap, aimed, auto | SNAP (30 %TU) | AIMED (60 %TU) | AUTO hold (35 %TU per x3) |
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

### Bad signal by mission

The helmet cameras (the soldier card's, the soldier cam, the map camera) glitch every few seconds on any mission, now and then tearing (bands of the picture slide sideways, a bright seam along their edge), and the soldier cam and the map camera get a spell of TV static every 10 seconds to 2 minutes. On the hardest ones, on the aliens' own ground, and on any mission while the soldier you watch is inside a UFO, they glitch a lot: a glitch every second or so, longer, in more and bigger patches, torn more often, with a little static all the time and longer, stronger spells every 5 to 40 seconds. Our mod lists those missions (alien deployment types) under `badSignal:` with level 2: Mars (both stages), an alien base, a base defence, and in TFTD an alien colony, T'leth and an artefact site. Level 1 is the usual, 0 none. A mod can list its own:

```yaml
firstPerson:
  badSignal:
    - missions: [STR_MY_ALIEN_FORTRESS]
      level: 2
```

### Underground maps

Past a map's edge the 3D view draws the world going on: the ground, the X-COM tape, the sky (under the sea, the water). Underground it draws rock instead, worked out from the map itself: a level whose outer ring of tiles is (nearly) all rock (walls and objects, or floors with something over them, like a base's sewers) is underground. So a map that starts on the surface and goes on underground has the sky over its surface and rock round its underground levels, and the ground beyond the edge lies at its surface's height; a map underground all the way up (every base, an alien colony's inside, Cydonia's complex, the XPiratez hideout) has rock round it and over it, in the colour of its own rock. Under rock no sun shines, so a ceiling there isn't shaded as turned away from it; and a dark rock ceiling (the hideout's) shows its texture, a few shades short of its darkest, as the rock beyond the edge does, where one plain colour would sink into near-black (a lighter one, like an X-COM base's grey, stays plain). A mission the view gets wrong can be listed as underground all the way up:

```yaml
firstPerson:
  undergroundMissions: [STR_MY_BUNKER]
```

Never list a map that starts on the surface, nor a mission name two games share for different maps (TFTD's seabed colony is `STR_ALIEN_BASE_ASSAULT`, as UFO's alien base is). Our list is empty: every map tried was told right by itself.

### Water

Past the map's edge the ground goes on as the commonest floor of the edge. The floors of the terrain data sets listed as water are the sea instead: where it reaches the edge, the sea goes on past it, and its top is drawn in waves (its colours taken from its own art). Ours lists TFTD's sea round islands, ports and ships:

```yaml
firstPerson:
  water: [SEA]
```

A list replaces the one before it (a mod adding its own water lists ours too).

### The item table

How an item looks in 3D is worked out from its rules and its inventory picture, and the item table (`items:`) can say otherwise for any item, by type. It holds two things so far (more may follow: it's meant to tune the 3D shapes item by item):

- `held:` **plate** (a flat piece one voxel thick) or **cushion** (rounded, thickest in the middle), how the picture is given volume in the hand and when thrown. Worked out: a melee weapon that isn't blunt (its damage isn't concussive or stun, the clubs' and stun rods') and whose picture is thin everywhere is a plate (swords, knives, axes); everything else a cushion.
- `shot:` **object** (flies as an object, with no powder: no muzzle flash, barely a kick, and a white streak unless it carries fire, acid or a shock, which keep their own look) or **powder** (a gun's round, with its flash). Worked out: weapons aimed with the throwing skill and not the firing skill (the big mods' bows, blowpipes, javelins) fire objects, and so do guns firing an arrow picture drawn mostly for those (the big mods' crossbows); every other gun fires powder.

Our mod's table has the XPiratez items the automatic rules get wrong (its wide blades, its gloves and claws, its harpoon guns and crossbows). A mod can add its own; a later entry overrides what it sets and leaves the rest:

```yaml
firstPerson:
  items:
    - types: [STR_MY_GREATSWORD, STR_MY_CLEAVER]
      held: plate
    - types: [STR_MY_SPEARGUN]
      shot: object
```

## 7. What you'll see

- The map rebuilt in 3D from the game's own voxel data, with the original art and proportions.
- Doors that swing (each turn of a door, open or shut, swings to where the game has it) and UFO doors that slide.
- Inside a UFO or a craft, its floor goes on under the diagonal pieces of its hull (the map leaves the ground there), coloured from every UFO floor beside it.
- Animated machinery and light, calm and eased: animated terrain steps a frame about three times a second and cross-fades, so nothing large blinks (the photosensitivity guideline WCAG 2.3 allows at most three flashes a second).
- UFOs that stand out: steady hulls with glowing, softly pulsing lights, their own coloured light around them and an aura of drifting motes (the "UFO lights and aura" setting).
- Light and shadow: lamps, the sun, your flashlight, flares. Indoors (a roof overhead, or rock) no sun shines: faces there are lit by the room's own light, as on the map.
- TFTD's sea in waves: swells rolling across it in the sea's own colours, lighter crests breaking into foam, deeper troughs, and where the sea reaches the map's edge it goes on past it.
- Bloom, night vision, rain, snow and dust, and in TFTD bubbles, marine snow and caustics.
- Smoke, fire, explosions and animated machinery that flow from frame to frame.
- Lifts you ride: standing on a grav lift (or a ladder) shows LIFT PLATFORM arrows, and the view rises or sinks to the next level, at half the walking pace (see *Walking speed*); ladders and lift pads that have no shape in the game's data show as see-through ghosts of their art.
- Units seen from the side they face you.
- Sound that fades with distance (and is a little quieter behind you).
- Melee that lands: the weapon winds up and drives in towards the crosshair with a swish, the view lunges, the blow jolts, and the target flinches.
- Weapons:
  - shots by damage type and by gun: a round with a short tracer streak for bullets, a beam for lasers, a glowing blob for plasma, a flame with a smoke trail for rockets and explosive shells (and white specks that hang along a rocket's path for about ten seconds), embers for incendiaries, a blue blob for stun; each bigger for a bigger gun, a pistol's the smallest, then a rifle's, an auto-cannon's, a heavy weapon's (a laser beam is wider for a heavier laser, and up close);
  - under the sea: yellow darts and harpoons, pale blue gauss beams, pale cyan sonic blobs, torpedoes with the same smoke and specks, and bubbles behind every shot;
  - the weapon in hand built from voxels, its barrel aimed at the crosshair: your shots leave its muzzle, in either hand;
  - arrows, darts, bolts and javelins (weapons aimed with the throwing skill, as the big mods' bows, and guns firing their arrow, as crossbows; see [the item table](#the-item-table)) fly as a white streak with no muzzle flash and barely a kick (a burning arrow trails embers);
  - recoil, muzzle flashes by weapon class (bullets, explosives, fire, lasers, plasma, stun) and size (bigger for bigger guns), hitmarkers with sound, enemies that flinch;
  - bullet holes (a dark core in a lighter ring) and scorch marks on whatever was hit, near or far (a far hole is a dot), blown-out walls in a burst of debris, burnt ground.
- When it gets tense:
  - enemy shots whizzing past you (you hear which side) with a flinch; a red arc on the screen's edge towards where a hit came from; blood on the view after a heavy hit or an alien's blow;
  - a red heartbeat at the screen edges when badly hurt;
  - going down: killed (red, then the helmet camera dies in static), unconscious (a fade to dark blue) or mind controlled (an orange haze), each with its message, before the next soldier's view comes in; killed by a Chryssalid (or a Tentaculat), TURNED INTO A ZOMBIE: down in red, then the body gets up again, lurching in a sickly green, until the camera gives out (their squad button shows a green skull, their camera says ZOMBIE);
  - on fire: the view in flames (a sheet of fire up from the bottom and the sides, embers drifting up, its light flickering), a BURNING banner and a flare-up when you catch fire, BURNING on the panel and the camera card; burning soldiers and aliens you see are in flames too;
  - breaking down at the start of a turn, seen through that soldier's eyes while they do what the rules make them do: panic (YOU HAVE A MENTAL BREAKDOWN, a fade to white, the heart racing, while they freeze, or drop their weapons and run) or berserk (BERSERK, white and red, every wild shot flashing red); then they come to, and their camera says PANIC or BERSERK for the rest of the turn;
  - tenser music while aliens are in sight;
  - "ENEMY SPOTTED!" and HIDDEN MOVEMENT banners (HIDDEN MOVEMENT glowing green on the civilians' turn).

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
- **Shots are drawn by kind,** not from a mod's own projectile pictures (an arrow, a bolt): a mod's arrows are white streaks, its custom damage types (Piratez's electric, warp, mind...) bullets for now. Using the mods' pictures is an idea for later.
- **Sound:** stereo can't tell front from back; sounds behind you are only a little quieter.
- **Not saved:** bullet holes, scorch marks, flares and each soldier's flight toggle are gone after a quickload or when the screen is rebuilt.
- **Leaning** shows what's round a corner, but the game's own sight still starts from the middle of the soldier's tile: an alien there shows once the soldier (or someone else) spots it. The weapon in hand stays level while the view tilts.
- **Leaning and fair play** (a player's note: the aliens can't lean by choice, so leaning gives the player a small edge). What the lean gives, exactly: it **shows no more than the game does**: the game's sight doesn't change, units it doesn't see aren't drawn, and the lean's room check leaves them out, so a peek shows terrain, never a hidden alien. A **shot** from a lean (and with *Lean shots*) starts from the game's own off-centre origins, a couple of voxels to the side. OXCE has a game option, **off-centre shooting** (`oxceEnableOffCentreShooting`), with which every unit, the aliens included, tries left and right of centre when the centre has no line of fire; with it on, the aliens have that reach too, and the player only gets to pick the side. For the most even game, play with that option on, or leave *Lean shots* off and lean only to look. (A shot whose exact point is blocked is tried from left and right of centre, and falls back on the game's own aim, left and right of centre too, only with that option on: the same tries the aliens get. FPS-style shooting is no part of the game's rules anyway, no roll and no "No line of fire", and its rounds leave from your eye.) An idea for later: shots from a lean only when that option is on. Letting the AI lean by choice belongs to the game itself (a fork such as Brutal-OXCE could), not to this add-on.
- **The game's own messages** (e.g. "has become unconscious") pause the 3D sequences behind them until you click OK, as all battlescape animation does.
- **Platforms:** Linux (x86-64, the Steam Deck included) is built and played here. Windows (64-bit) is built here too and played under Wine; the automatic builds check on a real Windows that it starts and finds the games. macOS (Apple silicon and Intel) is built by the automatic builds only, and hasn't been played on a real Mac yet: reports are very welcome.
- **Languages:** English (US and UK) only.

## 10. For developers

### What it is based on (exactly)

| | |
|---|---|
| Upstream repository | **https://github.com/MeridianOXC/OpenXcom.git** (OpenXcom Extended by Meridian) |
| Branch | **`oxce-plus`** |
| Commit | **`f0134ef8065799c7723953d64b565f7832bfd782`** — 2026-09-29 15:04:49 +0200, author Meridian, "Merge remote-tracking branch 'SupSuper/master' into oxce-plus" |
| Version string | **OpenXcom Extended 8.7.1 (v2026-09-19)** |
| OpenXcom3D | engine add-on and mod version **1.09** (`requiredExtendedEngine: OpenXcom3D`, `requiredExtendedVersion: "1.0.9"`: OXCE reads "1.09" as 1.9) |

`openxcom3d.patch` holds every change on top of that commit:

- the core hooks: 53 files, about 1,800 lines;
- the new `src/FirstPerson/` add-on;
- the `mod/OpenXcom3D/` data mod.

It applies with `git apply` to a clean checkout of the commit above.

Built with `-DOXCE_FIRSTPERSON=OFF`, it is plain OXCE again: the add-on is left out and the hooks are no-ops. A few fixes to the game itself stay in: a muzzle never inside what the unit stands on, and no division by zero for a sight line straight up or down.

To work on it: check out the commit above, `git apply openxcom3d.patch`, build (see [Option B](#option-b-build-from-source)), and make your changes; `git diff` (plus the new files under `src/FirstPerson/` and `mod/`) gives the next patch.

### Automatic builds

Every push and pull request runs two workflows on GitHub ([Actions](https://github.com/b0realis/OpenXcom3D/actions)). Neither machine has the original games' data, so each game is checked as far as it gets without it.

- **Build** builds the three downloads the way the releases are made and checks each where it runs:
  - *Linux:* the tarball and the AppImage, in a clean Ubuntu 26.04; SDL2 must have the releases' features, no more, no fewer (`tools/ci/sdl2_features.sh`). Then both start on Rocky Linux 9 (glibc 2.34, the oldest supported), Ubuntu 22.04, Debian 12, Fedora and Arch (`tools/ci/linux_release_check.sh`).
  - *Windows:* the zip, built on Linux by the build guide's own commands, then checked on GitHub's Windows (`tools/ci/windows_zip_check.ps1`): the game starts; `play.bat` with no data says where it goes; with a stand-in Steam (its folder in the registry, a second library on another disk) it finds both games, links them, writes the starting options and starts the game; and it follows the data when it moves.
  - *macOS:* the app, universal and for macOS 11 and newer (`tools/macos/`, `tools/make_release_macos.sh`), checked by `tools/macos/check.sh` (both architectures, no newer macOS asked for, only the system's libraries and the two inside, the signature), then started natively and under Rosetta.

  The downloads stay with each run for 30 days (the run's *Artifacts*). A tag `v*` also makes a draft release with all three, which a person reads over and publishes.
- **Build guide** (also once a week, since the systems change underneath it) runs the [build guide](https://b0realis.github.io/OpenXcom3D/)'s commands as a reader copies them: `tools/ci/guide_commands.py` reads them off the page and changes only what a machine without a person needs (package managers don't ask; the patch is the checkout's; the game is asked its version instead of started), each change marked next to the guide's own line. They run on fresh Ubuntu 26.04, 24.04 and 22.04, Debian 13, Fedora and Arch, in MSYS2 on GitHub's Windows, and `install.py --from-source` runs too.

Both get OXCE at the commit `install.py` names, with the patch applied (`tools/ci/fetch_oxce.sh`, which also checks that `src/` and `mod/` here match the patch).

### Code layout

**`src/FirstPerson/`, about 35,500 lines**

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
| `FirstPersonModeHud.cpp` | Top bar, squad buttons, info panel, soldier cards with the live helmet camera, the soldier cam, the map camera, banner, enemy markers |
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
| `FirstPersonVoxelModel.{h,cpp}` | An item's inventory picture made into a voxel model, a cushion or a blade's flat plate (the weapon or item in hand, thrown items) |
| `FirstPersonParticles.{h,cpp}` | The particle pool (sparks, smoke, weather...) |
| `FirstPersonSounds.{h,cpp}` | Sounds made in code (no files): hitmarker and kill ticks, heartbeat, dry-fire click, flare hiss, near-miss whizz, melee swoosh, the motion scanner's pulse and blips |
| `FirstPersonScanner.{h,cpp}` | The motion scanner held up in 3D: the game's own scanner picture turned with the view, a sweeping ring, blips that fade, and their beeps |
| `FirstPersonSpriteHits.cpp` | Setting *Full sprite hitboxes*: whether a voxel of a line of fire meets a unit's picture (a card through its middle facing the shooter, the frame from that side, kept per unit and side) |
| `FirstPersonQuickDeaths.cpp` | Setting *Quick deaths*: the falls under way, the frame of each, and a unit put in that frame's pose for drawing (on the map and in 3D) |
| `OptionsFirstPersonState.{h,cpp}` | 3D VIEW options tab: Settings / Controls sub-tabs, scrolling lists |
| `RuleFirstPerson.h` | `firstPerson:` ruleset section (header-only): enabled, view distance, window glass, weather, combat music, night vision gear, built-in items shown in hand, the item table (held shape, shots) |

What `FirstPersonView` draws:
- terrain from LOFT voxels with iso-sprite projection, animated frames, UFO doors cut out as they slide, doors swinging, window glass;
- smooth light plus sun face shading, Bayer dithering and horizon haze; a colour shaded past the darkest of its colour group turns black only where the place is really dark (night past the lights, an unlit room), and in a lit place stays that darkest colour (a burnt wooden floor dark brown, not a black hole); colour washes (hurt, dying, unconscious, mind control, breakdown, zombie); purple edges through a unit under our mind control; night vision recolouring; the fire over the view of a burning soldier;
- the outside world: ground, X-COM tape and posts (or the discreet edge's dots); the sky. Underground, rock: below a map's surface beyond its edges, and over a map that's underground all the way up (a base, an alien colony's inside, Cydonia's complex), found from the map's own edge (see [Underground maps](#underground-maps));
- terrain not explored yet: plain slate-coloured shapes (never black, which reads as missing), turning to a soft misty blue-grey further off (from half the soldiers' sight range on); a hole through the ground floor shows the earth below;
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
| `Projectile.h` | `getAmmo()` and `getWeapon()` getters (trail style from the ammo's damage type, size from the gun) |
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
| `TileEngine.cpp` | Upstream UB fix found by UBSan: `canTargetUnit` and `checkVoxelExposure` don't divide by zero when the shooter and the target stand in one column (the side scan lines then lie on the centre line); `getOriginVoxel`: on something tall under a ceiling, where the game pushes the muzzle under the floor above and 4 voxels lower, it never goes below the top of what the unit stands on (it went a voxel into it, and shots struck whatever stood about as high nearby); an FPS-style straight shot (`freeAim`) asks the first-person view, which moves its origin across the tile to the 3D eye's place, at the same height |
| `Explosion.{h,cpp}` | `getLastFrame()`: the last frame of an explosion's own animation (the 3D view's frame blend stops there) |
| `State.h`, `Game.{h,cpp}` | Generic hook: the top state may ask for mouse capture (relative mouse look); released with the cursor centred for every other state, each release counted (`getMouseReleases`) so the 3D view can ignore the cursor's jump |
| `UnitSprite.{h,cpp}` | `setViewRotation()`: draw a unit from any side |
| `Options.cpp` / `Options.inc.h` | `createOptionsOTHER()` → `FirstPerson::createOptions(_info)`; declarations at the end of the file |
| `OptionInfo.cpp` | Loading a key binding clears Shift, Alt and Ctrl, except for `keyFpsLeanHold` (the lean key, held with the strafe keys, Left Shift by default) |
| `OptionsBaseState.{h,cpp}` | 3D VIEW tab via `FirstPerson::hasOptionsTab/createOptionsTab` |
| `Mod.{h,cpp}` | Parses `firstPerson:` at the end of `Mod::loadFile` |
| `ModInfo.cpp` | Engine row `{"OpenXcom3D", 1.0.7}` (add-on builds only; 1.07) |
| `CrossPlatform.cpp` | Add-on builds use their own user/config folders (`~/.local/share/openxcom3d`, `~/.config/openxcom3d`, `Documents/OpenXcom3D`, …). Data folders are shared with stock OXCE |
| `OpenXcom.2010.vcxproj{,.filters}` | The add-on's files |

## 11. Version history

### 1.09 (2026-10-30)

Playtest reports on UFOs from inside, on smoke at night, on a walk through a door that never ended, on shots off the crosshair and on melee by the compass, at floors and straight up, on FPS style's lost snap shot, on names by the picture, on tails in the floor, on corpses in the way and on Page Up, settings for new players, holes under the zoom, a clean view and the motion scanner, full sprite hitboxes, quick deaths, and a logo of its own (patches 189–206).

- **A UFO's light inside it, even** (the owner, playing: "The ufo lights and aura put a while screen overlay of dots (daylight mission)... the aura effect should be only on the outside of the ufo to keep screen clean and sharp in the inside"): the UFO's light was mixed in by a dither, a whole pixel of its colour here and there, which inside a UFO, its soft light everywhere, laid a grid of dark dots over the view. Now the UFO itself (its inside and its hull) takes an even tint: tables of each colour with one to twelve twelfths of the light mixed in, each picked as bright as the colour itself, so a lit wall is one tint and its panels keep their detail; greys take all of it and strong colours little (a red lift pad stays red); inside, about a fifth of the way to the light's colour by day and two fifths at night. The pool on the ground around it keeps its sparkle, and the aura's motes and sparks never start on the UFO's own tiles (its rim, an ellipse, cut inside the corners of a boxy UFO). The grav lifts' beams stay.
- **Smoke opacity, by day and at night** (a player: "non full smoke can be blinding for some players. Suggest a setting for smoke opacity (for not full smoke) defaulted at 40% (or what current is). also suggest seperate night smoke setting, at half of current value by default"): settings *Smoke opacity* (45%, the look before) and *Night smoke opacity* (25%) for the smoke you see through, the thinnest smoke's opacity with thicker smoke more as before; the night value on a night mission (lit smoke on a dark night is the blinding kind) and where the smoke hangs in the dark by day, blended in between. Where sight runs out the smoke stays solid.
- **No walking on out of the map through a door** (a player: "a unit of mine walks into a door backwards or sideways and appears to walk infinitely out of bounds. When swapping to iso, they appear to of stopped in last available tile"): through a door (or a diagonal gap) the camera walks on by itself until it is well inside the new tile, a stretch only 6 voxels wide, and one slow frame (a tenth of a second moves it 6 to 13 voxels) could jump right over it: the camera then walked on for ever, the keys doing nothing, while on the map the soldier stood in the new tile. Now it never goes past the new tile.
- **Shots nearer the crosshair** (a player, with screenshots, issue #36: "odd cases of very visually off shots", "when aiming along flat ground, occasionally lasers jut in strange directions"): with the game's own targeting, when no muzzle point's line reaches the point under the crosshair (along a floor, the line from the gun, lower than the eye, grazes the bumps on the way; over a counter it meets its edge), the shot went for the game's own aim: the tile's middle, or another part of the unit. Now first for the nearest point within 4 voxels (a quarter tile; upwards first) whose line meets the same target near the crosshair as the eye sees it; only then the game's own aim. The game's line of fire and accuracy as before.
- **No turn south at your own tile** (the same report: "Firing into a wall you are up against to the west, somehow forces you to look south"): OXCE turned a unit aiming at its own tile (a wall it stands against, the floor at its feet) to face south, and with *Turning costs TU* charged for the turn: its direction to a point straight above or below was atan2 of 0 and -0. Now it keeps its facing, on the isometric map too.
- **Melee by the crosshair, not the compass** (a player, GitHub issue #25: "aiming at north west cabinet strikes the unit to north with melee"): a blow strikes a unit under the crosshair on any tile next to the soldier, diagonally too, the soldier turning to them first (paid with *Turning costs TU*; the compass says HIT +1 TU); a unit under it farther off is out of range; a wall, door or object under it within reach takes the blow (with *Melee hits walls and downed* or the mod's terrain melee; without, the blow strikes nothing). Only with the crosshair on none of these (open floor, the sky, out of reach) does the blow go the way the soldier faces, as on the map.
- **The unit pointed at by its hitbox** (a player, GitHub issue #26: the name showed only while a pixel of the unit's picture was under the crosshair; X-Com Files' small swarmids, moving oddly, made it blink): the unit you point at, for its name, the middle click, a medi-kit's patient and a melee blow, is now the one whose hitbox the game's own line through the crosshair (or the free cursor) meets, as a shot would, which doesn't change as the picture moves; only else the one whose picture is there (a unit far off, a few pixels across).
- **Tails on the floor, not in it** (a player, GitHub issue #31: X-Com Files' Asmodean Cobra "in the ground" in some rotations; "use the same technique as items"): a unit stands on a card at its feet, and what the isometric art draws below its feet (the front half of the floor's diamond: a Snakeman's tail, a long body in some facings) sank into the floor, ending in a straight cut. Now that part lies flat on the floor in front of the unit, as an item's does; for most units it's only a few pixels of the feet. A unit in the air keeps the upright card.
- **See-through corpses** (a player, GitHub issue #35: very tall corpses, XPiratez's boomasaurs and mongorns, "visually block shots", though the game's shots pass through items): a setting, *See-through corpses* (off at first): the corpse or item pointed at is drawn faint, at the ghost opacity, while you aim past it; aimed at the floor it lies on, solid.
- **Page Up flies** (a player, GitHub issue #32: "if I press page up, its very obvious my intention is to fly. Let it toggle flying on, if NOT inside lift"; the owner: and page down off): off a lift or ladder, Page Up with flight off switches flight on and flies up a level in one press (it only said "Flight off"); Page Down with flight on, standing on the ground, switches it off. W looking up, the lift's arrows and the classic HUD's buttons stay as they were.
- **Settings for new players** (a player, GitHub issue #27: "Many settings would probably be better changed as defaults or renamed to be more friendly to new players"; the owner's picks): new installs start with the game's own mechanics where the 3D view adds its own: *Movement* Tiles, *Flight cost overrides rules* NO (flight at the game's cost; it was free), *Jump over low obstacles* NO, *Night vision needs gear* NO; and *Shotgun pellets in flight* YES. *Hitboxes* has a new choice, **Aimed at** (the unit under the crosshair only), the new default. *Faithful X-COM targeting* is now shown the other way round as **FPS-style shooting (easier)** (NO), so it's clear it changes combat. *Full akimbo* and *Realistic akimbo* are one setting, **Akimbo** (Off, Full, One-hand; the one that was on carries over), and Y does nothing while it's Off. The settings list is in sections: View and camera, Movement, Aiming and combat, Graphics and light, HUD, Sound, Cheats. A player's own saved settings stay as they are.
- **Bullet holes grow with the zoom** (a player, GitHub issue #41: "Zooming in on bullet holes has them APPEAR to become smaller", a heavy laser's especially): a mark up close is drawn no more than 6 pixels from its middle, so it doesn't fill the view, and that cap counted the zoomed view's pixels: zoomed in 4x, the hole stayed as small on the screen while all round it grew. The cap now counts as without the zoom.
- **A clean view** (a player, GitHub issue #37: hiding the HUD left the crosshair and the shot info, "full hudless mode would be useful for a project"): P goes round three steps now: the HUD hidden (the crosshair stays, as before), then a clean view (the crosshair with its lines, the accuracy line and the name under it gone too), then all back.
- **The motion scanner held up in 3D** (GitHub issue #39; the owner: "orient the screen to where player is looking in 3d and make some retro animation like concentric circles from the center and bumping into signals that refresh. And some cool scary beep upon detection (like alien movie)", and "the 3d scanner should not overtake whole screen"): used from the 3D view, the scanner's round screen shows at the bottom of the view, the game going on round it, turning as you look round; a ring sweeps out every two seconds with a whoop, and each movement lights up as the ring reaches it with a pip, higher the nearer. It stays up while it's the item in hand. On the isometric map the game's own, as before.
- **Full sprite hitboxes** (the owner's, GitHub issue #42: "Setting for hit-boxes the same size as sprites!"): a setting (off at first): units are hit, and seen, wherever their full sprites show, a card facing whoever fires, for everyone's shots and line of fire checks, in 3D and on the map; big units too (a Cyberdisc is its disc, not its four tiles).
- **Quick deaths** (a player's report, GitHub issue #40: deaths that don't hold up an auto fire, all at once, "like doomguy chaingunning down zombies"): a setting (off at first): a unit killed or knocked out is down at once, its fall playing on its own at the game's pace with no turn, several at once, in 3D and on the map; the soldier you look through falls on their camera card while the view goes on to the next soldier at once.
- Fixed: the X-COM tape along the map's border read backwards ("MOC-X") on its south and west sides (a player's report, GitHub issue #44); it reads left to right from inside the map on every side.
- **The snap shot back in FPS style** (a player, GitHub issue #28: "Snap shot appears to vanish in favor of hold auto"): with *Faithful X-COM targeting* off, the quick slot (Z) of a gun with an auto shot fired one burst instead of the snap, which a click on AUTO already does; the gun's single snap shot couldn't be fired. Now Z fires the game's snap shot (its time units, the hip's wider spread), as on the map; a click on AUTO fires one burst, held it keeps firing.
- **Melee at floors, ceilings and straight up** (a player, GitHub issue #33: melee "doesn't apply to floors sections above or bellow, or units directly above you on lifts"): the setting **Melee hits floors** (YES, with terrain melee on) lets a blow strike the floor or ceiling under the crosshair, by the game's own hit (the game's terrain melee knows no floors). A unit straight above or below you under the crosshair (flying just above, in a shaft) is struck when the game's own line from the blow reaches it; never through a wall or floor: on a UFO lift the upper pad lies between, so a unit on the next level stays out of reach.
- **A logo of its own** (the owner's): the alien in the yellow X with 3D in its corner, in `docs/logo/` (as supplied, cleaned, and the icon in every size), now the icon of the game's window, the Windows game, the Mac app, the AppImage and the menu entry `install.py --desktop` makes (it replaces OXCE's icon files, so a build from source has it too), and on the web pages.
- **Melee on big units with full sprite hitboxes** (GitHub issue #57: "Melee is impossible, as sprite is considered too far"): melee reach is the game's own, by its own hit shapes, whatever the setting (a 2x2 unit's picture stands in its middle, beyond the tile the reach is measured to). And with the setting on, the game's line of fire checks (the AI choosing a shot, reaction fire, the shot's aim) aim at a big unit's middle, where its picture stands, so it can be shot from every side.
- **Quick deaths' falls at the game's pace** (GitHub issue #58: "should still play death animation at normal speed"): the fall plays at the game's own speed again; nothing waits for it, so a burst still fires on.
- **Akimbo never pairs a gun that blocks both hands** (GitHub issue #48: two pump shotguns were drawn as a pair, then wouldn't fire): a gun the game marks with a red 2 (it won't fire one with the other hand full) fires alone, in 3D and on the map; and *Realistic* is now called **One-hand**, which says what it does.
- **Auto-equip guns first** (GitHub issue #46: soldiers still spawned with claws, shovels and swords in XPiratez and X-Com Files): that's OXCE's own auto-equip, which counts a two-handed melee weapon as a rifle (ours has been OXCE's, unchanged, since 1.05); a new setting, off at first, hands out guns before melee weapons, in the mod's item order (1.04's way).
- **The aliens' turn cuts to a soldier shot at by an unseen alien** (GitHub issue #49: at night "gunfire from unseen units doesn't switch to units who can see the gun fire"): with *Aliens' turn cameras*, an alien none of your soldiers sees firing at one of them cuts the view to that soldier at once, turned to where the fire comes from (it only popped up their camera card).
- **HIDDEN MOVEMENT glows green on the civilians' turn** (GitHub issue #55): as the map's turn bar turns green for the civilians, the banner's glow does too; red and purple on the aliens' turn as before.

### 1.08 (2026-10-27, re-released 2026-10-28)

The two things found while taking 1.07's pictures, the sea in waves, and a play report on shotguns (patches 168–172); re-released the next day with seven more play reports and a bug hunt folded in (patches 173–188): aiming at the fringes, FPS-style shots from the eye, an accuracy line, stat bars that read, a check before an unsafe landing, sludge you wade through, catwalks and railings that show, and more.

- **Rooms in their own light** (the owner, on a 1.07 picture of a shop: its grey inside wall came out black): the 3D view shaded every face by the sun, three shades darker for one turned away from it and three for one facing it from under the roof's shadow, on top of the game's own light, which already makes a roofed tile darker. Indoors (a floor or an object on any level above, as the game counts a roof, or under rock) faces are now shaded as at night, only a little by their direction, and nothing there is in the sun's shadow: inside walls and floors look as on the map. A wall in another test scene that came out all black is grey.
- **The sea goes on past the island** (the owner: "Fix"): beyond the map the ground was one floor, the commonest of the map's edge: on an island map, whose south side is sea, the island's grass, so the sea stopped at the edge. Where the sea reaches the edge it now goes on past it (the mod's `water:` list of terrain data sets, TFTD's SEA), the land beyond the rest.
- **Waves** (the owner: "some nice sea wave-like effect (cheap on performance)"): TFTD's sea is drawn as waves rolling across it, three trains crossing at their own lengths, headings and speeds, their crests bending, in the sea's own colours (its art's plain sea-green, the teal of its deep in the troughs, foam on the highest crests), the same on the map and past its edge, so there are no seams at the tiles any more (the art's black edge drew a grid). A few table look-ups a pixel.
- **Each pellet on its own** (a player's report on OXCE's "shotgun void": the lead pellet decides the whole blast, and when it flies off the map every pellet is lost with it): a new setting, on: no pellet is ever lost, and with pellets in flight each hits as it gets there, the deaths with the last to land. The same for the two rounds of an akimbo volley (the other round hung in the air when the first left the map). Misses still flying when the last hit lands fly on to the map's edge, only to be seen. Off, the game's own way. On the way: FPS-style shooting's pellets all flew down the round's own line (now spread as the game spreads pellets); in 3D every pellet is drawn as the round is ("the pellet that has ACTUAL sprites, while all others are weird wireframe squiggles"); a shotgun in the other hand of a volley sent its pellets at the map's corner when they didn't fly.
- **Akimbo on the map** (the owner): Ctrl+Shift+click on a hand's weapon marks the soldier's two guns to fire together on the isometric map, both at once (a mini target by each) or in turn (two), as akimbo does in 3D; kept in the save (see [Akimbo on the map](#akimbo-on-the-map)).
- **Stat bars that read** (a player: "say a unit has 300 hp and takes 100 damage. The bar still appears full"): setting *Stat bars*, Scaled by default: when a maximum is longer than the bar, the stat shrinks for all your units alike, so the bars read right and soldiers stay comparable, on the map and in 3D (where morale above 52 had always looked full).
- **Check the landing** (a player: "No check that below terrain is walkable. Xcom files vampire castle is a good example, in iso, you are forbidden from jumping down into the blood moat, but in first person, you can freely jump off and soft lock"): setting *Check the landing*, on: a step off a ledge onto ground no one can walk on, or where no step leads on, stops at the edge with a warning; pushing that way again, or a jump, goes over.
- **The heartbeat only when it should** (a player: "Low hp heartbeat plays where it shouldn't - Plays when unit selected is unharmed"): it beats only for the soldier you control, on your turn, once hurt in this battle (not for health carried over from earlier fights, nor for a soldier watched in the aliens' turn), and *Hit, kill and swing sounds* off silences it.
- **Every round leaves its mark** (a player: "Super fast weapons, like xcom files light minigun, leave no holes"): the 3D view followed the game's one round in flight once a frame, so a round that started and landed between two frames (a fast gun's, or on a slow computer) went by unseen; now the map notes every landing and the view marks each one.
- **The map's cursor after a shot in 3D** (a player: "If you fire a shot in first, then move to iso, your cursor appears stuck on fire mode, but left click actually makes a unit turn"): the map shows the normal cursor, which is what a click there does; to fire again on the map, pick the fire mode as usual.
- **Akimbo bursts for any guns** (a player: the Mandrake and Origami were only an example, "better fixed as a GENERALIZED formula ... Make it apply to snap too"): it was already general (1.07, each gun fires its own mode's rounds, whatever the guns); now tested for multi-round snap shots too.
- **Aim on the open sky** (a player: "Aiming at edge of map behaves VERY weird (turn on laser, just scroll aim, some if it aims almost sideways)", "Aiming directly above a street light ... plenty of no line of fire"): with nothing under the crosshair, a shot went for a point 20 tiles along its line, which past the map's edge was pulled back one axis at a time onto a tile far off to the side, and the hit chance went blank. Now it goes for the point of open air on the crosshair's line, inside the map (and within the weapon's range), with the game's own targeting and in FPS style; the laser runs to the crosshair and the hit chance shows. (Empty air can be aimed at: the game takes an empty tile as a target, and carries the line on past the point.)
- **FPS style shoots from the eye** (the owner: "B - always part of FPS, never in faithful"): an FPS-style straight shot leaves from your eye's place on the tile (leaning out too) at the gun's height, so what you see past a corner you can hit; the game's own targeting keeps the game's muzzle points, as on the map.
- **The exact point from the off-centre muzzle points** (a player: "Trying to aim past the very corner of a craft opening occasionally snaps aim onto the hull"): with the game's own targeting, a blocked exact point is tried from the game's middle muzzle point and, with OXCE's *Off-centre shooting*, from its left and right ones, before the game's own aim takes over; the shot fires from the one that clears.
- **The accuracy line** (the owner: for both styles, a setting; first a dotted ring round the crosshair, then "a green line centered under the crosshair that widens with inaccuracy and has small notches", as the ring "disturbs the cleanness of crosshair"): setting *Accuracy line*, on: a green line under the crosshair, a notch up at each end, with the game's own targeting as wide as a successful roll strays to either side at that range; in FPS style the spread, which the dotted ring round the crosshair showed before.
- **Ghosts at the ghost opacity** (the owner: "tie it to the ghost opacity variable setting ... it looks far too ghostly"): a ghost no one can walk into (UFO's shop racks, TFTD's shapes) no longer shows a third fainter than a lift's; every ghost shows at *Ghost opacity*.
- **A glass door seen through as it swings** (a player: "Gas station door opacity is turned off when animation for opening plays"): the door swinging open is drawn from its voxels, but its panes went solid until it stood still; now they stay see-through.
- **No late flash after the map** (the bug hunt after 184): the 3D view follows the shots only while it draws; back from the map (or after the aliens' turn watched there) it finished the round it had been following, and played every round that had landed meanwhile, their sparks and laser beams all at once. Now a view drawing again after a pause starts from then on.
- **Catwalks, railings and gratings** (a player's picture: XPiratez's catwalks in Chem Town "look like nothing"): a floor or wall the game gives only a few voxels, where its art draws a whole grating, railing or mesh, has the rest of its art as a ghost in its plane (see *Parts without shape*); its voxels stay solid.
- **More from the bug hunt:** a steep shot at the open sky from the top of the map turned the soldier south first and, with the game's own aim, went for his own floor (now a tile on along the ray's heading); an akimbo volley's other round, landing first, flashed twice; a diagonal push into a corner of two unsafe ledges warned every frame and never went over; the sky shot's range margin was too thin on diagonals; a round that ended off the map, never seen, sparked at the edge; the sludge's floor was darkened with no surface over it (*Parts without shape* off); five comment lines in the robust pass split its shards mid-step.
- **Sludge you wade through** (a player: "sludge sewers in piratez base, despite no voxel date, they are very intentionally about thigh deep"): a floor the art draws as a box of liquid, slowing walking, gets a ghost surface at the box's height (see *Parts without shape*): XPiratez's sewers under its bases, where a soldier now shows sunk to the thighs, as on the map; in 1.08 a flat green floor with everyone standing on top.

### 1.07 (2026-10-27)

Two play reports on 1.06 (patches 159–167).

- **Units on a floor above stay up there** (a player: their sprites showed "on floor bellow them slightly"): a unit's picture reaches a few rows below its feet (the front of the floor's diamond), and from the room below those hung through the ceiling. A unit standing on a floor is now cut off at the floor's underside.
- **A mod's own mode names by the crosshair** (a player: X-Com Files' XM29 calls its aimed shot "Launch Missile", the crosshair said AIMED): a shot, blow, medi-kit, psi or priming action the mod renamed reads as in the action menu, in capitals (LAUNCH MISSILE, QUICKBURST x3, PUNCH!).
- **Shapes for the parts without voxels** (a player: UFO's shop racks "have no actual voxel data in oxce, but are still unwalkable"; the owner: every one in UFO and TFTD): UFO's metal racks (four legs, two shelves and the goods) and six TFTD parts stand as ghosts in their real shape with *Parts without shape* (Lifts or All); TFTD's sea round islands, ports and ships, which showed the grass outside the map, is water now, solid as on the map, as are a strip of the cargo ship and T'leth's great lid. A ghost no one can walk into is a third fainter than a lift's or a ladder's ("a subtle indication between walkable, and unwalkable tiles").
- **Holes in walls and doors are seen through** (a player: the gas station's glass doors "transparent in isometric mode are opaque in first person"): where a wall's or door's art leaves pixels transparent inside its outline (the glass of UFO's gas station doors, TFTD's chain-link fences and gates, gaps in a fence) the 3D view sees through as the map does. Shots still stop there as the game says.
- **A shotgun no longer slows the map down** (a player: "Buckshot weapons have SIGNIFICANT lag when fired iso mode ... this DID NOT happen in 1.04"): with *Shotgun pellets in flight* (1.05 drew the pellets on the map) every pellet was checked on every tile of the screen, with a trace down to the floor for each of its 35 sprite points: 22–46 ms a frame with a shotgun's 7 pellets (more on a bigger screen, which shows more tiles), so the blast crawled. Each is now drawn only on the tiles it spans, as the game's own round is: about 1.3 ms. Not OXCE's pellet rules, and without the setting there was no lag.
- **Two guns take turns, round by round** (a player's report: akimbo fired "ESSENTIALLY like a 2 round shotgun": both rounds aimed at the same moment and landing together, so they couldn't burst through two enemies in a row as an auto burst's rounds can): the guns now fire one round each in turn, the chosen gun first, each round aimed as it leaves and its hit, deaths and destroyed walls worked out before the next one goes, as the game does an auto burst's. The old way stays as *Akimbo volley* (the setting replaces *Akimbo in turn*, whose rounds waited for each other, faster; its key now switches between the two). A volley's round that gets there first shows its hit at once instead of hanging in the air until the other lands (a player: "bullets hit something and just sit there, floating").
- **Each gun its whole burst** (a player: with XPiratez's Linux Mandrake (4 rounds) chosen, the Linux Origami (5) fired one): the other gun fell back to a single shot when its burst was the longer; now each fires its own (4 + 5), the longer one finishing alone.
- **Both guns' lasers** (a player: "when using dual fire, both weapons need laser's for trajectory predictions"): with *Show trajectory* the other gun's line or arc shows too, from its own muzzle.
- **The glow over black takes the light's colour** (a player: "anything with a glowing aura (hitbox, weapon laser, fireflies) turns a creepy red glow when background is entirely black behind it in tftd"): the bloom brightens each pixel a few shades within its own colour group, and TFTD's blacks belong to groups that climb to bright red. A pixel too dark to show its group's colour now takes on the light's own: a green hitbox glows green, the red laser red, fireflies yellow.

### 1.06 (2026-10-26)

Four more play reports from the same player (patches 137–158; 141, 155 and 157 noted for later).

- **The soldier's rank before the name** (a player: "the RANK mini symbol of a unit on the right to the unit name", then "less obtrusive" to the left, and level with the letters): on the 3D info panel, the soldier cards and the soldier cam's title. A mod's own tiny ranks if it has them (XPiratez); stock OXCE's set is an empty placeholder, so for UFO and TFTD the game's own battlescape rank badge, averaged down to 13×11 (the owner chose it from previews), its bottom on the letters' bottom.
- **A big unit no longer blinded in a wide door** (a player's report: a 2×2 Reaper in a temple raid's two-tile door): a big unit's eye is the middle of its footprint, where a wide door's frame stands. Now an eye inside anything solid moves to the nearest free point in the unit's footprint, ahead first.
- **Melee by the crosshair** (a player: melee couldn't strike a structure diagonally in front; "base it entirely on where first person crosshair is"): with *Melee hits walls and downed* (or the mod's terrain melee), a blow strikes the wall, door or object under the crosshair within a tile and a half, on a tile next to the soldier, diagonally and a level up or down too.
- **Medi-kits by the crosshair** (a player: "the same targetting system ... proposed for melee"): a unit on any tile next to the soldier, diagonally too (the soldier turns to them first, paid with *Turning costs TU*), a body at the feet by its picture, or the soldier, if the kit allows; a unit farther off is out of range, and a body elsewhere says to stand over it.
- **Items on stairs, seen from the side** (a player: half an item's picture went into the steps; the owner: "just on top of stairs at half height"): on stairs, ramps and slopes an item lies level at the ground's height under its middle (on the steps' edges) and is drawn over the steps round it (by up to 6 voxels, never through a wall farther off). Flat floors, and anything with a floor over it (a shelf under a terrace), are as before.
- **Every cost by the crosshair** (a player: "crosshair should display an energy cost per action if applicable, as well as any other costs (health, stun, morale, whatever)"): a third line under the shot info, setting *Expanded crosshair info* (on): energy, health, stun, morale and mana, a cost the soldier can't pay in red.
- **Shots into the sky just vanish** (a player: "Shots make puffs of particles when hitting skybox"): the game's shot stops where it leaves the map, in the air, and the view made its sparks, dust and bullet hole there. A shot that ends with nothing solid near now leaves nothing.
- **Heat vision thins the smoke you see** (a player: a scout drone's 50% heat vision didn't look any better in 3D): it counted already for how far the soldier sees into smoke; now the smoke drawn over the picture is thinner by as much, in both looks (50%: half as thick, never under a fifth).
- **"More expensive", not "dearer"** (a player read it as a typo for "wearer"): two guns together cost the more expensive gun's shot and a tenth; the akimbo settings' descriptions say so.
- **Numpad diagonals** (a player: the numpad should make diagonals from two keys pressed together, as W, A, S and D do): 8 and 4 pressed together step ahead and left in one move (a press waits 60 ms for the other key); held together, they walk on diagonally.
- **The lift's cage** (a player: the bars round Cydonia's pyramid lifts should stand "on all sides of tile, like a cage"): a part without shape whose art is bars on the floor's two back edges stands on all four, and shows with *Parts without shape: Lifts* too.
- **Ghost opacity** (a player: a setting for how see-through the ghosts are): *Ghost opacity*, 10 to 100% (50% as before).
- **Climbing by the view in flight, and the climb arrows** (a player: "letting me look up and hold w to fly straight up", and an indicator by the compass): W looking up past 30° flies up a level with flight on, as on a lift; arrows left of the compass show where W can climb, the one it takes now lit.
- **Spray bursts** (a player: X-Com Files' miniguns spray between points with Ctrl+Shift on the map; "needs first person equivalent"): with the auto shot of a spray weapon, Ctrl+Shift+click marks the first point (the tile under the crosshair), each click the next, and the one that reaches the weapon's number fires the burst along them, as on the map; the right button takes back the last. Each point shows as a light green pole on its tile with a mini target where the rounds go and its number; a dotted line joins them.
- **A slanted weapon picture lies along the gun** (a player's screenshot: XPiratez's quarterstaff, a gun-type staff, stood in the air): a weapon picture drawn on a slant is no longer taken as standing straight up when that would make it wider across than along; it lies on the line to the crosshair like any gun.

### 1.05 (2026-10-26)

From the same player's play reports on 1.04 and the owner's own looks at it: a crash ending a battle found from the player's log, the cameras' feeds, the top bar, akimbo in turn, shotguns on the map, arcs at the exact point.

- **The map camera is a live feed too** (the owner, looking at 1.04's screenshot: it lacked the soldier cam's "static and screen tear along with red dot"): it has the SOLDIER CAM's blinking red dot before the name, and both now have TV static only now and then, not a sprinkle all the time: a spell every 10 seconds to 2 minutes of watching, rising from nothing to a snowstorm and falling back over a few seconds, each at its own pace. On the hardest missions and inside UFOs a little static stays all the time and the spells come every 5 to 40 seconds, longer and stronger. Every helmet camera glitches more often: every 1.5 to 5 seconds on any mission, torn now and then too (only the hardest missions tore before); there every 0.3 to 1.5 seconds, torn more often. And each camera's glitches differ from game to game, where every game played the same pattern.
- **Akimbo in turn as akimbo together, but for the turn** (the same player: "The idea of each bullet firing one side, one gun then the other is correct, but besides that one difference, it should be identical to normal dual fire setting"). 1.04 fired the two guns in turn each at full accuracy and its own cost (two pistols' snap shots: 20 time units). Now in turn is akimbo's own rules, each gun 30% less accurate and the pair for the more expensive gun's shot and a tenth (11 time units), and both rounds fly 30% faster than the game would fly them (from whatever its *Fire speed* setting, the gun and the ammo make it), so waiting for one round per gun isn't sluggish: two pistols in turn are over in about 1.2 s, two together in 1.4 s, one pistol in 1.1 s. One gun, or two together, fly at the normal speed; arcing guns, which always fire in turn after the landing, keep full accuracy and their own costs, as chosen for 1.04.
- **Shotgun pellets in flight on the map too** (the same player: firing a shotgun in 3D and switching to the map at once showed its blast flying, "something previously not possible", and asked for it there as standard, the round given a sprite where a mod's shotgun has none). With *Shotgun pellets in flight* the pellets fly beside the round on the isometric map as in 3D, drawn with the round's bullet sprite; shotgun ammo without one borrows the bullet sprite its damage type's ammo uses most. The other gun's round of an akimbo pair is drawn on the map too. And every pellet lands like a round, with or without the setting (the owner: "all pellets should impact, play sound and show bulletholes"): its hit sound (a few to a volley: the mixer has 16 channels) and, in 3D, sparks and a bullet hole; they used to land silent and without a mark.
- **Arcing shots at the exact point** (the owner: "Arcing weapons no longer can target sub tile accuracy"). The game aims an arc at a few fixed points of the tile (a unit's middle, an object, the floor's middle), and a forced one at the tile's corner, which *Force arcing shots* fired at whenever the game found no curve. With *Aim at exact point* an arcing shot now goes for the point under the crosshair first, a spot on the floor, a forced one too; its path shows it.
- **Primed grenades say turns** (agreed for this release after the owner took PRIMED 3 for seconds in Doom mode): the panel says **PRIMED 3 turns**, **PRIMED 1 turn**, **PRIMED this turn**.
- **A crash ending a battle, and freezes, fixed** (a player's report: a crash abandoning a long random mission, and freezes opening the inventory). Each time the game's window was resized (a window manager can resize it on every change of screen size, as the inventory's own makes, with OXCE's *Allow resize* on and a battlescape scale that follows the window), the 3D views were made again, a freeze, and the old ones stayed in the battlescape screen's list of its surfaces, to be deleted a second time when the battle ended: a crash, the likelier the more resizes a mission had seen. Now a remade surface takes the old one's place in both of the screen's lists, a resize to the same size changes nothing, and the 3D views are made again only when the canvas's size really changes.
- **Auto-equip is OXCE's own again** (the owner, relaying the player: releases should leave the game's auto-equip "ENTIRELY unchanged from a normal openxcom build, as modders custom create power rankings for auto equip"): 1.04's guns-first order is undone; the game hands out a craft's kit as OXCE does.
- **Shorter rocket trails** (the same player: rocket and cannon trails lasted "far, far too long"): the white specks a rocket or an explosive shell leaves along its path hang 8 to 12 seconds instead of 25 to 35 (the owner: the specks only; the smoke puffs and the bubbles under the sea as before).
- **Doom mode's grenades are always in real time** (the owner: "realtime grenade setting should be always on with doom mode", "Separate setting for realtime grenades not needed any more"): the setting *Doom real-time grenades* is gone; in Doom mode grenades burn for seconds, as *Doom prime on throw*, *Doom grenade fuse* and *Doom free grenades* say.
- **The soldiers in the middle of the top bar** (the owner: on a bigger screen the bar sat at the left; at the original resolution it "occupies whole top of the screen"): the soldier numbers with ◄ and ► stand in the middle of the screen, ISO/3D, HUD and INV at the left edge and the armor buttons and END at the right edge, so the bar spans the whole top on any screen. And a wide screen shows more than 10 soldiers at once, as many as fit at full size ("If screen permits we can have more soldier numbers up"). On the original resolution a full squad fills the bar as before. On the map, where the HUD button hides, INV takes its place, so no gap is left between 3D and INV (the room it frees fits an 11th soldier on the original screen).

### 1.04 (2026-10-25)

From two more of the veteran player's reports on 1.03 (a long one, then a shorter one), and the owner's asks: the build guide, the automatic builds and a first macOS build came with it.

- **The mod switches itself on:** the first time the game finds the OpenXcom3D mod in a profile (one that doesn't list it yet), it starts on. A game built and set up by hand had it off, and F8 did nothing until it was switched on under Mods. A player who switches it off keeps it off. (The releases' profiles list it on already.)
- **The build guide** on GitHub Pages: [b0realis.github.io/OpenXcom3D](https://b0realis.github.io/OpenXcom3D/), every step for Linux, Windows with MinGW (MSYS2, tested with its own compiler) and the Windows build on Linux, with the snags people hit.
- **[Automatic builds](#automatic-builds)** on GitHub: every change builds the Linux, Windows and macOS downloads and checks them on five Linux distributions, a real Windows and a Mac; the build guide's commands run on six Linux distributions and in MSYS2 on Windows. With it comes **a first macOS build** (Apple silicon and Intel, macOS 11 and newer), [untested on a real Mac](#option-a-on-macos-a-first-untested-build).
- **O opens the door you look at** (a player's report on 1.03): it opened only a door straight along the soldier's facing (the view rounded to the nearest of eight ways), so a door looked at from the side, or from the tile beside it while walking along a wall, didn't open. Now O opens the door under the crosshair if it's within reach, else the nearest one the view looks towards, and turns the soldier to it by the rules; with no door within reach it says so.
- **Tile moves stop at doors** (a player's report on 1.03): with *Movement: Tiles* (and with the numpad), a step into the tile before a closed door walked on through the door, paying for it and stepping into whatever waited behind; even a tap did. Now a move stops in front of the door, a key held from tile to tile stops there too, and only a fresh press opens the door and walks through. Free movement is as before: pushing against a door goes through it.
- **Bullet holes on everything, near and far** (a player's report on 1.03): holes on objects such as the desert's dunes came and went by range, by where the shot hit and by calibre. A rifle's hole vanished beyond about 7 tiles (at 640×400; 13 at 1280×800), a plasma scorch three times as far, and a hole on art that stands out in front of its collision shape (the dunes' steps) sat inside it, unseen. Now the marks are drawn in 3D on the surface the view shows: a far hole is a dot, a near one a small disc with a darker core, and it lies on slopes and objects as on walls.
- **Chain explosions at the map's pace** (a player's report on 1.03: a building-sized chemical tower in XPiratez took ages to go up in 3D). From a chain's seventh explosion on, the game runs each one as fast as it can, which the isometric map keeps up with; in 3D every step of it waited for a whole frame. Now, while explosions are on, the battle catches up on the steps it missed during a frame (up to 8, OXCE's own frame skipping): a 34-explosion chain at 1280×800 went from 16.5 s to 6.6 s, the map's 6.1.
- **Night vision at any time** (a player's report on 1.03): in 3D, N (and the NIGHT VISION button) switched night vision only when the game was idle on your turn, through the game's own button; during a chain of explosions, a shot in flight or the aliens' turn it did nothing. It only changes how the picture is drawn, so now it switches whenever you press it. (On the isometric map the game's own rule stays.)
- **Unexplored terrain misty with distance** (a player's report on 1.03: on open maps the far horizon showed a skyline of buildings that weren't there). It was terrain the squad hasn't seen yet, drawn as plain slate shapes, which grew darker with distance and stood out against the sky. Now they turn to a soft blue-grey further off (from 10 tiles, at most part of the way into the haze), and fade out fully at the view's far reach, as explored terrain does. Up close they're the slate shapes they were.
- **Underground maps in rock** (a player's report on 1.03: at the edge of an XPiratez base, a gap in the rubble looked down into a sea of green sludge, its sewers' floor repeated beyond the map; and the sky showed over every base). The 3D view now finds a map's surface from its edge: below it, rock beyond the edges; over a map underground all the way up, a rock ceiling instead of the sky (or, under the sea, the water), in the colour of the map's own rock. A map that starts on the surface keeps its sky. Outdoor maps look as before.
- **Force arcing shots, and new trajectory looks** (a player's report on 1.03: a Panzerfaust aimed at the side of a building was refused, and no path showed; holding Ctrl forced it). A new setting, *Force arcing shots* (on): an arcing shot the game would refuse fires forced, as with Ctrl held, its path in yellow; where the game's own aim works, it's kept. And a straight shot's path (*Show trajectory: All*) is now a thin red laser from the weapon, with a dot where it meets something; throws and arcs keep the cyan dashes.
- **Black only where it's dark** (found while testing the bullet holes: a house's wooden floor shot through, the burnt floor it turns into, was drawn pure black in a lit bedroom; so was the rock over the XPiratez hideout's rooms seen from below). Dark art shaded past the darkest colour of its group no longer turns black in a lit place: it stays that darkest colour; black still comes where the light is really dark (night past the soldiers' lights, unlit rooms), fading in. Under rock, a ceiling no longer takes the sun's shading, and a dark rock ceiling (the hideout's) shows its texture a few shades short of its darkest, as the rock beyond the map's edge, instead of one flat near-black colour.
- **The map camera** (a player's report on 1.03: "a mini first person window in iso mode, player turn ... in the upper left, and a toggle for it on a hotkey"). A new setting, *Map camera* (off; small, medium or large): on the isometric map in your turn, the selected soldier's helmet camera in a window at the top left, looking the way they face, or at the target while you aim on the map. A click on it goes to 3D, a right click opens the soldier cam there, P shows or hides it on the map; it keeps clear of OXCE's touch buttons.
- **No grass in the Abductor's north-west corners** (a player's report on 1.03). The UFO's floor under its diagonal hull took its colours from one neighbouring floor only; at the two north-west corners that one is a hull tile's own floor, just a triangle, and the rest stayed grass (or sand). Now each spot takes them from whichever UFO floor beside it has one there. Checked in every UFO: none is left.
- **Ghost lift pads see-through up close too** (a player's report on 1.03: "lift flanks are a core combat threat"). Ladders and lift pads without a shape in the game's data (*Parts without shape*) turned solid within a tile and a half; now they stay a dotted mesh up close (half the pixels), fading to a third far off as before.
- **Standing on something tall under a ceiling** (a player's report on 1.03: "vision is compressed down to the floor"). The city map's crate stacks reach almost to the ceiling: the game pushes a soldier's sight there down under the floor above, and the camera went 3 voxels lower still, onto the crates, the view a slit. Now it sits only 1 voxel under the game's eye, the room in view. And the game's muzzle, pushed under the floor and 4 voxels more, no longer goes below the top of the crates (a shot could strike a crate at that height nearby instead of flying over it).
- **Akimbo with arcing guns** (a player's report on 1.03: "Dual fire doesn't play nice with arcing weapons like grenade launchers"). The other gun never fired (its round can't follow an arc, nor fire one beside it), yet the chosen gun's shot was half as accurate for it. Now, when either gun's shot arcs, the two fire in turn, as the player suggested: the chosen gun first, the other once its shot has landed, each at full accuracy. Two straight guns still fire together.
- **No aimed akimbo, and a smaller akimbo penalty** (a player's report on 1.03: "how exactly do you aim 2 guns at once? ... aim penalty on dual could be something more like 30, instead of 50"). With two guns that pair, Aimed isn't offered (Y switches akimbo off to aim), and each gun fired together keeps 70% of its accuracy instead of half (FPS style: the spread about 1.4 times as wide instead of twice).
- **Falls, climbing and flying by the walking speed** (a player's report on 1.03: falling "should be much faster, and linked to walk speed setting even in cases of ladders and flying"). Every level up or down took 1.4 s, whatever the speed, while the game itself is done in about 0.3 s (and a fall glided as slowly as a lift). Now a level up or down (a lift, a ladder, flying, a fall) takes a tile and a half's walk at half the walking speed (0.75 s at speed 4), a fall speeding up as gravity would and landing with a little dip of the view.
- **Sweeping a burst** (a player's report on 1.03: the map's Ctrl+Shift spray has no first-person equivalent): kept on the isometric map, where two points on the ground make sense (F8 there and back). In FPS style a burst already sweeps when you drag the mouse during it: each round goes where the crosshair is as it fires. Now in the manual.
- **Bursts at the weapon's own pace** (a player's report on 1.03: the X-Com Files' turbo laser fires very fast on the map, much slower in 3D). On a slower computer each step of a burst waited for a whole 3D frame; now the game catches up while a shot flies or hits, so a burst takes as long as on the map (a fast 10-round burst on one core: 1.2 s before, 0.6 s now, as the map), and chains of explosions too.
- **A purple haze for units under your mind control** (a player's report on 1.03: "a purple haze aura on screen border"). Looking through an alien you've taken over, the view's edges are washed purple, breathing gently (its helmet camera on the card, the soldier cam and the map camera too), and its button on the top bar (after your soldiers') shows its number in purple, so it's plain whose eyes these are.
- **Soldiers spawn with guns** (a player's report: "a bit of a pain spawning with melee"; it was the game itself, on the map too). When the game hands out the craft's kit to soldiers without a saved layout (in New Battle, or on a mission soldiers you have never equipped), a two-handed melee weapon counted as a rifle, and the kit came in an order that changed with the mod list: in XPiratez the same soldiers held quarterstaffs, pitchforks and stop signs, and rifles once the mod list changed. Now guns come first (a gun: a firearm whose shot reaches past 12 tiles, since XPiratez's staffs and swords "lunge" a tile or two), then melee weapons, each in the mod's own item order, the same every time. The inventory's auto-equip key (Z, before a battle) goes the same way.
- **The aimed path under night vision** (a player's report: night vision washed it out, the forced shot's yellow unnoticeable). Night vision turned the path green with everything else. Now it's light in the goggles, as a laser sight is: a white-green core (white mixed into the goggles' brightest green, so it stays in their colours) in a green glare, the laser's dot white in the middle. Colour can't tell a forced arc there, so its dashes become dots. By day nothing changes.
- **No more "No line of fire" with the shot on the unit** (a player's report: "I definitely have a shot, my weapon preview IS ON the unit, but it says no line of fire"). With the game's own targeting and *Aim at exact point*, the game checks the line from the weapon to the exact point; whatever else it met first refused the shot, and OXCE's *Off-centre shooting* was never tried (the game tries it only in its own aim at a unit). Now a blocked exact point falls back on the game's own aim (any part of the unit a line reaches, off-centre with OXCE's option), and the path shows the line that will fire. A unit's head poking up into the tile above (on stairs or raised ground) refused even a shot that hit it: the game found no unit on that tile; now the shot aims at the unit's own tile. And a new setting, *Force blocked shots* (off): a shot nothing reaches fires anyway along the line to the crosshair, its path yellow.
- **No moving or turning mid-shot with tile moves or paid turns** (a player's report: "Player can attempt walking despite firing, or looking mid shooting, giving odd results ... ok for free look and free move"). With *Movement: Tiles* (and the numpad), a press during a shot was kept and walked a tile the moment the shot ended, and a key held through a burst walked on after it; now presses then do nothing, and a held key needs a fresh press. With *Turning costs TU*, the view could look all the way round during a shot, and after it the soldier turned step by step to catch up, paying for each; now the head stops 45° off while a shot is under way. Free movement and free turning are as they were.
- **A big unit's corpse in one piece** (a player's report: "2x2 unit corpses look very segmented and chopped to pieces, especially with reduced sprite size"). A cyberdisc's or a reaper's corpse lies in four parts, one on each of its tiles, and each was drawn on its own tile: shrunk round its own middle (with *Unit size* below 100%, gaps opened between them) and standing up on its own. Now, while the four parts lie there, they're put together as the map draws them and laid as one picture at the middle of the four tiles, scaled round it.
- **Items lie on slopes, not in them** (a player's report: "Item sprites on stairs or slopes partially clip inside. Perhaps a tilt of sprite"). An item's art lies flat on the floor towards you; where the ground in front rises towards you (a sloping hull, the seabed's ridges, rubble), that flat part sank into it. Now the 3D view looks at the ground in front of each item and tilts the flat part up the slope, as the player suggested. (On stairs the steps' edges hide what's behind them anyway.)
- **Show which way units face** (a player's report: "Alt being able to show unit facing would be useful, similar to how bleeding indicators are? Some units are not sprited to display facing"). Hold **Left Alt** (a key of its own, set under 3D VIEW; Alt or Ctrl may be bound): a ring round every unit in sight with an arrow the way it faces, in its side's colour, like the hitboxes, drawn over everything.
- **Shotgun pellets in flight** (a player's report: two guns' rounds fly side by side, "Give shotguns actual pellets, if only in first person?"). A new setting, *Shotgun pellets in flight* (off: the game's own way, the whole blast landing the moment it's fired). On, in 3D a shotgun's pellets fly beside its round, each to where the game's spread sends it, and hit as it lands.
- **A shot the game will refuse shows dimmed** (the owner's ask after the "No line of fire" report). With *Faithful X-COM targeting*, a straight shot the game will refuse shows its path as a dotted dark red instead of the red laser, so the refusal never comes as a surprise.
- **Akimbo for the more expensive gun and a tenth** (a player's report: "dual fire is DOUBLE cost for 2 identical guns, which makes it statistically unjustifiable"). Two guns fired together paid both shots, at 30% less accuracy each; now the pair costs the more expensive gun's shot and a tenth more (two pistols' snap shots: 11 time units instead of 20). Guns that fire in turn still pay each its own: they're two shots at full accuracy.
- **Akimbo in turn** (a player's report: an assignable button for "akimbo panic (two shots at once), akimbo focused (one weapon after the other)"). A new setting, *Akimbo in turn*, and a key for it (*Akimbo style*, unbound): the other gun fires a moment after the chosen one, each at full accuracy and its own cost, one click for both.
- **An off-hand shotgun fires its pellets** (a player's report: with a shotgun beside another gun, "making shotguns appear to fire singular slugs"). The other gun's round never traced its pellets; now a shotgun in either hand fires them (flying, with *Shotgun pellets in flight*).

### 1.03 (2026-10-23)

From a player's reports on 1.02, the owner's asks, and a look at XPiratez o2 (the Isolated Valley's temple ladder; Piratez plays in 3D).

- **Windows (64-bit)** (your ask): a first build for Windows 10 and 11, a zip with `play.bat`, which finds the games' data in Steam as `play.sh` does and links it in with a junction. Made on Linux with llvm-mingw and tested under Wine; see [Option A on Windows](#option-a-on-windows-the-prebuilt-release-64-bit). The 3D view's upscaling thread runs at low priority there too.
- **Aliens' turn cameras on the map:** with *Alien turn in 3D* off (or the map shown), the setting did nothing. Now the helmet camera cards pop up over the map: a soldier shot at (looking at the shooter), hit, mind controlled or going down, a soldier firing back (looking at the target), and the nearest soldier who sees the alien moving (looking at it).
- **Corpses through floors:** a body lying on the floor above no longer hangs down through the ceiling when seen from below (its front half was drawn on its standing card, below the floor), and one lying on a tall shelf, crate or stairs right under a floor is pressed down to fit under it instead of poking up into the room above. A downed unit's icons don't show through a ceiling either.
- **Sprint (Caps Lock):** steps are the game's run, faster, by the running rules; **SPRINT** under the compass.
- **En passant (`):** enemies coming into sight don't stop the walk, as Shift does on the map; **EN PASSANT** under the compass.
- **Flight off on a lift:** a soldier in flying armor with flight switched off rode a lift or a ladder to its top, and one more Page Up flew them on up into the air, where no key worked (Page Up and Page Down said "Flight off", W did nothing). Now with flight off they keep to the lift as anyone else does: no up arrow at the top, and the message names the flight key. One in the air with flight off comes down to land. Ladders on walls (OXCE's) no longer refuse such a soldier altogether.
- **Climbing with W and S:** on a lift or a ladder, W looking up climbs and looking down goes down (S the other way round), level by level while held.
- **Bloom on white:** whatever glows next to something white (snow, a bright sky, white paint) no longer turns it black around itself (the brightest shade of the greys was the empty black one); and a new setting, **Glowing outlines** (default NO), says whether the hitboxes and the path of a throw glow at all.
- **Doors no longer fold into their wall** (a player's report on 1.02): the game's open door is a door too, standing in the tile's other wall, and the open key turns it back. 3D took that for another opening, hid the shut door and drew the open one swung into the wall beside its hinge, the doorway looking open while the game had it shut. Now every turn of a door swings it through its tile to where the game has it, and a door turned again mid-swing swings back from where it got to. (A door opened from its own side used to swing away from you and stay on the far side, where the game doesn't have it.)
- **Helmet cameras glitch a lot on the hardest missions** (your ask): on Mars, in an alien base, in a base defence (and in TFTD's colonies, T'leth and artefact sites), and inside any UFO, the cameras' bad signal comes every second or two, in more and bigger patches, and the picture tears now and then (your ask); elsewhere it stays rare. The missions are a list in our mod's rules (see [Bad signal by mission](#bad-signal-by-mission)).
- **A failed close-quarters check turns your view** to where the shot was knocked (aside, at your feet or up; a player's report): the soldier faced that way already, and the view snapping back turned the soldier again, at a cost with *Turning costs TU*.
- **No grass inside UFOs:** a UFO's map leaves the ground's floor where the diagonal pieces of its hull stand, and 3D showed it inside; the UFO's floor now goes on inside up to the hull.
- **Swords and knives are flat** (your ask): a blade in the hand, or thrown, is a thin plate one voxel thick instead of a cushion; clubs, maces, fists and everything else keep the cushion.
- **The item table** (`firstPerson: items:` in our mod's rules, see [Changing it](#changing-it)): what the 3D view isn't left to work out, item by item, the automatic rules deciding the rest: so far how an item is held (plate or cushion) and what its shots are (objects or powder); our mod's table has the XPiratez items the rules get wrong.
- **Weapons of the big mods (XPiratez):** a sword drawn leaning comes down edge first (it led with its back); a weapon's picture is measured whole, as the art draws it (the hand slot shifted one drawn wider than its item partly off the picture, its tip cut off); arrows, darts and javelins fly as a white streak with no muzzle flash (they looked like bullets).
- **Straight up and down:** the view looks up and down to 89° (it stopped at 80°), and a unit straight overhead (a flyer, someone on a floor above) faces down at you instead of showing its edge, so it can be seen and aimed at.
- **Priming in one click:** the prime fire mode opens the game's timer at once, without the item's menu first; a psi amp's or a mind probe's use targets as panic does, no menu first.
- **Helmet cameras with a bad signal:** the card's camera and the soldier cam now and then glitch like a digital feed for a moment: coarse blocks, a green square or two, a frozen picture. Rare, brief, and off with *Muzzle flashes*.
- **The squad on the wheel** (your ask): with the cursor free (C), the mouse wheel over a soldier's button steps the soldier card, or the soldier cam, through the squad one soldier at a time, round from the last to the first and scrolling the bar along (it used to scroll the bar only, so with ten or fewer soldiers it did nothing); over ◄ and ► it still scrolls.
- **The trajectory follows a lean** (a player's report): the path shown for a shot now starts from the side the shot leaves from, so it moves when the soldier leans, as the shot itself does (it always started from the middle).
- **Corpses no longer swell** (a player's report): with *Unit size* under 100%, corpses and items on the floor are drawn at the units' size too, so a soldier going down no longer grows into a bigger corpse.
- **Parts without shape** (new setting, default Lifts): ladders and lift pads that the map draws but the game's data gives no shape (Piratez's wooden ladders, the UFOs' upper lift pads, which showed as holes) show as see-through ghosts of their art, solid up close.

### 1.02 (2026-10-22)

Gunplay: shots, flashes and the weapon in hand gone over class by class, in UFO and TFTD.

- **Shots with Particles off:** the setting only stops the particles now. Before, it also stopped the 3D view from following the shots: no laser beam, every round a pale dot from the middle of the screen instead of the muzzle, no flash on the weapon and no kick.
- **Every gun its own round:** pistol, rifle, auto-cannon and heavy cannon (laser pistol, laser rifle and heavy laser; plasma pistol, plasma rifle and heavy plasma; the TFTD guns the same) fire bigger and bigger rounds, beams and blobs, with bigger muzzle flashes; bullets fly with a short tracer streak; a laser beam is wider up close; under the sea darts, harpoons and shells are yellow (they melted into the sand).
- **Rockets:** a flame with a thicker smoke trail and white specks that hang along the path for half a minute and grey away (torpedoes the same).
- **Weapon in hand:** every weapon is held barrel first on the line to the crosshair, as the rifles were (pistols, cannons, launchers and the heavy weapons stood too upright; the rifle, the Sonic Pistol and the Thermal Shok were held backwards).
- **Doom free grenades:** a grenade thrown with the game's Throw (picked from its menu) no longer stays aimed: the next click threw the same grenade again from wherever it lay.
- **Smoke: two choices, Original and Modern** (*Solid* is gone, with the grey puffs it left hanging in the air; a *Dither* choice, 0.9's smoke brought back on a player's wish, was tried and left out). **Original** is now dense where the soldier can't see through the smoke: the game's smoke animation stacked on itself there (less on the cloud's outer tiles, so its outline stays smoke-like), so a smoke grenade's cloud hides what's behind it, and one see-through cloud where they can.
- **Settings:** *Particles* shows Off, Effects and All instead of 0, 1 and 2.

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
