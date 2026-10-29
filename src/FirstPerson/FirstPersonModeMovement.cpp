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
 * FirstPersonModeMovement.cpp: moving the soldier in real time.
 *
 * Walking, with every tile entered paid for by the original rules (the game's own walking
 * state); opening doors and walking through them; jumping, and vaulting over low obstacles;
 * riding grav lifts; flying; falling off ledges; and stopping when a new enemy is spotted.
 */
#include "FirstPersonMode.h"
#include <algorithm>
#include <cmath>
#include <string>
#include "FirstPersonView.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/Pathfinding.h"
#include "../Battlescape/TileEngine.h"
#include "../Battlescape/UnitTurnBState.h"
#include "../Battlescape/UnitWalkBState.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/RNG.h"
#include "../Engine/Sound.h"
#include "../Interface/Text.h"
#include "../Mod/Armor.h"
#include "../Mod/MapData.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleDamageType.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "FirstPersonModeShared.h"

namespace OpenXcom
{

/**
 * Keeps one coordinate of the camera inside a tile, at least EdgeMargin voxels away from the
 * tile's two edges on that axis. Works on one axis (x or y) at a time.
 * @param tileCoordinate The tile's coordinate on this axis, in tiles (a tile is 16 voxels across).
 * @param voxelCoordinate The camera's coordinate on this axis, in voxels.
 * @param sizeOffset 0 for a normal soldier. 8 for a big (2x2) unit: its position is its north-west
 *   tile, but its eye sits in the middle of its four tiles, 8 voxels further along.
 * @return The coordinate, moved back inside the tile if it was too close to an edge or past it.
 */
double FirstPersonMode::clampInsideTile(int tileCoordinate, double voxelCoordinate, int sizeOffset) const
{
	return std::clamp(voxelCoordinate, tileCoordinate * 16.0 + EdgeMargin + sizeOffset, tileCoordinate * 16.0 + 16.0 - EdgeMargin + sizeOffset);
}

/**
 * Could the unit step from the camera's tile into the neighbouring tile in this direction right
 * now, by the original rules? The game's pathfinding prices the step in time units and energy
 * (walls, objects, corners cut on a diagonal, stairs, the armor's movement costs); the soldier
 * must have both, and keep the time units the player reserved for a shot (the classic HUD's
 * "reserve TU" buttons). With flight switched off, or the setting "Falling" off, a step off a
 * ledge into the air isn't allowed either; with "Check the landing", nor one onto an unsafe landing
 * (see landingSafe) until the player has pushed that way again after its warning.
 * The movement code asks about the same directions several times a frame, so each answer is
 * kept in _stepAllowedCache until the cache is cleared (every frame, and after anything that
 * changes the answers, such as a step).
 * @param stepX The step's direction along x (east is positive): -1, 0 or 1.
 * @param stepY The step's direction along y (south is positive): -1, 0 or 1.
 * @return True if the step may be taken.
 */
bool FirstPersonMode::stepAllowed(BattleUnit *unit, int stepX, int stepY)
{
	// The game numbers the eight directions from 0 (north) to 7, clockwise. Anything else (no
	// movement at all, or more than one tile) isn't a step.
	const int direction = Pathfinding::vectorToDirection(Position(stepX, stepY, 0));
	if (direction < 0 || direction > 7)
	{
		return false;
	}

	// Work the answer out only once per direction.
	if (_stepAllowedCache[direction] < 0)
	{
		// What the pathfinding charges for this one step, from the camera's tile on the unit's
		// level, walked or (sprinting) run. A step that can't be made at all costs
		// INVALID_MOVE_COST time units.
		const Position start(_trackedTile.x, _trackedTile.y, unit->getPosition().z);
		const PathfindingStep stepCost = _save->getPathfinding()->getTUCost(start, direction, unit, nullptr,
			runsStep(unit, start, direction) ? BAM_RUN : BAM_NORMAL);
		bool allowed = stepCost.cost.time < Pathfinding::INVALID_MOVE_COST;

		// Flight switched off (or falling switched off): don't step off a ledge into the air.
		// The destination must have a floor, or the tile below it must be the top of a ramp or
		// of stairs going down: its ground raised at least half a level (the terrain level is
		// negative for raised ground, and a level is 24 voxels high), which is ground to walk on.
		if (allowed && ((canFly(unit) && !flightOn(unit)) || !Options::fpsFalling))
		{
			const Tile *destination = _save->getTile(start + Position(stepX, stepY, 0));
			const Tile *belowDestination = destination ? _save->getBelowTile(destination) : nullptr;
			allowed = !destination || !destination->hasNoFloor(_save)
				|| (belowDestination && !belowDestination->hasNoFloor(_save) && belowDestination->getTerrainLevel() <= -12);
		}

		// Falling, a step off a ledge (into a tile with no floor, not the top of stairs or a ramp
		// going down, and not flying) must land somewhere safe (setting "Check the landing", a
		// player's report: X-Com Files' blood moat soft-locked a soldier), or the player must have
		// pushed that way again after the warning (see pushedIntoUnsafeLanding).
		bool unsafe = false;
		if (allowed && Options::fpsFalling && Options::fpsSafeLanding && !(canFly(unit) && flightOn(unit)))
		{
			const Position ahead = start + Position(stepX, stepY, 0);
			const Tile *aheadTile = _save->getTile(ahead);
			const Tile *belowAhead = aheadTile ? _save->getBelowTile(aheadTile) : nullptr;
			const bool ledge = aheadTile && aheadTile->hasNoFloor(_save) && !(belowAhead && !belowAhead->hasNoFloor(_save) && belowAhead->getTerrainLevel() <= -12);
			const bool confirmed = _unsafeConfirmed && _unsafeDirection == direction && _unsafeFrom == start && SDL_GetTicks() - _unsafeWarnedAt < UnsafeLandingTime;
			unsafe = ledge && !confirmed && !landingSafe(unit, ahead);
			allowed = !unsafe;
		}

		// The soldier must be able to pay for it: enough time units and energy, with enough time
		// units left for the shot the player reserved (see reserveAllows). The debug "doom mode"
		// (endless time units, energy and ammo) skips this.
		if (allowed && !doomModeOn())
		{
			allowed = unit->getTimeUnits() >= stepCost.cost.time && unit->getEnergy() >= stepCost.cost.energy
				&& reserveAllows(unit, stepCost.cost.time, stepCost.cost.energy);
		}
		_stepAllowedCache[direction] = allowed ? 1 : unsafe ? 2 : 0;
	}
	return _stepAllowedCache[direction] == 1;
}

/**
 * Is the landing of a step off a ledge safe (setting "Check the landing")? The soldier falls
 * straight down from the tile ahead to the first with a floor (the game's own fall). It's unsafe
 * where no one can walk (a floor or an object the rules give no walking cost: TFTD's sea, X-Com
 * Files' blood moat, which the map never lets a soldier into) or where not a single step leads on
 * (eight ways round, and up or down a lift): the soldier would be stuck there for good.
 * @param unit The soldier.
 * @param ahead The tile the step goes into, on the soldier's level.
 * @return True if it's safe.
 */
bool FirstPersonMode::landingSafe(BattleUnit *unit, const Position &ahead) const
{
	// Where the fall ends.
	Position landing = ahead;
	while (landing.z > 0)
	{
		const Tile *tile = _save->getTile(landing);
		if (tile && !tile->hasNoFloor(_save))
		{
			break;
		}
		--landing.z;
	}
	const Tile *landingTile = _save->getTile(landing);
	if (!landingTile)
	{
		return true;
	}
	// Ground to stand on.
	if (landingTile->getTUCost(O_FLOOR, MT_WALK) >= Pathfinding::INVALID_MOVE_COST || landingTile->getTUCost(O_OBJECT, MT_WALK) >= Pathfinding::INVALID_MOVE_COST)
	{
		return false;
	}
	// A step leading on from it.
	Pathfinding *pathfinding = _save->getPathfinding();
	for (int direction = 0; direction < 10; ++direction)
	{
		if (pathfinding->getTUCost(landing, direction, unit, nullptr, BAM_NORMAL).cost.time < Pathfinding::INVALID_MOVE_COST)
		{
			return true;
		}
	}
	return false;
}

/**
 * The player pushed into a step off a ledge that stepAllowed refused for an unsafe landing
 * (setting "Check the landing"). The first push warns ("UNSAFE LANDING! PUSH AGAIN"); pushing that
 * way again within UnsafeLandingTime, after letting the keys go, or with a jump (deliberate), goes
 * over (the owner: "Another jump then just jumps"). Later, or from elsewhere, it warns afresh.
 * @param direction The step's direction (0 north .. 7, clockwise).
 * @param deliberate A jump (Space while pushing), rather than a push of the movement keys.
 */
void FirstPersonMode::pushedIntoUnsafeLanding(int direction, bool deliberate)
{
	const Uint32 now = SDL_GetTicks();
	if (_unsafeDirection == direction && _unsafeFrom == _trackedTile && now - _unsafeWarnedAt < UnsafeLandingTime)
	{
		if ((_unsafeReleased || deliberate) && !_unsafeConfirmed)
		{
			_unsafeConfirmed = true; // (stepAllowed lets it through from now on)
			std::fill(std::begin(_stepAllowedCache), std::end(_stepAllowedCache), -1);
		}
		return;
	}
	_state->warningLongRaw(_state->tr("STR_FPS_UNSAFE_LANDING")); // (the game's longer warning: read before it fades)
	_unsafeFrom = _trackedTile;
	_unsafeDirection = direction;
	_unsafeWarnedAt = now;
	_unsafeReleased = _unsafeConfirmed = false;
}

/**
 * Is this step the game's run (as Ctrl moves on the map), not a walk? Only while sprinting is
 * switched on (its key), with an armor that allows running (most do; a large unit's only when
 * its rules say so), and when the soldier can pay for the run: its time units and energy, and
 * the time units reserved for a shot. A soldier out of breath walks the step instead.
 * @param unit The soldier.
 * @param from The tile the step starts from.
 * @param direction The step's direction (0 north .. 7, clockwise).
 * @return True to run it.
 */
bool FirstPersonMode::runsStep(BattleUnit *unit, const Position &from, int direction) const
{
	if (!_sprintOn || !unit->getArmor()->allowsRunning(unit->isSmallUnit()))
	{
		return false;
	}
	const PathfindingStep runCost = _save->getPathfinding()->getTUCost(from, direction, unit, nullptr, BAM_RUN);
	if (runCost.cost.time >= Pathfinding::INVALID_MOVE_COST)
	{
		return false;
	}
	return doomModeOn() || (unit->getTimeUnits() >= runCost.cost.time && unit->getEnergy() >= runCost.cost.energy
		&& reserveAllows(unit, runCost.cost.time, runCost.cost.energy));
}

/**
 * Commits one step of the unit into the neighbouring tile through the game's own walking state
 * (UnitWalkBState), the same one an order on the isometric map uses, so every original rule
 * applies: the time units and energy are paid, doors open, stairs are climbed, mines go off,
 * units on both sides spot each other, reaction fire comes, and a soldier who steps into the
 * air falls. The walk is an "instant move" (no step animation on the map); the 3D view moves
 * the camera smoothly on its own.
 * The camera's tile becomes the destination at once, and the step stays pending until the game
 * has walked it (see updateCamera).
 * @param stepX The step's direction along x (east is positive): -1, 0 or 1.
 * @param stepY The step's direction along y (south is positive): -1, 0 or 1.
 * @return False if the step couldn't be started (the pathfinding wanted to go another way).
 */
bool FirstPersonMode::commitStep(BattleUnit *unit, int stepX, int stepY)
{
	// Ask the pathfinding for the way to the tile the game's own step that way ends on: the
	// neighbouring tile, or on stairs or a ramp the one a level up (or down), as the isometric
	// map's cursor would be put on it. (Asked for the neighbouring tile on the unit's own level,
	// the pathfinding only goes up a level for stairs shaped as the original games' are, so the
	// 3D view couldn't climb others, a mod's craft say, that the map climbs.) What we want is the
	// direct step; or, over a ledge, the step and the fall straight down after it.
	const Position from(_trackedTile.x, _trackedTile.y, unit->getPosition().z);
	const Position levelTarget(from.x + stepX, from.y + stepY, from.z); // the neighbouring tile on this level
	const int direction = Pathfinding::vectorToDirection(Position(stepX, stepY, 0));
	Pathfinding *pathfinding = _save->getPathfinding();
	const PathfindingStep gameStep = pathfinding->getTUCost(from, direction, unit, nullptr, BAM_NORMAL);
	const Position target = gameStep.cost.time < Pathfinding::INVALID_MOVE_COST ? gameStep.pos : levelTarget;
	// Turning by the rules (setting "Turning costs TU"): a step to the side or half to the side
	// is the game's own sidestep (its strafe, as Ctrl walks on the map: the soldier keeps facing
	// the way it faces, for one TU more than the step), not a turn to face the step and a paid
	// turn back. Steps further round turn as the game turns. With free turning the game turns
	// the soldier for the step, for free, and it faces the view again after (faceLookDirection).
	// Sprinting, any other step is the game's run (see runsStep); a sidestep stays one.
	const int offFacing = std::min((direction - unit->getDirection() + 8) % 8, (unit->getDirection() - direction + 8) % 8);
	const bool sidestep = Options::fpsTurnCost && offFacing >= 1 && offFacing <= 2 && target.z == from.z;
	const bool run = !sidestep && runsStep(unit, from, direction);
	if (_sprintOn && !sidestep)
	{
		_sprintStepRan = run; // (a step the soldier couldn't run sets the walking pace)
	}
	pathfinding->calculate(unit, target, sidestep ? BAM_STRAFE : run ? BAM_RUN : BAM_NORMAL);
	const std::vector<int> &path = pathfinding->getPath();

	// Over a ledge the search never plans the step into the air: it looks for the way to the
	// ground below instead, which is no way at all, or a walk round by the stairs when there are
	// some. Either way the step is set by hand, and the game's own falling takes over once the
	// soldier is out there (setting "Falling"; a flying soldier with flight on doesn't fall).
	// Otherwise the path must be the direct step: if the pathfinding prefers a detour (or found
	// no way), we don't take it. A step down stairs is never a fall: the tile ahead has no floor,
	// but the one below it is the top of the stairs, raised at least half a level (see
	// stepAllowed), and the game walks it as one step.
	const Tile *aheadTile = _save->getTile(levelTarget);
	const Tile *belowAheadTile = aheadTile ? _save->getBelowTile(aheadTile) : nullptr;
	const bool stairsDown = belowAheadTile && !belowAheadTile->hasNoFloor(_save) && belowAheadTile->getTerrainLevel() <= -12;
	const bool stepsOffLedge = path.size() != 1 && Options::fpsFalling && aheadTile && aheadTile->hasNoFloor(_save)
		&& !stairsDown && !(canFly(unit) && flightOn(unit));
	if (stepsOffLedge)
	{
		pathfinding->setPath(unit, { direction });
	}
	else if (path.size() != 1)
	{
		pathfinding->abortPath();
		return false;
	}
	_fallFromZ = stepsOffLedge ? from.z : -1; // The level the fall starts from, for the damage on landing.
	_unsafeDirection = -1; // (an unsafe landing's go-ahead is used, or no longer needed)
	_unsafeConfirmed = false;

	// Hand the step to the game's walking state.
	BattleAction walk;
	walk.type = BA_WALK;
	walk.actor = unit;
	walk.target = target;
	walk.instantMove = true; // No step animation: the 3D view moves smoothly on its own.
	walk.strafe = pathfinding->getStrafeMove(); // the sidestep above, if the game allows it here
	walk.run = run && !walk.strafe;
	// Walking on past an enemy coming into sight: en passant, or a run where the mod's rules say
	// running does (as the map decides it for a move with Shift or a run).
	walk.ignoreSpottedEnemies = _enPassantOn || (walk.run && Mod::EXTENDED_RUNNING_COST);
	_sidestepFacing = walk.strafe ? unit->getDirection() : -1;
	_state->_battleGame->statePushBack(new UnitWalkBState(_state->_battleGame, walk));

	// The camera's tile is the destination from now on. The step is pending until the game has
	// walked it, and the cached step answers no longer hold.
	_stepPending = true;
	_stepFrom = from;
	_trackedTile = Position(target.x, target.y, from.z);
	std::fill(std::begin(_stepAllowedCache), std::end(_stepAllowedCache), -1);

	// Stepping down onto a ramp or stairs (which stepAllowed lets through with flight off), a
	// flying suit doesn't walk down them: it floats off the edge into the air. With flight off,
	// it then comes down to land (see updateFlight).
	if (canFly(unit) && !flightOn(unit))
	{
		const Tile *destination = _save->getTile(levelTarget);
		if (destination && destination->hasNoFloor(_save))
		{
			_flightSwitchedOn[unit] = false;
			_landAfterStep = true;
		}
	}
	return true;
}

/**
 * Is there a closed door in the wall between the camera's tile and its neighbour in this
 * direction? Only a straight step goes through a door wall; a diagonal never counts. A normal
 * door the game has opened turns into its open version, which isn't a door any more; a UFO door
 * stays a door, and counts only while it's shut (not opening or open).
 * @param stepX The direction along x (east is positive): -1, 0 or 1.
 * @param stepY The direction along y (south is positive): -1, 0 or 1.
 * @param z The level (in tiles) to look on.
 */
bool FirstPersonMode::closedDoorTowards(int stepX, int stepY, int z) const
{
	if (stepX != 0 && stepY != 0)
	{
		return false;
	}

	// The wall between the two tiles: a tile owns its west and north walls, so going east or
	// south it belongs to the neighbour, going west or north to the camera's own tile.
	const Position wallOwner(_trackedTile.x + std::max(stepX, 0), _trackedTile.y + std::max(stepY, 0), z);
	const Tile *tile = _save->getTile(wallOwner);
	if (!tile)
	{
		return false;
	}
	const TilePart wallPart = stepX != 0 ? O_WESTWALL : O_NORTHWALL;
	const MapData *mapData = tile->getMapData(wallPart);
	return mapData && (mapData->isDoor() || (mapData->isUFODoor() && !tile->isUfoDoorOpen(wallPart)));
}

/**
 * A diagonal gap: two units (or two crates) standing corner to corner, with the way between them
 * open. The game lets a soldier step through it diagonally (only walls block a diagonal step),
 * but the camera is kept EdgeMargin voxels away from both blocked sides of its tile, so it never
 * gets to the corner to cross it. Pushing into such a corner takes the diagonal step, and
 * walkGuidedStep moves the camera through the gap.
 * @param newEyeX The camera's x this frame, in voxels, before it is kept off the tile's edges.
 * @param newEyeY The camera's y this frame, in voxels, before it is kept off the tile's edges.
 * @param wantedVelocityX The velocity the movement keys ask for along x, in voxels a second.
 * @param wantedVelocityY The velocity the movement keys ask for along y, in voxels a second.
 * @param sidesMayBeOpen A diagonal tile move: the diagonal step is taken here whether or not the
 *        sides are open, so it is one step and not two around the corner.
 * @return True if the step through the gap started.
 */
bool FirstPersonMode::stepThroughDiagonalGap(BattleUnit *unit, double newEyeX, double newEyeY, double wantedVelocityX, double wantedVelocityY, bool sidesMayBeOpen)
{
	// Pushing towards a corner: each axis has at least a quarter of the push (so between about
	// 15 and 75 degrees off the axes). Which corner: east or west, south or north.
	const double push = std::sqrt(wantedVelocityX * wantedVelocityX + wantedVelocityY * wantedVelocityY);
	if (push == 0.0 || std::abs(wantedVelocityX) < push * 0.25 || std::abs(wantedVelocityY) < push * 0.25)
	{
		return false;
	}
	const int stepX = wantedVelocityX > 0.0 ? 1 : -1, stepY = wantedVelocityY > 0.0 ? 1 : -1;

	// Up against both sides of that corner: no further than EdgeMargin from either edge (half a
	// voxel to spare). A big unit's eye sits 8 voxels off its tile's middle (see clampInsideTile).
	const int sizeOffset = 8 * (unit->getArmor()->getSize() - 1);
	const double westEdgeX = _trackedTile.x * 16.0 + sizeOffset, northEdgeY = _trackedTile.y * 16.0 + sizeOffset;
	const double toEdgeX = stepX > 0 ? westEdgeX + 16.0 - newEyeX : newEyeX - westEdgeX;
	const double toEdgeY = stepY > 0 ? northEdgeY + 16.0 - newEyeY : newEyeY - northEdgeY;
	if (toEdgeX > EdgeMargin + 0.5 || toEdgeY > EdgeMargin + 0.5)
	{
		return false;
	}

	// Both sides blocked (unless it's a diagonal tile move) and the diagonal open. (With a side
	// open, the camera crosses that edge as usual, and the diagonal is the next step from there.)
	if ((!sidesMayBeOpen && (stepAllowed(unit, stepX, 0) || stepAllowed(unit, 0, stepY))) || !stepAllowed(unit, stepX, stepY)
		|| !commitStep(unit, stepX, stepY))
	{
		return false;
	}
	_guidedStep = true;
	_guidedStepDoor = false;
	_guidedStepDirection[0] = stepX;
	_guidedStepDirection[1] = stepY;
	_guidedStepStart = SDL_GetTicks();
	return true;
}

/**
 * Moves the camera through a step that takes it on its own: through a door (see updateMovement,
 * which starts the step when the soldier walks into a closed door) or through a diagonal gap
 * (see stepThroughDiagonalGap). The game's walking state opens a door as part of the step;
 * meanwhile the camera waits in front of it while it swings or slides open in the 3D view. Then
 * the camera walks on, straight through the doorway or the gap, until it is well inside the new
 * tile, and hands back to the movement keys.
 * @param newEyeX Set to the camera's x for this frame, in voxels (when returning true).
 * @param newEyeY Set to the camera's y for this frame, in voxels (when returning true).
 * @param elapsedSeconds Seconds since the last frame.
 * @return True while the step moves the camera; false when there's no such step going on (any
 *   more).
 */
bool FirstPersonMode::walkGuidedStep(BattleUnit *unit, double &newEyeX, double &newEyeY, double elapsedSeconds)
{
	if (!_guidedStep)
	{
		return false;
	}

	// Is the unit still on the tile it steps from (the game hasn't moved it yet)? And how far a
	// big unit's eye sits from the middle of its tile (see clampInsideTile).
	const Position unitTile = unit->getPosition();
	const bool stillHere = unitTile.x == _stepFrom.x && unitTile.y == _stepFrom.y;
	const int sizeOffset = 8 * (unit->getArmor()->getSize() - 1);

	// Wait in the old tile while the game hasn't moved the unit yet, and while a door is still
	// opening in the 3D view: at least DoorWait ms, and at most 3 seconds in all, in case the door
	// never reports that it stopped. The wall the door is in belongs to the tile further east or
	// south (a tile owns its west and north walls).
	bool waiting = false;
	if (_guidedStepDoor)
	{
		const Position wall(_stepFrom.x + std::max(_guidedStepDirection[0], 0), _stepFrom.y + std::max(_guidedStepDirection[1], 0), _stepFrom.z);
		const int wallPart = _guidedStepDirection[0] != 0 ? O_WESTWALL : O_NORTHWALL;
		waiting = (SDL_GetTicks() - _guidedStepStart < DoorWait || _view->isDoorMoving(wall, wallPart)) && SDL_GetTicks() - _guidedStepStart < 3000;
	}
	if ((_stepPending && stillHere) || waiting)
	{
		newEyeX = clampInsideTile(_stepFrom.x, _eyeX, sizeOffset);
		newEyeY = clampInsideTile(_stepFrom.y, _eyeY, sizeOffset);
		return true;
	}

	// Where the camera is now: in which tile, and whether it is at least EdgeMargin voxels inside
	// the new tile on the axes it walks along.
	const int cameraX = (int)std::floor((_eyeX - sizeOffset) / 16.0), cameraY = (int)std::floor((_eyeY - sizeOffset) / 16.0);
	const bool inside =
		(_guidedStepDirection[0] == 0
			|| (_eyeX - sizeOffset - _trackedTile.x * 16.0 >= EdgeMargin && _trackedTile.x * 16.0 + 16.0 - (_eyeX - sizeOffset) >= EdgeMargin))
		&& (_guidedStepDirection[1] == 0
			|| (_eyeY - sizeOffset - _trackedTile.y * 16.0 >= EdgeMargin && _trackedTile.y * 16.0 + 16.0 - (_eyeY - sizeOffset) >= EdgeMargin));

	// The unit has moved on: walk through, straight on, at the walking speed setting (1 to 8
	// tiles a second; a diagonal no faster), kept away from a door's frame on the other axis.
	if (!stillHere && (cameraX != _trackedTile.x || cameraY != _trackedTile.y || !inside))
	{
		const double diagonal = (_guidedStepDirection[0] != 0 && _guidedStepDirection[1] != 0) ? std::sqrt(0.5) : 1.0;
		const double distance = std::clamp(Options::fpsWalkSpeed, 1, 8) * 16.0 * elapsedSeconds * diagonal;
		newEyeX = _eyeX + _guidedStepDirection[0] * distance;
		newEyeY = _eyeY + _guidedStepDirection[1] * distance;
		if (_guidedStepDirection[0] == 0) newEyeX = clampInsideTile(_trackedTile.x, newEyeX, sizeOffset);
		if (_guidedStepDirection[1] == 0) newEyeY = clampInsideTile(_trackedTile.y, newEyeY, sizeOffset);
		// Never past the new tile: no further than EdgeMargin short of its far side on the axes it
		// walks along, so the step always ends inside it. (A slow frame, a tenth of a second, moves
		// the camera 6 to 13 voxels, and the stretch where it counts as inside is 6 wide: jumped
		// over, the camera walked on for ever, out of the map, while on the map the soldier stood
		// in the new tile: a player's report, "walks into a door backwards or sideways and appears
		// to walk infinitely out of bounds". One that got past anyway comes back to it.)
		const double tileWest = _trackedTile.x * 16.0 + sizeOffset, tileNorth = _trackedTile.y * 16.0 + sizeOffset;
		if (_guidedStepDirection[0] > 0) newEyeX = std::min(newEyeX, tileWest + 16.0 - EdgeMargin);
		if (_guidedStepDirection[0] < 0) newEyeX = std::max(newEyeX, tileWest + EdgeMargin);
		if (_guidedStepDirection[1] > 0) newEyeY = std::min(newEyeY, tileNorth + 16.0 - EdgeMargin);
		if (_guidedStepDirection[1] < 0) newEyeY = std::max(newEyeY, tileNorth + EdgeMargin);
		return true;
	}

	// Through, or the step didn't happen (out of time units, say): once the game is done with
	// the step, it's over and the keys take over again.
	if (!_stepPending)
	{
		_guidedStep = false;
	}
	return false;
}

/**
 * Does the time unit reserve the player set (F1 to F4, J on the map) leave enough for a move, as
 * the game's own walk checks it: a reserve the soldier can't pay for any more (time units already
 * below the reserved shot's cost) holds nothing back. Only checked, so the game shows no message.
 * @param unit The soldier.
 * @param timeUnits The move's time units.
 * @param energy The move's energy.
 */
bool FirstPersonMode::reserveAllows(BattleUnit *unit, int timeUnits, int energy) const
{
	BattlescapeGame *battleGame = _state->_battleGame;
	return !battleGame->checkReservedTU(unit, 0, 0, true) || battleGame->checkReservedTU(unit, timeUnits, energy, true);
}

/**
 * Which way the movement keys push: forward (1) or back (-1), and strafing right (1) or left
 * (-1), each 0 when neither or both of its keys are held. While the lean key is held (setting
 * "Lean"), the strafe keys lean instead (see updateLean) and don't strafe.
 * @param forward Set to the forward push.
 * @param strafe Set to the sideways push.
 */
void FirstPersonMode::movementKeys(double &forward, double &strafe) const
{
	// With the game's Ctrl or Alt held, the keys are its shortcuts (Ctrl+S): no walking.
	if (gameModifierHeld())
	{
		forward = strafe = 0.0;
		return;
	}
	const Uint8 *keyStates = SDL_GetKeyState(nullptr);
	forward = (keyStates[Options::keyFpsForward] ? 1.0 : 0.0) - (keyStates[Options::keyFpsBack] ? 1.0 : 0.0);
	strafe = (keyStates[Options::keyFpsStrafeRight] ? 1.0 : 0.0) - (keyStates[Options::keyFpsStrafeLeft] ? 1.0 : 0.0);
	if (Options::fpsLean > 0 && Options::keyFpsLeanHold != SDLK_UNKNOWN && keyStates[Options::keyFpsLeanHold])
	{
		strafe = 0.0;
	}
}

/**
 * Which way the numpad's move keys held right now push (see handleKey: 8 ahead, 2 back, 4 left,
 * 6 right, relative to the view).
 * @param forward Set to the forward push: 1, -1 or 0.
 * @param strafe Set to the sideways push: 1 (right), -1 or 0.
 */
void FirstPersonMode::numpadKeys(int &forward, int &strafe) const
{
	const Uint8 *keyStates = SDL_GetKeyState(nullptr);
	forward = (keyStates[SDLK_KP8] ? 1 : 0) - (keyStates[SDLK_KP2] ? 1 : 0);
	strafe = (keyStates[SDLK_KP6] ? 1 : 0) - (keyStates[SDLK_KP4] ? 1 : 0);
}

/**
 * Tile moves: a move walks exactly one tile, to the middle of the neighbour in the direction it
 * pushes (one of eight, relative to the view). The numpad makes such moves whatever the movement
 * setting, almost at once (NumpadMoveSpeed), like a step on the isometric map: a press is
 * remembered until the move can start, and a key still held starts the next; a press waits
 * NumpadComboWait for a key of the other axis to join it, and keys held then join it too (a
 * player: 8 and 4 together should make a diagonal, as W and A do). With the setting
 * "Movement: Tiles", W, A, S and D make them too, at walking speed, once held a moment (a second
 * key pressed with the first, W and D say, makes a diagonal), and holding them walks on. A key
 * held on from one move to the next stops at a closed door (a player's report: a held walk went on
 * through it, into whatever waited behind): only a fresh press goes through, opening it.
 * While the game is busy with something besides the soldier's own step (a shot, a turn by the
 * rules, the aliens' fire), the move keys do nothing (a player's report: a press during a burst
 * walked as it ended; the owner: ignore it): a numpad press then is dropped, and keys held
 * through it start no move until let go.
 * The move is the normal walk steered to the middle of that tile, so everything walking does
 * still happens on the way (the step through the game's own walking, doors, falling off ledges,
 * diagonal gaps, vaults); this only works out the velocity, straight to the tile's middle,
 * stopping there without overshooting. A move that stops coming closer (the step refused, a
 * wall) is over, and so is one that a door or a vault took over. Between moves, with tile moves
 * set, the camera settles back to the middle of its tile.
 * @param forward, strafe The movement keys' push (see movementKeys).
 * @param canMove Whether the keys may move the soldier now (see updateMovement).
 * @param walkSpeed The walking speed, in voxels a second.
 * @param elapsedSeconds Seconds since the last frame.
 * @param velocityX, velocityY Set to the velocity, in voxels a second.
 * @return True while a move is under way (it walks into the next tile, paying for it); false
 *         when only settling, or still.
 */
bool FirstPersonMode::tileMoveVelocity(BattleUnit *unit, double forward, double strafe, bool canMove, double walkSpeed, double elapsedSeconds, double &velocityX, double &velocityY)
{
	velocityX = velocityY = 0.0;
	const Uint32 now = SDL_GetTicks();
	const bool keyMoves = Options::fpsMovement == 1;
	const bool pushing = keyMoves && (forward != 0.0 || strafe != 0.0);
	const int sizeOffset = 8 * (unit->getArmor()->getSize() - 1); // a big unit's eye sits 8 voxels off its tile's middle
	int numpadForward = 0, numpadStrafe = 0;
	numpadKeys(numpadForward, numpadStrafe);
	// Busy with something besides the soldier's own step (a step of ours keeps the game busy
	// until it's walked): keys held now wait to be let go.
	const bool busyElsewhere = !canAct() && !_stepPending;
	if (busyElsewhere && (pushing || numpadForward || numpadStrafe))
	{
		_keysHeldThroughAction = true;
	}
	else if (!pushing && !numpadForward && !numpadStrafe)
	{
		_keysHeldThroughAction = false;
	}
	if (!canMove || busyElsewhere || _keysHeldThroughAction)
	{
		_numpadMove[0] = _numpadMove[1] = 0; // (a press while the soldier may not move is dropped)
		numpadForward = numpadStrafe = 0;
	}
	// W, A, S, D let go, even for a moment: the next press is a fresh one.
	if (!pushing)
	{
		_tileMoveHeldOn = false;
	}

	// Start a move when the soldier may step: a numpad press (or a numpad key still held) at
	// once, else the movement keys held a moment. It goes into the neighbour the push points at
	// (turned onto the map, yaw 0 east and 90 south, and snapped to the nearest of the eight
	// directions), if the rules allow that step or a closed door is there (walking through
	// opens it).
	if (!_tileMove)
	{
		if (!pushing)
		{
			_tileMoveKeysSince = 0;
		}
		else if (!_tileMoveKeysSince)
		{
			_tileMoveKeysSince = now;
		}
		// The move's push, and whether it comes from a key held on since the last move (a numpad
		// key still down, or W, A, S, D never let go) rather than a fresh press.
		double moveForward = 0.0, moveStrafe = 0.0;
		bool quick = true;
		bool heldOn = false;
		const bool numpadPressWaits = (_numpadMove[0] || _numpadMove[1]) && now - _numpadPressAt < NumpadComboWait;
		if (numpadPressWaits)
		{
			// (a second key may still join the press)
		}
		else if (_numpadMove[0] || _numpadMove[1])
		{
			// The press, and a key of the other axis held with it.
			moveForward = _numpadMove[0] ? _numpadMove[0] : numpadForward;
			moveStrafe = _numpadMove[1] ? _numpadMove[1] : numpadStrafe;
		}
		else if (numpadForward || numpadStrafe)
		{
			moveForward = numpadForward;
			moveStrafe = numpadStrafe;
			heldOn = true;
		}
		else if (pushing && !_keysHeldThroughAction && now - _tileMoveKeysSince >= TileMoveKeyWait)
		{
			moveForward = forward;
			moveStrafe = strafe;
			quick = false;
			heldOn = _tileMoveHeldOn;
		}
		if ((moveForward != 0.0 || moveStrafe != 0.0) && canMove && !_stepPending && canAct() && unit->getStatus() == STATUS_STANDING)
		{
			_numpadMove[0] = _numpadMove[1] = 0; // the press is used (or the way is blocked)
			const double yaw = _view->getYawDegrees() * Pi / 180.0; // radians
			const double pushX = std::cos(yaw) * moveForward - std::sin(yaw) * moveStrafe, pushY = std::sin(yaw) * moveForward + std::cos(yaw) * moveStrafe;
			const double octant = std::round(std::atan2(pushY, pushX) / (Pi / 4.0)) * (Pi / 4.0);
			const int stepX = (int)std::lround(std::cos(octant)), stepY = (int)std::lround(std::sin(octant));
			// Into a closed door only with a fresh press (a held key stops in front of it).
			const bool throughDoor = closedDoorTowards(stepX, stepY, unit->getPosition().z);
			bool stepOk = stepAllowed(unit, stepX, stepY);
			const int stepDirection = Pathfinding::vectorToDirection(Position(stepX, stepY, 0));
			if (!stepOk && !throughDoor && stepDirection >= 0 && _stepAllowedCache[stepDirection] == 2)
			{
				pushedIntoUnsafeLanding(stepDirection, false); // (a ledge with an unsafe landing, see stepAllowed: the next press after letting go goes over)
				stepOk = stepAllowed(unit, stepX, stepY); // (the second press goes over)
			}
			if ((stepOk || throughDoor) && !(throughDoor && heldOn))
			{
				_tileMove = true;
				_tileMoveQuick = quick;
				_tileMoveTarget = Position(_trackedTile.x + stepX, _trackedTile.y + stepY, 0);
				_tileMoveClosest = 1e9;
				_tileMoveProgressAt = now;
				_tileMoveHeldOn = pushing; // W, A, S, D still held when this move ends: the next is held on
			}
		}
	}

	// No move: with tile moves set, settle back to the middle of the tile (after a refused step,
	// say); free walking leaves the camera where it is.
	if (!_tileMove)
	{
		const double toMiddleX = _trackedTile.x * 16.0 + 8.0 + sizeOffset - _eyeX, toMiddleY = _trackedTile.y * 16.0 + 8.0 + sizeOffset - _eyeY;
		const double distance = std::sqrt(toMiddleX * toMiddleX + toMiddleY * toMiddleY);
		if (keyMoves && distance > 0.01 && !_stepPending && elapsedSeconds > 0.0)
		{
			const double speed = std::min(walkSpeed, distance / elapsedSeconds);
			velocityX = toMiddleX / distance * speed;
			velocityY = toMiddleY / distance * speed;
		}
		return false;
	}

	// A move: straight to the target tile's middle. Arrived, or not coming any closer: it's
	// over (with the keys still held, the next move starts at once).
	const double toTargetX = _tileMoveTarget.x * 16.0 + 8.0 + sizeOffset - _eyeX, toTargetY = _tileMoveTarget.y * 16.0 + 8.0 + sizeOffset - _eyeY;
	const double distance = std::sqrt(toTargetX * toTargetX + toTargetY * toTargetY);
	if (distance < _tileMoveClosest - 0.01)
	{
		_tileMoveClosest = distance;
		_tileMoveProgressAt = now;
	}
	if (elapsedSeconds <= 0.0)
	{
		return true; // a frame with no time passed moves nothing
	}
	if (distance <= 0.01 || now - _tileMoveProgressAt > TileMoveStuck)
	{
		_tileMove = false;
		_tileMoveKeysSince = pushing ? now - TileMoveKeyWait : 0;
		return false;
	}
	const double speed = std::min(_tileMoveQuick ? NumpadMoveSpeed : walkSpeed, distance / elapsedSeconds);
	velocityX = toTargetX / distance * speed;
	velocityY = toTargetY / distance * speed;
	return true;
}

/**
 * Jumps (the jump key): a free hop of the camera, JumpHeight voxels up and down again over
 * JumpTime ms, which costs nothing and changes nothing in the game. When the movement keys push
 * into a step that something low blocks (a fence, a table, a crate), the jump becomes a vault
 * over it instead (setting "Jump over low obstacles", see tryVault), whose walk the rules do
 * charge for.
 */
void FirstPersonMode::jump(BattleUnit *unit)
{
	// No jump while already in the air, or while down, kneeling or busy (walking, turning...).
	// A flying soldier with flight on rises with the up key instead.
	if (_jumpStart || !unit || unit->isOut() || unit->isKneeled() || unit->getStatus() != STATUS_STANDING || (canFly(unit) && flightOn(unit)))
	{
		return;
	}

	// Which way the movement keys push: forward (1) or back (-1), and strafing right (1) or left (-1).
	double forward = 0.0, strafe = 0.0;
	movementKeys(forward, strafe);

	// Pushing towards a neighbouring tile while the soldier may act: if the rules block that
	// step, try to vault over whatever blocks it.
	if ((forward != 0.0 || strafe != 0.0) && !_stepPending && canAct() && _state->playableUnitSelected())
	{
		// The push turned from the view's frame onto the map (yaw 0 is east, 90 south), then
		// snapped to the nearer grid axis: a vault goes straight along x or y, never diagonally.
		const double yaw = _view->getYawDegrees() * Pi / 180.0; // radians
		const double pushX = std::cos(yaw) * forward - std::sin(yaw) * strafe, pushY = std::sin(yaw) * forward + std::cos(yaw) * strafe;
		const int stepX = std::abs(pushX) >= std::abs(pushY) ? (pushX > 0.0 ? 1 : -1) : 0;
		const int stepY = std::abs(pushX) >= std::abs(pushY) ? 0 : (pushY > 0.0 ? 1 : -1);

		// A key press comes between frames, so the cached step answers may be old: ask afresh.
		std::fill(std::begin(_stepAllowedCache), std::end(_stepAllowedCache), -1);
		if (!stepAllowed(unit, stepX, stepY))
		{
			// A ledge with an unsafe landing: the jump is a deliberate push (its warning, or its go-ahead).
			const int direction = Pathfinding::vectorToDirection(Position(stepX, stepY, 0));
			if (_stepAllowedCache[direction] == 2)
			{
				pushedIntoUnsafeLanding(direction, true);
				return;
			}
			if (tryVault(unit, stepX, stepY))
			{
				return;
			}
		}
	}

	// Otherwise a plain hop (currentJumpHeight lifts the eye along its arc).
	_jumpStart = SDL_GetTicks();
	_jumpTime = JumpTime;
	_jumpHeight = JumpHeight;
}

/**
 * Is this tile part something low enough to jump over? It must be a wall or an object, not a
 * door, standing on the floor (it doesn't raise the ground like stairs or a ramp), not a big
 * wall of the kinds that act as walls on some sides only (big wall kinds 2 to 9), and no higher
 * than VaultMaxHeight voxels (waist high, of a level's 24).
 * @param part The tile part to look at (a TilePart: O_WESTWALL, O_NORTHWALL or O_OBJECT).
 * @return True if the part is there and low enough to vault.
 */
bool FirstPersonMode::isLowObstacle(const Tile *tile, int part) const
{
	const MapData *mapData = tile ? tile->getMapData((TilePart)part) : nullptr;
	if (!mapData || mapData->isDoor() || mapData->isUFODoor() || mapData->getTerrainLevel() != 0 || mapData->getBigWall() > 1)
	{
		return false;
	}

	// Its height, from its shape for line of fire (LOFT): 12 layers from the floor up, each 2
	// voxels high; the highest layer with a shape in it gives the top.
	int height = 0;
	for (int layer = 0; layer < 12; ++layer)
	{
		if (mapData->getLoftID(layer)) height = (layer + 1) * 2;
	}
	return height > 0 && height <= VaultMaxHeight;
}

/**
 * Vaults over a low obstacle straight ahead (setting "Jump over low obstacles"; only units one
 * tile big): over a low wall onto the next tile, or over something low standing on the next
 * tile onto the one beyond it (with any low walls on the way). The step is walked by the rules,
 * with the pathfinding told to let exactly those obstacles through: the jump itself is free, and
 * the walk costs its normal time units (as if the obstacles weren't there). Meanwhile the camera
 * glides over the obstacle in a high arc (see glideOverObstacle and currentJumpHeight).
 * @param stepX The direction along x (east is positive): -1, 0 or 1.
 * @param stepY The direction along y (south is positive): -1, 0 or 1.
 * @return False if there's nothing to vault, or the soldier can't (no straight way over, not
 *   enough time units).
 */
bool FirstPersonMode::tryVault(BattleUnit *unit, int stepX, int stepY)
{
	if (!Options::fpsVault || unit->getArmor()->getSize() != 1)
	{
		return false;
	}

	// The tiles involved, and two helpers: the tile owning the wall between two neighbouring
	// tiles (a tile owns its west and north walls, so going east or south it is the farther one),
	// and whether a tile part is there and stops walking.
	const Position from(_trackedTile.x, _trackedTile.y, unit->getPosition().z);
	const Position next = from + Position(stepX, stepY, 0);
	const TilePart wallPart = stepX != 0 ? O_WESTWALL : O_NORTHWALL;
	auto wallBetween = [&](const Position &nearer, const Position &farther) { return _save->getTile(stepX + stepY > 0 ? farther : nearer); };
	auto blocking = [](const Tile *tile, TilePart part) {
		return tile->getMapData(part) && tile->getTUCost(part, MT_WALK) >= Pathfinding::INVALID_MOVE_COST;
	};
	const Tile *nextTile = _save->getTile(next);
	const Tile *wallTile = wallBetween(from, next);
	if (!nextTile || !wallTile)
	{
		return false;
	}

	// Collect what's in the way. A blocking wall between here and the next tile must be low.
	std::vector<std::pair<const Tile*, int>> vaultedParts;
	if (blocking(wallTile, wallPart))
	{
		if (!isLowObstacle(wallTile, wallPart)) return false;
		vaultedParts.push_back({ wallTile, wallPart });
	}

	// A blocking object on the next tile must be low too; then the soldier lands one tile
	// further on, over any (low) wall between the next tile and that one.
	Position landing = next;
	if (blocking(nextTile, O_OBJECT))
	{
		if (!isLowObstacle(nextTile, O_OBJECT)) return false;
		vaultedParts.push_back({ nextTile, O_OBJECT });
		landing = next + Position(stepX, stepY, 0);
		const Tile *beyondTile = _save->getTile(landing), *secondWallTile = wallBetween(next, landing);
		if (!beyondTile || !secondWallTile)
		{
			return false;
		}
		if (blocking(secondWallTile, wallPart))
		{
			if (!isLowObstacle(secondWallTile, wallPart)) return false;
			vaultedParts.push_back({ secondWallTile, wallPart });
		}
	}
	if (vaultedParts.empty())
	{
		return false; // Nothing low in the way: whatever stops the step isn't for jumping over.
	}

	// Let the pathfinding through those parts for this unit, and ask it for the way to the
	// landing tile: it must go straight there, one step over a wall or two over an object.
	Pathfinding *pathfinding = _save->getPathfinding();
	pathfinding->setVault(unit, vaultedParts);
	pathfinding->calculate(unit, landing, BAM_NORMAL);
	const int direction = Pathfinding::vectorToDirection(Position(stepX, stepY, 0));
	const std::vector<int> &path = pathfinding->getPath();
	bool canVault = path.size() == (landing == next ? 1u : 2u)
		&& std::all_of(path.begin(), path.end(), [direction](int pathDirection) { return pathDirection == direction; });

	// The soldier must have the walk's time units, keeping those reserved for a shot (the game's
	// "not enough time units" message if not). Energy is left to the walking state, which stops
	// the walk when it runs out (see updateCamera). The debug "doom mode" skips this.
	if (canVault && !doomModeOn())
	{
		const int timeUnitCost = pathfinding->getTotalTUCost();
		canVault = unit->getTimeUnits() >= timeUnitCost && reserveAllows(unit, timeUnitCost, 0);
		if (!canVault)
		{
			_state->warning("STR_NOT_ENOUGH_TIME_UNITS");
		}
	}

	// No vault: forget the path, and the pathfinding goes back to the plain rules.
	if (!canVault)
	{
		pathfinding->abortPath();
		pathfinding->setVault(nullptr, {});
		return false;
	}

	// Hand the walk to the game's walking state, as commitStep does; the camera's tile is the
	// landing tile from now on.
	BattleAction walk;
	walk.type = BA_WALK;
	walk.actor = unit;
	walk.target = landing;
	walk.instantMove = true;
	_state->_battleGame->statePushBack(new UnitWalkBState(_state->_battleGame, walk));
	_stepPending = true;
	_stepFrom = from;
	_trackedTile = Position(landing.x, landing.y, from.z);
	std::fill(std::begin(_stepAllowedCache), std::end(_stepAllowedCache), -1);

	// The camera's glide: from the eye to the middle of the landing tile (in voxels), in a high
	// arc lasting 450 ms over one tile or 650 ms over two.
	_vaulting = true;
	_vaultFrom[0] = _eyeX;
	_vaultFrom[1] = _eyeY;
	_vaultTo[0] = landing.x * 16.0 + 8.0;
	_vaultTo[1] = landing.y * 16.0 + 8.0;
	_jumpStart = SDL_GetTicks();
	_jumpTime = landing == next ? 450 : 650;
	_jumpHeight = VaultHeight;
	return true;
}

/**
 * While a vault is on, moves the camera in a straight line from where the vault started to the
 * middle of the landing tile, in time with the jump's arc (currentJumpHeight lifts it meanwhile),
 * and keeps the walking momentum at zero. The vault is over once the arc is done and the game
 * has walked the step.
 * @param newEyeX Set to the camera's x for this frame, in voxels (when returning true).
 * @param newEyeY Set to the camera's y for this frame, in voxels (when returning true).
 * @return True while the vault moves the camera (the movement keys wait).
 */
bool FirstPersonMode::glideOverObstacle(BattleUnit *, double &newEyeX, double &newEyeY)
{
	if (!_vaulting)
	{
		return false;
	}

	// How far through the arc the jump is, 0 to 1 (1 once it has landed).
	const double progress = _jumpStart ? std::min(1.0, (SDL_GetTicks() - _jumpStart) / (double)_jumpTime) : 1.0;
	newEyeX = _vaultFrom[0] + (_vaultTo[0] - _vaultFrom[0]) * progress;
	newEyeY = _vaultFrom[1] + (_vaultTo[1] - _vaultFrom[1]) * progress;
	_velocityX = _velocityY = 0.0;

	// Landed, and the game is done with the walk: the keys take over again.
	if (progress >= 1.0 && !_stepPending)
	{
		_vaulting = false;
	}
	return true;
}

/**
 * How high the jump lifts the eye right now, in voxels: a parabola rising to _jumpHeight at half
 * time and coming back down at _jumpTime ms. The head stops short of anything solid above it.
 * Landing ends the jump, with a footfall.
 * @return The lift, in voxels (0 when not jumping).
 */
double FirstPersonMode::currentJumpHeight(BattleUnit *unit)
{
	if (!_jumpStart)
	{
		return 0.0;
	}

	// How far through the jump we are, 0 to 1; at the end, land.
	const double progress = (SDL_GetTicks() - _jumpStart) / (double)std::max<Uint32>(1, _jumpTime);
	if (progress >= 1.0)
	{
		_jumpStart = 0;
		playFootsteps(unit, 8.0); // The landing: a full footstep's distance, so it sounds at once.
		return 0.0;
	}

	// The lift along the arc (4 t (1 - t) peaks at 1 half way). The head stops short of anything
	// above it (a ceiling, a UFO's hull, a ledge): look up voxel by voxel from the top of the head
	// (2 voxels above the eye), up to the top of the map (24 voxels per level), and stay a voxel
	// below the first solid one. Units don't count.
	const double lift = _jumpHeight * 4.0 * progress * (1.0 - progress);
	const int mapTopZ = _save->getMapSizeZ() * 24 - 1;
	for (int voxelsUp = 0; voxelsUp <= (int)std::ceil(lift); ++voxelsUp)
	{
		const int headTopZ = (int)_eyeZ + voxelsUp + 2;
		if (headTopZ > mapTopZ) break;
		if (_save->getTileEngine()->voxelCheck(Position((int)_eyeX, (int)_eyeY, headTopZ), nullptr, true) != V_EMPTY)
		{
			return std::min(lift, std::max(0.0, voxelsUp - 1.0));
		}
	}
	return lift;
}

/**
 * Stops a vault at once: the pathfinding goes back to the plain rules (no obstacles let
 * through), the camera back to the keys, and any jump in the air ends. Used when the 3D view is
 * switched off, when the camera starts following another unit, and when a vault was cut short
 * (see updateCamera).
 */
void FirstPersonMode::endVault()
{
	_save->getPathfinding()->setVault(nullptr, {});
	_vaulting = false;
	_jumpStart = 0;
}

/**
 * Climbing with W and S, as on Half-Life's ladders: while the soldier stands on a grav lift or a
 * ladder, or flies with flight switched on (a player: "letting me look up and hold w to fly
 * straight up"), W with the view looking up more than ClimbPitch degrees goes up a level and
 * looking down as much goes down (S the other way round), through moveUpDown, so the game
 * checks and charges it as it does the up and down keys. Held, it climbs on level by level, each
 * once the last ride is over. Looking more level, or where the way doesn't go on (a lift's top or
 * foot, a ceiling over a flyer, the ground under one), the key walks as usual: that's how the
 * soldier steps off. (The arrows by the compass show it: see updateClimbArrows.)
 * @param unit The soldier.
 * @param forward The movement keys' forward push (see movementKeys): 1 for W, -1 for S.
 * @return True if the push is the climb's, so it doesn't walk.
 */
bool FirstPersonMode::updateClimb(BattleUnit *unit, double forward)
{
	const double pitch = _view->getPitchDegrees();
	const bool lookingUp = pitch > ClimbPitch, lookingDown = pitch < -ClimbPitch;
	if (forward == 0.0 || (!lookingUp && !lookingDown))
	{
		return false;
	}

	// During a ride the push waits (it would drift the camera off the lift).
	const Uint32 now = SDL_GetTicks();
	if (_rideStart && now - _rideStart < _rideTime)
	{
		return true;
	}

	// Which way: W looking up (or S looking down) is up. Only where the lift, the ladder or the
	// flight goes that way.
	const int direction = (forward > 0.0) == lookingUp ? Pathfinding::DIR_UP : Pathfinding::DIR_DOWN;
	bool up = false, down = false;
	climbWays(unit, up, down);
	if (!(direction == Pathfinding::DIR_UP ? up : down))
	{
		return false;
	}

	// Climb as soon as the soldier is free to: not mid-step, the game idle. A climb the game
	// refused (its message says why) is tried again only after ClimbRetryTime.
	if (canAct() && unit->getStatus() == STATUS_STANDING && !_stepPending && !_state->_battleGame->isBusy() && now >= _climbRetryAt)
	{
		moveUpDown(unit, direction, nullptr);
		if (!_state->_battleGame->isBusy())
		{
			_climbRetryAt = now + ClimbRetryTime;
		}
	}
	return true;
}

/**
 * Real-time movement with the movement keys, called every frame. Inside a tile the camera moves
 * freely, with a little momentum, and nothing is paid. Crossing into the next tile commits a step
 * that the game walks and charges by its rules (see commitStep): the diagonal first, else
 * sliding along either axis. Where no step is possible (a wall, not enough time units...) the
 * camera stops short of the tile's edge. Walking into a closed door stops in front of it and
 * steps through it, and pushing into a diagonal gap steps through that (see walkGuidedStep). A
 * vault (see glideOverObstacle) moves the camera on its own.
 * @param canMove Whether the keys may move the soldier: true on the player's turn, false while
 *   the view only follows the soldier (the aliens' turn, a breakdown).
 * @param elapsedSeconds Seconds since the last frame.
 */
void FirstPersonMode::updateMovement(BattleUnit *unit, bool canMove, double elapsedSeconds)
{
	// After a new enemy was spotted the keys are ignored until they're all let go (see
	// checkSpotted). Otherwise, which way they push: forward (1) or back (-1), and strafing right
	// (1) or left (-1).
	double forward = 0.0, strafe = 0.0;
	movementKeys(forward, strafe);
	int numpadForward = 0, numpadStrafe = 0;
	numpadKeys(numpadForward, numpadStrafe);
	if (_movementHeld && forward == 0.0 && strafe == 0.0 && numpadForward == 0 && numpadStrafe == 0)
	{
		_movementHeld = false;
	}
	if (forward == 0.0 && strafe == 0.0 && numpadForward == 0 && numpadStrafe == 0)
	{
		_unsafeReleased = true; // (after an unsafe landing's warning, the next push goes over: see pushedIntoUnsafeLanding)
	}
	if (!canMove || _movementHeld)
	{
		forward = strafe = 0.0;
	}

	// On a grav lift or a ladder, W looking up climbs and looking down goes down (S the other way
	// round, see updateClimb); then the push doesn't walk.
	if (forward != 0.0 && updateClimb(unit, forward))
	{
		forward = 0.0;
	}

	// The camera's new position starts where it is now. A big unit's eye sits 8 voxels off the
	// middle of its tile (see clampInsideTile). The walking speed setting is in tiles a second
	// (1 to 8), here turned into voxels a second; sprinting, SprintPace times that, while the
	// soldier can run (its armor allows it, and the last step wasn't walked for want of breath).
	const int sizeOffset = 8 * (unit->getArmor()->getSize() - 1);
	double newEyeX = _eyeX, newEyeY = _eyeY;
	const bool sprinting = _sprintOn && _sprintStepRan && unit->getArmor()->allowsRunning(unit->isSmallUnit());
	const double walkSpeed = std::clamp(Options::fpsWalkSpeed, 1, 8) * 16.0 * (sprinting ? SprintPace : 1.0);

	// A vault in progress moves the camera on its own.
	if (glideOverObstacle(unit, newEyeX, newEyeY))
	{
		_eyeX = newEyeX;
		_eyeY = newEyeY;
		return;
	}

	// So does a step through a door or a diagonal gap, with footsteps. Its speed becomes the
	// momentum, so the walk carries on smoothly once the doorway or the gap is behind.
	if (walkGuidedStep(unit, newEyeX, newEyeY, elapsedSeconds))
	{
		const double distanceMoved = std::sqrt((newEyeX - _eyeX) * (newEyeX - _eyeX) + (newEyeY - _eyeY) * (newEyeY - _eyeY));
		playFootsteps(unit, distanceMoved);
		_velocityX = elapsedSeconds > 0.0 ? (newEyeX - _eyeX) / elapsedSeconds : 0.0;
		_velocityY = elapsedSeconds > 0.0 ? (newEyeY - _eyeY) / elapsedSeconds : 0.0;
		_eyeX = newEyeX;
		_eyeY = newEyeY;
		return;
	}

	// The velocity the keys ask for, in voxels a second: their push turned from the view's frame
	// onto the map (yaw 0 is east, 90 south), at walking speed in any direction (diagonals are
	// no faster). Tile by tile, the velocity that walks to the next tile's middle instead, with
	// no momentum: the moves start and stop crisply.
	double wantedVelocityX = 0.0, wantedVelocityY = 0.0;
	const bool tileMoves = Options::fpsMovement == 1 || _tileMove || ((_numpadMove[0] || _numpadMove[1] || numpadForward || numpadStrafe) && canMove && !_movementHeld);
	bool tileMoveDriving = false;
	if (tileMoves)
	{
		tileMoveDriving = tileMoveVelocity(unit, forward, strafe, canMove && !_movementHeld, walkSpeed, elapsedSeconds, wantedVelocityX, wantedVelocityY);
	}
	else if (forward != 0.0 || strafe != 0.0)
	{
		const double yaw = _view->getYawDegrees() * Pi / 180.0; // radians
		const double pushX = std::cos(yaw) * forward - std::sin(yaw) * strafe;
		const double pushY = std::sin(yaw) * forward + std::cos(yaw) * strafe;
		const double pushLength = std::sqrt(pushX * pushX + pushY * pushY);
		wantedVelocityX = pushX / pushLength * walkSpeed;
		wantedVelocityY = pushY / pushLength * walkSpeed;
	}

	// A little momentum: the soldier speeds up and slows down over a fraction of a second (the
	// velocity closes in on the wanted one at 12 times the gap per second), and with no key
	// held a crawl below 1 voxel a second stops dead. Then the camera moves by this frame's share.
	const double blend = tileMoves ? 1.0 : std::min(1.0, elapsedSeconds * 12.0);
	_velocityX += (wantedVelocityX - _velocityX) * blend;
	_velocityY += (wantedVelocityY - _velocityY) * blend;
	if (wantedVelocityX == 0.0 && std::abs(_velocityX) < 1.0) _velocityX = 0.0;
	if (wantedVelocityY == 0.0 && std::abs(_velocityY) < 1.0) _velocityY = 0.0;
	newEyeX += _velocityX * elapsedSeconds;
	newEyeY += _velocityY * elapsedSeconds;

	// Only a held key walks into the next tile (and pays for it); gliding to a stop doesn't.
	// Tile by tile, a move under way walks on (settling back into the tile doesn't).
	const bool driving = tileMoves ? tileMoveDriving : (wantedVelocityX != 0.0 || wantedVelocityY != 0.0);

	// A closed door ahead (soldiers one tile big only): stop DoorMargin voxels in front of it,
	// and walking on steps through it, the game opening it on the way. By tile moves, only a move
	// aimed through that door steps through it: one that only goes to the middle of the tile
	// before the door comes within DoorMargin of it too (the middle is 8 voxels from the edge),
	// and must stop there (a player's report: a tap walked on through the door).
	if (unit->getArmor()->getSize() == 1)
	{
		// The tile's west and north edges, in voxels, and the four sides: east, west, south, north.
		const double westEdgeX = _trackedTile.x * 16.0, northEdgeY = _trackedTile.y * 16.0;
		const int z = unit->getPosition().z;
		const int sides[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
		for (const auto &side : sides)
		{
			// Only a side the camera moves towards and is close to, with a closed door in it.
			const bool pushing = side[0] > 0 ? newEyeX > _eyeX && newEyeX > westEdgeX + 16.0 - DoorMargin
				: side[0] < 0 ? newEyeX < _eyeX && newEyeX < westEdgeX + DoorMargin
				: side[1] > 0 ? newEyeY > _eyeY && newEyeY > northEdgeY + 16.0 - DoorMargin
				: newEyeY < _eyeY && newEyeY < northEdgeY + DoorMargin;
			if (!pushing || !closedDoorTowards(side[0], side[1], z))
			{
				continue;
			}

			// Stop in front of the door.
			if (side[0] > 0) newEyeX = std::min(newEyeX, westEdgeX + 16.0 - DoorMargin);
			if (side[0] < 0) newEyeX = std::max(newEyeX, westEdgeX + DoorMargin);
			if (side[1] > 0) newEyeY = std::min(newEyeY, northEdgeY + 16.0 - DoorMargin);
			if (side[1] < 0) newEyeY = std::max(newEyeY, northEdgeY + DoorMargin);

			// Walking on, and the rules allow the step: take it, and let walkGuidedStep move the
			// camera from now on.
			const bool moveGoesThrough = !tileMoves || (_tileMove && _tileMoveTarget.x == _trackedTile.x + side[0] && _tileMoveTarget.y == _trackedTile.y + side[1]);
			if (!_stepPending && driving && moveGoesThrough && canMove && canAct() && unit->getStatus() == STATUS_STANDING && stepAllowed(unit, side[0], side[1])
				&& commitStep(unit, side[0], side[1]))
			{
				_guidedStep = true;
				_guidedStepDoor = true;
				_guidedStepDirection[0] = side[0];
				_guidedStepDirection[1] = side[1];
				_guidedStepStart = SDL_GetTicks();
				playFootsteps(unit, std::sqrt((newEyeX - _eyeX) * (newEyeX - _eyeX) + (newEyeY - _eyeY) * (newEyeY - _eyeY)));
				_eyeX = newEyeX;
				_eyeY = newEyeY;
				return;
			}
		}
	}

	// Pushing into a corner between two units or objects standing corner to corner, with the
	// diagonal step open (or walking a diagonal tile move): take it (see stepThroughDiagonalGap),
	// and let walkGuidedStep move the camera through the gap from the next frame on. Meanwhile it
	// stays where it was.
	if (!_stepPending && driving && canMove && canAct() && unit->getStatus() == STATUS_STANDING
		&& stepThroughDiagonalGap(unit, newEyeX, newEyeY, wantedVelocityX, wantedVelocityY,
			_tileMove && _tileMoveTarget.x != _trackedTile.x && _tileMoveTarget.y != _trackedTile.y))
	{
		return;
	}

	// Which neighbouring tile the new position falls into, per axis: -1, 0 (still this tile) or 1.
	const int crossingX = std::clamp((int)std::floor((newEyeX - sizeOffset) / 16.0) - _trackedTile.x, -1, 1);
	const int crossingY = std::clamp((int)std::floor((newEyeY - sizeOffset) / 16.0) - _trackedTile.y, -1, 1);
	if (crossingX != 0 || crossingY != 0)
	{
		// Try the step the camera crosses into; for a diagonal, if that one isn't allowed, slide
		// along either axis instead (east-west first, then north-south).
		bool stepped = false;
		const int candidates[3][2] = { { crossingX, crossingY }, { crossingX, 0 }, { 0, crossingY } };
		for (int candidate = 0; candidate < 3 && !stepped; ++candidate)
		{
			// Skip "no step", and the sliding candidates unless the crossing is diagonal.
			const int stepX = candidates[candidate][0], stepY = candidates[candidate][1];
			if ((stepX == 0 && stepY == 0) || (candidate > 0 && (crossingX == 0 || crossingY == 0)))
			{
				continue;
			}
			if (driving && canMove && canAct() && unit->getStatus() == STATUS_STANDING && stepAllowed(unit, stepX, stepY))
			{
				const Position oldTile = _trackedTile;
				if (commitStep(unit, stepX, stepY))
				{
					// Keep moving along the axes the step goes, but stay inside the old tile on
					// the other one.
					if (stepX == 0) newEyeX = clampInsideTile(oldTile.x, newEyeX, sizeOffset);
					if (stepY == 0) newEyeY = clampInsideTile(oldTile.y, newEyeY, sizeOffset);
					stepped = true;
				}
			}
		}

		// No step: stay inside this tile on the axes that would have left it.
		if (!stepped)
		{
			if (crossingX != 0) newEyeX = clampInsideTile(_trackedTile.x, newEyeX, sizeOffset);
			if (crossingY != 0) newEyeY = clampInsideTile(_trackedTile.y, newEyeY, sizeOffset);
		}
	}

	// Don't walk into the walls of the tile we're in: EdgeMargin voxels from each edge the
	// soldier couldn't step across. Its west and north edges, in voxels:
	// A held push stopped at a ledge with an unsafe landing says so (see pushedIntoUnsafeLanding).
	const double westEdgeX = _trackedTile.x * 16.0 + sizeOffset, northEdgeY = _trackedTile.y * 16.0 + sizeOffset;
	// (Only the first edge stopped at a frame: pushed diagonally into a corner of two such ledges,
	// the two warnings took turns every frame and a second push never went over.)
	bool unsafeEdgeSeen = false;
	auto stopAtEdge = [&](int stepX, int stepY)
	{
		const int direction = Pathfinding::vectorToDirection(Position(stepX, stepY, 0));
		if (!unsafeEdgeSeen && driving && canMove && !_stepPending && _stepAllowedCache[direction] == 2)
		{
			pushedIntoUnsafeLanding(direction, false);
			unsafeEdgeSeen = true;
		}
	};
	if (newEyeX < westEdgeX + EdgeMargin && !stepAllowed(unit, -1, 0)) { newEyeX = westEdgeX + EdgeMargin; stopAtEdge(-1, 0); }
	if (newEyeX > westEdgeX + 16.0 - EdgeMargin && !stepAllowed(unit, 1, 0)) { newEyeX = westEdgeX + 16.0 - EdgeMargin; stopAtEdge(1, 0); }
	if (newEyeY < northEdgeY + EdgeMargin && !stepAllowed(unit, 0, -1)) { newEyeY = northEdgeY + EdgeMargin; stopAtEdge(0, -1); }
	if (newEyeY > northEdgeY + 16.0 - EdgeMargin && !stepAllowed(unit, 0, 1)) { newEyeY = northEdgeY + 16.0 - EdgeMargin; stopAtEdge(0, 1); }

	// Whatever stopped us (walls, edges, a step not taken) takes the momentum out on that axis:
	// where the camera really moved slower than the momentum (by more than half a voxel a
	// second), the momentum drops to the real speed (to 0 below 1 voxel a second).
	if (elapsedSeconds > 0.0)
	{
		const double actualVelocityX = (newEyeX - _eyeX) / elapsedSeconds, actualVelocityY = (newEyeY - _eyeY) / elapsedSeconds;
		if (std::abs(actualVelocityX) < std::abs(_velocityX) - 0.5) _velocityX = std::abs(actualVelocityX) < 1.0 ? 0.0 : actualVelocityX;
		if (std::abs(actualVelocityY) < std::abs(_velocityY) - 0.5) _velocityY = std::abs(actualVelocityY) < 1.0 ? 0.0 : actualVelocityY;
	}

	// Move the camera, with footsteps and the head bob following the distance walked.
	const double distanceMoved = std::sqrt((newEyeX - _eyeX) * (newEyeX - _eyeX) + (newEyeY - _eyeY) * (newEyeY - _eyeY));
	playFootsteps(unit, distanceMoved);
	_headBobPhase += distanceMoved;
	_eyeX = newEyeX;
	_eyeY = newEyeY;
}

/**
 * Footstep sounds while walking in the 3D view. The game's walking state plays them only for
 * walks animated on the isometric map, and ours are instant moves, so they're played here: a
 * footstep every 8 voxels walked, two per tile as in the game. A unit with a movement sound of
 * its own plays that on every other footstep (once a tile); anyone else plays the pair of
 * footstep sounds of what they walk on, the two in turn.
 * @param distance How far the camera just moved, in voxels.
 */
void FirstPersonMode::playFootsteps(BattleUnit *unit, double distance)
{
	// Add up the distance walked until it makes a footstep.
	_footstepDistance += distance;
	if (_footstepDistance < 8.0)
	{
		return;
	}
	_footstepDistance = 0.0;
	Tile *tile = unit->getTile();
	if (!tile)
	{
		return;
	}

	// Pick the sound: the unit's own movement sound on every other footstep; or, from the pair of
	// footstep sounds of the floor (or of an object, or of the top of stairs below), the first or
	// the second in turn.
	int sound = Mod::NO_SOUND;
	if (unit->getMoveSound() != Mod::NO_SOUND)
	{
		if (_footstepPhase == 0) sound = unit->getMoveSound();
	}
	else
	{
		const int footstepType = tile->getFootstepSound(_save->getBelowTile(tile));
		if (footstepType > -1) sound = Mod::WALK_OFFSET + footstepType * 2 + _footstepPhase;
	}
	_footstepPhase ^= 1;

	// Play it, from the sounds for the battle's depth (underwater battles have their own).
	if (sound != Mod::NO_SOUND)
	{
		if (Sound *footstep = _game->getMod()->getSoundByDepth(_save->getDepth(), sound))
		{
			footstep->play();
		}
	}
}

/**
 * Which ways a grav lift (or a ladder) takes the soldier from where they stand: the moves up
 * and down the pathfinding allows from there, and for a flyer with flight switched off only a
 * walker's (see walkerMayMoveUpDown: at a lift's top, not on up into the air). Neither when the
 * soldier isn't on a lift or a ladder.
 * @param unit The soldier.
 * @param up Set to whether it goes up.
 * @param down Set to whether it goes down.
 */
void FirstPersonMode::liftWays(BattleUnit *unit, bool &up, bool &down) const
{
	up = down = false;
	const Tile *tile = unit->getTile();
	const MapData *floor = tile ? tile->getMapData(O_FLOOR) : nullptr;
	if (!tile || !((floor && floor->isGravLift()) || tile->hasLadder()))
	{
		return;
	}
	Pathfinding *pathfinding = _save->getPathfinding();
	const bool walkerOnly = canFly(unit) && !flightOn(unit);
	up = pathfinding->getTUCost(unit->getPosition(), Pathfinding::DIR_UP, unit, nullptr, BAM_NORMAL).cost.time < Pathfinding::INVALID_MOVE_COST
		&& (!walkerOnly || walkerMayMoveUpDown(unit->getPosition(), Pathfinding::DIR_UP));
	down = pathfinding->getTUCost(unit->getPosition(), Pathfinding::DIR_DOWN, unit, nullptr, BAM_NORMAL).cost.time < Pathfinding::INVALID_MOVE_COST
		&& (!walkerOnly || walkerMayMoveUpDown(unit->getPosition(), Pathfinding::DIR_DOWN));
}

/**
 * Which ways W and S may climb from where the soldier is (see updateClimb): a grav lift's or a
 * ladder's (see liftWays), or else, flying with flight switched on, the moves up and down the
 * pathfinding allows a flyer from there (not through a ceiling, nor below the ground).
 * @param unit The soldier.
 * @param up Set to whether the way goes up.
 * @param down Set to whether it goes down.
 */
void FirstPersonMode::climbWays(BattleUnit *unit, bool &up, bool &down) const
{
	liftWays(unit, up, down);
	if (up || down || !canFly(unit) || !flightOn(unit))
	{
		return;
	}
	Pathfinding *pathfinding = _save->getPathfinding();
	up = pathfinding->getTUCost(unit->getPosition(), Pathfinding::DIR_UP, unit, nullptr, BAM_NORMAL).cost.time < Pathfinding::INVALID_MOVE_COST;
	down = pathfinding->getTUCost(unit->getPosition(), Pathfinding::DIR_DOWN, unit, nullptr, BAM_NORMAL).cost.time < Pathfinding::INVALID_MOVE_COST;
}

/**
 * The lift controls, updated every frame. While the soldier stands on a grav lift (or a ladder)
 * that can take them up or down from here, purple arrows show, one for each way it goes, with
 * LIFT PLATFORM between them and the keys that do the same under it. A click on an arrow (with
 * the free cursor) or the key rides it (see moveUpDown). Hidden with the classic HUD (which has
 * its own up and down buttons), during a step or a ride, and while the soldier can't act.
 * @param unit The soldier we look through (null hides the controls).
 * @param now SDL ticks, in ms (the arrows pulse with time).
 */
void FirstPersonMode::updateLift(BattleUnit *unit, Uint32 now)
{
	// Which ways the lift goes from here (see liftWays).
	bool up = false, down = false;
	const bool riding = _rideStart && now - _rideStart < _rideTime;
	if (unit && isActive() && !_classicHud && !_hudHidden && !_stepPending && !riding && canAct() && unit->getStatus() == STATUS_STANDING)
	{
		liftWays(unit, up, down);
	}

	// Show an arrow for each way, and the texts with either; with neither, nothing is hovered.
	_liftUp->setVisible(up);
	_liftDown->setVisible(down);
	_txtLift->setVisible(up || down);
	_txtLiftKeys->setVisible(up || down);
	if (!up && !down)
	{
		_liftArrowHovered = 0;
		return;
	}

	// The keys that do the same (e.g. "PAGE UP / PAGE DOWN"), in capitals as the game spells key
	// names; written again only when the arrows shown change (bit 1: up, bit 2: down).
	const int liftWays = (up ? 1 : 0) | (down ? 2 : 0);
	if (liftWays != _liftKeysShownFor)
	{
		std::string keyNames = up ? keyLabel(Options::keyFpsUp) : std::string();
		const std::string downKey = down ? keyLabel(Options::keyFpsDown) : std::string();
		if (!downKey.empty()) keyNames += (keyNames.empty() ? "" : " / ") + downKey;
		_txtLiftKeys->setText(keyNames);
		_liftKeysShownFor = liftWays;
	}

	// Draw the arrows afresh every frame: they pulse, and light up under the cursor.
	if (up) drawLiftArrow(_liftUp, true, _liftArrowHovered == 1, now);
	if (down) drawLiftArrow(_liftDown, false, _liftArrowHovered == -1, now);
}

/**
 * Draws a lift arrow: a wide purple chevron pointing up or down, gently pulsing, bright when the
 * cursor is on it, with a dark purple outline and a lighter rim along its leading edge.
 * @param surface The arrow's surface (cleared and drawn over completely).
 * @param up True for the up arrow, false for the down arrow.
 * @param hover True while the cursor is on it.
 * @param now SDL ticks, in ms (for the pulse).
 */
void FirstPersonMode::drawLiftArrow(Surface *surface, bool up, bool hover, Uint32 now)
{
	// The colours, nearest in the battlescape palette: the outline, the body (pulsing a little
	// over about a second, or bright under the cursor) and the rim.
	const SDL_Color *palette = _state->getPalette();
	const double pulse = 0.5 + 0.5 * std::sin(now * 0.006);
	const Uint8 edge = nearestColor(palette, 50, 15, 90);
	const Uint8 fill = hover ? nearestColor(palette, 235, 190, 255) : nearestColor(palette, (int)(150 + 50 * pulse), (int)(60 + 40 * pulse), 255);
	const Uint8 rim = hover ? nearestColor(palette, 255, 235, 255) : nearestColor(palette, 210, 150, 255);

	// The chevron's shape: two arms, each a band reaching 2.2 pixels above and below a line that
	// falls from the apex (in the middle of the top row, for an up arrow) towards the bottom
	// corners. A pixel (x, y in the surface) is inside if its row, counted from the apex end, is
	// that close to the line's row in its column.
	const int width = surface->getWidth(), height = surface->getHeight();
	const double halfWidth = (width - 1) / 2.0, slope = (height - 4) / halfWidth, thickness = 2.2;
	auto inside = [&](int x, int y) {
		if (x < 0 || y < 0 || x >= width || y >= height) return false;
		const double armRow = 1.5 + std::abs(x - halfWidth) * slope; // Counted from the apex end.
		return std::abs((up ? y : height - 1 - y) - armRow) <= thickness;
	};

	// Paint it pixel by pixel: the body, the rim where the pixel ahead (towards where the arrow
	// points) is outside, and the outline around it.
	surface->clear();
	surface->lock();
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			if (inside(x, y))
			{
				const bool leadingEdge = !inside(x, up ? y - 1 : y + 1); // The leading edge catches the light.
				surface->setPixel(x, y, leadingEdge ? rim : fill);
			}
			else if (inside(x - 1, y) || inside(x + 1, y) || inside(x, y - 1) || inside(x, y + 1))
			{
				surface->setPixel(x, y, edge);
			}
		}
	}
	surface->unlock();
}

/**
 * Which lift arrow is at this point of our canvas. Each arrow counts a little beyond its edges
 * (3 pixels to the sides, 5 above and below), so the thin chevrons are easy to hit.
 * @param x The point's x on the canvas, in canvas pixels.
 * @param y The point's y on the canvas, in canvas pixels.
 * @return 1 for the up arrow, -1 for the down arrow, 0 for neither (or a hidden one).
 */
int FirstPersonMode::liftArrowAt(double x, double y) const
{
	auto isOver = [&](const Surface *arrow) {
		return arrow->getVisible() && x >= arrow->getX() - 3 && x < arrow->getX() + arrow->getWidth() + 3
			&& y >= arrow->getY() - 5 && y < arrow->getY() + arrow->getHeight() + 5;
	};
	return isOver(_liftUp) ? 1 : isOver(_liftDown) ? -1 : 0;
}

/**
 * The landing after a fall of one or more levels (a step off a ledge, setting "Falling"): the
 * view jolts, a footfall sounds, and the fall hurts by its height. One level does 2 to 6 points
 * of damage, two levels 12 to 20, three 22 to 30, and so on (10 more for each level). It hits a
 * leg from below: the armor counts for half and isn't worn down, half the power also stuns, and
 * from two levels up the leg may be wounded. Then the game checks for casualties, as after any
 * hit (the fall can kill or knock out).
 * @param levels How many levels the soldier fell.
 */
void FirstPersonMode::fallDamage(BattleUnit *unit, int levels)
{
	// The landing: a jolt (harder from higher up) and a footfall.
	_view->addShake(1.5f + levels);
	playFootsteps(unit, 8.0);
	if (unit->isOut())
	{
		return;
	}

	// The fall as a kind of damage, based on the mod's melee damage (so the armor's resistance
	// to melee applies).
	RuleDamageType fall = *_game->getMod()->getDamageType(DT_MELEE);
	fall.RandomType = DRT_FLAT;     // The power below, as it is (no random spread).
	fall.IgnoreDirection = true;    // The side and body part given below count as they are.
	fall.ArmorEffectiveness = 0.5f;
	fall.ToArmor = 0.0f;
	fall.ToWound = levels >= 2 ? 0.5f : 0.0f;
	fall.ToStun = 0.5f;

	// How hard it hits, then the hit itself: from below (the under armor), on a random leg.
	const int power = levels == 1 ? RNG::generate(2, 6) : levels * 10 - 8 + RNG::generate(0, 8);
	unit->damage(Position(0, 0, 0), power, &fall, _save, BattleActionAttack{}, SIDE_UNDER, RNG::generate(0, 1) ? BODYPART_LEFTLEG : BODYPART_RIGHTLEG);
	_state->_battleGame->checkForCasualties(&fall, BattleActionAttack{}, false, false);
}

/**
 * Can this unit fly at all (or swim, in battles underwater)? That is, does its armor's movement
 * type make it a flyer?
 */
bool FirstPersonMode::canFly(const BattleUnit *unit) const
{
	return unit && unit->getMovementType() == MT_FLY;
}

/**
 * Is flight switched on for this soldier (the flight key or the FLY button)? Until the player
 * switches it for a soldier, one already in the air flies and one on the ground doesn't.
 * Always false for a unit that can't fly.
 */
bool FirstPersonMode::flightOn(const BattleUnit *unit) const
{
	if (!canFly(unit))
	{
		return false;
	}
	auto switchedOn = _flightSwitchedOn.find(unit);
	return switchedOn != _flightSwitchedOn.end() ? switchedOn->second : unit->isFloating();
}

/**
 * Switches flight on or off for a soldier who can fly (the flight key or the FLY button), with a
 * message saying which. Switched off in the air, the soldier comes down to land, a level at a
 * time (see updateFlight). Only in the 3D view.
 */
void FirstPersonMode::toggleFlight(BattleUnit *unit)
{
	// Only for the player's own soldier, on their turn (in the aliens' turn the game's selected
	// unit is the alien acting).
	if (!unit || !isActive() || _save->getSide() != FACTION_PLAYER)
	{
		return;
	}
	// Armor that can't fly (swim, under the sea): a line says so, so the key doesn't seem dead.
	const bool underwater = _save->getDepth() > 0;
	if (!canFly(unit))
	{
		_state->warning(underwater ? "STR_FPS_CANNOT_SWIM" : "STR_FPS_CANNOT_FLY");
		return;
	}

	// Flip the switch; off in the air starts the landing. Whether steps off ledges are allowed
	// changes with it, so the cached step answers are dropped.
	const bool on = !flightOn(unit);
	_flightSwitchedOn[unit] = on;
	_landing = !on && unit->isFloating();
	_landingUnit = _landing ? unit : nullptr;
	_landingResumeTurn = 0;
	std::fill(std::begin(_stepAllowedCache), std::end(_stepAllowedCache), -1);
	_state->warning(on ? (underwater ? "STR_FPS_SWIM_ON" : "STR_FPS_FLIGHT_ON") : (underwater ? "STR_FPS_SWIM_OFF" : "STR_FPS_FLIGHT_OFF"));
}

/**
 * May a soldier who walks go up or down a level from here? The game's own rule for a unit that
 * can't fly (Pathfinding::validateUpDown): a ride between two grav lift floors, or a climb on a
 * ladder (OXCE's grav lift flag on a wall or an object) where nothing is in the way: no floor
 * above going up, none under the soldier going down. A flyer may also fly up or down wherever
 * nothing is in the way; with flight switched off it keeps to this (see moveUpDown).
 * @param position The soldier's tile.
 * @param direction Pathfinding::DIR_UP or Pathfinding::DIR_DOWN.
 * @return True if a walker may make the move.
 */
bool FirstPersonMode::walkerMayMoveUpDown(const Position &position, int direction) const
{
	const Tile *start = _save->getTile(position);
	const Tile *destination = _save->getTile(position + Position(0, 0, direction == Pathfinding::DIR_UP ? 1 : -1));
	if (!start || !destination)
	{
		return false;
	}
	if (start->hasGravLiftFloor() && destination->hasGravLiftFloor())
	{
		return true;
	}
	return start->hasLadder() && (direction == Pathfinding::DIR_UP ? destination->hasNoFloor(_save) : start->hasNoFloor(_save));
}

/**
 * Moves the soldier up or down a level (the up and down keys, a click on a lift arrow, or W and
 * S looking up or down on a lift, see updateClimb) through the battlescape's own up and down
 * buttons, so the game checks and charges the move as on the map: a grav lift ride, a ladder
 * climb, or flying. A soldier who can fly but has flight switched off keeps to what a walker
 * may do (a lift or a ladder, see walkerMayMoveUpDown: never off the top of one into the air),
 * with a message naming the flight key otherwise; one who is in the air all the same comes down
 * to land with the down key (see updateFlight).
 * @param direction Pathfinding::DIR_UP or Pathfinding::DIR_DOWN.
 * @param action The input event, passed on to the button's handler.
 */
void FirstPersonMode::moveUpDown(BattleUnit *unit, int direction, Action *action)
{
	if (!unit || !canAct())
	{
		return;
	}

	// A flyer with flight off: in the air, down lands it (level by level, see updateFlight);
	// otherwise only a walker's moves.
	if (canFly(unit) && !flightOn(unit))
	{
		if (direction == Pathfinding::DIR_DOWN && unit->isFloating())
		{
			_landing = true;
			_landingUnit = unit;
			_landingResumeTurn = 0;
			return;
		}
		if (!walkerMayMoveUpDown(unit->getPosition(), direction))
		{
			_state->warningRaw(_state->tr(_save->getDepth() > 0 ? "STR_FPS_SWIM_OFF_KEY" : "STR_FPS_FLIGHT_OFF_KEY").arg(keyLabel(Options::keyFpsFlight)));
			return;
		}
	}

	// Press the battlescape's up or down button.
	if (direction == Pathfinding::DIR_UP)
	{
		_state->btnUnitUpClick(action);
	}
	else
	{
		_state->btnUnitDownClick(action);
	}
}

/**
 * Flight, every frame. A soldier with flight switched on pays the setting "Flight cost per level"
 * in time units for each level up or down: the game's pathfinding asks us for that cost (see
 * Pathfinding::setFlightCost). A soldier with flight switched off in the air, or who floated off
 * a ledge with it off (see commitStep), comes down a level at a time, as if pressing the down
 * button, until on the ground, out of time units, or blocked.
 * @param unit The soldier we look through, or null when the 3D view isn't showing one.
 */
void FirstPersonMode::updateFlight(BattleUnit *unit)
{
	// Floated off a ledge with flight off: once the step is done, land if still in the air.
	if (_landAfterStep && (!unit || !_stepPending))
	{
		_landAfterStep = false;
		_landing = unit && unit->isFloating() && !flightOn(unit);
		_landingUnit = _landing ? unit : nullptr;
		_landingResumeTurn = 0;
	}

	// The landing is over on the ground, or when flight was switched back on. Looking through
	// another soldier (or the map) only interrupts it: it goes on when the soldier is looked
	// through again, still in the air with flight off. Held up (blocked below, out of time
	// units), it goes on at the next turn.
	if (_landingUnit && (flightOn(_landingUnit) || !_landingUnit->isFloating() || _landingUnit->isOut()))
	{
		_landingUnit = nullptr;
	}
	if (_landing && (!unit || unit != _landingUnit))
	{
		_landing = false;
	}
	if (!_landing && unit && unit == _landingUnit && _save->getTurn() >= _landingResumeTurn)
	{
		_landing = true;
	}
	// In the air with flight switched off all the same, and not coming down (nor held up): land.
	// Flight switched off in the air and floating off a ledge start the landing themselves; this
	// catches any other way up there, so a soldier is never left hanging where no key works.
	if (unit && !_landing && unit != _landingUnit && !_landAfterStep && !_stepPending && canFly(unit) && !flightOn(unit) && unit->isFloating())
	{
		_landing = true;
		_landingUnit = unit;
		_landingResumeTurn = 0;
	}

	// Tell the pathfinding who pays our flight cost for moves up and down (also while landing);
	// nobody (-1) means the game's own costs, as with the setting "Flight cost overrides rules"
	// off (each unit flies at its armor's own cost).
	const bool paysFlightCost = Options::fpsFlightCostOverride && unit && canFly(unit) && (flightOn(unit) || _landing);
	_save->getPathfinding()->setFlightCost(paysFlightCost ? unit : nullptr, paysFlightCost ? Options::fpsFlightCost : -1);

	// Landing: whenever the game is idle and the soldier stands (hovers) ready, one more level down.
	if (_landing && canAct() && unit->getStatus() == STATUS_STANDING && !_state->_battleGame->isBusy())
	{
		Pathfinding *pathfinding = _save->getPathfinding();
		const int downCost = pathfinding->getTUCost(unit->getPosition(), Pathfinding::DIR_DOWN, unit, nullptr, BAM_NORMAL).cost.time;
		if (!pathfinding->validateUpDown(unit, unit->getPosition(), Pathfinding::DIR_DOWN))
		{
			_landing = false; // Something below (another unit?): stay up until the next turn.
			_landingResumeTurn = _save->getTurn() + 1;
		}
		else if (downCost > unit->getTimeUnits())
		{
			_landing = false; // Out of time units: hover here until the next turn.
			_landingResumeTurn = _save->getTurn() + 1;
			_state->warning("STR_NOT_ENOUGH_TIME_UNITS");
		}
		else
		{
			_state->btnUnitDownClick(nullptr);
		}
	}
}

namespace
{

/**
 * A door a one-tile soldier can open from where it stands: the tile of its wall, from the
 * soldier's own, which wall, and the facing that opens it. These are the doors the game's own
 * opening reaches (TileEngine::unitOpensDoor, as for a right-click on the map): the four sides
 * of the soldier's tile, and the walls of the tiles round it that meet its corners. For a door
 * on a side, the facing is the one whose list holds that door alone, so no other door in the
 * list opens instead.
 */
struct ReachableDoor
{
	int x, y;       // the wall's tile, from the soldier's
	TilePart part;  // its west or its north wall
	int direction;  // the facing that opens it (0 north to 7, clockwise)
};
const ReachableDoor ReachableDoors[] = {
	{ 0, 0, O_NORTHWALL, 0 }, { 0, 0, O_WESTWALL, 6 },   // the tile's own north and west sides
	{ 1, 0, O_WESTWALL, 2 },  { 0, 1, O_NORTHWALL, 4 },  // its east and south sides (the next tiles' walls)
	{ 1, -1, O_WESTWALL, 1 }, { 1, 0, O_NORTHWALL, 1 },  // meeting its north-east corner
	{ 1, 1, O_WESTWALL, 3 },  { 1, 1, O_NORTHWALL, 3 },  // its south-east corner
	{ -1, 1, O_NORTHWALL, 5 }, { 0, 1, O_WESTWALL, 5 },  // its south-west corner
	{ 0, -1, O_WESTWALL, 7 }, { -1, 0, O_NORTHWALL, 7 }, // its north-west corner
};

}

/**
 * Opens a door without walking through it (the open door key), like right-clicking it on the
 * map: the door the player points at (the crosshair, or the free cursor), or else the closed one
 * within reach nearest the way the view looks (see doorWithinReach). The soldier turns to it by
 * the game's rules first: for free with free turning (the view stays where it looks); with
 * "Turning costs TU", a paid turn, after which the door opens by itself (openDoorAfterTurn). With
 * no door within reach, the warning line says so. A unit of more than one tile opens the door
 * its facing points at, as before (the game lets no such unit open a swinging door anyway).
 */
void FirstPersonMode::openDoor(BattleUnit *unit)
{
	// (only with the game idle: the turn that opens the door starts at once, before the next
	// frame turns the soldier back to where the view looks)
	if (!unit || !canAct() || unit->getStatus() != STATUS_STANDING || _state->_battleGame->isBusy())
	{
		return;
	}

	if (!unit->isBigUnit())
	{
		const int direction = doorWithinReach(unit);
		if (direction < 0)
		{
			_state->warning("STR_FPS_NO_DOOR_IN_REACH");
			return;
		}
		if (direction != unit->getDirection())
		{
			// Turning by the rules: the game's own turn, at the armor's cost; the door opens once
			// it's done (unless the turn stops, out of time units or at a newly spotted enemy).
			if (Options::fpsTurnCost)
			{
				Position towards;
				Pathfinding::directionToVector(direction, &towards);
				BattleAction turn;
				turn.actor = unit;
				turn.target = unit->getPosition() + towards;
				_doorAfterTurnUnit = unit;
				_doorAfterTurn = direction;
				_state->_battleGame->statePushBack(new UnitTurnBState(_state->_battleGame, turn));
				return;
			}
			// Free turning: the soldier faces the door for free (the next frame it faces where
			// the view looks again, see faceLookDirection).
			unit->setDirection(direction);
		}
	}
	openDoorAhead(unit);
}

/**
 * Which door the open door key opens, as the facing that opens it: the door the player points
 * at, when it's one within the soldier's reach (the ray the crosshair's own, the game's line of
 * fire, meets a door's wall first); or else the closed door within reach nearest the way the
 * view looks, up to DoorPointSlack degrees off it, so looking roughly at a door is enough. The
 * doors within reach are the game's own (ReachableDoors), on the level it opens them on: the one
 * above when the soldier stands high on stairs, as TileEngine::unitOpensDoor has it.
 * @param unit The soldier (one tile).
 * @return The facing (0 north to 7, clockwise), or -1 if there's no closed door within reach.
 */
int FirstPersonMode::doorWithinReach(BattleUnit *unit)
{
	const Position origin = unit->getPosition() + Position(0, 0, unit->getTile()->getTerrainLevel() < -12 ? 1 : 0);
	// Is this door within reach closed? (A UFO door already opening counts as open.)
	auto closedDoor = [&](const ReachableDoor &door)
	{
		const Tile *tile = _save->getTile(origin + Position(door.x, door.y, 0));
		return tile && (tile->isDoor(door.part) || (tile->isUfoDoor(door.part) && !tile->isUfoDoorOpen(door.part)));
	};

	// The door pointed at: the wall the ray meets first, and its tile.
	int pointerX, pointerY;
	pointer(pointerX, pointerY);
	Position hitVoxel;
	int hitType = V_EMPTY;
	aimAt(pointerX, pointerY, unit, hitVoxel, &hitType);
	if (hitType == V_WESTWALL || hitType == V_NORTHWALL)
	{
		const Position wallTile = hitVoxel.toTile();
		const TilePart wall = hitType == V_WESTWALL ? O_WESTWALL : O_NORTHWALL;
		for (const ReachableDoor &door : ReachableDoors)
		{
			if (door.part == wall && origin + Position(door.x, door.y, 0) == wallTile && closedDoor(door))
			{
				return door.direction;
			}
		}
	}

	// Else the nearest one the view looks towards: the door's middle seen from the eye, as a
	// yaw (0 = east, 90 = south, as the camera's), compared with the view's. A west wall runs
	// along its tile's west edge, a north wall along its north edge (16 voxels a tile).
	double eye[3];
	_view->getEye(eye);
	const double viewYaw = _view->getYawDegrees();
	int nearestDirection = -1;
	double nearestOff = DoorPointSlack;
	for (const ReachableDoor &door : ReachableDoors)
	{
		if (!closedDoor(door))
		{
			continue;
		}
		const Position wallTile = origin + Position(door.x, door.y, 0);
		const double doorX = wallTile.x * 16.0 + (door.part == O_WESTWALL ? 0.0 : 8.0);
		const double doorY = wallTile.y * 16.0 + (door.part == O_WESTWALL ? 8.0 : 0.0);
		const double doorYaw = std::atan2(doorY - eye[1], doorX - eye[0]) * 180.0 / Pi;
		const double off = std::abs(std::remainder(doorYaw - viewYaw, 360.0));
		if (off < nearestOff)
		{
			nearestOff = off;
			nearestDirection = door.direction;
		}
	}
	return nearestDirection;
}

/**
 * Opens the door the open door key turned the soldier to (turning by the rules), once the turn
 * is done: when the game is idle again and the soldier faces that way. A turn that stopped
 * short (out of time units, or at a newly spotted enemy, as the game stops it) opens nothing.
 * Another soldier selected meanwhile drops it. Called every frame on the player's turn.
 * @param unit The soldier we look through.
 */
void FirstPersonMode::openDoorAfterTurn(BattleUnit *unit)
{
	if (_doorAfterTurn < 0 || _state->_battleGame->isBusy())
	{
		return;
	}
	const bool turned = unit == _doorAfterTurnUnit && unit->getDirection() == _doorAfterTurn && unit->getStatus() == STATUS_STANDING;
	_doorAfterTurn = -1;
	_doorAfterTurnUnit = nullptr;
	if (turned && canAct())
	{
		openDoorAhead(unit);
	}
}

/**
 * Opens the door the soldier faces, as a right-click on it does on the map: a turn on the spot
 * towards the tile ahead, which the game's turning state (UnitTurnBState) turns into opening
 * the door in front at its usual cost (or says there aren't the time units).
 * @param unit The soldier.
 */
void FirstPersonMode::openDoorAhead(BattleUnit *unit)
{
	// The tile ahead: the soldier's facing (0 north to 7, clockwise) as a step of one tile.
	Position forwardStep;
	Pathfinding::directionToVector(unit->getDirection(), &forwardStep);

	// Turn towards it (already facing it: no turn), through the game's turning state.
	BattleAction turn;
	turn.type = BA_NONE;
	turn.actor = unit;
	turn.target = unit->getPosition() + forwardStep;
	_state->_battleGame->statePushBack(new UnitTurnBState(_state->_battleGame, turn));
}

/**
 * Stops the walk when a new enemy comes into view, as spotting one interrupts a move on the
 * isometric map: the movement keys are ignored until they're all let go (so holding a key
 * doesn't carry the soldier on into fire), and an "enemy spotted" banner shows for 1.5 seconds
 * (see updateBanner). With en passant on (its key) the walk goes on and only the banner shows,
 * as moving with Shift held does on the map. A new enemy shows as a rise in the number of units
 * the soldier has spotted this turn. Called every frame.
 * @param unit The soldier we look through.
 * @param playerTurn Whether it's the player's turn (only then is anything stopped).
 */
void FirstPersonMode::checkSpotted(BattleUnit *unit, bool playerTurn)
{
	// Another soldier, or not the player's turn: start counting afresh from what this soldier has
	// spotted already, with the keys free.
	const size_t spottedCount = unit->getUnitsSpottedThisTurn().size();
	if (unit != _spottingUnit || !playerTurn)
	{
		_spottingUnit = playerTurn ? unit : nullptr;
		_enemiesSpottedCount = spottedCount;
		_movementHeld = false;
		return;
	}

	// More than before: stop, and say so. En passant (its key) only says so, and so does a run
	// where the mod's rules say running walks on past them (see commitStep).
	if (spottedCount > _enemiesSpottedCount)
	{
		const bool walksOn = _enPassantOn || (_sprintOn && _sprintStepRan && Mod::EXTENDED_RUNNING_COST);
		if (!walksOn) _movementHeld = true;
		_spottedUntil = SDL_GetTicks() + 1500;
	}
	_enemiesSpottedCount = spottedCount;
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
