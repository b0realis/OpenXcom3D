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
 * FirstPersonViewUfo.cpp: UFOs that stand out from the Earth around them (the "UFO lights and
 * aura" option, on by default).
 *
 * In the isometric game a UFO's lights are a few pixels of its sprites that change from one
 * animation frame to the next. Seen up close in 3D those frames turned whole panels on and
 * off. With this option every UFO part gets a steady 3D-only model, made once per battle from
 * its original frames: the first frame's shape and colours, with the voxels that light up in
 * any frame marked as lights. The hull then stays still; only the lights glow, pulsing gently
 * in a slow wave around the UFO. Around each UFO its own light colours (picked from its
 * lights) fall on the ground and the walls nearby, under it and inside it, and motes of light
 * drift up around its hull. Stepped curves of the hull are shaded by the direction of the
 * surface around them and keep one colour per voxel, so they read as smooth metal instead of
 * stripes.
 *
 * It costs little: the models and tables are made once per battle (or when the palette
 * changes), the ray walk only reads a different model, and the shading looks up one byte per
 * pixel to know whether a UFO's light reaches it.
 */
#include "FirstPersonView.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <SDL.h>
#include "../Engine/Logger.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/Palette.h"
#include "../Engine/Surface.h"
#include "../Engine/SurfaceSet.h"
#include "../Mod/MapData.h"
#include "../Mod/MapDataSet.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleTerrain.h"
#include "../Mod/RuleUfo.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "FirstPersonParticles.h"
#include "FirstPersonViewShared.h"

namespace OpenXcom
{

namespace
{

/// How far a UFO's pool of light reaches past its footprint, in voxels (7 tiles).
const float UfoLightReach = 112.0f;
/// The most particles the auras may add to: past this many in all, they wait (the pool holds 6000).
const size_t UfoAuraParticleCap = 4500;
/// The fewest tiles a group of UFO tiles needs to count as a UFO (and not as stray debris).
const size_t UfoMinTiles = 4;
/// The outward normals of a voxel's six faces, in the order of the face bits (+x -x +y -y +z -z).
const float FaceNormals[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };

/**
 * How bright a palette colour looks (0..255), weighting red, green and blue as the eye does.
 */
inline float colorLuminance(const SDL_Color &color)
{
	return color.r * 0.299f + color.g * 0.587f + color.b * 0.114f;
}

/**
 * The palette colour closest to an RGB colour that is as bright as a given brightness: a
 * difference in brightness counts four times as much as one in hue, so a few shades of one
 * colour stay a few shades apart however they are tinted (the art's detail kept; a plain
 * search put the Abductor's wall panels' lilacs onto one flat blue). Colour 0 is never picked.
 * @param palette The palette.
 * @param red, green, blue The wanted colour (0..255 per channel).
 * @param luminance The brightness to keep (see colorLuminance).
 */
Uint8 nearestColorKeepingBrightness(const SDL_Color *palette, float red, float green, float blue, float luminance)
{
	int bestIndex = 1;
	float bestDistance = 1e9f;
	for (int index = 1; index < 256; ++index)
	{
		const float dr = palette[index].r - red, dg = palette[index].g - green, db = palette[index].b - blue;
		const float dl = colorLuminance(palette[index]) - luminance;
		const float distance = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f + dl * dl * 4.0f;
		if (distance < bestDistance)
		{
			bestDistance = distance;
			bestIndex = index;
		}
	}
	return (Uint8)bestIndex;
}

/**
 * The palette colour closest to an RGB colour (0..255 per channel), weighting the channels by
 * how bright they look. Colour 0 is transparent, so it is never picked.
 */
Uint8 nearestColor(const SDL_Color *palette, float red, float green, float blue)
{
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
}

/**
 * A fully bright colour of a given hue and saturation, as red, green and blue from 0 to 1.
 * @param hueDegrees The hue, in degrees around the colour wheel (0 red, 120 green, 240 blue).
 * @param saturation 0 grey .. 1 pure colour.
 * @param out Receives red, green and blue.
 */
void hueToRgb(float hueDegrees, float saturation, float out[3])
{
	// The classic HSV to RGB, with the value at full brightness.
	const float hue = std::fmod(std::fmod(hueDegrees, 360.0f) + 360.0f, 360.0f) / 60.0f;
	const int sector = (int)hue % 6;
	const float within = hue - std::floor(hue);
	const float low = 1.0f - saturation, falling = 1.0f - saturation * within, rising = 1.0f - saturation * (1.0f - within);
	const float table[6][3] = { { 1, rising, low }, { falling, 1, low }, { low, 1, rising }, { low, falling, 1 }, { rising, low, 1 }, { 1, low, falling } };
	for (int channel = 0; channel < 3; ++channel) out[channel] = table[sector][channel];
}

}

/**
 * Lists the map data sets UFOs are built from, once per battle (before the voxel models, which
 * treat UFO parts a little differently, see buildModel): the sets of every UFO's battlescape
 * terrain, minus the ones nearly every terrain uses (BLANKS and the like). Their parts get
 * steady models, wherever they stand (an alien base built from UFO walls too). Of those, the
 * ones no ordinary terrain uses at all (a UFO's hull) tell a UFO from an alien base.
 */
void FirstPersonView::collectUfoDataSets()
{
	_ufoDataSets.clear();
	_ufoOnlyDataSets.clear();
	const Mod *mod = _save ? _save->getMod() : nullptr;
	if (!mod)
	{
		return;
	}
	std::map<std::string, int> terrainsUsing; // how many ordinary terrains use each set
	auto collect = [](RuleTerrain *terrain, std::set<std::string> &into)
	{
		if (!terrain) return;
		for (const MapDataSet *dataSet : *terrain->getMapDataSets()) into.insert(dataSet->getName());
	};
	for (const auto &type : mod->getUfosList()) collect(mod->getUfo(type)->getBattlescapeTerrainData(), _ufoDataSets);
	const int terrainCount = (int)mod->getTerrainList().size();
	for (const auto &type : mod->getTerrainList())
	{
		std::set<std::string> terrainSets;
		collect(mod->getTerrain(type), terrainSets);
		for (const auto &setName : terrainSets) terrainsUsing[setName]++;
	}
	for (auto setName = _ufoDataSets.begin(); setName != _ufoDataSets.end();)
	{
		const int users = terrainsUsing.count(*setName) ? terrainsUsing[*setName] : 0;
		if (!users) _ufoOnlyDataSets.insert(*setName);
		if (users * 2 > terrainCount) setName = _ufoDataSets.erase(setName); // (nearly everywhere)
		else ++setName;
	}
}

/**
 * Finds the UFOs on the map, once per battle (after the voxel models and the mirrors): the map
 * data sets UFOs are built from, a steady model for every part of them (see buildUfoModel;
 * only by the view that owns the voxel models), the UFOs themselves as groups of connected
 * UFO tiles (their footprint, height, grav lifts and the pulse phase of each tile, by its angle
 * around the UFO), which tiles each UFO's light reaches and how brightly. Their light colours
 * are picked later, once the palette is known (see chooseUfoColors).
 */
void FirstPersonView::findUfos()
{
	// Start afresh: no UFOs, and (for the view that owns the models) no steady models.
	_ufos.clear();
	_ufoOfTile.clear();
	_ufoPhaseOfTile.clear();
	_ufoGlowOfTile.clear();
	_ufoPaletteHash = 0;
	if (!_sharedModelsOwner)
	{
		_ufoModels.clear();
	}
	const Mod *mod = _save ? _save->getMod() : nullptr;
	if (!mod)
	{
		return;
	}
	const Uint32 startTick = SDL_GetTicks();

	// The map data sets UFOs are built from (see collectUfoDataSets).
	const std::set<std::string> &ufoSets = _ufoDataSets, &exclusiveSets = _ufoOnlyDataSets;
	if (ufoSets.empty())
	{
		return;
	}

	// Which tiles belong to a UFO: any of their parts comes from a UFO's data set (1), from a
	// set only UFOs use (2).
	const int mapSizeX = _save->getMapSizeX(), mapSizeY = _save->getMapSizeY(), mapSizeZ = _save->getMapSizeZ(),
		tileCount = mapSizeX * mapSizeY * mapSizeZ;
	std::vector<char> isUfoTile(tileCount, 0);
	for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex)
	{
		const Tile *tile = _save->getTile(tileIndex);
		for (int part = 0; part < 4; ++part)
		{
			const MapData *mapData = tile->getMapData((TilePart)part);
			if (!mapData || !mapData->getDataset() || !ufoSets.count(mapData->getDataset()->getName())) continue;
			isUfoTile[tileIndex] = std::max<char>(isUfoTile[tileIndex], exclusiveSets.count(mapData->getDataset()->getName()) ? 2 : 1);
		}
	}

	// Group them into UFOs: each UFO tile not yet in a UFO starts a new one, which takes in its
	// connected UFO tiles (touching in any of the 26 directions). At most 250 UFOs (the tile
	// marks are bytes).
	_ufoOfTile.assign(tileCount, 0);
	_ufoPhaseOfTile.assign(tileCount, 0);
	_ufoGlowOfTile.assign(tileCount, 0);
	std::vector<float> columnDistance(mapSizeX * mapSizeY, Infinity); // per column (x, y): voxels to the nearest UFO column's edge, 0 under a UFO
	std::vector<char> grouped(tileCount, 0);
	std::vector<int> toVisit;
	for (int seed = 0; seed < tileCount && _ufos.size() < 250; ++seed)
	{
		if (!isUfoTile[seed] || grouped[seed]) continue;

		// Flood fill from the seed, keeping the box of the tiles (in tiles: x, y, level) and
		// whether any of them is built from a set only UFOs use.
		UfoSpot ufo;
		int low[3] = { mapSizeX, mapSizeY, mapSizeZ }, high[3] = { -1, -1, -1 };
		bool hull = false;
		toVisit.assign(1, seed);
		grouped[seed] = 1;
		while (!toVisit.empty())
		{
			const int tileIndex = toVisit.back();
			toVisit.pop_back();
			ufo.tiles.push_back(tileIndex);
			hull |= isUfoTile[tileIndex] == 2;
			const int cell[3] = { tileIndex % mapSizeX, (tileIndex / mapSizeX) % mapSizeY, tileIndex / (mapSizeX * mapSizeY) };
			for (int axis = 0; axis < 3; ++axis)
			{
				low[axis] = std::min(low[axis], cell[axis]);
				high[axis] = std::max(high[axis], cell[axis]);
			}
			// Queue the neighbouring UFO tiles not grouped yet.
			for (int dz = -1; dz <= 1; ++dz)
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
					{
						const int x = cell[0] + dx, y = cell[1] + dy, z = cell[2] + dz;
						if (x < 0 || y < 0 || z < 0 || x >= mapSizeX || y >= mapSizeY || z >= mapSizeZ) continue;
						const int neighbour = (z * mapSizeY + y) * mapSizeX + x;
						if (isUfoTile[neighbour] && !grouped[neighbour])
						{
							grouped[neighbour] = 1;
							toVisit.push_back(neighbour);
						}
					}
		}
		// Too small to be a UFO (stray debris), or without a UFO's hull (an alien base).
		if (ufo.tiles.size() < UfoMinTiles || !hull) continue;

		// Its footprint (the middle and half the size of its box, in voxels) and its height.
		ufo.centre[0] = (low[0] + high[0] + 1) * 8.0f;
		ufo.centre[1] = (low[1] + high[1] + 1) * 8.0f;
		ufo.half[0] = (high[0] - low[0] + 1) * 8.0f;
		ufo.half[1] = (high[1] - low[1] + 1) * 8.0f;
		ufo.bottom = low[2] * 24.0f;
		ufo.top = (high[2] + 1) * 24.0f;
		ufo.seen = false;
		ufo.colorsChosen = false;

		// Its grav lifts (floors the game lets units float up through), for their beams of light:
		// each beam starts on the first floor below the lift (the ground under a landed UFO).
		for (const int tileIndex : ufo.tiles)
		{
			const MapData *floor = _save->getTile(tileIndex)->getMapData(O_FLOOR);
			if (!floor || !floor->isGravLift()) continue;
			const int liftLevel = tileIndex / (mapSizeX * mapSizeY), column = tileIndex % (mapSizeX * mapSizeY);
			int bottomLevel = liftLevel;
			for (int level = liftLevel - 1; level >= 0; --level)
			{
				const MapData *below = _save->getTile(level * mapSizeX * mapSizeY + column)->getMapData(O_FLOOR);
				bottomLevel = level;
				if (below && !below->isGravLift()) break;
			}
			ufo.lifts.push_back(tileIndex);
			ufo.liftBottoms.push_back(bottomLevel * 24.0f + GroundTop);
		}

		// Each of its tiles pulses with the phase of its angle around the UFO (16 steps), so
		// its lights pulse in a wave running round the hull; it is marked as a UFO tile, and as
		// part of the outer skin when a tile beside it on its level is not the UFO's.
		for (const int tileIndex : ufo.tiles)
		{
			const int x = tileIndex % mapSizeX, y = (tileIndex / mapSizeX) % mapSizeY, z = tileIndex / (mapSizeX * mapSizeY);
			const float dx = x * 16.0f + 8.0f - ufo.centre[0], dy = y * 16.0f + 8.0f - ufo.centre[1];
			const float turn = std::atan2(dy, dx) / (2.0f * (float)Pi) + 1.0f; // 0.5 .. 1.5 turns
			bool skin = false;
			for (int side = 0; side < 4; ++side)
			{
				const int sideX = x + (side == 0) - (side == 1), sideY = y + (side == 2) - (side == 3);
				skin |= sideX < 0 || sideY < 0 || sideX >= mapSizeX || sideY >= mapSizeY || !isUfoTile[(z * mapSizeY + sideY) * mapSizeX + sideX];
			}
			_ufoPhaseOfTile[tileIndex] = (Uint8)(((int)(turn * 16.0f) & 15) | UfoTileBit | (skin ? UfoSkinBit : 0));
		}

		// How far each column of the map around it is from its nearest column, in voxels from
		// that column's edge (0 under the UFO): its pool of light hugs its shape (see below).
		for (const int tileIndex : ufo.tiles)
		{
			const int x = tileIndex % mapSizeX, y = (tileIndex / mapSizeX) % mapSizeY;
			const int reach = (int)std::ceil(UfoLightReach / 16.0f) + 1;
			for (int columnY = std::max(0, y - reach); columnY <= std::min(mapSizeY - 1, y + reach); ++columnY)
				for (int columnX = std::max(0, x - reach); columnX <= std::min(mapSizeX - 1, x + reach); ++columnX)
				{
					const float distance = std::max(0.0f, std::sqrt((float)((columnX - x) * (columnX - x) + (columnY - y) * (columnY - y))) * 16.0f - 8.0f);
					float &field = columnDistance[columnY * mapSizeX + columnX];
					field = std::min(field, distance);
				}
		}

		// The tiles its light reaches: around its box as far as the light goes, from the ground
		// up to a level above its roof. Where two UFOs' lights meet, the first found keeps it.
		const int reachTiles = (int)std::ceil(UfoLightReach / 16.0f) + 1;
		for (int z = 0; z <= std::min(mapSizeZ - 1, high[2] + 1); ++z)
			for (int y = std::max(0, low[1] - reachTiles); y <= std::min(mapSizeY - 1, high[1] + reachTiles); ++y)
				for (int x = std::max(0, low[0] - reachTiles); x <= std::min(mapSizeX - 1, high[0] + reachTiles); ++x)
				{
					Uint8 &mark = _ufoOfTile[(z * mapSizeY + y) * mapSizeX + x];
					if (!mark) mark = (Uint8)(_ufos.size() + 1);
				}
		_ufos.push_back(std::move(ufo));
	}
	// Every part of those sets that the map uses gets its steady model, kept with its frames
	// (UFO doors keep their own models: they slide open frame by frame). A part used on a
	// UFO's skin knows which way is out (the average way from the UFO's middle to the tiles it
	// stands on), so only its outer side is trimmed to its sprite (see buildUfoModel).
	int modelCount = 0;
	if (!_sharedModelsOwner)
	{
		std::unordered_map<const MapData*, std::array<float, 3>> outwards; // per part: the ways out added up (x, y), and how many
		for (const UfoSpot &ufo : _ufos)
		{
			for (const int tileIndex : ufo.tiles)
			{
				if (!(_ufoPhaseOfTile[tileIndex] & UfoSkinBit)) continue;
				const float dx = (tileIndex % mapSizeX) * 16.0f + 8.0f - ufo.centre[0], dy = ((tileIndex / mapSizeX) % mapSizeY) * 16.0f + 8.0f - ufo.centre[1];
				const float length = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
				for (int part = 0; part < 4; ++part)
				{
					const MapData *mapData = _save->getTile(tileIndex)->getMapData((TilePart)part);
					if (!mapData) continue;
					std::array<float, 3> &way = outwards[mapData]; // (zeroed the first time)
					way[0] += dx / length;
					way[1] += dy / length;
					way[2] += 1.0f;
				}
			}
		}
		for (MapDataSet *dataSet : *_save->getMapDataSets())
		{
			if (!ufoSets.count(dataSet->getName())) continue;
			for (size_t record = 0; record < dataSet->getSize(); ++record)
			{
				const MapData *mapData = dataSet->getObject(record);
				if (mapData->isUFODoor()) continue;
				// Which way is out for it: only if its uses agree (their average at least 0.7 long).
				float outward[2] = { 0.0f, 0.0f };
				auto way = outwards.find(mapData);
				if (way != outwards.end())
				{
					const float averageX = way->second[0] / way->second[2], averageY = way->second[1] / way->second[2];
					const float length = std::sqrt(averageX * averageX + averageY * averageY);
					if (length >= 0.7f)
					{
						outward[0] = averageX / length;
						outward[1] = averageY / length;
					}
				}
				getFrames(mapData); // (makes sure its frames are known)
				UfoModel &ufoModel = _ufoModels[mapData];
				buildUfoModel(mapData, outward, ufoModel);
				if (!ufoModel.model.empty)
				{
					_frames[mapData].steady = &ufoModel.model;
					++modelCount;
				}
			}
		}
	}

	// Each UFO's light, tile by tile (0..255 at the tile's middle; the shading blends it between
	// tiles like the game's light, see lightAt, and samples it in the air in front of each
	// face). Around a UFO it is a pool: fading with the distance from the UFO's nearest column
	// over seven tiles (closeness to the power 1.5), and with height, gone near the top of the
	// UFO. So the ground around it, the walls facing it, its underside over open ground (whose
	// air is outside the UFO) and its own outer skin (whose faces look out into that air, and
	// whose own tiles get some of it: a rim light) catch it. Inside the UFO it is a soft light
	// of its own, strongest low down.
	for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex)
	{
		if (!_ufoOfTile[tileIndex]) continue;
		const UfoSpot &ufo = _ufos[_ufoOfTile[tileIndex] - 1];
		const float distance = std::min(columnDistance[tileIndex % (mapSizeX * mapSizeY)], UfoLightReach);
		const float height = (tileIndex / (mapSizeX * mapSizeY)) * 24.0f + 12.0f - ufo.bottom, ufoHeight = ufo.top - ufo.bottom;
		const float closeness = 1.0f - distance / UfoLightReach;
		const float pool = closeness * std::sqrt(closeness) * (1.0f - smoothstep(ufoHeight * 0.35f, ufoHeight * 0.9f, height));
		float glow = pool;
		if (_ufoPhaseOfTile[tileIndex] & UfoTileBit)
		{
			// A tile of the UFO: its soft inner light, more on its outer skin (the rim).
			glow = 0.12f * (1.0f - smoothstep(ufoHeight * 0.2f, ufoHeight, height));
			if (_ufoPhaseOfTile[tileIndex] & UfoSkinBit) glow = std::max(glow, pool * 0.6f);
		}
		_ufoGlowOfTile[tileIndex] = (Uint8)std::clamp((int)(glow * 255.0f + 0.5f), 0, 255);
	}
	if (!_ufos.empty())
	{
		size_t liftCount = 0;
		for (const UfoSpot &ufo : _ufos) liftCount += ufo.lifts.size();
		Log(LOG_INFO) << "OpenXcom3D: " << _ufos.size() << " UFO(s) with " << liftCount << " grav lift tile(s), " << modelCount << " steady UFO models in "
			<< SDL_GetTicks() - startTick << " ms";
	}
}

/**
 * Builds a UFO part's steady 3D-only model from its original frames.
 *
 * The shape and colours are its first frame's. A tall part (not a floor or roof plate) on a
 * UFO's skin is trimmed to its sprite's outline on its outer side: its voxel shapes (LOFTs)
 * are coarse blocks, so the small scout's round hull came out as a box; the voxels that the
 * sprite leaves transparent go (unless that would take most of the part), but only on the
 * side away from the UFO's middle, so the walls stay plain from inside. Only the look
 * changes: the game's line of fire still uses its voxel shapes.
 *
 * A voxel whose colour changes from frame to frame by more than a shade or two of brightness
 * (or to another colour) is one of the part's lights: the lit panels, lift pads, screens and
 * navigation lights the isometric game animates. So is the bright top of a part the game
 * counts as a light source. Only the voxels the art really shows count (the front one on each
 * line of voxels that lands on the same sprite pixel; the ones behind it only take its colour).
 * Lights take their brightest colour of all the frames, and shine. The sides the art never
 * shows take their colours from the hull only (not from the lights, which would smear across
 * them). Each voxel's open faces are kept for smooth shading (see shadeHit); a face on the
 * tile's edge counts as closed (the next tile goes on there).
 * @param mapData The UFO part's record.
 * @param outward Which way is out of the UFO for this part (x, y, a unit vector), or (0, 0) if
 *        that isn't clear: then it isn't trimmed.
 * @param ufoModel Where to build it.
 */
void FirstPersonView::buildUfoModel(const MapData *mapData, const float outward[2], UfoModel &ufoModel)
{
	// Start from the first frame's model (trimmed to the sprite's outline if it is a tall part
	// on a UFO's skin and keeps at least 40% of its voxels that way), with no lights and no
	// open faces yet.
	VoxelModel &model = ufoModel.model;
	const Frames &frames = getFrames(mapData);
	const VoxelModel *firstFrame = getModel(mapData, frames.sprite[0], false);
	const VoxelModel *untrimmed = firstFrame;
	if (!firstFrame->empty && firstFrame->max[2] > 3 && (outward[0] != 0.0f || outward[1] != 0.0f))
	{
		const VoxelModel *trimmed = getModel(mapData, frames.sprite[0], true);
		auto solidVoxels = [](const VoxelModel &countedModel)
		{
			int count = 0;
			for (int z = 0; z < 24; ++z)
				for (int y = 0; y < 16; ++y)
					for (int x = 0; x < 16; ++x) count += countedModel.color[z][y][x] ? 1 : 0;
			return count;
		};
		if (!trimmed->empty && solidVoxels(*trimmed) * 10 >= solidVoxels(*firstFrame) * 4) firstFrame = trimmed;
	}
	model = *firstFrame;
	if (firstFrame != untrimmed)
	{
		// Keep the inner side whole: the trimming cuts along the lines the isometric camera
		// looks down, which run on into the UFO, so a voxel it took away on the side of the part
		// towards the UFO's middle (behind the middle of its solid voxels, seen from outside)
		// comes back, and the walls stay plain from inside.
		float middle[2] = { 0.0f, 0.0f };
		int solidCount = 0;
		for (int z = 0; z < 24; ++z)
			for (int y = 0; y < 16; ++y)
				for (int x = 0; x < 16; ++x)
				{
					if (!untrimmed->color[z][y][x]) continue;
					middle[0] += x + 0.5f;
					middle[1] += y + 0.5f;
					++solidCount;
				}
		middle[0] /= std::max(1, solidCount);
		middle[1] /= std::max(1, solidCount);
		for (int z = 0; z < 24; ++z)
			for (int y = 0; y < 16; ++y)
				for (int x = 0; x < 16; ++x)
				{
					if (model.color[z][y][x] || !untrimmed->color[z][y][x]) continue;
					if ((x + 0.5f - middle[0]) * outward[0] + (y + 0.5f - middle[1]) * outward[1] <= 0.0f) model.color[z][y][x] = untrimmed->color[z][y][x];
				}
		// Fill the pits the trimming left in the hull: a voxel taken away that still has at
		// least four of its six neighbours comes back, in its untrimmed colour (twice over,
		// for pits two voxels deep). Then the box around the solid voxels again.
		std::vector<int> refill;
		for (int pass = 0; pass < 2; ++pass)
		{
			refill.clear();
			for (int z = 0; z < 24; ++z)
				for (int y = 0; y < 16; ++y)
					for (int x = 0; x < 16; ++x)
					{
						if (model.color[z][y][x] || !untrimmed->color[z][y][x]) continue;
						int neighbours = 0;
						for (int face = 0; face < 6; ++face)
						{
							const int nx = x + (int)FaceNormals[face][0], ny = y + (int)FaceNormals[face][1], nz = z + (int)FaceNormals[face][2];
							if (nx >= 0 && ny >= 0 && nz >= 0 && nx < 16 && ny < 16 && nz < 24 && model.color[nz][ny][nx]) ++neighbours;
						}
						if (neighbours >= 4) refill.push_back((z * 16 + y) * 16 + x);
					}
			for (const int voxel : refill)
			{
				const int x = voxel % 16, y = (voxel / 16) % 16, z = voxel / 256;
				model.color[z][y][x] = untrimmed->color[z][y][x];
			}
		}
		for (int axis = 0; axis < 3; ++axis)
		{
			model.min[axis] = 255;
			model.max[axis] = 0;
		}
		for (int z = 0; z < 24; ++z)
			for (int y = 0; y < 16; ++y)
				for (int x = 0; x < 16; ++x)
				{
					if (!model.color[z][y][x]) continue;
					const int voxel[3] = { x, y, z };
					for (int axis = 0; axis < 3; ++axis)
					{
						model.min[axis] = std::min<Uint8>(model.min[axis], (Uint8)voxel[axis]);
						model.max[axis] = std::max<Uint8>(model.max[axis], (Uint8)voxel[axis]);
					}
				}
	}
	std::fill(&ufoModel.lit[0][0], &ufoModel.lit[0][0] + 24 * 16, (Uint16)0);
	std::fill(&ufoModel.exposed[0][0][0], &ufoModel.exposed[0][0][0] + 24 * 16 * 16, (Uint8)0);
	model.lit = ufoModel.lit;
	model.exposed = ufoModel.exposed;
	model.pulseKind = 0;
	if (model.empty)
	{
		return;
	}

	// The palette, to tell bright from dark: the view's (the battlescape's), or the mod's
	// battlescape palette if the view has none yet (or only black).
	const SDL_Color *palette = getPalette();
	int paletteBrightness = 0;
	for (int index = 0; palette && index < 256; ++index) paletteBrightness += palette[index].r + palette[index].g + palette[index].b;
	if (!paletteBrightness)
	{
		const Palette *modPalette = _game->getMod()->getPalette("PAL_BATTLESCAPE", false);
		palette = modPalette ? modPalette->getColors() : nullptr;
	}

	// The part's other frames (each different sprite once).
	std::vector<const VoxelModel*> otherFrames;
	std::vector<int> spritesTaken(1, frames.sprite[0]);
	for (int frame = 1; frame < 8; ++frame)
	{
		if (std::find(spritesTaken.begin(), spritesTaken.end(), frames.sprite[frame]) != spritesTaken.end()) continue;
		spritesTaken.push_back(frames.sprite[frame]);
		otherFrames.push_back(getModel(mapData, frames.sprite[frame], false));
	}

	// Find the lights, voxel by voxel, and count them against the solid voxels.
	const bool lightSource = mapData->getLightSource() > 0;
	int solidCount = 0, litCount = 0;
	for (int z = 0; z < 24 && palette; ++z)
		for (int y = 0; y < 16; ++y)
			for (int x = 0; x < 16; ++x)
			{
				const Uint8 color = model.color[z][y][x];
				if (!color) continue;
				++solidCount;
				// Its colour in every frame: the brightest one, and how much the brightness varies.
				Uint8 brightest = color;
				float lowest = colorLuminance(palette[color]), highest = lowest;
				bool otherColor = false;
				for (const VoxelModel *frameModel : otherFrames)
				{
					const Uint8 frameColor = frameModel->color[z][y][x];
					if (!frameColor) continue;
					const float luminance = colorLuminance(palette[frameColor]);
					if (luminance > highest) { highest = luminance; brightest = frameColor; }
					lowest = std::min(lowest, luminance);
					otherColor |= (frameColor >> 4) != (color >> 4);
				}
				// A light: it blinks between frames, or it is the bright top of a light source; and
				// it is the voxel the art shows (nothing solid in front of it, towards the
				// isometric camera: one voxel east, south and up at a time).
				const bool blinks = highest - lowest >= 16.0f || (otherColor && highest - lowest >= 6.0f);
				const bool bulb = lightSource && (color & 15) <= 3 && z >= firstFrame->glowMinZ;
				bool inFront = true;
				for (int step = 1; inFront && x + step < 16 && y + step < 16 && z + step < 24; ++step)
				{
					inFront = !model.color[z + step][y + step][x + step];
				}
				if ((blinks || bulb) && inFront)
				{
					ufoModel.lit[z][y] |= (Uint16)(1 << x);
					model.color[z][y][x] = brightest;
					++litCount;
				}
			}
	// How its lights pulse: a few small lights (1), or a big lit panel such as a lift pad, when
	// more than 30% of the part lights up (2: slower and fainter).
	model.pulseKind = litCount == 0 ? 0 : (litCount * 10 > solidCount * 3 ? 2 : 1);

	// The sides the art never shows show the hull, not its lights: what each column looks like
	// from above, and each row from the east and from the south, skipping the lights (a column
	// or row that is all lights keeps what it had).
	auto isLit = [&ufoModel](int x, int y, int z) { return ((ufoModel.lit[z][y] >> x) & 1) != 0; };
	for (int y = 0; y < 16; ++y)
		for (int x = 0; x < 16; ++x)
			for (int z = 23; z >= 0; --z)
			{
				if (model.color[z][y][x] && !isLit(x, y, z)) { model.plan[y][x] = model.color[z][y][x]; break; }
			}
	for (int z = 0; z < 24; ++z)
		for (int row = 0; row < 16; ++row)
		{
			for (int x = 15; x >= 0; --x)
			{
				if (model.color[z][row][x] && !isLit(x, row, z)) { model.east[z][row] = model.color[z][row][x]; break; }
			}
			for (int y = 15; y >= 0; --y)
			{
				if (model.color[z][y][row] && !isLit(row, y, z)) { model.south[z][row] = model.color[z][y][row]; break; }
			}
		}

	// Each voxel's open faces: a face is open when the voxel next to it, inside the tile, is
	// empty (past the tile's edge the next tile carries on, so that face counts as closed).
	for (int z = 0; z < 24; ++z)
		for (int y = 0; y < 16; ++y)
			for (int x = 0; x < 16; ++x)
			{
				if (!model.color[z][y][x]) continue;
				Uint8 openFaces = 0;
				for (int face = 0; face < 6; ++face)
				{
					const int nx = x + (int)FaceNormals[face][0], ny = y + (int)FaceNormals[face][1], nz = z + (int)FaceNormals[face][2];
					if (nx < 0 || ny < 0 || nz < 0 || nx > 15 || ny > 15 || nz > 23) continue;
					if (!model.color[nz][ny][nx]) openFaces |= (Uint8)(1 << face);
				}
				ufoModel.exposed[z][y][x] = openFaces;
			}
}

/**
 * Picks a UFO's light colours from its own colours, and makes its tint tables. The inner light,
 * under the UFO and close around it, is the colour of its hull (the lilac of X-COM's UFOs),
 * made vivid; the outer light, further out, is the colour its lights show most (green screens,
 * orange power, blue navigation lights), for a two-tone glow that no Earth light has. Colours
 * are judged by hue, each voxel weighted by how colourful it is (greys count for nothing).
 * When the two are too close, or a UFO has no colourful lights, the outer light is the hue
 * next door on the colour wheel; a UFO with a grey hull takes its lights' colour inside too,
 * and one with neither glows violet. Each tint table maps every palette colour to the light's
 * colour at the same brightness. Also picks the aura's mote colours.
 * @param ufo The UFO.
 */
void FirstPersonView::chooseUfoColors(UfoSpot &ufo)
{
	const SDL_Color *palette = getPalette();
	ufo.colorsChosen = true;
	if (!palette)
	{
		return;
	}

	// How often each part's steady model is used in the UFO.
	std::unordered_map<const VoxelModel*, int> modelUses;
	for (const int tileIndex : ufo.tiles)
	{
		const Tile *tile = _save->getTile(tileIndex);
		for (int part = 0; part < 4; ++part)
		{
			const MapData *mapData = tile->getMapData((TilePart)part);
			const VoxelModel *steady = mapData ? getFrames(mapData).steady : nullptr;
			if (steady) modelUses[steady]++;
		}
	}
	// How often each colour shows on its hull and in its lights.
	int hullCount[256] = {}, lightCount[256] = {};
	for (const auto &entry : modelUses)
	{
		const VoxelModel &model = *entry.first;
		for (int z = 0; z < 24; ++z)
			for (int y = 0; y < 16; ++y)
				for (int x = 0; x < 16; ++x)
				{
					const Uint8 color = model.color[z][y][x];
					if (!color) continue;
					if ((model.lit[z][y] >> x) & 1) lightCount[color] += entry.second;
					else hullCount[color] += entry.second;
				}
	}

	// The main hue of a colour count: a histogram of hues in twelve 30-degree bins, each colour
	// weighted by its count and its saturation (greys and near-blacks count for nothing); the
	// fullest bin's average hue, and its weight against all the colours counted (0 grey .. 1).
	auto mainHue = [palette](const int *counts, float &hue, float &colourfulness)
	{
		float binWeight[12] = {}, binHue[12] = {}, total = 0.0f;
		for (int index = 1; index < 256; ++index)
		{
			if (!counts[index]) continue;
			total += counts[index];
			const float red = palette[index].r / 255.0f, green = palette[index].g / 255.0f, blue = palette[index].b / 255.0f;
			const float maximum = std::max(red, std::max(green, blue)), minimum = std::min(red, std::min(green, blue));
			const float chroma = maximum - minimum;
			if (maximum < 0.15f || chroma < 0.08f) continue;
			float colorHue;
			if (maximum == red) colorHue = 60.0f * std::fmod((green - blue) / chroma + 6.0f, 6.0f);
			else if (maximum == green) colorHue = 60.0f * ((blue - red) / chroma + 2.0f);
			else colorHue = 60.0f * ((red - green) / chroma + 4.0f);
			const int bin = std::clamp((int)(colorHue / 30.0f), 0, 11);
			const float weight = counts[index] * (chroma / maximum);
			binWeight[bin] += weight;
			binHue[bin] += colorHue * weight;
		}
		const int bestBin = (int)(std::max_element(binWeight, binWeight + 12) - binWeight);
		hue = binWeight[bestBin] > 0.0f ? binHue[bestBin] / binWeight[bestBin] : -1.0f;
		colourfulness = total > 0.0f ? binWeight[bestBin] / total : 0.0f;
	};
	float hullHue, hullColourfulness, lightHue, lightColourfulness;
	mainHue(hullCount, hullHue, hullColourfulness);
	mainHue(lightCount, lightHue, lightColourfulness);

	// Inner: the hull's hue if it has one (at least a little colourful), else the lights', else
	// violet. Outer: the lights' hue if it is 40 degrees or more away; else the hue next door,
	// 60 degrees back (violet gives azure, orange gives magenta), or forward if that would land
	// on the yellows and oranges of fire (green gives cyan).
	const float innerHue = hullHue >= 0.0f && hullColourfulness > 0.05f ? hullHue : lightHue >= 0.0f ? lightHue : 280.0f;
	float outerHue = std::fmod(innerHue + 300.0f, 360.0f);
	const float hueGap = lightHue >= 0.0f ? std::abs(std::remainder(lightHue - innerHue, 360.0f)) : 0.0f;
	if (lightHue >= 0.0f && hueGap >= 40.0f) outerHue = lightHue;
	else if (outerHue >= 30.0f && outerHue <= 90.0f) outerHue = innerHue + 60.0f;
	Log(LOG_INFO) << "OpenXcom3D: UFO of " << ufo.tiles.size() << " tiles glows with hues " << (int)innerHue << " and " << (int)std::fmod(outerHue, 360.0f);

	// The tint tables: each colour, by its brightness, in the inner and the outer light's hue,
	// fully saturated (a little of its own brightness kept, so the light doesn't flatten what
	// it falls on).
	// Colour 0 stays as it is.
	float hues[2][3];
	hueToRgb(innerHue, 1.0f, hues[0]);
	hueToRgb(outerHue, 1.0f, hues[1]);
	for (int light = 0; light < 2; ++light)
	{
		ufo.tint[light][0] = 0;
		for (int index = 1; index < 256; ++index)
		{
			const float luminance = colorLuminance(palette[index]);
			ufo.tint[light][index] = nearestColor(palette, std::min(255.0f, hues[light][0] * (70.0f + luminance * 1.15f) + luminance * 0.08f),
				std::min(255.0f, hues[light][1] * (70.0f + luminance * 1.15f) + luminance * 0.08f),
				std::min(255.0f, hues[light][2] * (70.0f + luminance * 1.15f) + luminance * 0.08f));
		}
	}

	// The even tints, for the UFO itself (its floors, walls and consoles inside, and its hull):
	// each colour mixed with one to twelve twelfths of the inner light's colour (as the tint
	// table above has it, before it is matched to the palette), the nearest palette colour as
	// bright as the colour itself (see nearestColorKeepingBrightness). A grey takes all of
	// that share, a strong colour little of it (its saturation takes off up to four fifths),
	// so the light shows on the metal and the art keeps its colours: a red lift pad stays red
	// (a fifth of violet made it a dull mauve).
	// A surface lit the same all over then takes one colour, where the dithered full tint put
	// a grid of light-coloured dots over the whole view inside (a player: "a white screen
	// overlay of dots").
	for (int step = 0; step < UfoSpot::SoftTintSteps; ++step)
	{
		const float share = (step + 1.0f) / UfoSpot::SoftTintSteps;
		ufo.softTint[step][0] = 0;
		for (int index = 1; index < 256; ++index)
		{
			const float luminance = colorLuminance(palette[index]);
			float mixed[3];
			const float own[3] = { (float)palette[index].r, (float)palette[index].g, (float)palette[index].b };
			const float brightest = std::max(own[0], std::max(own[1], own[2])), dimmest = std::min(own[0], std::min(own[1], own[2]));
			const float saturation = brightest > 0.0f ? (brightest - dimmest) / brightest : 0.0f;
			const float colourShare = share * (1.0f - 0.8f * saturation);
			for (int channel = 0; channel < 3; ++channel)
			{
				const float lit = std::min(255.0f, hues[0][channel] * (70.0f + luminance * 1.15f) + luminance * 0.08f);
				mixed[channel] = own[channel] + (lit - own[channel]) * colourShare;
			}
			ufo.softTint[step][index] = nearestColorKeepingBrightness(palette, mixed[0], mixed[1], mixed[2], luminance);
		}
	}

	// The aura's motes: the two lights at full brightness, lifted a little towards white and
	// taken three shades brighter in their palette group (the groups run bright to dark), and a
	// white-hot spark.
	auto brightShade = [](Uint8 color) { return (Uint8)((color & 0xF0) | std::max(0, (color & 15) - 3)); }; // three shades brighter in its group
	ufo.moteColor[0] = brightShade(nearestColor(palette, 40.0f + hues[0][0] * 215.0f, 40.0f + hues[0][1] * 215.0f, 40.0f + hues[0][2] * 215.0f));
	ufo.moteColor[1] = brightShade(nearestColor(palette, 40.0f + hues[1][0] * 215.0f, 40.0f + hues[1][1] * 215.0f, 40.0f + hues[1][2] * 215.0f));
	ufo.moteColor[2] = nearestColor(palette, 225.0f + hues[0][0] * 30.0f, 225.0f + hues[0][1] * 30.0f, 225.0f + hues[0][2] * 30.0f);
}

/**
 * This frame's UFO lights, after the scene's colours (see updateColors): the light colours
 * picked again when the palette has changed, which UFOs have been seen (their light and aura
 * show only from then on, so they never give away a UFO nobody has spotted yet), the lights'
 * pulse, the pools' slow breathing, and the face shades for smooth hull shading.
 *
 * The pulse is gentle by design (the photosensitivity guideline WCAG 2.3): a small light dims
 * by up to 1.6 shades and back in 1.6 seconds, a big lit panel by 0.8 shades in 3.2 seconds,
 * both eased (a cosine), in a wave running round the UFO. With muzzle flashes switched off
 * (the setting for people bothered by flashing light) nothing pulses at all.
 */
void FirstPersonView::updateUfos()
{
	if (!_ufoLightsOn)
	{
		return;
	}
	// A new palette (another battle, the depth under the sea): pick the light colours again.
	if (_colors.paletteHash != _ufoPaletteHash)
	{
		_ufoPaletteHash = _colors.paletteHash;
		for (UfoSpot &ufo : _ufos) ufo.colorsChosen = false;
	}
	// Each UFO's colours (when needed), and whether any of its tiles has been seen yet.
	const bool debug = _save->getDebugMode();
	for (UfoSpot &ufo : _ufos)
	{
		if (!ufo.colorsChosen) chooseUfoColors(ufo);
		for (size_t index = 0; index < ufo.tiles.size() && !ufo.seen; ++index)
		{
			ufo.seen = debug || _tileViews[ufo.tiles[index]].discovered || _tileViews[ufo.tiles[index]].seen;
		}
	}

	// The pulse, per phase of the wave round the UFO: how many shades below full brightness a
	// small light and a big panel are now (0 brightest), eased in and out.
	const float seconds = SDL_GetTicks() / 1000.0f;
	const bool steady = !Options::fpsMuzzleFlash;
	for (int phase = 0; phase < 16; ++phase)
	{
		const float lightWave = 0.5f - 0.5f * std::cos(2.0f * (float)Pi * (seconds / 1.6f - phase / 16.0f));
		const float panelWave = 0.5f - 0.5f * std::cos(2.0f * (float)Pi * (seconds / 3.2f - phase / 16.0f));
		_ufoPulse[0][phase] = steady ? 0.4f : lightWave * 1.6f;
		_ufoPulse[1][phase] = steady ? 0.2f : panelWave * 0.8f;
	}
	// The pools of light breathe slowly (a five-second swell of 16%), or stay steady.
	_ufoBreath = steady ? 0.92f : 0.84f + 0.16f * (0.5f + 0.5f * std::sin(seconds * 2.0f * (float)Pi / 5.0f));

	// Smooth hull shading: for every set of open faces, the face shade of the direction they
	// face together (their normals added up), by the same rule as the faces' own shading in
	// updateColors. A set with two opposite faces (a thin wall or plate) has no one direction:
	// it keeps each face's own shade (-1).
	const float daylight = 1.0f - _colors.night;
	for (int openFaces = 0; openFaces < 64; ++openFaces)
	{
		float sum[3] = { 0, 0, 0 };
		bool opposite = false;
		for (int face = 0; face < 6; ++face)
		{
			if (!((openFaces >> face) & 1)) continue;
			for (int axis = 0; axis < 3; ++axis) sum[axis] += FaceNormals[face][axis];
			opposite |= (face & 1) == 0 && ((openFaces >> (face + 1)) & 1);
		}
		const float length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
		if (opposite || length < 0.5f)
		{
			_ufoFaceShade[openFaces] = -1.0f;
			continue;
		}
		const float lambert = std::max(0.0f, (sum[0] * _colors.sunDir[0] + sum[1] * _colors.sunDir[1] + sum[2] * _colors.sunDir[2]) / length);
		_ufoFaceShade[openFaces] = (1.0f - lambert) * (0.5f + 2.5f * daylight) + std::max(0.0f, -sum[2] / length);
	}
}

/**
 * Spawns the aura of the UFOs in sight (seen, and within 50 tiles): motes of their light
 * colours drifting slowly round the hull, a little outwards and up, most of them low down,
 * fading as they go; sparks streaking up the hull from its foot, a shimmer of energy; and a
 * beam of motes and streaks rising through each grav lift. About 70 motes a second around a
 * scout (28 with particles at effects only), more around a bigger UFO; each lives up to 6
 * seconds, so a UFO keeps some hundreds; past UfoAuraParticleCap particles in all, the auras
 * wait.
 * @param elapsedSeconds Seconds since the last frame: the spawn rates are per second.
 */
void FirstPersonView::spawnUfoAura(float elapsedSeconds)
{
	typedef FirstPersonParticles P; // short for the particle system: its kinds and groups
	for (const UfoSpot &ufo : _ufos)
	{
		// Only UFOs that have been seen, within 800 voxels (50 tiles) of the eye.
		const float dx = ufo.centre[0] - (float)_eye[0], dy = ufo.centre[1] - (float)_eye[1];
		if (!ufo.seen || !ufo.colorsChosen || dx * dx + dy * dy > 800.0f * 800.0f) continue;

		// How many motes this frame: by the length of its rim (the fraction carried by chance).
		const float rim = (float)Pi * (ufo.half[0] + ufo.half[1]);
		const float size = std::clamp(rim / 150.0f, 0.6f, 2.5f);
		const float rate = (Options::fpsParticles > 1 ? 70.0f : 28.0f) * size;
		const bool room = _particles.all().size() < UfoAuraParticleCap; // (past the cap the aura waits)
		const int moteCount = room ? (int)(elapsedSeconds * rate + _particles.random()) : 0;
		const float height = ufo.top - ufo.bottom;
		// (A point on one of the UFO's own tiles: its rim is an ellipse round its footprint,
		// which cuts inside the corners of a boxy UFO; the aura stays outside, the inside clean.)
		const int mapSizeX = _save->getMapSizeX(), mapSizeY = _save->getMapSizeY(), mapSizeZ = _save->getMapSizeZ();
		auto onUfoTile = [&](const float *pos)
		{
			const int x = (int)std::floor(pos[0] / 16.0f), y = (int)std::floor(pos[1] / 16.0f), z = (int)std::floor(pos[2] / 24.0f);
			if (x < 0 || y < 0 || z < 0 || x >= mapSizeX || y >= mapSizeY || z >= mapSizeZ) return false;
			return (_ufoPhaseOfTile[(z * mapSizeY + y) * mapSizeX + x] & UfoTileBit) != 0;
		};
		for (int i = 0; i < moteCount; ++i)
		{
			// A random point just outside the rim, at a random height (more of them low down),
			// not on the UFO's own tiles.
			P::Particle mote;
			const float angle = _particles.random(0, 2.0f * (float)Pi), out = _particles.random(1.02f, 1.25f);
			const float cosAngle = std::cos(angle), sinAngle = std::sin(angle);
			const float heightShare = _particles.random();
			mote.pos[0] = ufo.centre[0] + cosAngle * ufo.half[0] * out;
			mote.pos[1] = ufo.centre[1] + sinAngle * ufo.half[1] * out;
			mote.pos[2] = ufo.bottom + 2.0f + heightShare * heightShare * height;
			if (onUfoTile(mote.pos)) continue;
			// It drifts round the UFO (all of them the same way), a little outwards and up.
			const float swirl = _particles.random(3, 7), outwards = _particles.random(0.5f, 2.0f);
			mote.vel[0] = -sinAngle * swirl + cosAngle * outwards;
			mote.vel[1] = cosAngle * swirl + sinAngle * outwards;
			mote.vel[2] = _particles.random(2, 6);
			mote.age = 0; mote.life = _particles.random(3.0f, 6.0f); mote.gravity = 0; mote.drag = 0.15f;
			// Mostly the inner light, often the outer one, now and then a white-hot speck.
			const float colorRoll = _particles.random();
			mote.color = colorRoll < 0.55f ? ufo.moteColor[0] : colorRoll < 0.92f ? ufo.moteColor[1] : ufo.moteColor[2];
			const float sizeRoll = _particles.random();
			mote.size = sizeRoll < 0.15f ? 4 : sizeRoll < 0.6f ? 3 : 2;
			mote.kind = P::MOTE; mote.group = P::EFFECT;
			_particles.add(mote);
		}

		// The shimmer: about six times a second round a scout, a spark streaks up the hull
		// from its foot, fast and white-hot, gone within half a second (drawn as a short line;
		// not on the UFO's own tiles either).
		const int sparkCount = room ? (int)(elapsedSeconds * 6.0f * size + _particles.random()) : 0;
		for (int i = 0; i < sparkCount; ++i)
		{
			P::Particle spark;
			const float angle = _particles.random(0, 2.0f * (float)Pi);
			spark.pos[0] = ufo.centre[0] + std::cos(angle) * ufo.half[0] * 1.03f;
			spark.pos[1] = ufo.centre[1] + std::sin(angle) * ufo.half[1] * 1.03f;
			spark.pos[2] = ufo.bottom + _particles.random(1.0f, 6.0f);
			if (onUfoTile(spark.pos)) continue;
			spark.vel[0] = std::cos(angle) * 6.0f;
			spark.vel[1] = std::sin(angle) * 6.0f;
			spark.vel[2] = _particles.random(70, 110);
			spark.age = 0; spark.life = _particles.random(0.3f, 0.5f); spark.gravity = 0; spark.drag = 0.8f;
			spark.color = _particles.random() < 0.6f ? ufo.moteColor[2] : ufo.moteColor[0];
			spark.size = 1; spark.kind = P::STREAK; spark.group = P::EFFECT;
			_particles.add(spark);
		}

		// The grav lifts' beams: a tight column of motes and fast streaks of light rising from
		// the floor below each lift up through it to the level above, the motes turning slowly
		// (40 a second, 16 with particles at effects only; one in five a streak). They start
		// anywhere along the column and live until they reach its top, so the column is full
		// from the start.
		for (size_t lift = 0; lift < ufo.lifts.size(); ++lift)
		{
			const int liftTile = ufo.lifts[lift];
			const float beamBottom = ufo.liftBottoms[lift], beamTop = (liftTile / (mapSizeX * mapSizeY) + 1) * 24.0f + 8.0f;
			const int beamCount = room ? (int)(elapsedSeconds * (Options::fpsParticles > 1 ? 40.0f : 16.0f) + _particles.random()) : 0;
			for (int i = 0; i < beamCount; ++i)
			{
				P::Particle mote;
				const float angle = _particles.random(0, 2.0f * (float)Pi), radius = _particles.random(0.5f, 3.5f);
				const bool streak = _particles.random() < 0.2f; // one in five a streak of light
				mote.pos[0] = (liftTile % mapSizeX) * 16.0f + 8.0f + std::cos(angle) * radius;
				mote.pos[1] = ((liftTile / mapSizeX) % mapSizeY) * 16.0f + 8.0f + std::sin(angle) * radius;
				mote.pos[2] = beamBottom + _particles.random() * (beamTop - beamBottom) * 0.8f;
				mote.vel[0] = streak ? 0.0f : -std::sin(angle) * 3.0f;
				mote.vel[1] = streak ? 0.0f : std::cos(angle) * 3.0f;
				mote.vel[2] = streak ? _particles.random(30, 45) : _particles.random(9, 14);
				mote.age = 0; mote.life = std::clamp((beamTop - mote.pos[2]) / mote.vel[2], 0.2f, 4.0f); mote.gravity = 0; mote.drag = 0.0f;
				mote.color = _particles.random() < 0.6f ? ufo.moteColor[0] : ufo.moteColor[2];
				mote.size = _particles.random() < 0.5f ? 4 : 3;
				mote.kind = streak ? P::STREAK : P::MOTE; mote.group = P::EFFECT;
				_particles.add(mote);
			}
		}
	}
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
