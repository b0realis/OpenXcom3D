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
#include <vector>
#include "../Menu/OptionsBaseState.h" // OptionsOrigin
#include "../Battlescape/Position.h"

namespace OpenXcom
{

class Action;
struct BattleAction;
class BattlescapeState;
class BattleUnit;
class FirstPersonMode;
class Game;
class Mod;
class OptionInfo;
class State;

namespace FirstPerson
{

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

// ---- The battle (ProjectileFlyBState.cpp, ExplosionBState.cpp, BattlescapeGame.cpp) ----

/// An FPS-style shot's next round ("Faithful X-COM targeting" off): where the first-person view
/// aims it now (along the crosshair, with a spread and the recoil so far). The game's own
/// projectile code asks this for each round, and then traces the round with its usual line of
/// fire.
void freeAimTarget(BattlescapeState *state, const BattleAction &action, Position &target);
/// Is FPS-style shooting on for this unit's shots in the first-person view? Their bullet hits
/// then play quicker (a burst at a bullet's pace).
bool fastShots(BattlescapeState *state, const BattleUnit *attacker);
/// A unit panics or goes berserk: returns true if the first-person view shows it its own way
/// (the game then leaves out its "has panicked" box, which would freeze the view behind it).
bool breakdown(BattlescapeState *state, BattleUnit *unit);

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
inline void freeAimTarget(BattlescapeState *, const BattleAction &, Position &) { }
inline bool fastShots(BattlescapeState *, const BattleUnit *) { return false; }
inline bool breakdown(BattlescapeState *, BattleUnit *) { return false; }

#endif

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
