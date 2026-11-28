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
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <vector>
#ifdef __linux__
#include <sched.h>
#endif
#include <SDL.h>
#include "FirstPersonApi.h"
#include "../Battlescape/Map.h"
#include "../Savegame/Tile.h"
#include "../Engine/Surface.h"
#include "../Battlescape/TileEngine.h"
#include "../Engine/Game.h"
#include "../Engine/Logger.h"
#include "../Engine/Options.h"
#include "../Engine/Palette.h"
#include "../Mod/MapDataSet.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleEnviroEffects.h"
#include "../Mod/RuleInterface.h"
#include "../Savegame/BattleUnit.h" // not used here, but its PARTS_STRING constants are part of this file's object code
#include "../Savegame/SavedBattleGame.h"
#include "RuleFirstPerson.h"
#include "FirstPersonViewShared.h"
#include "FirstPersonViewRayWalk.h"

namespace OpenXcom
{

/// One turn of the sine in WaveSineSteps steps, for the sea's waves (see waveHeight).
static const std::array<float, WaveSineSteps> WaveSine = []
{
	std::array<float, WaveSineSteps> table{};
	for (int step = 0; step < WaveSineSteps; ++step) table[step] = (float)std::sin(step * 2.0 * Pi / WaveSineSteps);
	return table;
}();

/**
 * Looks up the battle's sky and weather, once a battle, from the mod's tables (RuleFirstPerson;
 * a player, GitHub issue #94: rain under a blue or starry sky; the owner: "check all special
 * missions so sky is ok"):
 * - the sky: the Earth's (day and night, the clouds, overcast in rain and snow), unless the
 *   battle's mission type, one of the terrains its map was built of or its environment (its
 *   enviroEffects, as the big mods name them, many missions sharing one), the most particular
 *   first, has another: deep space (the stars all round, nothing past the map's edge), orbit
 *   (the same over the Earth), the Moon's (black, the stars and the Earth over its ground),
 *   Mars' (butterscotch by day, the stars by night, no Moon), the sea's (as TFTD's battles under
 *   water, for a sea floor the game keeps dry: X-Com Files') or a void (a dark sky, no sun, moon
 *   or stars: other dimensions, dreams). A battle under water always has the sea's;
 * - the weather: its environment's (acid rain, a dust storm), else that of the first of its
 *   terrain data sets that has one (rain, snow, or dust for blowing sand), else none. None out
 *   of the air or under the sea.
 */
void FirstPersonView::chooseSky()
{
	if (_skyChosen)
	{
		return;
	}
	_skyChosen = true;
	const RuleFirstPerson &rules = _save->getMod()->getFirstPerson();
	const std::string enviro = _save->getEnviroEffects() ? _save->getEnviroEffects()->getType() : std::string();
	// The terrains the map was built of, block by block (a craft's or a UFO's blocks have its name instead).
	std::vector<std::string> terrains;
	for (const std::vector<std::string> &column : _save->getFlattenedMapTerrainNames())
	{
		for (const std::string &name : column)
		{
			if (!name.empty() && std::find(terrains.begin(), terrains.end(), name) == terrains.end()) terrains.push_back(name);
		}
	}
	const std::string sky = rules.getSky(enviro, _save->getMissionType(), terrains);
	_sky = sky == "space" ? SkySpace : sky == "orbit" ? SkyOrbit : sky == "moon" ? SkyMoon : sky == "mars" ? SkyMars
		: sky == "sea" ? SkySea : sky == "void" ? SkyVoid : SkyEarth;
	if (_save->getDepth() > 0)
	{
		_sky = SkySea;
	}
	_weather = rules.getEnviroWeather(enviro);
	for (const MapDataSet *mapDataSet : *_save->getMapDataSets())
	{
		if (!_weather.empty()) break;
		_weather = rules.getWeather(mapDataSet->getName());
	}
	if (airless() || _sky == SkySea)
	{
		_weather.clear();
	}
	// The battle's palette as it was before its environment recoloured it (the game keeps a
	// backup of each), for the sky's colours (see updateColors). Not for Mars: its sky's colours
	// are Mars' own, and the big mods' Martian palettes (made for Mars' ground) would recolour
	// them again; picked from the palette on the screen, they stay butterscotch.
	const std::string paletteName = _save->getDepth() > 0 ? "PAL_BATTLESCAPE_" + std::to_string(_save->getDepth()) : "PAL_BATTLESCAPE";
	Palette *backup = _sky == SkyMars ? nullptr : _save->getMod()->getPalette("BACKUP_" + paletteName, false);
	_skyPalette = backup ? backup->getColors() : nullptr;
	static const char *const SkyNames[] = { "earth", "space", "orbit", "moon", "mars", "sea", "void" };
	Log(LOG_VERBOSE) << "OpenXcom3D: " << _save->getMissionType() << " (" << (enviro.empty() ? "no environment" : enviro) << "): the sky "
		<< SkyNames[_sky] << ", weather " << (_weather.empty() ? "none" : _weather);
}

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
	chooseSky();
	const SDL_Color *palette = getPalette();
	// The colour of a palette closest to an RGB colour (0..255 per channel), weighting the
	// channels by how bright they look to the eye. Colour 0 is transparent, so it is never picked.
	auto nearestIn = [](const SDL_Color *colors, float red, float green, float blue) {
		int bestIndex = 1;
		float bestDistance = 1e9f;
		for (int index = 1; index < 256; ++index)
		{
			// dr, dg, db: how far this palette colour is from the wanted one, per channel.
			const float dr = colors[index].r - red, dg = colors[index].g - green, db = colors[index].b - blue;
			const float distance = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;
			if (distance < bestDistance)
			{
				bestDistance = distance;
				bestIndex = index;
			}
		}
		return (Uint8)bestIndex;
	};
	// Most colours are the closest the palette on the screen has. The sky's (and the weather's)
	// are the closest the battle's palette had before its environment recoloured it (the big
	// mods' heat, cold, acid rain, a red sun; but not Mars': see chooseSky), so that the
	// environment recolours the sky as it does the map; picked from the screen's palette, they
	// undid its tint.
	auto nearest = [&](float red, float green, float blue) { return nearestIn(palette, red, green, blue); };
	auto nearestSky = [&](float red, float green, float blue) { return nearestIn(_skyPalette ? _skyPalette : palette, red, green, blue); };
	// Under night vision the goggles show every colour as the one of their own group with its shade (the low four bits:
	// see applyScreenWash, as the map does it). That is right for the art's colours, whose groups run bright to dark; a
	// colour picked here as the palette's nearest to some RGB (the sky, the haze, a colour seen through glass) can sit
	// anywhere in its group, and came out as bright or dark as that happened to be: a night sky and the haze far away
	// glaring, bright through a window (a player, GitHub issue #122). So with night vision on, the world's own colours
	// and tables are picked as the goggles show them: each palette colour as its shade in the goggles' group (`seen`),
	// and the nearest of the goggles' sixteen colours for the result (`nearestSeen`), whose shade is then its brightness.
	const bool goggles = _nightVision != 0;
	const int gogglesGroup = goggles ? (_nightVision - 1) * 16 : 0;
	SDL_Color seen[256];
	for (int index = 0; index < 256; ++index) seen[index] = goggles ? palette[gogglesGroup | (index & 15)] : palette[index];
	auto nearestGoggles = [&](float red, float green, float blue) {
		int best = gogglesGroup | 15;
		float bestDistance = 1e9f;
		for (int shade = gogglesGroup ? 0 : 1; shade < 16; ++shade)
		{
			const SDL_Color &color = palette[gogglesGroup | shade];
			const float dr = color.r - red, dg = color.g - green, db = color.b - blue;
			const float distance = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;
			if (distance < bestDistance) { bestDistance = distance; best = gogglesGroup | shade; }
		}
		return (Uint8)best;
	};
	auto nearestSeen = [&](float red, float green, float blue) { return goggles ? nearestGoggles(red, green, blue) : nearest(red, green, blue); };
	auto nearestSkySeen = [&](float red, float green, float blue) { return goggles ? nearestGoggles(red, green, blue) : nearestSky(red, green, blue); };
	// Mixes two RGB colours: amount 0 gives `from`, 1 gives `to`.
	auto mix = [](const float *from, const float *to, float amount, float *out) {
		for (int channel = 0; channel < 3; ++channel) out[channel] = from[channel] + (to[channel] - from[channel]) * amount;
	};

	// The time of day comes from the battle's global shade (0 bright day .. 15 dark night):
	// full day up to shade 3, full night from shade 12. Night vision and the map's debug vision
	// (see mapShade) brighten open ground, as they do the map's tiles.
	const int globalShade = std::clamp(_save->getGlobalShade(), 0, 15);
	_colors.night = std::clamp((globalShade - 3) / 9.0f, 0.0f, 1.0f);
	_colors.underwater = _sky == SkySea;
	// Rain and snow fall from clouds that cover the Earth's sky (GitHub issue #94).
	_colors.overcast = _sky == SkyEarth && (_weather == "rain" || _weather == "snow");
	_colors.ambient = mapShade(globalShade);
	_colors.cloudTime = SDL_GetTicks() / 1000.0f; // seconds, to drift the clouds and ripple the sea floor's light
	// This frame's waves on the sea (see waveHeight): each train's sine-table steps per voxel east
	// and south, and how far it has run (kept within one turn); and its crests' bend: the steps per
	// voxel along them (east and south) and how deep it is, in steps.
	static_assert(WaveTrainCount == 3, "_waveSteps holds three trains");
	for (int train = 0; train < WaveTrainCount; ++train)
	{
		const WaveTrain &wave = WaveTrains[train];
		const float stepsPerVoxel = WaveSineSteps / wave.length;
		_waveSteps[train][0] = stepsPerVoxel * std::cos(wave.heading);
		_waveSteps[train][1] = stepsPerVoxel * std::sin(wave.heading);
		_waveSteps[train][2] = -(float)std::fmod(SDL_GetTicks() / 1000.0 * wave.speed * stepsPerVoxel, (double)WaveSineSteps);
		const float bendStepsPerVoxel = WaveSineSteps / wave.bendLength;
		_waveSteps[train][3] = -bendStepsPerVoxel * std::sin(wave.heading);
		_waveSteps[train][4] = bendStepsPerVoxel * std::cos(wave.heading);
		_waveSteps[train][5] = wave.bendDepth * stepsPerVoxel;
	}

	// The sky's colours at the horizon and straight up (the zenith), by day and by night: the
	// Earth's clear sky, under rain clouds, under snow clouds (paler), Mars' dusty one, the black
	// of space and of the Moon's sky, a void's dark violet, and under the sea the water's, the
	// same all day. And the clouds' lit and shadowed sides by day and by night (RGB).
	struct SkyRgb { float dayHorizon[3], dayZenith[3], nightHorizon[3], nightZenith[3]; };
	static const SkyRgb EarthSky = { { 190, 210, 230 }, { 70, 120, 200 }, { 24, 28, 50 }, { 3, 4, 14 } };
	static const SkyRgb RainSky = { { 150, 155, 162 }, { 108, 114, 124 }, { 34, 36, 42 }, { 16, 17, 21 } };
	static const SkyRgb SnowSky = { { 202, 206, 212 }, { 162, 168, 178 }, { 40, 42, 48 }, { 20, 21, 26 } };
	static const SkyRgb MarsSky = { { 214, 172, 132 }, { 160, 112, 82 }, { 32, 18, 14 }, { 5, 3, 4 } }; // (the night's dark red in the big mods' Martian palette, not its olive)
	static const SkyRgb SpaceSky = { { 4, 4, 8 }, { 2, 2, 5 }, { 4, 4, 8 }, { 2, 2, 5 } };
	static const SkyRgb VoidSky = { { 52, 26, 64 }, { 12, 5, 20 }, { 40, 20, 50 }, { 8, 4, 14 } };
	static const SkyRgb SeaSky = { { 8, 34, 46 }, { 34, 104, 118 }, { 8, 34, 46 }, { 34, 104, 118 } };
	static const float cloudLit[3] = { 240, 240, 245 }, cloudDark[3] = { 140, 146, 162 };
	static const float cloudNightLit[3] = { 46, 48, 66 }, cloudNightDark[3] = { 20, 22, 34 };
	const SkyRgb &skyRgb = _colors.underwater ? SeaSky : airless() ? SpaceSky : _sky == SkyMars ? MarsSky : _sky == SkyVoid ? VoidSky
		: !_colors.overcast ? EarthSky : _weather == "snow" ? SnowSky : RainSky;
	float horizon[3], zenith[3], litCloud[3], darkCloud[3];
	mix(skyRgb.dayHorizon, skyRgb.nightHorizon, _colors.night, horizon);
	mix(skyRgb.dayZenith, skyRgb.nightZenith, _colors.night, zenith);
	// The sky gradient: 32 steps from the horizon up to the zenith.
	for (int band = 0; band < 32; ++band)
	{
		float color[3];
		mix(horizon, zenith, band / 31.0f, color);
		_colors.sky[band] = nearestSkySeen(color[0], color[1], color[2]);
	}
	// The clouds: 8 steps from their lit side to their shadowed side, darker at night.
	mix(cloudLit, cloudNightLit, _colors.night, litCloud);
	mix(cloudDark, cloudNightDark, _colors.night, darkCloud);
	for (int level = 0; level < 8; ++level)
	{
		float color[3];
		mix(litCloud, darkCloud, level / 7.0f, color);
		_colors.cloud[level] = nearestSkySeen(color[0], color[1], color[2]);
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
		hazeKey = (hazeKey ^ (Uint32)_nightVision) * 16777619u;
		if (hazeKey != _colors.hazeTintKey)
		{
			_colors.hazeTintKey = hazeKey;
			const SDL_Color &hazeRgb = seen[_colors.haze];
			for (int step = 0; step <= SceneColors::HazeSteps; ++step)
			{
				const float amount = step / (float)SceneColors::HazeSteps;
				_colors.hazeTint[step][0] = 0;
				for (int index = 1; index < 256; ++index)
				{
					_colors.hazeTint[step][index] = step == 0 ? (Uint8)index : step == SceneColors::HazeSteps ? _colors.haze
						: nearestSeen(seen[index].r + (hazeRgb.r - seen[index].r) * amount, seen[index].g + (hazeRgb.g - seen[index].g) * amount,
							seen[index].b + (hazeRgb.b - seen[index].b) * amount);
				}
			}
		}
	}
	_colors.star = nearestSky(230, 230, 240);
	_colors.faintStar = nearestSky(120, 122, 140);
	_colors.moon = nearestSky(228, 228, 210);
	_colors.sun = nearestSky(255, 248, 200);
	_colors.earth[0] = nearestSkySeen(46, 96, 178);
	_colors.earth[1] = nearestSkySeen(8, 12, 28);
	_colors.earth[2] = nearestSkySeen(236, 238, 246);
	_colors.earth[3] = nearestSkySeen(130, 180, 240);
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
	_colors.unexplored = nearestSeen(78, 88, 110);
	_colors.rock = nearestSeen(108, 94, 80);
	_colors.spark = nearest(255, 240, 160);
	_colors.blood[0] = nearest(90, 0, 0);
	_colors.blood[1] = nearest(160, 12, 8);
	_colors.dust = nearest(200, 190, 170);
	_colors.rain = nearestSky(150, 170, 200);
	_colors.snow = nearestSky(240, 240, 250);
	_colors.speck = nearest(236, 236, 228);
	_colors.glassShard[0] = nearest(200, 228, 240);
	_colors.glassShard[1] = nearest(240, 248, 252);
	_colors.glassShard[2] = nearest(150, 178, 196);
	_colors.sand = nearestSky(210, 170, 110);
	_colors.firefly = nearest(200, 255, 100);
	_colors.bubble = nearest(170, 230, 240);
	_colors.tapeYellow = nearest(240, 200, 30);
	_colors.trajectory = nearest(150, 235, 255);
	_colors.trajectoryForced = nearest(250, 215, 60);
	_colors.laserSight = nearest(235, 30, 25);
	_colors.laserDot = nearest(255, 110, 90);
	_colors.nightLight = nearest(255, 255, 255);
	_colors.forcedDot = nearest(255, 245, 150);
	_colors.laserRefused = nearest(125, 35, 30);
	_colors.marker = nearest(235, 40, 30);
	// (the enemies only the squad sees: the very colour of the game's green buttons for them, the interface's
	// squadsightUnits; and those of them in a clear line of fire, yellow)
	_colors.markerSquadSeen = _game->getMod()->getInterface("battlescape")->getElement("squadsightUnits")->color;
	_colors.markerLineOfFire = nearest(250, 215, 40);
	// (our own wounded and those about to pass out: the very colours of the game's blue and purple buttons for them)
	_colors.markerWounded = _game->getMod()->getInterface("battlescape")->getElement("woundedUnits")->color;
	_colors.markerPassingOut = _game->getMod()->getInterface("battlescape")->getElement("passingOutUnits")->color;
	_colors.sprayMark = nearest(150, 245, 140);
	_colors.sprayNext = nearest(160, 160, 170);
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
		// Every colour (but the transparent 0) as seen through window glass, a little paler and
		// bluer, and washed with each screen wash's colour by its brightness (luminance).
		for (int index = 1; index < 256; ++index)
		{
			const float luminance = palette[index].r * 0.3f + palette[index].g * 0.59f + palette[index].b * 0.11f;
			_colors.redTint[index] = nearest(60 + luminance * 0.85f, luminance * 0.18f, luminance * 0.15f);
			_colors.blueTint[index] = nearest(luminance * 0.1f, luminance * 0.16f, 25 + luminance * 0.45f);
			_colors.orangeTint[index] = nearest(std::min(255.0f, 80 + luminance * 0.9f), 30 + luminance * 0.45f, luminance * 0.08f);
			_colors.whiteTint[index] = nearest(std::min(255.0f, 180 + luminance * 0.3f), std::min(255.0f, 180 + luminance * 0.3f),
				std::min(255.0f, 180 + luminance * 0.3f));
			_colors.greenTint[index] = nearest(15 + luminance * 0.3f, 35 + luminance * 0.7f, luminance * 0.2f);
			_colors.purpleTint[index] = nearest(std::min(255.0f, 55 + luminance * 0.75f), luminance * 0.25f, std::min(255.0f, 75 + luminance * 0.85f));
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

	// The tables the world itself is coloured through, made again when the palette changes or night vision is switched
	// (with it on, as the goggles show the colours: see `seen` above): every colour (but the transparent 0) as seen
	// through window glass, a little paler and bluer; lit by a flare's red light; and unexplored terrain drawn
	// (setting "Unseen terrain": Visible), toned UnseenToneShare of the way to a slate grey as bright as the colour,
	// so it shows what is there and still reads as not yet seen.
	const Uint32 worldTintKey = (hash ^ (Uint32)_nightVision) * 16777619u;
	if (worldTintKey != _colors.worldTintKey)
	{
		_colors.worldTintKey = worldTintKey;
		const float slateLuminance = UnseenToneRgb[0] * 0.3f + UnseenToneRgb[1] * 0.59f + UnseenToneRgb[2] * 0.11f;
		for (int index = 1; index < 256; ++index)
		{
			const SDL_Color &color = seen[index];
			const float luminance = color.r * 0.3f + color.g * 0.59f + color.b * 0.11f;
			// (Through the goggles glass is only a touch paler: the 30% of pale blue it adds by eye lifted a dark room six
			// shades of the goggles' sixteen. Before the goggles' own colours were used here (GitHub issue #122) it was worse:
			// a dark colour's paler mix was often nearest a colour high in a dark group, and showed at full brightness, so
			// an unlit ceiling seen through a window glared while the part of it a soldier's light fell on came out darker.)
			const float glassShare = goggles ? 0.07f : 0.3f;
			_colors.glassTint[index] = nearestSeen(color.r * (1.0f - glassShare) + 150 * glassShare, color.g * (1.0f - glassShare) + 180 * glassShare,
				color.b * (1.0f - glassShare) + 205 * glassShare);
			_colors.flareTint[index] = nearestSeen(std::min(255.0f, 70 + luminance * 1.15f), luminance * 0.32f, luminance * 0.24f);
			auto toned = [&](float channel, int axis) {
				return channel + (std::min(255.0f, UnseenToneRgb[axis] * luminance / slateLuminance) - channel) * UnseenToneShare;
			};
			_colors.unseenTint[index] = nearestSeen(toned(color.r, 0), toned(color.g, 1), toned(color.b, 2));
			for (int step = 0; step < DarkFadeSteps; ++step)
			{
				const float kept = 1.0f - (step + 1) / (float)(DarkFadeSteps + 1);
				_colors.darkFade[step][index] = nearestSeen(color.r * kept, color.g * kept, color.b * kept);
			}
		}
		_colors.glassTint[0] = 0;
		_colors.flareTint[0] = 0;
		_colors.unseenTint[0] = 0;
		for (int step = 0; step < DarkFadeSteps; ++step) _colors.darkFade[step][0] = 0;
	}

	// The sun stands high in the north-west by day; at night there is only the moon, and the
	// faces get a flatter light. (Unit vectors in the world: x east, y south, z up.)
	const float sunDirection[3] = { -0.45f, -0.5f, 0.74f }, moonDirection[3] = { 0.35f, -0.75f, 0.56f };
	for (int axis = 0; axis < 3; ++axis)
	{
		_colors.sunDir[axis] = sunDirection[axis];
		_colors.moonDir[axis] = moonDirection[axis];
	}
	// Each face direction (+x -x +y -y +z -z) is shaded by how much it turns away from the light
	// (Lambert's cosine law): the sun's, strongly by day, only a little at night, and indoors
	// always as at night (no sun shines there, see indoorsAt). Under rain or snow clouds and in a
	// void no sun shines: the light comes from the whole sky above, every wall alike, and the
	// clouds dim the day a little. Undersides (-z) get a whole shade more on top.
	const float daylight = 1.0f - _colors.night;
	const bool sunless = _colors.overcast || _sky == SkyVoid;
	static const float straightUp[3] = { 0.0f, 0.0f, 1.0f };
	const float *shadingFrom = sunless ? straightUp : sunDirection;
	for (int axis = 0; axis < 3; ++axis)
	{
		_colors.shadingDir[axis] = shadingFrom[axis];
	}
	_colors.shadingTurn = 0.5f + (sunless ? 1.3f : 2.5f) * daylight;
	_colors.shadingBase = sunless ? 0.6f * daylight : 0.0f;
	static const float faceNormals[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	for (int face = 0; face < 6; ++face)
	{
		auto lambert = [&](const float *from)
		{
			return std::max(0.0f, faceNormals[face][0] * from[0] + faceNormals[face][1] * from[1] + faceNormals[face][2] * from[2]);
		};
		const float underside = face == 5 ? 1.0f : 0.0f;
		_colors.faceShade[face] = _colors.shadingBase + (1.0f - lambert(shadingFrom)) * _colors.shadingTurn + underside;
		_colors.indoorFaceShade[face] = (1.0f - lambert(sunDirection)) * 0.5f + underside;
	}
	// The UFOs' lights for this frame (the UFO lights option).
	updateUfos();
}

/**
 * The isometric map's debug vision mode, which Ctrl+End steps through (OXCE's, see Map::reShade):
 * 0 off, 1 the light halved ("Reaver's tests"), 2 everything at full light (Meridian's debug
 * helper). The view lights its picture by it as the map does (a player, GitHub issue #104: the
 * map's full bright left the 3D view as dark as before).
 * @return The mode.
 */
int FirstPersonView::debugVision() const
{
	return _map ? _map->getDebugVisionMode() : 0;
}

/**
 * A shade of the game's light (0 brightest .. 15 darkest; a tile's, or the open sky's on the
 * ground) as the isometric map shows it now (see Map::reShade): halved, or at full light, in its
 * debug vision modes (see debugVision); otherwise the game's own, no darker than
 * NightVisionShade with night vision.
 * @param gameShade The game's shade.
 * @return The shade the view lights by.
 */
int FirstPersonView::mapShade(int gameShade) const
{
	switch (debugVision())
	{
	case 1: return std::clamp(gameShade, 0, 15) / 2;
	case 2: return 0;
	default: return std::clamp(gameShade, 0, _nightVision ? NightVisionShade : 15);
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
 * @param ufoGlow If not null, receives a UFO's light at the point (0..1), blended the same way
 *        (the UFO lights option, see findUfos).
 * @param roofed If not null, receives how much the point is under a roof (0..1), blended the
 *        same way: the share of the game's roof shade its light holds (see shadeHit's sun).
 * @return The shade, fractional when blended.
 */
float FirstPersonView::lightAt(float x, float y, float z, float *ufoGlow, float *roofed) const
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
		if (roofed) *roofed = ownTile.roofed ? 1.0f : 0.0f;
		return ownTileShade;
	}
	// The point in tile units measured from tile centres: the eight tiles around it start at
	// (lowX, lowY, lowZ), and the fractions say how far the point is towards the next ones.
	const float gridX = x / 16 - 0.5f, gridY = y / 16 - 0.5f, gridZ = z / 24 - 0.5f;
	const int lowX = (int)std::floor(gridX), lowY = (int)std::floor(gridY), lowZ = (int)std::floor(gridZ);
	const float fractionX = gridX - lowX, fractionY = gridY - lowY, fractionZ = gridZ - lowZ;
	float blendedShade = 0.0f, blendedGlow = 0.0f, blendedRoofed = 0.0f;
	// Add up the eight tiles' shades, each weighted by how close the point is to it. The bits
	// of `corner` pick the tile: bit 0 the next one east, bit 1 south, bit 2 up.
	for (int corner = 0; corner < 8; ++corner)
	{
		const int cornerX = lowX + (corner & 1), cornerY = lowY + ((corner >> 1) & 1),
			cornerZ = std::clamp(lowZ + ((corner >> 2) & 1), 0, mapSizeZ - 1); // no level below the ground or above the top
		float cornerShade, cornerGlow = 0.0f, cornerRoofed = 0.0f;
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
			cornerRoofed = (tileLight.discovered ? tileLight.roofed : ownTile.roofed) ? 1.0f : 0.0f;
		}
		const float weight = ((corner & 1) ? fractionX : 1 - fractionX) * (((corner >> 1) & 1) ? fractionY : 1 - fractionY)
			* (((corner >> 2) & 1) ? fractionZ : 1 - fractionZ);
		blendedShade += cornerShade * weight;
		blendedGlow += cornerGlow * weight;
		blendedRoofed += cornerRoofed * weight;
	}
	if (ufoGlow) *ufoGlow = blendedGlow / 255.0f;
	if (roofed) *roofed = blendedRoofed;
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
 * Where a line first meets window glass as the view draws it (a pane of glass, see paneOfGlass,
 * or a mod's window), if nothing else the view draws comes first. Aiming stops on the glass (the
 * owner, GitHub issue #75: "All aiming stops at glass"): the game's own line of fire through a
 * pane's voxels may slip through the gaps between its layers, where the view shows smooth glass.
 * @param from Where the line starts, in voxels.
 * @param direction Its unit direction.
 * @param maxDistance How far to look, in voxels.
 * @param point Set to where it meets the glass.
 * @param tile Set to the glass's tile index.
 * @param part Set to its part (O_WESTWALL, O_NORTHWALL...).
 * @return False if it meets no glass first within that distance.
 */
bool FirstPersonView::glassAlong(const float from[3], const float direction[3], float maxDistance, float point[3], int &tile, int &part) const
{
	RayContext rayContext;
	rayContext.tiles = _tileViews.data();
	rayContext.tileFlags = _tileFlags.data();
	rayContext.swings = _doorSwings.data();
	rayContext.size[0] = _save->getMapSizeX();
	rayContext.size[1] = _save->getMapSizeY();
	rayContext.size[2] = _save->getMapSizeZ();
	rayContext.maxDistance = maxDistance;
	rayContext.glassStops = true;
	Hit hit;
	float tEnd;   // where the ray left the map (not needed here)
	int endAxis;  // through which side (not needed here)
	if ((int)_tileFlags.size() != _save->getMapSizeXYZ() || castRay(rayContext, from, direction, hit, tEnd, endAxis) != RAY_HIT
		|| !hit.glass || hit.t > maxDistance || hit.part < 0)
	{
		return false;
	}
	for (int axis = 0; axis < 3; ++axis)
	{
		point[axis] = from[axis] + direction[axis] * hit.t;
	}
	tile = hit.tile;
	part = hit.part;
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
	if (_flashlight && _aimLight)
	{
		// (Setting "Flashlight": Far light. A light standing where the crosshair's line meets the world, see _aimLight:
		// the same however far off, on what faces it and nothing stands before.)
		const float fromLight[3] = { point[0] - _flashlightPos[0], point[1] - _flashlightPos[1], point[2] - _flashlightPos[2] };
		const float distance = std::sqrt(fromLight[0] * fromLight[0] + fromLight[1] * fromLight[1] + fromLight[2] * fromLight[2]);
		if (_aimLight == 1 && distance < AimLightReach)
		{
			const float closeness = 1.0f - distance / AimLightReach;
			const float facing = normal && distance > 0.5f ? -(normal[0] * fromLight[0] + normal[1] * fromLight[1] + normal[2] * fromLight[2]) / distance : 1.0f;
			const float power = closeness * closeness * AimLightPower * (0.4f + 0.6f * std::max(0.0f, facing));
			if (power > 0.25f && facing > 0.0f && !(shadows && shadowed(point, _flashlightPos)))
			{
				light += power;
			}
		}
	}
	else if (_flashlight && _dynamicFlashlight)
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
 * north), which take the colour of a face the art does show (a wall's, with the setting "Wall
 * paint", the paint of the side it faces: see wallPaintColor); and an animated part part way
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
/**
 * 3DIso (research): a hit coloured as the isometric map draws it: the hit part's sprite (its frame now) at this very
 * pixel, as the map blits it (Map::drawTerrain: the tile's place, less the part's y offset; a north wall beside a west
 * wall only its right half), else the voxel's own colour; shaded as the map shades it (StandardShade: down the
 * colour's group, black past its end): unexplored tiles 16, walls by Map::getWallShade, the rest by the tile.
 */
Uint8 FirstPersonView::isoColor(const Hit &hit, int pixelX, int pixelY) const
{
	const int sizeX = _save->getMapSizeX(), sizeY = _save->getMapSizeY();
	const Position place(hit.tile % sizeX, (hit.tile / sizeX) % sizeY, hit.tile / (sizeX * sizeY));
	Tile *tile = const_cast<Tile*>(_save->getTile(place));
	Uint8 color = hit.color;
	int shade = 0;
	if (tile && hit.part >= 0 && hit.part <= 3)
	{
		const TilePart part = (TilePart)hit.part;
		const SurfaceRaw<const Uint8> sprite = tile->getSprite(part);
		if (sprite)
		{
			const int screenX = place.x * 16 - place.y * 16 + (int)_isoOffset[0];
			const int screenY = place.x * 8 + place.y * 8 - place.z * 24 + (int)_isoOffset[1] - tile->getYOffset(part);
			const int spriteX = (int)std::floor(isoMapX(pixelX)) - screenX, spriteY = (int)std::floor(isoMapY(pixelY)) - screenY;
			const bool halfHidden = part == O_NORTHWALL && tile->getSprite(O_WESTWALL) && spriteX < sprite.getWidth() / 2;
			if (!halfHidden && spriteX >= 0 && spriteY >= 0 && spriteX < sprite.getWidth() && spriteY < sprite.getHeight())
			{
				const Uint8 drawn = sprite.getBuffer()[spriteY * sprite.getPitch() + spriteX];
				if (drawn) color = drawn;
			}
		}
		shade = _map->obstacleShadeOf(tile, part, (part == O_WESTWALL || part == O_NORTHWALL) ? _map->wallShadeOf(part, tile) : !tile->isDiscovered(O_FLOOR) ? 16 : _map->reShade(tile)); // (a door seen lit beside an unexplored floor, as the map has it; obstacles as drawTerrain shades them)
	}
	else if (tile)
	{
		shade = tile->isDiscovered(O_FLOOR) ? _map->reShade(tile) : 16;
	}
	if (!color) return 0;
	return isoShaded(color, shade);
}

/**
 * 3DIso, extra "coloured light": a pixel's colour (shaded) with the coloured lights near its point added as the 3D view's
 * shader adds them (coloredLight: the colour unshaded, a little brighter, times the light's colour and power; the power
 * falling off squared to its reach), here without the surface's facing nor shadows; back in the palette, the nearest
 * colour, dithered.
 */
Uint8 FirstPersonView::isoColouredLight(Uint8 shaded, Uint8 raw, const float point[3], int x, int y) const
{
	const SDL_Color *palette = getPalette();
	float added[3];
	if (!isoLightAdded(raw, point, added) || _isoNearest.size() != 32768) return shaded;
	const float wobble = (dither(x, y) - 0.5f) * 8.0f;
	const int red = std::clamp((int)(palette[shaded].r + added[0] + wobble), 0, 255), green = std::clamp((int)(palette[shaded].g + added[1] + wobble), 0, 255),
		blue = std::clamp((int)(palette[shaded].b + added[2] + wobble), 0, 255);
	return _isoNearest[((red >> 3) << 10) | ((green >> 3) << 5) | (blue >> 3)];
}

Uint32 FirstPersonView::isoFullColour(Uint8 raw, float shade, float flare, const float point[3], bool coloured) const
{
	const SDL_Color *palette = getPalette();
	const float clamped = std::clamp(shade, 0.0f, 15.0f);
	const int lower = (int)clamped;
	const float between = clamped - lower;
	const SDL_Color &from = palette[isoShaded(raw, lower)], &to = palette[isoShaded(raw, std::min(15, lower + 1))];
	float rgb[3] = { from.r + (to.r - from.r) * between, from.g + (to.g - from.g) * between, from.b + (to.b - from.b) * between };
	if (flare > 0.0f)
	{
		// (a flare's light is red, as the 3D view's: see flareTint)
		const float red = std::min(1.0f, flare * 1.5f);
		rgb[0] = rgb[0] * (1.0f + 0.35f * red) + 30.0f * red;
		rgb[1] *= 1.0f - 0.25f * red;
		rgb[2] *= 1.0f - 0.4f * red;
	}
	float added[3];
	if (coloured && isoLightAdded(raw, point, added))
		for (int channel = 0; channel < 3; ++channel) rgb[channel] += added[channel];
	const Uint32 red = (Uint32)std::clamp((int)std::lround(rgb[0]), 0, 255), green = (Uint32)std::clamp((int)std::lround(rgb[1]), 0, 255),
		blue = (Uint32)std::clamp((int)std::lround(rgb[2]), 0, 255);
	return std::max(1u, red << 16 | green << 8 | blue); // (0 would mean the palette's)
}

bool FirstPersonView::isoLightAdded(Uint8 raw, const float point[3], float added[3]) const
{
	const SDL_Color *palette = getPalette();
	added[0] = added[1] = added[2] = 0.0f;
	bool lit = false;
	for (int index = 0; index < _isoLightCount; ++index)
	{
		const float *light = _isoLights[index];
		const float dx = light[0] - point[0], dy = light[1] - point[1], dz = (light[2] - point[2]) * ZScaleF;
		const float span = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (span >= light[3]) continue;
		const float falling = 1.0f - span / light[3];
		const float power = falling * falling * light[7] * 0.75f;
		if (power < 0.02f) continue;
		const float surface[3] = { (float)palette[raw].r, (float)palette[raw].g, (float)palette[raw].b };
		for (int channel = 0; channel < 3; ++channel) added[channel] += (surface[channel] * 0.9f + 40.0f) * light[4 + channel] * power;
		lit = true;
	}
	return lit;
}

/**
 * 3DIso: a colour shaded as the map blits terrain (Surface::blitRaw): down its group by the shade, black past the group's
 * end (StandardShade); with the map's night vision on, in the night vision's group instead (ColorReplace).
 */
/**
 * 3DIso: whether the map draws what a ray met black, the squad not having found it: a wall not seen; anything on a tile whose
 * floor isn't, but a door in its walls the squad has seen, which the map lights (Map::getWallShade). A door swinging goes by
 * its tile.
 */
bool FirstPersonView::isoUnfound(const Hit &hit) const
{
	if (hit.tile < 0 || hit.tile >= (int)_tileViews.size()) return false;
	const TileView &tileView = _tileViews[hit.tile];
	const bool wall = hit.part == O_WESTWALL || hit.part == O_NORTHWALL;
	if (wall && !((tileView.seen >> hit.part) & 1)) return true;
	return !tileView.discovered && !(wall && ((tileView.seenDoors >> hit.part) & 1));
}

Uint8 FirstPersonView::isoShaded(Uint8 color, int shade) const
{
	const int nightVision = _map->getNightVisionColor();
	if (nightVision)
	{
		const int shaded = (color & 15) + shade;
		return (shaded & 0xF0) ? 15 : (Uint8)(((nightVision - 1) << 4) | shaded);
	}
	const Uint8 shaded = (Uint8)(color + shade);
	return ((shaded ^ color) & 0xF0) ? 15 : shaded;
}

/**
 * 3DIso (research): the map's painter for one pixel. Every tile whose sprite's box covers the pixel (on each level up to
 * the view level about ten: two columns across, five rows of eight pixels down), taken in the order the map draws them
 * (Map::drawTerrain: level by level up, then south, then east; in a tile its floor, west wall, north wall, object), the
 * last whose sprite has a pixel there wins: its colour in the map's shade. Its depth comes from the 3D world (see
 * renderRows): the voxels' where the ray hit that very tile, else where the ray passes its middle; a floor's art lies on
 * its tile's bottom, so its depth is where the ray crosses that, pushed back by IsoFloorBehind so that it hides no unit
 * on its own level (the map draws them over the floors), only those a level down.
 */
bool FirstPersonView::isoArtAlong(const float origin[3], const float direction[3], double mapX, double mapY, Uint8 &color, float &distance, int &tileIndex, int &foundPart, Uint8 *raw, int *mapShade, float *lightDistance, float *artPoint, bool smoothHere) const
{
	// (the map's pixel the point is in; a smooth frame is taken at the point itself, see prepareIsoTileArt)
	const int pixelX = (int)std::floor(mapX), pixelY = (int)std::floor(mapY);
	const int size[3] = { _save->getMapSizeX(), _save->getMapSizeY(), _save->getMapSizeZ() };
	if (_isoArt.size() != (size_t)size[0] * size[1] * size[2]) return false;
	const int offsetX = (int)_isoOffset[0], offsetY = (int)_isoOffset[1];
	// (OXC3D_ISO3D_PROBE "x y": which candidates a pixel of the map met, in the log; read once, safely on every thread)
	static const std::pair<int, int> probe = [] { int x = -1, y = -1; if (const char *asked = getenv("OXC3D_ISO3D_PROBE")) sscanf(asked, "%d %d", &x, &y); return std::make_pair(x, y); }();
	const bool probing = pixelX == probe.first && pixelY == probe.second;
	// A part's art at the pixel (0: none there): the frame as the map blits it (its tile's place, less its y offset; a north
	// wall beside a west wall only its right half), its smooth version taken at the point itself.
	auto artAt = [&](const IsoPartArt &art, bool halfHidden, int originX, int originY) -> Uint8 {
		const int spriteX = pixelX - originX, spriteY = pixelY - originY;
		if (spriteX < 0 || spriteY < 0 || spriteX >= art.width || spriteY >= art.height) return 0;
		if (halfHidden && spriteX < art.width / 2) return 0;
		if (art.smooth && smoothHere)
		{
			const int bigX = std::clamp((int)std::floor((mapX - originX) * _isoSmoothFactor), 0, art.smooth->w - 1);
			const int bigY = std::clamp((int)std::floor((mapY - originY) * _isoSmoothFactor), 0, art.smooth->h - 1);
			return ((const Uint8*)art.smooth->pixels)[bigY * art.smooth->pitch + bigX];
		}
		return art.pixels[spriteY * art.pitch + spriteX];
	};
	// Where on its tile an art pixel lies (voxels up from the tile's bottom, and across): a floor on the bottom, a wall on its
	// tile's edge (exact), an object on the plane through the tile's middle square to the view.
	auto placeOf = [&](int part, double spriteX, double spriteY, int yOffset, double place[3]) {
		if (part == O_WESTWALL) { place[0] = 1.0; place[1] = 16.0 - spriteX; place[2] = 24.0 + (16.0 - spriteX) / 2.0 + yOffset - spriteY; }
		else if (part == O_NORTHWALL) { place[0] = spriteX - 16.0; place[1] = 1.0; place[2] = 24.0 + (spriteX - 16.0) / 2.0 + yOffset - spriteY; }
		else { place[0] = spriteX / 2.0; place[1] = 16.0 - spriteX / 2.0; place[2] = 32.0 + yOffset - spriteY; }
	};
	static const int lastDrawnFirst[4] = { O_OBJECT, O_NORTHWALL, O_WESTWALL, O_FLOOR };
	// The levels' cut sliding between two levels (see setIsoCut): the level it's in shows only its art below the cut; at a
	// level's top, as the map draws, whole tiles.
	const float cutLevels = _isoTop / 24.0f;
	const bool cutBetween = std::abs(cutLevels - std::round(cutLevels)) > 0.01f;
	const int topLevel = cutBetween ? (int)std::floor(cutLevels) : _isoViewLevel;
	// The columns across (u = x - y) whose 32 pixels cover the pixel: at most two.
	const int lowU = (int)std::ceil((pixelX - offsetX - 31) / 16.0), highU = (int)std::floor((pixelX - offsetX) / 16.0);
	for (int z = std::min(topLevel, size[2] - 1); z >= _isoLowLevel; --z)
	{
		// The rows down (v = x + y) whose frames can reach the pixel (40 pixels tall, up to 24 more lifted by a y offset).
		// The candidates come in the map's order, last drawn first: south first (y down), then east first (x down).
		const int highV = (int)std::floor((pixelY - offsetY + 24 * z) / 8.0) + 3, lowV = (int)std::floor((pixelY - offsetY + 24 * z - 40) / 8.0) - 1;
		const int highY = (int)std::floor((highV - lowU) / 2.0), lowY = (int)std::ceil((lowV - highU) / 2.0);
		for (int y = std::min(highY, size[1] - 1); y >= std::max(lowY, 0); --y)
			for (int u = highU; u >= lowU; --u)
			{
				const int x = u + y, v = x + y;
				if (v < lowV || v > highV || x < 0 || x >= size[0]) continue;
				const int index = (z * size[1] + y) * size[0] + x;
				const IsoTileArt &tileArt = _isoArt[index];
				if (!tileArt.any) continue;
				const int tileX = x * 16 - y * 16 + offsetX, tileY = x * 8 + y * 8 - z * 24 + offsetY;
				if (pixelY - tileY < tileArt.top || pixelY - tileY >= tileArt.bottom) continue; // (none of its frames reaches the row)
				for (int part : lastDrawnFirst)
				{
					const IsoPartArt &art = tileArt.part[part];
					if (!art.pixels) continue;
					const int originY = tileY - art.yOffset;
					const Uint8 drawn = artAt(art, part == O_NORTHWALL && tileArt.westWall, tileX, originY);
					if (probing) { Log(LOG_INFO) << "PROBE candidate " << x << "," << y << "," << z << " part " << part << " at " << pixelX - tileX << "," << pixelY - originY << " value " << (int)drawn; }
					if (!drawn) continue;
					double place[3] = { 8.0, 8.0, 0.0 };
					if (part != O_FLOOR) placeOf(part, mapX - tileX, mapY - originY, art.yOffset, place);
					if (cutBetween && z == topLevel)
					{
						// (a floor fades in or out with the cut's way through its level, dithered: it lies at the level's bottom,
						// where the cut passes at once; the rest below the cut)
						if (part == O_FLOOR ? !(dither((int)std::floor(mapX * 2.0), (int)std::floor(mapY * 2.0)) < (_isoTop - z * 24.0f) / 24.0f)
							: !(z * 24.0 + place[2] < _isoTop)) continue;
					}
					color = isoShaded(drawn, art.shade);
					if (raw) *raw = drawn;
					if (mapShade) *mapShade = art.shade;
					const float middle[3] = { x * 16.0f + 8.0f, y * 16.0f + 8.0f, z * 24.0f + 12.0f };
					distance = (middle[0] - origin[0]) * direction[0] + (middle[1] - origin[1]) * direction[1] + (middle[2] - origin[2]) * direction[2];
					if (lightDistance) *lightDistance = distance;
					if (part == O_FLOOR && direction[2] < 0.0f)
					{
						const float IsoFloorBehind = 20.0f;
						distance = (origin[2] - (z * 24.0f + 1.0f)) / -direction[2];
						if (lightDistance) *lightDistance = distance; // (a floor is lit where it lies, a voxel over the tile's bottom)
						if (artPoint) for (int axis = 0; axis < 3; ++axis) artPoint[axis] = origin[axis] + direction[axis] * distance;
						distance += IsoFloorBehind;
					}
					else
					{
						// (a wall's depth where its art lies on the wall, an object's on its tile's middle plane: the effects and
						// the marks meet it there)
						const float point[3] = { (float)(x * 16.0 + place[0]), (float)(y * 16.0 + place[1]), (float)(z * 24.0 + std::clamp(place[2], 0.5, 40.0)) };
						distance = (point[0] - origin[0]) * direction[0] + (point[1] - origin[1]) * direction[1] + (point[2] - origin[2]) * direction[2];
						if (lightDistance) *lightDistance = distance;
						if (artPoint) std::copy(point, point + 3, artPoint);
					}
					tileIndex = index;
					foundPart = part;
					if (probing) { Log(LOG_INFO) << "PROBE chosen " << x << "," << y << "," << z << " part " << part << " colour " << (int)drawn; }
					return true;
				}
			}
	}
	return false;
}

Uint8 FirstPersonView::shadeHit(Hit &hit, const float rayOrigin[3], const float rayDirection[3], int pixelX, int pixelY, ShadeDetail *detail) const
{
	if (_iso && !_isoVoxelArt) return isoColor(hit, pixelX, pixelY);
	// (3DIso, turned or not: what the squad hasn't found is black, as on the map, no glow round it (unexplored windows and lamps
	// were the 3D view's slate, grey squares in the black). The flight: black at 3DIso's end, dissolving into the 3D view's way
	// over its first stretch, see setFlightUnknownDark.)
	if ((_iso || _isoTurned || (_flight && _flightUnknownDark > 0.0f)) && isoUnfound(hit)
		&& (_iso || _isoTurned || dither(pixelX + 2, pixelY + 3) < _flightUnknownDark)) { hit.emissive = false; return 15; }
	const TileView &tileView = _tileViews[hit.tile];
	// Unexplored parts are drawn as the setting "Unseen terrain" has them (see below). A wall
	// belongs to the tile east or south of it, but it's seen from both sides, so each part has its
	// own "seen" bit; a swinging door (part -1) goes by its tile. With the fog of war off in 3D
	// (setting "Fog of war in 3D"; a player, GitHub issue #115: going down stairs, the floor below
	// stayed dark while the soldier looked straight at it), what the view itself sees within the
	// game's sight range is drawn as seen, explored or not: as far as the game explores terrain
	// from a soldier (its max view distance, across the ground). The map's fog of war stays.
	auto seenByTheView = [&]() {
		if (Options::fpsFogOfWar == 1) return false;
		const float sight = _save->getTileEngine()->getMaxViewDistance() * (float)Position::TileXY; // in voxels
		const float across = rayDirection[0] * hit.t, down = rayDirection[1] * hit.t;
		return across * across + down * down <= sight * sight;
	};
	bool unseen = false; // unexplored, drawn as seen terrain is and toned (Unseen terrain: Visible, see the end)
	if ((hit.part >= 0 ? !((tileView.seen >> hit.part) & 1) : !tileView.discovered) && !seenByTheView())
	{
		// The underside of an unexplored level seen from an explored room below is just its
		// ceiling: draw it, lit by the room. Anything else unexplored is a plain shape.
		const int tilesPerLevel = _save->getMapSizeX() * _save->getMapSizeY();
		const bool exploredCeiling = hit.axis == 2 && rayDirection[2] > 0.0f && hit.tile >= tilesPerLevel
			&& _tileViews[hit.tile - tilesPerLevel].discovered;
		if (!exploredCeiling && Options::fpsUnseenTerrain == 2)
		{
			// Visible (the owner: for whole-map screenshots too): shaded below as seen terrain is,
			// then toned towards slate grey, in the usual haze.
			unseen = true;
		}
		else if (!exploredCeiling)
		{
			// Mist and Fog: shapes in a flat slate colour, shaded by the face like the rest, never
			// textured (nothing of the art shows) and never black (black reads as something
			// missing): the shade stops a few steps short of the colour group's end.
			const int faceAxis = hit.axis >= 0 ? hit.axis : 2;
			const int face = faceAxis * 2 + (rayDirection[faceAxis] > 0.0f ? 1 : 0);
			const int darkest = 12 - (_colors.unexplored & 15);
			const float sight = _save->getMod()->getMaxViewDistance() * (float)Position::TileXY; // the game's sight range, in voxels
			if (Options::fpsUnseenTerrain == 1)
			{
				// Fog: the shapes darker with distance, as 0.9 drew them (see UnseenFogDarkening),
				// and a fog rising smoothly over them from the edge of the game's sight to all of
				// it at the view's reach, as thick as the distance haze has it (none at Clear:
				// the dark shapes all the way out; none out of the air, as the haze).
				const Uint8 slate = shadeColor(_colors.unexplored, std::min(darkest, (int)(_colors.ambient * 0.5f + _colors.faceShade[face]
					+ hit.t / UnseenFogDarkening + dither(pixelX, pixelY))));
				const float fogShare = Options::fpsHaze == 2 || airless() ? 0.0f : Options::fpsHaze == 1 ? UnseenFogLight : UnseenFogNormal;
				const float fog = fogShare * smoothstep(std::min(sight, _maxDistance * UnseenFogFromMax), _maxDistance, hit.t);
				return hazeColor(slate, std::max(terrainHaze(hit.t), fog), pixelX + 2, pixelY + 1);
			}
			// Mist: further off the shapes turn misty (see UnexploredMistFrom): they once grew
			// darker with distance and stood out against the sky as a skyline of buildings that
			// weren't there (a player's report); a soft blue-grey reads as shapes in the haze. At
			// the view's far reach they fade out fully, as explored terrain does.
			const Uint8 slate = shadeColor(_colors.unexplored, std::min(darkest, (int)(_colors.ambient * 0.5f + _colors.faceShade[face] + dither(pixelX, pixelY))));
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
	bool showNextFrame = dither(pixelX + 2, pixelY + 1) < _animationBlend;
	// (Where the voxel's colours in the two frames are far apart, it goes over whole at the step's middle: see AnimatedNearShades.)
	if (hit.part >= 0 && !hit.ghost && tileView.part[hit.part] && tileView.next[hit.part] && !tileView.next[hit.part]->pulse)
	{
		const Uint8 nowColor = tileView.part[hit.part]->color[hit.voxel[2]][hit.voxel[1]][hit.voxel[0]];
		const Uint8 nextColor = tileView.next[hit.part]->color[hit.voxel[2]][hit.voxel[1]][hit.voxel[0]];
		if (nowColor && nextColor && (((nowColor ^ nextColor) & 0xF0) || std::abs((nowColor & 15) - (nextColor & 15)) > AnimatedNearShades))
		{
			showNextFrame = _animationBlend >= 0.5f;
		}
	}
	// The colour before shading: the voxel's own, unless the face is one the iso art doesn't show.
	Uint8 baseColor = hit.color;
	int rockCeilingMiddle = -1; // a ceiling under rock: its middle shade within its colour group (see the end)
	float waveShade = 0.0f;     // on the sea: the shade its wave adds (see waterColor)
	bool onSea = false;         // ...and that this is the sea's top
	const float hitHeight = rayOrigin[2] + rayDirection[2] * hit.t; // in voxels
	if (hit.part >= 0 && tileView.part[hit.part])
	{
		// The model the pixel shows: the part's next frame once dissolved into it. That frame's
		// own colour for the voxel, and whether it shines in that frame.
		// (the part's own ghost: that model, see VoxelModel::ghost)
		const VoxelModel &model = hit.ghost ? *tileView.part[hit.part]->ghost
			: showNextFrame && tileView.next[hit.part] ? *tileView.next[hit.part] : *tileView.part[hit.part];
		const int voxelX = hit.voxel[0], voxelY = hit.voxel[1], voxelZ = hit.voxel[2];
		if (!hit.ghost && &model != tileView.part[hit.part])
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
			// Setting "Wall paint" (GitHub issue #111): a wall's face the art never shows in the paint of the
			// side it faces (from behind a house its outside paint, not its rooms').
			if (const Uint8 painted = wallPaintColor(hit, tileView, shownColor)) shownColor = painted;
		}
		else if (hit.axis >= 0 && hit.axis < 2)
		{
			// A south or east face on a craft's hidden west or north skin (where the hull steps
			// in or out): the art sees it through the hull, so it too takes the mirrored side.
			shownColor = mirroredColor(hit, 1 - hit.axis, true, showNextFrame);
		}
		// A floor's side under a wall on the tile's edge: the wall's colour, as the map draws the wall down to the floor.
		if (hit.part == O_FLOOR && hit.axis >= 0 && hit.axis < 2)
		{
			if (const Uint8 wallColor = floorSideUnderWall(hit, rayDirection[hit.axis] > 0.0f, showNextFrame)) shownColor = wallColor;
		}
		if (shownColor && !ownColorAllRound) baseColor = shownColor;
		// The sea's top: waves (see waterColor), as on the sea beyond the map's edge. Its side
		// (where the tile beside it has no floor) its deep colour: the art's own is the black
		// edge it draws under a tile of sea.
		if (model.water && _waterColors[0])
		{
			onSea = faceIndex == 4;
			baseColor = onSea ? waterColor(rayOrigin[0] + rayDirection[0] * hit.t, rayOrigin[1] + rayDirection[1] * hit.t, hit.t, pixelX, pixelY, waveShade)
				: _waterColors[1];
		}
	}
	// The point the light is measured at: the hit, 1.5 voxels out from the face, so it samples
	// the air in front of the surface rather than the solid tile behind it.
	float surfacePoint[3];
	for (int axis = 0; axis < 3; ++axis)
	{
		surfacePoint[axis] = rayOrigin[axis] + rayDirection[axis] * hit.t + normal[axis] * 1.5f;
	}
	// The face's shading for its direction. Indoors (see indoorsAt) no sun shines: it is shaded
	// as at night, only a little by its direction, and nothing there is in the sun's shadow (the
	// game's light already has the roof's shade): every wall of a house turned from the sun or
	// facing it from under the roof sank three shades deeper, a shop's grey inside wall into the
	// darkest of its colours. Out in the sun, on a steady UFO model (the UFO lights option) it is
	// the direction the voxel's open faces face together, so a stepped curve of the hull shades
	// smoothly instead of in stripes.
	const bool indoors = indoorsAt(surfacePoint);
	float openFaceShading = _colors.faceShade[faceIndex], roofedFaceShading = _colors.indoorFaceShade[faceIndex];
	if (hit.part >= 0 && tileView.part[hit.part] && tileView.part[hit.part]->exposed)
	{
		const Uint8 openFaces = tileView.part[hit.part]->exposed[hit.voxel[2]][hit.voxel[1]][hit.voxel[0]];
		if (((openFaces >> faceIndex) & 1) && _ufoFaceShade[openFaces] >= 0.0f)
		{
			openFaceShading = _ufoFaceShade[openFaces];
			roofedFaceShading = _ufoRoofedFaceShade[openFaces];
		}
	}
	float faceShading = indoors ? roofedFaceShading : openFaceShading;
	// The shade (0 brightest .. 15 darkest): the game's light, the face's direction, one shade
	// darker per 320 voxels (20 tiles) of distance (more thinly spread over a longer view, see
	// distanceShade), and on the sea its wave. (The face's share is added once the sun is known.)
	float ufoGlow = 0.0f; // a UFO's light here (the UFO lights option), 0..1
	float roofed = 0.0f;  // how much the point is under a roof, blended like the light (see lightAt)
	float shade = lightAt(surfacePoint[0], surfacePoint[1], surfacePoint[2], _ufoLightsOn ? &ufoGlow : nullptr, &roofed) + distanceShade(hit.t) + waveShade;
	if (indoors && underRock(surfacePoint[2])) roofed = 1.0f;
	float sunShadow = 0.0f; // the shade a sun shadow adds (not darkness of the place: see the end)
	float flare = 0.0f; // red light from tactical flares
	if (hit.emissive)
	{
		// Lamps, lit screens and UFO lights shine whatever the light around them; the lights of
		// a steady UFO model pulse gently, in a wave round the UFO (see updateUfos).
		const VoxelModel *shining = hit.part >= 0 ? tileView.part[hit.part] : nullptr;
		shade = shining && shining->pulseKind ? _ufoPulse[shining->pulseKind - 1][_ufoPhaseOfTile[hit.tile] & 15] : std::min(shade + faceShading, 0.5f);
	}
	else if (Options::fpsShadows)
	{
		// The sun: a face turned towards it with something in the way is in shadow (only by
		// day, out of doors, and not too far away, as each test is a ray walked 260 voxels
		// towards the sun; none where no sun shines: under the sea, under rain or snow clouds,
		// in a void).
		// A sun shadow is as deep as the game's own shade under a roof by day (RoofShade), and a face
		// in it is shaded as one under a roof: under the edge of a roof (smooth light) the shade the
		// game's light already holds there counts towards it, so a shadow runs on under a craft's
		// wing or a porch without a step at the roof's tile.
		const float RoofShade = 2.0f;
		const float daylight = _colors.underwater || _colors.overcast || _sky == SkyVoid ? 0.0f : 1.0f - _colors.night;
		const float sunFacing = normal[0] * _colors.sunDir[0] + normal[1] * _colors.sunDir[1] + normal[2] * _colors.sunDir[2];
		if (roofed < 1.0f && daylight > 0.3f && sunFacing > 0.05f && hit.t < 640.0f)
		{
			const float towardsSun[3] = { surfacePoint[0] + _colors.sunDir[0] * 260.0f,
				surfacePoint[1] + _colors.sunDir[1] * 260.0f,
				surfacePoint[2] + _colors.sunDir[2] * 260.0f };
			if (shadowed(surfacePoint, towardsSun))
			{
				faceShading = roofedFaceShading;
				sunShadow = RoofShade * daylight * (1.0f - roofed);
			}
			else faceShading = openFaceShading + (roofedFaceShading - openFaceShading) * roofed; // (in the sun at a roof's edge: towards the roof's)
		}
		shade += faceShading + sunShadow;
		// Lamps and the flashlight, throwing shadows.
		shade -= pointLight(surfacePoint, normal, true);
	}
	else
	{
		// Lamps and the flashlight, without shadows.
		shade += faceShading;
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
		// Coloured by day too: the ground's pool strongly, the UFO's hull outside less, so its
		// detail stays readable, and its inside (under its own roof) by its soft inner light,
		// a fifth of the way to the light's colour by day (the tint keeps each colour's brightness).
		const float strength = onUfo && indoors ? UfoInsideTint + UfoInsideTintNight * darkness : onUfo ? 0.35f + 0.3f * darkness : 0.6f + 0.4f * darkness;
		ufoTint = std::min(0.9f, glow * strength);
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
	if (detail)
	{
		// (for 3DIso's coloured light and full colour: the colour unshaded, the light, the flare's share, where)
		detail->raw = baseColor;
		detail->shade = shade;
		detail->flare = flare;
		std::copy(surfacePoint, surfacePoint + 3, detail->point);
		detail->valid = true;
	}
	int wholeShades = (int)std::max(0.0f, shade + dither(pixelX, pixelY));
	const bool onSteadyUfo = hit.part >= 0 && tileView.part[hit.part] && tileView.part[hit.part]->exposed;
	const float placeShade = shade - faceShading - distanceShade(hit.t) - waveShade - sunShadow;
	const float blackShare = std::clamp((placeShade - DarkOverflowFrom) / (DarkOverflowTo - DarkOverflowFrom), 0.0f, 1.0f);
	const bool darkEnoughForBlack = !onSteadyUfo && blackShare > dither(pixelX + 1, pixelY + 2);
	if (rockCeilingMiddle >= 0 && !darkEnoughForBlack)
	{
		// A ceiling under rock keeps its texture, as the rock beyond the map does (see rockColor):
		// its middle shade stops RockMottleShades short of the darkest, its pattern round that.
		wholeShades = std::min(wholeShades, std::max(0, 15 - RockMottleShades - rockCeilingMiddle));
	}
	// (The sea stops short of its groups' last shades, all but black, see WaterDarkestShade.)
	const int darkest = onSea ? std::max(baseColor & 15, WaterDarkestShade) : 15;
	Uint8 color = darkEnoughForBlack ? shadeColor(baseColor, wholeShades) : (Uint8)((baseColor & 0xF0) | std::min(darkest, (baseColor & 15) + wholeShades));
	if (!onSea && !onSteadyUfo && (baseColor & 15) + wholeShades > 15)
	{
		// Past its group's end it goes to black by way of the palette's darker colours, the
		// darker the place (see DarkFadeSteps), not straight from the group's darkest.
		color = fadedToBlack((Uint8)(baseColor | 15), blackShare, dither(pixelX + 1, pixelY + 2));
	}
	if (flare * 1.5f > 0.04f + dither(pixelX + 1, pixelY + 3))
	{
		color = _colors.flareTint[color]; // the flare's light is red
	}
	if (ufoLighting && ufoOuterShare <= 0.0f && (_ufoPhaseOfTile[hit.tile] & UfoTileBit))
	{
		// On the UFO itself (inside it, and its hull): its inner light mixed in evenly, the
		// nearest of the even tints to the light's strength there, no dots (see chooseUfoColors).
		const int step = std::min(UfoSpot::SoftTintSteps, (int)std::lround(ufoTint * UfoSpot::SoftTintSteps));
		if (step > 0) color = ufoLighting->softTint[step - 1][color];
	}
	else if (ufoTint > dither(pixelX + 3, pixelY + 2))
	{
		color = ufoLighting->tint[ufoOuterShare > dither(pixelX + 1, pixelY) ? 1 : 0][color]; // a UFO's light on the ground around it: its inner or outer colour, sparkling
	}
	// Unexplored terrain drawn (Unseen terrain: Visible) toned towards slate grey (see updateColors).
	if (unseen)
	{
		color = _colors.unseenTint[color];
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
	if (_iso || _isoTurned) return 0.0f; // (3DIso, turned too: the map has no distance)
	const float haze = _colors.underwater ? smoothstep(_maxDistance * 0.12f, _maxDistance * 0.65f, distance)
		: smoothstep(_maxDistance * hazeStart(), _maxDistance, distance);
	return _flight ? haze * _flightDistanceShare : haze; // (3DIso's flight: from none, see setFlightDistanceShare)
}

/**
 * How many shades darker the distance makes terrain: one for every DistanceShadeVoxels (20 tiles),
 * or for every third of the view's reach when that is longer (DistanceShadeReachParts), so a long view
 * keeps the far ground lit (see DistanceShadeVoxels); at the usual reach of 60 tiles, as before.
 * @param distance How far away, in voxels.
 * @return The shades, 0 or more.
 */
float FirstPersonView::distanceShade(float distance) const
{
	if (_iso || _isoTurned) return 0.0f; // (3DIso, turned too: the map has no distance)
	const float shades = distance / std::max(DistanceShadeVoxels, _maxDistance / DistanceShadeReachParts);
	return _flight ? shades * _flightDistanceShare : shades; // (3DIso's flight: from none, see setFlightDistanceShare)
}

/**
 * Where the distance haze begins on land, as a share of the view's reach (setting "Distance
 * haze"): Normal from 45%, Light from 75%, Clear only in the last tenth, so the world doesn't
 * stop with a hard edge where the view ends. Out of the air (space, orbit, the Moon) always as
 * Clear: there's no air to be hazy.
 * @return The share, 0..1.
 */
float FirstPersonView::hazeStart() const
{
	return Options::fpsHaze == 2 || airless() ? 0.9f : Options::fpsHaze == 1 ? 0.75f : 0.45f;
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
 * tiled with the ground model chosen for the map, or the water's beyond a stretch of the border
 * where the water reaches it (see chooseGround). A map underground all the way up has rock
 * beyond every edge and over it as well (a player's report: a base's edge showed the ground of
 * its sewers far below, and the sky over it). In space (a station, a ship: see chooseSky) there
 * is nothing past the edge but space: no rock, no tape and no ground.
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
	const bool inSpace = _sky == SkySpace || _sky == SkyOrbit;
	if (!inSpace && (exitAxis != 2 ? exitPoint[2] < surfaceTop : _enclosed))
	{
		depth = tExit;
		const int face = exitAxis == 2 ? 5 : exitAxis * 2 + (rayDirection[exitAxis] > 0.0f ? 1 : 0);
		return exitAxis == 2 ? rockColor(exitPoint[0], exitPoint[1], face, tExit, pixelX, pixelY)
			: rockColor(alongBorder, exitPoint[2] / ZScaleF, face, tExit, pixelX, pixelY);
	}
	const float tapeBottom = surfaceTop + 8.0f; // the tape hangs 8 voxels above the ground, 7 voxels high
	// The discreet map edge (setting) has no posts and no tape: the ground beyond and the sky
	// only, the edge marked by dots on the floor instead (see drawEdgeDots). Nor has space.
	const bool tape = !Options::fpsDiscreetEdge && !inSpace;
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
			// rows run top to bottom, so tape rows 5..1 are glyph rows 0..4). The text reads left
			// to right from inside the map on every side: along the border eastwards on the north
			// side and southwards on the east side, but the other way on the south and west sides,
			// where those run to the left (a player's report, GitHub issue #44: "moc-x").
			const bool readsBackwards = exitAxis == 0 ? rayDirection[0] < 0.0f : rayDirection[1] > 0.0f;
			const float alongText = readsBackwards ? -alongBorder : alongBorder;
			const int textColumn = ((int)std::floor(alongText) % (TapeTextLength * 4) + TapeTextLength * 4) % (TapeTextLength * 4);
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
			const float groundX = rayOrigin[0] + rayDirection[0] * groundDistance;
			const float groundY = rayOrigin[1] + rayDirection[1] * groundDistance;
			// Beyond a stretch of the border where the water reaches it, the water goes on: the
			// tile of the map's outer ring straight back across the border from the spot (the
			// corner tile, beyond a corner) tells.
			const VoxelModel *ground = _groundModel;
			if (_waterModel)
			{
				const int mapSizeX = _save->getMapSizeX();
				const int edgeX = std::clamp((int)std::floor(groundX / Position::TileXY), 0, mapSizeX - 1);
				const int edgeY = std::clamp((int)std::floor(groundY / Position::TileXY), 0, _save->getMapSizeY() - 1);
				if (_waterAtEdge[edgeY * mapSizeX + edgeX]) ground = _waterModel;
			}
			// The model repeats every tile: the colour of its top layer (layer 1, or layer 0
			// where layer 1 is empty) at that spot; the sea's waves (see waterColor). Lit like
			// open ground under the sky.
			Uint8 color;
			float waveShade = 0.0f;
			if (ground->water && _waterColors[0])
			{
				color = waterColor(groundX, groundY, groundDistance, pixelX, pixelY, waveShade);
			}
			else
			{
				color = ground->color[1][(int)std::floor(groundY) & 15][(int)std::floor(groundX) & 15];
				if (!color) color = ground->color[0][(int)std::floor(groundY) & 15][(int)std::floor(groundX) & 15];
			}
			// Shaded past the darkest of its colour group, it turns black only where the open ground
			// is really dark, as on the map (see shadeHit, DarkOverflowFrom); else it stays the
			// darkest of its group (the sea's troughs at dusk were black blotches).
			const int shades = (int)(_colors.ambient + _colors.faceShade[4] + waveShade + distanceShade(groundDistance) + dither(pixelX, pixelY));
			// (The sea stops short of its groups' last shades, all but black: WaterDarkestShade.)
			const float blackShare = std::clamp((_colors.ambient - DarkOverflowFrom) / (DarkOverflowTo - DarkOverflowFrom), 0.0f, 1.0f);
			const int darkest = ground->water ? std::max(color & 15, WaterDarkestShade) : 15;
			const bool pastGroup = !ground->water && (color & 15) + shades > 15;
			color = pastGroup ? fadedToBlack((Uint8)(color | 15), blackShare, dither(pixelX + 1, pixelY + 2)) // (by way of the palette's darker colours: see DarkFadeSteps)
				: blackShare > dither(pixelX + 1, pixelY + 2) ? shadeColor(color, shades) : (Uint8)((color & 0xF0) | std::min(darkest, (color & 15) + std::max(0, shades)));
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
 * The sea's height at a spot (see WaveTrains): the trains of waves crossing there, each drifting
 * along its heading, its crests bending to and fro along their length; those whose lines would be
 * finer than the pixels flatten out with the distance. Two look-ups in a sine table and a few
 * multiplications a train: cheap enough for every pixel of sea.
 * @param x World position east, in voxels.
 * @param y World position south, in voxels.
 * @param distance How far away it is seen, in voxels.
 * @return The height, -1 (a trough) .. 1 (a crest).
 */
float FirstPersonView::waveHeight(float x, float y, float distance) const
{
	float height = 0.0f;
	for (int train = 0; train < WaveTrainCount; ++train)
	{
		const WaveTrain &wave = WaveTrains[train];
		if (distance >= wave.fadeTo)
		{
			continue;
		}
		const float fade = distance <= wave.fadeFrom ? 1.0f : (wave.fadeTo - distance) / (wave.fadeTo - wave.fadeFrom);
		const float *steps = _waveSteps[train];
		const float bend = WaveSine[(int)(x * steps[3] + y * steps[4]) & (WaveSineSteps - 1)] * steps[5];
		const int step = (int)(x * steps[0] + y * steps[1] + steps[2] + bend);
		height += WaveSine[step & (WaveSineSteps - 1)] * wave.share * fade;
	}
	return height;
}

/**
 * The sea's colour at a spot of its surface (the mod's water: its floors' tops on the map, and
 * the sea beyond the map's edge), before the light: its own colours (see chooseGround) in waves
 * rolling across it, the same everywhere, so the sea has no seams at the tiles' or the map's
 * edges. A crest is WaveShades lighter than the plain colour, a trough as much darker; foam
 * breaks on the highest crests and the deep colour shows in the troughs, dithered in as more of
 * either shows.
 * @param x World position east, in voxels.
 * @param y World position south, in voxels.
 * @param distance How far away it is seen, in voxels.
 * @param pixelX The pixel (or block of pixels), for the dither.
 * @param pixelY The same, down.
 * @param shade Out: the shades the wave adds to the colour's (0 on a crest .. twice WaveShades in a trough).
 * @return The palette colour.
 */
Uint8 FirstPersonView::waterColor(float x, float y, float distance, int pixelX, int pixelY, float &shade) const
{
	const float height = waveHeight(x, y, distance);
	shade = WaveShades * (1.0f - height);
	const float threshold = dither(pixelX + 3, pixelY + 1);
	if (height > FoamFrom && (height - FoamFrom) / (1.0f - FoamFrom) > threshold)
	{
		return _waterColors[2];
	}
	if (-height > DeepFrom && (-height - DeepFrom) / (1.0f - DeepFrom) > threshold)
	{
		return _waterColors[1];
	}
	return _waterColors[0];
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
 * Is a point indoors, where no sun shines? Under a roof: a floor or an object on any level above
 * its tile (see snapshotTiles), as the game counts a roof when it takes the sun's light away;
 * or under rock (see underRock). Off the map's sides it is out in the open.
 * @param point The point, in voxels (in the air in front of a face, see shadeHit).
 * @return True if it is.
 */
bool FirstPersonView::indoorsAt(const float point[3]) const
{
	if (underRock(point[2]))
	{
		return true;
	}
	const int mapSizeX = _save->getMapSizeX(), mapSizeY = _save->getMapSizeY();
	const int tileX = (int)std::floor(point[0] / Position::TileXY), tileY = (int)std::floor(point[1] / Position::TileXY);
	if (tileX < 0 || tileY < 0 || tileX >= mapSizeX || tileY >= mapSizeY)
	{
		return false;
	}
	const int tileZ = std::clamp((int)std::floor(point[2] / Position::TileZ), 0, _save->getMapSizeZ() - 1);
	return _tileLights[(tileZ * mapSizeY + tileY) * mapSizeX + tileX].roofed;
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
 * under the sea. Under rain or snow clouds (GitHub issue #94) a grey deck of them all over: no
 * sun, moon or stars; a void's dark sky has the same, slow violet wisps. Over Mars a dusty sky,
 * a smaller sun by day and the stars by night, no clouds and no Moon. Out of the air (space,
 * orbit, the Moon) a black sky full of stars (see airlessSkyColor).
 * @param rayDirection The ray's unit direction (z up).
 * @param pixelX The ray's pixel on the view (or block of pixels), for the dither pattern.
 * @param pixelY The same, down.
 * @return The palette colour to draw.
 */
Uint8 FirstPersonView::skyColor(const float rayDirection[3], int pixelX, int pixelY) const
{
	if (airless())
	{
		return airlessSkyColor(rayDirection, pixelX, pixelY);
	}
	// The gradient: how far up the ray points (0 at the horizon and below, 1 straight up),
	// eased so the horizon's colours spread further up, picks one of the 32 sky colours.
	const float elevation = std::max(rayDirection[2], 0.0f);
	const float gradient = std::pow(elevation, 0.6f) * 31.0f;
	if (_colors.overcast || _sky == SkyVoid)
	{
		// A low deck of cloud all over (a void's wisps): its noise where the ray meets it,
		// drifting with the wind, mottles it lighter and darker within the sky's own colours;
		// far off, low down, it melts into the horizon's haze.
		const float layerDistance = 1.0f / std::max(elevation, 0.06f);
		const float drift = _colors.cloudTime * (_sky == SkyVoid ? 0.01f : 0.05f);
		const float deck = cloudNoise(rayDirection[0] * layerDistance * 0.9f + drift, rayDirection[1] * layerDistance * 0.9f + drift * 0.4f);
		const float mottle = (deck - 0.5f) * 2.0f * CloudDeckMottle * smoothstep(0.0f, 0.3f, elevation);
		return _colors.sky[std::clamp((int)(gradient + mottle + dither(pixelX, pixelY)), 0, 31)];
	}
	Uint8 color = _colors.sky[std::clamp((int)(gradient + dither(pixelX, pixelY)), 0, 31)];
	if (_colors.underwater)
	{
		return color;
	}

	// The sun by day and the moon by night: small discs where the ray points almost straight
	// at them (the cosine of the angle very close to 1). From Mars the sun looks smaller, and
	// the Earth's Moon isn't there.
	const float daylight = 1.0f - _colors.night;
	const float sunDot = rayDirection[0] * _colors.sunDir[0] + rayDirection[1] * _colors.sunDir[1] + rayDirection[2] * _colors.sunDir[2];
	if (daylight > 0.4f && sunDot > (_sky == SkyMars ? 0.9998f : 0.9995f))
	{
		return _colors.sun;
	}
	if (_colors.night > 0.4f && _sky != SkyMars)
	{
		const float moonDot = rayDirection[0] * _colors.moonDir[0] + rayDirection[1] * _colors.moonDir[1]
			+ rayDirection[2] * _colors.moonDir[2];
		if (moonDot > 0.9992f)
		{
			return _colors.moon;
		}
	}

	// Clouds on a flat layer high up, thinning out towards the horizon, drifting slowly (none
	// over Mars). The ray meets the layer at a distance of about 1 / elevation (capped low
	// down); the noise there says how thick the cloud is: thin edges get the shadowed colours,
	// thick middles the lit ones.
	if (elevation > 0.02f && _colors.night < 0.9f && _sky != SkyMars)
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

	// Stars at night: the sky divided into small cells (see starCell), and a hash of each cell
	// lights up 6 in 1000 of them.
	if (_colors.night > 0.5f && elevation > 0.03f)
	{
		const Uint32 cellHash = starCell(rayDirection);
		if ((cellHash % 1000) < 6)
		{
			return _colors.star;
		}
	}
	return color;
}

/**
 * The sky out of the air (space, orbit, the Moon: see chooseSky): black, with stars and fainter
 * ones among them, all round in space and in orbit (nothing past a station's or a ship's edge
 * hides them), over the Moon down to its horizon; the sun a bright disc by day, with none of the
 * sky's blue round it. From orbit the Earth fills the view from a little below the horizon down
 * (OrbitEarthEdge): its seas and clouds drifting by as the station circles it, the glow of its
 * air along its edge, its night side dark. Over the Moon the Earth hangs high in the south-east
 * (MoonEarthDirection), lit on the sun's side.
 * @param rayDirection The ray's unit direction (z up).
 * @param pixelX The ray's pixel on the view (or block of pixels), for the dither pattern.
 * @param pixelY The same, down.
 * @return The palette colour to draw.
 */
Uint8 FirstPersonView::airlessSkyColor(const float rayDirection[3], int pixelX, int pixelY) const
{
	const float daylight = 1.0f - _colors.night;
	if (_sky == SkyOrbit && rayDirection[2] < -OrbitEarthEdge + OrbitAirGlow)
	{
		const float below = -rayDirection[2];
		// The glow of the air over the Earth's edge, thinning out upwards (the night side's faint).
		if (below < OrbitEarthEdge)
		{
			const float glow = (below - (OrbitEarthEdge - OrbitAirGlow)) / OrbitAirGlow * (daylight > 0.4f ? 1.0f : 0.4f);
			return glow > dither(pixelX, pixelY) ? _colors.earth[3] : _colors.sky[0];
		}
		if (daylight <= 0.4f)
		{
			return _colors.earth[1];
		}
		// Its face: where the ray meets it, as if it were flat far below, its clouds over its
		// seas, drifting by; near the edge, seen through more air, the glow mixes in.
		const float planeX = rayDirection[0] / below * 1.4f + _colors.cloudTime * 0.012f, planeY = rayDirection[1] / below * 1.4f;
		if (smoothstep(OrbitEarthEdge + 0.08f, OrbitEarthEdge, below) > dither(pixelX + 1, pixelY + 2))
		{
			return _colors.earth[3];
		}
		return cloudNoise(planeX, planeY) > 0.56f ? _colors.earth[2] : _colors.earth[0];
	}
	// The sun by day: a disc where the ray points almost straight at it.
	const float sunDot = rayDirection[0] * _colors.sunDir[0] + rayDirection[1] * _colors.sunDir[1] + rayDirection[2] * _colors.sunDir[2];
	if (daylight > 0.4f && sunDot > 0.9995f)
	{
		return _colors.sun;
	}
	if (_sky == SkyMoon)
	{
		const float along = rayDirection[0] * MoonEarthDirection[0] + rayDirection[1] * MoonEarthDirection[1] + rayDirection[2] * MoonEarthDirection[2];
		if (along > MoonEarthCos)
		{
			// The Earth: the point of its globe the ray meets, from across its disc (in its radii)
			// and the side facing the Moon; lit where it faces the sun, else its night side; the
			// glow of its air round its edge, its clouds over its seas.
			float across[3], normal[3];
			for (int axis = 0; axis < 3; ++axis) across[axis] = (rayDirection[axis] - MoonEarthDirection[axis] * along) / MoonEarthSin;
			const float acrossSquared = across[0] * across[0] + across[1] * across[1] + across[2] * across[2];
			const float facing = std::sqrt(std::max(0.0f, 1.0f - acrossSquared));
			for (int axis = 0; axis < 3; ++axis) normal[axis] = across[axis] - MoonEarthDirection[axis] * facing;
			const float sunlit = normal[0] * _colors.sunDir[0] + normal[1] * _colors.sunDir[1] + normal[2] * _colors.sunDir[2];
			if (sunlit < 0.1f * dither(pixelX, pixelY))
			{
				return _colors.earth[1];
			}
			if (acrossSquared > 0.82f)
			{
				return _colors.earth[3];
			}
			return cloudNoise(across[0] * 3.0f + 7.0f, across[1] * 3.0f + across[2] * 3.0f) > 0.55f ? _colors.earth[2] : _colors.earth[0];
		}
	}
	// The stars: the sky divided into small cells (see starCell), and a hash of each cell lights
	// up 6 in 1000 of them, and 12 more faintly (over the Moon only above its horizon: its ground
	// hides the rest).
	if (_sky != SkyMoon || rayDirection[2] > 0.0f)
	{
		const Uint32 cellHash = starCell(rayDirection) % 1000;
		if (cellHash < 6)
		{
			return _colors.star;
		}
		if (cellHash < 18)
		{
			return _colors.faintStar;
		}
	}
	return _colors.sky[0];
}

/**
 * How much of the soldier's sight the smoke on the line from the eye to a point uses up, by the
 * game's sight rules (see SmokeTrace): walked two voxels at a time up to the point (or as far as
 * the sight reaches, or off the map), its smoke added up tile by tile. An enemy the soldier
 * doesn't see with smoke on the line to it isn't drawn (see drawUnits).
 * @param point The point, in voxels.
 * @return The share of the sight left that the smoke on the line uses up: 1 or more where the
 *         sight runs out on it (the soldier can't see through the smoke there), less where they
 *         can, 0 with no smoke on it.
 */
float FirstPersonView::smokeSightUsedTo(const double point[3]) const
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
	const float Step = 2.0f, reach = std::min(_smokeSightVoxels, length);
	for (float from = 0.0f; from < reach; from += Step)
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
	// In VR an eye's picture is off centre (see renderEyes): its view axis meets it at (eyeAxisX, eyeAxisY), and a unit
	// of the image plane is eyeFocal pixels.
	const bool eyePass = _eyePass;
	const float eyeAxisX = (float)_eyeAxisX, eyeAxisY = (float)_eyeAxisY, eyeFocal = (float)_eyeFocal;
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

	// The parts without shape as opaque as the setting "Ghost opacity" has them: its percent of
	// the pixels up close, of GhostNearOpacity's 50 (the share is scaled by dividing each pixel's
	// dither threshold instead of multiplying the opacity: the same test, see ghostOpacity).
	const float ghostScale = std::clamp(Options::fpsGhostOpacity, 10, 100) / (GhostNearOpacity * 100.0f);
	// The ghosts drawn solid as the map draws them (crops, rubble: see buildRelief, buildCropBlock): on
	// every pixel, or with "See-through crops" on SeeThroughCropOpacity of them.
	const bool seeThroughCrops = Options::fpsSeeThroughCrops;
	rayContext.solidGhostThreshold = 0.0f;

	// With a pixel size above 1, one ray colours a block of pixels (pixelSize x pixelSize).
	// Auto (0) picks 2 for views bigger than AutoPixelMaxWidth x AutoPixelMaxHeight, else 1 (on the graphics card always 1: see
	// gpuCastAllRays); an eye's picture in VR always 1 (its size is a setting of its own, "VR eye picture").
	const bool bigView = !eyePass && (width > AutoPixelMaxWidth || height > AutoPixelMaxHeight);
	// A small view (a soldier's card camera, the map camera) always has a ray a pixel: bigger pixels made mush of it, and it costs little.
	const bool smallView = width * height <= SmallViewPixels;
	const int pixelSize = smallView || _iso ? 1 : Options::fpsPixelSize == 0 ? (bigView ? 2 : 1) : std::clamp(Options::fpsPixelSize, 1, 3);
	const int blockRows = (height + pixelSize - 1) / pixelSize, blockColumns = (width + pixelSize - 1) / pixelSize;
	for (int blockRow = _rayWorkers.nextPiece++; blockRow < blockRows; blockRow = _rayWorkers.nextPiece++)
	{
		// The block's top pixel row, and how far up the image plane its centre is.
		const int topRow = blockRow * pixelSize;
		const float planeV = eyePass ? (eyeAxisY - (topRow + pixelSize * 0.5f)) / eyeFocal : (1.0f - (topRow + pixelSize * 0.5f) * 2.0f / height) * halfPlaneHeight;
		for (int blockColumn = 0; blockColumn < blockColumns; ++blockColumn)
		{
			// The block's left pixel column, and how far right on the image plane its centre is.
			const int leftColumn = blockColumn * pixelSize;
			const float planeU = eyePass ? ((leftColumn + pixelSize * 0.5f) - eyeAxisX) / eyeFocal : ((leftColumn + pixelSize * 0.5f) * 2.0f / width - 1.0f) * halfPlaneWidth;
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
			// (3DIso: this pixel in our 3D art or in the original, by the share of each, dithered: see setIsoVoxelShare)
			const bool voxelHere = _iso && (_isoVoxelShare >= 1.0f || (_isoVoxelShare > 0.0f && dither(leftColumn + 5, topRow + 7) < _isoVoxelShare)); // (the cross-fade: a dithered share of the pixels)
			// 3DIso (see setIsoCamera): the eye is far back, so the ray starts where it enters the map under the view
			// level's top (worked out in double precision), that stretch added to its depth; a ray that misses it shows
			// nothing, as the map's black around it.
			float isoOrigin[3];
			const float *origin = rayOrigin;
			float isoSkipped = 0.0f;
			if (_iso || _flight)
			{
				double direction[3], length = 0.0;
				for (int axis = 0; axis < 3; ++axis) direction[axis] = _forward[axis] + planeU * _right[axis] + planeV * _up[axis];
				direction[2] /= ZScale;
				for (int axis = 0; axis < 3; ++axis) length += direction[axis] * direction[axis];
				length = std::sqrt(length);
				// (the flight's ceiling fading in: on a share of the pixels, no cut, see setFlightCeiling)
				const bool uncut = _flight && !_isoTurned && _flightCeiling > 0.0f && dither(leftColumn + 3, topRow + 1) < _flightCeiling;
				const double softCut = _isoCutSoftness > 0.0f ? -dither(leftColumn + 1, topRow + 2) * _isoCutSoftness : 0.0; // (sliding: see setIsoCut)
				const double high[3] = { rayContext.size[0] * 16.0, rayContext.size[1] * 16.0, uncut ? rayContext.size[2] * 24.0 : (double)_isoTop + softCut };
				const double low[3] = { 0.0, 0.0, _flight && !_isoTurned ? 0.0 : _isoLowLevel * 24.0 };
				double enter = 0.0, leave = 1e30;
				for (int axis = 0; axis < 3; ++axis)
				{
					direction[axis] /= length;
					if (std::abs(direction[axis]) < 1e-12) { if (_eye[axis] < low[axis] || _eye[axis] >= high[axis]) leave = -1.0; continue; }
					double near = (low[axis] - _eye[axis]) / direction[axis], far = (high[axis] - _eye[axis]) / direction[axis];
					if (near > far) std::swap(near, far);
					enter = std::max(enter, near);
					leave = std::min(leave, far);
				}
				if (leave <= enter + 0.02 && (_isoTurned || voxelHere))
				{
					// (3DIso turned, or in our 3D art: past the map, the map's black round it; the whole block)
					for (int pixelY = topRow; pixelY < std::min(height, topRow + pixelSize); ++pixelY)
						for (int pixelX = leftColumn; pixelX < std::min(width, leftColumn + pixelSize); ++pixelX)
						{
							_glowMask[pixelY * width + pixelX] = 0;
							*getRaw(pixelX, pixelY) = 0;
							_depthBuffer[pixelY * width + pixelX] = Infinity;
							if (_iso) { _isoKeys[pixelY * width + pixelX] = 0; _isoMarkDepth[pixelY * width + pixelX] = Infinity; }
						}
					continue;
				}
				if (leave <= enter + 0.02 && _flight)
				{
					// The flight: past the map, the sky above the horizon, the dark below it (the map's black round it).
					float missDirection[3];
					for (int axis = 0; axis < 3; ++axis) missDirection[axis] = (float)direction[axis];
					// (going down: the ground beyond the map, met from a point on the ray above the border's posts)
					Uint8 missColor = 0;
					float missDepth = Infinity;
					const double above = GroundTop + _surfaceLevel * 24.0 + 48.0;
					const double reach = (_eye[2] - above) / -direction[2];
					if (_flightUnknownDark > 0.0f && dither(leftColumn + 2, topRow + 3) < _flightUnknownDark)
					{
						// (round the map black as 3DIso has it, dissolving into the 3D view's ground and sky: see setFlightUnknownDark)
					}
					else if (missDirection[2] >= 0.0f)
					{
						missColor = skyColor(missDirection, leftColumn, topRow);
					}
					else if (reach > 0.0)
					{
						const float from[3] = { (float)(_eye[0] + direction[0] * reach), (float)(_eye[1] + direction[1] * reach), (float)above };
						float groundDepth = Infinity;
						missColor = colorOutsideMap(from, missDirection, 0.0f, 0, leftColumn, topRow, groundDepth);
						missDepth = (float)((reach + groundDepth) / length);
					}
					for (int pixelY = topRow; pixelY < std::min(height, topRow + pixelSize); ++pixelY)
						for (int pixelX = leftColumn; pixelX < std::min(width, leftColumn + pixelSize); ++pixelX)
						{
							_glowMask[pixelY * width + pixelX] = 0;
							*getRaw(pixelX, pixelY) = missColor;
							_depthBuffer[pixelY * width + pixelX] = missDepth;
						}
					continue;
				}
				if (leave <= enter + 0.02)
				{
					// Off the map's box: still the map's painter (a floor's art reaches past the box's front edge), its
					// depth from the far eye.
					Uint8 artColor = 0;
					float artDistance = Infinity;
					int artTile, artPart;
					const float eyeAt[3] = { (float)_eye[0], (float)_eye[1], (float)_eye[2] };
					float artDirection[3];
					for (int axis = 0; axis < 3; ++axis) artDirection[axis] = (float)direction[axis];
					if (!isoArtAlong(eyeAt, artDirection, isoMapX(leftColumn), isoMapY(topRow), artColor, artDistance, artTile, artPart, nullptr, nullptr, nullptr, nullptr, isoSmoothHere(leftColumn, topRow)))
					{
						artColor = 0;
						artDistance = Infinity;
					}
					_glowMask[topRow * width + leftColumn] = 0;
					_isoKeys[topRow * width + leftColumn] = artColor ? isoKeyOf(artTile, artPart) : 0;
					*getRaw(leftColumn, topRow) = artColor;
					_depthBuffer[topRow * width + leftColumn] = artDistance / (float)length;
					_isoMarkDepth[topRow * width + leftColumn] = Infinity; // (nothing to mark out here: the art's floor edge)
					continue;
				}
				enter += 0.01;
				for (int axis = 0; axis < 3; ++axis)
				{
					isoOrigin[axis] = (float)(_eye[axis] + direction[axis] * enter);
					rayDirection[axis] = (float)direction[axis];
				}
				isoOrigin[0] = std::clamp(isoOrigin[0], 0.0f, (float)high[0] - 0.001f);
				isoOrigin[1] = std::clamp(isoOrigin[1], 0.0f, (float)high[1] - 0.001f);
				isoOrigin[2] = std::clamp(isoOrigin[2], (float)low[2], (float)high[2] - 0.001f);
				origin = isoOrigin;
				isoSkipped = (float)enter;
			}
			// Walk the ray, and colour the block by how it ended. The dither patterns go by the
			// block's coordinates, so with big pixels they grow with them.
			Hit hit;
			float tEnd, distanceAlongRay;
			int endAxis;
			Uint8 color;
			rayContext.ghostThreshold = dither(blockColumn, blockRow) / ghostScale; // ghosts show on a share of the blocks (see ghostOpacity)
			if (seeThroughCrops) rayContext.solidGhostThreshold = dither(blockColumn, blockRow) / SeeThroughCropOpacity;
			if (_iso)
			{
				// (3DIso: as the map draws them, ghosts solid and glass opaque; in our 3D art the glass as in 3D)
				rayContext.ghostThreshold = -1.0f;
				rayContext.solidGhostThreshold = 0.0f;
				rayContext.glassStops = !voxelHere;
			}
			// (3DIso's own frames need no walk through the voxels: the map's painter gives the colours, the map's order the
			// overlaps, and the art's place the depth; with the extra "smooth light" the walk finds where a wall's pixel is lit)
			static const bool isoRays = getenv("OXC3D_ISO3D_RAYS") != nullptr;
			// (the original art needs no walk through the voxels: the map's painter gives the colours, the map's order the overlaps,
			// the art's place its depth and where it's lit; only a door swinging is the voxels')
			const RayEnd rayEnd = _iso && !voxelHere && !isoRays && _doorSwings.empty() ? RAY_FAR : castRay(rayContext, origin, rayDirection, hit, tEnd, endAxis);
			ShadeDetail shading;
			// (3DIso, turned or not: under its lowest level shown nothing, as "single layer" has it on the map)
			const bool belowShown = (_iso || _isoTurned) && _isoLowLevel > 0 && rayEnd == RAY_HIT && origin[2] + rayDirection[2] * hit.t < _isoLowLevel * 24.0f - 0.01f;
			switch (belowShown ? RAY_SIDE : rayEnd)
			{
			case RAY_HIT:
				// Something on the map.
				color = shadeHit(hit, origin, rayDirection, blockColumn, blockRow, _iso ? &shading : nullptr);
				distanceAlongRay = hit.t;
				break;
			case RAY_SIDE:
				// Out through the map's side: the border tape, the ground beyond, or the sky.
				if (_iso || _isoTurned || (_flight && _flightUnknownDark > 0.0f && dither(blockColumn + 2, blockRow + 3) < _flightUnknownDark)) { color = 0; distanceAlongRay = Infinity; break; }
				color = colorOutsideMap(origin, rayDirection, tEnd, endAxis, blockColumn, blockRow, distanceAlongRay);
				break;
			case RAY_FAR:
				// Past the view's reach: lost in the haze.
				color = _colors.haze;
				distanceAlongRay = Infinity;
				break;
			case RAY_BOTTOM:
			{
				// Out through the bottom of the map (through a hole blown in the ground floor):
				// the earth below, the ground's colour in deep shade, never a black hole. In
				// space, space (and in orbit the Earth far below).
				if (_sky == SkySpace || _sky == SkyOrbit)
				{
					color = skyColor(rayDirection, blockColumn, blockRow);
					distanceAlongRay = Infinity;
					break;
				}
				const float bottomX = origin[0] + rayDirection[0] * tEnd, bottomY = origin[1] + rayDirection[1] * tEnd;
				const Uint8 earth = _groundModel ? _groundModel->color[0][(int)std::floor(bottomY) & 15][(int)std::floor(bottomX) & 15] : 0;
				color = earth ? shadeColor(earth, std::min(12 - (earth & 15), 6 + (int)dither(blockColumn, blockRow))) : _colors.unexplored;
				distanceAlongRay = tEnd;
				break;
			}
			default:
				// Out through the top: the ground below the map's edge can still be in sight.
				if (_isoTurned || (_flight && _flightUnknownDark > 0.0f && dither(blockColumn + 2, blockRow + 3) < _flightUnknownDark)) { color = 0; distanceAlongRay = Infinity; break; } // (as the shader: 3DIso's black round the map)
				color = colorOutsideMap(origin, rayDirection, tEnd, 2, blockColumn, blockRow, distanceAlongRay);
				break;
			}
			// 3DIso: the colour the map's painter gives the pixel (see isoArtAlong); the voxels' depth where the art is the hit
			// tile's, else the art's tile's middle on the ray; none: nothing (the map's black), nothing in front of the units.
			if (voxelHere)
			{
				// 3DIso in our 3D art: the voxels as the 3D view colours them, ordered as the map orders their tile's part (the
				// map's overlays and the units go by it, see isoKeyOf); a door swinging as its tile's wall.
				const size_t at = (size_t)topRow * width + leftColumn;
				if (rayEnd == RAY_HIT && !belowShown)
				{
					_isoKeys[at] = isoKeyOf(hit.tile, hit.part >= 0 ? hit.part : O_NORTHWALL);
					_isoMarkDepth[at] = (hit.t + isoSkipped) / directionLength;
					// (what the squad hasn't found is black, as on the map: Map::drawTerrain's shade 16)
					if (isoUnfound(hit)) { color = isoShaded(color, 16); shading.valid = false; hit.emissive = false; }
					const bool coloured = _isoLightCount > 0 && shading.valid && !_map->getNightVisionColor();
					if (coloured) color = isoColouredLight(color, shading.raw, shading.point, leftColumn, topRow);
					if ((_isoExtras & IsoExtraTrueColour) && _isoFull.size() == (size_t)width * height && shading.valid && !_map->getNightVisionColor())
					{
						_isoFull[at] = isoFullColour(shading.raw, shading.shade, shading.flare, shading.point, coloured);
						_isoPainted[at] = color;
					}
				}
				else
				{
					color = 0;
					distanceAlongRay = Infinity;
					_isoKeys[at] = 0;
					_isoMarkDepth[at] = Infinity;
				}
			}
			else if (_iso)
			{
				Uint8 artColor;
				float artDistance;
				int artTile, artPart, mapShade;
				Uint8 artRaw;
				float lightDistance;
				float artPoint[3] = { 0.0f, 0.0f, 0.0f };
				const bool artFound = isoArtAlong(origin, rayDirection, isoMapX(leftColumn), isoMapY(topRow), artColor, artDistance, artTile, artPart, &artRaw, &mapShade, &lightDistance, artPoint, isoSmoothHere(leftColumn, topRow));
				// A door swinging (in 3D, see updateDoorSwings): the voxel door where the ray meets it, unless the map would draw
				// the art there after it (in front of it).
				const bool swingHit = rayEnd == RAY_HIT && hit.part < 0 && hit.tile >= 0 && (_tileFlags[hit.tile] & TileSwinging);
				const Uint32 swingKey = swingHit ? isoKeyOf(hit.tile, O_NORTHWALL) : 0;
				if (swingHit && (!artFound || isoKeyOf(artTile, artPart) <= swingKey))
				{
					// (color is already the hit's, see shadeHit)
					distanceAlongRay = hit.t;
					_isoKeys[topRow * width + leftColumn] = swingKey;
					_isoMarkDepth[topRow * width + leftColumn] = (hit.t + isoSkipped) / directionLength;
				}
				else if (artFound)
				{
					color = artColor;
					const bool ownHit = rayEnd == RAY_HIT && artTile == hit.tile && artPart != O_FLOOR;
					if (!ownHit) distanceAlongRay = artDistance;
					_isoKeys[topRow * width + leftColumn] = isoKeyOf(artTile, artPart);
					_isoMarkDepth[topRow * width + leftColumn] = ((ownHit ? hit.t : lightDistance) + isoSkipped) / directionLength; // (the marks' depth: see drawDecals)
					// Extra "smooth light": the game's light where the pixel is in the world (the voxel hit if it's its tile's, else
					// where the ray passes the art's place), a little in front of it, blended between the tiles and dithered;
					// not on the unexplored (black, as on the map) nor under the map's night vision (its own shades).
					const bool smooth = (_isoExtras & IsoExtraSmoothLight) && !_map->getNightVisionColor();
					const bool coloured = _isoLightCount > 0 && !_map->getNightVisionColor();
					const bool trueColour = (_isoExtras & IsoExtraTrueColour) && _isoFull.size() == (size_t)width * height && !_map->getNightVisionColor();
					if ((smooth || !_flares.empty() || coloured) && mapShade < 16)
					{
						// (where the light is taken: the voxel hit if it's the art's own tile's, else the art's own place; a little in front)
						float point[3];
						for (int axis = 0; axis < 3; ++axis) point[axis] = ownHit ? origin[axis] + rayDirection[axis] * (hit.t - 2.0f) : artPoint[axis] - rayDirection[axis] * 2.0f;
						float shade = smooth ? lightAt(point[0], point[1], point[2]) : (float)mapShade;
						// Tactical flares (3DIso's, thrown with the flare key): their light, red, with shadows, as in 3D (shadeHit).
						const float flare = _flares.empty() ? 0.0f : flareLight(point, nullptr, Options::fpsShadows);
						shade -= flare * 10.0f;
						if (smooth || flare > 0.0f)
						{
							color = isoShaded(artRaw, std::clamp((int)(shade + dither(leftColumn, topRow)), 0, 15));
							if (flare * 1.5f > 0.04f + dither(leftColumn + 1, topRow + 3)) color = _colors.flareTint[color];
						}
						// Extra "coloured light": the shots', blasts' and fires' light, in their colours (as the 3D view's in full colour).
						if (coloured) color = isoColouredLight(color, artRaw, point, leftColumn, topRow);
						// Extra "true colour": the same in full colour, the light without the palette's steps (see isoFullColours).
						if (trueColour)
						{
							const size_t at = (size_t)topRow * width + leftColumn;
							_isoFull[at] = isoFullColour(artRaw, shade, flare, point, coloured); // (shade: the light there less the flare's, above)
							_isoPainted[at] = color;
						}
					}
				}
				else
				{
					color = 0;
					distanceAlongRay = Infinity;
					_isoKeys[topRow * width + leftColumn] = 0;
					_isoMarkDepth[topRow * width + leftColumn] = Infinity;
				}
				hit.glass = false;
				hit.emissive = false;
			}
			// Seen through window glass on the way: a little paler and bluer. (3DIso and its flight's first stretch: not the map's black
			// behind it, what the squad hasn't found: an unexplored building's windows were grey squares in the dark)
			if (hit.glass && (!_iso || voxelHere) && !((_iso || _isoTurned || (_flight && _flightUnknownDark > 0.0f)) && (color == 0 || color == 15)))
			{
				color = _colors.glassTint[color];
			}
			// Fill the block: colour, depth, and whether it shines (a UFO's light counts three
			// times in the bloom, for a stronger halo round it).
			Uint8 glowing = (hit.emissive && distanceAlongRay < Infinity) ? 1 : 0;
			if (glowing && hit.part >= 0 && _tileViews[hit.tile].part[hit.part] && _tileViews[hit.tile].part[hit.part]->pulseKind) glowing = 3;
			const float viewDepth = (distanceAlongRay + isoSkipped) / directionLength; // distance along the view axis, for sprite sorting (3DIso: from the far eye)
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
	// The flares' light for this frame, which every ray hitting near a flare needs (the same for
	// every thread).
	prepareFlareLights();
	// 3DIso's coloured lights (extra "coloured light"), the fires nearest the view's middle; and the palette's nearest colours.
	_isoLightCount = 0;
	if (_iso && (_isoExtras & IsoExtraColouredLight) && _map)
	{
		const double from[3] = { _isoMiddle[0], _isoMiddle[1], _isoViewLevel * 24.0 };
		gatherColouredLights(_isoLights, _isoLightCount, 16, from);
		const SDL_Color *palette = getPalette();
		Uint32 sum = 0;
		for (int index = 0; index < 256; ++index) sum = sum * 31u + palette[index].r * 65536u + palette[index].g * 256u + palette[index].b;
		if (_isoNearest.empty() || sum != _isoNearestOf)
		{
			_isoNearestOf = sum;
			_isoNearest.assign(32768, 0);
			for (int cell = 0; cell < 32768; ++cell)
			{
				const int red = (cell >> 10) * 8 + 4, green = ((cell >> 5) & 31) * 8 + 4, blue = (cell & 31) * 8 + 4;
				int best = 1, bestDistance = 1 << 30;
				for (int index = 1; index < 256; ++index)
				{
					const int dr = palette[index].r - red, dg = palette[index].g - green, db = palette[index].b - blue;
					const int distance = dr * dr + dg * dg + db * db; // (as the card's table has it: the two agree)
					if (distance < bestDistance) { bestDistance = distance; best = index; }
				}
				_isoNearest[cell] = (Uint8)best;
			}
		}
	}
	// Setting "Renderer": GPU: the graphics card casts them; where it can't, the CPU as ever.
	// (OPENXCOM3D_GPU_SHADOW: the CPU draws, the card beside it, checked and timed: see gpuShadowRays.)
	static const bool gpuShadow = getenv("OPENXCOM3D_GPU_SHADOW") != nullptr;
	// (OPENXCOM3D_RAY_TIME: the log gets the rays' time, averaged over 120 frames of the main view, whoever cast them.)
	static const bool rayTime = getenv("OPENXCOM3D_RAY_TIME") != nullptr;
	const auto started = std::chrono::steady_clock::now();
	_hasTrueColor = false; // (set again by the card when it gives the frame's picture in full colour too)
	_hasCardPicture = false;
	_gpuPresenting = 0;
	_gpuDepthPending = false; // (a frame the CPU draws has its depth here already: nothing of an earlier frame's is fetched from the card)
	// (3DIso: the original art's painter on the card too, see IsoCardArt; OXC3D_ISO3D_RAYS and _PROBE, the research's checks, on the CPU)
	static const bool isoCpuChecks = getenv("OXC3D_ISO3D_RAYS") != nullptr || (getenv("OXC3D_ISO3D_PROBE") && *getenv("OXC3D_ISO3D_PROBE"));
	const bool onCard = !gpuShadow && Options::fpsRenderer == 1 && (!_iso || _isoVoxelShare >= 1.0f || (_isoCardArt.wanted && !isoCpuChecks)) && gpuCastAllRays();
	if (!onCard)
	{
		runOnAllThreads(&FirstPersonView::renderRows);
	}
	if (rayTime && getWidth() >= 320)
	{
		static double sum = 0.0;
		static int frames = 0;
		sum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		if (++frames == 120)
		{
			Log(LOG_INFO) << "OpenXcom3D: the " << getWidth() << "x" << getHeight() << " view's rays take " << sum / frames << " ms on the " << (onCard ? "GPU" : "CPU");
			sum = 0.0;
			frames = 0;
		}
	}
	if (onCard)
	{
		return;
	}
	if (gpuShadow)
	{
		gpuShadowRays(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
	}
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
