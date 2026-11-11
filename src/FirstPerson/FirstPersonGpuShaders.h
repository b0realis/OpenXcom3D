#pragma once
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
 * FirstPersonGpuShaders.h: the GPU renderer's shaders (GLSL 3.30), as text.
 *
 * They are the CPU renderer's own functions, line for line, under the same names:
 *  - WalkSource: castRay, traceTile, traceSwing (FirstPersonViewRayWalk.h);
 *  - ShadeSource: shadeHit, lightAt, shadowed, colorOutsideMap, skyColor and what they call
 *    (FirstPersonViewRayCasting.cpp), and main, which is renderRows' loop body.
 * Read the C++ for the why; change one, change the other. The numbers both share (the
 * constants of FirstPersonViewShared.h) are written into the shader from the C++ ones when it
 * is built (see FirstPersonGpu.cpp, shaderConstants), never typed twice.
 * Only FirstPersonGpu.cpp includes this.
 */

namespace OpenXcom
{

namespace
{

const char *const VertexSource = R"(#version 330 core
// One triangle over the whole picture.
void main()
{
	vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char *const HeaderSource = R"(
uniform usamplerBuffer uTileFlags;  // a byte per tile: FirstPersonView::TileFlag bits and the top
uniform usamplerBuffer uTiles;      // four texels per tile (see FirstPersonGpu.cpp, encodeTiles)
uniform usamplerBuffer uModelColor; // 6144 bytes per model: [z][y][x]
uniform usamplerBuffer uModelBits;  // 4 x 384 rows per model: glass, lookOnly, clear, lit; [z][y], bit x
uniform usamplerBuffer uModelInfo;  // a texel per model: flags, its ghost's number, glowMinZ | ceiling | min z | pulseKind
uniform usamplerBuffer uModelSides; // 1024 bytes per model: plan [y][x], east [z][y], south [z][x]
uniform usamplerBuffer uSolid;      // 384 rows per solid mask: [z][y], bit x
uniform usamplerBuffer uModelExposed; // 6144 bytes per steady UFO model: each voxel's open faces (the model's info says which block)
uniform usamplerBuffer uWallPaints; // a texel per wall paint: the wall's model, the donor's, flags, the wall's min x | min y << 8
uniform isamplerBuffer uMirrors;    // the crafts' mirrors: sixteen numbers each, then their rows (see FirstPersonGpu.cpp, encodeMirrors)
uniform usamplerBuffer uTables;     // rows of 256 bytes: the haze steps, the tints, the scene's colours (see FirstPersonGpu.cpp, encodeTables)
uniform samplerBuffer uWaveSine;    // the waves' sine table

uniform ivec3 uSize;         // the map in tiles
uniform float uMaxDistance;  // how far rays go, in voxels
uniform vec3 uEye, uForward, uRight, uUp;
uniform vec2 uHalfPlane;     // the image plane's half-width and half-height
uniform ivec2 uViewSize;     // the view in pixels
uniform int uPixelSize;      // pixels a ray colours each way
uniform float uGhostScale;
uniform int uSeeThroughCrops;
uniform vec4 uSwingA[16];    // a swinging door: hinge x, y, direction x, y
uniform vec4 uSwingB[16];    // ... z0, z1, its model's number, 1 if it was a west wall

layout(location = 0) out uint outColor;   // the palette colour
layout(location = 1) out float outDepth;  // along the view axis
layout(location = 2) out uint outGlow;    // 1 where the pixel shines, 3 a UFO's light (for the bloom)
#ifdef RESULTS
layout(location = 3) out uvec4 outResult; // FirstPersonGpu::RayResult (the check against the CPU's rays only)
#endif

const float Infinity = uintBitsToFloat(0x7F800000u);
const vec3 CellSize = vec3(16.0, 16.0, 24.0);
const ivec3 CellVoxels = ivec3(16, 16, 24);
const int RAY_HIT = 0, RAY_SIDE = 1, RAY_TOP = 2, RAY_BOTTOM = 3, RAY_FAR = 4;
const float GhostNearDistance = 24.0, GhostFarDistance = 112.0, GhostNearOpacity = 0.5, GhostFarOpacity = 0.3;
const float Bayer[16] = float[16](0.0 / 16.0, 8.0 / 16.0, 2.0 / 16.0, 10.0 / 16.0, 12.0 / 16.0, 4.0 / 16.0, 14.0 / 16.0, 6.0 / 16.0,
	3.0 / 16.0, 11.0 / 16.0, 1.0 / 16.0, 9.0 / 16.0, 15.0 / 16.0, 7.0 / 16.0, 13.0 / 16.0, 5.0 / 16.0);
)";

const char *const WalkSource = R"(
struct Hit
{
	uint color;
	float t;
	int axis;
	int part;
	bool ghost;
	ivec3 voxel;
	bool glass;
	bool emissive;
};

float dither(int x, int y) { return Bayer[(y & 3) * 4 + (x & 3)]; }

float ghostOpacity(float span)
{
	float fade = (span - GhostNearDistance) / (GhostFarDistance - GhostNearDistance);
	return GhostNearOpacity - clamp(fade, 0.0, 1.0) * (GhostNearOpacity - GhostFarOpacity);
}

uint modelColor(uint model, ivec3 local)
{
	return texelFetch(uModelColor, int(model) * 6144 + (local.z * 16 + local.y) * 16 + local.x).r;
}

uint modelRow(uint model, int mask, ivec3 local)
{
	return texelFetch(uModelBits, int(model) * 1536 + mask * 384 + local.z * 16 + local.y).r;
}

bool slab(float origin, float direction, float low, float high, inout float segmentStart, inout float segmentEnd, int axis, inout int enterAxis)
{
	if (direction == 0.0)
	{
		return origin >= low && origin < high;
	}
	float nearCrossing = (low - origin) / direction, farCrossing = (high - origin) / direction;
	if (nearCrossing > farCrossing) { float swapped = nearCrossing; nearCrossing = farCrossing; farCrossing = swapped; }
	if (nearCrossing > segmentStart) { segmentStart = nearCrossing; enterAxis = axis; }
	if (farCrossing < segmentEnd) segmentEnd = farCrossing;
	return segmentStart <= segmentEnd;
}

bool traceSwing(int swing, vec3 rayOrigin, vec3 rayDirection, float segmentStart, float segmentEnd, inout Hit hit)
{
	vec4 doorA = uSwingA[swing], doorB = uSwingB[swing];
	uint model = uint(doorB.z);
	bool westWall = doorB.w != 0.0;
	float alongX = doorA.z, alongY = doorA.w, acrossX = -alongY, acrossY = alongX;
	float fromHingeX = rayOrigin.x - doorA.x, fromHingeY = rayOrigin.y - doorA.y;
	float originAlong = fromHingeX * alongX + fromHingeY * alongY, originAcross = fromHingeX * acrossX + fromHingeY * acrossY;
	float directionAlong = rayDirection.x * alongX + rayDirection.y * alongY, directionAcross = rayDirection.x * acrossX + rayDirection.y * acrossY;

	float entry = segmentStart, leave = segmentEnd;
	int face = -1;
	if (!slab(originAlong, directionAlong, -1.0, 15.0, entry, leave, 0, face) || !slab(originAcross, directionAcross, -1.0, 1.0, entry, leave, 1, face)
		|| !slab(rayOrigin.z, rayDirection.z, doorB.x, doorB.y, entry, leave, 2, face))
	{
		return false;
	}

	int modelMinZ = int((texelFetch(uModelInfo, int(model)).z >> 16) & 0xFFu);
	int alongVoxel = clamp(int(originAlong + directionAlong * entry) + 1, 0, 15);
	int heightVoxel = clamp(int(rayOrigin.z + rayDirection.z * entry - doorB.x) + modelMinZ, 0, 23);
	uint color = 0u;
	for (int layer = 0; layer < 2 && color == 0u; ++layer)
	{
		ivec3 local = westWall ? ivec3(layer, alongVoxel, heightVoxel) : ivec3(alongVoxel, layer, heightVoxel);
		bool pane = (((modelRow(model, 2, local) | modelRow(model, 0, local)) >> uint(local.x)) & 1u) != 0u;
		if (pane) continue;
		color = modelColor(model, local);
	}
	if (color == 0u)
	{
		return false;
	}

	hit.color = color;
	hit.t = entry;
	hit.part = -1;
	hit.ghost = false;
	if (face == 2) hit.axis = 2;
	else if (face == 1) hit.axis = abs(acrossX) > abs(acrossY) ? 0 : 1;
	else hit.axis = abs(alongX) > abs(alongY) ? 0 : 1;
	return true;
}

bool traceTile(int tileIndex, ivec3 tileCell, vec3 rayOrigin, vec3 rayDirection, vec3 inverseDirection, float segmentStart, float segmentEnd, int enterAxis,
	float ghostThreshold, float solidGhostThreshold, inout Hit hit)
{
	uvec4 tileA = texelFetch(uTiles, tileIndex * 4), tileB = texelFetch(uTiles, tileIndex * 4 + 1);
	// Doors swinging through this tile.
	bool doorHit = false;
	for (int which = 0; which < 2; ++which)
	{
		int swing = int((tileB.y >> uint(16 * which)) & 0xFFFFu) - 1;
		if (swing >= 0 && traceSwing(swing, rayOrigin, rayDirection, segmentStart, segmentEnd, hit))
		{
			doorHit = true;
			segmentEnd = hit.t;
		}
	}
	if ((tileA.z & 0x40000000u) == 0u)
	{
		return doorHit; // nothing else in the tile
	}

	// Clip the segment to the box around the solid voxels in this tile.
	ivec3 corner = tileCell * CellVoxels;
	ivec3 low = corner + ivec3(tileA.z & 31u, (tileA.z >> 5) & 31u, (tileA.z >> 10) & 31u);
	ivec3 high = corner + ivec3((tileA.z >> 15) & 31u, (tileA.z >> 20) & 31u, (tileA.z >> 25) & 31u);
	float entry = segmentStart, leave = segmentEnd;
	int faceAxis = enterAxis;
	for (int axis = 0; axis < 3; ++axis)
	{
		if (rayDirection[axis] != 0.0)
		{
			float nearCrossing = (float(low[axis]) - rayOrigin[axis]) * inverseDirection[axis];
			float farCrossing = (float(high[axis] + 1) - rayOrigin[axis]) * inverseDirection[axis];
			if (nearCrossing > farCrossing) { float swapped = nearCrossing; nearCrossing = farCrossing; farCrossing = swapped; }
			if (nearCrossing > entry) { entry = nearCrossing; faceAxis = axis; }
			if (farCrossing < leave) leave = farCrossing;
			if (entry > leave) return doorHit;
		}
		else if (rayOrigin[axis] < float(low[axis]) || rayOrigin[axis] >= float(high[axis] + 1))
		{
			return doorHit; // running alongside the box, outside it
		}
	}

	// Set up the voxel walk.
	ivec3 voxel, stride;
	vec3 nextCrossing, crossingSpacing;
	for (int axis = 0; axis < 3; ++axis)
	{
		float entryPoint = rayOrigin[axis] + rayDirection[axis] * (entry + 1e-3);
		voxel[axis] = clamp(int(floor(entryPoint)), low[axis], high[axis]);
		if (rayDirection[axis] > 0.0)
		{
			stride[axis] = 1;
			crossingSpacing[axis] = inverseDirection[axis];
			nextCrossing[axis] = (float(voxel[axis] + 1) - rayOrigin[axis]) / rayDirection[axis];
		}
		else if (rayDirection[axis] < 0.0)
		{
			stride[axis] = -1;
			crossingSpacing[axis] = -inverseDirection[axis];
			nextCrossing[axis] = (float(voxel[axis]) - rayOrigin[axis]) / rayDirection[axis];
		}
		else
		{
			stride[axis] = 0;
			crossingSpacing[axis] = Infinity;
			nextCrossing[axis] = Infinity;
		}
	}

	// Walk voxel by voxel until something solid, or out of the box (at most 16 + 16 + 24 steps).
	uint parts[4] = uint[4](tileA.x & 0xFFFFu, tileA.x >> 16, tileA.y & 0xFFFFu, tileA.y >> 16);
	int solidRows = int(tileA.w) * 384;
	float span = entry;
	for (int guard = 0; guard < 64; ++guard)
	{
		ivec3 local = voxel - corner;
		uint bit = 1u << uint(local.x);
		if ((texelFetch(uSolid, solidRows + local.z * 16 + local.y).r & bit) != 0u)
		{
			for (int part = 0; part < 4; ++part)
			{
				uint model = parts[part];
				if (model == 0u) continue;
				uint color = modelColor(model, local);
				uvec4 info = texelFetch(uModelInfo, int(model));
				bool onGhost = color == 0u && info.y != 0u;
				if (onGhost)
				{
					model = info.y;
					color = modelColor(model, local);
					info = texelFetch(uModelInfo, int(model));
				}
				if (color == 0u) continue;
				if ((info.x & 1u) != 0u && ((info.x & 2u) != 0u ? 1.0 <= solidGhostThreshold : ghostOpacity(span) <= ghostThreshold))
				{
					continue; // a ghost, seen through on this pixel
				}
				if ((modelRow(model, 0, local) & bit) != 0u)
				{
					hit.glass = true; // window glass: seen through, tinted
					continue;
				}
				if ((modelRow(model, 2, local) & bit) != 0u)
				{
					continue; // behind a hole in the art
				}
				hit.color = color;
				hit.t = span;
				hit.axis = faceAxis;
				hit.part = part;
				hit.ghost = onGhost;
				hit.voxel = local;
				hit.emissive = !onGhost && ((tileB.x >> uint(16 + part)) & 1u) != 0u
					&& ((info.x & 4u) != 0u ? (modelRow(model, 3, local) & bit) != 0u : (color & 15u) <= 3u && local.z >= int(info.z & 0xFFu));
				return true;
			}
		}
		// Step into the neighbouring voxel across the nearest boundary (on a tie, the later axis).
		int axis = nextCrossing.x < nextCrossing.y ? (nextCrossing.x < nextCrossing.z ? 0 : 2) : (nextCrossing.y < nextCrossing.z ? 1 : 2);
		span = nextCrossing[axis];
		if (span > leave) return doorHit;
		voxel[axis] += stride[axis];
		if (voxel[axis] < low[axis] || voxel[axis] > high[axis]) return doorHit;
		nextCrossing[axis] += crossingSpacing[axis];
		faceAxis = axis;
	}
	return doorHit;
}

int castRay(vec3 rayOrigin, vec3 rayDirection, float maxDistance, float ghostThreshold, float solidGhostThreshold, inout Hit hit, out int hitTile, out float endDistance, out int endAxis)
{
	hit.glass = false;
	hit.emissive = false;
	hit.ghost = false;
	hitTile = 0;
	endDistance = 0.0;
	endAxis = -1;
	vec3 inverseDirection = vec3(rayDirection.x != 0.0 ? 1.0 / rayDirection.x : 0.0, rayDirection.y != 0.0 ? 1.0 / rayDirection.y : 0.0,
		rayDirection.z != 0.0 ? 1.0 / rayDirection.z : 0.0);

	// Set up the tile walk.
	ivec3 tileCell, stride;
	vec3 nextCrossing, crossingSpacing;
	for (int axis = 0; axis < 3; ++axis)
	{
		float positionInTiles = rayOrigin[axis] / CellSize[axis];
		tileCell[axis] = int(floor(positionInTiles));
		if (tileCell[axis] < 0 || tileCell[axis] >= uSize[axis])
		{
			endDistance = 0.0;
			endAxis = axis;
			return axis == 2 ? (tileCell[axis] < 0 ? RAY_BOTTOM : RAY_TOP) : RAY_SIDE;
		}
		float directionInTiles = rayDirection[axis] / CellSize[axis];
		if (directionInTiles > 0.0)
		{
			stride[axis] = 1;
			crossingSpacing[axis] = 1.0 / directionInTiles;
			nextCrossing[axis] = (float(tileCell[axis] + 1) - positionInTiles) / directionInTiles;
		}
		else if (directionInTiles < 0.0)
		{
			stride[axis] = -1;
			crossingSpacing[axis] = -1.0 / directionInTiles;
			nextCrossing[axis] = (positionInTiles - float(tileCell[axis])) / -directionInTiles;
		}
		else
		{
			stride[axis] = 0;
			crossingSpacing[axis] = Infinity;
			nextCrossing[axis] = Infinity;
		}
	}

	// Walk tile by tile, as far as the ray reaches.
	float span = 0.0;
	int enterAxis = -1;
	int tileIndex = (tileCell.z * uSize.y + tileCell.y) * uSize.x + tileCell.x;
	ivec3 indexStride = ivec3(stride.x, stride.y * uSize.x, stride.z * uSize.x * uSize.y);
	for (int guard = 0; guard < 4096 && span < maxDistance; ++guard)
	{
		uint flags = texelFetch(uTileFlags, tileIndex).r;
		if ((flags & 1u) != 0u)
		{
			float tileExit = min(nextCrossing.x, min(nextCrossing.y, nextCrossing.z));
			float solidTop = float(tileCell.z * 24 + int(flags >> 3) + 1) + 0.01;
			bool lookForSolid = (flags & 4u) != 0u || rayOrigin.z + rayDirection.z * span <= solidTop || rayOrigin.z + rayDirection.z * tileExit <= solidTop;
			if (lookForSolid && traceTile(tileIndex, tileCell, rayOrigin, rayDirection, inverseDirection, span, tileExit, enterAxis, ghostThreshold, solidGhostThreshold, hit))
			{
				hitTile = tileIndex;
				return RAY_HIT;
			}
		}
		// Step into the neighbouring tile across the nearest boundary (on a tie, the later axis).
		int axis = nextCrossing.x < nextCrossing.y ? (nextCrossing.x < nextCrossing.z ? 0 : 2) : (nextCrossing.y < nextCrossing.z ? 1 : 2);
		span = nextCrossing[axis];
		tileCell[axis] += stride[axis];
		tileIndex += indexStride[axis];
		if (tileCell[axis] < 0 || tileCell[axis] >= uSize[axis])
		{
			endDistance = span;
			endAxis = axis;
			return axis == 2 ? (stride.z < 0 ? RAY_BOTTOM : RAY_TOP) : RAY_SIDE;
		}
		nextCrossing[axis] += crossingSpacing[axis];
		enterAxis = axis;
	}
	endDistance = span;
	endAxis = -1;
	return RAY_FAR;
}
)";

const char *const ShadeSource = R"(
// ---- The scene (FirstPersonView::SceneColors and the view's own state) ----
uniform int uSky;            // FirstPersonView::Sky
uniform int uUnderwater, uOvercast, uEnclosed, uSurfaceLevel, uAmbient;
uniform int uFogOfWar, uUnseenTerrain, uHaze, uShadows, uSmoothLighting, uDiscreetEdge; // the settings
uniform int uGroundModel, uWaterModel; // the models of the ground and the water beyond the map (0 none)
uniform float uNight, uCloudTime, uAnimationBlend;
uniform float uSightView, uSightMod; // the game's sight ranges, in voxels (the tile engine's, the mod's)
uniform float uFaceShade[6], uIndoorFaceShade[6];
uniform vec3 uSunDir, uMoonDir;
uniform float uWaveSteps[18]; // this frame's wave trains (FirstPersonView::_waveSteps)
uniform vec4 uLamps[16];      // the lamps nearby: position, strength
uniform int uLampCount;
uniform int uFlashlight;      // 0 off, 1 the shoulder flashlight, 2 the dynamic one
uniform vec3 uFlashlightPos, uForwardInVoxels;
uniform vec4 uFlares[8];      // the flares' lights: position, fade
uniform float uFlareFlicker[8];
uniform int uFlareCount;
uniform float uMuzzleFlashLight;
uniform vec3 uMuzzleFlashPos;
uniform int uUfoLightsOn, uWallPaint;
uniform float uUfoPulse[32], uUfoBreath, uUfoFaceShade[64];

const int SkyEarth = 0, SkySpace = 1, SkyOrbit = 2, SkyMoon = 3, SkyMars = 4, SkySea = 5, SkyVoid = 6;
// The rows of uTables.
const int HazeRow = 0, GlassRow = 9, UnseenRow = 10, FlareRow = 11, SceneRow = 12, BlackRow = 13, UfoRows = 14, RowsPerUfo = 14;
// Where the scene's colours sit in their row.
const int SceneSky = 0, SceneCloud = 32, SceneHaze = 40, SceneStar = 41, SceneMoon = 42, SceneSun = 43, SceneTapeYellow = 44, SceneTapeBlack = 45,
	ScenePost = 46, SceneFaintStar = 47, SceneEarth = 48, SceneUnexplored = 52, SceneRock = 53, SceneWater = 54, SceneRockBase = 57;
const int TapeFont[30] = int[30](5, 5, 2, 5, 5, 0, 0, 7, 0, 0, 7, 4, 4, 4, 7, 7, 5, 5, 5, 7, 5, 7, 7, 5, 5, 0, 0, 0, 0, 0);
const int TapeText[7] = int[7](0, 1, 2, 3, 4, 5, 5);
const int TapeTextLength = 7;

uint table(int row, uint color) { return texelFetch(uTables, row * 256 + int(color)).r; }
uint scene(int index) { return texelFetch(uTables, SceneRow * 256 + index).r; }
bool airless() { return uSky == SkySpace || uSky == SkyOrbit || uSky == SkyMoon; }

uint shadeColor(uint color, int shade)
{
	if (shade <= 0)
	{
		return color;
	}
	uint shaded = color + uint(shade);
	if (shaded > 255u || ((shaded ^ color) & 0xF0u) != 0u)
	{
		return 15u;
	}
	return shaded;
}

float smoothStep(float edgeLow, float edgeHigh, float value)
{
	float progress = clamp((value - edgeLow) / (edgeHigh - edgeLow), 0.0, 1.0);
	return progress * progress * (3.0 - 2.0 * progress);
}

uint hash2(int x, int y)
{
	uint mixed = uint(x) * 374761393u + uint(y) * 668265263u;
	mixed = (mixed ^ (mixed >> 13)) * 1274126177u;
	return mixed ^ (mixed >> 16);
}

float cornerValue(int gridX, int gridY) { return float(hash2(gridX, gridY) & 0xFFFFu) / 65536.0; }

float valueNoise(float x, float y)
{
	int cornerX = int(floor(x)), cornerY = int(floor(y));
	float insideX = x - float(cornerX), insideY = y - float(cornerY);
	float easeX = insideX * insideX * (3.0 - 2.0 * insideX), easeY = insideY * insideY * (3.0 - 2.0 * insideY);
	float topLeft = cornerValue(cornerX, cornerY), topRight = cornerValue(cornerX + 1, cornerY);
	float bottomLeft = cornerValue(cornerX, cornerY + 1), bottomRight = cornerValue(cornerX + 1, cornerY + 1);
	return (topLeft + (topRight - topLeft) * easeX) + ((bottomLeft + (bottomRight - bottomLeft) * easeX) - (topLeft + (topRight - topLeft) * easeX)) * easeY;
}

float cloudNoise(float x, float y)
{
	return valueNoise(x, y) * 0.55 + valueNoise(x * 2.1 + 17.0, y * 2.1) * 0.3 + valueNoise(x * 4.3, y * 4.3 + 31.0) * 0.15;
}

float distanceShade(float span)
{
	return span / max(DistanceShadeVoxels, uMaxDistance / DistanceShadeReachParts);
}

float hazeStart()
{
	return uHaze == 2 || airless() ? 0.9 : uHaze == 1 ? 0.75 : 0.45;
}

float terrainHaze(float span)
{
	return uUnderwater != 0 ? smoothStep(uMaxDistance * 0.12, uMaxDistance * 0.65, span) : smoothStep(uMaxDistance * hazeStart(), uMaxDistance, span);
}

uint hazeColor(uint color, float haze, int pixelX, int pixelY)
{
	if (haze <= 0.0)
	{
		return color;
	}
	int stride = clamp(int(haze * float(HazeSteps) + dither(pixelX, pixelY)), 0, HazeSteps);
	return table(HazeRow + stride, color);
}

// A tile's light (texel 1) and its facts (texel 1's z: discovered, smoke, burning, roofed).
uvec4 tileLightTexel(int tileIndex) { return texelFetch(uTiles, tileIndex * 4 + 1); }

float lightAt(vec3 point, out float ufoGlow)
{
	int tileX = clamp(int(floor(point.x / 16.0)), 0, uSize.x - 1);
	int tileY = clamp(int(floor(point.y / 16.0)), 0, uSize.y - 1);
	int tileZ = clamp(int(floor(point.z / 24.0)), 0, uSize.z - 1);
	uvec4 ownTile = tileLightTexel((tileZ * uSize.y + tileY) * uSize.x + tileX);
	float ownTileShade = float(ownTile.x & 0xFFu), ownTileGlow = float(ownTile.x >> 24);
	if (uSmoothLighting == 0)
	{
		ufoGlow = ownTileGlow / 255.0;
		return ownTileShade;
	}
	float gridX = point.x / 16.0 - 0.5, gridY = point.y / 16.0 - 0.5, gridZ = point.z / 24.0 - 0.5;
	int lowX = int(floor(gridX)), lowY = int(floor(gridY)), lowZ = int(floor(gridZ));
	float fractionX = gridX - float(lowX), fractionY = gridY - float(lowY), fractionZ = gridZ - float(lowZ);
	float blendedShade = 0.0, blendedGlow = 0.0;
	for (int corner = 0; corner < 8; ++corner)
	{
		int cornerX = lowX + (corner & 1), cornerY = lowY + ((corner >> 1) & 1), cornerZ = clamp(lowZ + ((corner >> 2) & 1), 0, uSize.z - 1);
		float cornerShade, cornerGlow = 0.0;
		if (cornerX < 0 || cornerY < 0 || cornerX >= uSize.x || cornerY >= uSize.y)
		{
			cornerShade = float(uAmbient);
		}
		else
		{
			uvec4 tileLight = tileLightTexel((cornerZ * uSize.y + cornerY) * uSize.x + cornerX);
			bool discovered = (tileLight.z & 1u) != 0u;
			cornerShade = discovered ? float(tileLight.x & 0xFFu) : ownTileShade;
			cornerGlow = discovered ? float(tileLight.x >> 24) : ownTileGlow;
		}
		float weight = ((corner & 1) != 0 ? fractionX : 1.0 - fractionX) * (((corner >> 1) & 1) != 0 ? fractionY : 1.0 - fractionY)
			* (((corner >> 2) & 1) != 0 ? fractionZ : 1.0 - fractionZ);
		blendedShade += cornerShade * weight;
		blendedGlow += cornerGlow * weight;
	}
	ufoGlow = blendedGlow / 255.0;
	return blendedShade;
}

bool shadowed(vec3 from, vec3 to)
{
	vec3 direction = to - from;
	float span = sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
	if (span < 1.0)
	{
		return false;
	}
	direction /= span;
	Hit hit;
	int hitTile, endAxis;
	float endDistance;
	return castRay(from, direction, span, 2.0, 2.0, hit, hitTile, endDistance, endAxis) == RAY_HIT && hit.t < span;
}

float pointLight(vec3 point, vec3 normal, bool shadows)
{
	float light = 0.0;
	// Lamps: only the strongest in reach counts.
	if (uLampCount > 0 && (uNight > 0.3 || uUnderwater != 0))
	{
		const float lampReach = 120.0;
		int brightestLamp = -1;
		float brightestPower = 0.0, brightestDistance = 0.0;
		for (int lamp = 0; lamp < uLampCount; ++lamp)
		{
			float dx = uLamps[lamp].x - point.x, dy = uLamps[lamp].y - point.y, dz = uLamps[lamp].z - point.z;
			float span = sqrt(dx * dx + dy * dy + dz * dz);
			if (span > lampReach) continue;
			float power = uLamps[lamp].w * (1.0 - span / lampReach) * (1.0 - span / lampReach);
			if (power > brightestPower) { brightestPower = power; brightestLamp = lamp; brightestDistance = span; }
		}
		if (brightestLamp >= 0 && brightestPower > 0.03)
		{
			vec3 lampPos = uLamps[brightestLamp].xyz;
			vec3 towardsLamp = vec3((lampPos.x - point.x) / brightestDistance, (lampPos.y - point.y) / brightestDistance, (lampPos.z - point.z) / brightestDistance);
			float facing = normal.x * towardsLamp.x + normal.y * towardsLamp.y + normal.z * towardsLamp.z;
			vec3 nearLamp = vec3(lampPos.x - towardsLamp.x * 4.0, lampPos.y - towardsLamp.y * 4.0, lampPos.z - towardsLamp.z * 4.0);
			float night = uUnderwater != 0 ? 1.0 : uNight;
			if (facing <= 0.0 || (shadows && shadowed(point, nearLamp)))
			{
				light -= brightestPower * 3.0 * night;
			}
			else
			{
				light += brightestPower * 6.0 * (0.4 + 0.6 * facing) * night;
			}
		}
	}
	// The flashlight: the dynamic one a tight spot on the crosshair, the shoulder one a cone.
	if (uFlashlight == 2)
	{
		float range = 16.0 * 20.0;
		vec3 fromLight = point - uFlashlightPos;
		float span = sqrt(fromLight.x * fromLight.x + fromLight.y * fromLight.y + fromLight.z * fromLight.z);
		if (span > 0.5 && span < range)
		{
			float cosine = (fromLight.x * uForwardInVoxels.x + fromLight.y * uForwardInVoxels.y + fromLight.z * uForwardInVoxels.z) / span;
			if (cosine > 0.914)
			{
				float beam = smoothStep(0.974, 0.990, cosine) + 0.18 * smoothStep(0.914, 0.974, cosine);
				float falloff = 1.0 - 0.85 * span / range;
				float facing = -(normal.x * fromLight.x + normal.y * fromLight.y + normal.z * fromLight.z) / span;
				float power = beam * falloff * falloff * 13.0 * (0.4 + 0.6 * max(0.0, facing));
				if (power > 0.25 && facing > 0.0 && !(shadows && shadowed(point, uFlashlightPos)))
				{
					light += power;
				}
			}
		}
	}
	else if (uFlashlight == 1)
	{
		float range = 16.0 * 14.0;
		vec3 fromLight = point - uFlashlightPos;
		float span = sqrt(fromLight.x * fromLight.x + fromLight.y * fromLight.y + fromLight.z * fromLight.z);
		if (span > 0.5 && span < range)
		{
			float cosine = (fromLight.x * uForwardInVoxels.x + fromLight.y * uForwardInVoxels.y + fromLight.z * uForwardInVoxels.z) / span;
			if (cosine > 0.8)
			{
				float cone = smoothStep(0.8, 0.96, cosine);
				float falloff = 1.0 - span / range;
				float facing = -(normal.x * fromLight.x + normal.y * fromLight.y + normal.z * fromLight.z) / span;
				float power = cone * falloff * falloff * 10.0 * (0.35 + 0.65 * max(0.0, facing));
				if (power > 0.25 && facing > 0.0 && !(shadows && shadowed(point, uFlashlightPos)))
				{
					light += power;
				}
			}
		}
	}
	return light;
}

float flareLight(vec3 point, vec3 normal, bool shadows)
{
	// Every flare in reach, strongest first; the strongest one or two get a shadow ray.
	float powers[8];
	int order[8];
	int count = 0;
	float darkness = uUnderwater != 0 ? 0.6 : 0.3 + 0.7 * uNight;
	for (int flare = 0; flare < uFlareCount && count < 8; ++flare)
	{
		float dx = uFlares[flare].x - point.x, dy = uFlares[flare].y - point.y, dz = uFlares[flare].z - point.z;
		float span = sqrt(dx * dx + dy * dy + dz * dz);
		if (span > FlareReach || span < 0.01) continue;
		float power = (1.0 - span / FlareReach) * (1.0 - span / FlareReach) * uFlares[flare].w * uFlareFlicker[flare] * darkness;
		float facing = (normal.x * dx + normal.y * dy + normal.z * dz) / span;
		if (facing <= 0.0) continue;
		power *= 0.4 + 0.6 * facing;
		if (power < 0.02) continue;
		powers[flare] = power;
		order[count] = flare;
		++count;
	}
	for (int sorted = 1; sorted < count; ++sorted)
	{
		for (int rank = sorted; rank > 0 && powers[order[rank - 1]] < powers[order[rank]]; --rank)
		{
			int swapped = order[rank - 1]; order[rank - 1] = order[rank]; order[rank] = swapped;
		}
	}
	for (int rank = 0; rank < count && rank < 2; ++rank)
	{
		if (!shadows || powers[order[rank]] < 0.08 || !shadowed(point, uFlares[order[rank]].xyz))
		{
			return powers[order[rank]];
		}
	}
	return 0.0;
}

bool underRock(float height)
{
	return uEnclosed != 0 || height <= GroundTop + float(uSurfaceLevel) * 24.0;
}

bool indoorsAt(vec3 point)
{
	if (underRock(point.z))
	{
		return true;
	}
	int tileX = int(floor(point.x / 16.0)), tileY = int(floor(point.y / 16.0));
	if (tileX < 0 || tileY < 0 || tileX >= uSize.x || tileY >= uSize.y)
	{
		return false;
	}
	int tileZ = clamp(int(floor(point.z / 24.0)), 0, uSize.z - 1);
	return (tileLightTexel((tileZ * uSize.y + tileY) * uSize.x + tileX).z & 0x20000u) != 0u;
}

float waveHeight(float x, float y, float span)
{
	float height = 0.0;
	for (int train = 0; train < 3; ++train)
	{
		float share = WaveTrains[train * 3], fadeFrom = WaveTrains[train * 3 + 1], fadeTo = WaveTrains[train * 3 + 2];
		if (span >= fadeTo)
		{
			continue;
		}
		float fade = span <= fadeFrom ? 1.0 : (fadeTo - span) / (fadeTo - fadeFrom);
		float bend = texelFetch(uWaveSine, int(x * uWaveSteps[train * 6 + 3] + y * uWaveSteps[train * 6 + 4]) & (WaveSineSteps - 1)).r * uWaveSteps[train * 6 + 5];
		int stride = int(x * uWaveSteps[train * 6] + y * uWaveSteps[train * 6 + 1] + uWaveSteps[train * 6 + 2] + bend);
		height += texelFetch(uWaveSine, stride & (WaveSineSteps - 1)).r * share * fade;
	}
	return height;
}

uint waterColor(float x, float y, float span, int pixelX, int pixelY, out float shade)
{
	float height = waveHeight(x, y, span);
	shade = WaveShades * (1.0 - height);
	float threshold = dither(pixelX + 3, pixelY + 1);
	if (height > FoamFrom && (height - FoamFrom) / (1.0 - FoamFrom) > threshold)
	{
		return scene(SceneWater + 2);
	}
	if (-height > DeepFrom && (-height - DeepFrom) / (1.0 - DeepFrom) > threshold)
	{
		return scene(SceneWater + 1);
	}
	return scene(SceneWater);
}

uint rockColor(float alongU, float alongV, int face, float span, int pixelX, int pixelY)
{
	float mottle = valueNoise(alongU / 12.0, alongV / 9.0) * 0.65 + valueNoise(alongU / 3.0, alongV / 3.0) * 0.35;
	uint rock = scene(SceneRockBase) != 0u ? scene(SceneRockBase) : scene(SceneRock);
	int darkest = 12 - int(rock & 15u);
	int lit = min(darkest - RockMottleShades, int(float(uAmbient) * 0.5 + uFaceShade[face]));
	int shades = min(darkest, lit + int(mottle * float(RockMottleShades) + dither(pixelX, pixelY)));
	return hazeColor(shadeColor(rock, shades), terrainHaze(span), pixelX + 2, pixelY + 1);
}

uint airlessSkyColor(vec3 rayDirection, int pixelX, int pixelY)
{
	float daylight = 1.0 - uNight;
	if (uSky == SkyOrbit && rayDirection.z < -OrbitEarthEdge + OrbitAirGlow)
	{
		float below = -rayDirection.z;
		if (below < OrbitEarthEdge)
		{
			float glow = (below - (OrbitEarthEdge - OrbitAirGlow)) / OrbitAirGlow * (daylight > 0.4 ? 1.0 : 0.4);
			return glow > dither(pixelX, pixelY) ? scene(SceneEarth + 3) : scene(SceneSky);
		}
		if (daylight <= 0.4)
		{
			return scene(SceneEarth + 1);
		}
		float planeX = rayDirection.x / below * 1.4 + uCloudTime * 0.012, planeY = rayDirection.y / below * 1.4;
		if (smoothStep(OrbitEarthEdge + 0.08, OrbitEarthEdge, below) > dither(pixelX + 1, pixelY + 2))
		{
			return scene(SceneEarth + 3);
		}
		return cloudNoise(planeX, planeY) > 0.56 ? scene(SceneEarth + 2) : scene(SceneEarth);
	}
	float sunDot = dot(rayDirection, uSunDir);
	if (daylight > 0.4 && sunDot > 0.9995)
	{
		return scene(SceneSun);
	}
	if (uSky == SkyMoon)
	{
		float along = dot(rayDirection, MoonEarthDirection);
		if (along > MoonEarthCos)
		{
			vec3 across = (rayDirection - MoonEarthDirection * along) / MoonEarthSin;
			float acrossSquared = across.x * across.x + across.y * across.y + across.z * across.z;
			float facing = sqrt(max(0.0, 1.0 - acrossSquared));
			vec3 normal = across - MoonEarthDirection * facing;
			float sunlit = dot(normal, uSunDir);
			if (sunlit < 0.1 * dither(pixelX, pixelY))
			{
				return scene(SceneEarth + 1);
			}
			if (acrossSquared > 0.82)
			{
				return scene(SceneEarth + 3);
			}
			return cloudNoise(across.x * 3.0 + 7.0, across.y * 3.0 + across.z * 3.0) > 0.55 ? scene(SceneEarth + 2) : scene(SceneEarth);
		}
	}
	if (uSky != SkyMoon || rayDirection.z > 0.0)
	{
		float azimuth = atan(rayDirection.y, rayDirection.x), elevationAngle = asin(clamp(rayDirection.z, -1.0, 1.0));
		uint cellHash = hash2(int(floor(azimuth * 160.0)), int(floor(elevationAngle * 160.0))) % 1000u;
		if (cellHash < 6u)
		{
			return scene(SceneStar);
		}
		if (cellHash < 18u)
		{
			return scene(SceneFaintStar);
		}
	}
	return scene(SceneSky);
}

uint skyColor(vec3 rayDirection, int pixelX, int pixelY)
{
	if (airless())
	{
		return airlessSkyColor(rayDirection, pixelX, pixelY);
	}
	float elevation = max(rayDirection.z, 0.0);
	float gradient = pow(elevation, 0.6) * 31.0;
	if (uOvercast != 0 || uSky == SkyVoid)
	{
		float layerDistance = 1.0 / max(elevation, 0.06);
		float drift = uCloudTime * (uSky == SkyVoid ? 0.01 : 0.05);
		float deck = cloudNoise(rayDirection.x * layerDistance * 0.9 + drift, rayDirection.y * layerDistance * 0.9 + drift * 0.4);
		float mottle = (deck - 0.5) * 2.0 * CloudDeckMottle * smoothStep(0.0, 0.3, elevation);
		return scene(SceneSky + clamp(int(gradient + mottle + dither(pixelX, pixelY)), 0, 31));
	}
	uint color = scene(SceneSky + clamp(int(gradient + dither(pixelX, pixelY)), 0, 31));
	if (uUnderwater != 0)
	{
		return color;
	}
	float daylight = 1.0 - uNight;
	float sunDot = dot(rayDirection, uSunDir);
	if (daylight > 0.4 && sunDot > (uSky == SkyMars ? 0.9998 : 0.9995))
	{
		return scene(SceneSun);
	}
	if (uNight > 0.4 && uSky != SkyMars)
	{
		float moonDot = dot(rayDirection, uMoonDir);
		if (moonDot > 0.9992)
		{
			return scene(SceneMoon);
		}
	}
	if (elevation > 0.02 && uNight < 0.9 && uSky != SkyMars)
	{
		float layerDistance = 1.0 / max(elevation, 0.06);
		float cloudU = rayDirection.x * layerDistance * 1.6 + uCloudTime * 0.02;
		float cloudV = rayDirection.y * layerDistance * 1.6;
		float density = cloudNoise(cloudU, cloudV);
		float coverage = smoothStep(0.52, 0.72, density) * smoothStep(0.02, 0.2, elevation);
		if (coverage > dither(pixelX + 1, pixelY + 3))
		{
			float darkness = clamp((0.8 - density) * 10.0, 0.0, 7.0);
			return scene(SceneCloud + clamp(int(darkness + dither(pixelX, pixelY)), 0, 7));
		}
	}
	if (uNight > 0.5 && elevation > 0.03)
	{
		float azimuth = atan(rayDirection.y, rayDirection.x), elevationAngle = asin(min(rayDirection.z, 1.0));
		uint cellHash = hash2(int(floor(azimuth * 160.0)), int(floor(elevationAngle * 160.0)));
		if ((cellHash % 1000u) < 6u)
		{
			return scene(SceneStar);
		}
	}
	return color;
}

uint colorOutsideMap(vec3 rayOrigin, vec3 rayDirection, float tExit, int exitAxis, int pixelX, int pixelY, out float depth)
{
	vec3 exitPoint = rayOrigin + rayDirection * tExit;
	float alongBorder = exitAxis == 0 ? exitPoint.y : exitPoint.x;
	float surfaceTop = GroundTop + float(uSurfaceLevel) * 24.0;
	bool inSpace = uSky == SkySpace || uSky == SkyOrbit;
	if (!inSpace && (exitAxis != 2 ? exitPoint.z < surfaceTop : uEnclosed != 0))
	{
		depth = tExit;
		int face = exitAxis == 2 ? 5 : exitAxis * 2 + (rayDirection[exitAxis] > 0.0 ? 1 : 0);
		return exitAxis == 2 ? rockColor(exitPoint.x, exitPoint.y, face, tExit, pixelX, pixelY)
			: rockColor(alongBorder, exitPoint.z / ZScale, face, tExit, pixelX, pixelY);
	}
	float tapeBottom = surfaceTop + 8.0;
	bool tape = uDiscreetEdge == 0 && !inSpace;
	float borderShade = float(uAmbient) * 0.5;
	float pastLastPost = alongBorder - floor(alongBorder / 64.0) * 64.0;
	if (tape && pastLastPost < 1.5 && exitPoint.z >= surfaceTop && exitPoint.z < surfaceTop + 17.0)
	{
		depth = tExit;
		return shadeColor(scene(ScenePost), int(borderShade + (exitAxis == 0 ? 1.0 : 2.0) + dither(pixelX, pixelY)));
	}
	if (tape && exitPoint.z >= tapeBottom && exitPoint.z < tapeBottom + 7.0)
	{
		depth = tExit;
		int tapeRow = int(exitPoint.z - tapeBottom);
		uint color = scene(SceneTapeBlack);
		if (tapeRow > 0 && tapeRow < 6)
		{
			bool readsBackwards = exitAxis == 0 ? rayDirection.x < 0.0 : rayDirection.y > 0.0;
			float alongText = floor(readsBackwards ? -alongBorder : alongBorder);
			float textLength = float(TapeTextLength * 4);
			int textColumn = int(alongText - floor(alongText / textLength) * textLength);
			int glyph = TapeText[textColumn / 4], glyphColumn = textColumn % 4;
			bool ink = glyphColumn < 3 && (TapeFont[glyph * 5 + 5 - tapeRow] & (4 >> glyphColumn)) != 0;
			color = ink ? scene(SceneTapeBlack) : scene(SceneTapeYellow);
		}
		return shadeColor(color, int(borderShade + dither(pixelX, pixelY)));
	}
	if (rayDirection.z < 0.0 && uGroundModel != 0)
	{
		float groundDistance = (surfaceTop - rayOrigin.z) / rayDirection.z;
		if (groundDistance >= tExit)
		{
			depth = groundDistance;
			float groundX = rayOrigin.x + rayDirection.x * groundDistance;
			float groundY = rayOrigin.y + rayDirection.y * groundDistance;
			uint ground = uint(uGroundModel);
			if (uWaterModel != 0)
			{
				int edgeX = clamp(int(floor(groundX / 16.0)), 0, uSize.x - 1);
				int edgeY = clamp(int(floor(groundY / 16.0)), 0, uSize.y - 1);
				if (texelFetch(uTiles, (edgeY * uSize.x + edgeX) * 4 + 1).w != 0u) ground = uint(uWaterModel);
			}
			bool groundIsWater = (texelFetch(uModelInfo, int(ground)).x & 8u) != 0u;
			uint color;
			float waveShade = 0.0;
			if (groundIsWater && scene(SceneWater) != 0u)
			{
				color = waterColor(groundX, groundY, groundDistance, pixelX, pixelY, waveShade);
			}
			else
			{
				ivec2 spot = ivec2(int(floor(groundX)) & 15, int(floor(groundY)) & 15);
				color = modelColor(ground, ivec3(spot, 1));
				if (color == 0u) color = modelColor(ground, ivec3(spot, 0));
			}
			int shades = int(float(uAmbient) + uFaceShade[4] + waveShade + distanceShade(groundDistance) + dither(pixelX, pixelY));
			float blackShare = clamp((float(uAmbient) - DarkOverflowFrom) / (DarkOverflowTo - DarkOverflowFrom), 0.0, 1.0);
			int darkest = groundIsWater ? max(int(color & 15u), WaterDarkestShade) : 15;
			color = blackShare > dither(pixelX + 1, pixelY + 2) ? shadeColor(color, shades) : ((color & 0xF0u) | uint(min(darkest, int(color & 15u) + max(0, shades))));
			float haze = uUnderwater != 0 ? smoothStep(uMaxDistance * 0.12, uMaxDistance * 0.65, groundDistance)
				: smoothStep(uMaxDistance * hazeStart() * 0.67, uMaxDistance * 1.6, groundDistance);
			return hazeColor(color, haze, pixelX + 2, pixelY + 1);
		}
	}
	depth = Infinity;
	return skyColor(rayDirection, pixelX, pixelY);
}

// A steady UFO model's open faces at a voxel (VoxelModel::exposed), 0 on any other model.
uint exposedAt(uvec4 info, ivec3 voxel)
{
	return info.w == 0u ? 0u : texelFetch(uModelExposed, int(info.w) * 6144 + (voxel.z * 16 + voxel.y) * 16 + voxel.x).r;
}

uint voxelColorAt(ivec3 world, bool nextFrame, int onlyPart)
{
	if (world.x < 0 || world.y < 0 || world.z < 0 || world.x >= uSize.x * 16 || world.y >= uSize.y * 16 || world.z >= uSize.z * 24) return 0u;
	int tile = ((world.z / 24) * uSize.y + world.y / 16) * uSize.x + world.x / 16;
	uvec4 now = texelFetch(uTiles, tile * 4), next = texelFetch(uTiles, tile * 4 + 2);
	uint parts[4] = uint[4](now.x & 0xFFFFu, now.x >> 16, now.y & 0xFFFFu, now.y >> 16);
	uint nextParts[4] = uint[4](next.x & 0xFFFFu, next.x >> 16, next.y & 0xFFFFu, next.y >> 16);
	for (int part = 0; part < 4; ++part)
	{
		if (onlyPart >= 0 && part != onlyPart) continue;
		uint model = nextFrame && nextParts[part] != 0u ? nextParts[part] : parts[part];
		uint color = model != 0u ? modelColor(model, ivec3(world.x % 16, world.y % 16, world.z % 24)) : 0u;
		if (color != 0u) return color;
	}
	return 0u;
}

const ivec2 NearbyRows[9] = ivec2[9](ivec2(0, 0), ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1), ivec2(-2, 0), ivec2(2, 0), ivec2(-3, 0), ivec2(3, 0));

uint mirroredColor(Hit hit, int hitTile, int axis, bool skinOnly, bool nextFrame)
{
	int craftId = int(texelFetch(uTiles, hitTile * 4 + 3).x) - 1;
	if (craftId < 0)
	{
		return 0u;
	}
	// The craft's mirror: lo[3], size[3], sum[2], symmetric[2], then per axis where its start, pos and outer rows begin.
	int header = craftId * 16;
	if (texelFetch(uMirrors, header + 8 + axis).r == 0)
	{
		return 0u;
	}
	ivec3 lo = ivec3(texelFetch(uMirrors, header).r, texelFetch(uMirrors, header + 1).r, texelFetch(uMirrors, header + 2).r);
	ivec3 size = ivec3(texelFetch(uMirrors, header + 3).r, texelFetch(uMirrors, header + 4).r, texelFetch(uMirrors, header + 5).r);
	int sum = texelFetch(uMirrors, header + 6 + axis).r;
	int startRows = texelFetch(uMirrors, header + 10 + axis * 3).r, posRows = texelFetch(uMirrors, header + 11 + axis * 3).r, outerRows = texelFetch(uMirrors, header + 12 + axis * 3).r;

	ivec3 worldVoxel = ivec3((hitTile % uSize.x) * 16 + hit.voxel.x, ((hitTile / uSize.x) % uSize.y) * 16 + hit.voxel.y, (hitTile / (uSize.x * uSize.y)) * 24 + hit.voxel.z);
	int acrossPos = worldVoxel[1 - axis] - lo[1 - axis], upPos = worldVoxel.z - lo.z, acrossSize = size[1 - axis];
	if (acrossPos < 0 || acrossPos >= acrossSize || upPos < 0 || upPos >= size.z)
	{
		return 0u;
	}
	bool onSkin = texelFetch(uMirrors, outerRows + upPos * acrossSize + acrossPos).r == worldVoxel[axis];
	if (skinOnly && !onSkin)
	{
		return 0u;
	}
	int mirroredPos = sum - worldVoxel[axis];
	for (int nearby = 0; nearby < 9; ++nearby)
	{
		int rowAcross = acrossPos + NearbyRows[nearby].x, rowUp = upPos + NearbyRows[nearby].y;
		if (rowAcross < 0 || rowAcross >= acrossSize || rowUp < 0 || rowUp >= size.z) continue;
		int row = rowUp * acrossSize + rowAcross;
		int rowFirst = posRows + texelFetch(uMirrors, startRows + row).r, rowEnd = posRows + texelFetch(uMirrors, startRows + row + 1).r;
		if (rowFirst == rowEnd) continue;
		int bestPos = -1;
		if (onSkin)
		{
			int last = texelFetch(uMirrors, rowEnd - 1).r;
			if (abs(last - mirroredPos) <= 16) bestPos = last;
		}
		else
		{
			// The first face at or past the mirrored spot (std::lower_bound), and the one before it.
			int low = rowFirst, high = rowEnd;
			for (int guard = 0; guard < 16 && low < high; ++guard)
			{
				int middle = (low + high) / 2;
				if (texelFetch(uMirrors, middle).r < mirroredPos) low = middle + 1; else high = middle;
			}
			int bestDistance = 17;
			if (low != rowEnd)
			{
				int face = texelFetch(uMirrors, low).r;
				if (face - mirroredPos < bestDistance) { bestPos = face; bestDistance = face - mirroredPos; }
			}
			if (low != rowFirst)
			{
				int face = texelFetch(uMirrors, low - 1).r;
				if (mirroredPos - face < bestDistance) bestPos = face;
			}
		}
		if (bestPos < 0) continue;
		ivec3 sourceVoxel = worldVoxel;
		sourceVoxel[axis] = bestPos;
		sourceVoxel[1 - axis] = lo[1 - axis] + rowAcross;
		sourceVoxel.z = lo.z + rowUp;
		uint color = voxelColorAt(sourceVoxel, nextFrame, onSkin ? -1 : hit.part);
		if (color != 0u && table(BlackRow, color) == 0u) return color;
	}
	return 0u;
}

uint wallPaintColor(Hit hit, int hitTile, uint parts[4], uint ownColor)
{
	if (uWallPaint <= 0 || (hit.part != 1 && hit.part != 2) || hit.ghost || ownColor == 0u || hit.axis < 0 || hit.axis > 1)
	{
		return 0u;
	}
	int part = hit.part;
	bool end = hit.axis != part - 1; // a west wall's north end, a north wall's west end
	if (end)
	{
		int other = 3 - part;
		if (parts[other] != 0u && modelColor(parts[other], hit.voxel) != 0u)
		{
			part = other;
			end = false;
		}
	}
	uint paintIndexes = texelFetch(uTiles, hitTile * 4 + 2).w;
	uint paintIndex = part == 1 ? paintIndexes & 0xFFFFu : paintIndexes >> 16;
	if (paintIndex == 0u)
	{
		return 0u;
	}
	// The paint: the wall's model, the donor's, donorWest | openEnd << 1 | kind << 8 | paintGroup << 16, the wall's min x | min y << 8.
	uvec4 paint = texelFetch(uWallPaints, int(paintIndex) - 1);
	bool donorWest = (paint.z & 1u) != 0u, openEnd = (paint.z & 2u) != 0u;
	if (int((paint.z >> 8) & 0xFFu) > uWallPaint || parts[part] != paint.x || (ownColor >> 4) != ((paint.z >> 16) & 0xFFu))
	{
		return 0u;
	}
	int wallMin = hit.axis == 0 ? int(paint.w & 0xFFu) : int((paint.w >> 8) & 0xFFu);
	if ((end && !openEnd) || hit.voxel[hit.axis] != wallMin)
	{
		return 0u;
	}
	int column = part == 1 ? (donorWest ? 15 - hit.voxel.y : hit.voxel.y) : (donorWest ? hit.voxel.x : 15 - hit.voxel.x);
	int row = int(paint.y) * 1024 + (donorWest ? 256 : 640) + hit.voxel.z * 16;
	const int Nearby[7] = int[7](0, -1, 1, -2, 2, -3, 3);
	for (int which = 0; which < 7; ++which)
	{
		int along = column + Nearby[which];
		if (along >= 0 && along < 16)
		{
			uint color = texelFetch(uModelSides, row + along).r;
			if (color != 0u) return color;
		}
	}
	return 0u;
}

// A plate's underside colour (shadeHit's undersideColor): its ceiling colour or its plan's, and under rock, dark art its plan's pattern.
uint undersideColor(uint plate, ivec3 voxel, bool undersideUnderRock, inout int rockCeilingMiddle)
{
	uint ceiling = (texelFetch(uModelInfo, int(plate)).z >> 8) & 0xFFu;
	uint plan = texelFetch(uModelSides, int(plate) * 1024 + voxel.y * 16 + voxel.x).r;
	uint plain = ceiling != 0u ? ceiling : plan;
	rockCeilingMiddle = undersideUnderRock && int(plain & 15u) >= DarkCeilingShade ? int(plain & 15u) : -1;
	return rockCeilingMiddle >= 0 ? plan : plain;
}

uint shadeHit(inout Hit hit, int hitTile, vec3 rayOrigin, vec3 rayDirection, int pixelX, int pixelY)
{
	uvec4 tileParts = texelFetch(uTiles, hitTile * 4), tileFacts = tileLightTexel(hitTile), tileNext = texelFetch(uTiles, hitTile * 4 + 2);
	uint parts[4] = uint[4](tileParts.x & 0xFFFFu, tileParts.x >> 16, tileParts.y & 0xFFFFu, tileParts.y >> 16);
	uint nextParts[4] = uint[4](tileNext.x & 0xFFFFu, tileNext.x >> 16, tileNext.y & 0xFFFFu, tileNext.y >> 16);
	uint seenBits = (tileFacts.x >> 8) & 0xFFu, emissiveBits = (tileFacts.x >> 16) & 0xFFu;
	bool discovered = (tileFacts.z & 1u) != 0u;
	int tilesPerLevel = uSize.x * uSize.y;

	// Unexplored parts, as the setting "Unseen terrain" has them.
	bool seenByTheView = false;
	if (uFogOfWar != 1)
	{
		float across = rayDirection.x * hit.t, down = rayDirection.y * hit.t;
		seenByTheView = across * across + down * down <= uSightView * uSightView;
	}
	bool unseen = false;
	if ((hit.part >= 0 ? ((seenBits >> uint(hit.part)) & 1u) == 0u : !discovered) && !seenByTheView)
	{
		bool exploredCeiling = hit.axis == 2 && rayDirection.z > 0.0 && hitTile >= tilesPerLevel && (tileLightTexel(hitTile - tilesPerLevel).z & 1u) != 0u;
		if (!exploredCeiling && uUnseenTerrain == 2)
		{
			unseen = true;
		}
		else if (!exploredCeiling)
		{
			int faceAxis = hit.axis >= 0 ? hit.axis : 2;
			int face = faceAxis * 2 + (rayDirection[faceAxis] > 0.0 ? 1 : 0);
			uint unexplored = scene(SceneUnexplored);
			int darkest = 12 - int(unexplored & 15u);
			if (uUnseenTerrain == 1)
			{
				uint slate = shadeColor(unexplored, min(darkest, int(float(uAmbient) * 0.5 + uFaceShade[face] + hit.t / UnseenFogDarkening + dither(pixelX, pixelY))));
				float fogShare = uHaze == 2 || airless() ? 0.0 : uHaze == 1 ? UnseenFogLight : UnseenFogNormal;
				float fog = fogShare * smoothStep(min(uSightMod, uMaxDistance * UnseenFogFromMax), uMaxDistance, hit.t);
				return hazeColor(slate, max(terrainHaze(hit.t), fog), pixelX + 2, pixelY + 1);
			}
			uint slate = shadeColor(unexplored, min(darkest, int(float(uAmbient) * 0.5 + uFaceShade[face] + dither(pixelX, pixelY))));
			float mist = UnexploredMistMax * smoothStep(uSightMod * UnexploredMistFrom, uSightMod * UnexploredMistTo, hit.t);
			return hazeColor(slate, max(terrainHaze(hit.t), mist), pixelX + 2, pixelY + 1);
		}
	}
	// The face's outward normal and its index into the face shading.
	int faceIndex = 4;
	vec3 normal = vec3(0.0, 0.0, 1.0);
	if (hit.axis >= 0)
	{
		normal.z = 0.0;
		normal[hit.axis] = rayDirection[hit.axis] > 0.0 ? -1.0 : 1.0;
		faceIndex = hit.axis * 2 + (normal[hit.axis] < 0.0 ? 1 : 0);
	}
	bool showNextFrame = dither(pixelX + 2, pixelY + 1) < uAnimationBlend;
	uint baseColor = hit.color;
	int rockCeilingMiddle = -1;
	float waveShade = 0.0;
	bool onSea = false;
	float hitHeight = rayOrigin.z + rayDirection.z * hit.t;
	if (hit.part >= 0 && parts[hit.part] != 0u)
	{
		uint part = parts[hit.part];
		uint model = hit.ghost ? texelFetch(uModelInfo, int(part)).y : showNextFrame && nextParts[hit.part] != 0u ? nextParts[hit.part] : part;
		uvec4 info = texelFetch(uModelInfo, int(model));
		ivec3 voxel = hit.voxel;
		if (!hit.ghost && model != part)
		{
			uint nextFrameColor = modelColor(model, voxel);
			if (nextFrameColor != 0u)
			{
				baseColor = nextFrameColor;
				hit.emissive = ((emissiveBits >> uint(hit.part)) & 1u) != 0u && (nextFrameColor & 15u) <= 3u && voxel.z >= int(info.z & 0xFFu);
			}
		}
		bool ownColorAllRound = (exposedAt(info, voxel) & 0x15u) != 0u;
		// A face the iso art never shows: what its row shows from the side it does.
		bool underside = hit.axis == 2 && rayDirection.z > 0.0;
		bool undersideUnderRock = underside && underRock(hitHeight);
		uint shownColor = underside ? undersideColor(model, voxel, undersideUnderRock, rockCeilingMiddle) : 0u;
		if (shownColor != 0u && modelColor(model, ivec3(voxel.xy, 23)) != 0u && hitTile + tilesPerLevel < tilesPerLevel * uSize.z)
		{
			uvec4 above = texelFetch(uTiles, (hitTile + tilesPerLevel) * 4);
			uint partsAbove[4] = uint[4](above.x & 0xFFFFu, above.x >> 16, above.y & 0xFFFFu, above.y >> 16);
			for (int which = 0; which < 4; ++which)
			{
				uint partAbove = partsAbove[which];
				if (partAbove != 0u && modelColor(partAbove, ivec3(voxel.xy, 0)) != 0u
					&& texelFetch(uModelSides, int(partAbove) * 1024 + voxel.y * 16 + voxel.x).r != 0u)
				{
					shownColor = undersideColor(partAbove, voxel, undersideUnderRock, rockCeilingMiddle);
					break;
				}
			}
		}
		if (hit.axis >= 0 && hit.axis < 2 && rayDirection[hit.axis] > 0.0)
		{
			shownColor = mirroredColor(hit, hitTile, hit.axis, false, showNextFrame);
			if (shownColor == 0u) shownColor = hit.axis == 0 ? texelFetch(uModelSides, int(model) * 1024 + 256 + voxel.z * 16 + voxel.y).r
				: texelFetch(uModelSides, int(model) * 1024 + 640 + voxel.z * 16 + voxel.x).r;
			uint painted = wallPaintColor(hit, hitTile, parts, shownColor);
			if (painted != 0u) shownColor = painted;
		}
		else if (hit.axis >= 0 && hit.axis < 2)
		{
			shownColor = mirroredColor(hit, hitTile, 1 - hit.axis, true, showNextFrame);
		}
		if (shownColor != 0u && !ownColorAllRound) baseColor = shownColor;
		if ((info.x & 8u) != 0u && scene(SceneWater) != 0u)
		{
			onSea = faceIndex == 4;
			baseColor = onSea ? waterColor(rayOrigin.x + rayDirection.x * hit.t, rayOrigin.y + rayDirection.y * hit.t, hit.t, pixelX, pixelY, waveShade) : scene(SceneWater + 1);
		}
	}
	vec3 surfacePoint = rayOrigin + rayDirection * hit.t + normal * 1.5;
	bool indoors = indoorsAt(surfacePoint);
	float faceShading = indoors ? uIndoorFaceShade[faceIndex] : uFaceShade[faceIndex];
	// (the part itself, as the tile has it: a steady UFO model's open faces and its lights' pulse)
	uvec4 partInfo = hit.part >= 0 ? texelFetch(uModelInfo, int(parts[hit.part])) : uvec4(0u);
	bool onSteadyUfo = partInfo.w != 0u;
	uint ufoMarks = texelFetch(uTiles, hitTile * 4 + 2).z; // which UFO's light reaches the tile (+1) | its pulse phase and marks << 8
	if (!indoors && onSteadyUfo)
	{
		uint openFaces = exposedAt(partInfo, hit.voxel);
		if (((openFaces >> uint(faceIndex)) & 1u) != 0u && uUfoFaceShade[openFaces & 63u] >= 0.0) faceShading = uUfoFaceShade[openFaces & 63u];
	}
	float ufoGlow = 0.0;
	float shade = lightAt(surfacePoint, ufoGlow) + faceShading + distanceShade(hit.t) + waveShade;
	if (uUfoLightsOn == 0) ufoGlow = 0.0;
	float sunShadow = 0.0;
	if (hit.emissive)
	{
		int pulseKind = int(partInfo.z >> 24);
		shade = pulseKind != 0 ? uUfoPulse[(pulseKind - 1) * 16 + int((ufoMarks >> 8) & 15u)] : min(shade, 0.5);
	}
	else if (uShadows != 0)
	{
		float daylight = uUnderwater != 0 || uOvercast != 0 || uSky == SkyVoid ? 0.0 : 1.0 - uNight;
		float sunFacing = dot(normal, uSunDir);
		if (!indoors && daylight > 0.3 && sunFacing > 0.05 && hit.t < 640.0)
		{
			if (shadowed(surfacePoint, surfacePoint + uSunDir * 260.0)) sunShadow = 3.0 * daylight;
			shade += sunShadow;
		}
		shade -= pointLight(surfacePoint, normal, true);
	}
	else
	{
		shade -= pointLight(surfacePoint, normal, false);
	}
	float flare = 0.0;
	if (!hit.emissive && uFlareCount > 0)
	{
		flare = flareLight(surfacePoint, normal, uShadows != 0);
		shade -= flare * 10.0;
	}
	if (uMuzzleFlashLight > 0.0)
	{
		float dx = surfacePoint.x - uMuzzleFlashPos.x, dy = surfacePoint.y - uMuzzleFlashPos.y, dz = surfacePoint.z - uMuzzleFlashPos.z;
		float distanceSquared = dx * dx + dy * dy + dz * dz;
		if (distanceSquared < 96.0 * 96.0)
		{
			float closeness = 1.0 - sqrt(distanceSquared) / 96.0;
			shade -= closeness * closeness * uMuzzleFlashLight * (4.0 + 6.0 * uNight);
		}
	}
	// A UFO's light on what is around it and on itself.
	int ufoLighting = -1;
	float ufoTint = 0.0, ufoOuterShare = 0.0;
	bool onUfo = ((ufoMarks >> 8) & 0x80u) != 0u;
	if (ufoGlow > 0.004 && !hit.emissive && (ufoMarks & 0xFFu) != 0u)
	{
		ufoLighting = int(ufoMarks & 0xFFu) - 1;
		float darkness = uUnderwater != 0 ? 0.6 : uNight, glow = ufoGlow * uUfoBreath;
		shade -= glow * (2.0 + 6.0 * darkness);
		float strength = onUfo && indoors ? UfoInsideTint + UfoInsideTintNight * darkness : onUfo ? 0.35 + 0.3 * darkness : 0.6 + 0.4 * darkness;
		ufoTint = min(0.9, glow * strength);
		ufoOuterShare = onUfo ? 0.0 : 1.0 - smoothStep(0.15, 0.55, ufoGlow);
	}
	if (!hit.emissive && uUnderwater != 0 && faceIndex == 4 && hit.t < 480.0)
	{
		float seconds = uCloudTime;
		float ripple = sin(surfacePoint.x * 0.31 + seconds * 1.7) + sin(surfacePoint.y * 0.27 - seconds * 1.3) + sin((surfacePoint.x - surfacePoint.y) * 0.19 + seconds * 0.9);
		if (ripple > 1.3) shade -= (ripple - 1.3) * 1.8;
	}
	// Whole palette shades, the fraction dithered; black only where the place is really dark.
	int wholeShades = int(max(0.0, shade + dither(pixelX, pixelY)));
	float placeShade = shade - faceShading - distanceShade(hit.t) - waveShade - sunShadow;
	float blackShare = clamp((placeShade - DarkOverflowFrom) / (DarkOverflowTo - DarkOverflowFrom), 0.0, 1.0);
	bool darkEnoughForBlack = !onSteadyUfo && blackShare > dither(pixelX + 1, pixelY + 2);
	if (rockCeilingMiddle >= 0 && !darkEnoughForBlack)
	{
		wholeShades = min(wholeShades, max(0, 15 - RockMottleShades - rockCeilingMiddle));
	}
	int darkest = onSea ? max(int(baseColor & 15u), WaterDarkestShade) : 15;
	uint color = darkEnoughForBlack ? shadeColor(baseColor, wholeShades) : ((baseColor & 0xF0u) | uint(min(darkest, int(baseColor & 15u) + wholeShades)));
	if (flare * 1.5 > 0.04 + dither(pixelX + 1, pixelY + 3))
	{
		color = table(FlareRow, color); // the flare's light is red
	}
	if (ufoLighting >= 0 && ufoOuterShare <= 0.0 && onUfo)
	{
		int stride = min(12, int(floor(ufoTint * 12.0 + 0.5)));
		if (stride > 0) color = table(UfoRows + ufoLighting * RowsPerUfo + 2 + stride - 1, color);
	}
	else if (ufoTint > dither(pixelX + 3, pixelY + 2))
	{
		color = table(UfoRows + ufoLighting * RowsPerUfo + (ufoOuterShare > dither(pixelX + 1, pixelY) ? 1 : 0), color);
	}
	if (unseen)
	{
		color = table(UnseenRow, color);
	}
	return hazeColor(color, terrainHaze(hit.t), pixelX + 2, pixelY + 1);
}

void main()
{
	// The block of pixels this ray colours (rows from the top), and its centre on the image plane.
	int blockColumn = int(gl_FragCoord.x), blockRow = int(gl_FragCoord.y);
	int topRow = blockRow * uPixelSize, leftColumn = blockColumn * uPixelSize;
	float planeV = (1.0 - (float(topRow) + float(uPixelSize) * 0.5) * 2.0 / float(uViewSize.y)) * uHalfPlane.y;
	float planeU = ((float(leftColumn) + float(uPixelSize) * 0.5) * 2.0 / float(uViewSize.x) - 1.0) * uHalfPlane.x;
	vec3 rayDirection = uForward + planeU * uRight + planeV * uUp;
	rayDirection.z /= ZScale;
	float directionLength = sqrt(rayDirection.x * rayDirection.x + rayDirection.y * rayDirection.y + rayDirection.z * rayDirection.z);
	rayDirection /= directionLength;

	Hit hit;
	hit.color = 0u; hit.t = 0.0; hit.axis = -1; hit.part = 0; hit.voxel = ivec3(0);
	float ghostThreshold = dither(blockColumn, blockRow) / uGhostScale;
	float solidGhostThreshold = uSeeThroughCrops != 0 ? dither(blockColumn, blockRow) / SeeThroughCropOpacity : 0.0;
	int hitTile, endAxis;
	float endDistance;
	int rayEnd = castRay(uEye, rayDirection, uMaxDistance, ghostThreshold, solidGhostThreshold, hit, hitTile, endDistance, endAxis);
	// What the walk found, before the shading changes any of it (for the check against the CPU's).
#ifdef RESULTS
	outResult = uvec4(uint(hitTile),
		uint(rayEnd == RAY_HIT ? (hit.part & 7) : 0) | uint((rayEnd == RAY_HIT ? hit.axis : endAxis) + 1) << 4 | uint(hit.ghost) << 8 | uint(hit.glass) << 9 | uint(hit.emissive) << 10 | uint(rayEnd) << 12
			| hit.color << 16,
		uint(hit.voxel.x) | uint(hit.voxel.y) << 8 | uint(hit.voxel.z) << 16,
		floatBitsToUint(rayEnd == RAY_HIT ? hit.t : endDistance));
#endif

	// Colour the block by how the ray ended.
	uint color;
	float distanceAlongRay;
	if (rayEnd == RAY_HIT)
	{
		color = shadeHit(hit, hitTile, uEye, rayDirection, blockColumn, blockRow);
		distanceAlongRay = hit.t;
	}
	else if (rayEnd == RAY_SIDE)
	{
		color = colorOutsideMap(uEye, rayDirection, endDistance, endAxis, blockColumn, blockRow, distanceAlongRay);
	}
	else if (rayEnd == RAY_FAR)
	{
		color = scene(SceneHaze);
		distanceAlongRay = Infinity;
	}
	else if (rayEnd == RAY_BOTTOM)
	{
		if (uSky == SkySpace || uSky == SkyOrbit)
		{
			color = skyColor(rayDirection, blockColumn, blockRow);
			distanceAlongRay = Infinity;
		}
		else
		{
			float bottomX = uEye.x + rayDirection.x * endDistance, bottomY = uEye.y + rayDirection.y * endDistance;
			uint earth = uGroundModel != 0 ? modelColor(uint(uGroundModel), ivec3(int(floor(bottomX)) & 15, int(floor(bottomY)) & 15, 0)) : 0u;
			color = earth != 0u ? shadeColor(earth, min(12 - int(earth & 15u), 6 + int(dither(blockColumn, blockRow)))) : scene(SceneUnexplored);
			distanceAlongRay = endDistance;
		}
	}
	else
	{
		color = colorOutsideMap(uEye, rayDirection, endDistance, 2, blockColumn, blockRow, distanceAlongRay);
	}
	if (hit.glass)
	{
		color = table(GlassRow, color);
	}
	outColor = color;
	outDepth = distanceAlongRay / directionLength;
	// Whether it shines (a UFO's light counts three times in the bloom).
	bool glowing = rayEnd == RAY_HIT && hit.emissive && distanceAlongRay < Infinity;
	outGlow = !glowing ? 0u : hit.part >= 0 && (texelFetch(uModelInfo, int(texelFetch(uTiles, hitTile * 4)[hit.part / 2] >> uint(16 * (hit.part & 1))) & 0xFFFF).z >> 24) != 0u ? 3u : 1u;
}
)";

}

}
