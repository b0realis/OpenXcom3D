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
 * FirstPersonViewRayCasting.cpp: colours, light and the ray caster.
 *
 * The colours the view uses for the current palette and time of day, the game's light blended
 * smoothly, point lights with ray-traced shadows, shading the voxel a ray hit, the ground and
 * the taped-off border beyond the map, the sky, and renderRows, which casts the rays for a
 * share of the screen's rows on one thread.
 */
#include "FirstPersonView.h"
#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>
#include <SDL.h>
#include "../Engine/Options.h"
#include "../Savegame/BattleUnit.h" // not used here, but its PARTS_STRING constants are part of this file's object code
#include "../Savegame/SavedBattleGame.h"
#include "FirstPersonViewShared.h"
#include "FirstPersonViewRayWalk.h"

namespace OpenXcom
{

/**
 * Picks the colours the view needs this frame from the current palette, so that the UFO and
 * the TFTD palettes both work: the sky gradient, clouds, haze, the map border's tape and posts,
 * the particles' colours (fire, smoke, blood, weather, flares) and the enemy markers. When the
 * palette has changed it also rebuilds the colour tables: each colour as seen through window
 * glass, and each colour washed with red, blue, orange, white or green (the screen washes) or
 * lit by a flare's red light. Finally it sets the sun and the moon for the time of day, and how
 * much darker each face direction is shaded.
 */
void FirstPersonView::updateColors()
{
	const SDL_Color *palette = getPalette();
	// The palette colour closest to an RGB colour (0..255 per channel), weighting the channels
	// by how bright they look to the eye. Colour 0 is transparent, so it is never picked.
	auto nearest = [palette](float red, float green, float blue) {
		int bestIndex = 1;
		float bestDistance = 1e9f;
		for (int index = 1; index < 256; ++index)
		{
			// dr, dg, db: how far this palette colour is from the wanted one, per channel.
			const float dr = palette[index].r - red, dg = palette[index].g - green, db = palette[index].b - blue;
			const float distance = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;
			if (distance < bestDistance)
			{
				bestDistance = distance;
				bestIndex = index;
			}
		}
		return (Uint8)bestIndex;
	};
	// Mixes two RGB colours: amount 0 gives `from`, 1 gives `to`.
	auto mix = [](const float *from, const float *to, float amount, float *out) {
		for (int channel = 0; channel < 3; ++channel) out[channel] = from[channel] + (to[channel] - from[channel]) * amount;
	};

	// The time of day comes from the battle's global shade (0 bright day .. 15 dark night):
	// full day up to shade 3, full night from shade 12. Night vision brightens open ground.
	const int globalShade = std::clamp(_save->getGlobalShade(), 0, 15);
	_colors.night = std::clamp((globalShade - 3) / 9.0f, 0.0f, 1.0f);
	_colors.underwater = _save->getDepth() > 0;
	_colors.ambient = _nightVision ? std::min(globalShade, 4) : globalShade;
	_colors.cloudTime = SDL_GetTicks() / 1000.0f; // seconds, to drift the clouds and ripple the sea floor's light

	// The sky's colours at the horizon and straight up (the zenith) by day, by night and under
	// the sea, and the clouds' lit and shadowed sides by day and by night (RGB).
	static const float dayHorizon[3] = { 190, 210, 230 }, dayZenith[3] = { 70, 120, 200 };
	static const float nightHorizon[3] = { 24, 28, 50 }, nightZenith[3] = { 3, 4, 14 };
	static const float waterHorizon[3] = { 8, 34, 46 }, waterZenith[3] = { 34, 104, 118 };
	static const float cloudLit[3] = { 240, 240, 245 }, cloudDark[3] = { 140, 146, 162 };
	static const float cloudNightLit[3] = { 46, 48, 66 }, cloudNightDark[3] = { 20, 22, 34 };
	float horizon[3], zenith[3], litCloud[3], darkCloud[3];
	// Under the sea the water's colours stay the same all day; above it the sky darkens from
	// the day's colours to the night's.
	if (_colors.underwater)
	{
		mix(waterHorizon, waterHorizon, 0, horizon);
		mix(waterZenith, waterZenith, 0, zenith);
	}
	else
	{
		mix(dayHorizon, nightHorizon, _colors.night, horizon);
		mix(dayZenith, nightZenith, _colors.night, zenith);
	}
	// The sky gradient: 32 steps from the horizon up to the zenith.
	for (int band = 0; band < 32; ++band)
	{
		float color[3];
		mix(horizon, zenith, band / 31.0f, color);
		_colors.sky[band] = nearest(color[0], color[1], color[2]);
	}
	// The clouds: 8 steps from their lit side to their shadowed side, darker at night.
	mix(cloudLit, cloudNightLit, _colors.night, litCloud);
	mix(cloudDark, cloudNightDark, _colors.night, darkCloud);
	for (int level = 0; level < 8; ++level)
	{
		float color[3];
		mix(litCloud, darkCloud, level / 7.0f, color);
		_colors.cloud[level] = nearest(color[0], color[1], color[2]);
	}
	// The single colours: the haze far away is the sky at the horizon, the rest are picked
	// from fixed RGB values.
	_colors.haze = _colors.sky[0];
	_colors.star = nearest(230, 230, 240);
	_colors.moon = nearest(228, 228, 210);
	_colors.sun = nearest(255, 248, 200);
	_colors.fire[0] = nearest(255, 250, 200);
	_colors.fire[1] = nearest(255, 210, 60);
	_colors.fire[2] = nearest(240, 120, 20);
	_colors.fire[3] = nearest(160, 40, 10);
	_colors.smoke = nearest(130, 130, 130);
	_colors.spark = nearest(255, 240, 160);
	_colors.blood[0] = nearest(90, 0, 0);
	_colors.blood[1] = nearest(160, 12, 8);
	_colors.dust = nearest(200, 190, 170);
	_colors.rain = nearest(150, 170, 200);
	_colors.snow = nearest(240, 240, 250);
	_colors.sand = nearest(210, 170, 110);
	_colors.firefly = nearest(200, 255, 100);
	_colors.bubble = nearest(170, 230, 240);
	_colors.tapeYellow = nearest(240, 200, 30);
	_colors.marker = nearest(235, 40, 30);
	_colors.marine = nearest(150, 195, 190);
	_colors.flareFire[0] = nearest(240, 30, 20);
	_colors.flareFire[1] = nearest(255, 100, 30);
	_colors.flareFire[2] = nearest(255, 200, 80);
	_colors.tapeBlack = nearest(20, 20, 20);
	_colors.post = nearest(110, 110, 110);

	// The colour tables. Building them means 256 nearest-colour searches each, so they are
	// only rebuilt when the palette changes: a fingerprint of the palette (an FNV-1a hash of
	// its RGB values) tells whether it did.
	Uint32 hash = 2166136261u;
	for (int index = 0; index < 256; ++index)
	{
		hash = (hash ^ (palette[index].r | (palette[index].g << 8) | (palette[index].b << 16))) * 16777619u;
	}
	if (hash != _colors.paletteHash)
	{
		_colors.paletteHash = hash;
		_colors.glassTint[0] = 0;
		// Every colour (but the transparent 0) as seen through window glass, a little paler and
		// bluer, and washed with each screen wash's colour by its brightness (luminance).
		for (int index = 1; index < 256; ++index)
		{
			_colors.glassTint[index] = nearest(palette[index].r * 0.7f + 150 * 0.3f, palette[index].g * 0.7f + 180 * 0.3f,
				palette[index].b * 0.7f + 205 * 0.3f);
			const float luminance = palette[index].r * 0.3f + palette[index].g * 0.59f + palette[index].b * 0.11f;
			_colors.redTint[index] = nearest(60 + luminance * 0.85f, luminance * 0.18f, luminance * 0.15f);
			_colors.blueTint[index] = nearest(luminance * 0.1f, luminance * 0.16f, 25 + luminance * 0.45f);
			_colors.orangeTint[index] = nearest(std::min(255.0f, 80 + luminance * 0.9f), 30 + luminance * 0.45f, luminance * 0.08f);
			_colors.whiteTint[index] = nearest(std::min(255.0f, 180 + luminance * 0.3f), std::min(255.0f, 180 + luminance * 0.3f),
				std::min(255.0f, 180 + luminance * 0.3f));
			_colors.greenTint[index] = nearest(15 + luminance * 0.3f, 35 + luminance * 0.7f, luminance * 0.2f);
			_colors.flareTint[index] = nearest(std::min(255.0f, 70 + luminance * 1.15f), luminance * 0.32f, luminance * 0.24f);
		}
		// Colour 0 (black on the screen): the blue, orange, white and green washes colour even
		// black, with a dark shade of their own; the red wash and the flare's light leave it black.
		_colors.redTint[0] = 0;
		_colors.blueTint[0] = nearest(0, 2, 18);
		_colors.orangeTint[0] = nearest(60, 24, 0);
		_colors.whiteTint[0] = nearest(185, 185, 185);
		_colors.greenTint[0] = nearest(8, 22, 4);
		_colors.flareTint[0] = 0;
		// Four greys, dark to light, for the TV static.
		for (int level = 0; level < 4; ++level) _colors.grey[level] = nearest(30 + 60 * level, 30 + 60 * level, 30 + 60 * level);
	}

	// The sun stands high in the north-west by day; at night there is only the moon, and the
	// faces get a flatter light. (Unit vectors in the world: x east, y south, z up.)
	const float sunDirection[3] = { -0.45f, -0.5f, 0.74f }, moonDirection[3] = { 0.35f, -0.75f, 0.56f };
	for (int axis = 0; axis < 3; ++axis)
	{
		_colors.sunDir[axis] = sunDirection[axis];
		_colors.moonDir[axis] = moonDirection[axis];
	}
	// Each face direction (+x -x +y -y +z -z) is shaded by how much it turns away from the sun
	// (Lambert's cosine law): strongly by day, only a little at night. Undersides (-z) get a
	// whole shade more on top.
	const float daylight = 1.0f - _colors.night;
	static const float faceNormals[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	for (int face = 0; face < 6; ++face)
	{
		const float lambert = std::max(0.0f, faceNormals[face][0] * sunDirection[0] + faceNormals[face][1] * sunDirection[1]
			+ faceNormals[face][2] * sunDirection[2]);
		_colors.faceShade[face] = (1.0f - lambert) * (0.5f + 2.5f * daylight) + (face == 5 ? 1.0f : 0.0f);
	}
}

/**
 * The game's light at a point, as a shade (0 brightest .. 15 darkest). The game lights the map
 * tile by tile; with smooth lighting on, the shade is blended between the centres of the eight
 * tiles around the point (trilinear interpolation), so light fades gently instead of stepping
 * at tile edges. Tiles not discovered yet don't leak their light (they count as the point's own
 * tile), and open ground outside the map gets the ambient light of the sky.
 * @param x World position east, in voxels.
 * @param y World position south, in voxels.
 * @param z World height, in voxels.
 * @return The shade, fractional when blended.
 */
float FirstPersonView::lightAt(float x, float y, float z) const
{
	// The tile the point is in (clamped onto the map): its light is the answer without smooth
	// lighting, and stands in for undiscovered neighbours with it.
	const int mapSizeX = _save->getMapSizeX(), mapSizeY = _save->getMapSizeY(), mapSizeZ = _save->getMapSizeZ();
	const int tileX = std::clamp((int)std::floor(x / 16), 0, mapSizeX - 1);
	const int tileY = std::clamp((int)std::floor(y / 16), 0, mapSizeY - 1);
	const int tileZ = std::clamp((int)std::floor(z / 24), 0, mapSizeZ - 1);
	const float ownTileShade = _tileViews[(tileZ * mapSizeY + tileY) * mapSizeX + tileX].light;
	if (!_smoothLighting)
	{
		return ownTileShade;
	}
	// The point in tile units measured from tile centres: the eight tiles around it start at
	// (lowX, lowY, lowZ), and the fractions say how far the point is towards the next ones.
	const float gridX = x / 16 - 0.5f, gridY = y / 16 - 0.5f, gridZ = z / 24 - 0.5f;
	const int lowX = (int)std::floor(gridX), lowY = (int)std::floor(gridY), lowZ = (int)std::floor(gridZ);
	const float fractionX = gridX - lowX, fractionY = gridY - lowY, fractionZ = gridZ - lowZ;
	float blendedShade = 0.0f;
	// Add up the eight tiles' shades, each weighted by how close the point is to it. The bits
	// of `corner` pick the tile: bit 0 the next one east, bit 1 south, bit 2 up.
	for (int corner = 0; corner < 8; ++corner)
	{
		const int cornerX = lowX + (corner & 1), cornerY = lowY + ((corner >> 1) & 1),
			cornerZ = std::clamp(lowZ + ((corner >> 2) & 1), 0, mapSizeZ - 1); // no level below the ground or above the top
		float cornerShade;
		if (cornerX < 0 || cornerY < 0 || cornerX >= mapSizeX || cornerY >= mapSizeY)
		{
			// Off the map: open ground under the sky.
			cornerShade = (float)_colors.ambient;
		}
		else
		{
			const TileView &tileView = _tileViews[(cornerZ * mapSizeY + cornerY) * mapSizeX + cornerX];
			cornerShade = tileView.discovered ? tileView.light : ownTileShade;
		}
		const float weight = ((corner & 1) ? fractionX : 1 - fractionX) * (((corner >> 1) & 1) ? fractionY : 1 - fractionY)
			* (((corner >> 2) & 1) ? fractionZ : 1 - fractionZ);
		blendedShade += cornerShade * weight;
	}
	return blendedShade;
}

/**
 * Is there anything solid on the straight line between two points? This is the shadow ray:
 * a point is in a light's shadow when something stands between it and the light. It walks the
 * same voxel world as the view's rays, swinging doors included.
 * @param from The lit point, in voxels.
 * @param to The light, in voxels.
 * @return True if something blocks the line (points less than a voxel apart never are).
 */
bool FirstPersonView::shadowed(const float from[3], const float to[3]) const
{
	// The unit direction from one point to the other, and the distance between them.
	float direction[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
	const float distance = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
	if (distance < 1.0f)
	{
		return false;
	}
	for (float &component : direction) component /= distance;
	// Walk the ray through the map's tile snapshot, no further than the light.
	RayContext rayContext;
	rayContext.tiles = _tileViews.data();
	rayContext.swings = _doorSwings.data();
	rayContext.size[0] = _save->getMapSizeX();
	rayContext.size[1] = _save->getMapSizeY();
	rayContext.size[2] = _save->getMapSizeZ();
	rayContext.maxDistance = distance;
	Hit hit;
	float tEnd;   // where the ray left the map (not needed here)
	int endAxis;  // through which side (not needed here)
	return castRay(rayContext, from, direction, hit, tEnd, endAxis) == RAY_HIT && hit.t < distance;
}

/**
 * Extra light at a point from the lamps nearby and from the soldier's shoulder flashlight, in
 * shades to take off the game's shade (negative: darker, because a lamp's shadow is darker than
 * the game's flat light around it). Lamps only matter at night or under the sea; only the
 * strongest lamp in reach counts, so at most one shadow ray is cast for the lamps.
 * @param point The lit point, in voxels.
 * @param normal The surface's unit normal there, or null for a unit (lit from any side).
 * @param shadows Trace shadow rays (terrain); units are lit without them.
 * @return Shades of extra light (more is brighter).
 */
float FirstPersonView::pointLight(const float point[3], const float normal[3], bool shadows) const
{
	float light = 0.0f;
	// Lamps: a warm pool of light beneath them, and what stands in the way throws a shadow.
	if (!_nearbyLights.empty() && (_colors.night > 0.3f || _colors.underwater))
	{
		// Find the lamp that lights the point most: its strength, falling off with the square
		// of the distance's share of the lamps' reach.
		const float lampReach = 120.0f; // voxels
		const LightSpot *brightestLamp = nullptr;
		float brightestPower = 0.0f, brightestDistance = 0.0f;
		for (const auto &lamp : _nearbyLights)
		{
			// dx, dy, dz: from the point to the lamp.
			const float dx = lamp.pos[0] - point[0], dy = lamp.pos[1] - point[1], dz = lamp.pos[2] - point[2];
			const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (distance > lampReach) continue;
			const float power = lamp.strength * (1.0f - distance / lampReach) * (1.0f - distance / lampReach);
			if (power > brightestPower) { brightestPower = power; brightestLamp = &lamp; brightestDistance = distance; }
		}
		if (brightestLamp && brightestPower > 0.03f)
		{
			// A surface facing the lamp, with nothing in the way, is lit (more so facing it
			// squarely); one turned away or behind something is in the lamp's shadow. The shadow
			// ray stops 4 voxels short of the lamp, so the lamp's own voxels don't shadow it.
			const float towardsLamp[3] = { (brightestLamp->pos[0] - point[0]) / brightestDistance,
				(brightestLamp->pos[1] - point[1]) / brightestDistance,
				(brightestLamp->pos[2] - point[2]) / brightestDistance };
			const float facing = normal ? normal[0] * towardsLamp[0] + normal[1] * towardsLamp[1] + normal[2] * towardsLamp[2] : 1.0f;
			const float nearLamp[3] = { brightestLamp->pos[0] - towardsLamp[0] * 4.0f,
				brightestLamp->pos[1] - towardsLamp[1] * 4.0f,
				brightestLamp->pos[2] - towardsLamp[2] * 4.0f };
			const float night = _colors.underwater ? 1.0f : _colors.night; // lamps matter as much as it is dark
			if (facing <= 0.0f || (shadows && shadowed(point, nearLamp)))
			{
				light -= brightestPower * 3.0f * night;
			}
			else
			{
				light += brightestPower * 6.0f * (0.4f + 0.6f * facing) * night;
			}
		}
	}
	// The flashlight: a cone along the view direction, from the soldier's shoulder, reaching
	// 14 tiles and fading out with the square of the distance.
	if (_flashlight)
	{
		const float range = 16.0f * 14.0f; // voxels
		const float fromLight[3] = { point[0] - _flashlightPos[0], point[1] - _flashlightPos[1], point[2] - _flashlightPos[2] };
		const float distance = std::sqrt(fromLight[0] * fromLight[0] + fromLight[1] * fromLight[1] + fromLight[2] * fromLight[2]);
		if (distance > 0.5f && distance < range)
		{
			// How close the point is to the cone's axis: inside the cone when the angle's cosine
			// is over 0.8 (about 37 degrees off the axis), full strength from 0.96 (16 degrees).
			const float cosine = (fromLight[0] * (float)_forwardInVoxels[0] + fromLight[1] * (float)_forwardInVoxels[1]
				+ fromLight[2] * (float)_forwardInVoxels[2]) / distance;
			if (cosine > 0.8f)
			{
				const float cone = smoothstep(0.8f, 0.96f, cosine);
				const float falloff = 1.0f - distance / range;
				const float facing = normal ? -(normal[0] * fromLight[0] + normal[1] * fromLight[1] + normal[2] * fromLight[2]) / distance
					: 1.0f;
				const float power = cone * falloff * falloff * 10.0f * (0.35f + 0.65f * std::max(0.0f, facing));
				// Light only what faces the flashlight, isn't too faint, and isn't behind anything.
				if (power > 0.25f && facing > 0.0f && !(shadows && shadowed(point, _flashlightPos)))
				{
					light += power;
				}
			}
		}
	}
	return light;
}

/**
 * The colour of the voxel a ray hit inside the map: its own colour, lit and shaded.
 *
 * The light is the game's light sampled just off the surface, plus the face's shading for its
 * direction and one shade more per 20 tiles of distance; then the sun's shadows, lamps, the
 * flashlight, flares and muzzle flashes, and under the sea the sunlight rippling over the floor.
 * Lamps, lit screens and UFO lights shine whatever the light around them. The colour is the
 * voxel's own, except on faces the isometric art never shows (undersides, faces looking west or
 * north), which take the colour of a face the art does show; and an animated part part way
 * into its next frame dissolves into it. The light is dithered to whole palette shades, then
 * the haze closes in towards the edge of the view.
 * @param hit What the ray hit.
 * @param rayOrigin The eye, in voxels.
 * @param rayDirection The ray's unit direction in the voxel grid.
 * @param pixelX The ray's pixel on the view (or block of pixels, with a pixel size above 1),
 *               for the dither pattern.
 * @param pixelY The same, down.
 * @return The palette colour to draw.
 */
Uint8 FirstPersonView::shadeHit(const Hit &hit, const float rayOrigin[3], const float rayDirection[3], int pixelX, int pixelY) const
{
	const TileView &tileView = _tileViews[hit.tile];
	// Unexplored parts are black silhouettes. A wall belongs to the tile east or south of it,
	// but it's seen from both sides, so each part has its own "seen" bit; a swinging door
	// (part -1) goes by its tile.
	if (hit.part >= 0 ? !((tileView.seen >> hit.part) & 1) : !tileView.discovered)
	{
		// The underside of an unexplored level seen from an explored room below is just its
		// ceiling: draw it, lit by the room. Anything else unexplored is a black silhouette.
		const int tilesPerLevel = _save->getMapSizeX() * _save->getMapSizeY();
		const bool exploredCeiling = hit.axis == 2 && rayDirection[2] > 0.0f && hit.tile >= tilesPerLevel
			&& _tileViews[hit.tile - tilesPerLevel].discovered;
		if (!exploredCeiling)
		{
			return 0x0F;
		}
	}
	// The face's outward normal, against the ray on the axis it entered through (a top face,
	// +z, when that isn't known), and its index into the face shading (+x -x +y -y +z -z).
	int faceIndex = 4; // +z
	float normal[3] = { 0, 0, 1 };
	if (hit.axis >= 0)
	{
		normal[2] = 0;
		normal[hit.axis] = rayDirection[hit.axis] > 0 ? -1.0f : 1.0f;
		faceIndex = hit.axis * 2 + (normal[hit.axis] < 0 ? 1 : 0);
	}
	// The point the light is measured at: the hit, 1.5 voxels out from the face, so it samples
	// the air in front of the surface rather than the solid tile behind it.
	float surfacePoint[3];
	for (int axis = 0; axis < 3; ++axis)
	{
		surfacePoint[axis] = rayOrigin[axis] + rayDirection[axis] * hit.t + normal[axis] * 1.5f;
	}
	// The shade (0 brightest .. 15 darkest): the game's light, the face's direction, and one
	// shade darker per 320 voxels (20 tiles) of distance.
	float shade = lightAt(surfacePoint[0], surfacePoint[1], surfacePoint[2]) + _colors.faceShade[faceIndex] + hit.t / 320.0f;
	float flare = 0.0f; // red light from tactical flares
	if (hit.emissive)
	{
		shade = std::min(shade, 0.5f); // lamps, lit screens and UFO lights shine whatever the light around them
	}
	else if (Options::fpsShadows)
	{
		// The sun: a face turned towards it with something in the way is in shadow (only by
		// day, and not too far away, as each test is a ray walked 260 voxels towards the sun).
		const float daylight = _colors.underwater ? 0.0f : 1.0f - _colors.night;
		const float sunFacing = normal[0] * _colors.sunDir[0] + normal[1] * _colors.sunDir[1] + normal[2] * _colors.sunDir[2];
		if (daylight > 0.3f && sunFacing > 0.05f && hit.t < 640.0f)
		{
			const float towardsSun[3] = { surfacePoint[0] + _colors.sunDir[0] * 260.0f,
				surfacePoint[1] + _colors.sunDir[1] * 260.0f,
				surfacePoint[2] + _colors.sunDir[2] * 260.0f };
			if (shadowed(surfacePoint, towardsSun)) shade += 3.0f * daylight;
		}
		// Lamps and the flashlight, throwing shadows.
		shade -= pointLight(surfacePoint, normal, true);
	}
	else
	{
		// Lamps and the flashlight, without shadows.
		shade -= pointLight(surfacePoint, normal, false);
	}
	// Tactical flares light the surroundings brightly (their red tint is added below).
	if (!hit.emissive && !_flares.empty())
	{
		flare = flareLight(surfacePoint, normal, Options::fpsShadows);
		shade -= flare * 10.0f;
	}
	if (_muzzleFlashLight > 0.0f)
	{
		// A muzzle flash: brief, bright and close, within 96 voxels (6 tiles), stronger at
		// night. No shadows: it's over in a blink.
		const float dx = surfacePoint[0] - _muzzleFlashPos[0], dy = surfacePoint[1] - _muzzleFlashPos[1],
			dz = surfacePoint[2] - _muzzleFlashPos[2];
		const float distanceSquared = dx * dx + dy * dy + dz * dz;
		if (distanceSquared < 96.0f * 96.0f)
		{
			const float closeness = 1.0f - std::sqrt(distanceSquared) / 96.0f;
			shade -= closeness * closeness * _muzzleFlashLight * (4.0f + 6.0f * _colors.night);
		}
	}
	if (!hit.emissive && _colors.underwater && faceIndex == 4 && hit.t < 480.0f)
	{
		// Sunlight rippling over the sea floor (upward faces nearby): three sine waves crossing
		// each other and drifting with time; where they add up high, the floor is brighter.
		const float seconds = _colors.cloudTime;
		const float ripple = std::sin(surfacePoint[0] * 0.31f + seconds * 1.7f) + std::sin(surfacePoint[1] * 0.27f - seconds * 1.3f)
			+ std::sin((surfacePoint[0] - surfacePoint[1]) * 0.19f + seconds * 0.9f);
		if (ripple > 1.3f) shade -= (ripple - 1.3f) * 1.8f;
	}
	// The colour before shading: the voxel's own, unless the face is one the iso art doesn't show.
	Uint8 baseColor = hit.color;
	if (hit.part >= 0 && tileView.part[hit.part])
	{
		// A face the iso art never shows: what its row shows from the side it does.
		const VoxelModel &model = *tileView.part[hit.part];
		const int voxelX = hit.voxel[0], voxelY = hit.voxel[1], voxelZ = hit.voxel[2];
		// An underside (seen from below) shows the colour seen from above: a thin plate's
		// most common top colour, else the plan view's colour of this column.
		Uint8 shownColor = hit.axis == 2 && rayDirection[2] > 0.0f ? (model.ceiling ? model.ceiling : model.plan[voxelY][voxelX]) : 0;
		const int tilesPerLevel = _save->getMapSizeX() * _save->getMapSizeY();
		if (shownColor && model.color[23][voxelY][voxelX] && hit.tile + tilesPerLevel < (int)_tileViews.size())
		{
			// Its top is covered by what's on the level above (a craft's cabin under its roof
			// plates: the art shows the cabin there), so the top of that is what this is the
			// underside of.
			for (const VoxelModel *partAbove : _tileViews[hit.tile + tilesPerLevel].part)
			{
				if (partAbove && partAbove->color[0][voxelY][voxelX] && partAbove->plan[voxelY][voxelX])
				{
					shownColor = partAbove->ceiling ? partAbove->ceiling : partAbove->plan[voxelY][voxelX];
					break;
				}
			}
		}
		if (hit.axis >= 0 && hit.axis < 2 && rayDirection[hit.axis] > 0.0f)
		{
			// A west- or north-facing side: on a craft the mirrored side, else its own row seen
			// from the other side (the colour seen from the east, or from the south).
			shownColor = mirroredColor(hit, hit.axis);
			if (!shownColor) shownColor = hit.axis == 0 ? model.east[voxelZ][voxelY] : model.south[voxelZ][voxelX];
		}
		else if (hit.axis >= 0 && hit.axis < 2)
		{
			// A south or east face on a craft's hidden west or north skin (where the hull steps
			// in or out): the art sees it through the hull, so it too takes the mirrored side.
			shownColor = mirroredColor(hit, 1 - hit.axis, true);
		}
		if (shownColor) baseColor = shownColor;
	}
	// An animated part part way into its next frame: that frame's colour, dithered in (more of
	// the pixels show it the further the game's animation tick has gone).
	if (hit.part >= 0 && tileView.next[hit.part] && dither(pixelX + 2, pixelY + 1) < _animationBlend)
	{
		const Uint8 nextFrameColor = tileView.next[hit.part]->color[hit.voxel[2]][hit.voxel[1]][hit.voxel[0]];
		if (nextFrameColor) baseColor = nextFrameColor;
	}
	// Shade the colour by whole palette shades, the fraction dithered between two.
	Uint8 color = shadeColor(baseColor, (int)std::max(0.0f, shade + dither(pixelX, pixelY)));
	if (flare * 1.5f > 0.04f + dither(pixelX + 1, pixelY + 3))
	{
		color = _colors.flareTint[color]; // the flare's light is red
	}
	// Far away the haze takes over, dithered in; under the sea the murk closes in much sooner.
	const float haze = _colors.underwater ? smoothstep(_maxDistance * 0.12f, _maxDistance * 0.65f, hit.t)
		: smoothstep(_maxDistance * 0.45f, _maxDistance, hit.t);
	if (haze > dither(pixelX + 2, pixelY + 1))
	{
		color = _colors.haze;
	}
	return color;
}

/**
 * What a ray sees after leaving the map through its side (or its top): the X-COM tape on its
 * posts along the map's border, then the map's ground running on to the horizon, then the sky.
 * The tape and the posts stand on the border itself, so the ray meets them right where it
 * leaves the map; the ground beyond is a flat plane at floor height, tiled with the ground
 * model chosen for the map.
 * @param rayOrigin The eye, in voxels.
 * @param rayDirection The ray's unit direction in the voxel grid.
 * @param tExit How far along the ray it left the map.
 * @param exitAxis The axis of the map's side it left through (0 east/west, 1 north/south, 2 top).
 * @param pixelX The ray's pixel on the view (or block of pixels), for the dither pattern.
 * @param pixelY The same, down.
 * @param depth Out: how far along the ray the colour is (Infinity for the sky).
 * @return The palette colour to draw.
 */
Uint8 FirstPersonView::colorOutsideMap(const float rayOrigin[3], const float rayDirection[3], float tExit, int exitAxis, int pixelX, int pixelY,
	float &depth) const
{
	// Where the ray crosses the border, and how far along the border that is (the border runs
	// along y on the east and west sides, along x on the others).
	const float exitPoint[3] = { rayOrigin[0] + rayDirection[0] * tExit, rayOrigin[1] + rayDirection[1] * tExit,
		rayOrigin[2] + rayDirection[2] * tExit };
	const float alongBorder = exitAxis == 0 ? exitPoint[1] : exitPoint[0];
	const float tapeBottom = GroundTop + 8.0f; // the tape hangs 8 voxels above the ground, 7 voxels high
	const float borderShade = _colors.ambient * 0.5f;
	// A post every 64 voxels (4 tiles) along the border, 1.5 voxels thick and 17 voxels high,
	// shaded a little darker on the sides facing north or south than on those facing east or west.
	const float pastLastPost = alongBorder - std::floor(alongBorder / 64.0f) * 64.0f;
	if (pastLastPost < 1.5f && exitPoint[2] >= GroundTop && exitPoint[2] < GroundTop + 17.0f)
	{
		depth = tExit;
		return shadeColor(_colors.post, (int)(borderShade + (exitAxis == 0 ? 1.0f : 2.0f) + dither(pixelX, pixelY)));
	}
	// The tape: a black edge at the bottom and top, and between them "X-COM  " in black on
	// yellow, repeated along the border.
	if (exitPoint[2] >= tapeBottom && exitPoint[2] < tapeBottom + 7.0f)
	{
		depth = tExit;
		const int tapeRow = (int)(exitPoint[2] - tapeBottom); // 0..6, bottom to top
		Uint8 color = _colors.tapeBlack;
		if (tapeRow > 0 && tapeRow < 6)
		{
			// Each letter is 4 voxels wide (3 of glyph and a gap): find the letter under this
			// voxel column of the repeating text, and whether its glyph is inked there (glyph
			// rows run top to bottom, so tape rows 5..1 are glyph rows 0..4).
			const int textColumn = ((int)std::floor(alongBorder) % (TapeTextLength * 4) + TapeTextLength * 4) % (TapeTextLength * 4);
			const int glyph = TapeText[textColumn / 4], glyphColumn = textColumn % 4;
			const bool ink = glyphColumn < 3 && (TapeFont[glyph][5 - tapeRow] & (4 >> glyphColumn));
			color = ink ? _colors.tapeBlack : _colors.tapeYellow;
		}
		return shadeColor(color, (int)(borderShade + dither(pixelX, pixelY)));
	}
	// The ground beyond the map: where a ray going down meets the plane of the ground floor's
	// top, if that is beyond the border.
	if (rayDirection[2] < 0.0f && _groundModel)
	{
		const float groundDistance = (GroundTop - rayOrigin[2]) / rayDirection[2];
		if (groundDistance >= tExit)
		{
			depth = groundDistance;
			// The ground model repeats every tile: the colour of its top layer (layer 1, or
			// layer 0 where layer 1 is empty) at that spot, lit like open ground under the sky.
			const float groundX = rayOrigin[0] + rayDirection[0] * groundDistance;
			const float groundY = rayOrigin[1] + rayDirection[1] * groundDistance;
			Uint8 color = _groundModel->color[1][(int)std::floor(groundY) & 15][(int)std::floor(groundX) & 15];
			if (!color) color = _groundModel->color[0][(int)std::floor(groundY) & 15][(int)std::floor(groundX) & 15];
			color = shadeColor(color, (int)(_colors.ambient + _colors.faceShade[4] + groundDistance / 320.0f + dither(pixelX, pixelY)));
			// The haze closes in towards the horizon, spread over a longer distance than on the
			// map so the ground fades out gradually; under the sea the murk closes in as on the map.
			const float haze = _colors.underwater ? smoothstep(_maxDistance * 0.12f, _maxDistance * 0.65f, groundDistance)
				: smoothstep(_maxDistance * 0.3f, _maxDistance * 1.6f, groundDistance);
			if (haze > dither(pixelX + 2, pixelY + 1))
			{
				color = _colors.haze;
			}
			return color;
		}
	}
	// Nothing else in the way: the sky.
	depth = Infinity;
	return skyColor(rayDirection, pixelX, pixelY);
}

/**
 * The colour of the sky in a direction: a dithered gradient from the horizon up, with clouds
 * and the sun by day, stars and the moon by night, or just murky water when the battle is
 * under the sea.
 * @param rayDirection The ray's unit direction (z up).
 * @param pixelX The ray's pixel on the view (or block of pixels), for the dither pattern.
 * @param pixelY The same, down.
 * @return The palette colour to draw.
 */
Uint8 FirstPersonView::skyColor(const float rayDirection[3], int pixelX, int pixelY) const
{
	// The gradient: how far up the ray points (0 at the horizon and below, 1 straight up),
	// eased so the horizon's colours spread further up, picks one of the 32 sky colours.
	const float elevation = std::max(rayDirection[2], 0.0f);
	const float gradient = std::pow(elevation, 0.6f) * 31.0f;
	Uint8 color = _colors.sky[std::clamp((int)(gradient + dither(pixelX, pixelY)), 0, 31)];
	if (_colors.underwater)
	{
		return color;
	}

	// The sun by day and the moon by night: small discs where the ray points almost straight
	// at them (the cosine of the angle very close to 1).
	const float daylight = 1.0f - _colors.night;
	const float sunDot = rayDirection[0] * _colors.sunDir[0] + rayDirection[1] * _colors.sunDir[1] + rayDirection[2] * _colors.sunDir[2];
	if (daylight > 0.4f && sunDot > 0.9995f)
	{
		return _colors.sun;
	}
	if (_colors.night > 0.4f)
	{
		const float moonDot = rayDirection[0] * _colors.moonDir[0] + rayDirection[1] * _colors.moonDir[1]
			+ rayDirection[2] * _colors.moonDir[2];
		if (moonDot > 0.9992f)
		{
			return _colors.moon;
		}
	}

	// Clouds on a flat layer high up, thinning out towards the horizon, drifting slowly. The
	// ray meets the layer at a distance of about 1 / elevation (capped low down); the noise
	// there says how thick the cloud is: thin edges get the shadowed colours, thick middles
	// the lit ones.
	if (elevation > 0.02f && _colors.night < 0.9f)
	{
		const float layerDistance = 1.0f / std::max(elevation, 0.06f);
		const float cloudU = rayDirection[0] * layerDistance * 1.6f + _colors.cloudTime * 0.02f;
		const float cloudV = rayDirection[1] * layerDistance * 1.6f;
		const float density = cloudNoise(cloudU, cloudV);
		const float coverage = smoothstep(0.52f, 0.72f, density) * smoothstep(0.02f, 0.2f, elevation);
		if (coverage > dither(pixelX + 1, pixelY + 3))
		{
			const float darkness = std::clamp((0.8f - density) * 10.0f, 0.0f, 7.0f);
			return _colors.cloud[std::clamp((int)(darkness + dither(pixelX, pixelY)), 0, 7)];
		}
	}

	// Stars at night: the sky divided into small cells by azimuth and elevation angle, and a
	// hash of each cell lights up 6 in 1000 of them.
	if (_colors.night > 0.5f && elevation > 0.03f)
	{
		const float azimuth = std::atan2(rayDirection[1], rayDirection[0]), // radians
			elevationAngle = std::asin(std::min(rayDirection[2], 1.0f));
		const Uint32 cellHash = hash2((int)std::floor(azimuth * 160.0f), (int)std::floor(elevationAngle * 160.0f));
		if ((cellHash % 1000) < 6)
		{
			return _colors.star;
		}
	}
	return color;
}

/**
 * Casts the rays for a share of the view's rows, on one thread: every n-th row, starting at
 * the given one. For each pixel (or block of pixels, with a pixel size above 1) it walks a ray
 * from the eye through the voxel world and writes the colour it sees, the depth for the
 * sprites' depth test, and whether the pixel shines (for the glow around lights).
 * The threads only read the shared state, and each writes only its own rows.
 * @param firstBlockRow This thread's first row (of pixel blocks).
 * @param blockRowStep How many rows to step: the number of threads.
 */
void FirstPersonView::renderRows(int firstBlockRow, int blockRowStep)
{
	// The view's size in pixels, and the half-width and half-height of the image plane one
	// voxel in front of the eye (the tangents of half the field of view).
	const int width = getWidth(), height = getHeight();
	const float halfPlaneWidth = (float)_halfFovTangent;
	const float halfPlaneHeight = (float)(_halfFovTangent * height / width);
	// What every ray walks through: this frame's tile snapshot and door swings.
	RayContext rayContext;
	rayContext.tiles = _tileViews.data();
	rayContext.swings = _doorSwings.data();
	rayContext.size[0] = _save->getMapSizeX();
	rayContext.size[1] = _save->getMapSizeY();
	rayContext.size[2] = _save->getMapSizeZ();
	rayContext.maxDistance = _maxDistance;
	// The eye and the camera's axes, in single precision for speed.
	const float rayOrigin[3] = { (float)_eye[0], (float)_eye[1], (float)_eye[2] };
	const float forward[3] = { (float)_forward[0], (float)_forward[1], (float)_forward[2] };
	const float right[3] = { (float)_right[0], (float)_right[1], (float)_right[2] };
	const float up[3] = { (float)_up[0], (float)_up[1], (float)_up[2] };

	// With a pixel size above 1, one ray colours a block of pixels (pixelSize x pixelSize).
	const int pixelSize = std::clamp(Options::fpsPixelSize, 1, 3);
	const int blockRows = (height + pixelSize - 1) / pixelSize, blockColumns = (width + pixelSize - 1) / pixelSize;
	for (int blockRow = firstBlockRow; blockRow < blockRows; blockRow += blockRowStep)
	{
		// The block's top pixel row, and how far up the image plane its centre is.
		const int topRow = blockRow * pixelSize;
		const float planeV = (1.0f - (topRow + pixelSize * 0.5f) * 2.0f / height) * halfPlaneHeight;
		for (int blockColumn = 0; blockColumn < blockColumns; ++blockColumn)
		{
			// The block's left pixel column, and how far right on the image plane its centre is.
			const int leftColumn = blockColumn * pixelSize;
			const float planeU = ((leftColumn + pixelSize * 0.5f) * 2.0f / width - 1.0f) * halfPlaneWidth;
			// The ray through the block's centre: forward, plus right and up across the plane.
			float rayDirection[3];
			for (int axis = 0; axis < 3; ++axis)
			{
				rayDirection[axis] = forward[axis] + planeU * right[axis] + planeV * up[axis];
			}
			rayDirection[2] /= ZScaleF; // into the voxel grid (t stays along this ray; z below is the true depth)
			// Normalize it; its length before that turns a distance along the ray into a depth
			// along the view axis (below).
			const float directionLength = std::sqrt(rayDirection[0] * rayDirection[0] + rayDirection[1] * rayDirection[1]
				+ rayDirection[2] * rayDirection[2]);
			for (int axis = 0; axis < 3; ++axis)
			{
				rayDirection[axis] /= directionLength;
			}
			// Walk the ray, and colour the block by how it ended. The dither patterns go by the
			// block's coordinates, so with big pixels they grow with them.
			Hit hit;
			float tEnd, distanceAlongRay;
			int endAxis;
			Uint8 color;
			switch (castRay(rayContext, rayOrigin, rayDirection, hit, tEnd, endAxis))
			{
			case RAY_HIT:
				// Something on the map.
				color = shadeHit(hit, rayOrigin, rayDirection, blockColumn, blockRow);
				distanceAlongRay = hit.t;
				break;
			case RAY_SIDE:
				// Out through the map's side: the border tape, the ground beyond, or the sky.
				color = colorOutsideMap(rayOrigin, rayDirection, tEnd, endAxis, blockColumn, blockRow, distanceAlongRay);
				break;
			case RAY_FAR:
				// Past the view's reach: lost in the haze.
				color = _colors.haze;
				distanceAlongRay = Infinity;
				break;
			case RAY_BOTTOM:
				// Out through the bottom of the map: nothing down there.
				color = 0x0F;
				distanceAlongRay = Infinity;
				break;
			default:
				// Out through the top: the ground below the map's edge can still be in sight.
				color = colorOutsideMap(rayOrigin, rayDirection, tEnd, 2, blockColumn, blockRow, distanceAlongRay);
				break;
			}
			// Seen through window glass on the way: a little paler and bluer.
			if (hit.glass)
			{
				color = _colors.glassTint[color];
			}
			// Fill the block: colour, depth, and whether it shines.
			const Uint8 glowing = (hit.emissive && distanceAlongRay < Infinity) ? 1 : 0;
			const float viewDepth = distanceAlongRay / directionLength; // distance along the view axis, for sprite sorting
			for (int pixelY = topRow; pixelY < std::min(height, topRow + pixelSize); ++pixelY)
			{
				Uint8 *pixelRow = getRaw(0, pixelY);
				float *depthRow = &_depthBuffer[pixelY * width];
				for (int pixelX = leftColumn; pixelX < std::min(width, leftColumn + pixelSize); ++pixelX)
				{
					_glowMask[pixelY * width + pixelX] = glowing;
					pixelRow[pixelX] = color;
					depthRow[pixelX] = viewDepth;
				}
			}
		}
	}
}

/**
 * Casts the rays for every pixel of the view. The rows are shared out between up to eight
 * threads (this one included), each taking every n-th row so they all get a fair share of sky
 * and of busy ground. Returns when all of them are done.
 */
void FirstPersonView::castAllRays()
{
	// One thread per processor core, up to eight (worked out once).
	static const int threadCount = std::clamp((int)std::thread::hardware_concurrency(), 1, 8);
	// Start the helper threads on rows 1, 2, ..., take row 0 on this thread, and wait for them.
	std::vector<std::thread> helpers;
	for (int threadIndex = 1; threadIndex < threadCount; ++threadIndex)
	{
		helpers.emplace_back(&FirstPersonView::renderRows, this, threadIndex, threadCount);
	}
	renderRows(0, threadCount);
	for (auto &helper : helpers)
	{
		helper.join();
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
