# OpenXcom3D

*A humble attempt at a wish from young days: to stand on the battlescape of* UFO: Enemy Unknown *and look around through a soldier's eyes.*

![Night, through a soldier's eyes](docs/screenshots/night.png)

*UFO: Enemy Unknown* (X-COM: UFO Defense, 1994) is perhaps the greatest game of all time. OpenXcom3D adds a first-person 3D battlescape to **OpenXcom Extended**: you can switch between the isometric map and your soldier's view at any moment, and walk, fight and die in it. This is version **1.04**, for Linux and Windows.

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

## New in 1.04

Two more reports from the same player (thank you!) on 1.03, a long one and a short one:

- **Doors:** **O** opens the door you look at (turning to it first), and tile moves stop in front of a closed door instead of walking on through it.
- **Bullet holes** on everything, near and far, drawn in 3D: a far one a dot, a near one with a darker core, on slopes and objects as on walls.
- **At the map's pace:** chains of explosions and fast bursts (the X-Com Files' turbo laser) no longer crawl in 3D on slower computers.
- **Night vision** switches at any time, and under it the aimed path glares white-green, like a laser sight through goggles.
- **Underground maps in rock:** bases, colonies and caves sit in their own rock, with a rock ceiling instead of the sky; unexplored terrain turns misty with distance instead of a phantom skyline.
- **The map camera:** the selected soldier's helmet camera in a window on the isometric map (setting *Map camera*; **P** shows or hides it, a click goes to 3D, a right click opens the soldier cam).
- **No more "No line of fire" with the shot plainly on the unit:** the game's own aim and OXCE's off-centre shooting are tried before refusing, a unit's head poking into the tile above can be hit, and a shot the game will refuse shows its path dimmed. **Force arcing shots** fires a Panzerfaust at a building's side, and **Force blocked shots** (off) fires anyway when nothing reaches.
- **Akimbo:** two guns together cost the dearer gun's shot and a tenth (not twice) at a 30% penalty instead of half, or fire one a moment after the other at full accuracy (*Akimbo in turn*, with a key of its own); arcing guns fire in turn, two guns take no aimed shot, and a shotgun in either hand fires its pellets.
- **Falls, climbing and flying** by the walking speed, falls speeding up as gravity would.
- **Show facing:** hold **Left Alt** for a ring and an arrow round every unit in sight, the way it faces.
- **Shotgun pellets in flight** (setting, off): each pellet flies beside the round to where the game's spread sends it.
- **A purple haze** through the eyes of a unit under your mind control.
- **Soldiers spawn with guns:** OXCE's own auto-equip handed out staffs and pitchforks first in the big mods, and other weapons each time the mod list changed. Now guns come first, the same every time.
- **And more:** a big unit's corpse in one piece, items lying up slopes instead of in them, black only where it's really dark, no grass in the Abductor's corners, ghost lift pads see-through up close, no moving or turning mid-shot with tile moves or paid turns.
- **macOS:** a first build, untested on a real Mac, from the automatic builds (see the manual).

## New in 1.03

Another report from a player (thank you!), on 1.02, and a first look at **XPiratez**:

- **Windows:** a first build for 64-bit Windows 10 and 11, a zip to unpack, with a `play.bat` that finds your Steam games by itself, as on Linux. It is made on Linux and tested under Wine only, so word on how it runs on a real Windows machine would be very welcome.
- **The aliens' turn on the map:** with *Alien turn in 3D* off, the *Aliens' turn cameras* setting used to do nothing. Now the helmet camera cards pop up over the isometric map: a soldier shot at, hit, mind controlled or going down, a soldier firing back, and the one who sees the alien moving. Watching a death from a teammate's helmet is quite something.
- **Corpses through floors:** a body lying on the floor above no longer hangs down through the ceiling, and one on a shelf right under a floor no longer pokes up into the room above.
- **Sprint:** **Caps Lock** switches it on and off. Your soldier runs by the game's own running rules (fewer time units, more energy), at a faster pace; **SPRINT** shows under the compass.
- **En passant:** the **`** key. Enemies coming into sight no longer stop the walk, as Shift does on the map; **EN PASSANT** shows under the compass. (The name is the player's "French move".)
- **Ladders and lifts:** **W** and **S** climb a ladder or ride a lift by where you look, a soldier who can fly no longer floats off the top of a ladder with flight off, and ladders and lift pads that have no shape in the game's data show as see-through ghosts (setting *Parts without shape*).
- **Helmet cameras** now and then show a bad digital signal: a few blocks, a green square, a short freeze. On the hardest missions (Mars, an alien base, a base defence, and in TFTD the colonies, T'leth and the artefact sites) and inside any UFO they lose it again and again, and the picture tears. Elsewhere, never much.
- **Priming in one click:** PRIME opens the timer at once, and a psi-amp or a mind probe aims straight away.
- **Glow:** white next to a glowing outline no longer turns black, and hitbox outlines glow only with *Glowing outlines*.
- **Straight up and down:** you can look all the way up and down, and an alien right overhead shows and can be aimed at.
- **Doors** the open key turns back no longer fold into their wall, a failed close-quarters check turns your view to where the shot went (no turning back at a cost), and UFOs keep their floor under their curved hull (no more grass inside).
- **The squad on the wheel:** with the cursor free (**C**), the mouse wheel over the squad bar steps the soldier card, or the soldier cam, from one soldier to the next.
- **Two small ones from the report:** the trajectory shown for a shot now moves with a lean, as the shot does, and with *Unit size* under 100% corpses and items shrink too, so a soldier going down no longer swells into a bigger corpse.
- **XPiratez:** swords strike edge first and are flat blades, every weapon's picture is measured whole, and arrows, bolts and javelins fly as white streaks with no muzzle flash. An **item table** in our mod's rules sets an item's look by hand wherever the automatic rules get it wrong.

## New in 1.02

- **Gunplay, gone over gun by gun:** every weapon fires its own round now, bigger for a bigger gun (pistol, rifle, auto-cannon, heavy cannon; the lasers, the plasma guns and the TFTD guns the same), with a tracer streak, a muzzle flash and a kick to match. Lasers are red beams again, and every shot leaves the muzzle of the hand that holds the gun.
- **Rockets** leave a trail of smoke, and white specks that hang in the air for half a minute (torpedoes too).
- **Weapons in hand** all lie barrel first along the line to the crosshair, as the rifles did (the rifle itself had been held the wrong way round, sorry!).
- **Smoke** comes in two kinds now, Original and Modern. Original, the game's own smoke animation, closes up where your soldier can't see through a cloud, so a smoke grenade really hides what's behind it.
- **Fixes:** the *Particles* setting no longer takes the shots away with it (it now reads Off, Effects or All), and in Doom mode a grenade thrown from its menu can't be thrown a second time.

## New in 1.01

- **Light fixes:** the flashlight has its own key now, **F**, apart from the suit light (L), and one setting, *Flashlight*: Off, On (a wide beam in the dark) or Dynamic (a spot on the crosshair, day or night). Next fire mode moved to **J** (and the mouse wheel).
- **Soldier cam:** a right click on a soldier's button opens their helmet camera big, to look through anyone in the squad without switching to them.
- **Every key and setting gone over once more:** a key that can't act now says why, *Muzzle flashes* off stops every flash, and a dozen small fixes more.

## New in 1.0

- **Two more play-test reports** from the same veteran player (thank you again!), turned into another twenty-six small patches, each tested on its own. A few of them:
  - **smoke you can see through,** made from the game's own smoke (setting *Smoke*: Modern, Original or Solid), solid only where your soldier's sight runs out;
  - **doors** that never fold into their walls, **bullet holes** on every wall, **UFO walls** without black stripes, stairs 0.9 couldn't climb;
  - **melee weapons** held tip up and swung, **fists** and built-in weapons on G, **medi-kits and scanners** straight from the crosshair, a light switched off again;
  - **icons over downed units,** the **name of the unit** under the crosshair, a middle click that identifies it.
- **Zoom:** hold **U** (or the right button) and the view glides in to four times closer: SCOPING, with a thin scope cross.
- **The path of a throw,** drawn as you aim (setting *Show trajectory*), and **Confirm fire** for those who like to click twice.
- **The aliens' turn, watched:** the view cuts to whoever sees the alien moving and to anyone firing back, and a soldier in trouble pops up their helmet camera.
- **A tactical flashlight,** if you like: a light on the weapon, a bright spot right on the crosshair, as in Half-Life, lighting dark places by day too.
- **The compass** now tells you when a shot will cost time units to turn first.
- **Faster:** the same picture, drawn 20–32% faster at 1280×800, on every core of the machine.
- **Lean** moved to **Shift + A / D**, so Q and E are the hand keys again.

## New in 0.9

- **A play-test round:** a veteran player sent two long reports (thank you!), and they became some thirty small patches, each tested on its own: walking off ledges and through diagonal gaps, no more sky shimmering through buildings, soldiers and corpses seen from above, items lying on the floor, the blinking units of the big mods, close-quarters combat as the game rolls it, light no longer glowing through units, and more.
- **New settings,** most of them off, so the game plays as before until you switch them on: turning costs time units, a compass, moving a tile at a time (the numpad always does), Doom IDKFA and IDDQD, realistic akimbo, hitboxes, unit size, see-through smoke, a discreet map edge, distance haze, lean shots.
- **Lean out:** with the setting *Lean* on, hold **Q** or **E** to peek round a corner; with *Tilt* the view tilts as a head does, and a shot from there leaves from that side.
- **The classic HUD in 3D:** its buttons now act in the 3D view (centre, look a level up or down, the minimap on your soldier, the enemy buttons), and **P** hides the 3D HUD for a clear view.
- **Steam Deck:** the lean on the back buttons; akimbo, snap turns, the 3D HUD, the light and the flare on long presses.

## Help wanted

If someone in the ingenious OpenXcom community has the will to improve this further, help is very welcome: ideas for the art, translations, play-testing, and making the code better. Issues and pull requests are open.

**Mac players** especially, and **reports from Windows:** GitHub's machines now build a macOS version on every change, but none of us has a Mac, so it has only been started there, never played. If you have one, trying it (see [Getting it](#getting-it)) and saying how it went would help a lot. The Windows zip is made on Linux (with llvm-mingw) and played under Wine; GitHub's Windows checks that it starts and finds the games in Steam, but nobody has played a battle on a real Windows machine yet.

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
- small hooks in the core (40 files, about 900 lines): see the patch;
- [`mod/OpenXcom3D/`](mod/OpenXcom3D): the data mod that switches it on.

`src/` and `mod/` in this repository are the same files as in the patch, for reading. Built with `-DOXCE_FIRSTPERSON=OFF`, it is plain OXCE again.

## Getting it

You need your own copy of the original game data (*UFO: Enemy Unknown / X-COM: UFO Defense* and/or *X-COM: Terror From The Deep*, e.g. from Steam or GOG). Nothing of it is included here.

- **Linux (x86-64): download and play.** From the [Releases](../../releases) page take either:
  - the **AppImage**: one file. Make it executable (`chmod +x`, or *Properties → Permissions*) and run it;
  - or the **archive**: unpack it anywhere and run `./play.sh`.

  Nothing needs installing: the game comes with its own libraries and runs on any Linux from 2021 on (glibc 2.34: Ubuntu 22.04 and newer, Debian 12, Fedora, Arch, the Steam Deck). The first time, it finds *X-COM: UFO Defense* and *Terror From The Deep* in your Steam library by itself. Otherwise put the game's folder next to it, named `UFO` (or `TFTD`); its data is only linked, never copied or changed. `python3 install.py` can also install it, with a menu entry.
- **Steam Deck:** the same download, in Desktop Mode. Add `play.sh` (or the AppImage) to Steam as a non-Steam game to play in Game Mode, then run `steamdeck/add_controller_layout.sh` once: your OpenXcom3D shortcut then uses the controller layout made for it. The manual has the [steps](OpenXcom3D_manual.md#steam-deck).
- **Windows (64-bit, Windows 10 and 11): download and play.** From the [Releases](../../releases) page take the **zip**, unpack it somewhere of yours (Documents, the Desktop, `C:\Games`; not Program Files) and double-click `play.bat`. Nothing needs installing. The first time, it finds the games in your Steam library by itself; otherwise put the game's folder next to it, named `UFO` (or `TFTD`). It only links the data, never changes it. The game isn't signed, so Windows may warn the first time: *More info*, then *Run anyway*. It is played under Wine here, and checked on a real Windows by the automatic builds, so reports from players are welcome.
- **macOS (Apple silicon and Intel, macOS 11 and newer): a first, untested build.** The automatic builds make it on GitHub's Macs: open the latest run of [Build](../../actions/workflows/build.yml?query=branch%3Amain) and, under *Artifacts*, take **macos-release** (a GitHub account is needed; each run keeps it for 30 days). Inside is the zip: unpack it, put the game's folder next to `OpenXcom3D.app`, named `UFO` (or `TFTD`), and open the app. It isn't signed by Apple, so macOS refuses it the first time: *System Settings → Privacy & Security → Open Anyway* (the `README-macOS.txt` next to it says more). Nobody here has a Mac, so please tell us how it went.
- **From source:** `python3 install.py --from-source` clones OXCE at the commit above, applies the patch, builds and installs. Every step by hand, for Linux and for Windows (MinGW), is in the **[build guide](https://b0realis.github.io/OpenXcom3D/)**.

Start a game (or a **New Battle**) and in battle press **F8** to go into 3D. Mouse to look, **WASD** to walk, left button to fire, **F8** back to the map.

Everything else — installing by hand, every key, every setting, fire modes per weapon, night vision gear, how the code is laid out, the version history — is in the **[OpenXcom3D manual](OpenXcom3D_manual.md)**.

## Thanks

To the open-source community and the GNU project, whose tools and licence make work like this possible, and to the **OpenXcom** and **OpenXcom Extended** developers — SupSuper, Meridian and everyone who contributed — for doing an amazing job keeping this game alive. OpenXcom3D is only a small addition on top of their work.

To **John Carmack** and **id Software**, for spearheading 3D games. Wolfenstein 3D, Doom and Quake showed the world what a ray cast through a grid and a fast loop could do, and every first-person game since stands on their shoulders, this one included: its 3D view is a ray caster at heart. The cheats in the 3D view, **Doom mode**, **IDKFA** and **IDDQD**, are a small tribute to you. Thank you, guys!

The original games and their data belong to their owners. OpenXcom, OpenXcom Extended and OpenXcom3D are free software under the [GNU GPL v3](LICENSE).

I wish you all health, and enjoy playing.
