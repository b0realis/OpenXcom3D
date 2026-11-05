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
 * FirstPersonVoxelUnits.cpp: units given a body (see FirstPersonVoxelUnits.h).
 *
 * The parts' frames come from the game's own unit drawer (UnitSprite), each part on a frame of its
 * own (setPartCapture) for each of the 8 directions, with the unit's colours as the game draws them;
 * its walk's and death's frames likewise, the drawer told which to draw (setWalkOverride,
 * setFallOverride: the unit itself left as it is).
 * The isometric view draws a unit facing direction d with the projection column = 32 + (x - y),
 * row = 40 + (x + y) / 2 - up (in voxels from the middle of its tile's floor, on these 64x64
 * frames), so each frame is the same body seen from 45 degrees further round.
 */
#include "FirstPersonVoxelUnits.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include "../Battlescape/UnitSprite.h"
#include "../Engine/GraphSubset.h"
#include "../Engine/Script.h"
#include "../Engine/Surface.h"
#include "../Engine/SurfaceSet.h"
#include "../Mod/Armor.h"
#include "../Mod/RuleItem.h"
#include "../Mod/Unit.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"

namespace OpenXcom
{

namespace
{

// ---- The grid a body is made on: half a voxel a cell, 42 voxels up from 2 below the feet, and across and along 28 for
// a unit one tile big, 40 for one two tiles by two (set by useGrid for the unit being made: bodies are made on the
// game's thread only) ----
constexpr float Cell = 0.5f;
constexpr int GridUp = 84;
constexpr float UpFrom = -2.0f;
int GridAcross = 56, GridAlong = 56, GridCells = 56 * 56 * GridUp;
float AcrossFrom = -14.0f, AlongFrom = -14.0f;
inline int cellIndex(int across, int along, int up) { return (across * GridAlong + along) * GridUp + up; }
inline float cellRight(int across) { return AcrossFrom + (across + 0.5f) * Cell; }
inline float cellAhead(int along) { return AlongFrom + (along + 0.5f) * Cell; }
inline float cellUp(int up) { return UpFrom + (up + 0.5f) * Cell; }

// ---- The frames ----
/// The parts are drawn on 64x64 frames with the unit at (16, 8), as the 3D view draws units: the
/// middle of its tile's floor at (32, 40); a big unit's four tiles around their shared corner, at (32, 48).
constexpr int FrameSize = 64, FloorX = 32;
int FloorY = 40;

/// The grid and the floor's place on the frames for a unit of this size (in tiles across).
void useGrid(int size)
{
	const int half = size > 1 ? 20 : 14;
	GridAcross = GridAlong = (int)(2 * half / Cell);
	AcrossFrom = AlongFrom = (float)-half;
	GridCells = GridAcross * GridAlong * GridUp;
	FloorY = size > 1 ? 48 : 40;
}
/// Sprite pixels a voxel across a frame (the projection's x - y along the unit's own axes).
constexpr float PixelsAcross = 1.41421356f;
/// Direction 3 faces the camera; direction 1 shows the unit's right side (its front to the right),
/// direction 5 its left side (its front to the left).
constexpr int Front = 3, RightSide = 1, LeftSide = 5;
/// The parts a body is made of, and the drawer's body parts they come from: the body's own (the first BodyParts), then
/// the items in the right and left hands (their frames only say where the hand holds them).
constexpr int PartCount = 6, BodyParts = 4, RightItem = 4, LeftItem = 5;
constexpr int PartSlots[PartCount] = { BODYPART_TORSO, BODYPART_LEFTARM, BODYPART_RIGHTARM, BODYPART_LEGS, BODYPART_ITEM_RIGHTHAND, BODYPART_ITEM_LEFTHAND };
constexpr Uint8 PartCodes[PartCount] = { VoxelUnitModel::Torso, VoxelUnitModel::LeftArm, VoxelUnitModel::RightArm, VoxelUnitModel::Legs, VoxelUnitModel::Item, VoxelUnitModel::Item };
/// Each palette block's 16 shades pushed this much further from the unit's mean shade in that block (the owner:
/// "put more contrast to the source sprite image to make it more recognisable"); the colours stay the palette's.
constexpr float Contrast = 1.6f;
/// A colour this dark (luminance, of 255) is near black: the sprites' outlines (TFTD's aliens are outlined in black),
/// which on a body's surface would be black blotches; and the contrast doesn't push a colour that dark.
constexpr int NearBlack = 24;
/// A big head on a narrow body (the Sectoid's: 10 pixels on 6) gets a chin at the front of the face and a thin neck
/// behind it (the owner: "head joined to body by neck"): a head at least HeadWidthMin pixels wide on a torso at most
/// NarrowTorso of that; the chin's ChinRows rows above the body ChinDepth voxels deep, the neck NeckRadius round.
constexpr int HeadWidthMin = 9;
constexpr float NarrowTorso = 0.65f, ChinDepth = 2.5f, NeckRadius = 1.3f;
constexpr int ChinRows = 3;
/// The walk: the thighs swing this many degrees either way, the knee bends this much while its leg swings forward,
/// the arms swing against the legs; the torso bobs as the game's frames do (see Routine). A walker drawn on one frame
/// has its legs below WholeHip of its height, each to its side of the middle (what's within WholeMiddle of the widest
/// part of them from the middle stays with the body: between the legs).
constexpr double LegSwing = 22.0, KneeBend = 38.0, ArmSwing = 16.0;
constexpr float WholeHip = 0.42f, WholeMiddle = 0.12f;
/// A body down (its last death frame's pose) is at most this much of its standing height (see fitFall).
constexpr float DownAtMost = 0.4f;

/// How a unit's drawing routine draws it, for its voxel body: in parts on frames of their own, as routine 0 does (a
/// torso with the head, arms, legs: soldiers, Sectoids, Mutons, TFTD's aquanauts and aliens; the Floater without legs,
/// the Snakeman whose legs are its tail, the Chryssalid), or all of it on one frame (civilians, Ethereals and Zombies,
/// the walkers among them, and the creatures: Silacoid, Celatid, Biodrone, Tentaculat); and how it walks: whether its
/// legs swing (a one-frame walker's: its body below the hip), whether its free arms do, and the torso's bob (the
/// routine's own, as the game draws its walk: drawRoutine0's tables for 0, 10, 13-15, drawRoutine6's for the Snakeman;
/// the big walkers' once a footfall).
struct Routine { int number; bool whole, humanoid, legsSwing, armsSwing; int bob[8]; bool big = false; };
const Routine Routines[] = {
	{ 0, false, true, true, true, { 1, 0, -1, 0, 1, 0, -1, 0 } },
	{ 10, false, true, true, true, { 1, 1, 0, 0, 1, 1, 0, 0 } },
	{ 13, false, true, true, true, { 2, 1, 0, 1, 2, 1, 0, 1 } },
	{ 14, false, true, true, true, { 2, 1, 0, 1, 2, 1, 0, 1 } },
	{ 15, false, true, true, true, { 1, 0, 0, 1, 2, 1, 0, 0 } },
	{ 1, false, true, false, false, { 0, 0, 0, 0, 0, 0, 0, 0 } },
	{ 6, false, true, false, false, { 1, 1, 0, -1, -2, -2, -1, 0 } },
	{ 7, false, false, true, true, { 0, 0, 0, 0, 0, 0, 0, 0 } },
	{ 4, true, true, true, false, { 1, 0, -1, 0, 1, 0, -1, 0 } },
	{ 17, true, true, true, false, { 1, 0, -1, 0, 1, 0, -1, 0 } },
	{ 18, true, true, true, false, { 1, 0, -1, 0, 1, 0, -1, 0 } },
	{ 8, true, false, false, false, { 0, 0, 0, 0, 0, 0, 0, 0 } },
	{ 9, true, false, false, false, { 0, 0, 0, 0, 0, 0, 0, 0 } },
	{ 16, true, false, false, false, { 0, 0, 0, 0, 0, 0, 0, 0 } },
	{ 19, true, false, false, false, { 0, 0, 0, 0, 0, 0, 0, 0 } },
	// two tiles by two, all of each on one frame: the tanks, the Cyberdisc, the Sectopod and the Reaper (walkers); TFTD's
	// Coelacanth and Displacer, Hallucinoid, Triscene (a walker) and Xarquid
	{ 2, true, false, false, false, { 0, 0, 0, 0, 0, 0, 0, 0 }, true },
	{ 3, true, false, false, false, { 0, 0, 0, 0, 0, 0, 0, 0 }, true },
	{ 5, true, true, true, false, { 1, 0, 0, 0, 1, 0, 0, 0 }, true },
	{ 11, true, false, false, false, { 0, 0, 0, 0, 0, 0, 0, 0 }, true },
	{ 12, true, false, false, false, { 0, 0, 0, 0, 0, 0, 0, 0 }, true },
	{ 20, true, true, true, false, { 1, 0, 0, 0, 1, 0, 0, 0 }, true },
	{ 21, true, false, false, false, { 0, 0, 0, 0, 0, 0, 0, 0 }, true },
	{ 22, true, false, false, false, { 0, 0, 0, 0, 0, 0, 0, 0 }, true },
};

/// The routine's way of drawing, or null for one not given a voxel body.
const Routine *routineFor(int number)
{
	for (const Routine &routine : Routines) if (routine.number == number) return &routine;
	return nullptr;
}
/// An item in hand: as long as its widest hand frame draws it, its inventory picture slimmer across and through by
/// ItemSlim, and no thicker than a rifle's (whose picture rows are RifleScale voxels each); held in both hands, it lies
/// across the waist tilted ItemTilt, the muzzle to the unit's left and up.
constexpr float ItemSlim = 0.75f, RifleScale = 0.34f;
constexpr double ItemTilt = 20.0;
/// An arm's skeleton (see ArmFrames): two bones of one length from the shoulder to the hand, ArmRadiusMin to ArmRadiusMax
/// thick (the carved arm keeps a thick arm's own shape); a free arm ArmBend longer than its reach (a little bent), and
/// with no free arm, ArmFromTorso of the shoulders' height above the hips (without legs, ArmNoLegs of the shoulders'
/// height: the Floater). Where the hands hold an item, from its butt,
/// of its length: in both hands, the hand holding it at GripButt and the other at GripFore; in one, at GripOne, GripBelow
/// below its axis (a pistol's grip). ArmFarthest: how far from a drawn pixel the fit looks, pixels.
constexpr float ArmRadiusMin = 0.6f, ArmRadiusMax = 1.5f, ArmBend = 1.04f, ArmFromTorso = 1.25f, ArmNoLegs = 0.5f, ArmFarthest = 4.0f;
constexpr float GripButt = 0.3f, GripFore = 0.68f, GripOne = 0.2f, GripBelow = 0.15f;
constexpr double Pi = 3.14159265358979323846;

/// One part's frames for the 8 directions (palette colours, 0 empty).
struct PartFrames
{
	std::vector<Uint8> frame[8];
	bool any = false;
	Uint8 at(int direction, int column, int row) const
	{
		return column >= 0 && row >= 0 && column < FrameSize && row < FrameSize ? frame[direction][row * FrameSize + column] : 0;
	}
};

/// The directions' turns: the unit's right and ahead as map x, y.
struct Turn { float cosine, sine; };
const Turn Turns[8] = { { 1, 0 }, { 0.70710678f, 0.70710678f }, { 0, 1 }, { -0.70710678f, 0.70710678f }, { -1, 0 },
	{ -0.70710678f, -0.70710678f }, { 0, -1 }, { 0.70710678f, -0.70710678f } };

/// Where the view of direction d puts a body point on its frame: column, row and nearness to the camera (larger
/// nearer). Level: as a picture taken at the unit's height (the row by height alone, nearness along the ground);
/// else along the isometric view's slant.
inline void project(float right, float ahead, float up, int d, bool level, float &column, float &row, float &nearness)
{
	const float x = right * Turns[d].cosine + ahead * Turns[d].sine, y = right * Turns[d].sine - ahead * Turns[d].cosine;
	column = FloorX + (x - y);
	if (level)
	{
		row = FloorY - up;
		nearness = x + y;
	}
	else
	{
		row = FloorY + (x + y) * 0.5f - up;
		nearness = x + y + up;
	}
}

/// The runs of drawn pixels in a frame's row: first and last columns.
std::vector<std::pair<int, int>> runs(const PartFrames &part, int direction, int row)
{
	std::vector<std::pair<int, int>> found;
	int start = -1;
	for (int column = 0; column <= FrameSize; ++column)
	{
		const bool drawn = column < FrameSize && part.at(direction, column, row) != 0;
		if (drawn && start < 0) start = column;
		if (!drawn && start >= 0) { found.push_back({ start, column - 1 }); start = -1; }
	}
	return found;
}

/// The cells at the height of a frame's row (a row is a voxel up from the floor's row).
inline bool atRow(int up, int row)
{
	return std::abs(cellUp(up) - (FloorY - row - 0.5f)) <= 0.5f;
}

/// A part's shape from all 8 of its frames: a cell is kept where at most `tolerance` frames put it outside the
/// part's outline (the visual hull, a little forgiving of hand drawn frames that don't quite agree). Only the cells of
/// `only` are looked at (the part's slices: the rest isn't kept anyway).
std::vector<Uint8> carve(const PartFrames &part, int tolerance, const std::vector<Uint8> &only)
{
	std::vector<Uint8> kept(GridCells, 0);
	for (int across = 0; across < GridAcross; ++across)
	{
		for (int along = 0; along < GridAlong; ++along)
		{
			float column[8], rowFloor[8];
			for (int d = 0; d < 8; ++d)
			{
				float nearness;
				project(cellRight(across), cellAhead(along), 0.0f, d, false, column[d], rowFloor[d], nearness);
			}
			for (int up = 0; up < GridUp; ++up)
			{
				if (!only[cellIndex(across, along, up)]) continue;
				int outside = 0;
				for (int d = 0; d < 8 && outside <= tolerance; ++d)
				{
					if (!part.at(d, (int)std::floor(column[d]), (int)std::floor(rowFloor[d] - cellUp(up)))) ++outside;
				}
				kept[cellIndex(across, along, up)] = outside <= tolerance;
			}
		}
	}
	return kept;
}

/// Fills the cells of an elliptical slice at a frame row's height: centre (right, ahead) and half sizes, voxels.
void fillSlice(std::vector<Uint8> &cells, int row, float centreRight, float halfRight, float centreAhead, float halfAhead)
{
	halfRight = std::max(halfRight, 0.6f);
	halfAhead = std::max(halfAhead, 0.6f);
	for (int up = 0; up < GridUp; ++up)
	{
		if (!atRow(up, row)) continue;
		for (int across = 0; across < GridAcross; ++across)
		{
			const float r = (cellRight(across) - centreRight) / halfRight;
			if (r * r > 1.0f) continue;
			for (int along = 0; along < GridAlong; ++along)
			{
				const float a = (cellAhead(along) - centreAhead) / halfAhead;
				if (r * r + a * a <= 1.0f) cells[cellIndex(across, along, up)] = 1;
			}
		}
	}
}

/// A side run (direction 1: the front to the right; 5: to the left) as the unit's ahead axis: back and front edges.
void sideExtent(const std::pair<int, int> &first, const std::pair<int, int> &last, int side, float &back, float &fore)
{
	if (side == RightSide)
	{
		back = (first.first - FloorX) / PixelsAcross;
		fore = (last.second + 1 - FloorX) / PixelsAcross;
	}
	else
	{
		back = (FloorX - (last.second + 1)) / PixelsAcross;
		fore = (FloorX - first.first) / PixelsAcross;
	}
}

/// The head and torso: each row of the frame a slice as wide as the frontal frame's row and as deep as the side
/// frame's (bits beside the main run small round slices); with a chin (bodyTop >= 0), the chin's rows a shallow
/// piece at the front of the face and a thin neck behind them from the head's underside to the torso.
void headAndTorso(const PartFrames &torso, int bodyTop, std::vector<Uint8> &cells, std::vector<Uint8> &neck)
{
	std::vector<float> torsoAhead;
	for (int row = 0; row < FrameSize; ++row)
	{
		const auto front = runs(torso, Front, row), side = runs(torso, RightSide, row);
		if (front.empty() || side.empty()) continue;
		const auto main = *std::max_element(front.begin(), front.end(), [](const std::pair<int, int> &a, const std::pair<int, int> &b) { return a.second - a.first < b.second - b.first; });
		const float centreRight = (FloorX - (main.first + main.second + 1) * 0.5f) / PixelsAcross, halfRight = (main.second - main.first + 1) * 0.5f / PixelsAcross;
		float back, fore;
		sideExtent(side.front(), side.back(), RightSide, back, fore);
		if (bodyTop >= 0 && row >= bodyTop - ChinRows && row < bodyTop) back = std::max(back, fore - ChinDepth);
		if (bodyTop >= 0 && row >= bodyTop) torsoAhead.push_back((back + fore) * 0.5f);
		fillSlice(cells, row, centreRight, halfRight, (back + fore) * 0.5f, (fore - back) * 0.5f);
		for (const auto &run : front)
		{
			if (run == main) continue;
			fillSlice(cells, row, (FloorX - (run.first + run.second + 1) * 0.5f) / PixelsAcross, (run.second - run.first + 1) * 0.5f / PixelsAcross, (back + fore) * 0.5f, 0.6f);
		}
	}
	if (bodyTop < 0) return;
	float neckAhead = 0.0f;
	for (float ahead : torsoAhead) neckAhead += ahead / torsoAhead.size();
	for (int row = bodyTop - ChinRows - 1; row <= bodyTop; ++row)
	{
		fillSlice(neck, row, 0.0f, NeckRadius, neckAhead, NeckRadius);
	}
	for (int i = 0; i < GridCells; ++i) cells[i] |= neck[i];
}

/// An arm or the legs: each run of the frontal frame's rows its own slice (two legs stay two), as deep as the side
/// frame's row where the limb is nearest the camera.
std::vector<Uint8> limb(const PartFrames &part, int side)
{
	std::vector<Uint8> cells(GridCells, 0);
	for (int row = 0; row < FrameSize; ++row)
	{
		const auto front = runs(part, Front, row);
		if (front.empty()) continue;
		const auto profile = runs(part, side, row);
		float back = -0.8f, fore = 0.8f;
		if (!profile.empty()) sideExtent(profile.front(), profile.back(), side, back, fore);
		for (const auto &run : front)
		{
			fillSlice(cells, row, (FloorX - (run.first + run.second + 1) * 0.5f) / PixelsAcross, (run.second - run.first + 1) * 0.5f / PixelsAcross,
				(back + fore) * 0.5f, (fore - back) * 0.5f);
		}
	}
	return cells;
}

/// Where a body with a big head on a narrow torso has its chin's end (the row its body starts: two below the
/// shoulders, where the arms' frames begin), or -1 for a body without (a soldier's head on its neck, a Muton's).
int chinEnd(const PartFrames &torso, const PartFrames &leftArm, const PartFrames &rightArm)
{
	int shoulders = -1;
	for (int row = 0; row < FrameSize && shoulders < 0; ++row)
	{
		for (const PartFrames *arm : { &leftArm, &rightArm })
		{
			if (arm->any && !runs(*arm, Front, row).empty()) { shoulders = row; break; }
		}
	}
	if (shoulders < 0) return -1;
	int headWidest = 0, torsoWidest = 0;
	for (int row = 0; row < FrameSize; ++row)
	{
		int width = 0;
		for (const auto &run : runs(torso, Front, row)) width += run.second - run.first + 1;
		if (row < shoulders) headWidest = std::max(headWidest, width);
		else if (row >= shoulders + 2) torsoWidest = std::max(torsoWidest, width);
	}
	return headWidest >= HeadWidthMin && torsoWidest > 0 && torsoWidest <= NarrowTorso * headWidest ? shoulders + 2 : -1;
}

inline int luminance(const SDL_Color &colour)
{
	return (colour.r * 30 + colour.g * 59 + colour.b * 11) / 100;
}

/// The frames' outlines: a near black pixel with at most two near black neighbours (a line, not a black area such as
/// the Biodrone's dish) takes the colour most of its other neighbours have. Drawn in 2D at a silhouette's edges, they
/// would lie on the body's surface in 3D.
void removeOutlines(PartFrames parts[PartCount], const SDL_Color *palette)
{
	for (int p = 0; p < BodyParts; ++p)
	{
		if (!parts[p].any) continue;
		for (int d = 0; d < 8; ++d)
		{
			std::vector<Uint8> &frame = parts[p].frame[d];
			const std::vector<Uint8> before(frame);
			auto dark = [&](Uint8 colour) { return colour && luminance(palette[colour]) < NearBlack; };
			for (int row = 1; row < FrameSize - 1; ++row)
			{
				for (int column = 1; column < FrameSize - 1; ++column)
				{
					if (!dark(before[row * FrameSize + column])) continue;
					int darkAround = 0, found = 0;
					Uint8 around[8];
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx)
						{
							if (!dx && !dy) continue;
							const Uint8 colour = before[(row + dy) * FrameSize + column + dx];
							if (dark(colour)) ++darkAround;
							else if (colour) around[found++] = colour;
						}
					if (darkAround > 2 || !found) continue;
					Uint8 best = around[0];
					int bestCount = 0;
					for (int i = 0; i < found; ++i)
					{
						const int count = (int)std::count(around, around + found, around[i]);
						if (count > bestCount) { best = around[i]; bestCount = count; }
					}
					frame[row * FrameSize + column] = best;
				}
			}
		}
	}
}

/// Pushes the shades of every used palette block away from the unit's mean shade in that block (Contrast), but never
/// to near black from a colour that wasn't (some ramps, TFTD's dark greens, end in black).
void addContrast(PartFrames parts[PartCount], const SDL_Color *palette)
{
	double sum[16] = {}, count[16] = {};
	for (int p = 0; p < PartCount; ++p)
	{
		if (!parts[p].any) continue;
		for (int d = 0; d < 8; ++d)
		{
			for (Uint8 colour : parts[p].frame[d]) if (colour) { sum[colour / 16] += colour % 16; count[colour / 16] += 1; }
		}
	}
	for (int p = 0; p < PartCount; ++p)
	{
		if (!parts[p].any) continue;
		for (int d = 0; d < 8; ++d)
		{
			for (Uint8 &colour : parts[p].frame[d])
			{
				if (!colour) continue;
				const int block = colour / 16;
				const double mean = sum[block] / count[block];
				int shade = (int)std::lround(std::clamp(mean + (colour % 16 - mean) * Contrast, 0.0, 15.0));
				const bool wasDark = luminance(palette[colour]) < NearBlack;
				while (!wasDark && shade != colour % 16 && luminance(palette[block * 16 + shade]) < NearBlack) shade += shade < colour % 16 ? 1 : -1;
				colour = (Uint8)(block * 16 + shade);
				if (!colour) colour = 1; // (never the transparent colour)
			}
		}
	}
}

/// Turns the chosen points about the line through (ahead 0, up pivot) along the unit's right: feet forward for a
/// positive angle.
void swing(std::vector<float> &ahead, std::vector<float> &up, const std::vector<bool> &chosen, float pivot, double angle)
{
	const float c = (float)std::cos(angle), s = (float)std::sin(angle);
	for (size_t k = 0; k < chosen.size(); ++k)
	{
		if (!chosen[k]) continue;
		const float a = ahead[k], u = up[k] - pivot;
		ahead[k] = a * c - u * s;
		up[k] = pivot + a * s + u * c;
	}
}

/// The walk as its frames show it (drawn with UnitSprite::setWalkOverride): for each phase and each of the two views it's
/// matched in (WalkViews: the right side, where a leg swings across the view, and the front right, where the near leg
/// and the far one part), the frame of its legs (of all of it, for a body drawn on one frame) and of each arm (right,
/// left). Empty for a unit that doesn't walk in its own frames.
constexpr int WalkViews[2] = { RightSide, 2 };
struct WalkFrames
{
	std::vector<Uint8> legs[8][2], arms[8][2][2];
	bool any = false;
};

/// A pose of the walk: each thigh's swing (degrees; more, the foot forward) and knee's bend, each free arm's swing (more,
/// the hand forward), right then left; and the body's bob (voxels down).
struct WalkPose
{
	double thigh[2] = {}, knee[2] = {}, arm[2] = {};
	int bob = 0;
};

/// The walk's 8 poses of a standing body: each leg a thigh and a shin swung about the hip, the knee bending; the free arms
/// swung about the shoulders, not an arm holding an item (the game's frames hold it still); the body bobbing; the lowest
/// foot back on the ground. Each phase posed as the unit's walk frames show it, where it walks in frames of its own: the
/// stride (one thigh forward as much as the other back), the two knees, each free arm, and for a body drawn on one frame
/// the bob, whose outlines in the two views (WalkFrames) agree best with the frames'. Otherwise a stride of LegSwing either
/// way (the right leg forward at phase 0), the knee bending while its leg swings forward, the arms against the legs, the
/// routine's bob (see Routine).
void makeWalk(VoxelUnitModel &model, const Routine &routine, bool leftHolds, bool rightHolds, const WalkFrames &frames)
{
	const size_t count = model.part.size();
	float hip = -1e9f, footStanding = 1e9f;
	for (size_t k = 0; k < count; ++k)
	{
		if (model.part[k] != VoxelUnitModel::Legs) continue;
		hip = std::max(hip, model.up[k]);
		footStanding = std::min(footStanding, model.up[k]);
	}
	const float knee = hip * 0.5f;
	const bool legs = hip > -1e8f && routine.legsSwing;
	const bool holds[2] = { rightHolds, leftHolds };
	const Uint8 armParts[2] = { VoxelUnitModel::RightArm, VoxelUnitModel::LeftArm };
	std::vector<bool> leg[2], shin[2], arm[2];
	float shoulder[2] = { -1e9f, -1e9f };
	for (int side = 0; side < 2; ++side)
	{
		leg[side].resize(count); shin[side].resize(count); arm[side].resize(count);
		for (size_t k = 0; k < count; ++k)
		{
			leg[side][k] = model.part[k] == VoxelUnitModel::Legs && (model.right[k] > 0) == (side == 0);
			shin[side][k] = leg[side][k] && model.up[k] < knee;
			arm[side][k] = model.part[k] == armParts[side];
			if (arm[side][k]) shoulder[side] = std::max(shoulder[side], model.up[k]);
		}
	}
	// A pose's places (the body's points moved).
	auto posePoints = [&](const WalkPose &pose, std::vector<float> &ahead, std::vector<float> &up) {
		ahead = model.ahead;
		up = model.up;
		for (int side = 0; side < 2 && legs; ++side)
		{
			swing(ahead, up, shin[side], knee, -pose.knee[side] * Pi / 180.0);
			swing(ahead, up, leg[side], hip, pose.thigh[side] * Pi / 180.0);
		}
		for (int side = 0; side < 2; ++side)
		{
			if (routine.armsSwing && !holds[side] && shoulder[side] > -1e8f && pose.arm[side] != 0.0) swing(ahead, up, arm[side], shoulder[side], pose.arm[side] * Pi / 180.0);
		}
		float footNow = 1e9f;
		for (size_t k = 0; k < count; ++k)
		{
			if (model.part[k] != VoxelUnitModel::Legs) up[k] -= pose.bob;
			else footNow = std::min(footNow, up[k]);
		}
		if (footNow < 1e8f) for (float &height : up) height += footStanding - footNow;
	};
	// How well some of the body's points in a pose agree with a frame, in both views (each the most of the two outlines
	// in both); every other point is enough for an outline.
	std::vector<Uint8> outline(FrameSize * FrameSize);
	auto agreement = [&](const std::vector<float> &ahead, const std::vector<float> &up, const std::vector<bool> *which, const std::vector<Uint8> *viewFrames[2]) {
		float total = 0.0f;
		for (int view = 0; view < 2; ++view)
		{
			const std::vector<Uint8> &frame = *viewFrames[view];
			if (frame.empty()) continue;
			std::fill(outline.begin(), outline.end(), 0);
			for (size_t k = 0; k < count; k += 2)
			{
				if (which && !(*which)[k]) continue;
				float column, row, nearness;
				project(model.right[k], ahead[k], up[k], WalkViews[view], false, column, row, nearness);
				const int c = (int)std::floor(column), r = (int)std::floor(row);
				if (c >= 0 && r >= 0 && c < FrameSize && r < FrameSize) outline[r * FrameSize + c] = 1;
			}
			int both = 0, either = 0;
			for (int i = 0; i < FrameSize * FrameSize; ++i)
			{
				both += outline[i] && frame[i];
				either += outline[i] || frame[i];
			}
			total += either ? (float)both / either : 0.0f;
		}
		return total;
	};
	std::vector<bool> legPoints(count);
	for (size_t k = 0; k < count; ++k) legPoints[k] = model.part[k] == VoxelUnitModel::Legs;
	for (int phase = 0; phase < 8; ++phase)
	{
		// the routine's walk: a stride either way, the knee bending as its leg swings forward, the arms against the legs
		WalkPose pose;
		for (int side = 0; side < 2; ++side)
		{
			const double cycle = 2.0 * Pi * (phase / 8.0 + (side == 0 ? 0.0 : 0.5));
			pose.thigh[side] = LegSwing * std::cos(cycle);
			pose.knee[side] = KneeBend * std::max(0.0, -std::sin(cycle));
			pose.arm[side] = ArmSwing * std::cos(2.0 * Pi * phase / 8.0) * (side == 0 ? -1.0 : 1.0);
		}
		pose.bob = routine.bob[phase];
		std::vector<float> ahead, up;
		if (frames.any)
		{
			// as the walk frames show it: for a body drawn on one frame, first its bob, all of it against the frames;
			// then the legs (theirs, or all of it again) and each free arm against its own frames
			const std::vector<Uint8> *legFrames[2] = { &frames.legs[phase][0], &frames.legs[phase][1] };
			if (routine.whole)
			{
				float best = -1.0f;
				int bestBob = pose.bob;
				for (int bob = -1; bob <= 2; ++bob)
				{
					pose.bob = bob;
					posePoints(pose, ahead, up);
					const float here = agreement(ahead, up, nullptr, legFrames);
					if (here > best) { best = here; bestBob = bob; }
				}
				pose.bob = bestBob;
			}
			if (legs)
			{
				float best = -1.0f;
				WalkPose bestPose = pose;
				for (int stride = -4; stride <= 4; ++stride)
					for (int kneeRight = 0; kneeRight <= 2; ++kneeRight)
						for (int kneeLeft = 0; kneeLeft <= 2; ++kneeLeft)
						{
							WalkPose tried = pose;
							tried.thigh[0] = stride * 7.5;
							tried.thigh[1] = -stride * 7.5;
							tried.knee[0] = kneeRight * 25.0;
							tried.knee[1] = kneeLeft * 25.0;
							posePoints(tried, ahead, up);
							const float here = agreement(ahead, up, routine.whole ? nullptr : &legPoints, legFrames);
							if (here > best) { best = here; bestPose = tried; }
						}
				pose = bestPose;
			}
			for (int side = 0; side < 2 && routine.armsSwing; ++side)
			{
				if (holds[side] || shoulder[side] <= -1e8f) continue;
				const std::vector<Uint8> *armFrames[2] = { &frames.arms[phase][0][side], &frames.arms[phase][1][side] };
				float best = -1.0f;
				double bestSwing = pose.arm[side];
				for (int swung = -4; swung <= 4; ++swung)
				{
					WalkPose tried = pose;
					tried.arm[side] = swung * 7.5;
					posePoints(tried, ahead, up);
					const float here = agreement(ahead, up, &arm[side], armFrames);
					if (here > best) { best = here; bestSwing = tried.arm[side]; }
				}
				pose.arm[side] = bestSwing;
			}
		}
		posePoints(pose, ahead, up);
		model.walkRight[phase] = model.right;
		model.walkAhead[phase] = std::move(ahead);
		model.walkUp[phase] = std::move(up);
	}
}

/// An item in a hand: its inventory picture, as the game draws it in the hand (palette colours, 0 empty), and how it's held.
struct HeldItem
{
	std::vector<Uint8> picture;
	int width = 0, height = 0;
	bool twoHanded = false;
};

/// Where the 8 frames of a part agree its middle is (right, ahead, up): the point whose projections come nearest the
/// middles of its pixels in each frame (least squares); false if fewer than 2 frames show it.
bool partMiddle(const PartFrames &part, float middle[3])
{
	double normal[3][3] = {}, sums[3] = {};
	int views = 0;
	for (int d = 0; d < 8; ++d)
	{
		double columns = 0.0, rows = 0.0;
		int count = 0;
		for (int row = 0; row < FrameSize; ++row)
		{
			for (int column = 0; column < FrameSize; ++column)
			{
				if (!part.frame[d][row * FrameSize + column]) continue;
				columns += column + 0.5;
				rows += row + 0.5;
				++count;
			}
		}
		if (!count) continue;
		++views;
		// column - FloorX = right (cos - sin) + ahead (sin + cos); row - FloorY = (right (cos + sin) + ahead (sin - cos)) / 2 - up
		const double c = Turns[d].cosine, s = Turns[d].sine;
		const double equations[2][4] = { { c - s, s + c, 0.0, columns / count - FloorX }, { (c + s) / 2.0, (s - c) / 2.0, -1.0, rows / count - FloorY } };
		for (const auto &equation : equations)
		{
			for (int i = 0; i < 3; ++i)
			{
				for (int j = 0; j < 3; ++j) normal[i][j] += equation[i] * equation[j];
				sums[i] += equation[i] * equation[3];
			}
		}
	}
	if (views < 2) return false;
	auto determinant = [](const double m[3][3]) {
		return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	};
	const double whole = determinant(normal);
	if (std::abs(whole) < 1e-9) return false;
	for (int k = 0; k < 3; ++k)
	{
		double replaced[3][3];
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j) replaced[i][j] = j == k ? sums[i] : normal[i][j];
		middle[k] = (float)(determinant(replaced) / whole);
	}
	return true;
}

/// Where an item in hand is on the body: its middle (where its hand frames agree it is), its length (as its widest hand
/// frame draws it across: the inventory pictures draw a pistol nearly as big as a rifle), and its axes: along it from the
/// butt to the muzzle, across its picture to the picture's right (the magazine's side), and through it.
struct ItemPlace
{
	float middle[3], length;
	float along[3], across[3], through[3];
};

/// Where an item in hand is, from its hand frames and how it's held: pointing ahead when aimed or held in one hand,
/// across the waist when held in both, its picture's right (the magazine's side) down; false if no frame shows it.
bool placeItem(const PartFrames &frames, bool twoHanded, bool aiming, ItemPlace &place)
{
	if (!frames.any || !partMiddle(frames, place.middle)) return false;
	int widest = 0;
	for (int d = 0; d < 8; ++d)
	{
		int left = FrameSize, right = -1;
		for (int row = 0; row < FrameSize; ++row)
		{
			for (int column = 0; column < FrameSize; ++column)
			{
				if (!frames.frame[d][row * FrameSize + column]) continue;
				left = std::min(left, column);
				right = std::max(right, column);
			}
		}
		widest = std::max(widest, right - left + 1);
	}
	place.length = std::max(1.0f, (widest - 1) / PixelsAcross);
	const float c = (float)std::cos(ItemTilt * Pi / 180.0), s = (float)std::sin(ItemTilt * Pi / 180.0);
	const float ahead[3][3] = { { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 0.0f, 0.0f } };
	const float lying[3][3] = { { -c, 0.0f, s }, { -s, 0.0f, -c }, { 0.0f, 1.0f, 0.0f } };
	const float (&axes)[3][3] = twoHanded && !aiming ? lying : ahead;
	std::copy(axes[0], axes[0] + 3, place.along);
	std::copy(axes[1], axes[1] + 3, place.across);
	std::copy(axes[2], axes[2] + 3, place.through);
	return true;
}

/// Where a hand holds an item: in both hands, the hand holding it near the butt and the other further along; in one,
/// near the butt and below the axis (a pistol's grip).
void gripOf(const ItemPlace &place, bool twoHanded, bool atButt, float hand[3])
{
	const float along = place.length * ((twoHanded ? (atButt ? GripButt : GripFore) : GripOne) - 0.5f);
	const float below = twoHanded ? 0.0f : place.length * GripBelow;
	for (int i = 0; i < 3; ++i) hand[i] = place.middle[i] + along * place.along[i] + below * place.across[i];
}

inline float distance3(const float a[3], const float b[3])
{
	return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

/// The length of (x, y). (Not std::hypot: on floats that is hypotf, which asks for glibc 2.35, and the release must
/// start on 2.34: see tools/portable/check.sh.)
inline float planeLength(float x, float y)
{
	return std::sqrt(x * x + y * y);
}

/// The distance from a point to the segment a - b, on a frame.
inline float segmentDistance(float x, float y, float ax, float ay, float bx, float by)
{
	const float abx = bx - ax, aby = by - ay, length2 = abx * abx + aby * aby;
	const float t = length2 > 1e-9f ? std::clamp(((x - ax) * abx + (y - ay) * aby) / length2, 0.0f, 1.0f) : 0.0f;
	return planeLength(x - (ax + t * abx), y - (ay + t * aby));
}

/// An arm's frames as its skeleton is fitted to them. A thin arm (a soldier's, a Sectoid's: two pixels) is drawn a
/// little differently in each of its 8 frames, so carving keeps next to nothing of it, and an arm reaching forward (a
/// pistol's, the rifle's) slants down the frontal frame, where the slices put it lower. So the arm is also given a
/// skeleton: a shoulder, an elbow and a hand, the elbow placed where the frames agree best. Each frame's drawn pixels
/// (their middles), and for every pixel of the frame how far the nearest drawn one is (up to ArmFarthest).
struct ArmFrames
{
	std::vector<float> columns[8], rows[8], distance[8];
	explicit ArmFrames(const PartFrames &part)
	{
		for (int d = 0; d < 8; ++d)
		{
			distance[d].assign(FrameSize * FrameSize, ArmFarthest);
			for (int row = 0; row < FrameSize; ++row)
			{
				for (int column = 0; column < FrameSize; ++column)
				{
					if (!part.frame[d][row * FrameSize + column]) continue;
					columns[d].push_back(column + 0.5f);
					rows[d].push_back(row + 0.5f);
					for (int y = std::max(0, row - 4); y <= std::min(FrameSize - 1, row + 4); ++y)
					{
						for (int x = std::max(0, column - 4); x <= std::min(FrameSize - 1, column + 4); ++x)
						{
							float &here = distance[d][y * FrameSize + x];
							here = std::min(here, planeLength((float)(x - column), (float)(y - row)));
						}
					}
				}
			}
		}
	}
	/// How badly a shoulder - elbow - hand line fits the frames: the line's points off the drawn arm, and the drawn arm
	/// away from the line (each capped, so that a frame drawing the arm differently doesn't rule).
	float cost(const float shoulder[3], const float elbow[3], const float hand[3]) const
	{
		float total = 0.0f;
		const float *joints[3] = { shoulder, elbow, hand };
		for (int d = 0; d < 8; ++d)
		{
			if (columns[d].empty()) continue;
			float column[3], row[3], nearness;
			for (int j = 0; j < 3; ++j) project(joints[j][0], joints[j][1], joints[j][2], d, false, column[j], row[j], nearness);
			float on = 0.0f;
			int samples = 0;
			for (int bone = 0; bone < 2; ++bone)
			{
				const float dx = column[bone + 1] - column[bone], dy = row[bone + 1] - row[bone];
				const int steps = std::max(2, (int)(planeLength(dx, dy) * 2.0f));
				for (int i = 0; i < steps; ++i)
				{
					const float t = i / (float)(steps - 1);
					const int x = std::clamp((int)std::floor(column[bone] + t * dx), 0, FrameSize - 1), y = std::clamp((int)std::floor(row[bone] + t * dy), 0, FrameSize - 1);
					const float off = distance[d][y * FrameSize + x];
					on += std::min(off * off, 9.0f);
					++samples;
				}
			}
			float cover = 0.0f;
			for (size_t i = 0; i < columns[d].size(); ++i)
			{
				const float off = std::min(segmentDistance(columns[d][i], rows[d][i], column[0], row[0], column[1], row[1]),
					segmentDistance(columns[d][i], rows[d][i], column[1], row[1], column[2], row[2]));
				cover += std::min(off * off, 16.0f);
			}
			total += on / samples + cover / columns[d].size();
		}
		return total;
	}
};

/// The top (or the bottom) of an arm's drawing as a point on the body: the frontal frame gives its right, and its height
/// less 0.7 of how far ahead it is; the near side frame (1 for the right arm, 5 for the left) how far ahead. False if
/// either frame is empty.
bool armEnd(const PartFrames &part, bool rightArm, bool top, float point[3])
{
	float columns[2], rows[2];
	const int views[2] = { Front, rightArm ? RightSide : LeftSide };
	for (int v = 0; v < 2; ++v)
	{
		int found = -1;
		for (int i = 0; i < FrameSize && found < 0; ++i)
		{
			const int row = top ? i : FrameSize - 1 - i;
			for (int column = 0; column < FrameSize && found < 0; ++column) if (part.frame[views[v]][row * FrameSize + column]) found = row;
		}
		if (found < 0) return false;
		float sum = 0.0f;
		int count = 0;
		for (int column = 0; column < FrameSize; ++column) if (part.frame[views[v]][found * FrameSize + column]) { sum += column + 0.5f; ++count; }
		columns[v] = sum / count;
		rows[v] = found + 0.5f;
	}
	point[0] = (FloorX - columns[0]) / PixelsAcross;
	point[1] = (rightArm ? columns[1] - FloorX : FloorX - columns[1]) / PixelsAcross;
	point[2] = FloorY + point[1] / PixelsAcross - rows[0];
	return true;
}

/// The elbow of an arm from the shoulder to the hand with two bones this long: of the circle of places it can be, the
/// one the frames agree with best (a little in favour of lower than halfway between shoulder and hand); halfway when the
/// hand is at the arm's full reach.
void placeElbow(const float shoulder[3], const float hand[3], float bone, const ArmFrames &frames, float elbow[3])
{
	float axis[3] = { hand[0] - shoulder[0], hand[1] - shoulder[1], hand[2] - shoulder[2] };
	const float reach = distance3(shoulder, hand);
	for (int i = 0; i < 3; ++i) elbow[i] = (shoulder[i] + hand[i]) * 0.5f;
	if (reach >= 2.0f * bone - 1e-3f || reach < 1e-3f) return;
	for (float &a : axis) a /= reach;
	// two ways square to the arm: across it level, and the third
	float side[3] = { axis[1], -axis[0], 0.0f };
	if (planeLength(side[0], side[1]) < 1e-4f) { side[0] = -axis[2]; side[1] = 0.0f; side[2] = axis[0]; }
	const float sideLength = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
	for (float &a : side) a /= sideLength;
	const float other[3] = { axis[1] * side[2] - axis[2] * side[1], axis[2] * side[0] - axis[0] * side[2], axis[0] * side[1] - axis[1] * side[0] };
	const float radius = std::sqrt(bone * bone - reach * reach * 0.25f);
	float best = 1e30f;
	for (int k = 0; k < 36; ++k)
	{
		const float angle = (float)(2.0 * Pi * k / 36.0), c = std::cos(angle), s = std::sin(angle);
		float candidate[3];
		for (int i = 0; i < 3; ++i) candidate[i] = (shoulder[i] + hand[i]) * 0.5f + radius * (c * side[i] + s * other[i]);
		const float cost = frames.cost(shoulder, candidate, hand) + 0.3f * std::max(0.0f, candidate[2] - (shoulder[2] + hand[2]) * 0.5f);
		if (cost < best)
		{
			best = cost;
			std::copy(candidate, candidate + 3, elbow);
		}
	}
}

/// The arm's two bones' radii: the frames' pixels nearer each bone than the other over its drawn length is its width
/// (pixels, about 1.2 to a voxel), within ArmRadiusMin and ArmRadiusMax.
void armRadii(const ArmFrames &frames, const float shoulder[3], const float elbow[3], const float hand[3], float radius[2])
{
	float count[2] = {}, length[2] = {};
	const float *joints[3] = { shoulder, elbow, hand };
	for (int d = 0; d < 8; ++d)
	{
		if (frames.columns[d].empty()) continue;
		float column[3], row[3], nearness;
		for (int j = 0; j < 3; ++j) project(joints[j][0], joints[j][1], joints[j][2], d, false, column[j], row[j], nearness);
		for (size_t i = 0; i < frames.columns[d].size(); ++i)
		{
			const float upper = segmentDistance(frames.columns[d][i], frames.rows[d][i], column[0], row[0], column[1], row[1]);
			const float lower = segmentDistance(frames.columns[d][i], frames.rows[d][i], column[1], row[1], column[2], row[2]);
			count[upper <= lower ? 0 : 1] += 1.0f;
		}
		for (int bone = 0; bone < 2; ++bone) length[bone] += planeLength(column[bone + 1] - column[bone], row[bone + 1] - row[bone]) + 1.0f;
	}
	for (int bone = 0; bone < 2; ++bone) radius[bone] = std::clamp(count[bone] / std::max(length[bone], 1.0f) / 2.0f / 1.2f, ArmRadiusMin, ArmRadiusMax);
}

/// Fills the cells within a radius of the segment a - b.
void fillCapsule(std::vector<Uint8> &cells, const float a[3], const float b[3], float radius)
{
	auto range = [&](int axis, float from, int count, int &first, int &last) {
		first = std::max(0, (int)std::floor((std::min(a[axis], b[axis]) - radius - from) / Cell));
		last = std::min(count - 1, (int)std::ceil((std::max(a[axis], b[axis]) + radius - from) / Cell));
	};
	int across0, across1, along0, along1, up0, up1;
	range(0, AcrossFrom, GridAcross, across0, across1);
	range(1, AlongFrom, GridAlong, along0, along1);
	range(2, UpFrom, GridUp, up0, up1);
	const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, length2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
	for (int across = across0; across <= across1; ++across)
		for (int along = along0; along <= along1; ++along)
			for (int up = up0; up <= up1; ++up)
			{
				const float point[3] = { cellRight(across) - a[0], cellAhead(along) - a[1], cellUp(up) - a[2] };
				const float t = length2 > 1e-9f ? std::clamp((point[0] * ab[0] + point[1] * ab[1] + point[2] * ab[2]) / length2, 0.0f, 1.0f) : 0.0f;
				const float off[3] = { point[0] - t * ab[0], point[1] - t * ab[1], point[2] - t * ab[2] };
				if (off[0] * off[0] + off[1] * off[1] + off[2] * off[2] <= radius * radius) cells[cellIndex(across, along, up)] = 1;
			}
}

/// Adds an item in hand to a body: its inventory picture given volume (cushion style, as the 3D view's weapon in hand:
/// each drawn pixel a column through its thickness, thicker inside its outline and rounded at its edges; only the
/// points on its surface kept), placed as its hand frames show it (see placeItem). The games draw an item standing up
/// in its picture, a gun's muzzle at the top. Its colours are the picture's, the same from every side.
void addItem(VoxelUnitModel &model, const HeldItem &held, const ItemPlace &place)
{
	if (held.picture.empty()) return;
	const int width = held.width, height = held.height;
	// How deep inside the picture's outline each drawn pixel is: 1 at the outline, up to 5.
	std::vector<int> depth(width * height, 0);
	std::vector<Uint8> inside(width * height);
	for (int i = 0; i < width * height; ++i) inside[i] = held.picture[i] != 0;
	auto at = [&](const std::vector<Uint8> &cells, int x, int y) { return x >= 0 && y >= 0 && x < width && y < height && cells[y * width + x]; };
	for (int step = 1; step <= 5; ++step)
	{
		std::vector<Uint8> deeper(width * height, 0);
		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
			{
				if (!inside[y * width + x]) continue;
				depth[y * width + x] = step;
				deeper[y * width + x] = at(inside, x - 1, y) && at(inside, x + 1, y) && at(inside, x, y - 1) && at(inside, x, y + 1);
			}
		}
		inside.swap(deeper);
	}
	int top = height, bottom = -1, left = width, right = -1;
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			if (!held.picture[y * width + x]) continue;
			top = std::min(top, y); bottom = std::max(bottom, y);
			left = std::min(left, x); right = std::max(right, x);
		}
	}
	if (bottom < 0) return;
	const float *along = place.along, *across = place.across, *through = place.through, *middle = place.middle;
	const float scale = place.length / std::max(1, bottom - top), thin = std::min(1.0f, scale / RifleScale);
	const float halfLength = (bottom - top) * scale * 0.5f, middleAcross = (left + right) * 0.5f;
	auto halfThickness = [&](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height && depth[y * width + x] ? std::min(1.4f, 0.45f + 0.35f * depth[y * width + x]) * thin : 0.0f; };
	for (int y = top; y <= bottom; ++y)
	{
		for (int x = left; x <= right; ++x)
		{
			const Uint8 colour = held.picture[y * width + x];
			if (!colour) continue;
			const float half = halfThickness(x, y);
			const float sideways = std::min({ halfThickness(x - 1, y), halfThickness(x + 1, y), halfThickness(x, y - 1), halfThickness(x, y + 1) });
			const float a = (bottom - y) * scale - halfLength, c = (x - middleAcross) * scale * ItemSlim;
			for (float t = -half; t <= half + 1e-4f; t += Cell * thin)
			{
				// (a column's two ends, and its points that stand out beside a thinner neighbour or the outline)
				if (t > -half + 1e-4f && t + Cell * thin <= half + 1e-4f && std::abs(t) <= sideways) continue;
				const float thick = t * ItemSlim;
				model.right.push_back(middle[0] + a * along[0] + c * across[0] + thick * through[0]);
				model.ahead.push_back(middle[1] + a * along[1] + c * across[1] + thick * through[1]);
				model.up.push_back(middle[2] + a * along[2] + c * across[2] + thick * through[2]);
				model.part.push_back(VoxelUnitModel::Item);
				model.colours.insert(model.colours.end(), 16, colour);
			}
		}
	}
}

/// The fall a body takes, as its death frames show it, ending as its corpse's picture lies: for each picture the pose
/// (see VoxelUnitModel::FallPose) whose outline seen as the picture sees it (direction 3, along the isometric slant)
/// agrees best with the picture's (the most of the two outlines in both), tipping the way the last shows (the body down:
/// no taller than DownAtMost of its standing height) all along; then each picture paints the body in its pose (the
/// voxels it sees, nearest first). Without either, it tips over backwards.
void fitFall(VoxelUnitModel &model, const std::vector<std::vector<Uint8>> &deaths, const std::vector<Uint8> &corpse)
{
	const size_t count = model.part.size();
	std::vector<const std::vector<Uint8>*> pictures;
	for (const auto &death : deaths) pictures.push_back(&death);
	if (!corpse.empty()) pictures.push_back(&corpse);
	if (pictures.empty())
	{
		model.fall.assign(1, VoxelUnitModel::FallPose{ 1.0f, 1.0f, 90.0f });
		model.fallToward = 4;
		return;
	}
	std::vector<size_t> body; // (every other voxel: enough for an outline)
	for (size_t k = 0; k < count; k += 2) if (model.part[k] != VoxelUnitModel::Item) body.push_back(k);
	std::vector<Uint8> outline(FrameSize * FrameSize);
	float standingTop = 0.0f;
	for (size_t k : body) standingTop = std::max(standingTop, model.up[k]);
	auto agreement = [&](const VoxelUnitModel::FallPose &pose, const std::vector<Uint8> &death, float &top) {
		std::fill(outline.begin(), outline.end(), 0);
		top = 0.0f;
		for (size_t k : body)
		{
			float right = model.right[k], ahead = model.ahead[k], up = model.up[k], column, row, nearness;
			model.posed(pose, right, ahead, up);
			top = std::max(top, up);
			project(right, ahead, up, Front, false, column, row, nearness);
			const int c = (int)std::floor(column), r = (int)std::floor(row);
			if (c >= 0 && r >= 0 && c < FrameSize && r < FrameSize) outline[r * FrameSize + c] = 1;
		}
		int both = 0, either = 0;
		for (int i = 0; i < FrameSize * FrameSize; ++i)
		{
			const bool drawn = death[i] != 0;
			both += outline[i] && drawn;
			either += outline[i] || drawn;
		}
		return either ? (float)both / either : 0.0f;
	};
	const float heights[] = { 1.0f, 0.85f, 0.7f, 0.55f, 0.4f, 0.3f, 0.22f }, spreads[] = { 1.0f, 1.2f, 1.4f, 1.6f, 1.8f };
	const float angles[] = { 0.0f, 15.0f, 30.0f, 45.0f, 60.0f, 75.0f, 90.0f };
	auto bestPose = [&](const std::vector<Uint8> &death, int firstToward, int lastToward, int &toward, bool down) {
		VoxelUnitModel::FallPose best;
		float bestAgreement = -1.0f;
		for (int way = firstToward; way <= lastToward; ++way)
		{
			model.fallToward = way;
			for (float angle : angles)
			{
				if (angle == 0.0f && way != firstToward) continue; // (upright: the same whichever way)
				for (float height : heights)
					for (float spread : spreads)
					{
						const VoxelUnitModel::FallPose pose{ height, spread, angle };
						float top;
						const float here = agreement(pose, death, top);
						if (down && top > standingTop * DownAtMost) continue;
						if (here > bestAgreement) { bestAgreement = here; best = pose; toward = way; }
					}
			}
		}
		return best;
	};
	int toward = 4;
	model.fall.assign(pictures.size(), VoxelUnitModel::FallPose());
	model.fall.back() = bestPose(*pictures.back(), 0, 7, toward, true);
	model.fallToward = toward;
	for (size_t frame = 0; frame + 1 < pictures.size(); ++frame)
	{
		int same = toward;
		model.fall[frame] = bestPose(*pictures[frame], toward, toward, same, false);
	}
	model.fallToward = toward;
	// Each frame paints the body in its pose: the voxels nearest the frame's view at each pixel (and within a voxel of them).
	model.fallColours.assign(pictures.size() * count, 0);
	std::vector<int> pixelOf(count);
	std::vector<float> nearnessOf(count), front(FrameSize * FrameSize);
	for (size_t frame = 0; frame < pictures.size(); ++frame)
	{
		std::fill(front.begin(), front.end(), -1e9f);
		for (size_t k = 0; k < count; ++k)
		{
			float right = model.right[k], ahead = model.ahead[k], up = model.up[k], column, row;
			model.posed(model.fall[frame], right, ahead, up);
			project(right, ahead, up, Front, false, column, row, nearnessOf[k]);
			const int c = (int)std::floor(column), r = (int)std::floor(row);
			pixelOf[k] = c >= 0 && r >= 0 && c < FrameSize && r < FrameSize ? r * FrameSize + c : -1;
			if (pixelOf[k] >= 0) front[pixelOf[k]] = std::max(front[pixelOf[k]], nearnessOf[k]);
		}
		for (size_t k = 0; k < count; ++k)
		{
			if (pixelOf[k] >= 0 && nearnessOf[k] >= front[pixelOf[k]] - 1.0f) model.fallColours[frame * count + k] = (*pictures[frame])[pixelOf[k]];
		}
	}
}

/// Makes a unit's body from its parts' frames, the items in its hands (right, left), its death frames, its corpse's
/// picture and its walk frames; null if it has no torso.
std::unique_ptr<VoxelUnitModel> makeModel(PartFrames parts[PartCount], const HeldItem items[2], const std::vector<std::vector<Uint8>> &deaths,
	const std::vector<Uint8> &corpse, const WalkFrames &walks, bool aiming, const Routine &routine, const SDL_Color *palette)
{
	if (!parts[0].any) return nullptr;
	useGrid(routine.big ? 2 : 1);
	removeOutlines(parts, palette);
	addContrast(parts, palette);

	// The parts' shapes, each claiming its cells in this order (the torso first).
	std::vector<Uint8> owner(GridCells, 0);
	if (routine.whole)
	{
		// all of it on one frame: each run of each row its own slice (two legs stay two, an arm beside the body its own)
		const std::vector<Uint8> shape = limb(parts[0], RightSide), hull = carve(parts[0], 1, shape);
		for (int i = 0; i < GridCells; ++i) if (shape[i] && hull[i]) owner[i] = PartCodes[0];
	}
	else
	{
		std::vector<Uint8> torso(GridCells, 0), neck(GridCells, 0);
		headAndTorso(parts[0], chinEnd(parts[0], parts[1], parts[2]), torso, neck);
		const std::vector<Uint8> hull = carve(parts[0], 1, torso);
		for (int i = 0; i < GridCells; ++i) if ((torso[i] && hull[i]) || neck[i]) owner[i] = PartCodes[0];
	}
	// The items in hand, where their frames show them, and the arms' skeletons (see ArmFrames): each from the top of its
	// drawing to its hand (on the item it holds, or the bottom of its drawing), as long as a free arm reaches.
	ItemPlace places[2];
	const bool placed[2] = { placeItem(parts[RightItem], items[0].twoHanded, aiming, places[0]), placeItem(parts[LeftItem], items[1].twoHanded, aiming, places[1]) };
	std::vector<Uint8> skeletons[BodyParts];
	{
		struct Arm { int part; bool right, holds, found; float shoulder[3], hand[3]; };
		Arm arms[2] = { { 1, false, false, false, {}, {} }, { 2, true, false, false, {}, {} } };
		for (Arm &arm : arms)
		{
			const int own = arm.right ? 0 : 1, other = 1 - own;
			arm.found = parts[arm.part].any && armEnd(parts[arm.part], arm.right, true, arm.shoulder);
			if (placed[own]) { gripOf(places[own], items[own].twoHanded, true, arm.hand); arm.holds = true; }
			else if (placed[other] && items[other].twoHanded) { gripOf(places[other], true, false, arm.hand); arm.holds = true; }
			else arm.found = arm.found && armEnd(parts[arm.part], arm.right, false, arm.hand);
		}
		float length = 0.0f, shoulders = 0.0f, legsTop = 0.0f;
		int found = 0;
		for (const Arm &arm : arms)
		{
			if (!arm.found) continue;
			if (!arm.holds) length = std::max(length, distance3(arm.shoulder, arm.hand) * ArmBend);
			shoulders += arm.shoulder[2];
			++found;
		}
		if (length <= 0.0f && found && !parts[3].any)
		{
			length = std::max(4.0f, shoulders / found * ArmNoLegs);
		}
		else if (length <= 0.0f && found)
		{
			for (int d : { Front, RightSide })
			{
				for (int row = 0; row < FrameSize; ++row)
				{
					if (!runs(parts[3], d, row).empty()) { legsTop = std::max(legsTop, (float)(FloorY - row)); break; }
				}
			}
			length = std::max(4.0f, (shoulders / found - legsTop) * ArmFromTorso);
		}
		for (const Arm &arm : arms)
		{
			if (!arm.found) continue;
			const ArmFrames frames(parts[arm.part]);
			const float bone = std::max(length, distance3(arm.shoulder, arm.hand) * 1.02f) * 0.5f;
			float elbow[3], radius[2];
			placeElbow(arm.shoulder, arm.hand, bone, frames, elbow);
			armRadii(frames, arm.shoulder, elbow, arm.hand, radius);
			std::vector<Uint8> &cells = skeletons[arm.part];
			cells.assign(GridCells, 0);
			fillCapsule(cells, arm.shoulder, elbow, radius[0]);
			fillCapsule(cells, elbow, arm.hand, radius[1]);
		}
	}
	for (int p = 1; p < BodyParts; ++p)
	{
		if (!parts[p].any) continue;
		// the left arm is nearest the camera in direction 5, the right arm and the legs in 1; an arm's skeleton joins it
		std::vector<Uint8> shape = limb(parts[p], PartCodes[p] == VoxelUnitModel::LeftArm ? LeftSide : RightSide);
		const std::vector<Uint8> hull = carve(parts[p], 2, shape);
		const bool skeleton = !skeletons[p].empty();
		for (int i = 0; i < GridCells; ++i) if (((shape[i] && hull[i]) || (skeleton && skeletons[p][i])) && !owner[i]) owner[i] = PartCodes[p];
	}
	// Loose cells (fewer than 3 of their 6 neighbours filled) go, and with them the specks they'd show.
	auto filled = [&](int across, int along, int up) {
		return across >= 0 && along >= 0 && up >= 0 && across < GridAcross && along < GridAlong && up < GridUp && owner[cellIndex(across, along, up)];
	};
	std::vector<Uint8> keep(owner);
	for (int across = 0; across < GridAcross; ++across)
		for (int along = 0; along < GridAlong; ++along)
			for (int up = 0; up < GridUp; ++up)
			{
				if (!owner[cellIndex(across, along, up)]) continue;
				const int neighbours = filled(across - 1, along, up) + filled(across + 1, along, up) + filled(across, along - 1, up)
					+ filled(across, along + 1, up) + filled(across, along, up - 1) + filled(across, along, up + 1);
				if (neighbours < 3) keep[cellIndex(across, along, up)] = 0;
			}
	owner.swap(keep);

	// The surface: filled cells with an empty neighbour.
	auto model = std::make_unique<VoxelUnitModel>();
	std::vector<int> surfaceCells;
	for (int across = 0; across < GridAcross; ++across)
		for (int along = 0; along < GridAlong; ++along)
			for (int up = 0; up < GridUp; ++up)
			{
				if (!owner[cellIndex(across, along, up)]) continue;
				if (filled(across - 1, along, up) && filled(across + 1, along, up) && filled(across, along - 1, up) && filled(across, along + 1, up)
					&& filled(across, along, up - 1) && filled(across, along, up + 1)) continue;
				surfaceCells.push_back(cellIndex(across, along, up));
				model->right.push_back(cellRight(across));
				model->ahead.push_back(cellAhead(along));
				model->up.push_back(cellUp(up));
				model->part.push_back(owner[cellIndex(across, along, up)]);
			}
	const size_t count = surfaceCells.size();
	if (count == 0) return nullptr;

	// Colours: for each painting and direction, each part's frame paints the part's voxels it sees (the nearest at
	// each pixel and those within a voxel behind it).
	model->colours.assign(count * 16, 0);
	for (int painting = 0; painting < 2; ++painting)
	{
		const bool level = painting == 0;
		for (int d = 0; d < 8; ++d)
		{
			for (int p = 0; p < BodyParts; ++p)
			{
				if (!parts[p].any) continue;
				std::vector<float> front(FrameSize * FrameSize, -1e9f);
				std::vector<int> pixelOf(count, -1);
				std::vector<float> nearnessOf(count, 0.0f);
				for (size_t k = 0; k < count; ++k)
				{
					if (model->part[k] != PartCodes[p]) continue;
					float column, row, nearness;
					project(model->right[k], model->ahead[k], model->up[k], d, level, column, row, nearness);
					const int c = (int)std::floor(column), r = (int)std::floor(row);
					if (c < 0 || r < 0 || c >= FrameSize || r >= FrameSize) continue;
					pixelOf[k] = r * FrameSize + c;
					nearnessOf[k] = nearness;
					front[pixelOf[k]] = std::max(front[pixelOf[k]], nearness);
				}
				for (size_t k = 0; k < count; ++k)
				{
					if (pixelOf[k] < 0) continue;
					const Uint8 colour = parts[p].frame[d][pixelOf[k]];
					if (colour && nearnessOf[k] >= front[pixelOf[k]] - 1.0f) model->colours[k * 16 + painting * 8 + d] = colour;
				}
			}
		}
	}
	// Each surface voxel's neighbours on the grid (the 26 around it), for the fill and the smoothing.
	std::vector<int> voxelAt(GridCells, -1);
	for (size_t k = 0; k < count; ++k) voxelAt[surfaceCells[k]] = (int)k;
	auto neighbours = [&](size_t k, int found[26]) {
		const int cell = surfaceCells[k], up = cell % GridUp, along = (cell / GridUp) % GridAlong, across = cell / (GridUp * GridAlong);
		int many = 0;
		for (int da = -1; da <= 1; ++da)
			for (int db = -1; db <= 1; ++db)
				for (int dc = -1; dc <= 1; ++dc)
				{
					const int a = across + da, b = along + db, c = up + dc;
					if ((!da && !db && !dc) || a < 0 || b < 0 || c < 0 || a >= GridAcross || b >= GridAlong || c >= GridUp) continue;
					const int j = voxelAt[cellIndex(a, b, c)];
					if (j >= 0) found[many++] = j;
				}
		return many;
	};
	// In each painting, what a direction's frame doesn't see takes the nearest direction's colour that does, and a voxel
	// the painting doesn't see from anywhere, the painting's colours of the nearest voxel it does see: across the surface
	// (spreading out from those seen), or, for a bit apart from the rest, anywhere. (A voxel left without a colour would
	// show black: never, see the end.)
	for (int painting = 0; painting < 2; ++painting)
	{
		std::vector<size_t> seen;
		std::vector<int> from(count, -1);
		for (size_t k = 0; k < count; ++k)
		{
			bool any = false;
			for (int d = 0; d < 8; ++d)
			{
				Uint8 &colour = model->colours[k * 16 + painting * 8 + d];
				for (int step = 1; step <= 4 && !colour; ++step)
				{
					const Uint8 one = model->colours[k * 16 + painting * 8 + (d + step) % 8], other = model->colours[k * 16 + painting * 8 + (d + 8 - step) % 8];
					colour = one ? one : other;
				}
				any |= colour != 0;
			}
			if (any) { seen.push_back(k); from[k] = (int)k; }
		}
		std::vector<size_t> wave(seen);
		for (size_t next = 0; next < wave.size(); ++next)
		{
			int around[26];
			const int many = neighbours(wave[next], around);
			for (int n = 0; n < many; ++n)
			{
				if (from[around[n]] >= 0) continue;
				from[around[n]] = from[wave[next]];
				wave.push_back(around[n]);
			}
		}
		for (size_t k = 0; k < count && !seen.empty(); ++k)
		{
			if (model->colours[k * 16 + painting * 8]) continue;
			if (from[k] >= 0)
			{
				std::copy(model->colours.begin() + from[k] * 16 + painting * 8, model->colours.begin() + from[k] * 16 + painting * 8 + 8, model->colours.begin() + k * 16 + painting * 8);
				continue;
			}
			size_t nearest = seen.front();
			float best = 1e9f;
			for (size_t j : seen)
			{
				const float dr = model->right[j] - model->right[k], da = model->ahead[j] - model->ahead[k], du = model->up[j] - model->up[k];
				const float distance = dr * dr + da * da + du * du;
				if (distance < best) { best = distance; nearest = j; }
			}
			std::copy(model->colours.begin() + nearest * 16 + painting * 8, model->colours.begin() + nearest * 16 + painting * 8 + 8, model->colours.begin() + k * 16 + painting * 8);
		}
	}
	// A big unit takes the slanted painting from any height: its four tiles' rows lie up to 8 rows apart for the same
	// height (the level painting reads the frames as pictures taken at a unit's height: a face's, one tile deep).
	if (routine.big)
	{
		for (size_t k = 0; k < count; ++k) std::copy(model->colours.begin() + k * 16 + 8, model->colours.begin() + k * 16 + 16, model->colours.begin() + k * 16);
	}
	// (one painting with nothing seen at all takes the other's; and failing everything, the body's first colour)
	Uint8 anyColour = 0;
	for (Uint8 colour : model->colours) if (colour) { anyColour = colour; break; }
	for (size_t k = 0; k < count; ++k)
		for (int slot = 0; slot < 16; ++slot)
		{
			Uint8 &colour = model->colours[k * 16 + slot];
			if (!colour) colour = model->colours[k * 16 + (slot + 8) % 16];
			if (!colour) colour = anyColour ? anyColour : 1;
		}
	// Smoothed: each colour's shade the middle one of its own and its neighbours' of the same palette block (the
	// sprites' dithered shading, pushed apart by the contrast, speckled a body; edges between colours, such as the
	// eyes, stay). The owner chose this over the plain colours and over no contrast.
	{
		std::vector<Uint8> smoothed(model->colours);
		for (size_t k = 0; k < count; ++k)
		{
			int around[26], same[27];
			const int many = neighbours(k, around);
			int found = 0;
			same[found++] = (int)k;
			for (int n = 0; n < many; ++n) if (model->part[around[n]] == model->part[k]) same[found++] = around[n];
			for (int slot = 0; slot < 16; ++slot)
			{
				const Uint8 colour = model->colours[k * 16 + slot];
				int shades[27], shadeCount = 0;
				for (int n = 0; n < found; ++n)
				{
					const Uint8 other = model->colours[same[n] * 16 + slot];
					if (other / 16 == colour / 16) shades[shadeCount++] = other % 16;
				}
				if (shadeCount < 3) continue;
				std::nth_element(shades, shades + shadeCount / 2, shades + shadeCount);
				const Uint8 middleShade = (Uint8)(colour / 16 * 16 + shades[shadeCount / 2]);
				smoothed[k * 16 + slot] = middleShade ? middleShade : colour;
			}
		}
		model->colours.swap(smoothed);
	}
	// A walker drawn on one frame: its legs, for the walk, the body below the hip, but for its middle.
	if (routine.whole && routine.humanoid)
	{
		float top = 0.0f, side = 0.0f;
		for (size_t k = 0; k < count; ++k) top = std::max(top, model->up[k]);
		for (size_t k = 0; k < count; ++k) if (model->up[k] < top * WholeHip) side = std::max(side, std::abs(model->right[k]));
		for (size_t k = 0; k < count; ++k)
		{
			if (model->up[k] < top * WholeHip && std::abs(model->right[k]) > side * WholeMiddle) model->part[k] = VoxelUnitModel::Legs;
		}
	}
	// The items in hand, and the walk (an arm holding something held still, as the game's frames do: an item in both
	// hands holds both).
	if (placed[0]) addItem(*model, items[0], places[0]);
	if (placed[1]) addItem(*model, items[1], places[1]);
	const bool rightHolds = placed[0] || (placed[1] && items[1].twoHanded);
	const bool leftHolds = placed[1] || (placed[0] && items[0].twoHanded);
	makeWalk(*model, routine, leftHolds, rightHolds, walks);
	for (int toward = 0; toward < 8; ++toward)
	{
		const float sideways = (float)std::sin(toward * Pi / 4.0), forward = (float)std::cos(toward * Pi / 4.0);
		model->edge[toward] = -1e9f;
		for (size_t k = 0; k < model->part.size(); ++k)
		{
			if (model->part[k] != VoxelUnitModel::Item) model->edge[toward] = std::max(model->edge[toward], model->right[k] * sideways + model->ahead[k] * forward);
		}
	}
	fitFall(*model, deaths, corpse);
	return model;
}

/// FNV-1a: a hash of what a body is made from, so units drawn alike share one.
void hashBytes(Uint64 &hash, const Uint8 *bytes, size_t count)
{
	for (size_t i = 0; i < count; ++i) hash = (hash ^ bytes[i]) * 1099511628211ULL;
}

}

/// What a body is made from, for the builder's thread: the unit's parts' frames, the items in its hands, its pose and
/// drawing routine, the battle's palette; and the hash of all that, the battle it's for.
struct VoxelUnits::Job
{
	PartFrames parts[PartCount];
	HeldItem items[2];
	std::vector<std::vector<Uint8>> deaths;
	std::vector<Uint8> corpse;
	WalkFrames walks;
	bool aiming = false;
	const Routine *routine = nullptr;
	int unitId = 0, status = 0; // (the unit's, for dump's file names)
	bool kneeled = false;
	SDL_Color palette[256];
	Uint64 hash = 0;
	unsigned battle = 0;
};

/**
 * The pose a body's fall has come to, between its frames' poses: standing at 0, each frame's pose in turn, the last at 1.
 * @param progress How far the fall has come (0 to 1).
 * @return The pose.
 */
VoxelUnitModel::FallPose VoxelUnitModel::poseAt(double progress) const
{
	if (fall.empty() || progress <= 0.0) return FallPose();
	const double at = std::min(progress, 1.0) * fall.size();
	const int frame = std::min((int)std::ceil(at) - 1, (int)fall.size() - 1);
	const FallPose from = frame > 0 ? fall[frame - 1] : FallPose(), &to = fall[frame];
	const float weight = (float)std::clamp(at - frame, 0.0, 1.0);
	return { from.height + (to.height - from.height) * weight, from.spread + (to.spread - from.spread) * weight, from.angle + (to.angle - from.angle) * weight };
}

/**
 * Puts a body point in a pose of its fall: crumpled (its height, its breadth), then tipped over about the body's edge on
 * the side it falls to.
 * @param pose The pose.
 * @param across In and out: voxels to the unit's right of the middle of its feet.
 * @param along In and out: ahead.
 * @param height In and out: up.
 */
void VoxelUnitModel::posed(const FallPose &pose, float &across, float &along, float &height) const
{
	across *= pose.spread;
	along *= pose.spread;
	height *= pose.height;
	if (pose.angle <= 0.0f) return;
	const float sideways = (float)std::sin(fallToward * Pi / 4.0), forward = (float)std::cos(fallToward * Pi / 4.0);
	const float toward = across * sideways + along * forward, aside = -across * forward + along * sideways;
	const float edgeHere = edge[fallToward] * pose.spread, fromEdge = toward - edgeHere;
	const float c = (float)std::cos(pose.angle * Pi / 180.0), s = (float)std::sin(pose.angle * Pi / 180.0);
	const float tipped = edgeHere + fromEdge * c + height * s;
	height = -fromEdge * s + height * c;
	across = tipped * sideways - aside * forward;
	along = tipped * forward + aside * sideways;
}

/**
 * The fall's frame whose colours paint the body at this point of its fall (the frame it's nearest), or none before the
 * first half of the first.
 * @param progress How far the fall has come (0 to 1).
 * @return The frame, or -1 (the standing colours).
 */
int VoxelUnitModel::fallFrameAt(double progress) const
{
	if (fallColours.empty()) return -1;
	return std::min((int)std::lround(std::clamp(progress, 0.0, 1.0) * fall.size()) - 1, (int)fall.size() - 1);
}

/**
 * Whether a unit can be drawn as a voxel body: of a size and drawn by a routine the bodies know (see Routine), and up
 * (not down or falling).
 * @param unit The unit.
 * @return True if it can.
 */
bool VoxelUnits::suits(const BattleUnit *unit)
{
	const Armor *armor = unit->getArmor();
	const Routine *routine = routineFor(armor->getDrawingRoutine());
	return routine && armor->getSize() == (routine->big ? 2 : 1) && !unit->isOut() && unit->getStatus() != STATUS_COLLAPSING;
}

/**
 * The key a unit's body is kept under: the unit, its armour, kneeling, aiming, flying, what it holds, and a big unit's
 * turret's turn from its hull.
 * @param unit The unit.
 * @param standing As the unit would stand (the body the walk swings): not kneeling, aiming or flying.
 * @return The key.
 */
VoxelUnits::Key VoxelUnits::keyFor(const BattleUnit *unit, bool standing)
{
	auto held = [](const BattleItem *item) -> const RuleItem* {
		return item && (!item->getRules()->isFixed() || item->getRules()->getFixedShow()) ? item->getRules() : nullptr;
	};
	const int turret = unit->getArmor()->getSize() > 1 ? ((unit->getTurretDirection() - unit->getDirection()) % 8 + 8) % 8 : 0;
	return Key(unit->getId(), unit->getArmor(), !standing && unit->isKneeled(), !standing && unit->getStatus() == STATUS_AIMING,
		!standing && unit->isFloating(), held(unit->getRightHandWeapon()), held(unit->getLeftHandWeapon()), turret);
}

/**
 * The body for a unit as it is now, made from its sprites the first time it's seen so (a unit drawn just like one
 * seen before shares its body). A walking unit takes the body it stands in (and the walk's phase), if it was ever
 * seen standing; else there's none yet (it's drawn as its sprite meanwhile).
 * @param unit The unit.
 * @param unitSprite The 3D view's unit drawer (left turned to the unit's own direction when a body is made; the parts
 *        it doesn't give the body, such as fire, are drawn on its surface, to be cleared).
 * @param inventorySprites The game's BIGOBS.PCK (the items in hand).
 * @param floorSprites The game's FLOOROB.PCK (the corpse's picture, as the body lies once down).
 * @param save The battle (the items' recolouring scripts).
 * @param palette The battle's palette (which colours are near black).
 * @param builds How many bodies may still be made (counted down when one is; at 0 a unit without a body gets none).
 * @param walkPhase Out: the walk's phase to draw (0-7), or -1 when not walking.
 * @return The body, or null.
 */
const VoxelUnitModel *VoxelUnits::modelFor(BattleUnit *unit, UnitSprite &unitSprite, const SurfaceSet *inventorySprites, SurfaceSet *floorSprites,
	const SavedBattleGame *save, const SDL_Color *palette, int &builds, int &walkPhase)
{
	walkPhase = -1;
	collect();
	if (unit->getStatus() == STATUS_WALKING || unit->getStatus() == STATUS_FLYING)
	{
		auto found = _models.find(keyFor(unit, true));
		if (found == _models.end() || !found->second) return nullptr;
		walkPhase = ((unit->getWalkingPhase() % 8) + 8) % 8;
		return remember(unit, found->second);
	}
	const Key key = keyFor(unit, false);
	auto found = _models.find(key);
	if (found != _models.end()) return remember(unit, found->second);
	auto waiting = _waiting.find(key);
	if (waiting != _waiting.end())
	{
		auto made = _byFrames.find(waiting->second);
		if (made == _byFrames.end()) return nullptr; // (still being made)
		_models[key] = made->second;
		_waiting.erase(waiting);
		return remember(unit, made->second);
	}
	if (builds <= 0 || !palette) return nullptr;

	const Routine *routine = routineFor(unit->getArmor()->getDrawingRoutine());
	if (!routine) return nullptr;
	// Each part on a frame of its own, for each direction (the drawer turned to it), with the game's colours; the
	// drawer's other parts on a frame nobody looks at. A big unit's four tiles (and its turret, and what it floats on) on
	// the torso's frame, laid out as the 3D view lays out its sprite.
	PartFrames parts[PartCount];
	std::vector<std::unique_ptr<Surface>> frames;
	Surface discard(FrameSize, FrameSize);
	Surface *capture[BODYPART_LARGE_TURRET + 4];
	std::fill(std::begin(capture), std::end(capture), &discard);
	for (int p = 0; p < PartCount; ++p)
	{
		frames.push_back(std::make_unique<Surface>(FrameSize, FrameSize));
		capture[PartSlots[p]] = frames.back().get();
	}
	if (routine->big)
	{
		for (int slot = BODYPART_LARGE_TORSO; slot < BODYPART_LARGE_TURRET + 4; ++slot) capture[slot] = frames[0].get();
	}
	const GraphSubset mask(FrameSize, FrameSize);
	for (int d = 0; d < 8; ++d)
	{
		for (auto &frame : frames) frame->clear();
		unitSprite.setViewRotation(d - unit->getDirection());
		unitSprite.setPartCapture(capture);
		if (routine->big)
		{
			unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
			unitSprite.draw(unit, 1, 32, 16, 0, mask, false);
			unitSprite.draw(unit, 2, 0, 16, 0, mask, false);
			unitSprite.draw(unit, 3, 16, 24, 0, mask, false);
		}
		else
		{
			unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
		}
		unitSprite.setPartCapture(nullptr);
		for (int p = 0; p < PartCount; ++p)
		{
			std::vector<Uint8> &pixels = parts[p].frame[d];
			pixels.resize(FrameSize * FrameSize);
			for (int row = 0; row < FrameSize; ++row)
			{
				for (int column = 0; column < FrameSize; ++column)
				{
					pixels[row * FrameSize + column] = frames[p]->getPixel(column, row);
					parts[p].any |= pixels[row * FrameSize + column] != 0;
				}
			}
		}
	}
	// The death frames (the fall as the game draws it, facing the screen), for the body to fall as they show.
	std::vector<std::vector<Uint8>> deaths;
	if (!routine->big)
	{
		Surface *fallCapture[BODYPART_LARGE_TURRET + 4];
		std::fill(std::begin(fallCapture), std::end(fallCapture), &discard);
		fallCapture[BODYPART_COLLAPSING] = frames[0].get();
		unitSprite.setViewRotation(Front - unit->getDirection());
		for (int frame = 0; frame < unit->getArmor()->getDeathFrames() && frame < 32; ++frame)
		{
			frames[0]->clear();
			unitSprite.setFallOverride(frame);
			unitSprite.setPartCapture(fallCapture);
			unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
			unitSprite.setPartCapture(nullptr);
			unitSprite.setFallOverride(-1);
			std::vector<Uint8> pixels(FrameSize * FrameSize);
			bool any = false;
			for (int row = 0; row < FrameSize; ++row)
				for (int column = 0; column < FrameSize; ++column) any |= (pixels[row * FrameSize + column] = frames[0]->getPixel(column, row)) != 0;
			if (!any) break;
			deaths.push_back(std::move(pixels));
		}
	}
	// The walk's frames, each phase from the right side and the front right (see WalkFrames), for the walk to follow them:
	// its legs' (all of it, for a body drawn on one frame) and its arms'.
	WalkFrames walks;
	if (routine->legsSwing || routine->armsSwing)
	{
		auto pixelsOf = [](Surface *surface) {
			std::vector<Uint8> pixels(FrameSize * FrameSize);
			for (int row = 0; row < FrameSize; ++row)
				for (int column = 0; column < FrameSize; ++column) pixels[row * FrameSize + column] = surface->getPixel(column, row);
			return pixels;
		};
		for (int phase = 0; phase < 8; ++phase)
		{
			for (int view = 0; view < 2; ++view)
			{
				for (auto &frame : frames) frame->clear();
				unitSprite.setViewRotation(WalkViews[view] - unit->getDirection());
				unitSprite.setWalkOverride(phase);
				unitSprite.setPartCapture(capture);
				if (routine->big)
				{
					unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
					unitSprite.draw(unit, 1, 32, 16, 0, mask, false);
					unitSprite.draw(unit, 2, 0, 16, 0, mask, false);
					unitSprite.draw(unit, 3, 16, 24, 0, mask, false);
				}
				else
				{
					unitSprite.draw(unit, 0, 16, 8, 0, mask, false);
				}
				unitSprite.setPartCapture(nullptr);
				unitSprite.setWalkOverride(-1);
				walks.legs[phase][view] = pixelsOf(frames[routine->whole ? 0 : 3].get());
				if (!routine->whole)
				{
					walks.arms[phase][view][0] = pixelsOf(frames[2].get());
					walks.arms[phase][view][1] = pixelsOf(frames[1].get());
				}
			}
		}
		walks.any = true;
	}
	unitSprite.setViewRotation(0);
	// And its corpse's picture on the floor (FLOOROB.PCK, as the map draws a corpse), for the body to end lying as it shows.
	std::vector<Uint8> corpse;
	const auto &corpseItems = unit->getArmor()->getCorpseBattlescape();
	if (!routine->big && floorSprites && !corpseItems.empty() && corpseItems.front())
	{
		Surface *picture = floorSprites->getFrame(corpseItems.front()->getFloorSprite());
		if (picture)
		{
			frames[0]->clear();
			picture->blitNShade(frames[0].get(), 16, 8, 0);
			corpse.resize(FrameSize * FrameSize);
			bool any = false;
			for (int row = 0; row < FrameSize; ++row)
				for (int column = 0; column < FrameSize; ++column) any |= (corpse[row * FrameSize + column] = frames[0]->getPixel(column, row)) != 0;
			if (!any) corpse.clear();
		}
	}

	// The items in hand: their inventory pictures as the game draws them in a hand (with their own recolouring).
	HeldItem items[2];
	const BattleItem *hands[2] = { unit->getRightHandWeapon(), unit->getLeftHandWeapon() };
	for (int hand = 0; hand < 2; ++hand)
	{
		const BattleItem *item = hands[hand];
		if (!item || (item->getRules()->isFixed() && !item->getRules()->getFixedShow())) continue;
		items[hand].twoHanded = item->getRules()->isTwoHanded();
		const Surface *frame = inventorySprites ? item->getBigSprite(inventorySprites, save, 0) : nullptr;
		if (!frame) continue;
		Surface picture(frame->getWidth(), frame->getHeight());
		ScriptWorkerBlit worker;
		BattleItem::ScriptFill(&worker, item, save, BODYPART_ITEM_INVENTORY, 0, 0);
		worker.executeBlit(frame, &picture, 0, 0, 0);
		items[hand].width = picture.getWidth();
		items[hand].height = picture.getHeight();
		items[hand].picture.resize(items[hand].width * items[hand].height);
		for (int y = 0; y < items[hand].height; ++y)
			for (int x = 0; x < items[hand].width; ++x) items[hand].picture[y * items[hand].width + x] = picture.getPixel(x, y);
	}

	// A unit drawn just like one before (the same frames, pictures and pose) shares its body.
	const bool aiming = unit->getStatus() == STATUS_AIMING;
	Uint64 hash = 14695981039346656037ULL;
	for (const PartFrames &part : parts)
		for (const auto &frame : part.frame) hashBytes(hash, frame.data(), frame.size());
	for (const HeldItem &held : items)
	{
		hashBytes(hash, held.picture.data(), held.picture.size());
		const Uint8 flags[3] = { (Uint8)held.width, (Uint8)held.height, (Uint8)held.twoHanded };
		hashBytes(hash, flags, 3);
	}
	for (const auto &death : deaths) hashBytes(hash, death.data(), death.size());
	hashBytes(hash, corpse.data(), corpse.size());
	for (int phase = 0; phase < 8 && walks.any; ++phase)
		for (int view = 0; view < 2; ++view)
		{
			hashBytes(hash, walks.legs[phase][view].data(), walks.legs[phase][view].size());
			for (int side = 0; side < 2; ++side) hashBytes(hash, walks.arms[phase][view][side].data(), walks.arms[phase][view][side].size());
		}
	const Uint8 pose[2] = { (Uint8)aiming, (Uint8)routine->number };
	hashBytes(hash, pose, 2);
	auto alike = _byFrames.find(hash);
	if (alike != _byFrames.end())
	{
		_models[key] = alike->second;
		return remember(unit, alike->second);
	}
	// Else the builder makes it (unless it's making one alike already), and the unit is drawn as its sprite meanwhile.
	--builds;
	_waiting[key] = hash;
	if (_making.insert(hash).second)
	{
		auto job = std::make_unique<Job>();
		std::move(std::begin(parts), std::end(parts), std::begin(job->parts));
		std::move(std::begin(items), std::end(items), std::begin(job->items));
		job->deaths = std::move(deaths);
		job->corpse = std::move(corpse);
		job->walks = std::move(walks);
		job->aiming = aiming;
		job->routine = routine;
		std::copy(palette, palette + 256, job->palette);
		job->hash = hash;
		job->unitId = unit->getId();
		job->status = unit->getStatus();
		job->kneeled = unit->isKneeled();
		if (!_builder.joinable()) _builder = std::thread(&VoxelUnits::build, this);
		std::lock_guard<std::mutex> lock(_mutex);
		job->battle = _battle;
		_jobs.push_back(std::move(job));
		_wake.notify_one();
	}
	return nullptr;
}

/**
 * Notes a unit's body as the one it was last drawn with (a null body, of frames that can't make one, isn't).
 * @param unit The unit.
 * @param body Its body.
 * @return The body.
 */
const VoxelUnitModel *VoxelUnits::remember(const BattleUnit *unit, const std::shared_ptr<VoxelUnitModel> &body)
{
	if (body) _lastBodies[unit->getId()] = body;
	return body.get();
}

/**
 * The body a unit was last drawn with: what falls when it goes down, and lies where it fell.
 * @param unit The unit.
 * @return The body, or null if it was never drawn as one.
 */
const VoxelUnitModel *VoxelUnits::lastBody(const BattleUnit *unit) const
{
	auto found = _lastBodies.find(unit->getId());
	return found != _lastBodies.end() ? found->second.get() : nullptr;
}

/**
 * Nothing made yet: the builder's thread starts with the first body to make (with the setting "Voxel units" off, none).
 */
VoxelUnits::VoxelUnits()
{
}

/**
 * Stops the builder's thread, if it was started (after the body it's making, if any).
 */
VoxelUnits::~VoxelUnits()
{
	if (!_builder.joinable()) return;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_stopping = true;
	}
	_wake.notify_all();
	_builder.join();
}

/**
 * Forgets every body, and those being made (a new battle: what the builder makes of the old one's is dropped).
 */
void VoxelUnits::clear()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_jobs.clear();
		_made.clear();
		++_battle;
	}
	_models.clear();
	_byFrames.clear();
	_making.clear();
	_waiting.clear();
	_lastBodies.clear();
}

/**
 * The builder's loop, on its own thread: makes the bodies queued, one at a time, and hands them over (see collect),
 * until stopped. (The grid a body is made on is the builder's alone: see useGrid.)
 */
void VoxelUnits::build()
{
	for (;;)
	{
		std::unique_ptr<Job> job;
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_wake.wait(lock, [this] { return _stopping || !_jobs.empty(); });
			if (_stopping) return;
			job = std::move(_jobs.front());
			_jobs.pop_front();
		}
		std::shared_ptr<VoxelUnitModel> model(makeModel(job->parts, job->items, job->deaths, job->corpse, job->walks, job->aiming, *job->routine, job->palette));
		if (const char *folder = std::getenv("OXC3D_VOXELDUMP")) dump(*job, model.get(), folder);
		std::lock_guard<std::mutex> lock(_mutex);
		if (job->battle == _battle) _made.push_back({ job->hash, model });
	}
}

/**
 * A developer's aid (the environment variable OXC3D_VOXELDUMP names a folder): writes what a body was made from and
 * what it is, as unit<id>_<status>[k] (k: kneeling) and these endings: .frames (the parts' frames, 6 parts x 8
 * directions x 64x64, then the two items' pictures: width, height, pixels), .deaths (the death frames and the corpse's
 * picture), .walkframes (the walk's frames: phase x view x legs, right arm, left arm), .points (each voxel: right ahead
 * up part and its 16 colours), .walk (each phase's places) and .fall (the fall's way, poses, edges and colours). The
 * viewers in tools/voxel_units read them.
 * @param job What the body was made from.
 * @param model The body (or null: none could be made).
 * @param folder Where to write.
 */
void VoxelUnits::dump(const Job &job, const VoxelUnitModel *model, const char *folder)
{
	const std::string base = std::string(folder) + "/unit" + std::to_string(job.unitId) + "_" + std::to_string(job.status) + (job.kneeled ? "k" : "");
	if (FILE *file = std::fopen((base + ".frames").c_str(), "wb"))
	{
		for (const PartFrames &part : job.parts)
			for (const auto &frame : part.frame) std::fwrite(frame.data(), 1, frame.size(), file);
		for (const HeldItem &held : job.items)
		{
			const int size[2] = { held.width, held.height };
			std::fwrite(size, sizeof(int), 2, file);
			if (!held.picture.empty()) std::fwrite(held.picture.data(), 1, held.picture.size(), file);
		}
		std::fclose(file);
	}
	if (FILE *file = std::fopen((base + ".deaths").c_str(), "wb"))
	{
		for (const auto &death : job.deaths) std::fwrite(death.data(), 1, death.size(), file);
		if (!job.corpse.empty()) std::fwrite(job.corpse.data(), 1, job.corpse.size(), file);
		std::fclose(file);
	}
	if (job.walks.any)
	{
		if (FILE *file = std::fopen((base + ".walkframes").c_str(), "wb"))
		{
			const std::vector<Uint8> none(FrameSize * FrameSize, 0);
			for (int phase = 0; phase < 8; ++phase)
				for (int view = 0; view < 2; ++view)
				{
					const std::vector<Uint8> *frames[3] = { &job.walks.legs[phase][view], &job.walks.arms[phase][view][0], &job.walks.arms[phase][view][1] };
					for (const auto *frame : frames) std::fwrite(frame->empty() ? none.data() : frame->data(), 1, FrameSize * FrameSize, file);
				}
			std::fclose(file);
		}
	}
	if (!model) return;
	const size_t count = model->part.size();
	if (FILE *file = std::fopen((base + ".points").c_str(), "w"))
	{
		for (size_t k = 0; k < count; ++k)
		{
			std::fprintf(file, "%.2f %.2f %.2f %d", model->right[k], model->ahead[k], model->up[k], model->part[k]);
			for (int slot = 0; slot < 16; ++slot) std::fprintf(file, " %d", model->colours[k * 16 + slot]);
			std::fprintf(file, "\n");
		}
		std::fclose(file);
	}
	if (FILE *file = std::fopen((base + ".walk").c_str(), "w"))
	{
		for (int phase = 0; phase < 8; ++phase)
			for (size_t k = 0; k < count; ++k) std::fprintf(file, "%d %.2f %.2f %.2f\n", phase, model->walkRight[phase][k], model->walkAhead[phase][k], model->walkUp[phase][k]);
		std::fclose(file);
	}
	if (FILE *file = std::fopen((base + ".fall").c_str(), "w"))
	{
		std::fprintf(file, "%d %d\n", model->fallToward, (int)model->fall.size());
		for (const auto &pose : model->fall) std::fprintf(file, "%.3f %.3f %.1f\n", pose.height, pose.spread, pose.angle);
		for (float edge : model->edge) std::fprintf(file, "%.2f ", edge);
		std::fprintf(file, "\n");
		for (size_t pose = 0; pose < model->fall.size(); ++pose)
		{
			for (size_t k = 0; k < count; ++k) std::fprintf(file, "%d ", model->fallColours.empty() ? 0 : model->fallColours[pose * count + k]);
			std::fprintf(file, "\n");
		}
		std::fclose(file);
	}
}

/**
 * Takes the bodies the builder has made since last time (none made, for frames that can't make one: kept as such, so
 * the unit stays its sprite).
 */
void VoxelUnits::collect()
{
	std::vector<std::pair<Uint64, std::shared_ptr<VoxelUnitModel>>> made;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		made.swap(_made);
	}
	for (auto &body : made)
	{
		_making.erase(body.first);
		_byFrames[body.first] = std::move(body.second);
	}
}

}
// Thanks mom and dad! You were the ones who exposed me to my first comp.
