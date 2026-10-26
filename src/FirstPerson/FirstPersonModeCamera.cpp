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
#include "../Battlescape/UnitTurnBState.h"
#include "../Engine/Game.h"
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

	// Sensitivity 1..20 gives 0.05..1 degree per pixel (zoomed in, that much less). Moving the
	// mouse down tilts the view down, unless the mouse is inverted.
	const double degreesPerPixel = std::clamp(Options::fpsMouseSensitivity, 1, 20) * 0.05 / _zoom;
	_view->rotate(mouseDeltaX * degreesPerPixel, (Options::fpsInvertMouse ? mouseDeltaY : -mouseDeltaY) * degreesPerPixel);
}

/**
 * Zooms the view in while the zoom key (U) or, in mouse look, the right button is held (setting
 * "Zoom"): the view glides in to ZoomFactor times closer over ZoomSeconds, and back out when
 * let go, at a steady pace on a log scale (each step magnifies by the same share, so it feels
 * even), slow enough for the SCOPING line's number to be read as it climbs (see updateScope).
 * Only the view changes: the crosshair, the aim, the shot and the hit chance stay as they are.
 * The mouse and the turn keys turn the view that much slower meanwhile (see turnByMouse,
 * turnWithKeys).
 * @param elapsedSeconds Seconds since the last frame.
 */
void FirstPersonMode::updateZoom(double elapsedSeconds)
{
	const Uint8 *keyStates = SDL_GetKeyState(nullptr);
	const bool keyHeld = Options::keyFpsZoom != SDLK_UNKNOWN && keyStates[Options::keyFpsZoom] && !gameModifierHeld();
	const bool zoomedIn = Options::fpsZoom && isActive() && (keyHeld || (_zoomButtonHeld && _mouseLook));
	const double logTarget = zoomedIn ? std::log(ZoomFactor) : 0.0, logStep = std::log(ZoomFactor) * elapsedSeconds / ZoomSeconds;
	const double logZoom = std::log(_zoom);
	_zoom = std::exp(logTarget > logZoom ? std::min(logTarget, logZoom + logStep) : std::max(logTarget, logZoom - logStep));
}

/**
 * The field of view the 3D view shows now: the setting's (50 to 110 degrees), narrowed by the
 * zoom (the half-width's tangent divided by it, so everything looks that many times bigger).
 * @return The horizontal field of view, in degrees.
 */
double FirstPersonMode::fieldOfViewNow() const
{
	const double halfWidthTangent = std::tan(std::clamp(Options::fpsFieldOfView, 50, 110) * Pi / 360.0) / _zoom;
	return std::atan(halfWidthTangent) * 360.0 / Pi;
}

/**
 * Turns the camera with the battlescape's scroll keys (the arrow keys by default), which would
 * otherwise scroll the isometric map; the map isn't listening while it is hidden.
 * @param elapsedSeconds Seconds since the last frame.
 */
void FirstPersonMode::turnWithKeys(double elapsedSeconds)
{
	const Uint8 *keyStates = SDL_GetKeyState(nullptr);
	const double turnSpeed = 90.0 / _zoom; // degrees per second (zoomed in, that much slower)

	// Add up the turn of every key held: left and right turn, up and down tilt.
	double yawDegrees = 0.0, pitchDegrees = 0.0;
	if (keyStates[Options::keyBattleLeft]) yawDegrees -= turnSpeed * elapsedSeconds;
	if (keyStates[Options::keyBattleRight]) yawDegrees += turnSpeed * elapsedSeconds;
	if (keyStates[Options::keyBattleUp]) pitchDegrees += turnSpeed * elapsedSeconds;
	if (keyStates[Options::keyBattleDown]) pitchDegrees -= turnSpeed * elapsedSeconds;

	// A turn key's 45 degrees (comma and period by default) turn at SnapTurnSpeed, and so does a
	// level look (the classic HUD's map level buttons) tilt.
	if (_snapTurnDegrees != 0.0)
	{
		const double snapStep = std::copysign(std::min(std::abs(_snapTurnDegrees), SnapTurnSpeed * elapsedSeconds), _snapTurnDegrees);
		yawDegrees += snapStep;
		_snapTurnDegrees -= snapStep;
	}
	if (_snapPitchDegrees != 0.0)
	{
		const double tiltStep = std::copysign(std::min(std::abs(_snapPitchDegrees), SnapTurnSpeed * elapsedSeconds), _snapPitchDegrees);
		pitchDegrees += tiltStep;
		_snapPitchDegrees -= tiltStep;
	}

	// Turn only when a key is actually held (or a snap turn is under way).
	if (yawDegrees != 0.0 || pitchDegrees != 0.0)
	{
		_view->rotate(yawDegrees, pitchDegrees);
	}
}

/**
 * Looking around turns the soldier to face where the camera looks. Called every frame on the
 * player's turn. Nothing happens while the player can't act, while the soldier isn't simply
 * standing (walking, turning, falling...), or when the selected unit can't be played.
 * - By default turning is free: the unit faces the compass point (one of eight) the camera looks
 *   closest to, and what it sees is worked out again as after a normal turn, only without the
 *   time unit cost.
 * - With the setting "Turning costs TU", it's the game's own turn: the camera may look up to
 *   HeadTurn degrees off the soldier's facing (the head turns, for free); looking further turns
 *   the soldier a step (45 degrees) that way through the game's turning state, the one the
 *   isometric map uses, which spends the armor's turn cost and stops when it spots a new enemy.
 *   Without the time units, the view stops at the head's limit. A turn key (snap turn) turns the
 *   soldier too: once its 45 degrees are turned, the soldier faces the compass point the view
 *   looks closest to. While a shot is under way (ours, a burst, its hits; anyone's round or
 *   blast) the head looks no further than its limit, as without the time units, and turns no
 *   more (a player's report: looking round mid-shot, the soldier turned step by step after it,
 *   paying for each; the owner: hold the head's limit).
 */
void FirstPersonMode::faceLookDirection(BattleUnit *unit)
{
	if (Options::fpsTurnCost && shotUnderWay() && unit->getFaction() == FACTION_PLAYER && !unit->isOut())
	{
		// (held a hair inside the limit: exactly on it, rounding could tip it past once the shot
		// is over, and the soldier would turn a step after all)
		const double facingYaw = unit->getDirection() * 45.0 - 90.0;
		const double offFacing = std::remainder(_view->getYawDegrees() - facingYaw, 360.0);
		const double heldLimit = HeadTurn - 0.1;
		if (std::abs(offFacing) > heldLimit)
		{
			_view->rotate((offFacing > 0.0 ? heldLimit : -heldLimit) - offFacing, 0.0);
		}
		_snapTurnDegrees = 0.0;
		_snapTurnFacing = false;
		return;
	}
	if (!canAct() || unit->getStatus() != STATUS_STANDING || !_state->playableUnitSelected())
	{
		return;
	}

	// Turning by the rules: how far the view looks off the soldier's facing, in degrees
	// (clockwise positive, -180 to 180). The camera's yaw has 0 = east; a unit's direction has
	// 0 = north, clockwise in 45 degree steps.
	if (Options::fpsTurnCost)
	{
		const double facingYaw = unit->getDirection() * 45.0 - 90.0;
		const double offFacing = std::remainder(_view->getYawDegrees() - facingYaw, 360.0);
		const bool snapTurnDone = _snapTurnFacing && _snapTurnDegrees == 0.0;
		if (snapTurnDone && std::abs(offFacing) <= 22.5)
		{
			_snapTurnFacing = false; // the soldier faces where the snap turn looks
		}
		if (std::abs(offFacing) <= (snapTurnDone ? 22.5 : HeadTurn))
		{
			return;
		}
		const int step = offFacing > 0.0 ? 1 : -1;
		const int cost = unit->getTurnCost();
		if (!doomModeOn() && (unit->getTimeUnits() < cost || !reserveAllows(unit, cost, 0)))
		{
			// Not enough time units (or they're reserved for a shot): the view stops at the
			// head's limit, and the game's warning says why (at most every two seconds).
			_view->rotate(step * HeadTurn - offFacing, 0.0);
			_snapTurnDegrees = 0.0;
			_snapTurnFacing = false;
			const Uint32 now = SDL_GetTicks();
			if (now - _turnRefusedWarned > 2000)
			{
				_turnRefusedWarned = now;
				_state->warning("STR_NOT_ENOUGH_TIME_UNITS");
			}
			return;
		}
		// One step that way, through the game's own turning.
		Position towards;
		Pathfinding::directionToVector((unit->getDirection() + step + 8) % 8, &towards);
		BattleAction turn;
		turn.actor = unit;
		turn.target = unit->getPosition() + towards;
		_state->_battleGame->statePushBack(new UnitTurnBState(_state->_battleGame, turn));
		return;
	}

	// Free turning (a snap turn needs nothing more: the soldier always faces where the view
	// looks). The camera's yaw has 0 = east; a unit's direction has 0 = north, clockwise in 45
	// degree steps. Turn the yaw a quarter round, round it to the nearest step and wrap it into
	// 0..7 (the double modulo keeps negative angles in range).
	_snapTurnFacing = false;
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
	if (enemy && !enemy->isOut())
	{
		lookAtUnit(enemy);
	}
}

/**
 * Turns the camera towards a unit: at its chest, about 60% of its height above its feet.
 * @param target The unit to look at (one inside another's inventory, being carried, has no
 *               place on the map: the view stays as it is).
 */
void FirstPersonMode::lookAtUnit(const BattleUnit *target)
{
	double point[3];
	if (!chestPoint(target, point))
	{
		return;
	}
	_snapTurnDegrees = _snapPitchDegrees = 0.0;
	_view->lookAt(point);
}

/**
 * Turns the view towards a unit's chest (see chestPoint), gliding there at the snap turn's pace
 * (see turnWithKeys) rather than jumping: the aliens' turn cameras following an alien.
 * @param target The unit to look at (one off the map leaves the view as it is).
 */
void FirstPersonMode::glideToLookAt(const BattleUnit *target)
{
	double point[3], yawDegrees, pitchDegrees;
	if (!chestPoint(target, point) || !_view->anglesTo(point, yawDegrees, pitchDegrees))
	{
		return;
	}
	_snapTurnDegrees = std::remainder(yawDegrees - _view->getYawDegrees(), 360.0);
	_snapTurnFacing = false;
	_snapPitchDegrees = pitchDegrees - _view->getPitchDegrees();
}

/**
 * The point on a unit the view looks at: its chest, about 60% of its height above its feet.
 * In voxels (a tile is 16 x 16 voxels and a level 24 high): the middle of the unit's footprint
 * (1 or 2 tiles across), raised by the floor it stands on (the terrain level is negative for a
 * raised floor), by its hovering height and to its chest.
 * @param target The unit.
 * @param point Set to the point.
 * @return False for a unit with no place on the map (carried, inside another's inventory).
 */
bool FirstPersonMode::chestPoint(const BattleUnit *target, double point[3]) const
{
	const Position targetTile = target->getPosition();
	if (targetTile == TileEngine::invalid)
	{
		return false;
	}
	const Tile *tile = _save->getTile(targetTile);
	const int unitSize = target->getArmor()->getSize();
	point[0] = targetTile.x * 16.0 + 8.0 * unitSize;
	point[1] = targetTile.y * 16.0 + 8.0 * unitSize;
	point[2] = targetTile.z * 24.0 - (tile ? tile->getTerrainLevel() : 0) + target->getFloatHeight() + target->getHeight() * 0.6;
	return true;
}

/**
 * A right click on one of the visible enemy buttons, as the isometric map has it: the soldier
 * who sees that enemy and has the most of their time units left (as a share) is selected; in
 * 3D their view then turns to the enemy (on the map, the map centred on them).
 * @param enemyIndex Which enemy button, from 0 to BattlescapeState::VISIBLE_MAX - 1.
 */
void FirstPersonMode::lookFromBestSpotter(int enemyIndex)
{
	if (enemyIndex < 0 || enemyIndex >= BattlescapeState::VISIBLE_MAX || !canAct())
	{
		return;
	}
	BattleUnit *enemy = _state->_visibleUnit[enemyIndex];
	if (!enemy)
	{
		return;
	}
	// The game's own choice: of the squad who see the enemy, the one with the largest share of
	// their time units left (the first such, in the squad's order).
	BattleUnit *spotter = nullptr;
	int bestShare = -1;
	for (BattleUnit *unit : *_save->getUnits())
	{
		if (!unit->isSelectable(_save->getSide(), false, false) || !unit->hasVisibleUnit(enemy)) continue;
		const int share = unit->getBaseStats()->tu > 0 ? unit->getTimeUnits() * 100 / unit->getBaseStats()->tu : 0;
		if (share > bestShare)
		{
			bestShare = share;
			spotter = unit;
		}
	}
	if (!spotter)
	{
		return;
	}
	// Selecting another soldier switches the camera to them next frame (see updateCamera),
	// which then turns to the enemy; the soldier already looked through turns at once.
	if (spotter == _save->getSelectedUnit())
	{
		lookAtUnit(enemy);
		return;
	}
	_lookAtOnSwitch = enemy;
	_state->_battleGame->cancelAllActions();
	_state->_battleGame->primaryAction(spotter->getPosition());
}

/**
 * The classic HUD's centre button (and its key, Home by default) in 3D: the map would centre on
 * the soldier, who in 3D is where the camera already is; so the view turns back to where the
 * soldier faces, level, gliding there as a snap turn does.
 * @param unit The soldier looked through.
 */
void FirstPersonMode::lookStraightAhead(const BattleUnit *unit)
{
	// How far the view turns: to the soldier's facing (its direction counts from north,
	// clockwise; the camera's yaw from east), the short way round, and back to level.
	const double facingYaw = unit->getDirection() * 45.0 - 90.0;
	double turn = std::fmod(facingYaw - _view->getYawDegrees(), 360.0);
	if (turn > 180.0) turn -= 360.0;
	if (turn < -180.0) turn += 360.0;
	_snapTurnDegrees = turn;
	_snapTurnFacing = false;
	_snapPitchDegrees = -_view->getPitchDegrees();
}

/**
 * The classic HUD's map level buttons in 3D: the map would show the level above or below; the
 * 3D view shows every level already, so it looks towards it instead: LevelLookDegrees up or
 * down, gliding there, as far as the view tilts.
 * @param direction 1 to look up, -1 to look down.
 */
void FirstPersonMode::lookLevelUpDown(int direction)
{
	_snapPitchDegrees += direction * LevelLookDegrees;
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

	// Whatever the camera was doing is over: no step waiting, no door or gap to walk through,
	// no momentum, no ride or fall (and so no floor left out for one).
	_stepPending = false;
	_guidedStep = false;
	_velocityX = _velocityY = 0.0;
	_rideStart = 0;
	_fallStart = 0;
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
		endLean();
		_snapTurnDegrees = _snapPitchDegrees = 0.0;
		// A tile move (or a numpad press) belonged to the soldier before.
		_tileMove = false;
		_numpadMove[0] = _numpadMove[1] = 0;
		// Picked to look at someone (a right click on an enemy button): turn to them, from this
		// soldier's eye (the view's eye moves there only at the end of the frame).
		if (_lookAtOnSwitch)
		{
			_view->setEye(_eyeX, _eyeY, _eyeZ);
			lookAtUnit(_lookAtOnSwitch);
			_lookAtOnSwitch = nullptr;
		}
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
		// Up or down a level where it stands. A lift ride is already gliding, and a fall dropping
		// (see below); anything else (a landing) glides too, quicker, through the upper level's
		// floor (left out of the picture meanwhile, so the eye doesn't pass through it).
		if (!_rideStart && !_fallStart)
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
	// A fall (the game's: a step into the air, the floor gone under the soldier) drops instead, as
	// by gravity, from where the eye is (see below).
	const int verticalDirection = unit->getVerticalDirection();
	const bool falling = isFalling(unit);
	if (falling && !_fallStart)
	{
		const Position unitPosition = unit->getPosition();
		_view->setHiddenFloor(_save->getTileIndex(unitPosition));
		_fallStart = SDL_GetTicks();
		_fallEyeFrom = _eyeZ;
		_rideStart = 0;
	}
	else if (verticalDirection && !_rideVertical && !_fallStart)
	{
		const int rideDirection = verticalDirection == Pathfinding::DIR_UP ? 1 : -1; // 1 up, -1 down
		const Position unitPosition = unit->getPosition();
		_view->setHiddenFloor(_save->getTileIndex(Position(unitPosition.x, unitPosition.y, unitPosition.z + (rideDirection > 0 ? 1 : 0))));
		_rideStart = SDL_GetTicks();
		_rideTime = rideTime();
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
	if (_fallStart)
	{
		// Falling: down by g t^2 / 2, g such that the first level takes a ride's time (by the walk
		// speed); at most a level ahead of the game, which moves the unit a level at a time
		// (the eye waits for it there), and only while there's no floor under the unit yet (no
		// further than where it lands). Landed once the game has stopped the fall and the eye is
		// down: the view dips.
		const double seconds = (SDL_GetTicks() - _fallStart) / 1000.0;
		const double levelSeconds = rideTime() / 1000.0;
		const double gravity = 2.0 * Position::TileZ / (levelSeconds * levelSeconds); // voxels per second squared
		const double lowest = targetEyeZ - (falling && unit->haveNoFloorBelow() ? Position::TileZ : 0.0);
		_eyeZ = std::max(lowest, _fallEyeFrom - 0.5 * gravity * seconds * seconds);
		if (!falling && _eyeZ <= targetEyeZ + 0.01)
		{
			_eyeZ = targetEyeZ;
			_fallStart = 0;
			_landedAt = SDL_GetTicks();
			_view->setHiddenFloor(-1);
		}
	}
	else if (rideProgress < 1.0)
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

	// Leaning out (setting "Lean"): the eye goes to the side, a little lower.
	double leanDrop = 0.0;
	updateLean(unit, canMove, elapsedSeconds, leanDrop);

	// Place the eye: the lunge goes the way the camera looks, the lean to its side, the bob, a
	// jump and the lean's drop up and down; kept out of anything solid (see clearEye).
	const double yawRadians = _view->getYawDegrees() * Pi / 180.0;
	double eye[3] = { _eyeX + std::cos(yawRadians) * lunge + _leanShift[0], _eyeY + std::sin(yawRadians) * lunge + _leanShift[1],
		_eyeZ + headBob + currentJumpHeight(unit) - leanDrop - landingDip() };
	clearEye(unit, eye, yawRadians);
	_view->setEye(eye[0], eye[1], eye[2]);
}

/**
 * Keeps the eye out of anything solid (a player's report: a 2x2 unit standing in a door two tiles
 * wide was blinded, in XPiratez's temple raid). A big unit's eye is the middle of its footprint,
 * where its four tiles meet: right where a wide door's frame or middle post stands (walls lie on
 * the tiles' edges), and the 3D view drew from inside it. When the eye's voxel is solid (the
 * game's own voxels, units left out), it moves to the nearest free one along the ground, looked
 * for a voxel at a time up to EyeClearReach: ahead the way the view looks first, then to either
 * side, then behind; it stays inside the unit's footprint. Nothing is found: it stays.
 * @param unit The unit looked through.
 * @param eye The eye, in voxels (moved if it must).
 * @param yawRadians The way the view looks, along the ground.
 */
void FirstPersonMode::clearEye(BattleUnit *unit, double eye[3], double yawRadians) const
{
	TileEngine *tileEngine = _save->getTileEngine();
	auto solidAt = [&](double x, double y)
	{
		return tileEngine->voxelCheck(Position((int)std::floor(x), (int)std::floor(y), (int)std::floor(eye[2])), unit, true) != V_EMPTY;
	};
	if (!solidAt(eye[0], eye[1]))
	{
		return;
	}
	// The footprint, in voxels (a tile's edge margin in from its sides).
	const Position tile = unit->getPosition();
	const double size = unit->getArmor()->getSize() * 16.0;
	const double minX = tile.x * 16.0 + EdgeMargin, maxX = tile.x * 16.0 + size - EdgeMargin;
	const double minY = tile.y * 16.0 + EdgeMargin, maxY = tile.y * 16.0 + size - EdgeMargin;
	const double forward[2] = { std::cos(yawRadians), std::sin(yawRadians) };
	const double ways[4][2] = { { forward[0], forward[1] }, { -forward[1], forward[0] }, { forward[1], -forward[0] }, { -forward[0], -forward[1] } };
	for (double distance = 1.0; distance <= EyeClearReach; distance += 1.0)
	{
		for (const auto &way : ways)
		{
			const double x = eye[0] + way[0] * distance, y = eye[1] + way[1] * distance;
			if (x < minX || x > maxX || y < minY || y > maxY || solidAt(x, y))
			{
				continue;
			}
			eye[0] = x;
			eye[1] = y;
			return;
		}
	}
}

/**
 * How long going up or down a level takes (a grav lift, a ladder, flying, a fall): as long as
 * walking RideTiles tiles at RidePace of the walk speed setting (1 to 8 tiles a second).
 * @return The time, in milliseconds.
 */
Uint32 FirstPersonMode::rideTime() const
{
	const double pace = std::clamp(Options::fpsWalkSpeed, 1, 8) * RidePace; // tiles a second
	return (Uint32)std::lround(1000.0 * RideTiles / pace);
}

/**
 * Is the game making this unit fall: a step off a ledge into the air (see the steps), or the
 * floor gone from under it? Riding a lift or a ladder, or flying up and down (a flying suit's
 * landing a level at a time too), isn't a fall.
 * @param unit The unit.
 * @return True if it's among the game's falling units.
 */
bool FirstPersonMode::isFalling(BattleUnit *unit) const
{
	const std::list<BattleUnit*> *fallingUnits = _save->getFallingUnits();
	return std::find(fallingUnits->begin(), fallingUnits->end(), unit) != fallingUnits->end();
}

/**
 * How far the view dips just after a fall has landed: down and back up over LandingDipTime, at
 * most LandingDip voxels.
 * @return The dip, in voxels (0 when not just landed).
 */
double FirstPersonMode::landingDip() const
{
	const Uint32 sinceLanding = SDL_GetTicks() - _landedAt;
	if (!_landedAt || sinceLanding >= LandingDipTime)
	{
		return 0.0;
	}
	return LandingDip * std::sin(Pi * sinceLanding / (double)LandingDipTime);
}

/**
 * Where the 3D camera's eye is, as a height in voxels: the game's sight origin (where its own
 * line of sight starts), lowered a little by the setting "Eye height lowered by" (0..6 voxels,
 * half that when kneeling). X-COM soldiers fill a level almost to the ceiling, so the view sits a bit
 * lower to give rooms some headroom; the game's own line of sight stays where it is. But where
 * the game has pushed its sight origin down under a floor the soldier's head would reach (one
 * standing on something tall under a ceiling, see TileEngine::getSightOriginVoxel), the view
 * stays close to the game's own eye, only PressedEyeLower lower: lowered by the setting too, it
 * sank onto what the soldier stands on, the view a slit between that and the ceiling (a player's
 * report on 1.03).
 * @return The eye's height above the bottom of the map, in voxels.
 */
double FirstPersonMode::eyeHeight(const BattleUnit *unit) const
{
	// The game's sight origin, and its height with no floor above (as the game works it out).
	const int sightZ = _save->getTileEngine()->getSightOriginVoxel(const_cast<BattleUnit*>(unit)).z;
	const int unpressedZ = unit->getPosition().z * Position::TileZ - unit->getTile()->getTerrainLevel()
		+ unit->getHeight() + unit->getFloatHeight() - 1 + (unit->isBigUnit() ? 1 : 0);
	const bool pressedUnderFloor = unpressedZ > sightZ;
	const double loweredBy = pressedUnderFloor ? PressedEyeLower : std::clamp(Options::fpsEyeLower, 0, 6) * (unit->isKneeled() ? 0.5 : 1.0);
	// The middle of the sight origin's voxel (+ 0.5), lowered.
	return sightZ + 0.5 - loweredBy;
}

/**
 * Leaning out (setting "Lean": Peek, or Tilt): while the lean key is held with a strafe key (Left
 * Shift with A or D), or a side's own lean key is held, the soldier leans out that way, over
 * LeanTime: the eye moves up to LeanReach voxels to the side of where the view
 * looks, and up to LeanDrop lower, as a head bends; with Tilt the view tilts up to
 * LeanTiltDegrees too. The eye stops LeanWallMargin short of anything solid or anyone in sight
 * (see leanRoom), so it never looks into a wall or a teammate's head, and comes back in at once
 * when either gets closer (walking on while leaning). Letting go straightens up again. The soldier stays where they are, and so does the
 * game's sight: the lean shows what is round the corner, but who is seen there is still up to
 * the game. A shot fired while leaning out starts from that side (see fire and leanSide).
 * @param unit The soldier looked through.
 * @param canLean Whether the keys may lean now (the player's turn, a soldier of theirs).
 * @param elapsedSeconds Seconds since the last frame.
 * @param dropBy Set to how much lower the eye is, in voxels.
 */
void FirstPersonMode::updateLean(BattleUnit *unit, bool canLean, double elapsedSeconds, double &dropBy)
{
	// Which way the keys lean: -1 left, 1 right, 0 for neither or both (or leaning off).
	const Uint8 *keyStates = SDL_GetKeyState(nullptr);
	auto held = [keyStates](SDLKey key) { return key != SDLK_UNKNOWN && keyStates[key]; };
	double wanted = 0.0;
	if (Options::fpsLean > 0 && canLean && !unit->isOut())
	{
		const bool leanKey = held(Options::keyFpsLeanHold);
		const bool right = held(Options::keyFpsLeanRightOwn) || (leanKey && held(Options::keyFpsStrafeRight));
		const bool left = held(Options::keyFpsLeanLeftOwn) || (leanKey && held(Options::keyFpsStrafeLeft));
		wanted = (right ? 1.0 : 0.0) - (left ? 1.0 : 0.0);
	}
	// Into the lean (or out of it) at a steady pace, eased in and out.
	_lean += std::clamp(wanted - _lean, -elapsedSeconds / LeanTime, elapsedSeconds / LeanTime);
	const double progress = std::abs(_lean);
	const double amount = std::copysign(progress * progress * (3.0 - 2.0 * progress), _lean); // -1 .. 1
	dropBy = 0.0;
	if (amount == 0.0 && _leanOffset == 0.0)
	{
		endLean(); // standing straight
		return;
	}

	// The eye's way to the side, the view's right along the ground, and the room there is each
	// way. It goes out towards the lean as far as there is room, never quicker than a lean goes
	// (passing a wall while leaning doesn't let it jump out), but back in at once when a wall
	// comes closer.
	const double yawRadians = _view->getYawDegrees() * Pi / 180.0;
	const double rightX = -std::sin(yawRadians), rightY = std::cos(yawRadians);
	const double roomRight = leanRoom(unit, rightX, rightY), roomLeft = leanRoom(unit, -rightX, -rightY);
	const double wantedOffset = std::clamp(amount * LeanReach, -roomLeft, roomRight);
	const double step = 2.0 * elapsedSeconds * LeanReach / LeanTime; // twice a lean's own top speed
	_leanOffset = std::clamp(std::clamp(wantedOffset, _leanOffset - step, _leanOffset + step), -roomLeft, roomRight);
	_leanShift[0] = rightX * _leanOffset;
	_leanShift[1] = rightY * _leanOffset;
	dropBy = std::abs(_leanOffset) / LeanReach * LeanDrop;
	// The tilt follows the keys, against a wall too (positive tilts the head left).
	_view->setRoll(Options::fpsLean >= 2 ? -amount * LeanTiltDegrees : 0.0);
}

/**
 * How far the eye can lean out one way: up to LeanReach voxels, stopping LeanWallMargin short of
 * the first solid voxel at eye height on the way, looked for every LeanCheckStep voxels: a wall,
 * an object, the map's edge, or a unit in sight (not the soldier, and not one the player can't
 * see, which would give it away).
 * @param unit The soldier leaning.
 * @param sideX, sideY The way, along the ground (a unit vector; x east, y south).
 * @return The room, in voxels (0: none).
 */
double FirstPersonMode::leanRoom(BattleUnit *unit, double sideX, double sideY) const
{
	TileEngine *tileEngine = _save->getTileEngine();
	for (double distance = LeanCheckStep; distance <= LeanReach + LeanWallMargin; distance += LeanCheckStep)
	{
		const Position voxel((int)std::floor(_eyeX + sideX * distance), (int)std::floor(_eyeY + sideY * distance), (int)std::floor(_eyeZ));
		if (tileEngine->voxelCheck(voxel, unit, false, true) != V_EMPTY)
		{
			return std::max(0.0, distance - LeanCheckStep - LeanWallMargin); // from the last free point
		}
	}
	return LeanReach;
}

/**
 * Straightens up at once: no lean, no offset, no tilt. When the camera goes to another soldier,
 * and when a fall takes the view over.
 */
void FirstPersonMode::endLean()
{
	_lean = _leanOffset = 0.0;
	_leanShift[0] = _leanShift[1] = 0.0;
	_view->setRoll(0.0);
}

/**
 * Whether the soldier leans out far enough (more than LeanThreshold voxels) for a shot to start
 * from that side.
 */
bool FirstPersonMode::leaningOut() const
{
	return std::abs(_leanOffset) > LeanThreshold;
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
