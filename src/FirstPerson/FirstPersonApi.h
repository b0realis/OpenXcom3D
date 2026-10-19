#pragma once
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
 * OpenXcom3D first-person add-on: the only header the rest of the engine includes.
 * Built with -DOXCE_FIRSTPERSON=OFF (which defines OXCE_NO_FIRSTPERSON) every call
 * below is an inline no-op and the add-on's sources are left out of the build.
 * Built with it, the add-on still only switches on when an active mod declares
 * `firstPerson: enabled: true`.
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

/// Creates the first-person mode for a battlescape screen; nullptr unless an active mod enables it.
FirstPersonMode *create(Game *game, BattlescapeState *state);
/// Destroys it.
void destroy(FirstPersonMode *mode);
/// Per-frame update.
void think(FirstPersonMode *mode);
/// Gets the first look at an event; true if consumed.
bool handle(FirstPersonMode *mode, Action *action);
/// Blits the battlescape screen its own way; false to blit normally.
bool blit(FirstPersonMode *mode);
/// Should the mouse be captured for mouse look?
bool wantsMouseCapture(const FirstPersonMode *mode);
/// The battlescape screen was resized by (dX, dY) and moved every surface like its HUD: put ours right.
void resized(FirstPersonMode *mode, int dX, int dY);
/// Registers the add-on's options (called from Options' fork hook).
void createOptions(std::vector<OptionInfo> &info);
/// Should the options screen offer the 3D VIEW tab?
bool hasOptionsTab(const Mod *mod);
/// Creates the 3D VIEW options tab.
State *createOptionsTab(OptionsOrigin origin);
/// An FPS-style shot's next round: where the first-person view aims it now (crosshair, spread, recoil).
void freeAimTarget(BattlescapeState *state, const BattleAction &action, Position &target);
/// FPS-style shooting is on for this unit's shots (first-person view): their bullet hits play quicker.
bool fastShots(BattlescapeState *state, const BattleUnit *attacker);
/// A unit panics or goes berserk: true if the first-person view shows it (the game then leaves its infobox out).
bool breakdown(BattlescapeState *state, BattleUnit *unit);

#else

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
