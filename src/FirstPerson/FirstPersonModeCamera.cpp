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
 * FirstPersonModeCamera.cpp: the camera of the first-person mode.
 *
 * Turning with the mouse and the keys, turning the soldier to face where the camera looks,
 * turning towards a spotted enemy, and keeping the eye at the soldier's head as they walk,
 * kneel, ride lifts and fly.
 */
#include "FirstPersonMode.h"
#include <algorithm>
#include <cmath>
#include "FirstPersonView.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/Pathfinding.h"
#include "../Battlescape/TileEngine.h"
#include "../Battlescape/UnitFallBState.h"
#include "../Engine/Options.h"
#include "../Mod/Armor.h"
#include "../Mod/MapData.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "FirstPersonModeShared.h"

namespace OpenXcom
{

/**
 * Turns the camera by a relative mouse movement (mouse look): sideways turns, up and down
 * tilts (at most 80 degrees either way, see FirstPersonView::rotate). The soldier turns with
 * the camera afterwards, for free (see faceLookDirection). The settings "Mouse sensitivity" and
 * "Invert mouse" apply.
 * @param mouseDeltaX Mouse movement to the right, in pixels.
 * @param mouseDeltaY Mouse movement down, in pixels.
 */
void FirstPersonMode::turnByMouse(int mouseDeltaX, int mouseDeltaY)
{
	// A single huge jump (the cursor warped, a hiccup) must not spin the view round: cap it.
	mouseDeltaX = std::clamp(mouseDeltaX, -100, 100);
	mouseDeltaY = std::clamp(mouseDeltaY, -100, 100);

	// Sensitivity 1..20 gives 0.05..1 degree per pixel. Moving the mouse down tilts the view
	// down, unless the mouse is inverted.
	const double degreesPerPixel = std::clamp(Options::fpsMouseSensitivity, 1, 20) * 0.05;
	_view->rotate(mouseDeltaX * degreesPerPixel, (Options::fpsInvertMouse ? mouseDeltaY : -mouseDeltaY) * degreesPerPixel);
}

/**
 * Turns the camera with the battlescape's scroll keys (the arrow keys by default), which would
 * otherwise scroll the isometric map; the map isn't listening while it is hidden.
 * @param elapsedSeconds Seconds since the last frame.
 */
void FirstPersonMode::turnWithKeys(double elapsedSeconds)
{
	const Uint8 *keyStates = SDL_GetKeyState(nullptr);
	const double turnSpeed = 90.0; // degrees per second

	// Add up the turn of every key held: left and right turn, up and down tilt.
	double yawDegrees = 0.0, pitchDegrees = 0.0;
	if (keyStates[Options::keyBattleLeft]) yawDegrees -= turnSpeed * elapsedSeconds;
	if (keyStates[Options::keyBattleRight]) yawDegrees += turnSpeed * elapsedSeconds;
	if (keyStates[Options::keyBattleUp]) pitchDegrees += turnSpeed * elapsedSeconds;
	if (keyStates[Options::keyBattleDown]) pitchDegrees -= turnSpeed * elapsedSeconds;

	// Turn only when a key is actually held.
	if (yawDegrees != 0.0 || pitchDegrees != 0.0)
	{
		_view->rotate(yawDegrees, pitchDegrees);
	}
}

/**
 * Looking around turns the soldier for free: the unit faces the compass point (one of eight)
 * the camera looks closest to, and what it sees is worked out again as after a normal turn,
 * only without the time unit cost. Called every frame on the player's turn.
 * Nothing happens while the player can't act, while the soldier isn't simply standing (walking,
 * turning, falling...), or when the selected unit can't be played.
 */
void FirstPersonMode::faceLookDirection(BattleUnit *unit)
{
	if (!canAct() || unit->getStatus() != STATUS_STANDING || !_state->playableUnitSelected())
	{
		return;
	}

	// The camera's yaw has 0 = east; a unit's direction has 0 = north, clockwise in 45 degree
	// steps. Turn the yaw a quarter round, round it to the nearest step and wrap it into 0..7
	// (the double modulo keeps negative angles in range).
	const int direction = (((int)std::lround((_view->getYawDegrees() + 90.0) / 45.0)) % 8 + 8) % 8;
	if (direction == unit->getDirection())
	{
		return;
	}

	// Turn the soldier, then update what it sees, what others see of it (as UnitTurnBState does
	// after a turn) and the classic HUD's soldier info.
	unit->setDirection(direction);
	TileEngine *tileEngine = _save->getTileEngine();
	tileEngine->calculateFOV(unit);
	tileEngine->calculateFOV(unit->getPosition(), 1, false); // others' view of this unit, like UnitTurnBState
	_state->updateSoldierInfo(false);
}

/**
 * Turns the camera towards one of the visible enemies: the red numbered indicators of the
 * classic HUD (bottom right), picked by their key or a click. The camera aims at the enemy's
 * chest, about 60% of its height above its feet.
 * @param enemyIndex Which indicator, from 0 (the first) to BattlescapeState::VISIBLE_MAX - 1.
 */
void FirstPersonMode::lookAtEnemy(int enemyIndex)
{
	// Only an indicator that exists and shows an enemy still standing.
	if (enemyIndex < 0 || enemyIndex >= BattlescapeState::VISIBLE_MAX)
	{
		return;
	}
	BattleUnit *enemy = _state->_visibleUnit[enemyIndex];
	if (!enemy || enemy->isOut())
	{
		return;
	}

	// The point to look at, in voxels (a tile is 16 x 16 voxels and a level 24 high): the middle
	// of the enemy's footprint (1 or 2 tiles across), raised by the floor it stands on (the
	// terrain level is negative for a raised floor), by its hovering height and to its chest.
	const Position enemyTile = enemy->getPosition();
	const Tile *tile = _save->getTile(enemyTile);
	const int unitSize = enemy->getArmor()->getSize();
	const double target[3] = {
		enemyTile.x * 16.0 + 8.0 * unitSize,
		enemyTile.y * 16.0 + 8.0 * unitSize,
		enemyTile.z * 24.0 - (tile ? tile->getTerrainLevel() : 0) + enemy->getFloatHeight() + enemy->getHeight() * 0.6 };
	_view->lookAt(target);
}

/**
 * Puts the camera in the middle of the unit's tile (or of its 2x2 footprint for a big unit),
 * at eye height, and forgets any step, door, momentum or lift ride in progress. Used when the
 * camera starts following a unit, or the unit got somewhere without the camera walking it.
 */
void FirstPersonMode::snapToTile(BattleUnit *unit)
{
	// The middle of the footprint, in voxels (16 per tile).
	const Position unitTile = unit->getPosition();
	const int unitSize = unit->getArmor()->getSize();
	_eyeX = unitTile.x * 16.0 + 8.0 * unitSize;
	_eyeY = unitTile.y * 16.0 + 8.0 * unitSize;
	_eyeZ = eyeHeight(unit);
	_trackedTile = unitTile;

	// Whatever the camera was doing is over: no step waiting, no door, no momentum, no ride
	// (and so no floor left out for one).
	_stepPending = false;
	_stepThroughDoor = false;
	_velocityX = _velocityY = 0.0;
	_rideStart = 0;
	_view->setHiddenFloor(-1);
}

/**
 * Keeps the camera on a unit, once per frame while the 3D view shows: starts following a new
 * unit, finishes a step the game has walked (including a fall off a ledge, a vault cut short and
 * a step that didn't happen), glides the eye up or down a level for lifts, flying and falls,
 * re-places the camera when the unit got somewhere some other way, lets updateMovement walk it
 * with the keys, and finally places the eye with the head bob, a melee lunge and a jump on top.
 * @param unit The unit being looked through.
 * @param canMove Whether the movement keys may walk the unit (it's the player's turn).
 * @param elapsedSeconds Seconds since the last frame.
 */
void FirstPersonMode::updateCamera(BattleUnit *unit, bool canMove, double elapsedSeconds)
{
	const Position unitTile = unit->getPosition();

	// Work out where the camera stands, by what the unit has been doing since the last frame.
	if (unit != _trackedUnit)
	{
		// A different unit (newly selected, or the view was just switched on): look through it,
		// the way it faces, from the middle of its tile.
		_trackedUnit = unit;
		_view->setCameraUnit(unit);
		_view->lookAlong(unit->getDirection());
		endVault();
		snapToTile(unit);
	}
	else if (_stepPending)
	{
		// A step we asked the game to walk: once the game is done with it, see how it went.
		if (!_state->_battleGame->isBusy())
		{
			_stepPending = false;
			_save->getPathfinding()->setVault(nullptr, {});
			if (_fallFromZ >= 0 && unit->haveNoFloorBelow() && unit->getMovementType() != MT_FLY && !unit->isOut())
			{
				// The soldier stepped off a ledge into the air: the game's own fall takes them
				// down, and the camera waits for the landing as for another step.
				_save->addFallingUnit(unit);
				_state->_battleGame->statePushFront(new UnitFallBState(_state->_battleGame));
				_stepPending = true; // wait for the landing
				_trackedTile = unitTile;
				return updateCamera(unit, canMove, elapsedSeconds);
			}
			if (_fallFromZ >= 0)
			{
				// Landed after stepping off a ledge: fall damage for every level fallen.
				const int levelsFallen = _fallFromZ - unitTile.z;
				_fallFromZ = -1;
				if (levelsFallen > 0) fallDamage(unit, levelsFallen);
			}
			if (_vaulting && (unitTile.x != (int)std::floor(_vaultTo[0] / 16.0) || unitTile.y != (int)std::floor(_vaultTo[1] / 16.0)))
			{
				// The vault was cut short (reaction fire, out of energy, ...): the camera goes
				// back to where the soldier is.
				endVault();
				snapToTile(unit);
			}
			if (unitTile.x == _stepFrom.x && unitTile.y == _stepFrom.y)
			{
				// The step didn't happen (out of time units, an unseen unit in the way, ...): the
				// camera, which had walked on towards the edge, goes back inside the old tile.
				const int bigUnitOffset = 8 * (unit->getArmor()->getSize() - 1); // a 2x2 unit's middle is half a tile further on
				_eyeX = clampInsideTile(unitTile.x, _eyeX, bigUnitOffset);
				_eyeY = clampInsideTile(unitTile.y, _eyeY, bigUnitOffset);
			}
			_trackedTile = unitTile;
		}
	}
	else if (unitTile.x == _trackedTile.x && unitTile.y == _trackedTile.y && unitTile.z != _trackedTile.z)
	{
		// Up or down a level where it stands. A lift ride is already gliding (see below); a fall
		// or a landing glides too, quicker, through the upper level's floor (left out of the
		// picture meanwhile, so the eye doesn't pass through it).
		if (!_rideStart)
		{
			_view->setHiddenFloor(_save->getTileIndex(Position(unitTile.x, unitTile.y, std::max(unitTile.z, _trackedTile.z))));
			_rideStart = SDL_GetTicks();
			_rideTime = DropTime;
			_rideFrom = _eyeZ;
			_rideTo = _eyeZ + (unitTile.z - _trackedTile.z) * 24.0; // a level is 24 voxels high
			_rideLevel = _trackedTile.z;
		}
		_trackedTile = unitTile;
	}
	else if (unitTile != _trackedTile)
	{
		snapToTile(unit); // moved some other way: an order on the isometric map, falling, ...
	}

	// A lift ride (or flying up or down) glides the eye from the moment it starts, slowly. The
	// game moves the unit up half way through its own short move; the eye takes its time. The
	// floor between the two levels is left out of the picture during the ride.
	const int verticalDirection = unit->getVerticalDirection();
	if (verticalDirection && !_rideVertical)
	{
		const int rideDirection = verticalDirection == Pathfinding::DIR_UP ? 1 : -1; // 1 up, -1 down
		const Position unitPosition = unit->getPosition();
		_view->setHiddenFloor(_save->getTileIndex(Position(unitPosition.x, unitPosition.y, unitPosition.z + (rideDirection > 0 ? 1 : 0))));
		_rideStart = SDL_GetTicks();
		_rideTime = RideTime;
		_rideFrom = _eyeZ;
		_rideTo = _eyeZ + rideDirection * 24.0;
		_rideLevel = unitPosition.z;
	}
	_rideVertical = verticalDirection != 0;

	// Walk with the movement keys. Which steps are allowed is asked afresh every frame.
	std::fill(std::begin(_stepAllowedCache), std::end(_stepAllowedCache), -1);
	updateMovement(unit, canMove, elapsedSeconds);

	// The eye height: during a ride, eased in and out between the two levels; otherwise it
	// settles quickly on the unit's eye height (kneeling and standing up glide).
	const double targetEyeZ = eyeHeight(unit);
	const double rideProgress = _rideStart ? (SDL_GetTicks() - _rideStart) / (double)std::max<Uint32>(1, _rideTime) : 1.0;
	if (rideProgress < 1.0)
	{
		// Towards the new level's eye height once the unit is there, the estimate until then.
		const double goalEyeZ = unit->getPosition().z != _rideLevel ? targetEyeZ : _rideTo;
		_eyeZ = _rideFrom + (goalEyeZ - _rideFrom) * rideProgress * rideProgress * (3.0 - 2.0 * rideProgress); // eased in and out
	}
	else
	{
		// The ride is over (or there was none): show the floor again, and close in on the eye
		// height by a share of the gap each frame.
		if (_rideStart) _view->setHiddenFloor(-1);
		_rideStart = 0;
		_eyeZ += (targetEyeZ - _eyeZ) * std::min(1.0, elapsedSeconds * 12.0);
	}

	// Head bob (setting "Head bob"): a gentle dip of up to 0.6 voxels with every step (footsteps
	// fall every 8 voxels walked), fading in when walking starts and out when it stops.
	const bool walking = std::abs(_velocityX) + std::abs(_velocityY) > 4.0;
	_headBobAmount += ((walking && Options::fpsHeadBob ? 1.0 : 0.0) - _headBobAmount) * std::min(1.0, elapsedSeconds * 8.0);
	const double headBob = (std::cos(_headBobPhase * 2.0 * Pi / 8.0) - 1.0) * 0.3 * _headBobAmount;

	// A melee swing lunges the view forward a little (in voxels; the wind-up pulls it back),
	// and the blow lands with a jolt, once per swing.
	double windup = 0.0, strike = 0.0, lunge = 0.0;
	if (swingPhase(windup, strike))
	{
		lunge = 2.0 * strike - 1.0 * windup;
		if (strike > 0.9 && !_swingJolted)
		{
			_swingJolted = true;
			_view->addShake(1.4f);
		}
	}

	// Place the eye: the lunge goes the way the camera looks, the bob and a jump go up and down.
	const double yawRadians = _view->getYawDegrees() * Pi / 180.0;
	_view->setEye(_eyeX + std::cos(yawRadians) * lunge, _eyeY + std::sin(yawRadians) * lunge, _eyeZ + headBob + currentJumpHeight(unit));
}

/**
 * Where the 3D camera's eye is, as a height in voxels: the game's sight origin (where its own
 * line of sight starts), lowered a little by the setting "Eye height lowered by" (0..6 voxels,
 * half that when kneeling). X-COM soldiers fill a level almost to the ceiling, so the view sits a bit
 * lower to give rooms some headroom; the game's own line of sight stays where it is.
 * @return The eye's height above the bottom of the map, in voxels.
 */
double FirstPersonMode::eyeHeight(const BattleUnit *unit) const
{
	const double loweredBy = std::clamp(Options::fpsEyeLower, 0, 6) * (unit->isKneeled() ? 0.5 : 1.0);
	// The middle of the sight origin's voxel (+ 0.5), lowered.
	return _save->getTileEngine()->getSightOriginVoxel(const_cast<BattleUnit*>(unit)).z + 0.5 - loweredBy;
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
