# OpenXcom3D

*A humble attempt at a wish from young days: to stand on the battlescape of* UFO: Enemy Unknown *and look around through a soldier's eyes.*

![Night, through a soldier's eyes](docs/screenshots/night.png)

*UFO: Enemy Unknown* (X-COM: UFO Defense, 1994) is perhaps the greatest game of all time. OpenXcom3D adds a first-person 3D battlescape to **OpenXcom Extended**: you can switch between the isometric map and your soldier's view at any moment, and walk, fight and die in it. This is version **0.8.2**.

**Both games are supported:** *UFO: Enemy Unknown* and *X-COM: Terror From The Deep*.

## The goal

The research and testing goal of this project was to bring the battlescape into 3D while staying **faithful to the original game**: seamless, so that it feels as if it had been introduced the day *UFO: Enemy Unknown* was released. It uses **only the original assets** — the game's own maps, voxel shapes, sprites, palette and sounds (the few new sounds, such as the hit marker's tick, are made in code) — and no modern art spins that would split the game in two. Every action still goes through the original rules: time units, energy, accuracy, reaction fire, line of sight, doors, morale. The few deliberate additions (jumping over low obstacles, free flight between levels, night vision as gear, falling off ledges, FPS-style shooting, dual wield) are settings. A real-time mode will be attempted in the future, as a setting too.

The art, honestly, does not lend itself to full 3D: soldiers and aliens are 32×40 pixel sprites, terrain is 16×16×24 voxels, and there are 256 colours. Good ideas for that are welcome (a few are listed in the manual, under [Known limits and ideas](OpenXcom3D_manual.md#9-known-limits-and-ideas)). Nevertheless the gameplay turned out to be quite engaging and deep: a dark UFO corridor, a door that slides open, a shape you can't make out yet, and your squad's time units ticking away.

**But be careful!** It can be a traumatic experience.

| | |
|---|---|
| ![A city street at dusk, a terror site](docs/screenshots/city_dusk.png) | ![A heavy plasma round leaving the gun](docs/screenshots/plasma.png) |
| ![The Skyranger](docs/screenshots/skyranger.png) | ![Terror From The Deep: on the sea floor](docs/screenshots/tftd.png) |
| ![Dual wield](docs/screenshots/akimbo.png) | ![Signal lost](docs/screenshots/signal_lost.png) |
| ![Terror From The Deep, an island attack: a Gillman ahead, a Deep One in a teammate's helmet camera](docs/screenshots/tftd_island.png) | ![A rocket going off far down an alien base hall](docs/screenshots/alien_base.png) |

## New in 0.8.2

- **Download and play:** the Linux release now comes with its own libraries, so nothing has to be installed. Unpack it and run `./play.sh`, or take the single-file **AppImage**. It runs on Ubuntu 22.04 and newer, Debian 12, Fedora, Arch and the **Steam Deck**; all it needs is your copy of the original game.
- **Finds your games by itself:** *X-COM: UFO Defense* and *Terror From The Deep* in any Steam library (Steam installed as a Flatpak or a Snap too, and the Steam Deck's SD card), or a `UFO` / `TFTD` folder next to the game.
- **Steam Deck:** fullscreen on its screen, and a controller layout made for the game: the left stick walks, the right trackpad looks, L1 switches mouse look and the free cursor, R1 the isometric map and 3D.

## New in 0.8.1

- **The prebuilt release runs on old and new Linux systems:** anything from 2021 on (glibc 2.34: Ubuntu 22.04, Debian 12, Fedora 35, and every rolling one such as Arch), with the system's own SDL libraries in whatever version it has. If something is missing, `play.sh` says what and how to install it.
- **Pixel size Auto** (the new default) keeps big screens smooth: on 1920×1080 it holds 60 FPS.
- **Everything in a hand is 3D:** grenades, medi-kits, scanners, clips are built from their inventory pictures like the weapons, half in view at the bottom of the screen; **thrown items fly as 3D objects**, tumbling along the game's arc.
- **Grenades:** a primed one shows its fuse on the panel. In Doom mode they work in real time (one click pulls the pin, three seconds, boom, in your hand too if you wait): a first step towards a **real-time mode**, which will be attempted in the future.

## Help wanted

If someone in the ingenious OpenXcom community has the will to improve this further, help is very welcome: ideas for the art, translations, play-testing, and making the code better. Issues and pull requests are open.

**Windows and macOS builds** especially: only Linux is built and tested here. The patch applies to OXCE's own source, which builds on Windows with Visual Studio (`src/OpenXcom.2010.sln` already lists the add-on) and on macOS as usual. A build shared by someone who has the machine for it would let many more people play.

## What it is based on

OpenXcom3D stems from **OpenXcom Extended** at exactly this commit:

| | |
|---|---|
| Repository | **https://github.com/MeridianOXC/OpenXcom** |
| Branch | **`oxce-plus`** |
| Commit | **`f0134ef8065799c7723953d64b565f7832bfd782`** (2026-09-29 15:04:49 +0200, Meridian: "Merge remote-tracking branch 'SupSuper/master' into oxce-plus") |
| Version | **OpenXcom Extended 8.7.1 (v2026-09-19)** |

All changes on top of it are in one file, [`openxcom3d.patch`](openxcom3d.patch) (it applies with `git apply` to a clean checkout of that commit):

- [`src/FirstPerson/`](src/FirstPerson): the 3D view, an engine add-on (renderer, controller, sounds, options), one file per topic and commented throughout;
- small hooks in the core (36 files, about 570 lines): see the patch;
- [`mod/OpenXcom3D/`](mod/OpenXcom3D): the data mod that switches it on.

`src/` and `mod/` in this repository are the same files as in the patch, for reading. Built with `-DOXCE_FIRSTPERSON=OFF`, it is plain OXCE again.

## Getting it

You need your own copy of the original game data (*UFO: Enemy Unknown / X-COM: UFO Defense* and/or *X-COM: Terror From The Deep*, e.g. from Steam or GOG). Nothing of it is included here.

- **Linux (x86-64): download and play.** From the [Releases](../../releases) page take either:
  - the **AppImage**: one file. Make it executable (`chmod +x`, or *Properties → Permissions*) and run it;
  - or the **archive**: unpack it anywhere and run `./play.sh`.

  Nothing needs installing: the game comes with its own libraries and runs on any Linux from 2021 on (glibc 2.34: Ubuntu 22.04 and newer, Debian 12, Fedora, Arch, the Steam Deck). The first time, it finds *X-COM: UFO Defense* and *Terror From The Deep* in your Steam library by itself. Otherwise put the game's folder next to it, named `UFO` (or `TFTD`); its data is only linked, never copied or changed. `python3 install.py` can also install it, with a menu entry.
- **Steam Deck:** the same download, in Desktop Mode. Add `play.sh` (or the AppImage) to Steam as a non-Steam game to play in Game Mode, then run `steamdeck/add_controller_layout.sh` once: your OpenXcom3D shortcut then uses the controller layout made for it. The manual has the [steps](OpenXcom3D_manual.md#steam-deck).
- **Windows and macOS:** no builds from us yet; see [Help wanted](#help-wanted).
- **From source:** `python3 install.py --from-source` clones OXCE at the commit above, applies the patch, builds and installs.

Start a game (or a **New Battle**) and in battle press **F8** to go into 3D. Mouse to look, **WASD** to walk, left button to fire, **F8** back to the map.

Everything else — installing by hand, every key, every setting, fire modes per weapon, night vision gear, how the code is laid out, the version history — is in the **[OpenXcom3D manual](OpenXcom3D_manual.md)**.

## Thanks

To the open-source community and the GNU project, whose tools and licence make work like this possible, and to the **OpenXcom** and **OpenXcom Extended** developers — SupSuper, Meridian and everyone who contributed — for doing an amazing job keeping this game alive. OpenXcom3D is only a small addition on top of their work.

The original games and their data belong to their owners. OpenXcom, OpenXcom Extended and OpenXcom3D are free software under the [GNU GPL v3](LICENSE).

I wish you all health, and enjoy playing.
