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
 * FirstPersonVoxelBodies.cpp: how a body is made (see FirstPersonVoxelBodies.h and FirstPersonVoxelUnits.h).
 *
 * The parts' frames come from the game's own unit drawer (UnitSprite), each part on a frame of its
 * own (setPartCapture) for each of the 8 directions, with the unit's colours as the game draws them;
 * its walk's and death's frames likewise, the drawer told which to draw (setWalkOverride,
 * setFallOverride: the unit itself left as it is).
 * The isometric view draws a unit facing direction d with the projection column = 32 + (x - y),
 * row = 40 + (x + y) / 2 - up (in voxels from the middle of its tile's floor, on these 64x64
 * frames), so each frame is the same body seen from 45 degrees further round.
 */
#include "FirstPersonVoxelBodies.h"
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace OpenXcom
{

using namespace VoxelBodies;

namespace
{

// ---- The grid a body is made on: half a voxel a cell, 42 voxels up from 2 below the feet, and across and along 28 for
// a unit one tile big, 40 for one two tiles by two (set by useGrid for the unit being made, each of the
// builder's threads its own: see VoxelUnits::build) ----
constexpr float Cell = 0.5f;
constexpr int GridUp = 84 * VOXELSCALE;
constexpr float UpFrom = -2.0f * VOXELSCALE;
// (each begun with a constant: a thread's own that begins with another's value is looked up through a call each time)
thread_local int GridAcross = 56 * VOXELSCALE, GridAlong = 56 * VOXELSCALE, GridCells = 56 * VOXELSCALE * 56 * VOXELSCALE * GridUp;
thread_local float AcrossFrom = -14.0f * VOXELSCALE, AlongFrom = -14.0f * VOXELSCALE;
inline int cellIndex(int across, int along, int up) { return (across * GridAlong + along) * GridUp + up; }
inline float cellRight(int across) { return AcrossFrom + (across + 0.5f) * Cell; }
inline float cellAhead(int along) { return AlongFrom + (along + 0.5f) * Cell; }
inline float cellUp(int up) { return UpFrom + (up + 0.5f) * Cell; }

// ---- The frames ----
/// The parts are drawn on FrameSize frames with the unit at (16, 8), as the 3D view draws units: the
/// middle of its tile's floor at (32, 40); a big unit's four tiles around their shared corner, at (32, 48).
constexpr int FloorX = 32 * VOXELSCALE;
thread_local int FloorY = 40 * VOXELSCALE;
thread_local bool BigUnit = false; // (two tiles by two: see tileFloorRows)
thread_local bool SlantOnly = false; // (a big walker: read along the slant from any height, see project)

/// The grid and the floor's place on the frames for a unit of this size (in tiles across).
void useGrid(int size)
{
	const int half = (size > 1 ? 20 : 14) * VOXELSCALE;
	GridAcross = GridAlong = (int)(2 * half / Cell);
	AcrossFrom = AlongFrom = (float)-half;
	GridCells = GridAcross * GridAlong * GridUp;
	FloorY = (size > 1 ? 48 : 40) * VOXELSCALE;
	BigUnit = size > 1;
}
/// Sprite pixels a voxel across a frame (the projection's x - y along the unit's own axes).
constexpr float PixelsAcross = 1.41421356f;
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
constexpr int HeadWidthMin = 9 * VOXELSCALE;
constexpr float NarrowTorso = 0.65f, ChinDepth = 2.5f * VOXELSCALE, NeckRadius = 1.3f * VOXELSCALE;
constexpr int ChinRows = 3 * VOXELSCALE;
/// The walk: the thighs swing this many degrees either way, the knee bends this much while its leg swings forward,
/// the arms swing against the legs; the torso bobs as the game's frames do (see Routine). A walker drawn on one frame
/// has its legs below WholeHip of its height, each to its side of the middle (what's within WholeMiddle of the widest
/// part of them from the middle stays with the body: between the legs).
constexpr double LegSwing = 22.0, KneeBend = 38.0, ArmSwing = 16.0, BeastSwing = 26.0;
constexpr float WholeHip = 0.42f, WholeMiddle = 0.12f;
/// A body lying (see makeFalls) is at most this many voxels long, and this much thicker for each pixel inside its
/// corpse's picture's outline (in the picture's own measure).
constexpr float LyingLongest = 17.0f, LyingRound = 1.1f;
/// A death frame's shape lies flat once its picture is this low, of the standing height (leaning from FallUpright of it
/// on; see makeFalls).
constexpr float FallUpright = 0.85f, FallFlat = 0.4f;
/// The last death frame's shape leans back at least this many degrees, those before it in even steps; a low picture's
/// at most FallMostLean (flat, it would lie half as long again as it is drawn).
constexpr float FallLastLean = 50.0f, FallMostLean = 65.0f;
/// A big unit's wreck (see makeFalls) is never pressed lower than this much of its body's height.
constexpr float WreckLowest = 0.35f;
/// A piece added for a view (see matchViews) keeps to the body's own surface: a depth's agreement with the other views
/// counts BodyPull less for each voxel it lies from it (up to FarSteps half voxels: farther counts as that far).
constexpr float BodyPull = 1.0f;
constexpr Uint8 FarSteps = 24;

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
constexpr float ArmRadiusMin = 0.6f * VOXELSCALE, ArmRadiusMax = 1.5f * VOXELSCALE, ArmBend = 1.04f, ArmFromTorso = 1.25f, ArmNoLegs = 0.5f, ArmFarthest = 4.0f * VOXELSCALE;
constexpr float GripButt = 0.3f, GripFore = 0.68f, GripOne = 0.2f, GripBelow = 0.15f;
constexpr double Pi = 3.14159265358979323846;

/// The directions' turns: the unit's right and ahead as map x, y.
struct Turn { float cosine, sine; };
const Turn Turns[8] = { { 1, 0 }, { 0.70710678f, 0.70710678f }, { 0, 1 }, { -0.70710678f, 0.70710678f }, { -1, 0 },
	{ -0.70710678f, -0.70710678f }, { 0, -1 }, { 0.70710678f, -0.70710678f } };

/// Where the view of direction d puts a body point on its frame: column, row and nearness to the camera (larger
/// nearer). Level: as a picture taken at the unit's height (the row by height alone, a big unit's by height on its
/// tile's own floor; nearness along the ground); else along the isometric view's slant. A big unit on legs (the
/// Sectopod, the Reaper, the Triscene) is read along the slant from any height: its legs, drawn across the tiles' seams,
/// come apart read level by tile.
/// A big unit's frame is its four tiles' pictures, each standing on its own tile's floor (see VoxelUnits::modelFor): the
/// rows below the frame's floor row that the floor of the tile a place (x, y on the map, turned to the view) is on lies:
/// -8 the tile behind, 8 the one in front, 0 those beside; 0 for a unit one tile big.
inline float tileFloorRows(float x, float y)
{
	return BigUnit ? ((x < 0.0f ? -8.0f : 8.0f) + (y < 0.0f ? -8.0f : 8.0f)) * 0.5f * VOXELSCALE : 0.0f;
}

inline void project(float right, float ahead, float up, int d, bool level, float &column, float &row, float &nearness)
{
	const float x = right * Turns[d].cosine + ahead * Turns[d].sine, y = right * Turns[d].sine - ahead * Turns[d].cosine;
	column = FloorX + (x - y);
	if (level && !SlantOnly)
	{
		row = FloorY + tileFloorRows(x, y) - up;
		nearness = x + y;
	}
	else
	{
		row = FloorY + (x + y) * 0.5f - up;
		nearness = x + y + up;
	}
}

/// How deep behind a part's nearest voxel, along a direction's view, a voxel still takes that direction's picture (see the
/// painting in makeModel), voxels.
const float FaceDepth = 2.0f * VOXELSCALE;

/// A limb's slice among several in its row is at most this many times as deep as it is wide (see limb).
const float SliceDepthMost = 1.5f;
/// A row of the front drawing below the floor's row lies on the ground ahead (see fillSlice): how far ahead a row down
/// is, and how thick what lies there is made (body units).
const float GroundAheadARow = 1.41421356f, GroundPlate = 1.0f * VOXELSCALE;
/// A long body's side drawing (see profile): how many rows below the floor's row count as lying on the floor, and the
/// least it is across, each way (body units: a cube and a half through).
const int ProfileBelow = 8 * VOXELSCALE;
const float ProfileHalfLeast = 0.75f;
/// A long body's part this thick or more (half of it, body units: four voxels through) is a thick one: see profile.
const float ProfileThick = 2.0f * VOXELSCALE;
/// A long body's round part lying along it: how far across it is, of half its drawn height (see profile).
const float ProfileLyingRound = 0.8165f;
/// An animal's face (see makeModel's colours): how many cells behind the foremost one on its line a cell still has
/// the front drawing's colour (the same at the back).
const int AnimalFaceDepth = 2 * VOXELSCALE;
/// A limb's shape is carved by its drawings' outlines grown by so many pixels (see carve): a limb a pixel or two thin
/// (a skeleton's shin, a spider's leg) is drawn a pixel here or there from frame to frame, and cut by the outlines as
/// drawn it was cut away: the body ended in stumps above the floor (GitHub issue #211 again). Half a sprite's pixel:
/// none on the coarse build.
const int LimbCarveReach = VOXELSCALE / 2;

/// A hull's drawings are made from 30 degrees above the ground (the isometric view): the camera's way, along the ground
/// and up; a voxel within HullSkinDepth of the nearest on its pixel is on the skin a drawing sees (see makeModel).
constexpr float HullCameraAlong = 0.8660254f, HullCameraUp = 0.5f, HullSkinDepth = 1.5f * VOXELSCALE;
/// One body, the same from every side (see makeModel); false: matched to each view's drawing (see matchViews).
constexpr bool OneBody = true;
/// Cells this near each other (in cells, each way) are one piece of a body (see makeModel, dropFloating).
const int Joined = 2 * VOXELSCALE;

/// How far inside a slice's edge its cells' middles stay (see fillSlice), voxels.
const float SliceInset = 0.25f;

/// The most runs a frame's row may have, front and side, for the limbs' slices to be paired run by run (see limb).
const int MaxPairedRuns = 4;

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
std::vector<Uint8> carve(const PartFrames &part, int tolerance, const std::vector<Uint8> &only, int reach = 0)
{
	std::vector<Uint8> kept(GridCells, 0);
	// (With a reach: the outlines grown by so many pixels, see LimbCarveReach.)
	std::vector<Uint8> about[8];
	for (int d = 0; d < 8 && reach > 0; ++d)
	{
		about[d].assign(FrameSize * FrameSize, 0);
		for (int row = 0; row < FrameSize; ++row)
			for (int column = 0; column < FrameSize; ++column)
			{
				if (!part.frame[d][row * FrameSize + column]) continue;
				for (int r = std::max(0, row - reach); r <= std::min(FrameSize - 1, row + reach); ++r)
					for (int c = std::max(0, column - reach); c <= std::min(FrameSize - 1, column + reach); ++c) about[d][r * FrameSize + c] = 1;
			}
	}
	auto inside = [&](int d, int column, int row) -> bool
	{
		if (!reach) return part.at(d, column, row) != 0;
		return column >= 0 && row >= 0 && column < FrameSize && row < FrameSize && about[d][row * FrameSize + column];
	};
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
					if (!inside(d, (int)std::floor(column[d]), (int)std::floor(rowFloor[d] - cellUp(up)))) ++outside;
				}
				kept[cellIndex(across, along, up)] = outside <= tolerance;
			}
		}
	}
	return kept;
}

/// Whether a part's drawings are of a thin thing: under ThinInner of their pixels have a drawn pixel on all four sides
/// (a sprite's pixel away). The X-Com Files' cobra 0.34, its Chupacabra and bat 0.37; its dog 0.55, XPiratez's demons
/// 0.66 and more.
const float ThinInner = 0.42f;
bool thinDrawing(const PartFrames &part)
{
	int drawn = 0, inner = 0;
	const int step = VOXELSCALE;
	for (int d = 0; d < 8; ++d)
		for (int row = 0; row < FrameSize; ++row)
			for (int column = 0; column < FrameSize; ++column)
			{
				if (!part.frame[d][row * FrameSize + column]) continue;
				++drawn;
				inner += part.at(d, column - step, row) && part.at(d, column + step, row) && part.at(d, column, row - step) && part.at(d, column, row + step);
			}
	return drawn > 0 && inner < drawn * ThinInner;
}

/// A body's pieces are joined: no limb stands a hair off the body it belongs to (the owner: "No thin slits should be
/// present ever!", and of a snake, "Cobra missing body - disjoined body").
/// Each part of a body is made on its own and stays a quarter of a voxel inside its drawn edge (see SliceInset), so
/// an arm drawn against a shoulder stood a cell off it, all round: a Skeleton's two arms and its toes, a soldier's
/// arm raised to aim, hung in the air beside the body. And a long body is its side drawing given width (see profile),
/// in pieces where that has a pixel's gap in a thin tail or a leg's bar stands off the rounded trunk above it.
/// So, of the pieces (cells touching by a face, an edge or a corner): one under SpeckCells is a speck and goes;
/// wherever two stand SlitMost cells apart or less, the cells between are filled, all along; a piece still apart
/// gets a bridge three cells thick to the nearest cell of the rest, if that is JoinMost or nearer. One further off
/// stays where it is drawn. Cells gained are of the part beside them.
const int SpeckCells = 8, SlitMost = 2, JoinMost = (int)(3.0f * VOXELSCALE / Cell);
void joinPieces(std::vector<Uint8> &owner)
{
	std::vector<int> piece(GridCells, -1);
	std::vector<std::vector<int>> pieces;
	for (int start = 0; start < GridCells; ++start)
	{
		if (!owner[start] || piece[start] >= 0) continue;
		const int label = (int)pieces.size();
		pieces.emplace_back(1, start);
		piece[start] = label;
		for (size_t i = 0; i < pieces[label].size(); ++i)
		{
			const int cell = pieces[label][i], up = cell % GridUp, along = (cell / GridUp) % GridAlong, across = cell / (GridUp * GridAlong);
			for (int a = std::max(0, across - 1); a <= std::min(GridAcross - 1, across + 1); ++a)
				for (int b = std::max(0, along - 1); b <= std::min(GridAlong - 1, along + 1); ++b)
					for (int c = std::max(0, up - 1); c <= std::min(GridUp - 1, up + 1); ++c)
					{
						const int other = cellIndex(a, b, c);
						if (owner[other] && piece[other] < 0) { piece[other] = label; pieces[label].push_back(other); }
					}
		}
	}
	if (pieces.size() < 2) return;
	const int biggest = (int)(std::max_element(pieces.begin(), pieces.end(), [](const std::vector<int> &one, const std::vector<int> &other) { return one.size() < other.size(); }) - pieces.begin());
	auto place = [](int cell, int at[3]) { at[2] = cell % GridUp; at[1] = (cell / GridUp) % GridAlong; at[0] = cell / (GridUp * GridAlong); };
	// the specks go
	for (int label = 0; label < (int)pieces.size(); ++label)
	{
		if (label == biggest || (int)pieces[label].size() >= SpeckCells) continue;
		for (const int cell : pieces[label]) { owner[cell] = 0; piece[cell] = -1; }
		pieces[label].clear();
	}
	// (which pieces are one by now: each one's first of them)
	std::vector<int> group(pieces.size());
	for (size_t i = 0; i < group.size(); ++i) group[i] = (int)i;
	auto groupOf = [&](int label) { while (group[label] != label) label = group[label] = group[group[label]]; return label; };
	// the cells between two pieces a slit apart
	std::vector<std::pair<int, Uint8>> gained;
	for (int label = 0; label < (int)pieces.size(); ++label)
	{
		if (label == biggest) continue;
		for (const int cell : pieces[label])
		{
			int a[3];
			place(cell, a);
			for (int da = -SlitMost - 1; da <= SlitMost + 1; ++da)
				for (int db = -SlitMost - 1; db <= SlitMost + 1; ++db)
					for (int dc = -SlitMost - 1; dc <= SlitMost + 1; ++dc)
					{
						const int across = a[0] + da, along = a[1] + db, up = a[2] + dc;
						if (across < 0 || along < 0 || up < 0 || across >= GridAcross || along >= GridAlong || up >= GridUp) continue;
						const int other = piece[cellIndex(across, along, up)];
						if (other < 0 || other == label) continue;
						group[groupOf(other)] = groupOf(label);
						const int steps = std::max({ std::abs(da), std::abs(db), std::abs(dc) });
						for (int step = 1; step < steps; ++step)
						{
							const int between = cellIndex(a[0] + (int)std::lround(da * (float)step / steps), a[1] + (int)std::lround(db * (float)step / steps),
								a[2] + (int)std::lround(dc * (float)step / steps));
							if (!owner[between]) gained.push_back({ between, owner[cell] });
						}
					}
		}
	}
	for (const std::pair<int, Uint8> &cell : gained) if (!owner[cell.first] && cellUp(cell.first % GridUp) >= 0.0f) owner[cell.first] = cell.second;
	// a bridge for each piece still apart, the bigger first
	std::vector<int> order;
	for (int label = 0; label < (int)pieces.size(); ++label) if (!pieces[label].empty() && groupOf(label) != groupOf(biggest)) order.push_back(label);
	std::sort(order.begin(), order.end(), [&](int one, int other) { return pieces[one].size() > pieces[other].size(); });
	for (const int label : order)
	{
		if (groupOf(label) == groupOf(biggest)) continue;
		int best = JoinMost * JoinMost + 1, from = -1, to = -1;
		for (const int cell : pieces[label])
		{
			int a[3];
			place(cell, a);
			for (int other = 0; other < (int)pieces.size(); ++other)
			{
				if (groupOf(other) != groupOf(biggest)) continue;
				for (const int there : pieces[other])
				{
					int b[3];
					place(there, b);
					const int apart = (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
					if (apart < best) { best = apart; from = cell; to = there; }
				}
			}
		}
		if (from < 0) continue;
		int a[3], b[3];
		place(from, a);
		place(to, b);
		const int steps = std::max({ std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2]), 1 });
		for (int step = 0; step <= steps; ++step)
		{
			int at[3];
			for (int axis = 0; axis < 3; ++axis) at[axis] = a[axis] + (int)std::lround((b[axis] - a[axis]) * (float)step / steps);
			for (int da = -1; da <= 1; ++da)
				for (int db = -1; db <= 1; ++db)
					for (int dc = -1; dc <= 1; ++dc)
					{
						const int across = at[0] + da, along = at[1] + db, up = at[2] + dc;
						if (across < 0 || along < 0 || up < 0 || across >= GridAcross || along >= GridAlong || up >= GridUp || cellUp(up) < 0.0f) continue;
						if (!owner[cellIndex(across, along, up)]) owner[cellIndex(across, along, up)] = owner[from];
					}
		}
		group[groupOf(label)] = groupOf(biggest);
	}
}

/// A limb that ends early goes on down as its front drawing has it (a player, GitHub issue #211: XPiratez's "Imps
/// have 1 leg"; the owner: "Go and improve all these isues"). A body drawn on one frame in a stride has its legs one
/// before the other: the slice of a leg's row is stood where the other drawings show most of it (see limb), and where
/// they show the wrong leg there the carving cuts it off below the knee. So, row by row down the front drawing: a
/// stretch of drawn pixels that nothing of the body lands on, under a row the body does land on over half the stretch
/// or more, takes the body's cells behind each of that row's pixels, moved a row down: the thing goes on as it was,
/// wherever the front drawing has it. (Over half: what begins beside a thing is not carried on from the one pixel
/// next to it. A first version gave each stretch one slice as deep as all above it: a brim. And the same up the
/// drawing, for the tops of heads and for horns, built a tower on every head: a row up the front drawing is as
/// likely further back as higher.) Nothing is put under the floor.
/// @param part The frame's part (all of a body drawn on one frame).
/// @param code The part's code, for the cells it gains.
/// @param owner The body's cells so far: gained cells are added.
void continueLimbs(const PartFrames &part, Uint8 code, std::vector<Uint8> &owner)
{
	const int step = 1;
	const int pixels = FrameSize * FrameSize, rowCells = (int)std::lround(1.0f / Cell);
	std::vector<std::vector<int>> landing(pixels); // (the cells that land on each pixel of the front drawing)
	auto pixelOf = [&](int cell) -> int
	{
		const int up = cell % GridUp, along = (cell / GridUp) % GridAlong, across = cell / (GridUp * GridAlong);
		float column, row, nearness;
		project(cellRight(across), cellAhead(along), cellUp(up), Front, false, column, row, nearness);
		const int c = (int)std::floor(column), r = (int)std::floor(row);
		return c >= 0 && r >= 0 && c < FrameSize && r < FrameSize ? r * FrameSize + c : -1;
	};
	for (int cell = 0; cell < GridCells; ++cell)
	{
		if (!owner[cell]) continue;
		const int at = pixelOf(cell);
		if (at >= 0) landing[at].push_back(cell);
	}
	for (int row = step > 0 ? 1 : FrameSize - 2; row >= 0 && row < FrameSize; row += step)
	{
		for (int column = 0, start = -1; column <= FrameSize; ++column)
		{
			const bool lacking = column < FrameSize && part.frame[Front][row * FrameSize + column] && landing[row * FrameSize + column].empty();
			if (lacking && start < 0) start = column;
			if (lacking || start < 0) continue;
			const int first = start, last = column - 1;
			start = -1;
			int over = 0;
			for (int c = first; c <= last; ++c) over += !landing[(row - step) * FrameSize + c].empty();
			if (over * 2 < last - first + 1) continue;
			for (int c = first; c <= last; ++c)
			{
				const std::vector<int> from = landing[(row - step) * FrameSize + c]; // (a copy: the cells gained land on pixels too)
				for (const int cell : from)
				{
					// (a row down the drawing is a row's worth of cells lower; the half step between keeps it whole)
					for (int moved = 1; moved <= rowCells; ++moved)
					{
						const int up = cell % GridUp - moved * step;
						if (up < 0 || up >= GridUp || cellUp(up) < 0.0f) continue;
						const int gained = cell - cell % GridUp + up;
						if (owner[gained]) continue;
						owner[gained] = code;
						const int at = pixelOf(gained);
						if (at >= 0) landing[at].push_back(gained);
					}
				}
			}
		}
	}
}

/// A turret's shape (a big unit that can't move, mounted on a vehicle: see makeModel), made a plain block. Its 8 frames are all
/// drawn from the same height above it, so what they leave of it (see carve) carries a cone on its roof and one under
/// its belly, each as high as a third of its width: a flat turret came out a spinning top. Its slab is what's between
/// the lowest level its roof can be cut to and the highest its belly can, with every frame's drawn pixels still having
/// a cell behind them (but LevelLoss of a frame's pixels: hand drawn frames don't quite agree, and a rim's last pixels
/// don't hold a roof up). The body is then its outline from straight above, as thick as the slab, its edges cut at a
/// slant, with its barrels (what of the outline is thinner than BarrelHalf each way) lying on it: rough, but a hull
/// (the owner: "just a rhomboid shape with a cannon laying flat on surface ... a rough but ok looking object").
constexpr float LevelLoss = 0.02f, HullThin = 0.3f, HullBevel = 0.3f, BarrelHalf = 2.5f * VOXELSCALE;
void levelOff(const PartFrames &part, std::vector<Uint8> &cells)
{
	const int pixels = FrameSize * FrameSize;
	int cuts[2] = { GridUp - 1, 0 }; // (the slab's top and bottom levels)
	for (int pass = 0; pass < 2; ++pass) // (the roof, then the belly of what is under it)
	{
		// the lowest (for the belly: the highest) cell behind each pixel of each frame
		std::vector<int> ends[8];
		for (int d = 0; d < 8; ++d) ends[d].assign(pixels, -1);
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
				for (int up = 0; up <= cuts[0]; ++up)
				{
					if (!cells[cellIndex(across, along, up)]) continue;
					for (int d = 0; d < 8; ++d)
					{
						const int x = (int)std::floor(column[d]), y = (int)std::floor(rowFloor[d] - cellUp(up));
						if (x < 0 || y < 0 || x >= FrameSize || y >= FrameSize || !part.frame[d][y * FrameSize + x]) continue;
						int &end = ends[d][y * FrameSize + x];
						if (end < 0 || (pass == 0 ? up < end : up > end)) end = up;
					}
				}
			}
		}
		// the pixels that cutting each level away would bare, frame by frame, and the cut as far as they allow
		std::vector<int> bare[8];
		int allowed[8], lost[8] = {};
		for (int d = 0; d < 8; ++d)
		{
			bare[d].assign(GridUp, 0);
			int covered = 0;
			for (int end : ends[d]) if (end >= 0) { ++bare[d][end]; ++covered; }
			allowed[d] = pass == 0 ? (int)(covered * LevelLoss) : 0; // (the belly strictly: a hull stands on what is under it)
		}
		const int step = pass == 0 ? -1 : 1;
		for (int &cut = cuts[pass]; cut != cuts[1 - pass]; cut += step)
		{
			bool fits = true;
			for (int d = 0; d < 8 && fits; ++d) fits = lost[d] + bare[d][cut] <= allowed[d];
			if (!fits) break;
			for (int d = 0; d < 8; ++d) lost[d] += bare[d][cut];
		}
	}
	// The block: the outline seen from straight above (the columns filled through most of the slab), as thick as the slab
	// and no thinner than HullThin of the outline's width (a disc's slab is one cell: its drawings are a flat ellipse).
	const int plan = GridAcross * GridAlong;
	std::vector<Uint8> outline(plan, 0);
	int first[2] = { GridAcross, GridAlong }, last[2] = { -1, -1 };
	for (int across = 0; across < GridAcross; ++across)
	{
		for (int along = 0; along < GridAlong; ++along)
		{
			int filled = 0;
			for (int up = cuts[1]; up <= cuts[0]; ++up) filled += cells[cellIndex(across, along, up)] != 0;
			if (filled * 2 <= cuts[0] - cuts[1]) continue;
			outline[across * GridAlong + along] = 1;
			first[0] = std::min(first[0], across); last[0] = std::max(last[0], across);
			first[1] = std::min(first[1], along); last[1] = std::max(last[1], along);
		}
	}
	if (last[0] < 0) return;
	// how far inside the outline each column is, in cells (by steps to the four sides)
	auto inside = [&](const std::vector<Uint8> &shape) {
		std::vector<int> depth(plan, 0);
		for (int pass = 0; pass < 2; ++pass)
		{
			for (int n = 0; n < plan; ++n)
			{
				const int i = pass == 0 ? n : plan - 1 - n, across = i / GridAlong, along = i % GridAlong;
				if (!shape[i]) continue;
				const int back = pass == 0 ? -1 : 1;
				const int one = across + back >= 0 && across + back < GridAcross ? depth[i + back * GridAlong] : 0;
				const int other = along + back >= 0 && along + back < GridAlong ? depth[i + back] : 0;
				depth[i] = pass == 0 ? std::min(one, other) + 1 : std::min(depth[i], std::min(one, other) + 1);
			}
		}
		return depth;
	};
	// the body: the outline without what is thinner than a barrel (worn away BarrelHalf deep, grown back); the rest, its
	// barrels, lying on it as thick as they are wide
	const std::vector<int> depth = inside(outline);
	const int wear = (int)std::lround(BarrelHalf / Cell);
	std::vector<Uint8> body(plan, 0);
	for (int across = 0; across < GridAcross; ++across)
		for (int along = 0; along < GridAlong; ++along)
		{
			if (depth[across * GridAlong + along] <= wear) continue;
			for (int a = std::max(0, across - wear); a <= std::min(GridAcross - 1, across + wear); ++a)
				for (int b = std::max(0, along - wear + std::abs(a - across)); b <= std::min(GridAlong - 1, along + wear - std::abs(a - across)); ++b)
					if (outline[a * GridAlong + b]) body[a * GridAlong + b] = 1;
		}
	bool any = false;
	for (Uint8 cell : body) any = any || cell;
	if (!any) body = outline; // (all of it thin: the block is the outline)
	const std::vector<int> bodyDepth = inside(body);
	const int width = std::min(last[0] - first[0], last[1] - first[1]) + 1;
	int bottom = cuts[1], top = cuts[0];
	while (top - bottom + 1 < (int)(width * HullThin) && (top < GridUp - 1 || bottom > 0))
	{
		if (top < GridUp - 1) ++top; // (upwards: it stands where it stood)
		else --bottom;
	}
	// (its upper edge cut at a slant, HullBevel of its thickness deep: a block with the look of a hull, flat underneath)
	const int thick = top - bottom + 1, bevel = (int)(thick * HullBevel), middle = (top + bottom) / 2;
	int barrel = 0;
	for (int i = 0; i < plan; ++i) if (outline[i] && !body[i]) barrel = std::max(barrel, depth[i]);
	barrel = std::min(barrel, thick / 2);
	for (int across = 0; across < GridAcross; ++across)
		for (int along = 0; along < GridAlong; ++along)
		{
			const int i = across * GridAlong + along;
			for (int up = 0; up < GridUp; ++up)
			{
				bool keep = false;
				if (body[i]) keep = up >= bottom && up <= top && bodyDepth[i] > bevel - (top - up);
				else if (outline[i]) keep = std::abs(up - middle) < barrel;
				cells[cellIndex(across, along, up)] = keep;
			}
		}
}

/// A tank's hull cut flat (see makeModel): its 8 drawings are all made from the same height above it, so what they
/// leave of a flat hull (see carve) is two cones, one on its deck and one under its belly: "a dopey pyramid". Each is
/// cut off at the level where the hull is HullDeck of its widest, level by level from there: a deck for the turret to
/// stand on, sloping sides below it.
constexpr float HullDeck = 0.5f;
/// A tank's turret is read TurretSlack voxels lower than its front and side drawings' top rows; its barrel is where its
/// side drawing runs out ahead no higher than BarrelThin of the turret's tallest column (see makeModel).
constexpr float TurretSlack = 1.0f * VOXELSCALE, BarrelThin = 0.5f;
void flattenHull(std::vector<Uint8> &cells)
{
	std::vector<int> area(GridUp, 0);
	for (int across = 0; across < GridAcross; ++across)
		for (int along = 0; along < GridAlong; ++along)
			for (int up = 0; up < GridUp; ++up) area[up] += cells[cellIndex(across, along, up)] != 0;
	const int widest = (int)(std::max_element(area.begin(), area.end()) - area.begin());
	if (area[widest] == 0) return;
	int deck = widest, belly = widest;
	while (deck + 1 < GridUp && area[deck + 1] >= area[widest] * HullDeck) ++deck;
	while (belly > 0 && area[belly - 1] >= area[widest] * HullDeck) --belly;
	for (int across = 0; across < GridAcross; ++across)
		for (int along = 0; along < GridAlong; ++along)
			for (int up = 0; up < GridUp; ++up) if (up > deck || up < belly) cells[cellIndex(across, along, up)] = 0;
}

/// A long body (a beast on four legs: a dog, a cat, a rat, a pig) is one a mod's rules name a beast (the unit table,
/// see Routine), and no other: a unit that isn't named there is made as ever (the owner: a fix for one kind of unit
/// mustn't "break other voxel units that are good allready like sectoid, snakeman, mutton"). A named one whose side
/// drawing isn't LongNamed times as long as its frontal drawing is wide is left as it was all the same: it doesn't
/// stand as a beast does. (XPiratez's and The X-Com Files' animals are 1.9 to 4 times as long as wide, their
/// scorpions and beetles 1.55; a unit standing upright under 1.4.) Its legs are the rows of its side drawing below its belly:
/// from the ground up, the rows drawn for less than LegsShare of its longest row; and the belly's lowest rows between
/// them (BellyRise, BellyMost: see profile).
constexpr float LongNamed = 1.2f, LegsShare = 0.6f, BellyRise = 0.35f, BellyMost = 0.35f;

/// A frame's drawing: its first and last columns and rows; false if nothing is drawn.
bool drawnBox(const PartFrames &part, int direction, int &left, int &right, int &top, int &bottom)
{
	left = top = FrameSize; right = bottom = -1;
	for (int row = 0; row < FrameSize; ++row)
		for (int column = 0; column < FrameSize; ++column)
		{
			if (!part.at(direction, column, row)) continue;
			left = std::min(left, column); right = std::max(right, column);
			top = std::min(top, row); bottom = std::max(bottom, row);
		}
	return right >= 0;
}

/// Whether a body drawn on one frame, named a beast by a mod's rules, is a long one (see LongNamed).
bool longBody(const PartFrames &part)
{
	int left, right, top, bottom, frontLeft, frontRight, frontTop, frontBottom;
	if (!drawnBox(part, RightSide, left, right, top, bottom) || !drawnBox(part, Front, frontLeft, frontRight, frontTop, frontBottom)) return false;
	return (float)(right - left + 1) >= LongNamed * (float)(frontRight - frontLeft + 1);
}

/// A long body's shape (see longBody): its right side's drawing, given width. Every cell whose place along the body
/// and height that drawing has, read level at the body's middle (a side frame's rows are heights there: they shift
/// with the place across only), as far across as the body is there (below). Read as a body standing upright is, row
/// by row from the front (see limb), its frontal drawing's rows took the far end of it for its top: each pair of legs
/// came out one block, the gap under the belly filled, the feet cut off (a player, GitHub issue #191: attack dogs
/// "look like extremely adorable capybaras with stumpy legs"). And cut to width by the other seven drawings (see
/// carve) it lost what they don't agree on, which for a thing drawn by hand in eight poses is its every thin end: a
/// dog its snout and tail, a Chupacabra its legs, a cobra all but a coil (a player, GitHub issue #211).
/// Its breadth: round. The side drawing's stretch through a pixel, up and down or along the body, whichever is the
/// shorter, is a round thing's thickness there: a trunk is a barrel lying, a neck or a tail a round bar. A thick part
/// of an animal (Legged: one a mod's rules name a beast) is as broad as its frontal drawings have it at that height,
/// see LongReading; a thin part of its front half the front drawing has in two stretches, its ears, stands on both.
/// Its legs (an animal's only) are the rows below its belly (see LegsShare). The belly is drawn lower than it is: the
/// drawings look down on the body, so its near side's lower edge stands BellyRise of its width below its middle's.
/// That much of the belly is taken off between the legs, and the legs go up to there: "long canine like legs", at
/// the body's own height. LegsTop is set to where the legs end. Each leg is a bar where the frontal drawing has it
/// (the front's for the front pair, the back's for the hind pair), on its own side.

/// How a long body's frontal drawings, its front's and its back's, lie on it (see profile): they show its breadth at
/// each height, and its face. A row of one is a height, and lower the further along the body towards that end, Slant
/// rows a unit: none where the drawing is as tall as the side's (a dog drawn head on, its head above its chest), up
/// to the views' own slant where it is taller by the body's length seen from above (a rat, its back rising behind its
/// head). Middle is each drawing's own middle column (they are drawn a pixel off the frame's).
struct LongReading
{
	float slant = 0.0f, middle[2] = { (float)FloorX, (float)FloorX };
	float length = 1.0f; // (how long the body is, of its side drawing's length: see longShape)
	bool drawn[2] = { false, false };
	/// The row of an end's drawing (0 the front's, 1 the back's) a height at a place along the body is drawn on.
	int row(int end, float ahead, float up) const { return (int)std::floor(FloorY - up + (end ? -ahead : ahead) * slant); }
};

LongReading readLong(const PartFrames &part, bool legged)
{
	LongReading reading;
	int left, right, top, bottom;
	if (!drawnBox(part, RightSide, left, right, top, bottom)) return reading;
	int rows = 0, count = 0;
	for (int end = 0; legged && end < 2; ++end)
	{
		int l, r, t, b;
		if (!drawnBox(part, end ? (Front + 4) % 8 : Front, l, r, t, b)) continue;
		reading.drawn[end] = true;
		reading.middle[end] = (l + r + 1) * 0.5f;
		rows += b - t + 1;
		++count;
	}
	if (count) reading.slant = std::clamp((rows / (float)count - (bottom - top + 1)) * PixelsAcross / (right - left + 1), 0.0f, 0.70710678f);
	return reading;
}

std::vector<Uint8> profile(const PartFrames &part, float &legsTop, bool legged, const LongReading &reading)
{
	std::vector<Uint8> cells(GridCells, 0);
	legsTop = 0.0f;
	int left, right, top, bottom, frontLeft, frontRight, frontTop, frontBottom;
	if (!drawnBox(part, RightSide, left, right, top, bottom)) return cells;
	// the legs' rows: from the lowest row up, while a row is drawn for less than LegsShare of the longest one
	int longest = 0;
	std::vector<int> drawn(FrameSize, 0);
	for (int row = top; row <= bottom; ++row)
	{
		for (int column = left; column <= right; ++column) drawn[row] += part.at(RightSide, column, row) != 0;
		longest = std::max(longest, drawn[row]);
	}
	// (a long body that is no beast by name, a snake, has no legs: its lowest rows, shorter than its longest as a leg's
	// are, were kept to the columns of legs it hasn't got, and it lay in pieces)
	int legRows = 0;
	while (legged && bottom - legRows > top && drawn[bottom - legRows] < longest * LegsShare) ++legRows;
	const int belly = bottom - legRows; // (the lowest row of the body as drawn)
	// how far the belly is raised: BellyRise of the body's width, at most BellyMost of the rows above the legs
	int rise = 0;
	if (legRows > 0 && drawnBox(part, Front, frontLeft, frontRight, frontTop, frontBottom))
	{
		rise = std::min((int)std::lround(BellyRise * (frontRight - frontLeft + 1) / PixelsAcross), (int)(BellyMost * (belly - top + 1)));
	}
	const int legsFrom = belly - rise + 1; // (the legs' first row)
	legsTop = (float)(FloorY - legsFrom);
	// where the legs stand along the body: the columns the side drawing has in the legs' upper rows
	std::vector<Uint8> stands(FrameSize, 0);
	for (int row = belly + 1; row <= std::min(bottom, belly + 1 + VOXELSCALE); ++row)
		for (int column = left; column <= right; ++column) if (part.at(RightSide, column, row)) stands[column] = 1;
	// the columns a pair of legs keeps to: what a drawing (the front's, the back's) has in its lowest rows
	auto legColumns = [&](int direction) {
		std::vector<Uint8> columns(FrameSize, 0);
		int l, r, t, b;
		if (legRows < 2 || !drawnBox(part, direction, l, r, t, b)) return std::vector<Uint8>(FrameSize, 1);
		for (int row = b; row > b - (legRows - 1) && row >= t; --row)
			for (int column = l; column <= r; ++column) if (part.at(direction, column, row)) columns[column] = 1;
		return columns;
	};
	const std::vector<Uint8> frontLegs = legColumns(Front), hindLegs = legColumns((Front + 4) % 8);
	// (no wider anywhere than its frontal drawing)
	float widest = 1e9f;
	if (drawnBox(part, Front, frontLeft, frontRight, frontTop, frontBottom)) widest = (frontRight - frontLeft + 1) * 0.5f / PixelsAcross;
	// (An animal's breadth, where it is thick, is its frontal drawings': see below. Each row of the two: its first and
	// last columns, and how many stretches it is drawn in.)
	std::vector<int> rowFirst[2], rowLast[2], rowRuns[2];
	for (int end = 0; end < 2; ++end)
	{
		rowFirst[end].assign(FrameSize, FrameSize); rowLast[end].assign(FrameSize, -1); rowRuns[end].assign(FrameSize, 0);
		for (int row = 0; reading.drawn[end] && row < FrameSize; ++row)
			for (int column = 0; column < FrameSize; ++column)
			{
				if (!part.at(end ? (Front + 4) % 8 : Front, column, row)) continue;
				if (column == 0 || !part.at(end ? (Front + 4) % 8 : Front, column - 1, row)) ++rowRuns[end][row];
				rowFirst[end][row] = std::min(rowFirst[end][row], column);
				rowLast[end][row] = column;
			}
	}
	auto breadthAt = [&](int end, int row) {
		return row < 0 || row >= FrameSize || rowLast[end][row] < 0 ? 0.0f : (rowLast[end][row] - rowFirst[end][row] + 1) * 0.5f / PixelsAcross;
	};
	// (An animal's legs, row by row: each leg a bar where the frontal drawing has it at that height, so legs spread
	// wide stand as struts, as drawn. Kept to all the columns the drawing has in its lowest rows, as they were,
	// each was a wall as wide as its foot's way out: a Chupacabra, the owner's "disjoined body" and a thing with no
	// legs to tell. A row the drawing has in one stretch, where its legs meet under its chest, takes the nearest
	// row's below it that has two or more, or above it; a drawing with no such row leaves the legs as they were.)
	// (And each leg on its own side. The side drawing has a pair's two legs as two strokes, where they stand apart:
	// the near one, the unit's right, ends lower down the picture. Both stood on both bars, and each leg was the two
	// together, twice as thick as drawn: the owner, of the dog, "needs to be much more elegant". The strokes: what is
	// joined of the drawing from its first row under the belly that is in two stretches or more; of those of a
	// half of the body, front or hind, the lower-ending half are its right legs, the others its left; one alone, or
	// two ending level, is both.)
	std::vector<int> stroke((size_t)FrameSize * FrameSize, -1);
	std::vector<int> strokeSide;
	int strokesFrom = FrameSize;
	for (int row = belly + 1; legged && row <= bottom && strokesFrom == FrameSize; ++row)
	{
		int runs = 0;
		for (int column = left; column <= right; ++column) runs += part.at(RightSide, column, row) && !part.at(RightSide, column - 1, row);
		if (runs >= 2) strokesFrom = row;
	}
	if (strokesFrom < FrameSize)
	{
		struct Stroke { int label, count, bottom; float middle; };
		std::vector<Stroke> halves[2];
		for (int row = strokesFrom; row <= bottom; ++row)
			for (int column = left; column <= right; ++column)
			{
				if (!part.at(RightSide, column, row) || stroke[(size_t)row * FrameSize + column] >= 0) continue;
				Stroke found = { (int)strokeSide.size(), 0, row, 0.0f };
				std::vector<std::pair<int, int>> stack(1, { column, row });
				stroke[(size_t)row * FrameSize + column] = found.label;
				while (!stack.empty())
				{
					const std::pair<int, int> at = stack.back();
					stack.pop_back();
					++found.count;
					found.bottom = std::max(found.bottom, at.second);
					found.middle += at.first + 0.5f;
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx)
						{
							const int c = at.first + dx, r = at.second + dy;
							if (r < strokesFrom || r > bottom || c < left || c > right || !part.at(RightSide, c, r) || stroke[(size_t)r * FrameSize + c] >= 0) continue;
							stroke[(size_t)r * FrameSize + c] = found.label;
							stack.push_back({ c, r });
						}
				}
				found.middle /= found.count;
				strokeSide.push_back(0);
				if (found.count >= 3 * VOXELSCALE * VOXELSCALE) halves[found.middle >= FloorX ? 0 : 1].push_back(found);
			}
		for (std::vector<Stroke> &half : halves)
		{
			if (half.size() < 2) continue;
			std::sort(half.begin(), half.end(), [](const Stroke &one, const Stroke &other) { return one.bottom > other.bottom; });
			const size_t near = (half.size() + 1) / 2;
			if (half[near - 1].bottom == half[near].bottom) continue; // (ending level: no telling)
			for (size_t i = 0; i < half.size(); ++i) strokeSide[half[i].label] = i < near ? 1 : -1;
		}
	}
	auto legSide = [&](int column, int row) {
		if (strokesFrom == FrameSize || column < 0 || column >= FrameSize) return 0;
		for (int r = std::max(row, strokesFrom); r <= std::min(bottom, std::max(row, strokesFrom) + VOXELSCALE); ++r)
		{
			if (stroke[(size_t)r * FrameSize + column] >= 0) return strokeSide[stroke[(size_t)r * FrameSize + column]];
			if (row >= strokesFrom) break;
		}
		return 0;
	};
	std::vector<std::vector<std::pair<float, float>>> rowBars[2];
	std::vector<int> barsRow[2];
	for (int end = 0; end < 2; ++end)
	{
		rowBars[end].resize(FrameSize);
		barsRow[end].assign(FrameSize, -1);
		if (!reading.drawn[end]) continue;
		const int direction = end ? (Front + 4) % 8 : Front;
		for (int row = 0; row < FrameSize; ++row)
			for (int column = 0, start = -1; column <= FrameSize; ++column)
			{
				const bool on = column < FrameSize && part.at(direction, column, row);
				if (on && start < 0) start = column;
				if (on || start < 0) continue;
				const float middle = (reading.middle[end] - (start + column) * 0.5f) / PixelsAcross;
				rowBars[end][row].push_back({ end ? -middle : middle, (column - start) * 0.5f / PixelsAcross });
				start = -1;
			}
		for (int row = 0; row < FrameSize; ++row)
		{
			for (int lower = row; lower < FrameSize && barsRow[end][row] < 0; ++lower) if (rowRuns[end][lower] >= 2) barsRow[end][row] = lower;
			for (int upper = row; upper >= 0 && barsRow[end][row] < 0; --upper) if (rowRuns[end][upper] >= 2) barsRow[end][row] = upper;
		}
	}
	// (Each leg a bar where a frontal drawing has it, no thicker than the side drawing has it. The runs of a drawing's
	// leg columns: each one's middle and half width, across the body.)
	auto legBars = [&](const std::vector<Uint8> &columns, bool front) {
		std::vector<std::pair<float, float>> bars;
		for (int column = 0, start = -1; column <= FrameSize; ++column)
		{
			const bool on = column < FrameSize && columns[column];
			if (on && start < 0) start = column;
			if (!on && start >= 0)
			{
				const float middle = (FloorX - (start + column) * 0.5f) / PixelsAcross;
				bars.push_back({ front ? middle : -middle, (column - start) * 0.5f / PixelsAcross });
				start = -1;
			}
		}
		return bars;
	};
	const std::vector<std::pair<float, float>> frontBars = legBars(frontLegs, true), hindBars = legBars(hindLegs, false);
	for (int along = 0; along < GridAlong; ++along)
	{
		// (the body's place along its side drawing: see LongReading's length)
		const float drawnAhead = cellAhead(along) / reading.length;
		const int sideRight = (int)std::floor(FloorX + drawnAhead * PixelsAcross);
		for (int up = 0; up < GridUp; ++up)
		{
			// (nothing under the floor; what the side drawing has below the floor's row lies on it)
			if (cellUp(up) < 0.0f) continue;
			int row = (int)std::floor(FloorY - cellUp(up));
			if (cellUp(up) < GroundPlate)
			{
				for (int lower = row; lower <= std::min(FrameSize - 1, row + ProfileBelow); ++lower)
				{
					if (part.at(RightSide, sideRight, lower)) { row = lower; break; }
				}
			}
			if (!part.at(RightSide, sideRight, row)) continue;
			const bool leg = row >= legsFrom;
			// (Round: the drawing's stretch through this pixel, up and down or along the body, whichever is the shorter,
			// is a round thing's thickness there, and it is as far across as that from its middle. As deep as the
			// outline is near, all of it was a slab with its edges cut: the owner, of the dog, "needs to be much more
			// elegant".)
			int runTop = row, runBottom = row, runFirst = sideRight, runLast = sideRight;
			while (runTop > 0 && part.at(RightSide, sideRight, runTop - 1)) --runTop;
			// (the trunk's stretch ends at the belly: the legs under it are their own)
			while (runBottom < FrameSize - 1 && part.at(RightSide, sideRight, runBottom + 1) && (leg || runBottom + 1 < legsFrom)) ++runBottom;
			while (runFirst > 0 && part.at(RightSide, runFirst - 1, row)) --runFirst;
			while (runLast < FrameSize - 1 && part.at(RightSide, runLast + 1, row)) ++runLast;
			const float tall = (runBottom - runTop + 1) * 0.5f, lengthways = (runLast - runFirst + 1) * 0.5f;
			const bool lying = tall <= lengthways / PixelsAcross * reading.length;
			const float off = lying ? (row + 0.5f - (runTop + runBottom + 1) * 0.5f) / tall : (sideRight + 0.5f - (runFirst + runLast + 1) * 0.5f) / lengthways;
			// (A round bar lying is drawn thicker than it is: the drawings look down on it, and its near side's lower
			// edge stands below its middle's. Drawn 1.22 times its thickness: so much less across.)
			float breadth = lying ? tall * ProfileLyingRound : lengthways / PixelsAcross * reading.length;
			// (An animal's thick parts, its trunk and its head, are as broad as its frontal drawings have them at their
			// height: the front's for its front half, the back's for its hind half. A part lying, the trunk, as its
			// broadest row there; a part standing, a head on a neck, row by row. So a dog's head is narrower than its
			// chest, as drawn; round as its side drawing is thick, it was as broad as the trunk. And a thin part of
			// its front half the front drawing has in two stretches or more, its ears, its horns, stands where they
			// are: on either side, the same on both.)
			const int end = cellAhead(along) >= 0.0f ? 0 : 1;
			const int endRow = reading.row(end, drawnAhead, cellUp(up)) + (row - (int)std::floor(FloorY - cellUp(up)));
			bool pair = false;
			if (reading.drawn[end] && !leg)
			{
				if ((lying ? tall : breadth) >= ProfileThick)
				{
					float drawnBreadth = 0.0f;
					if (lying) for (int r = runTop; r <= runBottom; ++r) drawnBreadth = std::max(drawnBreadth, breadthAt(end, endRow + r - row));
					else drawnBreadth = breadthAt(end, endRow);
					if (drawnBreadth > 0.0f) breadth = drawnBreadth;
				}
				else pair = end == 0 && endRow >= 0 && endRow < FrameSize && rowRuns[0][endRow] >= 2;
			}
			const float half = std::min(widest, std::max(ProfileHalfLeast, breadth * std::sqrt(std::max(0.0f, 1.0f - off * off))));
			// (the belly's raised rows: only the legs are left of them)
			if (leg && row <= belly && !(sideRight >= 0 && sideRight < FrameSize && stands[sideRight])) continue;
			for (int across = 0; across < GridAcross; ++across)
			{
				if (pair)
				{
					// (the front drawing has the unit's right on its left; either side's stretch counts for both)
					const float offset = cellRight(across) * PixelsAcross;
					if (!part.at(Front, (int)std::floor(reading.middle[0] - offset), endRow) && !part.at(Front, (int)std::floor(reading.middle[0] + offset), endRow)) continue;
				}
				else if (!leg && std::abs(cellRight(across)) > half) continue;
				const int legBarsRow = leg && endRow >= 0 && endRow < FrameSize ? barsRow[end][endRow] : -1;
				if (leg)
				{
					const std::vector<std::pair<float, float>> &bars = legBarsRow >= 0 ? rowBars[end][legBarsRow] : end == 0 ? frontBars : hindBars;
					// (its own side's bars, where the drawing has one there)
					int side = legBarsRow >= 0 ? legSide(sideRight, row) : 0;
					bool sided = false;
					for (const auto &bar : bars) sided = sided || (side && (bar.first > 0.0f) == (side > 0));
					if (!sided) side = 0;
					bool onBar = false;
					for (const auto &bar : bars)
					{
						if (side && (bar.first > 0.0f) != (side > 0)) continue;
						onBar = onBar || std::abs(cellRight(across) - bar.first) <= std::min(bar.second, half + 0.5f);
					}
					if (!onBar) continue;
				}
				if (leg && legBarsRow < 0)
				{
					// (the front drawing has the unit's right on its left, the back drawing on its right)
					const int column = (int)std::floor(cellAhead(along) >= 0.0f ? FloorX - cellRight(across) * PixelsAcross : FloorX + cellRight(across) * PixelsAcross);
					if (column < 0 || column >= FrameSize || !(cellAhead(along) >= 0.0f ? frontLegs : hindLegs)[column]) continue;
				}
				cells[cellIndex(across, along, up)] = 1;
			}
		}
	}
	return cells;
}

/// A long body's shape at the length its drawings have it (the owner: "The dog and cobra should not be too large! Take
/// care on the size and keep original sprite sizes approximately"). Its side drawing alone makes it as long as that
/// drawing is wide, and the drawings from the quarters, drawn by hand, have most animals shorter: The X-Com Files' dog
/// is 28 pixels long from the side, 19.8 voxels, and 18 to 21 wide from the quarters, where a body that long would be
/// 22. Seen in the game beside its own sprites, from the same places, it was 7% wider from the side and 13% from a
/// quarter. So the six drawings that show its length each have a say: the body is made at its side drawing's length,
/// its width on each of the six is measured as the drawing's is, and it is made again at the length (LongLeast of the
/// side drawing's or more) that brings the six nearest their drawings' widths, each by its own share. The dog comes
/// out 0.9 of its side drawing's length; a snake drawn as long from every side stays as it is.
/// Reading's length is set to what was found.
const float LongLeast = 0.75f, LongStep = 0.025f;
std::vector<Uint8> longShape(const PartFrames &part, float &legsTop, bool legged, LongReading &reading)
{
	reading.length = 1.0f;
	std::vector<Uint8> shape = profile(part, legsTop, legged, reading);
	// where the body stands: its cells seen from above
	std::vector<std::pair<float, float>> stood;
	for (int across = 0; across < GridAcross; ++across)
		for (int along = 0; along < GridAlong; ++along)
			for (int up = 0; up < GridUp; ++up)
			{
				if (!shape[cellIndex(across, along, up)]) continue;
				stood.push_back({ cellRight(across), cellAhead(along) });
				break;
			}
	static const int Lengthways[6] = { RightSide, LeftSide, 0, 2, 4, 6 };
	float drawn[6];
	for (int i = 0; i < 6; ++i)
	{
		int left, right, top, bottom;
		drawn[i] = drawnBox(part, Lengthways[i], left, right, top, bottom) ? (float)(right - left + 1) : 0.0f;
	}
	float best = 1.0f, least = 1e30f;
	for (float length = 1.0f; length > LongLeast - 0.001f && !stood.empty(); length -= LongStep)
	{
		float off = 0.0f;
		for (int i = 0; i < 6; ++i)
		{
			if (drawn[i] <= 0.0f) continue;
			float first = 1e9f, last = -1e9f;
			for (const std::pair<float, float> &cell : stood)
			{
				float column, row, nearness;
				project(cell.first, cell.second * length, 0.0f, Lengthways[i], false, column, row, nearness);
				first = std::min(first, column);
				last = std::max(last, column);
			}
			const float share = (last - first + 1.0f - drawn[i]) / drawn[i];
			off += share * share;
		}
		if (off < least - 1e-6f) { least = off; best = length; }
	}
	if (best < 1.0f)
	{
		reading.length = best;
		shape = profile(part, legsTop, legged, reading);
	}
	return shape;
}

/// Fills the cells of an elliptical slice at a frame row's height: centre (right, ahead) and half sizes, voxels.
void fillSlice(std::vector<Uint8> &cells, int row, float centreRight, float halfRight, float centreAhead, float halfAhead)
{
	// (A cell is drawn as a square about its middle, VoxelSplatSize across: cells whose middles reach the slice's edge
	// stood a quarter of a voxel over it all round, and a slim body, a Sectoid's five pixels of torso, came out a
	// sixth too fat: the owner, "the sectoid in models looks too fat". So the middles stay SliceInset inside.)
	const float inset = SliceInset;
	halfRight = std::max(halfRight - inset, 0.35f);
	halfAhead = std::max(halfAhead - inset, 0.35f);
	// A row below the floor's row is no slice under the floor: the front drawing looks down along the slant, and what
	// it has below the floor's row lies on the ground ahead, so many rows down, so far ahead (a row is 1.41 of the
	// ahead axis: see project). Read as a height it sank into the floor the unit stands on: a foot's toes, the length
	// of a snake lying on the ground (a player, GitHub issue #211: "feet don't exist in voxel land", a cobra "slightly
	// inside the ground"). A plate GroundPlate thick on the floor, as wide as the row's run.
	const float below = row + 0.5f - FloorY;
	if (below > 0.0f && !BigUnit)
	{
		const float ahead = below * GroundAheadARow, half = 0.5f * GroundAheadARow;
		for (int up = 0; up < GridUp; ++up)
		{
			if (cellUp(up) < 0.0f || cellUp(up) > GroundPlate) continue;
			for (int across = 0; across < GridAcross; ++across)
			{
				if (std::abs(cellRight(across) - centreRight) > halfRight) continue;
				for (int along = 0; along < GridAlong; ++along)
				{
					if (std::abs(cellAhead(along) - ahead) <= half) cells[cellIndex(across, along, up)] = 1;
				}
			}
		}
		return;
	}
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

/// The first row of a torso's frontal drawing nearly as wide as its widest (9 tenths): the shoulders, the head above
/// them; FrameSize if nothing is drawn.
int shouldersRow(const PartFrames &torso)
{
	int widest = 0;
	for (int row = 0; row < FrameSize; ++row)
		for (const auto &run : runs(torso, Front, row)) widest = std::max(widest, run.second - run.first + 1);
	for (int row = 0; row < FrameSize && widest > 0; ++row)
		for (const auto &run : runs(torso, Front, row)) if ((run.second - run.first + 1) * 10 >= widest * 9) return row;
	return FrameSize;
}

/// The head and torso: each row of the frame a slice as wide as the frontal frame's row and as deep as the side
/// frame's (bits beside the main run small round slices); with a chin (bodyTop >= 0), the chin's rows a shallow
/// piece at the front of the face and a thin neck behind them from the head's underside to the torso.
/// DepthMost (the unit table's torsoDepth, for the units it names; 0: no limit): below the shoulders (from the first
/// row nearly as wide as the widest) a slice is at most that many times as deep as it is wide, cut at the front.
/// XPiratez's gals' torsos are drawn from the side with the hips up to 10 pixels deep on 8 wide: that part of the
/// drawing lies under the arm, hand on hip, and is never seen; as a body's depth it was a belly sticking out past
/// the chest (a player, GitHub issue #184: "Gals got fat").
void headAndTorso(const PartFrames &torso, int bodyTop, std::vector<Uint8> &cells, std::vector<Uint8> &neck, float depthMost)
{
	std::vector<float> torsoAhead;
	const int shoulders = depthMost > 0.0f ? shouldersRow(torso) : FrameSize;
	for (int row = 0; row < FrameSize; ++row)
	{
		const auto front = runs(torso, Front, row), side = runs(torso, RightSide, row);
		if (front.empty() || side.empty()) continue;
		const auto main = *std::max_element(front.begin(), front.end(), [](const std::pair<int, int> &a, const std::pair<int, int> &b) { return a.second - a.first < b.second - b.first; });
		const float centreRight = (FloorX - (main.first + main.second + 1) * 0.5f) / PixelsAcross, halfRight = (main.second - main.first + 1) * 0.5f / PixelsAcross;
		float back, fore;
		sideExtent(side.front(), side.back(), RightSide, back, fore);
		if (bodyTop >= 0 && row >= bodyTop - ChinRows && row < bodyTop) back = std::max(back, fore - ChinDepth);
		if (depthMost > 0.0f && row >= shoulders) fore = std::min(fore, back + depthMost * 2.0f * halfRight);
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
/// frame's row where the limb is nearest the camera. Where the side frame's row has several runs too (a leg forward
/// and a leg back), each slice is as deep as one of them: of the ways to give every frontal run and every side run a
/// slice with as few slices as can be, the one the four diagonal frames show most of (read level, at each slice's
/// middle); a tie goes as the row above went. (Every frontal run as deep as all the side runs together made four
/// legs of two, seen from between the directions: the owner, "check old models so they dont have 4 legs from the
/// side", "follow the contour of sprites".)
std::vector<Uint8> limb(const PartFrames &part, int side)
{
	std::vector<Uint8> cells(GridCells, 0);
	std::vector<std::pair<int, int>> pairsAbove;
	std::vector<float> aheadAbove; // (where each frontal run's slice stood in the row above, when it was cut to depth)
	for (int row = 0; row < FrameSize; ++row)
	{
		const auto front = runs(part, Front, row);
		if (front.empty()) { pairsAbove.clear(); continue; }
		const auto profile = runs(part, side, row);
		const int frontCount = (int)front.size(), sideCount = (int)profile.size();
		auto middleRight = [&](int run) { return (FloorX - (front[run].first + front[run].second + 1) * 0.5f) / PixelsAcross; };
		auto halfRight = [&](int run) { return (front[run].second - front[run].first + 1) * 0.5f / PixelsAcross; };
		if (sideCount < 2 || frontCount > MaxPairedRuns || sideCount > MaxPairedRuns)
		{
			float back = -0.8f, fore = 0.8f;
			if (!profile.empty()) sideExtent(profile.front(), profile.back(), side, back, fore);
			if (aheadAbove.size() != front.size()) aheadAbove.assign(front.size(), 1e9f);
			for (int run = 0; run < frontCount; ++run)
			{
				// Several runs from the front on one from the side (two thin legs side by side, their one side run as
				// long as their feet and their stance together): each no deeper than SliceDepthMost times its width, and
				// standing in the side run where the diagonal frames show most of it (read level; a tie: as the row
				// above, else the middle). Each as deep as the whole side run, a Sectoid's two-pixel legs were slabs,
				// twice as wide as the sprite's from every angle but the front (the owner: "looks too fat").
				float middle = (back + fore) * 0.5f, half = (fore - back) * 0.5f;
				const float halfMost = SliceDepthMost * halfRight(run);
				if (frontCount > 1 && !profile.empty() && half > halfMost)
				{
					float bestMiddle = middle;
					int bestShown = -1;
					for (float at = back + halfMost; at <= fore - halfMost + 0.01f; at += 0.25f)
					{
						int shown = 0;
						for (int d = 0; d < 8; d += 2)
						{
							for (float offset = -halfMost * 0.6f; offset <= halfMost * 0.61f; offset += halfMost * 0.6f)
							{
								float column, rowLevel, nearness;
								project(middleRight(run), at + offset, 0.0f, d, true, column, rowLevel, nearness);
								if (part.at(d, (int)std::floor(column), row)) ++shown;
							}
						}
						const float aim = aheadAbove[run] < 1e8f ? aheadAbove[run] : middle;
						if (shown > bestShown || (shown == bestShown && std::abs(at - aim) < std::abs(bestMiddle - aim))) { bestShown = shown; bestMiddle = at; }
					}
					middle = bestMiddle;
					half = halfMost;
					aheadAbove[run] = middle;
				}
				fillSlice(cells, row, middleRight(run), halfRight(run), middle, half);
			}
			pairsAbove.clear();
			continue;
		}
		// How much the diagonal frames show of a slice at a frontal run and a side run: 0 to 4.
		int shown[MaxPairedRuns][MaxPairedRuns];
		float backs[MaxPairedRuns], fores[MaxPairedRuns];
		for (int sideRun = 0; sideRun < sideCount; ++sideRun) sideExtent(profile[sideRun], profile[sideRun], side, backs[sideRun], fores[sideRun]);
		for (int run = 0; run < frontCount; ++run)
		{
			for (int sideRun = 0; sideRun < sideCount; ++sideRun)
			{
				shown[run][sideRun] = 0;
				for (int d = 0; d < 8; d += 2)
				{
					float column, rowLevel, nearness;
					project(middleRight(run), (backs[sideRun] + fores[sideRun]) * 0.5f, 0.0f, d, true, column, rowLevel, nearness);
					const int at = (int)std::floor(column);
					if (part.at(d, at, row) || part.at(d, at - 1, row) || part.at(d, at + 1, row)) ++shown[run][sideRun];
				}
			}
		}
		// Every choice of max(frontCount, sideCount) pairs that leaves no run out (a bit a pair: at most 36).
		const int pairCount = frontCount * sideCount, wanted = std::max(frontCount, sideCount);
		std::vector<std::pair<int, int>> best;
		int bestScore = -1;
		for (Uint64 choice = 1; choice < (Uint64(1) << pairCount); ++choice)
		{
			int picked = 0, score = 0;
			unsigned frontSeen = 0, sideSeen = 0;
			for (int pair = 0; pair < pairCount && picked <= wanted; ++pair)
			{
				if (!(choice >> pair & 1)) continue;
				++picked;
				frontSeen |= 1u << (pair / sideCount);
				sideSeen |= 1u << (pair % sideCount);
				score += shown[pair / sideCount][pair % sideCount];
			}
			if (picked != wanted || frontSeen != (1u << frontCount) - 1 || sideSeen != (1u << sideCount) - 1) continue;
			std::vector<std::pair<int, int>> pairs;
			for (int pair = 0; pair < pairCount; ++pair) if (choice >> pair & 1) pairs.push_back({ pair / sideCount, pair % sideCount });
			score = score * 2 + (pairs == pairsAbove ? 1 : 0);
			if (score > bestScore) { bestScore = score; best = pairs; }
		}
		for (const auto &pair : best)
		{
			fillSlice(cells, row, middleRight(pair.first), halfRight(pair.first), (backs[pair.second] + fores[pair.second]) * 0.5f, (fores[pair.second] - backs[pair.second]) * 0.5f);
		}
		pairsAbove = best;
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
		else if (row >= shoulders + 2 * VOXELSCALE) torsoWidest = std::max(torsoWidest, width);
	}
	return headWidest >= HeadWidthMin && torsoWidest > 0 && torsoWidest <= NarrowTorso * headWidest ? shoulders + 2 * VOXELSCALE : -1;
}

inline int luminance(const SDL_Color &colour)
{
	return (colour.r * 30 + colour.g * 59 + colour.b * 11) / 100;
}

/// A frame's solid drawing only (a big unit's: see makeModel): what's left of it where a 3x3 square of drawn pixels fits,
/// grown back by those squares; fine detail, a pixel or two wide (sparks, streaks), cleared.
void keepSolid(PartFrames &part)
{
	for (int d = 0; d < 8; ++d)
	{
		std::vector<Uint8> &frame = part.frame[d];
		std::vector<Uint8> core(FrameSize * FrameSize, 0), kept(FrameSize * FrameSize, 0);
		for (int row = 1; row < FrameSize - 1; ++row)
			for (int column = 1; column < FrameSize - 1; ++column)
			{
				bool full = true;
				for (int dy = -1; dy <= 1 && full; ++dy)
					for (int dx = -1; dx <= 1 && full; ++dx) full = frame[(row + dy) * FrameSize + column + dx] != 0;
				core[row * FrameSize + column] = full;
			}
		for (int row = 1; row < FrameSize - 1; ++row)
			for (int column = 1; column < FrameSize - 1; ++column)
			{
				if (!core[row * FrameSize + column]) continue;
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx) kept[(row + dy) * FrameSize + column + dx] = 1;
			}
		for (int i = 0; i < FrameSize * FrameSize; ++i) if (!kept[i]) frame[i] = 0;
	}
}

/// Each drawn pixel's shade the middle one of its own and its 8 neighbours' in the same palette block (as the body's
/// colours are smoothed, see makeModel; edges between colours stay): for the pieces added for views, painted straight
/// from the frames.
void smoothFrames(PartFrames parts[PartCount])
{
	for (int p = 0; p < PartCount; ++p)
	{
		if (!parts[p].any) continue;
		for (int d = 0; d < 8; ++d)
		{
			const std::vector<Uint8> before(parts[p].frame[d]);
			for (int row = 0; row < FrameSize; ++row)
				for (int column = 0; column < FrameSize; ++column)
				{
					const Uint8 colour = before[row * FrameSize + column];
					if (!colour) continue;
					int shades[9], count = 0;
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx)
						{
							const int c = column + dx, r = row + dy;
							if (c < 0 || r < 0 || c >= FrameSize || r >= FrameSize) continue;
							const Uint8 other = before[r * FrameSize + c];
							if (other && other / 16 == colour / 16) shades[count++] = other % 16;
						}
					if (count < 3) continue;
					std::nth_element(shades, shades + count / 2, shades + count);
					const Uint8 middle = (Uint8)(colour / 16 * 16 + shades[count / 2]);
					if (middle) parts[p].frame[d][row * FrameSize + column] = middle;
				}
		}
	}
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

/// Turns the chosen points about the line through (ahead pivotAhead, up pivot) along the unit's right: feet forward for
/// a positive angle.
void swing(std::vector<float> &ahead, std::vector<float> &up, const std::vector<bool> &chosen, float pivot, double angle, float pivotAhead = 0.0f)
{
	const float c = (float)std::cos(angle), s = (float)std::sin(angle);
	for (size_t k = 0; k < chosen.size(); ++k)
	{
		if (!chosen[k]) continue;
		const float a = ahead[k] - pivotAhead, u = up[k] - pivot;
		ahead[k] = pivotAhead + a * c - u * s;
		up[k] = pivot + a * s + u * c;
	}
}

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
void makeWalk(VoxelUnitModel &model, const Routine &routine, bool leftHolds, bool rightHolds, const WalkFrames &frames, bool beast)
{
	const size_t count = model.part.size(), own = std::min(count, model.addedFrom); // (the pieces added for a view: posed along, not fitted)
	if (beast)
	{
		// A long body on four legs (see longBody): each leg swings stiff about its own hip, the pairs across the body
		// together (the right foreleg with the left hind leg: a trot), BeastSwing either way; the lowest foot stays on
		// the ground. (Walked as a body on two legs, about one hip in its middle, its forelegs rose and its hind legs sank.)
		float hip = -1e9f, ground = 1e9f;
		std::vector<bool> leg[4];
		float middle[4] = {};
		int points[4] = {};
		for (auto &chosen : leg) chosen.resize(count);
		for (size_t k = 0; k < count; ++k)
		{
			if (model.part[k] != VoxelUnitModel::Legs) continue;
			const int which = (model.right[k] > 0.0f ? 0 : 1) + (model.ahead[k] > 0.0f ? 0 : 2);
			leg[which][k] = true;
			middle[which] += model.ahead[k];
			++points[which];
			hip = std::max(hip, model.up[k]);
			ground = std::min(ground, model.up[k]);
		}
		for (int phase = 0; phase < 8; ++phase)
		{
			std::vector<float> ahead = model.ahead, up = model.up;
			for (int which = 0; which < 4; ++which)
			{
				if (!points[which]) continue;
				const bool first = which == 0 || which == 3; // (the right foreleg and the left hind leg)
				const double angle = BeastSwing * std::cos(2.0 * Pi * (phase / 8.0 + (first ? 0.0 : 0.5))) * Pi / 180.0;
				swing(ahead, up, leg[which], hip, angle, middle[which] / points[which]);
			}
			float lowest = 1e9f;
			for (size_t k = 0; k < own; ++k) if (model.part[k] == VoxelUnitModel::Legs) lowest = std::min(lowest, up[k]);
			if (lowest < 1e8f) for (float &height : up) height += ground - lowest;
			model.walkRight[phase] = model.right;
			model.walkAhead[phase] = std::move(ahead);
			model.walkUp[phase] = std::move(up);
		}
		return;
	}
	float hip = -1e9f, footStanding = 1e9f;
	for (size_t k = 0; k < own; ++k)
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
			if (arm[side][k] && k < own) shoulder[side] = std::max(shoulder[side], model.up[k]);
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
			if (model.part[k] != VoxelUnitModel::Legs) up[k] -= pose.bob * VOXELSCALE; // (the bob is in the frames' own pixels)
			else if (k < own) footNow = std::min(footNow, up[k]);
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
			for (size_t k = 0; k < own; k += 2)
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
/// Upright: the item is drawn standing in the hand, taller than wide in every hand frame (a sword, a staff: see
/// placeItem); it is as long as it is drawn high, stands up, and one hand holds it at its lower end.
/// Straight: its picture's own long way is laid along it (see addItem): the pictures draw a staff or a spear from corner
/// to corner, and one stood in the hand, or laid the way its hand frames draw it, leant as its picture does.
/// Plate: an item standing in the hand that its hand frames draw broad from the front and the back and thin from the
/// sides (a shield): it stands with its picture's face ahead, the hand behind its middle.
struct ItemPlace
{
	float middle[3], length;
	float along[3], across[3], through[3];
	bool upright = false;
	bool plate = false;
	bool straight = false;
};
/// An item is drawn standing in the hand if its tallest hand frame is this many times as high as its widest is wide.
constexpr float ItemUpright = 1.5f;
/// An item's own length may lie inside the body for this many hundredths of it (looked at in ItemSamples places); one
/// deeper in is moved ahead, ItemOutMost voxels at most (see makeModel).
constexpr int ItemInBody = 30, ItemSamples = 16;
/// An arm coloured along its length (see makeModel) takes its colours in this many stretches from shoulder to hand.
constexpr int ArmBands = 10;
constexpr float ItemOutMost = 6.0f * VOXELSCALE;
/// An item standing in the hand is a plate if its front and back hand frames are each this many times as wide as the
/// wider of its two side frames.
constexpr float ItemPlate = 2.0f;
constexpr int ItemPlateFilled = 60;
/// A hand frame shows which way an item points if its drawing is this many times as long as it is wide; the frames'
/// own direction is taken for a melee weapon if it is this far (the cosine) from the way a gun would be held.
constexpr float ItemLong = 1.8f, ItemAxisAgree = 0.82f;

/// The way an item points, from its hand frames: the direction whose picture in each of the 8 views lies best along the
/// item's drawing there (each view's drawing has a long way: the direction it's spread widest in; the views that show
/// the item end on, a blob, say nothing). False if fewer than three views show it long. Its sign: ahead rather than
/// behind, then to the unit's left, then up (as a gun's muzzle is held). Length: how long the item is, voxels, where
/// the views show it.
bool itemAxis(const PartFrames &frames, float axis[3], float &length)
{
	double m[3][3] = {};
	int views = 0;
	struct Seen { double way[2], from, to; double p[2][3]; };
	std::vector<Seen> seen;
	for (int d = 0; d < 8; ++d)
	{
		double columns = 0.0, rows = 0.0;
		int count = 0;
		for (int row = 0; row < FrameSize; ++row)
			for (int column = 0; column < FrameSize; ++column)
				if (frames.frame[d][row * FrameSize + column]) { columns += column; rows += row; ++count; }
		if (count < 4 * VOXELSCALE) continue;
		columns /= count; rows /= count;
		double xx = 0.0, xy = 0.0, yy = 0.0;
		for (int row = 0; row < FrameSize; ++row)
			for (int column = 0; column < FrameSize; ++column)
				if (frames.frame[d][row * FrameSize + column]) { xx += (column - columns) * (column - columns); xy += (column - columns) * (row - rows); yy += (row - rows) * (row - rows); }
		xx /= count; xy /= count; yy /= count;
		const double turn = 0.5 * std::atan2(2.0 * xy, xx - yy), spread = std::sqrt((xx - yy) * (xx - yy) * 0.25 + xy * xy);
		const double longWay = (xx + yy) * 0.5 + spread, shortWay = std::max((xx + yy) * 0.5 - spread, 1.0 / 12.0 * VOXELSCALE * VOXELSCALE);
		if (longWay < ItemLong * ItemLong * shortWay) continue;
		Seen view;
		view.way[0] = std::cos(turn); view.way[1] = std::sin(turn);
		view.from = 1e9; view.to = -1e9;
		for (int row = 0; row < FrameSize; ++row)
			for (int column = 0; column < FrameSize; ++column)
				if (frames.frame[d][row * FrameSize + column])
				{
					const double at = (column - columns) * view.way[0] + (row - rows) * view.way[1];
					view.from = std::min(view.from, at); view.to = std::max(view.to, at);
				}
		// (the view's picture of a step right, ahead and up: see project, along the slant)
		const Turn &t = Turns[d];
		const double p[2][3] = { { t.cosine - t.sine, t.sine + t.cosine, 0.0 }, { (t.cosine + t.sine) * 0.5, (t.sine - t.cosine) * 0.5, -1.0 } };
		std::copy(&p[0][0], &p[0][0] + 6, &view.p[0][0]);
		double along[3], beside[3];
		for (int i = 0; i < 3; ++i)
		{
			along[i] = p[0][i] * view.way[0] + p[1][i] * view.way[1];
			beside[i] = -p[0][i] * view.way[1] + p[1][i] * view.way[0];
		}
		const double weight = std::sqrt(longWay);
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j) m[i][j] += weight * (along[i] * along[j] - beside[i] * beside[j]);
		seen.push_back(view);
		++views;
	}
	if (views < 3) return false;
	double shift = 0.0;
	for (const auto &row : m) for (double value : row) shift += std::abs(value);
	double v[3] = { 0.3, 0.8, 0.5 };
	for (int turn = 0; turn < 80; ++turn)
	{
		double next[3];
		for (int i = 0; i < 3; ++i) next[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] + shift * v[i];
		const double size = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
		if (size < 1e-12) return false;
		for (int i = 0; i < 3; ++i) v[i] = next[i] / size;
	}
	if (v[1] - 0.6 * v[0] + 0.3 * v[2] < 0.0) for (double &value : v) value = -value;
	for (int i = 0; i < 3; ++i) axis[i] = (float)v[i];
	// as long as the views that see it well across draw it
	length = 0.0f;
	for (const Seen &view : seen)
	{
		const double column = view.p[0][0] * v[0] + view.p[0][1] * v[1] + view.p[0][2] * v[2], row = view.p[1][0] * v[0] + view.p[1][1] * v[1] + view.p[1][2] * v[2];
		const double shown = std::sqrt(column * column + row * row); // (pixels a voxel of it, in this view)
		if (shown >= 0.9) length = std::max(length, (float)((view.to - view.from) / shown));
	}
	return length > 0.0f;
}

/// Where an item in hand is, from its hand frames and how it's held: pointing ahead when aimed or held in one hand,
/// across the waist when held in both, its picture's right (the magazine's side) down; false if no frame shows it.
/// An item drawn standing in the hand (see ItemPlace::upright) stands up, its picture's top up. As long as its widest
/// frame is wide, laid across the waist for both hands as a rifle is, X-Com Files' greatsword was five cubes at the
/// chest, and the arms reaching for it lay folded inside the body: no arms, no sword (a player, GitHub issue #196,
/// "Picking up many melee weapons causes amputations").
/// A shield, drawn broad from the front and thin from the sides, stands with its face ahead (see ItemPlace::plate): laid
/// as a sword is, its edge ahead, X-Com Files' combat shield was "slanted as if trying to slice with blade, rather than
/// smack with front face ... sheild is very clearly with flat face outward in handobs" (a player, GitHub issue #208).
/// A melee weapon points the way its hand frames draw it (see itemAxis), where that is not the way a gun is held: a
/// katana drawn raised ahead lay across the waist as a rifle does (a player, GitHub issue #199: "most melees are a
/// bit janked ... Works REALLY well for guns").
bool placeItem(const PartFrames &frames, bool twoHanded, bool aiming, bool melee, ItemPlace &place)
{
	if (!frames.any || !partMiddle(frames, place.middle)) return false;
	int widest = 0, tallest = 0, wide[8] = {}, filled[8] = {};
	for (int d = 0; d < 8; ++d)
	{
		int left = FrameSize, right = -1, top = FrameSize, bottom = -1, drawn = 0;
		for (int row = 0; row < FrameSize; ++row)
		{
			for (int column = 0; column < FrameSize; ++column)
			{
				if (!frames.frame[d][row * FrameSize + column]) continue;
				left = std::min(left, column);
				right = std::max(right, column);
				top = std::min(top, row);
				bottom = std::max(bottom, row);
				++drawn;
			}
		}
		widest = std::max(widest, right - left + 1);
		tallest = std::max(tallest, bottom - top + 1);
		wide[d] = std::max(0, right - left + 1);
		filled[d] = right >= left ? drawn * 100 / ((right - left + 1) * (bottom - top + 1)) : 0; // (of its box, in hundredths)
	}
	const int Back = (Front + 4) % 8;
	place.upright = tallest >= widest * ItemUpright;
	// (a plate: broad and tall from the front and the back, and solid there, most of its box drawn, not a staff with
	// sparks round it; thin from the sides; it need not be taller than wide)
	const int sideWide = std::max(wide[RightSide], wide[LeftSide]);
	place.plate = sideWide > 0 && std::min(wide[Front], wide[Back]) >= sideWide * ItemPlate && tallest >= std::min(wide[Front], wide[Back])
		&& std::min(wide[Front], wide[Back]) >= 5 * VOXELSCALE && std::min(filled[Front], filled[Back]) >= ItemPlateFilled;
	if (place.plate)
	{
		place.upright = true;
		place.length = std::max(1.0f, (float)(tallest - 1));
		// (its picture's top up, its right to the unit's left: from the front it is the picture)
		const float facing[3][3] = { { 0.0f, 0.0f, 1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
		std::copy(facing[0], facing[0] + 3, place.along);
		std::copy(facing[1], facing[1] + 3, place.across);
		std::copy(facing[2], facing[2] + 3, place.through);
		return true;
	}
	if (place.upright)
	{
		place.length = std::max(1.0f, (float)(tallest - 1));
		// (Its picture's right ahead, its edge to the front, where the side frames draw it clearly broader than the
		// front ones; else turned half way, its picture's right ahead and to the unit's left: a blade the frames draw as
		// broad from the front as from the side shows its flat to both. Edge on to the front, a knife was one cube
		// thin there: "knives being missing", a player, GitHub issue #199.)
		const float c = 0.70710678f;
		const float standing[3][3] = { { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
		const float turned[3][3] = { { 0.0f, 0.0f, 1.0f }, { -c, c, 0.0f }, { c, c, 0.0f } };
		const float (&axes)[3][3] = sideWide >= std::max(wide[Front], wide[Back]) + 2 * VOXELSCALE ? standing : turned;
		std::copy(axes[0], axes[0] + 3, place.along);
		std::copy(axes[1], axes[1] + 3, place.across);
		std::copy(axes[2], axes[2] + 3, place.through);
		place.straight = true;
		return true;
	}
	place.length = std::max(1.0f, (widest - 1) / PixelsAcross);
	const float c = (float)std::cos(ItemTilt * Pi / 180.0), s = (float)std::sin(ItemTilt * Pi / 180.0);
	const float ahead[3][3] = { { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 0.0f, 0.0f } };
	const float lying[3][3] = { { -c, 0.0f, s }, { -s, 0.0f, -c }, { 0.0f, 1.0f, 0.0f } };
	const float (&axes)[3][3] = twoHanded && !aiming ? lying : ahead;
	std::copy(axes[0], axes[0] + 3, place.along);
	std::copy(axes[1], axes[1] + 3, place.across);
	std::copy(axes[2], axes[2] + 3, place.through);
	// A melee weapon: the way its hand frames draw it, where they don't draw it held as a gun is.
	float drawn[3], drawnLength;
	if (melee && itemAxis(frames, drawn, drawnLength)
		&& std::abs(drawn[0] * place.along[0] + drawn[1] * place.along[1] + drawn[2] * place.along[2]) < ItemAxisAgree)
	{
		std::copy(drawn, drawn + 3, place.along);
		place.length = std::max(1.0f, drawnLength);
		place.straight = true;
		// (its picture's right, an edge or a hammer's head, as near down as may be; drawn pointing up or down: ahead)
		float down[3] = { drawn[2] * drawn[0], drawn[2] * drawn[1], drawn[2] * drawn[2] - 1.0f };
		float size = std::sqrt(down[0] * down[0] + down[1] * down[1] + down[2] * down[2]);
		if (size < 0.3f)
		{
			down[0] = -drawn[1] * drawn[0]; down[1] = 1.0f - drawn[1] * drawn[1]; down[2] = -drawn[1] * drawn[2];
			size = std::sqrt(down[0] * down[0] + down[1] * down[1] + down[2] * down[2]);
		}
		for (int i = 0; i < 3; ++i) place.across[i] = down[i] / size;
		place.through[0] = place.across[1] * place.along[2] - place.across[2] * place.along[1];
		place.through[1] = place.across[2] * place.along[0] - place.across[0] * place.along[2];
		place.through[2] = place.across[0] * place.along[1] - place.across[1] * place.along[0];
	}
	return true;
}

/// Where a hand holds an item: in both hands, the hand holding it near the butt and the other further along; in one,
/// near the butt and below the axis (a pistol's grip).
void gripOf(const ItemPlace &place, bool twoHanded, bool atButt, float hand[3])
{
	if (place.plate)
	{
		// (a plate: the hand behind its middle)
		for (int i = 0; i < 3; ++i) hand[i] = place.middle[i] - place.through[i] * 1.0f * VOXELSCALE;
		return;
	}
	twoHanded = twoHanded && !place.upright; // (an item standing in the hand: one hand, at its lower end, on its axis)
	const float along = place.length * ((twoHanded ? (atButt ? GripButt : GripFore) : GripOne) - 0.5f);
	const float below = twoHanded || place.upright ? 0.0f : place.length * GripBelow;
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
	// The picture's own measures: from a point on it, the way that is along the item (up the picture: the games draw an
	// item standing, a gun's muzzle at the top) and the way across it (to the picture's right), and how far it reaches
	// along and across. A long item drawn from corner to corner, where its place asks (ItemPlace::straight): along its
	// own long way, the way its pixels are spread widest, if that is ItemLong times its spread across.
	float origin[2] = { (left + right) * 0.5f, (float)bottom }, upWay[2] = { 0.0f, -1.0f }, reach = (float)(bottom - top), wideReach = (float)(right - left + 1);
	if (place.straight)
	{
		double columns = 0.0, rows = 0.0, xx = 0.0, xy = 0.0, yy = 0.0;
		int drawn = 0;
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x) if (held.picture[y * width + x]) { columns += x; rows += y; ++drawn; }
		columns /= drawn; rows /= drawn;
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
				if (held.picture[y * width + x]) { xx += (x - columns) * (x - columns); xy += (x - columns) * (y - rows); yy += (y - rows) * (y - rows); }
		const double turn = 0.5 * std::atan2(2.0 * xy, xx - yy), spread = std::sqrt((xx - yy) * (xx - yy) * 0.25 + xy * xy);
		const double longWay = (xx + yy) * 0.5 + spread, shortWay = std::max((xx + yy) * 0.5 - spread, drawn / 12.0);
		double way[2] = { std::cos(turn), std::sin(turn) };
		if (way[1] > 0.0) { way[0] = -way[0]; way[1] = -way[1]; }
		if (longWay >= ItemLong * ItemLong * shortWay && way[1] > -0.985)
		{
			double low = 1e9, high = -1e9, lowSide = 1e9, highSide = -1e9;
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
				{
					if (!held.picture[y * width + x]) continue;
					const double on = (x - columns) * way[0] + (y - rows) * way[1], beside = -(x - columns) * way[1] + (y - rows) * way[0];
					low = std::min(low, on); high = std::max(high, on); lowSide = std::min(lowSide, beside); highSide = std::max(highSide, beside);
				}
			const double sideMiddle = (lowSide + highSide) * 0.5;
			origin[0] = (float)(columns + low * way[0] - sideMiddle * way[1]);
			origin[1] = (float)(rows + low * way[1] + sideMiddle * way[0]);
			upWay[0] = (float)way[0]; upWay[1] = (float)way[1];
			reach = (float)(high - low);
			wideReach = (float)(highSide - lowSide + 1.0);
		}
	}
	const float rightWay[2] = { -upWay[1], upWay[0] };
	const float scale = place.length / std::max(1.0f, reach), thin = std::min(1.0f, scale / RifleScale);
	const float halfLength = reach * scale * 0.5f;
	// (The thickness is in the body's own measure whatever its fineness: on a fine body an item is half as thick in the
	// world as on a coarse one, slim as the owner chose it from pictures.)
	auto halfThickness = [&](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height && depth[y * width + x] ? std::min(1.4f, 0.45f + 0.35f * depth[y * width + x]) * thin : 0.0f; };
	// The item fills the cells of the body's own grid (carried on past the grid's edges: a rifle reaches out of it):
	// little cubes side by side as the body's are, no gaps between them from any side. A cell is the item's when its
	// middle, in the item's own measures (along it, across it, through it), lies on a drawn pixel within the item's
	// thickness there, or within most of a cell of the picture's plane: a thin item is a cell thick at the least.
	const float (*frame[3]) = { along, across, through };
	float inverse[3][3]; // (rows: what a place off the item's middle is along, across and through it)
	{
		const float *u = frame[0], *v = frame[1], *w = frame[2];
		const float det = u[0] * (v[1] * w[2] - v[2] * w[1]) - u[1] * (v[0] * w[2] - v[2] * w[0]) + u[2] * (v[0] * w[1] - v[1] * w[0]);
		if (std::abs(det) < 1e-6f) return;
		const float cross[3][3] = { { v[1] * w[2] - v[2] * w[1], v[2] * w[0] - v[0] * w[2], v[0] * w[1] - v[1] * w[0] },
			{ w[1] * u[2] - w[2] * u[1], w[2] * u[0] - w[0] * u[2], w[0] * u[1] - w[1] * u[0] },
			{ u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] } };
		for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) inverse[r][c] = cross[r][c] / det;
	}
	// The box of cells round the item (its picture's corners, its greatest thickness).
	const float halfAcross = wideReach * 0.5f * scale * ItemSlim, thickest = 1.4f * thin * ItemSlim + Cell;
	float low[3] = { 1e9f, 1e9f, 1e9f }, high[3] = { -1e9f, -1e9f, -1e9f };
	for (int corner = 0; corner < 8; ++corner)
	{
		const float a = (corner & 1 ? 1.0f : -1.0f) * (halfLength + scale), c = (corner & 2 ? 1.0f : -1.0f) * (halfAcross + scale), t = (corner & 4 ? 1.0f : -1.0f) * thickest;
		for (int axis = 0; axis < 3; ++axis)
		{
			const float at = middle[axis] + a * along[axis] + c * across[axis] + t * through[axis];
			low[axis] = std::min(low[axis], at); high[axis] = std::max(high[axis], at);
		}
	}
	const float from[3] = { AcrossFrom, AlongFrom, UpFrom };
	int first[3], size[3];
	for (int axis = 0; axis < 3; ++axis)
	{
		first[axis] = (int)std::floor((low[axis] - from[axis]) / Cell) - 1;
		size[axis] = (int)std::floor((high[axis] - from[axis]) / Cell) + 2 - first[axis];
		if (size[axis] <= 0 || size[axis] > 4096) return;
	}
	if ((double)size[0] * size[1] * size[2] > 8e6) return;
	std::vector<Uint8> filled((size_t)size[0] * size[1] * size[2], 0);
	auto cellAt = [&](int i, int j, int k) -> Uint8 { return i >= 0 && j >= 0 && k >= 0 && i < size[0] && j < size[1] && k < size[2] ? filled[((size_t)i * size[1] + j) * size[2] + k] : 0; };
	for (int i = 0; i < size[0]; ++i)
		for (int j = 0; j < size[1]; ++j)
			for (int k = 0; k < size[2]; ++k)
			{
				const float off[3] = { from[0] + (first[0] + i + 0.5f) * Cell - middle[0], from[1] + (first[1] + j + 0.5f) * Cell - middle[1], from[2] + (first[2] + k + 0.5f) * Cell - middle[2] };
				const float a = inverse[0][0] * off[0] + inverse[0][1] * off[1] + inverse[0][2] * off[2];
				const float c = inverse[1][0] * off[0] + inverse[1][1] * off[1] + inverse[1][2] * off[2];
				const float t = inverse[2][0] * off[0] + inverse[2][1] * off[1] + inverse[2][2] * off[2];
				const float up = (a + halfLength) / scale, beside = c / (scale * ItemSlim);
				const int x = (int)std::lround(origin[0] + up * upWay[0] + beside * rightWay[0]), y = (int)std::lround(origin[1] + up * upWay[1] + beside * rightWay[1]);
				if (x < 0 || y < 0 || x >= width || y >= height) continue;
				const Uint8 colour = held.picture[y * width + x];
				if (colour && std::abs(t) <= std::max(halfThickness(x, y) * ItemSlim, Cell * 0.6f)) filled[((size_t)i * size[1] + j) * size[2] + k] = colour;
			}
	// (its surface: the cells with an empty one beside them)
	for (int i = 0; i < size[0]; ++i)
		for (int j = 0; j < size[1]; ++j)
			for (int k = 0; k < size[2]; ++k)
			{
				const Uint8 colour = cellAt(i, j, k);
				if (!colour || (cellAt(i - 1, j, k) && cellAt(i + 1, j, k) && cellAt(i, j - 1, k) && cellAt(i, j + 1, k) && cellAt(i, j, k - 1) && cellAt(i, j, k + 1))) continue;
				model.right.push_back(from[0] + (first[0] + i + 0.5f) * Cell);
				model.ahead.push_back(from[1] + (first[1] + j + 0.5f) * Cell);
				model.up.push_back(from[2] + (first[2] + k + 0.5f) * Cell);
				model.part.push_back(VoxelUnitModel::Item);
				model.colours.insert(model.colours.end(), 16, colour);
			}
}

/// How deep inside a picture's outline each drawn pixel is: 1 at the outline, up to most (0: not drawn).
std::vector<int> insideDepth(const std::vector<Uint8> &picture, int width, int height, int most)
{
	std::vector<int> depth(width * height, 0);
	std::vector<Uint8> inside(width * height);
	for (int i = 0; i < width * height; ++i) inside[i] = picture[i] != 0;
	auto at = [&](const std::vector<Uint8> &cells, int x, int y) { return x >= 0 && y >= 0 && x < width && y < height && cells[y * width + x]; };
	for (int step = 1; step <= most; ++step)
	{
		std::vector<Uint8> deeper(width * height, 0);
		bool any = false;
		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
			{
				if (!inside[y * width + x]) continue;
				depth[y * width + x] = step;
				any |= (deeper[y * width + x] = at(inside, x - 1, y) && at(inside, x + 1, y) && at(inside, x, y - 1) && at(inside, x, y + 1));
			}
		}
		if (!any) break;
		inside.swap(deeper);
	}
	return depth;
}

/// A picture without its near black pixels' blotches (the sprites' outlines, see NearBlack): each takes a drawn
/// neighbour's colour that isn't, if it has one.
std::vector<Uint8> withoutOutline(const std::vector<Uint8> &picture, int width, int height, const SDL_Color *palette)
{
	std::vector<Uint8> out = picture;
	auto dark = [&](Uint8 colour) { return luminance(palette[colour]) < NearBlack; };
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			const Uint8 colour = picture[y * width + x];
			if (!colour || !dark(colour)) continue;
			for (int n = 0; n < 8; ++n)
			{
				static const int dx[8] = { 0, 0, -1, 1, -1, 1, -1, 1 }, dy[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };
				const int nx = x + dx[n], ny = y + dy[n];
				if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
				const Uint8 beside = picture[ny * width + nx];
				if (beside && !dark(beside)) { out[y * width + x] = beside; break; }
			}
		}
	}
	return out;
}

/// Fills a shape with the cells of the body's own grid (little cubes side by side, as the body's) in a box (low to
/// high: right, ahead, up) that colourAt gives a colour (0: empty; asked at each cell's middle); its surface kept, the
/// cells with an empty one beside them.
template <typename ColourAt>
void fillShape(VoxelUnitModel::FallShape &shape, const float low[3], const float high[3], ColourAt colourAt)
{
	const float from[3] = { AcrossFrom, AlongFrom, UpFrom };
	int first[3], size[3];
	for (int axis = 0; axis < 3; ++axis)
	{
		first[axis] = (int)std::floor((low[axis] - from[axis]) / Cell);
		size[axis] = (int)std::floor((high[axis] - from[axis]) / Cell) + 1 - first[axis];
		if (size[axis] <= 0 || size[axis] > 4096) return;
	}
	if ((double)size[0] * size[1] * size[2] > 4e7) return;
	std::vector<Uint8> filled((size_t)size[0] * size[1] * size[2], 0);
	auto cellAt = [&](int i, int j, int k) -> Uint8 { return i >= 0 && j >= 0 && k >= 0 && i < size[0] && j < size[1] && k < size[2] ? filled[((size_t)i * size[1] + j) * size[2] + k] : 0; };
	for (int i = 0; i < size[0]; ++i)
		for (int j = 0; j < size[1]; ++j)
			for (int k = 0; k < size[2]; ++k)
				filled[((size_t)i * size[1] + j) * size[2] + k] = colourAt(from[0] + (first[0] + i + 0.5f) * Cell, from[1] + (first[1] + j + 0.5f) * Cell, from[2] + (first[2] + k + 0.5f) * Cell);
	for (int i = 0; i < size[0]; ++i)
		for (int j = 0; j < size[1]; ++j)
			for (int k = 0; k < size[2]; ++k)
			{
				const Uint8 colour = cellAt(i, j, k);
				if (!colour || (cellAt(i - 1, j, k) && cellAt(i + 1, j, k) && cellAt(i, j - 1, k) && cellAt(i, j + 1, k) && cellAt(i, j, k - 1) && cellAt(i, j, k + 1))) continue;
				shape.right.push_back(from[0] + (first[0] + i + 0.5f) * Cell);
				shape.ahead.push_back(from[1] + (first[1] + j + 0.5f) * Cell);
				shape.up.push_back(from[2] + (first[2] + k + 0.5f) * Cell);
				shape.colour.push_back(colour);
			}
}

/// A picture of the unit going down or lying, drawn as the map sees it (a death frame, or its corpse on the floor: the
/// unit facing the camera, direction Front, along the isometric slant), given volume: the picture thrown along the
/// view onto a slab through the unit's feet, leaning back by an angle (0 upright, 90 flat on the floor) and, in front
/// of the feet, onto the floor (what the picture draws below the feet lies there); thicker inside the picture's outline
/// and rounded at its edges (as an item's, see addItem), up to halfMost either side, its underside on the floor as it
/// lies. Seen as the map sees it, it is the picture; from elsewhere it has the picture's volume.
void fallRelief(VoxelUnitModel::FallShape &shape, const std::vector<Uint8> &picture, float degrees, float halfMost)
{
	const std::vector<int> depth = insideDepth(picture, FrameSize, FrameSize, (int)std::ceil(halfMost) + 1);
	int left = FrameSize, right = -1, top = FrameSize, bottom = -1;
	for (int y = 0; y < FrameSize; ++y)
		for (int x = 0; x < FrameSize; ++x)
			if (picture[y * FrameSize + x]) { left = std::min(left, x); right = std::max(right, x); top = std::min(top, y); bottom = std::max(bottom, y); }
	if (right < 0) return;
	const float c = (float)std::cos(degrees * Pi / 180.0), s = (float)std::sin(degrees * Pi / 180.0);
	// The box looked through: no wider than the picture (direction Front: a column is the unit's right, turned about,
	// times PixelsAcross), and no farther ahead or behind than a point can lie that the view throws onto one of the
	// picture's rows (a row is the floor's, and RowsAhead more for each voxel ahead, less one for each up) and that is
	// within the slab or on the floor in front of the feet. (Most of the grid is neither: the whole of it was looked
	// through for every shape, the better part of a body's making.)
	const float RowsAhead = 0.70710678f; // (direction Front, read along the slant: see project)
	const float upMost = std::min((float)FloorY, std::max(0.0f, (float)(FloorY - top)) + 2.0f * halfMost + 2.0f);
	float behind = std::max(-FrameSize * 0.75f, (top - FloorY) / RowsAhead - 2.0f), before = std::min(FrameSize * 0.75f, (bottom + 1 - FloorY + upMost) / RowsAhead + 2.0f);
	if (c > 0.2f)
	{
		// (within the slab: off its middle plane by no more than twice halfMost ahead, halfMost behind; or on the floor)
		behind = std::max(behind, (-halfMost - upMost * s) / c - 2.0f);
		before = std::min(before, std::max(2.0f * halfMost / c, (bottom + 1 - FloorY + halfMost) / RowsAhead) + 2.0f);
	}
	const float low[3] = { -(right + 1 - FloorX) / PixelsAcross, behind, 0.0f };
	const float high[3] = { -(left - 1 - FloorX) / PixelsAcross, before, upMost };
	fillShape(shape, low, high, [&](float across, float along, float up) -> Uint8 {
		// (off the slab's middle plane: ahead of it upright, above it lying)
		const float off = along * c + up * s;
		const bool onFloor = along > 0.0f && up <= halfMost;
		if (!onFloor && (off > 2.0f * halfMost || off < -halfMost)) return 0;
		float column, row, nearness;
		project(across, along, up, Front, false, column, row, nearness);
		const int x = (int)std::floor(column), y = (int)std::floor(row);
		if (x < 0 || y < 0 || x >= FrameSize || y >= FrameSize) return 0;
		const Uint8 colour = picture[y * FrameSize + x];
		if (!colour) return 0;
		const float half = std::max(Cell * 0.6f, std::min(halfMost, 0.2f * VOXELSCALE + 0.8f * depth[y * FrameSize + x]));
		// (upright: half either side of the plane; lying: from the floor up; in front of the feet: on the floor)
		return (off <= half * (1.0f + s) && off >= -half * (1.0f - s)) || (onFloor && up <= half) ? colour : 0;
	});
}

/// The body lying: its corpse's inventory picture laid on the floor (the picture's top away behind the unit, where
/// its head came down), as long as length, given volume as an item's: thicker inside the picture's outline, rounded at
/// its edges, up to highMost off the floor.
void lyingShape(VoxelUnitModel::FallShape &shape, const HeldItem &lying, float length, float highMost)
{
	const int width = lying.width, height = lying.height;
	int top = height, bottom = -1, left = width, right = -1;
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
			if (lying.picture[y * width + x]) { top = std::min(top, y); bottom = std::max(bottom, y); left = std::min(left, x); right = std::max(right, x); }
	if (bottom < 0) return;
	const float scale = length / std::max(4, std::max(bottom - top, right - left) + 1);
	const float middleX = (left + right + 1) * 0.5f, middleY = (top + bottom + 1) * 0.5f;
	const std::vector<int> depth = insideDepth(lying.picture, width, height, (int)std::ceil(highMost / scale) + 1);
	const float low[3] = { (left - middleX) * scale, (top - middleY) * scale, 0.0f };
	const float high[3] = { (right + 1 - middleX) * scale, (bottom + 1 - middleY) * scale, highMost };
	fillShape(shape, low, high, [&](float across, float along, float up) -> Uint8 {
		const int x = (int)std::floor(middleX - across / scale), y = (int)std::floor(middleY + along / scale);
		if (x < 0 || y < 0 || x >= width || y >= height || up < 0.0f) return 0;
		const Uint8 colour = lying.picture[y * width + x];
		if (!colour) return 0;
		return up <= std::max(Cell * 1.2f, std::min(highMost, (depth[y * width + x] - 0.3f) * scale * LyingRound)) ? colour : 0;
	});
}

/// A wreck takes its picture's colours this deep under the skin the picture shows, voxels (see makeFalls).
constexpr float WreckSkinDepth = 1.5f * VOXELSCALE;
/// The fall a body takes: a shape for each of its death frames in turn (each frame's own picture given volume, upright
/// while the picture stands tall, laid back as it gets low: the body goes down as its sprite does, not as a plank
/// tipping over), then the body
/// lying: its corpse's inventory picture laid on the floor and given volume (or, without one, its corpse's picture on
/// the map's floor). The owner: "deaths should not be like planks! The body should organically collapse like in
/// sprites - and the death should be like a dead body sprite (in equipment menu - just voxelised to give volume!)".
void makeFalls(VoxelUnitModel &model, const std::vector<std::vector<Uint8>> &deaths, const std::vector<Uint8> &corpse, const HeldItem &lying, const SDL_Color *palette, bool big, bool hull)
{
	// The standing body's height and depth: the fall's shapes are as thick as it stood deep, and it lies as long as it
	// stood tall (not much longer than its tile: it lies in the tile's middle, not through the walls round it).
	float top = 0.0f, back = 1e9f, fore = -1e9f;
	for (size_t k = 0; k < std::min(model.part.size(), model.addedFrom); ++k)
	{
		if (model.part[k] == VoxelUnitModel::Item) continue;
		top = std::max(top, model.up[k]);
		if (model.part[k] == VoxelUnitModel::Torso) { back = std::min(back, model.ahead[k]); fore = std::max(fore, model.ahead[k]); }
	}
	const float halfMost = std::clamp((fore - back) * 0.5f, 1.5f * VOXELSCALE, 3.5f * VOXELSCALE);
	const float length = std::min(top, LyingLongest * VOXELSCALE);
	// How tall a frame's picture stands, of the standing height.
	auto tallOf = [&](const std::vector<Uint8> &death) {
		int topRow = FrameSize;
		for (int i = 0; i < FrameSize * FrameSize && topRow == FrameSize; ++i) if (death[i]) topRow = i / FrameSize;
		return top > 0.0f ? (FloorY - topRow) / top : 1.0f;
	};
	// (a "death" that ends standing isn't a fall: the Zombie's, a Chryssalid rising out of it; its shapes stay upright)
	const bool goesDown = !deaths.empty() && tallOf(deaths.back()) < FallUpright;
	float lean = 0.0f;
	for (size_t frame = 0; frame < deaths.size(); ++frame)
	{
		const auto &death = deaths[frame];
		// How far back the frame's slab leans: a little more each frame (the last frames draw the body on the floor, as
		// the map sees it), and more once its picture has come low; never back up.
		if (goesDown) lean = std::max({ lean, FallMostLean * std::clamp((FallUpright - tallOf(death)) / (FallUpright - FallFlat), 0.0f, 1.0f), FallLastLean * (frame + 1) / deaths.size() });
		VoxelUnitModel::FallShape shape;
		fallRelief(shape, withoutOutline(death, FrameSize, FrameSize, palette), lean, halfMost);
		if (!shape.colour.empty()) model.falls.push_back(std::move(shape));
	}
	VoxelUnitModel::FallShape down;
	if (!lying.picture.empty())
	{
		HeldItem clean = lying;
		clean.picture = withoutOutline(lying.picture, lying.width, lying.height, palette);
		lyingShape(down, clean, length, halfMost * 1.6f);
	}
	// A big unit's wreck is its own body (the owner: "It should be the same cyberdisc with a hole on top sitting on the
	// ground", "They should reuse the body and compare with downed sprite. General shape should stay the same"): set
	// down on the ground (a hull that hovered: down to its widest row, what its drawings' outlines left under the rim
	// being under the ground then), made as much lower as its wreck's picture stands lower than its own, and painted as
	// that picture shows it from the map's side: its dents and holes on the body it had, the body's own colours where
	// the picture has none.
	if (big && !corpse.empty())
	{
		const size_t own = std::min(model.part.size(), model.addedFrom);
		std::vector<int> wide(GridUp, 0);
		float standsFrom = 1e9f, highest = 0.0f; // (the top row of its own picture from the map's side, and its top)
		for (size_t k = 0; k < own; ++k)
		{
			if (model.part[k] == VoxelUnitModel::Item) continue;
			const int level = (int)std::floor((model.up[k] - UpFrom) / Cell);
			if (level >= 0 && level < GridUp) ++wide[level];
			float column, row, nearness;
			project(model.right[k], model.ahead[k], model.up[k], Front, false, column, row, nearness);
			standsFrom = std::min(standsFrom, row);
			highest = std::max(highest, model.up[k]);
		}
		const int rim = (int)(std::max_element(wide.begin(), wide.end()) - wide.begin());
		const float drop = hull ? std::max(0.0f, std::floor((cellUp(rim) - 1.0f * VOXELSCALE) / Cell) * Cell) : 0.0f; // (whole cells: the cubes stay on the grid)
		int wreckFrom = FrameSize;
		for (int i = 0; i < FrameSize * FrameSize && wreckFrom == FrameSize; ++i) if (corpse[i]) wreckFrom = i / FrameSize;
		const float tall = std::max(1.0f, highest - drop), lost = std::max(0.0f, wreckFrom - standsFrom - drop);
		const float squash = std::clamp((tall - lost) / tall, WreckLowest, 1.0f);
		std::unordered_set<long long> taken; // (two cubes pressed into one cell are one)
		for (size_t k = 0; k < own; ++k)
		{
			if (model.part[k] == VoxelUnitModel::Item || model.up[k] - drop < 0.0f) continue;
			const float up = (std::floor((model.up[k] - drop) * squash / Cell) + 0.5f) * Cell;
			const long long cell = ((long long)std::lround(model.right[k] / Cell * 2.0f) + 100000) * 1000000000000LL
				+ ((long long)std::lround(model.ahead[k] / Cell * 2.0f) + 100000) * 1000000LL + (long long)std::lround(up / Cell * 2.0f) + 100000;
			if (!taken.insert(cell).second) continue;
			down.right.push_back(model.right[k]);
			down.ahead.push_back(model.ahead[k]);
			down.up.push_back(up);
			down.colour.push_back(model.colours[k * 16]);
		}
		// (The picture only on the skin it shows: the wreck's nearest cubes on each of its pixels, from the map's side.
		// Thrown through the whole wreck, each pixel ran on down the far side and along the sides as a streak: the
		// owner's photo of a downed Cyberdisc, "vertical stripes". The rest keeps the body's own colours.)
		std::vector<float> skin((size_t)FrameSize * FrameSize + 1, -1e9f);
		auto pixelOf = [&](size_t k, float &nearness) -> size_t {
			float column, row;
			project(down.right[k], down.ahead[k], down.up[k], Front, false, column, row, nearness);
			const int c = (int)std::floor(column), r = (int)std::floor(row);
			return c >= 0 && r >= 0 && c < FrameSize && r < FrameSize ? (size_t)r * FrameSize + c : (size_t)FrameSize * FrameSize;
		};
		for (size_t k = 0; k < down.colour.size(); ++k)
		{
			float nearness;
			float &nearest = skin[pixelOf(k, nearness)];
			nearest = std::max(nearest, nearness);
		}
		for (size_t k = 0; k < down.colour.size(); ++k)
		{
			float nearness;
			const size_t pixel = pixelOf(k, nearness);
			if (pixel < (size_t)FrameSize * FrameSize && corpse[pixel] && nearness >= skin[pixel] - WreckSkinDepth) down.colour[k] = corpse[pixel];
		}
	}
	if (down.colour.empty() && !corpse.empty()) fallRelief(down, withoutOutline(corpse, FrameSize, FrameSize, palette), 90.0f, halfMost * 0.8f);
	if (!down.colour.empty()) model.falls.push_back(std::move(down));
	// What a shot meets of each (a unit going down is hit as it shows): the shape's cubes, in whole voxels.
	for (VoxelUnitModel::FallShape &shape : model.falls)
	{
		const std::vector<float> *place[3] = { &shape.right, &shape.ahead, &shape.up };
		int high[3];
		for (int i = 0; i < 3; ++i)
		{
			const auto ends = std::minmax_element(place[i]->begin(), place[i]->end());
			shape.hitFrom[i] = (int)std::floor(*ends.first);
			high[i] = (int)std::floor(*ends.second);
			shape.hitSize[i] = high[i] - shape.hitFrom[i] + 1;
		}
		shape.hits.assign((size_t)shape.hitSize[0] * shape.hitSize[1] * shape.hitSize[2], 0);
		for (size_t k = 0; k < shape.colour.size(); ++k)
		{
			const int x = (int)std::floor(shape.right[k]) - shape.hitFrom[0], y = (int)std::floor(shape.ahead[k]) - shape.hitFrom[1], z = (int)std::floor(shape.up[k]) - shape.hitFrom[2];
			shape.hits[((size_t)x * shape.hitSize[1] + y) * shape.hitSize[2] + z] = 1;
		}
	}
}

/// The part each frame pixel shows, as the game lays the parts over each other (composite: its colour there is the top
/// part's), for each direction; for a unit without one (drawn on one frame, a big unit's tiles), the first drawn there of
/// the items, the arms, the torso and the legs. -1 where nothing is drawn.
void partsOnTop(const PartFrames parts[PartCount], const std::vector<Uint8> composite[8], std::vector<int> onTop[8])
{
	const int preference[PartCount] = { RightItem, LeftItem, 2, 1, 0, 3 }; // (2 the right arm, 1 the left, 0 the torso, 3 the legs)
	for (int d = 0; d < 8; ++d)
	{
		onTop[d].assign(FrameSize * FrameSize, -1);
		for (int i = 0; i < FrameSize * FrameSize; ++i)
		{
			const Uint8 shown = composite[d].empty() ? 0 : composite[d][i];
			for (int p : preference)
			{
				if (!parts[p].any || !parts[p].frame[d][i]) continue;
				if (onTop[d][i] < 0) onTop[d][i] = p;
				if (shown && parts[p].frame[d][i] == shown)
				{
					onTop[d][i] = p;
					break;
				}
			}
		}
	}
}

/// A point on the line of sight of a frame's pixel (column, row: a place on the frame) in direction d's view, at a
/// nearness to the camera (see project): to the body's right, ahead and up. Level: the frame read as a picture taken
/// at the unit's height; else along the isometric view's slant.
inline void unproject(float column, float row, float nearness, int d, bool level, float &right, float &ahead, float &up)
{
	// (x - y and x + y on the map, and the height, from the projection)
	const float difference = column - FloorX;
	level = level && !SlantOnly;
	const float sum = level ? nearness : (nearness + (row - FloorY)) / 1.5f;
	const float x = (sum + difference) * 0.5f, y = (sum - difference) * 0.5f;
	up = level ? FloorY + tileFloorRows(x, y) - row : sum * 0.5f - (row - FloorY);
	right = x * Turns[d].cosine + y * Turns[d].sine;
	ahead = x * Turns[d].sine - y * Turns[d].cosine;
}

/// The colour slot (and the bit of VoxelUnitModel::views) of a view: direction d, level or slanted.
inline int viewSlot(int d, bool level)
{
	return (level ? 0 : 8) + d;
}

/// Whether a frame pixel is drawn (some part shows there; see partsOnTop): column, row a place on direction d's frame.
inline bool drawnAt(const std::vector<int> onTop[8], int d, float column, float row)
{
	const int c = (int)std::floor(column), r = (int)std::floor(row);
	return c >= 0 && r >= 0 && c < FrameSize && r < FrameSize && onTop[d][r * FrameSize + c] >= 0;
}

/// The views (bits as VoxelUnitModel::views) whose drawing a place of the body falls inside: right, ahead, up.
Uint16 viewsAt(const std::vector<int> onTop[8], float right, float ahead, float up)
{
	Uint16 views = 0;
	for (int reading = 0; reading < 2; ++reading)
	{
		for (int d = 0; d < 8; ++d)
		{
			float column, row, nearness;
			project(right, ahead, up, d, reading == 1, column, row, nearness);
			if (drawnAt(onTop, d, column, row)) views |= (Uint16)(1u << viewSlot(d, reading == 1));
		}
	}
	return views;
}

/// What a shot meets (see VoxelUnitModel::hits): the body's solid cells (owner: its filled half voxel cells, the inside
/// too), the items in its hands and the pieces added for views, in whole voxels; each the views it shows in (a cell's,
/// as its middle falls inside each view's solid drawing, see matchViews; a voxel's, as it shows). Nothing floats here
/// either: in each view, only the cells joined (within Joined cells, as dropFloating joins them) to a voxel that still
/// shows there are met; a speck hidden from the drawing isn't hit.
void makeHits(VoxelUnitModel &model, const std::vector<Uint8> &owner, const std::vector<int> solidOnTop[8])
{
	float low[3] = { 1e9f, 1e9f, 1e9f }, high[3] = { -1e9f, -1e9f, -1e9f };
	auto grow = [&](float right, float ahead, float up) {
		const float place[3] = { right, ahead, up };
		for (int i = 0; i < 3; ++i)
		{
			low[i] = std::min(low[i], place[i]);
			high[i] = std::max(high[i], place[i]);
		}
	};
	for (int across = 0; across < GridAcross; ++across)
		for (int along = 0; along < GridAlong; ++along)
			for (int up = 0; up < GridUp; ++up)
				if (owner[cellIndex(across, along, up)]) grow(cellRight(across), cellAhead(along), cellUp(up));
	for (size_t k = 0; k < model.part.size(); ++k) grow(model.right[k], model.ahead[k], model.up[k]);
	if (low[0] > high[0]) return;
	for (int i = 0; i < 3; ++i)
	{
		model.hitFrom[i] = (int)std::floor(low[i]);
		model.hitSize[i] = (int)std::floor(high[i]) - model.hitFrom[i] + 1;
	}
	model.hits.assign((size_t)model.hitSize[0] * model.hitSize[1] * model.hitSize[2], 0);
	auto at = [&](float right, float ahead, float up) -> Uint16 & {
		const int x = (int)std::floor(right) - model.hitFrom[0], y = (int)std::floor(ahead) - model.hitFrom[1], z = (int)std::floor(up) - model.hitFrom[2];
		return model.hits[((size_t)x * model.hitSize[1] + y) * model.hitSize[2] + z];
	};
	// Each solid cell's views, and the views in which a voxel that shows (not an item) lies in it.
	std::vector<Uint16> solid(GridCells, 0), shown(GridCells, 0), met(GridCells, 0);
	std::vector<int> cells; // (the solid cells)
	for (int across = 0; across < GridAcross; ++across)
		for (int along = 0; along < GridAlong; ++along)
			for (int up = 0; up < GridUp; ++up)
			{
				const int cell = cellIndex(across, along, up);
				if (!owner[cell]) continue;
				solid[cell] = OneBody ? (Uint16)0xFFFF : viewsAt(solidOnTop, cellRight(across), cellAhead(along), cellUp(up));
				if (solid[cell]) cells.push_back(cell);
			}
	// (One body, see OneBody, is one piece already, whole from every side: nothing of it floats, and looking for what
	// does through every solid cell, for each of the 16 views, was most of a body's making.)
	const bool matched = !OneBody && model.views.size() == model.part.size();
	for (size_t k = 0; matched && k < model.part.size(); ++k)
	{
		if (model.part[k] == VoxelUnitModel::Item) continue;
		const int across = (int)std::floor((model.right[k] - AcrossFrom) / Cell), along = (int)std::floor((model.ahead[k] - AlongFrom) / Cell),
			up = (int)std::floor((model.up[k] - UpFrom) / Cell);
		if (across >= 0 && along >= 0 && up >= 0 && across < GridAcross && along < GridAlong && up < GridUp) shown[cellIndex(across, along, up)] |= model.views[k];
	}
	std::vector<int> stack;
	if (OneBody) met = solid;
	for (int view = 0; view < 16 && !OneBody; ++view)
	{
		const Uint16 bit = (Uint16)(1u << view);
		for (int cell : cells) if ((solid[cell] & bit) && (shown[cell] & bit)) { met[cell] |= bit; stack.push_back(cell); }
		if (stack.empty())
		{
			for (int cell : cells) met[cell] |= solid[cell] & bit; // (a body unmatched, or nothing of it showing there: as it's solid)
			continue;
		}
		while (!stack.empty())
		{
			const int cell = stack.back(), up = cell % GridUp, along = (cell / GridUp) % GridAlong, across = cell / (GridUp * GridAlong);
			stack.pop_back();
			for (int a = std::max(0, across - Joined); a <= std::min(GridAcross - 1, across + Joined); ++a)
				for (int b = std::max(0, along - Joined); b <= std::min(GridAlong - 1, along + Joined); ++b)
					for (int c = std::max(0, up - Joined); c <= std::min(GridUp - 1, up + Joined); ++c)
					{
						const int other = cellIndex(a, b, c);
						if ((solid[other] & bit) && !(met[other] & bit)) { met[other] |= bit; stack.push_back(other); }
					}
		}
	}
	for (int cell : cells)
	{
		const int up = cell % GridUp, along = (cell / GridUp) % GridAlong, across = cell / (GridUp * GridAlong);
		at(cellRight(across), cellAhead(along), cellUp(up)) |= met[cell];
	}
	for (size_t k = 0; k < model.part.size(); ++k) at(model.right[k], model.ahead[k], model.up[k]) |= model.views[k];
}

/// The body matched to its drawings view by view (the owner's choice: "view-matched"). A hand drawn unit's 8 frames don't
/// quite agree on one shape (a claw, a tail, a crest drawn a little differently in each), so seen from one of the 8
/// directions the body shows only its voxels inside that direction's drawing (along the slant for a camera above, level
/// for one at the unit's height: see VoxelUnitModel::views), and what the drawing has that they leave bare is added for
/// that direction: each pixel of it on its line of sight, at the depth where the most other views (their neighbours the
/// most) have it inside their drawing and the fewest off the unit (outside their drawing's row), as near the body's own
/// surface as that allows (BodyPull: a claw stays on the arm it's drawn on, a hover field's sparks under the disc); the
/// depths then evened out over each connected piece of it (each the middle one of its 3x3 neighbours' in the piece: the
/// piece follows the body's surface, not one flat layer); four voxels to a pixel, of the part on top there, coloured as
/// the drawing. A piece shows from its own direction only (level and slanted): seen from another it lies along that
/// view's line of sight, and seen from beside that, it would stretch into a streak. A pixel whose line of sight never
/// enters the body's space (a big unit's lowest rows, read level on their tile's floor: below the ground) gets none. A
/// big unit's own voxels show where its solid drawing is (solidOnTop: without the fine detail, see keepSolid): the
/// sparks of a hover field below a disc are the views' pieces, not the disc's shape seen there.
void matchViews(VoxelUnitModel &model, const PartFrames parts[PartCount], const std::vector<int> onTop[8], const std::vector<int> solidOnTop[8])
{
	const float half = AcrossFrom < 0.0f ? -AcrossFrom : 14.0f, top = UpFrom + GridUp * Cell;
	auto drawn = [&](int d, float column, float row) { return drawnAt(onTop, d, column, row); };
	// The views a voxel's place falls inside the drawing of, and its colour in each: the part on top's there.
	auto fits = [&](float right, float ahead, float up, Uint8 colours[16]) {
		for (int reading = 0; reading < 2; ++reading)
		{
			for (int d = 0; d < 8; ++d)
			{
				float column, row, nearness;
				project(right, ahead, up, d, reading == 1, column, row, nearness);
				if (!drawn(d, column, row)) continue;
				const int pixel = (int)std::floor(row) * FrameSize + (int)std::floor(column);
				colours[viewSlot(d, reading == 1)] = parts[onTop[d][pixel]].frame[d][pixel];
			}
		}
		return viewsAt(onTop, right, ahead, up);
	};
	std::unordered_set<long long> pieceCells; // (the cells the views' pieces fill, with the view: see the end)
	const size_t own = model.part.size();
	model.views.resize(own);
	for (size_t k = 0; k < own; ++k) model.views[k] = viewsAt(solidOnTop, model.right[k], model.ahead[k], model.up[k]);
	model.addedFrom = own;
	// Each direction's drawing's rows, from their first drawn pixel to their last: a place outside its row is off the unit.
	int rowFirst[8][FrameSize], rowLast[8][FrameSize];
	for (int d = 0; d < 8; ++d)
	{
		for (int row = 0; row < FrameSize; ++row)
		{
			rowFirst[d][row] = FrameSize;
			rowLast[d][row] = -1;
			for (int column = 0; column < FrameSize; ++column)
			{
				if (onTop[d][row * FrameSize + column] < 0) continue;
				rowFirst[d][row] = std::min(rowFirst[d][row], column);
				rowLast[d][row] = column;
			}
		}
	}
	auto offUnit = [&](int d, float column, float row) {
		const int c = (int)std::floor(column), r = (int)std::floor(row);
		return r < 0 || r >= FrameSize || c < rowFirst[d][r] || c > rowLast[d][r];
	};
	// How far each cell of the grid is from the body's own surface: steps along the grid's axes (from its surface voxels'
	// cells outwards), up to FarSteps.
	auto cellOf = [&](float right, float ahead, float up) {
		const int across = (int)std::floor((right - AcrossFrom) / Cell), along = (int)std::floor((ahead - AlongFrom) / Cell);
		const int height = (int)std::floor((up - UpFrom) / Cell);
		return across < 0 || along < 0 || height < 0 || across >= GridAcross || along >= GridAlong || height >= GridUp ? -1 : cellIndex(across, along, height);
	};
	std::vector<Uint8> away(GridCells, FarSteps);
	std::vector<int> wave;
	for (size_t k = 0; k < own; ++k)
	{
		const int cell = cellOf(model.right[k], model.ahead[k], model.up[k]);
		if (cell < 0 || !away[cell]) continue;
		away[cell] = 0;
		wave.push_back(cell);
	}
	for (size_t next = 0; next < wave.size(); ++next)
	{
		const int cell = wave[next], height = cell % GridUp, along = (cell / GridUp) % GridAlong, across = cell / (GridUp * GridAlong);
		if (away[cell] + 1 >= FarSteps) continue;
		const int around[6][3] = { { across - 1, along, height }, { across + 1, along, height }, { across, along - 1, height }, { across, along + 1, height },
			{ across, along, height - 1 }, { across, along, height + 1 } };
		for (const auto &place : around)
		{
			if (place[0] < 0 || place[1] < 0 || place[2] < 0 || place[0] >= GridAcross || place[1] >= GridAlong || place[2] >= GridUp) continue;
			Uint8 &there = away[cellIndex(place[0], place[1], place[2])];
			if (there <= away[cell] + 1) continue;
			there = (Uint8)(away[cell] + 1);
			wave.push_back(cellIndex(place[0], place[1], place[2]));
		}
	}
	constexpr float NoPlace = -1e9f;
	std::vector<Uint8> bare(FrameSize * FrameSize), inPiece(FrameSize * FrameSize, 0);
	std::vector<float> depth(FrameSize * FrameSize, NoPlace), evened;
	std::vector<int> piece;
	for (int reading = 0; reading < 2; ++reading)
	{
		const bool level = reading == 1;
		for (int d = 0; d < 8; ++d)
		{
			const int slot = viewSlot(d, level);
			const Uint16 ownDirection = (Uint16)((1u << viewSlot(d, true)) | (1u << viewSlot(d, false)));
			// the drawing's pixels no voxel shown in this view covers
			for (int i = 0; i < FrameSize * FrameSize; ++i) bare[i] = onTop[d][i] >= 0 && onTop[d][i] < BodyParts; // (the body's drawing: the items are their own models)
			for (size_t k = 0; k < model.part.size(); ++k)
			{
				if (!(model.views[k] >> slot & 1)) continue;
				float column, row, nearness;
				project(model.right[k], model.ahead[k], model.up[k], d, level, column, row, nearness);
				const int c = (int)std::floor(column), r = (int)std::floor(row);
				if (c >= 0 && r >= 0 && c < FrameSize && r < FrameSize) bare[r * FrameSize + c] = 0;
			}
			// each one's depth on its line of sight: where the other views agree most it's on the unit, near the body's surface
			for (int pixel = 0; pixel < FrameSize * FrameSize; ++pixel)
			{
				depth[pixel] = NoPlace;
				if (!bare[pixel]) continue;
				float best = -1e30f;
				for (float nearness = -26.0f * VOXELSCALE; nearness <= 26.0f * VOXELSCALE; nearness += 0.5f)
				{
					float right, ahead, up;
					unproject(pixel % FrameSize + 0.5f, pixel / FrameSize + 0.5f, nearness, d, level, right, ahead, up);
					if (std::abs(right) > half || std::abs(ahead) > half || up < UpFrom || up > top) continue;
					const int cell = cellOf(right, ahead, up);
					float score = -BodyPull * (cell < 0 ? FarSteps : away[cell]) * Cell - 0.01f * planeLength(right, ahead); // (alike: nearest the middle)
					for (int e = 0; e < 8; ++e)
					{
						if (e == d) continue;
						float column, row, other;
						project(right, ahead, up, e, level, column, row, other);
						const float weight = e == (d + 1) % 8 || e == (d + 7) % 8 ? 2.0f : 1.0f;
						if (drawn(e, column, row)) score += weight;
						else if (offUnit(e, column, row)) score -= weight;
					}
					if (score > best)
					{
						best = score;
						depth[pixel] = nearness;
					}
				}
			}
			// in connected pieces, the depths evened out, and the pieces added
			for (int start = 0; start < FrameSize * FrameSize; ++start)
			{
				if (!bare[start]) continue;
				piece.assign(1, start);
				bare[start] = 0;
				for (size_t next = 0; next < piece.size(); ++next)
				{
					const int column = piece[next] % FrameSize, row = piece[next] / FrameSize;
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx)
						{
							const int c = column + dx, r = row + dy;
							if (c < 0 || r < 0 || c >= FrameSize || r >= FrameSize || !bare[r * FrameSize + c]) continue;
							bare[r * FrameSize + c] = 0;
							piece.push_back(r * FrameSize + c);
						}
				}
				for (int pixel : piece) inPiece[pixel] = 1;
				evened.assign(piece.size(), NoPlace);
				for (size_t i = 0; i < piece.size(); ++i)
				{
					if (depth[piece[i]] == NoPlace) continue;
					float around[9];
					int many = 0;
					const int column = piece[i] % FrameSize, row = piece[i] / FrameSize;
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx)
						{
							const int c = column + dx, r = row + dy;
							if (c < 0 || r < 0 || c >= FrameSize || r >= FrameSize || !inPiece[r * FrameSize + c] || depth[r * FrameSize + c] == NoPlace) continue;
							around[many++] = depth[r * FrameSize + c];
						}
					std::nth_element(around, around + many / 2, around + many);
					evened[i] = around[many / 2];
				}
				for (int pixel : piece) inPiece[pixel] = 0;
				for (size_t i = 0; i < piece.size(); ++i)
				{
					if (evened[i] == NoPlace) continue;
					const int pixel = piece[i], part = onTop[d][pixel];
					for (int quarter = 0; quarter < 4; ++quarter)
					{
						// (four to a pixel, at its quarters' middles: nearer each other than a voxel's square is wide, no gaps up close)
						float right, ahead, up;
						unproject(pixel % FrameSize + 0.25f + 0.5f * (quarter % 2), pixel / FrameSize + 0.25f + 0.5f * (quarter / 2), evened[i], d, level,
							right, ahead, up);
						// (in the cell of the body's grid it falls in, a cube among the body's cubes; once for a view)
						const int cellAcross = (int)std::floor((right - AcrossFrom) / Cell), cellAlong = (int)std::floor((ahead - AlongFrom) / Cell),
							cellHigh = (int)std::floor((up - UpFrom) / Cell);
						if (!pieceCells.insert((((long long)(cellAcross + 4096) << 26 | (long long)(cellAlong + 4096) << 13 | (long long)(cellHigh + 4096)) << 5) | slot).second) continue;
						right = cellRight(cellAcross); ahead = cellAhead(cellAlong); up = cellUp(cellHigh);
						Uint8 colours[16];
						std::fill(colours, colours + 16, parts[part].frame[d][pixel]);
						const Uint16 views = (fits(right, ahead, up, colours) | (Uint16)(1u << slot)) & ownDirection;
						colours[slot] = parts[part].frame[d][pixel];
						model.right.push_back(right);
						model.ahead.push_back(ahead);
						model.up.push_back(up);
						model.part.push_back(PartCodes[part]);
						model.colours.insert(model.colours.end(), colours, colours + 16);
						model.views.push_back(views);
					}
				}
			}
		}
	}
}

}

/**
 * The routine's way of drawing (see Routine).
 * @param number The drawing routine's number.
 * @return Its way, or null for one not given a voxel body.
 */
const Routine *VoxelBodies::routineFor(int number)
{
	for (const Routine &routine : Routines) if (routine.number == number) return &routine;
	return nullptr;
}

/// How far from the body's own voxels a view's added piece may lie (see dropFarPieces), voxels.
const float PieceReach = 1.0f * VOXELSCALE;

/// Hides the views' added pieces (see matchViews) that lie farther than PieceReach from the body's own voxels. A piece
/// fills what one direction's drawing has and the body leaves bare: a toe, a claw's tip, the rim of a muscle. Close to
/// the body it's part of its skin from any side; one standing off it reads, from a little beside its own direction, as a
/// limb too many (the owner on a Muton: "What is that between the legs? Third leg?").
static void dropFarPieces(VoxelUnitModel &model, size_t bodyEnd)
{
	if (model.views.size() != model.part.size()) return;
	std::vector<Uint8> own(GridCells, 0);
	auto cell = [](float at, float from) { return (int)std::floor((at - from) / Cell); };
	for (size_t k = 0; k < bodyEnd && k < model.part.size(); ++k)
	{
		const int across = cell(model.right[k], AcrossFrom), along = cell(model.ahead[k], AlongFrom), up = cell(model.up[k], UpFrom);
		if (across >= 0 && along >= 0 && up >= 0 && across < GridAcross && along < GridAlong && up < GridUp) own[cellIndex(across, along, up)] = 1;
	}
	const int reach = (int)std::ceil(PieceReach / Cell);
	for (size_t k = model.addedFrom; k < model.part.size(); ++k)
	{
		if (model.part[k] == VoxelUnitModel::Item) continue;
		const int across = cell(model.right[k], AcrossFrom), along = cell(model.ahead[k], AlongFrom), up = cell(model.up[k], UpFrom);
		bool near = false;
		for (int a = std::max(0, across - reach); a <= std::min(GridAcross - 1, across + reach) && !near; ++a)
			for (int b = std::max(0, along - reach); b <= std::min(GridAlong - 1, along + reach) && !near; ++b)
				for (int c = std::max(0, up - reach); c <= std::min(GridUp - 1, up + reach) && !near; ++c)
				{
					if (!own[cellIndex(a, b, c)]) continue;
					const float dr = cellRight(a) - model.right[k], da = cellAhead(b) - model.ahead[k], du = cellUp(c) - model.up[k];
					near = dr * dr + da * da + du * du <= PieceReach * PieceReach;
				}
		if (!near) model.views[k] = 0;
	}
}

/// Makes a unit's body from its parts' frames (and, for a unit drawn in parts, the parts laid over each other as the game
/// draws them, each direction's: which part shows where), the items in its hands (right, left), its death frames, its
/// corpse's picture and its walk frames; null if it has no torso.
/// Nothing floats: in each view, of the voxels that show there (see VoxelUnitModel::views) only the biggest piece
/// stays in sight, and what stands apart from it in that view is hidden there (a view's added piece left hanging beside
/// the body, a speck). Voxels within Joined cells of each other are one piece; the items in hand always show.
static void dropFloating(VoxelUnitModel &model)
{
	const size_t count = model.part.size();
	if (model.views.size() != count) return;
	std::vector<int> cellOf(count, -1);
	for (size_t k = 0; k < count; ++k)
	{
		const int across = (int)std::floor((model.right[k] - AcrossFrom) / Cell), along = (int)std::floor((model.ahead[k] - AlongFrom) / Cell),
			up = (int)std::floor((model.up[k] - UpFrom) / Cell);
		if (across >= 0 && along >= 0 && up >= 0 && across < GridAcross && along < GridAlong && up < GridUp) cellOf[k] = cellIndex(across, along, up);
		else if (model.part[k] != VoxelUnitModel::Item) model.views[k] = 0; // (off the body's grid altogether: apart from it)
	}
	std::vector<int> firstAt(GridCells, -1), nextAt(count, -1); // (the voxels in each cell, chained)
	std::vector<int> piece(count), stack, sizes;
	for (int view = 0; view < 16; ++view)
	{
		std::fill(firstAt.begin(), firstAt.end(), -1);
		for (size_t k = 0; k < count; ++k)
		{
			piece[k] = -1;
			if (cellOf[k] < 0 || !(model.views[k] >> view & 1) || model.part[k] == VoxelUnitModel::Item) continue;
			nextAt[k] = firstAt[cellOf[k]];
			firstAt[cellOf[k]] = (int)k;
			piece[k] = -2; // (shown, not yet in a piece)
		}
		sizes.clear();
		for (size_t start = 0; start < count; ++start)
		{
			if (piece[start] != -2) continue;
			const int label = (int)sizes.size();
			sizes.push_back(0);
			piece[start] = label;
			stack.push_back((int)start);
			while (!stack.empty())
			{
				const int k = stack.back();
				stack.pop_back();
				++sizes[label];
				const int cell = cellOf[k], up = cell % GridUp, along = (cell / GridUp) % GridAlong, across = cell / (GridUp * GridAlong);
				for (int a = std::max(0, across - Joined); a <= std::min(GridAcross - 1, across + Joined); ++a)
					for (int b = std::max(0, along - Joined); b <= std::min(GridAlong - 1, along + Joined); ++b)
						for (int c = std::max(0, up - Joined); c <= std::min(GridUp - 1, up + Joined); ++c)
							for (int other = firstAt[cellIndex(a, b, c)]; other >= 0; other = nextAt[other])
								if (piece[other] == -2) { piece[other] = label; stack.push_back(other); }
			}
		}
		if (sizes.size() < 2) continue;
		const int biggest = (int)(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
		for (size_t k = 0; k < count; ++k) if (piece[k] >= 0 && piece[k] != biggest) model.views[k] &= (Uint16)~(1u << view);
	}
}

/// A relief body is at most this thick either side of its drawing's plane, voxels; and this much of how deep inside
/// the drawing's outline a pixel is (a round blob of the drawing comes out about as deep as it is wide).
constexpr float ReliefHalfMost = 4.5f * VOXELSCALE, ReliefRound = 0.7f;
/// The body of a unit its sprite draws the same from every side, and lopsided (see Routine::relief): the one drawing,
/// standing upright through the middle of its feet and facing the way the unit does, given volume as a fall's shapes
/// are (see fallRelief): thicker inside the drawing's outline, rounded at its edges. A cube takes its pixel's colour,
/// the same from every side. (Carved from its eight drawings, all the same, such a body keeps only what the drawing
/// has on both sides of its middle: X-Com Files' Abomination, a cluster of bubbles off to one side, was left "flat
/// looking puddles", a player, GitHub issue #203.)
static std::unique_ptr<VoxelUnitModel> reliefModel(const PartFrames &part, const std::vector<std::vector<Uint8>> &deaths, const std::vector<Uint8> &corpse,
	const HeldItem &lying, const SDL_Color *palette)
{
	const std::vector<Uint8> picture = withoutOutline(part.frame[Front], FrameSize, FrameSize, palette);
	const std::vector<int> depth = insideDepth(picture, FrameSize, FrameSize, (int)std::ceil(ReliefHalfMost / ReliefRound) + 1);
	int left = FrameSize, right = -1, top = FrameSize, bottom = -1;
	for (int y = 0; y < FrameSize; ++y)
		for (int x = 0; x < FrameSize; ++x)
			if (picture[y * FrameSize + x]) { left = std::min(left, x); right = std::max(right, x); top = std::min(top, y); bottom = std::max(bottom, y); }
	if (right < 0) return nullptr;
	// (direction Front, level: a column is the unit's right, turned about, times PixelsAcross; a row its height: see project)
	VoxelUnitModel::FallShape shape;
	const float low[3] = { -(right + 1 - FloorX) / PixelsAcross, -ReliefHalfMost, std::max(0.0f, (float)(FloorY - bottom - 1)) };
	const float high[3] = { -(left - 1 - FloorX) / PixelsAcross, ReliefHalfMost, (float)(FloorY - top + 1) };
	fillShape(shape, low, high, [&](float across, float along, float up) -> Uint8 {
		float column, row, nearness;
		project(across, 0.0f, up, Front, true, column, row, nearness);
		const int x = (int)std::floor(column), y = (int)std::floor(row);
		if (x < 0 || y < 0 || x >= FrameSize || y >= FrameSize) return 0;
		const Uint8 colour = picture[y * FrameSize + x];
		if (!colour) return 0;
		const float half = std::max(Cell * 1.2f, std::min(ReliefHalfMost, 0.2f * VOXELSCALE + ReliefRound * depth[y * FrameSize + x]));
		return std::abs(along) <= half ? colour : 0;
	});
	const size_t count = shape.colour.size();
	if (count == 0) return nullptr;
	auto model = std::make_unique<VoxelUnitModel>();
	model->part.assign(count, VoxelUnitModel::Torso);
	model->views.assign(count, 0xFFFF);
	model->colours.resize(count * 16);
	for (size_t k = 0; k < count; ++k) std::fill(model->colours.begin() + k * 16, model->colours.begin() + k * 16 + 16, shape.colour[k]);
	model->addedFrom = count;
	// What a shot meets: its cubes, in whole voxels, from every side.
	const std::vector<float> *place[3] = { &shape.right, &shape.ahead, &shape.up };
	for (int i = 0; i < 3; ++i)
	{
		const auto ends = std::minmax_element(place[i]->begin(), place[i]->end());
		model->hitFrom[i] = (int)std::floor(*ends.first);
		model->hitSize[i] = (int)std::floor(*ends.second) - model->hitFrom[i] + 1;
	}
	model->hits.assign((size_t)model->hitSize[0] * model->hitSize[1] * model->hitSize[2], 0);
	for (size_t k = 0; k < count; ++k)
	{
		const int x = (int)std::floor(shape.right[k]) - model->hitFrom[0], y = (int)std::floor(shape.ahead[k]) - model->hitFrom[1], z = (int)std::floor(shape.up[k]) - model->hitFrom[2];
		model->hits[((size_t)x * model->hitSize[1] + y) * model->hitSize[2] + z] = 0xFFFF;
	}
	model->right = std::move(shape.right);
	model->ahead = std::move(shape.ahead);
	model->up = std::move(shape.up);
	for (int phase = 0; phase < 8; ++phase)
	{
		model->walkRight[phase] = model->right;
		model->walkAhead[phase] = model->ahead;
		model->walkUp[phase] = model->up;
	}
	makeFalls(*model, deaths, corpse, lying, palette, false, false);
	return model;
}

/// A tank made as high as the game's units are (see Routine::tankHull): its hull, from its underside to its deck,
/// stretched to reach hullTop, and its turret, from the deck to its top, to reach turretTop; heights in the body's own
/// measure. The cells and the cubes move by whole layers of the grid: a layer of the body as made becomes the one or
/// more layers that stand for it now (or shares one with its neighbour, where the body got lower), each cube with the
/// colours it was painted with as drawn: the sides stay closed, the cubes on the grid. The owner's ruling on GitHub
/// issue #201 ("height should, overall, reflect hitbox. Hull of main chasis should be, roughly kneel height, and turret
/// maxing out on normal unit height. Tanks are often used as mobile cover"): "follow reporter instructions".
static void standTank(VoxelUnitModel &model, std::vector<Uint8> &owner, float hullTop, float turretTop)
{
	int bottom = GridUp, deck = -1, top = -1;
	for (int cell = 0; cell < GridCells; ++cell)
	{
		if (!owner[cell]) continue;
		const int up = cell % GridUp;
		if (owner[cell] == PartCodes[1]) top = std::max(top, up);
		else { bottom = std::min(bottom, up); deck = std::max(deck, up); }
	}
	if (deck < 0 || top <= deck || deck <= bottom) return;
	auto layerAt = [](float height) { return (int)std::lround((height - UpFrom) / Cell - 0.5f); };
	const int hullLayer = std::clamp(layerAt(hullTop), bottom + 2, GridUp - 4), topLayer = std::clamp(layerAt(turretTop), hullLayer + 2, GridUp - 1);
	// The layer of the body as made that each layer stands for now (-1: none), and the layers each one became.
	std::vector<int> source(GridUp, -1);
	std::vector<std::vector<int>> became(GridUp);
	for (int up = 0; up <= topLayer; ++up)
	{
		source[up] = up <= bottom ? up : up <= hullLayer ? bottom + (int)std::lround((double)(up - bottom) * (deck - bottom) / (hullLayer - bottom))
			: deck + (int)std::lround((double)(up - hullLayer) * (top - deck) / (topLayer - hullLayer));
		became[source[up]].push_back(up);
	}
	for (int up = bottom + 1; up <= top; ++up)
	{
		// (a layer none stands for, where the body got lower: with the layer it would be)
		if (became[up].empty())
		{
			became[up].push_back(up <= deck ? bottom + (int)std::lround((double)(up - bottom) * (hullLayer - bottom) / (deck - bottom))
				: hullLayer + (int)std::lround((double)(up - deck) * (topLayer - hullLayer) / (top - deck)));
		}
	}
	std::vector<Uint8> stood(GridCells, 0);
	for (int across = 0; across < GridAcross; ++across)
		for (int along = 0; along < GridAlong; ++along)
			for (int up = 0; up < GridUp; ++up)
				if (source[up] >= 0) stood[cellIndex(across, along, up)] = owner[cellIndex(across, along, source[up])];
	owner.swap(stood);
	// The cubes: each one in every layer its own became, once a cell.
	const size_t count = model.part.size();
	const bool viewed = model.views.size() == count, noneAdded = model.addedFrom >= count;
	std::vector<float> right, ahead, up;
	std::vector<Uint8> part, colours;
	std::vector<Uint16> views;
	std::unordered_set<long long> taken;
	for (size_t k = 0; k < count; ++k)
	{
		const int layer = (int)std::floor((model.up[k] - UpFrom) / Cell);
		const int across = (int)std::floor((model.right[k] - AcrossFrom) / Cell), along = (int)std::floor((model.ahead[k] - AlongFrom) / Cell);
		static const std::vector<int> nowhere;
		const std::vector<int> &layers = layer >= 0 && layer < GridUp ? became[layer] : nowhere;
		for (int now : layers)
		{
			if (!taken.insert(((long long)across * 100000 + along) * 100000 + now).second) continue;
			right.push_back(model.right[k]);
			ahead.push_back(model.ahead[k]);
			up.push_back(cellUp(now));
			part.push_back(model.part[k]);
			colours.insert(colours.end(), model.colours.begin() + k * 16, model.colours.begin() + k * 16 + 16);
			if (viewed) views.push_back(model.views[k]);
		}
	}
	model.right.swap(right);
	model.ahead.swap(ahead);
	model.up.swap(up);
	model.part.swap(part);
	model.colours.swap(colours);
	if (viewed) model.views.swap(views);
	if (noneAdded) model.addedFrom = model.part.size();
}

std::unique_ptr<VoxelUnitModel> VoxelBodies::makeModel(PartFrames parts[PartCount], const std::vector<Uint8> composite[8], const HeldItem items[2],
	const std::vector<std::vector<Uint8>> &deaths, const std::vector<Uint8> &corpse, const HeldItem &lying, const WalkFrames &walks, bool aiming, const Routine &routine,
	const SDL_Color *palette)
{
	if (!parts[0].any) return nullptr;
	useGrid(routine.big ? 2 : 1);
	if (routine.relief && routine.whole && !routine.big) return reliefModel(parts[0], deaths, corpse, lying, palette);
	// (a hull: a big unit that doesn't walk. One drawn by a walker's routine but without walk frames is a hull too: XPiratez's
	// truck turret, a turret drawn by the Sectopod's routine that never moves.)
	const bool bigHull = routine.big && !(routine.legsSwing && walks.any);
	// (A turret: two tiles by two and unable to move, mounted on a vehicle. It alone is made a plain block and painted
	// from all eight drawings (see levelOff): the owner, "leave all units as before only the turrets (2x2) that cant
	// move make as simple shapes decorated from sprites and plop them on top of respective vehicles". Tanks, hover
	// tanks and the Cyberdisc keep the shape their eight drawings leave.)
	const bool turret = routine.big && routine.still;
	// (A tank: a hull with a turret of its own, whose drawings are on the left arm's frames, see VoxelUnits::modelFor.
	// Each is made a plain block as a turret is, the hull's and on it the turret's with its barrel, and painted from all
	// eight drawings: "turret, and body as seperate voxel models stacked", a player, GitHub issue #188.) The hull keeps
	// the shape its drawings leave, cut flat (see flattenHull); the turret is a plain block with its barrel (see levelOff).
	const bool tank = bigHull && !turret && parts[1].any;
	SlantOnly = routine.big && !bigHull;
	// (A long body on four legs, a dog or a cat, named so by the mod's rules: made from its side drawings, see longBody
	// and profile.)
	// (a long body: a beast the mod's rules name; and any thin thing drawn longer than wide, a snake: see profile)
	const bool beast = routine.whole && !routine.big && longBody(parts[0]) && (routine.beast || thinDrawing(parts[0]));
	LongReading longReading = beast ? readLong(parts[0], routine.beast) : LongReading(); // (its length is found with its shape: see longShape)
	float beastLegsTop = 0.0f;
	std::vector<int> onTop[8], solidOnTop[8];
	partsOnTop(parts, composite, onTop);
	if (routine.big)
	{
		PartFrames solid = parts[0]; // (a big unit is drawn on one frame: see Routine)
		keepSolid(solid);
		PartFrames solidParts[PartCount];
		solidParts[0] = solid;
		partsOnTop(solidParts, composite, solidOnTop);
	}
	else
	{
		std::copy(onTop, onTop + 8, solidOnTop);
	}
	removeOutlines(parts, palette);
	addContrast(parts, palette);

	// The parts' shapes, each claiming its cells in this order (the torso first).
	std::vector<Uint8> owner(GridCells, 0);
	if (routine.whole)
	{
		// all of it on one frame: each run of each row its own slice (two legs stay two, an arm beside the body its own); a
		// big unit's fine detail (the sparks and streaks of a hover field, a pixel or two wide) left out of its solid
		// shape, for the views' pieces to show as the drawings do (see matchViews)
		PartFrames solid = parts[0];
		// (with one body for every side, see OneBody, no view's pieces show that detail: it stays in the shape, a
		// Cacodemon's horns with it)
		if (routine.big && !OneBody) keepSolid(solid);
		// (a big unit's rows aren't heights: its four tiles' pictures stand on floors 8 rows apart, see tileFloorRows; its
		// shape is the 8 views' alone, strictly, but for a big walker's: its frames, as a one-tile unit's, don't quite
		// agree, and a strict hull of them lost its shell and horns)
		// (a long body, a beast on four legs: its side drawings' outline, see profile)
		const std::vector<Uint8> shape = routine.big ? std::vector<Uint8>(GridCells, 1) : beast ? longShape(solid, beastLegsTop, routine.beast, longReading) : limb(solid, RightSide);
		// (a thin thing drawn on one frame, a snake, a spider on long legs: carved as a limb is, see LimbCarveReach)
		// (a long body has its width from its own side drawing, see profile: the other drawings don't cut it)
		std::vector<Uint8> hull = beast ? std::vector<Uint8>(GridCells, 1) : carve(solid, bigHull ? 0 : 1, shape, !routine.big && thinDrawing(solid) ? LimbCarveReach : 0);
		if (turret) levelOff(solid, hull);
		if (tank) flattenHull(hull);
		for (int i = 0; i < GridCells; ++i) if (shape[i] && hull[i]) owner[i] = PartCodes[0];
		if (tank)
		{
			// the turret: its own drawings' block (its barrel is fine detail: nothing of it is left out), standing on
			// the hull: where there is air between them it reaches down to the hull's top
			std::vector<Uint8> gun = carve(parts[1], 0, std::vector<Uint8>(GridCells, 1));
			// (Its top as its front and side drawings have it: nothing of it higher, over each place across and along,
			// than those two show there, read as heights, TurretSlack lower for the slant they are drawn along. Its eight
			// outlines alone leave it one lump with a peak, the dome and what stands beside it run together: "turrets
			// could use better definition. Rocket tanks should roughly keep shape of rocket pods on sides", a player,
			// GitHub issue #201. So the dome rises above the pods beside it.)
			{
				std::vector<int> topFront(FrameSize, FrameSize), topSide(FrameSize, FrameSize);
				for (int column = 0; column < FrameSize; ++column)
					for (int row = FrameSize - 1; row >= 0; --row)
					{
						if (parts[1].frame[Front][row * FrameSize + column]) topFront[column] = row;
						if (parts[1].frame[RightSide][row * FrameSize + column]) topSide[column] = row;
					}
				for (int across = 0; across < GridAcross; ++across)
					for (int along = 0; along < GridAlong; ++along)
					{
						const int front = (int)std::floor(FloorX - cellRight(across) * PixelsAcross), side = (int)std::floor(FloorX + cellAhead(along) * PixelsAcross);
						const int row = front >= 0 && side >= 0 && front < FrameSize && side < FrameSize ? std::max(topFront[front], topSide[side]) : FrameSize;
						const float most = FloorY - row - TurretSlack;
						for (int up = 0; up < GridUp; ++up) if (cellUp(up) > most) gun[cellIndex(across, along, up)] = 0;
					}
			}
			for (int across = 0; across < GridAcross; ++across)
				for (int along = 0; along < GridAlong; ++along)
				{
					int lowest = -1, roof = -1;
					for (int up = 0; up < GridUp && lowest < 0; ++up) if (gun[cellIndex(across, along, up)]) lowest = up;
					if (lowest < 0) continue;
					for (int up = lowest - 1; up >= 0 && roof < 0; --up) if (owner[cellIndex(across, along, up)]) roof = up;
					for (int up = roof + 1; roof >= 0 && up < lowest; ++up) gun[cellIndex(across, along, up)] = 1;
				}
			// (Its barrel: where its side drawing runs out ahead of it thin, under BarrelThin of the turret's own height
			// there, for two pixels or more, in both side drawings: a round bar as long, as high up and as thick as
			// drawn, from inside the turret. "Tank barrel for cannon / laser / standard 1 barrel tank, should have a
			// protruding barrel, to give definition", the same issue; its outlines left the stub on the turret's face.)
			{
				auto thinEnd = [&](int d, bool atRight, int &from, int &to, int &top, int &bottom) {
					std::vector<int> high(FrameSize, 0), first(FrameSize, FrameSize), last(FrameSize, -1);
					int most = 0, left = FrameSize, right = -1;
					for (int column = 0; column < FrameSize; ++column)
					{
						for (int row = 0; row < FrameSize; ++row)
							if (parts[1].frame[d][row * FrameSize + column]) { ++high[column]; first[column] = std::min(first[column], row); last[column] = row; }
						most = std::max(most, high[column]);
						if (high[column]) { left = std::min(left, column); right = std::max(right, column); }
					}
					if (right < 0) return false;
					from = to = atRight ? right : left;
					top = FrameSize; bottom = -1;
					for (int column = from; column >= left && column <= right && high[column] > 0 && high[column] <= most * BarrelThin; column += atRight ? -1 : 1)
					{
						(atRight ? from : to) = column;
						top = std::min(top, first[column]); bottom = std::max(bottom, last[column]);
					}
					return bottom >= 0 && to - from + 1 >= 2 * VOXELSCALE;
				};
				int from, to, top, bottom, otherFrom, otherTo, otherTop, otherBottom;
				if (thinEnd(RightSide, true, from, to, top, bottom) && thinEnd(LeftSide, false, otherFrom, otherTo, otherTop, otherBottom))
				{
					const float height = FloorY - (top + bottom + 1) * 0.5f, thick = std::max(1.0f * VOXELSCALE, (bottom - top + 1) * 0.5f);
					const float root[3] = { 0.0f, (from - FloorX) / PixelsAcross - 2.0f * VOXELSCALE, height }, muzzle[3] = { 0.0f, (to + 1 - FloorX) / PixelsAcross, height };
					fillCapsule(gun, root, muzzle, thick);
				}
			}
			for (int i = 0; i < GridCells; ++i) if (gun[i]) owner[i] = PartCodes[1];
		}
	}
	else
	{
		std::vector<Uint8> torso(GridCells, 0), neck(GridCells, 0);
		headAndTorso(parts[0], chinEnd(parts[0], parts[1], parts[2]), torso, neck, routine.torsoDepth);
		const std::vector<Uint8> hull = carve(parts[0], 1, torso);
		for (int i = 0; i < GridCells; ++i) if ((torso[i] && hull[i]) || neck[i]) owner[i] = PartCodes[0];
	}
	// The items in hand, where their frames show them, and the arms' skeletons (see ArmFrames): each from the top of its
	// drawing to its hand (on the item it holds, or the bottom of its drawing), as long as a free arm reaches.
	ItemPlace places[2];
	const bool placed[2] = { placeItem(parts[RightItem], items[0].twoHanded, aiming, items[0].melee, places[0]),
		placeItem(parts[LeftItem], items[1].twoHanded, aiming, items[1].melee, places[1]) };
	// What it holds is held in front of it: an item whose own length lies inside the body for more than ItemInBody of
	// it (the hand frames only say where it is on the pictures; a deep body, a Muton's or a robe, reached out round
	// it) is moved ahead, half a voxel at a time, till it is not, ItemOutMost at most. The arms reach for it where it
	// is then. (A player, GitHub issue #207: "larger units ... have the gun imbedded in their stomach. While it being
	// slightly pushed in is fine, full inside the stomach is strange".)
	for (int hand = 0; hand < 2; ++hand)
	{
		if (!placed[hand] || places[hand].plate || routine.big || routine.whole) continue; // (a body drawn on one frame has its arms in it: the item lies on them)
		ItemPlace &place = places[hand];
		auto inBody = [&](float shift) {
			int in = 0;
			for (int step = 0; step < ItemSamples; ++step)
			{
				const float at = ((step + 0.5f) / ItemSamples - 0.5f) * place.length;
				const int across = (int)std::floor((place.middle[0] + at * place.along[0] - AcrossFrom) / Cell), along = (int)std::floor((place.middle[1] + shift + at * place.along[1] - AlongFrom) / Cell),
					up = (int)std::floor((place.middle[2] + at * place.along[2] - UpFrom) / Cell);
				if (across >= 0 && along >= 0 && up >= 0 && across < GridAcross && along < GridAlong && up < GridUp && owner[cellIndex(across, along, up)]) ++in;
			}
			return in;
		};
		float shift = 0.0f;
		while (shift < ItemOutMost && inBody(shift) * 100 > ItemSamples * ItemInBody) shift += Cell;
		place.middle[1] += shift;
	}
	std::vector<Uint8> skeletons[BodyParts];
	// (each arm's bones as placed: shoulder, elbow, hand; see the arms' colours below)
	struct ArmBones { bool any = false; float shoulder[3], elbow[3], hand[3]; };
	ArmBones bones[BodyParts];
	{
		struct Arm { int part; bool right, holds, found; float shoulder[3], hand[3]; };
		Arm arms[2] = { { 1, false, false, false, {}, {} }, { 2, true, false, false, {}, {} } };
		for (Arm &arm : arms)
		{
			const int own = arm.right ? 0 : 1, other = 1 - own;
			arm.found = !tank && parts[arm.part].any && armEnd(parts[arm.part], arm.right, true, arm.shoulder); // (a tank's left arm's frames are its turret's)
			if (placed[own]) { gripOf(places[own], items[own].twoHanded, true, arm.hand); arm.holds = true; }
			else if (placed[other] && items[other].twoHanded && !places[other].upright) { gripOf(places[other], true, false, arm.hand); arm.holds = true; }
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
			length = std::max(4.0f * VOXELSCALE, shoulders / found * ArmNoLegs);
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
			length = std::max(4.0f * VOXELSCALE, (shoulders / found - legsTop) * ArmFromTorso);
		}
		for (const Arm &arm : arms)
		{
			if (!arm.found) continue;
			const ArmFrames frames(parts[arm.part]);
			const float bone = std::max(length, distance3(arm.shoulder, arm.hand) * 1.02f) * 0.5f;
			float shoulder[3], elbow[3], hand[3], radius[2];
			std::copy(arm.shoulder, arm.shoulder + 3, shoulder);
			std::copy(arm.hand, arm.hand + 3, hand);
			placeElbow(shoulder, hand, bone, frames, elbow);
			armRadii(frames, shoulder, elbow, hand, radius);
			// Arms outside the body (the unit table's "arms: outside", for the units it names): a body drawn in a robe
			// is carved as wide as the robe, and the arms its drawings lay over the robe's sides were inside it, flat
			// stripes on its skin ("their arms are clearly outside robes, and should be painted OVER the robe, OR ...
			// extend the arms out", a player, GitHub issue #207). Each joint moves out to its own side till half its
			// bone's thickness is clear of the body; a hand on an item stays on it.
			if (routine.arms & 1)
			{
				const float side = arm.right ? 1.0f : -1.0f;
				auto clear = [&](float joint[3], float thick) {
					for (int step = 0; step < 24 * VOXELSCALE; ++step)
					{
						const int across = (int)std::floor((joint[0] - side * thick * 0.5f - AcrossFrom) / Cell), along = (int)std::floor((joint[1] - AlongFrom) / Cell),
							up = (int)std::floor((joint[2] - UpFrom) / Cell);
						if (across < 0 || along < 0 || up < 0 || across >= GridAcross || along >= GridAlong || up >= GridUp || owner[cellIndex(across, along, up)] != VoxelUnitModel::Torso) break;
						joint[0] += side * Cell;
					}
				};
				clear(shoulder, radius[0]);
				clear(elbow, std::max(radius[0], radius[1]));
				if (!arm.holds) clear(hand, radius[1]);
			}
			std::vector<Uint8> &cells = skeletons[arm.part];
			cells.assign(GridCells, 0);
			fillCapsule(cells, shoulder, elbow, radius[0]);
			fillCapsule(cells, elbow, hand, radius[1]);
			bones[arm.part].any = true;
			std::copy(shoulder, shoulder + 3, bones[arm.part].shoulder);
			std::copy(elbow, elbow + 3, bones[arm.part].elbow);
			std::copy(hand, hand + 3, bones[arm.part].hand);
		}
	}
	for (int p = 1; p < BodyParts; ++p)
	{
		if (!parts[p].any || tank) continue;
		// the left arm is nearest the camera in direction 5, the right arm and the legs in 1; an arm's skeleton joins it
		std::vector<Uint8> shape = limb(parts[p], PartCodes[p] == VoxelUnitModel::LeftArm ? LeftSide : RightSide);
		const std::vector<Uint8> hull = carve(parts[p], 2, shape, LimbCarveReach);
		const bool skeleton = !skeletons[p].empty();
		for (int i = 0; i < GridCells; ++i) if (((shape[i] && hull[i]) || (skeleton && skeletons[p][i])) && !owner[i]) owner[i] = PartCodes[p];
		// (arms outside the body, see above: the bones lie over the body where they touch it)
		if ((routine.arms & 1) && skeleton && (PartCodes[p] == VoxelUnitModel::LeftArm || PartCodes[p] == VoxelUnitModel::RightArm))
			for (int i = 0; i < GridCells; ++i) if (skeletons[p][i] && owner[i] == VoxelUnitModel::Torso) owner[i] = PartCodes[p];
	}
	// A body sculpted from its frontal drawings (the unit table's "sculpt: frontal", for the units it names; the owner:
	// the voxeliser "should maybe really look at the sprite frontals more (more than side positions) and carefully craft
	// body waist arms head and legs ... as drawn in sprites"): nothing of it stands outside what the front drawing or
	// the back drawing has drawn there (all the parts as the game lays them, along each drawing's own slant). The side
	// drawings give it its depth and no more: where they left the gap between an arm and the waist filled, or the legs
	// joined below the hips, the frontal outline cuts it open again.
	if (routine.frontal && !routine.big)
	{
		const int Back = (Front + 4) % 8;
		for (int across = 0; across < GridAcross; ++across)
			for (int along = 0; along < GridAlong; ++along)
				for (int up = 0; up < GridUp; ++up)
				{
					Uint8 &cell = owner[cellIndex(across, along, up)];
					if (!cell) continue;
					for (int d : { Front, Back })
					{
						float column, row, nearness;
						project(cellRight(across), cellAhead(along), cellUp(up), d, false, column, row, nearness);
						const int c = (int)std::floor(column), r = (int)std::floor(row);
						if (c < 0 || r < 0 || c >= FrameSize || r >= FrameSize || onTop[d][r * FrameSize + c] < 0) { cell = 0; break; }
					}
				}
	}
	// Loose cells (fewer than 3 of their 6 neighbours filled) go, and with them the specks they'd show.
	auto filled = [&](int across, int along, int up) {
		return across >= 0 && along >= 0 && up >= 0 && across < GridAcross && along < GridAlong && up < GridUp && owner[cellIndex(across, along, up)];
	};
	// (Not a long body's: it is one drawing given width, see profile, with no speck that drawing hasn't got; and thin
	// as a snake is, the loose cells were its middle: the owner, "Cobra missing body - disjoined body".)
	std::vector<Uint8> keep(owner);
	for (int across = 0; across < GridAcross && !beast; ++across)
		for (int along = 0; along < GridAlong; ++along)
			for (int up = 0; up < GridUp; ++up)
			{
				if (!owner[cellIndex(across, along, up)]) continue;
				const int neighbours = filled(across - 1, along, up) + filled(across + 1, along, up) + filled(across, along - 1, up)
					+ filled(across, along + 1, up) + filled(across, along, up - 1) + filled(across, along, up + 1);
				if (neighbours < 3) keep[cellIndex(across, along, up)] = 0;
			}
	owner.swap(keep);
	// One body: the biggest piece of it stays, and what stands apart from that goes (the owner: "No voxels should be in
	// space from the main body"). Cells within Joined cells of each other are one piece (a hand a cell off its arm).
	{
		std::vector<int> piece(GridCells, -1), stack;
		std::vector<int> sizes;
		const int reach = Joined;
		for (int start = 0; start < GridCells; ++start)
		{
			if (!owner[start] || piece[start] >= 0) continue;
			const int label = (int)sizes.size();
			sizes.push_back(0);
			piece[start] = label;
			stack.push_back(start);
			while (!stack.empty())
			{
				const int cell = stack.back();
				stack.pop_back();
				++sizes[label];
				const int up = cell % GridUp, along = (cell / GridUp) % GridAlong, across = cell / (GridUp * GridAlong);
				for (int a = std::max(0, across - reach); a <= std::min(GridAcross - 1, across + reach); ++a)
					for (int b = std::max(0, along - reach); b <= std::min(GridAlong - 1, along + reach); ++b)
						for (int c = std::max(0, up - reach); c <= std::min(GridUp - 1, up + reach); ++c)
						{
							const int other = cellIndex(a, b, c);
							if (owner[other] && piece[other] < 0) { piece[other] = label; stack.push_back(other); }
						}
			}
		}
		if (sizes.size() > 1 && !beast) // (a long body is whole as drawn, see above)
		{
			const int biggest = (int)(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
			for (int cell = 0; cell < GridCells; ++cell) if (owner[cell] && piece[cell] != biggest) owner[cell] = 0;
		}
	}

	joinPieces(owner);

	// (a body on one frame that is no long one: its limbs go on as its front drawing has them, see continueLimbs;
	// after the loose cells and the pieces apart went, which took a thin shin and all below it; not a thin thing, a
	// bat: its wings went on down as curtains)
	if (routine.whole && !routine.big && !beast && !tank && !turret && !thinDrawing(parts[0]))
	{
		continueLimbs(parts[0], PartCodes[0], owner);
	}

	// The waist: each part's shape narrows to nothing at its ends, and where the torso's lowest rows met the legs' highest
	// the body pinched to a point (the owner on a Sectoid: "legs and torso join is too small. It looks like body is
	// floating on top of legs"). The rows between the torso's fullest row near its foot and the legs' fullest near
	// their top are filled out: the torso's section there, as far across as the legs reach.
	if (!routine.whole && !routine.big)
	{
		std::vector<int> torsoCells(GridUp, 0), legCells(GridUp, 0);
		for (int across = 0; across < GridAcross; ++across)
			for (int along = 0; along < GridAlong; ++along)
				for (int up = 0; up < GridUp; ++up)
				{
					const Uint8 part = owner[cellIndex(across, along, up)];
					if (part == VoxelUnitModel::Torso) ++torsoCells[up];
					else if (part == VoxelUnitModel::Legs) ++legCells[up];
				}
		int torsoLow = 0, legsHigh = GridUp - 1;
		while (torsoLow < GridUp && !torsoCells[torsoLow]) ++torsoLow;
		while (legsHigh >= 0 && !legCells[legsHigh]) --legsHigh;
		const int look = 6 * VOXELSCALE; // (cells: three voxels of each part's end)
		// (only where the two meet: a torso standing well clear of its legs is drawn so)
		if (torsoLow < GridUp && legsHigh >= 0 && legsHigh < torsoLow + look && torsoLow - legsHigh <= look)
		{
			int torsoRow = torsoLow, legsRow = legsHigh;
			for (int up = torsoLow; up < std::min(GridUp, torsoLow + look); ++up) if (torsoCells[up] > torsoCells[torsoRow]) torsoRow = up;
			for (int up = legsHigh; up >= std::max(0, legsHigh - look); --up) if (legCells[up] > legCells[legsRow]) legsRow = up;
			int legsFrom = GridAcross, legsTo = -1;
			for (int across = 0; across < GridAcross; ++across)
				for (int along = 0; along < GridAlong; ++along)
					if (owner[cellIndex(across, along, legsRow)] == VoxelUnitModel::Legs) { legsFrom = std::min(legsFrom, across); legsTo = std::max(legsTo, across); }
			for (int up = legsRow + 1; up < torsoRow; ++up)
				for (int across = legsFrom; across <= legsTo; ++across)
					for (int along = 0; along < GridAlong; ++along)
					{
						Uint8 &cell = owner[cellIndex(across, along, up)];
						if (!cell && owner[cellIndex(across, along, torsoRow)] == VoxelUnitModel::Torso) cell = up <= legsHigh ? VoxelUnitModel::Legs : VoxelUnitModel::Torso;
					}
		}
	}

	// The voxels: each a filled cell of the grid, a little cube; drawn side by side they pack without a gap. Kept are the
	// cells of the body's skin: those with an empty cell beside them, and those that are skin in some view. (Seen from
	// one of the 8 directions the body shows only its cells inside that direction's drawing, see matchViews: where a
	// cell is left out there, the cell behind it is that view's skin. Without those one looked into the hollow body and
	// out at its far side: thin slits up close.) A cell shows in the views it's skin in.
	auto model = std::make_unique<VoxelUnitModel>();
	std::vector<int> surfaceCells;
	std::vector<Uint16> skin;
	{
		std::vector<Uint16> shown(GridCells, 0);
		for (int across = 0; across < GridAcross; ++across)
			for (int along = 0; along < GridAlong; ++along)
				for (int up = 0; up < GridUp; ++up)
					if (owner[cellIndex(across, along, up)]) shown[cellIndex(across, along, up)] = viewsAt(solidOnTop, cellRight(across), cellAhead(along), cellUp(up));
		static const int Beside[6][3] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
		for (int across = 0; across < GridAcross; ++across)
			for (int along = 0; along < GridAlong; ++along)
				for (int up = 0; up < GridUp; ++up)
				{
					const int cell = cellIndex(across, along, up);
					if (!owner[cell]) continue;
					bool outside = false;
					Uint16 bare = 0; // (the views a cell beside it is left out of)
					for (const int *step : Beside)
					{
						if (!filled(across + step[0], along + step[1], up + step[2])) outside = true;
						else bare |= (Uint16)~shown[cellIndex(across + step[0], along + step[1], up + step[2])];
					}
					if (!outside && (OneBody || !(bare & shown[cell]))) continue; // (one body: its outer skin alone)
					surfaceCells.push_back(cell);
					skin.push_back(outside ? (Uint16)0xFFFF : bare);
					model->right.push_back(cellRight(across));
					model->ahead.push_back(cellAhead(along));
					model->up.push_back(cellUp(up));
					model->part.push_back(owner[cell]);
				}
	}
	const size_t count = surfaceCells.size();
	if (count == 0) return nullptr;

	// Colours: for each painting and direction, each part's frame paints the part's voxels on its drawn pixels, as a
	// picture thrown along the view onto them (seen from that direction, what shows is the drawing; see matchViews);
	// a voxel off its part's drawing but on another part's there, by that one's. Only the part's voxels facing that
	// direction take the picture: those within FaceDepth of the part's nearest there. What lies deeper (the far side, the
	// walls along the view) is left for the directions that do face it (the fill below): thrown through the whole part,
	// an eye was a dark band through the head, there to see whenever the body wasn't seen exactly along the view (from
	// between two directions, or up close near the screen's edge, where the perspective shows a body's depth: the
	// owner's "like they use 2 superimposed photos/sprites").
	model->colours.assign(count * 16, 0);
	std::vector<float> faceNear(BodyParts * FrameSize * FrameSize);
	for (int painting = 0; painting < 2; ++painting)
	{
		const bool level = painting == 0;
		for (int d = 0; d < 8; ++d)
		{
			std::fill(faceNear.begin(), faceNear.end(), -1e9f);
			for (size_t k = 0; k < count; ++k)
			{
				const int p = std::find(PartCodes, PartCodes + BodyParts, model->part[k]) - PartCodes;
				float column, row, nearness;
				project(model->right[k], model->ahead[k], model->up[k], d, level, column, row, nearness);
				const int c = (int)std::floor(column), r = (int)std::floor(row);
				if (p >= BodyParts || c < 0 || r < 0 || c >= FrameSize || r >= FrameSize) continue;
				float &nearest = faceNear[(p * FrameSize + r) * FrameSize + c];
				nearest = std::max(nearest, nearness);
			}
			for (size_t k = 0; k < count; ++k)
			{
				const int p = std::find(PartCodes, PartCodes + BodyParts, model->part[k]) - PartCodes;
				float column, row, nearness;
				project(model->right[k], model->ahead[k], model->up[k], d, level, column, row, nearness);
				const int c = (int)std::floor(column), r = (int)std::floor(row);
				if (c < 0 || r < 0 || c >= FrameSize || r >= FrameSize) continue;
				const int pixel = r * FrameSize + c;
				if (p < BodyParts && nearness < faceNear[(p * FrameSize + r) * FrameSize + c] - FaceDepth) continue;
				Uint8 colour = p < BodyParts && parts[p].any ? parts[p].frame[d][pixel] : 0;
				if (!colour && onTop[d][pixel] >= 0 && onTop[d][pixel] < BodyParts) colour = parts[onTop[d][pixel]].frame[d][pixel]; // (never an item's: see drawnColour)
				if (colour) model->colours[k * 16 + painting * 8 + d] = colour;
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
	float wholeTop = 0.0f, wholeSide = 0.0f;
	if (beast)
	{
		// (a long body's legs: what's below its belly, see profile; the four swing each about its own hip, see makeWalk)
		for (size_t k = 0; k < count; ++k) if (model->up[k] < beastLegsTop) model->part[k] = VoxelUnitModel::Legs;
	}
	else if (routine.whole && routine.humanoid)
	{
		for (size_t k = 0; k < count; ++k) wholeTop = std::max(wholeTop, model->up[k]);
		for (size_t k = 0; k < count; ++k) if (model->up[k] < wholeTop * WholeHip) wholeSide = std::max(wholeSide, std::abs(model->right[k]));
		for (size_t k = 0; k < count; ++k)
		{
			if (model->up[k] < wholeTop * WholeHip && std::abs(model->right[k]) > wholeSide * WholeMiddle) model->part[k] = VoxelUnitModel::Legs;
		}
	}
	// The items in hand; the body matched to its drawings view by view; and the walk (an arm holding something held still,
	// as the game's frames do: an item in both hands holds both).
	const size_t itemsFrom = model->part.size();
	if (placed[0]) addItem(*model, items[0], places[0]);
	if (placed[1]) addItem(*model, items[1], places[1]);
	// (An item in hand is its inventory picture given volume, in that picture's colours from every side, whole from
	// every view: the weapon's own model put in the hands. It was painted by the frames' drawing of it and cut to each
	// view's drawing, with pieces added where the drawing had more: a rifle's ends came out ragged. The owner: "use
	// just already made models for weapons we use in 3d view and plop them on models".)
	PartFrames smoothed[PartCount]; // (the added pieces' colours, smoothed as the body's)
	std::copy(parts, parts + PartCount, smoothed);
	smoothFrames(smoothed);
	// (A big unit that hovers: the views' pieces only where its solid drawing is. The sparks and the glow drawn under a
	// disc or a tank are left out: the owner, "lets just omit that floating effect and generate only main body".)
	if (OneBody)
	{
		// One body, the same from every side (the owner: "the unit voxel model is as is in world ... It should just be one
		// model no?"): the whole shape the eight drawings agree on, nothing cut to the view or added for it, and each
		// voxel one colour, from the front drawing or the back one (the owner: "main colouring and detail take from
		// frontal and back sprites"): the one its outward side (by the empty cells round it) is turned to, thrown
		// straight through onto it, its own part's drawing first; the slanted painting's where it faces up (the top of a
		// head as the drawing shows it). Where neither has it drawn (a part standing out past both outlines), the
		// drawing of the direction it faces most that has.
		model->views.assign(model->part.size(), 0xFFFF);
		model->addedFrom = model->part.size();
		const int Back = (Front + 4) % 8, reach = 2 * VOXELSCALE;
		auto drawnColour = [&](size_t k, int d, int painting) -> Uint8 {
			float column, row, nearness;
			project(model->right[k], model->ahead[k], model->up[k], d, painting == 0, column, row, nearness);
			const int c = (int)std::floor(column), r = (int)std::floor(row);
			if (c < 0 || r < 0 || c >= FrameSize || r >= FrameSize) return 0;
			const int pixel = r * FrameSize + c, part = (int)(std::find(PartCodes, PartCodes + BodyParts, model->part[k]) - PartCodes);
			Uint8 colour = part < BodyParts && parts[part].any ? parts[part].frame[d][pixel] : 0;
			// (off its own part's drawing: the body part on top there. Never an item's: where a weapon is drawn over the
			// body, the cubes behind it took the weapon's grey, a patch of gun metal on a robe: a player, GitHub issue
			// #197, "gun model merges into body, dying voxels of body grey".)
			if (!colour && onTop[d][pixel] >= 0 && onTop[d][pixel] < BodyParts) colour = parts[onTop[d][pixel]].frame[d][pixel];
			for (int other = 0; !colour && other < BodyParts; ++other) if (parts[other].any) colour = parts[other].frame[d][pixel];
			return colour;
		};
		// (the body's skin seen from the front and from the back: the nearest of its voxels on each pixel)
		std::vector<float> frontSkin((size_t)FrameSize * FrameSize + 1, -1e9f), backSkin((size_t)FrameSize * FrameSize + 1, -1e9f);
		auto pixelAt = [&](size_t k, int d, float &nearness) -> size_t {
			float column, row;
			project(model->right[k], model->ahead[k], model->up[k], d, true, column, row, nearness);
			const int c = (int)std::floor(column), r = (int)std::floor(row);
			return c < 0 || r < 0 || c >= FrameSize || r >= FrameSize ? (size_t)FrameSize * FrameSize : (size_t)r * FrameSize + c;
		};
		for (size_t k = 0; k < count; ++k)
		{
			float nearness;
			float &front = frontSkin[pixelAt(k, Front, nearness)];
			front = std::max(front, nearness);
			float &back = backSkin[pixelAt(k, Back, nearness)];
			back = std::max(back, nearness);
		}
		// (A turret, see above. Its drawings show it from above, all round, and
		// its shape is read along that slant (see carve, levelOff): each of its voxels takes its pixel of the drawing that
		// sees it best, of all eight, thrown along the slant onto the skin that drawing sees. So from where a drawing was
		// made the body shows that drawing, cube for pixel. The skin each drawing sees: the nearest voxel on each pixel.)
		// (So a long body on four legs, see longBody: most of it is its flanks, which the front and back drawings show
		// nothing of. Painted by those two, a dog's flanks took its chest's colour: one tan barrel, where its side drawings
		// have a dark flank on light legs.)
		// (And so any body drawn on one frame, a creature: a frog, a demon. Its drawings show it from all round, and
		// thrown straight through it the front drawing's colours ran along its sides in streaks: a player, GitHub issue
		// #211, a Bullfrog's face "as if the front decided to adopt textures from the left". A body drawn in parts, a
		// human's, stays painted by its front and back.)
		const bool hull = turret || beast || tank || routine.whole;
		// (an animal's face, see below: the cells furthest ahead and furthest back on each line along the body)
		std::vector<int> foremost, rearmost;
		if (beast && routine.beast)
		{
			foremost.assign((size_t)GridAcross * GridUp, -1000); rearmost.assign((size_t)GridAcross * GridUp, 100000);
			for (int across = 0; across < GridAcross; ++across)
				for (int along = 0; along < GridAlong; ++along)
					for (int up = 0; up < GridUp; ++up)
					{
						if (!filled(across, along, up)) continue;
						foremost[(size_t)across * GridUp + up] = std::max(foremost[(size_t)across * GridUp + up], along);
						rearmost[(size_t)across * GridUp + up] = std::min(rearmost[(size_t)across * GridUp + up], along);
					}
		}
		std::vector<float> seenSkin[8];
		auto slantPixel = [&](size_t k, int d, float &nearness) -> size_t {
			float column, row;
			project(model->right[k], model->ahead[k], model->up[k], d, false, column, row, nearness);
			const int c = (int)std::floor(column), r = (int)std::floor(row);
			return c < 0 || r < 0 || c >= FrameSize || r >= FrameSize ? (size_t)FrameSize * FrameSize : (size_t)r * FrameSize + c;
		};
		for (int d = 0; hull && d < 8; ++d)
		{
			seenSkin[d].assign((size_t)FrameSize * FrameSize + 1, -1e9f);
			for (size_t k = 0; k < count; ++k)
			{
				float nearness;
				float &nearest = seenSkin[d][slantPixel(k, d, nearness)];
				nearest = std::max(nearest, nearness);
			}
		}
		for (size_t k = 0; k < count; ++k)
		{
			const int cell = surfaceCells[k], up = cell % GridUp, along = (cell / GridUp) % GridAlong, across = cell / (GridUp * GridAlong);
			float out[3] = {};
			for (int a = -reach; a <= reach; ++a)
				for (int b = -reach; b <= reach; ++b)
					for (int c = -reach; c <= reach; ++c)
						if (!filled(across + a, along + b, up + c)) { out[0] += a; out[1] += b; out[2] += c; }
			// (BackTop, the unit table's "top: back" for the units it names: what faces up is painted by the back drawing,
			// at its own height. XPiratez's humans' faces are drawn high on the head and stand out in front of the
			// hair above them: the top of that ledge, and the front of the head's top along the slant, took the front
			// drawing's pixels there, the forehead's: skin up the hair to the crown (a player, GitHub issue #181: "unit
			// seems to be given bald spot"). From behind a head is hair all over.)
			const bool facesUp = out[2] > planeLength(out[0], out[1]) * 1.5f;
			const int painting = facesUp && !routine.backTop ? 1 : 0;
			// (each direction's camera: towards its nearness, see project)
			float fit[8];
			for (int d = 0; d < 8; ++d) fit[d] = out[0] * (Turns[d].cosine + Turns[d].sine) + out[1] * (Turns[d].sine - Turns[d].cosine);
			int order[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
			std::sort(order, order + 8, [&](int one, int other) { return fit[one] > fit[other]; });
			// (front or back: the skin it lies nearer, along the line through the body from the front to the back; its
			// outward side where it's as near both)
			float nearFront, nearBack;
			const float underFront = frontSkin[pixelAt(k, Front, nearFront)] - nearFront, underBack = backSkin[pixelAt(k, Back, nearBack)] - nearBack;
			const int first = facesUp && routine.backTop ? Back
				: std::abs(underFront - underBack) < 1.0f * VOXELSCALE ? (fit[Front] >= fit[Back] ? Front : Back) : underFront < underBack ? Front : Back;
			Uint8 colour = 0;
			// (A long body is its right side's drawing given width, see profile: each cube has the colour of the pixel of
			// that drawing it stands behind, on either flank. Painted by whichever drawing faced a cube, a body no
			// other drawing shaped was a patchwork.)
			// (An animal's face: what of it looks ahead, with nothing of the body before it, has its pixel of the front
			// drawing, read as its breadth is (see profile, LongReading); what looks back, behind all else, the back
			// drawing's. A player, GitHub issue #211, of a dog's face: "I would hope it would prioritize there".)
			if (beast && routine.beast)
			{
				const float lengthways = std::abs(out[1]);
				const int end = out[1] > 0.0f ? 0 : 1;
				const int edge = end ? rearmost[(size_t)across * GridUp + up] : foremost[(size_t)across * GridUp + up];
				if (longReading.drawn[end] && lengthways >= std::abs(out[0]) && lengthways >= std::abs(out[2]) && std::abs(along - edge) <= AnimalFaceDepth)
				{
					// (the front drawing has the unit's right on its left, the back drawing on its right)
					const int column = (int)std::floor(longReading.middle[end] + (end ? 1.0f : -1.0f) * model->right[k] * PixelsAcross);
					colour = parts[0].at(end ? (Front + 4) % 8 : Front, column, longReading.row(end, model->ahead[k] / longReading.length, model->up[k]));
				}
			}
			if (beast && !colour)
			{
				const int column = (int)std::floor(FloorX + model->ahead[k] / longReading.length * PixelsAcross), row = (int)std::floor(FloorY - model->up[k]);
				for (int lower = 0; lower <= ProfileBelow && !colour; ++lower) colour = parts[0].at(RightSide, column, row + lower);
				for (int reach = 1; reach <= 2 * VOXELSCALE && !colour; ++reach)
					for (int dy = -reach; dy <= reach && !colour; ++dy)
						for (int dx = -reach; dx <= reach && !colour; ++dx) colour = parts[0].at(RightSide, column + dx, row + dy);
			}
			if (hull && !colour)
			{
				// (how squarely each drawing's camera, HullCameraUp above the ground, looks onto the voxel's outward side)
				float best = -1e9f;
				for (int d = 0; d < 8; ++d)
				{
					float nearness;
					const size_t pixel = slantPixel(k, d, nearness);
					if (nearness < seenSkin[d][pixel] - HullSkinDepth) continue;
					const float square = fit[d] * 0.70710678f * HullCameraAlong + out[2] * HullCameraUp;
					if (square <= best) continue;
					if (const Uint8 drawn = drawnColour(k, d, 1)) { colour = drawn; best = square; }
				}
				for (int i = 0; i < 8 && !colour; ++i) colour = drawnColour(k, order[i], 1);
			}
			if (!colour) colour = drawnColour(k, first, painting);
			if (!colour) colour = drawnColour(k, first == Front ? Back : Front, painting);
			for (int i = 0; i < 8 && !colour; ++i) colour = drawnColour(k, order[i], painting);
			for (int i = 0; i < 8 && !colour; ++i) colour = drawnColour(k, order[i], 1 - painting);
			for (int i = 0; i < 8 && !colour; ++i) colour = model->colours[k * 16 + order[i]];
			std::fill(model->colours.begin() + k * 16, model->colours.begin() + k * 16 + 16, colour);
		}
		// An arm's colours along its length (the unit table's "arms: along", for the units it names): an arm posed on
		// what it holds reaches ahead, where its drawings show it hanging or foreshortened; painted as a picture
		// thrown onto it, its sleeve's end and its hand came where the drawing's rows put them, not where they are
		// on the arm ("texture should probably be applied along length of limb, rather than height based? the dagon
		// disciples have longer, ragged looking sleeves", a player, GitHub issue #207). The drawing that shows the arm
		// longest gives its colours from the shoulder's end to the hand's, the commonest in each of ArmBands stretches;
		// each cube of the arm takes the colour of the stretch it is at along the bones.
		for (int p = 1; (routine.arms & 2) && p <= 2; ++p)
		{
			if (!bones[p].any || !smoothed[p].any) continue;
			int best = -1;
			double bestLong = 0.0, bestWay[2] = {}, bestMiddle[2] = {}, bestFrom = 0.0, bestTo = 0.0;
			for (int d = 0; d < 8; ++d)
			{
				double columns = 0.0, rows = 0.0;
				int drawn = 0;
				for (int row = 0; row < FrameSize; ++row)
					for (int column = 0; column < FrameSize; ++column)
						if (smoothed[p].frame[d][row * FrameSize + column]) { columns += column; rows += row; ++drawn; }
				if (drawn < 4 * VOXELSCALE) continue;
				columns /= drawn; rows /= drawn;
				double xx = 0.0, xy = 0.0, yy = 0.0;
				for (int row = 0; row < FrameSize; ++row)
					for (int column = 0; column < FrameSize; ++column)
						if (smoothed[p].frame[d][row * FrameSize + column]) { xx += (column - columns) * (column - columns); xy += (column - columns) * (row - rows); yy += (row - rows) * (row - rows); }
				const double turn = 0.5 * std::atan2(2.0 * xy, xx - yy);
				double way[2] = { std::cos(turn), std::sin(turn) };
				if (way[1] < 0.0) { way[0] = -way[0]; way[1] = -way[1]; } // (down the frame: from the shoulder to the hand)
				double from = 1e9, to = -1e9;
				for (int row = 0; row < FrameSize; ++row)
					for (int column = 0; column < FrameSize; ++column)
						if (smoothed[p].frame[d][row * FrameSize + column])
						{
							const double at = (column - columns) * way[0] + (row - rows) * way[1];
							from = std::min(from, at); to = std::max(to, at);
						}
				if (to - from > bestLong) { best = d; bestLong = to - from; bestWay[0] = way[0]; bestWay[1] = way[1]; bestMiddle[0] = columns; bestMiddle[1] = rows; bestFrom = from; bestTo = to; }
			}
			if (best < 0 || bestLong < 3.0 * VOXELSCALE) continue;
			Uint8 bands[ArmBands] = {};
			{
				int counts[ArmBands][256] = {};
				for (int row = 0; row < FrameSize; ++row)
					for (int column = 0; column < FrameSize; ++column)
					{
						const Uint8 colour = smoothed[p].frame[best][row * FrameSize + column];
						if (!colour) continue;
						const double at = ((column - bestMiddle[0]) * bestWay[0] + (row - bestMiddle[1]) * bestWay[1] - bestFrom) / (bestTo - bestFrom + 1e-9);
						++counts[std::clamp((int)(at * ArmBands), 0, ArmBands - 1)][colour];
					}
				for (int band = 0; band < ArmBands; ++band) bands[band] = (Uint8)(std::max_element(counts[band], counts[band] + 256) - counts[band]);
				for (int band = 1; band < ArmBands; ++band) if (!bands[band]) bands[band] = bands[band - 1];
				for (int band = ArmBands - 2; band >= 0; --band) if (!bands[band]) bands[band] = bands[band + 1];
			}
			const float upper = distance3(bones[p].shoulder, bones[p].elbow), lower = distance3(bones[p].elbow, bones[p].hand);
			if (upper + lower < 1.0f) continue;
			auto alongBone = [](const float a[3], const float b[3], const float at[3], float &off) {
				float t = 0.0f, size = 0.0f;
				for (int i = 0; i < 3; ++i) { t += (at[i] - a[i]) * (b[i] - a[i]); size += (b[i] - a[i]) * (b[i] - a[i]); }
				t = size > 0.0f ? std::clamp(t / size, 0.0f, 1.0f) : 0.0f;
				off = 0.0f;
				for (int i = 0; i < 3; ++i) off += (at[i] - a[i] - t * (b[i] - a[i])) * (at[i] - a[i] - t * (b[i] - a[i]));
				return t;
			};
			for (size_t k = 0; k < count; ++k)
			{
				if (model->part[k] != PartCodes[p]) continue;
				const float at[3] = { model->right[k], model->ahead[k], model->up[k] };
				float offUpper, offLower;
				const float onUpper = alongBone(bones[p].shoulder, bones[p].elbow, at, offUpper), onLower = alongBone(bones[p].elbow, bones[p].hand, at, offLower);
				// (past the hand's end the cubes are the hand's: the last stretch)
				const float reached = offUpper <= offLower ? onUpper * upper : upper + onLower * lower;
				const Uint8 colour = bands[std::clamp((int)(reached / (upper + lower) * ArmBands), 0, ArmBands - 1)];
				if (colour) std::fill(model->colours.begin() + k * 16, model->colours.begin() + k * 16 + 16, colour);
			}
		}
	}
	else
	{
	matchViews(*model, smoothed, bigHull ? solidOnTop : onTop, solidOnTop);
	for (size_t k = 0; k < count && k < model->views.size(); ++k) model->views[k] &= skin[k]; // (a cell inside the body shows where it's the view's skin)
	}
	for (size_t k = itemsFrom; k < model->addedFrom && k < model->views.size(); ++k) model->views[k] = 0xFFFF; // (the items: whole from every view)
	// (a one-frame walker's pieces over its legs are legs too: they swing with them)
	for (size_t k = model->addedFrom; wholeTop > 0.0f && k < model->part.size(); ++k)
	{
		if (model->part[k] != VoxelUnitModel::Item && model->up[k] < wholeTop * WholeHip && std::abs(model->right[k]) > wholeSide * WholeMiddle) model->part[k] = VoxelUnitModel::Legs;
	}
	if (!OneBody)
	{
		dropFarPieces(*model, itemsFrom);
		dropFloating(*model);
	}
	// (a tank's turret is of its body from here on: nothing of it swings, and a shot at it meets the tank)
	for (size_t k = 0; tank && k < model->part.size(); ++k) if (model->part[k] == PartCodes[1]) model->part[k] = PartCodes[0];
	// (a tank as high as the game's units are, by the owner's ruling: see standTank)
	if (tank && routine.tankTop > 0.0f) standTank(*model, owner, routine.tankHull * VOXELSCALE, routine.tankTop * VOXELSCALE);
	makeHits(*model, owner, solidOnTop);
	const bool rightHolds = placed[0] || (placed[1] && items[1].twoHanded && !places[1].upright);
	const bool leftHolds = placed[1] || (placed[0] && items[0].twoHanded && !places[0].upright);
	makeWalk(*model, routine, leftHolds, rightHolds, walks, beast);
	makeFalls(*model, deaths, corpse, lying, palette, routine.big, bigHull);
	return model;
}


#if VOXELSCALE == 1 || defined(VOXELSCALE_ALONE) // (the model's own functions, once: this source is built a second time, see FirstPersonVoxelBodiesFine.cpp; ALONE: the offline tools' build at a scale)
/**
 * The shape a body going down has at a point of its fall: each death frame's in turn, then the body lying.
 * @param progress How far the fall has come (0 standing, 1 down).
 * @return The shape, or null (a body without death frames or a corpse: it stays as it stood).
 */
const VoxelUnitModel::FallShape *VoxelUnitModel::fallAt(double progress) const
{
	if (falls.empty() || progress <= 0.0) return nullptr;
	return &falls[std::min((size_t)(std::min(progress, 1.0) * falls.size()), falls.size() - 1)];
}

/**
 * Whether a shot meets the body at a point (see hits): a solid voxel there that shows from where the shot comes.
 * @param across, along, height The point: voxels to the body's right, ahead and up from the middle of its feet.
 * @param direction The view the shot comes from (0-7, as the colours' direction: see colour).
 * @param level From about the body's height (else from above).
 * @return True if the shot meets it there.
 */
bool VoxelUnitModel::hitAt(float across, float along, float height, int direction, bool level) const
{
	const int x = (int)std::floor(across) - hitFrom[0], y = (int)std::floor(along) - hitFrom[1], z = (int)std::floor(height) - hitFrom[2];
	if (hits.empty() || x < 0 || y < 0 || z < 0 || x >= hitSize[0] || y >= hitSize[1] || z >= hitSize[2]) return false;
	return hits[((size_t)x * hitSize[1] + y) * hitSize[2] + z] >> ((level ? 0 : 8) + direction) & 1;
}

/**
 * Whether a shot meets a body going down at a point: one of the shape's cubes there (a shape is the same from every side).
 * @param across, along, height The point: voxels to the body's right, ahead and up from the middle of its feet.
 * @return True if the shot meets it there.
 */
bool VoxelUnitModel::FallShape::hitAt(float across, float along, float height) const
{
	const int x = (int)std::floor(across) - hitFrom[0], y = (int)std::floor(along) - hitFrom[1], z = (int)std::floor(height) - hitFrom[2];
	if (hits.empty() || x < 0 || y < 0 || z < 0 || x >= hitSize[0] || y >= hitSize[1] || z >= hitSize[2]) return false;
	return hits[((size_t)x * hitSize[1] + y) * hitSize[2] + z] != 0;
}
#endif

/**
 * Writes a body into files (see VoxelUnits::dump): name.points, name.walk and name.fall.
 * @param model The body.
 * @param base The files' name, without its ending.
 */
void VoxelBodies::writeModel(const VoxelUnitModel &model, const std::string &base)
{
	const size_t count = model.part.size();
	if (FILE *file = std::fopen((base + ".points").c_str(), "w"))
	{
		for (size_t k = 0; k < count; ++k)
		{
			std::fprintf(file, "%.2f %.2f %.2f %d", model.right[k], model.ahead[k], model.up[k], model.part[k]);
			for (int slot = 0; slot < 16; ++slot) std::fprintf(file, " %d", model.colours[k * 16 + slot]);
			std::fprintf(file, " %d %d\n", model.views.empty() ? 0xFFFF : model.views[k], k >= model.addedFrom);
		}
		std::fclose(file);
	}
	if (FILE *file = std::fopen((base + ".walk").c_str(), "w"))
	{
		for (int phase = 0; phase < 8; ++phase)
			for (size_t k = 0; k < count; ++k) std::fprintf(file, "%d %.2f %.2f %.2f\n", phase, model.walkRight[phase][k], model.walkAhead[phase][k], model.walkUp[phase][k]);
		std::fclose(file);
	}
	// (each of the fall's shapes as a body of its own: fall0, fall1, ... in the .points' form)
	for (size_t shape = 0; shape < model.falls.size(); ++shape)
	{
		if (FILE *file = std::fopen((base + ".fall" + std::to_string(shape) + ".points").c_str(), "w"))
		{
			const VoxelUnitModel::FallShape &fall = model.falls[shape];
			for (size_t k = 0; k < fall.colour.size(); ++k)
			{
				std::fprintf(file, "%.2f %.2f %.2f %d", fall.right[k], fall.ahead[k], fall.up[k], VoxelUnitModel::Torso);
				for (int slot = 0; slot < 16; ++slot) std::fprintf(file, " %d", fall.colour[k]);
				std::fprintf(file, " %d 0\n", 0xFFFF);
			}
			std::fclose(file);
		}
	}
}

}
// Thanks mom and dad! You were the ones who exposed me to my first comp.
