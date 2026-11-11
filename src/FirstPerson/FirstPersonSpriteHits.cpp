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
 *
 * A line of fire meets a unit's picture where it crosses the picture's card (see SpriteCard): the
 * very pixel the line goes through counts, solid or see-through, wherever the card reaches (over
 * the neighbouring tiles too: a bat's wings, a Cyberdisc's rim). The card stands as the 3D view
 * draws the picture, its feet on the floor's top, and for the soldier looked through in 3D it is
 * the card the view draws, leaning as it leans: what he sees is what he hits.
 */
#include "FirstPersonApi.h"
#include "FirstPersonSpriteHits.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>
#include <vector>
#include "FirstPersonMode.h"
#include "FirstPersonView.h"
#include "FirstPersonViewShared.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/UnitSprite.h"
#include "../Engine/GraphSubset.h"
#include "../Engine/Options.h"
#include "../Engine/Surface.h"
#include "../Mod/Armor.h"
#include "../Mod/MapData.h"
#include "../Mod/Mod.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"

namespace OpenXcom
{
namespace FirstPerson
{

namespace
{

/// How far a unit's picture reaches across the map from the middle of its tiles, in voxels: half its
/// 32 picture columns (a big unit's 64), SpritePixelX of a voxel each, and a voxel for the gate; a
/// card that leans (the looked-through soldier's own lines) up to its rows' height along the ground
/// more (53 rows, in true proportions).
const double PictureReachSmall = 16 * SpritePixelX + 1.0, PictureReachBig = 32 * SpritePixelX + 1.0, PictureLeanReach = 53 * ZScale;
/// How far below its anchor and above it a unit's picture reaches, in voxels: the rows under the
/// anchor (24 at most) and over it (48 at most), and a voxel for the gate.
const double PictureBelow = 25.0, PictureAbove = 48.0 + 1.0;
/// How far the 3D view may draw a unit from where the game has it between its small steps (it eases
/// after them, see FirstPersonView::drawUnits), in voxels: the looked-through soldier's own lines meet
/// the picture where it's drawn.
const double GlideReach = 8.0;
/// A line's voxel meets a card where the line crosses it: the voxel's middle within this of the
/// card's plane (a line steps up to a voxel along each axis at a time, up to 1.73 voxels across
/// a tilted card, so a gate this wide is never stepped over) and of the crossing along the line.
const double CardGate = 0.9, CardGateAlong = 1.5;
/// A line counts as the looked-through soldier's own (his crosshair, his shots: see SpriteCard)
/// when it starts within this of the 3D view's eye across the map, in voxels (the game's muzzle
/// point is up to 7 from the middle of his tile).
const double OwnLineReach = 12.0;
/// A point within this of straight above or below a unit across the map, in voxels, sees it from
/// no side: it's seen from the south-east, as the map shows it (see unitViewRotation, uprightCard).
const double StraightAboveReach = 1.0;

double dot(const double a[3], const double b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/**
 * What of a unit picks its sprite frame, besides the battle's animation frame and the side it's
 * seen from: its status (standing, walking, aiming...), facings, steps, falls, kneeling, hovering
 * and what it holds. A unit's picture is made again when any of it changes.
 */
struct Pose
{
	int status = -1, direction = 0, faceDirection = 0, turretDirection = 0, verticalDirection = 0;
	int walkingPhase = 0, diagonalPhase = 0, fallingPhase = 0;
	bool kneeled = false, floating = false;
	const BattleItem *rightHand = nullptr, *leftHand = nullptr;

	/// The unit's pose now.
	explicit Pose(const BattleUnit *unit = nullptr)
	{
		if (!unit) return;
		status = (int)unit->getStatus();
		direction = unit->getDirection();
		faceDirection = unit->getFaceDirection();
		turretDirection = unit->getTurretDirection();
		verticalDirection = unit->getVerticalDirection();
		walkingPhase = unit->getWalkingPhase();
		diagonalPhase = unit->getDiagonalWalkingPhase();
		fallingPhase = unit->getFallingPhase();
		kneeled = unit->isKneeled();
		floating = unit->isFloating();
		rightHand = unit->getRightHandWeapon();
		leftHand = unit->getLeftHandWeapon();
	}

	/// The same pose?
	bool operator==(const Pose &other) const
	{
		return status == other.status && direction == other.direction && faceDirection == other.faceDirection
			&& turretDirection == other.turretDirection && verticalDirection == other.verticalDirection
			&& walkingPhase == other.walkingPhase && diagonalPhase == other.diagonalPhase && fallingPhase == other.fallingPhase
			&& kneeled == other.kneeled && floating == other.floating && rightHand == other.rightHand && leftHand == other.leftHand;
	}
};

/**
 * The units' pictures as lines see them, kept while the battle's animation frame lasts (a tenth
 * of a second: within it the AI weighs many lines at the same units) and the unit keeps its pose
 * (it turns, kneels, takes aim or steps on: made again, see Pose): each unit's frame, as the 3D
 * view composes it from a side (see FirstPersonView::drawUnits), as its solid pixels.
 */
struct Picture
{
	Pose pose;
	std::vector<Uint8> pixels; // SpriteBufferSize square, 1 for a solid pixel
};
struct SpriteCache
{
	const SavedBattleGame *save = nullptr;    // the battle they were made for
	int animFrame = -1;                       // and its animation frame
	std::map<std::pair<const BattleUnit*, int>, Picture> pictures; // by unit and view rotation
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
	const Pose pose(unit);
	Picture &picture = cache.pictures[{ unit, rotation }];
	if (!picture.pixels.empty() && picture.pose == pose)
	{
		return picture.pixels;
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
	picture.pose = pose;
	picture.pixels.assign((size_t)SpriteBufferSize * SpriteBufferSize, 0);
	cache.buffer->lock();
	for (int y = 0; y < SpriteBufferSize; ++y)
	{
		for (int x = 0; x < SpriteBufferSize; ++x) picture.pixels[(size_t)y * SpriteBufferSize + x] = cache.buffer->getPixel(x, y) != 0;
	}
	cache.buffer->unlock();
	return picture.pixels;
}

/**
 * Whether a voxel is solid in the game's own shapes of the terrain (as TileEngine::voxelCheck
 * finds it: a part's line-of-fire template, an open UFO door left out; a grav lift's floor).
 * @param save The battle.
 * @param x, y, z The voxel.
 * @return True if it's solid; false if open, or off the map.
 */
bool gameSolidVoxel(SavedBattleGame *save, int x, int y, int z)
{
	if (x < 0 || y < 0 || z < 0) return false;
	const Tile *tile = save->getTile(Position(x / 16, y / 16, z / 24));
	if (!tile) return false;
	if (tile->hasGravLiftFloor() && z % 24 < 2)
	{
		const Tile *below = save->getBelowTile(tile);
		if (!(below && below->hasGravLiftFloor())) return true;
	}
	const std::vector<Uint16> *voxelData = save->getMod()->getVoxelData();
	for (int part = O_FLOOR; part <= O_OBJECT; ++part)
	{
		if ((part == O_WESTWALL || part == O_NORTHWALL) && tile->isUfoDoorOpen((TilePart)part)) continue;
		const MapData *data = tile->getMapData((TilePart)part);
		if (!data) continue;
		const size_t index = (size_t)data->getLoftID((z % 24) / 2) * 16 + y % 16;
		if (index < voxelData->size() && ((*voxelData)[index] & (1 << (15 - x % 16)))) return true;
	}
	return false;
}

/**
 * Where a unit's picture stands (see SpriteCard), worked out afresh: the anchor pixel at the middle
 * of its tiles, on its tile's floor (by the tile's terrain level; a unit on the move between the
 * tiles of its step, where the 3D view draws it), raised out of the floor's solid voxels as the 3D
 * view raises it (see FirstPersonView::standingHeight: it builds a floor two voxels
 * thick and stands units on its top). While the view shows the battle, by the view's own reckoning;
 * on the map the same way through the game's voxels (the same for every ordinary floor).
 * @param save The battle.
 * @param unit The unit.
 * @param view The 3D view showing the battle, or null.
 * @param anchor Set to the point, in voxels.
 */
void standingAnchor(SavedBattleGame *save, const BattleUnit *unit, const FirstPersonView *view, double anchor[3])
{
	const int size = unit->getArmor()->getSize();
	// The middle of a unit's tiles at a position, on the floor (by the tile's terrain level).
	auto place = [&](const Position &at, double out[3]) {
		const Tile *tile = save->getTile(at);
		out[0] = at.x * 16.0 + 8.0 * size;
		out[1] = at.y * 16.0 + 8.0 * size;
		out[2] = at.z * 24.0 - (tile ? tile->getTerrainLevel() : 0);
	};
	if (unit->getStatus() == STATUS_WALKING || unit->getStatus() == STATUS_FLYING)
	{
		// On the move: between the tile it left and the one it's going to, by how far into the step
		// the game is (a diagonal step takes twice as many phases), as the 3D view draws it (see
		// FirstPersonView::drawUnits): not on the tile the game moves it to half way.
		double from[3], to[3];
		place(unit->getLastPosition(), from);
		place(unit->getDestination(), to);
		const double walkPhase = unit->getWalkingPhase() + unit->getDiagonalWalkingPhase();
		const double progress = std::clamp(walkPhase / ((unit->getDirection() % 2) ? 16.0 : 8.0), 0.0, 1.0);
		for (int axis = 0; axis < 3; ++axis) anchor[axis] = from[axis] + (to[axis] - from[axis]) * progress;
	}
	else
	{
		place(unit->getPosition(), anchor);
	}
	double height;
	if (view && view->standingHeightNow(anchor, height))
	{
		anchor[2] = height;
		return;
	}
	// (FirstPersonView::riseOutOfSolid in the game's voxels: the first open voxel up, StandRaiseMax at most)
	const int x = (int)std::floor(anchor[0]), y = (int)std::floor(anchor[1]), z = (int)std::floor(anchor[2]);
	if (x >= save->getMapSizeX() * 16 || y >= save->getMapSizeY() * 16) return;
	for (int raise = 0; raise <= StandRaiseMax; ++raise)
	{
		if (z + raise >= save->getMapSizeZ() * 24) return;
		if (!gameSolidVoxel(save, x, y, z + raise))
		{
			if (raise > 0) anchor[2] = z + raise;
			return;
		}
	}
}

/**
 * Where a unit's picture stands (see standingAnchor), kept while the battle's animation frame lasts,
 * the unit on the same tile and as far into its step, and the 3D view showing the battle or not, as
 * it was: the lines ask it again and again.
 * @param save The battle.
 * @param unit The unit.
 * @param anchor Set to the point, in voxels.
 */
void pictureAnchor(SavedBattleGame *save, const BattleUnit *unit, double anchor[3])
{
	struct Anchor
	{
		const SavedBattleGame *save = nullptr;
		int animFrame = -1, status = -1, phase = -1;
		Position at;
		bool viewed = false;
		double point[3] = {};
	};
	static std::map<const BattleUnit*, Anchor> anchors;
	static const SavedBattleGame *anchorsSave = nullptr;
	if (anchorsSave != save)
	{
		anchors.clear(); // (a new battle's units)
		anchorsSave = save;
	}
	const FirstPersonView *view = FirstPersonMode::shootingView(save->getBattleState());
	Anchor &kept = anchors[unit];
	const int status = (int)unit->getStatus(), phase = unit->getWalkingPhase() + 100 * unit->getDiagonalWalkingPhase();
	if (kept.save != save || kept.animFrame != save->getAnimFrame() || kept.at != unit->getPosition() || kept.viewed != (view != nullptr)
		|| kept.status != status || kept.phase != phase)
	{
		kept.save = save;
		kept.animFrame = save->getAnimFrame();
		kept.status = status;
		kept.phase = phase;
		kept.at = unit->getPosition();
		kept.viewed = view != nullptr;
		standingAnchor(save, unit, view, kept.point);
	}
	std::copy(kept.point, kept.point + 3, anchor);
}

/**
 * The view rotation for a unit's frame seen from a point: the isometric art shows units from the
 * south-east (direction 3); turned by the direction from the unit to the point (0 north,
 * clockwise; atan2 measures from east, so a quarter turn is added), as the 3D view turns it.
 * @param towardsX, towardsY The way from the unit to the point.
 * @return The rotation.
 */
int rotationTowards(double towardsX, double towardsY)
{
	const double degrees = std::atan2(towardsY, towardsX) * 180.0 / Pi;
	const int direction = (((int)std::lround((degrees + 90.0) / 45.0)) % 8 + 8) % 8;
	return 3 - direction;
}

/**
 * The parts of a card every card shares: the unit, where it stands, its anchor pixel and the
 * setting "Unit size", and whether its art below the feet counts (a unit drawn standing in the
 * air: on a floor that art lies flat on the floor, see FirstPersonView::drawUnits).
 * @param save The battle.
 * @param unit The unit.
 * @param card The card to start.
 * @return The card's scale (world size per picture pixel).
 */
double startCard(SavedBattleGame *save, const BattleUnit *unit, SpriteCard &card)
{
	card.unit = unit;
	pictureAnchor(save, unit, card.anchor);
	card.anchorX = 32;
	card.anchorY = unit->getArmor()->getSize() == 1 ? 40 : 48; // the floor diamond's middle; a big unit's, the corner its four tiles share
	card.rowsBelow = unit->haveNoFloorBelow() || unit->getStatus() == STATUS_FLYING;
	return std::clamp(Options::fpsUnitSize, 80, 100) / 100.0;
}

/**
 * A unit's card for a line from a point (everyone's but the looked-through soldier's own): upright,
 * facing the point across the map (straight above or below it, the way the map shows units, from
 * the south-east), its frame the one seen from that side.
 * @param save The battle.
 * @param unit The unit.
 * @param from The point, in voxels.
 * @param card Set to the card.
 */
void uprightCard(SavedBattleGame *save, const BattleUnit *unit, const double from[3], SpriteCard &card)
{
	const double scale = startCard(save, unit, card);
	double towardsX = from[0] - card.anchor[0], towardsY = from[1] - card.anchor[1];
	if (std::hypot(towardsX, towardsY) < StraightAboveReach)
	{
		towardsX = 1.0;
		towardsY = 1.0;
	}
	const double length = std::hypot(towardsX, towardsY);
	towardsX /= length;
	towardsY /= length;
	// Looking along minus towards, right is (towardsY, -towardsX); a picture pixel is SpritePixelX
	// of a voxel across and one voxel up, as the isometric art draws, times the scale.
	const double column[3] = { towardsY * SpritePixelX * scale, -towardsX * SpritePixelX * scale, 0.0 };
	const double row[3] = { 0.0, 0.0, scale };
	const double normal[3] = { towardsX, towardsY, 0.0 };
	std::copy(column, column + 3, card.column);
	std::copy(row, row + 3, card.row);
	std::copy(normal, normal + 3, card.normal);
	card.picture = &spriteOf(save, unit, rotationTowards(towardsX, towardsY));
}

/**
 * A unit's card as the 3D view draws it (the looked-through soldier's own lines, see SpriteCard):
 * where it drew it last (a unit on the move as it glides between the game's small steps),
 * square to the view across the map, leaning as the view looks up or down (see FirstPersonView::
 * cardAxes), its frame the one the view shows (smooth up close, as the view draws it: see
 * FirstPersonView::smoothPicture).
 * @param save The battle.
 * @param unit The unit.
 * @param view The view.
 * @param card Set to the card.
 */
void drawnCard(SavedBattleGame *save, const BattleUnit *unit, const FirstPersonView *view, SpriteCard &card)
{
	const double scale = startCard(save, unit, card);
	// Where the view drew it last (it eases each unit between the game's small steps, see
	// FirstPersonView::drawUnits): there, its feet on the floor's top as the view stands it.
	double feet[3], height;
	if (view->drawnFeet(unit, feet) && view->standingHeightNow(feet, height))
	{
		card.anchor[0] = feet[0];
		card.anchor[1] = feet[1];
		card.anchor[2] = height;
	}
	view->cardAxes(card.anchor, scale, card.column, card.row);
	// The normal: column cross row, made a unit vector.
	double normal[3] = { card.column[1] * card.row[2] - card.column[2] * card.row[1], card.column[2] * card.row[0] - card.column[0] * card.row[2],
		card.column[0] * card.row[1] - card.column[1] * card.row[0] };
	const double length = std::sqrt(dot(normal, normal));
	for (int axis = 0; axis < 3; ++axis) card.normal[axis] = length > 0.0 ? normal[axis] / length : 0.0;
	double eye[3];
	view->getSteadyEye(eye);
	const int rotation = unitViewRotation(eye, card.anchor); // (the side the view draws it from)
	card.picture = &spriteOf(save, unit, rotation);
	// Drawn smooth up close (setting "Sprite upscaling"): that picture, the very frame (see FirstPersonView::smoothPicture).
	card.smooth = view->smoothPicture(unit, rotation, *card.picture, card.factor);
	if (!card.smooth) card.factor = 1;
}

/**
 * The line being walked (see spriteHitAt), worked out once for all its voxels: where it starts
 * and which way it goes, and the cards of the units near enough to it, with where it crosses each
 * and whether on a solid pixel. A line has a serial number from the game (TileEngine): a new one
 * is worked out afresh.
 */
struct LineCard
{
	BattleUnit *unit;
	SpriteCard card;
	double along;  // where the line crosses the card, in voxels from its start
	bool solid;    // on a solid pixel of the picture
};
struct LineContext
{
	const SavedBattleGame *save = nullptr;
	unsigned serial = 0;
	double start[3] = {}, direction[3] = {};
	std::vector<LineCard> cards;
	bool exact = false; // along the crosshair's exact ray (see ExactLine)
};

/**
 * The line being walked now (one at a time: the game's thread).
 */
LineContext &lineContext()
{
	static LineContext line;
	return line;
}

/**
 * The crosshair's exact ray (see ExactLine): set while the aim's line is walked.
 */
struct ExactRay
{
	bool active = false;
	double eye[3] = {}, direction[3] = {};
	Position start;
	bool crossed = false;   // the last exact line met a picture
	double crossing[3] = {}; // where
};
ExactRay &exactRay()
{
	static ExactRay ray;
	return ray;
}

/**
 * The curve being walked (see ArcLines): while it is, the pictures its steps meet face its start.
 */
struct ArcFacing
{
	bool active = false;
	double from[3] = {}; // the curve's start, in voxels (the middle of its first voxel)
};
ArcFacing &arcFacing()
{
	static ArcFacing facing;
	return facing;
}

/**
 * Works out a line afresh (see LineContext): its start and direction (between the middles of its
 * first and last voxels; the crosshair's own exact ray instead, see ExactLine), whose line it is
 * (the looked-through soldier's own when he is left out of it, the 3D view shows the battle and
 * it starts at his eye: then the pictures are the cards the view draws), and the cards of the
 * units it passes near, but the one left out (or all but the only one tested). A step of a curve
 * (see ArcLines): the cards face the curve's start, the line itself still what crosses them.
 * @param save The battle.
 * @param origin, target The line's first and last voxels.
 * @param serial The line's serial number.
 * @param excludeUnit The unit left out of it (or null).
 * @param excludeAllBut The only unit it tests (or null: all).
 */
void prepareLine(SavedBattleGame *save, const Position &origin, const Position &target, unsigned serial, const BattleUnit *excludeUnit, const BattleUnit *excludeAllBut)
{
	LineContext &line = lineContext();
	line.save = save;
	line.serial = serial;
	line.cards.clear();
	const ExactRay &ray = exactRay();
	line.exact = ray.active && origin == ray.start;
	double length;
	if (line.exact)
	{
		std::copy(ray.eye, ray.eye + 3, line.start);
		std::copy(ray.direction, ray.direction + 3, line.direction);
		length = std::sqrt(std::pow(target.x - origin.x, 2.0) + std::pow(target.y - origin.y, 2.0) + std::pow(target.z - origin.z, 2.0)) + 1.0;
	}
	else
	{
		const double start[3] = { origin.x + 0.5, origin.y + 0.5, origin.z + 0.5 };
		const double end[3] = { target.x + 0.5, target.y + 0.5, target.z + 0.5 };
		const double span[3] = { end[0] - start[0], end[1] - start[1], end[2] - start[2] };
		length = std::sqrt(dot(span, span));
		if (length < 1e-6) return; // (a line going nowhere crosses nothing)
		std::copy(start, start + 3, line.start);
		for (int axis = 0; axis < 3; ++axis) line.direction[axis] = span[axis] / length;
	}
	// Whose line: the looked-through soldier's own, from his eye, while the 3D view shows the battle.
	const FirstPersonView *view = FirstPersonMode::shootingView(save->getBattleState());
	bool own = false;
	if (view && excludeUnit && excludeUnit == view->getCameraUnit())
	{
		double eye[3];
		view->getEye(eye);
		own = std::hypot(line.start[0] - eye[0], line.start[1] - eye[1]) <= OwnLineReach;
	}
	// The units whose pictures the line may reach (down or left out of it not): where the line is
	// within the heights a unit's picture may have, from below its tile to above it, how near it
	// comes to the middle of its tiles across the map, within the picture's reach.
	// (first a box round the line, as far out as any picture reaches: most units lie outside it)
	const double flat = line.direction[0] * line.direction[0] + line.direction[1] * line.direction[1];
	const double boxReach = PictureReachBig + (own ? PictureLeanReach + GlideReach : 0.0) + 24.0;
	const double end[3] = { line.start[0] + line.direction[0] * length, line.start[1] + line.direction[1] * length, line.start[2] + line.direction[2] * length };
	const double boxLow[3] = { std::min(line.start[0], end[0]) - boxReach, std::min(line.start[1], end[1]) - boxReach, std::min(line.start[2], end[2]) - PictureAbove - 48.0 };
	const double boxHigh[3] = { std::max(line.start[0], end[0]) + boxReach, std::max(line.start[1], end[1]) + boxReach, std::max(line.start[2], end[2]) + PictureBelow + 24.0 };
	for (BattleUnit *unit : *save->getUnits())
	{
		const Position at = unit->getPosition();
		if (at.x * 16.0 < boxLow[0] || at.x * 16.0 > boxHigh[0] || at.y * 16.0 < boxLow[1] || at.y * 16.0 > boxHigh[1] || at.z * 24.0 < boxLow[2] || at.z * 24.0 > boxHigh[2]) continue;
		if (unit->isOut() || unit == excludeUnit || (excludeAllBut && unit != excludeAllBut) || !unit->getTile()) continue;
		const int size = unit->getArmor()->getSize();
		double anchor[3];
		pictureAnchor(save, unit, anchor);
		double from = -1.0, to = length + 1.0;
		const double slack = own ? GlideReach : 0.0;
		const double low = anchor[2] - PictureBelow - slack - line.start[2], high = anchor[2] + PictureAbove + slack - line.start[2];
		if (std::abs(line.direction[2]) > 1e-9)
		{
			const double first = low / line.direction[2], last = high / line.direction[2];
			from = std::max(from, std::min(first, last));
			to = std::min(to, std::max(first, last));
		}
		else if (low > 0.0 || high < 0.0)
		{
			continue;
		}
		if (from > to) continue;
		const double middleX = anchor[0] - line.start[0], middleY = anchor[1] - line.start[1];
		const double nearest = std::clamp(flat > 1e-12 ? (middleX * line.direction[0] + middleY * line.direction[1]) / flat : from, from, to);
		const double asideX = middleX - nearest * line.direction[0], asideY = middleY - nearest * line.direction[1];
		const double reach = (size == 1 ? PictureReachSmall : PictureReachBig) + (own ? PictureLeanReach + GlideReach : 0.0);
		if (asideX * asideX + asideY * asideY > reach * reach) continue;
		LineCard lineCard;
		lineCard.unit = unit;
		if (own) drawnCard(save, unit, view, lineCard.card);
		else uprightCard(save, unit, arcFacing().active ? arcFacing().from : line.start, lineCard.card);
		lineCard.solid = cardRay(lineCard.card, line.start, line.direction, lineCard.along);
		if (lineCard.solid) line.cards.push_back(lineCard);
	}
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
 * Which unit's picture a voxel of a line of fire hits (setting "Full sprite hitboxes"): the line
 * meets a picture where it crosses its card (see SpriteCard), on the very pixel it goes through
 * there (see cardRay), at the voxel of the line at that crossing; see-through pixels let it on.
 * Any unit's whose card reaches the line, not only the one on the voxel's tile: a picture
 * reaching over a neighbouring tile is hit there too. Of two met at the same voxel, the nearer.
 * @param save The battle.
 * @param voxel The voxel.
 * @param origin, target The line's first and last voxels.
 * @param serial The line's serial number (TileEngine counts the lines it walks).
 * @param excludeUnit The unit left out of the line (or null).
 * @param onlyVisible Whether only units the player sees count.
 * @param excludeAllBut The only unit tested (or null: all).
 * @return The unit hit, or null.
 */
BattleUnit *spriteHitAt(SavedBattleGame *save, const Position &voxel, const Position &origin, const Position &target, unsigned serial,
	const BattleUnit *excludeUnit, bool onlyVisible, const BattleUnit *excludeAllBut)
{
	LineContext &line = lineContext();
	if (line.save != save || line.serial != serial)
	{
		prepareLine(save, origin, target, serial, excludeUnit, excludeAllBut);
	}
	const double middle[3] = { voxel.x + 0.5, voxel.y + 0.5, voxel.z + 0.5 };
	const double fromStart[3] = { middle[0] - line.start[0], middle[1] - line.start[1], middle[2] - line.start[2] };
	const double voxelAlong = dot(fromStart, line.direction);
	BattleUnit *hit = nullptr;
	double hitAlong = std::numeric_limits<double>::max();
	for (const LineCard &lineCard : line.cards)
	{
		if (onlyVisible && !lineCard.unit->getVisible()) continue;
		const SpriteCard &card = lineCard.card;
		const double fromAnchor[3] = { middle[0] - card.anchor[0], middle[1] - card.anchor[1], middle[2] - card.anchor[2] };
		if (std::abs(dot(fromAnchor, card.normal)) > CardGate || std::abs(voxelAlong - lineCard.along) > CardGateAlong) continue;
		if (lineCard.along < hitAlong)
		{
			hit = lineCard.unit;
			hitAlong = lineCard.along;
		}
	}
	if (hit && line.exact)
	{
		ExactRay &ray = exactRay();
		ray.crossed = true;
		for (int axis = 0; axis < 3; ++axis) ray.crossing[axis] = line.start[axis] + line.direction[axis] * hitAlong;
	}
	return hit;
}

/**
 * Starts a curve's lines (see FirstPersonApi.h): with the setting on, the pictures they meet face
 * the curve's start.
 * @param save The battle.
 * @param origin The curve's first voxel.
 */
ArcLines::ArcLines(SavedBattleGame *save, const Position &origin) : _set(save && spriteHitboxes(save->getBattleState()))
{
	ArcFacing &facing = arcFacing();
	_outerActive = facing.active;
	std::copy(facing.from, facing.from + 3, _outerFrom);
	if (_set)
	{
		facing.active = true;
		facing.from[0] = origin.x + 0.5;
		facing.from[1] = origin.y + 0.5;
		facing.from[2] = origin.z + 0.5;
		lineContext().save = nullptr; // (worked out afresh, as ExactLine does)
	}
}

/**
 * Ends a curve's lines: the pictures face each line's own start again.
 */
ArcLines::~ArcLines()
{
	if (_set)
	{
		ArcFacing &facing = arcFacing();
		facing.active = _outerActive;
		std::copy(_outerFrom, _outerFrom + 3, facing.from);
		lineContext().save = nullptr;
	}
}

/**
 * Where an arcing shot at a unit aims across the map (see FirstPersonApi.h): with the setting on, a
 * big unit's middle (its picture stands there, see standingAnchor; the game aims at the tile's).
 * @param state The battle's screen.
 * @param unit The unit aimed at.
 * @param aim The point aimed at, its across the map (x, y) set to the big unit's middle.
 */
void arcAimAtPicture(BattlescapeState *state, const BattleUnit *unit, Position &aim)
{
	const int size = unit ? unit->getArmor()->getSize() : 1;
	if (size < 2 || !spriteHitboxes(state))
	{
		return;
	}
	aim.x = unit->getPosition().x * 16 + 8 * size;
	aim.y = unit->getPosition().y * 16 + 8 * size;
}

/**
 * Does a curve tried for an arcing shot land on the unit aimed at (see FirstPersonApi.h)? With the
 * setting on, a shot's curve that met the unit on the aimed tile (its picture: see spriteImpact, the
 * stop on one of its tiles) on any of its tiles.
 * @param save The battle.
 * @param thrown A throw's curve (never: an item stopped by a unit drops short of its tile).
 * @param meets What the curve met (V_UNIT...).
 * @param impact Where it stopped.
 * @param aimTile The tile of the point aimed at.
 * @return True if it lands on the unit aimed at.
 */
bool arcMeetsAimedUnit(SavedBattleGame *save, bool thrown, int meets, const Position &impact, const Position &aimTile)
{
	if (thrown || meets != V_UNIT || !spriteHitboxes(save->getBattleState()))
	{
		return false;
	}
	const Tile *aimed = save->getTile(aimTile), *stopped = save->getTile(impact.toTile());
	const BattleUnit *unit = aimed ? aimed->getOverlappingUnit(save) : nullptr;
	return unit && stopped && stopped->getOverlappingUnit(save) == unit;
}

/**
 * Where the game is to count a round that met a unit's picture as stopping (setting "Full sprite
 * hitboxes"; its hits, the body part and the blast are worked out from there, on the unit's tile):
 * the voxel itself if it lies on one of the unit's tiles, else the nearest voxel of them (a picture
 * reaching over a neighbouring tile, or above its level, was hit there).
 * @param save The battle.
 * @param unit The unit hit.
 * @param voxel The voxel of the line where it met the picture.
 * @return The voxel.
 */
Position spriteImpact(SavedBattleGame *save, const BattleUnit *unit, const Position &voxel)
{
	const int size = unit->getArmor()->getSize();
	const Position at = unit->getPosition();
	return Position(std::clamp<int>(voxel.x, at.x * 16, (at.x + size) * 16 - 1), std::clamp<int>(voxel.y, at.y * 16, (at.y + size) * 16 - 1),
		std::clamp<int>(voxel.z, at.z * 24, at.z * 24 + 23));
}

/**
 * The side a unit's frame is drawn from in a 3D view (see FirstPersonSpriteHits.h).
 */
int unitViewRotation(const double eye[3], const double feet[3])
{
	const double towardsX = eye[0] - feet[0], towardsY = eye[1] - feet[1];
	if (std::hypot(towardsX, towardsY) < StraightAboveReach)
	{
		return rotationTowards(1.0, 1.0); // from the south-east, as the map shows it
	}
	return rotationTowards(towardsX, towardsY);
}

/**
 * The card the lines from a 3D view's eye test for a unit (see FirstPersonSpriteHits.h).
 */
bool viewCard(SavedBattleGame *save, const BattleUnit *unit, const FirstPersonView *view, SpriteCard &card)
{
	if (!save || !unit || !view || unit->isOut() || !unit->getTile())
	{
		return false;
	}
	if (view == FirstPersonMode::shootingView(save->getBattleState()))
	{
		drawnCard(save, unit, view, card);
	}
	else
	{
		double eye[3];
		view->getEye(eye);
		uprightCard(save, unit, eye, card);
	}
	return true;
}

/**
 * Where a line crosses a card, and whether on a solid pixel (see FirstPersonSpriteHits.h): the
 * point on the card's plane, its picture column and row (the floor of how many pixels across and
 * down from the anchor pixel it lies, as the 3D view finds a pixel, see FirstPersonView::drawSprite).
 */
bool cardRay(const SpriteCard &card, const double start[3], const double direction[3], double &along)
{
	along = 0.0;
	const double facing = dot(direction, card.normal);
	if (std::abs(facing) < 1e-9 || !card.picture)
	{
		return false; // (a line along the card's plane: it's seen edge on)
	}
	const double toAnchor[3] = { card.anchor[0] - start[0], card.anchor[1] - start[1], card.anchor[2] - start[2] };
	along = dot(toAnchor, card.normal) / facing;
	const double fromAnchor[3] = { start[0] + direction[0] * along - card.anchor[0], start[1] + direction[1] * along - card.anchor[1],
		start[2] + direction[2] * along - card.anchor[2] };
	const double across = dot(fromAnchor, card.column) / dot(card.column, card.column);
	const double up = dot(fromAnchor, card.row) / dot(card.row, card.row);
	if (card.smooth)
	{
		// The smooth picture: factor times as many pixels each way, its anchor pixel as many times further in.
		const SDL_Surface *surface = const_cast<Surface*>(card.smooth)->getSurface(); // read only
		const double column = std::floor(card.factor * (card.anchorX + across)), row = std::floor(card.factor * (card.anchorY - up));
		if (column < 0 || row < 0 || column >= surface->w || row >= surface->h || (row >= card.anchorY * card.factor && !card.rowsBelow))
		{
			return false;
		}
		return ((const Uint8 *)surface->pixels)[(size_t)row * surface->pitch + (size_t)column] != 0;
	}
	const double column = std::floor(card.anchorX + across), row = std::floor(card.anchorY - up);
	if (column < 0 || row < 0 || column >= SpriteBufferSize || row >= SpriteBufferSize || (row >= card.anchorY && !card.rowsBelow))
	{
		return false;
	}
	return (*card.picture)[(size_t)row * SpriteBufferSize + (size_t)column] != 0;
}

/**
 * Starts the crosshair's exact ray (see FirstPersonSpriteHits.h).
 */
ExactLine::ExactLine(const double eye[3], const double direction[3], const Position &start)
{
	ExactRay &ray = exactRay();
	ray.active = true;
	std::copy(eye, eye + 3, ray.eye);
	std::copy(direction, direction + 3, ray.direction);
	ray.start = start;
	ray.crossed = false;
	lineContext().save = nullptr; // (worked out afresh: the same voxels may have been walked before without it)
}

/**
 * Ends the crosshair's exact ray.
 */
ExactLine::~ExactLine()
{
	exactRay().active = false;
	lineContext().save = nullptr;
}

/**
 * Where the last exact line met a picture (see FirstPersonSpriteHits.h).
 */
bool lastExactCrossing(double point[3])
{
	const ExactRay &ray = exactRay();
	if (!ray.crossed) return false;
	std::copy(ray.crossing, ray.crossing + 3, point);
	return true;
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
