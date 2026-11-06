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
 * FirstPersonChains.h: setting "Instant chain explosions": what the 3D views ask of a chain of
 * explosions settled at once (see FirstPersonChains.cpp): its fireballs rippling through it, the
 * map as it was where the ripple hasn't passed yet, and its dead standing until it has.
 */
#include <vector>
#include <SDL.h>
#include "../Battlescape/Position.h"

namespace OpenXcom
{

class BattleUnit;
class MapData;
class SavedBattleGame;

namespace FirstPerson
{

/// One sprite of a chain's fireballs as it shows now: a frame of the game's explosion sprites
/// (X1.PCK), where the game's own blast would scatter it (ExplosionBState::init).
struct ChainFireball
{
	Position position; // in voxels
	int frame;         // the frame shown now
	int lastFrame;     // its blast's last frame (it doesn't dissolve past it)
	float blend;       // how far it is into its next frame, 0..1
	Uint32 serial;     // counts every fireball sprite, so a view knows the ones new since its last frame
};

/// A tile as it was before a chain went off, shown so until the chain's ripple passes over it: its
/// parts and its fire (its smoke is the game's as ever).
struct ChainHeldTile
{
	const MapData *parts[4]; // floor, west wall, north wall, object (null where none)
	int fire;                // its fire (the game's number of turns)
	Uint32 revealAt;         // when the ripple passes (SDL ticks): from then on, the tile as the battle has it
};

/// The tiles a chain's ripple hasn't passed over yet, as they were (see chainHeldTiles): taken
/// once a frame by a view, each tile then looked up at no cost.
class ChainHeldTiles
{
public:
	ChainHeldTiles() = default;
	ChainHeldTiles(const std::vector<int> *entries, const std::vector<ChainHeldTile> *tiles, Uint32 now) : _entries(entries), _tiles(tiles), _now(now) { }
	/// The tile as it was, while the ripple hasn't passed over it; null for any other.
	const ChainHeldTile *at(int tileIndex) const
	{
		if (!_entries || tileIndex < 0 || tileIndex >= (int)_entries->size())
		{
			return nullptr;
		}
		const int entry = (*_entries)[tileIndex];
		return entry >= 0 && _now < (*_tiles)[entry].revealAt ? &(*_tiles)[entry] : nullptr;
	}
private:
	const std::vector<int> *_entries = nullptr;          // per tile: its entry in _tiles, or -1 (none: nothing held)
	const std::vector<ChainHeldTile> *_tiles = nullptr; // the tiles held
	Uint32 _now = 0;                                    // the frame's time
};

/// The fireballs of a chain's ripple showing now, in the order they started (none without one).
void chainFireballs(const SavedBattleGame *save, std::vector<ChainFireball> &fireballs);
/// Is a chain's ripple still passing (its dead not fallen yet)?
bool chainRippleShowing(const SavedBattleGame *save);
/// The tiles a chain's ripple hasn't passed over yet, as they were (none without one).
ChainHeldTiles chainHeldTiles(const SavedBattleGame *save);
/// The facing a unit the chain took down had before it, to be drawn standing so until the ripple
/// has passed (the dead fall after it); -1 for any other unit. Only one the player saw then.
int chainHeldStanding(const SavedBattleGame *save, const BattleUnit *unit);
/// Forgets any chain (a battle starting: its map and units are new).
void clearChains();

/// While it lives, a unit a chain took down is up as it was before the chain (standing, facing
/// as it did), for drawing; then it's put back as the battle has it, down (see QuickFallPose).
class ChainStandingPose
{
public:
	/// Stands the unit up, facing that way (nothing for a null unit).
	ChainStandingPose(BattleUnit *unit, int direction);
	/// Puts the unit back down, as the battle has it.
	~ChainStandingPose();
	ChainStandingPose(const ChainStandingPose&) = delete;
	ChainStandingPose &operator=(const ChainStandingPose&) = delete;
private:
	BattleUnit *_unit;   // the unit posed (null: none)
	int _gameDirection;  // its facing as the battle has it, put back after
};

}

}
