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

const char *const VertexSource = R"(
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
uniform usamplerBuffer uBlend;      // the palette (768 bytes: red, green, blue of each colour), then the palette colour nearest each colour of a 32 x 32 x 32 cube

uniform ivec3 uSize;         // the map in tiles
uniform float uMaxDistance;  // how far rays go, in voxels
uniform vec3 uEye, uForward, uRight, uUp;
uniform vec2 uHalfPlane;     // the image plane's half-width and half-height
uniform ivec2 uViewSize;     // the view in pixels
uniform int uPixelSize;      // pixels a ray colours each way
#ifdef LATER_RAYS
// For a pixel's second and further rays (setting "Supersampling"): the first ray's pictures. Where the first rays
// show nothing to smooth (see main), the later ones aren't cast.
uniform usampler2D uFirstColor;
uniform sampler2D uFirstDepth, uFirstTrue;
const float FlatContrast = 26.0 / 255.0; // a pixel within this of all its neighbours, a colour channel each, has nothing to smooth (two shades of a dither are nearer than this; the next voxel's colour mostly isn't)
const float NearContrast = 64.0 / 255.0; // the same where a voxel covers several pixels: there its neighbours' colours are the texture drawn large, not shimmer, and only a strong line between two of them is worth more rays
#endif
uniform vec2 uRayOffset;     // where in its pixel the ray goes, from the middle (0, 0: the CPU's ray; else one of several, setting "Supersampling")
uniform float uGhostScale;
uniform int uSeeThroughCrops;
uniform vec4 uSwingA[16];    // a swinging door: hinge x, y, direction x, y
uniform vec4 uSwingB[16];    // ... z0, z1, its model's number, 1 if it was a west wall

layout(location = 0) out uint outColor;   // the palette colour
layout(location = 1) out float outDepth;  // along the view axis
layout(location = 2) out uint outGlow;    // 1 where the pixel shines, 3 a UFO's light (for the bloom)
layout(location = 3) out vec4 outTrue;    // the pixel in full colour (setting "True colour"): red, green, blue; and the glow again (as outGlow / 255)
#ifdef RESULTS
layout(location = 4) out uvec4 outResult; // FirstPersonGpu::RayResult (the check against the CPU's rays only)
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
// The finer picture, on the card only (settings, all off by default: with them off the picture is the CPU's).
uniform int uSoftShadows, uAllLightShadows, uAmbientOcclusion;
#ifdef COLORED_LIGHTS
uniform vec4 uColorLightAt[16]; // setting "Coloured lights": each light's place (voxels) and reach
uniform vec4 uColorLightOf[16]; // ... its colour (0..1 each) and strength
uniform int uColorLightCount;
#endif
#ifdef LIGHT_SHAFTS
uniform int uShaftSmoke;        // setting "Light shafts": there is smoke on the map (else only the sea's water scatters light)
uniform float uShaftAir;        // ... how much of the light a voxel of clear air or water throws towards the eye (0 above the sea)
#endif
uniform int uReflections;       // setting "Reflections": the sea mirrors the sky and what stands in it
uniform int uSmoothLight; // setting "Smooth light": the full-colour picture blends between the palette's shades (and into the haze) where the palette's picture dithers
// (The coloured lights, the reflections and the light shafts are built into the program only when their settings are on
// (COLORED_LIGHTS, REFLECTIONS, LIGHT_SHAFTS; see FirstPersonGpu.cpp, programFor): with their code merely present, one driver's program ran at half
// the speed for everything else, ambient occlusion above all, though none of it was reached.)
// The pixel in full colour, where the shading worked one out that the palette hasn't (see shadeHit); else it is the palette colour's own.
vec3 fullColor;
bool fullColorSet;
vec3 paletteRgb(uint color) { int entry = int(color) * 3; return vec3(float(texelFetch(uBlend, entry).r), float(texelFetch(uBlend, entry + 1).r), float(texelFetch(uBlend, entry + 2).r)); }
const float LampReach = 120.0;     // how far a lamp lights (voxels)
const float SunDiscRadius = 0.04;  // the sun as a disc this wide (the tangent of its half-angle): the width of a shadow's soft edge, a 25th of the way it fell
const float CreaseShades = 4.5;    // how many palette shades the deepest corner is darkened
const float CreaseFadeFrom = 260.0, CreaseFadeTo = 420.0; // corners aren't darkened beyond this far (voxels): too fine to see, and they would shimmer
uniform float uUfoPulse[32], uUfoBreath, uUfoFaceShade[64];

const int SkyEarth = 0, SkySpace = 1, SkyOrbit = 2, SkyMoon = 3, SkyMars = 4, SkySea = 5, SkyVoid = 6;
// The rows of uTables.
const int HazeRow = 0, GlassRow = 9, UnseenRow = 10, FlareRow = 11, SceneRow = 12, BlackRow = 13, DarkFadeRow = 14, UfoRows = 14 + DarkFadeSteps, RowsPerUfo = 14;
// Where the scene's colours sit in their row.
const int SceneSky = 0, SceneCloud = 32, SceneHaze = 40, SceneStar = 41, SceneMoon = 42, SceneSun = 43, SceneTapeYellow = 44, SceneTapeBlack = 45,
	ScenePost = 46, SceneFaintStar = 47, SceneEarth = 48, SceneUnexplored = 52, SceneRock = 53, SceneWater = 54, SceneRockBase = 57;
const int TapeFont[30] = int[30](5, 5, 2, 5, 5, 0, 0, 7, 0, 0, 7, 4, 4, 4, 7, 7, 5, 5, 5, 7, 5, 7, 7, 5, 5, 0, 0, 0, 0, 0);
const int TapeText[7] = int[7](0, 1, 2, 3, 4, 5, 5);
const int TapeTextLength = 7;

uint table(int row, uint color) { return texelFetch(uTables, row * 256 + int(color)).r; }
uint scene(int index) { return texelFetch(uTables, SceneRow * 256 + index).r; }
// A group's darkest colour on its way to black by how dark the place is, through the palette's darker colours (see the view's fadedToBlack).
uint fadedToBlack(uint groupEnd, float blackShare, float threshold)
{
	int fadeStep = int(blackShare * float(DarkFadeSteps + 1) + threshold);
	return fadeStep <= 0 ? groupEnd : fadeStep > DarkFadeSteps ? 0x0Fu : table(DarkFadeRow + fadeStep - 1, groupEnd);
}
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
		int brightestLamp = -1, nextLamp = -1, thirdLamp = -1; // (the second and third strongest: for "Shadows from every light")
		float brightestPower = 0.0, nextPower = 0.0, thirdPower = 0.0;
		for (int lamp = 0; lamp < uLampCount; ++lamp)
		{
			float dx = uLamps[lamp].x - point.x, dy = uLamps[lamp].y - point.y, dz = uLamps[lamp].z - point.z;
			float span = sqrt(dx * dx + dy * dy + dz * dz);
			if (span > LampReach) continue;
			float power = uLamps[lamp].w * (1.0 - span / LampReach) * (1.0 - span / LampReach);
			if (power > brightestPower) { thirdPower = nextPower; thirdLamp = nextLamp; nextPower = brightestPower; nextLamp = brightestLamp; brightestPower = power; brightestLamp = lamp; }
			else if (power > nextPower) { thirdPower = nextPower; thirdLamp = nextLamp; nextPower = power; nextLamp = lamp; }
			else if (power > thirdPower) { thirdPower = power; thirdLamp = lamp; }
		}
		// The strongest lamp alone; with the setting "Shadows from every light", the three strongest in reach (a
		// weaker one's light and shadow are under a shade: a ray for each of sixteen lamps cost a big lit base 19 ms a
		// frame at 1080p): the strongest that lights the point lights it, the strongest it is hidden from darkens it
		// (with one lamp, the same thing). (One loop for both, so the shader holds the shadow ray's walk once here.)
		bool everyLamp = uAllLightShadows != 0;
		int firstLamp = everyLamp ? 0 : brightestLamp, endLamp = everyLamp ? uLampCount : brightestLamp + 1;
		if (brightestLamp < 0 || brightestPower <= 0.03) endLamp = firstLamp;
		float night = uUnderwater != 0 ? 1.0 : uNight, lit = 0.0, dark = 0.0;
		for (int lamp = firstLamp; lamp < endLamp; ++lamp)
		{
			if (lamp != brightestLamp && lamp != nextLamp && lamp != thirdLamp) continue;
			vec3 lampPos = uLamps[lamp].xyz;
			float dx = lampPos.x - point.x, dy = lampPos.y - point.y, dz = lampPos.z - point.z;
			float span = sqrt(dx * dx + dy * dy + dz * dz);
			if (span > LampReach || span < 0.01) continue;
			float power = uLamps[lamp].w * (1.0 - span / LampReach) * (1.0 - span / LampReach);
			if (power <= 0.03) continue;
			vec3 towardsLamp = vec3(dx / span, dy / span, dz / span);
			float facing = normal.x * towardsLamp.x + normal.y * towardsLamp.y + normal.z * towardsLamp.z;
			float asLit = power * 6.0 * (0.4 + 0.6 * facing) * night, asDark = power * 3.0 * night;
			vec3 nearLamp = vec3(lampPos.x - towardsLamp.x * 4.0, lampPos.y - towardsLamp.y * 4.0, lampPos.z - towardsLamp.z * 4.0);
			if (facing <= 0.0) dark = max(dark, asDark);
			else if (everyLamp && asLit <= lit && asDark <= dark) continue; // (it would change nothing: no ray)
			else if (shadows && shadowed(point, nearLamp)) dark = max(dark, asDark);
			else lit = max(lit, asLit);
		}
		light += lit - dark;
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

#ifdef COLORED_LIGHTS
/**
 * The coloured lights' share of a point's full colour (setting "Coloured lights": a shot in flight, an explosion, a
 * fire, a muzzle flash): each in reach that the face looks towards adds the surface's own colour in the light's, by
 * the square of how near it is; and, with shadows on, only where it is in sight.
 * @param rgb The point's colour so far (0..255 each).
 * @param surface The surface's own colour, unshaded.
 */
vec3 coloredLight(vec3 rgb, vec3 surface, vec3 point, vec3 normal)
{
	for (int light = 0; light < uColorLightCount; ++light)
	{
		vec3 towards = uColorLightAt[light].xyz - point;
		float span = length(towards), reach = uColorLightAt[light].w;
		if (span >= reach) continue;
		float facing = span > 0.5 ? dot(normal, towards) / span : 1.0;
		if (facing <= 0.0) continue;
		float power = (1.0 - span / reach) * (1.0 - span / reach) * uColorLightOf[light].w * (0.35 + 0.65 * facing);
		if (power < 0.02) continue;
		if (uShadows != 0 && span > 6.0 && shadowed(point, uColorLightAt[light].xyz - towards / span * 3.0)) continue;
		rgb += (surface * 0.9 + vec3(40.0)) * uColorLightOf[light].rgb * power;
	}
	return min(rgb, vec3(255.0));
}

#endif
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
	for (int rank = 0; rank < count && (rank < 2 || uAllLightShadows != 0); ++rank) // (setting "Shadows from every light": every flare gets its ray)
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
		if (uSmoothLight != 0)
		{
			float band = clamp(gradient + mottle, 0.0, 31.0);
			fullColor = mix(paletteRgb(scene(SceneSky + int(band))), paletteRgb(scene(SceneSky + min(31, int(band) + 1))), band - floor(band));
			fullColorSet = true;
		}
		return scene(SceneSky + clamp(int(gradient + mottle + dither(pixelX, pixelY)), 0, 31));
	}
	uint color = scene(SceneSky + clamp(int(gradient + dither(pixelX, pixelY)), 0, 31));
	// (In full colour, setting "Smooth light": the sky's bands and the clouds' edges blended; the sun, the moon and the stars as they are.)
	vec3 skyRgb = vec3(0.0);
	if (uSmoothLight != 0)
	{
		float band = clamp(gradient, 0.0, 31.0);
		skyRgb = mix(paletteRgb(scene(SceneSky + int(band))), paletteRgb(scene(SceneSky + min(31, int(band) + 1))), band - floor(band));
		fullColor = skyRgb;
		fullColorSet = true;
	}
	if (uUnderwater != 0)
	{
		return color;
	}
	float daylight = 1.0 - uNight;
	float sunDot = dot(rayDirection, uSunDir);
	if (daylight > 0.4 && sunDot > (uSky == SkyMars ? 0.9998 : 0.9995))
	{
		fullColorSet = false;
		return scene(SceneSun);
	}
	if (uNight > 0.4 && uSky != SkyMars)
	{
		float moonDot = dot(rayDirection, uMoonDir);
		if (moonDot > 0.9992)
		{
			fullColorSet = false;
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
		if (uSmoothLight != 0 && coverage > 0.0)
		{
			float level = clamp((0.8 - density) * 10.0, 0.0, 7.0);
			vec3 cloudRgb = mix(paletteRgb(scene(SceneCloud + int(level))), paletteRgb(scene(SceneCloud + min(7, int(level) + 1))), level - floor(level));
			fullColor = mix(skyRgb, cloudRgb, coverage);
		}
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
			fullColorSet = false;
			return scene(SceneStar);
		}
	}
	return color;
}

#ifdef REFLECTIONS
/**
 * The sky's full colour a way (for a reflection; the pixel's own full colour is left as it was).
 */
vec3 skyRgbTowards(vec3 direction, int pixelX, int pixelY)
{
	bool wasSet = fullColorSet;
	vec3 kept = fullColor;
	fullColorSet = false;
	uint sky = skyColor(direction, pixelX, pixelY);
	vec3 rgb = fullColorSet ? fullColor : paletteRgb(sky);
	fullColorSet = wasSet;
	fullColor = kept;
	return rgb;
}

/**
 * The way the sea mirrors a ray at a point (setting "Reflections"): off the water's face, tilted by the waves' slope
 * there, never down into it; and how much of the mirrored picture shows, more the flatter the ray meets the water.
 */
vec3 seaMirror(vec3 rayDirection, float x, float y, float span, out float share)
{
	float slopeX = waveHeight(x + 1.0, y, span) - waveHeight(x - 1.0, y, span), slopeY = waveHeight(x, y + 1.0, span) - waveHeight(x, y - 1.0, span);
	vec3 waterNormal = normalize(vec3(-slopeX * 0.5, -slopeY * 0.5, 1.0));
	vec3 mirrored = reflect(rayDirection, waterNormal);
	mirrored.z = max(mirrored.z, 0.03);
	float glancing = 1.0 - clamp(-dot(rayDirection, waterNormal), 0.0, 1.0);
	share = 0.1 + 0.65 * glancing * glancing * glancing;
	return normalize(mirrored);
}

#endif
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
			uint unlit = color;
			bool pastGroup = !groundIsWater && int(color & 15u) + shades > 15;
			color = pastGroup ? fadedToBlack(color | 15u, blackShare, dither(pixelX + 1, pixelY + 2))
				: blackShare > dither(pixelX + 1, pixelY + 2) ? shadeColor(color, shades) : ((color & 0xF0u) | uint(min(darkest, int(color & 15u) + max(0, shades))));
			float haze = uUnderwater != 0 ? smoothStep(uMaxDistance * 0.12, uMaxDistance * 0.65, groundDistance)
				: smoothStep(uMaxDistance * hazeStart() * 0.67, uMaxDistance * 1.6, groundDistance);
#ifdef REFLECTIONS
			bool mirrors = uReflections != 0 && groundIsWater && uUnderwater == 0;
#else
			bool mirrors = false;
#endif
			if (uSmoothLight != 0 || mirrors)
			{
				// (In full colour: the shade's fraction, the black of a dark place and the haze blended, as shadeHit does;
				// and the sea past the map's edge mirrors the sky.)
				float exact = max(0.0, float(uAmbient) + uFaceShade[4] + waveShade + distanceShade(groundDistance));
				if (uSmoothLight == 0) exact = floor(exact + dither(pixelX, pixelY));
				int low = int(exact);
				vec3 rgb = mix(paletteRgb((unlit & 0xF0u) | uint(min(darkest, int(unlit & 15u) + low))), paletteRgb((unlit & 0xF0u) | uint(min(darkest, int(unlit & 15u) + low + 1))), exact - float(low));
				if (blackShare > 0.0) rgb = mix(rgb, mix(paletteRgb(shadeColor(unlit, low)), paletteRgb(shadeColor(unlit, low + 1)), exact - float(low)), blackShare);
#ifdef REFLECTIONS
				if (mirrors)
				{
					float share;
					vec3 mirrored = seaMirror(rayDirection, groundX, groundY, groundDistance, share);
					rgb = mix(rgb, skyRgbTowards(mirrored, pixelX, pixelY), share);
				}
#endif
				float hazeShare = uSmoothLight != 0 ? haze : float(clamp(int(haze * float(HazeSteps) + dither(pixelX + 2, pixelY + 1)), 0, HazeSteps)) / float(HazeSteps);
				fullColor = mix(rgb, paletteRgb(scene(SceneHaze)), hazeShare);
				fullColorSet = true;
			}
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

// Whether a voxel is filled, this frame's models only (crease's look-ups: cheaper than voxelColorAt's).
float filledIn(uint parts[4], ivec3 local)
{
	for (int part = 0; part < 4; ++part)
	{
		if (parts[part] != 0u && modelColor(parts[part], local) != 0u) return 1.0;
	}
	return 0.0;
}

float filledAt(ivec3 world)
{
	if (world.x < 0 || world.y < 0 || world.z < 0 || world.x >= uSize.x * 16 || world.y >= uSize.y * 16 || world.z >= uSize.z * 24) return 0.0;
	uvec4 now = texelFetch(uTiles, (((world.z / 24) * uSize.y + world.y / 16) * uSize.x + world.x / 16) * 4);
	if (now.x == 0u && now.y == 0u) return 0.0;
	return filledIn(uint[4](now.x & 0xFFFFu, now.x >> 16, now.y & 0xFFFFu, now.y >> 16), ivec3(world.x % 16, world.y % 16, world.z % 24));
}

/**
 * How closed in a point on a face is, 0 (open) to 1: the share of sixteen places round it, out
 * from the face and to its sides, a ring near and a ring further, that hold a voxel. The foot
 * of a wall, an inner corner, the gap between two crates. Each near place counts the four
 * voxels round it in the face's plane by how near it is to each, so the darkening has no steps
 * where a place passes from one voxel to the next; a far place counts its one voxel (its step
 * is a fifth of a shade). Most places are in open air: the four voxels of one, when a single
 * empty tile holds them all, cost one look-up.
 */
float crease(vec3 point, vec3 normal)
{
	vec3 sideA = abs(normal.yzx), sideB = abs(normal.zxy); // (the normal lies along an axis: these are the other two)
	ivec3 stepA = ivec3(sideA), stepB = ivec3(sideB), mapVoxels = uSize * CellVoxels;
	float closed = 0.0;
	for (int place = 0; place < 8; ++place)
	{
		float angle = float(place) * 0.7853982;
		vec3 at = point + normal + (cos(angle) * sideA + sin(angle) * sideB) * 1.6 - 0.5 * (sideA + sideB);
		vec3 cell = floor(at);
		float alongA = dot(at - cell, sideA), alongB = dot(at - cell, sideB);
		ivec3 corner = ivec3(cell), opposite = corner + stepA + stepB;
		ivec3 cornerTile = corner / CellVoxels;
		float held;
		if (corner.x >= 0 && corner.y >= 0 && corner.z >= 0 && opposite.x < mapVoxels.x && opposite.y < mapVoxels.y && opposite.z < mapVoxels.z && cornerTile == opposite / CellVoxels)
		{
			uvec4 now = texelFetch(uTiles, ((cornerTile.z * uSize.y + cornerTile.y) * uSize.x + cornerTile.x) * 4);
			if (now.x == 0u && now.y == 0u) continue;
			uint parts[4] = uint[4](now.x & 0xFFFFu, now.x >> 16, now.y & 0xFFFFu, now.y >> 16);
			ivec3 local = corner - cornerTile * CellVoxels;
			held = mix(mix(filledIn(parts, local), filledIn(parts, local + stepA), alongA), mix(filledIn(parts, local + stepB), filledIn(parts, local + stepA + stepB), alongA), alongB);
		}
		else
		{
			held = mix(mix(filledAt(corner), filledAt(corner + stepA), alongA), mix(filledAt(corner + stepB), filledAt(opposite), alongA), alongB);
		}
		closed += held * 0.08;
	}
	for (int place = 0; place < 8; ++place)
	{
		float angle = float(place) * 0.7853982 + 0.3926991;
		closed += filledAt(ivec3(floor(point + normal * 3.0 + (cos(angle) * sideA + sin(angle) * sideB) * 4.5))) * 0.045;
	}
	return closed;
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
			// One ray at the sun; with the setting "Soft sun shadows", four at points round its disc, the ring of them
			// turned from pixel to pixel: the share that is blocked is the shadow's depth, so its edge fades.
			// The two at the disc's rim, opposite each other, go first: where both see the sun, or neither
			// does, the point is clear of a shadow's edge and the other two are not cast (most of a picture).
			// (One loop for all of it, so the shader holds the shadow ray's walk once here.)
			int sunRays = uSoftShadows != 0 ? 4 : 1;
			vec3 across = normalize(cross(uSunDir, abs(uSunDir.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
			vec3 along = cross(uSunDir, across);
			float turn = dither(pixelX + 1, pixelY + 3) * 1.5707963;
			float blocked = 0.0;
			for (int turnOf = 0; turnOf < sunRays; ++turnOf)
			{
				int sunRay = turnOf < 2 ? turnOf * 2 + 1 : (turnOf - 2) * 2; // (the rim's two, then the inner two)
				vec3 towardsSun = uSunDir;
				if (sunRays > 1)
				{
					float angle = turn + float(sunRay) * 1.5707963;
					towardsSun += (cos(angle) * across + sin(angle) * along) * (SunDiscRadius * ((sunRay & 1) != 0 ? 1.0 : 0.55));
				}
				if (shadowed(surfacePoint, surfacePoint + towardsSun * 260.0)) blocked += 1.0 / float(sunRays);
				if (turnOf == 1 && blocked != 1.0 / float(sunRays))
				{
					blocked *= 2.0; // (both rim rays agree: all four would)
					break;
				}
			}
			sunShadow = 3.0 * daylight * blocked;
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
	// (Setting "Ambient occlusion": corners and creases darker.)
	float creaseShade = 0.0;
	if (uAmbientOcclusion != 0 && !hit.emissive && !onSea && hit.t < CreaseFadeTo)
	{
		creaseShade = CreaseShades * crease(rayOrigin + rayDirection * hit.t + normal * 0.5, normal) * (1.0 - smoothStep(CreaseFadeFrom, CreaseFadeTo, hit.t));
		shade += creaseShade;
	}
	// Whole palette shades, the fraction dithered; black only where the place is really dark.
	int wholeShades = int(max(0.0, shade + dither(pixelX, pixelY)));
	float placeShade = shade - faceShading - distanceShade(hit.t) - waveShade - sunShadow - creaseShade;
	float blackShare = clamp((placeShade - DarkOverflowFrom) / (DarkOverflowTo - DarkOverflowFrom), 0.0, 1.0);
	bool darkEnoughForBlack = !onSteadyUfo && blackShare > dither(pixelX + 1, pixelY + 2);
	if (rockCeilingMiddle >= 0 && !darkEnoughForBlack)
	{
		wholeShades = min(wholeShades, max(0, 15 - RockMottleShades - rockCeilingMiddle));
	}
	int darkest = onSea ? max(int(baseColor & 15u), WaterDarkestShade) : 15;
	uint color = darkEnoughForBlack ? shadeColor(baseColor, wholeShades) : ((baseColor & 0xF0u) | uint(min(darkest, int(baseColor & 15u) + wholeShades)));
	if (!onSea && !onSteadyUfo && int(baseColor & 15u) + wholeShades > 15)
	{
		color = fadedToBlack(baseColor | 15u, blackShare, dither(pixelX + 1, pixelY + 2));
	}
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
#ifdef REFLECTIONS
	bool mirrors = uReflections != 0 && onSea && uUnderwater == 0;
#else
	bool mirrors = false;
#endif
#ifdef COLORED_LIGHTS
	bool colorLit = uColorLightCount > 0;
#else
	bool colorLit = false;
#endif
	if (uSmoothLight != 0 || colorLit || mirrors)
	{
		// (Setting "Smooth light", for the full-colour picture: where the palette's picture dithers between two shades,
		// the mix of the two by the shade's fraction; likewise into black where the place is dark, into the flare's red,
		// and into the haze. A UFO's tint and unseen terrain keep the palette's colour; only their haze is smooth.
		// Setting "Coloured lights": their light added, before the haze.)
		vec3 rgb = paletteRgb(color);
		if (uSmoothLight != 0 && !unseen && ufoLighting < 0)
		{
			float exact = max(0.0, shade);
			int low = int(exact);
			float share = exact - float(low);
			int cap = rockCeilingMiddle >= 0 ? max(0, 15 - RockMottleShades - rockCeilingMiddle) : 99;
			uint lowLit = (baseColor & 0xF0u) | uint(min(darkest, int(baseColor & 15u) + min(low, cap)));
			uint highLit = (baseColor & 0xF0u) | uint(min(darkest, int(baseColor & 15u) + min(low + 1, cap)));
			rgb = mix(paletteRgb(lowLit), paletteRgb(highLit), share);
			if (!onSteadyUfo && blackShare > 0.0)
			{
				rgb = mix(rgb, mix(paletteRgb(shadeColor(baseColor, low)), paletteRgb(shadeColor(baseColor, low + 1)), share), blackShare);
			}
			float red = clamp(flare * 1.5 - 0.04, 0.0, 1.0);
			if (red > 0.0)
			{
				rgb = mix(rgb, mix(paletteRgb(table(FlareRow, lowLit)), paletteRgb(table(FlareRow, highLit)), share), red);
			}
		}
#ifdef COLORED_LIGHTS
		if (colorLit && !hit.emissive && !unseen) rgb = coloredLight(rgb, paletteRgb(baseColor), surfacePoint, normal);
#endif
#ifdef REFLECTIONS
		if (mirrors)
		{
			// (Setting "Reflections": one ray off the sea's face; what it meets in the game's light there, or the sky.)
			float share;
			vec3 mirrored = seaMirror(rayDirection, surfacePoint.x, surfacePoint.y, hit.t, share);
			Hit met;
			int metTile, metAxis;
			float metDistance;
			vec3 seen;
			if (castRay(surfacePoint, mirrored, min(uMaxDistance, 640.0), 2.0, 2.0, met, metTile, metDistance, metAxis) == RAY_HIT)
			{
				float metGlow;
				float metShade = max(0.0, lightAt(surfacePoint + mirrored * met.t, metGlow) + distanceShade(met.t) + 1.5);
				seen = mix(paletteRgb((met.color & 0xF0u) | uint(min(15, int(met.color & 15u) + int(metShade)))),
					paletteRgb((met.color & 0xF0u) | uint(min(15, int(met.color & 15u) + int(metShade) + 1))), metShade - floor(metShade));
			}
			else
			{
				seen = skyRgbTowards(mirrored, pixelX, pixelY);
			}
			rgb = mix(rgb, seen, share);
		}
#endif
		float haze = terrainHaze(hit.t);
		if (uSmoothLight == 0) haze = float(clamp(int(haze * float(HazeSteps) + dither(pixelX + 2, pixelY + 1)), 0, HazeSteps)) / float(HazeSteps); // (in the palette's steps, dithered as there)
		if (haze > 0.0) rgb = mix(rgb, paletteRgb(scene(SceneHaze)), haze);
		fullColor = rgb;
		fullColorSet = true;
	}
	return hazeColor(color, terrainHaze(hit.t), pixelX + 2, pixelY + 1);
}

#ifdef LIGHT_SHAFTS
const int ShaftSteps = 20;        // places along a ray where the light in the air is added up
const float ShaftReach = 320.0;   // ... over this far at most (voxels)
const float ShaftSmoke = 0.001;   // how much of the light a voxel of smoke throws towards the eye, for each of a tile's units of smoke

// The smoke at a point: blended between the eight tiles round it, so a cloud's light has no tile's edges.
float smokeAt(vec3 point)
{
	vec3 grid = point / CellSize - 0.5;
	ivec3 low = ivec3(floor(grid));
	vec3 along = grid - vec3(low);
	float smoke = 0.0;
	for (int corner = 0; corner < 8; ++corner)
	{
		ivec3 tile = low + ivec3(corner & 1, (corner >> 1) & 1, corner >> 2);
		if (tile.x < 0 || tile.y < 0 || tile.z < 0 || tile.x >= uSize.x || tile.y >= uSize.y || tile.z >= uSize.z) continue;
		float held = float((tileLightTexel((tile.z * uSize.y + tile.y) * uSize.x + tile.x).z >> 8) & 0xFFu);
		vec3 share = mix(1.0 - along, along, vec3(corner & 1, (corner >> 1) & 1, corner >> 2));
		smoke += held * share.x * share.y * share.z;
	}
	return smoke;
}

/**
 * A ray's full colour with the light the air throws towards the eye on its way (setting "Light shafts"): the
 * flashlight's cone, the flares, the lamps by night and the coloured lights show as beams and glows where there is
 * smoke, and everywhere under the sea. The light is added up at ShaftSteps places along the ray, each by how much
 * smoke (or water) is there. Nothing hides a light from the air here: no shadow rays (twenty a pixel would cost
 * more than the whole picture), so a lamp behind a wall still glows in the smoke on this side of it.
 * @param rgb The ray's colour (0..255 each).
 * @param span How far the ray went (voxels).
 */
vec3 lightShafts(vec3 rgb, vec3 rayDirection, float span)
{
	float stride = min(span, ShaftReach) / float(ShaftSteps);
	float night = uUnderwater != 0 ? 1.0 : uNight;
	vec3 added = vec3(0.0);
	for (int place = 0; place < ShaftSteps; ++place)
	{
		vec3 at = uEye + rayDirection * ((float(place) + 0.5) * stride);
		float thick = uShaftAir + (uShaftSmoke != 0 ? smokeAt(at) * ShaftSmoke : 0.0);
		if (thick <= 0.0) continue;
		vec3 light = vec3(0.0);
		if (uFlashlight != 0)
		{
			// (The flashlight's cone, as pointLight has it.)
			float range = uFlashlight == 2 ? 16.0 * 20.0 : 16.0 * 14.0;
			vec3 fromLight = at - uFlashlightPos;
			float away = length(fromLight);
			if (away > 0.5 && away < range)
			{
				float cosine = dot(fromLight, uForwardInVoxels) / away;
				float beam = uFlashlight == 2 ? smoothStep(0.974, 0.990, cosine) + 0.18 * smoothStep(0.914, 0.974, cosine) : smoothStep(0.8, 0.96, cosine);
				float falloff = 1.0 - (uFlashlight == 2 ? 0.85 : 1.0) * away / range;
				// (Not right at the lamp: the air there, all but in the eye, would glare over the corner of the picture.)
				light += vec3(0.88, 0.93, 1.0) * (beam * falloff * falloff * smoothStep(16.0, 96.0, away));
			}
		}
		for (int flare = 0; flare < uFlareCount; ++flare)
		{
			float near = max(0.0, 1.0 - distance(uFlares[flare].xyz, at) / FlareReach);
			light += vec3(1.0, 0.45, 0.3) * (near * near * uFlares[flare].w * uFlareFlicker[flare]);
		}
		if (night > 0.3)
		{
			for (int lamp = 0; lamp < uLampCount; ++lamp)
			{
				float near = max(0.0, 1.0 - distance(uLamps[lamp].xyz, at) / LampReach);
				light += vec3(1.0, 0.9, 0.72) * (near * near * uLamps[lamp].w * night * 0.6);
			}
		}
#ifdef COLORED_LIGHTS
		for (int colored = 0; colored < uColorLightCount; ++colored)
		{
			float near = max(0.0, 1.0 - distance(uColorLightAt[colored].xyz, at) / uColorLightAt[colored].w);
			light += uColorLightOf[colored].rgb * (near * near * uColorLightOf[colored].w);
		}
#endif
		added += light * (thick * stride);
	}
	return min(rgb + added * 255.0, vec3(255.0));
}
#endif

/**
 * One ray through this fragment's block of pixels and what it shows.
 * @param offset Where in the block it goes, from the block's middle, in blocks (0, 0: the middle, the CPU's ray).
 * @param first The block's first ray: the one whose findings go to the check against the CPU's.
 */
void shadeRay(vec2 offset, bool first, out uint rayColor, out float rayDepth, out uint rayGlow, out vec3 rayFullColor)
{
	fullColorSet = false;
	// The block of pixels this ray colours (rows from the top), and its place on the image plane.
	int blockColumn = int(gl_FragCoord.x), blockRow = int(gl_FragCoord.y);
	int topRow = blockRow * uPixelSize, leftColumn = blockColumn * uPixelSize;
	float planeV = (1.0 - (float(topRow) + float(uPixelSize) * (0.5 + offset.y)) * 2.0 / float(uViewSize.y)) * uHalfPlane.y;
	float planeU = ((float(leftColumn) + float(uPixelSize) * (0.5 + offset.x)) * 2.0 / float(uViewSize.x) - 1.0) * uHalfPlane.x;
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
	if (first) outResult = uvec4(uint(hitTile),
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
		fullColor = fullColor * 0.7 + vec3(150.0, 180.0, 205.0) * 0.3; // (as the glass table mixes each colour: a little paler and bluer)
	}
	rayFullColor = fullColorSet ? fullColor : paletteRgb(color);
#ifdef LIGHT_SHAFTS
	rayFullColor = lightShafts(rayFullColor, rayDirection, distanceAlongRay);
#endif
	rayColor = color;
	rayDepth = distanceAlongRay / directionLength;
	// Whether it shines (a UFO's light counts three times in the bloom).
	bool glowing = rayEnd == RAY_HIT && hit.emissive && distanceAlongRay < Infinity;
	rayGlow = !glowing ? 0u : hit.part >= 0 && (texelFetch(uModelInfo, int(texelFetch(uTiles, hitTile * 4)[hit.part / 2] >> uint(16 * (hit.part & 1))) & 0xFFFF).z >> 24) != 0u ? 3u : 1u;
}

void main()
{
#ifdef LATER_RAYS
	// A second or further ray of the pixel: only where the first rays show an edge or fine texture: this pixel's full colour
	// against its eight neighbours', and its depth. On a flat stretch (most of a picture at a high resolution, where a
	// voxel covers many pixels) the pixel's first ray is handed on as it is: several rays there would all show the same.
	ivec2 at = ivec2(gl_FragCoord.xy), last = textureSize(uFirstTrue, 0) - 1;
	vec4 here = texelFetch(uFirstTrue, at, 0);
	float hereDepth = texelFetch(uFirstDepth, at, 0).r;
	float contrast = 0.0, depthStep = 0.0;
	for (int side = 0; side < 8; ++side)
	{
		ivec2 beside = clamp(at + ivec2(side < 3 ? -1 : side < 5 ? 0 : 1, side == 0 || side == 3 || side == 5 ? -1 : side == 1 || side == 6 ? 0 : 1), ivec2(0), last);
		vec3 apart = abs(texelFetch(uFirstTrue, beside, 0).rgb - here.rgb);
		contrast = max(contrast, max(apart.r, max(apart.g, apart.b)));
		float besideDepth = texelFetch(uFirstDepth, beside, 0).r;
		depthStep = max(depthStep, abs(besideDepth - hereDepth) / max(1.0, min(besideDepth, hereDepth)));
	}
	// (How many voxels wide the pixel is where it hits: under half of one the texture is drawn large.)
	float pixelSpan = hereDepth * 2.0 * uHalfPlane.x * float(uPixelSize) / float(uViewSize.x);
	if (contrast < mix(NearContrast, FlatContrast, clamp(pixelSpan * 2.0, 0.0, 1.0)) && depthStep < 0.03)
	{
		outColor = texelFetch(uFirstColor, at, 0).r;
		outDepth = hereDepth;
		outGlow = uint(here.a * 255.0 + 0.5);
		outTrue = here;
		return;
	}
#endif
	vec3 rayFullColor;
	shadeRay(uRayOffset, true, outColor, outDepth, outGlow, rayFullColor);
	outTrue = vec4(rayFullColor / 255.0, float(outGlow) / 255.0);
}
)";

/*
 * Several rays a pixel (setting "Supersampling"). The rays' program above casts one ray a pixel;
 * for several it is run once for each, the ray set off from the pixel's middle (uRayOffset),
 * each run into pictures of its own. This small program then makes the frame's picture of
 * them: where a pixel's rays all show one colour, that colour (a flat surface comes out exactly
 * as without them, dither and all); where they differ (an edge, a far fence, fine texture), the
 * palette colour nearest the mix of theirs (uBlend: the palette, then the nearest colour for
 * each cell of a 32 x 32 x 32 colour cube); the depth of the nearest; and it shines if any does.
 * (It is its own program, not a loop over the rays in the one above: at least one driver took
 * a minute to build that.)
 */
const char *const ResolveSource = R"(
uniform usampler2D uColor0, uColor1, uColor2, uColor3;
uniform sampler2D uDepth0, uDepth1, uDepth2, uDepth3;
uniform sampler2D uTrue0, uTrue1, uTrue2, uTrue3; // each ray in full colour, the glow with it (see outTrue)
uniform usamplerBuffer uBlend;
uniform int uSamples;
layout(location = 0) out uint outColor;
layout(location = 1) out float outDepth;
layout(location = 2) out uint outGlow;
layout(location = 3) out vec4 outTrue;

void main()
{
	ivec2 at = ivec2(gl_FragCoord.xy);
	uint colors[4] = uint[4](texelFetch(uColor0, at, 0).r, texelFetch(uColor1, at, 0).r, 0u, 0u);
	float depths[4] = float[4](texelFetch(uDepth0, at, 0).r, texelFetch(uDepth1, at, 0).r, 0.0, 0.0);
	vec4 full[4] = vec4[4](texelFetch(uTrue0, at, 0), texelFetch(uTrue1, at, 0), vec4(0.0), vec4(0.0));
	if (uSamples > 2)
	{
		colors[2] = texelFetch(uColor2, at, 0).r; depths[2] = texelFetch(uDepth2, at, 0).r; full[2] = texelFetch(uTrue2, at, 0);
		colors[3] = texelFetch(uColor3, at, 0).r; depths[3] = texelFetch(uDepth3, at, 0).r; full[3] = texelFetch(uTrue3, at, 0);
	}
	uint glows[4] = uint[4](uint(full[0].a * 255.0 + 0.5), uint(full[1].a * 255.0 + 0.5), uint(full[2].a * 255.0 + 0.5), uint(full[3].a * 255.0 + 0.5));
	uint color = colors[0], glow = glows[0];
	float depth = depths[0];
	vec3 mixed = vec3(0.0), fullMixed = vec3(0.0);
	bool allSame = true;
	for (int ray = 0; ray < uSamples; ++ray)
	{
		if (colors[ray] != color) allSame = false;
		int entry = int(colors[ray]) * 3;
		mixed += vec3(float(texelFetch(uBlend, entry).r), float(texelFetch(uBlend, entry + 1).r), float(texelFetch(uBlend, entry + 2).r));
		fullMixed += full[ray].rgb;
		depth = min(depth, depths[ray]);
		glow = max(glow, glows[ray]);
	}
	if (!allSame)
	{
		ivec3 cell = clamp(ivec3(mixed / float(uSamples)) >> 3, ivec3(0), ivec3(31));
		color = texelFetch(uBlend, 768 + (cell.r << 10 | cell.g << 5 | cell.b)).r;
	}
	outColor = color;
	outDepth = depth;
	outGlow = glow;
	outTrue = vec4(fullMixed / float(uSamples), float(glow) / 255.0); // (in full colour: the rays' own mix, no palette between)
}
)";


/**
 * The units and effects, drawn on the card over the rays' picture (see FirstPersonGpu::drawEffects):
 * a pixel walks the frame's primitives that can touch it, in the order the CPU's passes draw
 * them, with the pixel's colour, depth, glow and veils as its own variables, started from the
 * rays' pictures. A pixel never needs another pixel's state, so nothing is read from a picture
 * being drawn: the pass needs no extension (OpenGL 3.3, OpenGL ES 3.2 and the Mac's 4.1 alike).
 * Each kind is the CPU's routine for it, a pixel at a time: FirstPersonView::drawSprite (as its
 * tilted path works a pixel out, from its own ray), drawGroundShade, drawDecals, drawLine3D.
 * A primitive is EffectTexels texels of four floats (see FirstPersonViewGpuEffects.cpp, which
 * writes them, for what each holds).
 */
const char *const EffectsSource = R"(
uniform usampler2D uRayColor, uRayGlow; // the rays' pictures: the palette colour, the glow
uniform sampler2D uRayDepth;            // ... the depth along the view axis
uniform usampler2D uSeeThrough;         // a colour over another, by level: FirstPersonView's see-through tables end to end, 2048 bytes a row
uniform samplerBuffer uPrims;           // the primitives, EffectTexels texels each
uniform usamplerBuffer uBytes;          // the sprites' pixels and opacity maps
uniform usamplerBuffer uBins;           // a number per bin of the screen (and one more): where its list starts; then the lists, a primitive's number each
uniform vec3 uForward, uRight, uUp, uLevelRight, uLevelUp; // the camera, in true proportions (see FirstPersonView::updateBasis)
uniform vec2 uLevel;                    // straight ahead along the ground
uniform float uEyeZ, uFocal;            // the eye's height in voxels; the focal length in pixels
uniform ivec2 uViewSize, uBinGrid;      // the view in pixels; the bins across and down
uniform int uPixelSize;                 // pixels a ray coloured each way
// A frame with the outlines round the units' pictures (settings "Hitboxes" with "Full sprite hitboxes") is drawn in two goes:
// the primitives up to the outlines, then, from that go's pictures (an outline looks at the pixels beside it), the outlines
// and the rest.
uniform usampler2D uNotes;              // the first go's notes (the second go starts from them)
uniform int uContinue;                  // 1: the second go
uniform int uFirstPrim, uEndPrim;       // this go's primitives: from the first up to (not including) the other
uniform int uOutlineBase, uOutlineCount; // the outlined pictures' records (EffectOutlineTexels texels each, from this texel of uPrims), and how many: 0 none
uniform float uOutlineOpacity, uOutlineSlack; // how much of the outlines shows; how much farther than the scene a tested picture may be
uniform int uOutlineGlows;
uniform int uOwnOverNight;              // futuristic night vision's colour group + 1 (0: any other time)
layout(location = 0) out uint outColor;
layout(location = 1) out float outDepth;
layout(location = 2) out uint outGlow;  // the glow (1, 3), 32 where the pixel keeps its own colours under night vision, 64 where see-through smoke lies over it
layout(location = 3) out uvec4 outNotes; // the unit veil left, own colours | smoke veil << 1, and for the pointer: 1 + the last solid sprite's number, 1 + the last pile's

const float Bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
float dither(int x, int y) { return Bayer[(y & 3) * 4 + (x & 3)] / 16.0; }
uint shadeColor(uint color, int shade)
{
	if (shade <= 0) return color;
	uint shaded = color + uint(shade);
	return shaded > 255u || ((shaded ^ color) & 0xF0u) != 0u ? 15u : shaded;
}
uint darkened(uint color, int shades) { return color == 0u ? color : (color & 0xF0u) | uint(min(15, int(color & 15u) + shades)); }
uint seeThrough(int level, uint over, uint under)
{
	if (level >= SeeThroughLevels) return over;
	if (level <= 0) return under;
	int at = ((level - 1) << 16) | (int(over) << 8) | int(under); // (the tables end to end, 2048 bytes a row)
	return texelFetch(uSeeThrough, ivec2(at & 2047, at >> 11), 0).r;
}
vec4 prim(int base, int texel) { return texelFetch(uPrims, base + texel); }

// FirstPersonView::testedPictureBands for one pixel: the outlined picture the line from the eye through it hits (the
// nearest, if several; its record's number + 1, 0 none) where it shows (no farther than the scene there, with the
// slack), and how far ahead. See FirstPerson::cardRay.
int outlineOwner(ivec2 at, out float ownerDepth, out uint ownerColor)
{
	ownerDepth = 0.0;
	ownerColor = 0u;
	int owner = 0;
	if (at.x < 0 || at.y < 0 || at.x >= uViewSize.x || at.y >= uViewSize.y) return 0;
	float sceneDepth = texelFetch(uRayDepth, at, 0).r;
	float across = (float(at.x) + 0.5 - float(uViewSize.x) * 0.5) / uFocal, rise = (float(uViewSize.y) * 0.5 - (float(at.y) + 0.5)) / uFocal;
	vec3 trueRay = uForward + across * uRight + rise * uUp, ray = vec3(trueRay.xy, trueRay.z / ZScale); // (in voxels; one unit ahead, so how far along is the view depth)
	for (int record = 0; record < uOutlineCount; ++record)
	{
		int base = uOutlineBase + record * EffectOutlineTexels;
		vec4 box = texelFetch(uPrims, base);
		if (float(at.x) < box.x || float(at.y) < box.y || float(at.x) > box.z || float(at.y) > box.w) continue;
		vec4 t1 = texelFetch(uPrims, base + 1), t2 = texelFetch(uPrims, base + 2), t3 = texelFetch(uPrims, base + 3), t4 = texelFetch(uPrims, base + 4),
			t5 = texelFetch(uPrims, base + 5), t6 = texelFetch(uPrims, base + 6);
		float facing = dot(ray, t4.xyz);
		if (abs(facing) < 1e-9) continue;
		float along = dot(t1.xyz, t4.xyz) / facing;
		if (along <= 0.0) continue;
		vec3 fromAnchor = ray * along - t1.xyz;
		float column = floor(t5.x * (t3.w + dot(fromAnchor, t2.xyz) * t1.w)), row = floor(t5.x * (t4.w - dot(fromAnchor, t3.xyz) * t2.w));
		if (column < 0.0 || row < 0.0 || column >= t5.w || row >= t6.x || (row >= t4.w * t5.x && t5.y == 0.0)) continue;
		if (texelFetch(uBytes, int(t5.z) + int(row) * int(t5.w) + int(column)).r == 0u) continue;
		if (along < 1.0 || along > sceneDepth + uOutlineSlack) continue;
		if (owner != 0 && ownerDepth <= along) continue;
		owner = int(t6.y);
		ownerDepth = along;
		ownerColor = uint(t6.z);
	}
	return owner;
}

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy), from = pixel / uPixelSize;
	uint color = texelFetch(uRayColor, from, 0).r, glow = texelFetch(uRayGlow, from, 0).r;
	float depth = texelFetch(uRayDepth, from, 0).r;
	int veil = 255, smokeVeil = 0;
	uint own = 0u, lastSolid = 0u, lastPile = 0u;
	if (uContinue != 0)
	{
		uvec4 notes = texelFetch(uNotes, pixel, 0);
		veil = int(notes.r);
		own = notes.g & 1u;
		smokeVeil = int(notes.g >> 1);
		lastSolid = notes.b;
		lastPile = notes.a;
		glow &= 3u;
	}
	if (uOutlineCount > 0)
	{
		// FirstPersonView::outlineBands: a pixel that isn't an outlined picture's, borders one and shows something behind it
		// takes that picture's outline colour (the nearest such picture's).
		float hereDepth, nearest = depth, besideDepth;
		uint hereColor, besideColor, outlineColor = 0u;
		int here = outlineOwner(pixel, hereDepth, hereColor);
		bool outlined = false;
		for (int offsetY = -1; offsetY <= 1; ++offsetY)
		{
			for (int offsetX = -1; offsetX <= 1; ++offsetX)
			{
				int beside = outlineOwner(pixel + ivec2(offsetX, offsetY), besideDepth, besideColor);
				if (beside != 0 && beside != here && besideDepth < nearest)
				{
					outlined = true;
					outlineColor = besideColor;
					nearest = besideDepth;
				}
			}
		}
		if (outlined && !(uOutlineOpacity < 1.0 && dither(pixel.x, pixel.y) >= uOutlineOpacity))
		{
			color = outlineColor;
			glow = uint(uOutlineGlows);
		}
	}
	// The pixel's ray, reaching one unit ahead (so how far along it is the view depth too).
	float across = (float(pixel.x) + 0.5 - float(uViewSize.x) * 0.5) / uFocal, rise = (float(uViewSize.y) * 0.5 - (float(pixel.y) + 0.5)) / uFocal;
	vec3 ray = uForward + across * uRight + rise * uUp;
	int bin = (pixel.y / EffectBin) * uBinGrid.x + pixel.x / EffectBin;
	int listEnd = int(texelFetch(uBins, bin + 1).r);
	for (int entry = int(texelFetch(uBins, bin).r); entry < listEnd; ++entry)
	{
		int number = int(texelFetch(uBins, entry).r), base = number * EffectTexels;
		if (number < uFirstPrim || number >= uEndPrim) continue;
		vec4 box = prim(base, 1);
		if (float(pixel.x) < box.x || float(pixel.y) < box.y || float(pixel.x) >= box.z || float(pixel.y) >= box.w) continue;
		vec4 head = prim(base, 0);
		int kind = int(head.x), flags = int(head.y);
		if (kind == EffectSprite)
		{
			// FirstPersonView::drawSprite: where the ray meets the sprite (its lying rows on the floor in
			// front of the anchor, the rest on the card), which of its pixels that is, and the pixel put.
			vec4 t2 = prim(base, 2), t3 = prim(base, 3), t4 = prim(base, 4), t5 = prim(base, 5), t6 = prim(base, 6), t7 = prim(base, 7), t8 = prim(base, 8);
			vec3 anchor = t2.xyz, cardUp = t3.xyz, cardFacing = t4.xyz;
			float anchorAcross = t2.w, anchorFacing = t3.w, anchorAlong = t4.w, columnSize = t5.x, rowSize = t5.y, lyingRowSize = t5.z, floorHeight = t5.w,
				floorSlope = t6.x, anchorRise = t6.y, anchorX = t6.z, anchorY = t6.w;
			int spriteWidth = int(t7.x), spriteHeight = int(t7.y), pixelsAt = int(t7.z), opacityAt = int(t7.w);
			bool lying = (flags & 1) != 0, belowAnchor = dot(ray, uLevelUp) < anchorRise;
			if (lying && belowAnchor && floorHeight >= 0.0) continue;
			float distance = -1.0, spriteRow = 0.0;
			float rayAlong = ray.x * uLevel.x + ray.y * uLevel.y, descent = ray.z + floorSlope * rayAlong;
			if (lying && belowAnchor && descent < 0.0 && floorHeight < 0.0)
			{
				distance = (floorHeight + floorSlope * anchorAlong) / descent;
				spriteRow = anchorY + (anchorAlong - rayAlong * distance) / lyingRowSize;
			}
			if (spriteRow < anchorY || distance < 0.0)
			{
				float facing = dot(ray, cardFacing);
				if (facing <= 1e-9) continue;
				distance = anchorFacing / facing;
				spriteRow = anchorY - dot(ray * distance - anchor, cardUp) / rowSize;
				if (ray.z * distance / ZScale < t8.x) continue; // (nothing of the card below the floor it stands on)
			}
			if (!(distance >= 1.0 && spriteRow >= 0.0 && spriteRow < float(spriteHeight))) continue;
			int spriteX = int(floor(anchorX + (dot(ray, uLevelRight) * distance - anchorAcross) / columnSize));
			if (spriteX < 0 || spriteX >= spriteWidth) continue;
			int spriteAt = int(floor(spriteRow)) * spriteWidth + spriteX;
			uint spriteColor = texelFetch(uBytes, pixelsAt + spriteAt).r;
			float spriteDepth = max(1.0, distance - head.w);
			if (spriteColor == 0u || spriteDepth >= depth) continue;
			if ((flags & 8) != 0) lastPile = uint(number + 1);
			uint ownColours = (flags & 4) != 0 ? 1u : 0u;
			int shade = int(head.z);
			int pixelOpacity = (flags & 16) == 0 ? SeeThroughLevels
				: opacityAt >= 0 ? int(float(texelFetch(uBytes, opacityAt + spriteAt).r) * t8.z + 0.5) : int(float(SeeThroughLevels) * t8.z + 0.5);
			if (pixelOpacity < SeeThroughLevels)
			{
				// See-through (the smoke): mixed over what's there, within what a unit under it and the
				// smoke's cap leave; the depth stays.
				int smokeCap = int(t8.w);
				pixelOpacity = min(pixelOpacity, veil);
				if (smokeCap > 0) pixelOpacity = min(pixelOpacity, smokeCap - smokeVeil);
				if (pixelOpacity <= 0) continue;
				if (veil != 255) veil -= pixelOpacity;
				if (smokeCap > 0) smokeVeil += pixelOpacity;
				// A faint picture that keeps its colours under futuristic night vision (a body or a pile
				// pointed past, setting "See-through corpses"): over the world in the night vision's colours.
				bool ownOverNight = ownColours != 0u && uOwnOverNight != 0;
				if (ownOverNight && own == 0u && color != 0u) color = uint((uOwnOverNight - 1) * 16) | (color & 15u);
				color = seeThrough(pixelOpacity, shadeColor(spriteColor, shade), color);
				if (pixelOpacity > SeeThroughLevels / 2)
				{
					glow = 0u;
					own = ownColours;
				}
				else if (ownOverNight) own = 1u;
				continue;
			}
			if (t8.y < 1.0 && dither(pixel.x, pixel.y) >= t8.y) continue; // (right in front of the camera it fades out)
			color = shadeColor(spriteColor, shade);
			depth = spriteDepth;
			glow = 0u;
			veil = (flags & 2) != 0 ? UnitVeilMax : 255;
			own = ownColours;
			lastSolid = uint(number + 1);
		}
		else if (kind == EffectGroundShade)
		{
			// FirstPersonView::drawGroundShade: the ray down onto the ground plane; within the disc (or
			// the patch stretched away from a light) and on the floor right there, darkened, dithered.
			vec4 t2 = prim(base, 2), t3 = prim(base, 3);
			vec3 voxelRay = vec3(ray.xy, ray.z / ZScale);
			float voxelLength = length(voxelRay);
			if (voxelRay.z / voxelLength > -1e-4) continue;
			float along = t2.z / ray.z; // (how far along the ray, which reaches one unit ahead: the view depth there too)
			if (along <= 0.0) continue;
			vec2 offset = ray.xy * along - t2.xy;
			float closeness;
			if ((flags & 1) != 0)
			{
				float alongPatch = dot(offset, t3.xy), aside = abs(offset.x * t3.y - offset.y * t3.x);
				if (alongPatch < 0.0 || alongPatch > t3.z) continue;
				float share = alongPatch / t3.z, halfWidth = t2.w * (1.0 + (FlareShadowWidening - 1.0) * share);
				if (aside > halfWidth) continue;
				closeness = (1.0 - aside / halfWidth) * (1.0 - share);
			}
			else
			{
				float fromCentre = length(offset);
				if (fromCentre > t2.w) continue;
				closeness = 1.0 - fromCentre / t2.w;
			}
			if (abs(depth - along) > 3.0) continue;
			if (closeness * head.w > dither(pixel.x, pixel.y)) color = darkened(color, int(head.z));
		}
		else if (kind == EffectDecal)
		{
			// FirstPersonView::drawDecals: the pixels whose point in the scene lies within the mark's radius.
			vec4 t2 = prim(base, 2), t3 = prim(base, 3);
			float sceneRight = (float(pixel.x) + 0.5 - float(uViewSize.x) * 0.5) * depth / uFocal, sceneUp = (float(uViewSize.y) * 0.5 - (float(pixel.y) + 0.5)) * depth / uFocal;
			float fromMark = length(vec3(depth - t2.z, sceneRight - t3.x, sceneUp - t3.y));
			if (pixel == ivec2(int(t2.x), int(t2.y)))
			{
				if (fromMark <= t2.w + 2.0 * t3.z + 1.0) color = darkened(color, int(head.z));
				continue;
			}
			if (t2.w < t3.z * 0.5 || fromMark > t3.w) continue;
			if ((flags & 1) != 0 && fromMark > t3.w * 0.6 && dither(pixel.x, pixel.y) > 0.5) continue; // (a ragged edge)
			color = darkened(color, fromMark <= t3.w * 0.5 ? int(head.z) : int(head.w));
		}
		else if (kind == EffectLine)
		{
			// FirstPersonView::drawLine3D: its steps from one end to the other that cover this pixel
			// (a square of its thickness round each), any of them in front of the scene.
			vec4 t2 = prim(base, 2), t3 = prim(base, 3), t4 = prim(base, 4);
			int steps = int(t3.z), thickness = int(head.w);
			vec2 delta = t2.zw - t2.xy;
			bool alongX = abs(delta.x) >= abs(delta.y);
			float span = alongX ? delta.x : delta.y, here = alongX ? float(pixel.x) - t2.x : float(pixel.y) - t2.y;
			int middle = abs(span) < 1.0 ? 0 : int(here * float(steps) / span);
			int firstStep = abs(span) < 1.0 ? 0 : max(0, middle - thickness - 2), lastStep = abs(span) < 1.0 ? steps : min(steps, middle + thickness + 2);
			bool drawn = false;
			for (int step = firstStep; step <= lastStep && !drawn; ++step)
			{
				float progress = float(step) / float(steps);
				int x = int(t2.x + delta.x * progress), y = int(t2.y + delta.y * progress);
				float lineDepth = 1.0 / (t3.x + (t3.y - t3.x) * progress);
				int pointThickness = t4.x > 0.0 ? clamp(int(floor(t4.x * uFocal / lineDepth + 0.5)), 1, thickness) : thickness;
				int left = x - pointThickness / 2, top = y - pointThickness / 2;
				if (pixel.x < left || pixel.y < top || pixel.x >= left + pointThickness || pixel.y >= top + pointThickness) continue;
				drawn = (flags & 2) != 0 || lineDepth < depth + ((flags & 8) != 0 ? 0.0 : 1.0);
			}
			if (!drawn || (t3.w < 1.0 && dither(pixel.x, pixel.y) >= t3.w)) continue;
			color = uint(head.z);
			if ((flags & 4) == 0) glow = (flags & 1) != 0 ? 1u : 0u;
		}
		else if (kind == EffectDot)
		{
			// A square of pixels in front of the scene (an edge dot, a particle, the aimed path's dots).
			vec4 t2 = prim(base, 2);
			if ((flags & 4) != 0 && (pixel.x == int(box.x) || pixel.x == int(box.z) - 1) && (pixel.y == int(box.y) || pixel.y == int(box.w) - 1)) continue; // (its corners rounded off)
			if ((flags & 8) != 0 ? head.w > depth + t2.x : head.w >= depth + t2.x) continue;
			float threshold = dither(pixel.x, pixel.y);
			if (t2.w > threshold) continue; // (faded into the haze)
			color = t2.z < 0.0 || threshold < t2.z ? uint(head.z) : uint(t2.y);
			if ((flags & 1) != 0) glow = 1u;
			else if ((flags & 2) != 0) glow = 0u;
		}
		else if (kind == EffectDisc)
		{
			// A round dot in front of the scene, in rings (a shot's head, a flare), or only its rim (a bubble).
			vec4 t2 = prim(base, 2), t3 = prim(base, 3);
			vec2 at = vec2(pixel) + ((flags & 2) != 0 ? vec2(0.5) : vec2(0.0));
			float fromCentre = length(at - t2.xy);
			if (fromCentre > t2.z || head.w >= depth + t2.w) continue;
			if ((flags & 4) != 0)
			{
				if (fromCentre > t2.z - 1.0) color = uint(head.z);
				continue;
			}
			color = fromCentre < t2.z * t3.z ? uint(t3.x) : fromCentre < t2.z * t3.w ? uint(t3.y) : uint(head.z);
			if ((flags & 1) != 0) glow = 1u;
		}
		else if (kind == EffectQuad)
		{
			// FirstPersonView::fillQuadAtDepth: the pixels whose centres lie inside its four corners, at one depth.
			vec4 t2 = prim(base, 2), t3 = prim(base, 3);
			vec2 corners[4] = vec2[4](t2.xy, t2.zw, t3.xy, t3.zw);
			float rowCentre = float(pixel.y) + 0.5, leftX = 1e9, rightX = -1e9;
			for (int corner = 0; corner < 4; ++corner)
			{
				vec2 edgeStart = corners[corner], edgeEnd = corners[(corner + 1) & 3];
				if ((edgeStart.y <= rowCentre && edgeEnd.y > rowCentre) || (edgeEnd.y <= rowCentre && edgeStart.y > rowCentre))
				{
					float crossX = edgeStart.x + (rowCentre - edgeStart.y) / (edgeEnd.y - edgeStart.y) * (edgeEnd.x - edgeStart.x);
					leftX = min(leftX, crossX);
					rightX = max(rightX, crossX);
				}
			}
			if (float(pixel.x) < ceil(leftX - 0.5) || float(pixel.x) > floor(rightX - 0.5) || head.w >= depth) continue;
			color = uint(head.z);
			depth = head.w;
		}
	}
	outColor = color;
	outDepth = depth;
	outGlow = glow | (own << 5) | (smokeVeil > 0 ? 64u : 0u);
	outNotes = uvec4(uint(veil), own | uint(smokeVeil) << 1, lastSolid, lastPile);
}
)";


/**
 * The bloom on the card (FirstPersonView::drawLightHalos), after the units and effects, in three
 * steps: the shining pixels gathered into a grid a quarter as fine (how much shines in each
 * cell, and how far away it is) and their colours into one an eighth as fine; both blurred, across
 * then down, twice; and the picture brightened where the blurred glow lands, as the CPU's
 * brightenGlowBands does it.
 */
const char *const GlowGatherSource = R"(
uniform usampler2D uColor, uGlow; // the finished picture's palette colours, its glow
uniform sampler2D uDepth, uPalette; // its depth; the palette, a texel a colour
uniform ivec2 uViewSize;
uniform int uCell, uHue;          // the cell's size in pixels; 1: the colours' grid
uniform float uMaxDistance;
layout(location = 0) out vec4 outSum;

void main()
{
	ivec2 cell = ivec2(gl_FragCoord.xy);
	vec4 sum = vec4(0.0);
	for (int y = 0; y < uCell; ++y)
	{
		for (int x = 0; x < uCell; ++x)
		{
			ivec2 at = cell * uCell + ivec2(x, y);
			if (at.x >= uViewSize.x || at.y >= uViewSize.y) continue;
			uint shines = texelFetch(uGlow, at, 0).r & 3u;
			if (shines == 0u) continue;
			float weight = float(shines) * (1.0 / 16.0);
			if (uHue != 0) sum += vec4(weight * texelFetch(uPalette, ivec2(int(texelFetch(uColor, at, 0).r), 0), 0).rgb * 255.0, weight);
			else sum.xy += vec2(weight, weight * min(texelFetch(uDepth, at, 0).r, uMaxDistance)); // (something shining in the sky counts as far away)
		}
	}
	outSum = sum;
}
)";

const char *const GlowBlurSource = R"(
uniform sampler2D uSource;
uniform ivec2 uStep;   // across or down
uniform int uRadius;
uniform float uScale;  // what the sum is multiplied by
layout(location = 0) out vec4 outSum;

void main()
{
	ivec2 at = ivec2(gl_FragCoord.xy), last = textureSize(uSource, 0) - 1;
	vec4 sum = vec4(0.0);
	for (int offset = -uRadius; offset <= uRadius; ++offset) sum += texelFetch(uSource, clamp(at + uStep * offset, ivec2(0), last), 0);
	outSum = sum * uScale;
}
)";

const char *const GlowBrightenSource = R"(
uniform usampler2D uColor, uGlow, uNearest; // the finished picture; the palette colour nearest each 15-bit colour, 256 a row
uniform sampler2D uDepth, uGrid, uHueGrid, uPalette;
uniform float uGain;
layout(location = 0) out uint outColor;
layout(location = 1) out uint outGlow;

const float Bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
float dither(int x, int y) { return Bayer[(y & 3) * 4 + (x & 3)] / 16.0; }
float luminanceOf(vec3 rgb) { return rgb.r * 0.3 + rgb.g * 0.59 + rgb.b * 0.11; }

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	uint color = texelFetch(uColor, pixel, 0).r, glowBits = texelFetch(uGlow, pixel, 0).r;
	outColor = color;
	outGlow = glowBits;
	if ((glowBits & 3u) != 0u || color == 0u) return; // (the shining pixels themselves and the empty ones stay)
	// The glow here, blended between the four nearest cells, and how far away its light is.
	ivec2 gridSize = textureSize(uGrid, 0);
	vec2 grid = clamp((vec2(pixel) + 0.5) / 4.0 - 0.5, vec2(0.0), vec2(gridSize) - 1.001);
	ivec2 cell = ivec2(grid), next = min(gridSize - 1, cell + 1);
	vec2 blend = grid - vec2(cell);
	vec2 above = texelFetch(uGrid, cell, 0).xy * (1.0 - blend.x) + texelFetch(uGrid, ivec2(next.x, cell.y), 0).xy * blend.x;
	vec2 below = texelFetch(uGrid, ivec2(cell.x, next.y), 0).xy * (1.0 - blend.x) + texelFetch(uGrid, next, 0).xy * blend.x;
	vec2 here = above * (1.0 - blend.y) + below * blend.y;
	float glow = here.x;
	if (glow > 0.0 && texelFetch(uDepth, pixel, 0).r < here.y / glow - 16.0) return; // (well in front of the light: it doesn't shine through)
	float strength = glow * uGain;
	if (strength <= dither(pixel.x, pixel.y)) return;
	float second = dither(pixel.x + 1, pixel.y + 2);
	int steps = strength > 2.0 + second ? 3 : strength > 1.0 + second ? 2 : 1;
	// A pixel too dark to show its group's hue takes the light's own colour, the palette's nearest.
	vec4 hue = texelFetch(uHueGrid, pixel / 8, 0);
	float hueLuminance = hue.a > 0.0 ? luminanceOf(hue.rgb) / hue.a : 0.0;
	vec3 dark = texelFetch(uPalette, ivec2(int(color), 0), 0).rgb * 255.0;
	if (luminanceOf(dark) < DarkGlowLuma && hueLuminance >= 1.0)
	{
		float share = float(steps) * DarkGlowStepLuma / hueLuminance / hue.a;
		ivec3 five = ivec3(floor(min(vec3(255.0), dark + hue.rgb * share) * 31.0 / 255.0 + 0.5));
		int nearest = (five.r << 10) | (five.g << 5) | five.b;
		outColor = texelFetch(uNearest, ivec2(nearest & 255, nearest >> 8), 0).r;
	}
	else
	{
		int brightest = (color & 0xF0u) == 0u ? 1 : 0;
		outColor = (color & 0xF0u) | uint(max(brightest, int(color & 15u) - steps));
	}
}
)";


/**
 * The screen wash on the card (FirstPersonView::washBands), after the bloom: night vision's one
 * colour group (a unit's, a body's or an item's pixel keeping its own with the futuristic kind),
 * the colour wash, a camera feed's static, the hurt soldier's red edges and the purple ones of a
 * unit under our mind control; each as there, in that order.
 */
const char *const WashSource = R"(
uniform usampler2D uColor, uGlow, uTints; // the picture, its glow (bit 32: the pixel keeps its own colours); the six tint tables, a row each: red, blue, orange, white, green, purple
uniform ivec2 uViewSize;
uniform int uNightVision, uOwnColours;    // the night vision's colour group + 1 (0: off); 1: futuristic (own colours kept)
uniform float uTintAmount, uTintRedShare; // the colour wash: its share of the pixels, and of those the share washed red instead
uniform int uTintRow;                     // ... its table's row
uniform float uStaticAmount;
uniform uint uStaticSeed;
uniform ivec4 uGreys;                     // the static's four greys
uniform float uVignette, uControlHaze;
layout(location = 0) out uint outColor;

const float Bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
float dither(int x, int y) { return Bayer[(y & 3) * 4 + (x & 3)] / 16.0; }
uint tinted(int row, uint color) { return texelFetch(uTints, ivec2(int(color), row), 0).r; }

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	uint color = texelFetch(uColor, pixel, 0).r;
	if (uNightVision != 0 && color != 0u && !(uOwnColours != 0 && (texelFetch(uGlow, pixel, 0).r & 32u) != 0u)) color = uint((uNightVision - 1) * 16) | (color & 15u);
	if (uTintAmount > 0.0 && dither(pixel.x, pixel.y) < uTintAmount)
	{
		color = tinted(uTintRedShare > 0.0 && dither(pixel.y + 1, pixel.x + 2) < uTintRedShare ? 0 : uTintRow, color);
	}
	if (uStaticAmount > 0.0)
	{
		// The row's random numbers, a step a pixel from the row's own start: the pixel's, by squaring (the CPU steps along the row).
		uint seed = uStaticSeed ^ (uint(pixel.y) * 2654435761u), mul = 1u, add = 0u, stepMul = 1664525u, stepAdd = 1013904223u;
		for (uint steps = uint(pixel.x) + 1u; steps != 0u; steps >>= 1)
		{
			if ((steps & 1u) != 0u)
			{
				mul *= stepMul;
				add = add * stepMul + stepAdd;
			}
			stepAdd = (stepMul + 1u) * stepAdd;
			stepMul *= stepMul;
		}
		seed = seed * mul + add;
		if (float((seed >> 8) & 255u) < uStaticAmount * 256.0) color = uint(uGreys[int(seed >> 30)]);
	}
	if (uVignette > 0.0 || uControlHaze > 0.0)
	{
		vec2 centre = vec2(uViewSize) * 0.5;
		float share = length(vec2(pixel) - centre) / length(centre);
		if (uVignette > 0.0 && smoothstep(0.55, 1.0, share) * uVignette > dither(pixel.x + 3, pixel.y + 1)) color = tinted(0, color);
		else if (uControlHaze > 0.0 && smoothstep(ControlHazeFrom, 1.0, share) * uControlHaze > dither(pixel.x + 1, pixel.y + 2)) color = tinted(5, color);
	}
	outColor = color;
}
)";


/**
 * The frame as it is shown, put together on the card (FirstPersonGpu::present): the finished
 * picture with what the CPU drew last laid over it (the overlay: 0 where it drew nothing), each
 * pixel through the palette; or, in full colour, the rays' own full colour where the pixel still
 * has its ray's palette colour, and over a ray's pixel under see-through smoke the light the card
 * found in the air there (as FirstPersonMode::blit puts the full-colour picture together).
 */
const char *const PresentSource = R"(
uniform usampler2D uColor, uGlow, uOverlay, uRayColor; // the finished picture, its glow (bit 64: smoke over the pixel), the overlay, the rays' own palette colours
uniform sampler2D uRayTrue, uPalette;                  // the rays' full colours; the palette, a texel a colour
uniform int uFull, uShafts;                            // 1: in full colour; 1: with light in the air
uniform usampler2D uFlameTint;                         // each colour in the flames' orange light
uniform float uFlameGlow;                              // a burning soldier's flames light the lower part of the view this strongly (0: none; see FirstPersonView::drawBurning)
uniform ivec2 uViewSize;
layout(location = 0) out vec4 outPixel;

const float Bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
float dither(int x, int y) { return Bayer[(y & 3) * 4 + (x & 3)] / 16.0; }
vec3 paletteColor(uint color) { return texelFetch(uPalette, ivec2(int(color), 0), 0).rgb; }

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	uint made = texelFetch(uOverlay, pixel, 0).r;
	if (made == 0u)
	{
		made = texelFetch(uColor, pixel, 0).r;
		// The flames' light from below, on the picture under them: the bottom 30% of the view, more the lower, dithered in.
		int glowRows = int(float(uViewSize.y) * 0.3), firstRow = uViewSize.y - glowRows;
		if (uFlameGlow > 0.0 && pixel.y >= firstRow && dither(pixel.x, pixel.y) < pow(float(pixel.y - firstRow) / float(glowRows), 1.6) * uFlameGlow)
		{
			made = texelFetch(uFlameTint, ivec2(int(made), 0), 0).r;
		}
	}
	vec3 shown = paletteColor(made);
	if (uFull != 0)
	{
		uint ray = texelFetch(uRayColor, pixel, 0).r;
		vec3 full = texelFetch(uRayTrue, pixel, 0).rgb;
		if (made == ray) shown = full;
		else if (uShafts != 0 && (texelFetch(uGlow, pixel, 0).r & 64u) != 0u) shown = min(vec3(1.0), shown + max(vec3(0.0), full - paletteColor(ray)));
	}
	outPixel = vec4(shown, 1.0);
}
)";

}

}
