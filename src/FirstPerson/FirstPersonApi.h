#pragma once
/*
 * ==============================================================================================
 *  OpenXcom3D: a first-person 3D battlescape for OpenXcom Extended
 * ==============================================================================================
 *
 *  Our philosophy
 *
 *  - Faithful first. The 3D view should feel as if it had shipped with UFO: Enemy Unknown on
 *    the day the game came out.
 *  - Another camera on the same battle. The original rules decide everything: time units,
 *    energy, accuracy, line of fire, reaction fire, doors and morale. The 3D view only shows
 *    what the game does.
 *  - Only the original assets: the game's own maps, voxel shapes, sprites, palette and sounds.
 *  - Every addition is a setting. Jumping, night vision as gear, falling, FPS-style shooting
 *    and dual wield can all be switched off.
 *  - Light on the engine. Our code lives in this folder, the core only gets small hooks, and
 *    without the OpenXcom3D mod nothing changes.
 *  - Runs anywhere: a software renderer on the CPU, no graphics card needed.
 *  - Written for people: descriptive names, and a comment for every function and every block.
 *
 *  Date: 2026-10-19
 *
 *  We wish everyone all the best and all health!
 * ==============================================================================================
 */
/*
 * Copyright 2010-2026 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * FirstPersonApi.h: the OpenXcom3D first-person add-on as the rest of the engine sees it. This
 * is the only add-on header the core includes; each function below is called from one small
 * hook in a core file (named with it).
 *
 * Built with -DOXCE_FIRSTPERSON=OFF (which defines OXCE_NO_FIRSTPERSON), every call below is an
 * inline function that does nothing, and the add-on's sources are left out of the build: that's
 * stock OXCE. Built with it, the add-on still only switches on when an active mod declares
 * `firstPerson: enabled: true` (the OpenXcom3D mod does).
 */
#include <string>
#include <vector>
#include "../Menu/OptionsBaseState.h" // OptionsOrigin
#include "../Battlescape/Position.h"

namespace OpenXcom
{

class Action;
struct BattleAction;
class BattleItem;
class BattlescapeState;
class BattleUnit;
class FirstPersonMode;
class Game;
class Mod;
class OptionInfo;
class RuleItem;
class State;
class Surface;

namespace FirstPerson
{

/// Akimbo (dual wield): the share of its accuracy each gun keeps when both fire at once, a 30%
/// penalty (a player's report on 1.03; it was half). The game's shot and its pellets and the
/// other gun's round use it (ProjectileFlyBState), and the 3D view's spread and hit chance.
const double DualWieldAccuracy = 0.7;
/// Akimbo: two guns fired at once cost the dearer gun's shot and this share more (a player's report:
/// two identical guns cost twice one shot, at 30% less accuracy each; the owner's choice). The chosen
/// gun pays its own shot as the game charges it, the other gun the rest (dualWieldCharge). Guns that
/// fire in turn (an arcing one) are two shots at full accuracy, and pay in full.
const double DualWieldExtraCost = 0.1;
/// What the other gun pays when both fire, for one kind of cost (time units, energy): the pair's
/// cost (the dearer gun's and DualWieldExtraCost more, rounded) less what the chosen gun pays.
inline int dualWieldCharge(int chosen, int other)
{
	const int pair = (int)((chosen > other ? chosen : other) * (1.0 + DualWieldExtraCost) + 0.5);
	return pair > chosen ? pair - chosen : 0;
}

#ifndef OXCE_NO_FIRSTPERSON

// ---- The battlescape screen (BattlescapeState.cpp) ----

/// Creates the first-person mode for a battlescape screen, or returns nullptr unless an active
/// mod enables it. Called when the screen is made.
FirstPersonMode *create(Game *game, BattlescapeState *state);
/// Destroys it. Called when the screen goes away.
void destroy(FirstPersonMode *mode);
/// The per-frame update: camera, movement, HUD. Called from the screen's think().
void think(FirstPersonMode *mode);
/// Gets the first look at an event (keys, mouse); returns true if it used it, and the screen
/// then leaves it alone. Called from the screen's handle().
bool handle(FirstPersonMode *mode, Action *action);
/// Draws the battlescape screen its own way (our overlays are magnified from a canvas, and the
/// classic HUD may be hidden); returns false when there is no first-person mode, and the screen
/// then blits as usual. Called from the screen's blit().
bool blit(FirstPersonMode *mode);
/// Should the mouse be captured for mouse look right now? Asked by the screen each frame.
bool wantsMouseCapture(const FirstPersonMode *mode);
/// The screen was resized by (dX, dY) and moved every surface like its own bottom-centred HUD:
/// puts ours where they belong. Called from the screen's resize().
void resized(FirstPersonMode *mode, int widthChange, int heightChange);

// ---- Options (Options.cpp, OptionsBaseState.cpp) ----

/// Registers the add-on's options (the fps* settings and their keys). Called while the game
/// builds its list of options.
void createOptions(std::vector<OptionInfo> &options);
/// Should the options screen offer the 3D VIEW tab (only when an active mod enables the mode)?
bool hasOptionsTab(const Mod *mod);
/// Creates the 3D VIEW options tab. Called when the player clicks it.
State *createOptionsTab(OptionsOrigin origin);
/// Settles settings that take their first value from an older one (the smoke from "See-through
/// smoke", the flashlight from 1.0's two flashlight settings, the next fire mode key from 1.0's).
void settleOptions();
/// Does a mod the game finds for the first time (one not yet in the player's mod list) start
/// switched on? Our own mod does, so a game set up by hand plays in 3D at once; switched off, it
/// stays off.
bool activeWhenNew(const std::string &modId);
/// The smoke setting as it stands: 1 Original, 2 Modern (unset, or 1.0's Solid: see the function).
int smokeMode();
/// The flashlight setting as it stands: 0 Off, 1 On, 2 Dynamic (unset: from 1.0's settings).
int flashlightMode();
/// The longest Doom grenade fuse, in seconds (setting "Doom grenade fuse": 1 to this).
const int DoomFuseMaxSeconds = 5;

// ---- The battle (ProjectileFlyBState.cpp, ExplosionBState.cpp, BattlescapeGame.cpp) ----

/// An FPS-style shot's next round ("Faithful X-COM targeting" off): where the first-person view
/// aims it now (along the crosshair, with a spread and the recoil so far). The game's own
/// projectile code asks this for each round, and then traces the round with its usual line of
/// fire.
void freeAimTarget(BattlescapeState *state, const BattleAction &action, Position &target);
/// Is FPS-style shooting on for this unit's shots in the first-person view? Their bullet hits
/// then play quicker (a burst at a bullet's pace).
bool fastShots(BattlescapeState *state, const BattleUnit *attacker);
/// Setting "Shotgun pellets in flight": do a shotgun's pellets fly beside its round (in 3D and on
/// the map), rather than all landing at once? They're traced as the round is fired, and hit as it lands.
bool pelletsInFlight(BattlescapeState *state);
/// Setting "Each pellet on its own": do a shotgun's pellets and a volley's two rounds each land on
/// their own (in either view), none lost when the lead pellet leaves the map?
bool eachPellet(BattlescapeState *state);
/// Akimbo on the map (Ctrl and Shift held, a click on a hand's weapon): steps the soldier's two
/// guns through firing together on the map, both at once, in turn, and not. True if it took the click.
bool toggleMapAkimbo(BattlescapeState *state, BattleUnit *unit);
/// Akimbo on the map: its mini targets in both hands' boxes (one each both at once, two in turn),
/// drawn after the game draws the hands; undone once the two guns no longer pair up.
void drawMapAkimbo(BattlescapeState *state, BattleUnit *unit, Surface *leftHand, Surface *rightHand);
/// A shot fired on the map, as it's launched: with akimbo marked there, the other hand's gun
/// fires along, as in 3D (not while the 3D view shows: its shots bring their own).
void mapAkimbo(BattlescapeState *state, BattleAction &action);
/// A shotgun's pellet landed (ProjectileFlyBState::pelletLands), in either view: it's heard (a few
/// to a volley; not one whose hit the explosion state brings, which sounds its own) and the 3D view
/// marks where it struck (sparks, a bullet hole), as it does the round.
void pelletLanded(BattlescapeState *state, const Position &at, const Position &from, int impact, const BattleItem *weapon, const BattleItem *ammo, bool heard = true);
/// The bullet sprite for shotgun ammo that has none (Projectile): the one the mod's other ammo of
/// its damage type uses most, so the map can draw its round and pellets in flight.
int shotgunBulletSprite(const Mod *mod, const RuleItem *ammo);
/// A unit panics or goes berserk: returns true if the first-person view shows it its own way
/// (the game then leaves out its "has panicked" box, which would freeze the view behind it).
bool breakdown(BattlescapeState *state, BattleUnit *unit);
/// A close-quarters check failed: the shooter's shot is knocked aside at this tile, and the game
/// has turned the shooter to face it. The first-person view looks there too, so the soldier
/// isn't turned back to the old view afterwards (at a cost, with "Turning costs TU").
void closeQuartersDeflected(BattlescapeState *state, const BattleUnit *shooter, const Position &target);

// ---- Items and units (BattleItem.cpp, BattleUnit.cpp) ----

/// Doom IDKFA: does a shot by this unit spend no ammo (the selected soldier, on the player's
/// turn, in the 3D view)? Asked whenever a weapon would spend ammo, beside the game's own debug
/// mode, which spends none either.
bool endlessAmmo(BattlescapeState *state, const BattleUnit *unit);
/// Doom IDDQD: does this unit take no damage at all (X-COM's units)? Asked whenever a unit is
/// about to be hurt.
bool invincible(BattlescapeState *state, const BattleUnit *unit);

#else

// Stock OXCE: the add-on is left out, and every hook does nothing.
inline FirstPersonMode *create(Game *, BattlescapeState *) { return nullptr; }
inline void destroy(FirstPersonMode *) { }
inline void think(FirstPersonMode *) { }
inline bool handle(FirstPersonMode *, Action *) { return false; }
inline bool blit(FirstPersonMode *) { return false; }
inline bool wantsMouseCapture(const FirstPersonMode *) { return false; }
inline void resized(FirstPersonMode *, int, int) { }
inline void createOptions(std::vector<OptionInfo> &) { }
inline bool hasOptionsTab(const Mod *) { return false; }
inline State *createOptionsTab(OptionsOrigin) { return nullptr; }
inline bool activeWhenNew(const std::string &) { return false; }
inline void freeAimTarget(BattlescapeState *, const BattleAction &, Position &) { }
inline bool fastShots(BattlescapeState *, const BattleUnit *) { return false; }
inline bool pelletsInFlight(BattlescapeState *) { return false; }
inline bool eachPellet(BattlescapeState *) { return false; }
inline bool toggleMapAkimbo(BattlescapeState *, BattleUnit *) { return false; }
inline void drawMapAkimbo(BattlescapeState *, BattleUnit *, Surface *, Surface *) { }
inline void mapAkimbo(BattlescapeState *, BattleAction &) { }
inline void pelletLanded(BattlescapeState *, const Position &, const Position &, int, const BattleItem *, const BattleItem *, bool = true) { }
inline int shotgunBulletSprite(const Mod *, const RuleItem *) { return -1; } // (Mod::NO_SURFACE)
inline bool breakdown(BattlescapeState *, BattleUnit *) { return false; }
inline void closeQuartersDeflected(BattlescapeState *, const BattleUnit *, const Position &) { }
inline bool endlessAmmo(BattlescapeState *, const BattleUnit *) { return false; }
inline bool invincible(BattlescapeState *, const BattleUnit *) { return false; }

#endif

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
