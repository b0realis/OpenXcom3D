# OpenXcom3D manual

Version 0.7. OpenXcom3D is **OpenXcom Extended (OXCE)** with a first-person 3D battlescape: an engine add-on (`src/FirstPerson/`, with small hooks in the core) and a data mod (`OpenXcom3D`) that switches it on.

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

## 1. Installing

You need your own copy of the original game data: *UFO: Enemy Unknown / X-COM: UFO Defense* and/or *X-COM: Terror From The Deep*, e.g. from Steam or GOG.

Add-on builds keep their own user and config folders, so an existing OXCE profile is never touched:

- the release's `user/` folder, when started with `play.sh`;
- otherwise `~/.local/share/openxcom3d` and `~/.config/openxcom3d`.

### Option A: the prebuilt release (Linux x86-64)

The release folder `OpenXcom3D-0.7-linux-x86_64/` contains:

```
bin/openxcom              the engine (OXCE 8.7.1 + OpenXcom3D)
data/common, standard     OXCE's own data
user/mods/OpenXcom3D      the mod
user/options.cfg          a starting profile: UFO + OpenXcom3D, 1280x800 window
play.sh                   starts the game with this folder's data and profile
install.py, README.md, OpenXcom3D_manual.md
```

It needs these system libraries:

- SDL 1.2 (or sdl12-compat), SDL_mixer 1.2, SDL_image 1.2 and SDL_gfx;
- libpng, zlib, OpenGL.

On Debian/Ubuntu:

```sh
sudo apt install libsdl1.2debian libsdl-mixer1.2 libsdl-image1.2 libsdl-gfx1.2-5 libglu1-mesa
```

Package names differ a little between releases. `ldd bin/openxcom` lists anything missing.

#### With the installer

```sh
python3 install.py
```

It asks for:

- the install folder (default `~/Games/OpenXcom3D`);
- the original UFO data folder (it needs `GEODATA`, `GEOGRAPH`, `MAPS`, `UFOGRAPH`);
- the TFTD data folder.

Common Steam locations are found on their own.

It then copies the game, **links** (never copies or modifies) the game data folders into the profile, writes `options.cfg` and creates `play.sh`. Without asking:

```sh
python3 install.py --target ~/Games/OpenXcom3D --ufo /path/to/UFO --tftd /path/to/TFTD --yes [--desktop]
```

`--desktop` also adds a menu entry.

#### By hand

1. Copy the release folder anywhere.
2. Link your game data into its profile:
   ```sh
   ln -s /path/to/UFO  OpenXcom3D-0.7-linux-x86_64/user/UFO
   ln -s /path/to/TFTD OpenXcom3D-0.7-linux-x86_64/user/TFTD
   ```
3. Run `./play.sh`.

### Option B: build from source

#### With the installer

```sh
python3 install.py --from-source [--workdir ~/.cache/openxcom3d-build] [--jobs 8]
```

1. It checks the build tools; if something is missing it prints the `apt install` line.
2. It clones `oxce-plus` from GitHub and checks out the commit above.
3. It applies `openxcom3d.patch` and builds with CMake (Release).
4. It installs the result like option A.

`--repo` uses another clone, e.g. a local mirror.

#### By hand

```sh
sudo apt install git cmake g++ make pkg-config libsdl1.2-dev libsdl-mixer1.2-dev \
     libsdl-image1.2-dev libsdl-gfx1.2-dev zlib1g-dev libglu1-mesa-dev

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

- Start `./play.sh`, begin a game (or **New Battle**), and in battle press **F8** or click the **3D** button at the top left.
- **Options → 3D VIEW** has the settings and the key bindings.
- [Controls](#2-controls) below.

#### TFTD

1. Open **Mods**.
2. Choose *Terror From The Deep* as the master.
3. Keep *OpenXcom3D* enabled.

#### Screen size

- The default is a 1280×800 window with the battlescape at half the screen size (640×400), which keeps 60 FPS on modest machines.
- Bigger screens work: the 3D view and its controls scale up as a whole, so they never get tiny.
- **Pixel size** in 3D VIEW trades detail for speed.

### Uninstalling

Delete the install folder (and `~/.local/share/applications/openxcom3d.desktop` if you made a menu entry). Your game data folders were only linked, never changed.

## 2. Controls

All of these can be rebound in **Options → 3D VIEW → Controls**. That page also lists (under a heading) the game's own keys that work in 3D: turning, kneel, reload, inventory, hands, personal light, night vision, next/previous soldier, end turn, frame counter, quick save/load and the ten enemy keys.

| Input | Action |
|---|---|
| **F8** | Isometric ↔ 3D (or the ISO/3D button) |
| **Mouse** | Look around; the soldier turns with you for free |
| **W A S D** | Walk and step sideways in real time. Each tile costs its normal TUs; walking into a door opens it |
| **Left button** | Fire at the crosshair in the current fire mode |
| **Wheel / F** | Previous / next fire mode (snap, aimed, auto, throw, launch, melee, psi, use, prime) |
| **Z / X / V** | Snap / aimed / auto shot |
| **G** | Switch hand (the weapon moves to that side of the screen; with Dual wield on, which gun fires first) |
| **O** | Open the door in front of you without walking through |
| **T** | Throw a tactical flare (with "Unlimited tactical flares" on) |
| **B** | Suit flight on/off (flying armor; SWIM under water) |
| **Space** | Jump: a free hop. Walking into something low (a fence, table, crate, rock: up to waist height), it vaults over it, paying only the normal walk |
| **N** | Night vision on/off, with night vision gear (Scroll Lock, the game's own toggle, works too) |
| **Page Up / Page Down** | Fly up/down (flight on) or ride a grav lift. Standing on a lift, purple arrows with LIFT PLATFORM show which ways it goes; with a free cursor (C), click them to ride |
| **1 … 0** | Turn to visible enemy 1–10 (or click the red enemy buttons) |
| **Right button** | Cancel the current action (with a free cursor: hold to look) |
| **C** or **middle button** | Mouse look ↔ free cursor (to click the buttons, hover soldiers for their cards) |
| **H** | Classic HUD on/off (for reserve-TU buttons etc.) |
| **Arrow keys** | Turn |
| **Insert** | Doom mode (debug: endless TUs, energy and ammo) |

The original hotkeys still work, except for W, A, S, D and the keys above:

- K kneel, I inventory, R reload;
- Backspace end turn;
- Q / E use left / right hand;
- L personal light, which becomes a shoulder flashlight in the dark.

## 3. The top bar and items

**Top bar**, from left to right:

- **ISO/3D** · **HUD** · **INV**;
- **◄ soldiers ►**: up to 10 shown, the window follows your soldier, and the mouse wheel over the bar scrolls. Hovering one shows a card with stats and a live helmet camera. A skull marks the dead, a caduceus the unconscious, and an orange alien face a soldier under alien mind control;
- **SPECIAL** · **NIGHT VISION** · **SUIT LIGHT** · **SUIT FLY**, shown when the soldier has them (labels shorten to SPEC · NV · LIGHT · FLY on a narrow bar);
- **END** turn.

**Items:**

- Every item works from 3D. Grenades prime and throw.
- Medi-kit, scanner and mind probe open their menus.
- The **blaster launcher**, and psi or the mind probe aimed at no one in sight, switch to the isometric map to pick waypoints or a target, then come back.

## 4. Settings

| Setting | |
|---|---|
| **Soldier camera preview** | live view in the soldier cards |
| Mouse sensitivity, invert mouse, field of view, walking speed, head bob | |
| **Falling** | walk off a ledge and fall to the level below, hurt by the height (on; off: ledges stop you as on the map) |
| **Eye height lowered by** | the 3D camera sits this many voxels below the soldier's eye (default 3), so rooms feel less cramped; the game's line of sight is unchanged |
| **Faithful X-COM targeting** | on: the game's own accuracy (hit chance by the crosshair). **Off: FPS-style shooting** — rounds fly along the crosshair with a spread (skill and kneeling tighten it; the dotted ring shows it), recoil lifts your aim with every round and eases back, and each round flies at a bullet's pace. The game's three slots become: **quick = BURST** (one burst; a gun without auto keeps its single quick shot), **AIMED** = one tight round, **AUTO** = hold the trigger to keep firing until you let go or the clip is empty. Each gun offers only the modes it has in the game ([table](#5-fire-modes-per-weapon-fps-style-shooting)). Time units and damage stay the game's |
| **Dual wield** | off by default. On: a soldier with a gun in each hand fires both at the same moment, round for round (each with its own mode, time units and ammo), each half as accurate (FPS style: twice the spread); the second gun shows on the other side, and both flash and fire together. Without the time units for both, only the chosen hand fires, at full accuracy. Two-handed weapons also take the game's one-handed penalty |
| **Flight cost per level (TU)** | what rising or sinking a level costs with SUIT FLY on (0 = free; the original charges about 8) |
| **Jump over low obstacles** | Space vaults fences, tables and crates (on by default; it changes the rules a little, so it can be switched off: then Space is only a hop) |
| **Night vision needs gear** | in 3D only soldiers with night vision gear get NIGHT VISION (see below); off: everyone, like on the map |
| Smooth lighting, shadows, flashlight, combat music | |
| **Unlimited tactical flares** | the T key |
| **Weapon in hand** | the weapon at the bottom of the screen, Doom style |
| **3D weapons (voxel)** | the weapon in hand built from little cubes, so it has volume (off: the flat picture) |
| **Sprite upscaling** | soldiers, aliens, floor items, explosions and smoke up close scaled the way pixel art scales well (xBRZ, in the game's colours): **Smooth + detail** (default) / Smooth / Off |
| **Muzzle flashes** | turn off if flashing light bothers you (photosensitivity) |
| Particles (off / effects / everything), **pixel size** (1–3: detail vs. speed) | |
| Crosshair, shot info, aim at exact point (or at the tile) | |
| Alien turn in 3D, start battles in 3D, doom mode (debug), frame rate limit (60 … 240, unlimited), frame counter | |

## 5. Fire modes per weapon (FPS-style shooting)

With **Options → 3D VIEW → Faithful X-COM targeting** off, the 3D view keeps the game's three fire-mode slots (keys Z / X / V, mouse wheel or F to cycle) but fires them FPS style. Each gun offers only the modes it has in the game; the table is generated from the game's rulesets (`items.rul`). Time units are the game's, as a percentage of the soldier's time units (a burst costs the game's auto shot).

- **quick (Z)**: a gun with auto fires one **BURST** (the game's auto: its rounds); without auto, its single **SNAP**
- **AIMED (X)**: one tight round
- **AUTO (V)**: a click fires a burst; **holding the trigger keeps firing** until you let go, the clip is empty or the time units run out
- guided launchers (Blaster Launcher, Disruptor Pulse Launcher, ...) place their waypoints on the isometric map, as before

With **Dual wield** on, the other hand's gun fires at the same moment, round for round: the same mode if it has it, otherwise its first shot, never more rounds than the chosen gun fires (a pistol joins a rifle's auto with a snap that goes with the first round).

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

## 7. What you'll see

- The map rebuilt in 3D from the game's own voxel data, with the original art and proportions.
- Doors that swing and UFO doors that slide.
- Animated machinery and light.
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

The default 1280×800 window renders the 3D view at 640×400 on all CPU cores, which holds the game's 60 FPS cap.

Bigger windows and full screen scale the whole 3D view and its controls up together, at the same cost. Use **pixel size** 2 or 3 on slow machines.

## 9. Known limits and ideas

- **The art:** soldiers and aliens are 32×40 pixel sprites seen from the side they face you, terrain is the game's 16×16×24 voxel shapes coloured from the isometric pictures, all in 256 colours. Sides the isometric art never shows are filled in (crafts and UFOs from their mirrored side). Ideas that would add resolution without changing the style: rendering the view at a higher scale, smoother voxel shading, colour sampled at the exact hit point, units given volume like the weapon in hand. Good ideas are welcome.
- **Sound:** stereo can't tell front from back; sounds behind you are only a little quieter.
- **Not saved:** bullet holes, scorch marks, flares and each soldier's flight toggle are gone after a quickload or when the screen is rebuilt.
- **Lean and peek:** not done.
- **The game's own messages** (e.g. "has become unconscious") pause the 3D sequences behind them until you click OK, as all battlescape animation does.
- **Platforms:** tested on Linux only. The Windows project files list the add-on, untested.
- **Languages:** English (US and UK) only.

## 10. For developers

### What it is based on (exactly)

| | |
|---|---|
| Upstream repository | **https://github.com/MeridianOXC/OpenXcom.git** (OpenXcom Extended by Meridian) |
| Branch | **`oxce-plus`** |
| Commit | **`f0134ef8065799c7723953d64b565f7832bfd782`** — 2026-09-29 15:04:49 +0200, author Meridian, "Merge remote-tracking branch 'SupSuper/master' into oxce-plus" |
| Version string | **OpenXcom Extended 8.7.1 (v2026-09-19)** |
| OpenXcom3D | engine add-on and mod version **0.7** (`requiredExtendedEngine: OpenXcom3D`, `requiredExtendedVersion: "0.7"`) |

`openxcom3d.patch` holds every change on top of that commit:

- the core hooks: 36 files, about 560 lines;
- the new `src/FirstPerson/` add-on;
- the `mod/OpenXcom3D/` data mod.

It applies with `git apply` to a clean checkout of the commit above.

Built with `-DOXCE_FIRSTPERSON=OFF`, it is plain OXCE again: the add-on is left out and the hooks are no-ops.

To work on it: check out the commit above, `git apply openxcom3d.patch`, build (see [Option B](#option-b-build-from-source)), and make your changes; `git diff` (plus the new files under `src/FirstPerson/` and `mod/`) gives the next patch.

### Code layout

**`src/FirstPerson/`, about 11,800 lines**

| File | What |
|---|---|
| `FirstPersonApi.{h,cpp}` | **The only header core files include.** No-op inline stubs when built with `-DOXCE_FIRSTPERSON=OFF` (`OXCE_NO_FIRSTPERSON`). Also registers the add-on's options |
| `FirstPersonView.{h,cpp}` | Renderer, 8 threads (details below) |
| `FirstPersonParticles.{h,cpp}` | The particle pool (sparks, smoke, weather...) |
| `FirstPersonSounds.{h,cpp}` | Sounds made in code (no files): hitmarker and kill ticks, heartbeat, dry-fire click, flare hiss |
| `FirstPersonMode.{h,cpp}` | Controller: full-screen view, top bar, info panel, soldier cards, crosshair and fire modes, mouse look vs cursor, classic HUD, free facing, movement and doors, firing, alien turn, spotting, hurt/death feedback (killed, zombie, unconscious, mind control), breakdowns (panic, berserk), on fire, doom mode |
| `OptionsFirstPersonState.{h,cpp}` | 3D VIEW options tab: Settings / Controls sub-tabs, scrolling lists |
| `RuleFirstPerson.h` | `firstPerson:` ruleset section (header-only): enabled, view distance, window glass, weather |

What `FirstPersonView` draws:
- terrain from LOFT voxels with iso-sprite projection, animated frames, UFO doors cut out as they slide, doors swinging, window glass;
- smooth light plus sun face shading, Bayer dithering and horizon haze; colour washes (hurt, dying, unconscious, mind control, breakdown, zombie); night vision recolouring; the fire over the view of a burning soldier;
- the outside world: ground, X-COM tape and posts; the sky;
- billboards for units (gliding, faded right in front of the camera), floor items, projectiles and explosions;
- particles; red numbered markers over the visible enemies.

**Core changes** (see `openxcom3d.patch` for every line), placed where upstream rarely edits:

| File | Change |
|---|---|
| `CMakeLists.txt` (top) | `option(OXCE_FIRSTPERSON ... ON)` |
| `src/CMakeLists.txt` | Adds the `firstperson_src` list, or defines `OXCE_NO_FIRSTPERSON`; links Threads |
| `BattlescapeState.{h,cpp}` | `FirstPerson::create/destroy/think/handle/blit/wantsMouseCapture/resized`; `friend class FirstPersonMode` |
| `BattlescapeGame.h` | `BattleAction::targetVoxel` (exact aim point), `instantMove`, `freeAim` (FPS-style shot), `dualWield`/`dualWeapon`/`dualType` (dual wield) |
| `Projectile.h` | `getAmmo()` getter (trail style from the ammo's damage type) |
| `ProjectileFlyBState.{h,cpp}` | Uses `targetVoxel` when set (accuracy still applies); with `freeAim` asks the first-person view for each round's target; dual wield: the other gun's round fired, flown and landed with each round (like a shotgun's extra pellets), both half as accurate |
| `BattlescapeGame.cpp` | A soldier panicking or going berserk: in 3D the first-person view shows it (`FirstPerson::breakdown`, which holds the battle as long as the box would) instead of the "has panicked" box |
| `ExplosionBState.cpp` | FPS-style shooting: our own bullet hits play quicker (`FirstPerson::fastShots`) |
| `Projectile.cpp` | With `freeAim`: no accuracy roll, no line-of-fire refusal, the line carried on through the target, a bullet's speed |
| `UnitWalkBState.cpp` | `instantMove` walks take the no-animation path |
| `Map.{h,cpp}` | `getNightVisionColor()`, `isNightVisionOn()` getters; `setSoundListener()`: in 3D, `getSoundAngle` pans sounds for the 3D camera; `setNightVisionHoldBlocked()`: in 3D the hold key (Space) jumps; `getPartnerProjectile()`: the dual-wield round in flight |
| `Sound.{h,cpp}` | `packDistance()`: in 3D the listener packs the distance into the stereo angle, so sounds fade with distance without touching the call sites |
| `Pathfinding.{h,cpp}` | `setPath(unit, path)`: a step set by hand (the step off a ledge). `setFlightCost(unit, tu)`: the soldier flying in 3D with SUIT FLY on pays the "flight cost per level" setting for up/down (everyone else: the rules). `setVault(unit, parts)`: for the step a vault takes, those low tile parts don't block that one unit and cost nothing (`isBlocked`, the wall and object costs) |
| `main.cpp` | Window caption " - OpenXcom3D" (add-on builds) |
| `InteractiveSurface.cpp` | Upstream UB fixes found by UBSan: button 0 ignored (no shift by −1), `SDLK_ANY` inside the enum |
| `Script.h` | Upstream UB fix found by UBSan: `ScriptRef::compare` doesn't `memcmp` empty (null) refs |
| `State.h`, `Game.{h,cpp}` | Generic hook: the top state may ask for mouse capture (relative mouse look); released with the cursor centred for every other state |
| `UnitSprite.{h,cpp}` | `setViewRotation()`: draw a unit from any side |
| `Options.cpp` / `Options.inc.h` | `createOptionsOTHER()` → `FirstPerson::createOptions(_info)`; declarations at the end of the file |
| `OptionsBaseState.{h,cpp}` | 3D VIEW tab via `FirstPerson::hasOptionsTab/createOptionsTab` |
| `Mod.{h,cpp}` | Parses `firstPerson:` at the end of `Mod::loadFile` |
| `ModInfo.cpp` | Engine row `{"OpenXcom3D", 0.7}` (add-on builds only) |
| `CrossPlatform.cpp` | Add-on builds use their own user/config folders (`~/.local/share/openxcom3d`, `~/.config/openxcom3d`, `Documents/OpenXcom3D`, …). Data folders are shared with stock OXCE |
| `OpenXcom.2010.vcxproj{,.filters}` | The add-on's files |

## 11. Version history

### 0.7 (2026-10-18)

- **Breakdowns in 3D**: a soldier who panics or goes berserk at the start of a turn is seen through their own eyes instead of the game's box. Panic: the view fades to white, YOU HAVE A MENTAL BREAKDOWN, a racing heart, while they freeze, or drop their weapons and run. Berserk: BERSERK, white and red throbbing with each heartbeat, every wild shot flashing red. Then they come to. Their camera card and the panel say PANIC or BERSERK for the rest of the turn. (On the isometric map the game's own box, as before.)
- **Turned into a zombie**: a soldier killed by a Chryssalid (UFO) or a Tentaculat (TFTD) gets their own end in 3D: TURNED INTO A ZOMBIE, down in red, then up again, lurching in a sickly green, until the helmet camera gives out. A green skull on their squad button; their camera says ZOMBIE.
- **On fire**: the soldier's view in flames, rising from the bottom and the sides (heat rising cell by cell, as in the old fire effect of Doom), with embers and the light of it; catching fire makes them leap, with a BURNING banner. Their panel and camera card say BURNING. Burning soldiers and aliens in view burn in 3D too.
- Fixed: a soldier left behind in an earlier stage of a two-stage mission showed in the squad bar as unconscious; they have no button now.
- Fixed: a soldier killed by an overkill showed a health of 4294... on their card.

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
