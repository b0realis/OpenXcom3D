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
 * FirstPersonSpriteHits.cpp: setting "Full sprite hitboxes": a unit is hit where its picture shows.
 */
#include "FirstPersonApi.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>
#include "FirstPersonMode.h"
#include "FirstPersonViewShared.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/UnitSprite.h"
#include "../Engine/GraphSubset.h"
#include "../Engine/Options.h"
#include "../Engine/Surface.h"
#include "../Mod/Armor.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"

namespace OpenXcom
{
namespace FirstPerson
{

namespace
{

/**
 * The units' pictures as shooters see them, kept while the battle's animation frame lasts (a
 * tenth of a second: within it the AI weighs many lines at the same units): each unit's frame,
 * as the 3D view composes it from a side (see FirstPersonView::drawUnits), as its solid pixels.
 */
struct SpriteCache
{
	const SavedBattleGame *save = nullptr;    // the battle they were made for
	int animFrame = -1;                       // and its animation frame
	std::map<std::pair<const BattleUnit*, int>, std::vector<Uint8>> pictures; // by unit and view rotation: 1 for a solid pixel
	Surface *buffer = nullptr;                // where a frame is composed
};

/**
 * The cache (made on first use, kept for the game's life).
 */
SpriteCache &spriteCache()
{
	static SpriteCache cache;
	return cache;
}

/**
 * A unit's picture as seen from one side, as the 3D view composes it (see FirstPersonView::
 * drawUnits): its sprite in the game's own animation frame, turned by the view rotation (the
 * isometric art shows units from the south-east), a big unit's four parts put together as the
 * map draws them; its solid pixels, SpriteBufferSize square.
 * @param save The battle.
 * @param unit The unit.
 * @param rotation The view rotation (3 minus the direction from the unit to the one looking).
 * @return Its pixels, 1 where the picture is solid, row by row.
 */
const std::vector<Uint8> &spriteOf(SavedBattleGame *save, const BattleUnit *unit, int rotation)
{
	SpriteCache &cache = spriteCache();
	if (cache.save != save || cache.animFrame != save->getAnimFrame())
	{
		cache.save = save;
		cache.animFrame = save->getAnimFrame();
		cache.pictures.clear();
	}
	auto known = cache.pictures.find({ unit, rotation });
	if (known != cache.pictures.end())
	{
		return known->second;
	}
	if (!cache.buffer)
	{
		cache.buffer = new Surface(SpriteBufferSize, SpriteBufferSize);
	}
	cache.buffer->clear();
	UnitSprite unitSprite(cache.buffer, save->getMod(), save, save->getAnimFrame(), save->getDepth() != 0, 0, 0);
	unitSprite.setViewRotation(rotation);
	const GraphSubset mask(SpriteBufferSize, SpriteBufferSize);
	if (unit->getArmor()->getSize() == 1)
	{
		unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
	}
	else
	{
		unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
		unitSprite.draw(unit, 1, 32, 16, 0, mask, false);
		unitSprite.draw(unit, 2, 0, 16, 0, mask, false);
		unitSprite.draw(unit, 3, 16, 24, 0, mask, false);
	}
	std::vector<Uint8> &picture = cache.pictures[{ unit, rotation }];
	picture.assign((size_t)SpriteBufferSize * SpriteBufferSize, 0);
	cache.buffer->lock();
	for (int y = 0; y < SpriteBufferSize; ++y)
	{
		for (int x = 0; x < SpriteBufferSize; ++x) picture[(size_t)y * SpriteBufferSize + x] = cache.buffer->getPixel(x, y) != 0;
	}
	cache.buffer->unlock();
	return picture;
}

}

/**
 * Setting "Full sprite hitboxes" (the owner's, GitHub issue #42: "Setting for hit-boxes the same size
 * as sprites!"; the battle's rule, for everyone's shots and the game's line of fire checks, and so
 * its sight too, which walks the same lines (TileEngine::visible, canTargetUnit), in 3D and on the
 * map): on, and the mod's 3D view in the battle.
 * @param state The battle's screen.
 * @return True if units are hit by their pictures.
 */
bool spriteHitboxes(BattlescapeState *state)
{
	return FirstPersonMode::spriteHitboxes(state);
}

/**
 * Does this voxel of a line of fire hit the unit's picture (setting "Full sprite hitboxes")? The
 * picture stands as the 3D view draws it: a card through the middle of the unit's tiles, its feet
 * on the floor (the lowest of its tiles' ground), at the setting "Unit size", here upright and
 * facing the line's start (the shooter), its frame the one seen from the shooter's side. A voxel
 * hits where it lies on that card (within SpriteHitSlab of it) on one of the picture's solid
 * pixels; see-through pixels let the line on. One sprite pixel is SpritePixelX of a voxel across
 * and one voxel up, as the isometric art draws. A shot's impact (its line found the unit there,
 * and the game asks again for which part was hit) is always the unit's.
 * @param save The battle.
 * @param unit The unit on the voxel's tile.
 * @param voxel The voxel.
 * @param origin Where the line starts.
 * @param impact Whether it's a shot's impact.
 * @return True if the voxel hits the unit.
 */
bool spriteHit(SavedBattleGame *save, const BattleUnit *unit, const Position &voxel, const Position &origin, bool impact)
{
	if (impact)
	{
		return true;
	}
	// Where the unit stands: the middle of its tiles, on the highest of their ground (terrain
	// levels are negative), as the game's own hit shape stands (see TileEngine::voxelCheck).
	const int size = unit->getArmor()->getSize();
	int terrainHeight = 0;
	for (int x = 0; x < size; ++x)
	{
		for (int y = 0; y < size; ++y)
		{
			const Tile *tile = save->getTile(unit->getPosition() + Position(x, y, 0));
			if (tile) terrainHeight = std::min(terrainHeight, tile->getTerrainLevel());
		}
	}
	const double middleX = unit->getPosition().x * 16.0 + 8.0 * size, middleY = unit->getPosition().y * 16.0 + 8.0 * size;
	const double feet = unit->getPosition().z * 24.0 - terrainHeight;

	// The card faces the line's start (straight above or below it, the way the map shows units,
	// from the south-east).
	double towardsX = origin.x + 0.5 - middleX, towardsY = origin.y + 0.5 - middleY;
	const double length = std::hypot(towardsX, towardsY);
	if (length < 1.0)
	{
		towardsX = 1.0;
		towardsY = 1.0;
	}
	const double towardsLength = std::hypot(towardsX, towardsY);
	towardsX /= towardsLength;
	towardsY /= towardsLength;

	// The voxel on the card: how far in front of it, how far right of its middle as the shooter
	// sees it (looking along minus towards, right is (towardsY, -towardsX)), and how high.
	const double relativeX = voxel.x + 0.5 - middleX, relativeY = voxel.y + 0.5 - middleY;
	if (std::abs(relativeX * towardsX + relativeY * towardsY) > SpriteHitSlab)
	{
		return false;
	}
	const double right = relativeX * towardsY - relativeY * towardsX, up = voxel.z + 0.5 - feet;

	// The picture's pixel there: anchored at the floor diamond's middle (a big unit's, the corner
	// its four tiles share), at the setting "Unit size".
	const double scale = std::clamp(Options::fpsUnitSize, 80, 100) / 100.0;
	const int anchorX = 32, anchorY = size == 1 ? 40 : 48;
	const int column = (int)std::floor(anchorX + right / (SpritePixelX * scale)), row = (int)std::floor(anchorY - up / scale);
	if (column < 0 || row < 0 || column >= SpriteBufferSize || row >= SpriteBufferSize)
	{
		return false;
	}
	// The frame from the shooter's side: the isometric art shows units from the south-east
	// (direction 3); turned by the direction from the unit to the shooter (0 north, clockwise).
	const double toShooterDegrees = std::atan2(towardsY, towardsX) * 180.0 / Pi;
	const int shooterDirection = (((int)std::lround((toShooterDegrees + 90.0) / 45.0)) % 8 + 8) % 8;
	return spriteOf(save, unit, 3 - shooterDirection)[(size_t)row * SpriteBufferSize + column] != 0;
}

/**
 * Where a round that met a unit's picture struck it, for the armour side and the body part it hits
 * (setting "Full sprite hitboxes"; BattleUnit::damage tells the side by which way that point lies
 * from the unit's middle). The picture is a card through the middle, so the point lies to the side
 * along the card and hardly towards the shooter at all: a round from straight ahead would hit the
 * flank, or the rear. It's pushed out towards the shooter by SpriteHitPush, as the game's own hit
 * shape would have stopped the round on its near side; how far to the side and how high it struck
 * stay. A shooter straight above or below pushes nothing.
 * @param relative Where it struck, from the unit's middle (at its floor), in voxels.
 * @param unit The unit hit.
 * @param shooter Who fired.
 * @return The point to tell the side and the body part by.
 */
Position spriteHitRelative(const Position &relative, const BattleUnit *unit, const BattleUnit *shooter)
{
	const int unitSize = unit->getArmor()->getSize(), shooterSize = shooter->getArmor()->getSize();
	const double towardsX = (shooter->getPosition().x * 16.0 + 8.0 * shooterSize) - (unit->getPosition().x * 16.0 + 8.0 * unitSize);
	const double towardsY = (shooter->getPosition().y * 16.0 + 8.0 * shooterSize) - (unit->getPosition().y * 16.0 + 8.0 * unitSize);
	const double length = std::hypot(towardsX, towardsY);
	if (length < 1.0)
	{
		return relative;
	}
	return Position(relative.x + (int)std::lround(towardsX / length * SpriteHitPush), relative.y + (int)std::lround(towardsY / length * SpriteHitPush), relative.z);
}

}
}
