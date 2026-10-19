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
 */
#include "FirstPersonView.h"
#include "FirstPersonViewShared.h"

namespace OpenXcom
{

namespace
{

/**
 * Everything a ray needs to know about this frame's world: the tile snapshot, the doors that
 * are swinging, the map's size in tiles and how far the view reaches.
 */
struct RayContext
{
	const FirstPersonView::TileView *tiles;   // one per tile, x fastest, then y, then z (like the game's tile index)
	const FirstPersonView::DoorSwing *swings; // the doors swinging open this frame (TileView::swing indexes them)
	int size[3];                              // the map's size in tiles: x, y, levels
	float maxDistance;                        // how far rays go, in voxels
};

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

	// The voxel of the closed door at that point (its first two layers, the door leaf); its gaps let the ray through.
	const int alongVoxel = std::clamp((int)(originAlong + directionAlong * entry) + 1, 0, 15);
	const int heightVoxel = std::clamp((int)(rayOrigin[2] + rayDirection[2] * entry - door.z0) + door.model->min[2], 0, 23);
	Uint8 color = 0;
	for (int layer = 0; layer < 2 && !color; ++layer)
	{
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
	if (face == 2) hit.axis = 2;
	else if (face == 1) hit.axis = std::abs(acrossX) > std::abs(acrossY) ? 0 : 1;
	else hit.axis = std::abs(alongX) > std::abs(alongY) ? 0 : 1;
	return true;
}

/// How a ray's walk ended: it hit something, or it left the map through a side, the top or
/// the bottom, or it went as far as the view reaches.
enum RayEnd { RAY_HIT, RAY_SIDE, RAY_TOP, RAY_BOTTOM, RAY_FAR };

/**
 * Walks the voxels of one tile between two distances along the ray, looking for the first solid
 * one (window glass is seen through, but noted). Swinging doors reaching into the tile are
 * traced first; anything solid in front of a door hides it.
 * @param tileView The tile's snapshot.
 * @param tileCell The tile's coordinates (x, y, level).
 * @param segmentStart, segmentEnd The part of the ray inside this tile.
 * @param enterAxis Axis of the face the ray crossed to enter the tile, -1 at the eye.
 * @return True if something was hit (the hit is filled in).
 */
inline bool traceTile(const FirstPersonView::TileView &tileView, const FirstPersonView::DoorSwing *swings, const int tileCell[3], const float rayOrigin[3], const float rayDirection[3],
	float segmentStart, float segmentEnd, int enterAxis, FirstPersonView::Hit &hit)
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
			const float inverse = 1.0f / rayDirection[axis];
			float nearCrossing = (low[axis] - rayOrigin[axis]) * inverse;
			float farCrossing = (high[axis] + 1 - rayOrigin[axis]) * inverse;
			if (nearCrossing > farCrossing) std::swap(nearCrossing, farCrossing);
			if (nearCrossing > entry) { entry = nearCrossing; faceAxis = axis; }
			if (farCrossing < exit) exit = farCrossing;
		}
		else if (rayOrigin[axis] < low[axis] || rayOrigin[axis] >= high[axis] + 1)
		{
			return false;
		}
	}
	if (entry > exit)
	{
		return doorHit;
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
			crossingSpacing[axis] = 1.0f / rayDirection[axis];
			nextCrossing[axis] = (voxel[axis] + 1 - rayOrigin[axis]) / rayDirection[axis];
		}
		else if (rayDirection[axis] < 0.0f)
		{
			step[axis] = -1;
			crossingSpacing[axis] = -1.0f / rayDirection[axis];
			nextCrossing[axis] = (voxel[axis] - rayOrigin[axis]) / rayDirection[axis];
		}
		else
		{
			step[axis] = 0;
			crossingSpacing[axis] = Infinity;
			nextCrossing[axis] = Infinity;
		}
	}

	// Walk voxel by voxel until something solid, or out of the box.
	float distance = entry;
	while (true)
	{
		// This voxel, counted from the tile's corner.
		const int localX = voxel[0] - tileCell[0] * Position::TileXY;
		const int localY = voxel[1] - tileCell[1] * Position::TileXY;
		const int localZ = voxel[2] - tileCell[2] * Position::TileZ;
		// Is it solid in any of the tile's four parts (floor, west wall, north wall, object)?
		for (int part = 0; part < 4; ++part)
		{
			const FirstPersonView::VoxelModel *model = tileView.part[part];
			if (!model) continue;
			const Uint8 color = model->color[localZ][localY][localX];
			if (!color) continue;
			if (model->glass[localZ][localY] & (1 << localX))
			{
				hit.glass = true; // window glass: tint what's behind it
				continue;
			}
			hit.color = color;
			hit.t = distance;
			hit.axis = faceAxis;
			hit.part = part;
			hit.voxel[0] = (Uint8)localX;
			hit.voxel[1] = (Uint8)localY;
			hit.voxel[2] = (Uint8)localZ;
			// A light source shines where its brightest shades are, in the part of it that glows.
			hit.emissive = ((tileView.emissive >> part) & 1) && (color & 15) <= 3 && localZ >= model->glowMinZ;
			return true;
		}
		// Step into the neighbouring voxel across the nearest boundary.
		const int nextAxis = nextCrossing[0] < nextCrossing[1] ? (nextCrossing[0] < nextCrossing[2] ? 0 : 2) : (nextCrossing[1] < nextCrossing[2] ? 1 : 2);
		distance = nextCrossing[nextAxis];
		if (distance > exit) return doorHit;
		voxel[nextAxis] += step[nextAxis];
		if (voxel[nextAxis] < low[nextAxis] || voxel[nextAxis] > high[nextAxis]) return doorHit;
		nextCrossing[nextAxis] += crossingSpacing[nextAxis];
		faceAxis = nextAxis;
	}
}

/**
 * Casts one ray: walks the grid of tiles from the eye, and looks inside each tile that has
 * anything in it (traceTile) until something is hit or the ray leaves the map.
 * @param rayOrigin The eye, in voxels.
 * @param rayDirection The ray's direction, in voxel units (see the top of this file).
 * @param hit Filled in when something is hit.
 * @param endDistance, endAxis Where and through which face the ray left the map (when nothing was hit).
 * @return How the ray ended.
 */
inline RayEnd castRay(const RayContext &context, const float rayOrigin[3], const float rayDirection[3], FirstPersonView::Hit &hit, float &endDistance, int &endAxis)
{
	hit.glass = false;
	hit.emissive = false;

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

	// Walk tile by tile, as far as the view reaches.
	float distance = 0.0f;
	int enterAxis = -1;
	while (distance < context.maxDistance)
	{
		// Look inside this tile, between where the ray came in and where it leaves.
		const int tileIndex = (tileCell[2] * context.size[1] + tileCell[1]) * context.size[0] + tileCell[0];
		const auto &tileView = context.tiles[tileIndex];
		const float tileExit = std::min(nextCrossing[0], std::min(nextCrossing[1], nextCrossing[2]));
		if (!tileView.empty && traceTile(tileView, context.swings, tileCell, rayOrigin, rayDirection, distance, tileExit, enterAxis, hit))
		{
			hit.tile = tileIndex;
			return RAY_HIT;
		}
		// Step into the neighbouring tile across the nearest boundary; past the map's edge, the
		// ray is done (the caller draws the ground beyond, the sky or the void).
		const int nextAxis = nextCrossing[0] < nextCrossing[1] ? (nextCrossing[0] < nextCrossing[2] ? 0 : 2) : (nextCrossing[1] < nextCrossing[2] ? 1 : 2);
		distance = nextCrossing[nextAxis];
		tileCell[nextAxis] += step[nextAxis];
		if (tileCell[nextAxis] < 0 || tileCell[nextAxis] >= context.size[nextAxis])
		{
			endDistance = distance;
			endAxis = nextAxis;
			return nextAxis == 2 ? (step[nextAxis] < 0 ? RAY_BOTTOM : RAY_TOP) : RAY_SIDE;
		}
		nextCrossing[nextAxis] += crossingSpacing[nextAxis];
		enterAxis = nextAxis;
	}
	endDistance = distance;
	endAxis = -1;
	return RAY_FAR;
}

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
