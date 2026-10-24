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
 * share of the screen's rows on one thread (the threads are kept between frames).
 */
#include "FirstPersonView.h"
#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>
#ifdef __linux__
#include <sched.h>
#endif
#include <SDL.h>
#include "FirstPersonApi.h"
#include "../Engine/Logger.h"
#include "../Engine/Options.h"
#include "../Mod/Mod.h"
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
 * glass, each colour washed with red, blue, orange, white or green (the screen washes) or lit
 * by a flare's red light, and each colour laid over each other one (see-through drawing). Finally it sets the sun and the moon for the time of day, and how
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
	// The haze tables: every colour blended towards the haze in HazeSteps steps (the palette's
	// nearest to each mix), made again when the palette or the haze colour changes (the time of
	// day is set per battle). See hazeColor.
	{
		Uint32 hazeKey = 2166136261u;
		for (int index = 0; index < 256; ++index)
		{
			hazeKey = (hazeKey ^ (palette[index].r | (palette[index].g << 8) | (palette[index].b << 16))) * 16777619u;
		}
		hazeKey = (hazeKey ^ _colors.haze) * 16777619u;
		if (hazeKey != _colors.hazeTintKey)
		{
			_colors.hazeTintKey = hazeKey;
			const SDL_Color &hazeRgb = palette[_colors.haze];
			for (int step = 0; step <= SceneColors::HazeSteps; ++step)
			{
				const float amount = step / (float)SceneColors::HazeSteps;
				_colors.hazeTint[step][0] = 0;
				for (int index = 1; index < 256; ++index)
				{
					_colors.hazeTint[step][index] = step == 0 ? (Uint8)index : step == SceneColors::HazeSteps ? _colors.haze
						: nearest(palette[index].r + (hazeRgb.r - palette[index].r) * amount, palette[index].g + (hazeRgb.g - palette[index].g) * amount,
							palette[index].b + (hazeRgb.b - palette[index].b) * amount);
				}
			}
		}
	}
	_colors.star = nearest(230, 230, 240);
	_colors.moon = nearest(228, 228, 210);
	_colors.sun = nearest(255, 248, 200);
	_colors.fire[0] = nearest(255, 250, 200);
	_colors.fire[1] = nearest(255, 210, 60);
	_colors.fire[2] = nearest(240, 120, 20);
	_colors.fire[3] = nearest(160, 40, 10);
	_colors.hitbox[0] = nearest(90, 230, 110);
	_colors.hitbox[1] = nearest(255, 70, 60);
	_colors.hitbox[2] = nearest(250, 220, 70);
	_colors.downedIcon[0] = nearest(240, 110, 20);
	_colors.downedIcon[1] = nearest(255, 225, 80);
	_colors.downedIcon[2] = nearest(215, 25, 25);
	_colors.downedIcon[3] = nearest(250, 240, 120);
	_colors.downedIcon[4] = nearest(170, 205, 255);
	_colors.edgeDot = nearest(225, 45, 35);
	_colors.unexplored = nearest(78, 88, 110);
	_colors.rock = nearest(108, 94, 80);
	_colors.spark = nearest(255, 240, 160);
	_colors.blood[0] = nearest(90, 0, 0);
	_colors.blood[1] = nearest(160, 12, 8);
	_colors.dust = nearest(200, 190, 170);
	_colors.rain = nearest(150, 170, 200);
	_colors.snow = nearest(240, 240, 250);
	_colors.speck = nearest(236, 236, 228);
	_colors.sand = nearest(210, 170, 110);
	_colors.firefly = nearest(200, 255, 100);
	_colors.bubble = nearest(170, 230, 240);
	_colors.tapeYellow = nearest(240, 200, 30);
	_colors.trajectory = nearest(150, 235, 255);
	_colors.trajectoryForced = nearest(250, 215, 60);
	_colors.laserSight = nearest(235, 30, 25);
	_colors.laserDot = nearest(255, 110, 90);
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
			_colors.purpleTint[index] = nearest(std::min(255.0f, 55 + luminance * 0.75f), luminance * 0.25f, std::min(255.0f, 75 + luminance * 0.85f));
			_colors.flareTint[index] = nearest(std::min(255.0f, 70 + luminance * 1.15f), luminance * 0.32f, luminance * 0.24f);
		}
		// Colour 0 (black on the screen): the blue, orange, white, green and purple washes colour
		// even black, with a dark shade of their own; the red wash and the flare's light leave it
		// black.
		_colors.redTint[0] = 0;
		_colors.blueTint[0] = nearest(0, 2, 18);
		_colors.orangeTint[0] = nearest(60, 24, 0);
		_colors.whiteTint[0] = nearest(185, 185, 185);
		_colors.greenTint[0] = nearest(8, 22, 4);
		_colors.purpleTint[0] = nearest(40, 6, 58);
		_colors.flareTint[0] = 0;
		// Four greys, dark to light, for the TV static; two greens for a digital feed's broken blocks.
		for (int level = 0; level < 4; ++level) _colors.grey[level] = nearest(30 + 60 * level, 30 + 60 * level, 30 + 60 * level);
		_colors.glitchGreen[0] = nearest(40, 225, 70);
		_colors.glitchGreen[1] = nearest(10, 130, 40);
		// The smoke's colour: the game's own smoke, mixed (see smokeRgb).
		smokeRgb(_smokeRgb);
		_colors.smoke = nearest(_smokeRgb[0], _smokeRgb[1], _smokeRgb[2]);
		// The see-through tables (see seeThrough): first the palette's nearest colour to every
		// 15-bit RGB colour, then every colour laid over every other in each step, the mix (to
		// 5 bits a channel) looked up in that.
		_nearestRgb.resize(32768);
		for (int rgb15 = 0; rgb15 < 32768; ++rgb15)
		{
			_nearestRgb[rgb15] = nearest(((rgb15 >> 10) & 31) * 255 / 31.0f, ((rgb15 >> 5) & 31) * 255 / 31.0f, (rgb15 & 31) * 255 / 31.0f);
		}
		_seeThroughTables.resize((SeeThroughLevels - 1) << 16);
		for (int level = 1; level < SeeThroughLevels; ++level)
		{
			Uint8 *table = &_seeThroughTables[(level - 1) << 16];
			for (int over = 0; over < 256; ++over)
			{
				for (int under = 0; under < 256; ++under)
				{
					// Each channel: level steps of the colour over it, the rest of the one under it.
					auto channel = [level](int overValue, int underValue) {
						const int mixed = (overValue * level + underValue * (SeeThroughLevels - level) + SeeThroughLevels / 2) / SeeThroughLevels;
						return (mixed * 31 + 127) / 255;
					};
					table[(over << 8) | under] = _nearestRgb[(channel(palette[over].r, palette[under].r) << 10)
						| (channel(palette[over].g, palette[under].g) << 5) | channel(palette[over].b, palette[under].b)];
				}
			}
		}
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
	// The UFOs' lights for this frame (the UFO lights option).
	updateUfos();
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
 * @param ufoGlow If not null, receives a UFO's light at the point (0..1), blended the same way
 *        (the UFO lights option, see findUfos).
 * @return The shade, fractional when blended.
 */
float FirstPersonView::lightAt(float x, float y, float z, float *ufoGlow) const
{
	// The tile the point is in (clamped onto the map): its light is the answer without smooth
	// lighting, and stands in for undiscovered neighbours with it. (The light of each tile is
	// read from the compact copy, _tileLights: every pixel's shading reads eight of them.)
	const int mapSizeX = _save->getMapSizeX(), mapSizeY = _save->getMapSizeY(), mapSizeZ = _save->getMapSizeZ();
	const int tileX = std::clamp((int)std::floor(x / 16), 0, mapSizeX - 1);
	const int tileY = std::clamp((int)std::floor(y / 16), 0, mapSizeY - 1);
	const int tileZ = std::clamp((int)std::floor(z / 24), 0, mapSizeZ - 1);
	const TileLight &ownTile = _tileLights[(tileZ * mapSizeY + tileY) * mapSizeX + tileX];
	const float ownTileShade = ownTile.light;
	if (!_smoothLighting)
	{
		if (ufoGlow) *ufoGlow = ownTile.ufoGlow / 255.0f;
		return ownTileShade;
	}
	// The point in tile units measured from tile centres: the eight tiles around it start at
	// (lowX, lowY, lowZ), and the fractions say how far the point is towards the next ones.
	const float gridX = x / 16 - 0.5f, gridY = y / 16 - 0.5f, gridZ = z / 24 - 0.5f;
	const int lowX = (int)std::floor(gridX), lowY = (int)std::floor(gridY), lowZ = (int)std::floor(gridZ);
	const float fractionX = gridX - lowX, fractionY = gridY - lowY, fractionZ = gridZ - lowZ;
	float blendedShade = 0.0f, blendedGlow = 0.0f;
	// Add up the eight tiles' shades, each weighted by how close the point is to it. The bits
	// of `corner` pick the tile: bit 0 the next one east, bit 1 south, bit 2 up.
	for (int corner = 0; corner < 8; ++corner)
	{
		const int cornerX = lowX + (corner & 1), cornerY = lowY + ((corner >> 1) & 1),
			cornerZ = std::clamp(lowZ + ((corner >> 2) & 1), 0, mapSizeZ - 1); // no level below the ground or above the top
		float cornerShade, cornerGlow = 0.0f;
		if (cornerX < 0 || cornerY < 0 || cornerX >= mapSizeX || cornerY >= mapSizeY)
		{
			// Off the map: open ground under the sky.
			cornerShade = (float)_colors.ambient;
		}
		else
		{
			const TileLight &tileLight = _tileLights[(cornerZ * mapSizeY + cornerY) * mapSizeX + cornerX];
			cornerShade = tileLight.discovered ? tileLight.light : ownTileShade;
			cornerGlow = tileLight.discovered ? tileLight.ufoGlow : ownTile.ufoGlow;
		}
		const float weight = ((corner & 1) ? fractionX : 1 - fractionX) * (((corner >> 1) & 1) ? fractionY : 1 - fractionY)
			* (((corner >> 2) & 1) ? fractionZ : 1 - fractionZ);
		blendedShade += cornerShade * weight;
		blendedGlow += cornerGlow * weight;
	}
	if (ufoGlow) *ufoGlow = blendedGlow / 255.0f;
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
	rayContext.tileFlags = _tileFlags.data();
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
 * Where a line first meets the world as the view draws it: the same walk as the view's rays
 * (terrain, swinging doors), no units. The game's line of fire goes by the parts' collision
 * shapes, which in places hold voxels the art leaves empty (a craft's walls); a shot stopped by
 * those ends in front of the surface the view shows, and its mark is put here instead.
 * @param from Where the line starts, in voxels.
 * @param direction Its unit direction.
 * @param maxDistance How far to look, in voxels.
 * @param point Set to where it meets the world.
 * @return False if it meets nothing within that distance.
 */
bool FirstPersonView::visibleSurfaceAlong(const float from[3], const float direction[3], float maxDistance, float point[3]) const
{
	RayContext rayContext;
	rayContext.tiles = _tileViews.data();
	rayContext.tileFlags = _tileFlags.data();
	rayContext.swings = _doorSwings.data();
	rayContext.size[0] = _save->getMapSizeX();
	rayContext.size[1] = _save->getMapSizeY();
	rayContext.size[2] = _save->getMapSizeZ();
	rayContext.maxDistance = maxDistance;
	Hit hit;
	float tEnd;   // where the ray left the map (not needed here)
	int endAxis;  // through which side (not needed here)
	if ((int)_tileFlags.size() != _save->getMapSizeXYZ() || castRay(rayContext, from, direction, hit, tEnd, endAxis) != RAY_HIT || hit.t > maxDistance)
	{
		return false;
	}
	for (int axis = 0; axis < 3; ++axis)
	{
		point[axis] = from[axis] + direction[axis] * hit.t;
	}
	return true;
}

/**
 * Extra light at a point from the lamps nearby and from the soldier's flashlight, in
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
	// 14 tiles and fading out with the square of the distance. The dynamic flashlight (setting
	// "Flashlight": Dynamic) is a light on the weapon, as in Half-Life: a tight, bright round spot centred on the
	// crosshair (full strength within 8 degrees of it, fading out by 13), a faint halo round it
	// out to 24 degrees, reaching 20 tiles and fading more slowly.
	if (_flashlight && _dynamicFlashlight)
	{
		const float range = 16.0f * 20.0f; // voxels
		const float fromLight[3] = { point[0] - _flashlightPos[0], point[1] - _flashlightPos[1], point[2] - _flashlightPos[2] };
		const float distance = std::sqrt(fromLight[0] * fromLight[0] + fromLight[1] * fromLight[1] + fromLight[2] * fromLight[2]);
		if (distance > 0.5f && distance < range)
		{
			// How close the point is to the beam's axis (the cosine of the angle off it): the spot
			// from 0.974 (13 degrees) to full at 0.990 (8 degrees), the halo from 0.914 (24 degrees).
			const float cosine = (fromLight[0] * (float)_forwardInVoxels[0] + fromLight[1] * (float)_forwardInVoxels[1]
				+ fromLight[2] * (float)_forwardInVoxels[2]) / distance;
			if (cosine > 0.914f)
			{
				const float beam = smoothstep(0.974f, 0.990f, cosine) + 0.18f * smoothstep(0.914f, 0.974f, cosine);
				const float falloff = 1.0f - 0.85f * distance / range;
				const float facing = normal ? -(normal[0] * fromLight[0] + normal[1] * fromLight[1] + normal[2] * fromLight[2]) / distance
					: 1.0f;
				const float power = beam * falloff * falloff * 13.0f * (0.4f + 0.6f * std::max(0.0f, facing));
				// Light only what faces the light, isn't too faint, and isn't behind anything.
				if (power > 0.25f && facing > 0.0f && !(shadows && shadowed(point, _flashlightPos)))
				{
					light += power;
				}
			}
		}
	}
	else if (_flashlight)
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
 * @param hit What the ray hit; whether it shines (hit.emissive) is updated when the pixel shows
 *            an animated part's next frame.
 * @param rayOrigin The eye, in voxels.
 * @param rayDirection The ray's unit direction in the voxel grid.
 * @param pixelX The ray's pixel on the view (or block of pixels, with a pixel size above 1),
 *               for the dither pattern.
 * @param pixelY The same, down.
 * @return The palette colour to draw.
 */
Uint8 FirstPersonView::shadeHit(Hit &hit, const float rayOrigin[3], const float rayDirection[3], int pixelX, int pixelY) const
{
	const TileView &tileView = _tileViews[hit.tile];
	// Unexplored parts are slate-coloured shapes (see below). A wall belongs to the tile east or
	// south of it, but it's seen from both sides, so each part has its own "seen" bit; a swinging
	// door (part -1) goes by its tile.
	if (hit.part >= 0 ? !((tileView.seen >> hit.part) & 1) : !tileView.discovered)
	{
		// The underside of an unexplored level seen from an explored room below is just its
		// ceiling: draw it, lit by the room. Anything else unexplored is a plain shape.
		const int tilesPerLevel = _save->getMapSizeX() * _save->getMapSizeY();
		const bool exploredCeiling = hit.axis == 2 && rayDirection[2] > 0.0f && hit.tile >= tilesPerLevel
			&& _tileViews[hit.tile - tilesPerLevel].discovered;
		if (!exploredCeiling)
		{
			// Unexplored: shapes in a flat slate colour, shaded by the face like the rest, never
			// textured (nothing of the art shows) and never black (black reads as something
			// missing): the shade stops a few steps short of the colour group's end. Further off
			// they turn misty (see UnexploredMistFrom): they once grew darker with distance and
			// stood out against the sky as a skyline of buildings that weren't there (a player's
			// report); a soft blue-grey reads as shapes in the haze. At the view's far reach they
			// fade out fully, as explored terrain does.
			const int faceAxis = hit.axis >= 0 ? hit.axis : 2;
			const int face = faceAxis * 2 + (rayDirection[faceAxis] > 0.0f ? 1 : 0);
			const int darkest = 12 - (_colors.unexplored & 15);
			const Uint8 slate = shadeColor(_colors.unexplored, std::min(darkest, (int)(_colors.ambient * 0.5f + _colors.faceShade[face] + dither(pixelX, pixelY))));
			const float sight = _save->getMod()->getMaxViewDistance() * (float)Position::TileXY; // the game's sight range, in voxels
			const float mist = UnexploredMistMax * smoothstep(sight * UnexploredMistFrom, sight * UnexploredMistTo, hit.t);
			return hazeColor(slate, std::max(terrainHaze(hit.t), mist), pixelX + 2, pixelY + 1);
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
	// An animated part part way into its next frame shows that frame on a share of the pixels
	// (dithered; the share grows as the step goes on). Decided first, so that every colour below
	// comes from the same frame: a face the art never shows that took the next frame's raw
	// colour flashed a whole side of a UFO on and off with every step of its lights.
	const bool showNextFrame = dither(pixelX + 2, pixelY + 1) < _animationBlend;
	// The colour before shading: the voxel's own, unless the face is one the iso art doesn't show.
	Uint8 baseColor = hit.color;
	int rockCeilingMiddle = -1; // a ceiling under rock: its middle shade within its colour group (see the end)
	const float hitHeight = rayOrigin[2] + rayDirection[2] * hit.t; // in voxels
	if (hit.part >= 0 && tileView.part[hit.part])
	{
		// The model the pixel shows: the part's next frame once dissolved into it. That frame's
		// own colour for the voxel, and whether it shines in that frame.
		const VoxelModel &model = showNextFrame && tileView.next[hit.part] ? *tileView.next[hit.part] : *tileView.part[hit.part];
		const int voxelX = hit.voxel[0], voxelY = hit.voxel[1], voxelZ = hit.voxel[2];
		if (&model != tileView.part[hit.part])
		{
			const Uint8 nextFrameColor = model.color[voxelZ][voxelY][voxelX];
			if (nextFrameColor)
			{
				baseColor = nextFrameColor;
				hit.emissive = ((tileView.emissive >> hit.part) & 1) && (nextFrameColor & 15) <= 3 && voxelZ >= model.glowMinZ;
			}
		}
		// On a steady UFO model (the UFO lights option), a voxel the art shows from the east,
		// the south or above keeps its own colour on all its faces, so a stepped curve of the
		// hull shows one colour per voxel instead of stripes; only the voxels it never shows
		// (a hidden west or north skin, an underside) take another face's colour.
		const bool ownColorAllRound = model.exposed && (model.exposed[voxelZ][voxelY][voxelX] & 0x15);
		// A face the iso art never shows: what its row shows from the side it does.
		// An underside (seen from below) shows the colour seen from above: a thin plate's
		// most common top colour, else the plan view's colour of this column. Under rock (see
		// underRock), dark art (its colour in the darker half of its group, DarkCeilingShade)
		// shows its top's pattern instead, the rock's texture as the iso view shows its top:
		// one plain colour sank into its darkest shade (the XPiratez hideout's ceiling).
		const int tilesPerLevel = _save->getMapSizeX() * _save->getMapSizeY();
		const bool underside = hit.axis == 2 && rayDirection[2] > 0.0f;
		const bool undersideUnderRock = underside && underRock(hitHeight);
		auto undersideColor = [&](const VoxelModel &plate) -> Uint8
		{
			const Uint8 plain = plate.ceiling ? plate.ceiling : plate.plan[voxelY][voxelX];
			rockCeilingMiddle = undersideUnderRock && (plain & 15) >= DarkCeilingShade ? (plain & 15) : -1;
			return rockCeilingMiddle >= 0 ? plate.plan[voxelY][voxelX] : plain;
		};
		Uint8 shownColor = underside ? undersideColor(model) : 0;
		if (shownColor && model.color[23][voxelY][voxelX] && hit.tile + tilesPerLevel < (int)_tileViews.size())
		{
			// Its top is covered by what's on the level above (a craft's cabin under its roof
			// plates: the art shows the cabin there), so the top of that is what this is the
			// underside of.
			for (const VoxelModel *partAbove : _tileViews[hit.tile + tilesPerLevel].part)
			{
				if (partAbove && partAbove->color[0][voxelY][voxelX] && partAbove->plan[voxelY][voxelX])
				{
					shownColor = undersideColor(*partAbove);
					break;
				}
			}
		}
		if (hit.axis >= 0 && hit.axis < 2 && rayDirection[hit.axis] > 0.0f)
		{
			// A west- or north-facing side: on a craft the mirrored side, else its own row seen
			// from the other side (the colour seen from the east, or from the south).
			shownColor = mirroredColor(hit, hit.axis, false, showNextFrame);
			if (!shownColor) shownColor = hit.axis == 0 ? model.east[voxelZ][voxelY] : model.south[voxelZ][voxelX];
		}
		else if (hit.axis >= 0 && hit.axis < 2)
		{
			// A south or east face on a craft's hidden west or north skin (where the hull steps
			// in or out): the art sees it through the hull, so it too takes the mirrored side.
			shownColor = mirroredColor(hit, 1 - hit.axis, true, showNextFrame);
		}
		if (shownColor && !ownColorAllRound) baseColor = shownColor;
	}
	// The point the light is measured at: the hit, 1.5 voxels out from the face, so it samples
	// the air in front of the surface rather than the solid tile behind it.
	float surfacePoint[3];
	for (int axis = 0; axis < 3; ++axis)
	{
		surfacePoint[axis] = rayOrigin[axis] + rayDirection[axis] * hit.t + normal[axis] * 1.5f;
	}
	// The face's shading for its direction. On a steady UFO model (the UFO lights option) it
	// is the direction the voxel's open faces face together, so a stepped curve of the hull
	// shades smoothly instead of in stripes.
	float faceShading = _colors.faceShade[faceIndex];
	if (faceIndex == 5 && underRock(hitHeight))
	{
		faceShading = RockCeilingShade; // a ceiling under rock: no sun turns it away
	}
	if (hit.part >= 0 && tileView.part[hit.part] && tileView.part[hit.part]->exposed)
	{
		const Uint8 openFaces = tileView.part[hit.part]->exposed[hit.voxel[2]][hit.voxel[1]][hit.voxel[0]];
		if (((openFaces >> faceIndex) & 1) && _ufoFaceShade[openFaces] >= 0.0f) faceShading = _ufoFaceShade[openFaces];
	}
	// The shade (0 brightest .. 15 darkest): the game's light, the face's direction, and one
	// shade darker per 320 voxels (20 tiles) of distance.
	float ufoGlow = 0.0f; // a UFO's light here (the UFO lights option), 0..1
	float shade = lightAt(surfacePoint[0], surfacePoint[1], surfacePoint[2], _ufoLightsOn ? &ufoGlow : nullptr) + faceShading + hit.t / 320.0f;
	float sunShadow = 0.0f; // the shade a sun shadow adds (not darkness of the place: see the end)
	float flare = 0.0f; // red light from tactical flares
	if (hit.emissive)
	{
		// Lamps, lit screens and UFO lights shine whatever the light around them; the lights of
		// a steady UFO model pulse gently, in a wave round the UFO (see updateUfos).
		const VoxelModel *shining = hit.part >= 0 ? tileView.part[hit.part] : nullptr;
		shade = shining && shining->pulseKind ? _ufoPulse[shining->pulseKind - 1][_ufoPhaseOfTile[hit.tile] & 15] : std::min(shade, 0.5f);
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
			if (shadowed(surfacePoint, towardsSun)) sunShadow = 3.0f * daylight;
			shade += sunShadow;
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
	// A UFO's light (the UFO lights option): its colours on the ground and the walls around it,
	// on its underside and its outer skin, softly inside (sampled with the game's light above,
	// see findUfos). Coloured by day and brighter and more coloured at night and under the
	// sea; the inner colour close to the UFO, the outer one further out.
	const UfoSpot *ufoLighting = nullptr;
	float ufoTint = 0.0f, ufoOuterShare = 0.0f;
	if (ufoGlow > 0.004f && !hit.emissive && _ufoOfTile[hit.tile])
	{
		ufoLighting = &_ufos[_ufoOfTile[hit.tile] - 1];
		const float darkness = _colors.underwater ? 0.6f : _colors.night, glow = ufoGlow * _ufoBreath; // (the sea's light is bright enough)
		const bool onUfo = (_ufoPhaseOfTile[hit.tile] & UfoTileBit) != 0;
		shade -= glow * (2.0f + 6.0f * darkness);
		// Coloured by day too: the ground's pool strongly, the UFO itself less, so its hull's
		// detail stays readable (the tint keeps each colour's brightness).
		ufoTint = std::min(0.9f, glow * (onUfo ? 0.35f + 0.3f * darkness : 0.6f + 0.4f * darkness));
		ufoOuterShare = onUfo ? 0.0f : 1.0f - smoothstep(0.15f, 0.55f, ufoGlow); // (inside the UFO: the inner colour)
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
	// Shade the colour by whole palette shades, the fraction dithered between two. Shaded past
	// the darkest colour of its group, a colour turns black, as in the game, but only where the
	// place is really dark (see DarkOverflowFrom): the light there, without the face's turn from
	// the sun, the distance or a sun shadow. In a lit place it stays the darkest of its group:
	// dark art (a shot-away wooden floor, the rock over a base's rooms seen from below) showed
	// as black holes. On a UFO (its steady model, the UFO lights option) it never turns black:
	// the dark panels of the Harvester's gore tubes showed as black boxes.
	int wholeShades = (int)std::max(0.0f, shade + dither(pixelX, pixelY));
	const bool onSteadyUfo = hit.part >= 0 && tileView.part[hit.part] && tileView.part[hit.part]->exposed;
	const float placeShade = shade - faceShading - hit.t / 320.0f - sunShadow;
	const float blackShare = std::clamp((placeShade - DarkOverflowFrom) / (DarkOverflowTo - DarkOverflowFrom), 0.0f, 1.0f);
	const bool darkEnoughForBlack = !onSteadyUfo && blackShare > dither(pixelX + 1, pixelY + 2);
	if (rockCeilingMiddle >= 0 && !darkEnoughForBlack)
	{
		// A ceiling under rock keeps its texture, as the rock beyond the map does (see rockColor):
		// its middle shade stops RockMottleShades short of the darkest, its pattern round that.
		wholeShades = std::min(wholeShades, std::max(0, 15 - RockMottleShades - rockCeilingMiddle));
	}
	Uint8 color = darkEnoughForBlack ? shadeColor(baseColor, wholeShades) : (Uint8)((baseColor & 0xF0) | std::min(15, (baseColor & 15) + wholeShades));
	if (flare * 1.5f > 0.04f + dither(pixelX + 1, pixelY + 3))
	{
		color = _colors.flareTint[color]; // the flare's light is red
	}
	if (ufoTint > dither(pixelX + 3, pixelY + 2))
	{
		color = ufoLighting->tint[ufoOuterShare > dither(pixelX + 1, pixelY) ? 1 : 0][color]; // a UFO's light: its inner or outer colour
	}
	// Far away the haze takes over (see hazeColor and terrainHaze).
	return hazeColor(color, terrainHaze(hit.t), pixelX + 2, pixelY + 1);
}

/**
 * How much of the distance haze terrain at some distance is in: none near, all of it at the
 * view's reach, from where the setting "Distance haze" has it start; under the sea the murk
 * closes in much sooner.
 * @param distance How far away, in voxels.
 * @return The haze, 0 none .. 1 all haze.
 */
float FirstPersonView::terrainHaze(float distance) const
{
	return _colors.underwater ? smoothstep(_maxDistance * 0.12f, _maxDistance * 0.65f, distance)
		: smoothstep(_maxDistance * hazeStart(), _maxDistance, distance);
}

/**
 * Where the distance haze begins on land, as a share of the view's reach (setting "Distance
 * haze"): Normal from 45%, Light from 75%, Clear only in the last tenth, so the world doesn't
 * stop with a hard edge where the view ends.
 * @return The share, 0..1.
 */
float FirstPersonView::hazeStart() const
{
	return Options::fpsHaze == 2 ? 0.9f : Options::fpsHaze == 1 ? 0.75f : 0.45f;
}

/**
 * Fades a colour into the distance haze: blended towards the haze colour by the amount, from a
 * table of HazeSteps blends (the palette's nearest colour to each mix), dithered only between
 * the two steps next to the amount. Neighbouring steps are a shade apart, so the dither doesn't
 * show; dithering the haze colour itself over a dark wall made it look like the sky showing
 * through the wall in single pixels, and shimmer as the view moved.
 * @param color The colour.
 * @param haze How much haze: 0 none .. 1 all haze.
 * @param pixelX, pixelY The pixel, for the dither.
 * @return The colour in the haze.
 */
Uint8 FirstPersonView::hazeColor(Uint8 color, float haze, int pixelX, int pixelY) const
{
	if (haze <= 0.0f)
	{
		return color;
	}
	const int step = std::clamp((int)(haze * SceneColors::HazeSteps + dither(pixelX, pixelY)), 0, SceneColors::HazeSteps);
	return _colors.hazeTint[step][color];
}

/**
 * What a ray sees after leaving the map through its side (or its top): below the map's surface
 * (see chooseGround), the rock it's cut into; above it, the X-COM tape on its posts along the
 * map's border, then the map's ground running on to the horizon, then the sky. The rock, the
 * tape and the posts stand on the border itself, so the ray meets them right where it leaves the
 * map; the ground beyond is a flat plane at the surface's floor height (ground level outdoors),
 * tiled with the ground model chosen for the map. A map underground all the way up has rock
 * beyond every edge and over it as well (a player's report: a base's edge showed the ground of
 * its sewers far below, and the sky over it).
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
	// The top of the ground beyond the border: the surface's floor (a level is 24 voxels high).
	const float surfaceTop = GroundTop + _surfaceLevel * 24.0f;
	// Below it, the rock: a wall of it along the border (facing back into the map), or over an
	// enclosed map, its ceiling (facing down).
	if (exitAxis != 2 ? exitPoint[2] < surfaceTop : _enclosed)
	{
		depth = tExit;
		const int face = exitAxis == 2 ? 5 : exitAxis * 2 + (rayDirection[exitAxis] > 0.0f ? 1 : 0);
		return exitAxis == 2 ? rockColor(exitPoint[0], exitPoint[1], face, tExit, pixelX, pixelY)
			: rockColor(alongBorder, exitPoint[2] / ZScaleF, face, tExit, pixelX, pixelY);
	}
	const float tapeBottom = surfaceTop + 8.0f; // the tape hangs 8 voxels above the ground, 7 voxels high
	// The discreet map edge (setting) has no posts and no tape: the ground beyond and the sky
	// only, the edge marked by dots on the floor instead (see drawEdgeDots).
	const bool tape = !Options::fpsDiscreetEdge;
	const float borderShade = _colors.ambient * 0.5f;
	// A post every 64 voxels (4 tiles) along the border, 1.5 voxels thick and 17 voxels high,
	// shaded a little darker on the sides facing north or south than on those facing east or west.
	const float pastLastPost = alongBorder - std::floor(alongBorder / 64.0f) * 64.0f;
	if (tape && pastLastPost < 1.5f && exitPoint[2] >= surfaceTop && exitPoint[2] < surfaceTop + 17.0f)
	{
		depth = tExit;
		return shadeColor(_colors.post, (int)(borderShade + (exitAxis == 0 ? 1.0f : 2.0f) + dither(pixelX, pixelY)));
	}
	// The tape: a black edge at the bottom and top, and between them "X-COM  " in black on
	// yellow, repeated along the border.
	if (tape && exitPoint[2] >= tapeBottom && exitPoint[2] < tapeBottom + 7.0f)
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
	// The ground beyond the map: where a ray going down meets the plane of the surface's floor
	// top, if that is beyond the border.
	if (rayDirection[2] < 0.0f && _groundModel)
	{
		const float groundDistance = (surfaceTop - rayOrigin[2]) / rayDirection[2];
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
				: smoothstep(_maxDistance * hazeStart() * 0.67f, _maxDistance * 1.6f, groundDistance);
			return hazeColor(color, haze, pixelX + 2, pixelY + 1);
		}
	}
	// Nothing else in the way: the sky.
	depth = Infinity;
	return skyColor(rayDirection, pixelX, pixelY);
}

/**
 * Is an underside at some height a ceiling under rock? On a map underground all the way up (a
 * base, a colony's inside), everywhere; elsewhere up to the map's surface (see chooseGround): the
 * ground's own floor seen from the caves below it, and anything in them. (On a map whose surface
 * is its bottom level, nothing: a table's underside, a house's upper floor are plain ceilings.)
 * @param height The underside's height, in voxels.
 * @return True if it is.
 */
bool FirstPersonView::underRock(float height) const
{
	return _enclosed || height <= GroundTop + _surfaceLevel * 24.0f;
}

/**
 * The rock round an underground map (beyond its border, below its surface) and over one
 * underground all the way up: the map's own rock colour (see chooseGround) in a calm mottle, two
 * scales of value noise over the face, shaded by the face and the scene's light like the rest,
 * stopping a few steps short of black at its darkest (black reads as something missing), and
 * fading into the distance haze far off.
 * @param alongU Where on the rock face the point is, in voxels: along the border (or the map's x, for the ceiling).
 * @param alongV The same, up the border (or the map's y, for the ceiling).
 * @param face Which way the face looks (+x -x +y -y +z -z, the order of the face shading).
 * @param distance How far along the ray the rock is, in voxels.
 * @param pixelX The ray's pixel on the view (or block of pixels), for the dither pattern.
 * @param pixelY The same, down.
 * @return The palette colour to draw.
 */
Uint8 FirstPersonView::rockColor(float alongU, float alongV, int face, float distance, int pixelX, int pixelY) const
{
	// The light and the face first, stopping RockMottleShades short of the darkest shade the rock
	// may take, then the mottle on top of that, so it shows by day and by night alike.
	const float mottle = valueNoise(alongU / 12.0f, alongV / 9.0f) * 0.65f + valueNoise(alongU / 3.0f, alongV / 3.0f) * 0.35f;
	const Uint8 rock = _rockBase ? _rockBase : _colors.rock; // the map's own rock (see chooseGround), else a plain grey-brown
	const int darkest = 12 - (rock & 15);
	const int lit = std::min(darkest - RockMottleShades, (int)(_colors.ambient * 0.5f + _colors.faceShade[face]));
	const int shades = std::min(darkest, lit + (int)(mottle * RockMottleShades + dither(pixelX, pixelY)));
	return hazeColor(shadeColor(rock, shades), terrainHaze(distance), pixelX + 2, pixelY + 1);
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
 * The colour of the smoke's wall on a pixel: where a ray ran out of the soldier's sight in the
 * smoke (see SmokeTrace). The smoke's grey, shaded by its light, a shade lighter in thick billows
 * and darker in thin ones; the billows are cloud-like noise over the world, drifting slowly east
 * and up. (Smoke short of that is drawn see-through, as the game's own smoke: see
 * drawSeeThroughSmoke.)
 * @param light The light of the smoke (the game's shade, 0 brightest .. 15 darkest).
 * @param rayOrigin, rayDirection The ray.
 * @param distance Where along the ray the wall is.
 * @param billowOut If not null, set to how thick the billow is there (0 .. 1).
 * @return The colour.
 */
Uint8 FirstPersonView::smokeColor(Uint8 light, const float rayOrigin[3], const float rayDirection[3], float distance, float *billowOut) const
{
	// Where the smoke is, in voxels, and how thick its billow is there (0 .. 1).
	const float x = rayOrigin[0] + rayDirection[0] * distance, y = rayOrigin[1] + rayDirection[1] * distance,
		z = rayOrigin[2] + rayDirection[2] * distance;
	const float billow = cloudNoise((x + z * 0.7f) / 12.0f - _smokeDrift * 0.15f, (y - z * 0.5f) / 12.0f + _smokeDrift * 0.08f);
	if (billowOut) *billowOut = billow;
	// Its grey: a shade lighter in a thick billow (if the colour's group has a lighter one),
	// darker in a thin one, then shaded by the light.
	int color = _colors.smoke, shade = light;
	if (billow > 0.62f && (color & 15) > 0) --color;
	else if (billow < 0.38f) ++shade;
	return shadeColor((Uint8)color, shade);
}

/**
 * How much of the soldier's sight the smoke uses up looking through a point and on beyond it,
 * by the game's sight rules (see SmokeTrace, as the rays trace them for Modern's wall): the
 * line from the eye through the point, walked two voxels at a time as far as the sight reaches
 * (or off the map), its smoke added up tile by tile. Original smoke stacks its frames where the
 * soldier can't see through it (see drawSeeThroughSmoke).
 * @param point The point, in voxels.
 * @return The share of the sight left that the smoke on the line uses up: 1 or more where the
 *         sight runs out on it (the soldier can't see through the smoke there), less where they
 *         can, 0 with no smoke on it.
 */
float FirstPersonView::smokeSightUsedThrough(const double point[3]) const
{
	float direction[3] = { (float)(point[0] - _eye[0]), (float)(point[1] - _eye[1]), (float)(point[2] - _eye[2]) };
	const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
	if (length < 1.0f)
	{
		return 0.0f;
	}
	for (float &component : direction) component /= length;
	SmokeTrace smoke;
	smoke.sightVoxels = _smokeSightVoxels;
	smoke.smokeBlock = _smokeBlock;
	smoke.fireBlock = _fireBlock;
	const int sizeX = _save->getMapSizeX(), sizeY = _save->getMapSizeY(), sizeZ = _save->getMapSizeZ();
	const float Step = 2.0f;
	for (float from = 0.0f; from < _smokeSightVoxels; from += Step)
	{
		// This stretch of the line, and the tile its middle is in (off the map: the line ends).
		const float middle = from + Step * 0.5f;
		const int tileX = (int)std::floor((_eye[0] + direction[0] * middle) / Position::TileXY);
		const int tileY = (int)std::floor((_eye[1] + direction[1] * middle) / Position::TileXY);
		const int tileZ = (int)std::floor((_eye[2] + direction[2] * middle) / Position::TileZ);
		if (tileX < 0 || tileY < 0 || tileZ < 0 || tileX >= sizeX || tileY >= sizeY || tileZ >= sizeZ)
		{
			break;
		}
		if (addSmoke(smoke, _tileViews[(tileZ * sizeY + tileY) * sizeX + tileX], from, from + Step) >= 0.0f)
		{
			return 1.0f; // the sight runs out here
		}
	}
	return smoke.cost > 0.0f ? smoke.cost / std::max(1.0f, smoke.sightVoxels - smoke.lastSmoke) : 0.0f;
}

/**
 * Casts the rays for a share of the view's rows, on one thread: it takes the next row nobody
 * has taken yet from the shared counter, one row at a time, until there are none left (so the
 * threads share the work out by how fast they are). For each pixel (or block of pixels, with a
 * pixel size above 1) it walks a ray from the eye through the voxel world and writes the
 * colour it sees, the depth for the sprites' depth test, and whether the pixel shines (for the
 * glow around lights). Every pixel comes out the same whichever thread casts its ray.
 * The threads only read the shared state, and each writes only the rows it took.
 */
void FirstPersonView::renderRows()
{
	// The view's size in pixels, and the half-width and half-height of the image plane one
	// voxel in front of the eye (the tangents of half the field of view).
	const int width = getWidth(), height = getHeight();
	const float halfPlaneWidth = (float)_halfFovTangent;
	const float halfPlaneHeight = (float)(_halfFovTangent * height / width);
	// What every ray walks through: this frame's tile snapshot and door swings.
	RayContext rayContext;
	rayContext.tiles = _tileViews.data();
	rayContext.tileFlags = _tileFlags.data();
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

	// Smoke as the game's sight rules have it (setting "Smoke": Modern): traced along each
	// ray with the sight of the soldier the view looks through.
	SmokeTrace smokeSight;
	smokeSight.sightVoxels = _smokeSightVoxels;
	smokeSight.smokeBlock = _smokeBlock;
	smokeSight.fireBlock = _fireBlock;
	const bool traceSmoke = FirstPerson::smokeMode() == 2;

	// With a pixel size above 1, one ray colours a block of pixels (pixelSize x pixelSize).
	// Auto (0) picks 2 for views bigger than AutoPixelMaxWidth x AutoPixelMaxHeight, else 1.
	const bool bigView = width > AutoPixelMaxWidth || height > AutoPixelMaxHeight;
	const int pixelSize = Options::fpsPixelSize == 0 ? (bigView ? 2 : 1) : std::clamp(Options::fpsPixelSize, 1, 3);
	const int blockRows = (height + pixelSize - 1) / pixelSize, blockColumns = (width + pixelSize - 1) / pixelSize;
	for (int blockRow = _rayWorkers.nextPiece++; blockRow < blockRows; blockRow = _rayWorkers.nextPiece++)
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
			SmokeTrace smoke = smokeSight;
			rayContext.ghostThreshold = dither(blockColumn, blockRow); // ghosts show on a share of the blocks (see ghostOpacity)
			const RayEnd rayEnd = castRay(rayContext, rayOrigin, rayDirection, hit, tEnd, endAxis, traceSmoke ? &smoke : nullptr);
			switch (rayEnd)
			{
			case RAY_SMOKE:
				// Into smoke too thick to see through: the smoke, shaded by its light.
				color = smokeColor(smoke.light, rayOrigin, rayDirection, smoke.wallAt);
				distanceAlongRay = smoke.wallAt;
				hit.emissive = false;
				break;
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
			{
				// Out through the bottom of the map (through a hole blown in the ground floor):
				// the earth below, the ground's colour in deep shade, never a black hole.
				const float bottomX = rayOrigin[0] + rayDirection[0] * tEnd, bottomY = rayOrigin[1] + rayDirection[1] * tEnd;
				const Uint8 earth = _groundModel ? _groundModel->color[0][(int)std::floor(bottomY) & 15][(int)std::floor(bottomX) & 15] : 0;
				color = earth ? shadeColor(earth, std::min(12 - (earth & 15), 6 + (int)dither(blockColumn, blockRow))) : _colors.unexplored;
				distanceAlongRay = tEnd;
				break;
			}
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
			// Seen through smoke that used up more than about a third of the sight left: the wall of
			// smoke shows through, the more the nearer the smoke came to blocking the sight, so
			// the wall's edge is soft, and as ragged as the smoke's billows (a real blend, not a
			// dither). Thinner smoke is left to the see-through smoke drawn over the picture.
			if (traceSmoke && rayEnd != RAY_SMOKE && smoke.cost > 0.0f)
			{
				const float sightLeft = std::max(1.0f, smoke.sightVoxels - smoke.lastSmoke);
				float billow;
				const Uint8 wall = smokeColor(smoke.light, rayOrigin, rayDirection, smoke.lastSmoke, &billow);
				const float nearWall = std::clamp((smoke.cost / sightLeft + (billow - 0.5f) * 0.5f - 0.4f) / 0.6f, 0.0f, 1.0f);
				const int wallOpacity = (int)std::lround(nearWall * nearWall * (3.0f - 2.0f * nearWall) * SeeThroughLevels);
				if (wallOpacity > 0)
				{
					color = seeThrough(wallOpacity, wall, color);
					if (wallOpacity > SeeThroughLevels / 2) hit.emissive = false;
				}
			}
			// Fill the block: colour, depth, and whether it shines (a UFO's light counts three
			// times in the bloom, for a stronger halo round it).
			Uint8 glowing = (hit.emissive && distanceAlongRay < Infinity) ? 1 : 0;
			if (glowing && hit.part >= 0 && _tileViews[hit.tile].part[hit.part] && _tileViews[hit.tile].part[hit.part]->pulseKind) glowing = 3;
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
 * Casts the rays for every pixel of the view, on all the view's threads (see runOnAllThreads):
 * each takes rows from the top of the view one at a time until none are left.
 */
void FirstPersonView::castAllRays()
{
	// The flares' light for this frame, which every ray hitting near a flare needs, and the
	// time the smoke's billows drift by (the same for every thread).
	prepareFlareLights();
	_smokeDrift = (SDL_GetTicks() % 1000000) / 1000.0f;
	runOnAllThreads(&FirstPersonView::renderRows);
}

/**
 * How many processor threads the game may run on: on Linux the CPUs in its affinity mask, so a
 * game held to fewer cores (with taskset, in a container, by a launcher) starts no more threads
 * than it has cores to run them on; elsewhere every processor thread the machine has.
 * @return The number of processor threads, at least 1.
 */
static int usableProcessorThreads()
{
#ifdef __linux__
	cpu_set_t allowed;
	if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0 && CPU_COUNT(&allowed) > 0)
	{
		return CPU_COUNT(&allowed);
	}
#endif
	return std::max(1, (int)std::thread::hardware_concurrency());
}

/**
 * Runs a job on all the view's threads: the helpers are woken (and started, the first time),
 * everyone does the job, taking its pieces from the shared counter until none are left, and this
 * returns when the last piece is done.
 * Since the threads take pieces as they go, a slower core (a hybrid CPU's efficient cores, a
 * second thread on a core) still adds what it can and never holds the frame up; and a helper
 * that is slow to wake (its core asleep) joins only while there is work left, or not at all.
 * @param job The job: a member function that loops over pieces taken from _rayWorkers.nextPiece.
 */
void FirstPersonView::runOnAllThreads(void (FirstPersonView::*job)())
{
	// One thread per processor thread the game may use, up to MaxRenderThreads (worked out
	// once); the helpers are started on the first job, then kept for the next ones.
	static const int threadCount = std::clamp(usableProcessorThreads(), 1, MaxRenderThreads);
	if (!_rayWorkers.started)
	{
		_rayWorkers.started = true;
		for (int helper = 1; helper < threadCount; ++helper)
		{
			_rayWorkers.threads.emplace_back(&FirstPersonView::rayWorkerLoop, this);
		}
		Log(LOG_INFO) << "OpenXcom3D: the " << getWidth() << "x" << getHeight() << " 3D view draws on " << threadCount << " threads";
	}
	// Hand out the pieces from the first again, open the round, and wake the helpers for it.
	_rayWorkers.nextPiece = 0;
	{
		std::lock_guard<std::mutex> lock(_rayWorkers.mutex);
		_rayWorkers.job = job;
		++_rayWorkers.round;
		_rayWorkers.open = true;
	}
	_rayWorkers.wake.notify_all();
	// This thread does its share too, until no piece is left. Then it waits for the helpers
	// still on their last piece, and closes the round: a helper waking only now doesn't start
	// on a job that is done (and never takes a piece of the next one for it).
	(this->*job)();
	std::unique_lock<std::mutex> lock(_rayWorkers.mutex);
	_rayWorkers.finished.wait(lock, [this] { return _rayWorkers.busy == 0; });
	_rayWorkers.open = false;
}

/**
 * What each of the view's helper threads does as long as the view lives: it sleeps until a job
 * is handed out, joins it while the round is open, does its share (pieces until none are
 * left), tells the drawing thread when it is the last one busy, and goes back to sleep. It ends
 * when the view goes away.
 */
void FirstPersonView::rayWorkerLoop()
{
	unsigned lastRound = 0; // the last job this helper joined
	while (true)
	{
		// Sleep until there is a new job open to join (or the view is going away).
		void (FirstPersonView::*job)() = nullptr;
		{
			std::unique_lock<std::mutex> lock(_rayWorkers.mutex);
			_rayWorkers.wake.wait(lock, [this, lastRound] { return _rayWorkers.stop || (_rayWorkers.open && _rayWorkers.round != lastRound); });
			if (_rayWorkers.stop)
			{
				return;
			}
			lastRound = _rayWorkers.round;
			job = _rayWorkers.job;
			++_rayWorkers.busy;
		}
		// Its share of the job: pieces until there are none left.
		(this->*job)();
		// The last helper busy wakes the drawing thread (if it waits).
		std::lock_guard<std::mutex> lock(_rayWorkers.mutex);
		if (--_rayWorkers.busy == 0)
		{
			_rayWorkers.finished.notify_one();
		}
	}
}

/**
 * Stops the view's helper threads when the view goes away: tells them to end (under the lock,
 * so none can miss it), wakes them, and waits for each to end. Between jobs they only sleep, so
 * none is in the middle of one.
 */
FirstPersonView::RayWorkers::~RayWorkers()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		stop = true;
	}
	wake.notify_all();
	for (auto &thread : threads)
	{
		thread.join();
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
