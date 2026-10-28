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
 * FirstPersonViewRayWalk.h: the heart of the renderer, the walk of one ray through the voxel
 * world. It steps tile by tile across the map, and voxel by voxel only inside tiles that have
 * something in them, until it hits a solid voxel (or a swinging door) or leaves the map.
 * Only the FirstPersonView*.cpp files that cast rays include it.
 *
 * How to read the distances: a ray is rayOrigin + rayDirection * distance, in voxel units
 * (16 voxels across a tile, 24 up). "Distance" along the ray is that multiplier, so the walk
 * compares distances where each axis's next boundary is crossed (a DDA, "digital differential
 * analyzer": the classic grid walk of Amanatides and Woo), once on the grid of tiles and once on
 * the grid of voxels inside a tile.
 *
 * This is the hottest code of the renderer: every pixel's ray, and every shadow ray, runs it.
 * What keeps it quick (each shortcut gives exactly what the plain walk would):
 *  - One byte per tile (RayContext::tileFlags, made by snapshotTiles) says whether there is
 *    anything in the tile, smoke on it, a door swinging through it, and how high its highest
 *    solid voxel is. Most tiles a ray crosses are air, or a floor it passes over: the walk steps
 *    on reading only that byte.
 *  - In a tile with something in it, the ray is first clipped to the box round its solid voxels
 *    (most tiles are mostly air too).
 *  - In the box, one bit per voxel (TileView::solid, the parts' voxels together) says whether
 *    any part has anything there before the parts themselves are looked at.
 * The data a ray reads is kept small and together for the processor's caches; anything a ray
 * doesn't need belongs elsewhere (TileView holds only what rays and shading read).
 */
#include "FirstPersonView.h"
#include "FirstPersonViewShared.h"

namespace OpenXcom
{

namespace
{

/**
 * Everything a ray needs to know about this frame's world: the tile snapshot, a byte per tile
 * saying whether there is anything to look at in it, the doors that are swinging, the map's size
 * in tiles and how far the view reaches; and for the view's own rays, the pixel's threshold for
 * ghosts (see ghostOpacity).
 */
struct RayContext
{
	const FirstPersonView::TileView *tiles;   // one per tile, x fastest, then y, then z (like the game's tile index)
	const Uint8 *tileFlags;                   // one per tile, the same way: FirstPersonView::TileFlag bits and the top (see snapshotTiles)
	const FirstPersonView::DoorSwing *swings; // the doors swinging open this frame (TileView::swing indexes them)
	int size[3];                              // the map's size in tiles: x, y, levels
	float maxDistance;                        // how far rays go, in voxels
	float ghostThreshold = 2.0f;              // a ghost stops the ray where its opacity is above this (the pixel's dither threshold); above 1, never (shadows, aim, flares)
};

// How solid a ghost looks (see VoxelModel::ghostly): on what share of the pixels it shows,
// GhostNearOpacity up close (GhostNearDistance voxels, a tile and a half), fading to
// GhostFarOpacity from GhostFarDistance (seven tiles) on, so it's plain up close and doesn't
// clutter the distance. Not solid by default: up close too it's a mesh to see through, as a unit
// above or below a lift pad is a threat to watch (a player's report on 1.03; it was solid up
// close). The setting "Ghost opacity" scales both (50%, as here; 100% solid up close: see
// renderRows).
const float GhostNearDistance = 24.0f, GhostFarDistance = 112.0f, GhostNearOpacity = 0.5f, GhostFarOpacity = 0.3f;

/**
 * The share of the pixels a ghost shows on at a distance along the ray (in voxels):
 * GhostNearOpacity up close, GhostFarOpacity far off, straight in between. The view's rays meet
 * a ghost voxel only where this is above the pixel's dither threshold
 * (RayContext::ghostThreshold), so it shows as a screen of dots that thickens a little as the
 * soldier comes closer.
 */
inline float ghostOpacity(float distance)
{
	const float fade = (distance - GhostNearDistance) / (GhostFarDistance - GhostNearDistance);
	return GhostNearOpacity - std::clamp(fade, 0.0f, 1.0f) * (GhostNearOpacity - GhostFarOpacity);
}

/**
 * Clips a ray's segment to a slab: the space between two parallel planes across one axis.
 * Narrows [segmentStart, segmentEnd] to the part inside the slab and remembers through which
 * face the ray came in (the face shading needs it).
 * @param origin The ray's start, along this axis.
 * @param direction The ray's direction, along this axis.
 * @param low, high The slab's planes, along this axis.
 * @param axis Which axis this is (stored in enterAxis if the ray enters through this slab's face).
 * @return False if nothing of the segment is left inside.
 */
inline bool slab(float origin, float direction, float low, float high, float &segmentStart, float &segmentEnd, int axis, int &enterAxis)
{
	// A ray running parallel to the planes is either between them all along, or never.
	if (direction == 0.0f)
	{
		return origin >= low && origin < high;
	}
	// Where the ray crosses the two planes, nearer first.
	float nearCrossing = (low - origin) / direction, farCrossing = (high - origin) / direction;
	if (nearCrossing > farCrossing) std::swap(nearCrossing, farCrossing);
	// Keep only the part of the segment between the crossings.
	if (nearCrossing > segmentStart) { segmentStart = nearCrossing; enterAxis = axis; }
	if (farCrossing < segmentEnd) segmentEnd = farCrossing;
	return segmentStart <= segmentEnd;
}

/**
 * Traces a ray against a door swinging open: a slab 16 voxels long and 2 thick, turned around
 * its hinge by the current angle, coloured from the closed door's voxel model.
 * @return True if the ray hits the door between segmentStart and segmentEnd (the hit is filled in).
 */
inline bool traceSwing(const FirstPersonView::DoorSwing &door, const float rayOrigin[3], const float rayDirection[3], float segmentStart, float segmentEnd, FirstPersonView::Hit &hit)
{
	// Work in the door's own frame: "along" runs from the hinge along the door, "across" through it.
	const float alongX = door.dir[0], alongY = door.dir[1], acrossX = -alongY, acrossY = alongX;
	const float fromHingeX = rayOrigin[0] - door.hinge[0], fromHingeY = rayOrigin[1] - door.hinge[1];
	const float originAlong = fromHingeX * alongX + fromHingeY * alongY, originAcross = fromHingeX * acrossX + fromHingeY * acrossY;
	const float directionAlong = rayDirection[0] * alongX + rayDirection[1] * alongY, directionAcross = rayDirection[0] * acrossX + rayDirection[1] * acrossY;

	// Clip the ray to the door's box: along it, through its thickness, and its height.
	float entry = segmentStart, exit = segmentEnd;
	int face = -1;
	if (!slab(originAlong, directionAlong, -1.0f, 15.0f, entry, exit, 0, face) || !slab(originAcross, directionAcross, -1.0f, 1.0f, entry, exit, 1, face)
		|| !slab(rayOrigin[2], rayDirection[2], door.z0, door.z1, entry, exit, 2, face))
	{
		return false;
	}

	// The voxel of the closed door at that point (its first two layers, the door leaf); its gaps let
	// the ray through, and so do its panes, the voxels behind a hole in its art (a glass door's, see
	// findArtHoles), as they do while it's shut (a player's report: the gas station's glass door
	// went solid while it swung open).
	const int alongVoxel = std::clamp((int)(originAlong + directionAlong * entry) + 1, 0, 15);
	const int heightVoxel = std::clamp((int)(rayOrigin[2] + rayDirection[2] * entry - door.z0) + door.model->min[2], 0, 23);
	Uint8 color = 0;
	for (int layer = 0; layer < 2 && !color; ++layer)
	{
		const bool pane = door.westWall ? (door.model->clear[heightVoxel][alongVoxel] >> layer) & 1 : (door.model->clear[heightVoxel][layer] >> alongVoxel) & 1;
		if (pane) continue;
		color = door.westWall ? door.model->color[heightVoxel][alongVoxel][layer] : door.model->color[heightVoxel][layer][alongVoxel];
	}
	if (!color)
	{
		return false;
	}

	// A hit: shade it by the world axis closest to the face the ray hit.
	hit.color = color;
	hit.t = entry;
	hit.part = -1;
	hit.ghost = false;
	if (face == 2) hit.axis = 2;
	else if (face == 1) hit.axis = std::abs(acrossX) > std::abs(acrossY) ? 0 : 1;
	else hit.axis = std::abs(alongX) > std::abs(alongY) ? 0 : 1;
	return true;
}

/// How a ray's walk ended: it hit something, or it left the map through a side, the top or
/// the bottom, or it went as far as the view reaches.
enum RayEnd { RAY_HIT, RAY_SIDE, RAY_TOP, RAY_BOTTOM, RAY_FAR, RAY_SMOKE };

/**
 * The smoke along a ray, by the game's sight rules (setting "Smoke": Modern). In the game
 * every voxel a line of sight walks through a tile's smoke uses up smoke / 3 voxels of the
 * soldier's sight (half as much within two tiles of the eye; less with heat vision), and what
 * is further than the sight left can't be seen. Where the ray's smoke has used up all the sight
 * there is left, it ends in the smoke (RAY_SMOKE): too thick to see through, a wall of smoke.
 * Short of that, the smoke is drawn see-through over what the ray hits, as the game's own smoke
 * (see FirstPersonView::drawSeeThroughSmoke), and the wall shows through it where the smoke has
 * used up most of the sight (see FirstPersonView::renderRows), so its edge is soft.
 */
struct SmokeTrace
{
	float sightVoxels = 0.0f;    // in: the soldier's sight range, voxels
	float smokeBlock = 0.0f;     // in: sight used up per voxel walked through 1 point of smoke
	float fireBlock = 0.0f;      // in: the same through the smoke of a burning tile
	float cost = 0.0f;           // out: the sight the smoke used up along the ray, voxels
	float lastSmoke = 0.0f;      // out: how far along the ray the last smoke on it ended
	float wallAt = 0.0f;         // out (RAY_SMOKE): where the ray ended in the smoke
	Uint8 light = 15;            // out: the light of the thickest smoke on the way, to shade it
	float thickest = 0.0f;       // (the sight a voxel of that smoke uses up)
};

/// Within this many voxels of the eye, smoke uses up half as much sight (two tiles, as in the game).
const float SmokeNearEye = 32.0f;

/**
 * Adds a stretch of one tile's smoke to a ray's SmokeTrace, and finds where (if anywhere on the
 * stretch) the smoke so far has used up all the sight left.
 * @param smoke The trace.
 * @param tileView The tile, with its smoke.
 * @param from, to The stretch of the ray, as distances along it.
 * @return Where the sight runs out on the stretch, or a negative number if it doesn't.
 */
inline float addSmoke(SmokeTrace &smoke, const FirstPersonView::TileView &tileView, float from, float to)
{
	const float perVoxel = tileView.smoke * (tileView.burning ? smoke.fireBlock : smoke.smokeBlock);
	if (perVoxel <= 0.0f || to <= from)
	{
		return -1.0f;
	}
	if (perVoxel > smoke.thickest)
	{
		smoke.thickest = perVoxel;
		smoke.light = tileView.light;
	}
	smoke.lastSmoke = to;
	// The stretch in up to two pieces: within SmokeNearEye of the eye (half the rate), and beyond.
	const float pieces[3] = { from, std::clamp(SmokeNearEye, from, to), to };
	for (int piece = 0; piece < 2; ++piece)
	{
		const float start = pieces[piece], end = pieces[piece + 1];
		if (end <= start) continue;
		const float rate = piece == 0 ? perVoxel * 0.5f : perVoxel;
		// The sight used up at distance t is cost + rate * (t - start); what's left there is
		// sightVoxels - t. They meet at t = (sightVoxels - cost + rate * start) / (1 + rate).
		const float meet = std::max(start, (smoke.sightVoxels - smoke.cost + rate * start) / (1.0f + rate));
		if (meet <= end)
		{
			smoke.cost += rate * (meet - start);
			return meet;
		}
		smoke.cost += rate * (end - start);
	}
	return -1.0f;
}

/**
 * Walks the voxels of one tile between two distances along the ray, looking for the first solid
 * one (window glass is seen through, but noted; a ghost too, on some pixels). Swinging doors
 * reaching into the tile are traced first; anything solid in front of a door hides it.
 *
 * Most tiles a ray looks into are mostly air (a floor, a wall along one side), so the ray is
 * first clipped to the box round the tile's solid voxels: two out of three tiles end there. In
 * the box it steps voxel by voxel; one bit per voxel (the tile's solid mask, the parts' voxels
 * together) says whether there is anything there at all before the parts are looked at.
 * @param tileView The tile's snapshot.
 * @param tileCell The tile's coordinates (x, y, level).
 * @param inverseDirection 1 / rayDirection on each axis (worked out once per ray; unused on an
 *        axis the ray runs parallel to).
 * @param segmentStart, segmentEnd The part of the ray inside this tile.
 * @param enterAxis Axis of the face the ray crossed to enter the tile, -1 at the eye.
 * @param ghostThreshold A ghost's voxel stops the ray only where its opacity is above this (see
 *        RayContext::ghostThreshold).
 * @return True if something was hit (the hit is filled in).
 */
inline bool traceTile(const FirstPersonView::TileView &tileView, const FirstPersonView::DoorSwing *swings, const int tileCell[3], const float rayOrigin[3], const float rayDirection[3],
	const float inverseDirection[3], float segmentStart, float segmentEnd, int enterAxis, float ghostThreshold, FirstPersonView::Hit &hit)
{
	// Doors swinging through this tile: the nearest hit shortens the segment, so only what's in
	// front of the door can still win.
	bool doorHit = false;
	for (Sint16 swing : tileView.swing)
	{
		if (swing >= 0 && traceSwing(swings[swing], rayOrigin, rayDirection, segmentStart, segmentEnd, hit))
		{
			doorHit = true;
			segmentEnd = hit.t;
		}
	}
	if (tileView.max[0] < tileView.min[0])
	{
		return doorHit; // nothing else in the tile
	}

	// Clip the segment to the box around the solid voxels in this tile: most tiles are mostly air.
	// The axes in order (so a tie between two faces goes the same way every time), stopping as
	// soon as nothing of the segment is left: a later axis can only shorten it further.
	int low[3], high[3];
	float entry = segmentStart, exit = segmentEnd;
	int faceAxis = enterAxis;
	for (int axis = 0; axis < 3; ++axis)
	{
		const int tileStart = tileCell[axis] * (int)CellSize[axis];
		low[axis] = tileStart + tileView.min[axis];
		high[axis] = tileStart + tileView.max[axis];
		if (rayDirection[axis] != 0.0f)
		{
			float nearCrossing = (low[axis] - rayOrigin[axis]) * inverseDirection[axis];
			float farCrossing = (high[axis] + 1 - rayOrigin[axis]) * inverseDirection[axis];
			if (nearCrossing > farCrossing) std::swap(nearCrossing, farCrossing);
			if (nearCrossing > entry) { entry = nearCrossing; faceAxis = axis; }
			if (farCrossing < exit) exit = farCrossing;
			if (entry > exit) return doorHit;
		}
		else if (rayOrigin[axis] < low[axis] || rayOrigin[axis] >= high[axis] + 1)
		{
			return doorHit; // running alongside the box, outside it
		}
	}

	// Set up the voxel walk: the voxel where the ray enters the box, which way it steps on each
	// axis, how far along the ray one voxel is on each axis, and where it next crosses a voxel
	// boundary on each axis.
	int voxel[3], step[3];
	float nextCrossing[3], crossingSpacing[3];
	for (int axis = 0; axis < 3; ++axis)
	{
		const float entryPoint = rayOrigin[axis] + rayDirection[axis] * (entry + 1e-3f);
		voxel[axis] = std::clamp((int)std::floor(entryPoint), low[axis], high[axis]);
		if (rayDirection[axis] > 0.0f)
		{
			step[axis] = 1;
			crossingSpacing[axis] = inverseDirection[axis];
			nextCrossing[axis] = (voxel[axis] + 1 - rayOrigin[axis]) / rayDirection[axis];
		}
		else if (rayDirection[axis] < 0.0f)
		{
			step[axis] = -1;
			crossingSpacing[axis] = -inverseDirection[axis];
			nextCrossing[axis] = (voxel[axis] - rayOrigin[axis]) / rayDirection[axis];
		}
		else
		{
			step[axis] = 0;
			crossingSpacing[axis] = Infinity;
			nextCrossing[axis] = Infinity;
		}
	}

	// Walk voxel by voxel until something solid, or out of the box. One step across a voxel
	// boundary on an axis: false when that leaves the segment or the box. (Only ever called with
	// a fixed axis, for the same reason as the tile walk's, see castRay.)
	float distance = entry;
	auto stepAcross = [&](const int axis) {
		distance = nextCrossing[axis];
		if (distance > exit) return false;
		voxel[axis] += step[axis];
		if (voxel[axis] < low[axis] || voxel[axis] > high[axis]) return false;
		nextCrossing[axis] += crossingSpacing[axis];
		faceAxis = axis;
		return true;
	};
	while (true)
	{
		// This voxel, counted from the tile's corner.
		const int localX = voxel[0] - tileCell[0] * Position::TileXY;
		const int localY = voxel[1] - tileCell[1] * Position::TileXY;
		const int localZ = voxel[2] - tileCell[2] * Position::TileZ;
		// Is it solid in any of the tile's four parts (floor, west wall, north wall, object)?
		// The tile's solid mask (the parts' voxels together) says first whether any of them has
		// anything there: most voxels a ray steps through are air.
		if ((tileView.solid[localZ][localY] >> localX) & 1)
		{
			for (int part = 0; part < 4; ++part)
			{
				const FirstPersonView::VoxelModel *model = tileView.part[part];
				if (!model) continue;
				Uint8 color = model->color[localZ][localY][localX];
				// Where the part has no voxel, its own ghost may (VoxelModel::ghost: a liquid's surface
				// over a floor, the rest of a fence's or a grating's art).
				const bool onGhost = !color && model->ghost;
				if (onGhost)
				{
					model = model->ghost;
					color = model->color[localZ][localY][localX];
				}
				if (!color) continue;
				if (model->ghostly && ghostOpacity(distance) <= ghostThreshold)
				{
					continue; // a ghost, seen through on this pixel
				}
				if (model->glass[localZ][localY] & (1 << localX))
				{
					hit.glass = true; // window glass: tint what's behind it
					continue;
				}
				if (model->clear[localZ][localY] & (1 << localX))
				{
					continue; // behind a hole in the art (a glass door's pane): seen through, as on the map
				}
				hit.color = color;
				hit.t = distance;
				hit.axis = faceAxis;
				hit.part = part;
				hit.ghost = onGhost;
				hit.voxel[0] = (Uint8)localX;
				hit.voxel[1] = (Uint8)localY;
				hit.voxel[2] = (Uint8)localZ;
				// A light source shines where its brightest shades are, in the part of it that glows;
				// a steady UFO model where its lights are (see buildUfoModel).
				hit.emissive = !onGhost && ((tileView.emissive >> part) & 1)
					&& (model->lit ? ((model->lit[localZ][localY] >> localX) & 1) != 0 : (color & 15) <= 3 && localZ >= model->glowMinZ);
				return true;
			}
		}
		// Step into the neighbouring voxel across the nearest boundary (on a tie, the later axis).
		const bool inBox = nextCrossing[0] < nextCrossing[1] ? (nextCrossing[0] < nextCrossing[2] ? stepAcross(0) : stepAcross(2))
			: (nextCrossing[1] < nextCrossing[2] ? stepAcross(1) : stepAcross(2));
		if (!inBox)
		{
			return doorHit;
		}
	}
}

/**
 * Casts one ray: walks the grid of tiles from the eye, and looks inside each tile that has
 * anything in it (traceTile) until something is hit or the ray leaves the map.
 * @param rayOrigin The eye, in voxels.
 * @param rayDirection The ray's direction, in voxel units (see the top of this file).
 * @param hit Filled in when something is hit.
 * @param endDistance, endAxis Where and through which face the ray left the map (when nothing was hit).
 * @param smoke Null, or the smoke to trace along the ray (see SmokeTrace): the ray may then end
 *        in thick smoke (RAY_SMOKE).
 * @return How the ray ended.
 */
inline RayEnd castRay(const RayContext &context, const float rayOrigin[3], const float rayDirection[3], FirstPersonView::Hit &hit, float &endDistance, int &endAxis,
	SmokeTrace *smoke = nullptr)
{
	hit.glass = false;
	hit.emissive = false;
	hit.ghost = false;
	// 1 / direction on each axis, for the tiles' boxes (an axis the ray runs parallel to never
	// uses it).
	const float inverseDirection[3] = { rayDirection[0] != 0.0f ? 1.0f / rayDirection[0] : 0.0f,
		rayDirection[1] != 0.0f ? 1.0f / rayDirection[1] : 0.0f, rayDirection[2] != 0.0f ? 1.0f / rayDirection[2] : 0.0f };

	// Set up the tile walk: the eye's tile, which way the ray steps on each axis, how far along
	// the ray one tile is on each axis, and where it next crosses a tile boundary on each axis.
	int tileCell[3], step[3];
	float nextCrossing[3], crossingSpacing[3];
	for (int axis = 0; axis < 3; ++axis)
	{
		const float positionInTiles = rayOrigin[axis] / CellSize[axis];
		tileCell[axis] = (int)std::floor(positionInTiles);
		// An eye outside the map (high above it, flying) sees nothing of it this way.
		if (tileCell[axis] < 0 || tileCell[axis] >= context.size[axis])
		{
			endDistance = 0.0f;
			endAxis = axis;
			return axis == 2 ? (tileCell[axis] < 0 ? RAY_BOTTOM : RAY_TOP) : RAY_SIDE;
		}
		const float directionInTiles = rayDirection[axis] / CellSize[axis];
		if (directionInTiles > 0.0f)
		{
			step[axis] = 1;
			crossingSpacing[axis] = 1.0f / directionInTiles;
			nextCrossing[axis] = (tileCell[axis] + 1 - positionInTiles) / directionInTiles;
		}
		else if (directionInTiles < 0.0f)
		{
			step[axis] = -1;
			crossingSpacing[axis] = -1.0f / directionInTiles;
			nextCrossing[axis] = (positionInTiles - tileCell[axis]) / -directionInTiles;
		}
		else
		{
			step[axis] = 0;
			crossingSpacing[axis] = Infinity;
			nextCrossing[axis] = Infinity;
		}
	}

	// Walk tile by tile, as far as the view reaches. Most tiles on the way are empty air: one
	// byte per tile (the flags, see snapshotTiles) says so, and the walk steps on without
	// touching the tile's snapshot.
	const Uint8 wanted = smoke ? FirstPersonView::TileTraced | FirstPersonView::TileSmoky : FirstPersonView::TileTraced;
	float distance = 0.0f;
	int enterAxis = -1;
	int tileIndex = (tileCell[2] * context.size[1] + tileCell[1]) * context.size[0] + tileCell[0];
	const int indexStride[3] = { step[0], step[1] * context.size[0], step[2] * context.size[0] * context.size[1] }; // a step on each axis, in tiles of the index
	// One step across a tile boundary on an axis, into the next tile that way; false when that
	// leaves the map (noting where, and through which side). It is only ever called with a fixed
	// axis (see below), never one picked at run time: that lets the compiler keep the walk's
	// numbers in registers, instead of in arrays in memory it must store and load again every step.
	auto stepAcross = [&](const int axis) {
		distance = nextCrossing[axis];
		tileCell[axis] += step[axis];
		tileIndex += indexStride[axis];
		if (tileCell[axis] < 0 || tileCell[axis] >= context.size[axis])
		{
			endDistance = distance;
			endAxis = axis;
			return false;
		}
		nextCrossing[axis] += crossingSpacing[axis];
		enterAxis = axis;
		return true;
	};
	while (distance < context.maxDistance)
	{
		const Uint8 flags = context.tileFlags[tileIndex];
		if (flags & wanted)
		{
			// Where the ray leaves the tile. Anything solid to look for on the way? Not when it
			// passes above all of the tile's solid voxels (a floor, most often) and no door swings
			// through it: the tile's box would turn it away (see traceTile; a hundredth of a voxel
			// to spare, so rounding never decides it). And smoke to trace through?
			const float tileExit = std::min(nextCrossing[0], std::min(nextCrossing[1], nextCrossing[2]));
			const float solidTop = (float)(tileCell[2] * Position::TileZ + (flags >> FirstPersonView::TileTopShift) + 1) + 0.01f;
			const bool lookForSolid = (flags & FirstPersonView::TileTraced) && ((flags & FirstPersonView::TileSwinging)
				|| rayOrigin[2] + rayDirection[2] * distance <= solidTop || rayOrigin[2] + rayDirection[2] * tileExit <= solidTop);
			const bool traceSmokeHere = smoke && (flags & FirstPersonView::TileSmoky);
			if (lookForSolid || traceSmokeHere)
			{
				// Look inside this tile, between where the ray came in and where it leaves.
				const auto &tileView = context.tiles[tileIndex];
				// The smoke in the tile, when traced: where in the tile (if anywhere) it gets too
				// thick to see through. Only what is in front of that point can still be hit;
				// behind it, the ray ends in the smoke.
				float smokeWall = -1.0f;
				SmokeTrace smokeBefore;
				if (traceSmokeHere)
				{
					smokeBefore = *smoke;
					smokeWall = addSmoke(*smoke, tileView, distance, tileExit);
				}
				const float traceEnd = smokeWall >= 0.0f ? smokeWall : tileExit;
				if (lookForSolid && traceTile(tileView, context.swings, tileCell, rayOrigin, rayDirection, inverseDirection, distance, traceEnd, enterAxis, context.ghostThreshold, hit))
				{
					// Hit something in the tile: only the smoke in front of it counts.
					if (traceSmokeHere)
					{
						*smoke = smokeBefore;
						addSmoke(*smoke, tileView, distance, hit.t);
					}
					hit.tile = tileIndex;
					return RAY_HIT;
				}
				if (smokeWall >= 0.0f)
				{
					smoke->wallAt = smokeWall;
					return RAY_SMOKE;
				}
			}
		}
		// Step into the neighbouring tile across the nearest boundary (on a tie, the later axis);
		// past the map's edge, the ray is done (the caller draws the ground beyond, the sky or the
		// void).
		const bool onMap = nextCrossing[0] < nextCrossing[1] ? (nextCrossing[0] < nextCrossing[2] ? stepAcross(0) : stepAcross(2))
			: (nextCrossing[1] < nextCrossing[2] ? stepAcross(1) : stepAcross(2));
		if (!onMap)
		{
			return endAxis == 2 ? (step[2] < 0 ? RAY_BOTTOM : RAY_TOP) : RAY_SIDE;
		}
	}
	endDistance = distance;
	endAxis = -1;
	return RAY_FAR;
}

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
