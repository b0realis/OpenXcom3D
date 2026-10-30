"""Writes the player's guide into pages/play.html on GitHub Pages (https://b0realis.github.io/OpenXcom3D/play.html), after
its hand-written steps to get the game running: every key, every setting with what it does and how it changes the play,
the 3D view's icons and messages, and every feature with its pictures. The words live here as data (KEYS, SETTINGS,
SCREEN, FEATURES), so a new key or setting is one entry. Only what lies between the page's guide markers is rewritten:
its styles (/* guide:css:begin */ ... /* guide:css:end */), its sections (<!-- guide:begin --> ... <!-- guide:end -->)
and its search (<!-- guide:script:begin --> ... <!-- guide:script:end -->); the steps around them are the page's own.
The page asks no other site for anything. Its pictures are in pages/img/guide/ (1280x800, from UFO and TFTD; XPiratez
only for its own features).
usage: python3 tools/make_guide.py   (updates pages/play.html)"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = 'img/guide/'
MANUAL = 'https://github.com/b0realis/OpenXcom3D/blob/main/OpenXcom3D_manual.md'

# ---------------------------------------------------------------------------------------------------------------------
# Keys: (group, [(keys, what it does, how it plays)]). The defaults; all can be rebound in Options > 3D VIEW > Controls
# (the 3D view's own and the game's night vision and frame counter keys; the game's others in its own Controls).
# ---------------------------------------------------------------------------------------------------------------------
KEYS = [
    ('Getting around', [
        ('<kbd>F8</kbd>', 'Switch between the isometric map and 3D (or the ISO/3D button). Back on the map it\'s centred on your soldier.',
         'Switch whenever you like, even mid-turn: the battle is the same one.'),
        ('<kbd>W</kbd> <kbd>A</kbd> <kbd>S</kbd> <kbd>D</kbd>', 'Walk and step sideways in real time. Each tile costs its normal time units; walking into a door opens it. On a lift or a ladder, or flying, W while looking up climbs and looking down goes down.',
         'Free movement can stand you anywhere in a tile, which matters for peeking and lean shots.'),
        ('<kbd>Numpad 8 2 4 6</kbd>', 'One tile ahead, back, left or right, almost at once, as a step on the map; two together make a diagonal. <kbd>7</kbd> <kbd>9</kbd> snap-turn 45&deg;.',
         'Tile-exact moves when you count time units.'),
        ('<kbd>Caps Lock</kbd>', 'Sprint on/off: every step is the game\'s run (Ctrl on the map). SPRINT shows under the compass.',
         'Three quarters of a walk\'s time units, half as much energy again: cross open ground fast.'),
        ('<kbd>`</kbd>', 'En passant on/off: an enemy coming into sight no longer stops the walk (as Shift on the map). ENEMY SPOTTED still shows.',
         'Lets you finish a dash, at your own risk.'),
        ('<kbd>Space</kbd>', 'Jump: a free hop; walking into something low (a fence, a crate, up to waist height) vaults over it.',
         'A small bend of the rules (setting <i>Jump over low obstacles</i>).'),
        ('<kbd>Page Up</kbd> <kbd>Page Down</kbd>', 'Fly up/down, ride a grav lift, climb a ladder. Off a lift, Page Up switches flight on, Page Down on the ground off. Purple arrows with LIFT PLATFORM show where a lift goes.',
         ''),
        ('<kbd>B</kbd>', 'Suit flight on/off (flying armor; SWIM under water).', ''),
        ('<kbd>O</kbd>', 'Open the door you look at without walking through it, if it\'s in reach.', 'Look before you step into a room.'),
    ]),
    ('Looking', [
        ('Mouse', 'Look around, all but straight up and down; the soldier turns with you for free (or by the rules: <i>Turning costs TU</i>).', ''),
        ('<kbd>&larr;</kbd> <kbd>&rarr;</kbd> <kbd>&uarr;</kbd> <kbd>&darr;</kbd>', 'Turn left / right, look up / down.', ''),
        ('<kbd>Home</kbd>', 'Look straight ahead, the way the soldier faces (the map\'s centre key).', ''),
        ('<kbd>,</kbd> <kbd>.</kbd>', 'Snap turn 45&deg; left / right.', ''),
        ('<kbd>1</kbd> &hellip; <kbd>0</kbd>', 'Turn to visible enemy 1 to 10 (or click the red enemy buttons).', 'Find every alien in sight in one key.'),
        ('<kbd>Left Shift</kbd> + <kbd>A</kbd> / <kbd>D</kbd>, held', 'Lean out left / right to peek round a corner. Off until the setting <i>Lean</i> is Peek or Tilt; <i>Lean left</i> and <i>Lean right</i> can also get keys of their own. The eye moves up to six voxels; a shot from a lean leaves from that side.',
         'You see the street round the corner; who is seen is still the game\'s call.'),
        ('<kbd>U</kbd> or the right button, held', 'Zoom: the view glides in four times closer (setting <i>Zoom</i>). SCOPING shows how far.', 'Only the picture changes: aim and hit chance stay the same.'),
        ('<kbd>Left Alt</kbd>, held', 'Show facing: a ring round every unit in sight with an arrow the way it faces, in its side\'s colour.', 'Where an alien looks decides whether it reacts to you.'),
        ('<kbd>N</kbd> or <kbd>Scroll Lock</kbd>', 'Night vision on/off, with night vision gear (Scroll Lock: the game\'s own key for it).', ''),
        ('<kbd>F</kbd>', 'The flashlight on/off (setting <i>Flashlight</i>).', 'Only the 3D view is lit: aliens don\'t see you any better for it.'),
    ]),
    ('Fighting', [
        ('Left button', 'Fire at the crosshair in the current fire mode.', ''),
        ('<kbd>Wheel</kbd> / <kbd>J</kbd>', 'Previous / next fire mode: snap, aimed, auto, launch, melee, psi, use, throw, prime.', ''),
        ('<kbd>Z</kbd> <kbd>X</kbd> <kbd>V</kbd>', 'Snap / aimed / auto shot.', 'In FPS style: snap = one round from the hip, aimed = one tight round, auto = a burst, hold the trigger for more.'),
        ('<kbd>G</kbd>', 'Switch hand: right, left, and the built-in weapon, each that gives something to use.', ''),
        ('<kbd>Y</kbd>', 'Akimbo off, and back on to the kind the setting <i>Akimbo</i> says (Full or One-hand); with the setting Off it does nothing.', 'Two guns: more rounds, each 30% less accurate. Off to take an aimed shot.'),
        ('<i>Akimbo style</i> (unbound)', 'Switch two guns between taking turns round by round and firing at once (<i>Akimbo volley</i>).', ''),
        ('<kbd>Ctrl</kbd>+<kbd>Shift</kbd> + click', 'With a spray weapon\'s auto shot (X-Com Files\' miniguns): mark the spray burst\'s points, as on the map.', ''),
        ('Right button', 'Cancel the current action, or forget a target marked for <i>Confirm fire</i>; with nothing to cancel, hold to zoom (with a free cursor: hold and drag to look round).', ''),
        ('<kbd>T</kbd>', 'Throw a tactical flare (setting <i>Unlimited tactical flares</i>).', 'Lights the 3D view only.'),
        ('<kbd>Insert</kbd>', 'The Doom cheats off, and the same ones back on.', ''),
    ]),
    ('The screen', [
        ('<kbd>C</kbd> or the middle button', 'Mouse look &harr; free cursor, to click buttons and hover soldiers for their cards. With <i>Middle click identifies</i> a middle click on a unit shows it.', ''),
        ('<kbd>H</kbd>', 'The classic HUD on/off: every button of it works in 3D.', 'The map\'s whole bottom bar, if you prefer it.'),
        ('<kbd>Wheel</kbd> over a soldier\'s button', 'With the cursor free: steps the soldier\'s card, or the SOLDIER CAM, through the squad one soldier at a time (over &#9664; and &#9654; it scrolls the bar).', 'Look through every soldier\'s eyes in a few turns of the wheel.'),
        ('<kbd>P</kbd>', 'The 3D HUD hidden; again, the crosshair too (a clean view); again, all back. On the map: the map camera on/off.', ''),
        ('<kbd>F7</kbd>', 'The frame counter on/off (the game\'s key).', ''),
    ]),
    ('The game\'s own keys, in 3D too', [
        ('<kbd>K</kbd>', 'Kneel / stand: the view drops to kneeling height.', 'Kneeling steadies the aim, in either style.'),
        ('<kbd>I</kbd>', 'Inventory.', ''),
        ('<kbd>R</kbd>', 'Reload.', ''),
        ('<kbd>Q</kbd> <kbd>E</kbd>', 'The left / right hand\'s action menu.', ''),
        ('<kbd>L</kbd>', 'The suit light: the game\'s personal light for the whole squad.', 'Unlike the flashlight it lights your tiles by the game\'s rules: aliens see you more easily at night.'),
        ('<kbd>Tab</kbd>', 'Next soldier; the 3D view looks through them. <i>Previous soldier</i> has no key until you give it one: click &#9664; in the top bar.', ''),
        ('<kbd>M</kbd>', 'The minimap, on your soldier.', ''),
        ('<kbd>Backspace</kbd>', 'End turn.', ''),
        ('<kbd>F5</kbd> / <kbd>F9</kbd>', 'Quick save / load.', ''),
    ]),
]

# ---------------------------------------------------------------------------------------------------------------------
# Settings: (group, [(name, default, choices, what it does, how it plays)]), in the order Options > 3D VIEW > Settings lists
# them (one list there; the groups are FirstPersonApi.cpp's, where the settings are registered).
# ---------------------------------------------------------------------------------------------------------------------
SETTINGS = [
    ('View and camera', [
        ('Soldier camera preview', 'YES', 'YES / NO', 'Hovering a soldier\'s button shows their card with a live helmet camera. A right click opens their SOLDIER CAM, big.',
         'See what each soldier sees without leaving the one you play.'),
        ('Map camera', 'Off', 'Off / Small / Medium / Large', 'On the isometric map, the selected soldier\'s helmet camera in a window at the top left; it turns to your target while you aim.',
         'The map with a soldier\'s eyes beside it: aim on the map, check the view.'),
        ('Mouse sensitivity', '5', '1 to 20', 'How fast the view turns with the mouse.', ''),
        ('Invert mouse', 'NO', 'YES / NO', 'Mouse forward looks down, as in a flight simulator.', ''),
        ('Field of view', '75', '50 to 110', 'How wide the 3D view is, in degrees.', 'Wider shows more around you; narrower makes far targets bigger.'),
        ('Zoom', 'YES', 'YES / NO', 'Hold U or the right button to look four times closer.', 'Only the picture: the shot and the hit chance stay the same.'),
        ('Start battles in 3D', 'NO', 'YES / NO', 'Begin every battle in the first-person view.', ''),
        ('Alien turn in 3D', 'YES', 'YES / NO', 'Stay in 3D during the aliens\' turn, through your last soldier\'s eyes. NO: the map for their turn.', ''),
        ('Aliens\' turn cameras', 'YES', 'YES / NO', 'In their turn the view cuts to where the action is: a soldier firing back, the nearest soldier who sees a moving alien; helmet camera cards pop up for soldiers shot at, hit or mind controlled.', ''),
        ('Camera when a soldier falls', 'YES', 'YES / NO', 'With <i>Aliens\' turn cameras</i>, in their turn: a soldier going down out of view pops up their helmet camera card, DECEASED or UNCONSCIOUS.', ''),
        ('Eye height lowered by', '3', '0 to 6 voxels', 'Sits the 3D camera lower, so rooms feel less cramped. What the soldier sees by the rules doesn\'t change.', ''),
        ('Head bob', 'YES', 'YES / NO', 'The view bobs with the soldier\'s steps.', 'Off if it makes you queasy.'),
    ]),
    ('Movement', [
        ('Turning costs TU', 'NO', 'YES / NO', 'NO: the soldier turns with the view for free. YES: the head turns 45&deg; for free, beyond that the soldier turns at the game\'s cost, as on the map.',
         'YES is the classic rule: facing costs time units and decides reaction fire. NO is relaxed play.'),
        ('Movement', 'Tiles', 'Free / Tiles', 'Free: walk anywhere, stepping into the next tile when you cross into it. Tiles: a press walks one tile.',
         'Tiles feels like the map; Free lets you stand to one side of a tile (lean shots, peeking).'),
        ('Walking speed', '4', '1 to 8 tiles a second', 'How fast the soldier walks in 3D. Every tile still costs its time units.', ''),
        ('Flight cost per level (TU)', '0', '0 to 24', 'What a flying (or swimming) soldier pays to rise or sink a level in 3D.', 'The original game charges about 8.'),
        ('Flight cost overrides rules', 'NO', 'YES / NO', 'YES: every flyer pays the cost above. NO: each pays its armor\'s own cost.', 'NO keeps the game\'s slower flyers slow.'),
        ('Jump over low obstacles', 'NO', 'YES / NO', 'Space while walking into something low vaults over it, paying only the walk.', 'Bends the rules a little: on the map a fence makes you walk round.'),
        ('Falling', 'YES', 'YES / NO', 'Walk off a ledge and fall to the level below, hurt by the height.', 'A shortcut the map never offers, at the price of the fall.'),
        ('Check the landing', 'YES', 'YES / NO', 'A step off a ledge onto ground no one can walk on (a moat, the sea) or where no step leads on stops at the edge: UNSAFE LANDING! PUSH AGAIN.',
         'Keeps a soldier out of places they could never leave.'),
    ]),
    ('Aiming and combat', [
        ('FPS-style shooting (easier)', 'NO', 'YES / NO', 'NO: the game\'s own accuracy, its hit chance by the crosshair. YES: FPS-style shooting, far easier: rounds along the crosshair with a spread, recoil, from your eye.',
         'The big choice: see <a href="#styles">Two ways to play</a>.'),
        ('Akimbo', 'Off', 'Off / Full / One-hand', 'A gun in each hand fires both on every shot, the two taking turns round by round, each 30% less accurate, for the dearer gun\'s shot and a tenth.',
         'More rounds for the time units, worse aim; no aimed shots with two guns.'),
        ('Akimbo volley', 'NO', 'YES / NO', 'NO: two guns take turns round by round, each round resolved before the next. YES: both at once, a volley, their hits landing together.',
         'In turns a burst can kill one alien and go on to the next.'),
        ('Lean shots', 'NO', 'YES / NO', 'Standing to one side of the tile shoots from that side: the game\'s own off-centre origins.', 'Peek and shoot round a corner (with the game\'s own targeting; FPS style always shoots from the eye).'),
        ('Lean', 'Off', 'Off / Peek / Tilt', 'Left Shift with A or D leans out to peek; Tilt tilts the view too.', 'Shows round corners; a shot from a lean leaves from that side.'),
        ('Melee hits walls and downed', 'NO', 'YES / NO', 'A blow at the wall, door or object under the crosshair strikes it; a blow can hit a body lying in front of you.', 'Break through a wall with a hammer, as the map\'s terrain melee does in some mods.'),
        ('Melee hits floors', 'YES', 'YES / NO', 'With terrain melee on: the floor or ceiling you aim at takes the blow too.', 'Break through the floor at your feet or the ceiling above.'),
        ('Aim at exact point', 'YES', 'YES / NO', 'Shots go for the exact point under the crosshair (on the open sky, the air on its line). NO: for its tile, as in the original.',
         'The game\'s accuracy applies either way.'),
        ('Show trajectory', 'Arcs', 'Arcs / All / Off', 'While aiming, the path the next throw or shot will take: a cyan arc for throws, a red laser for shots (All).', 'No surprise "No line of fire": a refused shot\'s laser shows dimmed.'),
        ('Confirm fire', 'NO', 'YES / NO', 'The first click marks the target and shows its path; a second click fires.', 'Never waste a shot on a slip of the mouse.'),
        ('Force arcing shots', 'YES', 'YES / NO', 'An arcing shot the game would refuse fires anyway, as with Ctrl held; its path shows yellow.', ''),
        ('Force blocked shots', 'NO', 'YES / NO', 'A straight shot with no line of fire fires anyway along the line to the crosshair, as with Ctrl held.', 'A round may clear thin cover, or hit it.'),
        ('Shotgun pellets in flight', 'YES', 'YES / NO', 'A shotgun\'s pellets fly beside its round, in 3D and on the map.', ''),
        ('Each pellet on its own', 'YES', 'YES / NO', 'A shotgun\'s pellets and an akimbo volley\'s two rounds each land on their own: none lost when the lead pellet leaves the map.',
         'OXCE\'s "shotgun void" closed: shotguns hit as their pellets say.'),
        ('Full sprite hitboxes', 'NO', 'YES / NO', 'Units are hit, and seen, wherever their full sprites show (a card facing whoever fires), for everyone, in 3D and on the map.', 'What you see is what you hit: shots pass between legs and hit arms.'),
        ('Quick deaths', 'NO', 'YES / NO', 'Units killed or knocked out are down at once: bursts fire on, falls play on their own and together, in 3D and on the map; your soldier falls on their camera card while the view goes on to the next.', 'Doom-style pace: chaingun through a group without waiting for each fall.'),
        ('Unlimited tactical flares', 'YES', 'YES / NO', 'T throws a red flare at the crosshair, free and as often as you like.', 'Lights the 3D view only.'),
        ('Night vision needs gear', 'NO', 'YES / NO', 'Only soldiers with night vision gear (power suits, flying suits...) get NIGHT VISION in 3D. NO: everyone.', 'YES keeps the night dark for the unequipped.'),
    ]),
    ('Graphics and light', [
        ('Smooth lighting', 'YES', 'YES / NO', 'The battle\'s light blends smoothly from tile to tile. NO: tile by tile, as the map shades it.', ''),
        ('Shadows', 'YES', 'YES / NO', 'Ray-traced shadows from the sun, lamps and your flashlight, and under every unit.', 'Off on slow machines.'),
        ('Distance haze', 'Normal', 'Normal / Light / Clear', 'How soon far terrain fades into the haze.', ''),
        ('Flashlight', 'On', 'On / Dynamic / Off', 'The 3D view\'s own light, on F. On: a wide beam from the shoulder, in the dark. Dynamic: a tight spot on the crosshair, day or night.',
         'Lights only the 3D view: what the soldiers see by the rules doesn\'t change.'),
        ('Smoke', 'Modern', 'Modern / Original', 'Modern: the game\'s smoke as soft see-through clouds, solid where your soldier can\'t see through. Original: the games\' own smoke animation.',
         'Either way units the game hides stay hidden.'),
        ('Smoke opacity', '45%', '5% to 80%', 'How solid smoke you can see through looks: the thinnest, thicker smoke more. Where your soldier can\'t see through it, it stays solid.',
         'Lower it if smoke hides too much of the street; your soldier sees what the game says either way.'),
        ('Night smoke opacity', '25%', '5% to 80%', 'The same in the dark: on a night mission (lamp-lit or suit-lit smoke too), and by day on dark maps and in unlit rooms.',
         'Lower if smoke at night is blinding.'),
        ('Weapon in hand', 'YES', 'YES / NO', 'The weapon in hand at the bottom of the view, built in 3D from its inventory picture, its barrel on the crosshair.', ''),
        ('Sprite upscaling', 'Smooth + detail', 'Smooth + detail / Smooth / Off', 'Units, items and effects up close scaled the way pixel art scales well (xBRZ).', ''),
        ('Unit size', '100%', '80% to 100%', 'Soldiers, aliens, corpses and items drawn a little smaller, so tall sprites fit under low ceilings.', 'Only the picture: what shots hit stays the game\'s (see <i>Hitboxes</i>).'),
        ('Muzzle flashes', 'YES', 'YES / NO', 'Gunfire, hits and blasts flash. NO: no flashes, steady UFO lights, no camera glitches.', 'Off if flashing light bothers you.'),
        ('Particles', 'All', 'All / Effects / Off', 'Effects: sparks, debris, smoke, fire, trails. All: also dust, fireflies, bubbles and weather.', ''),
        ('UFO lights and aura', 'YES', 'YES / NO', 'UFOs glow: steady hulls with pulsing lights, their own coloured light, evenly inside them, and an aura of motes outside; only once a UFO has been seen.', 'Inside a UFO the view stays sharp: its light only colours it.'),
        ('Parts without shape', 'Lifts', 'Lifts / All / Off', 'Parts the map draws with no shape in the game\'s data, shown as see-through ghosts: ladders, lift pads, the shops\' racks, liquid over a floor, the rest of fences and catwalk gratings the game gives only a few voxels.',
         'Ghosts are only pictures: shots, sight and walking go through them as the game says.'),
        ('Ghost opacity', '50%', '10% to 100%', 'How solid the see-through ghosts look up close (lift pads, ladders, racks, catwalks, sludge); far off 60% of that.', ''),
        ('See-through corpses', 'NO', 'YES / NO', 'A corpse or item under the crosshair turns faint (at the ghost opacity) while you aim past it.', 'See past the big mods\' tall corpses to what your shot reaches.'),
        ('Discreet map edge', 'NO', 'YES / NO', 'Small red dots along the map\'s edge instead of the yellow tape.', ''),
        ('Pixel size', 'Auto', 'Auto / 1 / 2 / 3', 'Each ray colours a block of pixels: chunkier, much faster on big screens.', 'Auto: full detail up to 640&times;400, blocks of 2 above.'),
        ('Frame rate limit', '60', '60 to 240, unlimited', 'The game\'s own frame rate cap.', 'Raise it for a 120 or 144 Hz screen if your computer keeps up.'),
        ('Frame counter', 'NO', 'YES / NO', 'The game\'s frames-per-second counter (as F7).', ''),
    ]),
    ('HUD', [
        ('Crosshair', 'YES', 'YES / NO', 'The crosshair; its shape tells the fire mode.', ''),
        ('Shot info', 'YES', 'YES / NO', 'The fire mode, hit chance and time units right of the crosshair.', 'Yellow: you can; red: you can\'t.'),
        ('Expanded crosshair info', 'YES', 'YES / NO', 'The action\'s other costs under it: energy, health, stun, morale, mana.', 'The big mods\' weapons often cost energy.'),
        ('Accuracy line', 'YES', 'YES / NO', 'A green line under the crosshair, as wide as a shot strays to either side: the game\'s successful roll (its own targeting), or the spread (FPS style).',
         'With the game\'s own targeting it is short: a hit strays little, a miss far; the hit chance says how likely each is.'),
        ('Unit names', 'YES', 'YES / NO', 'The name of the unit you point at, under the crosshair.', ''),
        ('Stat bars', 'Scaled', 'Scaled / Percentage / Game\'s own', 'How the time units, energy, health and morale bars read, on the map and in 3D. Scaled: one scale for all your units, so a 300-HP unit\'s bar shows its loss.',
         'Two soldiers\' bars stay comparable.'),
        ('Hitboxes', 'Aimed at', 'Off / Aimed at / Blink / Always', 'Outlines the voxels a shot can really hit: the unit you aim at, or every unit in sight (blinking or always).', 'Sprites are often taller or wider than what can be hit.'),
        ('Glowing outlines', 'NO', 'YES / NO', 'The hitboxes and a throw\'s path glow.', ''),
        ('Downed unit icons', 'Crosshair', 'Crosshair / Always / Off', 'Icons for an unconscious unit: burning, bleeding, shock, out cold.', 'Know whom to carry out first.'),
        ('Compass', 'YES', 'YES / NO', 'A compass at the top right: your heading, the way the soldier faces, and (with turning at a cost) how far the head turns for free.', ''),
        ('Middle click identifies', 'YES', 'YES / NO', 'A middle click on a unit opens the game\'s unit screen, as on the map.', ''),
    ]),
    ('Sound', [
        ('Dynamic music', 'YES', 'YES / NO', 'Tenser music while aliens are in sight.', ''),
        ('Hit, kill and swing sounds', 'YES', 'YES / NO', 'A tick for a hit, a double tick for a kill, a swoosh for your swings, and your heartbeat once hurt below half health.', ''),
    ]),
    ('Cheats', [
        ('Doom mode (debug)', 'NO', 'YES / NO', 'A cheat: in 3D the selected soldier never runs out of time units or energy; grenades go off in real time.', 'For testing and fun, a first step towards real time.'),
        ('Doom IDKFA', 'NO', 'YES / NO', 'Doom mode plus endless ammo.', ''),
        ('Doom IDDQD', 'NO', 'YES / NO', 'Doom mode plus invincible soldiers.', ''),
        ('Doom prime on throw', 'YES', 'YES / NO', 'In Doom mode a grenade is armed as you throw it. NO: priming pulls the pin, and the fuse burns in your hand.', ''),
        ('Doom free grenades with airburst', 'NO', 'YES / NO', 'In Doom mode a grenade flies wherever the crosshair points and bursts in the air if its fuse runs out.', ''),
        ('Doom grenade fuse (seconds)', '3', '1 to 5', 'In Doom mode, how long a grenade burns.', ''),
    ]),
]

# ---------------------------------------------------------------------------------------------------------------------
# On screen: the HUD's parts (numbered on the annotated picture: x%, y% from its top left) and the icons and messages.
# ---------------------------------------------------------------------------------------------------------------------
HUD_MARKS = [
    (1, 11, 9, 'ISO, HUD, INV', 'The map and back, the classic HUD, the inventory.'),
    (2, 51, 9, 'The squad', 'Your soldiers in the middle of the top bar: click to select, hover for the card with a live helmet camera, right click for the SOLDIER CAM; &#9664; and &#9654; for the previous and next. A skull marks the dead, a caduceus the unconscious, an orange alien face one under mind control.'),
    (3, 80, 9, 'LIGHT, END and the rest', 'The suit light, the end of the turn, and when the soldier has them NIGHT VISION and SUIT FLY.'),
    (4, 85, 22, 'The compass', 'Your heading under the white arrow, the way the soldier faces (a yellow triangle), and a shot\'s turn and its cost.'),
    (5, 44, 46, 'The crosshair', 'Its shape is the fire mode; the accuracy line under it; the shot info beside it (mode, hit chance, time units, the other costs below); the name of the unit pointed at under all that.'),
    (6, 27, 76, 'The soldier', 'Rank and name, what matters now (KNEELING, BURNING, WOUNDS, PANIC), a line per hand with its ammo, the four stat bars.'),
    (7, 66, 84, 'The weapon in hand', 'Built in 3D from its inventory picture, its barrel on the line to the crosshair; your shots leave its muzzle.'),
    (8, 92, 61, 'The enemies in sight', 'A red button for each, a red number over its head: click the button, or press 1 to 0, to turn to it.'),
]
CROSSHAIRS = [('Snap', 'An open cross'), ('Aimed', 'A scope ring'), ('Auto', 'Brackets, a dot per round'), ('Throw', 'An arc onto a ring'),
              ('Launch', 'A diamond'), ('Melee', 'Claw marks'), ('Psi', 'Rings spreading out'), ('Use, prime', 'A box'),
              ('A hit', 'White ticks on the diagonals'), ('A kill', 'Longer red ticks')]
SIGNS = [
    ('The crosshair\'s colour', 'Yellow: you can do it. Red: you can\'t (time units, ammo, range). Purple: psi. Grey: not your move.'),
    ('The shot info', 'SNAP 40% 15TU: the fire mode, the hit chance (green; yellow with range or sight penalties; red when it can\'t be taken) and the time units. With two guns DUAL (round by round), VOLLEY (both at once) or IN TURN (an arcing pair, gun after gun) in front.'),
    ('The other costs', 'EN 6 STUN 2 under the shot info: energy, health, stun, morale, mana. A cost you can\'t pay is red, and the game would refuse the action.'),
    ('The accuracy line', 'A green line under the crosshair, a notch up at each end: as far to either side as a successful roll can stray (the game\'s own targeting), or the spread in FPS style, widening as you fire. Grey when it\'s not your move.'),
    ('The trajectory', 'A light cyan arc for a throw or an arcing shot, a diamond where it lands; a thin red laser for a straight shot, a bright dot where it meets something. Yellow: forced (it goes whatever is in the way). Dim dotted red: the game will refuse it.'),
    ('Unit names', 'The unit under the crosshair, by name or race: green X-COM, red aliens, yellow civilians.'),
    ('Downed unit icons', 'Over an unconscious body: a flame (burning), a drop (bleeding), a bolt (shock), Zz (out cold).'),
    ('Hitboxes and facing rings', 'Outlines of what a shot can really hit (setting <i>Hitboxes</i>); with <kbd>Left Alt</kbd> a ring and arrow round every unit in sight, the way it faces.'),
    ('Enemy markers', 'Red numbers over the aliens in sight: the keys 1 to 0 turn to them.'),
    ('Lift arrows', 'Purple arrows and LIFT PLATFORM on a grav lift or a ladder: the ways it goes. Arrows left of the compass show where W climbs.'),
    ('Under the compass', 'SPRINT (green while running, grey when the soldier can\'t), EN PASSANT, TURN 1 TU, SHOT +1 TU, NO TU.'),
    ('At the right', 'SCOPING 2.6x while zooming.'),
    ('Warnings', 'UNSAFE LANDING! PUSH AGAIN at a ledge over ground no one can walk on; the game\'s own warnings (No Line of Fire!, Out of Range!), on the warning line.'),
    ('Banners', 'ENEMY SPOTTED!, HIDDEN MOVEMENT, BURNING, YOU HAVE A MENTAL BREAKDOWN, BERSERK, TURNED INTO A ZOMBIE.'),
    ('Camera cards', 'A soldier\'s helmet camera popping up: TARGETED, HIT, MIND CONTROLLED, FIRING, WATCHING, DECEASED, UNCONSCIOUS. A blinking red dot and now and then a bad signal or static: it\'s a camera, not your view. SIGNAL LOST when the soldier you look through falls.'),
    ('The info panel\'s right side', 'DOOM MODE while the cheat is on (IDDQD, IDKFA or both in its place with those); with IDDQD the stats turn silver.'),
]

# ---------------------------------------------------------------------------------------------------------------------
# Features: (id, title, pictures [(file, caption)], keys, what it is, how it plays)
# ---------------------------------------------------------------------------------------------------------------------
FEATURES = [
    ('aim', 'Aiming, by the crosshair', [('sky_edge.png', 'On the open sky at the map\'s edge, the laser runs to the crosshair and the hit chance shows'), ('peek_fps.png', 'FPS style: leaning out past a wall\'s end, the round leaves from the eye')],
     'Left button, <kbd>Z</kbd> <kbd>X</kbd> <kbd>V</kbd>, wheel',
     'With <i>Aim at exact point</i> a shot goes for the very voxel under the crosshair, or on the open sky for the air on its line. If the line from the gun is blocked, the game\'s other muzzle points are tried (left and right with OXCE\'s <i>Off-centre shooting</i>), then the game\'s own aim, as a click on the map does. The laser (<i>Show trajectory: All</i>) shows the line that will fire.',
     'With the game\'s own targeting it is the map\'s rules with a finer click. In FPS style the round leaves from your eye: what you see past a corner, you can hit. With <i>Full sprite hitboxes</i> a unit is hit, and seen, where its picture shows: between the legs is a miss, a gun held out a hit.'),
    ('line', 'The accuracy line', [('accuracy_line.png', 'The game\'s own targeting (left): a short line, as far as a hit strays. FPS style (right): the spread of a burst')],
     '', 'A green line under the crosshair, a notch up at each end, as wide as a round can land to either side. With the game\'s own targeting: the farthest a successful roll can send the round, by the game\'s own roll. In FPS style: the spread 95% of the rounds land in, widening with the bloom as you fire.',
     'A successful roll in X-COM strays little, mostly sideways; a miss strays far. The line shows the first, the hit chance how often you get it.'),
    ('akimbo', 'Akimbo', [('ufo_akimbo_both_lasers.png', 'Two laser pistols, both lasers showing'), ('ufo_akimbo_on_the_map.png', 'Akimbo on the map: two mini targets in each hand\'s box, in turn')],
     '<kbd>Y</kbd>, <i>Akimbo style</i>, <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+click on the map',
     'A gun in each hand fires both (the setting <i>Akimbo</i>, Full or One-hand): round by round in turn, or at once as a volley, each its own burst, each 30% less accurate, for the dearer gun\'s shot and a tenth. On the map Ctrl+Shift+click a hand\'s weapon marks the pair.',
     'More rounds for your time units, worse aim; no aimed shots with two guns. In turn a burst can drop one alien and go on to the next.'),
    ('lean', 'Lean and peek', [('lean_tilt.png', 'Leaning out with Tilt, round a corner')],
     '<kbd>Left Shift</kbd> + <kbd>A</kbd> / <kbd>D</kbd>',
     'The eye moves up to six voxels to the side and a little lower, stopping short of walls and of anyone in sight; with Tilt the view tilts too. A shot from a lean leaves from that side: the game\'s own off-centre point (its own targeting) or your eye (FPS style).',
     'You see the street round the corner, but who is spotted is still the game\'s call: a peek never shows a hidden alien.'),
    ('move', 'Walking, jumping, falling', [('landing_warning.png', 'TFTD, Check the landing: a step towards the sea stops at the edge, UNSAFE LANDING! PUSH AGAIN'), ('sprint_dusk.png', 'Sprinting down a street at dusk')],
     '<kbd>W</kbd><kbd>A</kbd><kbd>S</kbd><kbd>D</kbd>, numpad, <kbd>Space</kbd>, <kbd>Caps Lock</kbd>, <kbd>`</kbd>',
     'Walk freely or tile by tile, every tile at its time units. Space vaults low obstacles; walking off a ledge falls to the level below, hurt by the height. A step onto ground no one can walk on, or where no step leads on, asks first.',
     'Falls and vaults are shortcuts the map doesn\'t have (each a setting). Sprint costs less time and more energy; en passant keeps walking when an enemy shows up.'),
    ('fly', 'Lifts, ladders and flight', [('ufo_flight_climb_arrow.png', 'A flying suit: the arrow left of the compass lit yellow, W climbs')],
     '<kbd>Page Up</kbd> <kbd>Page Down</kbd>, <kbd>B</kbd>, <kbd>W</kbd> looking up',
     'Ride a grav lift, climb a ladder, fly with SUIT FLY on: W looking up climbs, looking down sinks, each level paid as on the map. Purple arrows show the ways.',
     ''),
    ('doors', 'Doors', [],
     '<kbd>O</kbd>, or walk into it',
     'Doors open as you walk into them, or with O from where you stand (the one you look at). They swing to where the game has them; UFO doors slide. Glass doors are seen through, while they swing too.',
     'Open a door without stepping into the room behind it.'),
    ('melee', 'Melee, medi-kits and items', [('ufo_medikit_teammate.png', 'A medi-kit by the crosshair: USE: and the teammate\'s name'), ('ufo_motion_scanner.png', 'The motion scanner held up: the sweep going out, two blips behind the soldier')],
     'Left button in the melee or use mode',
     'Melee strikes what\'s under the crosshair: a blow swings, lunges and jolts. A medi-kit treats whoever you look at, next to you; aiming at nothing, yourself. The motion scanner is held up at the bottom of the view, turning as you look round, a ring sweeping out with a whoop and a pip for each movement it finds; the mind probe to its screen; priming opens the timer at once.',
     ''),
    ('throw', 'Throwing and grenades', [('airburst.png', 'Doom free grenades: an airburst over a field')],
     'Fire mode throw or prime; <kbd>T</kbd> for a flare',
     'Anything in a hand can be thrown along the game\'s arc (the cyan path shows it). In Doom mode grenades burn in real time, and with free grenades fly wherever the crosshair points and burst in the air.',
     ''),
    ('zoom', 'Zoom', [('scope_night.png', 'SCOPING at night, the blue scope cross on the crosshair')],
     '<kbd>U</kbd> or the right button, held', 'The view glides in four times closer, the weapon lowers, the mouse slows.', 'Only the picture: the shot and its odds don\'t change.'),
    ('light', 'Light and night', [('night_vision_laser.png', 'Night vision: the laser sight light in the goggles')],
     '<kbd>F</kbd>, <kbd>N</kbd>, <kbd>T</kbd>, <kbd>L</kbd>',
     'Your flashlight (on F, wide or a tight spot on the crosshair), night vision with the gear for it, red tactical flares, and the game\'s suit light (L). Light and shadow come from lamps, the sun, your flashlight and flares; indoors (a roof overhead, or rock) no sun shines, and rooms are lit by their own light, as on the map.',
     'The flashlight and flares light only your view; the suit light is the game\'s, and lets aliens see your soldiers at night.'),
    ('smoke', 'Smoke', [('smoke.png', 'Modern smoke: soft see-through clouds, solid where the soldier can\'t see through')],
     '', 'Modern: the game\'s smoke as soft clouds you see through, solid where your soldier can\'t see through by the game\'s sight rules. Original: the games\' own animation, stacked thick there. Heat vision sees smoke thinner.',
     'What you see through matches what your soldier sees through.'),
    ('cams', 'Helmet cameras', [('soldier_cam_dusk.png', 'A SOLDIER CAM at dusk'), ('map_camera.png', 'The map camera on the isometric map'), ('killed_in_action.png', 'Killed in action: the picture turns red, then breaks into static, SIGNAL LOST')],
     'Hover a soldier\'s button; right click for the SOLDIER CAM; the wheel over a button; <kbd>P</kbd> on the map',
     'Every soldier\'s helmet camera, live: on their card, big as the SOLDIER CAM, or over the map (setting <i>Map camera</i>), with a blinking red dot, now and then a bad signal or static, worse on hard missions. With the cursor free, the mouse wheel over a soldier\'s button steps the card or the SOLDIER CAM through the squad. When the soldier you look through falls, the picture turns red and breaks into static: SIGNAL LOST.',
     'See what each soldier sees; aim on the map with a soldier\'s eyes beside it.'),
    ('deaths', 'Quick deaths', [('ufo_quick_death_card.png', 'The soldier looked through is down: the view goes on with the next one, while the fallen soldier\'s card, DECEASED, shows his last look')],
     '', 'With <i>Quick deaths</i> a unit killed or knocked out is down at once: a burst fires on through it to whoever stands behind, several fall together, each fall on its own, at the game\'s pace, in 3D and on the map. When the soldier you look through goes down, their fall plays on their camera card while the view goes on to the next soldier at once.',
     'The pace of a shooter: chaingun through a group without waiting for each fall.'),
    ('aliens', 'The aliens\' turn', [('under_fire.png', 'Under fire in the aliens\' turn')],
     '', 'Stay in 3D for the aliens\' turn: the view cuts to a soldier firing back or to the nearest one who sees a moving alien; camera cards pop up for soldiers shot at, hit, mind controlled or going down.',
     'Watch the reaction fire you set up.'),
    ('ghosts', 'Ghosts: lifts, racks, catwalks, sludge', [('ufo_shop_racks.png', 'UFO\'s shop racks as ghosts in their real shape, at Ghost opacity'), ('piratez_catwalk.png', 'XPiratez: a catwalk\'s grating overhead, its rest a ghost'), ('piratez_sludge.png', 'XPiratez: the sewer\'s sludge, a soldier wading in it to the thighs')],
     '', 'Some parts the map draws have no shape, or only a few voxels, in the game\'s data. They show as see-through ghosts of their art: ladders and lift pads, the shops\' racks, the rest of fences, railings and catwalk gratings, and the surface of liquid over a floor (XPiratez\'s sewers). The game\'s own voxels stay solid.',
     'Ghosts are pictures: shots, sight and walking go through them as the game says. Set how solid with <i>Ghost opacity</i>.'),
    ('sea', 'The sea, and under it', [('tftd_sea_past_the_edge.png', 'TFTD: the sea rolling in waves past an island\'s edge'), ('tftd_ruins.png', 'TFTD: ruins on the seabed, bubbles rising, light rippling over the sand')],
     '', 'Terror From The Deep\'s sea rolls in waves, and where it reaches the map\'s edge it goes on past it. Under the water: bubbles, marine snow drifting down, and light rippling over the sand.', 'The particles are a setting (<i>Particles</i>), if you want the water clear.'),
    ('shotguns', 'Shotguns', [('shotgun_pellets_map.png', 'Pellets in flight on the map')],
     '', 'Every pellet lands on its own (<i>Each pellet on its own</i>), and with <i>Shotgun pellets in flight</i> each flies beside the round and hits as it gets there.',
     'No more "shotgun void": a miss by the lead pellet no longer takes the whole blast with it.'),
    ('bars', 'Stat bars', [('stat_bars.png', 'A soldier at 60 morale of 100: the game\'s own bars (left), morale full; Scaled (right), what\'s gone shows')],
     '', 'Bars that read: with <i>Scaled</i> a stat whose longest maximum won\'t fit shrinks for all your units alike, on the map and in 3D.', 'Two soldiers\' bars stay comparable.'),
    ('perf', 'Performance', [],
     '', '<i>Pixel size</i> 2 or 3 colours a block of pixels per ray, much faster on big screens; <i>Shadows</i> and <i>Particles</i> off help slow machines; the frame rate cap is the game\'s own.', ''),
]

# The gallery at the end: the collages (pages/img/gallery/), (file, title, caption)
GALLERY_IMG = 'img/gallery/'
GALLERY = [
    ('collage_108.png', 'Field report 1.08', 'Terror From The Deep\'s sea past an island\'s edge, XPiratez\'s blunderbuss and two guns at once, a spray burst marked, akimbo in 3D and on the isometric map, a Gill Man on an island, and the map camera over the battle.'),
    ('collage_more.png', 'More from 1.08', 'The open sky at the map\'s edge, a peek past a corner in FPS style, the accuracy line, the landing warning by the sea, wading in XPiratez\'s sludge, the shop\'s racks as ghosts, a tilted lean, smoke, a grenade\'s arc.'),
    ('collage_moments.png', 'Moments', 'Under fire, a soldier cam at dusk, a sprint down an empty street, night vision, an airburst, a rocket in the street, aquanauts with vibro blades, the ruins on the seabed, and the end of a signal.'),
]

# ---------------------------------------------------------------------------------------------------------------------
STYLES = '''<section id="styles" class="sec gutter"><div class="col">
<h2>Two ways to play</h2>
<p class="intro">One setting decides how shots work: <i>FPS-style shooting (easier)</i>. Everything else (time units, energy, damage, line of sight, reaction fire, the aliens' minds) is the original game's either way.</p>
<div class="two">
  <div class="card">
    <h3>The game's own targeting <span class="tag">the default</span></h3>
    <ul>
      <li>The game's own accuracy roll; the hit chance shows by the crosshair, as the map's cursor shows it.</li>
      <li>Shots go for the exact point under the crosshair (or its tile, with <i>Aim at exact point</i> off), from the game's own muzzle points; "No line of fire" where the game says so (the laser shows it dimmed first).</li>
      <li>The accuracy line: how far a successful roll strays.</li>
      <li>Snap, aimed and auto as in the game.</li>
      <li>What you can do from a tile in 3D is what you can do on the map: switching with F8 never changes a shot.</li>
    </ul>
  </div>
  <div class="card">
    <h3>FPS style <span class="tag">FPS-style shooting on</span></h3>
    <ul>
      <li>No dice: rounds fly along the crosshair with a spread (firing skill and kneeling tighten it), recoil lifts your aim, the bloom grows as you fire.</li>
      <li>Each round leaves from your eye, so what you see past a corner, you can hit; and hits whatever is in the way.</li>
      <li>Snap = one round from the hip, aimed = one tight round, auto = a burst, hold the trigger for more.</li>
      <li>Rounds fly at a bullet's pace.</li>
      <li>Your skill decides more, the soldier's less: relaxed, and a different game.</li>
    </ul>
  </div>
</div>
<h3>Settings that bend the rules a little</h3>
<p>Each a setting, so you choose: <i>Jump over low obstacles</i> (vault a fence for the walk), <i>Falling</i> (step off a ledge), <i>Lean shots</i> and <i>Lean</i> (shoot from the side of your tile), <i>Akimbo</i>, <i>Melee hits walls and downed</i>, <i>Force arcing shots</i> and <i>Force blocked shots</i> (as Ctrl on the map), <i>Turning costs TU</i> off (free turns), and the Doom cheats. Everything else only changes what you see.</p>
</div></section>'''

def size_in(folder, name):
    """A picture's width and height in a folder of pages/ (1280x800 for one not there yet)."""
    try:
        from PIL import Image
        return Image.open(os.path.join(ROOT, 'pages', folder, name)).size
    except (ImportError, OSError):
        return 1280, 800


def size(name):
    """A picture's width and height as the HTML gives them (read from the file; 1280x800 for one not there yet)."""
    try:
        from PIL import Image
        return Image.open(os.path.join(ROOT, 'pages', IMG, name)).size
    except (ImportError, OSError):
        return 1280, 800


def sections():
    """The guide's sections, as HTML, for between the page's guide markers."""
    out = []
    w = out.append
    w('<div class="guide">')
    # start
    w('''<section id="start" class="sec gutter"><div class="col">
<h2>Start here</h2>
<ol class="quick">
  <li>In a battle press <kbd>F8</kbd>: you're in your soldier's eyes. <kbd>F8</kbd> again goes back to the map, any time.</li>
  <li>Look with the mouse, walk with <kbd>W</kbd><kbd>A</kbd><kbd>S</kbd><kbd>D</kbd>: every tile costs its time units, as on the map.</li>
  <li>Click to fire at the crosshair; <kbd>Z</kbd> <kbd>X</kbd> <kbd>V</kbd> (or the wheel) pick snap, aimed or auto.</li>
  <li><kbd>C</kbd> frees the cursor to click the buttons; <kbd>H</kbd> brings the map's whole bottom HUD into 3D.</li>
  <li><kbd>Tab</kbd> goes to the next soldier, <kbd>Backspace</kbd> ends the turn.</li>
  <li><b>Esc &rarr; Options &rarr; 3D VIEW</b> has the settings and the keys, from the main menu or in a battle.</li>
</ol>
</div></section>''')
    w(STYLES)
    # keys
    w('<section id="keys" class="sec gutter"><div class="col">\n<h2>Keys</h2>\n<p class="intro">The defaults: every one can be rebound in <b>Options &rarr; 3D VIEW &rarr; Controls</b> (click its row, press the new key; a right click clears it).</p>')
    for group, rows in KEYS:
        w('<h3>%s</h3>\n<div class="tablewrap"><table class="grid keys"><thead><tr><th>Key</th><th>What it does</th><th>How it plays</th></tr></thead><tbody>' % group)
        for keys, what, plays in rows:
            w('<tr class="row"><td class="k">%s</td><td>%s</td><td class="plays">%s</td></tr>' % (keys, what, plays))
        w('</tbody></table></div>')
    w('''<h3>The classic HUD in 3D</h3>
<p><kbd>H</kbd> shows the game's own bottom HUD over the 3D view with a free cursor; every button works. The map level buttons look 30&deg; up or down, the centre button turns the view back to the way the soldier faces, the red enemy buttons turn the view to that enemy (right click: switch to the soldier who sees it), and a shot picked from a hand's menu becomes the fire mode.</p>
<h3>On the Steam Deck</h3>
<p>The controller layout made for OpenXcom3D maps all of this to the Deck's buttons: see <a href="#deck">the Steam Deck steps</a> above.</p>
</div></section>''')
    # screen
    w('<section id="screen" class="sec gutter"><div class="col">\n<h2>On screen</h2>\n<p class="intro">The 3D view\'s heads-up display, its icons and its messages.</p>')
    w('<figure class="tour"><div class="marks"><img src="%shud.png" width="1280" height="800" loading="lazy" alt="The 3D view with its heads-up display: the top bar, the compass, the crosshair with its shot info, the soldier\'s panel and the weapon in hand.">' % IMG)
    for n, x, y, title, _ in HUD_MARKS:
        w('<span class="mark" style="left:%d%%;top:%d%%" aria-hidden="true">%d</span>' % (x, y, n))
    w('</div><figcaption>The heads-up display, numbered below.</figcaption></figure>\n<ol class="legend">')
    for n, x, y, title, text in HUD_MARKS:
        w('<li class="row"><b>%s.</b> %s</li>' % (title, text))
    w('</ol>\n<h3>The crosshair: one shape per fire mode, ticks for a hit</h3>\n<figure class="strip"><img src="%scrosshairs.png" width="1280" height="200" loading="lazy" alt="The crosshair\'s shapes, one per fire mode: snap, aimed, auto, throw, launch, melee, psi, use; and the ticks of a hit and a kill."></figure>\n<ul class="chips">' % IMG)
    for name, shape in CROSSHAIRS:
        w('<li><b>%s</b> %s</li>' % (name, shape))
    w('</ul>\n<h3>Signs and messages</h3>\n<div class="tablewrap"><table class="grid"><tbody>')
    for name, text in SIGNS:
        w('<tr class="row"><th>%s</th><td>%s</td></tr>' % (name, text))
    w('</tbody></table></div>\n<figure class="pair"><img src="%strajectory.png" width="1280" height="800" loading="lazy" alt="The trajectory of a throw: a light cyan arc onto a diamond where it lands."><figcaption>The trajectory: a grenade\'s arc, and where it lands.</figcaption></figure>\n</div></section>' % IMG)
    # settings
    w('<section id="settings" class="sec gutter"><div class="col">\n<h2>Settings</h2>\n<p class="intro"><b>Options &rarr; 3D VIEW &rarr; Settings</b>, from the main menu or in a battle: one list there, in this order, grouped here. A left click flips YES / NO or raises a number, a right click lowers it; the default is in bold.</p>')
    for group, rows in SETTINGS:
        w('<h3>%s</h3>\n<div class="tablewrap"><table class="grid settings"><thead><tr><th>Setting</th><th>Choices</th><th>What it does</th><th>How it plays</th></tr></thead><tbody>' % group)
        for name, default, choices, what, plays in rows:
            shown = ' / '.join('<b>%s</b>' % c if c == default else c for c in choices.split(' / ')) if default in choices.split(' / ') else '%s <span class="def">(<b>%s</b>)</span>' % (choices, default)
            w('<tr class="row"><td class="s">%s</td><td class="c">%s</td><td>%s</td><td class="plays">%s</td></tr>' % (name, shown, what, plays))
        w('</tbody></table></div>')
    w('</div></section>')
    # features
    w('<section id="features" class="sec gutter"><div class="col">\n<h2>Features</h2>\n<p class="intro">What there is to do in 3D, how it plays, and what it looks like.</p>')
    for fid, title, pictures, keys, what, plays in FEATURES:
        w('<article class="feature row" id="f-%s"><h3>%s</h3>' % (fid, title))
        if pictures:
            # (two pictures to a row; a wide one, side by side halves, takes the whole row)
            w('<div class="shots">' + ''.join('<figure%s><a href="%s%s"><img src="%s%s" width="%d" height="%d" loading="lazy" alt="%s"></a><figcaption>%s</figcaption></figure>'
                % ((' class="wide"' if size(f)[0] > 2 * size(f)[1] else '', IMG, f, IMG, f) + size(f) + (c, c)) for f, c in pictures) + '</div>')
        w('<p>%s</p>' % what)
        if keys:
            w('<p class="meta"><span>Keys</span> %s</p>' % keys)
        if plays:
            w('<p class="meta"><span>How it plays</span> %s</p>' % plays)
        w('</article>')
    w('</div></section>')
    # the gallery
    w('<section id="gallery" class="sec gutter"><div class="col gallery">\n<h2>Gallery</h2>\n<p class="intro">From the field: the battles in pictures, from <i>UFO</i>, <i>Terror From The Deep</i> and XPiratez. Click one for it at full size.</p>')
    for f, title, caption in GALLERY:
        width, height = size_in(GALLERY_IMG, f)
        w('<figure class="row"><a href="%s%s"><img src="%s%s" width="%d" height="%d" loading="lazy" alt="%s: %s"></a><figcaption><b>%s</b> %s</figcaption></figure>'
          % (GALLERY_IMG, f, GALLERY_IMG, f, width, height, title, caption, title, caption))
    w('</div></section>\n</div>')
    return '\n'.join(out)


def splice(text, begin, end, inner):
    """The text with what lies between two markers replaced."""
    a = text.index(begin) + len(begin)
    b = text.index(end, a)
    return text[:a] + '\n' + inner + '\n' + text[b:]


EXTRA_CSS = '''
/* ---------- the bar: sections and search (the system switch sits with the steps) ---------- */
.jump { display: flex; flex-wrap: wrap; gap: 0.3rem 0.9rem; font: 700 0.8rem/1.4 var(--font-mono); text-transform: uppercase; letter-spacing: 0.05em; }
.jump a { text-decoration: none; color: var(--accent); }
.jump a:hover { color: var(--gold); }
.find { display: flex; align-items: center; gap: 0.5rem; font: 700 0.8rem/1 var(--font-mono); text-transform: uppercase; letter-spacing: 0.05em; color: var(--muted); }
.find input {
  font: 0.92rem var(--font-body); padding: 0.45rem 0.6rem; border: 1px solid var(--edge); border-radius: 2px;
  background: var(--term-bg); color: var(--fg); width: 15rem; max-width: 60vw; box-shadow: inset 0 0 12px rgb(58 114 196 / 0.18);
}
.find input::placeholder { color: var(--term-dim); }
.ospanel {
  display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 0.6rem 1.5rem; margin: 0 0 1rem;
  background: var(--surface); border: 1px solid var(--edge); border-radius: 2px; padding: 0.65rem 0.9rem; box-shadow: var(--shadow);
}
.part { font-size: clamp(1.4rem, 3.5vw, 1.9rem); margin: 0 0 0.75rem; color: var(--gold); text-transform: uppercase; display: flex; align-items: center; gap: 0.75rem; }
/* ---------- the guide: each section's title an X-COM screen's, gold with a line ---------- */
.guide .col { max-width: 64rem; }
.sec .col { padding-block: 2.25rem 0.5rem; }
.sec h2, .part {
  display: flex; align-items: center; gap: 0.75rem; font-size: clamp(1.4rem, 3.5vw, 1.9rem); margin: 0 0 0.6rem;
  color: var(--gold); text-transform: uppercase; letter-spacing: 0.06em; text-shadow: 0 0 18px rgb(242 196 67 / 0.25);
}
.sec h2::before, .part::before { content: "//"; color: var(--edge); text-shadow: none; }
.sec h2::after, .part::after { content: ""; flex: 1; height: 2px; background: linear-gradient(90deg, var(--edge), transparent); }
.sec h3 { font: 700 0.95rem/1.3 var(--font-mono); text-transform: uppercase; letter-spacing: 0.06em; color: var(--cyan); margin: 1.8rem 0 0.6rem; }
.guide .intro { max-width: 46rem; color: var(--muted); margin: 0 0 1rem; }
.quick { display: grid; gap: 0.5rem; padding-left: 1.5rem; max-width: 46rem; }
.quick li::marker { color: var(--gold); font-family: var(--font-mono); font-weight: 700; }
.two { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 1rem; }
.card { background: var(--surface); border: 1px solid var(--line); border-top: 3px solid var(--gold); border-radius: 2px; padding: 0.9rem 1.1rem; box-shadow: var(--shadow); }
.two .card:last-child { border-top-color: var(--cyan); }
.card h3 { margin-top: 0.2rem; color: var(--fg); font-size: 1rem; }
.card ul { padding-left: 1.1rem; margin: 0; display: grid; gap: 0.4rem; }
.card li::marker { color: var(--gold); }
.tag { font: 700 0.68rem/1 var(--font-mono); background: transparent; color: var(--gold); border: 1px solid var(--gold); padding: 0.2rem 0.4rem; border-radius: 2px; vertical-align: middle; letter-spacing: 0.06em; }
.two .card:last-child .tag { color: var(--cyan); border-color: var(--cyan); }
/* ---------- tables: X-COM panels ---------- */
.tablewrap { overflow-x: auto; border: 1px solid var(--edge); border-radius: 2px; background: var(--surface); box-shadow: var(--shadow); }
table.grid { border-collapse: collapse; width: 100%; font-size: 0.93rem; }
.grid th, .grid td { text-align: left; vertical-align: top; padding: 0.55rem 0.75rem; border-top: 1px solid var(--line); }
.grid thead th {
  border-top: 0; background: linear-gradient(#173159, #10233f); font: 700 0.74rem/1.3 var(--font-mono);
  text-transform: uppercase; letter-spacing: 0.08em; color: var(--gold); border-bottom: 1px solid var(--edge);
}
.grid tbody tr:hover { background: var(--raise); }
.grid tbody th { width: 13rem; color: var(--fg); }
.grid td.k { white-space: nowrap; width: 12rem; }
.grid td.s { font-weight: 700; width: 12rem; color: #f0f5ff; }
.grid td.c { width: 10rem; color: var(--muted); font: 0.84rem/1.5 var(--font-mono); }
.grid td.c b { color: var(--gold); }
.grid td.plays { color: var(--muted); width: 30%; }
.def { color: var(--muted); }
.guide kbd { margin: 0.08em 0.1em; }
/* ---------- pictures: helmet camera frames ---------- */
.guide figure { margin: 0 0 1rem; }
.guide figcaption { font-size: 0.85rem; color: var(--muted); margin-top: 0.45rem; }
.marks { position: relative; }
.marks img, .shots img, .strip img, .pair img, .gallery img {
  border: 2px solid var(--edge); border-radius: 2px; image-rendering: pixelated;
  box-shadow: 0 0 0 1px #02060d, 0 0 22px rgb(58 114 196 / 0.25), 0 12px 30px rgb(0 0 0 / 0.55);
}
.shots a:hover img, .gallery a:hover img { border-color: var(--gold); }
.pair { max-width: 40rem; }
.mark { position: absolute; transform: translate(-50%, -50%); width: 1.8rem; height: 1.8rem; border-radius: 50%; display: grid; place-items: center;
  background: radial-gradient(circle at 35% 30%, #ffe28a, var(--gold) 60%, #b98a17); color: var(--gold-ink); font: 700 0.95rem/1 var(--font-mono);
  box-shadow: 0 0 0 3px rgb(0 0 0 / 0.7), 0 0 14px rgb(242 196 67 / 0.6); }
.legend { display: grid; gap: 0.4rem; padding-left: 1.5rem; max-width: 52rem; }
.legend li::marker { color: var(--gold); font: 700 1em var(--font-mono); }
.legend b { color: #f0f5ff; }
.chips { display: flex; flex-wrap: wrap; gap: 0.4rem; list-style: none; padding: 0; margin: 0 0 1rem; }
.chips li { background: linear-gradient(#1d3b6b, #12284c); border: 1px solid #02060d; box-shadow: var(--bevel); border-radius: 2px; padding: 0.25rem 0.7rem; font-size: 0.88rem; }
.chips b { color: var(--gold); font-family: var(--font-mono); text-transform: uppercase; font-size: 0.85em; }
/* ---------- features: one panel each ---------- */
.feature { background: var(--surface); border: 1px solid var(--line); border-left: 3px solid var(--edge); border-radius: 2px; padding: 0.9rem 1.1rem 0.5rem; margin-top: 1rem; box-shadow: var(--shadow); }
.feature:hover { border-left-color: var(--gold); }
.feature h3 { margin-top: 0.1rem; }
.shots { display: grid; grid-template-columns: repeat(auto-fill, minmax(min(22rem, 100%), 1fr)); gap: 0.75rem; margin: 0.5rem 0 0.75rem; }
.shots figure.wide { grid-column: 1 / -1; }
.shots figure { margin: 0; }
.feature p { max-width: 50rem; margin: 0 0 0.6rem; }
.meta span { display: inline-block; min-width: 7.5rem; font: 700 0.74rem/1.3 var(--font-mono); text-transform: uppercase; letter-spacing: 0.08em; color: var(--cyan); }
/* ---------- the gallery ---------- */
.gallery figure { margin: 0 0 1.75rem; }
.gallery figcaption { display: flex; flex-wrap: wrap; gap: 0.25rem 0.75rem; align-items: baseline; margin-top: 0.55rem; }
.gallery figcaption b { font: 700 0.8rem/1.3 var(--font-mono); text-transform: uppercase; letter-spacing: 0.1em; color: var(--gold); }
.hidden { display: none !important; }
@media (max-width: 720px) {
  .two { grid-template-columns: minmax(0, 1fr); }
  /* the tables' rows as blocks: the key or setting, its choices, what it does, how it plays */
  .grid thead { display: none; }
  .grid, .grid tbody, .grid tr, .grid td, .grid tbody th { display: block; width: auto !important; white-space: normal; }
  .grid tr { border-top: 1px solid var(--line); padding: 0.55rem 0.8rem; }
  .grid tbody tr:first-child { border-top: 0; }
  .grid td, .grid tbody th { border-top: 0; padding: 0.1rem 0; }
  .grid td.k { font-weight: 700; }
  .grid td.plays:empty { display: none; }
  .grid td.plays::before { content: "How it plays: "; font-weight: 700; color: var(--cyan); }
  .mark { width: 1.3rem; height: 1.3rem; font-size: 0.75rem; }
  .feature { padding: 0.75rem 0.8rem 0.4rem; }
}
'''

SCRIPT = '''<script>
/* Find: shows only the rows, legend lines and feature cards that have every word typed (nothing is stored). */
(function () {
  var input = document.getElementById('find');
  var rows = Array.prototype.slice.call(document.querySelectorAll('.row'));
  input.addEventListener('input', function () {
    var words = input.value.toLowerCase().split(/\\s+/).filter(Boolean);
    rows.forEach(function (row) {
      var text = row.textContent.toLowerCase();
      var shown = words.every(function (word) { return text.indexOf(word) >= 0; });
      row.classList.toggle('hidden', !shown);
    });
  });
})();
</script>'''

if __name__ == '__main__':
    path = os.path.join(ROOT, 'pages', 'play.html')
    text = open(path).read()
    text = splice(text, '/* guide:css:begin */', '/* guide:css:end */', EXTRA_CSS.strip('\n'))
    text = splice(text, '<!-- guide:begin -->', '<!-- guide:end -->', sections())
    text = splice(text, '<!-- guide:script:begin -->', '<!-- guide:script:end -->', SCRIPT)
    open(path, 'w').write(text)
    print('updated', path, sum(len(g[1]) for g in KEYS), 'keys', sum(len(g[1]) for g in SETTINGS), 'settings', len(FEATURES), 'features')
